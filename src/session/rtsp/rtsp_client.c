#include "session/rtsp/rtsp_client_internal.h"
#include "zms/ops/service/zms_have_tls.h"
#include "zms/engine/media/media_limits.h"
#include "zms/session/rtsp/rtsp_digest.h"
#include "zms/session/rtsp/rtsp.h"
#include "ztk/util/timer.h"
#include "ztk/util/log.h"
#include "zms/engine/module_registry.h"
#include "zms/media/codec/payload/payload_registry.h"
#include "zms/media/wire/rtp_packet.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *state_name(zms_rtsp_client_state st)
{
    switch (st) {
    case ST_IDLE:
        return "IDLE";
    case ST_CONNECTING:
        return "CONNECTING";
    case ST_OPTIONS:
        return "OPTIONS";
    case ST_DESCRIBE:
        return "DESCRIBE";
    case ST_SETUP:
        return "SETUP";
    case ST_PLAY:
        return "PLAY";
    case ST_PLAYING:
        return "PLAYING";
    case ST_ERROR:
        return "ERROR";
    default:
        return "?";
    }
}

static ztk_err_t parse_rtsp_url(const char *url, zms_rtsp_url *out)
{
    if (!url || !out) {
        return ZTK_ERR_INVALID;
    }
    memset(out, 0, sizeof(*out));

    const char *p = url;
    if (strncmp(p, "rtsps://", 8) == 0) {
        out->use_tls = 1;
        out->scheme = "rtsps";
        out->port = 443;
        p += 8;
    } else if (strncmp(p, "rtsp://", 7) == 0) {
        out->scheme = "rtsp";
        out->port = ZMS_RTSP_DEFAULT_PORT;
        p += 7;
    } else {
        return ZTK_ERR_INVALID;
    }

    const char *slash = strchr(p, '/');
    const char *at = strrchr(p, '@');
    if (at) {
        char cred[192];
        size_t clen = (size_t)(at - p);
        if (clen >= sizeof(cred)) {
            clen = sizeof(cred) - 1;
        }
        memcpy(cred, p, clen);
        cred[clen] = '\0';
        char *colon = strchr(cred, ':');
        if (colon) {
            *colon = '\0';
            strncpy(out->user, cred, sizeof(out->user) - 1);
            strncpy(out->pass, colon + 1, sizeof(out->pass) - 1);
        } else {
            strncpy(out->user, cred, sizeof(out->user) - 1);
        }
    }
    const char *host_start = at ? at + 1 : p;
    const char *host_end = slash ? slash : p + strlen(p);

    const char *colon = NULL;
    for (const char *c = host_start; c < host_end; ++c) {
        if (*c == ':') {
            colon = c;
            break;
        }
    }

    size_t host_len = colon ? (size_t)(colon - host_start) : (size_t)(host_end - host_start);
    if (host_len == 0 || host_len >= sizeof(out->host)) {
        return ZTK_ERR_INVALID;
    }
    memcpy(out->host, host_start, host_len);
    out->host[host_len] = '\0';

    if (colon) {
        out->port = (uint16_t)atoi(colon + 1);
        if (out->port == 0) {
            out->port = out->use_tls ? 443 : ZMS_RTSP_DEFAULT_PORT;
        }
    }

    if (slash) {
        strncpy(out->path, slash, sizeof(out->path) - 1);
    } else {
        strncpy(out->path, "/", sizeof(out->path) - 1);
    }

    {
        int n = snprintf(out->full, sizeof(out->full), "%s://%s:%u%s", out->scheme, out->host,
                         out->port, out->path);
        if (n < 0 || (size_t)n >= sizeof(out->full)) {
            return ZTK_ERR_INVALID;
        }
    }
    return ZTK_OK;
}

static ztk_err_t client_io_connect(zms_rtsp_client *c)
{
    if (!c) {
        return ZTK_ERR_INVALID;
    }
#if ZMS_HAVE_PULL_TLS
    if (c->tls) {
        return ztk_tls_client_connect(c->tls, c->url.host, c->url.port);
    }
#endif
    if (c->tcp) {
        return ztk_tcp_client_connect(c->tcp, c->url.host, c->url.port);
    }
    return ZTK_ERR_STATE;
}

