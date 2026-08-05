/**
 * @file mpegts_mux_feed.c
 * @brief 共享 MPEG-TS codec 喂入辅助（封装 zmk libmpeg/mpeg4）。
 *        供 HLS 分段与 SRT 连续 TS mux 路径复用，业务/play 文件勿直接包含 lib 头。
 *
 * Copyright (c) zero-media-server
 */
#include "zms/media/container/mpegts/mpegts_mux_feed.h"
#include "zms/egress/egress_sidecar_param_sets.h"
#include "zms/media/codec/h264/h264_es.h"
#include "zms/media/codec/h265/h265_es.h"
#include "zms/media/codec/aac/aac_over_rtp.h"
#include "zms/media/codec/aac/aac_config.h"
#include "mpeg-proto.h"
#include "mpeg4-hevc.h"
#include <string.h>

int zms_mpegts_hevc_esinfo(const uint8_t *hvcc, size_t hvcc_len, uint8_t *out, size_t cap)
{
    struct mpeg4_hevc_t hevc;
    uint64_t v;
    uint64_t copied44;
    uint8_t *p;

    if (!hvcc || hvcc_len < 7 || !out || cap < 6 + 2 + 13) {
        return -1;
    }
    memset(&hevc, 0, sizeof(hevc));
    if (mpeg4_hevc_decoder_configuration_record_load(hvcc, hvcc_len, &hevc) <= 0) {
        return -1;
    }

    p = out;
    *p++ = 0x05;
    *p++ = 4;
    memcpy(p, "HEVC", 4);
    p += 4;

    *p++ = 0x38;
    *p++ = 13;
    *p++ = (uint8_t)((hevc.general_profile_space << 6) | (hevc.general_tier_flag << 5) |
                     (hevc.general_profile_idc & 0x1f));
    p[0] = (uint8_t)((hevc.general_profile_compatibility_flags >> 24) & 0xff);
    p[1] = (uint8_t)((hevc.general_profile_compatibility_flags >> 16) & 0xff);
    p[2] = (uint8_t)((hevc.general_profile_compatibility_flags >> 8) & 0xff);
    p[3] = (uint8_t)(hevc.general_profile_compatibility_flags & 0xff);
    p += 4;

    copied44 = (hevc.general_constraint_indicator_flags >> 4) & 0xFFFFFFFFFFFULL;
    v = (1ULL << 63) | (1ULL << 60) | (copied44 << 16) | ((uint64_t)hevc.general_level_idc << 8);
    p[0] = (uint8_t)((v >> 56) & 0xff);
    p[1] = (uint8_t)((v >> 48) & 0xff);
    p[2] = (uint8_t)((v >> 40) & 0xff);
    p[3] = (uint8_t)((v >> 32) & 0xff);
    p[4] = (uint8_t)((v >> 24) & 0xff);
    p[5] = (uint8_t)((v >> 16) & 0xff);
    p[6] = (uint8_t)((v >> 8) & 0xff);
    p[7] = (uint8_t)(v & 0xff);
    return (int)(p + 8 - out);
}

void zms_mpegts_feed_h264(const zms_mpegts_mux_feed_view *f, const uint8_t *annexb, size_t len,
                          uint32_t dts_ms, int keyframe)
{
    const uint8_t *mux_ptr = annexb;
    size_t mux_len = len;
    uint32_t rel;

    if (!f || !f->sink || !f->mux_av || !f->video_armed || !annexb || len < 4) {
        return;
    }
    if (!*f->video_armed) {
        if (!keyframe) {
            return;
        }
        *f->video_armed = 1;
        zms_mux_av_timeline_lock_origin(f->mux_av, dts_ms);
    }
    if (keyframe && f->params && f->params->sps && f->params->pps && f->mux_buf) {
        size_t full = 0;
        if (zms_h264_annexb_prepend_sps_pps(f->params->sps, f->params->sps_len, f->params->pps,
                                            f->params->pps_len, annexb, len, f->mux_buf,
                                            f->mux_buf_cap, &full) == ZTK_OK &&
            full) {
            mux_ptr = f->mux_buf;
            mux_len = full;
        }
    }
    rel = zms_mux_av_timeline_pts(f->mux_av, ZMS_TRACK_VIDEO, dts_ms);
    (void)f->sink(f->sink_user, PSI_STREAM_H264, mux_ptr, mux_len, (int64_t)rel, (int64_t)rel,
                  keyframe ? ZMS_CONTAINER_MUX_FLAG_KEYFRAME : 0);
}

