#include "zms/media/container/container_dispatcher.h"
#include "zms/media/codec/codec_id.h"
#include "zms/engine/media/media_limits.h"
#include "vod/io/mov_file_buffer.h"
#include "mov-format.h"
#include "mov-reader.h"
#include "mov-internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MP4_DEMUX_READ_BUF ZMS_VOD_DEMUX_READ_MIN

typedef struct {
    zms_container_demux_opts cfg;
    FILE *fp;
    mov_reader_t *mov;
    uint8_t *read_buf;
    size_t read_cap;
    int eof;
    int samples_delivered;
    zms_codec_id video_codec;
    zms_codec_id audio_codec;
    uint32_t video_track;
    uint32_t audio_track;
    uint64_t duration_ms;
} mp4_demux_ctx;

static zms_codec_id mp4_codec_from_mov_object(uint8_t object)
{
    switch (object) {
    case MOV_OBJECT_H264:
        return ZMS_CODEC_H264;
    case MOV_OBJECT_H265:
        return ZMS_CODEC_H265;
    case MOV_OBJECT_AAC:
    case MOV_OBJECT_AAC_MAIN:
    case MOV_OBJECT_AAC_LC:
    case MOV_OBJECT_AAC_SSR:
        return ZMS_CODEC_AAC;
    case MOV_OBJECT_H266:
        return ZMS_CODEC_H266;
    default:
        return ZMS_CODEC_INVALID;
    }
}

static void mp4_emit_config(mp4_demux_ctx *c, uint32_t track, zms_codec_id codec, const void *extra,
                            size_t bytes, uint16_t width, uint16_t height, uint32_t sample_rate,
                            uint8_t channels)
{
    zms_container_packet pkt;

    if (!c || !c->cfg.on_packet || codec == ZMS_CODEC_INVALID) {
        return;
    }
    if (!extra && bytes > 0) {
        return;
    }
    memset(&pkt, 0, sizeof(pkt));
    pkt.kind = ZMS_CONTAINER_PKT_MP4_SAMPLE;
    pkt.track_id = track;
    pkt.codec = codec;
    pkt.config = 1;
    pkt.width = width;
    pkt.height = height;
    pkt.sample_rate = sample_rate;
    pkt.channels = channels;
    pkt.data = (const uint8_t *)extra;
    pkt.len = bytes;
    c->cfg.on_packet(&pkt, c->cfg.user);
}

static void mp4_on_video(void *param, uint32_t track, uint8_t object, int width, int height,
                         const void *extra, size_t bytes)
{
    mp4_demux_ctx *c = (mp4_demux_ctx *)param;

    (void)width;
    (void)height;
    if (!c) {
        return;
    }
    c->video_track = track;
    c->video_codec = mp4_codec_from_mov_object(object);
    if (extra && bytes > 0) {
        mp4_emit_config(c, track, c->video_codec, extra, bytes, (uint16_t)(width > 0 ? width : 0),
                        (uint16_t)(height > 0 ? height : 0), 0, 0);
    }
}

static void mp4_on_audio(void *param, uint32_t track, uint8_t object, int channel_count,
                         int bit_per_sample, int sample_rate, const void *extra, size_t bytes)
{
    mp4_demux_ctx *c = (mp4_demux_ctx *)param;

    (void)channel_count;
    (void)bit_per_sample;
    (void)sample_rate;
    if (!c) {
        return;
    }
    c->audio_track = track;
    c->audio_codec = mp4_codec_from_mov_object(object);
    if (c->audio_codec == ZMS_CODEC_AAC) {
        mp4_emit_config(c, track, c->audio_codec, extra, bytes, 0, 0,
                        sample_rate > 0 ? (uint32_t)sample_rate : 0,
                        channel_count > 0 ? (uint8_t)channel_count : 0);
    }
}

