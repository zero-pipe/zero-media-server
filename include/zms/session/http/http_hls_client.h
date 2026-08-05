#ifndef ZMS_SESSION_HTTP_HLS_CLIENT_H
#define ZMS_SESSION_HTTP_HLS_CLIENT_H

/**
 * HLS 拉流客户端（TS 切片 + m3u8 刷新；fMP4 待补） */
#include "zms/zms_export.h"
#include "zms/engine/frame.h"
#include "ztk/ztk_errno.h"

#ifdef __cplusplus
extern "C" {
#endif

struct ztk_poller;
struct ztk_ssl_ctx;
typedef struct ztk_poller ztk_poller;
typedef struct ztk_ssl_ctx ztk_ssl_ctx;

typedef struct zms_http_hls_client zms_http_hls_client;

typedef void (*zms_http_hls_client_on_ready_cb)(void *user);
typedef void (*zms_http_hls_client_on_frame_cb)(const zms_frame *frame, void *user);
typedef void (*zms_http_hls_client_on_error_cb)(ztk_err_t err, void *user);

typedef struct zms_http_hls_client_opts {
    ztk_poller *poller;
    /** http(s)://index.m3u8 */
    const char *url;
    ztk_ssl_ctx *ssl_ctx;
    zms_http_hls_client_on_ready_cb on_ready;
    zms_http_hls_client_on_frame_cb on_frame;
    zms_http_hls_client_on_error_cb on_error;
    void *user;
} zms_http_hls_client_opts;

ZMS_API zms_http_hls_client *zms_http_hls_client_create(const zms_http_hls_client_opts *opts);
ZMS_API void zms_http_hls_client_destroy(zms_http_hls_client *c);
ZMS_API ztk_err_t zms_http_hls_client_play(zms_http_hls_client *c);
ZMS_API void zms_http_hls_client_stop(zms_http_hls_client *c);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_SESSION_HTTP_HLS_CLIENT_H */
