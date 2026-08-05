/**
 * @file stream_hub.c
 * @brief 流注册表、推流/播放读者挂接与生命周期。
 *
 * Copyright (c) zero-media-server
 */
#include "zms/engine/stream/stream_hub.h"
#include "zms/engine/media_event.h"
#include "zms/engine/stream/stream_stats.h"
#include "zms/engine/media/media_limits.h"
#include "zms/engine/stream/stream_url.h"
#include "zms/media/codec/codec_id.h"
#include "zms/media/container/flv/flv_tag_probe.h"
#include "zms/vod/io/vod_buffer.h"
#include "zms/vod/io/vod_source.h"
#include "ztk/thread/sync.h"
#include "ztk/util/log.h"
#include "ztk/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct zms_source_node {
    zms_media_source src;
    struct zms_source_node *next;
} zms_source_node;

static zms_source_node *g_head;
static int g_stream_hub_initialized;
static ztk_mutex *g_registry_mtx;

static int source_active(const zms_media_source *s)
{
    return s && (s->gop_queue || s->vod_buffer);
}

static zms_media_source *source_node_alloc(void)
{
    zms_source_node *n = (zms_source_node *)calloc(1, sizeof(*n));

    if (!n) {
        return NULL;
    }
    n->next = g_head;
    g_head = n;
    return &n->src;
}

static void source_node_free(zms_media_source *s)
{
    zms_source_node **pp = &g_head;

    if (!s) {
        return;
    }
    /* 丢弃不透明分片 sidecar 袋（录制器应已销毁）。 */
    if (s->segment_sidecar) {
        free(s->segment_sidecar);
        s->segment_sidecar = NULL;
    }
    while (*pp) {
        if (&(*pp)->src == s) {
            zms_source_node *dead = *pp;
            *pp = dead->next;
            free(dead);
            return;
        }
        pp = &(*pp)->next;
    }
}

static int registry_count_active(void)
{
    int n = 0;

    for (zms_source_node *node = g_head; node; node = node->next) {
        if (source_active(&node->src)) {
            ++n;
        }
    }
    return n;
}

static int registry_count_live(void)
{
    int n = 0;

    for (zms_source_node *node = g_head; node; node = node->next) {
        if (node->src.gop_queue) {
            ++n;
        }
    }
    return n;
}

void zms_media_source_registry_lock(void)
{
    zms_media_source_registry_init();
    ztk_mutex_lock(g_registry_mtx);
}

void zms_media_source_registry_unlock(void)
{
    if (g_registry_mtx) {
        ztk_mutex_unlock(g_registry_mtx);
    }
}

void zms_media_source_registry_init(void)
{
    if (g_stream_hub_initialized) {
        return;
    }
    g_head = NULL;
    g_registry_mtx = ztk_mutex_create(ZTK_MUTEX_NORMAL);
    g_stream_hub_initialized = 1;
}

static void strip_query(char *s)
{
    if (!s) {
        return;
    }
    char *q = strchr(s, '?');
    if (q) {
        *q = '\0';
    }
    char *h = strchr(s, '#');
    if (h) {
        *h = '\0';
    }
}

void zms_media_split_path(const char *path_or_url, char *app, char *stream)
{
    if (app) {
        app[0] = '\0';
    }
    if (stream) {
        stream[0] = '\0';
    }
    if (!path_or_url || !path_or_url[0] || !app || !stream) {
        return;
    }

    const char *seg = path_or_url;
    const char *scheme = strstr(path_or_url, "://");
    if (scheme) {
        seg = scheme + 3;
        const char *at = strchr(seg, '@');
        if (at) {
            seg = at + 1;
        }
        seg = strchr(seg, '/');
        if (!seg || !seg[1]) {
            return;
        }
        seg++;
    } else if (seg[0] == '/') {
        seg++;
    }

    const char *slash = strchr(seg, '/');
    if (!slash) {
        strncpy(app, seg, ZMS_APP_MAX - 1);
        app[ZMS_APP_MAX - 1] = '\0';
        strip_query(app);
        return;
    }
    size_t alen = (size_t)(slash - seg);
    if (alen >= ZMS_APP_MAX) {
        alen = ZMS_APP_MAX - 1;
    }
    memcpy(app, seg, alen);
    app[alen] = '\0';
    strncpy(stream, slash + 1, ZMS_STREAM_MAX - 1);
    stream[ZMS_STREAM_MAX - 1] = '\0';
    strip_query(stream);
}

