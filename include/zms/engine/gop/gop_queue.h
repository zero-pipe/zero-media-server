#ifndef ZMS_ENGINE_GOP_GOP_QUEUE_H
#define ZMS_ENGINE_GOP_GOP_QUEUE_H

/**
 * @file gop_queue.h
 * @brief 直播多协议播放共用的 GOP 环形队列。
 *
 * 每个 source 一条 ES 车道：Annex-B / raw AAC，供 RTSP、HLS、FLV 重封装。
 * 写入时经 es_codec 刷新 ES keyframe；读者独立附着，GOP 索引支持起播 seek。
 *
 * Copyright (c) zero-media-server
 */
#include "zms/engine/frame.h"
#include "zms/engine/gop/gop_limits.h"
#include "zms/zms_export.h"
#include "ztk/util/buf.h"
#include "ztk/ztk_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zms_gop_queue zms_gop_queue;
typedef struct zms_gop_reader zms_gop_reader;

/** 环形槽位的公开视图（仅 ES 载荷）。 */
typedef struct zms_gop_slot {
    zms_codec_id codec;
    zms_track_type track;
    /** 入站归一化后的解码时间戳（ms）；驱动 A/V 交织与 pacing。 */
    uint32_t dts_ms;
    /** 显示时间戳；0 表示与 @a dts_ms 相同。 */
    uint32_t pts_ms;
    int keyframe;
    int config_frame;
    int drop_able;
    uint8_t *data;
    size_t len;
} zms_gop_slot;

ZMS_API zms_gop_queue *zms_gop_queue_create(void);
ZMS_API void zms_gop_queue_destroy(zms_gop_queue *r);
ZMS_API void zms_gop_queue_clear(zms_gop_queue *r);

/**
 * @brief 设置新建队列的默认保留 GOP 数（进程级；create 时拷贝到实例）。
 * @param n 钳制到 [1, ZMS_GOP_QUEUE_MAX_GOP]。
 */
ZMS_API void zms_gop_queue_set_default_target_gops(unsigned n);

/** @brief 覆盖单队列保留 GOP 数（已存在的 source）。 */
ZMS_API void zms_gop_queue_set_target_gops(zms_gop_queue *r, unsigned n);

/**
 * @brief 设置新建队列的默认时间窗缓存（毫秒；0=仅按 GOP 数/容量裁剪）。
 */
ZMS_API void zms_gop_queue_set_default_cache_ms(unsigned ms);

/** @brief 覆盖单队列时间窗缓存（毫秒）。 */
ZMS_API void zms_gop_queue_set_cache_ms(zms_gop_queue *r, unsigned ms);

/** @return 最慢读者与写端之间未读槽位数。 */
ZMS_API size_t zms_gop_queue_pending_count(const zms_gop_queue *r);

/** @return 当前保留的 GOP 段数（空则为 0）。 */
ZMS_API size_t zms_gop_queue_gop_count(const zms_gop_queue *r);

/** @return 所有附着读者中的最大读滞后（无读者则为 0）。 */
ZMS_API size_t zms_gop_queue_max_reader_lag(const zms_gop_queue *r);

/**
 * @brief 为下一写槽准备可写 buf（优先复用 refcnt==1 的槽位缓冲，否则共享池 alloc）。
 * @note 填好后配合 @ref zms_gop_queue_write_buf；失败返回 NULL。
 */
ZMS_API ztk_buf *zms_gop_queue_alloc_write(zms_gop_queue *r, size_t size);

/**
 * @brief 向环形缓冲写入一帧归一化 ES。
 * @param frame H.264/H.265 Annex-B、raw AAC 等。
 * @note 单写者（入站线程）；读者使用独立游标。
 */
ZMS_API ztk_err_t zms_gop_queue_write(zms_gop_queue *r, const zms_frame *frame);

/**
 * @brief 接管 buf 写入一帧（所有权转入环形缓冲）。
 * @param buf 载荷缓冲；成功后调用方不得再使用或 unref。
 */
ZMS_API ztk_err_t zms_gop_queue_write_buf(zms_gop_queue *r, ztk_buf *buf, const zms_frame *frame);

/** 排空环级合并唤醒（遗留 / PLAY 订阅）。 */
ZMS_API int zms_gop_queue_drain_wake(zms_gop_queue *r);

