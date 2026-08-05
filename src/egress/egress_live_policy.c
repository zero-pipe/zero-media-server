/**
 * @file egress_live_policy.c
 * @brief 直播出站 pacing / snap / catchup 策略。
 */
#include "zms/egress/egress_live_policy.h"
#include "zms/engine/media_clock.h"
#include "zms/engine/gop/gop_queue.h"
#include "zms/egress/egress_clock.h"
#include "zms/egress/rtp/rtp_muxer.h"
#include "ztk/util/log.h"

int zms_egress_live_pump_budget(int epoch_on, int catchup_done, size_t lag, int caller_budget)
{
    int budget = caller_budget > 0 ? caller_budget : (int)ZMS_EGRESS_FRAME_BUDGET_LIVE;

    if (zms_egress_live_lag_catchup(lag) || !epoch_on || !catchup_done) {
        budget = (int)ZMS_EGRESS_BURST_BUDGET;
    }
    if (epoch_on && catchup_done && budget > (int)ZMS_EGRESS_FRAME_BUDGET_LIVE) {
        budget = (int)ZMS_EGRESS_FRAME_BUDGET_LIVE;
    }
    return budget;
}

int zms_egress_live_peek_paced(zms_gop_reader *rd, zms_gop_slot *peek, const zms_egress_clock *clk,
                               int pace, int read_timeout_ms)
{
    if (!rd || !peek) {
        return -1;
    }
    if (zms_gop_reader_peek_muxed(rd, peek, read_timeout_ms) <= 0) {
        return -1;
    }
    if (pace && clk && zms_egress_clock_epoch_locked(clk) &&
        !zms_egress_clock_media_due(clk, peek->dts_ms, ZMS_EGRESS_PACE_LEAD_MS)) {
        return 0;
    }
    return 1;
}

void zms_egress_live_snap_gop(zms_gop_reader *rd, struct zms_rtp_muxer *mux)
{
    uint32_t anchor = 0;
    zms_gop_slot at;

    if (rd) {
        zms_gop_reader_seek_live(rd);
        if (zms_gop_reader_slot_at_read(rd, &at) && at.track == ZMS_TRACK_VIDEO) {
            anchor = at.dts_ms;
        }
    }
    if (mux) {
        zms_egress_clock *clk = zms_rtp_muxer_play_clock_mut(mux);
        if (clk && zms_egress_clock_epoch_locked(clk)) {
            zms_egress_clock_sync_wall(clk, anchor);
        } else {
            zms_rtp_muxer_jump_live(mux);
        }
    }
}

int zms_egress_live_should_pump(zms_gop_reader *rd, int woke, int catchup_done, int epoch_on)
{
    size_t lag;

    if (!rd) {
        return 0;
    }
    if (woke) {
        return 1;
    }
    if (!catchup_done || !epoch_on) {
        return 1;
    }
    lag = zms_gop_reader_lag(rd);
    return lag > 0;
}

static const zms_egress_clock *live_play_clock(zms_rtp_muxer *mux,
                                               const zms_egress_live_state *live)
{
    if (mux) {
        return zms_rtp_muxer_play_clock(mux);
    }
    if (live && live->play_clk) {
        return live->play_clk;
    }
    return NULL;
}

void zms_egress_live_seek_rtsp_attach(zms_gop_reader *rd)
{
    if (rd) {
        zms_gop_reader_seek_gop_key(rd);
    }
}

static int live_play_maybe_kick_slow_consumer(zms_gop_reader *rd, const zms_egress_live_state *live,
                                              int catchup_done, int epoch_on, size_t lag,
                                              unsigned session_no)
{
    unsigned kick_lag;
    void *user;

    kick_lag = ZMS_EGRESS_SLOW_CONSUMER_KICK_LAG;
    if (kick_lag == 0 || !catchup_done || !epoch_on || lag <= kick_lag) {
        return 0;
    }
    if (!live || !live->on_slow_consumer) {
        return 0;
    }
    user = live->slow_consumer_user ? live->slow_consumer_user : live->snap_user;
    if (session_no) {
        ztk_warn("[live_play] reader_kick lag=%zu kick_lag=%u session=%u", lag, kick_lag,
                 session_no);
    }
    live->on_slow_consumer(user);
    return 1;
}

