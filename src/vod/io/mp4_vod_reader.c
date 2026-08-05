/**
 * @file mp4_vod_reader.c
 * @brief 经 libmov 解复用 MP4 写入 vod_buffer，由 poller 驱动泵送。
 *
 * Copyright (c) zero-media-server
 */
#include "zms/vod/io/mp4_vod_reader.h"
#include "zms/media/container/container_dispatcher.h"
#include "zms/engine/media_clock.h"
#include "zms/engine/media/media_limits.h"
#include "zms/media/codec/h264/h264_over_rtmp.h"
#include "zms/media/container/flv/flv_tag_probe.h"
#include "zms/media/codec/bitstream/annexb.h"
#include "zms/media/codec/h264/h264_es.h"
#include "zms/media/codec/h265/h265_es.h"
#include "zms/engine/media_track.h"
#include "zms/media/codec/aac/aac_config.h"
#include "zms/media/codec/h264/h264_config.h"
#include "zms/media/codec/h265/h265_config.h"
#include "zms/vod/vod_flv_index.h"
#include "zms/vod/vod_flv_metadata.h"
#include "zms/util/buf_pool.h"
#include "ztk/util/log.h"
#include "ztk/util/timer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#define zms_file_stricmp _stricmp
#else
#define zms_file_stricmp strcasecmp
#endif

#define MP4_TICK_MS 5
#define MP4_FIFO_HW 384u
#define MP4_SEEK_SYNC_MAX 256
#define MP4_SEEK_PREFILL_MAX 96
#define MP4_ANNEXB_MIN (128 * 1024)

struct zms_mp4_pending_slot {
    int used;
    uint32_t track;
    int64_t dts_ms;
    int64_t pts_ms;
    int key;
    uint8_t *data;
    size_t cap;
    size_t len;
};

struct zms_mp4_reader {
    char path[512];
    zms_container_id container_id;
    const zms_container_demuxer_ops *mp4_ops;
    void *mp4_demux;
    zms_vod_buffer *fifo;
    zms_media_source *src;
    void *owner_ctx;
    ztk_poller *poller;
    ztk_timer *timer;

    zms_avc_config avc;
    zms_hevc_config hevc;
    zms_aac_config aac;
    zms_codec_id video_codec;
    zms_codec_id audio_codec;
    uint32_t video_track;
    uint32_t audio_track;

    uint8_t *scratch;
    size_t scratch_cap;
    uint8_t *annexb_buf;
    size_t annexb_cap;

    struct zms_mp4_pending_slot pending;

    int64_t clock0_wall;
    int64_t dts0;
    int clock_armed;

    double speed;
    int loop;
    size_t fifo_high_water;
    uint64_t duration_ms;
    int eof;
    int have_video_cfg;
    int have_audio_cfg;
    int drop_until_sync;
    int pump_hold;
    zms_vod_flv_index *flv_index;
};

static int mp4_owns_source_meta(const zms_mp4_reader *r)
{
    if (!r || !r->src) {
        return 0;
    }
    return !r->src->publisher_ctx || r->src->publisher_ctx == (void *)r ||
           (r->owner_ctx && r->src->publisher_ctx == r->owner_ctx);
}

static uint32_t mp4_read_be_size(const uint8_t *p, int nalu_bytes)
{
    uint32_t n = 0;
    int i;

    for (i = 0; i < nalu_bytes; ++i) {
        n = (n << 8) + p[i];
    }
    return n;
}

static int mp4_avcc_sample_valid(const uint8_t *p, size_t len, int nalu_bytes)
{
    size_t off = 0;

    if (!p || nalu_bytes < 1 || nalu_bytes > 4 || len < (size_t)nalu_bytes) {
        return 0;
    }
    while (off + (size_t)nalu_bytes <= len) {
        uint32_t n = mp4_read_be_size(p + off, nalu_bytes);
        off += (size_t)nalu_bytes;
        if (n == 0 || off + n > len) {
            return 0;
        }
        off += n;
    }
    return off == len;
}

static int mp4_sample_is_annexb(const uint8_t *p, size_t len, int avcc_nalu_bytes)
{
    if (!p || len < 3) {
        return 0;
    }
    if (p[0] == 0 && p[1] == 0 && p[2] == 1) {
        return 1;
    }
    if (len >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) {
        if (avcc_nalu_bytes > 0 && mp4_avcc_sample_valid(p, len, avcc_nalu_bytes)) {
            return 0;
        }
        return 1;
    }
    return 0;
}

static int blob_is_annexb(const uint8_t *p, size_t len)
{
    return mp4_sample_is_annexb(p, len, 0);
}

static int pacing_ready(zms_mp4_reader *r, int64_t dts_ms)
{
    (void)dts_ms;
    /* VOD demux 尽快填满 vod_buffer；RTSP/RTMP 播放路径负责输出 pacing。 */
    if (!r) {
        return 0;
    }
    return 1;
}

static void pending_clear(struct zms_mp4_pending_slot *p)
{
    if (!p) {
        return;
    }
    if (p->data) {
        zms_buf_pool_release(p->data, p->cap);
    }
    p->data = NULL;
    p->cap = 0;
    p->len = 0;
    p->used = 0;
}

