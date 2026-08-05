#ifndef ZMS_MEDIA_WIRE_RTP_PACKET_H
#define ZMS_MEDIA_WIRE_RTP_PACKET_H

/**
 * @file rtp_packet.h
 * @brief RFC 3550 RTP 头解析与 RTSP interleaved 成帧（$ + channel + len）。
 *
 * 仅线格式（RFC 3550 + RTSP interleaved 成帧）。
 * 编解码载荷 demux 位于 media/codec/*_over_rtp。
 * 置于 session/ 之外，避免 media/payload 依赖 session。
 */
#include "zms/zms_export.h"
#include "ztk/ztk_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZMS_RTP_HDR_SIZE 12u
#define ZMS_RTSP_INTERLEAVED_HDR 4u
/** 常见 RTSP RTP 时钟（H.264/AAC 常为 90 kHz；可能与 SDP sample-rate 不同）。 */
#define ZMS_RTP90_CLOCK 90000u

ZMS_API uint32_t zms_rtp90_ts_to_ms(uint32_t rtp_ts);
ZMS_API uint32_t zms_rtp90_ms_to_ts(uint32_t ms);

/**
 * RTP 头的逻辑解析视图（RFC 3550）。
 * 非线布局：勿 memcpy 到/从网络；请用 zms_rtp_parse。
 */
typedef struct zms_rtp_header {
    uint8_t version;
    uint8_t padding;
    uint8_t extension;
    uint8_t csrc_count;
    uint8_t marker;
    uint8_t pt;
    uint16_t seq;
    uint32_t timestamp;
    uint32_t ssrc;
} zms_rtp_header;

typedef struct zms_rtp_packet {
    const uint8_t *data;
    size_t size;
    zms_rtp_header hdr;
    const uint8_t *payload;
    size_t payload_size;
    uint8_t interleaved_channel;
} zms_rtp_packet;

ZMS_API ztk_err_t zms_rtp_parse(const uint8_t *data, size_t len, zms_rtp_packet *out);
ZMS_API size_t zms_rtp_payload_offset(const uint8_t *data, size_t len);
/** 取自 RTP 头字节 1 的 payload type（已剥离 M 位）。 */
ZMS_API uint8_t zms_rtp_payload_type(const uint8_t *data, size_t len);
/** 原地改写 PT；保留 RTP marker 位。 */
ZMS_API void zms_rtp_set_payload_type(uint8_t *data, size_t len, uint8_t pt);
ZMS_API int zms_rtp_is_rtcp(const uint8_t *data, size_t len);
ZMS_API size_t zms_rtsp_interleaved_write(uint8_t *out, size_t cap, uint8_t channel,
                                          const void *payload, size_t len);
ZMS_API ztk_err_t zms_rtsp_interleaved_read(const uint8_t *data, size_t len, uint8_t *channel,
                                            const uint8_t **payload, size_t *payload_len);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_MEDIA_WIRE_RTP_PACKET_H */
