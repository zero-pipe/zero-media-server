#ifndef ZMS_SRC_CONTAINER_FLV_FLV_FILE_MUXER_H
#define ZMS_SRC_CONTAINER_FLV_FLV_FILE_MUXER_H

#include "zms/media/codec/codec_id.h"
#include "zms/util/buf_pool.h"
#include <stddef.h>
#include <stdint.h>

#define ZMS_FLV_MUX_PENDING_MAX 8
#define ZMS_FLV_MUX_PENDING_BODY_MAX 2048u

typedef struct zms_flv_mux_pending {
    int pending_cnt;
    int pending_emit;
    uint8_t pending_type[ZMS_FLV_MUX_PENDING_MAX];
    uint32_t pending_tag_dts_ms[ZMS_FLV_MUX_PENDING_MAX];
    /** 按槽懒分配（buf_pool），避免每 muxer 嵌 16K */
    uint8_t *pending_body[ZMS_FLV_MUX_PENDING_MAX];
    size_t pending_body_cap[ZMS_FLV_MUX_PENDING_MAX];
    size_t pending_len[ZMS_FLV_MUX_PENDING_MAX];
    uint32_t audio_step_ms;
} zms_flv_mux_pending;

void zms_flv_mux_pending_clear(zms_flv_mux_pending *pend);

/**
 * 为 @a cfg 构建 FLV 视频 sequence-header（config）tag BODY，按 codec 变换
 *（H265/H266/AV1/VPx）或 H264 透传。HTTP-FLV tag 封装与 RTMP 播放路径共用
 *（二者发出 body 的方式不同）。
 *
 * @param fallback_vc 无法从 @a cfg 探测 codec 时的提示（RTMP 源 hint）；无则 ZMS_CODEC_INVALID
 * @param scratch     变换 codec 用的调用方缓冲
 * @param body,body_len 接收 body（H264 别名 @a cfg，否则 @a scratch）
 * @return 1 成功，0 codec 不适用/变换无输出，-1 参数无效
 */
int zms_flv_video_cfg_body(const uint8_t *cfg, size_t clen, zms_codec_id fallback_vc,
                           uint8_t *scratch, size_t scratch_cap, const uint8_t **body,
                           size_t *body_len);

int zms_flv_mux_write_video_cfg_tag(const uint8_t *cfg, size_t clen, uint8_t *out, size_t cap,
                                    size_t *out_len);

int zms_flv_mux_try_split_aac_tag(zms_flv_mux_pending *pend, const uint8_t *tag, size_t tag_len,
                                  uint32_t pkt_tag_dts_ms);

static inline uint8_t *zms_flv_mux_tag_buf(uint8_t **heap, size_t *heap_cap, size_t need,
                                           uint8_t *stack, size_t stack_cap)
{
    if (need <= stack_cap) {
        return stack;
    }
    if (!heap || !zms_buf_pool_slot_resize(heap, heap_cap, need)) {
        return NULL;
    }
    return *heap;
}

static inline uint8_t *zms_flv_mux_tag_buf_poller(uint8_t **heap, size_t *heap_cap, size_t need,
                                                  uint8_t *stack, size_t stack_cap,
                                                  struct ztk_poller *pol)
{
    if (need <= stack_cap) {
        return stack;
    }
    if (!heap) {
        return NULL;
    }
    if (pol) {
        if (!zms_buf_pool_slot_resize_poller(heap, heap_cap, need, pol)) {
            return NULL;
        }
    } else if (!zms_buf_pool_slot_resize(heap, heap_cap, need)) {
        return NULL;
    }
    return *heap;
}

#endif /* ZMS_SRC_CONTAINER_FLV_FLV_FILE_MUXER_H */