static int pending_set(struct zms_mp4_pending_slot *p, uint32_t track, int64_t dts_ms,
                       int64_t pts_ms, int key, const void *data, size_t len)
{
    if (!p || !data || len == 0) {
        return -1;
    }
    pending_clear(p);
    p->data = (uint8_t *)zms_buf_pool_acquire(len, &p->cap);
    if (!p->data) {
        return -1;
    }
    memcpy(p->data, data, len);
    p->len = len;
    p->track = track;
    p->dts_ms = dts_ms;
    p->pts_ms = pts_ms;
    p->key = key;
    p->used = 1;
    return 0;
}

static const uint8_t *next_annexb_nal(const uint8_t *p, const uint8_t *end, int *nal_type)
{
    while (p + 3 < end) {
        if (p[0] == 0 && p[1] == 0 && p[2] == 1) {
            if (nal_type) {
                *nal_type = p[3] & 0x1f;
            }
            return p + 3;
        }
        if (p + 4 < end && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) {
            if (nal_type) {
                *nal_type = p[4] & 0x1f;
            }
            return p + 4;
        }
        ++p;
    }
    return NULL;
}

static int h264_annexb_has_nal_type(const uint8_t *annexb, size_t len, int want)
{
    const uint8_t *end = annexb + len;
    const uint8_t *p = annexb;
    while (p < end) {
        int t = 0;
        const uint8_t *nal = next_annexb_nal(p, end, &t);
        if (!nal) {
            break;
        }
        if (t == want) {
            return 1;
        }
        p = nal + 1;
    }
    return 0;
}

static int h264_annexb_has_slice(const uint8_t *annexb, size_t len)
{
    return h264_annexb_has_nal_type(annexb, len, 1) || h264_annexb_has_nal_type(annexb, len, 5);
}

static int h265_annexb_has_vcl(const uint8_t *annexb, size_t len)
{
    const uint8_t *end = annexb + len;
    const uint8_t *p = annexb;

    while (p < end) {
        size_t nlen = 0;
        const uint8_t *nal = zms_annexb_find_nal(p, end, &nlen);
        int t;

        if (!nal || nlen == 0) {
            break;
        }
        t = (nal[0] >> 1) & 0x3f;
        if (t >= 0 && t < 32) {
            return 1;
        }
        p = nal + nlen;
    }
    return 0;
}

static ztk_err_t mp4_set_h265_video_config(zms_mp4_reader *r, const uint8_t *hvcc, size_t hvcc_len,
                                           int width, int height)
{
    if (!r || !hvcc || hvcc_len == 0 || !r->fifo) {
        return ZTK_ERR_INVALID;
    }
    if (zms_vod_buffer_set_video_config(r->fifo, hvcc, hvcc_len) != ZTK_OK) {
        return ZTK_ERR_NOMEM;
    }
    r->have_video_cfg = 1;
    if (mp4_owns_source_meta(r)) {
        r->src->has_video = 1;
        r->src->video.codec = ZMS_CODEC_H265;
        r->src->video.ready = 1;
        if (width > 0 && height > 0) {
            r->src->video.width = (uint32_t)width;
            r->src->video.height = (uint32_t)height;
        }
    }
    return ZTK_OK;
}

static uint8_t *mp4_scratch(zms_mp4_reader *r)
{
    if (!r) {
        return NULL;
    }
    if (!zms_buf_pool_slot_resize(&r->scratch, &r->scratch_cap, ZMS_MEDIA_IO_BUF_SIZE)) {
        return NULL;
    }
    return r->scratch;
}

static ztk_err_t mp4_try_set_h264_config(zms_mp4_reader *r, const uint8_t *annexb, size_t len)
{
    const uint8_t *sps = NULL;
    const uint8_t *pps = NULL;
    size_t sps_len = 0;
    size_t pps_len = 0;
    size_t cfg_len = 0;
    uint8_t *scratch;

    if (!r || r->have_video_cfg || !r->fifo) {
        return ZTK_OK;
    }
    if (!zms_h264_annexb_extract_sps_pps(annexb, len, &sps, &sps_len, &pps, &pps_len)) {
        return ZTK_OK;
    }
    scratch = mp4_scratch(r);
    if (!scratch) {
        return ZTK_ERR_NOMEM;
    }
    if (zms_rtmp_avc_seq_header(sps, sps_len, pps, pps_len, scratch, r->scratch_cap, &cfg_len) !=
        ZTK_OK) {
        return ZTK_ERR_INVALID;
    }
    if (zms_vod_buffer_set_video_config(r->fifo, scratch, cfg_len) != ZTK_OK) {
        return ZTK_ERR_NOMEM;
    }
    r->have_video_cfg = 1;
    if (mp4_owns_source_meta(r)) {
        r->src->has_video = 1;
        r->src->video.codec = ZMS_CODEC_H264;
        r->src->video.ready = 1;
        (void)zms_video_track_from_avc(&r->src->video, scratch, cfg_len);
    }
    return ZTK_OK;
}