static int client_io_is_connected(const zms_rtsp_client *c)
{
    if (!c) {
        return 0;
    }
#if ZMS_HAVE_PULL_TLS
    if (c->tls) {
        return ztk_tls_client_is_connected(c->tls);
    }
#endif
    if (c->tcp) {
        return ztk_tcp_client_is_connected(c->tcp);
    }
    return 0;
}

static void client_io_close(zms_rtsp_client *c)
{
    if (!c) {
        return;
    }
#if ZMS_HAVE_PULL_TLS
    if (c->tls) {
        ztk_tls_client_close(c->tls);
        return;
    }
#endif
    if (c->tcp) {
        ztk_tcp_client_close(c->tcp);
    }
}

static void client_cancel_retry(zms_rtsp_client *c)
{
    if (c && c->retry_timer) {
        ztk_poller_timer_cancel(c->retry_timer);
        c->retry_timer = NULL;
    }
}

static void client_cancel_close(zms_rtsp_client *c)
{
    if (c && c->close_timer) {
        ztk_poller_timer_cancel(c->close_timer);
        c->close_timer = NULL;
    }
}

static void rtsp_client_payload_on_frame(const zms_frame *frame, void *user)
{
    zms_rtsp_client *c = (zms_rtsp_client *)user;
    if (!c || !c->opts.on_frame || !frame) {
        return;
    }
    c->opts.on_frame(frame, c->opts.user);
}

static void rtsp_client_payload_teardown(zms_rtsp_client *c)
{
    if (c) {
        zms_payload_track_bank_clear(&c->payload);
    }
}

static void client_reset_state(zms_rtsp_client *c)
{
    if (!c) {
        return;
    }
    rtsp_client_payload_teardown(c);
    rtsp_client_udp_teardown(c);
    c->state = ST_IDLE;
    c->cseq = 1;
    c->session_id[0] = '\0';
    c->content_base[0] = '\0';
    c->setup_index = 0;
    c->teardown_sent = 0;
    c->auto_udp_tried = 0;
    c->auth_retry_done = 0;
    c->digest_realm[0] = '\0';
    c->digest_nonce[0] = '\0';
    memset(&c->session, 0, sizeof(c->session));
    zms_rtsp_splitter_enable_rtp(c->splitter, 0);
    if (c->receiver) {
        zms_rtp_receiver_reset(c->receiver);
    }
}

static uint64_t client_retry_task(void *user)
{
    zms_rtsp_client *c = (zms_rtsp_client *)user;
    c->retry_timer = NULL;
    if (!c || c->stopping) {
        return 0;
    }
    ztk_debug("RTSP client retry #%d url=%s", c->failed_count + 1, c->url.full);
    client_reset_state(c);
    if (client_io_connect(c) != ZTK_OK) {
        return (uint64_t)c->reconnect_delay_ms;
    }
    c->state = ST_CONNECTING;
    return 0;
}

void rtsp_client_fail(zms_rtsp_client *c, ztk_err_t err)
{
    if (!c || c->stopping) {
        return;
    }
    ztk_warn("RTSP client fail state=%s err=%d url=%s", state_name(c->state), (int)err,
             c->url.full);
    c->state = ST_ERROR;
    if (c->opts.on_error) {
        c->opts.on_error(err, c->opts.user);
    }
    if (c->retry_count >= 0 && c->failed_count >= c->retry_count) {
        return;
    }
    ++c->failed_count;
    client_cancel_retry(c);
    c->retry_timer =
        ztk_poller_do_delay(c->opts.poller, (uint64_t)c->reconnect_delay_ms, client_retry_task, c);
}

static ztk_err_t client_send_raw(zms_rtsp_client *c, const char *data, size_t len)
{
#if ZMS_HAVE_PULL_TLS
    if (c->tls) {
        return ztk_tls_client_send(c->tls, data, len);
    }
#endif
    if (c->tcp) {
        return ztk_tcp_client_send(c->tcp, data, len);
    }
    return ZTK_ERR_STATE;
}