static void mp4_onread(void *param, uint32_t track, const void *buffer, size_t bytes, int64_t pts,
                       int64_t dts, int flags)
{
    mp4_demux_ctx *c = (mp4_demux_ctx *)param;
    zms_container_packet pkt;
    zms_codec_id codec = ZMS_CODEC_INVALID;

    if (!c || !c->cfg.on_packet || !buffer || bytes == 0) {
        return;
    }
    if (track == c->video_track) {
        codec = c->video_codec;
    } else if (track == c->audio_track) {
        codec = c->audio_codec;
    } else {
        return;
    }

    memset(&pkt, 0, sizeof(pkt));
    pkt.kind = ZMS_CONTAINER_PKT_MP4_SAMPLE;
    pkt.track_id = track;
    pkt.codec = codec;
    pkt.pts_ms = (uint32_t)(pts > 0 ? pts : dts);
    pkt.dts_ms = (uint32_t)dts;
    pkt.key = (flags & MOV_AV_FLAG_KEYFREAME) ? 1 : 0;
    pkt.data = (const uint8_t *)buffer;
    pkt.len = bytes;
    c->cfg.on_packet(&pkt, c->cfg.user);
    ++c->samples_delivered;
}

typedef struct {
    uint32_t track;
    size_t bytes;
    int64_t pts;
    int64_t dts;
    int flags;
    mp4_demux_ctx *demux;
} mp4_read_sample;

static void *mp4_alloc_sample(void *param, uint32_t track, size_t bytes, int64_t pts, int64_t dts,
                              int flags)
{
    mp4_read_sample *sample = (mp4_read_sample *)param;
    mp4_demux_ctx *c;
    void *buf;

    if (!sample || !sample->demux || bytes == 0) {
        return NULL;
    }
    c = sample->demux;
    sample->track = track;
    sample->bytes = bytes;
    sample->pts = pts;
    sample->dts = dts;
    sample->flags = flags;
    if (c->read_cap < bytes) {
        size_t cap = bytes;
        if (cap < MP4_DEMUX_READ_BUF) {
            cap = MP4_DEMUX_READ_BUF;
        }
        buf = realloc(c->read_buf, cap);
        if (!buf) {
            return NULL;
        }
        c->read_buf = (uint8_t *)buf;
        c->read_cap = cap;
    }
    return c->read_buf;
}

static void mp4_free_read_buf(mp4_demux_ctx *c)
{
    if (!c) {
        return;
    }
    free(c->read_buf);
    c->read_buf = NULL;
    c->read_cap = 0;
}

static void mp4_close_file(mp4_demux_ctx *c)
{
    if (!c) {
        return;
    }
    if (c->mov) {
        mov_reader_destroy(c->mov);
        c->mov = NULL;
    }
    if (c->fp) {
        fclose(c->fp);
        c->fp = NULL;
    }
    mp4_free_read_buf(c);
    c->eof = 0;
    c->samples_delivered = 0;
    c->video_codec = ZMS_CODEC_INVALID;
    c->audio_codec = ZMS_CODEC_INVALID;
    c->video_track = 0;
    c->audio_track = 0;
    c->duration_ms = 0;
}

static void *mp4_create(const zms_container_demux_opts *opts)
{
    mp4_demux_ctx *c;

    if (!opts || !opts->on_packet) {
        return NULL;
    }
    c = (mp4_demux_ctx *)calloc(1, sizeof(*c));
    if (!c) {
        return NULL;
    }
    c->cfg = *opts;
    return c;
}

static void mp4_destroy(void *ctx)
{
    mp4_demux_ctx *c = (mp4_demux_ctx *)ctx;

    if (!c) {
        return;
    }
    mp4_close_file(c);
    free(c);
}

static ztk_err_t mp4_feed(void *ctx, const uint8_t *buf, size_t len)
{
    (void)ctx;
    (void)buf;
    (void)len;
    return ZTK_ERR_INVALID;
}

