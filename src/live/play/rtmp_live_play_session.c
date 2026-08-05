/**
 * @file rtmp_live_play_session.c
 * @brief RTMP 直播播放路径：bootstrap、kick、flush 与出站管线。
 *
 * Copyright (c) zero-media-server
 */
#include "session/rtmp/rtmp_session_internal.h"
#include "ztk/platform.h"
#include "zms/session/session_dispatcher.h"
#include "zms/session/play_binding.h"
#include "zms/session/codec_filter.h"
#include "zms/egress/egress_live_policy.h"
#include "zms/media/container/flv/flv_tag_probe.h"
#include "zms/media/codec/aac/aac_over_rtmp.h"
#include "zms/util/hex_decode.h"
#include "media/container/flv/flv_file_muxer.h"
#include "zms/ops/api/webhook/webhook_client.h"
#include "zms/engine/media_event.h"
#include "zms/session/rtmp/rtmp_amf.h"
#include "zms/engine/gop/gop_queue.h"
#include "zms/session/rtmp/rtmp.h"
#include "zms/vod/play/vod_play_lane.h"
#include "zms/vod/io/vod_source.h"
#include "zms/media/codec/h264/h264_es.h"
#include "zms/media/codec/h265/h265_es.h"
#include "zms/engine/media_clock.h"
#include "zms/egress/egress_pacing.h"
#include "zms/egress/egress_clock.h"
#include "zms/util/buf_pool.h"
#include "zms/vod/io/vod_reader.h"
#include "ztk/net/tcp_server.h"
#include "ztk/poller/poller.h"
#include "ztk/thread/sync.h"
#include "ztk/util/log.h"
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <sys/stat.h>
#else
#include <sys/stat.h>
#include "zms/util/hex_decode.h"
#endif

typedef struct zms_rtmp_play_kick_ctx {
    zms_rtmp_session *s;
    unsigned token;
} zms_rtmp_play_kick_ctx;

int zms_rtmp_session_alive(zms_rtmp_session *s)
{
    return s && s->tcp && ztk_tcp_session_user(s->tcp) == s;
}

static void rtmp_play_kick_async(void *user)
{
    zms_rtmp_play_kick_ctx *ctx = (zms_rtmp_play_kick_ctx *)user;
    zms_rtmp_session *s;
    unsigned tok;

    if (!ctx) {
        return;
    }
    s = ctx->s;
    tok = ctx->token;
    free(ctx);
    if (!s || s->destroy_token != tok || s->destroy_scheduled) {
        return;
    }
    zms_rtmp_session_lock(s);
    if (!s->destroy_scheduled) {
        zms_rtmp_session_play_kick(s);
    }
    zms_rtmp_session_unlock(s);
}

static uint8_t *zms_rtmp_session_tag_buf(zms_rtmp_session *s, size_t need, uint8_t *stack,
                                         size_t stack_cap)
{
    ztk_poller *pol;

    if (need <= stack_cap) {
        return stack;
    }
    pol = s && s->tcp ? ztk_tcp_session_poller(s->tcp) : NULL;
    if (!s || !zms_buf_pool_slot_resize_poller(&s->play_tag_buf, &s->play_tag_cap, need, pol)) {
        return NULL;
    }
    return s->play_tag_buf;
}

static void rtmp_send_video_cfg_tag(zms_rtmp_session *s, const uint8_t *cfg, size_t clen)
{
    uint8_t body_stack[4096];
    const uint8_t *body = NULL;
    size_t body_len = 0;
    zms_codec_id fallback;

    if (!s || !s->rtmp_server || !cfg || clen == 0) {
        return;
    }
    fallback = (s->source && s->source->video.codec != ZMS_CODEC_INVALID) ? s->source->video.codec
                                                                          : ZMS_CODEC_INVALID;
    if (zms_flv_video_cfg_body(cfg, clen, fallback, body_stack, sizeof(body_stack), &body,
                               &body_len) != 1) {
        return;
    }
    rtmp_server_send_video(s->rtmp_server, body, body_len, 0);
}