static ztk_err_t mp4_write_annexb_video(zms_mp4_reader *r, const uint8_t *annexb, size_t len,
                                        uint32_t dts_ms, uint32_t pts_ms, int key)
{
    zms_frame frame;
    size_t cap = 0;
    uint8_t *buf;

    if (!r || !r->fifo || !annexb || len == 0) {
        return ZTK_ERR_INVALID;
    }

    if (r->drop_until_sync) {
        if (r->video_codec == ZMS_CODEC_H264) {
            if (!key && !zms_h264_annexb_is_sync_key(annexb, len)) {
                return ZTK_OK;
            }
            r->drop_until_sync = 0;
            if (!h264_annexb_has_slice(annexb, len)) {
                return ZTK_OK;
            }
        } else if (r->video_codec == ZMS_CODEC_H265) {
            if (!key && !zms_h265_annexb_is_sync_key(annexb, len)) {
                return ZTK_OK;
            }
            r->drop_until_sync = 0;
            if (!h265_annexb_has_vcl(annexb, len)) {
                return ZTK_OK;
            }
        }
    }

    if (!h264_annexb_has_slice(annexb, len) && r->video_codec == ZMS_CODEC_H264) {
        return ZTK_OK;
    }
    if (!h265_annexb_has_vcl(annexb, len) && r->video_codec == ZMS_CODEC_H265) {
        return ZTK_OK;
    }

    if (r->video_codec == ZMS_CODEC_H264) {
        (void)mp4_try_set_h264_config(r, annexb, len);
    }

    if (r->video_codec == ZMS_CODEC_H264 && !h264_annexb_has_slice(annexb, len)) {
        return ZTK_OK;
    }

    buf = (uint8_t *)zms_buf_pool_acquire(len, &cap);
    if (!buf) {
        return ZTK_ERR_NOMEM;
    }
    memcpy(buf, annexb, len);

    zms_frame_init(&frame);
    frame.data = buf;
    frame.size = len;
    frame.capacity = cap;
    frame.owned = 1;
    frame.codec = r->video_codec;
    frame.track = ZMS_TRACK_VIDEO;
    frame.dts_ms = dts_ms;
    frame.pts_ms = pts_ms ? pts_ms : dts_ms;
    if (r->video_codec == ZMS_CODEC_H264) {
        frame.keyframe = key || zms_h264_annexb_is_sync_key(annexb, len);
    } else if (r->video_codec == ZMS_CODEC_H265) {
        frame.keyframe = key || zms_h265_annexb_is_sync_key(annexb, len);
    } else {
        frame.keyframe = key;
    }
    if (zms_vod_buffer_write(r->fifo, &frame) != ZTK_OK) {
        zms_buf_pool_release(buf, cap);
        return ZTK_ERR_NOMEM;
    }
    return ZTK_OK;
}

static ztk_err_t mp4_ensure_annexb(zms_mp4_reader *r, size_t mp4_len)
{
    size_t need = mp4_len + 65536u;

    if (need < MP4_ANNEXB_MIN) {
        need = MP4_ANNEXB_MIN;
    }
    if (!r) {
        return ZTK_ERR_INVALID;
    }
    if (r->annexb_cap >= need) {
        return ZTK_OK;
    }
    {
        uint8_t *p = (uint8_t *)realloc(r->annexb_buf, need);
        if (!p) {
            return ZTK_ERR_NOMEM;
        }
        r->annexb_buf = p;
        r->annexb_cap = need;
    }
    return ZTK_OK;
}

static int mp4_h264_mp4_to_annexb(zms_mp4_reader *r, const uint8_t *mp4, size_t mp4_len,
                                  uint8_t *out, size_t cap)
{
    int n;
    int nalu;
    int cur;

    if (!r || !mp4 || mp4_len == 0 || !out) {
        return -1;
    }

    cur = zms_avc_config_nalu_length(&r->avc);
    if (cur > 0 && mp4_avcc_sample_valid(mp4, mp4_len, cur)) {
        n = zms_avc_config_mp4_to_annexb(&r->avc, mp4, mp4_len, out, cap);
        if (n > 0) {
            return n;
        }
    }
    for (nalu = 4; nalu >= 1; --nalu) {
        if (cur == nalu) {
            continue;
        }
        if (!mp4_avcc_sample_valid(mp4, mp4_len, nalu)) {
            continue;
        }
        n = zms_avc_nalu_to_annexb(nalu, mp4, mp4_len, out, cap);
        if (n > 0) {
            return n;
        }
    }
    return zms_avc_nalu_to_annexb(0, mp4, mp4_len, out, cap);
}

static int mp4_h265_mp4_to_annexb(zms_mp4_reader *r, const uint8_t *mp4, size_t mp4_len,
                                  uint8_t *out, size_t cap)
{
    int n;
    int lsm1;
    int cur;

    if (!r || !mp4 || mp4_len == 0 || !out) {
        return -1;
    }

    cur = zms_hevc_config_length_size(&r->hevc);
    if (mp4_avcc_sample_valid(mp4, mp4_len, cur)) {
        n = zms_hevc_config_mp4_to_annexb(&r->hevc, mp4, mp4_len, out, cap);
        if (n > 0) {
            return n;
        }
    }
    for (lsm1 = 3; lsm1 >= 0; --lsm1) {
        if (cur == lsm1 + 1) {
            continue;
        }
        if (!mp4_avcc_sample_valid(mp4, mp4_len, lsm1 + 1)) {
            continue;
        }
        n = zms_hevc_nalu_to_annexb(lsm1 + 1, mp4, mp4_len, out, cap);
        if (n > 0) {
            return n;
        }
    }
    return zms_hevc_nalu_to_annexb(1, mp4, mp4_len, out, cap);
}