static int key_match(const zms_media_source *s, const char *schema, const char *app,
                     const char *stream)
{
    return source_active(s) && strcmp(s->schema, schema) == 0 && strcmp(s->app, app) == 0 &&
           strcmp(s->stream, stream) == 0;
}

int zms_media_source_count(void)
{
    int n;

    zms_media_source_registry_lock();
    n = registry_count_active();
    zms_media_source_registry_unlock();
    return n;
}

void zms_media_source_log_registry(const zms_media_server_ports *ports)
{
    zms_media_source_registry_lock();
    int n = registry_count_active();
    if (!ports) {
        ztk_debug("[source] registry_dump active=%d", n);
        zms_media_source_registry_unlock();
        return;
    }
    ztk_debug("[source] registry_dump active=%d rtmp=%u rtsp=%u http=%u", n, ports->rtmp,
              ports->rtsp, ports->http);
    if (n == 0) {
        ztk_debug("[source] registry_empty hint=push_rtmp_or_rtsp_record_before_play");
        ztk_debug("[source] registry_empty hint=check_duplicate_demo_or_log_file");
        zms_media_source_registry_unlock();
        return;
    }
    for (zms_source_node *node = g_head; node; node = node->next) {
        if (source_active(&node->src)) {
            zms_media_urls_log_stream(&node->src, ports);
        }
    }
    zms_media_source_registry_unlock();
}

zms_media_source *zms_media_source_find(const char *schema, const char *app, const char *stream)
{
    if (!schema || !app || !stream) {
        return NULL;
    }
    zms_media_source_registry_lock();
    zms_media_source *found = NULL;
    for (zms_source_node *node = g_head; node; node = node->next) {
        if (key_match(&node->src, schema, app, stream)) {
            found = &node->src;
            break;
        }
    }
    zms_media_source_registry_unlock();
    return found;
}

static zms_media_source *find_by_stream_requested(const char *schema, const char *app,
                                                  const char *name)
{
    if (!schema || !app || !name || !name[0]) {
        return NULL;
    }
    zms_media_source_registry_lock();
    zms_media_source *found = NULL;
    for (zms_source_node *node = g_head; node; node = node->next) {
        const zms_media_source *s = &node->src;
        if (!s->gop_queue || strcmp(s->schema, schema) != 0 || strcmp(s->app, app) != 0) {
            continue;
        }
        if (strcmp(s->stream_requested, name) == 0 || strcmp(s->stream, name) == 0) {
            found = &node->src;
            break;
        }
    }
    zms_media_source_registry_unlock();
    return found;
}

static int stream_ends_with_id(const char *stream, const char *id)
{
    if (!stream || !id || !id[0]) {
        return 0;
    }
    size_t slen = strlen(stream);
    size_t id_len = strlen(id);
    if (slen < id_len) {
        return 0;
    }
    if (strcmp(stream + slen - id_len, id) != 0) {
        return 0;
    }
    return slen == id_len || stream[slen - id_len - 1] == '/';
}

/** demo/VLC live/stream；仅一条流时解析为 live 流 */
static zms_media_source *find_single_live_stream_alias(const char *app, const char *stream)
{
    if (!app || strcmp(app, "live") != 0 || !stream || strcmp(stream, "stream") != 0) {
        return NULL;
    }
    zms_media_source_registry_lock();
    zms_media_source *found = NULL;
    int n = 0;
    for (zms_source_node *node = g_head; node; node = node->next) {
        if (!node->src.gop_queue || strcmp(node->src.app, app) != 0) {
            continue;
        }
        if (!found) {
            found = &node->src;
        }
        ++n;
        if (n > 1) {
            found = NULL;
            break;
        }
    }
    zms_media_source_registry_unlock();
    return (n == 1) ? found : NULL;
}