static const char *method_public_name(zms_rtsp_method method)
{
    switch (method) {
    case ZMS_RTSP_OPTIONS:
        return "OPTIONS";
    case ZMS_RTSP_DESCRIBE:
        return "DESCRIBE";
    case ZMS_RTSP_SETUP:
        return "SETUP";
    case ZMS_RTSP_PLAY:
        return "PLAY";
    case ZMS_RTSP_PAUSE:
        return "PAUSE";
    case ZMS_RTSP_TEARDOWN:
        return "TEARDOWN";
    case ZMS_RTSP_GET_PARAMETER:
        return "GET_PARAMETER";
    case ZMS_RTSP_SET_PARAMETER:
        return "SET_PARAMETER";
    case ZMS_RTSP_ANNOUNCE:
        return "ANNOUNCE";
    case ZMS_RTSP_RECORD:
        return "RECORD";
    default:
        return "OPTIONS";
    }
}

static void client_apply_credentials(zms_rtsp_client *c)
{
    if (!c) {
        return;
    }
    if (c->opts.username && c->opts.username[0]) {
        strncpy(c->url.user, c->opts.username, sizeof(c->url.user) - 1);
    }
    if (c->opts.password && c->opts.password[0]) {
        strncpy(c->url.pass, c->opts.password, sizeof(c->url.pass) - 1);
    }
}

static ztk_err_t client_send(zms_rtsp_client *c, zms_rtsp_method method, const char *url,
                             const char *extra, const char *body, size_t body_len)
{
    char buf[4096];
    char auth[512];
    char merged[1024];
    size_t n = 0;

    auth[0] = '\0';
    if (c->digest_realm[0] && c->digest_nonce[0] && c->url.user[0] && c->url.pass[0]) {
        (void)zms_rtsp_digest_build_authorization(method_public_name(method), url, c->url.user,
                                                  c->url.pass, c->digest_realm, c->digest_nonce,
                                                  auth, sizeof(auth));
    }

    merged[0] = '\0';
    if (extra && extra[0]) {
        strncpy(merged, extra, sizeof(merged) - 1);
    }
    if (auth[0]) {
        size_t ml = strlen(merged);
        if (ml + strlen(auth) + 1 < sizeof(merged)) {
            strcat(merged + ml, auth);
        }
    }
    const char *send_extra = merged[0] ? merged : NULL;

    ztk_err_t err = zms_rtsp_message_build_request(buf, sizeof(buf), method, url, c->cseq++,
                                                   c->session_id[0] ? c->session_id : NULL,
                                                   send_extra, body, body_len, &n);
    if (err != ZTK_OK) {
        return err;
    }
    return client_send_raw(c, buf, n);
}

static const char *setup_url(zms_rtsp_client *c, const zms_media_track *track)
{
    static char buf[ZMS_RTSP_SETUP_URL_MAX];

    buf[0] = '\0';
    if (track->control[0] == '/') {
        snprintf(buf, sizeof(buf), "rtsp://%s:%u%s", c->url.host, c->url.port, track->control);
    } else if (strstr(track->control, "://")) {
        strncpy(buf, track->control, sizeof(buf) - 1);
    } else if (c->content_base[0]) {
        snprintf(buf, sizeof(buf), "%s%s%s", c->content_base,
                 c->content_base[strlen(c->content_base) - 1] == '/' ? "" : "/", track->control);
    } else {
        snprintf(buf, sizeof(buf), "%s%s", c->url.full, track->control);
    }
    buf[sizeof(buf) - 1] = '\0';
    return buf;
}

static void send_options(zms_rtsp_client *c)
{
    c->state = ST_OPTIONS;
    ztk_debug("RTSP client -> OPTIONS %s", c->url.full);
    client_send(c, ZMS_RTSP_OPTIONS, c->url.full, NULL, NULL, 0);
}

static void send_describe(zms_rtsp_client *c)
{
    c->state = ST_DESCRIBE;
    ztk_debug("RTSP client -> DESCRIBE %s", c->url.full);
    const char *extra = "Accept: application/sdp\r\n";
    client_send(c, ZMS_RTSP_DESCRIBE, c->url.full, extra, NULL, 0);
}

