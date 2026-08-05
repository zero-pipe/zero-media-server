
#include "zms/engine/media_event.h"
#include "zms/engine/stream/stream_url.h"
#include "zms/ops/api/webhook/webhook_client.h"
#include "zms/egress/egress_mp4_recorder.h"
#include "zms/media/codec/codec_id.h"
#include "zms/vod/io/vod_source.h"
#include "ztk/poller/poller.h"
#include "ztk/platform.h"
#include "ztk/thread/sync.h"
#include "ztk/util/log.h"
#include "ztk/util/timer.h"
#include <stdlib.h>
#include <string.h>

/*
 * 全局媒体事件上下文。
 * 启动时由 zms_media_events_set() / zms_media_events_set_server_ports() 设置一次，
 * 须在任何工作线程启动前完成，故无需互斥锁。
 */
typedef struct zms_media_event_ctx {
    zms_media_events events;
    ztk_poller *poller;
    int none_reader_delay_ms;
    zms_media_server_ports server_ports;
    int has_server_ports;
} zms_media_event_ctx;

static zms_media_event_ctx g_ctx = {
    {NULL, NULL, NULL, NULL, NULL, NULL, NULL}, NULL, 20000, {0, 0, 0, 0}, 0};

ZMS_API const char *zms_media_origin_str(zms_media_origin origin)
{
    switch (origin) {
    case ZMS_ORIGIN_RTMP_PUSH:
        return "rtmp_push";
    case ZMS_ORIGIN_RTSP_PUSH:
        return "rtsp_push";
    case ZMS_ORIGIN_SRT_PUSH:
        return "srt_push";
    case ZMS_ORIGIN_RTP_PS_PUSH:
        return "rtp_ps_push";
    case ZMS_ORIGIN_WEBRTC_PUSH:
        return "webrtc_push";
    case ZMS_ORIGIN_PULL:
        return "pull";
    case ZMS_ORIGIN_MP4_VOD:
        return "mp4_vod";
    default:
        return "unknown";
    }
}

void zms_media_events_set_server_ports(const zms_media_server_ports *ports)
{
    if (!ports) {
        return;
    }
    g_ctx.server_ports = *ports;
    g_ctx.has_server_ports = 1;
}

const zms_media_server_ports *zms_media_events_server_ports(void)
{
    return g_ctx.has_server_ports ? &g_ctx.server_ports : NULL;
}

void zms_media_events_set(ztk_poller *poller, const zms_media_events *events,
                          int none_reader_delay_ms)
{
    g_ctx.poller = poller;
    if (events) {
        g_ctx.events = *events;
    } else {
        memset(&g_ctx.events, 0, sizeof(g_ctx.events));
    }
    if (none_reader_delay_ms > 0) {
        g_ctx.none_reader_delay_ms = none_reader_delay_ms;
    }
}

ztk_poller *zms_media_events_poller(void)
{
    return g_ctx.poller;
}

void zms_media_tuple_from_source(const zms_media_source *src, zms_media_tuple *tuple)
{
    if (!tuple) {
        return;
    }
    memset(tuple, 0, sizeof(*tuple));
    if (!src) {
        return;
    }
    strncpy(tuple->schema, src->schema, sizeof(tuple->schema) - 1);
    strncpy(tuple->app, src->app, sizeof(tuple->app) - 1);
    strncpy(tuple->stream, src->stream, sizeof(tuple->stream) - 1);
}

typedef struct zms_media_none_reader_ctx {
    zms_media_tuple tuple;
    ztk_poller_timer *timer;
    struct zms_media_none_reader_ctx *next;
} zms_media_none_reader_ctx;

static zms_media_none_reader_ctx *g_none_reader_pending;
static ztk_mutex *g_none_reader_mtx;
static int g_media_events_fini;

static void none_reader_pending_init(void)
{
    if (!g_none_reader_mtx) {
        g_none_reader_mtx = ztk_mutex_create(ZTK_MUTEX_NORMAL);
    }
}

