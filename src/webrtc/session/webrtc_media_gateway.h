#ifndef ZMS_SRC_WEBRTC_MEDIA_GATEWAY_H
#define ZMS_SRC_WEBRTC_MEDIA_GATEWAY_H

/**
 * WebRTC UDP 分类 + SDP PT 映射（协议壳，不含 SRTP）
 *
 * SRTP/SRTCP: zms/webrtc/webrtc_srtp.h
 * Plain RTP:  zms/media/wire/rtp_packet.h
 * Plain RTCP: zms/session/rtp/rtcp.h + webrtc_rtcp.c 薄封装
 */
#include "zms/media/wire/rtp_packet.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** RTSP/FLV mux 共用的 canonical PT（payload_registry 默认 96/97） */
#define ZMS_WEBRTC_CANON_VIDEO_PT 96u
#define ZMS_WEBRTC_CANON_AUDIO_PT 97u

typedef enum {
    ZMS_WEBRTC_GATEWAY_STUN = 0,
    ZMS_WEBRTC_GATEWAY_DTLS,
    ZMS_WEBRTC_GATEWAY_SRTCP,
    ZMS_WEBRTC_GATEWAY_SRTP,
    ZMS_WEBRTC_GATEWAY_UNKNOWN
} zms_webrtc_gateway_pkt_kind;

typedef struct zms_webrtc_gateway_ingest_cfg {
    uint8_t canon_video_pt;
    uint8_t canon_audio_pt;
    /** SDP 协商得到的 wire PT；表示映射到 canonical 的入站 PT */
    uint8_t wire_video_pt;
    uint8_t wire_audio_pt;
    int answer_has_video;
    int answer_has_audio;
} zms_webrtc_gateway_ingest_cfg;

zms_webrtc_gateway_pkt_kind zms_webrtc_gateway_classify(const uint8_t *data, size_t len);

void zms_webrtc_gateway_ingest_cfg_defaults(zms_webrtc_gateway_ingest_cfg *cfg);

/** @return 1=视频，0=音频，-1=不匹配 */
int zms_webrtc_gateway_match_media_track(const zms_webrtc_gateway_ingest_cfg *cfg, uint8_t pt);

/** 将 SDP 协商 PT 映射为 canonical PT，供 payload demux 与 RTSP 路径一致 */
void zms_webrtc_gateway_remap_pt(uint8_t *rtp, size_t len, const zms_webrtc_gateway_ingest_cfg *cfg,
                                 int is_video);

/** WHEP 出站：canonical PT → SDP wire PT（WHIP remap_pt 的逆操作） */
void zms_webrtc_gateway_remap_pt_egress(uint8_t *rtp, size_t len,
                                        const zms_webrtc_gateway_ingest_cfg *cfg, int is_video);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_SRC_WEBRTC_MEDIA_GATEWAY_H */