static const uint8_t *rtmp_vod_video_config(const zms_rtmp_session *s, size_t *clen)
{
    const uint8_t *cfg;

    if (clen) {
        *clen = 0;
    }
    if (!s || !s->source) {
        return NULL;
    }
    if (s->vod_lane) {
        cfg = zms_vod_play_lane_video_config(s->vod_lane, clen);
        if (cfg && clen && *clen > 0) {
            return cfg;
        }
    }
    return zms_media_source_video_config(s->source, clen);
}

static const uint8_t *rtmp_vod_audio_config(const zms_rtmp_session *s, size_t *clen)
{
    const uint8_t *cfg;

    if (clen) {
        *clen = 0;
    }
    if (!s || !s->source) {
        return NULL;
    }
    if (s->vod_lane) {
        cfg = zms_vod_play_lane_audio_config(s->vod_lane, clen);
        if (cfg && clen && *clen > 0) {
            return cfg;
        }
    }
    return zms_media_source_audio_config(s->source, clen);
}

static uint64_t rtmp_play_pos_to_ms(double start, const zms_media_source *src, double duration_arg)
{
    double dur_sec = 0;

    if (start <= 0.0) {
        return 0;
    }
    if (src && zms_media_source_is_vod(src)) {
        dur_sec = zms_vod_source_duration_ms(src) / 1000.0;
    } else if (duration_arg > 0.0) {
        dur_sec = duration_arg;
    }
    if (dur_sec > 0.0 && start > dur_sec * 1.5) {
        return (uint64_t)(start + 0.5);
    }
    return (uint64_t)(start * 1000.0 + 0.5);
}

static double rtmp_vod_file_size_bytes(const zms_media_source *src)
{
    char path[512];
    struct stat st;

    if (!src || !zms_vod_resolve_file_path(src->app, src->stream, path, sizeof(path))) {
        return 0.0;
    }
    if (stat(path, &st) != 0) {
        return 0.0;
    }
    return (double)st.st_size;
}

static size_t amf_encode_key(uint8_t *out, size_t cap, const char *key)
{
    size_t klen = key ? strlen(key) : 0;
    if (cap < 2 + klen) {
        return 0;
    }
    /* AMF0 对象成员名：U16 长度 + UTF-8（无类型字节） */
    out[0] = (uint8_t)((klen >> 8) & 0xff);
    out[1] = (uint8_t)(klen & 0xff);
    if (klen) {
        memcpy(out + 2, key, klen);
    }
    return 2 + klen;
}

static size_t amf_encode_bool(uint8_t *out, size_t cap, int v)
{
    if (cap < 2) {
        return 0;
    }
    out[0] = ZMS_AMF_BOOLEAN;
    out[1] = v ? 1 : 0;
    return 2;
}

void zms_rtmp_session_send_data_onstatus(zms_rtmp_session *s, const char *code, const char *desc)
{
    uint8_t amf[256];
    size_t pos = 0;

    if (!s || !s->rtmp_server || !code) {
        return;
    }
    pos += zms_amf_encode_string(amf + pos, sizeof(amf) - pos, "onStatus");
    amf[pos++] = ZMS_AMF_OBJECT;
    pos += amf_encode_key(amf + pos, sizeof(amf) - pos, "level");
    pos += zms_amf_encode_string(amf + pos, sizeof(amf) - pos, "status");
    pos += amf_encode_key(amf + pos, sizeof(amf) - pos, "code");
    pos += zms_amf_encode_string(amf + pos, sizeof(amf) - pos, code);
    if (desc && desc[0]) {
        pos += amf_encode_key(amf + pos, sizeof(amf) - pos, "description");
        pos += zms_amf_encode_string(amf + pos, sizeof(amf) - pos, desc);
    }
    pos += zms_amf_encode_object_end(amf + pos, sizeof(amf) - pos);
    rtmp_server_send_script(s->rtmp_server, amf, pos, 0);
}

