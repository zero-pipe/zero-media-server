#ifndef ZMS_SESSION_DISPATCHER_H
#define ZMS_SESSION_DISPATCHER_H

/**
 * @file session_dispatcher.h
 * @brief Session 分发：URL/会话枢纽解复用与出站管线。
 */
#include "zms/engine/stream/stream_hub.h"
#include "zms/egress/egress_source.h"
#include "zms/zms_export.h"
#include "ztk/ztk_errno.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZMS_SESSION_RTMP "rtmp"
#define ZMS_SESSION_RTSP "rtsp"
#define ZMS_SESSION_HTTP_FLV "http-flv"
#define ZMS_SESSION_HTTP_TS "http-ts"
#define ZMS_SESSION_WEBRTC "webrtc"
#define ZMS_SESSION_SRT "srt"
#define ZMS_SESSION_RTP_PS "rtp-ps"

typedef enum zms_session_live_mode {
    ZMS_SESSION_LIVE_GOP = 0,
    ZMS_SESSION_LIVE_KEY = 1,
    ZMS_SESSION_LIVE_EDGE = 2,
} zms_session_live_mode;

typedef struct zms_session_play_opts {
    const char *player;
    uint64_t seek_ms;
} zms_session_play_opts;

typedef struct zms_session_publish_opts {
    const char *schema;
} zms_session_publish_opts;

typedef struct zms_session_dispatch_ops {
    const char *name;
    ztk_err_t (*on_play_live)(void *session, zms_media_source *src,
                              const zms_session_play_opts *opts);
    ztk_err_t (*on_play_vod)(void *session, zms_media_source *src,
                             const zms_session_play_opts *opts);
    ztk_err_t (*on_publish)(void *session, zms_media_source *src,
                            const zms_session_publish_opts *opts);
    void (*on_teardown)(void *session);
} zms_session_dispatch_ops;

ZMS_API void zms_session_dispatch_register(const zms_session_dispatch_ops *ops);
ZMS_API void zms_session_dispatch_register_all(void);

ZMS_API const zms_session_dispatch_ops *zms_session_dispatch_find(const char *name);

/** 统一直播读者附着（GOP / live_key / live_edge）。 */
ZMS_API ztk_err_t zms_session_play_open_live(zms_egress_source *play, zms_media_source *src,
                                             zms_session_live_mode mode);

ZMS_API ztk_err_t zms_session_play_open_vod(zms_egress_source *play, zms_media_source *src,
                                            uint64_t seek_ms);

ZMS_API void zms_session_play_close(zms_egress_source *play);

/** 按协议名分发播放（直播 / VOD 自动分支）。 */
ZMS_API ztk_err_t zms_session_attach_play(const char *protocol, void *session,
                                          zms_media_source *src, const zms_session_play_opts *opts);

ZMS_API void zms_session_detach_play(const char *protocol, void *session);

ZMS_API ztk_err_t zms_session_attach_publish(const char *protocol, void *session,
                                             zms_media_source *src,
                                             const zms_session_publish_opts *opts);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_SESSION_DISPATCHER_H */
