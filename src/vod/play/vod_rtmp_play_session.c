/**
 * @file rtmp_vod_play_session.c
 * @brief RTMP 点播路径：lane 打开/挂接，以及 NetStream onpause/onseek/onplayctrl/getStreamLength。
 *        归属 vod（src/vod/play）；由 RTMP NetStream 命令分发与直播 session 核心驱动。
 *
 * Copyright (c) zero-media-server
 */
#include "session/rtmp/rtmp_session_internal.h"
#include "zms/session/session_dispatcher.h"
#include "zms/egress/egress_live_policy.h"
#include "zms/engine/gop/gop_queue.h"
#include "zms/engine/media_clock.h"
#include "zms/vod/play/vod_play_lane.h"
#include "zms/vod/io/vod_source.h"
#include "zms/vod/io/vod_reader.h"
#include "zms/egress/egress_pacing.h"
#include "zms/egress/egress_clock.h"
#include "zms/vod/io/vod_thread_pool.h"
#include "ztk/net/tcp_server.h"
#include "ztk/poller/poller.h"
#include "ztk/util/log.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef struct zms_rtmp_vod_lane_ctx {
    zms_rtmp_session *s;
    zms_media_source *src;
    zms_vod_play_lane *lane;
    uint64_t start_ms;
    unsigned token;
} zms_rtmp_vod_lane_ctx;

/** RTMP 点播 catchup：seek/resume 后预填 client buffer */
#define ZMS_RTMP_VOD_PLAY_FRAME_BUDGET_SEEK 96u

static uint32_t rtmp_netstream_ms_arg(double val, const zms_media_source *src)
{
    uint64_t dur_ms = 0;
    uint64_t ms;

    if (val <= 0.0) {
        return 0;
    }
    ms = (uint64_t)(val + 0.5);
    if (src && zms_media_source_is_vod(src)) {
        dur_ms = zms_vod_source_duration_ms(src);
    }
    if (dur_ms > 0 && ms > dur_ms) {
        ms = dur_ms;
    }
    return ms > UINT32_MAX ? UINT32_MAX : (uint32_t)ms;
}

static void zms_rtmp_session_vod_pump_hold(zms_rtmp_session *s, int hold)
{
    if (s && s->vod_lane) {
        zms_vod_play_lane_set_pump_hold(s->vod_lane, hold);
    }
}

static void zms_rtmp_session_vod_set_play_scale(zms_rtmp_session *s, double scale)
{
    if (!s || scale <= 0.0 || scale > 16.0) {
        return;
    }
    zms_egress_clock_set_scale(&s->play_clk, scale);
    ztk_info("[rtmp] vod_scale session=%u scale=%.3f", s->session_no, scale);
}

static void zms_rtmp_session_vod_save_pause_pos(zms_rtmp_session *s)
{
    zms_gop_slot slot;

    if (!s || !s->vod_reader) {
        return;
    }
    if (zms_vod_buffer_reader_peek_muxed(s->vod_reader, &slot)) {
        s->vod_pause_ms = slot.dts_ms;
        s->vod_pause_pos_valid = 1;
    }
}

static void zms_rtmp_session_vod_resume_after_seek(zms_rtmp_session *s)
{
    if (!s) {
        return;
    }
    zms_rtmp_session_vod_pump_hold(s, 0);
    if (zms_egress_clock_is_paused(&s->play_clk)) {
        zms_egress_clock_resume(&s->play_clk);
    }
}

ztk_err_t zms_rtmp_session_play_vod_lane_attach(zms_rtmp_session *s, zms_media_source *src,
                                                uint64_t seek_ms)
{
    ztk_poller *pol;
    uint64_t play_ms = 0;

    if (!s || !src) {
        return ZTK_ERR_INVALID;
    }
    pol = s->tcp ? ztk_tcp_session_poller(s->tcp) : NULL;
    if (!s->vod_lane) {
        s->vod_lane = zms_vod_play_lane_open(src, pol);
        if (!s->vod_lane) {
            return ZTK_ERR_STATE;
        }
    }
    if (pol) {
        zms_vod_reader_bind_poller_lite(zms_vod_play_lane_reader(s->vod_lane), pol);
    }
    if (seek_ms) {
        play_ms = zms_vod_play_lane_seek_ms(s->vod_lane, seek_ms);
    } else {
        (void)zms_vod_play_lane_prepare(s->vod_lane, 0);
    }
    zms_vod_play_lane_prefill(s->vod_lane);
    zms_vod_play_lane_align_reader(s->vod_lane);
    s->vod_reader = zms_vod_play_lane_buffer_reader(s->vod_lane);
    if (!s->vod_reader) {
        return ZTK_ERR_STATE;
    }
    s->vod_last_seek_ms = play_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)play_ms;
    s->gop_reader = NULL;
    /* lane 持有 fifo reader；勿单独 detach play.readers.vod。 */
    s->play.readers.gop = NULL;
    s->play.readers.vod = NULL;
    zms_egress_source_close(&s->play);
    s->play.source = src;
    s->play.is_live = 0;
    s->play.readers.vod = s->vod_reader;
    return ZTK_OK;
}

