#include "session/rtsp/rtsp_session_internal.h"
#include "zms/session/rtp/rtcp.h"
#include "zms/egress/egress_clock.h"
#include "zms/egress/rtp/rtp_muxer.h"
#include "ztk/net/tcp_server.h"
#include "ztk/util/log.h"
#include <string.h>

/** 双轨点播：音视频 SR 分轨发送，避免只发视频 SR 导致 VLC 长时间无音频 */
void zms_rtsp_session_send_rtcp_sr_tracks(zms_rtsp_session *rs, int send_video, int send_audio)
{
    const zms_rtp_muxer_stats *st;
    const zms_egress_clock *clk;
    uint32_t ntp_sec;
    uint32_t ntp_frac;
    uint32_t vhz;
    uint32_t ahz;
    uint8_t rtcp[64];
    size_t n;

    if (!rs || rs->mode != ZMS_RTSP_SESSION_MODE_PLAY || (!send_video && !send_audio)) {
        return;
    }
    st = zms_rtp_muxer_get_stats(rs->play_rtp_muxer);
    clk = zms_rtp_muxer_play_clock(rs->play_rtp_muxer);
    if (!st) {
        return;
    }
    vhz = rs->video_clock_hz > 0 ? rs->video_clock_hz : 90000u;
    ahz = rs->audio_clock_hz > 0 ? rs->audio_clock_hz
                                 : (uint32_t)(rs->audio_rate > 0 ? rs->audio_rate : 44100);
    if (send_video && rs->source && rs->source->has_video && st->video_pkt_count > 0) {
        zms_egress_clock_sr_ntp(clk, st->video_last_rtp_ts, vhz, &ntp_sec, &ntp_frac);
        n = zms_rtcp_build_sr(rtcp, sizeof(rtcp), rs->video_rtp_ssrc, ntp_sec, ntp_frac,
                              st->video_last_rtp_ts, st->video_pkt_count, st->video_octet_count);
        if (n) {
            zms_rtsp_session_send_media(rs, rs->video_rtcp_ch, rtcp, n);
        }
    }
    if (send_audio && rs->source && rs->source->has_audio && st->audio_pkt_count > 0) {
        zms_egress_clock_sr_ntp(clk, st->audio_last_rtp_ts, ahz, &ntp_sec, &ntp_frac);
        n = zms_rtcp_build_sr(rtcp, sizeof(rtcp), rs->audio_rtp_ssrc, ntp_sec, ntp_frac,
                              st->audio_last_rtp_ts, st->audio_pkt_count, st->audio_octet_count);
        if (n) {
            zms_rtsp_session_send_media(rs, rs->audio_rtcp_ch, rtcp, n);
        }
    }
}

void zms_rtsp_session_play_try_rtcp_boot(zms_rtsp_session *rs)
{
    const zms_rtp_muxer_stats *st;
    int sent = 0;

    if (!rs || !rs->play_rtp_muxer || !rs->source) {
        return;
    }
    if (rs->vod_reader && zms_rtp_muxer_catchup_on(rs->play_rtp_muxer)) {
        return;
    }
    st = zms_rtp_muxer_get_stats(rs->play_rtp_muxer);
    if (!st) {
        return;
    }
    if (rs->source->has_video && !rs->play_rtcp_video_sr_sent && st->video_pkt_count > 0) {
        zms_rtsp_session_send_rtcp_sr_tracks(rs, 1, 0);
        rs->play_rtcp_video_sr_sent = 1;
        sent = 1;
    }
    if (rs->source->has_audio && !rs->play_rtcp_audio_sr_sent && st->audio_pkt_count > 0) {
        zms_rtsp_session_send_rtcp_sr_tracks(rs, 0, 1);
        rs->play_rtcp_audio_sr_sent = 1;
        sent = 1;
    }
    if ((!rs->source->has_video || rs->play_rtcp_video_sr_sent) &&
        (!rs->source->has_audio || rs->play_rtcp_audio_sr_sent)) {
        rs->play_rtcp_boot_sent = 1;
    }
    if (sent && rs->tcp) {
        ztk_tcp_session_flush(rs->tcp);
    }
}

void zms_rtsp_session_send_rtcp_srs(zms_rtsp_session *rs)
{
    zms_rtsp_session_send_rtcp_sr_tracks(rs, 1, 1);
}

void zms_rtsp_session_play_on_rtcp(zms_rtsp_session *rs, const uint8_t *data, size_t len)
{
    zms_rtcp_rr rr;
    if (zms_rtcp_parse_rr(data, len, &rr) == ZTK_OK) {
        (void)rr;
    }
}
