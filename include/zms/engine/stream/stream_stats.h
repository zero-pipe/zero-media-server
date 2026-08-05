#ifndef ZMS_ENGINE_STREAM_STATS_H
#define ZMS_ENGINE_STREAM_STATS_H

/**
 * @file stream_stats.h
 * @brief 供 HTTP API 使用的每路流流量计数与质量指标。
 *
 * Copyright (c) zero-media-server
 */
#include "zms/zms_export.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct zms_media_source;
struct zms_gop_queue;

/** 存储于 @ref zms_media_source 的滚动计数器。 */
typedef struct zms_media_stream_stats {
    uint64_t ingress_bytes;
    uint64_t egress_bytes;
    uint64_t ingress_speed_bps;
    uint64_t egress_speed_bps;
    uint64_t speed_tick_ms;
    uint64_t ingress_at_tick;
    uint64_t egress_at_tick;

    /* 帧率统计（1 秒滑动窗口） */
    uint64_t frame_count_v; /**< 累计入站视频帧数 */
    uint64_t frame_count_a; /**< 累计入站音频帧数 */
    uint64_t fps_tick_ms;   /**< 上次 fps 窗口起始的单调时间戳 */
    uint64_t fps_v_at_tick; /**< fps_tick_ms 时刻的 frame_count_v 快照 */
    uint64_t fps_a_at_tick; /**< fps_tick_ms 时刻的 frame_count_a 快照 */
    uint32_t video_fps;     /**< 估算视频帧率（帧/秒） */
    uint32_t audio_fps;     /**< 估算音频帧率（帧/秒） */

    /* 质量 / 错误计数 */
    uint64_t dropped_frames; /**< 丢弃帧数（GOP 满、OOM、编解码不支持） */
} zms_media_stream_stats;

typedef struct zms_media_stats_view {
    uint64_t ingress_bytes;
    uint64_t egress_bytes;
    int64_t bytes_speed;
    int64_t egress_speed;
    size_t gop_queue_pending;
    size_t gop_queue_max_lag;
    size_t gop_queue_gop_count;
    int gop_queue_readers;
    uint32_t video_fps;
    uint32_t audio_fps;
    uint64_t dropped_frames;
} zms_media_stats_view;

ZMS_API void zms_media_stats_on_ingress(struct zms_media_source *s, size_t nbytes);
ZMS_API void zms_media_stats_on_egress(struct zms_media_source *s, size_t nbytes);
/** 计入一帧入站帧（视频或音频）；更新 fps 计数器。 */
ZMS_API void zms_media_stats_on_frame(struct zms_media_source *s, int is_video);
/** 计入一帧丢弃帧（GOP 满 / OOM / 不支持的编解码）。 */
ZMS_API void zms_media_stats_on_drop(struct zms_media_source *s);
ZMS_API void zms_media_stats_reset(struct zms_media_source *s);
ZMS_API void zms_media_stats_fill(const struct zms_media_source *s, struct zms_gop_queue *ring,
                                  zms_media_stats_view *out);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_ENGINE_STREAM_STATS_H */
