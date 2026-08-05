#include "session/rtsp/rtsp_session_internal.h"
#include "zms/session/rtsp/rtsp_session_auth.h"
#include "ztk/util/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void zms_rtsp_session_handle_setup(zms_rtsp_session *rs, const zms_rtsp_message *msg)
{
    if (!rs || !msg) {
        return;
    }
    if (!zms_rtsp_auth_check(rs, msg)) {
        return;
    }

    const char *transport = zms_rtsp_message_get(msg, "Transport");
    if (zms_rtsp_test_reject_tcp_setup() && rs->ingress == NULL && transport &&
        strstr(transport, "interleaved") && !strstr(transport, "UDP") &&
        !strstr(transport, "udp")) {
        ztk_info("RTSP SETUP 461 (test_reject_tcp_setup): url=%s", msg->url);
        zms_rtsp_session_send_resp(rs, 461, "Unsupported Transport", NULL, NULL, 0);
        return;
    }
    const int is_push = (rs->ingress != NULL);

    if (!rs->session_id[0]) {
        snprintf(rs->session_id, sizeof(rs->session_id), "%u", (unsigned)rand());
    }

    int tid = zms_rtsp_session_setup_track_id(msg->url, rs->ingress ? &rs->publish_sdp : NULL);
    if (tid < 0) {
        tid = 0;
    }

    if (rs->rtp_mode == ZMS_RTSP_RTP_TCP && transport) {
        rs->rtp_mode = zms_rtsp_transport_parse_mode(transport);
    }

    if (rs->rtp_mode == ZMS_RTSP_RTP_UDP) {
        char extra[384];
        if (is_push) {
            if ((unsigned)tid >= rs->publish_sdp.track_count) {
                zms_rtsp_session_send_resp(rs, 404, "Not Found", NULL, NULL, 0);
                return;
            }
            rs->publish_track_setup[tid] = 1;
            if (zms_rtsp_session_setup_udp_record(rs, tid, transport, extra, sizeof(extra)) != 0) {
                rs->publish_track_setup[tid] = 0;
                ztk_warn("RTSP SETUP UDP RECORD failed: track=%d url=%s", tid, msg->url);
                zms_rtsp_session_send_resp(rs, 461, "Unsupported Transport", NULL, NULL, 0);
                return;
            }
        } else {
            if (zms_rtsp_session_setup_udp_play(rs, tid, transport, extra, sizeof(extra)) != 0) {
                ztk_warn("RTSP SETUP UDP PLAY failed: track=%d url=%s", tid, msg->url);
                zms_rtsp_session_send_resp(rs, 461, "Unsupported Transport", NULL, NULL, 0);
                return;
            }
            if (tid == 0) {
                rs->play_video_setup = 1;
            } else {
                rs->play_audio_setup = 1;
            }
        }
        zms_rtsp_session_send_resp(rs, 200, "OK", extra, NULL, 0);
        return;
    }

    if (transport && !strstr(transport, "TCP") && !strstr(transport, "tcp") &&
        !strstr(transport, "interleaved")) {
        ztk_warn("RTSP SETUP: unsupported transport (%s)", transport);
        zms_rtsp_session_send_resp(rs, 461, "Unsupported Transport", NULL, NULL, 0);
        return;
    }

    uint8_t ich = (uint8_t)(tid * 2);

    if (rs->ingress && rs->publish_sdp.track_count > 0) {
        if ((unsigned)tid >= rs->publish_sdp.track_count) {
            zms_rtsp_session_send_resp(rs, 404, "Not Found", NULL, NULL, 0);
            return;
        }
        rs->publish_sdp.tracks[tid].interleaved_rtp = ich;
        rs->publish_sdp.tracks[tid].interleaved_rtcp = (uint8_t)(ich + 1);
        rs->publish_track_setup[tid] = 1;
        if (rs->publish_sdp.tracks[tid].type == ZMS_TRACK_VIDEO) {
            rs->video_rtp_ch = ich;
            rs->video_rtcp_ch = (uint8_t)(ich + 1);
            if (!is_push) {
                rs->play_video_setup = 1;
            }
        } else if (rs->publish_sdp.tracks[tid].type == ZMS_TRACK_AUDIO) {
            rs->audio_rtp_ch = ich;
            rs->audio_rtcp_ch = (uint8_t)(ich + 1);
            if (!is_push) {
                rs->play_audio_setup = 1;
            }
        }
    } else {
        if (tid == 0) {
            rs->video_rtp_ch = ich;
            rs->video_rtcp_ch = (uint8_t)(ich + 1);
            rs->play_video_setup = 1;
        } else {
            rs->audio_rtp_ch = ich;
            rs->audio_rtcp_ch = (uint8_t)(ich + 1);
            rs->play_audio_setup = 1;
        }
    }

    char extra[256];
    snprintf(extra, sizeof(extra),
             "Transport: RTP/AVP/TCP;unicast;interleaved=%u-%u\r\n"
             "Session: %s;timeout=60\r\n",
             (unsigned)ich, (unsigned)(ich + 1), rs->session_id);
    ztk_info("RTSP SETUP 200 TCP: track=%d interleaved=%u-%u session=%s push=%d url=%s", tid,
             (unsigned)ich, (unsigned)(ich + 1), rs->session_id, is_push ? 1 : 0, msg->url);
    zms_rtsp_session_send_resp(rs, 200, "OK", extra, NULL, 0);
}