static ztk_err_t feed_h264(zms_mp4_reader *r, const uint8_t *mp4, size_t mp4_len, int64_t dts_ms,
                           int64_t pts_ms, int key)
{
    int n;
    int avcc_nalu = r ? zms_avc_config_nalu_length(&r->avc) : 4;

    if (avcc_nalu <= 0) {
        avcc_nalu = 4;
    }
    if (mp4_sample_is_annexb(mp4, mp4_len, avcc_nalu)) {
        return mp4_write_annexb_video(r, mp4, mp4_len, (uint32_t)dts_ms, (uint32_t)pts_ms, key);
    }
    if (mp4_ensure_annexb(r, mp4_len) != ZTK_OK) {
        return ZTK_ERR_NOMEM;
    }
    n = mp4_h264_mp4_to_annexb(r, mp4, mp4_len, r->annexb_buf, r->annexb_cap);
    if (n <= 0) {
        return ZTK_ERR_INVALID;
    }
    return mp4_write_annexb_video(r, r->annexb_buf, (size_t)n, (uint32_t)dts_ms, (uint32_t)pts_ms,
                                  key);
}

static ztk_err_t feed_h265(zms_mp4_reader *r, const uint8_t *mp4, size_t mp4_len, int64_t dts_ms,
                           int64_t pts_ms, int key)
{
    int n;
    int avcc_nalu = r ? zms_hevc_config_length_size(&r->hevc) : 4;

    if (avcc_nalu <= 0) {
        avcc_nalu = 4;
    }
    if (mp4_sample_is_annexb(mp4, mp4_len, avcc_nalu)) {
        return mp4_write_annexb_video(r, mp4, mp4_len, (uint32_t)dts_ms, (uint32_t)pts_ms, key);
    }
    if (mp4_ensure_annexb(r, mp4_len) != ZTK_OK) {
        return ZTK_ERR_NOMEM;
    }
    n = mp4_h265_mp4_to_annexb(r, mp4, mp4_len, r->annexb_buf, r->annexb_cap);
    if (n <= 0) {
        return ZTK_ERR_INVALID;
    }
    return mp4_write_annexb_video(r, r->annexb_buf, (size_t)n, (uint32_t)dts_ms, (uint32_t)pts_ms,
                                  key);
}

static ztk_err_t feed_aac(zms_mp4_reader *r, const uint8_t *aac, size_t len, int64_t dts_ms,
                          int64_t pts_ms)
{
    zms_frame frame;
    size_t cap = 0;
    uint8_t *buf;

    if (!r || !r->fifo || !aac || len == 0) {
        return ZTK_ERR_INVALID;
    }

    buf = (uint8_t *)zms_buf_pool_acquire(len, &cap);
    if (!buf) {
        return ZTK_ERR_NOMEM;
    }
    memcpy(buf, aac, len);

    zms_frame_init(&frame);
    frame.data = buf;
    frame.size = len;
    frame.capacity = cap;
    frame.owned = 1;
    frame.codec = ZMS_CODEC_AAC;
    frame.track = ZMS_TRACK_AUDIO;
    frame.dts_ms = (uint32_t)dts_ms;
    frame.pts_ms = (uint32_t)(pts_ms ? pts_ms : dts_ms);
    if (zms_vod_buffer_write(r->fifo, &frame) != ZTK_OK) {
        zms_buf_pool_release(buf, cap);
        return ZTK_ERR_NOMEM;
    }
    return ZTK_OK;
}

static ztk_err_t feed_sample(zms_mp4_reader *r, uint32_t track, const void *buf, size_t bytes,
                             int64_t dts_ms, int64_t pts_ms, int key)
{
    if (!r || !r->fifo || !buf || bytes == 0) {
        return ZTK_ERR_INVALID;
    }
    if (r->drop_until_sync && track != r->video_track) {
        return ZTK_OK;
    }
    if (!pacing_ready(r, dts_ms)) {
        if (pending_set(&r->pending, track, dts_ms, pts_ms, key, buf, bytes) != 0) {
            return ZTK_ERR_NOMEM;
        }
        return ZTK_OK;
    }

    if (track == r->video_track) {
        if (r->video_codec == ZMS_CODEC_H264) {
            return feed_h264(r, (const uint8_t *)buf, bytes, dts_ms, pts_ms, key);
        }
        if (r->video_codec == ZMS_CODEC_H265) {
            return feed_h265(r, (const uint8_t *)buf, bytes, dts_ms, pts_ms, key);
        }
        return ZTK_ERR_NOT_IMPL;
    }
    if (track == r->audio_track && r->audio_codec == ZMS_CODEC_AAC) {
        return feed_aac(r, (const uint8_t *)buf, bytes, dts_ms, pts_ms);
    }
    return ZTK_ERR_NOT_IMPL;
}

