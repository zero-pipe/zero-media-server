#include "webrtc/session/webrtc_session_internal.h"
#include "webrtc/session/webrtc_ice_internal.h"
#include "webrtc/session/webrtc_media_internal.h"
#include "zms/egress/egress_source.h"
#include "zms/engine/media/media_limits.h"
#include "zms/webrtc/webrtc_service.h"
#include "webrtc/whep/whep_play_session.h"
#include "live/publish/webrtc/webrtc_whip_ingress.h"
#include "zms/live/ingest/common/stream_ingest.h"
#include "zms/util/buf_pool.h"
#include "zms/engine/media_event.h"
#include "zms/session/session_dispatcher.h"
#include "ztk/thread/sync.h"
#include "ztk/util/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned g_webrtc_session_no;
static zms_webrtc_session *g_webrtc_sessions[ZMS_WEBRTC_SESSION_MAX];
static int g_webrtc_session_count;
static ztk_mutex *g_webrtc_session_mtx;

static void webrtc_session_registry_init(void)
{
    if (!g_webrtc_session_mtx) {
        g_webrtc_session_mtx = ztk_mutex_create(ZTK_MUTEX_NORMAL);
    }
}

static int webrtc_session_registry_full(void)
{
    return g_webrtc_session_count >= (int)ZMS_WEBRTC_SESSION_MAX;
}

static int webrtc_session_reg_add(zms_webrtc_session *s)
{
    int i;

    if (!s) {
        return -1;
    }
    webrtc_session_registry_init();
    ztk_mutex_lock(g_webrtc_session_mtx);
    for (i = 0; i < g_webrtc_session_count; ++i) {
        if (g_webrtc_sessions[i] == s) {
            ztk_mutex_unlock(g_webrtc_session_mtx);
            return 0;
        }
    }
    if (g_webrtc_session_count >= (int)ZMS_WEBRTC_SESSION_MAX) {
        ztk_warn("[webrtc] session registry full (max=%u)", (unsigned)ZMS_WEBRTC_SESSION_MAX);
        ztk_mutex_unlock(g_webrtc_session_mtx);
        return -1;
    }
    g_webrtc_sessions[g_webrtc_session_count++] = s;
    ztk_mutex_unlock(g_webrtc_session_mtx);
    return 0;
}

static void webrtc_session_reg_remove(zms_webrtc_session *s)
{
    int i;

    if (!s || !g_webrtc_session_mtx) {
        return;
    }
    ztk_mutex_lock(g_webrtc_session_mtx);
    for (i = 0; i < g_webrtc_session_count; ++i) {
        if (g_webrtc_sessions[i] == s) {
            g_webrtc_sessions[i] = g_webrtc_sessions[g_webrtc_session_count - 1];
            g_webrtc_sessions[--g_webrtc_session_count] = NULL;
            break;
        }
    }
    ztk_mutex_unlock(g_webrtc_session_mtx);
}

zms_webrtc_session *zms_webrtc_session_find(const char *id)
{
    int i;
    zms_webrtc_session *s = NULL;

    if (!id || !id[0] || !g_webrtc_session_mtx) {
        return NULL;
    }
    ztk_mutex_lock(g_webrtc_session_mtx);
    for (i = 0; i < g_webrtc_session_count; ++i) {
        if (g_webrtc_sessions[i] && strcmp(g_webrtc_sessions[i]->id, id) == 0) {
            s = g_webrtc_sessions[i];
            break;
        }
    }
    ztk_mutex_unlock(g_webrtc_session_mtx);
    return s;
}

int zms_webrtc_session_io_buf_ensure(zms_webrtc_session *s)
{
    if (!s || !s->poller) {
        return -1;
    }
    if (s->io_buf && s->io_cap >= ZMS_WEBRTC_PLAY_CRYPT_BYTES) {
        return 0;
    }
    if (!zms_buf_pool_slot_resize_poller(&s->io_buf, &s->io_cap, ZMS_WEBRTC_PLAY_CRYPT_BYTES,
                                         s->poller)) {
        return -1;
    }
    return 0;
}

void zms_webrtc_session_io_buf_release(zms_webrtc_session *s)
{
    if (!s) {
        return;
    }
    zms_buf_pool_slot_clear_poller(&s->io_buf, &s->io_cap, s->poller);
}

static void webrtc_on_udp_packet(ztk_udp_server *srv, const char *peer_ip, uint16_t peer_port,
                                 const void *data, size_t len, void *user)
{
    zms_webrtc_session *s = (zms_webrtc_session *)user;
    (void)srv;
    if (s) {
        zms_webrtc_session_on_udp(s, peer_ip, peer_port, data, len);
    }
}