static void zms_rtmp_session_send_data_start(zms_rtmp_session *s)
{
    uint8_t amf[128];
    size_t pos = 0;

    if (!s || !s->rtmp_server) {
        return;
    }
    pos += zms_amf_encode_string(amf + pos, sizeof(amf) - pos, "onStatus");
    amf[pos++] = ZMS_AMF_OBJECT;
    pos += amf_encode_key(amf + pos, sizeof(amf) - pos, "code");
    pos += zms_amf_encode_string(amf + pos, sizeof(amf) - pos, "NetStream.Data.Start");
    pos += zms_amf_encode_object_end(amf + pos, sizeof(amf) - pos);
    rtmp_server_send_script(s->rtmp_server, amf, pos, 0);
}

static size_t rtmp_build_play_onmetadata(zms_rtmp_session *s, uint8_t *amf, size_t cap)
{
    size_t pos = 0;
    double width = 0, height = 0, fps = 0;
    double video_codec = 7.0, audio_codec = 10.0;
    double audio_rate = 44100.0, stereo = 1.0;
    double duration_sec = 0.0;
    zms_codec_id vc = ZMS_CODEC_INVALID;

    if (!s || !s->source || !amf || cap == 0) {
        return 0;
    }

    if (zms_media_source_is_vod(s->source)) {
        duration_sec = zms_vod_source_duration_ms(s->source) / 1000.0;
    }

    if (s->source->video.ready) {
        vc = s->source->video.codec;
    }
    if (vc == ZMS_CODEC_INVALID && s->source->gop_queue) {
        size_t vlen = 0;
        const uint8_t *vcfg = zms_media_source_video_config(s->source, &vlen);
        if (vcfg && vlen) {
            vc = zms_flv_video_config_codec(vcfg, vlen);
        }
    }
    video_codec = zms_flv_metadata_videocodecid(vc != ZMS_CODEC_INVALID ? vc : ZMS_CODEC_H264);

    if (zms_media_source_is_vod(s->source)) {
        double file_size = rtmp_vod_file_size_bytes(s->source);
        if (file_size > 0.0) {
            pos += amf_encode_key(amf + pos, cap - pos, "fileSize");
            pos += zms_amf_encode_number(amf + pos, cap - pos, file_size);
        }
    }

    if (s->source->video.ready) {
        width = (double)s->source->video.width;
        height = (double)s->source->video.height;
        fps = (double)s->source->video.fps;
    }
    if (s->source->audio.ready) {
        audio_rate = (double)s->source->audio.sample_rate;
        stereo = s->source->audio.channels > 1 ? 1.0 : 0.0;
    }

    pos += zms_amf_encode_string(amf + pos, cap - pos, "onMetaData");
    amf[pos++] = ZMS_AMF_OBJECT;
    pos += amf_encode_key(amf + pos, cap - pos, "duration");
    pos += zms_amf_encode_number(amf + pos, cap - pos, duration_sec);
    if (duration_sec > 0.0) {
        pos += amf_encode_key(amf + pos, cap - pos, "canSeekToEnd");
        pos += amf_encode_bool(amf + pos, cap - pos, 1);
    }
    pos += amf_encode_key(amf + pos, cap - pos, "width");
    pos += zms_amf_encode_number(amf + pos, cap - pos, width);
    pos += amf_encode_key(amf + pos, cap - pos, "height");
    pos += zms_amf_encode_number(amf + pos, cap - pos, height);
    if (fps > 0.0) {
        pos += amf_encode_key(amf + pos, cap - pos, "framerate");
        pos += zms_amf_encode_number(amf + pos, cap - pos, fps);
    }
    pos += amf_encode_key(amf + pos, cap - pos, "videocodecid");
    pos += zms_amf_encode_number(amf + pos, cap - pos, video_codec);
    pos += amf_encode_key(amf + pos, cap - pos, "audiocodecid");
    pos += zms_amf_encode_number(amf + pos, cap - pos, audio_codec);
    pos += amf_encode_key(amf + pos, cap - pos, "audiosamplerate");
    pos += zms_amf_encode_number(amf + pos, cap - pos, audio_rate);
    pos += amf_encode_key(amf + pos, cap - pos, "stereo");
    pos += zms_amf_encode_number(amf + pos, cap - pos, stereo);
    pos += zms_amf_encode_object_end(amf + pos, cap - pos);
    return pos;
}