static void mp4_apply_video_config(zms_mp4_reader *r, const zms_container_packet *pkt)
{
    const uint8_t *extra;
    size_t bytes;
    int width;
    int height;

    if (!r || !pkt || !pkt->config) {
        return;
    }
    extra = pkt->data;
    bytes = pkt->len;
    width = (int)pkt->width;
    height = (int)pkt->height;
    r->video_track = pkt->track_id;
    r->video_codec = pkt->codec;
    if (mp4_owns_source_meta(r)) {
        r->src->has_video = 1;
        r->src->video.codec = r->video_codec;
    }
    if (r->video_codec == ZMS_CODEC_H264 && extra && bytes > 0) {
        uint8_t flv_cfg[512];

        if (blob_is_annexb(extra, bytes)) {
            (void)zms_avc_config_load_annexb(&r->avc, extra, bytes);
            (void)mp4_try_set_h264_config(r, extra, bytes);
        } else {
            (void)zms_avc_config_load_record(&r->avc, extra, bytes);
            if (r->fifo && bytes + 5 <= sizeof(flv_cfg)) {
                flv_cfg[0] = 0x17;
                flv_cfg[1] = 0x00;
                flv_cfg[2] = flv_cfg[3] = flv_cfg[4] = 0;
                memcpy(flv_cfg + 5, extra, bytes);
                if (zms_vod_buffer_set_video_config(r->fifo, flv_cfg, bytes + 5) == ZTK_OK) {
                    r->have_video_cfg = 1;
                    if (mp4_owns_source_meta(r)) {
                        r->src->video.ready = 1;
                        (void)zms_video_track_from_avc(&r->src->video, flv_cfg, bytes + 5);
                        if (width > 0 && height > 0) {
                            r->src->video.width = (uint32_t)width;
                            r->src->video.height = (uint32_t)height;
                        }
                    }
                }
            }
        }
    } else if (r->video_codec == ZMS_CODEC_H265 && extra && bytes > 0) {
        if (blob_is_annexb(extra, bytes)) {
            uint8_t hvcc[1024];
            size_t hvcc_len = 0;

            if (zms_h265_hvcc_from_annexb(extra, bytes, hvcc, sizeof(hvcc), &hvcc_len) == ZTK_OK &&
                hvcc_len > 0) {
                (void)zms_hevc_config_load_record(&r->hevc, hvcc, hvcc_len);
                (void)mp4_set_h265_video_config(r, hvcc, hvcc_len, width, height);
            }
        } else {
            (void)zms_hevc_config_load_record(&r->hevc, extra, bytes);
            (void)mp4_set_h265_video_config(r, extra, bytes, width, height);
        }
    }
    ztk_info("mp4_reader: video track=%u codec=%s %dx%d", (unsigned)pkt->track_id,
             zms_codec_name(r->video_codec),
             width > 0 ? width : (r->src ? (int)r->src->video.width : 0),
             height > 0 ? height : (r->src ? (int)r->src->video.height : 0));
}

static void mp4_apply_audio_config(zms_mp4_reader *r, const zms_container_packet *pkt)
{
    const uint8_t *extra;
    size_t bytes;
    int sample_rate;
    int channel_count;
    uint8_t asc[16];
    size_t asc_len;
    uint8_t flv_cfg[32];

    if (!r || !pkt || !pkt->config) {
        return;
    }
    extra = pkt->data;
    bytes = pkt->len;
    sample_rate = (int)pkt->sample_rate;
    channel_count = (int)pkt->channels;
    if (sample_rate < 8000) {
        sample_rate = 44100;
    }
    r->audio_track = pkt->track_id;
    r->audio_codec = pkt->codec;
    if (mp4_owns_source_meta(r)) {
        r->src->has_audio = 1;
        r->src->audio.codec = r->audio_codec;
        r->src->audio.sample_rate = sample_rate > 0 ? (uint32_t)sample_rate : 44100;
        r->src->audio.channels = channel_count > 0 ? (uint8_t)channel_count : 2;
        r->src->audio.ready = 1;
    }
    if (r->audio_codec == ZMS_CODEC_AAC && r->fifo) {
        if (extra && bytes > 0) {
            (void)zms_aac_config_load_asc(&r->aac, extra, bytes);
        } else {
            zms_aac_config_set_defaults(&r->aac, sample_rate, channel_count);
        }
        asc_len = zms_aac_config_save_asc(&r->aac, asc, sizeof(asc));
        if (asc_len > 0 && asc_len + 2 <= sizeof(flv_cfg)) {
            flv_cfg[0] = 0xaf;
            flv_cfg[1] = 0x00;
            memcpy(flv_cfg + 2, asc, asc_len);
            if (zms_vod_buffer_set_audio_config(r->fifo, flv_cfg, asc_len + 2) == ZTK_OK) {
                r->have_audio_cfg = 1;
            }
        }
    }
    ztk_info("mp4_reader: audio track=%u codec=%s %uHz", (unsigned)pkt->track_id,
             zms_codec_name(r->audio_codec), sample_rate > 0 ? (unsigned)sample_rate : 0);
}

