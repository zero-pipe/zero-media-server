#ifndef ZMS_WEBRTC_SRTP_H
#define ZMS_WEBRTC_SRTP_H

/**
 * WebRTC SRTP/SRTCP 加密壳（RFC 3711 AES-128-CM + HMAC-SHA1 80-bit）。
 *
 * 操作 zms/session/rtp/* 的明文 RTP/RTCP 缓冲；不解析载荷。
 */
#include "zms/zms_export.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZMS_WEBRTC_SRTP_KEY_LEN 16u
#define ZMS_WEBRTC_SRTP_SALT_LEN 14u
#define ZMS_WEBRTC_SRTP_TAG_LEN 10u

typedef struct zms_webrtc_srtp zms_webrtc_srtp;

ZMS_API zms_webrtc_srtp *zms_webrtc_srtp_create(void);
ZMS_API void zms_webrtc_srtp_destroy(zms_webrtc_srtp *ctx);

/** WHEP 出站 / 播放发送方向。 */
ZMS_API int zms_webrtc_srtp_init_send(zms_webrtc_srtp *ctx, const uint8_t *key,
                                      const uint8_t *salt);
ZMS_API int zms_webrtc_srtp_protect(zms_webrtc_srtp *ctx, uint8_t *rtp, size_t *len_io, size_t cap);

/** WHIP 入站 / 播放接收方向。 */
ZMS_API int zms_webrtc_srtp_init_recv(zms_webrtc_srtp *ctx, const uint8_t *key,
                                      const uint8_t *salt);
ZMS_API int zms_webrtc_srtp_unprotect(zms_webrtc_srtp *ctx, uint8_t *rtp, size_t *len_io);

/** 明文 RTCP compound SRTCP（RFC 3711 §4.2）。 */
ZMS_API int zms_webrtc_srtcp_protect(zms_webrtc_srtp *ctx, uint8_t *rtcp, size_t *len_io,
                                     size_t cap);
ZMS_API int zms_webrtc_srtcp_unprotect(zms_webrtc_srtp *ctx, uint8_t *rtcp, size_t *len_io);

/** 会话级暂存（SRTP HMAC）；须为 ZMS_WEBRTC_PLAY_CRYPT_BYTES。 */
ZMS_API void zms_webrtc_srtp_bind_scratch(zms_webrtc_srtp *ctx, uint8_t *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_WEBRTC_SRTP_H */
