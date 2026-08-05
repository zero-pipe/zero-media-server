/**
 * @file stream_ingest.c
 * @brief 直播推流接入：将 RTMP 等协议帧写入 gop_queue。
 *
 * Copyright (c) zero-media-server
 */
#include "zms/live/ingest/common/stream_ingest.h"
#include "zms/live/ingest/common/ingest_codec.h"
#include "zms/media/codec/aac/aac_over_rtmp.h"
#include "zms/media/codec/aac/aac_over_rtp.h"
#include "zms/media/codec/h264/h264_es.h"
#include "zms/media/container/demux_pipeline.h"
#include "zms/media/container/flv/flv_tag_probe.h"
#include "zms/session/rtmp/rtmp.h"
#include "zms/engine/media_event.h"
#include "zms/engine/media_clock.h"
#include "zms/session/codec_filter.h"
#include "zms/engine/gop/gop_queue.h"
#include "zms/engine/stream_track.h"
#include "zms/util/buf_pool.h"
#include "live/ingest/common/ingest_internal.h"
#include "ztk/util/buf.h"
#include "zms/util/log_throttle.h"
#include "ztk/util/log.h"
#include <stdlib.h>
#include <string.h>

static uint32_t ingest_linear_ms_pts(zms_live_ingest *ch, uint32_t raw_ms)
{
    if (ch->av_origin_set && raw_ms >= ch->av_origin_ms) {
        return raw_ms - ch->av_origin_ms;
    }
    return raw_ms;
}

uint32_t live_ingest_video_pts(zms_live_ingest *ch, uint32_t raw_ms)
{
    if (ch->last_v_valid && ch->last_v_raw_ms == raw_ms) {
        return ch->last_v_pts;
    }
    ch->last_v_raw_ms = raw_ms;
    if (ch->tl.linear_ms) {
        ch->last_v_pts = ingest_linear_ms_pts(ch, raw_ms);
    } else {
        ch->last_v_pts = zms_media_timeline_video(&ch->tl, raw_ms);
    }
    ch->last_v_valid = 1;
    return ch->last_v_pts;
}

uint32_t live_ingest_audio_pts(zms_live_ingest *ch, uint32_t raw_ms)
{
    if (ch->last_a_valid && ch->last_a_raw_ms == raw_ms) {
        return ch->last_a_pts;
    }
    ch->last_a_raw_ms = raw_ms;
    if (ch->tl.linear_ms) {
        ch->last_a_pts = ingest_linear_ms_pts(ch, raw_ms);
    } else {
        ch->last_a_pts = zms_media_timeline_audio(&ch->tl, raw_ms);
    }
    ch->last_a_valid = 1;
    return ch->last_a_pts;
}

uint8_t *live_ingest_work_buf(zms_live_ingest *ch)
{
    if (!ch) {
        return NULL;
    }
    if (!ingest_slot_resize(ch, &ch->work_buf, &ch->work_cap, ZMS_LIVE_INGEST_WORK_BUF)) {
        return NULL;
    }
    return ch->work_buf;
}

uint8_t *live_ingest_large_buf(zms_live_ingest *ch, size_t need)
{
    if (!ch || need == 0) {
        return NULL;
    }
    if (!ingest_slot_resize(ch, &ch->large_buf, &ch->large_cap, need)) {
        return NULL;
    }
    return ch->large_buf;
}

void live_ingest_set_video_config(zms_live_ingest *ch, const void *data, size_t len)
{
    ztk_err_t err;

    if (!ch || !ch->source || !ch->source->gop_queue || !data || len == 0) {
        return;
    }
    err = zms_gop_queue_set_video_config(ch->source->gop_queue, data, len);
    if (err != ZTK_OK) {
        ztk_warn("ingress: set video config failed err=%d len=%u", (int)err, (unsigned)len);
    }
}

void live_ingest_set_audio_config(zms_live_ingest *ch, const void *data, size_t len)
{
    if (!ch || !ch->source || !ch->source->gop_queue || !data || len == 0) {
        return;
    }
    (void)zms_gop_queue_set_audio_config(ch->source->gop_queue, data, len);
}