static void zms_rtmp_session_send_play_onmetadata(zms_rtmp_session *s)
{
    uint8_t stack[4096];
    size_t meta_len;

    if (!s || !s->rtmp_server || !s->source) {
        return;
    }

    /* 播放侧 onMetaData：ffmpeg 对 videocodecid=12 / Enhanced hvc1 tag 更友好，便于 ffplay 走 legacy 路径。 */
    meta_len = rtmp_build_play_onmetadata(s, stack, sizeof(stack));
    if (meta_len == 0) {
        return;
    }
    rtmp_server_send_script(s->rtmp_server, stack, meta_len, 0);
}

static void rtmp_egress_on_tag(uint8_t msg_type, uint32_t tag_dts_ms, const uint8_t *body,
                               size_t len, void *user)
{
    zms_rtmp_session *s = (zms_rtmp_session *)user;

    if (!s || !s->rtmp_server || !body || len == 0) {
        return;
    }
    if (msg_type == ZMS_RTMP_MSG_AUDIO) {
        rtmp_server_send_audio(s->rtmp_server, body, len, tag_dts_ms);
        if (!s->logged_audio) {
            s->logged_audio = 1;
            ztk_debug("[rtmp] first_audio session=%u ts=%u len=%u", s->session_no,
                      (unsigned)tag_dts_ms, (unsigned)len);
        }
    } else if (msg_type == ZMS_RTMP_MSG_VIDEO) {
        rtmp_server_send_video(s->rtmp_server, body, len, tag_dts_ms);
        if (!s->logged_video) {
            s->logged_video = 1;
            ztk_debug("[rtmp] first_video session=%u ts=%u len=%u", s->session_no,
                      (unsigned)tag_dts_ms, (unsigned)len);
        }
    }
}

static void rtmp_play_resend_stream_config(zms_rtmp_session *s)
{
    size_t clen;
    uint8_t cfg_stack[4096];
    uint8_t *cfg_buf;

    if (!s || !s->rtmp_server || !s->source) {
        return;
    }
    if (!s->source->gop_queue) {
        return;
    }
    clen = 0;
    {
        size_t cfg_cap = 0;

        (void)zms_media_source_video_config(s->source, &cfg_cap);
        cfg_buf =
            zms_rtmp_session_tag_buf(s, cfg_cap > 0 ? cfg_cap : 1, cfg_stack, sizeof(cfg_stack));
        clen =
            cfg_buf ? zms_gop_queue_copy_video_config(s->source->gop_queue, cfg_buf, cfg_cap) : 0;
        if (clen) {
            rtmp_send_video_cfg_tag(s, cfg_buf, clen);
        }
    }
    {
        uint8_t acfg_stack[4096];
        uint8_t *acfg_buf;
        size_t acfg_cap = 0;

        (void)zms_media_source_audio_config(s->source, &acfg_cap);
        acfg_buf = zms_rtmp_session_tag_buf(s, acfg_cap > 0 ? acfg_cap : 1, acfg_stack,
                                            sizeof(acfg_stack));
        clen = acfg_buf ? zms_gop_queue_copy_audio_config(s->source->gop_queue, acfg_buf, acfg_cap)
                        : 0;
        if (clen) {
            rtmp_server_send_audio(s->rtmp_server, acfg_buf, clen, 0);
        }
    }
}

static void rtmp_play_on_snap(void *user)
{
    zms_rtmp_session *s = (zms_rtmp_session *)user;

    if (!s) {
        return;
    }
    s->play_video_armed = 0;
    zms_egress_clock_unlock(&s->play_clk);
    rtmp_play_resend_stream_config(s);
}

static void rtmp_play_on_slow_consumer(void *user)
{
    zms_rtmp_session *s = (zms_rtmp_session *)user;

    if (!s || s->destroy_scheduled) {
        return;
    }
    ztk_warn("[rtmp] reader_kick session=%u", s->session_no);
    zms_rtmp_session_play_teardown(s);
    s->state = ZMS_RTMP_SESSION_STATE_IDLE;
    s->source = NULL;
    if (s->tcp) {
        ztk_tcp_session_close(s->tcp);
    }
}

