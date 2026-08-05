#include "zms/session/session_dispatcher.h"
#include "zms/media/container/demux_pipeline.h"
#include "zms/vod/io/vod_source.h"
#include "session/rtsp/rtsp_session_internal.h"
#include "ztk/util/log.h"
#include <string.h>
static ztk_err_t rtsp_protocol_on_play_live(void *session, zms_media_source *src,
                                            const zms_session_play_opts *opts)
{
    zms_rtsp_session *rs = (zms_rtsp_session *)session;
    (void)opts;
    if (!rs || !src) {
        return ZTK_ERR_INVALID;
    }
    if (zms_session_play_open_live(&rs->play, src, ZMS_SESSION_LIVE_GOP) != ZTK_OK) {
        return ZTK_ERR_STATE;
    }
    rs->gop_reader = rs->play.readers.gop;
    return ZTK_OK;
}

static ztk_err_t rtsp_protocol_on_play_vod(void *session, zms_media_source *src,
                                           const zms_session_play_opts *opts)
{
    zms_rtsp_session *rs = (zms_rtsp_session *)session;
    uint64_t seek_ms = opts ? opts->seek_ms : 0;
    if (!rs || !src || !zms_media_source_is_vod(src)) {
        return ZTK_ERR_INVALID;
    }
    return zms_rtsp_session_play_vod_lane_attach(rs, src, seek_ms);
}

static void rtsp_protocol_record_on_frame(const zms_frame *frame, void *user)
{
    zms_rtsp_session *rs = (zms_rtsp_session *)user;
    if (!rs || !rs->ingress || rs->mode != ZMS_RTSP_SESSION_MODE_RECORD || !frame ||
        frame->size == 0) {
        return;
    }
    if (frame->codec == ZMS_CODEC_H264 && !rs->logged_record_h264) {
        rs->logged_record_h264 = 1;
        ztk_info("RTSP RECORD first H264 frame: size=%u key=%d", (unsigned)frame->size,
                 frame->keyframe);
    }
    (void)zms_live_ingest_input_frame(rs->ingress, frame);
}

static ztk_err_t rtsp_protocol_on_publish(void *session, zms_media_source *src,
                                          const zms_session_publish_opts *opts)
{
    zms_rtsp_session *rs = (zms_rtsp_session *)session;
    zms_demux_pipeline_opts pcfg;
    (void)opts;
    if (!rs || !src || !rs->ingress) {
        return ZTK_ERR_INVALID;
    }
    if (rs->record_pipeline) {
        return ZTK_OK;
    }
    memset(&pcfg, 0, sizeof(pcfg));
    pcfg.container = ZMS_CONTAINER_RTSP_INTERLEAVED;
    pcfg.on_frame = rtsp_protocol_record_on_frame;
    pcfg.user = rs;
    rs->record_pipeline = zms_demux_pipeline_create(&pcfg);
    return rs->record_pipeline ? ZTK_OK : ZTK_ERR_STATE;
}

static void rtsp_protocol_on_teardown(void *session)
{
    zms_rtsp_session *rs = (zms_rtsp_session *)session;
    if (!rs) {
        return;
    }
    zms_rtsp_session_egress_close(rs);
    if (rs->record_pipeline) {
        zms_demux_pipeline_destroy(rs->record_pipeline);
        rs->record_pipeline = NULL;
    }
}

static const zms_session_dispatch_ops k_rtsp_dispatch = {
    .name = ZMS_SESSION_RTSP,
    .on_play_live = rtsp_protocol_on_play_live,
    .on_play_vod = rtsp_protocol_on_play_vod,
    .on_publish = rtsp_protocol_on_publish,
    .on_teardown = rtsp_protocol_on_teardown,
};

void zms_rtsp_register(void)
{
    static int registered; /* 启动阶段，单线程 */
    if (registered) {
        return;
    }
    registered = 1;
    zms_session_dispatch_register(&k_rtsp_dispatch);
}
