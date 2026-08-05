#ifndef ZMS_SESSION_RTSP_SERVICE_H
#define ZMS_SESSION_RTSP_SERVICE_H

#include "zms/zms_export.h"
#include "ztk/ztk_errno.h"
#include "ztk/net/tcp_server.h"
#include "ztk/poller/poller_pool.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zms_rtsp_service zms_rtsp_service;

typedef struct zms_rtsp_service_opts {
    ztk_poller *poller;
    ztk_poller_pool *poller_pool;
    const char *host;
    uint16_t port;
    /** SDP o= / c= 对外公告地址；NULL 则回退到 extern_ip / 127.0.0.1 */
    const char *advertise_host;
} zms_rtsp_service_opts;

ZMS_API zms_rtsp_service *zms_rtsp_service_create(const zms_rtsp_service_opts *opts);
ZMS_API void zms_rtsp_service_destroy(zms_rtsp_service *srv);
ZMS_API ztk_err_t zms_rtsp_service_start(zms_rtsp_service *srv);
ZMS_API void zms_rtsp_service_stop(zms_rtsp_service *srv);
ZMS_API uint16_t zms_rtsp_service_port(const zms_rtsp_service *srv);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_SESSION_RTSP_SERVICE_H */
