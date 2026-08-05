#include "zms/util/poller_drain.h"
#include "ztk/poller/poller.h"
#include "ztk/platform.h"

void zms_poller_pool_drain_async(ztk_poller_pool *pool)
{
    unsigned i;
    unsigned n;

    if (!pool) {
        return;
    }
    n = ztk_poller_pool_size(pool);
    for (i = 0; i < n; ++i) {
        ztk_poller *p = ztk_poller_pool_at(pool, i);
        if (!p) {
            continue;
        }
        (void)ztk_poller_wake(p);
        ztk_poller_process_pending(p);
    }
}

void zms_poller_pool_drain_async_wait(ztk_poller_pool *pool, unsigned wait_ms)
{
    zms_poller_pool_drain_async(pool);
    if (wait_ms > 0) {
        ztk_sleep_ms((int)wait_ms);
    }
    zms_poller_pool_drain_async(pool);
}
