#include "session/http/http_session_internal.h"
#include "zms/live/play/http_flv/flv_live_muxer.h"
#include "zms/egress/mpegts/mpegts_egress.h"
#include "zms/vod/play/vod_flv_muxer.h"
#include "zms/session/play_binding.h"
#include "ztk/platform.h"
#include "zms/vod/play/vod_play_lane.h"
#include "zms/session/codec_filter.h"
#include "zms/session/session_dispatcher.h"
#include "zms/ops/api/webhook/webhook_client.h"
#include "zms/engine/media_event.h"
#include "zms/session/http/websocket/websocket_framer.h"
#include "zms/session/http/websocket/websocket_handshake.h"
#include "ztk/net/tcp_server.h"
#include "ztk/util/buf.h"
#include "ztk/util/log.h"
#include <stdio.h>
#include <string.h>
#define ZMS_HTTP_TS_SEND_PAYLOAD 1316
static int http_stream_send_payload(zms_http_session *hs, const void *data, size_t len)
{
    if (!hs || !hs->tcp || !data || len == 0) {
        return -1;
    }
    if (hs->ws_mode) {
        uint8_t whdr[10];
        size_t whlen = zms_ws_framer_server_binary_hdr(whdr, sizeof(whdr), len);
        if (whlen == 0 || ztk_tcp_session_send(hs->tcp, whdr, whlen) != ZTK_OK) {
            return -1;
        }
    }
    return ztk_tcp_session_send(hs->tcp, data, len) == ZTK_OK ? 0 : -1;
}

static int http_flv_send_mux_chunk(zms_http_session *hs,
                                   int (*mux_next)(void *, uint8_t *, size_t, size_t *),
                                   void *muxer)
{
    ztk_buf *buf;
    size_t n = 0;
    int r;
    ztk_poller *pol;
    if (!hs || !hs->tcp || !mux_next || !muxer) {
        return 0;
    }
    pol = ztk_tcp_session_poller(hs->tcp);
    buf = ztk_buf_alloc_local(pol, hs->send_cap);
    if (!buf) {
        return -1;
    }
    r = mux_next(muxer, (uint8_t *)ztk_buf_data(buf), ztk_buf_cap(buf), &n);
    if (r <= 0 || n == 0) {
        ztk_buf_unref(buf);
        return r;
    }
    ztk_buf_set_len(buf, n);
    if (http_stream_send_payload(hs, ztk_buf_data(buf), n) != 0) {
        ztk_buf_unref(buf);
        return -1;
    }
    ztk_buf_unref(buf);
    return r;
}

static int http_ts_send_mux_chunk(zms_http_session *hs, zms_mpegts_egress *muxer)
{
    uint8_t buf[ZMS_HTTP_TS_SEND_PAYLOAD];
    size_t n = 0;
    int r;
    if (!hs || !hs->tcp || !muxer) {
        return 0;
    }
    r = zms_mpegts_egress_next(muxer, buf, sizeof(buf), &n);
    if (r <= 0 || n == 0) {
        return r;
    }
    if (http_stream_send_payload(hs, buf, n) != 0) {
        return -1;
    }
    return r;
}

static int http_flv_mux_next(void *muxer, uint8_t *out, size_t cap, size_t *out_len)
{
    return zms_flv_live_muxer_next((zms_flv_live_muxer *)muxer, out, cap, out_len);
}

static ztk_err_t http_flv_live_start_hdr(void *muxer, int has_audio, int has_video, uint8_t *out,
                                         size_t cap, size_t *out_len)
{
    return zms_flv_live_muxer_start((zms_flv_live_muxer *)muxer, has_audio, has_video, out, cap,
                                    out_len);
}

