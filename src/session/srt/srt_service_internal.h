#ifndef ZMS_SRC_SESSION_SRT_SERVICE_INTERNAL_H
#define ZMS_SRC_SESSION_SRT_SERVICE_INTERNAL_H

#include "srt_service_struct.h"
#include "zms/media/container/demux_pipeline.h"
#include "zms/live/ingest/common/stream_ingest.h"
#include "zms/egress/egress_source.h"
#include "zms/engine/stream/stream_hub.h"
#include "ztk/poller/poller.h"
#include <stdint.h>

/* Play muxer：此处不完整（LoD）；srt_play_session.c / teardown .c 需 include mpegts_egress.h。 */
typedef struct zms_mpegts_egress zms_mpegts_egress;

#ifdef ZMS_HAVE_SRT
#include <srt/srt.h>
#else
typedef int SRTSOCKET;
#endif

typedef enum zms_srt_session_mode {
    ZMS_SRT_SESSION_MODE_PUBLISH = 0,
    ZMS_SRT_SESSION_MODE_PLAY = 1,
} zms_srt_session_mode;

struct zms_srt_session {
    zms_srt_service *server;
    struct zms_srt_session *next;
    unsigned session_no;
    SRTSOCKET sock;
    ztk_poller *poller;
    volatile int stopping;
    volatile int destroy_scheduled;
    zms_srt_session_mode mode;
    zms_live_ingest *ingress;
    zms_media_source *source;
    zms_demux_pipeline *publish_pipeline;
    zms_egress_source play;
    zms_mpegts_egress *live_muxer;
    int reader_attached;
    uint64_t
        play_start_ms; /**< 播放开始 ztk_monotonic_ms()；供 on_play_stop 上报 duration_ms */
    int logged_recv;
    int logged_send;
    int logged_recv_stat;
    int logged_recv_stall;
    uint64_t recv_bytes;
    uint64_t send_bytes;
    uint64_t last_recv_bytes;
    unsigned recv_calls;
    unsigned stall_polls;
    char app[ZMS_APP_MAX];
    char stream[ZMS_STREAM_MAX];
};

void zms_srt_session_publisher_kick(void *ctx, int force);
void zms_srt_session_schedule_destroy(zms_srt_session *sess);
void zms_srt_session_destroy_now(zms_srt_session *sess);
void zms_srt_apply_session_opts(SRTSOCKET sock);

int zms_srt_session_begin_publish(zms_srt_session *sess, const char *app, const char *stream,
                                  const char *streamid);
void zms_srt_session_teardown_publish(zms_srt_session *sess);
void zms_srt_session_finish_publish(zms_srt_session *sess);
void zms_srt_session_drain_recv(zms_srt_session *sess);

int zms_srt_session_begin_play(zms_srt_session *sess, const char *app, const char *stream,
                               const char *streamid);
void zms_srt_session_teardown_play(zms_srt_session *sess);
void zms_srt_session_finish_play(zms_srt_session *sess);
void zms_srt_session_pump_send(zms_srt_session *sess);

#endif /* ZMS_SRC_SESSION_SRT_SERVICE_INTERNAL_H */
