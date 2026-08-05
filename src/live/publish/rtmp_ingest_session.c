#include "session/rtmp/rtmp_session_internal.h"
#include "zms/session/session_dispatcher.h"
#include "zms/ops/api/webhook/webhook_client.h"
#include "zms/egress/egress_mp4_recorder.h"
#include "zms/engine/media_event.h"
#include "ztk/poller/poller.h"
#include "ztk/util/log.h"
#include <string.h>

int rtmp_srv_onpublish(void *param, const char *app_name, const char *stream_name, const char *type)
{
    zms_rtmp_session *s = (zms_rtmp_session *)param;

    (void)type;
    if (!s) {
        return -1;
    }

    zms_rtmp_session_resolve_target(s, app_name, stream_name);
    s->ingress = zms_live_ingest_create_publish(s->app, s->stream, NULL);
    if (s->ingress && s->tcp) {
        zms_live_ingest_set_poller(s->ingress, ztk_tcp_session_poller(s->tcp));
    }
    s->source = s->ingress ? zms_live_ingest_source(s->ingress) : NULL;
    if (!s->source) {
        ztk_error("RTMP #%u publish failed: app=%s stream=%s", s->session_no, s->app, s->stream);
        return -1;
    }

    if (!zms_webhook_allow_publish(s->source, ZMS_ORIGIN_RTMP_PUSH, s->tcp, NULL)) {
        ztk_warn("RTMP #%u publish denied: app=%s stream=%s", s->session_no, s->app, s->stream);
        zms_live_ingest_destroy(s->ingress);
        s->ingress = NULL;
        s->source = NULL;
        return -1;
    }

    s->state = ZMS_RTMP_SESSION_STATE_PUBLISHING;
    {
        zms_session_publish_opts pubcfg;

        memset(&pubcfg, 0, sizeof(pubcfg));
        pubcfg.schema = ZMS_SESSION_RTMP;
        if (zms_session_attach_publish(ZMS_SESSION_RTMP, s, s->source, &pubcfg) != ZTK_OK) {
            ztk_warn("RTMP #%u publish attach failed: app=%s stream=%s", s->session_no, s->app,
                     s->stream);
            zms_live_ingest_destroy(s->ingress);
            s->ingress = NULL;
            s->source = NULL;
            return -1;
        }
    }
    zms_media_event_publish(s->source, ZMS_ORIGIN_RTMP_PUSH);
    zms_media_source_set_publisher(s->source, s, zms_rtmp_session_publisher_kick);
    if (s->source->enable_mp4) {
        ztk_poller *pol = s->tcp ? ztk_tcp_session_poller(s->tcp) : NULL;
        (void)zms_mp4_recorder_start(s->source, pol);
    }

    ztk_info("RTMP #%u publish start: app=%s client_stream=%s stream_id=%s enable_mp4=%d",
             s->session_no, s->app, s->stream, s->source->stream, s->source->enable_mp4);

    return 0;
}

int rtmp_srv_onvideo(void *param, const void *data, size_t bytes, uint32_t timestamp)
{
    zms_rtmp_session *s = (zms_rtmp_session *)param;

    if (!s || s->state != ZMS_RTMP_SESSION_STATE_PUBLISHING || !s->ingress || !data || bytes < 2) {
        return 0;
    }

    if (bytes >= 2 && ((const uint8_t *)data)[1] == 0 && s->source && !s->source->has_video) {
        ztk_info("RTMP #%u received AVC seq header: app=%s stream=%s bytes=%u", s->session_no,
                 s->app, s->stream, (unsigned)bytes);
    }

    return zms_live_ingest_input_rtmp_video(s->ingress, timestamp, data, bytes) == ZTK_OK ? 0 : -1;
}

int rtmp_srv_onaudio(void *param, const void *data, size_t bytes, uint32_t timestamp)
{
    zms_rtmp_session *s = (zms_rtmp_session *)param;

    if (!s || s->state != ZMS_RTMP_SESSION_STATE_PUBLISHING || !s->ingress || !data || bytes < 2) {
        return 0;
    }

    if (bytes >= 2 && ((const uint8_t *)data)[1] == 0 && s->source && !s->source->has_audio) {
        ztk_info("RTMP #%u received AAC seq header: app=%s stream=%s bytes=%u", s->session_no,
                 s->app, s->stream, (unsigned)bytes);
    }

    return zms_live_ingest_input_rtmp_audio(s->ingress, timestamp, data, bytes) == ZTK_OK ? 0 : -1;
}

int rtmp_srv_onscript(void *param, const void *data, size_t bytes, uint32_t timestamp)
{
    (void)param;
    (void)data;
    (void)bytes;
    (void)timestamp;
    return 0;
}
