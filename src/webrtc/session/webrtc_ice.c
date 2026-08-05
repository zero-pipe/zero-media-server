#if defined(ZMS_WEBRTC_USE_LIBICE) && ZMS_WEBRTC_USE_LIBICE

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#define webrtc_strcasecmp _stricmp
#else
#include <arpa/inet.h>
#include <strings.h>
#define webrtc_strcasecmp strcasecmp
#endif

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "webrtc/session/webrtc_ice_internal.h"
#include "webrtc/session/webrtc_media_internal.h"
#include "zms/engine/media/media_limits.h"
#include "webrtc/whep/whep_play_session.h"
#include "live/publish/webrtc/webrtc_whip_ingress.h"
#include "zms/media/wire/rtp_packet.h"
#include "ice-agent.h"
#include "stun-agent.h"
#include "sockutil.h"
#include "stun-internal.h"
#include "list.h"
#include "ztk/poller/poller.h"
#include "ztk/util/log.h"

typedef struct {
    struct ice_agent_t *agent;
    ztk_poller *poller;
} webrtc_ice_agent_pol;

static webrtc_ice_agent_pol g_ice_agent_pol[ZMS_WEBRTC_ICE_AGENT_MAX];
static int g_ice_agent_pol_n;

static void webrtc_ice_agent_pol_register(struct ice_agent_t *agent, ztk_poller *pol)
{
    int i;

    if (!agent || !pol) {
        return;
    }
    for (i = 0; i < g_ice_agent_pol_n; ++i) {
        if (g_ice_agent_pol[i].agent == agent) {
            g_ice_agent_pol[i].poller = pol;
            return;
        }
    }
    if (g_ice_agent_pol_n < (int)ZMS_WEBRTC_ICE_AGENT_MAX) {
        g_ice_agent_pol[g_ice_agent_pol_n++] = (webrtc_ice_agent_pol){agent, pol};
    } else {
        ztk_warn("[webrtc] ICE agent poller map full (max=%u)", (unsigned)ZMS_WEBRTC_ICE_AGENT_MAX);
    }
}

static void webrtc_ice_agent_pol_unregister(struct ice_agent_t *agent)
{
    int i;

    if (!agent) {
        return;
    }
    for (i = 0; i < g_ice_agent_pol_n; ++i) {
        if (g_ice_agent_pol[i].agent == agent) {
            g_ice_agent_pol[i] = g_ice_agent_pol[--g_ice_agent_pol_n];
            return;
        }
    }
}

static ztk_poller *webrtc_ice_agent_pol_lookup(struct ice_agent_t *agent)
{
    int i;

    if (!agent) {
        return NULL;
    }
    for (i = 0; i < g_ice_agent_pol_n; ++i) {
        if (g_ice_agent_pol[i].agent == agent) {
            return g_ice_agent_pol[i].poller;
        }
    }
    return NULL;
}

struct webrtc_ice_checklist_view {
    /* 必须与 libice ice-checklist.c 中 ice_checklist_t 头部一致 */
    int32_t ref;
    locker_t locker;
    struct ice_agent_t *ice;
};

static struct ice_agent_t *webrtc_ice_agent_from_checklist_ptr(void *checklist)
{
    struct webrtc_ice_checklist_view *l = (struct webrtc_ice_checklist_view *)checklist;
    ztk_poller *pol;

    if (!l || !l->ice) {
        return NULL;
    }
    /* 仅当 ice 已注册到 poller 表时才采信，避免野指针 */
    pol = webrtc_ice_agent_pol_lookup(l->ice);
    if (!pol) {
        return NULL;
    }
    return l->ice;
}

ztk_poller *zms_webrtc_ice_timer_poller(void *param)
{
    struct ice_agent_t *agent;

    if (!param) {
        return NULL;
    }
    /*
     * ice_checklist_start 传入的是 ice_checklist_t*。
     * 旧逻辑把 param 强转 stun_request_t，再解引用 req->param，
     * 在 Windows 上会偶发 0xc0000005（WHEP SDP ready 后立刻 FAULT）。
     * 这里只按 checklist 布局取 ice，lookup 失败则回退 g_ice_poller。
     */
    agent = webrtc_ice_agent_from_checklist_ptr(param);
    if (agent) {
        return webrtc_ice_agent_pol_lookup(agent);
    }
    return NULL;
}

