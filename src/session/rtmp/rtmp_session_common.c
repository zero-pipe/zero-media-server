#include "session/rtmp/rtmp_session_internal.h"
#include "zms/session/session_dispatcher.h"
#include "zms/session/play_binding.h"
#include "zms/engine/media_event.h"
#include "zms/live/ingest/common/stream_ingest.h"
#include <stdio.h>
#include <string.h>

static void merge_slash_in_app(char *app, char *stream)
{
    char *slash = strchr(app, '/');
    if (!slash) {
        return;
    }
    *slash = '\0';
    if (!slash[1]) {
        return;
    }
    if (stream[0]) {
        char buf[ZMS_STREAM_MAX];
        snprintf(buf, sizeof(buf), "%s/%s", slash + 1, stream);
        strncpy(stream, buf, sizeof(stream) - 1);
        stream[ZMS_STREAM_MAX - 1] = '\0';
    } else {
        strncpy(stream, slash + 1, ZMS_STREAM_MAX - 1);
        stream[ZMS_STREAM_MAX - 1] = '\0';
    }
}

static void rtmp_split_connect_app(const char *app_field, char *app, char *conn_stream)
{
    if (app) {
        app[0] = '\0';
    }
    if (conn_stream) {
        conn_stream[0] = '\0';
    }
    if (!app_field || !app_field[0] || !app) {
        return;
    }
    if (!strchr(app_field, '/')) {
        strncpy(app, app_field, ZMS_APP_MAX - 1);
        app[ZMS_APP_MAX - 1] = '\0';
        return;
    }
    {
        char fake[384];
        snprintf(fake, sizeof(fake), "rtmp://host/%s", app_field);
        zms_media_split_path(fake, app, conn_stream);
    }
}

zms_media_source *zms_rtmp_session_find_play_source(const char *rtmp_app, const char *play_name,
                                                    char *out_app, size_t app_cap, char *out_stream,
                                                    size_t stream_cap)
{
    char app[ZMS_APP_MAX];
    char stream[ZMS_STREAM_MAX];
    char conn_stream[ZMS_STREAM_MAX];
    char play_app[ZMS_APP_MAX];
    zms_media_source *src = NULL;

    app[0] = stream[0] = conn_stream[0] = play_app[0] = '\0';

    rtmp_split_connect_app(rtmp_app, app, conn_stream);

    if (play_name && play_name[0]) {
        if (strchr(play_name, '/')) {
            char path[384];
            if (strstr(play_name, "://")) {
                snprintf(path, sizeof(path), "%s", play_name);
            } else if (app[0]) {
                snprintf(path, sizeof(path), "rtmp://host/%s/%s", app, play_name);
            } else {
                snprintf(path, sizeof(path), "rtmp://host/%s", play_name);
            }
            zms_media_split_path(path, play_app, stream);
            if (play_app[0]) {
                strncpy(app, play_app, sizeof(app) - 1);
            }
        } else {
            strncpy(stream, play_name, sizeof(stream) - 1);
        }
    }

    if (!app[0]) {
        strncpy(app, "live", sizeof(app) - 1);
    }
    app[sizeof(app) - 1] = '\0';
    if (!stream[0] && conn_stream[0]) {
        strncpy(stream, conn_stream, sizeof(stream) - 1);
    }
    stream[sizeof(stream) - 1] = '\0';

    src = zms_media_source_find_for_play(ZMS_SCHEMA_RTMP, app, stream);
    if (!src && stream[0] && !strchr(stream, '/')) {
        char alt[ZMS_STREAM_MAX];
        int n = snprintf(alt, sizeof(alt), "stream/%s", stream);
        if (n > 0 && (size_t)n < sizeof(alt)) {
            src = zms_media_source_find_for_play(ZMS_SCHEMA_RTMP, app, alt);
        }
        if (src) {
            strncpy(stream, alt, sizeof(stream) - 1);
        }
    }
    if (!src && (!stream[0] || strcmp(stream, "stream") == 0)) {
        src = zms_media_source_find_for_play(ZMS_SCHEMA_RTMP, app, "stream");
    }

    if (src) {
        strncpy(app, src->app, sizeof(app) - 1);
        strncpy(stream, src->stream, sizeof(stream) - 1);
    }
    app[sizeof(app) - 1] = '\0';
    stream[sizeof(stream) - 1] = '\0';

    if (out_app && app_cap) {
        snprintf(out_app, app_cap, "%s", app);
    }
    if (out_stream && stream_cap) {
        snprintf(out_stream, stream_cap, "%s", stream);
    }
    return src;
}

