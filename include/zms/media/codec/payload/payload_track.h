#ifndef ZMS_CODEC_PAYLOAD_PAYLOAD_TRACK_H
#define ZMS_CODEC_PAYLOAD_PAYLOAD_TRACK_H

/**
 * @file payload_track.h
 * @brief 按轨 payload 解复用库（RTSP client / RECORD 共用）。
 *
 * 使用 stream_limits 中的 ZMS_TRACK_SLOT_MAX — 不依赖 session/rtsp。
 */
#include "zms/media/codec/payload/payload_registry.h"
#include "zms/engine/stream/stream_limits.h"
#include "zms/media/wire/rtp_packet.h"
#include "zms/zms_export.h"
#include "ztk/ztk_errno.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zms_payload_track_slot {
    zms_codec_id codec;
    void *ctx;
} zms_payload_track_slot;

typedef struct zms_payload_track_bank {
    zms_payload_track_slot slots[ZMS_TRACK_SLOT_MAX];
} zms_payload_track_bank;

ZMS_API void zms_payload_track_bank_clear(zms_payload_track_bank *bank);

ZMS_API ztk_err_t zms_payload_track_bank_input_rtp(zms_payload_track_bank *bank, int track_index,
                                                   zms_codec_id codec, zms_wire_format_id wire,
                                                   uint32_t rtp_clock_hz,
                                                   zms_payload_frame_cb on_frame, void *user,
                                                   const zms_rtp_packet *pkt);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_CODEC_PAYLOAD_PAYLOAD_TRACK_H */
