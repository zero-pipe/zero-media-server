/**
 * RTSP 播放示例（RTP-over-TCP interleaved，H264）
 *
 * 用法: demo_rtsp_player rtsp://host:554/live/{stream}
 */
#include "ztk/ztk.h"
#include "zms/zms.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile int g_stop;

static void on_ready(void *user)
{
    (void)user;
    printf("[rtsp] playing\r\n");
}

static void on_track(const zms_media_track *track, void *user)
{
    (void)user;
    printf("[rtsp] track type=%d codec=%d pt=%d control=%s\r\n", (int)track->type,
           (int)track->codec, track->payload_type, track->control);
}

static void on_frame(const zms_frame *frame, void *user)
{
    static unsigned count;
    (void)user;
    count++;
    if (count <= 5 || (count % 100) == 0) {
        printf("[rtsp] frame #%u size=%zu key=%d pts=%llu\r\n", count, frame->size, frame->keyframe,
               (unsigned long long)frame->pts_ms);
    }
}

static void on_error(ztk_err_t err, void *user)
{
    (void)user;
    printf("[rtsp] error: %s\r\n", ztk_strerror(err));
    g_stop = 1;
}

int main(int argc, char **argv)
{
    const char *url = argc > 1 ? argv[1] : "rtsp://127.0.0.1:554/live/demo";
    printf("ZMS RTSP player demo: %s\r\n", url);

    ztk_poller *poller = ztk_poller_create();
    if (!poller) {
        fprintf(stderr, "poller create failed\r\n");
        return 1;
    }

    zms_player_opts opts = {
        .poller = poller,
        .url = url,
        .rtsp_rtp_mode = ZMS_RTSP_RTP_AUTO,
        .on_ready = on_ready,
        .on_track = on_track,
        .on_frame = on_frame,
        .on_error = on_error,
        .user = NULL,
    };

    zms_player *player = zms_player_create(&opts);
    if (!player) {
        fprintf(stderr, "player create failed\r\n");
        ztk_poller_destroy(poller);
        return 1;
    }

    if (zms_player_play(player) != ZTK_OK) {
        fprintf(stderr, "play failed\r\n");
        zms_player_destroy(player);
        ztk_poller_destroy(poller);
        return 1;
    }

    ztk_poller_run(poller, &g_stop);

    zms_player_destroy(player);
    ztk_poller_destroy(poller);
    return 0;
}
