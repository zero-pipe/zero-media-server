#ifndef ZMS_ENGINE_GOP_GOP_LIMITS_H
#define ZMS_ENGINE_GOP_GOP_LIMITS_H

/**
 * @file gop_limits.h
 * @brief GOP 队列容量与读者策略常量。
 *
 * 常量与存储实现头文件解耦，便于 media/protocol 模块共享策略而不依赖 gop_queue.h。
 */

/** 每路直播源保留的 ring 槽位数。 */
#define ZMS_GOP_QUEUE_CAPACITY 512u

/** 单队列最多跟踪的 GOP 边界数。 */
#define ZMS_GOP_QUEUE_MAX_GOP 32u

/** 压力下丢弃旧 GOP 前的目标保留 GOP 数。
 *  运行时覆盖：[general] gop_target_gops / zms_gop_queue_set_default_target_gops。 */
#define ZMS_GOP_QUEUE_TARGET_GOPS 3u

/**
 * 默认时间窗缓存（毫秒）。0 表示禁用。
 * 运行时：[general] gop_cache_sec → 毫秒，经 zms_gop_queue_set_default_cache_ms。
 */
#define ZMS_GOP_QUEUE_DEFAULT_CACHE_MS 0u

/** 读者槽位初始容量；按需动态扩容。 */
#define ZMS_GOP_QUEUE_READERS_INIT_CAP 64u

/** 直播读者滞后超过此阈值时向直播边缘 snap。 */
#define ZMS_GOP_QUEUE_PLAY_MAX_LAG 240u

#endif /* ZMS_ENGINE_GOP_GOP_LIMITS_H */
