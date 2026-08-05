#ifndef ZMS_SESSION_RTSP_CLIENT_H
#define ZMS_SESSION_RTSP_CLIENT_H

#include "zms/zms_export.h"
#include "ztk/ztk_errno.h"
#include "zms/engine/frame.h"
#include "zms/engine/media_track.h"
#include "rtsp_transport.h"
#include "rtsp_sdp.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ztk_poller;
struct ztk_tcp_client;
struct ztk_ssl_ctx;
typedef struct ztk_poller ztk_poller;
typedef struct ztk_ssl_ctx ztk_ssl_ctx;

typedef void (*zms_rtsp_client_on_ready_cb)(void *user);
typedef void (*zms_rtsp_client_on_track_cb)(const zms_media_track *track, void *user);
typedef void (*zms_rtsp_client_on_frame_cb)(const zms_frame *frame, void *user);
typedef void (*zms_rtsp_client_on_error_cb)(ztk_err_t err, void *user);

typedef struct zms_rtsp_client zms_rtsp_client;

typedef struct zms_rtsp_client_opts {
    ztk_poller *poller;
    /** rtsp:// rtsps:// */
    const char *url;
    /** rtsps 时必填（通常由 zms_pull_ssl_ctx 注入） */
    ztk_ssl_ctx *ssl_ctx;
    /** RTP 传输：AUTO（先 TCP，SETUP 461 时回退 UDP）/ TCP / UDP */
    zms_rtsp_rtp_mode rtp_mode;
    /** 可选；未设时从 url user:pass@ 解析 */
    const char *username;
    const char *password;
    /** -1=无限重试 */
    int retry_count;
    int reconnect_delay_ms;
    zms_rtsp_client_on_ready_cb on_ready;
    zms_rtsp_client_on_track_cb on_track;
    zms_rtsp_client_on_frame_cb on_frame;
    zms_rtsp_client_on_error_cb on_error;
    void *user;
} zms_rtsp_client_opts;

ZMS_API zms_rtsp_client *zms_rtsp_client_create(const zms_rtsp_client_opts *opts);
ZMS_API void zms_rtsp_client_destroy(zms_rtsp_client *c);
ZMS_API ztk_err_t zms_rtsp_client_play(zms_rtsp_client *c);
ZMS_API void zms_rtsp_client_stop(zms_rtsp_client *c);
ZMS_API const zms_sdp_session *zms_rtsp_client_session(const zms_rtsp_client *c);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_SESSION_RTSP_CLIENT_H */