/** 排空该读者按会话的唤醒（MPSC + wake_pending）。 */
ZMS_API int zms_gop_reader_drain_wake(const zms_gop_reader *rd);

ZMS_API ztk_err_t zms_gop_queue_set_video_config(zms_gop_queue *r, const void *data, size_t len);
ZMS_API ztk_err_t zms_gop_queue_set_audio_config(zms_gop_queue *r, const void *data, size_t len);
ZMS_API const uint8_t *zms_gop_queue_video_config(const zms_gop_queue *r, size_t *len);
ZMS_API const uint8_t *zms_gop_queue_audio_config(const zms_gop_queue *r, size_t *len);

/**
 * @brief 加锁复制视频配置（避免与并发 set_*_config 产生 UAF）。
 * @return 已复制字节数；不可用时为 0。
 */
ZMS_API size_t zms_gop_queue_copy_video_config(zms_gop_queue *r, uint8_t *buf, size_t cap);
ZMS_API size_t zms_gop_queue_copy_audio_config(zms_gop_queue *r, uint8_t *buf, size_t cap);
ZMS_API zms_codec_id zms_gop_queue_audio_codec(const zms_gop_queue *r);

/** 当前附着 gop_queue 的读者数（PLAY session 上限见 media_limits.h） */
ZMS_API int zms_gop_reader_count(const zms_gop_queue *r);
ZMS_API zms_gop_reader *zms_gop_reader_attach(zms_gop_queue *r);

/** 附着并从缓冲头部顺序读（非直播 GOP 尾）。 */
ZMS_API zms_gop_reader *zms_gop_reader_attach_beginning(zms_gop_queue *r);
ZMS_API void zms_gop_reader_detach(zms_gop_reader *rd);

/** 读游标向直播边缘前移（仅向前裁剪；起播 bootstrap 用 seek_gop_key）。 */
ZMS_API void zms_gop_reader_seek_live(zms_gop_reader *rd);

/** 定位到当前 GOP 内首个同步关键帧（RTMP/HTTP-FLV 起播）。 */
ZMS_API void zms_gop_reader_seek_gop_key(zms_gop_reader *rd);

/** 先到直播边缘，再落到最近 GOP 同步点（RTSP attach use_gop）。 */
ZMS_API void zms_gop_reader_seek_live_key(zms_gop_reader *rd);

/**
 * @brief seek_live_key 后推进到窗口内首个 IDR；若尚无则停在同步点等待。
 */
ZMS_API void zms_gop_reader_seek_live_idr(zms_gop_reader *rd);

/**
 * @brief RTSP bootstrap：仅当前 GOP 最后一个 IDR（无 CRA 回退）。
 */
ZMS_API void zms_gop_reader_seek_rtsp_idr(zms_gop_reader *rd);

/** 从 read_idx 向前找到首个解码起播帧（如 H.265 IDR）。 */
ZMS_API void zms_gop_reader_seek_decode_start(zms_gop_reader *rd);

/** 快照 read_idx 处槽位（诊断 / bootstrap 附着检查）。 */
ZMS_API int zms_gop_reader_slot_at_read(const zms_gop_reader *rd, zms_gop_slot *out);

/** @return 非零表示读游标落在解码起播帧上。 */
ZMS_API int zms_gop_reader_at_decode_start(const zms_gop_reader *rd);

/** @return 读者相对写端的帧滞后（0 表示已追上直播）。 */
ZMS_API size_t zms_gop_reader_lag(const zms_gop_reader *rd);

/**
 * @brief 读取下一槽位并推进游标。
 * @return 成功为 1，无数据为 0，错误为负。
 */
ZMS_API int zms_gop_reader_read(zms_gop_reader *rd, zms_gop_slot *out);

/**
 * @brief 按 dts_ms peek/交织（RTSP PLAY / FLV 的 A/V 复用）。
 * @param max_skew_ms 一路相对另一路允许的最大超前（ms）。
 */
ZMS_API int zms_gop_reader_peek_muxed(zms_gop_reader *rd, zms_gop_slot *out, uint32_t max_skew_ms);
ZMS_API int zms_gop_reader_read_muxed(zms_gop_reader *rd, zms_gop_slot *out, uint32_t max_skew_ms);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_ENGINE_GOP_GOP_QUEUE_H */
