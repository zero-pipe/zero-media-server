#ifndef ZMS_SESSION_RTMP_CLIENT_H
#define ZMS_SESSION_RTMP_CLIENT_H

/**
 * RTMP 拉流客户端（play）：接收音视频 FLV tag，经回调交给 channel。
 */
#include "zms/zms_export.h"
#include "ztk/ztk_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ztk_poller;
struct ztk_ssl_ctx;
typedef struct ztk_poller ztk_poller;
typedef struct ztk_ssl_ctx ztk_ssl_ctx;

typedef struct zms_rtmp_client zms_rtmp_client;

typedef void (*zms_rtmp_client_on_ready_cb)(void *user);
typedef void (*zms_rtmp_client_on_media_cb)(uint8_t msg_type, uint32_t tag_dts_ms, const void *body,
                                            size_t len, void *user);
typedef void (*zms_rtmp_client_on_error_cb)(ztk_err_t err, void *user);

typedef struct zms_rtmp_client_opts {
    ztk_poller *poller;
    /** rtmp:// rtmps://host[:port]/app/stream[?query] */
    const char *url;
    /** rtmps 时必填（通常由 zms_pull_ssl_ctx 注入） */
    ztk_ssl_ctx *ssl_ctx;
    zms_rtmp_client_on_ready_cb on_ready;
    zms_rtmp_client_on_media_cb on_media;
    zms_rtmp_client_on_error_cb on_error;
    void *user;
} zms_rtmp_client_opts;

/**
 * 拉流 URL → RTMP connect app + play 名（与 ZMS RTMP 服务 / librtmp 约定一致）。
 * rtmp://host/live/stream/id → app=live/stream play=id
 * rtmp://host/live/proxied/stream/id → app=live/proxied play=stream/id
 */
ZMS_API ztk_err_t zms_rtmp_parse_pull_url(const char *url, char *connect_app, size_t app_cap,
                                          char *play_name, size_t play_cap);

ZMS_API zms_rtmp_client *zms_rtmp_client_create(const zms_rtmp_client_opts *opts);
ZMS_API void zms_rtmp_client_destroy(zms_rtmp_client *c);
ZMS_API ztk_err_t zms_rtmp_client_play(zms_rtmp_client *c);
ZMS_API void zms_rtmp_client_stop(zms_rtmp_client *c);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_SESSION_RTMP_CLIENT_H */