static ztk_err_t mp4_input_tag(void *ctx, uint8_t type_id, const uint8_t *body, size_t len,
                               uint32_t tag_dts_ms)
{
    (void)ctx;
    (void)type_id;
    (void)body;
    (void)len;
    (void)tag_dts_ms;
    return ZTK_ERR_INVALID;
}

static ztk_err_t mp4_open_file(void *ctx, const char *path)
{
    mp4_demux_ctx *c = (mp4_demux_ctx *)ctx;
    struct mov_reader_trackinfo_t info;

    if (!c || !path || !path[0]) {
        return ZTK_ERR_INVALID;
    }

    mp4_close_file(c);
    c->fp = fopen(path, "rb");
    if (!c->fp) {
        return ZTK_ERR_IO;
    }

    c->mov = mov_reader_create(zms_mov_file_buffer(), c->fp);
    if (!c->mov) {
        fclose(c->fp);
        c->fp = NULL;
        return ZTK_ERR_IO;
    }

    memset(&info, 0, sizeof(info));
    info.onvideo = mp4_on_video;
    info.onaudio = mp4_on_audio;
    if (mov_reader_getinfo(c->mov, &info, c) != 0) {
        mp4_close_file(c);
        return ZTK_ERR_INVALID;
    }
    if (c->video_codec != ZMS_CODEC_H264 && c->video_codec != ZMS_CODEC_H265) {
        mp4_close_file(c);
        return ZTK_ERR_NOT_IMPL;
    }

    c->duration_ms = mov_reader_getduration(c->mov);
    c->eof = 0;
    return ZTK_OK;
}

static void mp4_close_file_op(void *ctx)
{
    mp4_close_file((mp4_demux_ctx *)ctx);
}

static int mp4_pump(void *ctx)
{
    mp4_demux_ctx *c = (mp4_demux_ctx *)ctx;
    mp4_read_sample sample;
    int n;

    if (!c || !c->mov || c->eof) {
        return 0;
    }

    c->samples_delivered = 0;
    memset(&sample, 0, sizeof(sample));
    sample.demux = c;
    n = mov_reader_read2(c->mov, mp4_alloc_sample, &sample);
    if (n > 0) {
        if (c->read_buf && sample.bytes > 0) {
            mp4_onread(c, sample.track, c->read_buf, sample.bytes, sample.pts, sample.dts,
                       sample.flags);
        }
        return c->samples_delivered > 0 ? c->samples_delivered : 1;
    }
    if (n == 0) {
        c->eof = 1;
        return 0;
    }
    return -1;
}

static ztk_err_t mp4_seek(void *ctx, int64_t *seek_ms)
{
    mp4_demux_ctx *c = (mp4_demux_ctx *)ctx;

    if (!c || !c->mov || !seek_ms) {
        return ZTK_ERR_INVALID;
    }
    if (mov_reader_seek(c->mov, seek_ms) != 0) {
        return ZTK_ERR_INVALID;
    }
    c->eof = 0;
    return ZTK_OK;
}

static uint64_t mp4_duration_ms(void *ctx)
{
    mp4_demux_ctx *c = (mp4_demux_ctx *)ctx;

    return c ? c->duration_ms : 0;
}

void zms_container_demux_mp4_codecs(void *ctx, zms_codec_id *video, zms_codec_id *audio)
{
    mp4_demux_ctx *c = (mp4_demux_ctx *)ctx;

    if (video) {
        *video = c ? c->video_codec : ZMS_CODEC_INVALID;
    }
    if (audio) {
        *audio = c ? c->audio_codec : ZMS_CODEC_INVALID;
    }
}

const zms_container_demuxer_ops zms_container_mp4_ops = {
    ZMS_CONTAINER_MP4, "mp4",         mp4_create,      mp4_destroy,
    mp4_feed,          mp4_input_tag, mp4_open_file,   mp4_close_file_op,
    mp4_pump,          mp4_seek,      mp4_duration_ms,
};

struct mov_reader_t *zms_container_mp4_mov_reader(void *ctx)
{
    mp4_demux_ctx *c = (mp4_demux_ctx *)ctx;

