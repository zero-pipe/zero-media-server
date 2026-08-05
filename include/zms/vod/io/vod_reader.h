#ifndef ZMS_VOD_READER_VOD_READER_H
#define ZMS_VOD_READER_VOD_READER_H

/**
 * @file vod_reader.h
 * @brief 通用点播文件读取门面：磁盘容器 → vod_buffer。
 *
 * VOD 扩展边界。当前 MP4/MOV/MKV/FLV 由 mp4_vod_reader 支撑；
 * 新文件容器应在此附着，勿将容器专用 reader 名泄漏到 play 代码。
 */
#include "zms/engine/stream/stream_hub.h"
#include "zms/vod/io/vod_buffer.h"
#include "zms/zms_export.h"
#include "ztk/poller/poller.h"
#include <stddef.h>
#include <stdint.h>

struct zms_vod_flv_index;

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zms_vod_reader zms_vod_reader;

typedef struct zms_vod_reader_opts {
    const char *file_path;
    const char *app;
    const char *stream;
    zms_vod_buffer *fifo;
    zms_media_source *source;
    double speed;
    int loop;
    /** 未消费 fifo 深度超过此值时暂停 demux。 */
    size_t fifo_high_water;
} zms_vod_reader_opts;

ZMS_API zms_vod_reader *zms_vod_reader_open(const zms_vod_reader_opts *opts);
ZMS_API void zms_vod_reader_close(zms_vod_reader *r);
ZMS_API void zms_vod_reader_bind_poller(zms_vod_reader *r, ztk_poller *poller);
ZMS_API void zms_vod_reader_bind_poller_lite(zms_vod_reader *r, ztk_poller *poller);
ZMS_API void zms_vod_reader_prefill(zms_vod_reader *r);
ZMS_API void zms_vod_reader_prepare_play(zms_vod_reader *r);
ZMS_API uint64_t zms_vod_reader_seek_ms(zms_vod_reader *r, uint64_t ms);
ZMS_API zms_media_source *zms_vod_reader_source(zms_vod_reader *r);
/** 打开时的磁盘路径；无则返回 NULL */
ZMS_API const char *zms_vod_reader_file_path(const zms_vod_reader *r);
ZMS_API uint64_t zms_vod_reader_duration_ms(const zms_vod_reader *r);
ZMS_API const struct zms_vod_flv_index *zms_vod_reader_flv_index(zms_vod_reader *r);
ZMS_API void zms_vod_reader_set_pump_hold(zms_vod_reader *r, int hold);
ZMS_API int zms_vod_reader_pump(zms_vod_reader *r);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_VOD_READER_VOD_READER_H */
