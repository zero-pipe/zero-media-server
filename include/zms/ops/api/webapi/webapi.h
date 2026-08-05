#ifndef ZMS_API_WEBAPI_WEBAPI_H
#define ZMS_API_WEBAPI_WEBAPI_H

/**
 * HTTP REST API。
 */
#include "zms/zms_export.h"
#include "ztk/ztk_errno.h"
#include "ztk/net/tcp_server.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 对齐 server/WebApi.h ApiErr */
typedef enum zms_api_err {
    ZMS_API_SUCCESS = 0,
    ZMS_API_OTHER_FAILED = -1,
    ZMS_API_AUTH_FAILED = -100,
    ZMS_API_INVALID_ARGS = -300,
    ZMS_API_NOT_FOUND = -500,
} zms_api_err;

typedef struct zms_config zms_config;

typedef struct zms_web_api_opts {
    const char *api_secret;
    ztk_poller *poller;
    ztk_poller_pool *poller_pool;
    const zms_config *cfg;
} zms_web_api_opts;

/**
 * 处理 /index/api/* 请求。
 * @param method GET/POST
 * @param path_with_query 含 query 的 path，如 /index/api/getMediaList?app=live
 * @return 写入 body 的字节数；*http_status 为 HTTP 状态码
 */
ZMS_API size_t zms_web_api_handle(ztk_tcp_session *tcp, const char *method,
                                  const char *path_with_query, const zms_web_api_opts *opts,
                                  int *http_status, char *body, size_t body_cap);

/** 记录单调时钟启动时间，供 health/statistic 计算 uptime（服务启动时调用一次）。 */
ZMS_API void zms_web_api_note_boot(void);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_API_WEBAPI_WEBAPI_H */
