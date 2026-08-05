#ifndef ZMS_SRC_SESSION_RTMP_INTERNAL_H
#define ZMS_SRC_SESSION_RTMP_INTERNAL_H

#include "rtmp-server.h"
#include "zms/egress/egress_pipeline.h"
#include "zms/media/container/demux_pipeline.h"
#include "zms/live/ingest/common/stream_ingest.h"
#include "zms/engine/gop/gop_queue.h"
#include "zms/vod/io/vod_buffer.h"
#include "zms/engine/media/media_limits.h"
#include "zms/engine/stream/stream_hub.h"
#include "zms/egress/egress_source.h"
#include "zms/egress/egress_clock.h"
#include "zms/engine/media_clock.h"
#include "ztk/net/tcp_server.h"
#include "ztk/thread/sync.h"
#include <stdint.h>

/* 播放侧 VOD lane：此处不完整（LoD）；vod_rtmp_play_session.c 含完整头。 */
typedef struct zms_vod_play_lane zms_vod_play_lane;

typedef enum zms_rtmp_session_state {
    ZMS_RTMP_SESSION_STATE_IDLE,
    ZMS_RTMP_SESSION_STATE_PUBLISHING,
    ZMS_RTMP_SESSION_STATE_PLAYING,
} zms_rtmp_session_state;

struct zms_rtmp_session {
    unsigned session_no;
    ztk_tcp_session *tcp;
    rtmp_server_t *rtmp_server;
    zms_rtmp_session_state state;
    char app[ZMS_APP_MAX];
    char stream[ZMS_STREAM_MAX];
    zms_media_source *source;
    zms_live_ingest *ingress;
    zms_demux_pipeline *publish_pipeline;
    zms_egress_source play;
    zms_gop_reader *gop_reader;
    zms_egress_pipeline *egress_pipe;
    zms_vod_buffer_reader *vod_reader;
    zms_vod_play_lane *vod_lane;
    int play_boot_sent;
    int play_video_armed;
    int play_live_catchup;
    uint64_t play_lag_resync_ms;
    int play_vod_catchup;
    int logged_audio;
    int logged_video;
    int play_reader_attached;
    uint64_t
        live_play_start_ms; /**< 直播播放开始 ztk_monotonic_ms()；供 on_play_stop 上报 duration_ms */
    int play_boot_pending;
    int destroy_scheduled;
    unsigned destroy_token;
    zms_egress_clock play_clk;
    zms_mux_av_timeline play_timeline;
    uint64_t vod_play_start_ms;
    uint32_t vod_last_seek_ms;
    uint32_t vod_pause_ms;
    int vod_pause_pos_valid;
    uint8_t *play_tag_buf;
    size_t play_tag_cap;
    uint8_t *play_es_buf;
    size_t play_es_cap;
    ztk_mutex *play_mtx;
};

typedef struct zms_rtmp_session zms_rtmp_session;

void zms_rtmp_session_play_teardown(zms_rtmp_session *s);
void zms_rtmp_session_teardown(zms_rtmp_session *s);
void zms_rtmp_session_send(zms_rtmp_session *s, const void *data, size_t len);
void zms_rtmp_session_flush_tcp(zms_rtmp_session *s);

void zms_rtmp_session_resolve_target(zms_rtmp_session *s, const char *app_in,
                                     const char *stream_in);
struct zms_media_source;
struct zms_media_source *zms_rtmp_session_find_play_source(const char *rtmp_app,
                                                           const char *play_name, char *out_app,
                                                           size_t app_cap, char *out_stream,
                                                           size_t stream_cap);
void zms_rtmp_session_play_readers_detach(zms_rtmp_session *s);
void zms_rtmp_session_play_readers_attach(zms_rtmp_session *s);

int rtmp_srv_send(void *param, const void *header, size_t header_len, const void *payload,
                  size_t payload_len);
int rtmp_srv_onpublish(void *param, const char *app_name, const char *stream_name,
                       const char *type);
int rtmp_srv_onplay(void *param, const char *app_name, const char *stream_name, double start,
                    double duration, uint8_t reset);
int rtmp_srv_onpause(void *param, int pause, uint32_t ms);
int rtmp_srv_onseek(void *param, uint32_t ms);
int rtmp_srv_onplayctrl(void *param, double speed);
int rtmp_srv_ongetduration(void *param, const char *app_name, const char *stream_name,
                           double *duration);
int rtmp_srv_onvideo(void *param, const void *data, size_t bytes, uint32_t timestamp);
int rtmp_srv_onaudio(void *param, const void *data, size_t bytes, uint32_t timestamp);
int rtmp_srv_onscript(void *param, const void *data, size_t bytes, uint32_t timestamp);

void zms_rtmp_session_play_bootstrap(zms_rtmp_session *s);
void zms_rtmp_session_play_kick(zms_rtmp_session *s);
void zms_rtmp_session_schedule_play_kick(zms_rtmp_session *s);
void zms_rtmp_session_play_flush(zms_rtmp_session *s);
void zms_rtmp_session_play_flush_nolock(zms_rtmp_session *s);
void zms_rtmp_session_egress_create(zms_rtmp_session *s);
void zms_rtmp_session_egress_close(zms_rtmp_session *s);
ztk_err_t zms_rtmp_session_play_vod_lane_attach(zms_rtmp_session *s, zms_media_source *src,
                                                uint64_t seek_ms);

/* --- 直播核心（rtmp_live_play_session.c）与
 *     VOD 播放路径（rtmp_vod_play_session.c）共享的会话 I/O 辅助。 --- */
int zms_rtmp_session_alive(zms_rtmp_session *s);
void zms_rtmp_session_play_fail(zms_rtmp_session *s);
void zms_rtmp_session_play_kick_finish(zms_rtmp_session *s);
void zms_rtmp_session_send_data_onstatus(zms_rtmp_session *s, const char *code, const char *desc);

/* --- VOD 播放控制辅助（rtmp_vod_play_session.c），由
 *     RTMP NetStream 命令 dispatch 驱动。 --- */
int zms_rtmp_session_vod_attach(zms_rtmp_session *s, uint64_t seek_ms);
int zms_rtmp_session_vod_replay_seek(zms_rtmp_session *s, uint64_t start_ms);
void zms_rtmp_session_play_kick_vod_prime(zms_rtmp_session *s, uint64_t start_ms);
int zms_rtmp_session_vod_lane_try_async(zms_rtmp_session *s, uint64_t start_ms);

void zms_rtmp_session_lock(zms_rtmp_session *s);
void zms_rtmp_session_unlock(zms_rtmp_session *s);
void zms_rtmp_session_publish_teardown(zms_rtmp_session *s);
void zms_rtmp_session_publisher_kick(void *ctx, int force);

#endif /* ZMS_SRC_SESSION_RTMP_INTERNAL_H */
