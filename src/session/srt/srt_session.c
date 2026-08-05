#include "srt_poller.h"
#include "zms/session/srt/srt_streamid.h"
#include "zms/session/session_dispatcher.h"
#include "ztk/platform.h"
#include "ztk/util/log.h"
#include <stdlib.h>
#include <string.h>

static void srt_sock_nonblock(SRTSOCKET sock)
{
    int no = 0;

    if (sock == SRT_INVALID_SOCK) {
        return;
    }
    (void)srt_setsockopt(sock, 0, SRTO_RCVSYN, &no, sizeof(no));
}

static void srt_log_negotiated_opts(zms_srt_session *s)
{
    int lat = 0;
    int peer = 0;
    int plsize = 0;
    int len;

    if (!s || s->sock == SRT_INVALID_SOCK) {
        return;
    }
    len = (int)sizeof(lat);
    if (srt_getsockopt(s->sock, 0, SRTO_LATENCY, &lat, &len) != 0) {
        return;
    }
    len = (int)sizeof(peer);
    (void)srt_getsockopt(s->sock, 0, SRTO_PEERLATENCY, &peer, &len);
    len = (int)sizeof(plsize);
    (void)srt_getsockopt(s->sock, 0, SRTO_PAYLOADSIZE, &plsize, &len);
    ztk_info("SRT #%d negotiated latency=%dms peer=%dms payload=%d", s->session_no, lat, peer,
             plsize);
}

static int srt_session_setup(zms_srt_session *s, const char *streamid)
{
    char app[ZMS_APP_MAX];
    char stream[ZMS_STREAM_MAX];
    zms_srt_stream_mode mode = ZMS_SRT_MODE_PUBLISH;

    if (!s) {
        return -1;
    }
    if (zms_srt_streamid_parse(streamid, app, stream, &mode) != 0) {
        ztk_warn("SRT #%d bad streamid: %s", s->session_no, streamid ? streamid : "");
        return -1;
    }
    if (mode == ZMS_SRT_MODE_PLAY) {
        return zms_srt_session_begin_play(s, app, stream, streamid);
    }
    if (mode != ZMS_SRT_MODE_PUBLISH) {
        ztk_warn("SRT #%d invalid streamid mode: %s", s->session_no, streamid ? streamid : "");
        return -1;
    }
    return zms_srt_session_begin_publish(s, app, stream, streamid);
}

static void srt_session_finish_publish_async(void *user)
{
    zms_srt_session_finish_publish((zms_srt_session *)user);
}

static void srt_session_finish_play_async(void *user)
{
    zms_srt_session_finish_play((zms_srt_session *)user);
}

static void srt_session_destroy_task(void *user)
{
    zms_srt_session_destroy_now((zms_srt_session *)user);
}

void zms_srt_session_destroy_now(zms_srt_session *s)
{
    const zms_session_dispatch_ops *ops;
    zms_srt_service *srv;

    if (!s) {
        return;
    }
    srv = s->server;
    if (s->destroy_scheduled) {
        return;
    }
    s->destroy_scheduled = 1;
    s->stopping = 1;

    if (srv && srv->epoll_id >= 0 && s->sock != SRT_INVALID_SOCK) {
        (void)srt_epoll_remove_usock(srv->epoll_id, s->sock);
    }
    if (s->sock != SRT_INVALID_SOCK) {
        srt_close(s->sock);
        s->sock = SRT_INVALID_SOCK;
    }

    ops = zms_session_dispatch_find(ZMS_SESSION_SRT);
    if (ops && ops->on_teardown) {
        ops->on_teardown(s);
    }
    if (s->mode == ZMS_SRT_SESSION_MODE_PUBLISH) {
        zms_srt_session_teardown_publish(s);
    } else {
        zms_srt_session_teardown_play(s);
    }

    if (srv) {
        zms_srt_session_unlink(srv, s);
    }
    if (s->ingress) {
        zms_live_ingest_destroy(s->ingress);
        s->ingress = NULL;
    }
    s->source = NULL;
    s->publish_pipeline = NULL;
    free(s);
}

