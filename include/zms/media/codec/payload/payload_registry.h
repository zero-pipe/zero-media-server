#ifndef ZMS_CODEC_PAYLOAD_PAYLOAD_REGISTRY_H
#define ZMS_CODEC_PAYLOAD_PAYLOAD_REGISTRY_H

/**
 * Payload 层：codec × wire（如 H264+RTP）解复用，与 L6 Session / RTP 壳解耦 */
#include "zms/media/codec/codec_id.h"
#include "zms/engine/frame.h"
#include "zms/media/wire_format.h"
#include "zms/media/wire/rtp_packet.h"
#include "zms/zms_export.h"
#include "ztk/ztk_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*zms_payload_frame_cb)(const zms_frame *frame, void *user);

typedef struct zms_payload_demux_opts {
    zms_codec_id codec;
    zms_wire_format_id wire;
    uint32_t rtp_clock_hz;
    /** SDP 中的 RTP payload type；0 表示编解码默认值。 */
    int payload_type;
    zms_payload_frame_cb on_frame;
    void *user;
} zms_payload_demux_opts;

typedef struct zms_payload_demux_ops {
    zms_codec_id codec;
    zms_wire_format_id wire;
    const char *name;

    void *(*create)(const zms_payload_demux_opts *opts);
    void (*destroy)(void *ctx);
    ztk_err_t (*input_rtp)(void *ctx, const zms_rtp_packet *pkt, zms_frame *out);
} zms_payload_demux_ops;

ZMS_API void zms_payload_register_demux(const zms_payload_demux_ops *ops);
ZMS_API const zms_payload_demux_ops *zms_payload_demux_find(zms_codec_id codec,
                                                            zms_wire_format_id wire);

/** 注册全部内建 RTP payload demux（各 {codec}_over_rtp.c 导出 demux_ops） */
ZMS_API void zms_rtp_payload_register_all(void);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_CODEC_PAYLOAD_PAYLOAD_REGISTRY_H */
