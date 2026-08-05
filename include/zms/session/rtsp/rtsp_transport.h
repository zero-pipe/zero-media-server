#ifndef ZMS_SESSION_RTSP_TRANSPORT_H
#define ZMS_SESSION_RTSP_TRANSPORT_H

#include "zms/zms_export.h"
#include "ztk/ztk_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum zms_rtsp_rtp_mode {
    ZMS_RTSP_RTP_TCP = 0,
    ZMS_RTSP_RTP_UDP = 1,
    /** 拉流客户端：TCP interleaved，SETUP 461 时回退 UDP（见 rtsp_client.c） */
    ZMS_RTSP_RTP_AUTO = 2,
} zms_rtsp_rtp_mode;

/** 根据 SETUP Transport 头判断模式；无法识别时默认 TCP */
ZMS_API zms_rtsp_rtp_mode zms_rtsp_transport_parse_mode(const char *transport_hdr);

/** opts.rtp_mode AUTO 时由拉流端协商（默认 TCP，461 回退 UDP）；显式 TCP/UDP 则直接选用 */
ZMS_API zms_rtsp_rtp_mode zms_rtsp_transport_resolve_mode(zms_rtsp_rtp_mode mode);

ZMS_API int zms_rtsp_transport_parse_interleaved(const char *transport, uint8_t *rtp_ch,
                                                 uint8_t *rtcp_ch);
ZMS_API int zms_rtsp_transport_parse_server_port(const char *transport, uint16_t *rtp_port,
                                                 uint16_t *rtcp_port);
ZMS_API int zms_rtsp_transport_parse_client_port(const char *transport, uint16_t *rtp_port,
                                                 uint16_t *rtcp_port);
ZMS_API int zms_rtsp_transport_parse_ssrc(const char *transport, uint32_t *ssrc);

typedef void (*zms_rtsp_udp_on_packet_fn)(void *user, int track_idx, int is_rtcp,
                                          const uint8_t *data, size_t len, const char *peer_ip,
                                          uint16_t peer_port);

/** 进程级：RTP/RTCP 本地端口 会话回调 */
ZMS_API void zms_rtsp_udp_registry_init(void);
ZMS_API void zms_rtsp_udp_registry_fini(void);

ZMS_API ztk_err_t zms_rtsp_udp_registry_bind(uint16_t local_port, int track_idx, int is_rtcp,
                                             zms_rtsp_udp_on_packet_fn cb, void *user);

ZMS_API void zms_rtsp_udp_registry_unbind(uint16_t local_port);

/** 测试/调试：按端口分发一包（通常由 udp_client 回调调用） */
ZMS_API void zms_rtsp_udp_registry_dispatch(uint16_t local_port, const uint8_t *data, size_t len,
                                            const char *peer_ip, uint16_t peer_port);

/** 从全局池分配 RTP/RTCP 端口 */
ZMS_API ztk_err_t zms_rtsp_transport_acquire_ports(uint16_t *rtp_port, uint16_t *rtcp_port);
ZMS_API void zms_rtsp_transport_release_ports(uint16_t rtp_port);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_SESSION_RTSP_TRANSPORT_H */