struct zms_webrtc_ice {
    zms_webrtc_session *session;
    struct ice_agent_t *agent;
    struct sockaddr_storage local_addr;
    socklen_t local_len;
    int connected;
    int remote_count;
};

static int webrtc_ice_sockaddr_from_hostport(const char *ip, uint16_t port,
                                             struct sockaddr_storage *out, socklen_t *out_len)
{
    if (!ip || !out || !out_len) {
        return -1;
    }
    return socket_addr_from(out, out_len, ip, port);
}

static void webrtc_ice_peer_from_sockaddr(zms_webrtc_session *s, const struct sockaddr *sa)
{
    char ip[SOCKET_ADDRLEN];
    u_short port;

    if (!s || !sa) {
        return;
    }
    if (socket_addr_to(sa, socket_addr_len(sa), ip, &port) != 0) {
        return;
    }
    strncpy(s->peer_ip, ip, sizeof(s->peer_ip) - 1);
    s->peer_port = port;
    s->peer_known = 1;
}

static int webrtc_ice_onsend(void *param, int protocol, const struct sockaddr *local,
                             const struct sockaddr *remote, const void *data, int bytes)
{
    zms_webrtc_session *s = (zms_webrtc_session *)param;
    char ip[SOCKET_ADDRLEN];
    u_short port;

    (void)protocol;
    (void)local;
    if (!s || !remote || !data || bytes <= 0 || !s->udp) {
        return -1;
    }
    if (socket_addr_to(remote, socket_addr_len(remote), ip, &port) != 0) {
        return -1;
    }
    if (ztk_udp_server_sendto(s->udp, ip, port, data, (size_t)bytes) != ZTK_OK) {
        return -1;
    }
    return 0;
}

static void webrtc_ice_ondata(void *param, uint8_t stream, uint16_t component, const void *data,
                              int bytes)
{
    zms_webrtc_session *s = (zms_webrtc_session *)param;
    struct ice_candidate_t rc;

    (void)stream;
    (void)component;
    if (!s || !data || bytes <= 0) {
        return;
    }
    if (s->ice && s->ice->agent && ice_agent_get_remote_candidate(s->ice->agent, 0, 1, &rc) == 0) {
        webrtc_ice_peer_from_sockaddr(s, (const struct sockaddr *)&rc.addr);
    }
    if (s->mode == ZMS_WEBRTC_SESSION_PUBLISH) {
        zms_webrtc_whip_ingress_on_udp(s, data, (size_t)bytes);
        return;
    }
    if (!s->dtls_ready) {
        (void)zms_webrtc_play_on_stun_dtls(s, data, (size_t)bytes);
    } else {
        zms_webrtc_play_input(s, data, (size_t)bytes);
    }
}

static void webrtc_ice_onconnected(void *param, uint64_t flags, uint64_t mask)
{
    zms_webrtc_session *s = (zms_webrtc_session *)param;
    struct ice_candidate_t rc;

    (void)mask;
    if (!s || !s->ice) {
        return;
    }
    if (flags & 1u) {
        if (s->ice->connected) {
            return;
        }
        s->ice->connected = 1;
        if (s->ice->agent && ice_agent_get_remote_candidate(s->ice->agent, 0, 1, &rc) == 0) {
            webrtc_ice_peer_from_sockaddr(s, (const struct sockaddr *)&rc.addr);
        }
        ztk_info("[webrtc] ICE connected id=%s peer=%s:%u", s->id, s->peer_ip,
                 (unsigned)s->peer_port);
        if (s->mode == ZMS_WEBRTC_SESSION_PUBLISH) {
            zms_webrtc_session_try_dtls_client(s);
        } else if (s->ice->agent) {
            /* Play：停 libice 定时器/检查；媒体对 peer_known 直接 sendto。 */
            (void)ice_agent_stop(s->ice->agent);
        }
    }
}

static void webrtc_ice_ongather(void *param, int code)
{
    zms_webrtc_session *s = (zms_webrtc_session *)param;
    if (s) {
        ztk_info("[webrtc] ICE gather done id=%s code=%d", s->id, code);
    }
}

