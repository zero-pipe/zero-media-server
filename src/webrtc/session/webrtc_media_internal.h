#ifndef ZMS_SRC_WEBRTC_MEDIA_INTERNAL_H
#define ZMS_SRC_WEBRTC_MEDIA_INTERNAL_H

#include "zms/webrtc/webrtc_srtp.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int zms_webrtc_stun_is_binding_req(const uint8_t *data, size_t len);
size_t zms_webrtc_stun_binding_reply(const uint8_t *req, size_t req_len, uint8_t *out,
                                     size_t out_cap, uint32_t xor_ip_be, uint16_t xor_port,
                                     const char *pwd);

int zms_webrtc_dtls_global_init(void);
void zms_webrtc_dtls_global_fini(void);
const char *zms_webrtc_dtls_fingerprint(void);

typedef struct zms_webrtc_dtls zms_webrtc_dtls;

zms_webrtc_dtls *zms_webrtc_dtls_create(void);
zms_webrtc_dtls *zms_webrtc_dtls_create_client(void);
void zms_webrtc_dtls_destroy(zms_webrtc_dtls *d);
/** 仅 Client 角色：握手未起时发出 DTLS ClientHello。 */
int zms_webrtc_dtls_kick(zms_webrtc_dtls *d, uint8_t *out, size_t out_cap, size_t *out_len);
/** @return 1 已连接，0 进行中，-1 错误。有待发响应时写入 out。 */
int zms_webrtc_dtls_input(zms_webrtc_dtls *d, const uint8_t *pkt, size_t len, uint8_t *out,
                          size_t out_cap, size_t *out_len);
int zms_webrtc_dtls_is_connected(const zms_webrtc_dtls *d);
int zms_webrtc_dtls_export_server_srtp(const zms_webrtc_dtls *d, uint8_t *key, uint8_t *salt);
int zms_webrtc_dtls_export_client_srtp(const zms_webrtc_dtls *d, uint8_t *key, uint8_t *salt);

int zms_webrtc_packet_is_dtls(const uint8_t *data, size_t len);
int zms_webrtc_packet_is_rtp(const uint8_t *data, size_t len);

/** 薄封装 zms/session/rtp/rtcp.h，供 WebRTC 复合包解析。 */
int zms_webrtc_rtcp_is_psfb_pli(const uint8_t *data, size_t len);
int zms_webrtc_rtcp_is_rtpfb_nack(const uint8_t *data, size_t len);
size_t zms_webrtc_rtcp_build_sr(uint8_t *out, size_t cap, uint32_t ssrc, uint32_t ntp_sec,
                                uint32_t ntp_frac, uint32_t rtp_ts, uint32_t packet_count,
                                uint32_t octet_count);
void zms_webrtc_rtcp_for_each_compound(const uint8_t *data, size_t len,
                                       void (*cb)(const uint8_t *block, size_t block_len,
                                                  void *user),
                                       void *user);

size_t zms_webrtc_rtp_add_twcc_ext(uint8_t *rtp, size_t len, size_t cap, uint16_t twcc_seq,
                                   size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_SRC_WEBRTC_MEDIA_INTERNAL_H */
