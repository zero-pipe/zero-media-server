#ifndef ZMS_SRC_SESSION_HTTP_SESSION_INTERNAL_H
#define ZMS_SRC_SESSION_HTTP_SESSION_INTERNAL_H

/**
 * @file http_session_internal.h
 * @brief HTTP session 结构体与路由辅助。
 *
 * Play muxer 在此为不完整类型（LoD）：具体头文件仅由创建/销毁/驱动它们的 .c 包含。
 */
#include "zms/session/http/http_service.h"
#include "zms/session/http/http_request_reader.h"
#include "zms/engine/stream/stream_hub.h"
#include "zms/egress/egress_source.h"
#include "ztk/net/tcp_server.h"
#include "ztk/poller/poller_pool.h"
#include "ztk/util/buf.h"
#include <stdint.h>
#include <stdio.h>

typedef enum zms_http_session_state {
    ZMS_HTTP_SESSION_STATE_IDLE,
    ZMS_HTTP_SESSION_STATE_STREAMING,
    ZMS_HTTP_SESSION_STATE_WS_STREAMING,
    ZMS_HTTP_SESSION_STATE_FILE_SENDING,
    ZMS_HTTP_SESSION_STATE_HLS_SENDING,
} zms_http_session_state;

typedef struct zms_http_session zms_http_session;

typedef struct zms_flv_live_muxer zms_flv_live_muxer;
typedef struct zms_mpegts_egress zms_mpegts_egress;
typedef struct zms_vod_flv_muxer zms_vod_flv_muxer;
typedef struct zms_vod_play_lane zms_vod_play_lane;

struct zms_http_session {
    ztk_tcp_session *tcp;
    zms_http_service *server;
    zms_http_request_reader *splitter;
    zms_flv_live_muxer *live_muxer;
    zms_mpegts_egress *ts_muxer;
    zms_egress_source play;
    zms_vod_flv_muxer *vod_muxer;
    zms_media_source *source;
    zms_vod_play_lane *vod_lane;
    zms_http_session_state state;
    int reader_attached;
    FILE *file_fp;
    uint64_t file_remain;
    size_t hls_send_len;
    size_t hls_send_off;
    const char *play_event;
    uint64_t
        play_start_ms; /**< 播放开始 ztk_monotonic_ms()；供 on_play_stop 上报 duration_ms */
    int ws_mode;
    uint8_t *send_buf;
    size_t send_cap;
};

struct zms_http_service {
    ztk_tcp_server *tcp;
    char api_secret[128];
    ztk_poller *poller;
    ztk_poller_pool *poller_pool;
    const struct zms_config *cfg;
#if defined(ZMS_ENABLE_WEBRTC) && ZMS_ENABLE_WEBRTC
    struct zms_webrtc_service *webrtc;
#endif
};

extern struct zms_http_service *g_http_service_instance;

void zms_http_session_stop_stream(zms_http_session *hs);
void zms_http_session_stream_flush(zms_http_session *hs);
void zms_http_session_flush(zms_http_session *hs);

/** 写出 (live 或 vod) muxer start() 产生的 FLV 文件头。 */
typedef ztk_err_t (*zms_flv_start_hdr_fn)(void *muxer, int has_audio, int has_video, uint8_t *out,
                                          size_t cap, size_t *out_len);

/**
 * HTTP-FLV bootstrap 尾部共享：经 @p start 发出 FLV 头，进入
 * ZMS_HTTP_SESSION_STATE_STREAMING 并 kick 首块 flush。直播与点播共用。
 */
void zms_http_flv_emit_header_and_stream(zms_http_session *hs, int has_audio, int has_video,
                                         zms_flv_start_hdr_fn start, void *muxer);

void zms_http_response_send_error(zms_http_session *hs, int code, const char *reason);
void zms_http_response_send_json(zms_http_session *hs, int status, const char *body,
                                 size_t body_len);
void zms_http_response_send_bytes(zms_http_session *hs, int status, const char *ctype,
                                  const void *body, size_t body_len);
void zms_http_response_send_bytes_buf(zms_http_session *hs, int status, const char *ctype,
                                      ztk_buf *body);

void zms_http_route_parse_flv_path(const char *path, char *app, char *stream, uint64_t *seek_ms);
/** 直播 HTTP-TS {app}/{stream}.ts（排除 HLS 分片 {stream}.{N}.ts）*/
int zms_http_route_parse_live_ts_path(const char *path, char *app, char *stream);
int zms_http_route_parse_mp4_path(const char *path, char *app, char *stream);
int zms_http_route_parse_dash_path(const char *path, char *app, char *stream, char *file,
                                   size_t file_cap);
int zms_http_route_parse_vod_hls_path(const char *path, char *app, char *stream, char *file,
                                      size_t file_cap);
int zms_http_route_parse_hls_path(const char *path, char *app, char *stream, char *file,
                                  size_t file_cap);

/** 直播 m3u8 0.ts 改为 /{app}/{stream}.0.ts 绝对路径 */
size_t zms_http_route_rewrite_live_hls_m3u8(char *buf, size_t cap, const char *app,
                                            const char *stream);

/** 点播 m3u8：分片行改为 /{app}/{dir}/绝对路径（与磁盘 URL 一致） */
size_t zms_http_route_rewrite_vod_hls_m3u8(char *buf, size_t cap, const char *app,
                                           const char *m3u8_rel);

void zms_http_live_flv_start(zms_http_session *hs, zms_media_source *src, const char *app,
                             const char *stream, const char *ws_key);
void zms_http_live_ts_start(zms_http_session *hs, zms_media_source *src, const char *app,
                            const char *stream);
void zms_http_vod_flv_start(zms_http_session *hs, zms_media_source *src, const char *app,
                            const char *stream, uint64_t seek_ms, const char *range_hdr);
void zms_http_vod_mp4_serve(zms_http_session *hs, zms_media_source *src, const char *app,
                            const char *stream, const char *range_hdr, int head_only);

void zms_http_live_hls_serve(zms_http_session *hs, zms_media_source *src, const char *app,
                             const char *stream, const char *file);
void zms_http_live_dash_serve(zms_http_session *hs, zms_media_source *src, const char *app,
                              const char *stream, const char *file);
void zms_http_vod_hls_serve(zms_http_session *hs, zms_media_source *src, const char *app,
                            const char *stream, const char *file);

void zms_http_response_send_static_file(zms_http_session *hs, zms_media_source *src,
                                        const char *path, const char *ctype, const char *player);
void zms_http_response_send_vod_flv_file(zms_http_session *hs, zms_media_source *src,
                                         const char *flv_path, const char *range_hdr,
                                         uint64_t query_seek_ms);
void zms_http_response_send_vod_mp4_file(zms_http_session *hs, zms_media_source *src,
                                         const char *mp4_path, const char *range_hdr,
                                         int head_only);
void zms_http_response_send_hls_body(zms_http_session *hs, const char *ctype, size_t body_len);

/** 按绝对路径下载附件（Content-Disposition: attachment）。 */
void zms_http_response_send_download_file(zms_http_session *hs, const char *path);

#endif /* ZMS_SRC_SESSION_HTTP_SESSION_INTERNAL_H */
