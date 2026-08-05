#include "zms/session/session_dispatcher.h"
#include "zms/media/container/demux_pipeline.h"
#include "zms/vod/io/vod_source.h"
#include "session/rtmp/rtmp_session_internal.h"
static ztk_err_t rtmp_protocol_on_play_live(void *session, zms_media_source *src,
                                            const zms_session_play_opts *opts)
{
    zms_rtmp_session *s = (zms_rtmp_session *)session;
    (void)opts;
    if (!s || !src) {
        return ZTK_ERR_INVALID;
    }
    if (zms_session_play_open_live(&s->play, src, ZMS_SESSION_LIVE_GOP) != ZTK_OK) {
        return ZTK_ERR_STATE;
    }
    s->gop_reader = s->play.readers.gop;
    s->vod_reader = NULL;
    zms_rtmp_session_egress_create(s);
    return ZTK_OK;
}

static ztk_err_t rtmp_protocol_on_play_vod(void *session, zms_media_source *src,
                                           const zms_session_play_opts *opts)
{
    zms_rtmp_session *s = (zms_rtmp_session *)session;
    uint64_t seek_ms = opts ? opts->seek_ms : 0;
    if (!s || !src || !zms_media_source_is_vod(src)) {
        return ZTK_ERR_INVALID;
    }
    if (zms_rtmp_session_play_vod_lane_attach(s, src, seek_ms) != ZTK_OK) {
        return ZTK_ERR_STATE;
    }
    zms_rtmp_session_egress_create(s);
    return ZTK_OK;
}

static ztk_err_t rtmp_protocol_on_publish(void *session, zms_media_source *src,
                                          const zms_session_publish_opts *opts)
{
    zms_rtmp_session *s = (zms_rtmp_session *)session;
    (void)opts;
    if (!s || !src || !s->ingress) {
        return ZTK_ERR_INVALID;
    }
    if (s->publish_pipeline) {
        return ZTK_OK;
    }
    s->publish_pipeline = zms_live_ingest_rtmp_demux_create(s->ingress);
    return s->publish_pipeline ? ZTK_OK : ZTK_ERR_STATE;
}

static void rtmp_protocol_publish_teardown(zms_rtmp_session *s)
{
    if (!s) {
        return;
    }
    if (s->publish_pipeline) {
        zms_live_ingest_rtmp_demux_release(s->ingress);
        zms_demux_pipeline_destroy(s->publish_pipeline);
        s->publish_pipeline = NULL;
    }
}

static void rtmp_protocol_on_teardown(void *session)
{
    zms_rtmp_session *s = (zms_rtmp_session *)session;
    if (!s) {
        return;
    }
    rtmp_protocol_publish_teardown(s);
    zms_rtmp_session_egress_close(s);
}

static const zms_session_dispatch_ops k_rtmp_dispatch = {
    .name = ZMS_SESSION_RTMP,
    .on_play_live = rtmp_protocol_on_play_live,
    .on_play_vod = rtmp_protocol_on_play_vod,
    .on_publish = rtmp_protocol_on_publish,
    .on_teardown = rtmp_protocol_on_teardown,
};

void zms_rtmp_register(void)
{
    static int registered; /* 启动阶段，单线程 */
    if (registered) {
        return;
    }
    registered = 1;
    zms_session_dispatch_register(&k_rtmp_dispatch);
}
