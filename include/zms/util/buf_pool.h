#ifndef ZMS_UTIL_MEMORY_BUF_POOL_H
#define ZMS_UTIL_MEMORY_BUF_POOL_H

/**
 * ZMS 缓冲池（对齐 [general] bufPoolEnable / bufPoolMode / bufPoolMaxPerBucket）。
 *
 * - global：仅全局池（thread_safe）→ zms_buf_pool_acquire、ztk_buf_alloc（ring）
 * - per_poller：仅各 poller 本地池（无锁；须 zms_poller_pool_buf_attach）
 * - hybrid：全局(加锁) + per_poller(无锁)（推荐服务器）
 *
 * 会话热路径优先 zms_buf_pool_slot_*_poller，避免打到全局锁。
 * 跨 poller 共享载荷（GOP ring）用 ztk_buf_alloc；连接内发送用 alloc_local。
 * 无锁本地池末次 unref 若不在 owner 线程，ztk 会 async 回投再入池。
 * 池内部为每档 freelist；可用 zms_buf_pool_get_stats 看命中/越档。
 */
#include "zms/zms_export.h"
#include "ztk/util/buf.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct zms_config;
struct ztk_poller;
struct ztk_poller_pool;

ZMS_API void zms_buf_pool_init(const struct zms_config *cfg);
ZMS_API void zms_buf_pool_fini(void);

/** poller_pool 创建并 start 后调用（per_poller / hybrid） */
ZMS_API void zms_poller_pool_buf_attach(struct ztk_poller_pool *pool, const struct zms_config *cfg);
ZMS_API void zms_poller_pool_buf_detach(struct ztk_poller_pool *pool);

ZMS_API int zms_buf_pool_enabled(void);
ZMS_API int zms_buf_pool_mode(void);

/** 全局共享池统计；无全局池时返回 0 并清零 out */
ZMS_API int zms_buf_pool_get_stats(ztk_buf_pool_stats *out);
ZMS_API void zms_buf_pool_reset_stats(void);

/** 裸指针槽位（ingress 工作区等），走全局池 */
ZMS_API void *zms_buf_pool_acquire(size_t size, size_t *out_cap);
ZMS_API void zms_buf_pool_release(void *ptr, size_t cap);

ZMS_API void zms_buf_pool_slot_clear(uint8_t **data, size_t *cap);
ZMS_API void *zms_buf_pool_slot_resize(uint8_t **data, size_t *cap, size_t len);

/** 同 poller 槽位：优先 poller 本地池（RTMP tag 等连接内热路径） */
ZMS_API void zms_buf_pool_slot_clear_poller(uint8_t **data, size_t *cap, struct ztk_poller *poller);
ZMS_API void *zms_buf_pool_slot_resize_poller(uint8_t **data, size_t *cap, size_t len,
                                              struct ztk_poller *poller);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_UTIL_MEMORY_BUF_POOL_H */
