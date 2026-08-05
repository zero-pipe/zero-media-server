#ifndef ZMS_LIVE_PROXY_LIVE_PULL_PROXY_H
#define ZMS_LIVE_PROXY_LIVE_PULL_PROXY_H

/**
 * @file live_pull_proxy.h
 * @brief 拉流代理：将远端 RTSP/RTMP/HTTP-FLV/HLS 接入本地 media_source。
 *
 * 创建绑定已注册源的 @ref zms_live_ingest，观众可通过本地服务器的
 * RTSP、RTMP、HTTP-FLV、HLS 播放。
 *
 * Copyright (c) zero-media-server
 */
#include "zms/live/ingest/common/stream_ingest.h"
#include "zms/engine/stream/stream_hub.h"
#include "zms/session/rtsp/rtsp_transport.h"
#include "zms/zms_export.h"
#include "ztk/ztk_errno.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ztk_poller;
typedef struct ztk_poller ztk_poller;
struct ztk_poller_pool;
typedef struct ztk_poller_pool ztk_poller_pool;

typedef struct zms_live_pull_proxy zms_live_pull_proxy;

typedef void (*zms_live_pull_proxy_on_ready_cb)(void *user);
typedef void (*zms_live_pull_proxy_on_error_cb)(ztk_err_t err, void *user);

typedef struct zms_live_pull_proxy_opts {
    /** poller 与 poller_pool 须恰好设置其一。 */
    ztk_poller *poller;
    ztk_poller_pool *poller_pool;
    /** 拉流 URL：rtsp(s)://、rtmp(s)://、http(s)://…flv、http(s)://…m3u8 */
    const char *pull_url;
    /** rtsps/rtmps 且未用默认 SSL 上下文时必填。 */
    struct ztk_ssl_ctx *ssl_ctx;
    /** 本地 app；NULL 或空默认为 "live"。 */
    const char *app;
    /**
     * 本地 stream 名；NULL/空/auto 从拉流 URL 路径推导尾段
     *（如 live/test_265 → test_265）。
     */
    const char *stream;
    /** 可选前缀，加在推导出的 stream 尾段前。 */
    const char *proxy_prefix;
    /** RTSP 拉流 RTP 模式（RTMP 拉流忽略）。 */
    zms_rtsp_rtp_mode rtp_mode;
    int retry_count;
    int reconnect_delay_ms;
    zms_live_pull_proxy_on_ready_cb on_ready;
    zms_live_pull_proxy_on_error_cb on_error;
    void *user;
} zms_live_pull_proxy_opts;

ZMS_API zms_live_pull_proxy *zms_live_pull_proxy_create(const zms_live_pull_proxy_opts *opts);
ZMS_API void zms_live_pull_proxy_destroy(zms_live_pull_proxy *p);
ZMS_API ztk_err_t zms_live_pull_proxy_start(zms_live_pull_proxy *p);
ZMS_API void zms_live_pull_proxy_stop(zms_live_pull_proxy *p);
ZMS_API zms_media_source *zms_live_pull_proxy_source(zms_live_pull_proxy *p);
ZMS_API zms_live_ingest *zms_live_pull_proxy_ingress(zms_live_pull_proxy *p);

/** 构造 Web API key：vhost/app/stream（vhost 空则用 __defaultVhost__）。 */
ZMS_API void zms_live_pull_proxy_make_key(const char *vhost, const char *app, const char *stream,
                                          char *key, size_t key_cap);

ZMS_API zms_live_pull_proxy *zms_live_pull_proxy_find_by_key(const char *key);

typedef int (*zms_live_pull_proxy_visit_cb)(const char *key, zms_live_pull_proxy *p, void *user);
ZMS_API int zms_live_pull_proxy_foreach(zms_live_pull_proxy_visit_cb cb, void *user);

/** 首段之后的路径尾：live/test_265 → test_265。 */
ZMS_API int zms_live_pull_proxy_path_tail(const char *pull_url, char *tail, size_t tail_cap);

/** 本地 stream = {prefix/}{tail}；空前缀仅用 tail。 */
ZMS_API int zms_live_pull_proxy_build_stream(const char *pull_url, const char *prefix, char *stream,
                                             size_t stream_cap);

ZMS_API const char *zms_live_pull_proxy_app(const zms_live_pull_proxy *p);
ZMS_API const char *zms_live_pull_proxy_stream(const zms_live_pull_proxy *p);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_LIVE_PROXY_LIVE_PULL_PROXY_H */
