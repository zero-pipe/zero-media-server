/**
 * RTSP 服务示例（推 RECORD + 播放 PLAY）
 *
 *   demo_rtsp_server [--config config.ini] [--log file] [--port N] [--seconds N]
 */
#include "demo_server_runtime.h"
#include "zms/session/rtsp/rtsp_service.h"
#include "zms/session/rtsp/rtsp_session_auth.h"
#include "ztk/util/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int parse_rtsp_args(int argc, char **argv, zms_demo_server_args *args, uint16_t *port)
{
    if (zms_demo_server_parse_args(argc, argv, args) != 0) {
        return 1;
    }

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            *port = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--config") == 0) {
            ++i;
        } else if (strcmp(argv[i], "--log") == 0) {
            ++i;
        } else if (strcmp(argv[i], "--seconds") == 0) {
            ++i;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    zms_demo_server_args args;
    zms_demo_server_runtime rt;
    zms_rtsp_service *rtsp = NULL;
    zms_rtsp_service_opts sopts;
    uint16_t port = 0;
    ztk_err_t err;

    zms_demo_server_args_default(&args);
    if (parse_rtsp_args(argc, argv, &args, &port) != 0) {
        zms_demo_server_print_usage(argv[0], "[--port N]");
        return 1;
    }

    if (zms_demo_server_runtime_init(&rt, &args) != ZTK_OK) {
        return 1;
    }

    if (!port) {
        port = (uint16_t)rt.cfg.rtsp.port;
    }

    if (rt.cfg.rtsp.auth_user[0]) {
        zms_rtsp_auth_configure(rt.cfg.rtsp.auth_user, rt.cfg.rtsp.auth_pass);
        ztk_info("RTSP Digest auth enabled user=%s", rt.cfg.rtsp.auth_user);
    }
    zms_rtsp_set_test_reject_tcp_setup(rt.cfg.rtsp.test_reject_tcp_setup);

    memset(&sopts, 0, sizeof(sopts));
    sopts.poller_pool = rt.poller_pool;
    sopts.host = "0.0.0.0";
    sopts.port = port;

    rtsp = zms_rtsp_service_create(&sopts);
    if (!rtsp) {
        ztk_error("RTSP service create failed");
        zms_demo_server_runtime_fini(&rt);
        return 1;
    }

    err = zms_rtsp_service_start(rtsp);
    if (err != ZTK_OK) {
        ztk_error("RTSP listen failed port=%u: %s", (unsigned)port, ztk_strerror(err));
#if !defined(_WIN32)
        if (port < 1024) {
            ztk_error("RTSP port %u needs root on Linux; use e.g. 8554 in config.ini",
                      (unsigned)port);
        }
#endif
        zms_rtsp_service_destroy(rtsp);
        zms_demo_server_runtime_fini(&rt);
        return 1;
    }

    port = zms_rtsp_service_port(rtsp);
    printf("RTSP listen %u\r\n", (unsigned)port);
    printf("publish: ffmpeg -re -stream_loop -1 -i {file}.mp4 -c copy -f rtsp -rtsp_transport tcp "
           "rtsp://127.0.0.1:%u/live/{stream}\r\n",
           (unsigned)port);
    printf("play   : ffplay -rtsp_transport tcp rtsp://127.0.0.1:%u/live/{stream}\r\n",
           (unsigned)port);
    if (args.seconds < 0) {
        printf("Press Ctrl+C to exit.\r\n");
    }

    zms_demo_server_runtime_wait(&rt, args.seconds);

    zms_rtsp_service_stop(rtsp);
    zms_rtsp_service_destroy(rtsp);
    zms_demo_server_runtime_fini(&rt);
    return 0;
}
