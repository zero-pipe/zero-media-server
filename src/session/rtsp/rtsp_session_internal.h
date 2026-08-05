#ifndef ZMS_SRC_SESSION_RTSP_INTERNAL_H
#define ZMS_SRC_SESSION_RTSP_INTERNAL_H

#include "zms/media/container/demux_pipeline.h"
#include "zms/egress/egress_pipeline.h"
#include "zms/live/ingest/common/stream_ingest.h"
#include "zms/engine/gop/gop_queue.h"
#include "zms/vod/io/vod_buffer.h"
#include "zms/engine/media/media_limits.h"
#include "zms/engine/stream/stream_hub.h"
#include "zms/egress/egress_source.h"
#include "zms/session/rtsp/rtsp_parser.h"
#include "zms/egress/rtp/rtp_play_pump.h"
#include "zms/egress/rtp/rtp_muxer.h"
#include "zms/session/rtsp/rtsp_splitter.h"
#include "zms/session/rtsp/rtsp_transport.h"
#include "zms/session/rtsp/rtsp_sdp.h"
#include "zms/session/rtp/rtp_receiver.h"
#include "zms/util/buf_pool.h"
#include "ztk/net/tcp_server.h"
#include "ztk/net/udp_client.h"
#include "ztk/poller/poller.h"
#include <stdint.h>

/* 播放侧 VOD lane：此处不完整（LoD）；驱动它的 .c 需 include vod_play_lane.h。 */
typedef struct zms_vod_play_lane zms_vod_play_lane;

typedef enum zms_rtsp_session_mode {
    ZMS_RTSP_SESSION_MODE_IDLE,
    ZMS_RTSP_SESSION_MODE_PLAY,
    ZMS_RTSP_SESSION_MODE_RECORD,
} zms_rtsp_session_mode;

typedef struct zms_rtsp_session zms_rtsp_session;

typedef struct zms_rtsp_destroy_job {
    zms_rtsp_session *rs;
    unsigned token;
} zms_rtsp_destroy_job;

typedef struct zms_rtsp_session_udp_track {
    ztk_udp_client *rtp;
    ztk_udp_client *rtcp;
    uint16_t local_rtp_port;
    uint16_t local_rtcp_port;
    uint16_t peer_rtp_port;
    uint16_t peer_rtcp_port;
    struct {
        zms_rtsp_session *rs;
        int track_idx;
    } rtp_cb;
} zms_rtsp_session_udp_track;

