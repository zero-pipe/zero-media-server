/**
 * @file media_clock.c
 * @brief 入站 DeltaStamp、media_timeline 与出站时钟辅助。
 *
 * Copyright (c) zero-media-server
 */
#include "zms/engine/media_clock.h"
#include "zms/engine/media/media_limits.h"
#include "ztk/util/timer.h"
#include <string.h>

#define ZMS_MPEGTS_CLOCK_HZ 90000u
#define ZMS_NTP_UNIX_OFFSET 2208988800ULL
#define ZMS_STAMP_MAX_DELTA_MS 300u
#define ZMS_STAMP_SYNC_DRIFT_MS 300
#define ZMS_STAMP_SYNC_INPUT_MS 5000

static int64_t i64_abs(int64_t v)
{
    return v >= 0 ? v : -v;
}

void zms_track_stamp_reset(zms_track_stamp *s)
{
    if (!s) {
        return;
    }
    uint32_t max_delta = s->max_delta_ms > 0 ? s->max_delta_ms : ZMS_STAMP_MAX_DELTA_MS;
    zms_track_stamp *master = s->sync_master;
    memset(s, 0, sizeof(*s));
    s->max_delta_ms = max_delta;
    s->sync_master = master;
    s->last_delta = 1;
}

void zms_track_stamp_sync_to(zms_track_stamp *s, zms_track_stamp *master, int count)
{
    if (!s || !master) {
        return;
    }
    s->sync_master = master;
    s->need_sync += count > 0 ? count : 1;
}

static int64_t stamp_delta(zms_track_stamp *s, int64_t stamp)
{
    if (!s->last_stamp) {
        if (stamp) {
            s->last_stamp = stamp;
        }
        return 0;
    }

    int64_t ret = stamp - s->last_stamp;
    if (ret >= 0) {
        s->last_stamp = stamp;
        if (ret > (int64_t)s->max_delta_ms) {
            s->need_sync++;
            return s->last_delta > 0 ? s->last_delta : 1;
        }
        s->last_delta = ret > 0 ? ret : 1;
        return ret;
    }

    s->last_stamp = stamp;
    if (-ret > (int64_t)s->max_delta_ms) {
        s->need_sync++;
        return s->last_delta > 0 ? s->last_delta : 1;
    }
    return ret;
}

static void stamp_sync_l(zms_track_stamp *s)
{
    zms_track_stamp *m = s->sync_master;
    if (!m || !m->last_dts_in) {
        return;
    }
    if (!s->need_sync && !m->need_sync) {
        return;
    }

    int64_t in_diff = s->last_dts_in - m->last_dts_in;
    if (i64_abs(in_diff) >= ZMS_STAMP_SYNC_INPUT_MS && s->need_sync <= 3) {
        return;
    }

    int64_t rel_diff = s->relative_ms - m->relative_ms;
    int64_t dts_diff = rel_diff;
    if (dts_diff > ZMS_STAMP_SYNC_DRIFT_MS) {
        dts_diff = 0;
    } else if (dts_diff < -ZMS_STAMP_SYNC_DRIFT_MS) {
        dts_diff = 0;
    }

    int64_t target = m->relative_ms + dts_diff;
    if (target > s->relative_ms) {
        s->relative_ms = target;
        s->last_stamp = s->last_dts_in;
    } else if (target < m->relative_ms) {
        m->relative_ms = target;
        m->last_stamp = m->last_dts_in;
    }

    if (s->need_sync > 0) {
        s->need_sync--;
    }
    if (m->need_sync > 0) {
        m->need_sync--;
    }
}

uint32_t zms_track_stamp_revise(zms_track_stamp *s, uint64_t raw_ms)
{
    if (!s) {
        return (uint32_t)raw_ms;
    }
    if (!s->max_delta_ms) {
        s->max_delta_ms = ZMS_STAMP_MAX_DELTA_MS;
    }

    int64_t dts = (int64_t)raw_ms;
    if (s->last_dts_in != dts) {
        s->relative_ms += stamp_delta(s, dts);
        s->last_dts_in = dts;
    }

    stamp_sync_l(s);
    if (s->relative_ms < 0) {
        return 0;
    }
    return (uint32_t)s->relative_ms;
}

