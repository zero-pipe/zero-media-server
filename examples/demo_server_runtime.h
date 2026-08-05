#ifndef ZMS_DEMO_SERVER_RUNTIME_H
#define ZMS_DEMO_SERVER_RUNTIME_H

/**
 * 示例程序公共运行时：配置加载、日志、poller_pool、缓冲池等。
 * demo_rtsp_server / demo_rtmp_server / demo_media_server 共用同一套初始化语义。
 *
 * 信号处理（仅 POSIX）：
 *   SIGTERM / SIGINT  → 设置 g_zms_stop = 1，wait() 返回，进程优雅退出
 *   SIGHUP            → reopen 日志文件（配合 logrotate 使用）
 */
#include "zms/ops/service/config.h"
#include "ztk/poller/poller_pool.h"
#include "ztk/ztk_errno.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zms_demo_server_args {
    const char *config_path;
    const char *log_file_override;
    /** -1 = 一直运行；>= 0 = 运行指定秒数后退出 */
    int seconds;
} zms_demo_server_args;

typedef struct zms_demo_server_runtime {
    zms_config cfg;
    ztk_poller_pool *poller_pool;
    ztk_poller *main_poller;
    unsigned poller_count;
} zms_demo_server_runtime;

/**
 * 全局停止标志：SIGTERM/SIGINT 信号处理器将其置 1。
 * 主循环应检查该标志以实现优雅退出。
 */
extern volatile int g_zms_stop;

void zms_demo_server_args_default(zms_demo_server_args *args);

/** 解析 --config / --log / --seconds / -h；成功返回 0，help 返回 0，错误返回 1 */
int zms_demo_server_parse_args(int argc, char **argv, zms_demo_server_args *args);

void zms_demo_server_print_usage(const char *prog, const char *extra);

ztk_err_t zms_demo_server_runtime_init(zms_demo_server_runtime *rt,
                                       const zms_demo_server_args *args);

void zms_demo_server_runtime_fini(zms_demo_server_runtime *rt);

/** seconds < 0 时阻塞直到收到 SIGTERM/SIGINT；否则 sleep 指定秒数后返回 */
void zms_demo_server_runtime_wait(const zms_demo_server_runtime *rt, int seconds);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_DEMO_SERVER_RUNTIME_H */