struct zms_rtsp_session {
    unsigned session_no;
    ztk_tcp_session *tcp;
    zms_rtsp_splitter *splitter;
    zms_egress_source play;
    zms_gop_reader *gop_reader;
    zms_vod_buffer_reader *vod_reader;
    zms_vod_play_lane *vod_lane;
    zms_media_source *source;
    zms_live_ingest *ingress;
    zms_demux_pipeline *record_pipeline;
    zms_sdp_session publish_sdp;
    int publish_track_setup[ZMS_SDP_TRACK_MAX];
    char session_id[32];
    int auth_ok;
    char auth_nonce[64];
    char content_base[512];
    char app[ZMS_APP_MAX];
    char stream[ZMS_STREAM_MAX];
    unsigned cseq;
    zms_rtsp_session_mode mode;
    int logged_record;
    int logged_record_h264;
    unsigned record_rtp_count;
    uint8_t video_rtp_ch;
    uint8_t video_rtcp_ch;
    uint8_t audio_rtp_ch;
    uint8_t audio_rtcp_ch;
    uint32_t video_rtp_ssrc;
    uint32_t audio_rtp_ssrc;
    zms_rtp_muxer *play_rtp_muxer; /**< RTP 封装器（非 FLV/timeline） */
    zms_egress_pipeline *egress_pipe;
    int play_rtcp_tick;
    int play_boot_sent;
    int play_rtcp_boot_sent;
    int play_rtcp_video_sr_sent;
    int play_rtcp_audio_sr_sent;
    int play_paused;
    uint64_t play_seek_ms;
    double play_scale;
    /** 首屏 GOP 已发过，seek_live 一次；避免每 tick 再 jump_live */
    int play_live_catchup;
    uint64_t play_lag_resync_ms;
    int play_config_pending;
    int close_pending;
    int destroy_scheduled;
    zms_codec_id audio_codec;
    int audio_rate;
    int audio_channels;
    uint32_t video_clock_hz;
    uint32_t audio_clock_hz;
    int play_audio_setup;
    int play_video_setup;
    int play_reader_attached;
    uint64_t
        play_start_ms; /**< 播放开始 ztk_monotonic_ms()；供 on_play_stop 上报 duration_ms */
    unsigned destroy_token;
    zms_rtp_play_sender *play_sender;
    ztk_poller *poller;
    zms_rtsp_rtp_mode rtp_mode;
    char peer_ip[64];
    /** SDP o= / c= 对外公告地址（来自 zms_rtsp_service_opts.advertise_host） */
    char advertise_host[64];
    zms_rtsp_session_udp_track udp_tracks[ZMS_SDP_TRACK_MAX];
    zms_rtp_receiver *record_receiver;
    unsigned play_udp_video_send_ok;
    unsigned play_udp_video_send_fail;
    /** RTSP 信令响应缓冲（buf_pool，按需扩到 ZMS_MEDIA_IO_BUF_SIZE） */
    uint8_t *resp_buf;
    size_t resp_cap;
    uint8_t *play_interleaved_buf;
    size_t play_interleaved_cap;
};

typedef struct zms_rtsp_vod_play_ctx {
    zms_rtsp_session *rs;
    zms_media_source *src;
    uint64_t seek_ms;
    zms_vod_play_lane *lane;
    int is_replay;
    int was_paused;
    int did_seek;
    int range_parsed;
    int range_now;
    double scale;
    char range_req[160];
} zms_rtsp_vod_play_ctx;

typedef void (*zms_rtsp_method_fn)(zms_rtsp_session *rs, const zms_rtsp_message *msg);

void zms_rtsp_session_send_resp(zms_rtsp_session *rs, int code, const char *reason,
                                const char *extra, const char *body, size_t body_len);
void zms_rtsp_session_parse_url(const char *url, char *app, char *stream);
int zms_rtsp_session_match(zms_rtsp_session *rs, const char *session_hdr);
int zms_rtsp_session_setup_track_id(const char *url, const zms_sdp_session *sdp);
int zms_rtsp_session_record_any_track_setup(const zms_rtsp_session *rs);
int zms_rtsp_session_record_track_by_channel(const zms_rtsp_session *rs, uint8_t channel);
void zms_rtsp_session_load_audio_params(zms_rtsp_session *rs);
/** VOD 总时长（ms）：publisher 元数据 + 文件 probe 回退 */
uint64_t zms_rtsp_session_vod_duration_ms(const zms_rtsp_session *rs);
/** VOD 当前播放位置（ms）：优先已发送视频 RTP 时间 */
uint64_t zms_rtsp_session_vod_play_position_ms(const zms_rtsp_session *rs);
void zms_rtsp_session_build_sdp(zms_rtsp_session *rs, char *out, size_t cap);
void zms_rtsp_session_send_rtp_interleaved(zms_rtsp_session *rs, uint8_t channel,
                                           const uint8_t *rtp, size_t rtp_len);
void zms_rtsp_session_send_rtcp_srs(zms_rtsp_session *rs);
void zms_rtsp_session_write_interleaved(void *user, uint8_t channel, const uint8_t *payload,
                                        size_t len);
void zms_rtsp_session_publisher_kick(void *ctx, int force);