static void mp4_container_cb(const zms_container_packet *pkt, void *user)
{
    zms_mp4_reader *r = (zms_mp4_reader *)user;

    if (!r || !pkt || pkt->kind != ZMS_CONTAINER_PKT_MP4_SAMPLE) {
        return;
    }
    if (pkt->config) {
        if (pkt->codec == ZMS_CODEC_H264 || pkt->codec == ZMS_CODEC_H265) {
            mp4_apply_video_config(r, pkt);
        } else if (pkt->codec == ZMS_CODEC_AAC) {
            mp4_apply_audio_config(r, pkt);
        }
        return;
    }
    if (!pkt->data || pkt->len == 0) {
        return;
    }
    if (!pacing_ready(r, (int64_t)pkt->dts_ms)) {
        (void)pending_set(&r->pending, pkt->track_id, (int64_t)pkt->dts_ms, (int64_t)pkt->pts_ms,
                          pkt->key, pkt->data, pkt->len);
        return;
    }
    (void)feed_sample(r, pkt->track_id, pkt->data, pkt->len, (int64_t)pkt->dts_ms,
                      (int64_t)pkt->pts_ms, pkt->key);
}

static int mp4_flush_pending(zms_mp4_reader *r)
{
    struct zms_mp4_pending_slot *p;
    ztk_err_t err;

    if (!r || !r->pending.used) {
        return 0;
    }
    p = &r->pending;
    if (!pacing_ready(r, p->dts_ms)) {
        return 0;
    }
    err = feed_sample(r, p->track, p->data, p->len, p->dts_ms, p->pts_ms, p->key);
    pending_clear(p);
    return err == ZTK_OK ? 1 : -1;
}

static int mp4_demux_pump(zms_mp4_reader *r)
{
    if (!r || !r->mp4_ops || !r->mp4_demux || !r->mp4_ops->pump || r->eof) {
        return 0;
    }
    return r->mp4_ops->pump(r->mp4_demux);
}

static int mp4_pump_once(zms_mp4_reader *r)
{
    int pushed = 0;
    int n;

    if (!r || !r->mp4_demux || r->eof) {
        return 0;
    }

    while (mp4_flush_pending(r) > 0) {
        ++pushed;
    }

    if (r->fifo && !r->drop_until_sync && r->fifo_high_water > 0 &&
        zms_vod_buffer_pending(r->fifo) >= r->fifo_high_water) {
        return pushed;
    }

    n = mp4_demux_pump(r);
    if (n > 0) {
        return pushed + 1;
    }
    if (n == 0) {
        r->eof = 1;
        if (r->loop && r->mp4_ops && r->mp4_ops->seek) {
            int64_t zero = 0;
            r->eof = 0;
            r->clock_armed = 0;
            pending_clear(&r->pending);
            if (r->mp4_ops->seek(r->mp4_demux, &zero) == ZTK_OK) {
                return mp4_pump_once(r);
            }
        }
        return pushed;
    }
    return pushed > 0 ? pushed : -1;
}

static int mp4_pump(zms_mp4_reader *r)
{
    int pushed = 0;
    int n;

    if (!r || !r->mp4_demux || r->eof) {
        return 0;
    }

    while (mp4_flush_pending(r) > 0) {
        ++pushed;
    }

    while (r->fifo && !r->drop_until_sync && r->fifo_high_water > 0 &&
           zms_vod_buffer_pending(r->fifo) >= r->fifo_high_water) {
        return pushed;
    }

    for (;;) {
        n = mp4_demux_pump(r);
        if (n > 0) {
            ++pushed;
            if (r->fifo && !r->drop_until_sync && r->fifo_high_water > 0 &&
                zms_vod_buffer_pending(r->fifo) >= r->fifo_high_water) {
                return pushed;
            }
            continue;
        }
        if (n == 0) {
            r->eof = 1;
            if (r->loop && r->mp4_ops && r->mp4_ops->seek) {
                int64_t zero = 0;
                r->eof = 0;
                r->clock_armed = 0;
                pending_clear(&r->pending);
                if (r->mp4_ops->seek(r->mp4_demux, &zero) == ZTK_OK) {
                    continue;
                }
            }
            return pushed;
        }
        return pushed > 0 ? pushed : -1;
    }
}

static void mp4_tick(void *user)
{
    zms_mp4_reader *r = (zms_mp4_reader *)user;
    if (!r || r->pump_hold) {
        return;
    }
    (void)mp4_pump(r);
}

static zms_container_id file_container_id(const char *path)
{
    size_t n;

    if (!path) {
        return ZMS_CONTAINER_MP4;
    }
    n = strlen(path);
    if (n >= 4) {
        const char *ext = path + n - 4;
        if (zms_file_stricmp(ext, ".flv") == 0) {
            return ZMS_CONTAINER_FLV_FILE;
        }
        if (zms_file_stricmp(ext, ".mkv") == 0 || zms_file_stricmp(ext, ".mka") == 0) {
            return ZMS_CONTAINER_MKV;
        }
        if (zms_file_stricmp(ext, ".mov") == 0 || zms_file_stricmp(ext, ".m4v") == 0) {
            return ZMS_CONTAINER_MP4;
        }
    }
    return ZMS_CONTAINER_MP4;
}

