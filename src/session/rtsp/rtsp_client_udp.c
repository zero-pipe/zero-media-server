#include "session/rtsp/rtsp_client_internal.h"
#include "zms/media/wire/rtp_packet.h"
#include "zms/session/rtp/rtcp.h"
#include "ztk/util/log.h"
#include <stdio.h>
#include <string.h>

static int peer_ip_loopback(const char *host)
{
    return host && (strncmp(host, "127.", 4) == 0 || strcmp(host, "localhost") == 0 ||
                    strcmp(host, "::1") == 0);
}

static void udp_learn_rtp_peer(zms_rtsp_client_udp_track *ut, const char *from_ip,
                               uint16_t from_port)
{
    if (!ut || !ut->rtp || !from_ip || !from_port) {
        return;
    }
    if (ut->peer_rtp != from_port) {
        ut->peer_rtp = from_port;
        ztk_udp_client_set_peer(ut->rtp, from_ip, from_port);
        ztk_info("RTSP client UDP NAT rtp track=%d peer %s:%u", ut->track_idx, from_ip,
                 (unsigned)from_port);
    }
}

static void client_udp_on_rtp(ztk_udp_client *client, const void *data, size_t len,
                              const char *from_ip, uint16_t from_port, void *user)
{
    zms_rtsp_client_udp_track *ut = (zms_rtsp_client_udp_track *)user;
    zms_rtsp_client *c = ut ? ut->owner : NULL;
    (void)client;
    if (!c || !ut || c->stopping || !data || len < 12) {
        return;
    }
    if (zms_rtp_is_rtcp(data, len)) {
        return;
    }
    udp_learn_rtp_peer(ut, from_ip, from_port);
    if (c->receiver) {
        (void)zms_rtp_receiver_input(c->receiver, ut->track_idx, (const uint8_t *)data, len);
    }
}

static void client_udp_on_rtcp(ztk_udp_client *client, const void *data, size_t len,
                               const char *from_ip, uint16_t from_port, void *user)
{
    zms_rtsp_client_udp_track *ut = (zms_rtsp_client_udp_track *)user;
    (void)client;
    (void)data;
    (void)len;
    if (!ut || !ut->rtcp || !from_ip || !from_port) {
        return;
    }
    if (ut->peer_rtcp != from_port) {
        ut->peer_rtcp = from_port;
        ztk_udp_client_set_peer(ut->rtcp, from_ip, from_port);
    }
}

static void udp_teardown_track(zms_rtsp_client *c, unsigned track_idx)
{
    if (!c || track_idx >= ZMS_RTSP_CLIENT_UDP_MAX) {
        return;
    }
    zms_rtsp_client_udp_track *ut = &c->udp[track_idx];
    if (ut->rtp) {
        ztk_udp_client_destroy(ut->rtp);
        ut->rtp = NULL;
    }
    if (ut->rtcp) {
        ztk_udp_client_destroy(ut->rtcp);
        ut->rtcp = NULL;
    }
    if (ut->local_rtp) {
        zms_rtsp_transport_release_ports(ut->local_rtp);
        ut->local_rtp = 0;
        ut->local_rtcp = 0;
    }
    memset(ut, 0, sizeof(*ut));
}

void rtsp_client_udp_teardown(zms_rtsp_client *c)
{
    if (!c) {
        return;
    }
    for (unsigned i = 0; i < ZMS_RTSP_CLIENT_UDP_MAX; ++i) {
        udp_teardown_track(c, i);
    }
}

