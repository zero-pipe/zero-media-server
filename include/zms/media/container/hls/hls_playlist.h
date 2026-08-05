#ifndef ZMS_CONTAINER_HLS_PLAYLIST_H
#define ZMS_CONTAINER_HLS_PLAYLIST_H

/**
 * @file hls_playlist.h
 * @brief M3U8 播放列表生成门面（基于 zmk libhls hls_m3u8 API）。
 *
 * 使播放/业务层无需包含 `hls-m3u8.h`；HLS maker 经本薄封装驱动 EXT-X-MAP /
 * 分片条目 / 列表渲染。
 */
#include "zms/zms_export.h"
#include "ztk/ztk_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zms_hls_m3u8 zms_hls_m3u8;

/** @param live 滑动窗口/直播参数（透传 libhls）；@param version 3=TS，7=fMP4。 */
ZMS_API zms_hls_m3u8 *zms_hls_m3u8_create(int live, int version);
ZMS_API void zms_hls_m3u8_destroy(zms_hls_m3u8 *m);

/** EXT-X-MAP init 分片 URI。@return 0 成功。 */
ZMS_API int zms_hls_m3u8_set_x_map(zms_hls_m3u8 *m, const char *name);

/** 追加媒体分片条目。@return 0 成功。 */
ZMS_API int zms_hls_m3u8_add(zms_hls_m3u8 *m, const char *name, int64_t pts_ms, int64_t duration_ms,
                             int discontinuity);

/** 渲染播放列表文本。@param eof 1=追加 EXT-X-ENDLIST。@return 0 成功。 */
ZMS_API int zms_hls_m3u8_playlist(zms_hls_m3u8 *m, int eof, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_CONTAINER_HLS_PLAYLIST_H */