static int webrtc_ice_proto_from_transport(const char *transport)
{
    if (!transport) {
        return STUN_PROTOCOL_UDP;
    }
    if (webrtc_strcasecmp(transport, "UDP") == 0) {
        return STUN_PROTOCOL_UDP;
    }
    if (webrtc_strcasecmp(transport, "TCP") == 0) {
        return STUN_PROTOCOL_TCP;
    }
    if (webrtc_strcasecmp(transport, "TLS") == 0) {
        return STUN_PROTOCOL_TLS;
    }
    if (webrtc_strcasecmp(transport, "DTLS") == 0) {
        return STUN_PROTOCOL_DTLS;
    }
    return STUN_PROTOCOL_UDP;
}

static enum ice_candidate_type_t webrtc_ice_type_from_name(const char *name)
{
    if (!name) {
        return ICE_CANDIDATE_HOST;
    }
    if (strcmp(name, "host") == 0) {
        return ICE_CANDIDATE_HOST;
    }
    if (strcmp(name, "srflx") == 0) {
        return ICE_CANDIDATE_SERVER_REFLEXIVE;
    }
    if (strcmp(name, "prflx") == 0) {
        return ICE_CANDIDATE_PEER_REFLEXIVE;
    }
    if (strcmp(name, "relay") == 0) {
        return ICE_CANDIDATE_RELAYED;
    }
    return ICE_CANDIDATE_HOST;
}

struct sdp_ice_candidate_t {
    char foundation[33];
    char transport[8];
    char candtype[8];
    uint16_t component;
    uint16_t port;
    uint32_t priority;
    char address[65];
};

static int webrtc_ice_parse_candidate_value(const char *value, struct sdp_ice_candidate_t *sc)
{
    char typ[16];
    unsigned component;
    unsigned port;
    unsigned priority;

    if (!value || !sc) {
        return -1;
    }
    memset(sc, 0, sizeof(*sc));
    if (sscanf(value, "%32s %u %7s %u %64s %u typ %15s", sc->foundation, &component, sc->transport,
               &priority, sc->address, &port, typ) < 7) {
        return -1;
    }
    sc->component = (uint16_t)component;
    sc->port = (uint16_t)port;
    sc->priority = priority;
    snprintf(sc->candtype, sizeof(sc->candtype), "%s", typ);
    return 0;
}

static int webrtc_ice_add_remote_candidate(struct ice_agent_t *agent,
                                           const struct sdp_ice_candidate_t *sc, uint8_t stream)
{
    struct ice_candidate_t c;
    socklen_t len;

    memset(&c, 0, sizeof(c));
    c.stream = stream;
    c.component = sc->component ? sc->component : 1;
    c.priority = sc->priority ? sc->priority : 1;
    c.protocol = webrtc_ice_proto_from_transport(sc->transport);
    c.type = webrtc_ice_type_from_name(sc->candtype);
    if (sc->foundation[0]) {
        snprintf(c.foundation, sizeof(c.foundation), "%s", sc->foundation);
    }
    if (socket_addr_from(&c.addr, &len, sc->address, sc->port) != 0) {
        return -1;
    }
    memcpy(&c.host, &c.addr, sizeof(c.host));
    return ice_agent_add_remote_candidate(agent, &c);
}

static int webrtc_ice_parse_offer_candidates(struct ice_agent_t *agent, const char *offer,
                                             size_t offer_len)
{
    const char *p = offer;
    const char *end = offer + offer_len;
    int count = 0;

    if (!agent || !offer) {
        return 0;
    }
    while (p < end) {
        const char *line = p;
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t line_len = nl ? (size_t)(nl - line) : (size_t)(end - line);

        if (line_len > 0 && line[line_len - 1] == '\r') {
            --line_len;
        }
        if (line_len > 12 && strncmp(line, "a=candidate:", 12) == 0) {
            struct sdp_ice_candidate_t sc;

            if (webrtc_ice_parse_candidate_value(line + 12, &sc) == 0 &&
                webrtc_ice_add_remote_candidate(agent, &sc, 0) == 0) {
                ++count;
            }
        }
        if (!nl) {
            break;
        }
        p = nl + 1;
    }
    return count;
}

