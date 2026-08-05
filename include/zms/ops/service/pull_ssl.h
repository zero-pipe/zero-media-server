#ifndef ZMS_SERVICE_PULL_SSL_H
#define ZMS_SERVICE_PULL_SSL_H

#include "zms/ops/service/config.h"
#include "zms/zms_export.h"

#ifdef __cplusplus
extern "C" {
#endif

struct ztk_ssl_ctx;

/** 按 [ssl] 配置返回进程内共享 TLS 上下文（懒创建，勿 destroy） */
ZMS_API struct ztk_ssl_ctx *zms_pull_ssl_ctx(const zms_config *cfg);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_SERVICE_PULL_SSL_H */
