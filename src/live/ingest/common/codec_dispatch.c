#include "zms/session/codec_filter.h"
#include "zms/live/ingest/common/ingest_codec.h"
#include "zms/media/codec/av1/av1_over_rtmp.h"
#include "zms/media/codec/codec_id.h"
#include "live/ingest/common/ingest_internal.h"
#include "zms/util/log_throttle.h"
#include "ztk/util/log.h"
#include <stddef.h>
#include <string.h>

typedef ztk_err_t (*ingest_codec_input_fn)(zms_live_ingest *ch, const zms_frame *frame);

static ingest_codec_input_fn g_ingest_codec[ZMS_CODEC_H266 + 1];

static int ingest_publish_codec_ok(zms_live_ingest *ch, zms_codec_id video, zms_codec_id audio,
                                   int has_video, int has_audio)
{
    zms_session_cap_role role;
    ztk_err_t err;

    if (!ch || !ch->source) {
        return 1;
    }
    role = zms_session_capability_publish_role(ch->source);
    err = zms_session_capability_check(role, video, audio, has_video, has_audio);
    if (err == ZTK_OK) {
        return 1;
    }
    zms_log_warn_throttle("cap_publish_reject", 10000,
                          "[capability] reject %s app=%s stream=%s video=%s audio=%s allowed=%s",
                          zms_session_capability_role_name(role), ch->source->app,
                          ch->source->stream,
                          video != ZMS_CODEC_INVALID ? zms_codec_name(video) : "-",
                          audio != ZMS_CODEC_INVALID ? zms_codec_name(audio) : "-",
                          zms_session_capability_allowed_hint(role));
    return 0;
}

static void ingest_es_ensure_video(zms_live_ingest *ch, zms_codec_id codec)
{
    zms_codec_id ac = ZMS_CODEC_INVALID;

    if (!ch || !ch->source || ch->have_video_cfg) {
        return;
    }
    if (ch->source->has_audio && ch->source->audio.codec != ZMS_CODEC_INVALID) {
        ac = ch->source->audio.codec;
    }
    if (!ingest_publish_codec_ok(ch, codec, ac, 1,
                                 ch->source->has_audio && ac != ZMS_CODEC_INVALID)) {
        return;
    }
    ch->have_video_cfg = 1;
    ch->source->has_video = 1;
    ch->source->video.codec = codec;
    ch->source->video.ready = 1;
    ztk_info("track video: codec=%s", zms_codec_name(codec));
}

static void ingest_es_ensure_audio(zms_live_ingest *ch, zms_codec_id codec, uint32_t rate)
{
    zms_codec_id vc = ZMS_CODEC_INVALID;

    if (!ch || !ch->source || ch->have_audio_cfg) {
        return;
    }
    if (ch->source->has_video && ch->source->video.codec != ZMS_CODEC_INVALID) {
        vc = ch->source->video.codec;
    }
    if (!ingest_publish_codec_ok(ch, vc, codec, ch->source->has_video && vc != ZMS_CODEC_INVALID,
                                 1)) {
        return;
    }
    ch->have_audio_cfg = 1;
    ch->source->has_audio = 1;
    ch->source->audio.codec = codec;
    ch->source->audio.ready = 1;
    ch->source->audio.sample_rate = rate > 0 ? (int)rate : 48000;
    ch->source->audio.channels = codec == ZMS_CODEC_OPUS ? 2 : 1;
    zms_media_timeline_set_audio(&ch->tl, codec, rate > 0 ? rate : 48000);
    ztk_info("track audio: codec=%s rate=%u", zms_codec_name(codec), rate > 0 ? rate : 48000u);
}

static ztk_err_t ingest_codec_input_h264(zms_live_ingest *ch, const zms_frame *frame)
{
    return zms_live_ingest_input_h264_annexb(ch, frame->data, frame->size, (uint32_t)frame->dts_ms,
                                             (uint32_t)frame->pts_ms, frame->keyframe);
}

static ztk_err_t ingest_codec_input_h265(zms_live_ingest *ch, const zms_frame *frame)
{
    return zms_live_ingest_input_h265_annexb(ch, frame->data, frame->size, (uint32_t)frame->dts_ms,
                                             (uint32_t)frame->pts_ms, frame->keyframe);
}

static ztk_err_t ingest_codec_input_aac(zms_live_ingest *ch, const zms_frame *frame)
{
    return zms_live_ingest_input_aac_es(ch, frame->data, frame->size, (uint32_t)frame->dts_ms);
}