/** HLS 相对分片误解析为 /live/{prefix}/{N}.ts 时，匹配唯一子流 prefix/<id> proxied/... */
static zms_media_source *find_single_live_stream_prefix(const char *app, const char *prefix)
{
    size_t plen;
    zms_media_source *found = NULL;
    int n = 0;

    if (!app || strcmp(app, "live") != 0 || !prefix || !prefix[0]) {
        return NULL;
    }
    plen = strlen(prefix);
    zms_media_source_registry_lock();
    for (zms_source_node *node = g_head; node; node = node->next) {
        const zms_media_source *s = &node->src;
        if (!s->gop_queue || strcmp(s->app, app) != 0) {
            continue;
        }
        if (strncmp(s->stream, prefix, plen) != 0) {
            continue;
        }
        if (s->stream[plen] != '\0' && s->stream[plen] != '/') {
            continue;
        }
        if (s->stream[plen] == '\0') {
            continue;
        }
        if (!found) {
            found = &node->src;
        }
        ++n;
        if (n > 1) {
            found = NULL;
            break;
        }
    }
    zms_media_source_registry_unlock();
    return (n == 1) ? found : NULL;
}

static zms_media_source *find_by_stream_id_suffix(const char *app, const char *id)
{
    if (!app || !id || !id[0] || strchr(id, '/')) {
        return NULL;
    }
    zms_media_source_registry_lock();
    zms_media_source *found = NULL;
    int matches = 0;
    for (zms_source_node *node = g_head; node; node = node->next) {
        const zms_media_source *s = &node->src;
        if (!s->gop_queue || strcmp(s->app, app) != 0) {
            continue;
        }
        if (!stream_ends_with_id(s->stream, id)) {
            continue;
        }
        found = &node->src;
        ++matches;
        if (matches > 1) {
            found = NULL;
            break;
        }
    }
    zms_media_source_registry_unlock();
    return found;
}

zms_media_source *zms_media_source_find_api(const char *schema, const char *app, const char *stream)
{
    if (!app || !stream) {
        return NULL;
    }
    if (schema && schema[0]) {
        zms_media_source *s = zms_media_source_find(schema, app, stream);
        if (s) {
            return s;
        }
        s = find_by_stream_requested(schema, app, stream);
        if (s) {
            return s;
        }
        if (strcmp(schema, ZMS_SCHEMA_RTSP) == 0 || strcmp(schema, ZMS_SCHEMA_SRT) == 0) {
            s = zms_media_source_find(ZMS_SCHEMA_RTP_PS, app, stream);
            if (s) {
                return s;
            }
            s = find_by_stream_requested(ZMS_SCHEMA_RTP_PS, app, stream);
            if (s) {
                return s;
            }
            s = zms_media_source_find(ZMS_SCHEMA_RTMP, app, stream);
            if (s) {
                return s;
            }
            return find_by_stream_requested(ZMS_SCHEMA_RTMP, app, stream);
        }
        if (strcmp(schema, ZMS_SCHEMA_RTMP) == 0) {
            s = zms_media_source_find(ZMS_SCHEMA_SRT, app, stream);
            if (s) {
                return s;
            }
            s = find_by_stream_requested(ZMS_SCHEMA_SRT, app, stream);
            if (s) {
                return s;
            }
            s = zms_media_source_find(ZMS_SCHEMA_RTSP, app, stream);
            if (s) {
                return s;
            }
            s = find_by_stream_requested(ZMS_SCHEMA_RTSP, app, stream);
            if (s) {
                return s;
            }
            s = zms_media_source_find(ZMS_SCHEMA_RTP_PS, app, stream);
            if (s) {
                return s;
            }
            return find_by_stream_requested(ZMS_SCHEMA_RTP_PS, app, stream);
        }
    }
    zms_media_source *s = zms_media_source_find(ZMS_SCHEMA_RTMP, app, stream);
    if (s) {
        return s;
    }
    s = find_by_stream_requested(ZMS_SCHEMA_RTMP, app, stream);
    if (s) {
        return s;
    }
    s = zms_media_source_find(ZMS_SCHEMA_RTP_PS, app, stream);
    if (s) {
        return s;
    }
    return find_by_stream_requested(ZMS_SCHEMA_RTP_PS, app, stream);
}

