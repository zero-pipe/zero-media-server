#ifndef ZMS_EGRESS_PACING_H
#define ZMS_EGRESS_PACING_H

/**
 * @file egress_pacing.h
 * @brief 进程级出站 pacing 配置。
 *
 * 进程启动时经 @ref zms_egress_pacing_init() 初始化一次；所有读者经宏访问 const 指针（零开销内联）。
 *
 * Copyright (c) zero-media-server
 */
#include "zms/zms_export.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zms_egress_pacing {
    uint32_t ring_max_lag;
    uint32_t catchup_lag;
    int frame_budget_live;
    int frame_budget_catchup;
    int burst_budget;
    int flv_budget_live;
    int flv_budget_vod;
    int vod_catchup_start;
    int vod_catchup_seek;
    uint32_t vod_prefill_lag;
    uint32_t resync_lag;
    uint32_t resync_lag_max;
    uint32_t resync_cooldown_ms;
    uint32_t pace_lead_ms;
    uint32_t slow_consumer_kick_lag;
} zms_egress_pacing;

ZMS_API void zms_egress_pacing_defaults(zms_egress_pacing *p);
ZMS_API void zms_egress_pacing_init(const zms_egress_pacing *cfg);
ZMS_API void zms_egress_pacing_fini(void);
ZMS_API const zms_egress_pacing *zms_egress_pacing_get(void);

#define ZMS_EGRESS_RING_MAX_LAG (zms_egress_pacing_get()->ring_max_lag)
#define ZMS_EGRESS_CATCHUP_LAG (zms_egress_pacing_get()->catchup_lag)
#define ZMS_EGRESS_FRAME_BUDGET_LIVE (zms_egress_pacing_get()->frame_budget_live)
#define ZMS_EGRESS_FRAME_BUDGET_CATCHUP (zms_egress_pacing_get()->frame_budget_catchup)
#define ZMS_EGRESS_BURST_BUDGET (zms_egress_pacing_get()->burst_budget)
#define ZMS_EGRESS_FLV_BUDGET_LIVE (zms_egress_pacing_get()->flv_budget_live)
#define ZMS_EGRESS_FLV_BUDGET_VOD (zms_egress_pacing_get()->flv_budget_vod)
#define ZMS_EGRESS_VOD_CATCHUP_START (zms_egress_pacing_get()->vod_catchup_start)
#define ZMS_EGRESS_VOD_CATCHUP_SEEK (zms_egress_pacing_get()->vod_catchup_seek)
#define ZMS_EGRESS_VOD_PREFILL_LAG (zms_egress_pacing_get()->vod_prefill_lag)
#define ZMS_EGRESS_RESYNC_LAG (zms_egress_pacing_get()->resync_lag)
#define ZMS_EGRESS_RESYNC_LAG_MAX (zms_egress_pacing_get()->resync_lag_max)
#define ZMS_EGRESS_RESYNC_COOLDOWN_MS (zms_egress_pacing_get()->resync_cooldown_ms)
#define ZMS_EGRESS_PACE_LEAD_MS (zms_egress_pacing_get()->pace_lead_ms)
#define ZMS_EGRESS_SLOW_CONSUMER_KICK_LAG (zms_egress_pacing_get()->slow_consumer_kick_lag)

#ifdef __cplusplus
}
#endif

#endif /* ZMS_EGRESS_PACING_H */