int zms_rtmp_session_vod_attach(zms_rtmp_session *s, uint64_t seek_ms)
{
    zms_session_play_opts pcfg;

    if (!s || !s->source) {
        return 0;
    }
    memset(&pcfg, 0, sizeof(pcfg));
    pcfg.player = ZMS_SESSION_RTMP;
    pcfg.seek_ms = seek_ms;
    return zms_session_attach_play(ZMS_SESSION_RTMP, s, s->source, &pcfg) == ZTK_OK;
}

static uint32_t rtmp_vod_buffer_anchor_ms(zms_vod_buffer_reader *rd, uint32_t ms)
{
    zms_gop_slot slot;

    if (rd && zms_vod_buffer_reader_peek_muxed(rd, &slot)) {
        return slot.dts_ms;
    }
    return ms;
}

static void zms_rtmp_session_vod_do_seek(zms_rtmp_session *s, uint32_t ms, int flush_after)
{
    size_t lag;
    int same_pos;

    if (!s || !s->source || !zms_media_source_is_vod(s->source)) {
        return;
    }
    if (!s->vod_lane && !zms_rtmp_session_vod_attach(s, ms)) {
        ztk_warn("[rtmp] seek_failed session=%u reason=vod_lane_unavailable", s->session_no);
        return;
    }
    zms_rtmp_session_vod_resume_after_seek(s);

    same_pos = (ms == s->vod_last_seek_ms);
    if (same_pos) {
        lag = s->vod_reader ? zms_vod_buffer_reader_lag(s->vod_reader) : 0;
        if (lag > ZMS_EGRESS_CATCHUP_LAG) {
            s->play_vod_catchup = (int)ZMS_RTMP_VOD_PLAY_FRAME_BUDGET_SEEK;
            ztk_debug("[rtmp] seek_dedupe session=%u ms=%u lag=%zu", s->session_no, (unsigned)ms,
                      lag);
            if (flush_after && s->state == ZMS_RTMP_SESSION_STATE_PLAYING &&
                !s->play_boot_pending) {
                zms_rtmp_session_play_flush_nolock(s);
            }
            return;
        }
        ztk_debug("[rtmp] seek_refresh session=%u ms=%u fifo_lag=%zu", s->session_no, (unsigned)ms,
                  lag);
    } else {
        s->vod_last_seek_ms = ms;
        zms_mux_av_timeline_reset(&s->play_timeline);
    }

    ms = (uint32_t)zms_vod_play_lane_seek_ms(s->vod_lane, ms);
    zms_vod_play_lane_prefill(s->vod_lane);
    zms_vod_play_lane_align_reader(s->vod_lane);
    ms = rtmp_vod_buffer_anchor_ms(s->vod_reader, ms);
    zms_egress_clock_rebase(&s->play_clk, ms);
    s->play_video_armed = 0;
    s->logged_video = 0;
    s->logged_audio = 0;
    s->play_vod_catchup = (int)ZMS_RTMP_VOD_PLAY_FRAME_BUDGET_SEEK;
    ztk_info("[rtmp] seek session=%u ms=%u", s->session_no, (unsigned)ms);
    if (flush_after && s->state == ZMS_RTMP_SESSION_STATE_PLAYING && !s->play_boot_pending) {
        zms_rtmp_session_play_flush_nolock(s);
    }
}