static int filter_match(const zms_media_source *s, const zms_media_source_filter *f)
{
    if (!source_active(s)) {
        return 0;
    }
    if (!f) {
        return 1;
    }
    if (f->schema && f->schema[0]) {
        if (strcmp(f->schema, s->schema) != 0 && strcmp(f->schema, ZMS_SCHEMA_RTSP) != 0 &&
            strcmp(f->schema, ZMS_SCHEMA_SRT) != 0) {
            return 0;
        }
    }
    if (f->app && f->app[0] && strcmp(f->app, s->app) != 0) {
        return 0;
    }
    if (f->stream && f->stream[0] && strcmp(f->stream, s->stream) != 0) {
        return 0;
    }
    return 1;
}

void zms_media_source_foreach(zms_media_source_visit_fn fn, void *user,
                              const zms_media_source_filter *filter)
{
    if (!fn) {
        return;
    }
    zms_media_source_registry_lock();
    for (zms_source_node *node = g_head; node; node = node->next) {
        if (!filter_match(&node->src, filter)) {
            continue;
        }
        if (fn(&node->src, user) != 0) {
            break;
        }
    }
    zms_media_source_registry_unlock();
}

void zms_media_source_set_publisher(zms_media_source *s, void *ctx,
                                    void (*kick)(void *ctx, int force))
{
    if (!s) {
        return;
    }
    zms_media_source_registry_lock();
    if (s->publisher_kick && s->publisher_ctx && s->publisher_ctx != ctx) {
        s->publisher_kick(s->publisher_ctx, 1);
    }
    s->publisher_ctx = ctx;
    s->publisher_kick = kick;
    zms_media_source_registry_unlock();
}

void zms_media_source_clear_publisher(zms_media_source *s, void *ctx)
{
    if (!s) {
        return;
    }
    zms_media_source_registry_lock();
    if (s->publisher_ctx != ctx) {
        zms_media_source_registry_unlock();
        return;
    }
    s->publisher_ctx = NULL;
    s->publisher_kick = NULL;
    zms_media_source_registry_unlock();
}

int zms_media_source_close(zms_media_source *s, int force)
{
    if (!s) {
        return 0;
    }
    if (zms_media_source_is_vod(s)) {
        zms_vod_source_stop(s);
        return 1;
    }
    if (!s->gop_queue) {
        return 0;
    }
    zms_media_source_registry_lock();
    int origin = s->publish_origin;
    int was_pub = s->publishing;
    void *pub = s->publisher_ctx;
    void (*kick)(void *ctx, int force) = s->publisher_kick;
    s->publishing = 0;
    s->publisher_ctx = NULL;
    s->publisher_kick = NULL;
    zms_media_source_registry_unlock();

    if (kick) {
        kick(pub, force);
    }
    (void)pub;
    if (was_pub) {
        zms_media_event_publish_fini(s, (zms_media_origin)origin);
    }

    zms_media_source_clear(s);
    return 1;
}

