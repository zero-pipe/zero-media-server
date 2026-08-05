#ifndef ZMS_CODEC_AV1_AV1_OVER_RTP_H
#define ZMS_CODEC_AV1_AV1_OVER_RTP_H

#include "zms/media/codec/payload/payload_registry.h"
#include "zms/zms_export.h"
#include "ztk/ztk_errno.h"
#include "zms/engine/frame.h"
#include "zms/media/wire/rtp_packet.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*zms_av1_over_rtp_on_frame_cb)(const zms_frame *frame, void *user);
typedef struct zms_av1_over_rtp_demuxer zms_av1_over_rtp_demuxer;

typedef struct zms_av1_over_rtp_demuxer_opts {
    zms_av1_over_rtp_on_frame_cb on_frame_cb;
    void *user;
    uint32_t rtp_clock_hz;
    int jitter_ms;
    int payload_type;
} zms_av1_over_rtp_demuxer_opts;

ZMS_API zms_av1_over_rtp_demuxer *
zms_av1_over_rtp_demuxer_create(const zms_av1_over_rtp_demuxer_opts *opts);
ZMS_API void zms_av1_over_rtp_demuxer_destroy(zms_av1_over_rtp_demuxer *d);
ZMS_API ztk_err_t zms_av1_over_rtp_demuxer_input_rtp(zms_av1_over_rtp_demuxer *d,
                                                     const zms_rtp_packet *pkt);

extern const zms_payload_demux_ops zms_av1_over_rtp_demux_ops;

#ifdef __cplusplus
}
#endif

#endif /* ZMS_CODEC_AV1_AV1_OVER_RTP_H */
