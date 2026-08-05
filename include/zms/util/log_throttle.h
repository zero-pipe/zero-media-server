#ifndef ZMS_UTIL_LOG_LOG_THROTTLE_H
#define ZMS_UTIL_LOG_LOG_THROTTLE_H

/**
 * @file log_throttle.h
 * @brief 按 key 限制重复日志行（进程内）。
 */
#include "zms/zms_export.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 相同 @a key 的 warn 行之间的默认最小间隔。 */
#define ZMS_LOG_THROTTLE_WARN_MS 60000u

ZMS_API void zms_log_warn_throttle(const char *key, uint64_t interval_ms, const char *fmt, ...);

ZMS_API void zms_log_debug_throttle(const char *key, uint64_t interval_ms, const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_UTIL_LOG_LOG_THROTTLE_H */
