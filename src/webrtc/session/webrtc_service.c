#include "zms/webrtc/webrtc_service.h"
#include "session/http/http_session_internal.h"
#include "zms/engine/media/media_limits.h"
#include "ztk/net/tcp_server.h"
#include "webrtc/session/webrtc_media_internal.h"
#include "webrtc/session/webrtc_ice_internal.h"
#include "webrtc/session/webrtc_session_internal.h"
#include "ztk/util/log.h"
#include <stdlib.h>
#include <string.h>
struct zms_webrtc_service {
    char host[64];
    char advertise_host[64];
    ztk_poller *poller;
    uint16_t port_min;
    uint16_t port_max;
};

static zms_webrtc_service *g_webrtc_service;
zms_webrtc_service *zms_webrtc_service_instance(void)
{
    return g_webrtc_service;
}

void zms_webrtc_service_set_instance(zms_webrtc_service *srv)
{
    g_webrtc_service = srv;
}

const char *zms_webrtc_service_bind_host(const zms_webrtc_service *srv)
{
    return (srv && srv->host[0]) ? srv->host : "0.0.0.0";
}

const char *zms_webrtc_service_advertise_host(const zms_webrtc_service *srv)
{
    if (srv && srv->advertise_host[0]) {
        return srv->advertise_host;
    }
    return "127.0.0.1";
}

ztk_poller *zms_webrtc_service_poller(const zms_webrtc_service *srv)
{
    return srv ? srv->poller : NULL;
}

ztk_poller *zms_webrtc_service_resolve_poller(zms_http_session *hs)
{
    zms_webrtc_service *svc = zms_webrtc_service_instance();
    if (hs && hs->tcp) {
        return ztk_tcp_session_poller(hs->tcp);
    }
    if (svc && svc->poller) {
        return svc->poller;
    }
    if (hs && hs->server) {
        return hs->server->poller;
    }
    return NULL;
}

zms_webrtc_service *zms_webrtc_service_create(const zms_webrtc_service_opts *opts)
{
    zms_webrtc_service *srv;
    if (!opts || !opts->poller) {
        return NULL;
    }
    srv = (zms_webrtc_service *)calloc(1, sizeof(*srv));
    if (!srv) {
        return NULL;
    }
    srv->poller = opts->poller;
    srv->port_min = opts->port_min ? opts->port_min : 50000;
    srv->port_max = opts->port_max ? opts->port_max : 60000;
    if (opts->host && opts->host[0]) {
        strncpy(srv->host, opts->host, sizeof(srv->host) - 1);
    } else {
        strncpy(srv->host, "0.0.0.0", sizeof(srv->host) - 1);
    }
    if (opts->advertise_host && opts->advertise_host[0]) {
        strncpy(srv->advertise_host, opts->advertise_host, sizeof(srv->advertise_host) - 1);
    } else {
        strncpy(srv->advertise_host, "127.0.0.1", sizeof(srv->advertise_host) - 1);
    }
    int libice_on;
    (void)zms_webrtc_dtls_global_init();
    zms_webrtc_ice_port_init(opts->poller);
#if defined(ZMS_WEBRTC_USE_LIBICE) && ZMS_WEBRTC_USE_LIBICE
    libice_on = 1;
#else
    libice_on = 0;
#endif
    ztk_info("[webrtc] play egress build=20250628 poller=http_session rtp_flush=%u libice=%d",
             (unsigned)ZMS_WEBRTC_PLAY_RTP_FLUSH, libice_on);
    g_webrtc_service = srv;
    return srv;
}

void zms_webrtc_service_destroy(zms_webrtc_service *srv)
{
    if (!srv) {
        return;
    }
    zms_webrtc_session_destroy_all();
    if (g_webrtc_service == srv) {
        g_webrtc_service = NULL;
    }
    zms_webrtc_ice_port_fini();
    free(srv);
    if (!g_webrtc_service) {
        zms_webrtc_dtls_global_fini();
    }
}
