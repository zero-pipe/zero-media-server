#ifndef ZMS_EGRESS_SIDECAR_PARAM_SETS_H
#define ZMS_EGRESS_SIDECAR_PARAM_SETS_H

/**
 * @file egress_sidecar_param_sets.h
 * @brief HLS/DASH sidecar 写入器用的共享 VPS/SPS/PPS 缓存。
 */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zms_sidecar_param_sets {
    uint8_t *vps;
    uint8_t *sps;
    uint8_t *pps;
    size_t vps_len;
    size_t sps_len;
    size_t pps_len;
} zms_sidecar_param_sets;

void zms_sidecar_param_sets_init(zms_sidecar_param_sets *ps);
void zms_sidecar_param_sets_clear(zms_sidecar_param_sets *ps);

/** 缓存 RTMP/FLV 视频 sequence header 中的原始参数集（H.264/H.265）。 */
int zms_sidecar_cache_rtmp_video_cfg(zms_sidecar_param_sets *ps, const uint8_t *cfg, size_t clen);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_EGRESS_SIDECAR_PARAM_SETS_H */
