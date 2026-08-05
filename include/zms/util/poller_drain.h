#ifndef ZMS_UTIL_POLLER_DRAIN_H
#define ZMS_UTIL_POLLER_DRAIN_H

#include "zms/zms_export.h"
#include "ztk/poller/poller_pool.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 排空池中每个 poller 的异步任务队列（非 poller 线程可安全调用）。 */
ZMS_API void zms_poller_pool_drain_async(struct ztk_poller_pool *pool);

/** 唤醒并排空两次，中间短睡眠（关停辅助）。 */
ZMS_API void zms_poller_pool_drain_async_wait(struct ztk_poller_pool *pool, unsigned wait_ms);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_UTIL_POLLER_DRAIN_H */
