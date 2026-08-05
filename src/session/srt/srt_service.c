/**
 * @file srt_service.c
 * @brief SRT 监听：MPEG-TS 推流写入 gop_queue。
 */
#include "srt_service_internal.h"
#include "srt_poller.h"
#include "zms/session/session_dispatcher.h"
#include "zms/session/srt/srt_service.h"
#include "ztk/util/log.h"
#include <srt/srt.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

static int g_srt_service_initialized;

static int srt_ensure_startup(void)
{
    if (g_srt_service_initialized) {
        return 0;
    }
    if (srt_startup() != 0) {
        return -1;
    }
    g_srt_service_initialized = 1;
    return 0;
}

int zms_srt_service_available(void)
{
    return 1;
}

#define ZMS_SRT_LIVE_LATENCY_MS 200
#define ZMS_SRT_LIVE_PAYLOAD 1316 /* 188*7, MPEG-TS over SRT live */
#define ZMS_SRT_RCVBUF_BYTES (24 * 1024 * 1024)

static void srt_apply_listener_opts(SRTSOCKET sock)
{
    int transtype = SRTT_LIVE;
    int latency = ZMS_SRT_LIVE_LATENCY_MS;
    int payload = ZMS_SRT_LIVE_PAYLOAD;
    int rcvbuf = ZMS_SRT_RCVBUF_BYTES;
    int tsbpd = 1;
    int tlpktdrop = 1;
    int no = 0;

    if (sock == SRT_INVALID_SOCK) {
        return;
    }
    (void)srt_setsockopt(sock, 0, SRTO_TRANSTYPE, &transtype, sizeof(transtype));
    (void)srt_setsockopt(sock, 0, SRTO_LATENCY, &latency, sizeof(latency));
    (void)srt_setsockopt(sock, 0, SRTO_TSBPDMODE, &tsbpd, sizeof(tsbpd));
    (void)srt_setsockopt(sock, 0, SRTO_PAYLOADSIZE, &payload, sizeof(payload));
    (void)srt_setsockopt(sock, 0, SRTO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    (void)srt_setsockopt(sock, 0, SRTO_TLPKTDROP, &tlpktdrop, sizeof(tlpktdrop));
    (void)srt_setsockopt(sock, 0, SRTO_RCVSYN, &no, sizeof(no));
}

void zms_srt_apply_session_opts(SRTSOCKET sock)
{
    int payload = ZMS_SRT_LIVE_PAYLOAD;
    int rcvbuf = ZMS_SRT_RCVBUF_BYTES;
    int tlpktdrop = 1;
    int no = 0;

    if (sock == SRT_INVALID_SOCK) {
        return;
    }
    /* 握手后勿改 TSBPD/latency（会破坏投递）。 */
    (void)srt_setsockopt(sock, 0, SRTO_PAYLOADSIZE, &payload, sizeof(payload));
    (void)srt_setsockopt(sock, 0, SRTO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    (void)srt_setsockopt(sock, 0, SRTO_TLPKTDROP, &tlpktdrop, sizeof(tlpktdrop));
    (void)srt_setsockopt(sock, 0, SRTO_RCVSYN, &no, sizeof(no));
    (void)srt_setsockopt(sock, 0, SRTO_SNDSYN, &no, sizeof(no));
}

static int srt_is_any_host(const char *host)
{
    char buf[64];
    size_t n;
    const char *h = host;

    if (!h || !h[0]) {
        return 1;
    }
    if (strcmp(h, "0.0.0.0") == 0 || strcmp(h, "*") == 0) {
        return 1;
    }
    n = strlen(h);
    if (n >= sizeof(buf)) {
        n = sizeof(buf) - 1;
    }
    memcpy(buf, h, n);
    buf[n] = '\0';
    while (n > 0 &&
           (buf[n - 1] == ' ' || buf[n - 1] == '\t' || buf[n - 1] == '\r' || buf[n - 1] == '\n')) {
        buf[--n] = '\0';
    }
    return !buf[0] || strcmp(buf, "0.0.0.0") == 0 || strcmp(buf, "*") == 0;
}

static int srt_parse_ipv4(const char *host, struct in_addr *out)
{
    char buf[64];
    size_t n;
    const char *h = host;

    if (!out || !h || !h[0]) {
        return -1;
    }
    n = strlen(h);
    if (n >= sizeof(buf)) {
        n = sizeof(buf) - 1;
    }
    memcpy(buf, h, n);
    buf[n] = '\0';
    while (n > 0 &&
           (buf[n - 1] == ' ' || buf[n - 1] == '\t' || buf[n - 1] == '\r' || buf[n - 1] == '\n')) {
        buf[--n] = '\0';
    }
#if defined(_WIN32)
    if (InetPtonA(AF_INET, buf, out) != 1) {
        return -1;
    }
    return 0;
#else
    return inet_pton(AF_INET, buf, out) == 1 ? 0 : -1;
#endif
}

static SRTSOCKET srt_create_listener(const char *host, uint16_t port)
{
    SRTSOCKET ls;
    struct sockaddr_in sa;
    int yes = 1;

    ls = srt_create_socket();
    if (ls == SRT_INVALID_SOCK) {
        ztk_error("SRT create_socket failed: %s", srt_getlasterror_str());
        return SRT_INVALID_SOCK;
    }

    (void)srt_setsockopt(ls, 0, SRTO_REUSEADDR, &yes, sizeof(yes));
    srt_apply_listener_opts(ls);

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    if (srt_is_any_host(host)) {
        sa.sin_addr.s_addr = INADDR_ANY;
    } else if (srt_parse_ipv4(host, &sa.sin_addr) != 0) {
        ztk_error("SRT invalid listen host: %s", host ? host : "(null)");
        srt_close(ls);
        return SRT_INVALID_SOCK;
    }

    if (srt_bind(ls, (struct sockaddr *)&sa, sizeof(sa)) == SRT_INVALID_SOCK) {
        ztk_error("SRT bind %s:%u failed: %s", host && host[0] ? host : "0.0.0.0", (unsigned)port,
                  srt_getlasterror_str());
        srt_close(ls);
        return SRT_INVALID_SOCK;
    }
    if (srt_listen(ls, 128) == SRT_INVALID_SOCK) {
        ztk_error("SRT listen %u failed: %s", (unsigned)port, srt_getlasterror_str());
        srt_close(ls);
        return SRT_INVALID_SOCK;
    }
    return ls;
}

zms_srt_service *zms_srt_service_create(const zms_srt_service_opts *opts)
{
    zms_srt_service *srv;

    if (!opts || !opts->poller) {
        return NULL;
    }
    if (srt_ensure_startup() != 0) {
        ztk_error("SRT startup failed: %s", srt_getlasterror_str());
        return NULL;
    }

    zms_session_dispatch_register_all();

    srv = (zms_srt_service *)calloc(1, sizeof(*srv));
    if (!srv) {
        return NULL;
    }
    srv->poller = opts->poller;
    srv->port = opts->port ? opts->port : 9000;
    srv->epoll_id = -1;
    srv->listen_sock = srt_create_listener(opts->host, srv->port);
    if (srv->listen_sock == SRT_INVALID_SOCK) {
        free(srv);
        return NULL;
    }
    return srv;
}

ztk_err_t zms_srt_service_start(zms_srt_service *srv)
{
    if (!srv || srv->running) {
        return ZTK_ERR_INVALID;
    }
    srv->running = 1;
    zms_srt_poller_init(srv);
    if (srv->epoll_id < 0 || !srv->io_timer) {
        srv->running = 0;
        return ZTK_ERR_STATE;
    }
    ztk_info("SRT listen 0.0.0.0:%u (poller epoll)", (unsigned)srv->port);
    return ZTK_OK;
}

void zms_srt_service_stop(zms_srt_service *srv)
{
    if (!srv || !srv->running) {
        return;
    }
    srv->running = 0;
    zms_srt_poller_fini(srv);
    if (srv->listen_sock != SRT_INVALID_SOCK) {
        srt_close(srv->listen_sock);
        srv->listen_sock = SRT_INVALID_SOCK;
    }
}

void zms_srt_service_destroy(zms_srt_service *srv)
{
    if (!srv) {
        return;
    }
    zms_srt_service_stop(srv);
    free(srv);
    if (g_srt_service_initialized) {
        srt_cleanup();
        g_srt_service_initialized = 0;
    }
}

uint16_t zms_srt_service_port(const zms_srt_service *srv)
{
    return srv ? srv->port : 0;
}
