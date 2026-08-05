#ifndef ZMS_SESSION_SRT_SERVICE_H
#define ZMS_SESSION_SRT_SERVICE_H

#include "zms/zms_export.h"
#include "ztk/ztk_errno.h"
#include "ztk/poller/poller.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zms_srt_service zms_srt_service;

typedef struct zms_srt_service_opts {
    ztk_poller *poller;
    const char *host;
    uint16_t port;
} zms_srt_service_opts;

/** 二进制在 ZMS_ENABLE_SRT 且链接 libsrt 时编译则为非零。 */
ZMS_API int zms_srt_service_available(void);

ZMS_API zms_srt_service *zms_srt_service_create(const zms_srt_service_opts *opts);
ZMS_API void zms_srt_service_destroy(zms_srt_service *srv);
ZMS_API ztk_err_t zms_srt_service_start(zms_srt_service *srv);
ZMS_API void zms_srt_service_stop(zms_srt_service *srv);
ZMS_API uint16_t zms_srt_service_port(const zms_srt_service *srv);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_SESSION_SRT_SERVICE_H */
