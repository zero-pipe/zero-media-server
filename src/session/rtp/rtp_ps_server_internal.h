#ifndef ZMS_SRC_SESSION_RTP_PS_SERVER_INTERNAL_H
#define ZMS_SRC_SESSION_RTP_PS_SERVER_INTERNAL_H

#include "zms/session/rtp/rtp_ps_server.h"
#include "zms/live/ingest/common/stream_ingest.h"
#include "zms/media/container/mpegps/mpegps_demuxer.h"
#include "ztk/net/tcp_client.h"
#include "ztk/net/tcp_server.h"
#include "ztk/net/udp_server.h"
#include <stdint.h>

struct ztk_poller_pool;
typedef struct ztk_poller_pool ztk_poller_pool;

struct zms_rtp_ps_server_slot {
    char key[256];
    char app[ZMS_APP_MAX];
    char stream[ZMS_STREAM_MAX];
    uint16_t port;
    int payload_type;
    uint32_t ssrc_filter;
    int enable_ssrc_filter;
    int tcp_mode;
    ztk_poller *poller;
    ztk_poller_pool *poller_pool;
    ztk_udp_server *udp;
    ztk_tcp_server *tcp_srv;
    ztk_tcp_session *tcp_sess;
    ztk_tcp_client *tcp_cli;
    char tcp_peer_ip[64];
    uint16_t tcp_peer_port;
    uint8_t *tcp_asm;
    size_t tcp_asm_len;
    size_t tcp_asm_cap;
    zms_live_ingest *ingress;
    zms_media_source *source;
    void *rtp_ps_decoder;
    zms_mpegps_demuxer *ps_demuxer;
    uint8_t *rtp_ps_assembly;
    size_t rtp_ps_assembly_cap;
    int publish_started;
    unsigned recv_pkts;
    unsigned raw_udp_pkts;
};

zms_rtp_ps_server_slot *zms_rtp_ps_server_slot_from_udp(ztk_udp_server *udp);

#endif /* ZMS_SRC_SESSION_RTP_PS_SERVER_INTERNAL_H */