static void send_setup(zms_rtsp_client *c)
{
    if (c->setup_index >= c->session.track_count) {
        c->state = ST_PLAY;
        ztk_debug("RTSP client -> PLAY %s", c->url.full);
        client_send(c, ZMS_RTSP_PLAY, c->url.full, "Range: npt=0.000-\r\n", NULL, 0);
        return;
    }

    c->state = ST_SETUP;
    const zms_media_track *track = &c->session.tracks[c->setup_index];
    char extra[256];

    if (c->rtp_mode == ZMS_RTSP_RTP_UDP) {
        if (rtsp_client_udp_prepare_track(c, c->setup_index) != ZTK_OK) {
            rtsp_client_fail(c, ZTK_ERR_IO);
            return;
        }
        if (rtsp_client_udp_build_setup_extra(c, c->setup_index, extra, sizeof(extra)) < 0) {
            rtsp_client_fail(c, ZTK_ERR_IO);
            return;
        }
    } else {
        snprintf(extra, sizeof(extra), "Transport: RTP/AVP/TCP;unicast;interleaved=%u-%u\r\n",
                 track->interleaved_rtp, track->interleaved_rtcp);
    }

    ztk_debug("RTSP client -> SETUP track=%u mode=%s", c->setup_index,
              c->rtp_mode == ZMS_RTSP_RTP_UDP ? "udp" : "tcp");
    client_send(c, ZMS_RTSP_SETUP, setup_url(c, track), extra, NULL, 0);
}

void rtsp_client_emit_rtp_frame(zms_rtsp_client *c, int track_index, const zms_rtp_packet *pkt)
{
    const zms_media_track *track;
    static int logged_track[ZMS_SDP_TRACK_MAX];
    uint32_t clock_hz;

    if (!c || !c->opts.on_frame || !pkt || track_index < 0 ||
        (unsigned)track_index >= c->session.track_count) {
        return;
    }

    track = &c->session.tracks[track_index];
    if (track->codec == ZMS_CODEC_INVALID) {
        return;
    }
    if (!zms_payload_demux_find(track->codec, ZMS_WIRE_FORMAT_RTP)) {
        return;
    }

    clock_hz = (uint32_t)(track->sample_rate > 0 ? track->sample_rate : 0);
    if (!clock_hz) {
        clock_hz = track->type == ZMS_TRACK_VIDEO ? 90000u : 44100u;
    }

    if (!logged_track[track_index]) {
        logged_track[track_index] = 1;
        ztk_debug("RTSP client first RTP via payload: track=%d codec=%s pt=%u", track_index,
                  zms_codec_name(track->codec), (unsigned)pkt->hdr.pt);
    }

    (void)zms_payload_track_bank_input_rtp(&c->payload, track_index, track->codec,
                                           ZMS_WIRE_FORMAT_RTP, clock_hz,
                                           rtsp_client_payload_on_frame, c, pkt);
}

static void on_sorted_rtp(const zms_rtp_packet *pkt, int track_index, void *user)
{
    zms_rtsp_client *c = (zms_rtsp_client *)user;
    if (!c || track_index < 0 || (unsigned)track_index >= c->session.track_count) {
        return;
    }
    rtsp_client_emit_rtp_frame(c, track_index, pkt);
}

static void on_rtp_payload(uint8_t channel, const uint8_t *data, size_t len, void *user)
{
    zms_rtsp_client *c = (zms_rtsp_client *)user;
    if (!c || !c->receiver || c->rtp_mode != ZMS_RTSP_RTP_TCP) {
        return;
    }

    if (zms_rtp_is_rtcp(data, len)) {
        return;
    }

    int track_index = -1;
    for (unsigned i = 0; i < c->session.track_count; ++i) {
        if (c->session.tracks[i].interleaved_rtp == channel) {
            track_index = (int)i;
            break;
        }
    }
    if (track_index < 0) {
        return;
    }
    zms_rtp_receiver_input(c->receiver, track_index, data, len);
}