zms_media_source *zms_media_source_register(const char *schema, const char *app, const char *stream)
{
    if (!schema || !app || !stream) {
        return NULL;
    }

    zms_media_source_registry_lock();

    zms_media_source *exist = NULL;
    for (zms_source_node *node = g_head; node; node = node->next) {
        if (key_match(&node->src, schema, app, stream)) {
            exist = &node->src;
            break;
        }
    }
    if (exist) {
        if (exist->vod_buffer) {
            zms_media_source_registry_unlock();
            zms_vod_source_stop(exist);
            return zms_media_source_register(schema, app, stream);
        }
        ztk_info("[source] publish_reregister app=%s stream=%s action=kick_prev", app, stream);
        int origin = exist->publish_origin;
        int was_pub = exist->publishing;
        void *pub = exist->publisher_ctx;
        void (*kick)(void *ctx, int force) = exist->publisher_kick;
        exist->publishing = 0;
        exist->publisher_ctx = NULL;
        exist->publisher_kick = NULL;
        zms_media_source_registry_unlock();
        if (kick) {
            kick(pub, 1);
        }
        if (was_pub) {
            zms_media_event_publish_fini(exist, (zms_media_origin)origin);
        }
        zms_media_source_clear(exist);
        return exist;
    }

    zms_media_source *slot = source_node_alloc();
    if (!slot) {
        zms_media_source_registry_unlock();
        ztk_error("media source register failed: out of memory for %s/%s", app, stream);
        return NULL;
    }
    strncpy(slot->schema, schema, sizeof(slot->schema) - 1);
    strncpy(slot->app, app, sizeof(slot->app) - 1);
    strncpy(slot->stream, stream, sizeof(slot->stream) - 1);
    slot->gop_queue = zms_gop_queue_create();
    if (!slot->gop_queue) {
        source_node_free(slot);
        zms_media_source_registry_unlock();
        ztk_error("media source register failed: out of memory for %s/%s", app, stream);
        return NULL;
    }
    slot->create_stamp_ms = ztk_monotonic_ms();
    {
        int total = registry_count_active();
        zms_media_source_registry_unlock();
        ztk_info("[source] source_register schema=%s app=%s stream=%s total=%d", schema, app,
                 stream, total);
        return slot;
    }
}

zms_media_source *zms_media_source_register_vod(const char *schema, const char *app,
                                                const char *stream)
{
    zms_media_source *slot;

    if (!schema || !app || !stream) {
        return NULL;
    }

    zms_media_source_registry_lock();

    for (zms_source_node *node = g_head; node; node = node->next) {
        if (key_match(&node->src, schema, app, stream)) {
            zms_media_source_registry_unlock();
            return &node->src;
        }
    }

    slot = source_node_alloc();
    if (!slot) {
        zms_media_source_registry_unlock();
        return NULL;
    }
    strncpy(slot->schema, schema, sizeof(slot->schema) - 1);
    strncpy(slot->app, app, sizeof(slot->app) - 1);
    strncpy(slot->stream, stream, sizeof(slot->stream) - 1);
    slot->vod_buffer = zms_vod_buffer_create(0);
    if (!slot->vod_buffer) {
        source_node_free(slot);
        zms_media_source_registry_unlock();
        return NULL;
    }
    slot->create_stamp_ms = ztk_monotonic_ms();
    slot->publish_origin = ZMS_ORIGIN_MP4_VOD;
    zms_media_source_registry_unlock();
    ztk_info("[source] vod_register app=%s stream=%s", app, stream);
    return slot;
}

void zms_media_source_unregister_vod(zms_media_source *s)
{
    if (!s || !s->vod_buffer) {
        return;
    }
    zms_media_source_registry_lock();
    ztk_info("[source] vod_unregister app=%s stream=%s", s->app, s->stream);
    zms_vod_buffer_destroy(s->vod_buffer);
    source_node_free(s);
    zms_media_source_registry_unlock();
}

