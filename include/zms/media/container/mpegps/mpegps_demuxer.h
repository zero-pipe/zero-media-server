#ifndef ZMS_CONTAINER_MPEGPS_MPEGPS_DEMUXER_H
#define ZMS_CONTAINER_MPEGPS_MPEGPS_DEMUXER_H

/**
 * @file mpegps_demuxer.h
 * @brief MPEG-PS ES 解复用（GB28181 RTP/PS 接入路径）。
 */
#include "zms/media/codec/payload/payload_registry.h"
#include "zms/zms_export.h"
#include "ztk/ztk_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zms_mpegps_demuxer zms_mpegps_demuxer;

typedef void (*zms_mpegps_h264_ps_fn)(const uint8_t *sps, size_t sps_len, const uint8_t *pps,
                                      size_t pps_len, void *user);

typedef struct zms_mpegps_demuxer_opts {
    zms_payload_frame_cb on_frame;
    zms_mpegps_h264_ps_fn on_h264_ps;
    void *user;
} zms_mpegps_demuxer_opts;

ZMS_API zms_mpegps_demuxer *zms_mpegps_demuxer_create(const zms_mpegps_demuxer_opts *opts);
ZMS_API void zms_mpegps_demuxer_destroy(zms_mpegps_demuxer *d);

/** 喂入重组好的 PS 字节流（来自 RTP/PS payload）。 */
ZMS_API ztk_err_t zms_mpegps_demuxer_feed(zms_mpegps_demuxer *d, const uint8_t *data, size_t len);
ZMS_API ztk_err_t zms_mpegps_demuxer_flush(zms_mpegps_demuxer *d);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_CONTAINER_MPEGPS_MPEGPS_DEMUXER_H */
