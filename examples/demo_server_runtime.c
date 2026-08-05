#include "demo_server_runtime.h"

#include "zms/media/container/flv/flv_tag_pack.h"
#include "zms/egress/egress_pacing.h"
#include "zms/vod/io/vod_thread_pool.h"
#include "zms/engine/module_registry.h"
#include "zms/util/buf_pool.h"
#include "zms/util/poller_drain.h"
#include "zms/vod/io/vod_source.h"
#include "ztk/util/log.h"
#include "ztk/ztk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <signal.h>
#endif

/* 全局停止标志，由信号/控制台处理器置 1 */
volatile int g_zms_stop = 0;

#if defined(_WIN32)
static BOOL WINAPI win_ctrl_handler(DWORD ctrl_type)
{
    if (ctrl_type == CTRL_C_EVENT || ctrl_type == CTRL_BREAK_EVENT ||
        ctrl_type == CTRL_CLOSE_EVENT || ctrl_type == CTRL_SHUTDOWN_EVENT) {
        g_zms_stop = 1;
        return TRUE;
    }
    return FALSE;
}
#else
static void sig_stop(int sig)
{
    (void)sig;
    g_zms_stop = 1;
}

static void sig_hup(int sig)
{
    (void)sig;
    ztk_log_reopen_file();
    /* 注意：ztk_info 在信号处理器里不严格安全，但日志函数仅做 fprintf，实践上可接受 */
    ztk_info("SIGHUP: log file reopened");
}
#endif

static void demo_install_signals(void)
{
#if defined(_WIN32)
    SetConsoleCtrlHandler(win_ctrl_handler, TRUE);
#else
    struct sigaction sa;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);

    sa.sa_handler = sig_stop;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    sa.sa_handler = sig_hup;
    sigaction(SIGHUP, &sa, NULL);

    /* 忽略 SIGPIPE，防止向断开的客户端写入时进程崩溃 */
    signal(SIGPIPE, SIG_IGN);
#endif
}

void zms_demo_server_args_default(zms_demo_server_args *args)
{
    if (!args) {
        return;
    }
    args->config_path = NULL;
    args->log_file_override = NULL;
    args->seconds = -1;
}

static void demo_usage(const char *prog, const char *extra)
{
    fprintf(stderr, "usage: %s [--config config.ini] [--log file] [--seconds N]", prog);
    if (extra && extra[0]) {
        fprintf(stderr, " %s", extra);
    }
    fputc('\n', stderr);
}

void zms_demo_server_print_usage(const char *prog, const char *extra)
{
    demo_usage(prog, extra);
}

int zms_demo_server_parse_args(int argc, char **argv, zms_demo_server_args *args)
{
    if (!args) {
        return 1;
    }
    zms_demo_server_args_default(args);

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            args->config_path = argv[++i];
        } else if (strcmp(argv[i], "--log") == 0 && i + 1 < argc) {
            args->log_file_override = argv[++i];
        } else if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            args->seconds = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            return 0;
        } else {
            return 1;
        }
    }
    return 0;
}

static ztk_err_t demo_setup_logging(zms_config *cfg, const char *log_override)
{
    if (log_override && log_override[0]) {
        strncpy(cfg->general.log_file, log_override, sizeof(cfg->general.log_file) - 1);
    }

    ztk_log_set_level(cfg->general.log_level);
    if (cfg->general.log_file[0]) {
        if (ztk_log_open_file(cfg->general.log_file) != 0) {
            ztk_warn("failed to open log file: %s (console only)", cfg->general.log_file);
            return ZTK_OK;
        }
        /* 配置按大小轮转（log_max_mb=0 表示不轮转） */
        if (cfg->general.log_max_mb > 0) {
            ztk_log_set_rotate((size_t)cfg->general.log_max_mb * 1024u * 1024u,
                               cfg->general.log_keep_count);
            ztk_info("logging to stderr and %s (rotate every %u MB, keep %d)",
                     cfg->general.log_file, cfg->general.log_max_mb, cfg->general.log_keep_count);
        } else {
            ztk_info("logging to stderr and %s (no rotate)", cfg->general.log_file);
        }
    }
    return ZTK_OK;
}

ztk_err_t zms_demo_server_runtime_init(zms_demo_server_runtime *rt,
                                       const zms_demo_server_args *args)
{
    ztk_poller_pool_opts_t popts;

    if (!rt) {
        return ZTK_ERR_INVALID;
    }
    memset(rt, 0, sizeof(*rt));

    zms_config_default(&rt->cfg);
    if (args && args->config_path && args->config_path[0]) {
        if (zms_config_load_ini(&rt->cfg, args->config_path) != ZTK_OK) {
            fprintf(stderr, "failed to load config: %s\r\n", args->config_path);
            return ZTK_ERR_IO;
        }
    }

    zms_egress_pacing_init(&rt->cfg.play.pacing);
    zms_vod_config_apply(&rt->cfg);
    zms_buf_pool_init(&rt->cfg);
    zms_vod_thread_pool_init(&rt->cfg);

    ztk_platform_init();
    zms_modules_register_all();

    if (demo_setup_logging(&rt->cfg, args ? args->log_file_override : NULL) != ZTK_OK) {
        return ZTK_ERR_IO;
    }

    rt->poller_count = rt->cfg.general.poller_threads > 0 ? rt->cfg.general.poller_threads : 2;
    popts = (ztk_poller_pool_opts_t){
        .size = rt->poller_count,
        .prefer_current_thread = 0,
        .thread_priority = ZTK_THREAD_PRIO_NORMAL,
    };
    rt->poller_pool = ztk_poller_pool_create(&popts);
    if (!rt->poller_pool || ztk_poller_pool_start(rt->poller_pool) != ZTK_OK) {
        ztk_error("poller_pool start failed");
        if (rt->poller_pool) {
            ztk_poller_pool_destroy(rt->poller_pool);
        }
        rt->poller_pool = NULL;
        zms_vod_thread_pool_fini();
        zms_buf_pool_fini();
        return ZTK_ERR_IO;
    }

    rt->main_poller = ztk_poller_pool_get(rt->poller_pool, 0);
    zms_poller_pool_buf_attach(rt->poller_pool, &rt->cfg);
    return ZTK_OK;
}

void zms_demo_server_runtime_fini(zms_demo_server_runtime *rt)
{
    if (!rt) {
        return;
    }
    if (rt->poller_pool) {
        zms_poller_pool_drain_async_wait(rt->poller_pool, 50);
        zms_poller_pool_buf_detach(rt->poller_pool);
        ztk_poller_pool_stop(rt->poller_pool);
        ztk_poller_pool_destroy(rt->poller_pool);
        rt->poller_pool = NULL;
    }
    zms_vod_thread_pool_fini();
    zms_buf_pool_fini();
    zms_egress_pacing_fini();
    ztk_log_close_file();
    ztk_platform_fini();
    rt->main_poller = NULL;
}

void zms_demo_server_runtime_wait(const zms_demo_server_runtime *rt, int seconds)
{
    (void)rt;
    demo_install_signals();
    if (seconds < 0) {
        /* 永久运行，直到收到 SIGTERM/SIGINT */
        while (!g_zms_stop) {
            ztk_sleep_ms(200);
        }
        ztk_info("signal received, shutting down...");
    } else {
        int run = seconds > 0 ? seconds : 5;
        int elapsed = 0;
        printf("running for %d second(s)\r\n", run);
        while (!g_zms_stop && elapsed < run * 5) {
            ztk_sleep_ms(200);
            ++elapsed;
        }
    }
}
