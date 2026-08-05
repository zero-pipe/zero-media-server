#ifndef ZMS_EGRESS_PIPELINE_H
#define ZMS_EGRESS_PIPELINE_H

/**
 * @file egress_pipeline.h
 * @brief 统一出站泵：读帧并输出线格式字节。
 *
 * 经 zms_egress_source 附着读者。直播/VOD pacing 见 egress_pacing.h。
 * 线路径：RTP（RTSP/WebRTC）、FLV tag（RTMP/HTTP-FLV）。
 */
#include "zms/media/container/container_dispatcher.h"
#include "zms/egress/egress_live_policy.h"
#include "zms/vod/play/vod_flv_egress.h"
#include "zms/egress/egress_source.h"
#include "zms/media/wire_format.h"
#include "zms/egress/rtp/rtp_muxer.h"
#include "zms/zms_export.h"
#include "ztk/ztk_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct zms_vod_buffer_reader;
struct zms_vod_reader;
struct zms_media_source;
struct zms_mux_av_timeline;
struct zms_egress_clock;
struct ztk_poller;

typedef struct zms_egress_pipeline zms_egress_pipeline;

/** 原始线路输出（如 RTSP interleaved）。 */
typedef void (*zms_egress_wire_cb)(const uint8_t *data, size_t len, void *user);

/** FLV/RTMP tag 回调（msg_type 8=audio，9=video）。 */
typedef void (*zms_egress_flv_tag_cb)(uint8_t msg_type, uint32_t tag_dts_ms, const uint8_t *body,
                                      size_t len, void *user);

typedef struct zms_egress_flv_tag {
    uint8_t msg_type;
    uint32_t tag_dts_ms;
    const uint8_t *body;
    size_t len;
} zms_egress_flv_tag;

/** RTP mux 绑定：自有 mux（首选）或借用会话 mux。 */
typedef struct zms_egress_rtsp_bind {
    zms_rtp_muxer *mux;
    const zms_rtp_muxer_opts *opts;
    zms_rtp_mux_on_rtp on_rtp;
    void *user;
} zms_egress_rtsp_bind;

/** FLV/RTMP 出站：将 H.264/AAC（及同类）ES 重封装为 tag。 */
typedef struct zms_egress_flv_bind {
    struct zms_media_source *source;
    struct zms_mux_av_timeline *timeline;
    struct zms_egress_clock *play_clk;
    zms_egress_flv_tag_cb on_tag;
    void *user;
    int *video_armed;
} zms_egress_flv_bind;

typedef struct zms_egress_pipeline_opts {
    zms_wire_format_id wire;
    zms_container_id container;
    zms_egress_source *reader;
    zms_egress_wire_cb on_wire;
    void *user;
    const zms_egress_rtsp_bind *rtsp;
    const zms_egress_flv_bind *flv;
} zms_egress_pipeline_opts;

ZMS_API zms_egress_pipeline *zms_egress_pipeline_create(const zms_egress_pipeline_opts *opts);
ZMS_API void zms_egress_pipeline_destroy(zms_egress_pipeline *p);

/** 绑定 I/O poller，使 tag/ES 暂存走 per-poller 池。 */
ZMS_API void zms_egress_pipeline_bind_poller(zms_egress_pipeline *p, struct ztk_poller *pol);

/** 将直播 gop_queue pump 为 RTP。@return 本轮发出的帧/tag 数。 */
ZMS_API int zms_egress_pipeline_pump_live(zms_egress_pipeline *p, int frame_budget,
                                          int read_timeout_ms, unsigned session_no,
                                          const zms_egress_live_state *live);

ZMS_API int zms_egress_pipeline_pump_vod(zms_egress_pipeline *p,
                                         struct zms_vod_buffer_reader *vod_rd,
                                         struct zms_vod_reader *vod_demux, int frame_budget,
                                         unsigned session_no);

/** RTSP PLAY pump：@a vod_rd 非空走 VOD fifo，否则 live gop_queue → RTP。 */
ZMS_API int zms_egress_pipeline_pump_rtsp(zms_egress_pipeline *p,
                                          struct zms_vod_buffer_reader *vod_rd,
                                          struct zms_vod_reader *vod_demux, int frame_budget,
                                          int read_timeout_ms, unsigned session_no,
                                          const zms_egress_live_state *live);

ZMS_API int zms_egress_pipeline_pump_flv_live(zms_egress_pipeline *p, int frame_budget,
                                              int read_timeout_ms, unsigned session_no,
                                              const zms_egress_live_state *live);

/** 将每会话 FLV outbox flush 到 on_tag（RTMP play tick 末尾）。 */
ZMS_API int zms_egress_pipeline_flush_flv_outbox(zms_egress_pipeline *p, int tag_budget);

/** @return 已排队尚未分发的 FLV tag 数。 */
ZMS_API size_t zms_egress_pipeline_flv_outbox_pending(const zms_egress_pipeline *p);

/**
 * 拉取一条直播 FLV 媒体 tag（body 在下一次 pull/pump 前有效）。
 * @return 1 有 tag，0 无
 */
ZMS_API int zms_egress_pipeline_pull_flv_live(zms_egress_pipeline *p, zms_egress_flv_tag *out,
                                              int read_timeout_ms, unsigned session_no,
                                              const zms_egress_live_state *live);

/** 将 VOD fifo pump 为 FLV tag（paced）。@return 本轮 tag 数。 */
ZMS_API int zms_egress_pipeline_pump_flv_vod(zms_egress_pipeline *p,
                                             const zms_flv_vod_egress_bind *vcfg, int frame_budget,
                                             unsigned session_no);

/** @return 1 表示 VOD FLV tag 就绪，0 表示 pacing 未到期或空。 */
ZMS_API int zms_egress_pipeline_pull_flv_vod(zms_egress_pipeline *p,
                                             const zms_flv_vod_egress_bind *vcfg,
                                             zms_egress_flv_tag *out, unsigned session_no);

/** 滞后 resync 后：将读者 snap 到直播 GOP sync。 */
ZMS_API void zms_egress_pipeline_snap_live(zms_egress_pipeline *p);

ZMS_API void zms_egress_pipeline_jump_live(zms_egress_pipeline *p);
ZMS_API size_t zms_egress_pipeline_gop_lag(const zms_egress_pipeline *p);
ZMS_API zms_rtp_muxer *zms_egress_pipeline_rtsp_mux(const zms_egress_pipeline *p);

ZMS_API ztk_err_t zms_egress_pipeline_pump(zms_egress_pipeline *p, int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_EGRESS_PIPELINE_H */
