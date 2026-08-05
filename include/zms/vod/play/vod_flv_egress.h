#ifndef ZMS_VOD_EGRESS_FLV_EGRESS_H
#define ZMS_VOD_EGRESS_FLV_EGRESS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct zms_vod_buffer_reader;
struct zms_egress_clock;

/** egress_pipeline 与 vod/flv_muxer 共用的 VOD fifo → FLV tag 出站绑定。 */
typedef struct zms_flv_vod_egress_bind {
    struct zms_vod_buffer_reader *vod_rd;
    struct zms_egress_clock *play_clk;
    int *catchup_left;
    uint8_t **es_buf;
    size_t *es_cap;
    /** 1 = epoch 锁定后按 play_clock pacing（HTTP-FLV 实时 / RTMP VOD）。 */
    int pace_when_locked;
} zms_flv_vod_egress_bind;

#ifdef __cplusplus
}
#endif

#endif /* ZMS_VOD_EGRESS_FLV_EGRESS_H */
