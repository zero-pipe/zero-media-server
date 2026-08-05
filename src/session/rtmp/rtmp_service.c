#include "zms/session/rtmp/rtmp_service.h"
#include "zms/session/rtmp/rtmp_session.h"
#include "zms/session/session_dispatcher.h"
#include "session/rtmp/rtmp_session_internal.h"
#include "ztk/net/tcp_server.h"
#include "ztk/poller/poller_pool.h"
#include <stdlib.h>

struct zms_rtmp_service {
    ztk_tcp_server *tcp;
    ztk_poller *poller;
};

static void *session_create_user(ztk_tcp_server *srv, ztk_tcp_session *session)
{
    (void)srv;
    zms_rtmp_session_opts opts = {.tcp = session};
    return zms_rtmp_session_create(&opts);
}

static void on_recv(ztk_tcp_session *session, const void *data, size_t len, void *user)
{
    zms_rtmp_session *s = (zms_rtmp_session *)user;
    if (!s || s->destroy_scheduled) {
        return;
    }
    zms_rtmp_session_on_recv(s, data, len);
}

static void on_error(ztk_tcp_session *session, void *user)
{
    zms_rtmp_session *s = (zms_rtmp_session *)user;
    if (s) {
        zms_rtmp_session_schedule_destroy(s, session);
    }
}

static void on_manager(ztk_tcp_session *session, void *user)
{
    (void)session;
    zms_rtmp_session *s = (zms_rtmp_session *)user;
    if (!s || s->destroy_scheduled) {
        return;
    }
    zms_rtmp_session_on_manager(s);
}

zms_rtmp_service *zms_rtmp_service_create(const zms_rtmp_service_opts *opts)
{
    if (!opts || !opts->poller_pool) {
        return NULL;
    }
    zms_session_dispatch_register_all();
    zms_rtmp_service *srv = (zms_rtmp_service *)calloc(1, sizeof(*srv));
    if (!srv) {
        return NULL;
    }
    srv->poller = opts->poller ? opts->poller : ztk_poller_pool_get(opts->poller_pool, 0);

    ztk_tcp_session_ops_t ops = {on_recv, on_error, on_manager};
    ztk_tcp_server_opts_t topts = {
        .host = opts->host ? opts->host : "0.0.0.0",
        .port = opts->port ? opts->port : 1935,
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

void zms_rtmp_service_destroy(zms_rtmp_service *srv)
{
    if (!srv) {
        return;
    }
    ztk_tcp_server_destroy(srv->tcp);
    free(srv);
}

ztk_err_t zms_rtmp_service_start(zms_rtmp_service *srv)
{
    if (!srv || !srv->tcp) {
        return ZTK_ERR_INVALID;
    }
    return ztk_tcp_server_start(srv->tcp);
}

void zms_rtmp_service_stop(zms_rtmp_service *srv)
{
    if (srv && srv->tcp) {
        ztk_tcp_server_stop(srv->tcp);
    }
}

uint16_t zms_rtmp_service_port(const zms_rtmp_service *srv)
{
    if (!srv || !srv->tcp) {
        return 0;
    }
    return ztk_tcp_server_port(srv->tcp);
}
