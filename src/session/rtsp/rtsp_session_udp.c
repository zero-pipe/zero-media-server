#include "session/rtsp/rtsp_session_internal.h"
#include "ztk/net/socket.h"
#include "ztk/util/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void zms_rtsp_session_refresh_peer_ip(zms_rtsp_session *rs)
{
    if (!rs) {
        return;
    }
    rs->peer_ip[0] = '\0';
    if (!rs->tcp) {
        return;
    }
    ztk_socket *sock = ztk_tcp_session_socket(rs->tcp);
    if (sock) {
        (void)ztk_socket_get_peer(sock, rs->peer_ip, sizeof(rs->peer_ip), NULL);
    }
}

static int parse_client_port(const char *transport, uint16_t *rtp_port, uint16_t *rtcp_port)
{
    const char *p = transport ? strstr(transport, "client_port=") : NULL;
    if (!p) {
        return -1;
    }
    p += strlen("client_port=");
    unsigned rtp = 0, rtcp = 0;
    if (sscanf(p, "%u-%u", &rtp, &rtcp) < 2) {
        return -1;
    }
    if (rtp == 0 || rtcp == 0) {
        return -1;
    }
    *rtp_port = (uint16_t)rtp;
    *rtcp_port = (uint16_t)rtcp;
    return 0;
}

static int peer_ip_match(const zms_rtsp_session *rs, const char *from_ip)
{
    if (!rs || !from_ip || !from_ip[0]) {
        return 0;
    }
    if (!rs->peer_ip[0]) {
        return 1;
    }
    return strcmp(rs->peer_ip, from_ip) == 0;
}

/** PLAY：SETUP 已知 client_port 时接受任意源 IP（WSL↔Windows NAT、TCP peer=127.0.0.1）*/
static int play_udp_packet_accept(const zms_rtsp_session *rs, const zms_rtsp_session_udp_track *ut,
                                  const char *from_ip, uint16_t from_port, int rtcp_sock)
{
    if (!from_ip || !from_ip[0] || !from_port) {
        return 0;
    }
    if (!rs || !ut) {
        return 0;
    }
    if (rtcp_sock) {
        if (from_port == ut->peer_rtcp_port) {
            return 1;
        }
    } else if (from_port == ut->peer_rtp_port) {
        return 1;
    }
    return peer_ip_match(rs, from_ip);
}

static void zms_rtsp_session_note_udp_peer(zms_rtsp_session *rs, const char *from_ip, int track_idx)
{
    if (!rs || !from_ip || !from_ip[0]) {
        return;
    }
    if (strcmp(rs->peer_ip, from_ip) == 0) {
        return;
    }
    if (track_idx >= 0) {
        ztk_info("RTSP #%u UDP NAT: peer %s -> %s (track=%d)", rs->session_no,
                 rs->peer_ip[0] ? rs->peer_ip : "?", from_ip, track_idx);
    } else {
        ztk_info("RTSP #%u UDP NAT: peer %s -> %s (all tracks)", rs->session_no,
                 rs->peer_ip[0] ? rs->peer_ip : "?", from_ip);
    }
    snprintf(rs->peer_ip, sizeof(rs->peer_ip), "%s", from_ip);
}

static void udp_learn_rtp_peer(zms_rtsp_session *rs, zms_rtsp_session_udp_track *ut,
                               const char *from_ip, uint16_t from_port)
{
    if (!ut || !ut->rtp || !from_ip || !from_port) {
        return;
    }
    if (ut->peer_rtp_port != from_port) {
        ut->peer_rtp_port = from_port;
    }
    ztk_udp_client_set_peer(ut->rtp, from_ip, from_port);
    if (rs) {
        zms_rtsp_session_note_udp_peer(rs, from_ip, ut->rtp_cb.track_idx);
    }
}

static void udp_learn_rtcp_peer(zms_rtsp_session *rs, zms_rtsp_session_udp_track *ut,
                                const char *from_ip, uint16_t from_port)
{
    if (!ut || !ut->rtcp || !from_ip || !from_port) {
        return;
    }
    if (ut->peer_rtcp_port != from_port) {
        ut->peer_rtcp_port = from_port;
    }
    ztk_udp_client_set_peer(ut->rtcp, from_ip, from_port);
    if (rs) {
        zms_rtsp_session_note_udp_peer(rs, from_ip, ut->rtp_cb.track_idx);
    }
}

