#ifndef ZMS_SRC_WEBRTC_SESSION_INTERNAL_H
#define ZMS_SRC_WEBRTC_SESSION_INTERNAL_H

#include "zms/media/codec/codec_id.h"
#include "zms/engine/media/media_limits.h"
#include "zms/engine/stream/stream_hub.h"
#include "zms/egress/egress_source.h"
#include "ztk/net/udp_server.h"
#include "ztk/poller/poller.h"
#include "ztk/util/buf.h"
#include "ztk/util/timer.h"
#include <stddef.h>
#include <stdint.h>

#define ZMS_WEBRTC_ICE_UFRAG_LEN 32
#define ZMS_WEBRTC_ICE_PWD_LEN 64
#define ZMS_WEBRTC_SESSION_ID_LEN 32
#define ZMS_WEBRTC_PEER_IP_LEN 64

typedef struct zms_webrtc_play_rtp_slot {
    uint8_t track;
    ztk_buf *buf;
} zms_webrtc_play_rtp_slot;

struct zms_egress_pipeline;
struct zms_rtp_muxer;
struct zms_webrtc_dtls;
struct zms_webrtc_srtp;
struct zms_webrtc_ice;
struct zms_live_ingest;

typedef enum zms_webrtc_session_mode {
    ZMS_WEBRTC_SESSION_PLAY = 0,
    ZMS_WEBRTC_SESSION_PUBLISH = 1,
} zms_webrtc_session_mode;

typedef struct zms_webrtc_session zms_webrtc_session;

struct zms_webrtc_session {
    char id[ZMS_WEBRTC_SESSION_ID_LEN];
    char app[ZMS_APP_MAX];
    char stream[ZMS_STREAM_MAX];
    zms_webrtc_session_mode mode;
    zms_media_source *source;
    zms_egress_source play;
    struct zms_live_ingest *ingest;
    ztk_udp_server *udp;
    ztk_poller *poller;
    uint16_t port;
    char peer_ip[ZMS_WEBRTC_PEER_IP_LEN];
    uint16_t peer_port;
    int peer_known;
    char local_ufrag[ZMS_WEBRTC_ICE_UFRAG_LEN + 1];
    char local_pwd[ZMS_WEBRTC_ICE_PWD_LEN + 1];
    char remote_ufrag[ZMS_WEBRTC_ICE_UFRAG_LEN + 1];
    char remote_pwd[ZMS_WEBRTC_ICE_PWD_LEN + 1];
    int dtls_ready;
    int dtls_as_client;
    int media_started;
    unsigned session_no;
    uint8_t video_pt;
    uint8_t audio_pt;
    zms_codec_id video_codec;
    zms_codec_id audio_codec;
    int audio_rate;
    int audio_channels;
    char video_mid[8];
    char audio_mid[8];
    int offer_audio_before_video;
    int offer_has_video;
    int offer_has_audio;
    int answer_has_video;
    int answer_has_audio;
    struct zms_webrtc_dtls *dtls;
    struct zms_webrtc_srtp *video_srtp;
    struct zms_webrtc_srtp *audio_srtp;
    struct zms_egress_pipeline *egress_pipe;
    struct zms_rtp_muxer *play_rtp_muxer;
    ztk_poller_timer *pump_timer;
    int live_catchup_done;
    uint64_t play_start_ms;
    unsigned rtcp_tick;
    uint16_t twcc_seq;
    int rtcp_video_sr_sent;
    int rtcp_audio_sr_sent;
    uint8_t *io_buf;
    size_t io_cap;
    zms_webrtc_play_rtp_slot *play_rtp_q;
    unsigned play_rtp_q_r;
    unsigned play_rtp_q_w;
    unsigned play_rtp_q_n;
    unsigned play_trace_seq;
    int play_pump_armed;
    int play_reader_attached;
    void *whip_ingress;
    struct zms_webrtc_ice *ice;
    uint8_t whip_h264_sps[256];
    uint8_t whip_h264_pps[256];
    size_t whip_h264_sps_len;
    size_t whip_h264_pps_len;
    int whip_h264_have_sprop;
    uint8_t play_h264_sps[256];
    uint8_t play_h264_pps[128];
    size_t play_h264_sps_len;
    size_t play_h264_pps_len;
    uint16_t play_h264_seq_bias;
};

zms_webrtc_session *zms_webrtc_session_create(zms_media_source *src, const char *app,
                                              const char *stream, ztk_poller *poller);
zms_webrtc_session *zms_webrtc_session_create_publish(const char *app, const char *stream,
                                                      ztk_poller *poller);
void zms_webrtc_session_destroy(zms_webrtc_session *s);
void zms_webrtc_session_teardown(zms_webrtc_session *s);
void zms_webrtc_session_destroy_all(void);

zms_webrtc_session *zms_webrtc_session_find(const char *id);

int zms_webrtc_session_io_buf_ensure(zms_webrtc_session *s);
void zms_webrtc_session_io_buf_release(zms_webrtc_session *s);

int zms_webrtc_session_build_answer(const zms_webrtc_session *s, const char *offer,
                                    size_t offer_len, char *answer, size_t answer_cap,
                                    size_t *answer_len);
int zms_webrtc_session_build_publish_answer(const zms_webrtc_session *s, const char *offer,
                                            size_t offer_len, char *answer, size_t answer_cap,
                                            size_t *answer_len);

void zms_webrtc_publish_apply_offer_h264_sprop(zms_webrtc_session *s);

void zms_webrtc_session_on_udp(zms_webrtc_session *s, const char *peer_ip, uint16_t peer_port,
                               const void *data, size_t len);

#endif /* ZMS_SRC_WEBRTC_SESSION_INTERNAL_H */
