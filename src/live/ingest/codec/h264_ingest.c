#include "zms/live/ingest/common/stream_ingest.h"
#include "zms/live/ingest/common/ingest_codec.h"
#include "zms/media/codec/h264/h264_es.h"
#include "zms/media/codec/h264/h264_sps.h"
#include "zms/media/codec/h264/h264_over_rtmp.h"
#include "zms/media/container/flv/flv_tag_probe.h"
#include "live/ingest/common/ingest_internal.h"
#include "ztk/util/log.h"
#include <string.h>

static const uint8_t *next_annexb_nal(const uint8_t *p, const uint8_t *end, int *nal_type)
{
    while (p + 3 < end) {
        size_t start = 0;
        if (p + 4 <= end && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) {
            start = 4;
        } else if (p[0] == 0 && p[1] == 0 && p[2] == 1) {
            start = 3;
        }
        if (start) {
            const uint8_t *nal = p + start;
            if (nal >= end) {
                return NULL;
            }
            if (nal_type) {
                *nal_type = nal[0] & 0x1f;
            }
            return nal;
        }
        ++p;
    }
    return NULL;
}

static int h264_annexb_has_nal_type(const uint8_t *annexb, size_t len, int want_type)
{
    if (!annexb || len < 5) {
        return 0;
    }
    const uint8_t *end = annexb + len;
    const uint8_t *p = annexb;
    while (p < end) {
        int t = 0;
        const uint8_t *nal = next_annexb_nal(p, end, &t);
        if (!nal) {
            break;
        }
        if (t == want_type) {
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

static int h264_annexb_first_slice(const uint8_t *annexb, size_t len)
{
    const uint8_t *end = annexb + len;
    const uint8_t *p = annexb;
    while (p < end) {
        int t = 0;
        const uint8_t *nal = next_annexb_nal(p, end, &t);
        if (!nal) {
            break;
        }
        int type = t;
        if (type >= 1 && type <= 5) {
            return (nal[1] & 0x80) != 0;
        }
        p = nal + 1;
    }
    return 1;
}

void zms_live_ingest_h264_annexb_frame_flags(const uint8_t *annexb, size_t len, int *config_frame,
                                             int *drop_able)
{
    if (config_frame) {
        *config_frame = 0;
    }
    if (drop_able) {
        *drop_able = 0;
    }
    if (!annexb || len < 4) {
        return;
    }
    int has_slice = h264_annexb_has_slice(annexb, len);
    int has_cfg =
        h264_annexb_has_nal_type(annexb, len, 7) || h264_annexb_has_nal_type(annexb, len, 8);
    int has_sei_aud =
        h264_annexb_has_nal_type(annexb, len, 6) || h264_annexb_has_nal_type(annexb, len, 9);
    if (config_frame && !has_slice && has_cfg) {
        *config_frame = 1;
    }
    if (drop_able && !has_slice && has_sei_aud && !has_cfg) {
        *drop_able = 1;
    }
}

ztk_err_t zms_live_ingest_set_h264_sps_pps(zms_live_ingest *ch, const uint8_t *sps, size_t sps_len,
                                           const uint8_t *pps, size_t pps_len)
{
    zms_h264_sps_info sps_info;
    int have_sps = 0;
    uint32_t cur_w;
    uint32_t cur_h;
    int size_better = 0;
    int need_cfg;
    int fps_better = 0;

    if (!ch || !ch->source || !sps || !pps || sps_len == 0 || pps_len == 0) {
        return ZTK_ERR_INVALID;
    }

    have_sps = zms_h264_sps_parse(sps, sps_len, &sps_info);
    cur_w = ch->source->video.width;
    cur_h = ch->source->video.height;
    if (have_sps) {
        size_better = zms_video_size_should_replace(cur_w, cur_h, (uint32_t)sps_info.width,
                                                    (uint32_t)sps_info.height);
        if (sps_info.fps > 0.0f &&
            (ch->source->video.fps <= 0.0f ||
             (size_better && sps_info.fps != ch->source->video.fps))) {
            fps_better = 1;
        }
    }
    need_cfg = !ch->have_video_cfg;

    /* 配置已有且分辨率/帧率无需升级时直接返回，避免占位 SPS 反复覆盖 */
    if (!need_cfg && !size_better && !fps_better) {
        return ZTK_OK;
    }

    {
        uint8_t *buf = live_ingest_work_buf(ch);
        size_t cfg_len = 0;
        ztk_err_t err = zms_rtmp_avc_seq_header(sps, sps_len, pps, pps_len, buf,
                                                ZMS_LIVE_INGEST_WORK_BUF, &cfg_len);
        if (err != ZTK_OK) {
            return err;
        }

        if (need_cfg || size_better) {
            if (ch->defer_gop_vcfg && need_cfg) {
                ch->video_cfg_len = cfg_len;
                ch->have_video_cfg = 1;
            } else {
                live_ingest_set_video_config(ch, buf, cfg_len);
                ch->have_video_cfg = 1;
            }
            ch->source->has_video = 1;

            if (need_cfg || !ch->source->video.ready) {
                (void)zms_video_track_from_avc(&ch->source->video, buf, cfg_len);
            } else if (size_better && have_sps) {
                /* 保留 ready/codec，仅升级尺寸，避免占位→真实时丢元数据 */
                ch->source->video.width = (uint32_t)sps_info.width;
                ch->source->video.height = (uint32_t)sps_info.height;
                if (sps_info.fps > 0.0f) {
                    ch->source->video.fps = sps_info.fps;
                }
                if (!zms_rtmp_avc_profile_level_id(buf, cfg_len,
                                                   ch->source->video.profile_level_id)) {
                    /* keep previous profile_level_id */
                }
                ztk_info("track video: size updated %ux%u -> %ux%u fps=%.2f (replace placeholder SPS)",
                         cur_w, cur_h, ch->source->video.width, ch->source->video.height,
                         (double)ch->source->video.fps);
            }
        } else if (fps_better && have_sps) {
            ch->source->video.fps = sps_info.fps;
        }

        /* from_avc 后若仍可疑，再用裸 SPS 补一次（极端 AVCC 解析失败场景） */
        if (have_sps &&
            zms_video_size_should_replace(ch->source->video.width, ch->source->video.height,
                                          (uint32_t)sps_info.width, (uint32_t)sps_info.height)) {
            uint32_t before_w = ch->source->video.width;
            uint32_t before_h = ch->source->video.height;
            ch->source->video.width = (uint32_t)sps_info.width;
            ch->source->video.height = (uint32_t)sps_info.height;
            if (sps_info.fps > 0.0f) {
                ch->source->video.fps = sps_info.fps;
            }
            ztk_info("track video: %ux%u -> %ux%u fps=%.2f profile=%s (from SPS)", before_w,
                     before_h, ch->source->video.width, ch->source->video.height,
                     (double)ch->source->video.fps, ch->source->video.profile_level_id);
        } else if (need_cfg && ch->source->video.width >= ZMS_VIDEO_WIDTH_MIN_VALID &&
                   ch->source->video.height > 0) {
            ztk_info("track video: %ux%u fps=%.2f profile=%s%s", ch->source->video.width,
                     ch->source->video.height, (double)ch->source->video.fps,
                     ch->source->video.profile_level_id,
                     zms_video_size_is_suspicious(ch->source->video.width, ch->source->video.height)
                         ? " (suspicious, wait in-band SPS)"
                         : "");
        }
    }
    return ZTK_OK;
}

ztk_err_t zms_live_ingest_input_h264_annexb(zms_live_ingest *ch, const uint8_t *annexb, size_t len,
                                            uint32_t dts_ms, uint32_t pts_ms, int keyframe)
{
    if (!ch || !annexb || len == 0) {
        return ZTK_ERR_INVALID;
    }

    /* 有 SPS/PPS 时交给 set_h264_sps_pps：缺配置则建配置；仅拒绝占位回退覆盖可信尺寸 */
    {
        const uint8_t *sps = NULL, *pps = NULL;
        size_t sps_len = 0, pps_len = 0;
        if (zms_h264_annexb_extract_sps_pps(annexb, len, &sps, &sps_len, &pps, &pps_len) && sps &&
            pps) {
            (void)zms_live_ingest_set_h264_sps_pps(ch, sps, sps_len, pps, pps_len);
        }
    }

    int idr = zms_h264_annexb_is_idr(annexb, len);
    int key = idr;
    int config_frame = 0;
    int drop_able = 0;
    (void)keyframe;
    uint32_t raw_dts;
    uint32_t raw_pts;
    uint32_t norm_dts;
    uint32_t norm_pts;

    if (ch->defer_gop_vcfg && !ch->gop_vcfg_applied) {
        if (!idr) {
            return ZTK_OK;
        }
        if (ch->have_video_cfg && ch->video_cfg_len > 0) {
            live_ingest_set_video_config(ch, live_ingest_work_buf(ch), ch->video_cfg_len);
        }
        ch->gop_vcfg_applied = 1;
    }

    if (idr && ch->tl.linear_ms && !ch->av_origin_set) {
        ch->av_origin_ms = dts_ms;
        ch->av_origin_set = 1;
        ztk_info("ingress: AV origin at IDR raw_dts=%u", (unsigned)dts_ms);
    }

    raw_dts = dts_ms;
    raw_pts = pts_ms;
    if (raw_pts == 0 && raw_dts != 0) {
        raw_pts = raw_dts;
    }

    if (!ch->source->gop_queue) {
        return ZTK_ERR_INVALID;
    }

    zms_frame frame;
    zms_frame_init(&frame);
    frame.data = (uint8_t *)annexb;
    frame.size = len;
    norm_dts = live_ingest_video_pts(ch, raw_dts);
    if (ch->tl.linear_ms) {
        if (ch->av_origin_set && raw_pts >= ch->av_origin_ms) {
            norm_pts = raw_pts - ch->av_origin_ms;
        } else {
            norm_pts = raw_pts;
        }
        if (norm_pts < norm_dts) {
            norm_pts = norm_dts;
        }
    } else {
        norm_pts = norm_dts;
        if (raw_pts > raw_dts) {
            norm_pts = norm_dts + (raw_pts - raw_dts);
        }
        if (norm_pts < norm_dts) {
            norm_pts = norm_dts;
        }
    }
    frame.dts_ms = norm_dts;
    frame.pts_ms = norm_pts;
    frame.codec = ZMS_CODEC_H264;
    frame.track = ZMS_TRACK_VIDEO;
    frame.keyframe = key;
    zms_live_ingest_h264_annexb_frame_flags(annexb, len, &config_frame, &drop_able);
    frame.config_frame = config_frame;
    frame.drop_able = drop_able;
    if (drop_able || config_frame) {
        return ZTK_OK;
    }

    /* SRT MPEG-TS 专用：demux CC-gap 丢 P 帧后填补小 dts 空洞。 */
    if (ch->tl.linear_ms && ch->defer_gop_vcfg && !key && ch->tl_last_v_gop_valid) {
        uint32_t step = 42;
        uint32_t gap;

        if (ch->source && ch->source->video.fps > 0.0f) {
            step = (uint32_t)(1000.0f / ch->source->video.fps + 0.5f);
        }
        if (step < 1) {
            step = 1;
        }
        gap = norm_dts > ch->tl_last_v_gop_norm ? norm_dts - ch->tl_last_v_gop_norm : 0;
        /* 填补丢 P 帧造成的空洞（非整 GOP / IDR 间距）。 */
        if (gap > step * 2 && gap < 750) {
            uint32_t cts = norm_pts > norm_dts ? norm_pts - norm_dts : 0;

            norm_dts = ch->tl_last_v_gop_norm + step;
            norm_pts = norm_dts + cts;
            frame.dts_ms = norm_dts;
            frame.pts_ms = norm_pts;
        }
    }
    if (ch->tl.linear_ms && ch->defer_gop_vcfg) {
        ch->tl_last_v_gop_norm = norm_dts;
        ch->tl_last_v_gop_valid = 1;
    }
    live_ingest_write_frame(ch, &frame);
    ch->source->has_video = 1;
    return ZTK_OK;
}