ztk_err_t rtsp_client_udp_prepare_track(zms_rtsp_client *c, unsigned track_idx)
{
    if (!c || track_idx >= ZMS_RTSP_CLIENT_UDP_MAX) {
        return ZTK_ERR_INVALID;
    }

    zms_rtsp_client_udp_track *ut = &c->udp[track_idx];
    if (ut->active) {
        return ZTK_OK;
    }

    uint16_t local_rtp = 0, local_rtcp = 0;
    if (zms_rtsp_transport_acquire_ports(&local_rtp, &local_rtcp) != ZTK_OK) {
        return ZTK_ERR_IO;
    }

    static const ztk_udp_client_ops_t rtp_ops = {client_udp_on_rtp};
    static const ztk_udp_client_ops_t rtcp_ops = {client_udp_on_rtcp};

    ut->owner = c;
    ut->track_idx = (int)track_idx;
    ztk_udp_client_opts_t copts = {c->opts.poller, &rtp_ops, ut};
    ut->rtp = ztk_udp_client_create(&copts);
    copts.ops = &rtcp_ops;
    ut->rtcp = ztk_udp_client_create(&copts);
    if (!ut->rtp || !ut->rtcp) {
        udp_teardown_track(c, track_idx);
        return ZTK_ERR_NOMEM;
    }

    const char *bind_host = peer_ip_loopback(c->url.host) ? c->url.host : "0.0.0.0";
    if (ztk_udp_client_bind(ut->rtp, bind_host, local_rtp, 1) != ZTK_OK ||
        ztk_udp_client_bind(ut->rtcp, bind_host, local_rtcp, 1) != ZTK_OK) {
        udp_teardown_track(c, track_idx);
        return ZTK_ERR_IO;
    }

    ut->local_rtp = local_rtp;
    ut->local_rtcp = local_rtcp;
    ut->active = 1;
    ut->rtp_ssrc = (track_idx == 0) ? 0x12345678u : 0x87654321u;
    return ZTK_OK;
}

int rtsp_client_udp_build_setup_extra(zms_rtsp_client *c, unsigned track_idx, char *buf, size_t cap)
{
    if (!c || !buf || cap < 64 || track_idx >= ZMS_RTSP_CLIENT_UDP_MAX) {
        return -1;
    }
    zms_rtsp_client_udp_track *ut = &c->udp[track_idx];
    if (!ut->active) {
        return -1;
    }
    int n = snprintf(buf, cap, "Transport: RTP/AVP;unicast;client_port=%u-%u\r\n",
                     (unsigned)ut->local_rtp, (unsigned)ut->local_rtcp);
    return (n > 0 && (size_t)n < cap) ? n : -1;
}

void rtsp_client_udp_apply_setup_response(zms_rtsp_client *c, unsigned track_idx,
                                          const char *transport)
{
    if (!c || !transport || track_idx >= ZMS_RTSP_CLIENT_UDP_MAX) {
        return;
    }
    zms_rtsp_client_udp_track *ut = &c->udp[track_idx];
    if (!ut->active) {
        return;
    }

    uint16_t srv_rtp = 0, srv_rtcp = 0;
    if (zms_rtsp_transport_parse_server_port(transport, &srv_rtp, &srv_rtcp) != 0) {
        ztk_warn("RTSP client SETUP UDP: missing server_port track=%u", track_idx);
        return;
    }
    uint32_t ssrc = 0;
    if (zms_rtsp_transport_parse_ssrc(transport, &ssrc) == 0) {
        ut->rtp_ssrc = ssrc;
    }

    ut->peer_rtp = srv_rtp;
    ut->peer_rtcp = srv_rtcp;
    ztk_udp_client_set_peer(ut->rtp, c->url.host, srv_rtp);
    ztk_udp_client_set_peer(ut->rtcp, c->url.host, srv_rtcp);
    ztk_info("RTSP client SETUP UDP ok track=%u client=%u-%u server=%u-%u", track_idx,
             (unsigned)ut->local_rtp, (unsigned)ut->local_rtcp, (unsigned)srv_rtp,
             (unsigned)srv_rtcp);
}

void rtsp_client_udp_on_play(zms_rtsp_client *c)
{
    if (!c) {
        return;
    }
    uint8_t rtcp[64];
    for (unsigned i = 0; i < c->session.track_count && i < ZMS_RTSP_CLIENT_UDP_MAX; ++i) {
        zms_rtsp_client_udp_track *ut = &c->udp[i];
        if (!ut->active || !ut->rtcp) {
            continue;
        }
        size_t n = zms_rtcp_build_rr(rtcp, sizeof(rtcp), ut->rtp_ssrc, ut->rtp_ssrc, 0, 0, 0);
        if (n > 0) {
            (void)ztk_udp_client_send(ut->rtcp, rtcp, n);
        }
    }
}
