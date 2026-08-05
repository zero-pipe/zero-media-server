#ifndef ZMS_SESSION_RTMP_SERVICE_H
#define ZMS_SESSION_RTMP_SERVICE_H

#include "zms/zms_export.h"
#include "ztk/ztk_errno.h"
#include "ztk/net/tcp_server.h"
#include "ztk/poller/poller_pool.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zms_rtmp_service zms_rtmp_service;

typedef struct zms_rtmp_service_opts {
    ztk_poller *poller;
    ztk_poller_pool *poller_pool;
    const char *host;
    uint16_t port;
} zms_rtmp_service_opts;

ZMS_API zms_rtmp_service *zms_rtmp_service_create(const zms_rtmp_service_opts *opts);
ZMS_API void zms_rtmp_service_destroy(zms_rtmp_service *srv);
ZMS_API ztk_err_t zms_rtmp_service_start(zms_rtmp_service *srv);
ZMS_API void zms_rtmp_service_stop(zms_rtmp_service *srv);
ZMS_API uint16_t zms_rtmp_service_port(const zms_rtmp_service *srv);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_SESSION_RTMP_SERVICE_H */