void zms_media_timeline_link_stamps(zms_media_timeline *tl)
{
    if (!tl || tl->stamp_linked) {
        return;
    }
    zms_track_stamp_sync_to(&tl->ast, &tl->vst, 1);
    tl->stamp_linked = 1;
}

void zms_media_timeline_set_av_clamp(zms_media_timeline *tl, int enabled)
{
    if (!tl) {
        return;
    }
    tl->av_clamp_disabled = enabled ? 0 : 1;
}

zms_time_base zms_time_base_ms(void)
{
    zms_time_base tb = {1, 1000};
    return tb;
}

zms_time_base zms_time_base_from_hz(uint32_t clock_hz)
{
    zms_time_base tb = {1, clock_hz > 0 ? clock_hz : ZMS_MPEGTS_CLOCK_HZ};
    return tb;
}

uint64_t zms_ts_rescale_q(uint64_t ts, zms_time_base from_tb, zms_time_base to_tb)
{
    if (!from_tb.den || !to_tb.den) {
        return ts;
    }
    return (ts * (uint64_t)to_tb.num * (uint64_t)from_tb.den) /
           ((uint64_t)to_tb.den * (uint64_t)from_tb.num);
}

uint32_t zms_rtp_clock_to_ms(uint32_t rtp_ts, uint32_t clock_hz)
{
    uint32_t hz = clock_hz > 0 ? clock_hz : ZMS_MPEGTS_CLOCK_HZ;
    return (uint32_t)((uint64_t)rtp_ts * 1000u / (uint64_t)hz);
}

uint32_t zms_ms_to_rtp_clock(uint32_t ms, uint32_t clock_hz)
{
    uint32_t hz = clock_hz > 0 ? clock_hz : ZMS_MPEGTS_CLOCK_HZ;
    return (uint32_t)((uint64_t)ms * (uint64_t)hz / 1000u);
}

uint64_t zms_ms_to_mpegts_90k(uint32_t ms)
{
    return (uint64_t)ms * ZMS_MPEGTS_CLOCK_HZ / 1000u;
}

uint32_t zms_mpegts_90k_to_ms(int64_t ticks_90k)
{
    if (ticks_90k < 0) {
        return 0;
    }
    return (uint32_t)(ticks_90k / 90);
}

uint64_t zms_wall_ms(void)
{
    return ztk_wall_ms();
}

void zms_wall_ms_to_ntp(uint64_t wall_ms, uint32_t *ntp_sec, uint32_t *ntp_frac)
{
    if (ntp_sec) {
        *ntp_sec = (uint32_t)(wall_ms / 1000u + ZMS_NTP_UNIX_OFFSET);
    }
    if (ntp_frac) {
        uint32_t ms = (uint32_t)(wall_ms % 1000u);
        *ntp_frac = (uint32_t)((uint64_t)ms * 0x100000000ULL / 1000u);
    }
}

uint32_t zms_codec_frame_duration_ms(zms_codec_id codec, uint32_t sample_rate_hz)
{
    switch (codec) {
    case ZMS_CODEC_AAC: {
        uint32_t rate = sample_rate_hz > 0 ? sample_rate_hz : 44100;
        return (1024u * 1000u + rate / 2) / rate;
    }
    case ZMS_CODEC_H264:
        return 40u;
    default:
        return ZMS_MEDIA_TS_MIN_STEP_MS;
    }
}

void zms_stream_clock_reset(zms_stream_clock *c)
{
    if (!c) {
        return;
    }
    memset(c, 0, sizeof(*c));
}

void zms_stream_clock_init(zms_stream_clock *c, zms_track_type track, zms_codec_id codec,
                           uint32_t sample_rate_hz)
{
    if (!c) {
        return;
    }
    c->track = track;
    c->frame_dur_ms = zms_codec_frame_duration_ms(codec, sample_rate_hz);
    if (track == ZMS_TRACK_VIDEO && c->frame_dur_ms < 40) {
        c->frame_dur_ms = 40;
    }
    c->learned_delta_ms = (track == ZMS_TRACK_VIDEO) ? 133u : c->frame_dur_ms;
}