/** RECORD：与 PLAY 相同，SETUP 已知 client_port 时接受 NAT 源 IP */
static int record_udp_packet_accept(const zms_rtsp_session *rs,
                                    const zms_rtsp_session_udp_track *ut, const char *from_ip,
                                    uint16_t from_port, int rtcp_sock)
{
    if (!from_ip || !from_ip[0] || !from_port) {
        return 0;
    }
    if (!rs || !ut) {
        return 0;
    }
    if (rtcp_sock) {
        if (from_port == ut->peer_rtcp_port) {
            return 1;
        }
    } else if (from_port == ut->peer_rtp_port) {
        return 1;
    }
    return peer_ip_match(rs, from_ip);
}

static void record_udp_on_rtp_packet(ztk_udp_client *client, const void *data, size_t len,
                                     const char *from_ip, uint16_t from_port, void *user)
{
    zms_rtsp_session_udp_track *ut = (zms_rtsp_session_udp_track *)user;
    zms_rtsp_session *rs = ut ? ut->rtp_cb.rs : NULL;
    int track_idx = ut ? ut->rtp_cb.track_idx : -1;
    (void)client;

    if (!rs || track_idx < 0 || rs->mode != ZMS_RTSP_SESSION_MODE_RECORD || !data || len < 12) {
        return;
    }
    if (!record_udp_packet_accept(rs, ut, from_ip, from_port, 0)) {
        return;
    }

    udp_learn_rtp_peer(rs, ut, from_ip, from_port);
    zms_rtsp_session_note_udp_peer(rs, from_ip, track_idx);

    if (zms_rtp_is_rtcp(data, len)) {
        return;
    }

    zms_rtsp_session_record_input_rtp_raw(rs, track_idx, (const uint8_t *)data, len);
}

static void record_udp_on_rtcp_packet(ztk_udp_client *client, const void *data, size_t len,
                                      const char *from_ip, uint16_t from_port, void *user)
{
    zms_rtsp_session_udp_track *ut = (zms_rtsp_session_udp_track *)user;
    zms_rtsp_session *rs = ut ? ut->rtp_cb.rs : NULL;
    (void)client;
    if (!rs || !ut || !data || len < 8) {
        return;
    }
    if (!record_udp_packet_accept(rs, ut, from_ip, from_port, 1)) {
        return;
    }
    udp_learn_rtcp_peer(rs, ut, from_ip, from_port);
    (void)data;
    (void)len;
}

static void play_udp_sync_rtp_peer(zms_rtsp_session_udp_track *ut, const char *from_ip)
{
    if (!ut || !ut->rtp || !from_ip || !from_ip[0] || !ut->peer_rtp_port) {
        return;
    }
    ztk_udp_client_set_peer(ut->rtp, from_ip, ut->peer_rtp_port);
}

/** 同一播放器各 track 共享 IP（NAT 学习一次即可修正全部发送目标）*/
static void play_udp_sync_all_tracks(zms_rtsp_session *rs, const char *from_ip)
{
    int i;

    if (!rs || !from_ip || !from_ip[0]) {
        return;
    }
    zms_rtsp_session_note_udp_peer(rs, from_ip, -1);
    for (i = 0; i < ZMS_SDP_TRACK_MAX; ++i) {
        zms_rtsp_session_udp_track *ut = &rs->udp_tracks[i];

        if (ut->rtp && ut->peer_rtp_port) {
            ztk_udp_client_set_peer(ut->rtp, from_ip, ut->peer_rtp_port);
        }
        if (ut->rtcp && ut->peer_rtcp_port) {
            ztk_udp_client_set_peer(ut->rtcp, from_ip, ut->peer_rtcp_port);
        }
    }
}

static void play_udp_on_rtcp_packet(ztk_udp_client *client, const void *data, size_t len,
                                    const char *from_ip, uint16_t from_port, void *user)
{
    zms_rtsp_session_udp_track *ut = (zms_rtsp_session_udp_track *)user;
    zms_rtsp_session *rs = ut ? ut->rtp_cb.rs : NULL;
    (void)client;

    if (!rs || !ut || !data || len < 8) {
        return;
    }
    if (!play_udp_packet_accept(rs, ut, from_ip, from_port, 1)) {
        return;
    }
    udp_learn_rtcp_peer(rs, ut, from_ip, from_port);
    /* 客户端 RTCP 源用于校正 RTP 发送目标（NAT / WSL host / ffplay）。 */
    play_udp_sync_all_tracks(rs, from_ip);
    if (rs->mode == ZMS_RTSP_SESSION_MODE_PLAY && zms_rtp_is_rtcp(data, len)) {
        zms_rtsp_session_play_on_rtcp(rs, (const uint8_t *)data, len);
    }
}

