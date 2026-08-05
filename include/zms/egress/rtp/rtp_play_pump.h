#ifndef ZMS_EGRESS_RTP_PLAY_PUMP_H
#define ZMS_EGRESS_RTP_PLAY_PUMP_H

/**
 * @file rtp_play_pump.h
 * @brief 共享 RTP 播放泵与发送队列（RTSP PLAY / WebRTC WHEP 数据面）。
 *
 * 与 @ref zms_rtp_muxer（slot RTP）配合。RTSP 会话负责信令并实现 TCP interleaved 写回调。
 *
 * Copyright (c) zero-media-server
 */
#include "zms/engine/stream/stream_hub.h"
#include "zms/egress/egress_pacing.h"
#include "zms/egress/rtp/rtp_muxer.h"
#include "zms/egress/egress_source.h"
#include "zms/zms_export.h"
#include "ztk/ztk_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct zms_egress_pipeline;

struct zms_gop_reader;
struct zms_vod_buffer_reader;
struct zms_vod_play_lane;
struct zms_vod_reader;
struct ztk_poller;

#define ZMS_RTP_PLAY_RTPQ_CAP 512u
#define ZMS_RTP_PLAY_RTPQ_HIGH_WATER 128u
#define ZMS_RTP_PLAY_FRAME_BUDGET 32
#define ZMS_RTP_PLAY_FRAME_BUDGET_LIVE 8u
#define ZMS_RTP_PLAY_FRAME_BUDGET_SEEK ZMS_EGRESS_FRAME_BUDGET_CATCHUP
#define ZMS_RTP_PLAY_CATCHUP_LAG ZMS_EGRESS_CATCHUP_LAG
#define ZMS_RTP_PLAY_RESYNC_LAG ZMS_EGRESS_RESYNC_LAG
#define ZMS_RTP_PLAY_RESYNC_LAG_MAX ZMS_EGRESS_RESYNC_LAG_MAX
#define ZMS_RTP_PLAY_RESYNC_COOLDOWN_MS ZMS_EGRESS_RESYNC_COOLDOWN_MS
#define ZMS_RTP_PLAY_RTP_FLUSH 256
#define ZMS_RTP_VOD_CATCHUP_FRAMES_START ZMS_EGRESS_VOD_CATCHUP_START
#define ZMS_RTP_VOD_CATCHUP_FRAMES_SEEK ZMS_EGRESS_VOD_CATCHUP_SEEK
#define ZMS_RTP_VOD_PREFILL_LAG ZMS_EGRESS_VOD_PREFILL_LAG
/** 双轨 RTP-Info 最坏情况缓冲（peer + app + stream ×2）。 */
#define ZMS_RTSP_RTP_INFO_MAX 768u
/** PLAY 200 额外头（Session + Range/Scale + RTP-Info）。 */
#define ZMS_RTSP_PLAY_EXTRA_MAX 1024u

typedef struct zms_rtp_play_sender zms_rtp_play_sender;

/** PLAY Range：npt=12.3- / npt=1:00:00.000- / npt=now- */
ZMS_API int zms_rtsp_parse_range_npt_ms(const char *range_hdr, uint64_t *out_ms, int *out_now);
ZMS_API double zms_rtsp_parse_play_scale(const char *hdr, double current);
ZMS_API uint32_t zms_rtp_play_vod_anchor_ms(struct zms_vod_buffer_reader *rd, uint32_t seek_ms);

typedef struct zms_rtsp_play_rtp_info_args {
    const char *host;
    const char *app;
    const char *stream;
    const zms_media_source *source;
    zms_rtp_muxer *mux;
    uint32_t video_clock_hz;
    uint32_t audio_clock_hz;
    int audio_rate;
    uint32_t anchor_ms;
    /** 1 = VOD 线性 rtptime（anchor_ms * hz / 1000）；0 = 直播时钟映射。 */
    int vod_linear_rtp;
} zms_rtsp_play_rtp_info_args;

ZMS_API void zms_rtsp_play_format_rtp_info(const zms_rtsp_play_rtp_info_args *args, char *rtp_info,
                                           size_t cap);

typedef void (*zms_rtp_play_mux_rtp_cb)(zms_rtp_mux_track track, const uint8_t *rtp, size_t len,
                                        void *user);

typedef struct zms_rtp_play_mux_handle {
    struct zms_egress_pipeline *egress;
    zms_rtp_muxer *mux;
} zms_rtp_play_mux_handle;

typedef struct zms_rtp_play_mux_opts {
    zms_rtp_muxer_opts mux_opts;
    zms_egress_source *reader;
    zms_rtp_play_mux_rtp_cb on_rtp;
    void *user;
} zms_rtp_play_mux_opts;

ZMS_API ztk_err_t zms_rtp_play_mux_create(zms_rtp_play_mux_handle *out,
                                          const zms_rtp_play_mux_opts *opts);
ZMS_API void zms_rtp_play_mux_destroy(zms_rtp_play_mux_handle *h);

