#ifndef ZMS_VOD_HLS_H
#define ZMS_VOD_HLS_H

/**
 * 点播 HLS（URL 映射到磁盘路径）。
 *
 * - 静态：任意 `{root}/{app}/*.m3u8` + 同目录 `.ts`
 * - 动态：与 MP4 同目录生成 `{stem}.m3u8`、`{N}.ts`
 */
#include "zms/engine/stream/stream_hub.h"
#include "zms/zms_export.h"
#include "ztk/ztk_errno.h"

struct ztk_poller;
typedef struct ztk_poller ztk_poller;

#ifdef __cplusplus
extern "C" {
#endif

/** 检查 MP4 同目录是否存在静态 m3u8 包 */
ZMS_API int zms_vod_hls_has_static_pack(const char *app, const char *stream);

/** 由 MP4 stream 名解析 m3u8 磁盘路径（推导 {stem}.m3u8） */
ZMS_API int zms_vod_hls_resolve_playlist(const char *app, const char *stream, char *out,
                                         size_t out_cap);

/** 由相对 URL 解析 TS 分片文件路径（如 hls_h265/foo/000.ts） */
ZMS_API int zms_vod_hls_resolve_segment_file(const char *app, const char *stream, const char *rel,
                                             char *out, size_t out_cap);

/** 按需确保/生成 m3u8（静态包已存在时为 no-op） */
ZMS_API ztk_err_t zms_vod_hls_ensure_playlist(zms_media_source *src);

/** 回退：按需 mux 一片 TS（静态包存在时仅检查文件是否存在） */
ZMS_API ztk_err_t zms_vod_hls_ensure_segment(zms_media_source *src, uint64_t seg_no,
                                             ztk_poller *poller);

ZMS_API int zms_vod_hls_playlist_path(const char *app, const char *stream, char *out,
                                      size_t out_cap);
ZMS_API int zms_vod_hls_segment_path(const char *app, const char *stream, uint64_t seg_no,
                                     char *out, size_t out_cap);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_VOD_HLS_H */
