/**
 * @file zms_server.c
 * @brief 媒体服务器门面：配置 / poller / 协议装配（自 demo_media_server 上收）。
 */
#include "zms/server.h"

#include "zms/engine/module_registry.h"
#include "zms/engine/stream/stream_hub.h"
#include "zms/engine/stream/stream_url.h"
#include "zms/egress/egress_pacing.h"
#include "zms/live/play/hls/http_hls_sidecar.h"
#include "zms/live/proxy/live_pull_proxy.h"
#include "zms/ops/api/webhook/webhook_client.h"
#include "zms/egress/egress_mp4_recorder.h"
#include "zms/ops/service/pull_ssl.h"
#include "zms/session/http/http_service.h"
#include "zms/session/rtmp/rtmp_service.h"
#include "zms/session/rtsp/rtsp_service.h"
#include "zms/session/rtsp/rtsp_session_auth.h"
#include "zms/session/rtsp/rtsp_transport.h"
#include "zms/util/buf_pool.h"
#include "zms/util/poller_drain.h"
#include "zms/engine/gop/gop_queue.h"
#include "zms/engine/gop/gop_limits.h"
#include "zms/ops/api/webapi/webapi.h"
#include "zms/vod/io/vod_source.h"
#include "zms/vod/io/vod_thread_pool.h"
#include "ztk/poller/poller_pool.h"
#include "ztk/util/log.h"
#include "ztk/ztk.h"

/** destroy 时排空 poller async 的等待（ms）；过短易漏掉跨线程 buf release */
#ifndef ZMS_SERVER_SHUTDOWN_DRAIN_MS
#define ZMS_SERVER_SHUTDOWN_DRAIN_MS 200u
#endif

#ifdef ZMS_HAVE_SRT
#include "zms/session/srt/srt_service.h"
#endif

#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct zms_server {
    zms_config cfg;
    ztk_poller_pool *poller_pool;
    ztk_poller *main_poller;
    unsigned poller_count;

    zms_server_hooks hooks;
    int hooks_set;

    zms_rtmp_service *rtmp;
    zms_http_service *http;
    zms_rtsp_service *rtsp;
#ifdef ZMS_HAVE_SRT
    zms_srt_service *srt;
#endif
    zms_live_pull_proxy *proxy;

    int started;
    volatile int stop_flag;
};

static zms_server *g_signal_server;

#if defined(_WIN32)
static BOOL WINAPI zms_server_win_ctrl(DWORD ctrl_type)
{
    if (ctrl_type == CTRL_C_EVENT || ctrl_type == CTRL_BREAK_EVENT ||
        ctrl_type == CTRL_CLOSE_EVENT || ctrl_type == CTRL_SHUTDOWN_EVENT) {
        if (g_signal_server) {
            g_signal_server->stop_flag = 1;
        }
        return TRUE;
    }
    return FALSE;
}
#else
static void zms_server_sig_stop(int sig)
{
    (void)sig;
    if (g_signal_server) {
        g_signal_server->stop_flag = 1;
    }
}

static void zms_server_sig_hup(int sig)
{
    (void)sig;
    ztk_log_reopen_file();
}
#endif