zms_media_source *zms_media_source_register_publish(const char *schema, const char *app,
                                                    const char *stream_requested)
{
    zms_media_source *exist;
    zms_media_source *slot;
    int total;

    if (!schema || !app || !stream_requested || !stream_requested[0]) {
        return NULL;
    }

    zms_media_source_registry_lock();

    exist = NULL;
    for (zms_source_node *node = g_head; node; node = node->next) {
        if (key_match(&node->src, schema, app, stream_requested)) {
            exist = &node->src;
            break;
        }
    }
    if (exist) {
        if (exist->vod_buffer) {
            zms_media_source_registry_unlock();
            return NULL;
        }
        ztk_info("[source] publish_reregister app=%s stream=%s", app, stream_requested);
        {
            int origin = exist->publish_origin;
            int was_pub = exist->publishing;
            void *pub = exist->publisher_ctx;
            void (*kick)(void *ctx, int force) = exist->publisher_kick;

            exist->publishing = 0;
            exist->publisher_ctx = NULL;
            exist->publisher_kick = NULL;
            strncpy(exist->stream_requested, stream_requested, sizeof(exist->stream_requested) - 1);
            exist->stream_requested[sizeof(exist->stream_requested) - 1] = '\0';
            zms_media_source_registry_unlock();
            if (kick) {
                kick(pub, 1);
            }
            if (was_pub) {
                zms_media_event_publish_fini(exist, (zms_media_origin)origin);
            }
            zms_media_source_clear(exist);
            zms_media_source_registry_lock();
            total = registry_count_live();
            zms_media_source_registry_unlock();
            ztk_info("[source] publish_reuse app=%s stream=%s total=%d", app, stream_requested,
                     total);
            return exist;
        }
    }

    slot = source_node_alloc();
    if (!slot) {
        zms_media_source_registry_unlock();
        ztk_error("media source register_publish failed: out of memory app=%s stream=%s", app,
                  stream_requested);
        return NULL;
    }
    strncpy(slot->schema, schema, sizeof(slot->schema) - 1);
    strncpy(slot->app, app, sizeof(slot->app) - 1);
    strncpy(slot->stream, stream_requested, sizeof(slot->stream) - 1);
    strncpy(slot->stream_requested, stream_requested, sizeof(slot->stream_requested) - 1);
    slot->gop_queue = zms_gop_queue_create();
    if (!slot->gop_queue) {
        source_node_free(slot);
        zms_media_source_registry_unlock();
        ztk_error("media source register_publish failed: out of memory app=%s stream=%s", app,
                  stream_requested);
        return NULL;
    }

    slot->create_stamp_ms = ztk_monotonic_ms();
    total = registry_count_live();
    zms_media_source_registry_unlock();

    ztk_info("[source] publish_start app=%s stream=%s total=%d", app, stream_requested, total);
    return slot;
}

void zms_media_source_clear(zms_media_source *s)
{
    if (!s || !s->gop_queue) {
        return;
    }
    ztk_info("[source] source_clear app=%s stream=%s video=%d audio=%d", s->app, s->stream,
             s->has_video, s->has_audio);
    zms_gop_queue_clear(s->gop_queue);
    zms_media_stats_reset(s);
    s->has_video = 0;
    s->has_audio = 0;
    zms_video_track_clear(&s->video);
    zms_audio_track_clear(&s->audio);
}

void zms_media_source_unregister(zms_media_source *s)
{
    if (!s || !s->gop_queue) {
        return;
    }
    zms_media_source_registry_lock();
    ztk_info("[source] source_unregister app=%s stream=%s", s->app, s->stream);
    zms_gop_queue_destroy(s->gop_queue);
    source_node_free(s);
    zms_media_source_registry_unlock();
}

const uint8_t *zms_media_source_video_config(const zms_media_source *s, size_t *len)
{
    if (!s) {
        return NULL;
    }
    zms_media_source_registry_lock();
    const uint8_t *p = NULL;
    if (s->gop_queue) {
        p = zms_gop_queue_video_config(s->gop_queue, len);
    } else if (s->vod_buffer) {
        p = zms_vod_buffer_video_config(s->vod_buffer, len);
    }
    if (p && len && *len > 0) {
        zms_media_source_registry_unlock();
        return p;
    }
    if (len) {
        *len = 0;
    }
    zms_media_source_registry_unlock();
    return NULL;
}

