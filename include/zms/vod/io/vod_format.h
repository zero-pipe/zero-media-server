#ifndef ZMS_VOD_READER_VOD_FORMAT_H
#define ZMS_VOD_READER_VOD_FORMAT_H

/**
 * @file vod_format.h
 * @brief 点播磁盘格式描述符注册表。
 *
 * VOD format 绑定文件扩展名、容器 demux id、FLV-wrap 支持与时长探测。
 * 新文件容器（如 AVI）应在 VOD 路径解析前在此注册描述符。
 */
#include "zms/media/container/container_dispatcher.h"
#include "zms/zms_export.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t (*zms_vod_format_probe_duration_fn)(const char *path);

typedef struct zms_vod_format_desc {
    const char *name;
    zms_container_id container;
    const char *const *extensions;
    const char *default_extension;
    int can_flv_wrap;
    int native_file_send;
    zms_vod_format_probe_duration_fn probe_duration_ms;
} zms_vod_format_desc;

typedef int (*zms_vod_format_visit_cb)(const zms_vod_format_desc *fmt, const char *ext, void *user);

ZMS_API void zms_vod_format_register(const zms_vod_format_desc *fmt);
ZMS_API void zms_vod_format_register_all(void);

ZMS_API const zms_vod_format_desc *zms_vod_format_find_by_extension(const char *ext);
ZMS_API const zms_vod_format_desc *zms_vod_format_find_by_path(const char *path);
ZMS_API int zms_vod_format_foreach_extension(zms_vod_format_visit_cb cb, void *user);

ZMS_API const char *zms_vod_format_default_extension(void);
ZMS_API int zms_vod_format_stream_has_supported_ext(const char *stream);
ZMS_API int zms_vod_format_stream_is_native_file(const char *stream);
ZMS_API int zms_vod_format_stream_is_flv_wrap_suffix(const char *stream);
ZMS_API uint64_t zms_vod_format_probe_duration_ms(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_VOD_READER_VOD_FORMAT_H */
