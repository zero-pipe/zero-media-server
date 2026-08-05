#ifndef ZMS_SESSION_HTTP_FLV_CLIENT_H
#define ZMS_SESSION_HTTP_FLV_CLIENT_H

/**
 * HTTP(S)-FLV 拉流客户端：GET + FLV tag 解析。
 * 归一化入站：on_media 原样 tag 交给 channel.input_rtmp_*（与 RTMP 拉流一致）。
 * on_frame：经 flv_tag_demuxer 解为 ES（仅 demo/legacy）。
 */
#include "zms/zms_export.h"
#include "zms/engine/frame.h"
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

typedef struct zms_http_flv_client zms_http_flv_client;

typedef void (*zms_http_flv_client_on_ready_cb)(void *user);
typedef void (*zms_http_flv_client_on_media_cb)(uint8_t msg_type, uint32_t tag_dts_ms,
                                                const void *body, size_t len, void *user);
typedef void (*zms_http_flv_client_on_frame_cb)(const zms_frame *frame, void *user);
typedef void (*zms_http_flv_client_on_error_cb)(ztk_err_t err, void *user);

typedef struct zms_http_flv_client_opts {
    ztk_poller *poller;
    /** http:// https://host[:port]/path…flv */
    const char *url;
    /** https 时必填 */
    ztk_ssl_ctx *ssl_ctx;
    zms_http_flv_client_on_ready_cb on_ready;
    /** 推荐：与 rtmp_client.on_media 相同，走 channel FLV/RTMP 入站 */
    zms_http_flv_client_on_media_cb on_media;
    /** 可选：ES 帧（on_media 二选一或仅 legacy 使用 on_frame） */
    zms_http_flv_client_on_frame_cb on_frame;
    zms_http_flv_client_on_error_cb on_error;
    void *user;
} zms_http_flv_client_opts;

ZMS_API zms_http_flv_client *zms_http_flv_client_create(const zms_http_flv_client_opts *opts);
ZMS_API void zms_http_flv_client_destroy(zms_http_flv_client *c);
ZMS_API ztk_err_t zms_http_flv_client_play(zms_http_flv_client *c);
ZMS_API void zms_http_flv_client_stop(zms_http_flv_client *c);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_SESSION_HTTP_FLV_CLIENT_H */
