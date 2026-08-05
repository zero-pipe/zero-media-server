/**
 * @file egress_pacing.c
 * @brief 播放 pacing 运行时阈值（config.ini / zms_egress_pacing_init）。
 */
#include "zms/egress/egress_pacing.h"
#include "zms/engine/gop/gop_limits.h"
#include <string.h>

static zms_egress_pacing g_pacing;
static int g_egress_pacing_initialized;

static void play_pacing_clamp(zms_egress_pacing *p)
{
    if (!p) {
        return;
    }
    if (p->ring_max_lag < 32u) {
        p->ring_max_lag = 32u;
    }
    if (p->catchup_lag < 1u) {
        p->catchup_lag = 1u;
    }
    if (p->frame_budget_live < 1u) {
        p->frame_budget_live = 1u;
    }
    if (p->frame_budget_catchup < p->frame_budget_live) {
        p->frame_budget_catchup = p->frame_budget_live;
    }
    if (p->burst_budget < p->frame_budget_live) {
        p->burst_budget = p->frame_budget_catchup;
    }
    if (p->flv_budget_live < 1u) {
        p->flv_budget_live = 1u;
    }
    if (p->flv_budget_vod < 1u) {
        p->flv_budget_vod = 1u;
    }
    if (p->resync_lag < 1u) {
        p->resync_lag = 1u;
    }
    if (p->resync_lag_max < p->ring_max_lag) {
        p->resync_lag_max = p->ring_max_lag * 4u;
    }
    if (p->resync_cooldown_ms < 500u) {
        p->resync_cooldown_ms = 500u;
    }
    if (p->pace_lead_ms < 50u) {
        p->pace_lead_ms = 50u;
    }
}

void zms_egress_pacing_defaults(zms_egress_pacing *p)
{
    if (!p) {
        return;
    }
    memset(p, 0, sizeof(*p));
    p->ring_max_lag = ZMS_GOP_QUEUE_PLAY_MAX_LAG;
    p->catchup_lag = 16u;
    p->frame_budget_live = 8u;
    p->frame_budget_catchup = 96u;
    p->burst_budget = 96u;
    p->flv_budget_live = 48u;
    p->flv_budget_vod = 8u;
    p->vod_catchup_start = 96u;
    p->vod_catchup_seek = 32u;
    p->vod_prefill_lag = 32u;
    p->resync_lag = 16u;
    p->resync_lag_max = ZMS_GOP_QUEUE_PLAY_MAX_LAG * 4u;
    p->resync_cooldown_ms = 3000u;
    p->pace_lead_ms = 350u;
    p->slow_consumer_kick_lag = p->resync_lag_max;
}

void zms_egress_pacing_init(const zms_egress_pacing *cfg)
{
    if (!cfg) {
        zms_egress_pacing_defaults(&g_pacing);
    } else {
        g_pacing = *cfg;
        if (g_pacing.burst_budget == 0) {
            g_pacing.burst_budget = g_pacing.frame_budget_catchup;
        }
        if (g_pacing.resync_lag_max == 0) {
            g_pacing.resync_lag_max = g_pacing.ring_max_lag * 4u;
        }
    }
    play_pacing_clamp(&g_pacing);
    g_egress_pacing_initialized = 1;
}

void zms_egress_pacing_fini(void)
{
    g_egress_pacing_initialized = 0;
    memset(&g_pacing, 0, sizeof(g_pacing));
}

const zms_egress_pacing *zms_egress_pacing_get(void)
{
    if (!g_egress_pacing_initialized) {
        zms_egress_pacing_init(NULL);
    }
    return &g_pacing;
}