static void none_reader_pending_add(zms_media_none_reader_ctx *ctx)
{
    if (!ctx) {
        return;
    }
    none_reader_pending_init();
    ztk_mutex_lock(g_none_reader_mtx);
    ctx->next = g_none_reader_pending;
    g_none_reader_pending = ctx;
    ztk_mutex_unlock(g_none_reader_mtx);
}

static void none_reader_pending_remove(zms_media_none_reader_ctx *ctx)
{
    zms_media_none_reader_ctx **pp;

    if (!ctx || !g_none_reader_mtx) {
        return;
    }
    ztk_mutex_lock(g_none_reader_mtx);
    pp = &g_none_reader_pending;
    while (*pp) {
        if (*pp == ctx) {
            *pp = ctx->next;
            break;
        }
        pp = &(*pp)->next;
    }
    ztk_mutex_unlock(g_none_reader_mtx);
}

static uint64_t on_none_reader_fire(void *user)
{
    zms_media_none_reader_ctx *ctx = (zms_media_none_reader_ctx *)user;

    if (!ctx) {
        return 0;
    }
    none_reader_pending_remove(ctx);
    ctx->timer = NULL;

    if (g_media_events_fini || !g_ctx.poller) {
        free(ctx);
        return 0;
    }

    zms_media_source *src =
        zms_media_source_find(ctx->tuple.schema, ctx->tuple.app, ctx->tuple.stream);
    int readers = src ? zms_media_source_reader_count(src) : 0;
    if (readers > 0) {
        free(ctx);
        return 0;
    }

    ztk_debug("[event] none_reader app=%s stream=%s", ctx->tuple.app, ctx->tuple.stream);
    if (g_ctx.events.on_media_none_reader) {
        g_ctx.events.on_media_none_reader(&ctx->tuple, g_ctx.events.user);
    }
    zms_webhook_on_none_reader(&ctx->tuple);
    if (src && zms_media_source_is_vod(src)) {
        zms_vod_source_stop(src);
    }
    free(ctx);
    return 0;
}