int zms_rtmp_session_vod_replay_seek(zms_rtmp_session *s, uint64_t start_ms)
{
    uint32_t seek_ms;

    if (!s || !s->vod_lane) {
        return -1;
    }
    seek_ms = start_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)start_ms;
    zms_rtmp_session_vod_do_seek(s, seek_ms, 0);
    zms_rtmp_session_send_data_onstatus(s, "NetStream.Play.Reset", "Resetting and playing.");
    zms_rtmp_session_send_data_onstatus(s, "NetStream.Seek.Notify", "Seeking.");
    zms_rtmp_session_send_data_onstatus(s, "NetStream.Play.Start", "Start video on demand.");
    zms_rtmp_session_play_flush_nolock(s);
    zms_rtmp_session_flush_tcp(s);
    return 0;
}

void zms_rtmp_session_play_kick_vod_prime(zms_rtmp_session *s, uint64_t start_ms)
{
    uint32_t kick_ms = start_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)start_ms;

    if (!s || !s->vod_lane) {
        return;
    }
    if (start_ms) {
        s->vod_last_seek_ms = kick_ms;
        s->play_video_armed = 0;
    }
    s->play_vod_catchup = (int)ZMS_RTMP_VOD_PLAY_FRAME_BUDGET_SEEK;
    zms_egress_clock_arm(&s->play_clk);
    (void)zms_egress_clock_lock_epoch(&s->play_clk, kick_ms);
    zms_mux_av_timeline_reset(&s->play_timeline);
}

static void rtmp_vod_lane_open_blocking(void *user)
{
    zms_rtmp_vod_lane_ctx *job = (zms_rtmp_vod_lane_ctx *)user;

    if (!job) {
        return;
    }
    job->lane = zms_vod_play_lane_open(job->src, NULL);
    if (!job->lane) {
        return;
    }
    if (job->start_ms) {
        (void)zms_vod_play_lane_seek_ms(job->lane, job->start_ms);
    } else {
        (void)zms_vod_play_lane_prepare(job->lane, 0);
    }
    zms_vod_play_lane_prefill(job->lane);
    zms_vod_play_lane_align_reader(job->lane);
    if (!zms_vod_play_lane_buffer_reader(job->lane)) {
        zms_vod_play_lane_close(job->lane);
        job->lane = NULL;
    }
}

static void rtmp_vod_lane_open_on_io(void *user)
{
    zms_rtmp_vod_lane_ctx *job = (zms_rtmp_vod_lane_ctx *)user;
    zms_rtmp_session *s;
    ztk_poller *pol;
    uint64_t start_ms;

    if (!job) {
        return;
    }
    s = job->s;
    start_ms = job->start_ms;
    if (!zms_rtmp_session_alive(s) || !s || s->destroy_token != job->token ||
        s->destroy_scheduled) {
        if (job->lane) {
            zms_vod_play_lane_close(job->lane);
        }
        free(job);
        return;
    }
    if (!job->lane) {
        ztk_warn("[rtmp] vod_lane_open_failed session=%u app=%s stream=%s", s->session_no, s->app,
                 s->stream);
        zms_rtmp_session_lock(s);
        if (!s->destroy_scheduled && s->play_boot_pending) {
            zms_rtmp_session_play_fail(s);
        }
        zms_rtmp_session_unlock(s);
        free(job);
        return;
    }
    pol = s->tcp ? ztk_tcp_session_poller(s->tcp) : NULL;
    s->vod_lane = job->lane;
    job->lane = NULL;
    free(job);

    zms_rtmp_session_lock(s);
    if (!s->destroy_scheduled && s->play_boot_pending) {
        if (zms_rtmp_session_vod_attach(s, start_ms)) {
            zms_rtmp_session_play_kick_vod_prime(s, start_ms);
            zms_rtmp_session_play_kick_finish(s);
        } else {
            zms_rtmp_session_play_fail(s);
        }
    }
    zms_rtmp_session_unlock(s);
    (void)pol;
}

int zms_rtmp_session_vod_lane_try_async(zms_rtmp_session *s, uint64_t start_ms)
{
    ztk_poller *pol;
    zms_rtmp_vod_lane_ctx *job;

    if (!zms_vod_thread_pool_enabled() || !s || s->vod_lane) {
        return 0;
    }
    pol = s->tcp ? ztk_tcp_session_poller(s->tcp) : NULL;
    if (!pol) {
        return 0;
    }
    job = (zms_rtmp_vod_lane_ctx *)calloc(1, sizeof(*job));
    if (!job) {
        return 0;
    }
    job->s = s;
    job->src = s->source;
    job->start_ms = start_ms;
    job->token = s->destroy_token;
    if (zms_vod_thread_pool_run(pol, rtmp_vod_lane_open_blocking, rtmp_vod_lane_open_on_io, job) !=
        ZTK_OK) {
        free(job);
        return 0;
    }
    return 1;
}