typedef struct zms_rtp_play_run_ctx {
    zms_rtp_muxer *mux;
    struct zms_egress_pipeline *egress;
    zms_rtp_play_sender *sender;
    struct zms_gop_reader *gop_reader;
    struct zms_vod_buffer_reader *vod_reader;
    struct zms_vod_play_lane *vod_lane;
    const zms_media_source *source;
    int *close_pending;
    int *destroy_scheduled;
    int *play_config_pending;
    int *play_live_catchup;
    uint64_t *play_lag_resync_ms;
    unsigned session_no;
} zms_rtp_play_run_ctx;

ZMS_API int zms_rtp_play_frame_budget(const zms_rtp_play_run_ctx *ctx);
ZMS_API int zms_rtp_play_kick_budget(const zms_rtp_play_run_ctx *ctx);
ZMS_API int zms_rtp_play_run_pump(zms_rtp_play_run_ctx *ctx, int budget, int flush);

typedef struct zms_rtp_play_vod_seek_state {
    zms_rtp_muxer *mux;
    zms_rtp_play_sender *sender;
    struct zms_vod_buffer_reader *vod_reader;
    uint64_t seek_ms;
    double scale;
    int is_replay;
    int was_paused;
    int did_seek;
} zms_rtp_play_vod_seek_state;

ZMS_API void zms_rtp_play_apply_vod_seek(zms_rtp_play_vod_seek_state *st);

ZMS_API void zms_rtsp_play_format_vod_play_200(char *extra, size_t extra_cap,
                                               const char *session_id, double scale,
                                               uint64_t seek_ms, double vod_dur_sec,
                                               const char *rtp_info);
ZMS_API void zms_rtsp_play_format_live_play_200(char *extra, size_t extra_cap,
                                                const char *session_id, const char *rtp_info);

/** 写 interleaved 帧：$ + channel + length + payload。 */
typedef void (*zms_rtp_play_write_fn)(void *user, uint8_t channel, const uint8_t *payload,
                                      size_t len);

ZMS_API zms_rtp_play_sender *zms_rtp_play_sender_create(zms_rtp_play_write_fn write, void *user,
                                                        unsigned cap);
ZMS_API void zms_rtp_play_sender_destroy(zms_rtp_play_sender *s);
ZMS_API void zms_rtp_play_sender_reset(zms_rtp_play_sender *s);
ZMS_API void zms_rtp_play_sender_set_session_no(zms_rtp_play_sender *s, unsigned session_no);
/** 绑定 I/O poller，使 RTP 发送队列使用 ztk_buf_alloc_local。 */
ZMS_API void zms_rtp_play_sender_set_poller(zms_rtp_play_sender *s, struct ztk_poller *poller);
ZMS_API size_t zms_rtp_play_sender_pending(const zms_rtp_play_sender *s);

/**
 * @param queued 1 = PLAY 期间入队（避免编码器回调重入 flush）。
 */
ZMS_API void zms_rtp_play_sender_submit(zms_rtp_play_sender *s, int queued, uint8_t channel,
                                        const uint8_t *payload, size_t len);

ZMS_API void zms_rtp_play_sender_flush(zms_rtp_play_sender *s, int budget, int close_pending);

/** 直播 bootstrap：发 config、seek_live_idr，首个关键帧 insertConfig；在 sync 点锁定 epoch。 */
ZMS_API int zms_rtp_play_bootstrap_live(zms_rtp_muxer *mux, const zms_media_source *src,
                                        struct zms_gop_reader *reader, unsigned session_no);

/** 直播边缘 bootstrap：仅 stream config + seek_live_idr（WebRTC WHEP 低延迟）。 */
ZMS_API int zms_rtp_play_bootstrap_live_edge(zms_rtp_muxer *mux, const zms_media_source *src,
                                             struct zms_gop_reader *reader, unsigned session_no);

/** VOD bootstrap：发 config，从头读 fifo；有 lane 时优先 lane fifo。 */
ZMS_API int zms_rtp_play_bootstrap_vod(zms_rtp_muxer *mux, const zms_media_source *src,
                                       struct zms_vod_buffer_reader *reader, unsigned session_no,
                                       struct zms_vod_play_lane *lane, uint32_t anchor_ms);

typedef struct zms_rtp_play_pump {
    zms_rtp_muxer *mux;
    zms_rtp_play_sender *sender;
    struct zms_gop_reader *gop_reader;
    struct zms_vod_buffer_reader *vod_reader;
    struct zms_vod_reader *vod_demux;
    const zms_media_source *source;
    /** 会话 close/destroy 标志；每轮 pump 检查。 */
    const int *close_flag;
    const int *destroy_flag;
    int *config_pending;
    int *live_catchup_done;
    uint64_t *live_resync_at_ms;
    unsigned session_no;
    struct zms_egress_pipeline *egress;
} zms_rtp_play_pump;

/** 读 ring/fifo、mux、flush 发送队列；返回本轮处理的帧数。 */
ZMS_API int zms_rtp_play_pump_run(zms_rtp_play_pump *p, int frame_budget, int rtp_flush_budget);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_EGRESS_RTP_PLAY_PUMP_H */
