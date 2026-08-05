#include "zms/util/buf_pool.h"
#include "zms/ops/service/config.h"
#include "ztk/poller/poller_pool.h"
#include "ztk/util/buf.h"
#include "ztk/util/log.h"
#include <stdlib.h>
#include <string.h>

static int g_buf_pool_enabled = 1;
static int g_buf_pool_mode = ZMS_BUF_POOL_MODE_HYBRID;
static ztk_buf_pool *g_global_pool;

static ztk_buf_pool_opts pool_opts_from_cfg(const zms_config *cfg, int thread_safe)
{
    ztk_buf_pool_opts opt;

    memset(&opt, 0, sizeof(opt));
    opt.max_per_bucket = cfg && cfg->general.buf_pool_max_per_bucket > 0
                             ? cfg->general.buf_pool_max_per_bucket
                             : ZMS_BUF_POOL_MAX_PER_BUCKET_DEFAULT;
    opt.thread_safe = thread_safe ? 1 : 0;
    return opt;
}

void zms_buf_pool_init(const zms_config *cfg)
{
    zms_buf_pool_fini();

    g_buf_pool_enabled = cfg ? cfg->general.buf_pool_enable : 1;
    g_buf_pool_mode = cfg ? cfg->general.buf_pool_mode : ZMS_BUF_POOL_MODE_HYBRID;

    if (!g_buf_pool_enabled) {
        ztk_buf_set_shared_pool(NULL);
        ztk_info("buf pool disabled (bufPoolEnable=0), ztk_buf_alloc uses malloc");
        return;
    }

    if (g_buf_pool_mode == ZMS_BUF_POOL_MODE_GLOBAL ||
        g_buf_pool_mode == ZMS_BUF_POOL_MODE_HYBRID) {
        /* 全局池跨 poller / ring，必须加锁 */
        ztk_buf_pool_opts opt = pool_opts_from_cfg(cfg, 1);
        g_global_pool = ztk_buf_pool_create(&opt);
        ztk_buf_set_shared_pool(g_global_pool);
    } else {
        ztk_buf_set_shared_pool(NULL);
    }
}

void zms_buf_pool_fini(void)
{
    if (g_global_pool) {
        ztk_buf_pool_stats st;
        ztk_buf_pool_get_stats(g_global_pool, &st);
        ztk_info("buf pool fini stats: hit=%llu miss=%llu oversize=%llu cached=%llu dropped=%llu",
                 (unsigned long long)st.acquire_hit, (unsigned long long)st.acquire_miss,
                 (unsigned long long)st.acquire_oversize, (unsigned long long)st.release_cached,
                 (unsigned long long)st.release_dropped);
    }
    ztk_buf_set_shared_pool(NULL);
    if (g_global_pool) {
        ztk_buf_pool_destroy(g_global_pool);
        g_global_pool = NULL;
    }
}

void zms_poller_pool_buf_attach(ztk_poller_pool *pool, const zms_config *cfg)
{
    if (!pool || !cfg || !cfg->general.buf_pool_enable) {
        return;
    }
    if (cfg->general.buf_pool_mode != ZMS_BUF_POOL_MODE_PER_POLLER &&
        cfg->general.buf_pool_mode != ZMS_BUF_POOL_MODE_HYBRID) {
        return;
    }

    {
        /* poller 本地池：单线程访问，无锁 */
        ztk_buf_pool_opts opt = pool_opts_from_cfg(cfg, 0);
        if (ztk_poller_pool_attach_buf_pools(pool, &opt) != ZTK_OK) {
            ztk_warn("poller_pool attach buf pools failed");
        }
    }
}

void zms_poller_pool_buf_detach(ztk_poller_pool *pool)
{
    if (pool) {
        ztk_poller_pool_detach_buf_pools(pool);
    }
}

int zms_buf_pool_enabled(void)
{
    return g_buf_pool_enabled;
}

int zms_buf_pool_mode(void)
{
    return g_buf_pool_mode;
}

int zms_buf_pool_get_stats(ztk_buf_pool_stats *out)
{
    if (!out) {
        return 0;
    }
    memset(out, 0, sizeof(*out));
    if (!g_buf_pool_enabled || !g_global_pool) {
        return 0;
    }
    ztk_buf_pool_get_stats(g_global_pool, out);
    return 1;
}

void zms_buf_pool_reset_stats(void)
{
    if (g_global_pool) {
        ztk_buf_pool_reset_stats(g_global_pool);
    }
}

void *zms_buf_pool_acquire(size_t size, size_t *out_cap)
{
    if (size == 0) {
        return NULL;
    }
    if (g_buf_pool_enabled && g_global_pool) {
        return ztk_buf_pool_acquire(g_global_pool, size, out_cap);
    }
    void *p = malloc(size);
    if (p && out_cap) {
        *out_cap = size;
    }
    return p;
}

void zms_buf_pool_release(void *ptr, size_t cap)
{
    if (!ptr) {
        return;
    }
    if (g_buf_pool_enabled && g_global_pool) {
        ztk_buf_pool_release(g_global_pool, ptr, cap);
    } else {
        free(ptr);
    }
    (void)cap;
}

void zms_buf_pool_slot_clear(uint8_t **data, size_t *cap)
{
    if (!data) {
        return;
    }
    if (*data) {
        zms_buf_pool_release(*data, cap ? *cap : 0);
        *data = NULL;
    }
    if (cap) {
        *cap = 0;
    }
}

void *zms_buf_pool_slot_resize(uint8_t **data, size_t *cap, size_t len)
{
    if (!data || !cap || len == 0) {
        return NULL;
    }
    if (*data && *cap >= len) {
        return *data;
    }
    zms_buf_pool_slot_clear(data, cap);
    *data = (uint8_t *)zms_buf_pool_acquire(len, cap);
    return *data;
}

static void *acquire_for_poller(ztk_poller *poller, size_t size, size_t *out_cap)
{
    ztk_buf_pool *local;

    if (size == 0) {
        return NULL;
    }
    local = ztk_poller_buf_pool(poller);
    if (g_buf_pool_enabled && local) {
        return ztk_buf_pool_acquire(local, size, out_cap);
    }
    return zms_buf_pool_acquire(size, out_cap);
}

static void release_for_poller(ztk_poller *poller, void *ptr, size_t cap)
{
    ztk_buf_pool *local;

    if (!ptr) {
        return;
    }
    local = ztk_poller_buf_pool(poller);
    if (g_buf_pool_enabled && local) {
        ztk_buf_pool_release(local, ptr, cap);
    } else {
        zms_buf_pool_release(ptr, cap);
    }
}

void zms_buf_pool_slot_clear_poller(uint8_t **data, size_t *cap, ztk_poller *poller)
{
    if (!data) {
        return;
    }
    if (*data) {
        release_for_poller(poller, *data, cap ? *cap : 0);
        *data = NULL;
    }
    if (cap) {
        *cap = 0;
    }
}

void *zms_buf_pool_slot_resize_poller(uint8_t **data, size_t *cap, size_t len, ztk_poller *poller)
{
    if (!data || !cap || len == 0) {
        return NULL;
    }
    if (*data && *cap >= len) {
        return *data;
    }
    zms_buf_pool_slot_clear_poller(data, cap, poller);
    *data = (uint8_t *)acquire_for_poller(poller, len, cap);
    return *data;
}