void live_ingest_write_frame(zms_live_ingest *ch, const zms_frame *frame)
{
    if (!ch || !ch->source || !ch->source->gop_queue || !frame || !frame->data ||
        frame->size == 0) {
        return;
    }
    zms_frame copy = *frame;
    copy.owned = 0;
    if (zms_gop_queue_write(ch->source->gop_queue, &copy) == ZTK_OK) {
        zms_media_stats_on_ingress(ch->source, frame->size);
        zms_media_stats_on_frame(ch->source, frame->track == ZMS_TRACK_VIDEO);
    } else {
        zms_media_stats_on_drop(ch->source);
    }
}

ztk_err_t live_ingest_write_frame_buf(zms_live_ingest *ch, ztk_buf *buf, const zms_frame *frame)
{
    ztk_err_t err;

    if (!ch || !ch->source || !ch->source->gop_queue || !buf || !frame) {
        return ZTK_ERR_INVALID;
    }
    err = zms_gop_queue_write_buf(ch->source->gop_queue, buf, frame);
    if (err != ZTK_OK) {
        ztk_buf_unref(buf);
        zms_media_stats_on_drop(ch->source);
    } else {
        zms_media_stats_on_ingress(ch->source, frame->size);
        zms_media_stats_on_frame(ch->source, frame->track == ZMS_TRACK_VIDEO);
    }
    return err;
}

zms_live_ingest *zms_live_ingest_create(const char *app, const char *stream,
                                        const zms_protocol_opts *opts)
{
    if (!app || !stream) {
        return NULL;
    }

    zms_live_ingest *ch = (zms_live_ingest *)calloc(1, sizeof(*ch));
    if (!ch) {
        return NULL;
    }

    if (opts) {
        ch->opt = *opts;
    } else {
        zms_protocol_opts_default(&ch->opt);
    }

    ch->source = zms_media_source_register(ZMS_SCHEMA_RTMP, app, stream);
    if (!ch->source) {
        free(ch);
        return NULL;
    }
    ch->owns_source = 0;
    zms_media_timeline_reset(&ch->tl);
    return ch;
}

zms_live_ingest *zms_live_ingest_create_publish(const char *app, const char *stream_requested,
                                                const zms_protocol_opts *opts)
{
    return zms_live_ingest_create_publish_schema(ZMS_SCHEMA_RTMP, app, stream_requested, opts);
}

zms_live_ingest *zms_live_ingest_create_publish_schema(const char *schema, const char *app,
                                                       const char *stream_requested,
                                                       const zms_protocol_opts *opts)
{
    const char *reg_schema = schema && schema[0] ? schema : ZMS_SCHEMA_RTMP;

    if (!app || !stream_requested) {
        return NULL;
    }

    zms_live_ingest *ch = (zms_live_ingest *)calloc(1, sizeof(*ch));
    if (!ch) {
        return NULL;
    }

    if (opts) {
        ch->opt = *opts;
    } else {
        zms_protocol_opts_default(&ch->opt);
    }

    ch->source = zms_media_source_register_publish(reg_schema, app, stream_requested);
    if (!ch->source) {
        free(ch);
        return NULL;
    }
    ch->owns_source = 0;
    zms_media_timeline_reset(&ch->tl);
    return ch;
}

zms_live_ingest *zms_live_ingest_bind(zms_media_source *src)
{
    if (!src || !src->gop_queue) {
        return NULL;
    }
    zms_live_ingest *ch = (zms_live_ingest *)calloc(1, sizeof(*ch));
    if (!ch) {
        return NULL;
    }
    ch->source = src;
    ch->owns_source = 0;
    zms_protocol_opts_default(&ch->opt);
    zms_media_timeline_reset(&ch->tl);
    return ch;
}