void zms_rtmp_session_egress_create(zms_rtmp_session *s)
{
    zms_egress_pipeline_opts ecfg;
    zms_egress_flv_bind fbind;

    if (!s || !s->source) {
        return;
    }
    zms_egress_pipeline_destroy(s->egress_pipe);
    s->egress_pipe = NULL;
    if (!s->play.readers.gop && !s->play.readers.vod && !s->vod_reader) {
        return;
    }
    memset(&ecfg, 0, sizeof(ecfg));
    memset(&fbind, 0, sizeof(fbind));
    fbind.source = s->source;
    fbind.play_clk = &s->play_clk;
    fbind.timeline = &s->play_timeline;
    fbind.on_tag = rtmp_egress_on_tag;
    fbind.user = s;
    fbind.video_armed = &s->play_video_armed;
    ecfg.wire = ZMS_WIRE_FORMAT_RTMP;
    ecfg.reader = &s->play;
    ecfg.flv = &fbind;
    s->egress_pipe = zms_egress_pipeline_create(&ecfg);
    if (s->egress_pipe && s->tcp) {
        zms_egress_pipeline_bind_poller(s->egress_pipe, ztk_tcp_session_poller(s->tcp));
    }
}

void zms_rtmp_session_egress_close(zms_rtmp_session *s)
{
    zms_play_binding bind;

    if (!s) {
        return;
    }
    zms_egress_pipeline_destroy(s->egress_pipe);
    s->egress_pipe = NULL;
    memset(&bind, 0, sizeof(bind));
    bind.source = &s->source;
    bind.play = &s->play;
    bind.gop_reader = &s->gop_reader;
    bind.vod_reader = &s->vod_reader;
    bind.vod_lane = &s->vod_lane;
    bind.reader_attached = &s->play_reader_attached;
    bind.play_start_ms = &s->live_play_start_ms;
    bind.player = "rtmp";
    zms_play_binding_close_readers(&bind);
}

