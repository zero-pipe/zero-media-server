/**
 * RTSP 拉流代理示例：从外部 RTSP 拉流，注册为本地 live/stream
 *
 * 用法（需先有 demo_media_server 在跑）：
 *   demo_proxy_pull rtsp://192.168.1.100:554/live/stream/<id> [app] [stream] [udp]
 * stream 省略时 auto 注册为 live/proxied/stream/<id>（方案 A）。
 * 注意：独立进程仅用于验证 rtsp_client 拉流；真正对外播放请用 demo_media_server 内置 proxy_pull HTTP API addStreamProxy（与 server 同进程） */
#include "zms/engine/stream/stream_url.h"
#include "zms/live/proxy/live_pull_proxy.h"
#include "zms/session/rtsp/rtsp_transport.h"
#include "ztk/util/log.h"
#include "ztk/ztk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 全局停止标志，由信号/控制台处理器置 1 */
volatile int g_zms_stop = 0;

#if defined(_WIN32)
#include <windows.h>
static BOOL WINAPI proxy_ctrl_handler(DWORD t)
{
    if (t == CTRL_C_EVENT || t == CTRL_BREAK_EVENT || t == CTRL_CLOSE_EVENT ||
        t == CTRL_SHUTDOWN_EVENT) {
        g_zms_stop = 1;
        return TRUE;
    }
    return FALSE;
}
#else
#include <signal.h>
static void proxy_sig_stop(int s)
{
    (void)s;
    g_zms_stop = 1;
}
static void proxy_sig_hup(int s)
{
    (void)s;
    ztk_log_reopen_file();
}
#endif

static void proxy_install_signals(void)
{
#if defined(_WIN32)
    SetConsoleCtrlHandler(proxy_ctrl_handler, TRUE);
#else
    struct sigaction sa;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = proxy_sig_stop;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sa.sa_handler = proxy_sig_hup;
    sigaction(SIGHUP, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
#endif
}
static zms_live_pull_proxy *g_proxy;

static void on_proxy_ready(void *user)
{
    (void)user;
    zms_media_source *src = g_proxy ? zms_live_pull_proxy_source(g_proxy) : NULL;
    if (!src) {
        return;
    }
    zms_media_server_ports ports = {.rtmp = 1935, .rtsp = 554, .http = 8080};
    zms_media_urls urls;
    zms_media_urls_build(&urls, src, &ports);
    printf("proxy ready play locally:\r\n");
    printf("  RTSP:     %s\r\n", urls.rtsp_play);
    printf("  RTMP:     %s\r\n", urls.rtmp_play);
    printf("  HTTP-FLV: %s\r\n", urls.http_flv);
    printf("  HLS:      %s\r\n", urls.hls);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <pull_url> [app] [stream|auto] [udp]\r\n", argv[0]);
        return 1;
    }

    ztk_platform_init();
    ztk_log_set_level(ZTK_LOG_INFO);

    ztk_poller *poller = ztk_poller_create();
    if (!poller) {
        fprintf(stderr, "poller create failed\r\n");
        return 1;
    }

    zms_live_pull_proxy_opts opts = {0};
    opts.poller = poller;
    opts.pull_url = argv[1];
    opts.app = argc > 2 ? argv[2] : NULL;
    opts.stream = argc > 3 ? argv[3] : NULL;
    if (argc > 4 && (strcmp(argv[4], "udp") == 0 || strcmp(argv[4], "UDP") == 0)) {
        opts.rtp_mode = ZMS_RTSP_RTP_UDP;
    }
    opts.on_ready = on_proxy_ready;

    g_proxy = zms_live_pull_proxy_create(&opts);
    zms_live_pull_proxy *proxy = g_proxy;
    if (!proxy || zms_live_pull_proxy_start(proxy) != ZTK_OK) {
        fprintf(stderr, "live_pull_proxy start failed\r\n");
        ztk_poller_destroy(poller);
        return 1;
    }

    printf("live pull proxy %s -> %s/%s (Ctrl+C to stop)\r\n", argv[1],
           zms_live_pull_proxy_app(proxy), zms_live_pull_proxy_stream(proxy));
    proxy_install_signals();
    ztk_poller_run(poller, &g_zms_stop);

    zms_live_pull_proxy_destroy(proxy);
    ztk_poller_destroy(poller);
    return 0;
}