void zms_live_ingest_destroy(zms_live_ingest *ch)
{
    if (!ch) {
        return;
    }
    zms_live_ingest_h265_hevc_au_reset(ch);
    zms_demux_pipeline_destroy(ch->rtmp_demux);
    ingest_slot_clear(ch, &ch->work_buf, &ch->work_cap);
    ingest_slot_clear(ch, &ch->large_buf, &ch->large_cap);
    free(ch);
}

void zms_live_ingest_set_poller(zms_live_ingest *ch, struct ztk_poller *poller)
{
    if (!ch) {
        return;
    }
    ch->io_poller = poller;
}

zms_media_source *zms_live_ingest_source(zms_live_ingest *ch)
{
    return ch ? ch->source : NULL;
}

void zms_live_ingest_reset_upstream(zms_live_ingest *ch)
{
    if (!ch) {
        return;
    }
    zms_live_ingest_h265_hevc_au_reset(ch);
    zms_demux_pipeline_destroy(ch->rtmp_demux);
    ch->rtmp_demux = NULL;
    ch->have_video_cfg = 0;
    ch->have_audio_cfg = 0;
    ch->gop_vcfg_applied = 0;
    ch->video_cfg_len = 0;
    ch->av_origin_set = 0;
    ch->av_origin_ms = 0;
    ch->last_v_valid = 0;
    ch->last_a_valid = 0;
    ch->rtmp_ingress.active = 0;
    ch->rtmp_frame_got = 0;
    ch->tl_last_v_gop_norm = 0;
    ch->tl_last_v_gop_valid = 0;
    zms_media_timeline_reset(&ch->tl);
}

void zms_live_ingest_reset(zms_live_ingest *ch)
{
    if (!ch || !ch->source) {
        return;
    }
    zms_live_ingest_reset_upstream(ch);
    zms_media_source_clear(ch->source);
}

void zms_live_ingest_set_audio_codec(zms_live_ingest *ch, zms_codec_id codec, uint32_t sample_rate)
{
    if (ch) {
        zms_media_timeline_set_audio(&ch->tl, codec, sample_rate);
    }
}

void zms_live_ingest_set_rtp_clocks(zms_live_ingest *ch, uint32_t video_clock_hz,
                                    zms_codec_id audio_codec, uint32_t audio_clock_hz)
{
    if (!ch) {
        return;
    }
    zms_media_timeline_set_video(&ch->tl, ZMS_CODEC_H264, video_clock_hz);
    if (audio_codec != ZMS_CODEC_INVALID) {
        zms_media_timeline_set_audio(&ch->tl, audio_codec, audio_clock_hz);
    }
    ch->last_v_valid = 0;
    ch->last_a_valid = 0;
}

void zms_live_ingest_set_stamp_max_delta(zms_live_ingest *ch, uint32_t max_delta_ms)
{
    if (!ch || max_delta_ms == 0) {
        return;
    }
    ch->tl.vst.max_delta_ms = max_delta_ms;
    ch->tl.ast.max_delta_ms = max_delta_ms;
}

void zms_live_ingest_set_stamp_av_clamp(zms_live_ingest *ch, int enabled)
{
    if (!ch) {
        return;
    }
    zms_media_timeline_set_av_clamp(&ch->tl, enabled);
    ch->last_v_valid = 0;
    ch->last_a_valid = 0;
}

void zms_live_ingest_set_timeline_linear_ms(zms_live_ingest *ch, int on)
{
    if (!ch) {
        return;
    }
    ch->tl.linear_ms = on ? 1 : 0;
    ch->last_v_valid = 0;
    ch->last_a_valid = 0;
    ch->av_origin_set = 0;
    ch->av_origin_ms = 0;
    ch->tl_last_v_gop_norm = 0;
    ch->tl_last_v_gop_valid = 0;
}

void zms_live_ingest_set_defer_gop_vcfg(zms_live_ingest *ch, int on)
{
    if (!ch) {
        return;
    }
    ch->defer_gop_vcfg = on ? 1 : 0;
    ch->gop_vcfg_applied = 0;
    ch->video_cfg_len = 0;
}

ztk_err_t zms_live_ingest_input_frame(zms_live_ingest *ch, const zms_frame *frame)
{
    return zms_live_ingest_codec_input_dispatch(ch, frame);
}