static ztk_err_t server_setup_logging(zms_config *cfg, const char *log_override)
{
    if (log_override && log_override[0]) {
        strncpy(cfg->general.log_file, log_override, sizeof(cfg->general.log_file) - 1);
        cfg->general.log_file[sizeof(cfg->general.log_file) - 1] = '\0';
    }

    ztk_log_set_level(cfg->general.log_level);
    if (cfg->general.log_file[0]) {
        if (ztk_log_open_file(cfg->general.log_file) != 0) {
            ztk_warn("failed to open log file: %s (console only)", cfg->general.log_file);
            return ZTK_OK;
        }
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

static ztk_err_t server_runtime_boot(zms_server *s, const char *log_override)
{
    ztk_poller_pool_opts_t popts;
    unsigned gop_tg;

    zms_egress_pacing_init(&s->cfg.play.pacing);
    zms_vod_config_apply(&s->cfg);
    zms_mp4_recorder_configure(&s->cfg.record);
    zms_buf_pool_init(&s->cfg);
    zms_vod_thread_pool_init(&s->cfg);
    gop_tg = s->cfg.general.gop_target_gops;
    if (gop_tg == 0) {
        gop_tg = ZMS_GOP_QUEUE_TARGET_GOPS;
    }
    zms_gop_queue_set_default_target_gops(gop_tg);
    zms_gop_queue_set_default_cache_ms(s->cfg.general.gop_cache_sec * 1000u);

    ztk_platform_init();
    zms_modules_register_all();

    if (server_setup_logging(&s->cfg, log_override) != ZTK_OK) {
        return ZTK_ERR_IO;
    }

    s->poller_count = s->cfg.general.poller_threads > 0 ? s->cfg.general.poller_threads : 2;
    popts = (ztk_poller_pool_opts_t){
        .size = s->poller_count,
        .prefer_current_thread = 0,
        .thread_priority = ZTK_THREAD_PRIO_NORMAL,
    };
    s->poller_pool = ztk_poller_pool_create(&popts);
    if (!s->poller_pool || ztk_poller_pool_start(s->poller_pool) != ZTK_OK) {
        ztk_error("poller_pool start failed");
        if (s->poller_pool) {
            ztk_poller_pool_destroy(s->poller_pool);
            s->poller_pool = NULL;
        }
        zms_vod_thread_pool_fini();
        zms_buf_pool_fini();
        return ZTK_ERR_IO;
    }

    s->main_poller = ztk_poller_pool_get(s->poller_pool, 0);
    zms_poller_pool_buf_attach(s->poller_pool, &s->cfg);
    return ZTK_OK;
}

static void server_runtime_shutdown(zms_server *s)
{
    if (s->poller_pool) {
        /*
         * 1) 线程仍在跑时排空 async（含无锁池跨线程 release 回投）
         * 2) join poller 线程
         * 3) 再排空一次（stop 后队列里可能还有尾包）
         * 4) 再 detach 本地池，避免 owner 已空仍入 freelist
         */
        zms_poller_pool_drain_async_wait(s->poller_pool, ZMS_SERVER_SHUTDOWN_DRAIN_MS);
        ztk_poller_pool_stop(s->poller_pool);
        zms_poller_pool_drain_async(s->poller_pool);
        zms_poller_pool_buf_detach(s->poller_pool);
        ztk_poller_pool_destroy(s->poller_pool);
        s->poller_pool = NULL;
    }
    s->main_poller = NULL;
    zms_vod_thread_pool_fini();
    zms_buf_pool_fini();
    zms_egress_pacing_fini();
    ztk_log_close_file();
    ztk_platform_fini();
}

static void server_fill_ports(const zms_server *s, zms_media_server_ports *ports)
{
    memset(ports, 0, sizeof(*ports));
    ports->rtmp = s->rtmp ? (unsigned)zms_rtmp_service_port(s->rtmp) : s->cfg.rtmp.port;
    ports->rtsp = s->rtsp ? (unsigned)zms_rtsp_service_port(s->rtsp) : s->cfg.rtsp.port;
    ports->http = s->http ? (unsigned)zms_http_service_port(s->http) : s->cfg.http.port;
#ifdef ZMS_HAVE_SRT
    ports->srt = s->srt ? (unsigned)zms_srt_service_port(s->srt) : s->cfg.srt.port;
#endif
}

static void print_play_urls(const zms_media_server_ports *ports, const zms_media_source *src)
{
    zms_media_urls u;

    if (!ports || !src) {
        return;
    }
    zms_media_urls_build(&u, src, ports);
    if (u.http_flv[0]) {
        printf("      HTTP-FLV : %s\r\n", u.http_flv);
    }
    if (u.hls[0]) {
        printf("      HTTP-HLS : %s\r\n", u.hls);
    }
    if (u.dash[0]) {
        printf("      HTTP-DASH: %s\r\n", u.dash);
    }
    if (u.rtsp_play[0]) {
        printf("      RTSP     : %s\r\n", u.rtsp_play);
    }
    if (u.rtmp_play[0]) {
        printf("      RTMP     : %s\r\n", u.rtmp_play);
    }
}

static void print_generic_play_urls(const zms_media_server_ports *ports, const zms_config *cfg)
{
    printf("      HTTP-FLV : http://127.0.0.1:%u/live/{stream}.flv\r\n", ports->http);
    printf("      HTTP-HLS : http://127.0.0.1:%u/live/{stream}.m3u8\r\n", ports->http);
    printf("      HTTP-DASH: http://127.0.0.1:%u/live/{stream}.mpd\r\n", ports->http);
    printf("      RTSP     : rtsp://127.0.0.1:%u/live/{stream}\r\n", ports->rtsp);
    printf("      RTMP     : rtmp://127.0.0.1:%u/live/{stream}\r\n", ports->rtmp);
#ifdef ZMS_HAVE_SRT
    if (ports->srt) {
        printf("      SRT      : srt://127.0.0.1:%u?streamid=#!::r=live/{stream},m=request\r\n",
               ports->srt);
    }
#endif
#if defined(ZMS_ENABLE_WEBRTC) && ZMS_ENABLE_WEBRTC
    if (!cfg || cfg->webrtc.enable) {
        printf(
            "      WHEP     : POST http://127.0.0.1:%u/index/api/whep?app=live&stream={stream}\r\n",
            ports->http);
        printf(
            "      WHIP     : POST http://127.0.0.1:%u/index/api/whip?app=live&stream={stream}\r\n",
            ports->http);
    }
#else
    (void)cfg;
#endif
}

static ztk_err_t server_start_services(zms_server *s)
{
    zms_config *cfg = &s->cfg;
    zms_media_events events;
    zms_http_hls_opts hls;
    zms_rtmp_service_opts ropts;
    zms_http_service_opts fopts;
    zms_rtsp_service_opts sopts;
    ztk_err_t err;
    const char *lhost;

    memset(&events, 0, sizeof(events));
    if (s->hooks_set) {
        events = s->hooks;
    }

    hls = (zms_http_hls_opts){
        .enable = cfg->protocol.enable_hls,
        .segment_duration_sec =
            cfg->hls.segment_duration_sec > 0.f ? cfg->hls.segment_duration_sec : 2.f,
        .segment_count = cfg->hls.segment_count ? cfg->hls.segment_count : 3,
        .enable_audio = cfg->protocol.enable_audio,
    };

    zms_http_hls_init(s->main_poller, &hls, &events, cfg->general.stream_none_reader_delay_ms);
    zms_webhook_init(s->main_poller, cfg);
    zms_webhook_server_started(cfg);

    lhost = cfg->general.listen_host[0] ? cfg->general.listen_host : "0.0.0.0";

    if (cfg->protocol.enable_rtmp) {
        memset(&ropts, 0, sizeof(ropts));
        ropts.poller_pool = s->poller_pool;
        ropts.host = lhost;
        ropts.port = (uint16_t)cfg->rtmp.port;
        s->rtmp = zms_rtmp_service_create(&ropts);
        if (!s->rtmp) {
            ztk_error("RTMP service create failed");
            return ZTK_ERR_IO;
        }
    }

    memset(&fopts, 0, sizeof(fopts));
    fopts.poller = s->main_poller;
    fopts.poller_pool = s->poller_pool;
    fopts.host = lhost;
    fopts.port = (uint16_t)cfg->http.port;
    fopts.api_secret = cfg->http.api_secret;
    fopts.cfg = cfg;
    s->http = zms_http_service_create(&fopts);
    if (!s->http) {
        ztk_error("HTTP service create failed");
        return ZTK_ERR_IO;
    }

    if (cfg->protocol.enable_rtsp) {
        memset(&sopts, 0, sizeof(sopts));
        sopts.poller_pool = s->poller_pool;
        sopts.host = lhost;
        sopts.port = (uint16_t)cfg->rtsp.port;
        sopts.advertise_host = cfg->general.extern_ip[0] ? cfg->general.extern_ip : NULL;
        s->rtsp = zms_rtsp_service_create(&sopts);
        if (!s->rtsp) {
            ztk_error("RTSP service create failed");
            return ZTK_ERR_IO;
        }
    }

#ifdef ZMS_HAVE_SRT
    if (cfg->protocol.enable_srt) {
        zms_srt_service_opts srtopts = {
            .poller = s->main_poller,
            .host = lhost,
            .port = (uint16_t)(cfg->srt.port ? cfg->srt.port : 9000),
        };
        s->srt = zms_srt_service_create(&srtopts);
        if (!s->srt) {
            ztk_error("SRT service create failed");
            return ZTK_ERR_IO;
        }
    }
#else
    if (cfg->protocol.enable_srt) {
        ztk_warn("enable_srt=1 but binary built without ZMS_ENABLE_SRT");
    }
#endif

    if (s->rtmp) {
        err = zms_rtmp_service_start(s->rtmp);
        if (err != ZTK_OK) {
            ztk_error("RTMP listen failed port=%u: %s", cfg->rtmp.port, ztk_strerror(err));
            return err;
        }
    }

    err = zms_http_service_start(s->http);
    if (err != ZTK_OK) {
        ztk_error("HTTP listen failed port=%u: %s", cfg->http.port, ztk_strerror(err));
        return err;
    }

    if (s->rtsp) {
        if (cfg->rtsp.auth_user[0]) {
            zms_rtsp_auth_configure(cfg->rtsp.auth_user, cfg->rtsp.auth_pass);
            ztk_info("RTSP Digest auth enabled user=%s", cfg->rtsp.auth_user);
        }
        zms_rtsp_set_test_reject_tcp_setup(cfg->rtsp.test_reject_tcp_setup);

        err = zms_rtsp_service_start(s->rtsp);
        if (err != ZTK_OK) {
            ztk_error("RTSP listen failed port=%u: %s", cfg->rtsp.port, ztk_strerror(err));
#if !defined(_WIN32)
            if (cfg->rtsp.port < 1024) {
                ztk_error("RTSP port %u needs root on Linux; use e.g. 8554 in config.ini",
                          cfg->rtsp.port);
            }
#endif
            return err;
        }
    }

#ifdef ZMS_HAVE_SRT
    if (s->srt) {
        err = zms_srt_service_start(s->srt);
        if (err != ZTK_OK) {
            ztk_error("SRT listen failed port=%u: %s", cfg->srt.port, ztk_strerror(err));
            return err;
        }
    }
#endif

    return ZTK_OK;
}

static ztk_err_t server_start_proxy(zms_server *s)
{
    zms_config *cfg = &s->cfg;

    if (!cfg->proxy.pull[0]) {
        return ZTK_OK;
    }

    {
        zms_live_pull_proxy_opts popt = {
            .poller_pool = s->poller_pool,
            .pull_url = cfg->proxy.pull,
            .ssl_ctx = zms_pull_ssl_ctx(cfg),
            .app = cfg->proxy.app[0] ? cfg->proxy.app : NULL,
            .stream = cfg->proxy.stream[0] ? cfg->proxy.stream : NULL,
            .proxy_prefix = cfg->proxy.prefix[0] ? cfg->proxy.prefix : NULL,
            .retry_count = cfg->proxy.retry_count,
            .reconnect_delay_ms = cfg->proxy.reconnect_delay_ms,
        };
        s->proxy = zms_live_pull_proxy_create(&popt);
        if (!s->proxy || zms_live_pull_proxy_start(s->proxy) != ZTK_OK) {
            ztk_error("live_pull_proxy start failed: %s", cfg->proxy.pull);
            zms_live_pull_proxy_destroy(s->proxy);
            s->proxy = NULL;
            return ZTK_ERR_IO;
        }
        ztk_info("proxy active: %s -> %s/%s", cfg->proxy.pull, zms_live_pull_proxy_app(s->proxy),
                 zms_live_pull_proxy_stream(s->proxy));
    }
    return ZTK_OK;
}

static void server_stop_services(zms_server *s)
{
    zms_http_hls_fini();
    zms_webhook_fini();
    zms_live_pull_proxy_destroy(s->proxy);
    s->proxy = NULL;
    if (s->rtsp) {
        zms_rtsp_service_stop(s->rtsp);
        zms_rtsp_service_destroy(s->rtsp);
        s->rtsp = NULL;
    }
    zms_rtsp_udp_registry_fini();
#ifdef ZMS_HAVE_SRT
    if (s->srt) {
        zms_srt_service_stop(s->srt);
        zms_srt_service_destroy(s->srt);
        s->srt = NULL;
    }
#endif
    if (s->http) {
        zms_http_service_stop(s->http);
        zms_http_service_destroy(s->http);
        s->http = NULL;
    }
    if (s->rtmp) {
        zms_rtmp_service_stop(s->rtmp);
        zms_rtmp_service_destroy(s->rtmp);
        s->rtmp = NULL;
    }
    zms_media_events_fini();
    s->started = 0;
}

static zms_server *server_alloc(void)
{
    zms_server *s = (zms_server *)calloc(1, sizeof(*s));
    return s;
}

zms_server *zms_server_create_from_ini(const char *config_path, const char *log_override)
{
    zms_server *s = server_alloc();
    if (!s) {
        return NULL;
    }

    zms_config_default(&s->cfg);
    if (config_path && config_path[0]) {
        if (zms_config_load_ini(&s->cfg, config_path) != ZTK_OK) {
            fprintf(stderr, "failed to load config: %s\r\n", config_path);
            free(s);
            return NULL;
        }
    }

    if (server_runtime_boot(s, log_override) != ZTK_OK) {
        free(s);
        return NULL;
    }
    return s;
}

zms_server *zms_server_create(const zms_config *cfg)
{
    zms_server *s = server_alloc();
    if (!s) {
        return NULL;
    }
    if (cfg) {
        s->cfg = *cfg;
    } else {
        zms_config_default(&s->cfg);
    }
    if (server_runtime_boot(s, NULL) != ZTK_OK) {
        free(s);
        return NULL;
    }
    return s;
}

void zms_server_destroy(zms_server *s)
{
    if (!s) {
        return;
    }
    if (g_signal_server == s) {
        g_signal_server = NULL;
    }
    if (s->started) {
        server_stop_services(s);
    }
    server_runtime_shutdown(s);
    free(s);
}

int zms_server_set_hooks(zms_server *s, const zms_server_hooks *hooks)
{
    if (!s) {
        return -1;
    }
    if (hooks) {
        s->hooks = *hooks;
        s->hooks_set = 1;
    } else {
        memset(&s->hooks, 0, sizeof(s->hooks));
        s->hooks_set = 0;
    }
    return 0;
}

ztk_err_t zms_server_start(zms_server *s)
{
    ztk_err_t err;

    if (!s) {
        return ZTK_ERR_INVALID;
    }
    if (s->started) {
        return ZTK_OK;
    }

    err = server_start_services(s);
    if (err != ZTK_OK) {
        server_stop_services(s);
        return err;
    }

    (void)server_start_proxy(s);
    s->started = 1;
    s->stop_flag = 0;

    {
        zms_media_server_ports ports;
        server_fill_ports(s, &ports);
        zms_web_api_note_boot();
        ztk_info("ZMS media server started (poller_pool=%u)", s->poller_count);
#ifdef _WIN32
        ztk_info("process pid=%lu", (unsigned long)GetCurrentProcessId());
#endif
        ztk_info("Listen RTMP=%u RTSP=%u HTTP=%u", ports.rtmp, ports.rtsp, ports.http);
#ifdef ZMS_HAVE_SRT
        if (s->srt) {
            ztk_info("Listen SRT=%u", ports.srt);
        }
#endif
        zms_media_events_set_server_ports(&ports);
        zms_media_source_log_registry(&ports);
    }
    return ZTK_OK;
}

void zms_server_stop(zms_server *s)
{
    if (!s) {
        return;
    }
    s->stop_flag = 1;
    if (s->started) {
        ztk_info("shutting down...");
        server_stop_services(s);
    }
}

void zms_server_request_stop(zms_server *s)
{
    if (s) {
        s->stop_flag = 1;
    }
}

void zms_server_install_default_signals(zms_server *s)
{
    g_signal_server = s;
#if defined(_WIN32)
    SetConsoleCtrlHandler(zms_server_win_ctrl, TRUE);
#else
    {
        struct sigaction sa;
        sa.sa_flags = 0;
        sigemptyset(&sa.sa_mask);
        sa.sa_handler = zms_server_sig_stop;
        sigaction(SIGTERM, &sa, NULL);
        sigaction(SIGINT, &sa, NULL);
        sa.sa_handler = zms_server_sig_hup;
        sigaction(SIGHUP, &sa, NULL);
        signal(SIGPIPE, SIG_IGN);
    }
#endif
}

void zms_server_run(zms_server *s, int seconds)
{
    if (!s) {
        return;
    }
    if (seconds < 0) {
        while (!s->stop_flag) {
            ztk_sleep_ms(200);
        }
        ztk_info("signal received, shutting down...");
    } else {
        int run = seconds > 0 ? seconds : 5;
        int elapsed = 0;
        printf("running for %d second(s)\r\n", run);
        while (!s->stop_flag && elapsed < run * 5) {
            ztk_sleep_ms(200);
            ++elapsed;
        }
    }
}

const zms_config *zms_server_config(const zms_server *s)
{
    return s ? &s->cfg : NULL;
}

int zms_server_is_running(const zms_server *s)
{
    return s && s->started;
}

void zms_server_print_endpoints(const zms_server *s)
{
    zms_media_server_ports ports;
    const zms_config *cfg;

    if (!s || !s->started) {
        return;
    }
    cfg = &s->cfg;
    server_fill_ports(s, &ports);

    printf("ZMS media server running (log: %s)\r\n", cfg->general.log_file);
    if (s->proxy) {
        printf("  [proxy] %s -> %s/%s\r\n", cfg->proxy.pull, zms_live_pull_proxy_app(s->proxy),
               zms_live_pull_proxy_stream(s->proxy));
    } else {
        printf("  [1] Publish:\r\n");
        if (s->rtmp) {
            printf("      ffmpeg -re -stream_loop -1 -i {file}.mp4 -c copy -f flv "
                   "rtmp://127.0.0.1:%u/live/{stream}\r\n",
                   ports.rtmp);
        }
#ifdef ZMS_HAVE_SRT
        if (s->srt) {
            printf("      ffmpeg -re -i {file}.mp4 -c copy -f mpegts "
                   "'srt://127.0.0.1:%u?streamid=#!::r=live/{stream},m=publish'\r\n",
                   ports.srt);
        }
#endif
    }
    printf("  [2] Play:\r\n");
    if (s->proxy) {
        zms_media_source *psrc = zms_live_pull_proxy_source(s->proxy);
        if (psrc) {
            print_play_urls(&ports, psrc);
        } else {
            print_generic_play_urls(&ports, cfg);
        }
    } else {
        print_generic_play_urls(&ports, cfg);
        printf("      HTTP-HLS VOD: http://127.0.0.1:%u/vod/{file}.m3u8\r\n", ports.http);
    }
    printf("Press Ctrl+C to exit.\r\n");
}
