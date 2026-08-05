#ifndef ZMS_ENGINE_FRAME_H
#define ZMS_ENGINE_FRAME_H

/**
 * @file frame.h
 * @brief 与协议无关的媒体载荷（ES 单元）
 *
 * @ref zms_frame 携带归一化后的 codec 字节、入站时间戳与 GOP 元数据
 *
 * Copyright (c) zero-media-server
 */
#include "zms/media/codec/codec_id.h"
#include "zms/zms_export.h"
#include "ztk/ztk_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zms_gop_slot zms_gop_slot;

/** ingress 写入并在 gop_queue 中流转的内存 ES 单元 */
typedef struct zms_frame {
    uint8_t *data;
    size_t size;
    size_t capacity;
    uint64_t dts_ms;
    uint64_t pts_ms;
    zms_codec_id codec;
    zms_track_type track;
    int keyframe;
    /** SPS/PPS/VPS/AAC AudioSpecificConfig 等参数集。 */
    int config_frame;
    /** SEI/AUD 等可丢弃单元；不推进 GOP 状态。 */
    int drop_able;
    int owned;
} zms_frame;

ZMS_API void zms_frame_init(zms_frame *f);
ZMS_API void zms_frame_clear(zms_frame *f);
ZMS_API ztk_err_t zms_frame_reserve(zms_frame *f, size_t cap);
ZMS_API ztk_err_t zms_frame_assign(zms_frame *f, const void *data, size_t len, int copy);

/** es_codec 注册表从 ES 内容推导 keyframe 标志（单一 hub，避免各协议重复判定） */
ZMS_API void zms_frame_refresh_key_from_es(zms_frame *f);

/** @return 非零表示视频 config keyframe（GOP 边界） */
ZMS_API int zms_frame_video_gop_marker(const zms_frame *f);

/** @return 非零表示 caching 已开始后该帧可写入 gop_queue */
ZMS_API int zms_gop_queue_storable(const zms_frame *f, int cache_started, int has_video);

/** @return 非零表示该帧在 ring 中开启新 GOP */
ZMS_API int zms_gop_queue_new_gop(const zms_frame *f, int video_key_pos, int has_video);

/** @return 非零表示 es_codec 注册表判定为 egress sync 点 */
ZMS_API int zms_frame_is_egress_sync(const zms_frame *f);

/**
 * @return 非零表示解码器可仅凭此帧起播（如 H.264/H.265 IDR）
 *
 * 参数集可通过 insertConfig 单独发送；起播判定不要求同包携带
 */
ZMS_API int zms_frame_is_decode_start(const zms_frame *f);

/** @return 与 @ref zms_frame_is_decode_start 相同（首个可解码 VCL） */
ZMS_API int zms_frame_is_play_start(const zms_frame *f);

/**
 * 将入站 @a dts_ms / @a pts_ms 归一化为 ring slot 时间轴字段
 * @a pts_ms 与 DTS 相同时为 0（见 @ref zms_gop_slot.dts_ms）
 */
ZMS_API void zms_gop_queue_timeline_from_frame(const zms_frame *f, uint32_t *dts_ms,
                                               uint32_t *pts_ms);

/** @return ring slot DTS（ms），用于交织 / pacing */
ZMS_API uint32_t zms_gop_slot_dts_ms(const zms_gop_slot *slot);

/** @return ring slot PTS（ms），用于 mux tag；无 PTS 时回退 DTS */
ZMS_API uint32_t zms_gop_slot_pts_ms(const zms_gop_slot *slot);

/** @return 非零表示 slot 为 egress sync 点（egress 重封装起播） */
ZMS_API int zms_gop_slot_is_egress_sync(const zms_gop_slot *slot);

/** 若该 slot 为 egress sync 点，则刷新其 keyframe 标志 */
ZMS_API void zms_gop_slot_refresh_sync_key(zms_gop_slot *slot);

/** @return 非零表示 slot 为解码起播点（H.264/H.265 IDR 等） */
ZMS_API int zms_gop_slot_is_decode_start(const zms_gop_slot *slot);

/** 若该 slot 为解码起播点，则刷新 keyframe 标志 */
ZMS_API void zms_gop_slot_refresh_decode_key(zms_gop_slot *slot);

/** @return 非零表示 slot 为播放起播点（首个可解码 VCL） */
ZMS_API int zms_gop_slot_is_play_start(const zms_gop_slot *slot);

/** 若该 slot 为播放起播点，则刷新 keyframe 标志 */
ZMS_API void zms_gop_slot_refresh_play_key(zms_gop_slot *slot);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_ENGINE_FRAME_H */
