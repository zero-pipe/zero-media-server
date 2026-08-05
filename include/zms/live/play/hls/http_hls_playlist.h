#ifndef ZMS_LIVE_EGRESS_HLS_PLAYLIST_H
#define ZMS_LIVE_EGRESS_HLS_PLAYLIST_H

/**
 * HLS 切片索引与 m3u8（m3u8 由 vendored libhls/hls-m3u8 生成；TS 字节存于内存环）
 */
#include "zms/zms_export.h"
#include "ztk/util/buf.h"
#include "ztk/ztk_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zms_http_hls_segment {
    char name[64];
    uint32_t dur_ms;
    ztk_buf *buf;
} zms_http_hls_segment;

typedef struct zms_http_hls_playlist zms_http_hls_playlist;

typedef struct zms_http_hls_playlist_opts {
    float segment_duration_sec;
    unsigned segment_count;
    /** 1 = HLS version 7 fMP4 (.m4s + init.mp4)，用于 VP8/VP9/AV1 等 TS 不友好编码 */
    int use_fmp4;
} zms_http_hls_playlist_opts;

ZMS_API zms_http_hls_playlist *zms_http_hls_playlist_create(const zms_http_hls_playlist_opts *opts);
ZMS_API void zms_http_hls_playlist_destroy(zms_http_hls_playlist *m);
/** 清空切片环与 m3u8（点播新会话） */
ZMS_API void zms_http_hls_playlist_reset(zms_http_hls_playlist *m);

/** 写入完整 TS 切片并更新 m3u8 列表（由 hls_media 回调触发） */
ZMS_API ztk_err_t zms_http_hls_playlist_push_segment(zms_http_hls_playlist *m, const void *data,
                                                     size_t len, int64_t pts_ms, int64_t dts_ms,
                                                     uint64_t duration_ms, int discontinuity);

/**
 * @brief 写入 TS 切片并接管 @a buf 所有权（调用方不得再 unref）。
 */
ZMS_API ztk_err_t zms_http_hls_playlist_push_segment_buf(zms_http_hls_playlist *m, ztk_buf *buf,
                                                         int64_t pts_ms, int64_t dts_ms,
                                                         uint64_t duration_ms, int discontinuity);

ZMS_API ztk_err_t zms_http_hls_playlist_build_m3u8(const zms_http_hls_playlist *m, char *out,
                                                   size_t cap, size_t *out_len);

/** 点播：按总时长生成完整 m3u8 索引（VLC 显示总时长）；add_endlist 在整片结束时可置 1 */
ZMS_API ztk_err_t zms_http_hls_playlist_build_vod_index_m3u8(const zms_http_hls_playlist *m,
                                                             char *out, size_t cap, size_t *out_len,
                                                             uint64_t total_dur_ms,
                                                             uint32_t seg_dur_ms, int add_endlist);

ZMS_API int zms_http_hls_playlist_has_segment(const zms_http_hls_playlist *m, const char *name);
ZMS_API ztk_err_t zms_http_hls_playlist_copy_segment(zms_http_hls_playlist *m, const char *name,
                                                     uint8_t *buf, size_t cap, size_t *out_len);
/** 命中切片则 ztk_buf_ref 给调用方；调用方须 ztk_buf_unref */
ZMS_API ztk_err_t zms_http_hls_playlist_ref_segment(zms_http_hls_playlist *m, const char *name,
                                                    ztk_buf **out_buf, size_t *out_len);

/** 已就绪切片数量；out_name/out_len 可选，返回最新一片名与字节数 */
ZMS_API int zms_http_hls_playlist_segment_count(const zms_http_hls_playlist *m);
ZMS_API int zms_http_hls_playlist_latest_segment(const zms_http_hls_playlist *m, char *out_name,
                                                 size_t name_cap, size_t *out_len);

ZMS_API int zms_http_hls_playlist_use_fmp4(const zms_http_hls_playlist *m);

/** fMP4 init segment（init.mp4）；@a buf 所有权转移给 maker */
ZMS_API ztk_err_t zms_http_hls_playlist_store_init_segment(zms_http_hls_playlist *m, ztk_buf *buf);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_LIVE_EGRESS_HLS_PLAYLIST_H */