void zms_rtmp_session_play_bootstrap(zms_rtmp_session *s)
{
    size_t clen;

    if (!s || !s->rtmp_server || !s->source || s->play_boot_sent) {
        return;
    }

    /*
     * VOD 不在此发 onMetaData / Data.Start / PublishNotify，否则 ffmpeg RTMP 可能报 Invalid data。
     * 直播 bootstrap：plain onMetaData + hvc1。
     */
    if (!zms_media_source_is_vod(s->source)) {
        zms_rtmp_session_send_data_start(s);
        zms_rtmp_session_send_data_onstatus(s, "NetStream.Play.PublishNotify", "Now published.");
        zms_rtmp_session_send_play_onmetadata(s);
    }

    /* 兼容 ffmpeg：先发 video seq header，再发 audio seq header */
    clen = 0;
    if (s->source->gop_queue) {
        size_t cfg_cap = 0;
        uint8_t cfg_stack[4096];
        uint8_t *cfg_buf;

        (void)zms_media_source_video_config(s->source, &cfg_cap);
        cfg_buf =
            zms_rtmp_session_tag_buf(s, cfg_cap > 0 ? cfg_cap : 1, cfg_stack, sizeof(cfg_stack));
        clen =
            cfg_buf ? zms_gop_queue_copy_video_config(s->source->gop_queue, cfg_buf, cfg_cap) : 0;
        if (clen) {
            rtmp_send_video_cfg_tag(s, cfg_buf, clen);
        }
    } else if (zms_media_source_is_vod(s->source)) {
        const uint8_t *vcfg = rtmp_vod_video_config(s, &clen);
        uint8_t cfg_stack[4096];
        uint8_t *cfg_buf;

        if (vcfg && clen) {
            cfg_buf = zms_rtmp_session_tag_buf(s, clen, cfg_stack, sizeof(cfg_stack));
            if (cfg_buf) {
                memcpy(cfg_buf, vcfg, clen);
                rtmp_send_video_cfg_tag(s, cfg_buf, clen);
            }
        }
    }

    clen = 0;
    if (s->source->gop_queue) {
        uint8_t acfg_stack[4096];
        uint8_t *acfg_buf;
        size_t acfg_cap = 0;

        (void)zms_media_source_audio_config(s->source, &acfg_cap);
        acfg_buf = zms_rtmp_session_tag_buf(s, acfg_cap > 0 ? acfg_cap : 1, acfg_stack,
                                            sizeof(acfg_stack));
        clen = acfg_buf ? zms_gop_queue_copy_audio_config(s->source->gop_queue, acfg_buf, acfg_cap)
                        : 0;
        if (clen) {
            rtmp_server_send_audio(s->rtmp_server, acfg_buf, clen, 0);
        }
    } else if (zms_media_source_is_vod(s->source)) {
        const uint8_t *acfg = rtmp_vod_audio_config(s, &clen);
        if (acfg && clen) {
            rtmp_server_send_audio(s->rtmp_server, acfg, clen, 0);
        }
    } else if (s->source->has_audio) {
        uint8_t asc[16];
        size_t asc_len = 0;
        if (s->source->audio.ready && s->source->audio.asc_hex[0]) {
            if (zms_hex_decode(s->source->audio.asc_hex, asc, sizeof(asc), &asc_len) != ZTK_OK) {
                asc_len = 0;
            }
        }
        if (asc_len == 0) {
            char hex[16];
            int rate = s->source->audio.ready ? s->source->audio.sample_rate : 44100;
            int ch = s->source->audio.ready ? s->source->audio.channels : 2;
            if (zms_aac_build_config_hex(rate, ch, hex, sizeof(hex)) &&
                zms_hex_decode(hex, asc, sizeof(asc), &asc_len) == ZTK_OK) {
                ztk_debug("[rtmp] synthesize_aac_config session=%u hex=%s", s->session_no, hex);
            }
        }
        if (asc_len) {
            uint8_t buf[32];
            size_t tag_len = 0;
            if (zms_rtmp_aac_seq_header(asc, asc_len, buf, sizeof(buf), &tag_len) == ZTK_OK) {
                rtmp_server_send_audio(s->rtmp_server, buf, tag_len, 0);
            }
        }
    }

    s->play_boot_sent = 1;
}

void zms_rtmp_session_play_fail(zms_rtmp_session *s)
{
    if (!s) {
        return;
    }
    zms_rtmp_session_play_teardown(s);
    s->state = ZMS_RTMP_SESSION_STATE_IDLE;
    s->source = NULL;
    if (s->tcp) {
        ztk_tcp_session_close(s->tcp);
    }
}

void zms_rtmp_session_play_kick_finish(zms_rtmp_session *s)
{
    if (!s) {
        return;
    }

    if (!s->vod_reader && s->gop_reader) {
        zms_egress_clock_arm(&s->play_clk);
        zms_egress_live_seek_rtsp_attach(s->gop_reader);
    }

    zms_rtmp_session_play_bootstrap(s);
    if (s->source && zms_media_source_is_vod(s->source)) {
        s->play_video_armed = 1;
    }

    if (s->source && !s->play_reader_attached) {
        zms_media_source_reader_add(s->source);
        s->live_play_start_ms = ztk_monotonic_ms();
        zms_media_event_play(s->source, "rtmp");
        s->play_reader_attached = 1;
    }

    ztk_info("[rtmp] play_start session=%u app=%s stream=%s video=%d audio=%d es=%d vod_lag=%zu",
             s->session_no, s->app, s->stream, s->source ? s->source->has_video : 0,
             s->source ? s->source->has_audio : 0, s->gop_reader ? 1 : 0,
             s->vod_reader ? zms_vod_buffer_reader_lag(s->vod_reader) : 0);

    s->play_boot_pending = 0;
    zms_rtmp_session_play_flush_nolock(s);
    if (s->gop_reader && !s->play_video_armed) {
        int burst;

        for (burst = 0; burst < 12 && !s->play_video_armed; ++burst) {
            zms_rtmp_session_play_flush_nolock(s);
        }
    }
}