void zms_rtsp_session_handle_options(zms_rtsp_session *rs, const zms_rtsp_message *msg);
void zms_rtsp_session_handle_announce(zms_rtsp_session *rs, const zms_rtsp_message *msg);
void zms_rtsp_session_handle_describe(zms_rtsp_session *rs, const zms_rtsp_message *msg);
void zms_rtsp_session_handle_setup(zms_rtsp_session *rs, const zms_rtsp_message *msg);
void zms_rtsp_session_handle_record(zms_rtsp_session *rs, const zms_rtsp_message *msg);
void zms_rtsp_session_handle_play(zms_rtsp_session *rs, const zms_rtsp_message *msg);
void zms_rtsp_session_handle_pause(zms_rtsp_session *rs, const zms_rtsp_message *msg);
void zms_rtsp_session_handle_get_parameter(zms_rtsp_session *rs, const zms_rtsp_message *msg);
void zms_rtsp_session_handle_teardown(zms_rtsp_session *rs, const zms_rtsp_message *msg);

void zms_rtsp_session_on_message(const zms_rtsp_message *msg, void *user);
void zms_rtsp_session_on_rtp(uint8_t channel, const uint8_t *data, size_t len, void *user);
void zms_rtsp_session_play_on_rtcp(zms_rtsp_session *rs, const uint8_t *data, size_t len);
void zms_rtsp_session_record_payload_teardown(zms_rtsp_session *rs);
void zms_rtsp_session_record_on_rtp(zms_rtsp_session *rs, uint8_t channel,
                                    const zms_rtp_packet *pkt);
void zms_rtsp_session_record_on_rtp_track(zms_rtsp_session *rs, int track_idx,
                                          const zms_rtp_packet *pkt);
void zms_rtsp_session_record_input_rtp_raw(zms_rtsp_session *rs, int track_idx, const uint8_t *data,
                                           size_t len);
int zms_rtsp_session_setup_udp_record(zms_rtsp_session *rs, int track_idx,
                                      const char *transport_hdr, char *extra, size_t extra_cap);
int zms_rtsp_session_setup_udp_play(zms_rtsp_session *rs, int track_idx, const char *transport_hdr,
                                    char *extra, size_t extra_cap);
void zms_rtsp_session_udp_teardown(zms_rtsp_session *rs);
void zms_rtsp_session_send_media(zms_rtsp_session *rs, uint8_t channel, const uint8_t *payload,
                                 size_t len);
void zms_rtsp_session_record_receiver_ensure(zms_rtsp_session *rs);
void zms_rtsp_session_play_tick(zms_rtsp_session *rs);
void zms_rtsp_session_play_kick(zms_rtsp_session *rs);
ztk_err_t zms_rtsp_session_play_vod_lane_attach(zms_rtsp_session *rs, zms_media_source *src,
                                                uint64_t seek_ms);
void zms_rtsp_session_egress_close(zms_rtsp_session *rs);
void zms_rtsp_session_play_mux_destroy(zms_rtsp_session *rs);
void zms_rtsp_session_fill_play_ctx(zms_rtsp_session *rs, zms_rtp_play_run_ctx *ctx);
zms_rtsp_play_rtp_info_args zms_rtsp_session_rtp_info_args(zms_rtsp_session *rs, uint32_t anchor_ms,
                                                           int vod_linear);
void zms_rtsp_session_play_mux_create(zms_rtsp_session *rs);
void zms_rtsp_session_play_try_rtcp_boot(zms_rtsp_session *rs);
void zms_rtsp_session_send_rtcp_sr_tracks(zms_rtsp_session *rs, int send_video, int send_audio);
void zms_rtsp_session_vod_play_continue(zms_rtsp_session *rs, int is_replay, int was_paused,
                                        int did_seek, uint64_t seek_ms, double scale,
                                        const char *range_req, int range_parsed, int range_now);
int zms_rtsp_session_vod_open_try_async(zms_rtsp_vod_play_ctx *job);

void zms_rtsp_session_teardown(zms_rtsp_session *rs);
void zms_rtsp_session_schedule_destroy(zms_rtsp_session *rs, ztk_tcp_session *session);

#endif /* ZMS_SRC_SESSION_RTSP_INTERNAL_H */
