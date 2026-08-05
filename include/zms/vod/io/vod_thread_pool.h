#ifndef ZMS_VOD_THREAD_POOL_H
#define ZMS_VOD_THREAD_POOL_H

/**
 * VOD 磁盘阻塞任务：ztk_thread_pool 执行 work，完成后 ztk_poller_async 回 I/O 线程。
 * 会话/点播事件循环用 poller_pool；VOD 文件操作用 thread_pool。
 *
 * init：zms_vod_thread_pool_init（[general] vodWorkThreads，0=在 poller 上同步执行）。
 */
#include "zms/zms_export.h"
#include "ztk/ztk_errno.h"

#ifdef __cplusplus
extern "C" {
#endif

struct zms_config;
struct ztk_poller;

typedef void (*zms_vod_thread_pool_work_fn)(void *user);
typedef void (*zms_vod_thread_pool_on_io_fn)(void *user);

ZMS_API void zms_vod_thread_pool_init(const struct zms_config *cfg);
ZMS_API void zms_vod_thread_pool_fini(void);
ZMS_API int zms_vod_thread_pool_enabled(void);

/**
 * @param io 目标 poller（通常为 ztk_tcp_session_poller）
 * @note user 须在 on_io 返回前有效；on_io 内应检查连接是否仍有效
 */
ZMS_API ztk_err_t zms_vod_thread_pool_run(struct ztk_poller *io, zms_vod_thread_pool_work_fn work,
                                          zms_vod_thread_pool_on_io_fn on_io, void *user);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_VOD_THREAD_POOL_H */