void zms_rtmp_session_schedule_play_kick(zms_rtmp_session *s)
{
    zms_rtmp_play_kick_ctx *ctx;
    ztk_poller *pol;

    if (!s || !s->play_boot_pending) {
        return;
    }
    pol = s->tcp ? ztk_tcp_session_poller(s->tcp) : NULL;
    /*
     * onplay 在 zms_rtmp_session_lock（on_recv）下运行。may_sync=1 会在 poller 线程内联
     * play_kick，与非递归 play_mtx 死锁。
     */
    if (!pol) {
        zms_rtmp_session_play_kick(s);
        return;
    }
    ctx = (zms_rtmp_play_kick_ctx *)malloc(sizeof(*ctx));
    if (!ctx) {
        zms_rtmp_session_play_kick(s);
        return;
    }
    ctx->s = s;
    ctx->token = s->destroy_token;
    if (ztk_poller_async(pol, rtmp_play_kick_async, ctx, 0) != ZTK_OK) {
        free(ctx);
        zms_rtmp_session_play_kick(s);
    }
}

void zms_rtmp_session_play_kick(zms_rtmp_session *s)
{
    if (!s || s->destroy_scheduled || !s->play_boot_pending) {
        return;
    }

    /* VOD lane 打开前先回 Play.Start，便于 VLC/librtmp 立即 seek */
    if (rtmp_server_start(s->rtmp_server, 0, NULL) != 0) {
        ztk_warn("[rtmp] play_start_failed session=%u app=%s stream=%s", s->session_no, s->app,
                 s->stream);
        zms_rtmp_session_play_fail(s);
        return;
    }
    zms_rtmp_session_flush_tcp(s);

    if (s->source && zms_media_source_is_vod(s->source)) {
        uint64_t start_ms = s->vod_play_start_ms;

        s->vod_play_start_ms = 0;
        if (!s->vod_lane && zms_rtmp_session_vod_lane_try_async(s, start_ms)) {
            return;
        }
        if (!zms_rtmp_session_vod_attach(s, start_ms)) {
            ztk_warn("[rtmp] vod_attach_failed session=%u app=%s stream=%s", s->session_no, s->app,
                     s->stream);
            zms_rtmp_session_play_fail(s);
            return;
        }
        zms_rtmp_session_play_kick_vod_prime(s, start_ms);
        zms_rtmp_session_play_kick_finish(s);
        return;
    }

    if (!s->gop_reader) {
        zms_rtmp_session_play_readers_attach(s);
    }
    zms_rtmp_session_play_kick_finish(s);
}