static void ingest_rtmp_demux_on_frame(const zms_frame *frame, void *user)
{
    zms_live_ingest *ch = (zms_live_ingest *)user;
    zms_frame out;
    ztk_buf *buf;
    uint8_t *dst;

    if (!ch || !frame || !frame->data || frame->size == 0 || !ch->source ||
        !ch->source->gop_queue) {
        return;
    }

    ch->rtmp_frame_got = 1;
    buf = zms_gop_queue_alloc_write(ch->source->gop_queue, frame->size);
    if (!buf) {
        return;
    }
    dst = (uint8_t *)ztk_buf_data(buf);
    memcpy(dst, frame->data, frame->size);
    ztk_buf_set_len(buf, frame->size);

    zms_frame_init(&out);
    out.data = dst;
    out.size = frame->size;
    out.codec = frame->codec;
    out.track = frame->track;
    out.keyframe = frame->keyframe;
    out.owned = 0;

    if (frame->track == ZMS_TRACK_VIDEO) {
        uint32_t ts = live_ingest_video_pts(ch, ch->rtmp_ingress.raw_tag_dts_ms);
        out.dts_ms = out.pts_ms = ts;
        if (frame->codec == ZMS_CODEC_H264) {
            zms_live_ingest_h264_annexb_frame_flags(dst, frame->size, &out.config_frame,
                                                    &out.drop_able);
        }
        ch->source->has_video = 1;
    } else if (frame->track == ZMS_TRACK_AUDIO) {
        uint32_t ts = live_ingest_audio_pts(ch, ch->rtmp_ingress.raw_tag_dts_ms);
        const uint8_t *raw = dst;
        size_t raw_len = frame->size;

        out.dts_ms = out.pts_ms = ts;
        if (frame->codec == ZMS_CODEC_AAC &&
            zms_aac_es_to_raw(dst, frame->size, &raw, &raw_len) == ZTK_OK && raw != dst) {
            memmove(dst, raw, raw_len);
            out.size = raw_len;
            ztk_buf_set_len(buf, raw_len);
        }
        ch->source->has_audio = 1;
    } else {
        ztk_buf_unref(buf);
        return;
    }

    out.data = (uint8_t *)ztk_buf_data(buf);
    (void)live_ingest_write_frame_buf(ch, buf, &out);
}

