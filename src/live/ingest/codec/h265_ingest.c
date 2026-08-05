#include "zms/live/ingest/common/stream_ingest.h"
#include "zms/live/ingest/common/ingest_codec.h"
#include "zms/media/codec/h265/h265_es.h"
#include "zms/media/codec/bitstream/annexb.h"
#include "zms/util/buf_pool.h"
#include "live/ingest/common/ingest_internal.h"
#include "ztk/util/buf.h"
#include "ztk/util/log.h"
#include <string.h>

static ztk_err_t hevc_au_flush(zms_live_ingest *ch)
{
    zms_frame frame;
    ztk_buf *buf;
    void *dst;

    if (!ch || !ch->source->gop_queue || !ch->hevc_au.active || ch->hevc_au.len == 0) {
        return ZTK_OK;
    }

    /* GOP ring 可跨 poller 读：必须走共享池，勿 alloc_local */
    buf = ztk_buf_alloc(ch->hevc_au.len);
    if (!buf) {
        return ZTK_ERR_NOMEM;
    }
    dst = (void *)ztk_buf_data(buf);
    memcpy(dst, ch->hevc_au.buf, ch->hevc_au.len);
    ztk_buf_set_len(buf, ch->hevc_au.len);

    zms_frame_init(&frame);
    frame.data = (uint8_t *)dst;
    frame.size = ch->hevc_au.len;
    frame.dts_ms = ch->hevc_au.dts_ms;
    frame.pts_ms = ch->hevc_au.pts_ms ? ch->hevc_au.pts_ms : ch->hevc_au.dts_ms;
    frame.codec = ZMS_CODEC_H265;
    frame.track = ZMS_TRACK_VIDEO;
    frame.config_frame = 0;
    frame.keyframe = ch->hevc_au.key;

    ch->hevc_au.active = 0;
    ch->hevc_au.len = 0;
    ch->hevc_au.key = 0;

    return live_ingest_write_frame_buf(ch, buf, &frame);
}

static ztk_err_t hevc_au_begin(zms_live_ingest *ch, uint32_t raw_tag_dts_ms, uint32_t dts_ms,
                               uint32_t pts_ms)
{
    if (!ch) {
        return ZTK_ERR_INVALID;
    }
    if (ch->hevc_au.active && ch->hevc_au.raw_tag_dts_ms != raw_tag_dts_ms) {
        ztk_err_t err = hevc_au_flush(ch);
        if (err != ZTK_OK) {
            return err;
        }
    }
    if (!ch->hevc_au.active) {
        ch->hevc_au.raw_tag_dts_ms = raw_tag_dts_ms;
        ch->hevc_au.dts_ms = dts_ms;
        ch->hevc_au.pts_ms = pts_ms;
        ch->hevc_au.active = 1;
        ch->hevc_au.key = 0;
        ch->hevc_au.len = 0;
    } else if (ch->hevc_au.raw_tag_dts_ms == raw_tag_dts_ms) {
        ch->hevc_au.dts_ms = dts_ms;
        ch->hevc_au.pts_ms = pts_ms;
    }
    return ZTK_OK;
}

static ztk_err_t hevc_au_append_nal(zms_live_ingest *ch, const uint8_t *nal, size_t nlen, int idr)
{
    size_t need;

    if (!ch || !nal || nlen == 0) {
        return ZTK_ERR_INVALID;
    }
    need = ch->hevc_au.len + 4 + nlen;
    if (!ingest_slot_resize(ch, &ch->hevc_au.buf, &ch->hevc_au.cap, need)) {
        return ZTK_ERR_NOMEM;
    }
    ch->hevc_au.buf[ch->hevc_au.len++] = 0;
    ch->hevc_au.buf[ch->hevc_au.len++] = 0;
    ch->hevc_au.buf[ch->hevc_au.len++] = 0;
    ch->hevc_au.buf[ch->hevc_au.len++] = 1;
    memcpy(ch->hevc_au.buf + ch->hevc_au.len, nal, nlen);
    ch->hevc_au.len += nlen;
    if (idr) {
        ch->hevc_au.key = 1;
    }
    return ZTK_OK;
}

ztk_err_t zms_live_ingest_h265_hevc_au_flush(zms_live_ingest *ch)
{
    return hevc_au_flush(ch);
}

