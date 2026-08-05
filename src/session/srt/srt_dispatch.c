#include "zms/session/session_dispatcher.h"
#include "zms/session/play_binding.h"
#include "zms/media/container/demux_pipeline.h"
#include "zms/live/ingest/common/stream_ingest.h"
#include "zms/egress/mpegts/mpegts_egress.h"
#include "srt_service_internal.h"
#include "ztk/util/log.h"
#include <string.h>
static void srt_protocol_on_h264_ps(const uint8_t *sps, size_t sps_len, const uint8_t *pps,
                                    size_t pps_len, void *user)
{
    zms_srt_session *s = (zms_srt_session *)user;
    if (!s || !s->ingress || !sps || !pps || sps_len == 0 || pps_len == 0) {
        return;
    }
    if (zms_live_ingest_set_h264_sps_pps(s->ingress, sps, sps_len, pps, pps_len) != ZTK_OK) {
        ztk_warn("SRT #%d H264 config apply failed (sps=%u pps=%u)", s->session_no,
                 (unsigned)sps_len, (unsigned)pps_len);
    }
}

static void srt_protocol_on_frame(const zms_frame *frame, void *user)
{
    zms_srt_session *s = (zms_srt_session *)user;
    if (!s || !s->ingress || !frame || frame->size == 0) {
        return;
    }
    (void)zms_live_ingest_input_frame(s->ingress, frame);
}

static ztk_err_t srt_protocol_on_publish(void *session, zms_media_source *src,
                                         const zms_session_publish_opts *opts)
{
    zms_srt_session *s = (zms_srt_session *)session;
    zms_demux_pipeline_opts pcfg;
    (void)opts;
    (void)src;
    if (!s || !s->ingress) {
        return ZTK_ERR_INVALID;
    }
    if (s->publish_pipeline) {
        return ZTK_OK;
    }
    /* MPEG-TS PTS/DTS 为 90kHz tick；时间线走标准 ingress（同 RTSP RECORD）。 */
    zms_live_ingest_set_rtp_clocks(s->ingress, 90000, ZMS_CODEC_AAC, 90000);
    /* libmpeg 损坏/丢包边缘后 TS 可间隙 >300ms；默认 stamp clamp 会破坏 GOP 间距。 */
    zms_live_ingest_set_stamp_max_delta(s->ingress, 10000);
    /* PES A/V PTS 原点不同（如 audio dts=0、video dts=1566）；RTP 式 300ms clamp 会把 video 归零。 */
    zms_live_ingest_set_stamp_av_clamp(s->ingress, 0);
    /* demux PTS/DTS 已在 MPEG-TS 轴上为 ms；勿按 track 重归零。 */
    zms_live_ingest_set_timeline_linear_ms(s->ingress, 1);
    /* 推迟 ring video_config 至 IDR，避免缓存 IDR 前 P/B 帧。 */
    zms_live_ingest_set_defer_gop_vcfg(s->ingress, 1);
    memset(&pcfg, 0, sizeof(pcfg));
    pcfg.container = ZMS_CONTAINER_MPEGTS;
    pcfg.on_frame = srt_protocol_on_frame;
    pcfg.on_mpegts_h264_ps = srt_protocol_on_h264_ps;
    pcfg.user = s;
    s->publish_pipeline = zms_demux_pipeline_create(&pcfg);
    return s->publish_pipeline ? ZTK_OK : ZTK_ERR_STATE;
}

static ztk_err_t srt_protocol_on_play_live(void *session, zms_media_source *src,
                                           const zms_session_play_opts *opts)
{
    zms_srt_session *s = (zms_srt_session *)session;
    (void)opts;
    if (!s || !src) {
        return ZTK_ERR_INVALID;
    }
    return zms_session_play_open_live(&s->play, src, ZMS_SESSION_LIVE_GOP);
}

static void srt_protocol_on_teardown(void *session)
{
    zms_srt_session *s = (zms_srt_session *)session;
    zms_play_binding bind;

    if (!s) {
        return;
    }
    if (s->live_muxer) {
        zms_mpegts_egress_destroy(s->live_muxer);
        s->live_muxer = NULL;
    }
    if (s->mode == ZMS_SRT_SESSION_MODE_PLAY) {
        memset(&bind, 0, sizeof(bind));
        bind.source = &s->source;
        bind.play = &s->play;
        bind.reader_attached = &s->reader_attached;
        bind.play_start_ms = &s->play_start_ms;
        bind.player = "srt";
        zms_play_binding_close_readers(&bind);
    }
    if (s->publish_pipeline) {
        (void)zms_demux_pipeline_flush(s->publish_pipeline);
        zms_demux_pipeline_destroy(s->publish_pipeline);
        s->publish_pipeline = NULL;
    }
}

static const zms_session_dispatch_ops k_srt_dispatch = {
    .name = ZMS_SESSION_SRT,
    .on_play_live = srt_protocol_on_play_live,
    .on_play_vod = NULL,
    .on_publish = srt_protocol_on_publish,
    .on_teardown = srt_protocol_on_teardown,
};

void zms_srt_register(void)
{
    static int registered; /* 启动阶段，单线程 */
    if (registered) {
        return;
    }
    registered = 1;
    zms_session_dispatch_register(&k_srt_dispatch);
}