int zms_egress_live_rtsp_prep(zms_gop_reader *rd, zms_rtp_muxer *mux,
                              const zms_egress_live_state *live, unsigned session_no,
                              int *pump_budget_io)
{
    size_t lag;
    const zms_egress_clock *clk;
    int catchup_done;
    int epoch_on;

    if (!rd) {
        return 0;
    }

    lag = zms_gop_reader_lag(rd);
    clk = live_play_clock(mux, live);
    catchup_done = live && live->live_catchup_done && *live->live_catchup_done;
    epoch_on = clk && zms_egress_clock_epoch_locked(clk);

    /* 首屏 catchup 期间跳过 snap：on_snap 会清 armed/config 并在 IDR 卡住。 */
    if (lag > ZMS_EGRESS_RING_MAX_LAG && (catchup_done || epoch_on)) {
        zms_egress_live_snap_gop(rd, mux);
        if (live && live->on_snap) {
            live->on_snap(live->snap_user);
        } else if (session_no) {
            ztk_debug("[live_play] snap_gop lag=%zu session=%u", zms_gop_reader_lag(rd),
                      session_no);
        }
        lag = zms_gop_reader_lag(rd);
        clk = live_play_clock(mux, live);
        catchup_done = live && live->live_catchup_done && *live->live_catchup_done;
        epoch_on = clk && zms_egress_clock_epoch_locked(clk);
    }

    if (live && live->live_catchup_done) {
        if (!catchup_done && epoch_on && lag <= ZMS_EGRESS_CATCHUP_LAG) {
            *live->live_catchup_done = 1;
            catchup_done = 1;
        } else if (catchup_done && epoch_on &&
                   lag > ZMS_EGRESS_RING_MAX_LAG + ZMS_EGRESS_RESYNC_LAG) {
            uint64_t now = zms_wall_ms();
            uint64_t last = live->live_resync_at_ms ? *live->live_resync_at_ms : 0;

            if (now - last >= ZMS_EGRESS_RESYNC_COOLDOWN_MS) {
                zms_egress_live_snap_gop(rd, mux);
                if (live && live->on_snap) {
                    live->on_snap(live->snap_user);
                }
                if (live->live_resync_at_ms) {
                    *live->live_resync_at_ms = now;
                }
                epoch_on = clk && zms_egress_clock_epoch_locked(clk);
                lag = zms_gop_reader_lag(rd);
                if (session_no) {
                    ztk_warn("[live_play] lag_resync lag=%zu session=%u", lag, session_no);
                }
            }
        } else if (!epoch_on && lag > ZMS_EGRESS_RING_MAX_LAG) {
            zms_gop_reader_seek_live(rd);
            lag = zms_gop_reader_lag(rd);
        }
    } else if (lag > ZMS_EGRESS_RING_MAX_LAG) {
        zms_gop_reader_seek_live(rd);
    }

    if (pump_budget_io) {
        *pump_budget_io = zms_egress_live_pump_budget(epoch_on, catchup_done, lag, *pump_budget_io);
    }
    if (pump_budget_io && live && live->pump_budget_cap > 0 &&
        *pump_budget_io > live->pump_budget_cap) {
        *pump_budget_io = live->pump_budget_cap;
    }

    return live_play_maybe_kick_slow_consumer(rd, live, catchup_done, epoch_on, lag, session_no);
}

int zms_egress_live_flv_prep(zms_gop_reader *rd, const zms_egress_live_state *live,
                             unsigned session_no, int *pump_budget_io)
{
    return zms_egress_live_rtsp_prep(rd, NULL, live, session_no, pump_budget_io);
}
