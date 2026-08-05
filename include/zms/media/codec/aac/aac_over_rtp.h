#ifndef ZMS_CODEC_AAC_AAC_OVER_RTP_H
#define ZMS_CODEC_AAC_AAC_OVER_RTP_H

/**
 * AAC over RTP：
 * - 无状态：ES 遍历 / raw 规范化
 * - 有状态 demuxer：RTP 包 → AAC AU（librtp mpeg4-generic，内置 jitter/reorder）
 * PLAY 打包由 rtsp_muxer 处理。
 */
#include "zms/media/codec/payload/payload_registry.h"
#include "zms/zms_export.h"
#include "zms/media/wire/rtp_packet.h"
#include "ztk/ztk_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*zms_aac_au_cb)(const uint8_t *au, size_t len, void *user);

ZMS_API ztk_err_t zms_aac_es_foreach_frame(const uint8_t *es, size_t len, zms_aac_au_cb cb,
                                           void *user);

/** CC gap：要求 PES payload 被 ADTS 帧完整填满，拒绝半帧/脏数据。 */
ZMS_API int zms_aac_es_strict_valid(const uint8_t *es, size_t len);

/** ring ES 规范化：若含 ADTS 则返回 raw AAC 区（无 ADTS）；否则原样。 */
ZMS_API ztk_err_t zms_aac_es_to_raw(const uint8_t *es, size_t len, const uint8_t **raw,
                                    size_t *raw_len);

typedef void (*zms_aac_over_rtp_on_frame_cb)(const uint8_t *aac, size_t len, uint64_t dts_ms,
                                             void *user);
typedef struct zms_aac_over_rtp_demuxer zms_aac_over_rtp_demuxer;

typedef struct zms_aac_over_rtp_demuxer_opts {
    zms_aac_over_rtp_on_frame_cb on_frame_cb;
    void *user;
    uint32_t rtp_clock_hz;
    int jitter_ms;
    int payload_type;
} zms_aac_over_rtp_demuxer_opts;

ZMS_API zms_aac_over_rtp_demuxer *
zms_aac_over_rtp_demuxer_create(const zms_aac_over_rtp_demuxer_opts *opts);
ZMS_API void zms_aac_over_rtp_demuxer_destroy(zms_aac_over_rtp_demuxer *d);
ZMS_API ztk_err_t zms_aac_over_rtp_demuxer_input_rtp(zms_aac_over_rtp_demuxer *d,
                                                     const zms_rtp_packet *pkt);

extern const zms_payload_demux_ops zms_aac_over_rtp_demux_ops;

#ifdef __cplusplus
}
#endif

#endif /* ZMS_CODEC_AAC_AAC_OVER_RTP_H */