void zms_srt_session_schedule_destroy(zms_srt_session *sess)
{
    if (!sess || sess->destroy_scheduled) {
        return;
    }
    sess->stopping = 1;
    if (sess->poller && ztk_poller_is_current_thread(sess->poller)) {
        zms_srt_session_destroy_now(sess);
    } else if (sess->poller) {
        (void)ztk_poller_async(sess->poller, srt_session_destroy_task, sess, 0);
    } else {
        zms_srt_session_destroy_now(sess);
    }
}

void zms_srt_session_publisher_kick(void *ctx, int force)
{
    zms_srt_session *s = (zms_srt_session *)ctx;

    (void)force;
    if (!s) {
        return;
    }
    s->stopping = 1;
    if (s->sock != SRT_INVALID_SOCK) {
        srt_close(s->sock);
    }
}

zms_srt_session *zms_srt_session_accept(zms_srt_service *srv, SRTSOCKET client,
                                        const char *streamid)
{
    zms_srt_session *s;
    int events = SRT_EPOLL_IN | SRT_EPOLL_OUT | SRT_EPOLL_ERR;

    s = (zms_srt_session *)calloc(1, sizeof(*s));
    if (!s) {
        return NULL;
    }
    s->server = srv;
    s->session_no = ++srv->session_serial;
    s->sock = client;
    s->poller = srv->poller;
    zms_srt_apply_session_opts(client);
    srt_sock_nonblock(client);

    if (srt_session_setup(s, streamid) != 0) {
        free(s);
        return NULL;
    }
    if (s->mode == ZMS_SRT_SESSION_MODE_PUBLISH) {
        events = SRT_EPOLL_IN | SRT_EPOLL_ERR;
    }
    if (srt_epoll_add_usock(srv->epoll_id, client, &events) == SRT_ERROR) {
        const zms_session_dispatch_ops *ops = zms_session_dispatch_find(ZMS_SESSION_SRT);
        if (ops && ops->on_teardown) {
            ops->on_teardown(s);
        }
        if (s->mode == ZMS_SRT_SESSION_MODE_PUBLISH) {
            zms_srt_session_teardown_publish(s);
        } else {
            zms_srt_session_teardown_play(s);
        }
        if (s->ingress) {
            zms_live_ingest_destroy(s->ingress);
        }
        free(s);
        return NULL;
    }
    zms_srt_session_link(srv, s);
    srt_log_negotiated_opts(s);
    if (s->mode == ZMS_SRT_SESSION_MODE_PUBLISH) {
        zms_srt_session_drain_recv(s);
        if (s->poller && ztk_poller_is_current_thread(s->poller)) {
            zms_srt_session_finish_publish(s);
        } else if (ztk_poller_async(s->poller, srt_session_finish_publish_async, s, 0) != ZTK_OK) {
            srt_session_finish_publish_async(s);
        }
    } else {
        zms_srt_session_pump_send(s);
        if (s->poller && ztk_poller_is_current_thread(s->poller)) {
            zms_srt_session_finish_play(s);
        } else if (ztk_poller_async(s->poller, srt_session_finish_play_async, s, 0) != ZTK_OK) {
            srt_session_finish_play_async(s);
        }
    }
    return s;
}

zms_srt_session *zms_srt_session_find(zms_srt_service *srv, SRTSOCKET sock)
{
    zms_srt_session *s;

    if (!srv) {
        return NULL;
    }
    for (s = srv->sessions; s; s = s->next) {
        if (s->sock == sock) {
            return s;
        }
    }
    return NULL;
}

void zms_srt_session_link(zms_srt_service *srv, zms_srt_session *sess)
{
    if (!srv || !sess) {
        return;
    }
    sess->next = srv->sessions;
    srv->sessions = sess;
}

void zms_srt_session_unlink(zms_srt_service *srv, zms_srt_session *sess)
{
    zms_srt_session **pp;

    if (!srv || !sess) {
        return;
    }
    for (pp = &srv->sessions; *pp; pp = &(*pp)->next) {
        if (*pp == sess) {
            *pp = sess->next;
            sess->next = NULL;
            return;
        }
    }
}
