#ifndef ZMS_VOD_READER_MP4_VOD_READER_H
#define ZMS_VOD_READER_MP4_VOD_READER_H

/**
 * @file mp4_vod_reader.h
 * @brief 文件 VOD demuxer：MP4/MOV/MKV/FLV → vod_buffer
 *
 * 不使用 channel 或 gop_queue。MP4 sample DTS 已是相对毫秒，绕过直播 media_timeline。
 *
 * Copyright (c) zero-media-server
 */
#include "zms/engine/stream/stream_hub.h"
#include "zms/vod/io/vod_buffer.h"
#include "zms/zms_export.h"
#include "ztk/ztk_errno.h"
#include "ztk/poller/poller.h"
#include <stddef.h>

struct zms_vod_flv_index;

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zms_mp4_reader zms_mp4_reader;

typedef struct zms_mp4_reader_opts {
    const char *file_path;
    const char *app;
    const char *stream;
    zms_vod_buffer *fifo;
    zms_media_source *source;
    double speed;
    int loop;
    /** 未消费 fifo 深度超过此值时暂停 demux。 */
    size_t fifo_high_water;
    /** 可选门面所有者，存入 zms_media_source::publisher_ctx。 */
    void *owner_ctx;
} zms_mp4_reader_opts;

ZMS_API zms_mp4_reader *zms_mp4_reader_open(const zms_mp4_reader_opts *opts);
ZMS_API void zms_mp4_reader_close(zms_mp4_reader *r);
ZMS_API void zms_mp4_reader_bind_poller(zms_mp4_reader *r, ztk_poller *poller);

/** 绑定 poller 并有限 prefill（每 play lane，避免深栈 pump）。 */
ZMS_API void zms_mp4_reader_bind_poller_lite(zms_mp4_reader *r, ztk_poller *poller);

/** open 后同步预读帧到 vod_buffer。 */
ZMS_API void zms_mp4_reader_prefill(zms_mp4_reader *r);

/** 重置 fifo，从文件头扫描到首个可解码 sync 帧并 prefill。 */
ZMS_API void zms_mp4_reader_prepare_play(zms_mp4_reader *r);

/** 按媒体毫秒 seek；重置 fifo 并 prefill 到 sync 关键帧；返回实际位置。 */
ZMS_API uint64_t zms_mp4_reader_seek_ms(zms_mp4_reader *r, uint64_t ms);
ZMS_API zms_media_source *zms_mp4_reader_source(zms_mp4_reader *r);
/** 打开时的磁盘路径；无则返回 NULL */
ZMS_API const char *zms_mp4_reader_file_path(const zms_mp4_reader *r);
ZMS_API uint64_t zms_mp4_reader_duration_ms(const zms_mp4_reader *r);
ZMS_API const struct zms_vod_flv_index *zms_mp4_reader_flv_index(zms_mp4_reader *r);

/** 暂停/恢复 demux 定时器（RTMP 背压或播放器暂停）。 */
ZMS_API void zms_mp4_reader_set_pump_hold(zms_mp4_reader *r, int hold);

/** 无 poller 时同步推进 demux（FLV 索引构建等）。 */
ZMS_API int zms_mp4_reader_pump(zms_mp4_reader *r);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_VOD_READER_MP4_VOD_READER_H */
