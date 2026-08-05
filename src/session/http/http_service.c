#include "zms/session/http/http_service.h"
#include "zms/session/session_dispatcher.h"
#include "zms/session/http/websocket/websocket_framer.h"
#include "zms/ops/service/config.h"
#include "session/http/http_router.h"
#include "session/http/http_session_internal.h"
#include "zms/session/http/http_request_reader.h"
#include "zms/session/play_binding.h"
#include "zms/engine/media_event.h"
#include "zms/live/play/http_flv/flv_live_muxer.h"
#include "zms/egress/mpegts/mpegts_egress.h"
#include "zms/vod/play/vod_flv_muxer.h"
#include "zms/vod/play/vod_play_lane.h"
#if defined(ZMS_ENABLE_WEBRTC) && ZMS_ENABLE_WEBRTC
#include "zms/webrtc/webrtc_service.h"
#endif
#include "ztk/net/tcp_server.h"
#include "ztk/poller/poller_pool.h"
#include "ztk/util/log.h"
#include <stdlib.h>
#include <string.h>

struct zms_http_service *g_http_service_instance;

static zms_play_binding http_play_binding(zms_http_session *hs)
{
    zms_play_binding b;

    memset(&b, 0, sizeof(b));
    b.source = &hs->source;
    b.play = &hs->play;
    b.vod_lane = &hs->vod_lane;
    b.reader_attached = &hs->reader_attached;
    b.play_start_ms = &hs->play_start_ms;
    b.player = hs->play_event ? hs->play_event : "http-flv";
    return b;
}

static void http_play_teardown(zms_http_session *hs)
{
    zms_play_binding bind;

    if (!hs) {
        return;
    }
    /* Wire muxer 归协议所有；binding 仅关闭 readers/lane/记账。 */
    zms_flv_live_muxer_destroy(hs->live_muxer);
    zms_vod_flv_muxer_destroy(hs->vod_muxer);
    hs->live_muxer = NULL;
    hs->vod_muxer = NULL;
    if (hs->ts_muxer) {
        zms_mpegts_egress_destroy(hs->ts_muxer);
        hs->ts_muxer = NULL;
    }
    if (hs->play.readers.gop) {
        zms_session_dispatch_register_all();
        zms_session_detach_play(ZMS_SESSION_HTTP_TS, &hs->play);
    }
    bind = http_play_binding(hs);
    zms_play_binding_close(&bind, 1);
    hs->play_event = NULL;
    hs->ws_mode = 0;
}

static int http_ws_send_raw(void *user, const void *data, size_t len)
{
    zms_http_session *hs = (zms_http_session *)user;
    if (!hs || !hs->tcp) {
        return -1;
    }
    return ztk_tcp_session_send(hs->tcp, data, len) == ZTK_OK ? 0 : -1;
}

static void http_ws_on_close(void *user)
{
    zms_http_session *hs = (zms_http_session *)user;
    if (hs) {
        zms_http_session_stop_stream(hs);
    }
}

void zms_http_session_stop_stream(zms_http_session *hs)
{
    if (!hs) {
        return;
    }
    if (hs->tcp) {
        ztk_tcp_session_out_discard(hs->tcp);
    }
    if (hs->file_fp) {
        fclose(hs->file_fp);
        hs->file_fp = NULL;
    }
    hs->file_remain = 0;
    hs->hls_send_len = 0;
    hs->hls_send_off = 0;
    http_play_teardown(hs);
    hs->state = ZMS_HTTP_SESSION_STATE_IDLE;
    if (hs->splitter) {
        zms_http_request_reader_reset(hs->splitter);
    }
}

static int http_looks_like_request(const void *data, size_t len)
{
    const char *s;

    if (!data || len < 4) {
        return 0;
    }
    s = (const char *)data;
    return strncmp(s, "GET ", 4) == 0 || strncmp(s, "HEAD ", 5) == 0 ||
           strncmp(s, "POST ", 5) == 0 || strncmp(s, "PUT ", 4) == 0 ||
           strncmp(s, "OPTIONS ", 8) == 0;
}

static void on_http_request(const zms_http_request *req, void *user)
{
    zms_http_session *hs = (zms_http_session *)user;

    if (!hs || !req) {
        return;
    }
    if (!zms_http_router_dispatch(hs, req)) {
        zms_http_response_send_error(hs, 404, "Not Found");
    }
}

static void on_recv(ztk_tcp_session *session, const void *data, size_t len, void *user)
{
    zms_http_session *hs = (zms_http_session *)user;
    if (!hs) {
        return;
    }
    if (hs->state == ZMS_HTTP_SESSION_STATE_WS_STREAMING) {
        (void)zms_ws_framer_consume_client((const uint8_t *)data, len, http_ws_send_raw,
                                           http_ws_on_close, hs);
    } else if (hs->state == ZMS_HTTP_SESSION_STATE_IDLE) {
        zms_http_request_reader_input(hs->splitter, data, len);
    } else if (http_looks_like_request(data, len)) {
        zms_http_session_stop_stream(hs);
        zms_http_request_reader_input(hs->splitter, data, len);
    }
    zms_http_session_flush(hs);
}