void zms_http_flv_emit_header_and_stream(zms_http_session *hs, int has_audio, int has_video,
                                         zms_flv_start_hdr_fn start, void *muxer)
{
    ztk_poller *pol;
    ztk_buf *hdr;
    size_t flv_len = 0;
    if (!hs || !hs->tcp || !start || !muxer) {
        return;
    }
    pol = ztk_tcp_session_poller(hs->tcp);
    hdr = ztk_buf_alloc_local(pol, hs->send_cap);
    if (hdr &&
        start(muxer, has_audio, has_video, (uint8_t *)ztk_buf_data(hdr), ztk_buf_cap(hdr),
              &flv_len) == ZTK_OK &&
        flv_len > 0) {
        ztk_buf_set_len(hdr, flv_len);
        (void)http_stream_send_payload(hs, ztk_buf_data(hdr), flv_len);
        ztk_buf_unref(hdr);
    } else if (hdr) {
        ztk_buf_unref(hdr);
    }
    ztk_tcp_session_flush(hs->tcp);
    hs->state =
        hs->ws_mode ? ZMS_HTTP_SESSION_STATE_WS_STREAMING : ZMS_HTTP_SESSION_STATE_STREAMING;
    zms_http_session_stream_flush(hs);
}

static int http_flv_vod_mux_next(void *muxer, uint8_t *out, size_t cap, size_t *out_len)
{
    return zms_vod_flv_muxer_next((zms_vod_flv_muxer *)muxer, out, cap, out_len);
}

void zms_http_session_stream_flush(zms_http_session *hs)
{
    if (!hs || (hs->state != ZMS_HTTP_SESSION_STATE_STREAMING &&
                hs->state != ZMS_HTTP_SESSION_STATE_WS_STREAMING)) {
        return;
    }
    if (!hs->live_muxer && !hs->vod_muxer && !hs->ts_muxer) {
        return;
    }
    if (hs->vod_lane) {
        zms_play_binding bind;

        memset(&bind, 0, sizeof(bind));
        bind.vod_lane = &hs->vod_lane;
        zms_play_binding_demux_fill(&bind, 32);
    }
    for (int i = 0; i < 24; ++i) {
        int r;
        if (hs->tcp && ztk_tcp_session_out_pending(hs->tcp) > 128 * 1024) {
            break;
        }
        if (hs->tcp) {
            ztk_tcp_session_flush(hs->tcp);
        }
        if (hs->ts_muxer) {
            r = http_ts_send_mux_chunk(hs, hs->ts_muxer);
        } else if (hs->live_muxer) {
            r = http_flv_send_mux_chunk(hs, http_flv_mux_next, hs->live_muxer);
        } else {
            r = http_flv_send_mux_chunk(hs, http_flv_vod_mux_next, hs->vod_muxer);
        }
        if (r <= 0) {
            break;
        }
    }
    if (hs->tcp) {
        ztk_tcp_session_flush(hs->tcp);
    }
}