    return c ? c->mov : NULL;
}

/* mov_reader_t 在公开 lib 头中为 opaque；真实布局（mov-internal.h
 * 私有）以 {flags, have_read_mfra} 开头，后接嵌入的 mov_t。 */
static struct mov_t *mp4_mov_of_reader(mov_reader_t *mr)
{
    typedef struct {
        int flags;
        int have_read_mfra;
        struct mov_t mov;
    } mov_reader_layout;
    return mr ? &((mov_reader_layout *)mr)->mov : NULL;
}

static int64_t mp4_sample_dts_ms(const struct mov_track_t *track, const struct mov_sample_t *s)
{
    if (!track || !s || track->mdhd.timescale == 0) {
        return 0;
    }
    return s->dts * 1000 / track->mdhd.timescale;
}

static struct mov_track_t *mp4_pick_next_track(struct mov_t *mov)
{
    struct mov_track_t *best = NULL;
    int64_t best_dts = 0;
    int i;

    for (i = 0; i < mov->track_count; ++i) {
        struct mov_track_t *t = &mov->tracks[i];
        const struct mov_sample_t *s;

        if (t->sample_offset >= t->sample_count) {
            continue;
        }
        s = &t->samples[t->sample_offset];
        {
            int64_t dts = mp4_sample_dts_ms(t, s);
            if (!best || dts < best_dts ||
                (dts == best_dts && s->offset < best->samples[best->sample_offset].offset)) {
                best = t;
                best_dts = dts;
            }
        }
    }
    return best;
}

int zms_container_mp4_for_each_sample(void *ctx, zms_mp4_sample_fn fn, void *user)
{
    mp4_demux_ctx *c = (mp4_demux_ctx *)ctx;
    struct mov_t *mov;
    struct mov_track_t *track;
    int i;
    int visited = 0;
    int rc = 0;

    if (!c || !c->mov || !fn) {
        return -1;
    }
    mov = mp4_mov_of_reader(c->mov);
    if (!mov) {
        return -1;
    }

    for (i = 0; i < mov->track_count; ++i) {
        mov->tracks[i].sample_offset = 0;
    }

    while ((track = mp4_pick_next_track(mov)) != NULL) {
        const struct mov_sample_t *s = &track->samples[track->sample_offset++];
        zms_mp4_sample_info info;

        info.dts_ms = mp4_sample_dts_ms(track, s);
        info.bytes = s->bytes;
        info.is_video = track->handler_type == MOV_VIDEO;
        info.key = info.is_video && (s->flags & MOV_AV_FLAG_KEYFREAME);
        ++visited;
        rc = fn(&info, user);
        if (rc < 0) {
            break;
        }
    }

    for (i = 0; i < mov->track_count; ++i) {
        mov->tracks[i].sample_offset = 0;
    }
    return rc < 0 ? rc : visited;
}

static void mp4_noop_packet(const zms_container_packet *pkt, void *user)
{
    (void)pkt;
    (void)user;
}

uint64_t zms_container_mp4_file_duration_ms(const char *path)
{
    const zms_container_demuxer_ops *ops;
    zms_container_demux_opts cfg;
    void *ctx;
    uint64_t dur;

    if (!path || !path[0]) {
        return 0;
    }
    ops = zms_container_demuxer_find(ZMS_CONTAINER_MP4);
    if (!ops || !ops->create || !ops->open_file || !ops->duration_ms || !ops->destroy) {
        return 0;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.id = ZMS_CONTAINER_MP4;
    cfg.on_packet = mp4_noop_packet;
    ctx = ops->create(&cfg);
    if (!ctx) {
        return 0;
    }
    if (ops->open_file(ctx, path) != ZTK_OK) {
        ops->destroy(ctx);
        return 0;
    }
    dur = ops->duration_ms(ctx);
    ops->destroy(ctx);
    return dur;
}