static void retry_current_request(zms_rtsp_client *c)
{
    if (!c) {
        return;
    }
    switch (c->state) {
    case ST_OPTIONS:
        send_options(c);
        break;
    case ST_DESCRIBE:
        send_describe(c);
        break;
    case ST_SETUP:
        send_setup(c);
        break;
    case ST_PLAY:
        client_send(c, ZMS_RTSP_PLAY, c->url.full, "Range: npt=0.000-\r\n", NULL, 0);
        break;
    default:
        break;
    }
}

static int handle_auth_401(zms_rtsp_client *c, const zms_rtsp_message *msg)
{
    if (!c || c->auth_retry_done || !c->url.user[0]) {
        return 0;
    }
    const char *www = zms_rtsp_message_get(msg, "WWW-Authenticate");
    char realm[128], nonce[128];
    zms_rtsp_auth_scheme scheme =
        zms_rtsp_digest_parse_www_auth(www, realm, sizeof(realm), nonce, sizeof(nonce));
    if (scheme != ZMS_RTSP_AUTH_DIGEST || !realm[0] || !nonce[0]) {
        return 0;
    }
    strncpy(c->digest_realm, realm, sizeof(c->digest_realm) - 1);
    strncpy(c->digest_nonce, nonce, sizeof(c->digest_nonce) - 1);
    c->auth_retry_done = 1;
    ztk_debug("RTSP client 401 -> retry with Digest user=%s", c->url.user);
    retry_current_request(c);
    return 1;
}

static void rtsp_client_auto_fallback_udp(zms_rtsp_client *c)
{
    if (!c || c->auto_udp_tried) {
        return;
    }
    ztk_debug("RTSP client AUTO: TCP SETUP failed (461), fallback to UDP");
    c->auto_udp_tried = 1;
    c->rtp_mode = ZMS_RTSP_RTP_UDP;
    c->setup_index = 0;
    rtsp_client_udp_teardown(c);
    send_setup(c);
}

static void handle_response(zms_rtsp_client *c, const zms_rtsp_message *msg)
{
    if (msg->status_code == 401) {
        if (handle_auth_401(c, msg)) {
            return;
        }
        ztk_warn("RTSP client auth failed status=401 state=%s", state_name(c->state));
        rtsp_client_fail(c, ZTK_ERR_IO);
        return;
    }
    if (msg->status_code == 461 && c->requested_rtp_mode == ZMS_RTSP_RTP_AUTO &&
        c->rtp_mode == ZMS_RTSP_RTP_TCP && !c->auto_udp_tried && c->state == ST_SETUP) {
        rtsp_client_auto_fallback_udp(c);
        return;
    }
    if (msg->status_code != 200) {
        ztk_warn("RTSP client %s failed status=%d", state_name(c->state), msg->status_code);
        rtsp_client_fail(c, ZTK_ERR_IO);
        return;
    }

    const char *sid = zms_rtsp_message_get(msg, "Session");
    if (sid && sid[0]) {
        strncpy(c->session_id, sid, sizeof(c->session_id) - 1);
        char *semi = strchr(c->session_id, ';');
        if (semi) {
            *semi = '\0';
        }
    }

    const char *base = zms_rtsp_message_get(msg, "Content-Base");
    if (base && base[0]) {
        strncpy(c->content_base, base, sizeof(c->content_base) - 1);
    }

    switch (c->state) {
    case ST_OPTIONS:
        send_describe(c);
        break;
    case ST_DESCRIBE:
        if (msg->body && msg->body_len > 0) {
            if (zms_sdp_parse(msg->body, msg->body_len, &c->session) != ZTK_OK) {
                rtsp_client_fail(c, ZTK_ERR_INVALID);
                return;
            }
            ztk_debug("RTSP client DESCRIBE ok tracks=%u transport=%s", c->session.track_count,
                      c->rtp_mode == ZMS_RTSP_RTP_UDP ? "udp" : "tcp");
            for (unsigned i = 0; i < c->session.track_count; ++i) {
                if (c->opts.on_track) {
                    c->opts.on_track(&c->session.tracks[i], c->opts.user);
                }
            }
        }
        c->setup_index = 0;
        send_setup(c);
        break;
    case ST_SETUP: {
        const char *tr = zms_rtsp_message_get(msg, "Transport");
        if (tr && c->setup_index < c->session.track_count) {
            if (c->rtp_mode == ZMS_RTSP_RTP_UDP) {
                rtsp_client_udp_apply_setup_response(c, c->setup_index, tr);
            } else {
                uint8_t ich = 0, icc = 0;
                if (zms_rtsp_transport_parse_interleaved(tr, &ich, &icc) == 0) {
                    c->session.tracks[c->setup_index].interleaved_rtp = ich;
                    c->session.tracks[c->setup_index].interleaved_rtcp = icc;
                    ztk_debug("RTSP client SETUP ok track=%u interleaved=%u-%u", c->setup_index,
                              (unsigned)ich, (unsigned)icc);
                }
            }
        }
        c->setup_index++;
        send_setup(c);
        break;
    }
    case ST_PLAY:
        c->state = ST_PLAYING;
        c->failed_count = 0;
        client_cancel_retry(c);
        if (c->rtp_mode == ZMS_RTSP_RTP_TCP) {
            zms_rtsp_splitter_enable_rtp(c->splitter, 1);
        } else {
            rtsp_client_udp_on_play(c);
        }
        ztk_debug("RTSP client PLAYING: tracks=%u url=%s", c->session.track_count, c->url.full);
        if (c->opts.on_ready) {
            c->opts.on_ready(c->opts.user);
        }
        break;
    default:
        break;
    }
}

