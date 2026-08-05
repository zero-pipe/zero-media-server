#ifndef ZMS_VOD_BUFFER_VOD_BUFFER_H
#define ZMS_VOD_BUFFER_VOD_BUFFER_H

/**
 * @file vod_buffer.h
 * @brief 点播播放缓冲（顺序 ES 缓冲，非 GOP 缓存）。
 *
 * 每次播放会话使用独立 reader；vod_reader 在背压下将 sample 泵入缓冲。
 * 不与直播 gop_queue 路径共享。
 *
 * Copyright (c) zero-media-server
 */
#include "zms/engine/gop/gop_queue.h"
#include "zms/zms_export.h"
#include "ztk/ztk_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZMS_VOD_BUFFER_DEFAULT_CAP 512u

typedef struct zms_vod_buffer zms_vod_buffer;
typedef struct zms_vod_buffer_reader zms_vod_buffer_reader;

ZMS_API zms_vod_buffer *zms_vod_buffer_create(size_t cap);
ZMS_API void zms_vod_buffer_destroy(zms_vod_buffer *buf);

ZMS_API ztk_err_t zms_vod_buffer_write(zms_vod_buffer *buf, const zms_frame *frame);
ZMS_API ztk_err_t zms_vod_buffer_set_video_config(zms_vod_buffer *buf, const void *data,
                                                  size_t len);
ZMS_API ztk_err_t zms_vod_buffer_set_audio_config(zms_vod_buffer *buf, const void *data,
                                                  size_t len);
ZMS_API void zms_vod_buffer_reset(zms_vod_buffer *buf);

/** 将 reader 移到最早未消费帧（prepare_play / seek 之后）。 */
ZMS_API void zms_vod_buffer_reader_seek_beginning(zms_vod_buffer_reader *rd);
ZMS_API const uint8_t *zms_vod_buffer_video_config(const zms_vod_buffer *buf, size_t *len);
ZMS_API const uint8_t *zms_vod_buffer_audio_config(const zms_vod_buffer *buf, size_t *len);
ZMS_API size_t zms_vod_buffer_pending(const zms_vod_buffer *buf);

/** @return 非零表示缓冲含 H.264 IDR（PLAY 前扫描）。 */
ZMS_API int zms_vod_buffer_has_h264_idr(const zms_vod_buffer *buf);

/** @return 非零表示缓冲含 H.264/H.265 视频 sync 关键帧。 */
ZMS_API int zms_vod_buffer_has_video_sync_key(const zms_vod_buffer *buf);

/**
 * @param from_beginning 1 = 从最旧帧读；0 = 从附着游标读。
 */
ZMS_API zms_vod_buffer_reader *zms_vod_buffer_reader_attach(zms_vod_buffer *buf,
                                                            int from_beginning);
ZMS_API void zms_vod_buffer_reader_detach(zms_vod_buffer_reader *rd);
ZMS_API int zms_vod_buffer_reader_read_muxed(zms_vod_buffer_reader *rd, zms_gop_slot *slot);
ZMS_API int zms_vod_buffer_reader_peek_muxed(zms_vod_buffer_reader *rd, zms_gop_slot *slot);

/**
 * @brief 加锁 peek/复制 ES（可抵御并发 mp4 泵覆盖）。
 */
ZMS_API int zms_vod_buffer_reader_peek_muxed_es(zms_vod_buffer_reader *rd, zms_gop_slot *slot,
                                                uint8_t **es_buf, size_t *es_cap,
                                                struct ztk_poller *pol);
ZMS_API void zms_vod_buffer_reader_advance(zms_vod_buffer_reader *rd);
ZMS_API void zms_vod_buffer_reader_seek_video_key(zms_vod_buffer_reader *rd);

/**
 * @brief 在缓冲窗口内按媒体时间 seek。
 * @return 1 成功；实际位置写入 @a out_ms。
 */
ZMS_API int zms_vod_buffer_reader_seek_ms(zms_vod_buffer_reader *rd, uint64_t ms, uint64_t *out_ms);
ZMS_API size_t zms_vod_buffer_reader_lag(const zms_vod_buffer_reader *rd);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_VOD_BUFFER_VOD_BUFFER_H */
