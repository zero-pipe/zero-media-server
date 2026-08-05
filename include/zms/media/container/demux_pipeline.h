#ifndef ZMS_CONTAINER_DEMUX_DEMUX_PIPELINE_H
#define ZMS_CONTAINER_DEMUX_DEMUX_PIPELINE_H

/**
 * @file demux_pipeline.h
 * @brief 容器 + payload 解复用管线 → zms_frame 回调。
 *
 * 公共接口保持精简：具体 mpegts/flv demuxer 实现在 .c 中。
 */
#include "zms/media/codec/codec_id.h"
#include "zms/media/container/container_registry.h"
#include "zms/media/codec/payload/payload_registry.h"
#include "zms/zms_export.h"
#include "ztk/ztk_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zms_demux_pipeline zms_demux_pipeline;
typedef struct zms_rtp_packet zms_rtp_packet;

/** 可选 H.264 参数集回调（MPEG-TS 入站）。 */
typedef void (*zms_demux_pipeline_h264_ps_fn)(const uint8_t *sps, size_t sps_len,
                                             const uint8_t *pps, size_t pps_len, void *user);

typedef struct zms_demux_pipeline_opts {
    zms_container_id container;
    zms_payload_frame_cb on_frame;
    zms_demux_pipeline_h264_ps_fn on_mpegts_h264_ps;
    void *user;
} zms_demux_pipeline_opts;

ZMS_API zms_demux_pipeline *zms_demux_pipeline_create(const zms_demux_pipeline_opts *opts);
ZMS_API void zms_demux_pipeline_destroy(zms_demux_pipeline *p);

ZMS_API void zms_demux_pipeline_set_track(zms_demux_pipeline *p, int track_index,
                                          zms_codec_id codec, uint32_t rtp_clock_hz);

ZMS_API ztk_err_t zms_demux_pipeline_feed(zms_demux_pipeline *p, const uint8_t *buf, size_t len);
ZMS_API void zms_demux_pipeline_flush(zms_demux_pipeline *p);
ZMS_API ztk_err_t zms_demux_pipeline_input_rtp(zms_demux_pipeline *p, int track_index,
                                               const zms_rtp_packet *pkt);
ZMS_API ztk_err_t zms_demux_pipeline_input_flv_tag(zms_demux_pipeline *p, int track_index,
                                                   uint8_t type_id, const uint8_t *body, size_t len,
                                                   uint32_t tag_dts_ms);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_CONTAINER_DEMUX_DEMUX_PIPELINE_H */
