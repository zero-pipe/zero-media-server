#include "zms/vod/io/vod_thread_pool.h"
#include "zms/ops/service/config.h"
#include "ztk/poller/poller.h"
#include "ztk/thread/thread_pool.h"
#include "ztk/util/log.h"
#include <stdlib.h>

static ztk_thread_pool *g_pool;

void zms_vod_thread_pool_init(const zms_config *cfg)
{
    zms_vod_thread_pool_fini();
    if (!cfg || cfg->general.vod_work_threads == 0) {
        return;
    }

    {
        ztk_thread_pool_opts_t tp_opts;
        tp_opts.thread_count = cfg->general.vod_work_threads;
        tp_opts.priority = ZTK_THREAD_PRIO_NORMAL;
        tp_opts.auto_start = 1;
        g_pool = ztk_thread_pool_create(&tp_opts);
        if (!g_pool) {
            ztk_warn("vod thread_pool create failed, VOD file work runs on poller");
        }
    }
}

void zms_vod_thread_pool_fini(void)
{
    if (g_pool) {
        ztk_thread_pool_destroy(g_pool);
        g_pool = NULL;
    }
}

int zms_vod_thread_pool_enabled(void)
{
    return g_pool != NULL;
}

typedef struct zms_vod_thread_pool_run_ctx {
    ztk_poller *io;
    zms_vod_thread_pool_work_fn work;
    zms_vod_thread_pool_on_io_fn on_io;
    void *user;
} zms_vod_thread_pool_run_ctx;

static void on_io_tramp(void *arg)
{
    zms_vod_thread_pool_run_ctx *ctx = (zms_vod_thread_pool_run_ctx *)arg;
    if (ctx->on_io) {
        ctx->on_io(ctx->user);
    }
    free(ctx);
}

static void work_tramp(void *arg)
{
    zms_vod_thread_pool_run_ctx *ctx = (zms_vod_thread_pool_run_ctx *)arg;
    if (ctx->work) {
        ctx->work(ctx->user);
    }
    if (ctx->io) {
        ztk_poller_async(ctx->io, on_io_tramp, ctx, 0);
    } else {
        on_io_tramp(ctx);
    }
}

ztk_err_t zms_vod_thread_pool_run(ztk_poller *io, zms_vod_thread_pool_work_fn work,
                                  zms_vod_thread_pool_on_io_fn on_io, void *user)
{
    zms_vod_thread_pool_run_ctx *ctx;

    if (!work || !on_io) {
        return ZTK_ERR_INVALID;
    }

    if (!g_pool) {
        work(user);
        on_io(user);
        return ZTK_OK;
    }

    ctx = (zms_vod_thread_pool_run_ctx *)calloc(1, sizeof(*ctx));
    if (!ctx) {
        return ZTK_ERR_NOMEM;
    }
    ctx->io = io;
    ctx->work = work;
    ctx->on_io = on_io;
    ctx->user = user;

    if (ztk_thread_pool_async(g_pool, work_tramp, ctx, 0) != ZTK_OK) {
        free(ctx);
        return ZTK_ERR_PLATFORM;
    }
    return ZTK_OK;
}