void zms_http_live_flv_start(zms_http_session *hs, zms_media_source *src, const char *app,
                             const char *stream, const char *ws_key)
{
    char hdr[512];
    int n;
    const char *player = ws_key && ws_key[0] ? "ws-flv" : "http-flv";
    if (!hs || !src || !src->gop_queue) {
        zms_http_response_send_error(hs, 404, "Not Found");
        return;
    }
    if (zms_session_capability_check_source(ZMS_PROTO_CAP_HTTP_FLV_PLAY, src) != ZTK_OK) {
        zms_session_capability_log_reject(player, src, ZMS_PROTO_CAP_HTTP_FLV_PLAY);
        zms_http_response_send_error(hs, 406, "Not Acceptable");
        return;
    }
    {
        zms_media_tuple tuple;
        zms_media_tuple_from_source(src, &tuple);
        if (!zms_webhook_allow_play(&tuple, player, hs->tcp, NULL)) {
            zms_http_response_send_error(hs, 403, "Forbidden");
            return;
        }
    }
    hs->live_muxer = zms_flv_live_muxer_create(src);
    if (!hs->live_muxer) {
        zms_http_response_send_error(hs, 500, "Internal Server Error");
        return;
    }
    if (hs->tcp) {
        zms_flv_live_muxer_bind_poller(hs->live_muxer, ztk_tcp_session_poller(hs->tcp));
    }
    zms_flv_live_muxer_bind_source(hs->live_muxer, src);
    hs->source = src;
    ztk_debug("%s live play: app=%s stream=%s video=%d audio=%d", player, app, stream,
              src->has_video, src->has_audio);
    zms_media_source_reader_add(src);
    hs->play_start_ms = ztk_monotonic_ms();
    zms_media_event_play(src, player);
    hs->reader_attached = 1;
    hs->play_event = player;
    if (ws_key && ws_key[0]) {
        char accept[32];
        if (zms_ws_handshake_accept(ws_key, accept, sizeof(accept)) != 0) {
            zms_http_response_send_error(hs, 400, "Bad Request");
            return;
        }
        n = (int)zms_ws_handshake_response(hdr, sizeof(hdr), accept);
        if (n <= 0) {
            zms_http_response_send_error(hs, 500, "Internal Server Error");
            return;
        }
        hs->ws_mode = 1;
    } else {
        n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 200 OK\r\n"
                     "Connection: keep-alive\r\n"
                     "Content-Type: video/x-flv\r\n"
                     "Access-Control-Allow-Origin: *\r\n\r\n");
    }
    ztk_tcp_session_send(hs->tcp, hdr, (size_t)n);
    zms_http_flv_emit_header_and_stream(hs, src->has_audio, src->has_video, http_flv_live_start_hdr,
                                        hs->live_muxer);
}

void zms_http_live_ts_start(zms_http_session *hs, zms_media_source *src, const char *app,
                            const char *stream)
{
    char hdr[384];
    int n;
    zms_session_play_opts pcfg;
    if (!hs || !src || !src->gop_queue) {
        zms_http_response_send_error(hs, 404, "Not Found");
        return;
    }
    if (zms_session_capability_check_source(ZMS_PROTO_CAP_HTTP_TS_PLAY, src) != ZTK_OK) {
        zms_session_capability_log_reject("http-ts", src, ZMS_PROTO_CAP_HTTP_TS_PLAY);
        zms_http_response_send_error(hs, 406, "Not Acceptable");
        return;
    }
    {
        zms_media_tuple tuple;
        zms_media_tuple_from_source(src, &tuple);
        if (!zms_webhook_allow_play(&tuple, "http-ts", hs->tcp, NULL)) {
            zms_http_response_send_error(hs, 403, "Forbidden");
            return;
        }
    }
    zms_session_dispatch_register_all();
    zms_egress_source_init(&hs->play);
    memset(&pcfg, 0, sizeof(pcfg));
    pcfg.player = ZMS_SESSION_HTTP_TS;
    if (zms_session_attach_play(ZMS_SESSION_HTTP_TS, &hs->play, src, &pcfg) != ZTK_OK) {
        zms_http_response_send_error(hs, 500, "Internal Server Error");
        return;
    }
    hs->ts_muxer = zms_mpegts_egress_create(src, &hs->play);
    if (!hs->ts_muxer) {
        zms_session_detach_play(ZMS_SESSION_HTTP_TS, &hs->play);
        zms_http_response_send_error(hs, 500, "Internal Server Error");
        return;
    }
    hs->source = src;
    ztk_debug("HTTP-TS live play: app=%s stream=%s video=%d audio=%d", app, stream, src->has_video,
              src->has_audio);
    zms_media_source_reader_add(src);
    hs->play_start_ms = ztk_monotonic_ms();
    zms_media_event_play(src, "http-ts");
    hs->reader_attached = 1;
    hs->play_event = "http-ts";
    n = snprintf(hdr, sizeof(hdr),
                 "HTTP/1.1 200 OK\r\n"
                 "Connection: keep-alive\r\n"
                 "Content-Type: video/mp2t\r\n"
                 "Access-Control-Allow-Origin: *\r\n\r\n");
    ztk_tcp_session_send(hs->tcp, hdr, (size_t)n);
    hs->state = ZMS_HTTP_SESSION_STATE_STREAMING;
    zms_http_session_stream_flush(hs);
}