static ztk_err_t webrtc_session_bind_udp(zms_webrtc_session *s)
{
    zms_webrtc_service *svc = zms_webrtc_service_instance();
    ztk_udp_server_opts_t uopts;
    static const ztk_udp_server_ops_t ops = {webrtc_on_udp_packet};
    if (!s || !s->poller) {
        return ZTK_ERR_INVALID;
    }
    memset(&uopts, 0, sizeof(uopts));
    uopts.host = zms_webrtc_service_bind_host(svc);
    uopts.port = 0;
    uopts.reuse = 1;
    uopts.poller = s->poller;
    uopts.ops = &ops;
    uopts.user = s;
    s->udp = ztk_udp_server_create(&uopts);
    if (!s->udp || ztk_udp_server_start(s->udp) != ZTK_OK) {
        return ZTK_ERR_STATE;
    }
    s->port = ztk_udp_server_port(s->udp);
    s->ice = zms_webrtc_ice_create(s);
    if (!s->ice) {
        ztk_udp_server_stop(s->udp);
        ztk_udp_server_destroy(s->udp);
        return ZTK_ERR_STATE;
    }
    return ZTK_OK;
}

static void webrtc_session_unbind_udp(zms_webrtc_session *s)
{
    if (!s) {
        return;
    }
    if (s->ice) {
        zms_webrtc_ice_destroy(s->ice);
        s->ice = NULL;
    }
    if (s->udp) {
        ztk_udp_server_stop(s->udp);
        ztk_udp_server_destroy(s->udp);
        s->udp = NULL;
    }
}

zms_webrtc_session *zms_webrtc_session_create(zms_media_source *src, const char *app,
                                              const char *stream, ztk_poller *poller)
{
    zms_webrtc_session *s;
    if (!src || !app || !stream || !poller) {
        return NULL;
    }
    webrtc_session_registry_init();
    ztk_mutex_lock(g_webrtc_session_mtx);
    if (webrtc_session_registry_full()) {
        ztk_warn("[webrtc] WHEP create rejected: session limit %u",
                 (unsigned)ZMS_WEBRTC_SESSION_MAX);
        ztk_mutex_unlock(g_webrtc_session_mtx);
        return NULL;
    }
    ztk_mutex_unlock(g_webrtc_session_mtx);
    s = (zms_webrtc_session *)calloc(1, sizeof(*s));
    if (!s) {
        return NULL;
    }
    s->poller = poller;
    s->source = src;
    strncpy(s->app, app, sizeof(s->app) - 1);
    strncpy(s->stream, stream, sizeof(s->stream) - 1);
    ++g_webrtc_session_no;
    s->session_no = g_webrtc_session_no;
    s->mode = ZMS_WEBRTC_SESSION_PLAY;
    snprintf(s->id, sizeof(s->id), "whep%u", s->session_no);
    if (webrtc_session_bind_udp(s) != ZTK_OK) {
        free(s);
        return NULL;
    }
    if (webrtc_session_reg_add(s) != 0) {
        webrtc_session_unbind_udp(s);
        free(s);
        return NULL;
    }
    ztk_info("[webrtc] session #%u id=%s app=%s stream=%s port=%u", s->session_no, s->id, app,
             stream, (unsigned)s->port);
    return s;
}

zms_webrtc_session *zms_webrtc_session_create_publish(const char *app, const char *stream,
                                                      ztk_poller *poller)
{
    zms_webrtc_session *s;
    if (!app || !stream || !poller) {
        return NULL;
    }
    webrtc_session_registry_init();
    ztk_mutex_lock(g_webrtc_session_mtx);
    if (webrtc_session_registry_full()) {
        ztk_warn("[webrtc] WHIP create rejected: session limit %u",
                 (unsigned)ZMS_WEBRTC_SESSION_MAX);
        ztk_mutex_unlock(g_webrtc_session_mtx);
        return NULL;
    }
    ztk_mutex_unlock(g_webrtc_session_mtx);
    s = (zms_webrtc_session *)calloc(1, sizeof(*s));
    if (!s) {
        return NULL;
    }
    s->poller = poller;
    s->mode = ZMS_WEBRTC_SESSION_PUBLISH;
    strncpy(s->app, app, sizeof(s->app) - 1);
    strncpy(s->stream, stream, sizeof(s->stream) - 1);
    s->ingest = zms_live_ingest_create(app, stream, NULL);
    if (!s->ingest) {
        free(s);
        return NULL;
    }
    zms_live_ingest_set_poller(s->ingest, poller);
    s->source = zms_live_ingest_source(s->ingest);
    ++g_webrtc_session_no;
    s->session_no = g_webrtc_session_no;
    snprintf(s->id, sizeof(s->id), "whip%u", s->session_no);
    if (webrtc_session_bind_udp(s) != ZTK_OK) {
        zms_live_ingest_destroy(s->ingest);
        free(s);
        return NULL;
    }
    if (webrtc_session_reg_add(s) != 0) {
        webrtc_session_unbind_udp(s);
        zms_live_ingest_destroy(s->ingest);
        free(s);
        return NULL;
    }
    ztk_info("[webrtc] WHIP session #%u id=%s app=%s stream=%s port=%u", s->session_no, s->id, app,
             stream, (unsigned)s->port);
    return s;
}