static ztk_err_t ingest_codec_input_g711(zms_live_ingest *ch, const zms_frame *frame)
{
    return zms_live_ingest_input_g711_es(ch, frame->codec, frame->data, frame->size,
                                         (uint32_t)frame->dts_ms);
}

static void ingest_es_try_store_av1_config(zms_live_ingest *ch, const zms_frame *frame)
{
    uint8_t av1c[2048];
    uint8_t hdr[4096];
    size_t hdr_len = 0;
    size_t clen = 0;
    int w = 0;
    int h = 0;
    int n;

    if (!ch || !ch->source || !ch->source->gop_queue || !frame || frame->track != ZMS_TRACK_VIDEO ||
        frame->codec != ZMS_CODEC_AV1 || !frame->data || frame->size == 0) {
        return;
    }
    if (zms_gop_queue_video_config(ch->source->gop_queue, &clen) && clen > 0) {
        return;
    }
    if (!zms_av1_obu_has_sequence_header(frame->data, frame->size)) {
        return;
    }
    n = zms_av1_extradata_from_obu(frame->data, frame->size, av1c, sizeof(av1c), &w, &h);
    if (n <= 0) {
        return;
    }
    if (zms_av1_flv_sequence_header(av1c, (size_t)n, hdr, sizeof(hdr), &hdr_len) != ZTK_OK ||
        hdr_len == 0) {
        return;
    }
    live_ingest_set_video_config(ch, hdr, hdr_len);
    if (w > 0) {
        ch->source->video.width = w;
    }
    if (h > 0) {
        ch->source->video.height = h;
    }
    ztk_info("track video: AV1 config stored w=%d h=%d", w, h);
}

static ztk_err_t ingest_codec_input_es(zms_live_ingest *ch, const zms_frame *frame)
{
    zms_frame out;
    uint32_t pts;

    if (!ch || !frame || !frame->data || frame->size == 0) {
        return ZTK_ERR_INVALID;
    }
    if (frame->track == ZMS_TRACK_VIDEO) {
        if (frame->codec == ZMS_CODEC_AV1) {
            ingest_es_try_store_av1_config(ch, frame);
        }
        ingest_es_ensure_video(ch, frame->codec);
        pts = live_ingest_video_pts(ch, (uint32_t)frame->dts_ms);
    } else if (frame->track == ZMS_TRACK_AUDIO && frame->codec == ZMS_CODEC_OPUS) {
        ingest_es_ensure_audio(ch, ZMS_CODEC_OPUS, ch->tl.audio_clock_hz);
        pts = live_ingest_audio_pts(ch, (uint32_t)frame->dts_ms);
    } else {
        return ZTK_ERR_INVALID;
    }

    out = *frame;
    out.dts_ms = out.pts_ms = pts;
    out.owned = 0;
    live_ingest_write_frame(ch, &out);
    return ZTK_OK;
}

void zms_live_ingest_codec_register_all(void)
{
    static int registered; /* 启动阶段，单线程 */

    if (registered) {
        return;
    }
    registered = 1;
    memset(g_ingest_codec, 0, sizeof(g_ingest_codec));
    g_ingest_codec[ZMS_CODEC_H264] = ingest_codec_input_h264;
    g_ingest_codec[ZMS_CODEC_H265] = ingest_codec_input_h265;
    g_ingest_codec[ZMS_CODEC_AAC] = ingest_codec_input_aac;
    g_ingest_codec[ZMS_CODEC_G711A] = ingest_codec_input_g711;
    g_ingest_codec[ZMS_CODEC_G711U] = ingest_codec_input_g711;
    g_ingest_codec[ZMS_CODEC_AV1] = ingest_codec_input_es;
    g_ingest_codec[ZMS_CODEC_VP8] = ingest_codec_input_es;
    g_ingest_codec[ZMS_CODEC_VP9] = ingest_codec_input_es;
    g_ingest_codec[ZMS_CODEC_H266] = ingest_codec_input_es;
    g_ingest_codec[ZMS_CODEC_OPUS] = ingest_codec_input_es;
}

ztk_err_t zms_live_ingest_codec_input_dispatch(zms_live_ingest *ch, const zms_frame *frame)
{
    ingest_codec_input_fn fn;

    if (!ch || !frame || !frame->data || frame->size == 0) {
        return ZTK_ERR_INVALID;
    }
    if (frame->codec <= ZMS_CODEC_INVALID || frame->codec > ZMS_CODEC_H266) {
        return ZTK_ERR_NOT_IMPL;
    }
    fn = g_ingest_codec[frame->codec];
    if (!fn) {
        return ZTK_ERR_NOT_IMPL;
    }
    return fn(ch, frame);
}
