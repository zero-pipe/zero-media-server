#include "session/srt/srt_service_internal.h"
#include "zms/ops/api/webhook/webhook_client.h"
#include "zms/egress/egress_mp4_recorder.h"
#include "zms/engine/media_event.h"
#include "zms/session/session_dispatcher.h"
#include "ztk/util/log.h"
#include <string.h>

#define ZMS_SRT_RECV_BUF 65536

int zms_srt_session_begin_publish(zms_srt_session *s, const char *app, const char *stream,
                                  const char *streamid)
{
    zms_session_publish_opts pubcfg;

    if (!s || !app[0] || !stream[0]) {
        return -1;
    }

    s->mode = ZMS_SRT_SESSION_MODE_PUBLISH;
    strncpy(s->app, app, sizeof(s->app) - 1);
    strncpy(s->stream, stream, sizeof(s->stream) - 1);

    s->ingress = zms_live_ingest_create_publish_schema(ZMS_SCHEMA_SRT, app, stream, NULL);
    if (s->ingress) {
        zms_live_ingest_set_poller(s->ingress, s->poller);
    }
    s->source = s->ingress ? zms_live_ingest_source(s->ingress) : NULL;
    if (!s->source) {
        ztk_error("SRT #%d publish failed: app=%s stream=%s", s->session_no, app, stream);
        return -1;
    }

    if (!zms_webhook_allow_publish(s->source, ZMS_ORIGIN_SRT_PUSH, NULL, streamid)) {
        ztk_warn("SRT #%d publish denied: app=%s stream=%s", s->session_no, app, stream);
        zms_live_ingest_destroy(s->ingress);
        s->ingress = NULL;
        s->source = NULL;
        return -1;
    }

    memset(&pubcfg, 0, sizeof(pubcfg));
    pubcfg.schema = ZMS_SESSION_SRT;
    if (zms_session_attach_publish(ZMS_SESSION_SRT, s, s->source, &pubcfg) != ZTK_OK) {
        ztk_warn("SRT #%d attach publish failed: app=%s stream=%s", s->session_no, app, stream);
        zms_live_ingest_destroy(s->ingress);
        s->ingress = NULL;
        s->source = NULL;
        return -1;
    }
    return 0;
}

void zms_srt_session_teardown_publish(zms_srt_session *s)
{
    zms_media_source *src;

    if (!s) {
        return;
    }
    src = s->source;
    if (s->ingress) {
        zms_live_ingest_reset(s->ingress);
    }
    if (src) {
        zms_media_source_clear_publisher(src, s);
        zms_media_event_publish_fini(src, ZMS_ORIGIN_SRT_PUSH);
    }
}

void zms_srt_session_finish_publish(zms_srt_session *s)
{
    if (!s || s->stopping || s->destroy_scheduled || !s->source) {
        return;
    }

    zms_media_event_publish(s->source, ZMS_ORIGIN_SRT_PUSH);
    zms_media_source_set_publisher(s->source, s, zms_srt_session_publisher_kick);
    if (s->source->enable_mp4) {
        (void)zms_mp4_recorder_start(s->source, s->poller);
    }
    ztk_info("SRT #%d publish start: app=%s stream=%s stream_id=%s enable_mp4=%d", s->session_no,
             s->app, s->stream, s->source->stream, s->source->enable_mp4);
}

static void srt_session_feed_ts(zms_srt_session *s, const uint8_t *buf, int n)
{
    if (!s || !buf || n <= 0 || !s->publish_pipeline || s->stopping || s->destroy_scheduled) {
        return;
    }
    (void)zms_demux_pipeline_feed(s->publish_pipeline, buf, (size_t)n);
}

void zms_srt_session_drain_recv(zms_srt_session *s)
{
    uint8_t buf[ZMS_SRT_RECV_BUF];
    SRT_SOCKSTATUS st;
    int total = 0;

    if (!s || s->stopping || s->destroy_scheduled || s->mode != ZMS_SRT_SESSION_MODE_PUBLISH) {
        return;
    }

    st = srt_getsockstate(s->sock);
    if (st == SRTS_BROKEN || st == SRTS_NONEXIST || st == SRTS_CLOSED) {
        ztk_info("SRT #%d disconnected state=%d bytes=%llu", s->session_no, (int)st,
                 (unsigned long long)s->recv_bytes);
        zms_srt_session_schedule_destroy(s);
        return;
    }

    for (;;) {
        int n = srt_recvmsg(s->sock, (char *)buf, (int)sizeof(buf));
        if (n > 0) {
            s->recv_bytes += (uint64_t)n;
            s->recv_calls++;
            s->last_recv_bytes = s->recv_bytes;
            s->stall_polls = 0;
            srt_session_feed_ts(s, buf, n);
            ++total;
            if (!s->logged_recv) {
                s->logged_recv = 1;
                ztk_info("SRT #%d first recv %d bytes", s->session_no, n);
            }
            continue;
        }
        if (n == 0) {
            break;
        }
        if (n == SRT_ERROR && srt_getlasterror(NULL) == SRT_EASYNCRCV) {
            break;
        }
        ztk_info("SRT #%d recv end err=%s pkts=%d bytes=%llu", s->session_no,
                 srt_getlasterror_str(), total, (unsigned long long)s->recv_bytes);
        zms_srt_session_schedule_destroy(s);
        return;
    }
}