static int webrtc_ice_add_local_host(struct zms_webrtc_ice *ctx, const char *host, uint16_t port)
{
    struct ice_candidate_t c;
    socklen_t len;

    if (!ctx || !ctx->agent || !host) {
        return -1;
    }
    memset(&c, 0, sizeof(c));
    c.stream = 0;
    c.component = 1;
    c.type = ICE_CANDIDATE_HOST;
    c.protocol = STUN_PROTOCOL_UDP;
    snprintf(c.foundation, sizeof(c.foundation), "1");
    if (socket_addr_from(&c.addr, &len, host, port) != 0) {
        return -1;
    }
    memcpy(&c.host, &c.addr, sizeof(c.host));
    ice_candidate_priority(&c);
    memcpy(&ctx->local_addr, &c.addr, sizeof(ctx->local_addr));
    ctx->local_len = len;
    return ice_agent_add_local_candidate(ctx->agent, &c);
}

struct zms_webrtc_ice *zms_webrtc_ice_create(zms_webrtc_session *session)
{
    struct zms_webrtc_ice *ctx;
    struct ice_agent_handler_t handler;

    if (!session) {
        return NULL;
    }
    ctx = (struct zms_webrtc_ice *)calloc(1, sizeof(*ctx));
    if (!ctx) {
        return NULL;
    }
    ctx->session = session;
    memset(&handler, 0, sizeof(handler));
    handler.send = webrtc_ice_onsend;
    handler.ondata = webrtc_ice_ondata;
    handler.ongather = webrtc_ice_ongather;
    handler.onconnected = webrtc_ice_onconnected;
    ctx->agent = ice_agent_create(0, &handler, session);
    if (!ctx->agent) {
        free(ctx);
        return NULL;
    }
    ice_agent_set_icelite(ctx->agent, 1);
    webrtc_ice_agent_pol_register(ctx->agent, session->poller);
    session->ice = ctx;
    return ctx;
}

void zms_webrtc_ice_destroy(struct zms_webrtc_ice *ice)
{
    if (!ice) {
        return;
    }
    if (ice->session) {
        ice->session->ice = NULL;
    }
    if (ice->agent) {
        webrtc_ice_agent_pol_unregister(ice->agent);
        ice_agent_stop(ice->agent);
        ice_agent_destroy(ice->agent);
        ice->agent = NULL;
    }
    free(ice);
}

int zms_webrtc_ice_setup(struct zms_webrtc_ice *ice, const char *offer, size_t offer_len,
                         const char *local_ufrag, const char *local_pwd, const char *remote_ufrag,
                         const char *remote_pwd, const char *advertise_host, uint16_t local_port)
{
    if (!ice || !ice->agent || !local_ufrag || !local_pwd || !remote_ufrag || !remote_pwd ||
        !advertise_host) {
        return -1;
    }
    ztk_info("[webrtc] ICE setup begin id=%s host=%s:%u", ice->session ? ice->session->id : "?",
             advertise_host, (unsigned)local_port);
    ice_agent_set_local_auth(ice->agent, local_ufrag, local_pwd);
    ice_agent_set_remote_auth(ice->agent, 0, remote_ufrag, remote_pwd);
    if (webrtc_ice_add_local_host(ice, advertise_host, local_port) != 0) {
        ztk_warn("[webrtc] ICE add local host failed id=%s host=%s:%u",
                 ice->session ? ice->session->id : "?", advertise_host, (unsigned)local_port);
        return -1;
    }
    ice->remote_count = webrtc_ice_parse_offer_candidates(ice->agent, offer, offer_len);
    if (ice_agent_start(ice->agent) != 0) {
        ztk_warn("[webrtc] ICE agent start failed id=%s remotes=%d",
                 ice->session ? ice->session->id : "?", ice->remote_count);
        return -1;
    }
    ztk_info("[webrtc] ICE setup id=%s host=%s:%u remotes=%d",
             ice->session ? ice->session->id : "?", advertise_host, (unsigned)local_port,
             ice->remote_count);
    return 0;
}