void zms_webrtc_session_teardown(zms_webrtc_session *s)
{
    zms_media_source *src;

    if (!s) {
        return;
    }
    if (s->mode == ZMS_WEBRTC_SESSION_PUBLISH) {
        return;
    }
    zms_session_detach_play(ZMS_SESSION_WEBRTC, &s->play);
    src = s->source;
    if (s->play_reader_attached && src) {
        zms_media_source_reader_remove(src);
        zms_media_event_stop(src, "webrtc", s->play_start_ms);
        s->play_reader_attached = 0;
        s->play_start_ms = 0;
    }
}

void zms_webrtc_session_destroy(zms_webrtc_session *s)
{
    if (!s) {
        return;
    }
    webrtc_session_reg_remove(s);
    if (s->mode == ZMS_WEBRTC_SESSION_PUBLISH) {
        zms_webrtc_whip_ingress_stop(s);
    } else {
        zms_webrtc_session_teardown(s);
        zms_webrtc_play_stop(s);
        if (s->play.readers.gop && s->source) {
            zms_egress_source_close(&s->play);
        }
    }
    webrtc_session_unbind_udp(s);
    zms_webrtc_session_io_buf_release(s);
    free(s);
}

void zms_webrtc_session_destroy_all(void)
{
    zms_webrtc_session *pending[ZMS_WEBRTC_SESSION_MAX];
    int n = 0;
    int i;

    webrtc_session_registry_init();
    ztk_mutex_lock(g_webrtc_session_mtx);
    for (i = 0; i < g_webrtc_session_count; ++i) {
        pending[n++] = g_webrtc_sessions[i];
    }
    g_webrtc_session_count = 0;
    memset(g_webrtc_sessions, 0, sizeof(g_webrtc_sessions));
    ztk_mutex_unlock(g_webrtc_session_mtx);
    for (i = 0; i < n; ++i) {
        zms_webrtc_session_destroy(pending[i]);
    }
}

void zms_webrtc_session_on_udp(zms_webrtc_session *s, const char *peer_ip, uint16_t peer_port,
                               const void *data, size_t len)
{
    if (!s || !data || len == 0) {
        return;
    }
    if (s->ice) {
        zms_webrtc_ice_on_udp(s->ice, peer_ip, peer_port, data, len);
        return;
    }
    if (peer_ip && peer_ip[0]) {
        strncpy(s->peer_ip, peer_ip, sizeof(s->peer_ip) - 1);
        s->peer_port = peer_port;
        s->peer_known = 1;
    }
    if (s->mode == ZMS_WEBRTC_SESSION_PUBLISH) {
        zms_webrtc_whip_ingress_on_udp(s, data, len);
        return;
    }
    if (!s->dtls_ready) {
        (void)zms_webrtc_play_on_stun_dtls(s, data, len);
        return;
    }
    zms_webrtc_play_input(s, data, len);
}

void zms_webrtc_session_try_dtls_client(zms_webrtc_session *s)
{
#if defined(ZMS_HAVE_WEBRTC_DTLS) && ZMS_HAVE_WEBRTC_DTLS
    uint8_t out[ZMS_WEBRTC_PLAY_DTLS_IO];
    size_t out_len;
    int st;
    if (!s || !s->dtls_as_client || s->dtls_ready || !s->peer_known) {
        return;
    }
    if (!s->dtls) {
        s->dtls = zms_webrtc_dtls_create_client();
    }
    if (!s->dtls) {
        return;
    }
    st = zms_webrtc_dtls_kick(s->dtls, out, sizeof(out), &out_len);
    if (st < 0) {
        ztk_warn("[webrtc] DTLS client kick failed id=%s", s->id);
        return;
    }
    if (out_len > 0) {
        ztk_info("[webrtc] DTLS ClientHello id=%s peer=%s:%u bytes=%zu", s->id, s->peer_ip,
                 (unsigned)s->peer_port, out_len);
        zms_webrtc_session_send_udp(s, out, out_len);
    }
    if (st == 1 && !s->dtls_ready) {
        s->dtls_ready = 1;
    }
#else
    (void)s;
#endif
}