zms_mp4_reader *zms_mp4_reader_open(const zms_mp4_reader_opts *opts)
{
    zms_mp4_reader *r;
    zms_container_demux_opts cfg;
    zms_container_id cid;

    if (!opts || !opts->file_path || !opts->fifo) {
        return NULL;
    }

    r = (zms_mp4_reader *)calloc(1, sizeof(*r));
    if (!r) {
        return NULL;
    }

    snprintf(r->path, sizeof(r->path), "%s", opts->file_path);
    r->fifo = opts->fifo;
    r->src = opts->source;
    r->owner_ctx = opts->owner_ctx;
    r->speed = opts->speed > 0 ? opts->speed : 1.0;
    r->loop = opts->loop;
    r->fifo_high_water = opts->fifo_high_water > 0 ? opts->fifo_high_water : MP4_FIFO_HW;

    cid = file_container_id(r->path);
    r->container_id = cid;
    r->mp4_ops = zms_container_demuxer_find(cid);
    if (!r->mp4_ops || !r->mp4_ops->create || !r->mp4_ops->open_file) {
        free(r);
        return NULL;
    }

    memset(&cfg, 0, sizeof(cfg));
    cfg.id = cid;
    cfg.on_packet = mp4_container_cb;
    cfg.user = r;
    r->mp4_demux = r->mp4_ops->create(&cfg);
    if (!r->mp4_demux) {
        free(r);
        return NULL;
    }

    if (r->mp4_ops->open_file(r->mp4_demux, r->path) != ZTK_OK) {
        ztk_warn("file_reader: open failed: %s", r->path);
        r->mp4_ops->destroy(r->mp4_demux);
        free(r);
        return NULL;
    }

    if (cid == ZMS_CONTAINER_MKV) {
        zms_container_demux_mkv_codecs(r->mp4_demux, &r->video_codec, &r->audio_codec);
    } else if (cid == ZMS_CONTAINER_FLV_FILE) {
        zms_container_demux_flv_file_codecs(r->mp4_demux, &r->video_codec, &r->audio_codec);
    } else {
        zms_container_demux_mp4_codecs(r->mp4_demux, &r->video_codec, &r->audio_codec);
    }

    if (r->video_codec != ZMS_CODEC_H264 && r->video_codec != ZMS_CODEC_H265) {
        ztk_warn("file_reader: unsupported video codec %s (%s)", zms_codec_name(r->video_codec),
                 r->path);
        r->mp4_ops->destroy(r->mp4_demux);
        free(r);
        return NULL;
    }

    r->duration_ms = r->mp4_ops->duration_ms ? r->mp4_ops->duration_ms(r->mp4_demux) : 0;

    ztk_info("file_reader: open %s container=%s duration=%llu ms", r->path, zms_container_name(cid),
             (unsigned long long)r->duration_ms);

    /* FLV 索引经 container/mp4 遍历 MP4 sample table；仅 MP4 ctx 适用。 */
    if (r->src && r->src->vod_buffer && r->src->publish_origin == 8 &&
        r->container_id == ZMS_CONTAINER_MP4 && mp4_owns_source_meta(r)) {
        size_t vlen = 0;
        size_t alen = 0;
        size_t meta = 384;
        zms_vod_flv_index *idx = NULL;
        int round;

        (void)zms_vod_buffer_video_config(r->fifo, &vlen);
        (void)zms_vod_buffer_audio_config(r->fifo, &alen);
        for (round = 0; round < 4; ++round) {
            if (idx) {
                zms_vod_flv_index_free(idx);
            }
            idx = zms_vod_flv_index_build(r->mp4_demux, vlen, alen, meta);
            if (!idx) {
                break;
            }
            {
                size_t meta2 = zms_vod_flv_metadata_body_size(r->src, r->duration_ms / 1000.0, idx);
                if (meta2 == 0 || meta2 == meta) {
                    break;
                }
                meta = meta2;
            }
        }
        r->flv_index = idx;
    }
    return r;
}

static void mp4_prefill_limited(zms_mp4_reader *r, int max_loops)
{
    int i;

    if (!r || !r->fifo || max_loops <= 0) {
        return;
    }
    for (i = 0; i < max_loops; ++i) {
        if (r->fifo_high_water > 0 && zms_vod_buffer_pending(r->fifo) >= r->fifo_high_water) {
            break;
        }
        if (mp4_pump(r) <= 0) {
            break;
        }
    }
}

static void mp4_prefill(zms_mp4_reader *r)
{
    mp4_prefill_limited(r, 4096);
}

void zms_mp4_reader_prefill(zms_mp4_reader *r)
{
    mp4_prefill(r);
}