void zms_mpegts_feed_h265(const zms_mpegts_mux_feed_view *f, const uint8_t *annexb, size_t len,
                          uint32_t dts_ms, int keyframe)
{
    const uint8_t *mux_ptr = annexb;
    size_t mux_len = len;
    int sync;
    uint32_t rel;

    if (!f || !f->sink || !f->mux_av || !f->video_armed || !annexb || len < 4) {
        return;
    }
    sync = keyframe || zms_h265_annexb_is_sync_key(annexb, len);
    if (!*f->video_armed) {
        if (!sync) {
            return;
        }
        *f->video_armed = 1;
        zms_mux_av_timeline_lock_origin(f->mux_av, dts_ms);
    }
    if (sync && f->params && f->params->sps_len > 0 && f->params->pps_len > 0 && f->mux_buf) {
        size_t full = 0;
        if (zms_h265_annexb_build_rtp_au(f->params->vps_len ? f->params->vps : NULL,
                                         f->params->vps_len, f->params->sps, f->params->sps_len,
                                         f->params->pps, f->params->pps_len, annexb, len, 1,
                                         f->mux_buf, f->mux_buf_cap, &full) == ZTK_OK &&
            full > 0) {
            mux_ptr = f->mux_buf;
            mux_len = full;
        }
    } else if (f->mux_buf) {
        size_t vcl = 0;
        if (zms_h265_annexb_copy_vcl(annexb, len, f->mux_buf, f->mux_buf_cap, &vcl) == ZTK_OK &&
            vcl > 0) {
            mux_ptr = f->mux_buf;
            mux_len = vcl;
        }
    }
    rel = zms_mux_av_timeline_pts(f->mux_av, ZMS_TRACK_VIDEO, dts_ms);
    (void)f->sink(f->sink_user, PSI_STREAM_H265, mux_ptr, mux_len, (int64_t)rel, (int64_t)rel,
                  sync ? ZMS_CONTAINER_MUX_FLAG_KEYFRAME : 0);
}

typedef struct {
    const zms_mpegts_mux_feed_view *f;
    int stream_type;
    uint32_t base_dts_ms;
    unsigned idx;
} mpegts_aac_ctx;

static int mpegts_feed_aac_frame(const uint8_t *au, size_t len, void *user)
{
    mpegts_aac_ctx *c = (mpegts_aac_ctx *)user;
    const zms_mpegts_mux_feed_view *f;
    uint32_t rel;
    int hdr;

    if (!c || !c->f || !au || len == 0 || len > 0x1fff) {
        return -1;
    }
    f = c->f;
    if (!f->aac || !f->adts) {
        return -1;
    }
    hdr = zms_aac_config_adts_header((const zms_aac_config *)f->aac, (uint16_t)len, f->adts,
                                     f->adts_cap);
    if (hdr < 7 || (size_t)hdr + len > f->adts_cap) {
        return -1;
    }
    memcpy(f->adts + hdr, au, len);
    rel = zms_mux_av_timeline_pts(f->mux_av, ZMS_TRACK_AUDIO, c->base_dts_ms + c->idx * 23u);
    (void)f->sink(f->sink_user, c->stream_type, f->adts, (size_t)hdr + len, (int64_t)rel,
                  (int64_t)rel, 0);
    c->idx++;
    return 0;
}

void zms_mpegts_feed_aac(const zms_mpegts_mux_feed_view *f, int stream_type, const uint8_t *es,
                         size_t es_len, uint32_t dts_ms)
{
    mpegts_aac_ctx c;

    if (!f || !f->sink || !f->mux_av || !es || es_len == 0) {
        return;
    }
    c.f = f;
    c.stream_type = stream_type;
    c.base_dts_ms = dts_ms;
    c.idx = 0;
    (void)zms_aac_es_foreach_frame(es, es_len, mpegts_feed_aac_frame, &c);
}
