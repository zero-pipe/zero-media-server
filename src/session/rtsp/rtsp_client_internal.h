#ifndef ZMS_SRC_SESSION_RTSP_CLIENT_INTERNAL_H
#define ZMS_SRC_SESSION_RTSP_CLIENT_INTERNAL_H

#include "zms/session/rtsp/rtsp_client.h"
#include "zms/session/rtsp/rtsp_transport.h"
#include "zms/session/rtsp/rtsp_splitter.h"
#include "zms/session/rtsp/rtsp_parser.h"
#include "zms/session/rtp/rtp_receiver.h"
#include "zms/media/codec/payload/payload_track.h"
#include "ztk/net/tcp_client.h"
#include "ztk/net/tls_client.h"
#include "ztk/net/udp_client.h"
#include "ztk/util/timer.h"
#include <stdint.h>

#define ZMS_RTSP_CLIENT_UDP_MAX ZMS_SDP_TRACK_MAX
/** scheme(5)+://+host(256)+:65535+path(512) 上界 */
#define ZMS_RTSP_URL_FULL_MAX 1024
/** 完整 URL + track control 拼接上界 */
#define ZMS_RTSP_SETUP_URL_MAX (ZMS_RTSP_URL_FULL_MAX + 128 + 8)

typedef enum zms_rtsp_client_state {
    ST_IDLE = 0,
    ST_CONNECTING,
    ST_OPTIONS,
    ST_DESCRIBE,
    ST_SETUP,
    ST_PLAY,
    ST_PLAYING,
    ST_ERROR,
} zms_rtsp_client_state;

typedef struct zms_rtsp_url {
    char host[256];
    uint16_t port;
    char path[512];
    char full[ZMS_RTSP_URL_FULL_MAX];
    char user[64];
    char pass[128];
    const char *scheme;
    int use_tls;
} zms_rtsp_url;

typedef struct zms_rtsp_client_udp_track {
    struct zms_rtsp_client *owner;
    int track_idx;
    ztk_udp_client *rtp;
    ztk_udp_client *rtcp;
    uint16_t local_rtp;
    uint16_t local_rtcp;
    uint16_t peer_rtp;
    uint16_t peer_rtcp;
    uint32_t rtp_ssrc;
    int active;
} zms_rtsp_client_udp_track;

struct zms_rtsp_client {
    zms_rtsp_client_opts opts;
    ztk_tcp_client *tcp;
    ztk_tls_client *tls;
    zms_rtsp_splitter *splitter;
    zms_rtp_receiver *receiver;
    zms_payload_track_bank payload;
    zms_sdp_session session;
    zms_rtsp_url url;
    zms_rtsp_client_state state;
    zms_rtsp_rtp_mode requested_rtp_mode;
    zms_rtsp_rtp_mode rtp_mode;
    int auto_udp_tried;
    int auth_retry_done;
    char digest_realm[128];
    char digest_nonce[128];
    unsigned cseq;
    char session_id[128];
    char content_base[512];
    unsigned setup_index;
    zms_rtsp_client_udp_track udp[ZMS_RTSP_CLIENT_UDP_MAX];
    int stopping;
    int teardown_sent;
    ztk_poller_timer *close_timer;
    int retry_count;
    int reconnect_delay_ms;
    int failed_count;
    ztk_poller_timer *retry_timer;
};

void rtsp_client_emit_rtp_frame(zms_rtsp_client *c, int track_index, const zms_rtp_packet *pkt);
void rtsp_client_fail(zms_rtsp_client *c, ztk_err_t err);

void rtsp_client_udp_teardown(zms_rtsp_client *c);
ztk_err_t rtsp_client_udp_prepare_track(zms_rtsp_client *c, unsigned track_idx);
void rtsp_client_udp_apply_setup_response(zms_rtsp_client *c, unsigned track_idx,
                                          const char *transport);
void rtsp_client_udp_on_play(zms_rtsp_client *c);
int rtsp_client_udp_build_setup_extra(zms_rtsp_client *c, unsigned track_idx, char *buf,
                                      size_t cap);

#endif /* ZMS_SRC_SESSION_RTSP_CLIENT_INTERNAL_H */