static void schedule_none_reader_check(const zms_media_tuple *tuple)
{
    zms_media_none_reader_ctx *ctx;

    if (g_media_events_fini || !g_ctx.poller || g_ctx.none_reader_delay_ms <= 0 || !tuple) {
        return;
    }
    {
        zms_media_source *src = zms_media_source_find(tuple->schema, tuple->app, tuple->stream);
        int is_vod = src && zms_media_source_is_vod(src);
        if (!is_vod && !g_ctx.events.on_media_none_reader &&
            !zms_webhook_none_reader_configured()) {
            return;
        }
    }
    ctx = (zms_media_none_reader_ctx *)malloc(sizeof(*ctx));
    if (!ctx) {
        return;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->tuple = *tuple;
    ctx->timer = ztk_poller_do_delay(g_ctx.poller, (uint64_t)g_ctx.none_reader_delay_ms,
                                     on_none_reader_fire, ctx);
    if (!ctx->timer) {
        free(ctx);
        return;
    }
    none_reader_pending_add(ctx);
}

static void notify_reader_changed(zms_media_source *src)
{
    if (!src) {
        return;
    }
    zms_media_tuple tuple;
    zms_media_tuple_from_source(src, &tuple);
    int count = zms_media_source_reader_count(src);
    if (g_ctx.events.on_media_reader_changed) {
        g_ctx.events.on_media_reader_changed(&tuple, count, g_ctx.events.user);
    }

    if (count > 0) {
        return;
    }
    schedule_none_reader_check(&tuple);
}

void zms_media_event_publish(zms_media_source *src, zms_media_origin origin)
{
    if (!src) {
        return;
    }
    zms_media_source_registry_lock();
    src->publish_origin = (int)origin;
    src->publishing = 1;
    zms_media_source_registry_unlock();
    zms_media_tuple tuple;
    zms_media_tuple_from_source(src, &tuple);
    ztk_info("[event] publish app=%s stream=%s origin=%s video=%s %dx%d audio=%s %dHz", tuple.app,
             tuple.stream, zms_media_origin_str(origin),
             src->has_video ? zms_codec_name(src->video.codec) : "-",
             src->has_video ? (int)src->video.width : 0,
             src->has_video ? (int)src->video.height : 0,
             src->has_audio ? zms_codec_name(src->audio.codec) : "-",
             src->has_audio ? (int)src->audio.sample_rate : 0);
    if (g_ctx.has_server_ports) {
        zms_media_urls_log_publish(src, &g_ctx.server_ports);
    }
    if (g_ctx.events.on_media_publish) {
        g_ctx.events.on_media_publish(&tuple, origin, g_ctx.events.user);
    }
    zms_webhook_on_stream_register(src, origin);
}

void zms_media_event_publish_fini(zms_media_source *src, zms_media_origin origin)
{
    if (!src) {
        return;
    }
    zms_mp4_recorder_stop(src);
    zms_media_source_registry_lock();
    if (!src->publishing) {
        zms_media_source_registry_unlock();
        return;
    }
    src->publishing = 0;
    zms_media_source_registry_unlock();
    zms_media_tuple tuple;
    zms_media_tuple_from_source(src, &tuple);
    ztk_debug("[event] publish_fini app=%s stream=%s origin=%s", tuple.app, tuple.stream,
              zms_media_origin_str(origin));
    if (g_ctx.events.on_media_publish_fini) {
        g_ctx.events.on_media_publish_fini(&tuple, origin, g_ctx.events.user);
    }
    zms_webhook_on_stream_unregister(src, origin);
}

void zms_media_event_play(zms_media_source *src, const char *player_schema)
{
    if (!src) {
        return;
    }
    zms_media_tuple tuple;
    zms_media_tuple_from_source(src, &tuple);
    ztk_debug("[event] play app=%s stream=%s player=%s", tuple.app, tuple.stream,
              player_schema ? player_schema : "?");
    if (g_ctx.events.on_media_play) {
        g_ctx.events.on_media_play(&tuple, player_schema, g_ctx.events.user);
    }
}

void zms_media_event_stop(zms_media_source *src, const char *player_schema, uint64_t play_start_ms)
{
    uint64_t duration_ms;

    if (!src) {
        return;
    }
    zms_media_tuple tuple;
    zms_media_tuple_from_source(src, &tuple);
    ztk_debug("[event] stop app=%s stream=%s player=%s", tuple.app, tuple.stream,
              player_schema ? player_schema : "?");
    if (g_ctx.events.on_media_stop) {
        g_ctx.events.on_media_stop(&tuple, player_schema, g_ctx.events.user);
    }
    duration_ms = (play_start_ms > 0) ? (ztk_monotonic_ms() - play_start_ms) : 0;
    zms_webhook_on_play_stop(&tuple, player_schema, duration_ms);
}

void zms_media_source_reader_add(zms_media_source *src)
{
    if (!src) {
        return;
    }
    zms_media_source_registry_lock();
    ++src->reader_count;
    zms_media_source_registry_unlock();
    notify_reader_changed(src);
}

void zms_media_source_reader_remove(zms_media_source *src)
{
    if (!src) {
        return;
    }
    zms_media_source_registry_lock();
    if (src->reader_count <= 0) {
        zms_media_source_registry_unlock();
        return;
    }
    --src->reader_count;
    zms_media_source_registry_unlock();
    notify_reader_changed(src);
}

int zms_media_source_reader_count(const zms_media_source *src)
{
    if (!src) {
        return 0;
    }
    zms_media_source_registry_lock();
    int n = src->reader_count;
    zms_media_source_registry_unlock();
    return n;
}

void zms_media_events_fini(void)
{
    zms_media_none_reader_ctx *pending;
    zms_media_none_reader_ctx *next;

    g_media_events_fini = 1;
    g_ctx.poller = NULL;
    memset(&g_ctx.events, 0, sizeof(g_ctx.events));

    none_reader_pending_init();
    ztk_mutex_lock(g_none_reader_mtx);
    pending = g_none_reader_pending;
    g_none_reader_pending = NULL;
    ztk_mutex_unlock(g_none_reader_mtx);

    while (pending) {
        next = pending->next;
        if (pending->timer) {
            ztk_poller_timer_cancel(pending->timer);
        }
        free(pending);
        pending = next;
    }
}