int rtmp_srv_onpause(void *param, int pause, uint32_t ms)
{
    zms_rtmp_session *s = (zms_rtmp_session *)param;
    uint32_t seek_ms = 0;

    if (!s || !s->source || !zms_media_source_is_vod(s->source)) {
        return 0;
    }
    seek_ms = rtmp_netstream_ms_arg((double)ms, s->source);
    ztk_info("[rtmp] pause session=%u pause=%d ms=%u", s->session_no, pause, (unsigned)seek_ms);
    if (pause) {
        if (seek_ms > 0) {
            zms_rtmp_session_vod_do_seek(s, seek_ms, 0);
        } else {
            zms_rtmp_session_vod_save_pause_pos(s);
        }
        zms_egress_clock_pause(&s->play_clk);
        zms_rtmp_session_vod_pump_hold(s, 1);
    } else {
        if (seek_ms > 0) {
            zms_rtmp_session_vod_do_seek(s, seek_ms, 0);
        } else if (s->vod_pause_pos_valid) {
            zms_rtmp_session_vod_do_seek(s, s->vod_pause_ms, 0);
            s->vod_pause_pos_valid = 0;
        }
        zms_rtmp_session_vod_resume_after_seek(s);
        s->play_vod_catchup = (int)ZMS_RTMP_VOD_PLAY_FRAME_BUDGET_SEEK;
    }
    if (!pause && s->state == ZMS_RTMP_SESSION_STATE_PLAYING && !s->play_boot_pending) {
        zms_rtmp_session_play_flush_nolock(s);
    }
    return 0;
}

int rtmp_srv_onplayctrl(void *param, double speed)
{
    zms_rtmp_session *s = (zms_rtmp_session *)param;

    if (!s || !s->source || !zms_media_source_is_vod(s->source)) {
        return 0;
    }
    zms_rtmp_session_vod_set_play_scale(s, speed);
    s->play_vod_catchup = (int)ZMS_RTMP_VOD_PLAY_FRAME_BUDGET_SEEK;
    if (s->state == ZMS_RTMP_SESSION_STATE_PLAYING && !s->play_boot_pending) {
        zms_rtmp_session_play_flush_nolock(s);
    }
    return 0;
}

int rtmp_srv_onseek(void *param, uint32_t ms)
{
    zms_rtmp_session *s = (zms_rtmp_session *)param;
    uint32_t seek_ms;

    if (!s) {
        return 0;
    }
    seek_ms = rtmp_netstream_ms_arg((double)ms, s->source);
    ztk_info("[rtmp] seek_request session=%u raw_ms=%u seek_ms=%u", s->session_no, (unsigned)ms,
             (unsigned)seek_ms);
    if (zms_egress_clock_is_paused(&s->play_clk)) {
        zms_egress_clock_resume(&s->play_clk);
    }
    /* 媒体必须在 librtmp 发出 Seek.Notify/Play.Start 之后再推；flush 留给 on_recv */
    zms_rtmp_session_vod_do_seek(s, seek_ms, 0);
    return 0;
}

int rtmp_srv_ongetduration(void *param, const char *app_name, const char *stream_name,
                           double *duration)
{
    zms_rtmp_session *s = (zms_rtmp_session *)param;
    zms_media_source *src;
    uint64_t dur_ms = 0;

    if (!duration) {
        return 0;
    }
    *duration = 0.0;
    src = s && s->source ? s->source : NULL;
    if (!src && app_name && stream_name) {
        src = zms_media_source_find_api(ZMS_SCHEMA_RTMP, app_name, stream_name);
    }
    if (src && zms_media_source_is_vod(src)) {
        dur_ms = zms_vod_source_duration_ms(src);
    }
    if (!dur_ms && app_name && stream_name) {
        dur_ms = zms_vod_probe_duration_ms(app_name, stream_name);
    }
    if (dur_ms > 0) {
        *duration = dur_ms / 1000.0;
    }
    if (*duration > 0.0) {
        ztk_debug("[rtmp] get_stream_length session=%u sec=%.3f", s ? s->session_no : 0, *duration);
    }
    return 0;
}