static void play_udp_on_rtp_packet(ztk_udp_client *client, const void *data, size_t len,
                                   const char *from_ip, uint16_t from_port, void *user)
{
    zms_rtsp_session_udp_track *ut = (zms_rtsp_session_udp_track *)user;
    zms_rtsp_session *rs = ut ? ut->rtp_cb.rs : NULL;
    (void)client;
    (void)data;
    (void)len;
    if (!rs || !ut) {
        return;
    }
    if (!play_udp_packet_accept(rs, ut, from_ip, from_port, 0)) {
        return;
    }
    /* PLAY 打洞：客户端 RTP 端口首包（含 RTCP 混流） */
    udp_learn_rtp_peer(rs, ut, from_ip, from_port);
    play_udp_sync_rtp_peer(ut, from_ip);
}

static void udp_teardown_track(zms_rtsp_session *rs, int track_idx)
{
    if (!rs || track_idx < 0 || (unsigned)track_idx >= ZMS_SDP_TRACK_MAX) {
        return;
    }
    zms_rtsp_session_udp_track *ut = &rs->udp_tracks[track_idx];
    if (ut->rtp) {
        ztk_udp_client_destroy(ut->rtp);
        ut->rtp = NULL;
    }
    if (ut->rtcp) {
        ztk_udp_client_destroy(ut->rtcp);
        ut->rtcp = NULL;
    }
    if (ut->local_rtp_port) {
        zms_rtsp_transport_release_ports(ut->local_rtp_port);
        ut->local_rtp_port = 0;
        ut->local_rtcp_port = 0;
    }
    memset(ut, 0, sizeof(*ut));
}

void zms_rtsp_session_udp_teardown(zms_rtsp_session *rs)
{
    if (!rs) {
        return;
    }
    for (int i = 0; i < ZMS_SDP_TRACK_MAX; ++i) {
        udp_teardown_track(rs, i);
    }
}

void zms_rtsp_session_send_media(zms_rtsp_session *rs, uint8_t channel, const uint8_t *payload,
                                 size_t len)
{
    if (!rs || !payload || len == 0) {
        return;
    }

    if (rs->rtp_mode == ZMS_RTSP_RTP_UDP) {
        int track = (int)(channel / 2);
        int is_rtcp = channel % 2;
        if (track < 0 || track >= ZMS_SDP_TRACK_MAX) {
            return;
        }
        zms_rtsp_session_udp_track *ut = &rs->udp_tracks[track];
        ztk_udp_client *cli = is_rtcp ? ut->rtcp : ut->rtp;
        if (!cli) {
            if (!is_rtcp && track == 0 && rs->play_udp_video_send_fail < 3) {
                ztk_warn("RTSP PLAY UDP: drop track=%d ch=%u (socket not SETUP)", track,
                         (unsigned)channel);
            }
            if (!is_rtcp) {
                rs->play_udp_video_send_fail++;
            }
            return;
        }
        if (ztk_udp_client_send(cli, payload, len) != ZTK_OK) {
            if (!is_rtcp && rs->play_udp_video_send_fail < 5) {
                ztk_warn("RTSP PLAY UDP: send failed track=%d ch=%u len=%u", track,
                         (unsigned)channel, (unsigned)len);
            }
            if (!is_rtcp) {
                rs->play_udp_video_send_fail++;
            }
            return;
        }
        if (!is_rtcp && track == 0 && rs->play_udp_video_send_ok++ == 0) {
            const char *dest = rs->peer_ip[0] ? rs->peer_ip : "?";
            ztk_info("RTSP #%u PLAY UDP first video send: %u bytes -> %s:%u", rs->session_no,
                     (unsigned)len, dest, (unsigned)ut->peer_rtp_port);
        }
        return;
    }
    zms_rtsp_session_send_rtp_interleaved(rs, channel, payload, len);
}