void zms_rtmp_session_resolve_target(zms_rtmp_session *s, const char *app_in, const char *stream_in)
{
    char app[ZMS_APP_MAX];
    char stream[ZMS_STREAM_MAX];

    if (!s) {
        return;
    }
    app[0] = stream[0] = '\0';
    if (app_in && app_in[0]) {
        strncpy(app, app_in, sizeof(app) - 1);
    } else if (s->app[0]) {
        strncpy(app, s->app, sizeof(app) - 1);
    }

    if (stream_in && stream_in[0]) {
        strncpy(stream, stream_in, sizeof(stream) - 1);
    } else if (s->stream[0]) {
        strncpy(stream, s->stream, sizeof(stream) - 1);
    }

    if (!app[0]) {
        strncpy(app, "live", sizeof(app) - 1);
    }
    merge_slash_in_app(app, stream);

    strncpy(s->app, app, sizeof(s->app) - 1);
    s->app[sizeof(s->app) - 1] = '\0';
    strncpy(s->stream, stream, sizeof(s->stream) - 1);
    s->stream[sizeof(s->stream) - 1] = '\0';
}

void zms_rtmp_session_send(zms_rtmp_session *s, const void *data, size_t len)
{
    if (s && s->tcp && data && len) {
        ztk_tcp_session_send(s->tcp, data, len);
    }
}

void zms_rtmp_session_flush_tcp(zms_rtmp_session *s)
{
    if (s && s->tcp) {
        ztk_tcp_session_flush(s->tcp);
    }
}

int rtmp_srv_send(void *param, const void *header, size_t header_len, const void *payload,
                  size_t payload_len)
{
    zms_rtmp_session *s = (zms_rtmp_session *)param;
    int sent = 0;

    if (!s) {
        return -1;
    }
    if (header && header_len) {
        zms_rtmp_session_send(s, header, header_len);
        sent += (int)header_len;
    }
    if (payload && payload_len) {
        zms_rtmp_session_send(s, payload, payload_len);
        sent += (int)payload_len;
    }
    if (s->tcp) {
        ztk_tcp_session_flush(s->tcp);
    }
    return sent == (int)(header_len + payload_len) ? sent : -1;
}

static void rtmp_publisher_kick(void *ctx, int force)
{
    zms_rtmp_session *s = (zms_rtmp_session *)ctx;
    (void)force;
    if (s && s->tcp) {
        ztk_tcp_session_close(s->tcp);
    }
}

void zms_rtmp_session_play_teardown(zms_rtmp_session *s)
{
    zms_play_binding bind;

    if (!s) {
        return;
    }
    s->play_boot_pending = 0;
    zms_session_detach_play(ZMS_SESSION_RTMP, s);
    memset(&bind, 0, sizeof(bind));
    bind.source = &s->source;
    bind.reader_attached = &s->play_reader_attached;
    bind.play_start_ms = &s->live_play_start_ms;
    bind.player = "rtmp";
    zms_play_binding_reader_stop(&bind);
}

void zms_rtmp_session_play_readers_detach(zms_rtmp_session *s)
{
    if (!s) {
        return;
    }
    s->play_boot_pending = 0;
    zms_session_detach_play(ZMS_SESSION_RTMP, s);
}

static void zms_rtmp_session_publish_stop(zms_rtmp_session *s)
{
    zms_media_source *src;

    if (!s || s->state != ZMS_RTMP_SESSION_STATE_PUBLISHING) {
        return;
    }
    src = s->source;
    if (s->ingress) {
        zms_live_ingest_reset(s->ingress);
    }
    if (src) {
        zms_media_source_clear_publisher(src, s);
    }
    if (src) {
        zms_media_event_publish_fini(src, ZMS_ORIGIN_RTMP_PUSH);
    }
    s->state = ZMS_RTMP_SESSION_STATE_IDLE;
}

void zms_rtmp_session_teardown(zms_rtmp_session *s)
{
    if (!s) {
        return;
    }
    zms_rtmp_session_play_teardown(s);
    zms_rtmp_session_publish_stop(s);
    s->source = NULL;
}

void zms_rtmp_session_play_readers_attach(zms_rtmp_session *s)
{
    zms_session_play_opts pcfg;

    if (!s || !s->source) {
        return;
    }
    zms_rtmp_session_play_readers_detach(s);
    memset(&pcfg, 0, sizeof(pcfg));
    pcfg.player = ZMS_SESSION_RTMP;
    if (zms_session_attach_play(ZMS_SESSION_RTMP, s, s->source, &pcfg) != ZTK_OK) {
        return;
    }
}

void zms_rtmp_session_publish_teardown(zms_rtmp_session *s)
{
    zms_rtmp_session_teardown(s);
}

/* publisher kick 导出给 media_source */
void zms_rtmp_session_publisher_kick(void *ctx, int force)
{
    rtmp_publisher_kick(ctx, force);
}
