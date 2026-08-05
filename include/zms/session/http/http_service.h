#ifndef ZMS_SESSION_HTTP_SERVICE_H
#define ZMS_SESSION_HTTP_SERVICE_H

#include "zms/zms_export.h"
#include "ztk/ztk_errno.h"
#include "ztk/net/tcp_server.h"
#include "ztk/poller/poller_pool.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zms_http_service zms_http_service;

struct zms_config;

typedef struct zms_http_service_opts {
    ztk_poller *poller;
    ztk_poller_pool *poller_pool;
    const char *host;
    uint16_t port;
    /** [api] secret，对应 WebApi CHECK_SECRET；空则本机免鉴权 */
    const char *api_secret;
    /** HTTP API / WebHook；可为 NULL */
    const struct zms_config *cfg;
} zms_http_service_opts;

ZMS_API zms_http_service *zms_http_service_create(const zms_http_service_opts *opts);
ZMS_API void zms_http_service_destroy(zms_http_service *srv);
ZMS_API ztk_err_t zms_http_service_start(zms_http_service *srv);
ZMS_API void zms_http_service_stop(zms_http_service *srv);
ZMS_API uint16_t zms_http_service_port(const zms_http_service *srv);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_SESSION_HTTP_SERVICE_H */