static void on_rtsp_message(const zms_rtsp_message *msg, void *user)
{
    zms_rtsp_client *c = (zms_rtsp_client *)user;
    if (!c || c->stopping) {
        return;
    }
    if (msg->is_response) {
        handle_response(c, msg);
    }
}

static void on_transport_connected(zms_rtsp_client *c)
{
    ztk_debug("RTSP client connected %s:%u tls=%d", c->url.host, (unsigned)c->url.port,
              c->url.use_tls);
    send_options(c);
}

static void on_transport_recv(zms_rtsp_client *c, const void *data, size_t len)
{
    if (!c || c->stopping) {
        return;
    }
    zms_rtsp_splitter_input(c->splitter, data, len);
}

static void on_tcp_connect(ztk_tcp_client *client, void *user)
{
    zms_rtsp_client *c = (zms_rtsp_client *)user;
    (void)client;
    on_transport_connected(c);
}

static void on_tcp_recv(ztk_tcp_client *client, const void *data, size_t len, void *user)
{
    zms_rtsp_client *c = (zms_rtsp_client *)user;
    (void)client;
    on_transport_recv(c, data, len);
}

static void on_tcp_error(ztk_tcp_client *client, void *user)
{
    zms_rtsp_client *c = (zms_rtsp_client *)user;
    (void)client;
    rtsp_client_fail(c, ZTK_ERR_IO);
}

static void on_tls_connect(ztk_tls_client *client, void *user)
{
    zms_rtsp_client *c = (zms_rtsp_client *)user;
    (void)client;
    on_transport_connected(c);
}

static void on_tls_recv(ztk_tls_client *client, const void *data, size_t len, void *user)
{
    zms_rtsp_client *c = (zms_rtsp_client *)user;
    (void)client;
    on_transport_recv(c, data, len);
}

static void on_tls_error(ztk_tls_client *client, void *user)
{
    zms_rtsp_client *c = (zms_rtsp_client *)user;
    (void)client;
    rtsp_client_fail(c, ZTK_ERR_IO);
}

static uint64_t client_close_task(void *user)
{
    zms_rtsp_client *c = (zms_rtsp_client *)user;
    c->close_timer = NULL;
    if (c) {
        client_io_close(c);
    }
    return 0;
}

