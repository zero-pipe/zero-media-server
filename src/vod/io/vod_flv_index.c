#include "zms/vod/vod_flv_index.h"
#include "zms/media/container/container_dispatcher.h"
#include "ztk/util/log.h"
#include <stdlib.h>
#include <string.h>

#define VFI_MAX_KEYFRAMES 512
#define VFI_MIN_KEYFRAME_SEC 2.0
#define VFI_META_EST 384

static size_t flv_tag_total(size_t body_len)
{
    return 11 + body_len + 4;
}

static size_t estimate_flv_body(int is_video, uint32_t sample_bytes)
{
    if (is_video) {
        return (size_t)sample_bytes + (size_t)(sample_bytes / 10) + 20;
    }
    return (size_t)sample_bytes + 4;
}

static int vfi_push_keyframe(zms_vod_flv_index *idx, double time_sec, double byte_pos,
                             double last_time)
{
    if (!idx || time_sec < 0.0) {
        return 0;
    }
    if (idx->count > 0 && time_sec - last_time < VFI_MIN_KEYFRAME_SEC) {
        return 0;
    }
    if (idx->count >= VFI_MAX_KEYFRAMES) {
        return 0;
    }
    {
        size_t n = idx->count + 1;
        double *t2 = (double *)realloc(idx->times, n * sizeof(double));
        double *p2 = (double *)realloc(idx->filepositions, n * sizeof(double));
        if (!t2 || !p2) {
            free(t2);
            free(p2);
            return -1;
        }
        idx->times = t2;
        idx->filepositions = p2;
        idx->times[idx->count] = time_sec;
        idx->filepositions[idx->count] = byte_pos;
        idx->count = n;
    }
    return 1;
}

void zms_vod_flv_index_free(zms_vod_flv_index *idx)
{
    if (!idx) {
        return;
    }
    free(idx->times);
    free(idx->filepositions);
    free(idx);
}

typedef struct {
    zms_vod_flv_index *idx;
    uint64_t byte_pos;
    uint64_t tags_only;
    double last_kf_time;
    int failed;
} vfi_build_ctx;

static int vfi_on_sample(const zms_mp4_sample_info *s, void *user)
{
    vfi_build_ctx *b = (vfi_build_ctx *)user;
    size_t body = estimate_flv_body(s->is_video, s->bytes);
    double time_sec = s->dts_ms / 1000.0;

    if (s->is_video && s->key) {
        if (vfi_push_keyframe(b->idx, time_sec, (double)b->byte_pos, b->last_kf_time) < 0) {
            b->failed = 1;
            return -1;
        }
        if (b->idx->count > 0) {
            b->last_kf_time = b->idx->times[b->idx->count - 1];
        }
    }
    b->byte_pos += flv_tag_total(body);
    b->tags_only += flv_tag_total(body);
    return 0;
}

zms_vod_flv_index *zms_vod_flv_index_build(void *mp4_demux, size_t video_cfg_len,
                                           size_t audio_cfg_len, size_t metadata_bytes)
{
    zms_vod_flv_index *idx;
    vfi_build_ctx b;
    uint64_t byte_pos;

    if (!mp4_demux) {
        return NULL;
    }

    idx = (zms_vod_flv_index *)calloc(1, sizeof(*idx));
    if (!idx) {
        return NULL;
    }

    byte_pos = 13 + 4;
    if (metadata_bytes == 0) {
        metadata_bytes = VFI_META_EST;
    }
    byte_pos += flv_tag_total(metadata_bytes);
    if (video_cfg_len) {
        byte_pos += flv_tag_total(video_cfg_len);
    }
    if (audio_cfg_len) {
        byte_pos += flv_tag_total(audio_cfg_len);
    }

    if (vfi_push_keyframe(idx, 0.0, (double)byte_pos, -VFI_MIN_KEYFRAME_SEC) < 0) {
        goto fail;
    }

    b.idx = idx;
    b.byte_pos = byte_pos;
    b.tags_only = 0;
    b.last_kf_time = -VFI_MIN_KEYFRAME_SEC;
    b.failed = 0;
    if (zms_container_mp4_for_each_sample(mp4_demux, vfi_on_sample, &b) < 0 || b.failed) {
        goto fail;
    }

    idx->filesize = (double)b.byte_pos;
    idx->metadata_bytes = metadata_bytes;
    ztk_info("vod flv index: keyframes=%u filesize=%.0f tags=%llu", (unsigned)idx->count,
             idx->filesize, (unsigned long long)b.tags_only);
    return idx;

fail:
    zms_vod_flv_index_free(idx);
    return NULL;
}

double zms_vod_flv_index_byte_at_ms(const zms_vod_flv_index *idx, uint64_t play_ms)
{
    double t;
    size_t i;

    if (!idx || idx->count == 0) {
        return 0.0;
    }
    t = play_ms / 1000.0;
    if (t <= idx->times[0]) {
        return idx->filepositions[0];
    }
    for (i = 1; i < idx->count; ++i) {
        if (idx->times[i] > t) {
            double t0 = idx->times[i - 1];
            double t1 = idx->times[i];
            double p0 = idx->filepositions[i - 1];
            double p1 = idx->filepositions[i];
            if (t1 <= t0) {
                return p0;
            }
            return p0 + (p1 - p0) * (t - t0) / (t1 - t0);
        }
    }
    return idx->filepositions[idx->count - 1];
}

zms_vod_flv_index_view *zms_vod_flv_index_view_create(const zms_vod_flv_index *idx,
                                                      uint64_t play_ms)
{
    zms_vod_flv_index_view *view;
    double base;
    double play_sec;
    size_t i;
    size_t n;

    if (!idx || idx->count == 0) {
        return NULL;
    }
    view = (zms_vod_flv_index_view *)calloc(1, sizeof(*view));
    if (!view) {
        return NULL;
    }

    base = zms_vod_flv_index_byte_at_ms(idx, play_ms);
    play_sec = play_ms / 1000.0;
    view->filesize = idx->filesize > base ? idx->filesize - base : 0.0;

    for (i = 0; i < idx->count; ++i) {
        if (idx->times[i] + 0.001 >= play_sec) {
            view->count++;
        }
    }
    if (view->count == 0) {
        view->count = 1;
    }

    view->times = (double *)calloc(view->count, sizeof(double));
    view->filepositions = (double *)calloc(view->count, sizeof(double));
    if (!view->times || !view->filepositions) {
        zms_vod_flv_index_view_free(view);
        return NULL;
    }

    n = 0;
    for (i = 0; i < idx->count && n < view->count; ++i) {
        if (idx->times[i] + 0.001 < play_sec) {
            continue;
        }
        view->times[n] = idx->times[i];
        view->filepositions[n] = idx->filepositions[i] - base;
        if (view->filepositions[n] < 0.0) {
            view->filepositions[n] = 0.0;
        }
        n++;
    }
    if (n == 0) {
        view->times[0] = play_sec;
        view->filepositions[0] = 0.0;
    } else {
        view->count = n;
    }
    return view;
}

void zms_vod_flv_index_view_free(zms_vod_flv_index_view *view)
{
    if (!view) {
        return;
    }
    free(view->times);
    free(view->filepositions);
    free(view);
}