static void on_manager(ztk_tcp_session *session, void *user)
{
    (void)session;
    zms_http_session_flush((zms_http_session *)user);
}

static void on_error(ztk_tcp_session *session, void *user)
{
    (void)session;
    zms_http_session *hs = (zms_http_session *)user;
    if (!hs) {
        return;
    }
    if (hs->file_fp) {
        fclose(hs->file_fp);
        hs->file_fp = NULL;
    }
    http_play_teardown(hs);
    zms_http_request_reader_destroy(hs->splitter);
    free(hs->send_buf);
    free(hs);
}

static void *session_create_user(ztk_tcp_server *srv, ztk_tcp_session *session)
{
    (void)srv;
    zms_http_session *hs = (zms_http_session *)calloc(1, sizeof(*hs));
    if (!hs) {
        return NULL;
    }
    hs->server = g_http_service_instance;
    hs->send_cap = 4 * 1024 * 1024;
    hs->send_buf = (uint8_t *)malloc(hs->send_cap);
    if (!hs->send_buf) {
        free(hs);
        return NULL;
    }
    hs->tcp = session;
    {
        zms_http_request_reader_opts opts = {on_http_request, hs};
        hs->splitter = zms_http_request_reader_create(&opts);
    }
    if (!hs->splitter) {
        free(hs->send_buf);
        free(hs);
        return NULL;
    }
    return hs;
}

zms_http_service *zms_http_service_create(const zms_http_service_opts *opts)
{
    if (!opts || !opts->poller_pool) {
        return NULL;
    }
    zms_session_dispatch_register_all();
    zms_http_service *srv = (zms_http_service *)calloc(1, sizeof(*srv));
    if (!srv) {
        return NULL;
    }
    if (opts->api_secret && opts->api_secret[0]) {
        strncpy(srv->api_secret, opts->api_secret, sizeof(srv->api_secret) - 1);
    }
    srv->poller = opts->poller ? opts->poller : ztk_poller_pool_get(opts->poller_pool, 0);
    srv->poller_pool = opts->poller_pool;
    srv->cfg = opts->cfg;
    g_http_service_instance = srv;

#if defined(ZMS_ENABLE_WEBRTC) && ZMS_ENABLE_WEBRTC
    if (opts->cfg && opts->cfg->webrtc.enable) {
        zms_webrtc_service_opts wopts = {
            .poller = srv->poller,
            .host = opts->host && opts->host[0] ? opts->host : "0.0.0.0",
            .advertise_host =
                opts->cfg->general.extern_ip[0] ? opts->cfg->general.extern_ip : "127.0.0.1",
            .port_min = (uint16_t)(opts->cfg->webrtc.port_min ? opts->cfg->webrtc.port_min : 50000),
            .port_max = (uint16_t)(opts->cfg->webrtc.port_max ? opts->cfg->webrtc.port_max : 60000),
        };
        srv->webrtc = zms_webrtc_service_create(&wopts);
        if (!srv->webrtc) {
            ztk_warn("WebRTC service create failed");
        } else {
            ztk_info("WebRTC ICE advertise_host=%s",
                     zms_webrtc_service_advertise_host(srv->webrtc));
        }
    }
#endif

    ztk_tcp_session_ops_t ops = {on_recv, on_error, on_manager};
    ztk_tcp_server_opts_t topts = {
        .host = opts->host ? opts->host : "0.0.0.0",
        .port = opts->port ? opts->port : 8080,
        .backlog = 64,
        .poller_pool = opts->poller_pool,
        .session_ops = &ops,
        .session_create_user = session_create_user,
        .manager_interval_sec = 0.02f,
    };
    srv->tcp = ztk_tcp_server_create(&topts);
    if (!srv->tcp) {
        free(srv);
        return NULL;
    }
    return srv;
}

void zms_http_service_destroy(zms_http_service *srv)
{
    if (!srv) {
        return;
    }
#if defined(ZMS_ENABLE_WEBRTC) && ZMS_ENABLE_WEBRTC
    if (srv->webrtc) {
        zms_webrtc_service_destroy(srv->webrtc);
        srv->webrtc = NULL;
    }
#endif
    if (g_http_service_instance == srv) {
        g_http_service_instance = NULL;
    }
    ztk_tcp_server_destroy(srv->tcp);
    free(srv);
}

ztk_err_t zms_http_service_start(zms_http_service *srv)
{
    if (!srv || !srv->tcp) {
        return ZTK_ERR_INVALID;
    }
    return ztk_tcp_server_start(srv->tcp);
}

void zms_http_service_stop(zms_http_service *srv)
{
    if (srv && srv->tcp) {
        ztk_tcp_server_stop(srv->tcp);
    }
}

uint16_t zms_http_service_port(const zms_http_service *srv)
{
    if (!srv || !srv->tcp) {
        return 0;
    }
    return ztk_tcp_server_port(srv->tcp);
}
