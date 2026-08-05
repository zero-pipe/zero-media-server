#include "zms/session/rtmp/rtmp_session.h"
#include "session/rtmp/rtmp_session_internal.h"
#include "zms/util/buf_pool.h"
#include "ztk/net/tcp_server.h"
#include "ztk/poller/poller.h"
#include "ztk/util/timer.h"
#include "ztk/thread/sync.h"
#include "ztk/util/log.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    zms_rtmp_session *s;
    unsigned token;
} rtmp_destroy_job;

static void zms_rtmp_session_destroy_task(void *user);

static uint64_t zms_rtmp_session_destroy_delayed(void *user)
{
    zms_rtmp_session_destroy_task(user);
    return 0;
}

static void zms_rtmp_session_destroy_task(void *user)
{
    rtmp_destroy_job *job = (rtmp_destroy_job *)user;
    zms_rtmp_session *s = job ? job->s : NULL;
    unsigned token = job ? job->token : 0;

    if (job) {
        free(job);
    }
    if (!s || s->destroy_token != token) {
        return;
    }
    zms_rtmp_session_destroy(s);
}

static unsigned g_rtmp_session_serial;

void zms_rtmp_session_lock(zms_rtmp_session *s)
{
    if (s && s->play_mtx) {
        ztk_mutex_lock(s->play_mtx);
    }
}

void zms_rtmp_session_unlock(zms_rtmp_session *s)
{
    if (s && s->play_mtx) {
        ztk_mutex_unlock(s->play_mtx);
    }
}

zms_rtmp_session *zms_rtmp_session_create(const zms_rtmp_session_opts *opts)
{
    struct rtmp_server_handler_t handler;
    zms_rtmp_session *s;

    if (!opts || !opts->tcp) {
        return NULL;
    }

    s = (zms_rtmp_session *)calloc(1, sizeof(*s));
    if (!s) {
        return NULL;
    }

    s->tcp = opts->tcp;
    s->session_no = ++g_rtmp_session_serial;
    s->play_mtx = ztk_mutex_create(ZTK_MUTEX_NORMAL);

    memset(&handler, 0, sizeof(handler));
    handler.send = rtmp_srv_send;
    handler.onpublish = rtmp_srv_onpublish;
    handler.onplay = rtmp_srv_onplay;
    handler.onpause = rtmp_srv_onpause;
    handler.onseek = rtmp_srv_onseek;
    handler.ongetduration = rtmp_srv_ongetduration;
    handler.onvideo = rtmp_srv_onvideo;
    handler.onaudio = rtmp_srv_onaudio;
    handler.onscript = rtmp_srv_onscript;

    s->rtmp_server = rtmp_server_create(s, &handler);
    if (!s->rtmp_server) {
        ztk_mutex_destroy(s->play_mtx);
        free(s);
        return NULL;
    }

    ztk_info("RTMP #%u: session create", s->session_no);
    return s;
}

void zms_rtmp_session_destroy(zms_rtmp_session *s)
{
    if (!s) {
        return;
    }
    s->destroy_token++;
    s->play_boot_pending = 0;
    s->destroy_scheduled = 1;
    ztk_info("RTMP #%u: session destroy", s->session_no);
    zms_rtmp_session_lock(s);
    if (!s->destroy_scheduled) {
        zms_rtmp_session_teardown(s);
    }
    zms_live_ingest_destroy(s->ingress);
    s->ingress = NULL;
    rtmp_server_destroy(s->rtmp_server);
    s->rtmp_server = NULL;
    zms_live_ingest_destroy(s->ingress);
    {
        ztk_poller *pol = s->tcp ? ztk_tcp_session_poller(s->tcp) : NULL;
        zms_buf_pool_slot_clear_poller(&s->play_tag_buf, &s->play_tag_cap, pol);
        zms_buf_pool_slot_clear_poller(&s->play_es_buf, &s->play_es_cap, pol);
    }
    if (s->play_mtx) {
        zms_rtmp_session_unlock(s);
        ztk_mutex_destroy(s->play_mtx);
        s->play_mtx = NULL;
    }
    free(s);
}

void zms_rtmp_session_on_recv(zms_rtmp_session *s, const void *data, size_t len)
{
    int r;
    int flush;

    if (!s || s->destroy_scheduled || !s->rtmp_server || !data || len == 0) {
        return;
    }
    zms_rtmp_session_lock(s);
    if (s->destroy_scheduled || !s->rtmp_server) {
        zms_rtmp_session_unlock(s);
        return;
    }
    r = rtmp_server_input(s->rtmp_server, (const uint8_t *)data, len);
    if (r != 0) {
        ztk_warn("RTMP #%u chunk input error: %d", s->session_no, r);
    }
    flush = !s->destroy_scheduled && s->state == ZMS_RTMP_SESSION_STATE_PLAYING &&
            !s->play_boot_pending;
    zms_rtmp_session_unlock(s);
    if (flush) {
        zms_rtmp_session_play_flush(s);
    }
}

void zms_rtmp_session_on_error(zms_rtmp_session *s)
{
    zms_rtmp_session_schedule_destroy(s, NULL);
}

void zms_rtmp_session_schedule_destroy(zms_rtmp_session *s, ztk_tcp_session *session)
{
    rtmp_destroy_job *job;
    ztk_poller *pol;

    if (!s) {
        return;
    }
    zms_rtmp_session_lock(s);
    if (s->destroy_scheduled) {
        zms_rtmp_session_unlock(s);
        return;
    }
    ztk_info("RTMP #%u: schedule_destroy tcp=%p", s->session_no, (void *)s->tcp);
    s->destroy_scheduled = 1;
    s->play_boot_pending = 0;
    s->tcp = NULL;
    s->destroy_token++;
    zms_rtmp_session_teardown(s);
    zms_rtmp_session_unlock(s);

    job = (rtmp_destroy_job *)malloc(sizeof(*job));
    if (!job) {
        zms_rtmp_session_destroy(s);
        return;
    }
    job->s = s;
    job->token = s->destroy_token;

    pol = session ? ztk_tcp_session_poller(session) : NULL;
    if (pol && ztk_poller_do_delay(pol, 0, zms_rtmp_session_destroy_delayed, job)) {
        return;
    }
    zms_rtmp_session_destroy_task(job);
}

void zms_rtmp_session_on_manager(zms_rtmp_session *s)
{
    if (!s || s->destroy_scheduled || !s->tcp) {
        return;
    }
    if (s->state == ZMS_RTMP_SESSION_STATE_PLAYING && !s->play_boot_pending) {
        zms_rtmp_session_play_flush(s);
    }
}