int rtmp_srv_onplay(void *param, const char *app_name, const char *stream_name, double start,
                    double duration, uint8_t reset)
{
    zms_rtmp_session *s = (zms_rtmp_session *)param;
    zms_media_tuple tuple;
    uint64_t start_ms;

    if (!s) {
        return -1;
    }

    s->source = zms_rtmp_session_find_play_source(app_name, stream_name, s->app, sizeof(s->app),
                                                  s->stream, sizeof(s->stream));
    start_ms = rtmp_play_pos_to_ms(start, s->source, duration);
    ztk_info("[rtmp] play_request session=%u app=%s stream=%s start=%.3f duration=%.3f reset=%u "
             "seek_ms=%llu",
             s->session_no, s->app, s->stream, start, duration, (unsigned)reset,
             (unsigned long long)start_ms);
    if (!s->source ||
        (!zms_media_source_use_gop_queue_play(s->source) && !zms_media_source_is_vod(s->source))) {
        ztk_warn("[rtmp] play_not_found session=%u app=%s stream=%s", s->session_no, s->app,
                 s->stream);
        return -1;
    }

    /* VLC/ffplay 拖拽：会话已在播放时第二次 play(start>0) 或 play(reset) */
    if (s->state == ZMS_RTMP_SESSION_STATE_PLAYING && !s->play_boot_pending && s->vod_lane &&
        (start_ms > 0 || reset)) {
        if (start_ms == 0 && reset) {
            start_ms = 0;
        }
        return zms_rtmp_session_vod_replay_seek(s, start_ms);
    }

    zms_media_tuple_from_source(s->source, &tuple);
    if (!zms_webhook_allow_play(&tuple, "rtmp", s->tcp, NULL)) {
        ztk_warn("[rtmp] play_denied session=%u app=%s stream=%s", s->session_no, s->app,
                 s->stream);
        return -1;
    }

    if (!zms_media_source_is_vod(s->source) &&
        zms_session_capability_check_source(ZMS_PROTO_CAP_RTMP_PLAY, s->source) != ZTK_OK) {
        zms_session_capability_log_reject("rtmp", s->source, ZMS_PROTO_CAP_RTMP_PLAY);
        return -1;
    }

    s->vod_play_start_ms = start_ms;

    s->play_boot_sent = 0;
    s->play_video_armed = 0;
    s->play_live_catchup = 0;
    s->play_lag_resync_ms = 0;
    s->vod_pause_pos_valid = 0;
    s->logged_audio = 0;
    s->logged_video = 0;
    zms_egress_clock_init(&s->play_clk);
    zms_egress_clock_set_scale(&s->play_clk, 1.0);
    zms_mux_av_timeline_reset(&s->play_timeline);
    s->play_boot_pending = 1;
    s->state = ZMS_RTMP_SESSION_STATE_PLAYING;

    /* 延后到 rtmp_server_input 返回后再 async kick，避免与配置更新竞态导致 HLS 等路径 UAF */
    zms_rtmp_session_schedule_play_kick(s);
    return RTMP_SERVER_ASYNC_START;
}

void zms_rtmp_session_play_flush_nolock(zms_rtmp_session *s)
{
    if (!s || s->destroy_scheduled || s->state != ZMS_RTMP_SESSION_STATE_PLAYING ||
        !s->rtmp_server || !s->tcp) {
        return;
    }

    if (s->egress_pipe) {
        if (s->vod_reader) {
            zms_flv_vod_egress_bind vcfg;

            if (zms_egress_clock_is_paused(&s->play_clk)) {
                return;
            }
            if (s->vod_lane) {
                zms_vod_play_lane_demux_fill(s->vod_lane, 32);
            }
            memset(&vcfg, 0, sizeof(vcfg));
            vcfg.vod_rd = s->vod_reader;
            vcfg.play_clk = &s->play_clk;
            vcfg.catchup_left = &s->play_vod_catchup;
            vcfg.es_buf = &s->play_es_buf;
            vcfg.es_cap = &s->play_es_cap;
            vcfg.pace_when_locked = 1;
            (void)zms_egress_pipeline_pump_flv_vod(s->egress_pipe, &vcfg, 0, s->session_no);
        } else if (s->gop_reader) {
            zms_egress_live_state live_st;
            size_t lag = zms_gop_reader_lag(s->gop_reader);
            int epoch_on = zms_egress_clock_epoch_locked(&s->play_clk);
            int budget = zms_egress_live_pump_budget(epoch_on, s->play_live_catchup, lag,
                                                     (int)ZMS_EGRESS_FLV_BUDGET_LIVE);

            memset(&live_st, 0, sizeof(live_st));
            live_st.live_catchup_done = &s->play_live_catchup;
            live_st.live_resync_at_ms = &s->play_lag_resync_ms;
            live_st.play_clk = &s->play_clk;
            live_st.on_snap = rtmp_play_on_snap;
            live_st.snap_user = s;
            live_st.on_slow_consumer = rtmp_play_on_slow_consumer;
            live_st.slow_consumer_user = s;
            (void)zms_egress_pipeline_pump_flv_live(s->egress_pipe, budget, 500, s->session_no,
                                                    &live_st);
        }
        zms_rtmp_session_flush_tcp(s);
    }
}

void zms_rtmp_session_play_flush(zms_rtmp_session *s)
{
    zms_rtmp_session_lock(s);
    zms_rtmp_session_play_flush_nolock(s);
    zms_rtmp_session_unlock(s);
}