zms_rtsp_client *zms_rtsp_client_create(const zms_rtsp_client_opts *opts)
{
    if (!opts || !opts->poller || !opts->url) {
        return NULL;
    }

    zms_rtsp_client *c = (zms_rtsp_client *)calloc(1, sizeof(*c));
    if (!c) {
        return NULL;
    }
    c->opts = *opts;
    c->cseq = 1;
    c->requested_rtp_mode = opts->rtp_mode;
    c->rtp_mode = (opts->rtp_mode == ZMS_RTSP_RTP_UDP) ? ZMS_RTSP_RTP_UDP : ZMS_RTSP_RTP_TCP;
    c->retry_count = opts->retry_count;
    if (c->retry_count == 0) {
        c->retry_count = -1;
    }
    c->reconnect_delay_ms = opts->reconnect_delay_ms > 0 ? opts->reconnect_delay_ms : 2000;

    if (parse_rtsp_url(opts->url, &c->url) != ZTK_OK) {
        free(c);
        return NULL;
    }
    client_apply_credentials(c);

    zms_rtsp_splitter_opts sopts = {on_rtsp_message, on_rtp_payload, c};
    c->splitter = zms_rtsp_splitter_create(&sopts);
    if (!c->splitter) {
        free(c);
        return NULL;
    }

    zms_rtp_receiver_opts ropts = {ZMS_SDP_TRACK_MAX, ZMS_RTP_JITTER_SLOTS_DEFAULT, on_sorted_rtp,
                                   c};
    c->receiver = zms_rtp_receiver_create(&ropts);

    zms_modules_register_all();

    if (c->url.use_tls) {
#if !defined(ZTK_HAVE_OPENSSL) || !ZTK_HAVE_OPENSSL
        zms_rtsp_client_destroy(c);
        return NULL;
#else
        if (!opts->ssl_ctx) {
            zms_rtsp_client_destroy(c);
            return NULL;
        }
        ztk_tls_client_ops_t tops = {on_tls_connect, on_tls_recv, on_tls_error};
        ztk_tls_client_opts_t topts = {opts->poller, opts->ssl_ctx, &tops, c, c->url.host};
        c->tls = ztk_tls_client_create(&topts);
        if (!c->tls) {
            zms_rtsp_client_destroy(c);
            return NULL;
        }
#endif
    } else {
        ztk_tcp_client_ops_t tops = {on_tcp_connect, on_tcp_recv, on_tcp_error};
        ztk_tcp_client_opts_t topts = {opts->poller, &tops, c};
        c->tcp = ztk_tcp_client_create(&topts);
        if (!c->tcp) {
            zms_rtsp_client_destroy(c);
            return NULL;
        }
    }
    if (!c->receiver) {
        zms_rtsp_client_destroy(c);
        return NULL;
    }
    ztk_debug("RTSP client create url=%s mode=%s user=%s", c->url.full,
              c->requested_rtp_mode == ZMS_RTSP_RTP_AUTO
                  ? "auto"
                  : (c->rtp_mode == ZMS_RTSP_RTP_UDP ? "udp" : "tcp"),
              c->url.user[0] ? c->url.user : "-");
    return c;
}

void zms_rtsp_client_destroy(zms_rtsp_client *c)
{
    if (!c) {
        return;
    }
    client_cancel_retry(c);
    client_cancel_close(c);
    zms_rtsp_client_stop(c);
    ztk_tcp_client_destroy(c->tcp);
#if ZMS_HAVE_PULL_TLS
    ztk_tls_client_destroy(c->tls);
#endif
    zms_rtsp_splitter_destroy(c->splitter);
    zms_rtp_receiver_destroy(c->receiver);
    rtsp_client_payload_teardown(c);
    free(c);
}

ztk_err_t zms_rtsp_client_play(zms_rtsp_client *c)
{
    if (!c) {
        return ZTK_ERR_INVALID;
    }
    c->stopping = 0;
    client_reset_state(c);
    c->state = ST_CONNECTING;
    return client_io_connect(c);
}

void zms_rtsp_client_stop(zms_rtsp_client *c)
{
    if (!c || c->stopping) {
        return;
    }
    c->stopping = 1;
    client_cancel_retry(c);
    if (c->session_id[0] && client_io_is_connected(c) && !c->teardown_sent) {
        c->teardown_sent = 1;
        client_send(c, ZMS_RTSP_TEARDOWN, c->url.full, NULL, NULL, 0);
    }
    rtsp_client_udp_teardown(c);
    client_cancel_close(c);
    c->close_timer = ztk_poller_do_delay(c->opts.poller, 80, client_close_task, c);
}

const zms_sdp_session *zms_rtsp_client_session(const zms_rtsp_client *c)
{
    return c ? &c->session : NULL;
}