void zms_webrtc_ice_on_udp(struct zms_webrtc_ice *ice, const char *peer_ip, uint16_t peer_port,
                           const void *data, size_t len)
{
    struct sockaddr_storage remote;
    socklen_t remote_len;
    const uint8_t *pkt = (const uint8_t *)data;

    if (!ice || !ice->agent || !pkt || len == 0) {
        return;
    }
    if (peer_ip && peer_ip[0]) {
        if (webrtc_ice_sockaddr_from_hostport(peer_ip, peer_port, &remote, &remote_len) == 0) {
            webrtc_ice_peer_from_sockaddr(ice->session, (const struct sockaddr *)&remote);
        }
    }
    /* libice stun_agent 将 DTLS（content-type 204）当 STUN 丢弃；mux 到媒体路径。 */
    if (zms_webrtc_packet_is_dtls(pkt, len) || zms_webrtc_packet_is_rtp(pkt, len) ||
        zms_rtp_is_rtcp(pkt, len)) {
        webrtc_ice_ondata(ice->session, 0, 1, data, (int)len);
        return;
    }
    if (ice->local_len == 0) {
        return;
    }
    if (peer_ip && peer_ip[0] &&
        webrtc_ice_sockaddr_from_hostport(peer_ip, peer_port, &remote, &remote_len) == 0) {
        (void)ice_agent_input(ice->agent, STUN_PROTOCOL_UDP,
                              (const struct sockaddr *)&ice->local_addr,
                              (const struct sockaddr *)&remote, data, (int)len);
    }
}

int zms_webrtc_ice_send(struct zms_webrtc_ice *ice, const void *data, size_t len)
{
    if (!ice || !ice->agent || !data || len == 0) {
        return -1;
    }
    if (ice->connected && ice->session && ice->session->peer_known && ice->session->udp) {
        if (ztk_udp_server_sendto(ice->session->udp, ice->session->peer_ip, ice->session->peer_port,
                                  data, len) == ZTK_OK) {
            return 0;
        }
        /* 已连接：勿回退 ice_agent_send（libice index bug + 媒体突发避开 STUN 路径）。 */
        return -1;
    }
    if (ice_agent_send(ice->agent, 0, 1, data, (int)len) == 0) {
        return 0;
    }
    if (ice->session && ice->session->peer_known && ice->session->udp) {
        return ztk_udp_server_sendto(ice->session->udp, ice->session->peer_ip,
                                     ice->session->peer_port, data, len) == ZTK_OK
                   ? 0
                   : -1;
    }
    return -1;
}

int zms_webrtc_ice_connected(const struct zms_webrtc_ice *ice)
{
    return ice && ice->connected;
}

int zms_webrtc_session_send_udp(zms_webrtc_session *s, const void *data, size_t len)
{
    ztk_err_t err;

    if (!s || !data || len == 0) {
        return -1;
    }
    /* 媒体突发：peer 已知后始终直接 sendto（跳过 libice send 路径）。 */
    if (s->udp && s->peer_known) {
        err = ztk_udp_server_sendto(s->udp, s->peer_ip, s->peer_port, data, len);
        if (err == ZTK_OK) {
            return 0;
        }
        static unsigned fail_log;
        if (fail_log < 8) {
            ++fail_log;
            ztk_warn("[webrtc] udp sendto fail id=%s peer=%s:%u len=%zu err=%d", s->id, s->peer_ip,
                     (unsigned)s->peer_port, len, (int)err);
        }
        return -1;
    }
#if defined(ZMS_WEBRTC_USE_LIBICE) && ZMS_WEBRTC_USE_LIBICE
    if (s->ice) {
        return zms_webrtc_ice_send(s->ice, data, len) == 0 ? 0 : -1;
    }
#endif
    return -1;
}

#else /* !ZMS_WEBRTC_USE_LIBICE */

#include "webrtc/session/webrtc_ice_internal.h"
#include "webrtc/whep/whep_play_session.h"
#include "live/publish/webrtc/webrtc_whip_ingress.h"
#include "zms/media/wire/rtp_packet.h"
#include "ztk/net/udp_server.h"
#include "ztk/util/log.h"
#include <stdlib.h>
#include <string.h>

/** LAN / host-candidate WHEP：无 libice；peer 来自首 UDP，SDP 已带 host candidate。 */
struct zms_webrtc_ice {
    zms_webrtc_session *session;
    int connected;
};

void zms_webrtc_ice_port_init(ztk_poller *poller)
{
    (void)poller;
}