const uint8_t *zms_media_source_audio_config(const zms_media_source *s, size_t *len)
{
    if (!s) {
        return NULL;
    }
    zms_media_source_registry_lock();
    const uint8_t *p = NULL;
    if (s->gop_queue) {
        p = zms_gop_queue_audio_config(s->gop_queue, len);
    } else if (s->vod_buffer) {
        p = zms_vod_buffer_audio_config(s->vod_buffer, len);
    }
    if (p && len && *len > 0) {
        zms_media_source_registry_unlock();
        return p;
    }
    if (len) {
        *len = 0;
    }
    zms_media_source_registry_unlock();
    return NULL;
}

static zms_codec_id play_video_codec(const zms_media_source *s)
{
    size_t len = 0;
    const uint8_t *cfg;

    if (!s) {
        return ZMS_CODEC_INVALID;
    }
    if (s->video.ready && s->video.codec != ZMS_CODEC_INVALID) {
        return s->video.codec;
    }
    if (s->gop_queue) {
        cfg = zms_gop_queue_video_config(s->gop_queue, &len);
        if (cfg && len > 0) {
            return zms_flv_tag_video_codec(cfg, len);
        }
    } else if (s->vod_buffer) {
        cfg = zms_vod_buffer_video_config(s->vod_buffer, &len);
        if (cfg && len > 0) {
            return zms_flv_tag_video_codec(cfg, len);
        }
    }
    return ZMS_CODEC_INVALID;
}

static zms_codec_id play_audio_codec(const zms_media_source *s)
{
    size_t len = 0;
    const uint8_t *cfg;

    if (!s) {
        return ZMS_CODEC_INVALID;
    }
    if (s->audio.ready && s->audio.codec != ZMS_CODEC_INVALID) {
        return s->audio.codec;
    }
    if (s->gop_queue) {
        cfg = zms_gop_queue_audio_config(s->gop_queue, &len);
        if (cfg && len > 0) {
            return zms_flv_tag_audio_codec(cfg, len);
        }
    } else if (s->vod_buffer) {
        cfg = zms_vod_buffer_audio_config(s->vod_buffer, &len);
        if (cfg && len > 0) {
            return zms_flv_tag_audio_codec(cfg, len);
        }
    }
    return ZMS_CODEC_INVALID;
}

static int codec_gop_queue_playable(zms_codec_id c, zms_track_type track)
{
    if (track == ZMS_TRACK_VIDEO) {
        return c == ZMS_CODEC_H264 || c == ZMS_CODEC_H265 || c == ZMS_CODEC_AV1 ||
               c == ZMS_CODEC_VP8 || c == ZMS_CODEC_VP9 || c == ZMS_CODEC_H266;
    }
    if (track == ZMS_TRACK_AUDIO) {
        return c == ZMS_CODEC_AAC || c == ZMS_CODEC_G711A || c == ZMS_CODEC_G711U ||
               c == ZMS_CODEC_OPUS;
    }
    return 0;
}

int zms_media_source_use_gop_queue_play(const zms_media_source *s)
{
    zms_codec_id vc, ac;

    if (!s || !s->gop_queue) {
        return 0;
    }
    if (!s->has_video) {
        ac = play_audio_codec(s);
        if (ac != ZMS_CODEC_INVALID) {
            return codec_gop_queue_playable(ac, ZMS_TRACK_AUDIO);
        }
        return 0;
    }
    vc = play_video_codec(s);
    if (!codec_gop_queue_playable(vc, ZMS_TRACK_VIDEO)) {
        return 0;
    }
    if (s->has_audio) {
        ac = play_audio_codec(s);
        if (ac != ZMS_CODEC_INVALID && !codec_gop_queue_playable(ac, ZMS_TRACK_AUDIO)) {
            return 0;
        }
    }
    return 1;
}