static uint32_t stream_clock_step(const zms_stream_clock *c)
{
    if (c->track == ZMS_TRACK_VIDEO && c->learned_delta_ms >= 16 && c->learned_delta_ms <= 500) {
        return c->learned_delta_ms;
    }
    return c->frame_dur_ms > 0 ? c->frame_dur_ms : ZMS_MEDIA_TS_MIN_STEP_MS;
}

uint32_t zms_stream_clock_advance(zms_stream_clock *c, uint64_t raw_ms)
{
    if (!c) {
        return (uint32_t)raw_ms;
    }

    if (!c->started) {
        c->started = 1;
        c->last_raw_ms = raw_ms;
        c->last_pts_ms = 0;
        return 0;
    }

    if (c->last_pts_ms > ZMS_MEDIA_TS_REWIND_THRESHOLD_MS &&
        raw_ms + ZMS_MEDIA_TS_REWIND_THRESHOLD_MS < c->last_raw_ms) {
        c->last_pts_ms += stream_clock_step(c);
        c->last_raw_ms = raw_ms;
        return c->last_pts_ms;
    }

    if (raw_ms > c->last_raw_ms) {
        uint32_t rel = (uint32_t)(raw_ms - c->last_raw_ms);
        if (c->track == ZMS_TRACK_VIDEO && rel >= 16 && rel <= 500) {
            c->learned_delta_ms = rel;
        }
        c->last_pts_ms += rel;
    } else if (raw_ms == c->last_raw_ms) {
        return c->last_pts_ms;
    } else {
        c->last_pts_ms += stream_clock_step(c);
    }

    c->last_raw_ms = raw_ms;
    return c->last_pts_ms;
}

void zms_media_timeline_reset(zms_media_timeline *tl)
{
    if (!tl) {
        return;
    }
    memset(tl, 0, sizeof(*tl));
    tl->video_clock_hz = ZMS_MPEGTS_CLOCK_HZ;
    tl->audio_clock_hz = 44100;
    tl->vst.max_delta_ms = ZMS_STAMP_MAX_DELTA_MS;
    tl->ast.max_delta_ms = ZMS_STAMP_MAX_DELTA_MS;
    zms_stream_clock_init(&tl->video, ZMS_TRACK_VIDEO, ZMS_CODEC_H264, 0);
    zms_stream_clock_init(&tl->audio, ZMS_TRACK_AUDIO, ZMS_CODEC_AAC, tl->audio_clock_hz);
}

void zms_media_timeline_set_video(zms_media_timeline *tl, zms_codec_id codec, uint32_t clock_hz)
{
    if (!tl) {
        return;
    }
    tl->video_clock_hz = clock_hz > 0 ? clock_hz : ZMS_MPEGTS_CLOCK_HZ;
    zms_stream_clock_init(&tl->video, ZMS_TRACK_VIDEO, codec, 0);
}

void zms_media_timeline_set_audio(zms_media_timeline *tl, zms_codec_id codec, uint32_t clock_hz)
{
    if (!tl) {
        return;
    }
    tl->audio_codec = codec;
    tl->audio_clock_hz = clock_hz > 0 ? clock_hz : 44100;
    zms_stream_clock_init(&tl->audio, ZMS_TRACK_AUDIO, codec, tl->audio_clock_hz);
    zms_media_timeline_link_stamps(tl);
}

static void stamp_clamp_av(zms_media_timeline *tl)
{
    if (!tl || tl->av_clamp_disabled || !tl->stamp_linked || !tl->ast.last_dts_in ||
        !tl->vst.last_dts_in) {
        return;
    }
    int64_t diff = tl->vst.relative_ms - tl->ast.relative_ms;
    if (diff > ZMS_STAMP_SYNC_DRIFT_MS) {
        tl->vst.relative_ms = tl->ast.relative_ms;
        tl->vst.last_stamp = tl->vst.last_dts_in;
    } else if (diff < -ZMS_STAMP_SYNC_DRIFT_MS) {
        tl->ast.relative_ms = tl->vst.relative_ms;
        tl->ast.last_stamp = tl->ast.last_dts_in;
    }
}

