#ifndef ZMS_VOD_SOURCE_H
#define ZMS_VOD_SOURCE_H

/**
 * 文件点播源：已注册 VOD format demux → vod_buffer，供 RTSP/RTMP/HTTP-FLV（不复用 gop_queue）。
 */
#include "zms/engine/stream/stream_hub.h"
#include "zms/ops/service/config.h"
#include "zms/vod/io/vod_buffer.h"
#include "zms/zms_export.h"
#include "ztk/poller/poller.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zms_vod_config {
    char record_app[ZMS_APP_MAX];
    char record_root[ZMS_CFG_PATH_MAX];
    int file_repeat;
} zms_vod_config;

ZMS_API void zms_vod_config_default(zms_vod_config *cfg);
ZMS_API void zms_vod_config_apply(zms_config *cfg);
ZMS_API const zms_vod_config *zms_vod_config_get(void);

/** app 是否为点播 record_app（配置项） */
ZMS_API int zms_vod_is_record_app(const char *app);

/** 相对路径安全校验（禁止 .. 与绝对路径） */
ZMS_API int zms_vod_rel_path_safe(const char *rel);

/** 解析磁盘路径：{root}/{app}/{rel}，URL 与目录一一对应 */
ZMS_API int zms_vod_resolve_rel_path(const char *app, const char *rel, char *out, size_t out_cap);

/**
 * 解析点播磁盘文件：{root}/{app}/{stream}
 * - `foo.{format}.flv`：format FLV-wrap 时，打开对应磁盘容器文件
 * - `foo.flv` / `foo.mkv` / `foo.mov` 等：已注册磁盘格式文件
 */
ZMS_API int zms_vod_resolve_file_path(const char *app, const char *stream, char *out,
                                      size_t out_cap);

/** `*.{format}.flv`：容器点播转 FLV 拉流名，不是磁盘上的 `.flv` 文件 */
ZMS_API int zms_vod_stream_is_vod_flv_wrap_suffix(const char *stream);

/** 磁盘原生文件直出格式（当前为 `.flv`，排除 `*.{format}.flv` 封装后缀） */
ZMS_API int zms_vod_stream_is_native_flv_file(const char *stream);

/** stream 是否已带已注册磁盘容器后缀（非原生直出文件） */
ZMS_API int zms_vod_stream_has_disk_container_ext(const char *stream);

/**
 * 点播 stream 规范名：RTMP 前缀；`*.{format}.flv` 去尾 `.flv`。
 * 原生容器后缀保留；无后缀则补默认 VOD format 后缀。
 */
ZMS_API void zms_vod_canonical_stream(const char *stream_in, char *stream_out, size_t out_cap);

/** m3u8 相对路径转对应 MP4 stream（foo/bar.m3u8 → foo/bar.mp4） */
ZMS_API int zms_vod_m3u8_rel_to_mp4_stream(const char *m3u8_rel, char *stream_out, size_t out_cap);

/** MP4 stream 转对应 m3u8 相对路径（foo/bar.mp4 → foo/bar.m3u8） */
ZMS_API int zms_vod_mp4_stream_to_m3u8_rel(const char *mp4_stream, char *m3u8_rel, size_t rel_cap);

/** 去掉 .mp4 得到基名，如 test.mp4 → test */
ZMS_API void zms_vod_stream_basename(const char *stream_in, char *base_out, size_t out_cap);

/** 由 TS 相对路径推断 MP4 stream（按需动态生成分片） */
ZMS_API int zms_vod_infer_stream_from_ts(const char *app, const char *ts_rel, char *stream_out,
                                         size_t out_cap);

ZMS_API int zms_media_source_is_vod(const zms_media_source *s);

/**
 * 打开文件点播并注册媒体源（publish_origin=mp4_vod，历史枚举名）。
 * @param file_path 空则按 app/stream 自动解析
 */
ZMS_API zms_media_source *zms_vod_source_open(const char *app, const char *stream,
                                              const char *file_path, ztk_poller *poller);
ZMS_API void zms_vod_source_stop(zms_media_source *src);

/** find_for_play 懒加载：app==record_app 时按路径打开 */
ZMS_API zms_media_source *zms_vod_source_find_or_open(const char *schema, const char *app,
                                                      const char *stream, ztk_poller *poller);

/** PLAY/seek：重建 demux+fifo 到文件头同步 */
ZMS_API void zms_vod_source_prepare_play(zms_media_source *src);
/** 按媒体时间 seek(ms)，返回实际位置 */
ZMS_API uint64_t zms_vod_source_seek_ms(zms_media_source *src, uint64_t ms);
/** 单读者 seek：优先 fifo 内重定位，否则 demux（共享 fifo） */
ZMS_API uint64_t zms_vod_source_seek_for_reader(zms_media_source *src, zms_vod_buffer_reader *rd,
                                                uint64_t ms);
ZMS_API uint64_t zms_vod_source_duration_ms(const zms_media_source *src);
/**
 * 已打开点播源的磁盘路径（loadMP4File 显式路径或解析路径）。
 * @return 1 成功写入 out，0 无 publisher / 无路径
 */
ZMS_API int zms_vod_source_file_path(const zms_media_source *src, char *out, size_t out_cap);
/** 只读探测 VOD 文件 duration，不注册媒体源、不启 HLS */
ZMS_API uint64_t zms_vod_probe_duration_ms(const char *app, const char *stream);
ZMS_API const struct zms_vod_flv_index *zms_vod_source_flv_index(const zms_media_source *src);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_VOD_SOURCE_H */
