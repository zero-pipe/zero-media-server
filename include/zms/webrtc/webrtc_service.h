#ifndef ZMS_WEBRTC_SERVICE_H
#define ZMS_WEBRTC_SERVICE_H

#include "zms/zms_export.h"
#include "ztk/poller/poller.h"
#include "ztk/ztk_errno.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zms_webrtc_service zms_webrtc_service;

typedef struct zms_webrtc_service_opts {
    ztk_poller *poller;
    const char *host;
    /** ICE candidate / SDP c= 地址（默认 127.0.0.1）。 */
    const char *advertise_host;
    uint16_t port_min;
    uint16_t port_max;
} zms_webrtc_service_opts;

ZMS_API zms_webrtc_service *zms_webrtc_service_create(const zms_webrtc_service_opts *opts);
ZMS_API void zms_webrtc_service_destroy(zms_webrtc_service *srv);

ZMS_API zms_webrtc_service *zms_webrtc_service_instance(void);
ZMS_API void zms_webrtc_service_set_instance(zms_webrtc_service *srv);
ZMS_API const char *zms_webrtc_service_bind_host(const zms_webrtc_service *srv);
ZMS_API const char *zms_webrtc_service_advertise_host(const zms_webrtc_service *srv);
/** ICE 定时器与全部 WebRTC 会话须共享此 poller（线程安全）。 */
ZMS_API ztk_poller *zms_webrtc_service_poller(const zms_webrtc_service *srv);
/** 优先 WebRTC 服务 poller 而非 HTTP TCP poller（避免 libice 与会话竞态）。 */
ZMS_API ztk_poller *zms_webrtc_service_resolve_poller(struct zms_http_session *hs);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_WEBRTC_SERVICE_H */