uint32_t zms_media_timeline_video(zms_media_timeline *tl, uint64_t raw_ms)
{
    if (!tl) {
        return (uint32_t)raw_ms;
    }
    zms_media_timeline_link_stamps(tl);
    (void)zms_track_stamp_revise(&tl->vst, raw_ms);
    stamp_clamp_av(tl);
    return (uint32_t)tl->vst.relative_ms;
}

uint32_t zms_media_timeline_audio(zms_media_timeline *tl, uint64_t raw_ms)
{
    if (!tl) {
        return (uint32_t)raw_ms;
    }
    zms_media_timeline_link_stamps(tl);
    (void)zms_track_stamp_revise(&tl->ast, raw_ms);
    stamp_clamp_av(tl);
    return (uint32_t)tl->ast.relative_ms;
}

void zms_mux_timeline_reset(zms_mux_timeline *m)
{
    if (!m) {
        return;
    }
    memset(m, 0, sizeof(*m));
}

uint32_t zms_mux_timeline_pts(zms_mux_timeline *m, uint32_t ring_pts_ms)
{
    if (!m) {
        return ring_pts_ms;
    }

    if (!m->origin_set) {
        m->origin_ms = ring_pts_ms;
        m->origin_set = 1;
        m->last_out_ms = 0;
        return 0;
    }

    uint32_t out = ring_pts_ms >= m->origin_ms ? ring_pts_ms - m->origin_ms : 0;
    if (out <= m->last_out_ms) {
        out = m->last_out_ms + 1u;
    }
    m->last_out_ms = out;
    return out;
}

void zms_mux_av_timeline_reset(zms_mux_av_timeline *m)
{
    if (!m) {
        return;
    }
    memset(m, 0, sizeof(*m));
}

void zms_mux_av_timeline_lock_origin(zms_mux_av_timeline *m, uint32_t origin_ms)
{
    if (!m) {
        return;
    }
    m->shared_origin_ms = origin_ms;
    m->use_shared_origin = 1;
    m->origin_ms[0] = m->origin_ms[1] = origin_ms;
    m->origin_set[0] = m->origin_set[1] = 1;
    m->last_out_ms[0] = m->last_out_ms[1] = 0;
}

void zms_mux_av_timeline_shift_origin(zms_mux_av_timeline *m, uint32_t origin_ms)
{
    if (!m) {
        return;
    }
    m->shared_origin_ms = origin_ms;
    m->use_shared_origin = 1;
    m->origin_ms[0] = m->origin_ms[1] = origin_ms;
    m->origin_set[0] = m->origin_set[1] = 1;
}

uint32_t zms_mux_av_timeline_pts(zms_mux_av_timeline *m, zms_track_type track, uint32_t ring_pts_ms)
{
    unsigned idx = (track == ZMS_TRACK_AUDIO) ? 1u : 0u;
    if (!m) {
        return ring_pts_ms;
    }

    if (m->use_shared_origin) {
        uint32_t rel = ring_pts_ms >= m->shared_origin_ms ? ring_pts_ms - m->shared_origin_ms : 0;
        /* 首帧样本允许 t=0；输出开始后才强制单调。 */
        if (m->last_out_ms[idx] > 0 && rel <= m->last_out_ms[idx]) {
            rel = m->last_out_ms[idx] + 1u;
        }
        m->last_out_ms[idx] = rel;
        return rel;
    }

    if (!m->origin_set[idx]) {
        m->origin_ms[idx] = ring_pts_ms;
        m->origin_set[idx] = 1;
    }

    uint32_t rel = ring_pts_ms >= m->origin_ms[idx] ? ring_pts_ms - m->origin_ms[idx] : 0;
    /* 仅按 track 单调（VLC FLV：A/V tag 可共享文件顺序，非单一全局 DTS）。 */
    if (rel <= m->last_out_ms[idx]) {
        rel = m->last_out_ms[idx] + 1u;
    }
    m->last_out_ms[idx] = rel;
    return rel;
}