void zms_webrtc_ice_port_fini(void) {}

struct zms_webrtc_ice *zms_webrtc_ice_create(zms_webrtc_session *session)
{
    struct zms_webrtc_ice *ice;

    if (!session) {
        return NULL;
    }
    ice = (struct zms_webrtc_ice *)calloc(1, sizeof(*ice));
    if (ice) {
        ice->session = session;
        session->ice = ice;
    }
    return ice;
}

void zms_webrtc_ice_destroy(struct zms_webrtc_ice *ice)
{
    free(ice);
}

int zms_webrtc_ice_setup(struct zms_webrtc_ice *ice, const char *offer, size_t offer_len,
                         const char *local_ufrag, const char *local_pwd, const char *remote_ufrag,
                         const char *remote_pwd, const char *advertise_host, uint16_t local_port)
{
    (void)offer;
    (void)offer_len;
    (void)local_ufrag;
    (void)local_pwd;
    (void)remote_ufrag;
    (void)remote_pwd;
    if (!ice || !advertise_host) {
        return -1;
    }
    ztk_info("[webrtc] lite ICE setup id=%s host=%s:%u (no libice)",
             ice->session ? ice->session->id : "?", advertise_host, (unsigned)local_port);
    return 0;
}

static void webrtc_lite_ice_peer(zms_webrtc_session *s, const char *peer_ip, uint16_t peer_port,
                                 struct zms_webrtc_ice *ice)
{
    if (!s || !peer_ip || !peer_ip[0]) {
        return;
    }
    strncpy(s->peer_ip, peer_ip, sizeof(s->peer_ip) - 1);
    s->peer_port = peer_port;
    s->peer_known = 1;
    if (ice && !ice->connected) {
        ice->connected = 1;
        ztk_info("[webrtc] lite ICE connected id=%s peer=%s:%u", s->id, s->peer_ip,
                 (unsigned)s->peer_port);
    }
}

void zms_webrtc_ice_on_udp(struct zms_webrtc_ice *ice, const char *peer_ip, uint16_t peer_port,
                           const void *data, size_t len)
{
    zms_webrtc_session *s;
    const uint8_t *pkt = (const uint8_t *)data;

    if (!ice || !ice->session || !pkt || len == 0) {
        return;
    }
    s = ice->session;
    webrtc_lite_ice_peer(s, peer_ip, peer_port, ice);
    if (s->mode == ZMS_WEBRTC_SESSION_PUBLISH) {
        zms_webrtc_whip_ingress_on_udp(s, data, len);
        return;
    }
    if (!s->dtls_ready) {
        (void)zms_webrtc_play_on_stun_dtls(s, data, len);
    } else {
        zms_webrtc_play_input(s, data, len);
    }
}

int zms_webrtc_ice_send(struct zms_webrtc_ice *ice, const void *data, size_t len)
{
    zms_webrtc_session *s;

    if (!ice || !data || len == 0) {
        return -1;
    }
    s = ice->session;
    if (!s || !s->udp || !s->peer_known) {
        return -1;
    }
    return ztk_udp_server_sendto(s->udp, s->peer_ip, s->peer_port, data, len) == ZTK_OK ? 0 : -1;
}

int zms_webrtc_ice_connected(const struct zms_webrtc_ice *ice)
{
    return ice && ice->connected;
}

ztk_poller *zms_webrtc_ice_timer_poller(void *param)
{
    (void)param;
    return NULL;
}

int zms_webrtc_session_send_udp(zms_webrtc_session *s, const void *data, size_t len)
{
    ztk_err_t err;

    if (!s || !data || len == 0) {
        return -1;
    }
    if (!s->udp || !s->peer_known) {
        return -1;
    }
    err = ztk_udp_server_sendto(s->udp, s->peer_ip, s->peer_port, data, len);
    if (err == ZTK_OK) {
        return 0;
    }
    static unsigned fail_log;
    if (fail_log < 8) {
        ++fail_log;
        ztk_warn("[webrtc] udp sendto fail id=%s peer=%s:%u len=%zu err=%d", s->id, s->peer_ip,
                 (unsigned)s->peer_port, len, (int)err);
    }
    return -1;
}

#endif /* ZMS_WEBRTC_USE_LIBICE */