void zms_media_subscriber_detach(zms_media_subscriber *r)
{
    if (!r) {
        return;
    }
    if (r->gop) {
        zms_gop_reader_detach(r->gop);
        r->gop = NULL;
    }
    if (r->vod) {
        zms_vod_buffer_reader_detach(r->vod);
        r->vod = NULL;
    }
}

ztk_err_t zms_media_source_subscribe(zms_media_source *s, int seek_live, zms_media_subscriber *out)
{
    if (!s || !out) {
        return ZTK_ERR_INVALID;
    }
    zms_media_subscriber_detach(out);
    if (!zms_media_source_use_gop_queue_play(s) || !s->gop_queue) {
        return ZTK_ERR_STATE;
    }
    if (seek_live) {
        out->gop = zms_gop_reader_attach(s->gop_queue);
    } else {
        out->gop = zms_gop_reader_attach_beginning(s->gop_queue);
    }
    if (!out->gop) {
        return ZTK_ERR_NOMEM;
    }
    if (seek_live) {
        zms_gop_reader_seek_live(out->gop);
    }
    return ZTK_OK;
}

ztk_err_t zms_media_source_subscribe_gop(zms_media_source *s, zms_media_subscriber *out)
{
    if (!s || !out) {
        return ZTK_ERR_INVALID;
    }
    zms_media_subscriber_detach(out);
    if (!zms_media_source_use_gop_queue_play(s) || !s->gop_queue) {
        return ZTK_ERR_STATE;
    }
    out->gop = zms_gop_reader_attach(s->gop_queue);
    if (!out->gop) {
        return ZTK_ERR_NOMEM;
    }
    zms_gop_reader_seek_gop_key(out->gop);
    return ZTK_OK;
}

ztk_err_t zms_media_source_subscribe_vod(zms_media_source *s, zms_media_subscriber *out)
{
    if (!s || !out) {
        return ZTK_ERR_INVALID;
    }
    zms_media_subscriber_detach(out);
    if (!zms_media_source_is_vod(s) || !s->vod_buffer) {
        return ZTK_ERR_STATE;
    }
    out->vod = zms_vod_buffer_reader_attach(s->vod_buffer, 1);
    if (!out->vod) {
        return ZTK_ERR_NOMEM;
    }
    zms_vod_buffer_reader_seek_video_key(out->vod);
    return ZTK_OK;
}

zms_media_source *zms_media_source_find_for_play(const char *schema, const char *app,
                                                 const char *stream)
{
    zms_media_source *s = zms_media_source_find_api(schema, app, stream);
    if (s) {
        return s;
    }
    if (!app || !stream || !stream[0]) {
        return s;
    }
    s = zms_vod_source_find_or_open(schema, app, stream, NULL);
    if (s) {
        return s;
    }
    if (strcmp(app, "live") != 0) {
        return NULL;
    }

    char alt[ZMS_STREAM_MAX];
    /* RTSP 推流曾用 stream/<id>，VLC 常只 play <id> */
    if (!strchr(stream, '/')) {
        int n = snprintf(alt, sizeof(alt), "stream/%s", stream);
        if (n > 0 && (size_t)n < sizeof(alt)) {
            s = zms_media_source_find_api(schema, app, alt);
            if (s) {
                return s;
            }
        }
        s = find_by_stream_id_suffix(app, stream);
        if (s) {
            return s;
        }
    } else {
        const char *tail = strrchr(stream, '/');
        if (tail && tail[1]) {
            s = find_by_stream_id_suffix(app, tail + 1);
            if (s) {
                return s;
            }
        }
        if (strncmp(stream, "stream/", 7) == 0) {
            s = find_single_live_stream_alias(app, "stream");
        }
        if (s) {
            return s;
        }
        s = find_single_live_stream_prefix(app, stream);
        if (s) {
            return s;
        }
    }
    return find_single_live_stream_alias(app, stream);
}