void zms_live_ingest_h265_hevc_au_reset(zms_live_ingest *in)
{
    if (!in) {
        return;
    }
    (void)hevc_au_flush(in);
    ingest_slot_clear(in, &in->hevc_au.buf, &in->hevc_au.cap);
    in->hevc_au.len = 0;
    in->hevc_au.active = 0;
    in->hevc_au.key = 0;
}

ztk_err_t zms_live_ingest_set_h265_vps_sps_pps(zms_live_ingest *ch, const uint8_t *vps,
                                               size_t vps_len, const uint8_t *sps, size_t sps_len,
                                               const uint8_t *pps, size_t pps_len)
{
    uint8_t *buf;
    size_t cfg_len = 0;
    ztk_err_t err;

    if (!ch || !ch->source || !sps || !pps || sps_len == 0 || pps_len == 0) {
        return ZTK_ERR_INVALID;
    }

    buf = live_ingest_work_buf(ch);
    err = zms_h265_hvcc_from_param_sets(vps, vps_len, sps, sps_len, pps, pps_len, buf,
                                        ZMS_LIVE_INGEST_WORK_BUF, &cfg_len);
    if (err == ZTK_OK) {
        live_ingest_set_video_config(ch, buf, cfg_len);
        ch->have_video_cfg = 1;
        ch->source->has_video = 1;
        ch->source->video.codec = ZMS_CODEC_H265;
        ch->source->video.ready = 1;
    }
    return err;
}

ztk_err_t zms_live_ingest_input_h265_annexb(zms_live_ingest *ch, const uint8_t *annexb, size_t len,
                                            uint32_t dts_ms, uint32_t pts_ms, int keyframe)
{
    const uint8_t *cur;
    const uint8_t *end;
    int vcl_n = 0;

    (void)keyframe;
    if (!ch || !annexb || len == 0) {
        return ZTK_ERR_INVALID;
    }

    {
        int sync = keyframe || zms_h265_annexb_is_sync_key(annexb, len);

        if (sync && ch->tl.linear_ms && !ch->av_origin_set) {
            ch->av_origin_ms = dts_ms;
            ch->av_origin_set = 1;
            ztk_info("ingress: AV origin at H265 sync raw_dts=%u", (unsigned)dts_ms);
        }
        if (ch->defer_gop_vcfg && sync && !ch->gop_vcfg_applied) {
            ch->gop_vcfg_applied = 1;
        }
    }

    if (!ch->have_video_cfg) {
        size_t cfg_len = 0;
        uint8_t *buf = live_ingest_work_buf(ch);

        if (buf && zms_h265_hvcc_from_annexb(annexb, len, buf, ZMS_LIVE_INGEST_WORK_BUF,
                                             &cfg_len) == ZTK_OK) {
            live_ingest_set_video_config(ch, buf, cfg_len);
            ch->have_video_cfg = 1;
            ch->source->has_video = 1;
            ch->source->video.codec = ZMS_CODEC_H265;
            ch->source->video.ready = 1;
            ztk_info("track video: H265 (cfg len=%u)", (unsigned)cfg_len);
        }
    }

    if (!ch->source->gop_queue) {
        return ZTK_ERR_INVALID;
    }

    {
        uint32_t dts = live_ingest_video_pts(ch, dts_ms);
        uint32_t pts = dts;

        if (pts_ms != 0 && pts_ms != dts_ms) {
            if (pts_ms > dts_ms) {
                pts = dts + (pts_ms - dts_ms);
            } else {
                pts = live_ingest_video_pts(ch, pts_ms);
            }
        }
        if (hevc_au_begin(ch, dts_ms, dts, pts) != ZTK_OK) {
            return ZTK_ERR_NOMEM;
        }
    }

    cur = annexb;
    end = annexb + len;
    while (cur < end) {
        size_t nlen = 0;
        const uint8_t *nal = zms_annexb_find_nal(cur, end, &nlen);
        int t;

        if (!nal || nlen == 0) {
            break;
        }
        t = (nal[0] >> 1) & 0x3f;
        if (t == 32 || t == 33 || t == 34) {
            cur = nal + nlen;
            continue;
        }
        if (t >= 32) {
            cur = nal + nlen;
            continue;
        }
        if (hevc_au_append_nal(ch, nal, nlen, t >= 16 && t <= 21) != ZTK_OK) {
            return ZTK_ERR_NOMEM;
        }
        vcl_n++;
        cur = nal + nlen;
    }
    if (vcl_n == 0) {
        return ZTK_ERR_INVALID;
    }

    ch->source->has_video = 1;
    return hevc_au_flush(ch);
}