static void ingest_ensure_rtmp_demux(zms_live_ingest *ch)
{
    zms_demux_pipeline_opts cfg;

    if (!ch || ch->rtmp_demux) {
        return;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.container = ZMS_CONTAINER_FLV_TAG;
    cfg.on_frame = ingest_rtmp_demux_on_frame;
    cfg.user = ch;
    ch->rtmp_demux = zms_demux_pipeline_create(&cfg);
}

zms_demux_pipeline *zms_live_ingest_rtmp_demux_create(zms_live_ingest *ch)
{
    if (!ch) {
        return NULL;
    }
    ingest_ensure_rtmp_demux(ch);
    return ch->rtmp_demux;
}

void zms_live_ingest_rtmp_demux_release(zms_live_ingest *ch)
{
    if (ch) {
        ch->rtmp_demux = NULL;
    }
}

static ztk_err_t ingest_rtmp_feed_tag(zms_live_ingest *ch, uint8_t type_id, uint32_t tag_dts_ms,
                                      const void *body, size_t body_len, uint8_t msg_type)
{
    ztk_err_t err;

    if (!ch || !body || body_len == 0) {
        return ZTK_ERR_INVALID;
    }
    if (!ch->source || !ch->source->gop_queue) {
        return ZTK_ERR_INVALID;
    }

    ingest_ensure_rtmp_demux(ch);
    if (!ch->rtmp_demux) {
        return ZTK_ERR_NOMEM;
    }

    ch->rtmp_ingress.active = 1;
    ch->rtmp_ingress.raw_tag_dts_ms = tag_dts_ms;
    ch->rtmp_ingress.msg_type = msg_type;
    ch->rtmp_frame_got = 0;

    err = zms_demux_pipeline_input_flv_tag(ch->rtmp_demux, 0, type_id, (const uint8_t *)body,
                                           body_len, tag_dts_ms);
    ch->rtmp_ingress.active = 0;
    return err;
}

ztk_err_t zms_live_ingest_input_rtmp_video(zms_live_ingest *ch, uint32_t tag_dts_ms,
                                           const void *body, size_t body_len)
{
    zms_flv_tag_packet_kind vpkt;

    if (!ch || !body || body_len < 2) {
        return ZTK_ERR_INVALID;
    }
    const uint8_t *p = (const uint8_t *)body;

    zms_codec_id vc = zms_flv_tag_video_codec(p, body_len);
    if (vc != ZMS_CODEC_H264 && vc != ZMS_CODEC_H265 && vc != ZMS_CODEC_H266 &&
        vc != ZMS_CODEC_AV1 && vc != ZMS_CODEC_VP8 && vc != ZMS_CODEC_VP9) {
        if (ch->have_video_cfg && ch->source->video.codec != ZMS_CODEC_INVALID) {
            vc = ch->source->video.codec;
        } else {
            zms_log_warn_throttle("ingress:unsupported_rtmp_video", ZMS_LOG_THROTTLE_WARN_MS,
                                  "[ingress] unsupported_rtmp_video len=%u first=0x%02x",
                                  (unsigned)body_len, p[0]);
            return ZTK_ERR_NOT_IMPL;
        }
    }

    vpkt = zms_flv_tag_video_packet_kind(p, body_len);
    if (vpkt == ZMS_FLV_TAG_PKT_INVALID) {
        zms_log_warn_throttle("ingress:bad_rtmp_video_hdr", ZMS_LOG_THROTTLE_WARN_MS,
                              "[ingress] bad_rtmp_video_hdr len=%u first=0x%02x",
                              (unsigned)body_len, p[0]);
        return ZTK_ERR_INVALID;
    }

    if (vpkt == ZMS_FLV_TAG_PKT_SEQ_HEADER) {
        zms_codec_id ac = ZMS_CODEC_INVALID;
        if (ch->source->has_audio && ch->source->audio.codec != ZMS_CODEC_INVALID) {
            ac = ch->source->audio.codec;
        }
        if (zms_session_capability_check(ZMS_PROTO_CAP_RTMP_PUBLISH, vc, ac, 1,
                                         ch->source->has_audio && ac != ZMS_CODEC_INVALID) !=
            ZTK_OK) {
            zms_session_capability_log_reject("rtmp-publish", ch->source,
                                              ZMS_PROTO_CAP_RTMP_PUBLISH);
            return ZTK_ERR_NOT_IMPL;
        }
        ch->have_video_cfg = 1;
        ch->source->has_video = 1;
        ch->source->video.codec = vc;
        live_ingest_set_video_config(ch, body, body_len);
        if (vc == ZMS_CODEC_H264) {
            (void)zms_video_track_from_avc(&ch->source->video, p, body_len);
        } else {
            ch->source->video.ready = 1;
        }
        ztk_debug("[ingress] track_video codec=%s cfg_len=%u", zms_codec_name(vc),
                  (unsigned)body_len);
        (void)ingest_rtmp_feed_tag(ch, 9, tag_dts_ms, p, body_len, ZMS_RTMP_MSG_VIDEO);
        return ZTK_OK;
    }

    if (vpkt != ZMS_FLV_TAG_PKT_RAW) {
        return ZTK_OK;
    }

    {
        ztk_err_t err;

        err = ingest_rtmp_feed_tag(ch, 9, tag_dts_ms, p, body_len, ZMS_RTMP_MSG_VIDEO);
        if (ch->rtmp_frame_got) {
            return ZTK_OK;
        }
        if (err != ZTK_OK) {
            zms_log_warn_throttle("ingress:rtmp_demux_fail", ZMS_LOG_THROTTLE_WARN_MS,
                                  "[ingress] rtmp_demux_fail len=%u first=0x%02x pkt=%d",
                                  (unsigned)body_len, p[0], (int)vpkt);
            return err;
        }
        return ZTK_OK;
    }
}

static int ingest_aac_seq_header_valid(const uint8_t *body, size_t body_len, int *hdr_len)
{
    const uint8_t *asc = NULL;
    size_t asc_len = 0;
    int sr = 0;
    int ch_n = 0;

    if (hdr_len) {
        *hdr_len = 0;
    }
    if (!body || body_len < 4) {
        return 0;
    }
    if (zms_flv_tag_aac_seq_header_asc(body, body_len, &asc, &asc_len) != ZTK_OK) {
        return 0;
    }
    if (asc_len < 2 || !zms_aac_parse_asc(asc, asc_len, &sr, &ch_n)) {
        return 0;
    }
    if (hdr_len) {
        *hdr_len = (int)(asc - body);
    }
    return 1;
}

ztk_err_t zms_live_ingest_input_rtmp_audio(zms_live_ingest *ch, uint32_t tag_dts_ms,
                                           const void *body, size_t body_len)
{
    zms_codec_id acodec;

    if (!ch || !body || body_len < 2) {
        return ZTK_ERR_INVALID;
    }
    const uint8_t *p = (const uint8_t *)body;
    acodec = zms_flv_tag_audio_codec(p, body_len);
    if (acodec == ZMS_CODEC_OPUS &&
        zms_flv_tag_audio_packet_kind(p, body_len) == ZMS_FLV_TAG_PKT_SEQ_HEADER) {
        zms_session_capability_log_reject("rtmp-publish", ch->source, ZMS_PROTO_CAP_RTMP_PUBLISH);
        return ZTK_ERR_NOT_IMPL;
    }
    if (ingest_aac_seq_header_valid(p, body_len, NULL)) {
        if (!ch->have_audio_cfg) {
            zms_codec_id vc = ZMS_CODEC_INVALID;
            if (ch->source->has_video && ch->source->video.codec != ZMS_CODEC_INVALID) {
                vc = ch->source->video.codec;
            }
            if (zms_session_capability_check(ZMS_PROTO_CAP_RTMP_PUBLISH, vc, ZMS_CODEC_AAC,
                                             ch->source->has_video && vc != ZMS_CODEC_INVALID,
                                             1) != ZTK_OK) {
                zms_session_capability_log_reject("rtmp-publish", ch->source,
                                                  ZMS_PROTO_CAP_RTMP_PUBLISH);
                return ZTK_ERR_NOT_IMPL;
            }
            live_ingest_set_audio_config(ch, body, body_len);
            ch->have_audio_cfg = 1;
            ch->source->has_audio = 1;
            (void)zms_audio_track_from_rtmp(&ch->source->audio, p, body_len);
            zms_media_timeline_set_audio(&ch->tl, ZMS_CODEC_AAC,
                                         ch->source->audio.sample_rate > 0
                                             ? (uint32_t)ch->source->audio.sample_rate
                                             : 44100);
            ztk_debug("[ingress] track_audio codec=AAC cfg_len=%u", (unsigned)body_len);
        }
        return ZTK_OK;
    }
    if (acodec == ZMS_CODEC_G711A || acodec == ZMS_CODEC_G711U) {
        zms_session_capability_log_reject("rtmp-publish", ch->source, ZMS_PROTO_CAP_RTMP_PUBLISH);
        return ZTK_ERR_NOT_IMPL;
    }

    {
        ztk_err_t err;

        err = ingest_rtmp_feed_tag(ch, 8, tag_dts_ms, p, body_len, ZMS_RTMP_MSG_AUDIO);
        if (ch->rtmp_frame_got) {
            return ZTK_OK;
        }
        if (err != ZTK_OK) {
            return err;
        }
        /* raw AAC tag demux pipeline 处理后未触发 on_frame 属正常情况
         * （如首帧 codec 探测期）；返回 OK 避免调用方误判为错误 */
        return ZTK_OK;
    }
}
