#include "zms/egress/egress_clock.h"
#include "zms/engine/media_clock.h"

#define ZMS_NTP_UNIX_OFFSET 2208988800ULL

static uint64_t ntp_to_wall_ms(uint32_t ntp_sec, uint32_t ntp_frac)
{
    uint64_t sec = ntp_sec >= ZMS_NTP_UNIX_OFFSET ? (uint64_t)ntp_sec - ZMS_NTP_UNIX_OFFSET : 0;
    return sec * 1000u + (uint64_t)ntp_frac * 1000u / 0x100000000ULL;
}

void zms_egress_clock_init(zms_egress_clock *c)
{
    if (!c) {
        return;
    }
    c->epoch_ms = 0;
    c->epoch_locked = 0;
    c->epoch_ntp_sec = 0;
    c->epoch_ntp_frac = 0;
    c->play_wall_ms = 0;
    c->abs_rtp_ts = 0;
    c->paused = 0;
    c->pause_wall_ms = 0;
    c->play_scale = 1.0;
}

void zms_egress_clock_reset(zms_egress_clock *c)
{
    zms_egress_clock_init(c);
}

void zms_egress_clock_arm(zms_egress_clock *c)
{
    if (!c) {
        return;
    }
    zms_wall_ms_to_ntp(zms_wall_ms(), &c->epoch_ntp_sec, &c->epoch_ntp_frac);
    c->epoch_locked = 0;
    c->epoch_ms = 0;
    c->play_wall_ms = 0;
}

void zms_egress_clock_unlock(zms_egress_clock *c)
{
    if (!c) {
        return;
    }
    c->epoch_locked = 0;
    c->epoch_ms = 0;
    c->play_wall_ms = 0;
}

void zms_egress_clock_rebase(zms_egress_clock *c, uint32_t anchor_ms)
{
    if (!c) {
        return;
    }
    c->epoch_ms = anchor_ms;
    c->epoch_locked = 1;
    c->play_wall_ms = zms_wall_ms();
}

void zms_egress_clock_sync_wall(zms_egress_clock *c, uint32_t dts_ms)
{
    uint32_t rel;
    uint64_t now;

    if (!c || !c->epoch_locked) {
        return;
    }
    rel = zms_egress_clock_rel_ms(c, dts_ms);
    now = zms_wall_ms();
    c->play_wall_ms = now >= (uint64_t)rel ? now - (uint64_t)rel : now;
}

int zms_egress_clock_lock_epoch(zms_egress_clock *c, uint32_t dts_ms)
{
    if (!c || c->epoch_locked) {
        return 0;
    }
    c->epoch_ms = dts_ms;
    c->epoch_locked = 1;
    c->play_wall_ms = zms_wall_ms();
    return 1;
}

void zms_egress_clock_set_scale(zms_egress_clock *c, double scale)
{
    if (!c) {
        return;
    }
    if (scale <= 0.0 || scale > 16.0) {
        scale = 1.0;
    }
    c->play_scale = scale;
}

void zms_egress_clock_pause(zms_egress_clock *c)
{
    if (!c || !c->epoch_locked || c->paused) {
        return;
    }
    c->paused = 1;
    c->pause_wall_ms = zms_wall_ms();
}

void zms_egress_clock_resume(zms_egress_clock *c)
{
    uint64_t now;

    if (!c || !c->paused) {
        return;
    }
    now = zms_wall_ms();
    if (now > c->pause_wall_ms && c->play_wall_ms) {
        c->play_wall_ms += now - c->pause_wall_ms;
    }
    c->paused = 0;
    c->pause_wall_ms = 0;
}

int zms_egress_clock_is_paused(const zms_egress_clock *c)
{
    return c && c->paused;
}

int zms_egress_clock_media_due(const zms_egress_clock *c, uint32_t dts_ms, uint32_t lead_ms)
{
    uint32_t rel;
    uint64_t now;
    uint64_t elapsed;
    double scale;

    if (!c || !c->epoch_locked) {
        return 1;
    }
    if (c->paused) {
        return 0;
    }
    rel = zms_egress_clock_rel_ms(c, dts_ms);
    now = zms_wall_ms();
    elapsed = now >= c->play_wall_ms ? now - c->play_wall_ms : 0;
    scale = c->play_scale > 0.0 ? c->play_scale : 1.0;
    if (scale != 1.0) {
        elapsed = (uint64_t)((double)elapsed * scale);
    }
    return rel <= elapsed + (uint64_t)lead_ms;
}

int zms_egress_clock_epoch_locked(const zms_egress_clock *c)
{
    return c && c->epoch_locked;
}

uint32_t zms_egress_clock_rel_ms(const zms_egress_clock *c, uint32_t dts_ms)
{
    if (!c || !c->epoch_locked || dts_ms < c->epoch_ms) {
        return 0;
    }
    return dts_ms - c->epoch_ms;
}

uint32_t zms_egress_clock_rtp_ts_rel(uint32_t rel_ms, uint32_t clock_hz)
{
    if (clock_hz == 0) {
        clock_hz = 90000u;
    }
    return zms_ms_to_rtp_clock(rel_ms, clock_hz);
}

uint32_t zms_egress_clock_rtp_ts(const zms_egress_clock *c, uint32_t dts_ms, uint32_t clock_hz)
{
    return zms_egress_clock_rtp_ts_rel(zms_egress_clock_rel_ms(c, dts_ms), clock_hz);
}

void zms_egress_clock_sr_ntp(const zms_egress_clock *c, uint32_t rtp_ts, uint32_t clock_hz,
                             uint32_t *ntp_sec, uint32_t *ntp_frac)
{
    uint32_t rel_ms;
    uint64_t arm_wall;

    if (!c) {
        zms_wall_ms_to_ntp(zms_wall_ms(), ntp_sec, ntp_frac);
        return;
    }
    if (clock_hz == 0) {
        clock_hz = 90000u;
    }
    rel_ms = zms_rtp_clock_to_ms(rtp_ts, clock_hz);
    if (c->abs_rtp_ts && c->epoch_locked) {
        rel_ms = rel_ms >= c->epoch_ms ? rel_ms - c->epoch_ms : 0;
    }
    if (c->play_wall_ms) {
        zms_wall_ms_to_ntp(c->play_wall_ms + rel_ms, ntp_sec, ntp_frac);
    } else {
        arm_wall = ntp_to_wall_ms(c->epoch_ntp_sec, c->epoch_ntp_frac);
        zms_wall_ms_to_ntp(arm_wall + rel_ms, ntp_sec, ntp_frac);
    }
}
