#ifndef ZMS_LIVE_EGRESS_DASH_PLAYLIST_H
#define ZMS_LIVE_EGRESS_DASH_PLAYLIST_H

#include "zms/zms_export.h"
#include "ztk/util/buf.h"
#include "ztk/ztk_errno.h"
#include <stddef.h>
#include <stdint.h>

struct dash_mpd_t;

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zms_http_dash_segment {
    char name[128];
    ztk_buf *buf;
} zms_http_dash_segment;

typedef struct zms_http_dash_playlist zms_http_dash_playlist;

typedef struct zms_http_dash_playlist_opts {
    float segment_duration_sec;
    unsigned segment_count;
    const char *prefix;
} zms_http_dash_playlist_opts;

ZMS_API zms_http_dash_playlist *
zms_http_dash_playlist_create(const zms_http_dash_playlist_opts *opts);
ZMS_API void zms_http_dash_playlist_destroy(zms_http_dash_playlist *m);

ZMS_API struct dash_mpd_t *zms_http_dash_playlist_mpd(zms_http_dash_playlist *m);

/** dash_mpd segment callback：缓存 init/.m4s 分片 */
ZMS_API int zms_http_dash_playlist_on_segment(void *param, int track, const void *data,
                                              size_t bytes, int64_t pts, int64_t dts,
                                              int64_t duration, const char *name);

ZMS_API ztk_err_t zms_http_dash_playlist_build_mpd(zms_http_dash_playlist *m, char *out, size_t cap,
                                                   size_t *out_len);
/** 拷贝 on_segment 后缓存的 MPD（无 recorder 锁，供 HTTP 热路径） */
ZMS_API ztk_err_t zms_http_dash_playlist_copy_mpd(const zms_http_dash_playlist *m, char *out,
                                                  size_t cap, size_t *out_len);
ZMS_API ztk_err_t zms_http_dash_playlist_copy_segment(zms_http_dash_playlist *m, const char *name,
                                                      uint8_t *buf, size_t cap, size_t *out_len);
ZMS_API ztk_err_t zms_http_dash_playlist_ref_segment(zms_http_dash_playlist *m, const char *name,
                                                     ztk_buf **out_buf, size_t *out_len);
/** 缓存中是否已有非 init 的媒体分片（.m4v/.m4a） */
ZMS_API int zms_http_dash_playlist_has_media_segment(const zms_http_dash_playlist *m);
ZMS_API int zms_http_dash_playlist_segment_count(const zms_http_dash_playlist *m);
ZMS_API const char *zms_http_dash_playlist_prefix(const zms_http_dash_playlist *m);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_LIVE_EGRESS_DASH_PLAYLIST_H */
