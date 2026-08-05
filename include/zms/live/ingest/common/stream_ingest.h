#ifndef ZMS_LIVE_INGEST_COMMON_STREAM_INGEST_H
#define ZMS_LIVE_INGEST_COMMON_STREAM_INGEST_H

/**
 * @file stream_ingest.h
 * @brief 直播入站汇聚：归一化时间戳并写入 gop_queue。
 *
 * RTMP、RTSP RECORD、SRT publish、代理拉流等直播入站路径均在此汇聚。
 * 对直播流应用 @ref zms_media_timeline，将容器载荷解为 ES 后写入 gop_queue。
 *
 * Copyright (c) zero-media-server
 */
#include "zms/engine/stream/stream_hub.h"
#include "zms/engine/frame.h"
#include "zms/media/container/demux_pipeline.h"
#include "zms/engine/media/media_limits.h"
#include "zms/live/ingest/common/protocol_opts.h"
#include "zms/zms_export.h"
#include "ztk/ztk_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ztk_poller;

#define ZMS_LIVE_INGEST_WORK_BUF ZMS_MEDIA_IO_BUF_SIZE
#define ZMS_LIVE_INGEST_SCRATCH_PAD 256u
#define ZMS_VIDEO_WIDTH_MIN_VALID 64u

typedef struct zms_live_ingest zms_live_ingest;

/** 以精确 app/stream 创建入站（代理与测试）。 */
ZMS_API zms_live_ingest *zms_live_ingest_create(const char *app, const char *stream,
                                                const zms_protocol_opts *opts);

/** 创建推流入站；服务端可能追加唯一 stream 后缀。 */
ZMS_API zms_live_ingest *zms_live_ingest_create_publish(const char *app,
                                                        const char *stream_requested,
                                                        const zms_protocol_opts *opts);

/** 以显式 schema 创建推流入站（如 ZMS_SCHEMA_SRT）。 */
ZMS_API zms_live_ingest *zms_live_ingest_create_publish_schema(const char *schema, const char *app,
                                                               const char *stream_requested,
                                                               const zms_protocol_opts *opts);

/** 绑定到已注册的媒体源。 */
ZMS_API zms_live_ingest *zms_live_ingest_bind(zms_media_source *src);
ZMS_API void zms_live_ingest_destroy(zms_live_ingest *in);
/** 绑定会话 poller：工作区 / large / hevc_au 走 poller 本地池（须在首次 input 前调用） */
ZMS_API void zms_live_ingest_set_poller(zms_live_ingest *in, struct ztk_poller *poller);
ZMS_API zms_media_source *zms_live_ingest_source(zms_live_ingest *in);
ZMS_API void zms_live_ingest_reset(zms_live_ingest *in);
/** 仅重置入站状态（demux/timeline/config）；保留 gop_queue 供活跃读者。 */
ZMS_API void zms_live_ingest_reset_upstream(zms_live_ingest *in);

ZMS_API ztk_err_t zms_live_ingest_input_h264_annexb(zms_live_ingest *in, const uint8_t *annexb,
                                                    size_t len, uint32_t dts_ms, uint32_t pts_ms,
                                                    int keyframe);

ZMS_API ztk_err_t zms_live_ingest_input_aac_es(zms_live_ingest *in, const uint8_t *aac, size_t len,
                                               uint32_t dts_ms);

ZMS_API ztk_err_t zms_live_ingest_input_h265_annexb(zms_live_ingest *in, const uint8_t *annexb,
                                                    size_t len, uint32_t dts_ms, uint32_t pts_ms,
                                                    int keyframe);

ZMS_API ztk_err_t zms_live_ingest_input_g711_es(zms_live_ingest *in, zms_codec_id codec,
                                                const uint8_t *g711, size_t len, uint32_t dts_ms);

/** 预构建 @ref zms_frame 的统一入站入口。 */
ZMS_API ztk_err_t zms_live_ingest_input_frame(zms_live_ingest *in, const zms_frame *frame);

ZMS_API ztk_err_t zms_live_ingest_input_rtmp_video(zms_live_ingest *in, uint32_t tag_dts_ms,
                                                   const void *body, size_t body_len);

ZMS_API ztk_err_t zms_live_ingest_input_rtmp_audio(zms_live_ingest *in, uint32_t tag_dts_ms,
                                                   const void *body, size_t body_len);

/** RTMP demux 管线；协议层拥有，入站借用。 */
ZMS_API zms_demux_pipeline *zms_live_ingest_rtmp_demux_create(zms_live_ingest *in);
ZMS_API void zms_live_ingest_rtmp_demux_release(zms_live_ingest *in);

ZMS_API ztk_err_t zms_live_ingest_set_aac_config_hex(zms_live_ingest *in, const char *config_hex);
ZMS_API ztk_err_t zms_live_ingest_ensure_aac_config(zms_live_ingest *in, int sample_rate,
                                                    int channels);
ZMS_API void zms_live_ingest_set_audio_codec(zms_live_ingest *in, zms_codec_id codec,
                                             uint32_t sample_rate);
ZMS_API void zms_live_ingest_set_rtp_clocks(zms_live_ingest *in, uint32_t video_clock_hz,
                                            zms_codec_id audio_codec, uint32_t audio_clock_hz);
/** 放宽 stamp_revise 增量钳制（默认 300ms）。 */
ZMS_API void zms_live_ingest_set_stamp_max_delta(zms_live_ingest *in, uint32_t max_delta_ms);
/** MPEG-TS/SRT：PES 音/视频 PTS 原点不同；关闭 300ms A/V 钳制（默认开启）。 */
ZMS_API void zms_live_ingest_set_stamp_av_clamp(zms_live_ingest *in, int enabled);
/** MPEG-TS/SRT：demux PTS/DTS 毫秒直接写入 gop_queue（共用 PES 时间轴）。 */
ZMS_API void zms_live_ingest_set_timeline_linear_ms(zms_live_ingest *in, int on);
/** MPEG-TS/SRT：首个 IDR 前延迟 gop_queue video_config（避免 IDR 前 GOP 缓存）。 */
ZMS_API void zms_live_ingest_set_defer_gop_vcfg(zms_live_ingest *in, int on);
ZMS_API ztk_err_t zms_live_ingest_set_h264_sps_pps(zms_live_ingest *in, const uint8_t *sps,
                                                   size_t sps_len, const uint8_t *pps,
                                                   size_t pps_len);
ZMS_API ztk_err_t zms_live_ingest_set_h265_vps_sps_pps(zms_live_ingest *in, const uint8_t *vps,
                                                       size_t vps_len, const uint8_t *sps,
                                                       size_t sps_len, const uint8_t *pps,
                                                       size_t pps_len);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_LIVE_INGEST_COMMON_STREAM_INGEST_H */