uint64_t zms_mp4_reader_seek_ms(zms_mp4_reader *r, uint64_t ms)
{
    int i;
    int64_t ts;
    int timer_was = 0;

    if (!r || !r->fifo) {
        return 0;
    }
    if (r->duration_ms > 0 && ms > r->duration_ms) {
        ms = r->duration_ms;
    }
    if (r->timer) {
        ztk_timer_stop(r->timer);
        timer_was = 1;
    }
    zms_vod_buffer_reset(r->fifo);
    r->eof = 0;
    r->clock_armed = 0;
    r->clock0_wall = 0;
    r->dts0 = 0;
    r->drop_until_sync =
        (r->video_codec == ZMS_CODEC_H264 || r->video_codec == ZMS_CODEC_H265) ? 1 : 0;
    pending_clear(&r->pending);
    ts = (int64_t)ms;
    if (!r->mp4_demux || !r->mp4_ops || !r->mp4_ops->seek ||
        r->mp4_ops->seek(r->mp4_demux, &ts) != ZTK_OK) {
        int64_t zero = 0;
        r->drop_until_sync = 0;
        if (r->mp4_demux && r->mp4_ops && r->mp4_ops->seek) {
            (void)r->mp4_ops->seek(r->mp4_demux, &zero);
        }
        ms = 0;
    } else {
        ms = (uint64_t)(ts < 0 ? 0 : ts);
    }
    for (i = 0; r->drop_until_sync && i < MP4_SEEK_SYNC_MAX && !r->eof; ++i) {
        if (mp4_pump_once(r) <= 0) {
            break;
        }
    }
    if (r->drop_until_sync) {
        ztk_warn("mp4_reader: no sync sample after seek %llu ms", (unsigned long long)ms);
    }
    r->drop_until_sync = 0;
    if (r->eof && r->mp4_demux && r->mp4_ops && r->mp4_ops->seek) {
        int64_t rewind = (int64_t)ms;
        r->eof = 0;
        pending_clear(&r->pending);
        (void)r->mp4_ops->seek(r->mp4_demux, &rewind);
    }
    mp4_prefill_limited(r, MP4_SEEK_PREFILL_MAX);
    if (timer_was && r->poller) {
        r->timer = ztk_timer_start(r->poller, MP4_TICK_MS, 1, mp4_tick, r);
    }
    return ms;
}

void zms_mp4_reader_prepare_play(zms_mp4_reader *r)
{
    (void)zms_mp4_reader_seek_ms(r, 0);
}

static void mp4_bind_poller(zms_mp4_reader *r, ztk_poller *poller, int max_prefill_loops)
{
    if (!r) {
        return;
    }
    if (r->timer) {
        ztk_timer_stop(r->timer);
        r->timer = NULL;
    }
    r->poller = poller;
    if (poller) {
        r->timer = ztk_timer_start(poller, MP4_TICK_MS, 1, mp4_tick, r);
        if (max_prefill_loops <= 0) {
            mp4_prefill(r);
        } else {
            mp4_prefill_limited(r, max_prefill_loops);
        }
    }
}

void zms_mp4_reader_bind_poller(zms_mp4_reader *r, ztk_poller *poller)
{
    mp4_bind_poller(r, poller, 0);
}

void zms_mp4_reader_bind_poller_lite(zms_mp4_reader *r, ztk_poller *poller)
{
    mp4_bind_poller(r, poller, MP4_SEEK_PREFILL_MAX);
}

void zms_mp4_reader_close(zms_mp4_reader *r)
{
    if (!r) {
        return;
    }
    if (r->timer) {
        ztk_timer_stop(r->timer);
        r->timer = NULL;
    }
    pending_clear(&r->pending);
    zms_buf_pool_slot_clear(&r->scratch, &r->scratch_cap);
    free(r->annexb_buf);
    r->annexb_buf = NULL;
    r->annexb_cap = 0;
    if (r->flv_index && (!r->src || mp4_owns_source_meta(r))) {
        zms_vod_flv_index_free(r->flv_index);
    }
    r->flv_index = NULL;
    if (r->mp4_ops && r->mp4_demux) {
        r->mp4_ops->destroy(r->mp4_demux);
    }
    free(r);
}

zms_media_source *zms_mp4_reader_source(zms_mp4_reader *r)
{
    return r ? r->src : NULL;
}

const char *zms_mp4_reader_file_path(const zms_mp4_reader *r)
{
    return (r && r->path[0]) ? r->path : NULL;
}

uint64_t zms_mp4_reader_duration_ms(const zms_mp4_reader *r)
{
    return r ? r->duration_ms : 0;
}

const zms_vod_flv_index *zms_mp4_reader_flv_index(zms_mp4_reader *r)
{
    if (!r) {
        return NULL;
    }
    if (r->src && r->src->publisher_ctx && r->src->publisher_ctx != r &&
        r->src->publisher_ctx != r->owner_ctx) {
        return NULL;
    }
    return r->flv_index;
}

void zms_mp4_reader_set_pump_hold(zms_mp4_reader *r, int hold)
{
    if (!r) {
        return;
    }
    r->pump_hold = hold ? 1 : 0;
}

int zms_mp4_reader_pump(zms_mp4_reader *r)
{
    if (!r || !r->mp4_demux || r->eof) {
        return 0;
    }
    return mp4_pump_once(r);
}
