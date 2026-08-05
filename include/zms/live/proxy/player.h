#ifndef ZMS_PLAYER_PLAYER_H
#define ZMS_PLAYER_PLAYER_H

/**
 * @file player.h
 * @brief 统一拉流播放器 SDK 门面。
 *
 * 嵌入 ZMS 的应用可用本 API 拉取远端流并接收归一化轨/帧，供解码器与渲染集成。
 */
#include "zms/engine/frame.h"
#include "zms/engine/media_track.h"
#include "zms/session/rtsp/rtsp_transport.h"
#include "zms/zms_export.h"
#include "ztk/ztk_errno.h"

#ifdef __cplusplus
extern "C" {
#endif

struct ztk_poller;
struct ztk_ssl_ctx;
typedef struct ztk_poller ztk_poller;
typedef struct ztk_ssl_ctx ztk_ssl_ctx;

typedef struct zms_player zms_player;

typedef void (*zms_player_on_ready_cb)(void *user);
typedef void (*zms_player_on_track_cb)(const zms_media_track *track, void *user);
typedef void (*zms_player_on_frame_cb)(const zms_frame *frame, void *user);
typedef void (*zms_player_on_error_cb)(ztk_err_t err, void *user);

typedef struct zms_player_opts {
    ztk_poller *poller;
    /** RTSP(S)、HTTP(S)-FLV、HTTP(S)-HLS URL。RTMP 帧模式尚未实现。 */
    const char *url;
    /** TLS 协议在底层客户端需要 TLS 上下文时必填。 */
    ztk_ssl_ctx *ssl_ctx;
    /** 仅 RTSP；AUTO 默认 TCP，支持时 UDP 回退。 */
    zms_rtsp_rtp_mode rtsp_rtp_mode;
    int retry_count;
    int reconnect_delay_ms;
    zms_player_on_ready_cb on_ready;
    zms_player_on_track_cb on_track;
    zms_player_on_frame_cb on_frame;
    zms_player_on_error_cb on_error;
    void *user;
} zms_player_opts;

ZMS_API zms_player *zms_player_create(const zms_player_opts *opts);
ZMS_API void zms_player_destroy(zms_player *p);
ZMS_API ztk_err_t zms_player_play(zms_player *p);
ZMS_API void zms_player_stop(zms_player *p);
ZMS_API const char *zms_player_protocol(const zms_player *p);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_PLAYER_PLAYER_H */