static int setup_udp_track(zms_rtsp_session *rs, int track_idx, const char *transport_hdr,
                           char *extra, size_t extra_cap, const ztk_udp_client_ops_t *rtp_ops,
                           const ztk_udp_client_ops_t *rtcp_ops, const char *log_tag)
{
    if (!rs || !transport_hdr || !extra || extra_cap < 128 || track_idx < 0 ||
        (unsigned)track_idx >= ZMS_SDP_TRACK_MAX || !rtp_ops || !rtcp_ops) {
        return -1;
    }

    zms_rtsp_session_refresh_peer_ip(rs);
    if (!rs->peer_ip[0]) {
        ztk_warn("RTSP SETUP UDP: no peer ip");
        return -1;
    }

    uint16_t client_rtp = 0, client_rtcp = 0;
    if (parse_client_port(transport_hdr, &client_rtp, &client_rtcp) != 0) {
        ztk_warn("RTSP SETUP UDP: missing client_port");
        return -1;
    }

    zms_rtsp_session_udp_track *ut = &rs->udp_tracks[track_idx];
    if (ut->rtp) {
        return -1;
    }

    uint16_t local_rtp = 0, local_rtcp = 0;
    if (zms_rtsp_transport_acquire_ports(&local_rtp, &local_rtcp) != ZTK_OK) {
        ztk_warn("RTSP SETUP UDP: port pool exhausted");
        return -1;
    }

    ztk_udp_client_opts_t copts = {rs->poller, rtp_ops, ut};
    ut->rtp = ztk_udp_client_create(&copts);
    copts.ops = rtcp_ops;
    ztk_udp_client *rtcp_cli = ztk_udp_client_create(&copts);
    if (!ut->rtp || !rtcp_cli) {
        if (ut->rtp) {
            ztk_udp_client_destroy(ut->rtp);
            ut->rtp = NULL;
        }
        if (rtcp_cli) {
            ztk_udp_client_destroy(rtcp_cli);
        }
        zms_rtsp_transport_release_ports(local_rtp);
        return -1;
    }
    ut->rtcp = rtcp_cli;
    ut->rtp_cb.rs = rs;
    ut->rtp_cb.track_idx = track_idx;
    ut->peer_rtp_port = client_rtp;
    ut->peer_rtcp_port = client_rtcp;
    ut->local_rtp_port = local_rtp;
    ut->local_rtcp_port = local_rtcp;

    /*
     * 服务端始终 bind 0.0.0.0：WSL NAT 下 TCP peer 常为 127.0.0.1，但 Windows
     * ffplay 的 RTCP 从宿主机地址打入；若绑 loopback 则收不到打洞包。
     */
    if (ztk_udp_client_bind(ut->rtp, "0.0.0.0", local_rtp, 1) != ZTK_OK ||
        ztk_udp_client_bind(ut->rtcp, "0.0.0.0", local_rtcp, 1) != ZTK_OK) {
        udp_teardown_track(rs, track_idx);
        return -1;
    }

    ztk_udp_client_set_peer(ut->rtp, rs->peer_ip, client_rtp);
    ztk_udp_client_set_peer(ut->rtcp, rs->peer_ip, client_rtcp);

    uint8_t ich = (uint8_t)(track_idx * 2);
    if (track_idx == 0) {
        rs->video_rtp_ch = ich;
        rs->video_rtcp_ch = (uint8_t)(ich + 1);
    } else if (track_idx == 1) {
        rs->audio_rtp_ch = ich;
        rs->audio_rtcp_ch = (uint8_t)(ich + 1);
    }

    uint32_t ssrc = (track_idx == 0) ? rs->video_rtp_ssrc : rs->audio_rtp_ssrc;
    if (!ssrc) {
        ssrc = (track_idx == 0) ? 0x12345678u : 0x87654321u;
    }

    int n = snprintf(extra, extra_cap,
                     "Transport: RTP/AVP/UDP;unicast;"
                     "client_port=%u-%u;"
                     "server_port=%u-%u;"
                     "ssrc=%08X\r\n"
                     "Session: %s;timeout=60\r\n",
                     (unsigned)client_rtp, (unsigned)client_rtcp, (unsigned)local_rtp,
                     (unsigned)local_rtcp, ssrc, rs->session_id);
    if (n < 0 || (size_t)n >= extra_cap) {
        udp_teardown_track(rs, track_idx);
        return -1;
    }

    ztk_info("RTSP SETUP 200 UDP %s: track=%d client=%u-%u server=%u-%u peer=%s ch=%u-%u", log_tag,
             track_idx, (unsigned)client_rtp, (unsigned)client_rtcp, (unsigned)local_rtp,
             (unsigned)local_rtcp, rs->peer_ip, (unsigned)ich, (unsigned)(ich + 1));
    return 0;
}

int zms_rtsp_session_setup_udp_record(zms_rtsp_session *rs, int track_idx,
                                      const char *transport_hdr, char *extra, size_t extra_cap)
{
    static const ztk_udp_client_ops_t rtp_ops = {record_udp_on_rtp_packet};
    static const ztk_udp_client_ops_t rtcp_ops = {record_udp_on_rtcp_packet};
    return setup_udp_track(rs, track_idx, transport_hdr, extra, extra_cap, &rtp_ops, &rtcp_ops,
                           "RECORD");
}

int zms_rtsp_session_setup_udp_play(zms_rtsp_session *rs, int track_idx, const char *transport_hdr,
                                    char *extra, size_t extra_cap)
{
    static const ztk_udp_client_ops_t rtp_ops = {play_udp_on_rtp_packet};
    static const ztk_udp_client_ops_t rtcp_ops = {play_udp_on_rtcp_packet};
    return setup_udp_track(rs, track_idx, transport_hdr, extra, extra_cap, &rtp_ops, &rtcp_ops,
                           "PLAY");
}
