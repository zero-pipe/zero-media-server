/**
 * RTMP 服务示例（推 publish + 播放 play）
 *
 *   demo_rtmp_server [--config config.ini] [--log file] [--port N] [--seconds N]
 */
#include "demo_server_runtime.h"
#include "zms/session/rtmp/rtmp_service.h"
#include "ztk/util/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int parse_rtmp_args(int argc, char **argv, zms_demo_server_args *args, uint16_t *port)
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
    zms_rtmp_service *rtmp = NULL;
    zms_rtmp_service_opts ropts;
    uint16_t port = 0;
    ztk_err_t err;

    zms_demo_server_args_default(&args);
    if (parse_rtmp_args(argc, argv, &args, &port) != 0) {
        zms_demo_server_print_usage(argv[0], "[--port N]");
        return 1;
    }

    if (zms_demo_server_runtime_init(&rt, &args) != ZTK_OK) {
        return 1;
    }

    if (!port) {
        port = (uint16_t)rt.cfg.rtmp.port;
    }

    memset(&ropts, 0, sizeof(ropts));
    ropts.poller_pool = rt.poller_pool;
    ropts.host = "0.0.0.0";
    ropts.port = port;

    rtmp = zms_rtmp_service_create(&ropts);
    if (!rtmp) {
        ztk_error("RTMP service create failed");
        zms_demo_server_runtime_fini(&rt);
        return 1;
    }

    err = zms_rtmp_service_start(rtmp);
    if (err != ZTK_OK) {
        ztk_error("RTMP listen failed port=%u: %s", (unsigned)port, ztk_strerror(err));
        zms_rtmp_service_destroy(rtmp);
        zms_demo_server_runtime_fini(&rt);
        return 1;
    }

    port = zms_rtmp_service_port(rtmp);
    printf("RTMP listen %u\r\n", (unsigned)port);
    printf("publish: ffmpeg -re -stream_loop -1 -i {file}.mp4 -c copy -f flv "
           "rtmp://127.0.0.1:%u/live/{stream}\r\n",
           (unsigned)port);
    printf("play   : ffplay rtmp://127.0.0.1:%u/live/{stream}\r\n", (unsigned)port);
    if (args.seconds < 0) {
        printf("Press Ctrl+C to exit.\r\n");
    }

    zms_demo_server_runtime_wait(&rt, args.seconds);

    zms_rtmp_service_stop(rtmp);
    zms_rtmp_service_destroy(rtmp);
    zms_demo_server_runtime_fini(&rt);
    return 0;
}
