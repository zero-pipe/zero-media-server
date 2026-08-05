#ifndef ZMS_CONTAINER_MPEGTS_DEMUXER_H
#define ZMS_CONTAINER_MPEGTS_DEMUXER_H

/**
 * @file mpegts_demuxer.h
 * @brief MPEG-TS ES 解复用（SRT/HLS 接入路径）。
 */
#include "zms/zms_export.h"
#include "zms/media/codec/payload/payload_registry.h"
#include "ztk/ztk_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zms_mpegts_demuxer zms_mpegts_demuxer;

typedef void (*zms_mpegts_h264_ps_fn)(const uint8_t *sps, size_t sps_len, const uint8_t *pps,
                                      size_t pps_len, void *user);

typedef struct zms_mpegts_demuxer_opts {
    zms_payload_frame_cb on_frame;
    zms_mpegts_h264_ps_fn on_h264_ps;
    void *user;
} zms_mpegts_demuxer_opts;

ZMS_API zms_mpegts_demuxer *zms_mpegts_demuxer_create(const zms_mpegts_demuxer_opts *opts);
ZMS_API void zms_mpegts_demuxer_destroy(zms_mpegts_demuxer *d);

/** 喂入原始 TS 字节流（188 字节包无需对齐）。 */
ZMS_API ztk_err_t zms_mpegts_demuxer_feed(zms_mpegts_demuxer *d, const uint8_t *data, size_t len);
ZMS_API ztk_err_t zms_mpegts_demuxer_flush(zms_mpegts_demuxer *d);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_CONTAINER_MPEGTS_DEMUXER_H */
