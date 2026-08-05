/**
 * @file log_throttle.c
 * @brief 按 key 的进程内日志节流。
 */
#include "zms/util/log_throttle.h"
#include "ztk/platform.h"
#include "ztk/util/log.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define ZMS_LOG_THROTTLE_MAX 48
#define ZMS_LOG_THROTTLE_KEY_MAX 96
#define ZMS_LOG_THROTTLE_MSG_MAX 4096

typedef struct {
    char key[ZMS_LOG_THROTTLE_KEY_MAX];
    uint64_t last_emit_ms;
    unsigned suppressed;
} zms_log_throttle_slot;

static zms_log_throttle_slot g_throttle[ZMS_LOG_THROTTLE_MAX];

static zms_log_throttle_slot *throttle_slot(const char *key, int create)
{
    zms_log_throttle_slot *free_slot = NULL;
    size_t i;

    if (!key || !key[0]) {
        return NULL;
    }
    for (i = 0; i < ZMS_LOG_THROTTLE_MAX; ++i) {
        if (g_throttle[i].key[0] == '\0') {
            if (!free_slot) {
                free_slot = &g_throttle[i];
            }
            continue;
        }
        if (strcmp(g_throttle[i].key, key) == 0) {
            return &g_throttle[i];
        }
    }
    if (!create || !free_slot) {
        return NULL;
    }
    strncpy(free_slot->key, key, sizeof(free_slot->key) - 1);
    free_slot->key[sizeof(free_slot->key) - 1] = '\0';
    return free_slot;
}

static void log_throttle_emit(ztk_log_level_t level, const char *key, uint64_t interval_ms,
                              const char *fmt, va_list ap)
{
    zms_log_throttle_slot *sl;
    uint64_t now;
    char msg[ZMS_LOG_THROTTLE_MSG_MAX];
    int n;

    sl = throttle_slot(key, 1);
    if (!sl) {
        return;
    }
    now = ztk_monotonic_ms();
    if (sl->last_emit_ms != 0 && interval_ms > 0 && now - sl->last_emit_ms < interval_ms) {
        sl->suppressed++;
        return;
    }

    n = vsnprintf(msg, sizeof(msg), fmt, ap);
    if (n < 0) {
        return;
    }
    if (sl->suppressed > 0) {
        int room = (int)sizeof(msg) - n;
        if (room > 0) {
            snprintf(msg + n, (size_t)room, " (suppressed %u)", sl->suppressed);
        }
        sl->suppressed = 0;
    }
    sl->last_emit_ms = now;
    ztk_log_write(level, __FILE__, __LINE__, "%s", msg);
}

void zms_log_warn_throttle(const char *key, uint64_t interval_ms, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    log_throttle_emit(ZTK_LOG_WARN, key, interval_ms, fmt, ap);
    va_end(ap);
}

void zms_log_debug_throttle(const char *key, uint64_t interval_ms, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    log_throttle_emit(ZTK_LOG_DEBUG, key, interval_ms, fmt, ap);
    va_end(ap);
}
