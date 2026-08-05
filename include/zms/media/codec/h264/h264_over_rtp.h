#ifndef ZMS_CODEC_H264_OVER_RTP_H
#define ZMS_CODEC_H264_OVER_RTP_H

/**
 * H.264 Annex-B RTP demuxer（RTP→ES）：jitter/reorder + AU 拼装一步完成。
 * PLAY 打包由 librtsp rtsp_muxer 承担。
 */
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

typedef void (*zms_h264_over_rtp_on_frame_cb)(const zms_frame *frame, void *user);

typedef struct zms_h264_over_rtp_demuxer zms_h264_over_rtp_demuxer;

typedef struct zms_h264_over_rtp_demuxer_opts {
    zms_h264_over_rtp_on_frame_cb on_frame_cb;
    void *user;
    /** SDP rtpmap 时钟（Hz）；0 → 90000 */
    uint32_t rtp_clock_hz;
    /** librtp jitter 缓冲（毫秒）；0 → 200 */
    int jitter_ms;
    /** RTP 载荷类型；0 → 96 */
    int payload_type;
} zms_h264_over_rtp_demuxer_opts;

ZMS_API zms_h264_over_rtp_demuxer *
zms_h264_over_rtp_demuxer_create(const zms_h264_over_rtp_demuxer_opts *opts);
ZMS_API void zms_h264_over_rtp_demuxer_destroy(zms_h264_over_rtp_demuxer *d);
ZMS_API ztk_err_t zms_h264_over_rtp_demuxer_input_rtp(zms_h264_over_rtp_demuxer *d,
                                                      const zms_rtp_packet *pkt);

/** payload 注册表插件；定义于 h264_over_rtp.c */
extern const zms_payload_demux_ops zms_h264_over_rtp_demux_ops;

#ifdef __cplusplus
}
#endif

#endif /* ZMS_CODEC_H264_OVER_RTP_H */
