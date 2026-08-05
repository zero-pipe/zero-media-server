#include "zms/vod/io/vod_source.h"
#include "zms/vod/io/vod_format.h"
#include "zms/vod/io/vod_reader.h"
#include "zms/vod/vod_flv_index.h"
#include "zms/engine/media_event.h"
#include "ztk/util/log.h"
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define ZMS_VOD_ACCESS _access
#else
#include <dirent.h>
#include <unistd.h>
#define ZMS_VOD_ACCESS access
#endif

#define ZMS_VOD_DIR_PATH_MAX (ZMS_CFG_PATH_MAX + ZMS_APP_MAX + ZMS_STREAM_MAX + 4)
#define ZMS_VOD_M3U8_REL_MAX (ZMS_STREAM_MAX * 2 + 4)

static zms_vod_config g_vod_cfg;

void zms_vod_config_default(zms_vod_config *cfg)
{
    if (!cfg) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    strncpy(cfg->record_app, "vod", sizeof(cfg->record_app) - 1);
    strncpy(cfg->record_root, "./www", sizeof(cfg->record_root) - 1);
    cfg->file_repeat = 0;
}

void zms_vod_config_apply(zms_config *cfg)
{
    if (!cfg) {
        return;
    }
    zms_vod_config_default(&g_vod_cfg);
    if (cfg->record.app[0]) {
        strncpy(g_vod_cfg.record_app, cfg->record.app, sizeof(g_vod_cfg.record_app) - 1);
    }
    /*
     * 点播扫盘根目录保持 ./www（兼容 ./www/vod/*.mp4）。
     * [record] root=./www/record 仅给直播录像器使用，勿覆盖 VOD root。
     */
    if (cfg->record.root[0]) {
        const char *r = cfg->record.root;
        if (!strstr(r, "/record") && !strstr(r, "\\record")) {
            strncpy(g_vod_cfg.record_root, r, sizeof(g_vod_cfg.record_root) - 1);
        }
    }
    g_vod_cfg.file_repeat = cfg->record.file_repeat;
}

const zms_vod_config *zms_vod_config_get(void)
{
    return &g_vod_cfg;
}

static int file_exists(const char *path)
{
#ifdef _WIN32
    return _access(path, 0) == 0;
#else
    return access(path, F_OK) == 0;
#endif
}

static void vod_strip_media_suffix(char *stream)
{
    char *dot;

    if (!stream) {
        return;
    }
    if (!zms_vod_format_stream_has_supported_ext(stream)) {
        return;
    }
    dot = strrchr(stream, '.');
    if (dot && dot != stream) {
        *dot = '\0';
    }
}

/* librtmp/ffplay 点播类型前缀：flv:sample、mp3:sample、mp4:sample.m4v */
static void vod_strip_rtmp_type_prefix(char *stream)
{
    static const char *types[] = {"mp4", "flv", "mp3", "m4v", "m4a", NULL};
    char *colon;
    const char **t;
    size_t prefix_len;

    if (!stream || !stream[0]) {
        return;
    }
    colon = strchr(stream, ':');
    if (!colon || colon == stream) {
        return;
    }
    prefix_len = (size_t)(colon - stream);
    for (t = types; *t; t++) {
        if (prefix_len == strlen(*t) && strncmp(stream, *t, prefix_len) == 0) {
            memmove(stream, colon + 1, strlen(colon + 1) + 1);
            return;
        }
    }
}

int zms_vod_stream_is_vod_flv_wrap_suffix(const char *stream)
{
    return zms_vod_format_stream_is_flv_wrap_suffix(stream);
}

int zms_vod_stream_is_native_flv_file(const char *stream)
{
    return stream && !zms_vod_stream_is_vod_flv_wrap_suffix(stream) &&
           zms_vod_format_stream_is_native_file(stream);
}

static void vod_strip_vod_flv_wrap_suffix(char *stream)
{
    size_t n;

    if (!stream || !zms_vod_stream_is_vod_flv_wrap_suffix(stream)) {
        return;
    }
    n = strlen(stream);
    stream[n - 4] = '\0';
}

static int vod_has_media_ext(const char *stream)
{
    return zms_vod_format_stream_has_supported_ext(stream);
}

int zms_vod_stream_has_disk_container_ext(const char *stream)
{
    return vod_has_media_ext(stream) && !zms_vod_format_stream_is_native_file(stream);
}

void zms_vod_canonical_stream(const char *stream_in, char *stream_out, size_t out_cap)
{
    size_t n;

    if (!stream_out || out_cap == 0) {
        return;
    }
    stream_out[0] = '\0';
    if (!stream_in || !stream_in[0]) {
        return;
    }
    strncpy(stream_out, stream_in, out_cap - 1);
    stream_out[out_cap - 1] = '\0';
    vod_strip_rtmp_type_prefix(stream_out);
    if (zms_vod_stream_is_vod_flv_wrap_suffix(stream_out)) {
        vod_strip_vod_flv_wrap_suffix(stream_out);
        return;
    }
    if (vod_has_media_ext(stream_out)) {
        return;
    }
    {
        const char *def_ext = zms_vod_format_default_extension();
        n = strlen(stream_out);
        if (!def_ext || n + strlen(def_ext) >= out_cap) {
            return;
        }
        strcat(stream_out, def_ext);
    }
}

int zms_vod_rel_path_safe(const char *rel)
{
    if (!rel || !rel[0]) {
        return 0;
    }
    if (rel[0] == '/' || rel[0] == '\\') {
        return 0;
    }
    if (strstr(rel, "..") != NULL) {
        return 0;
    }
    return 1;
}

int zms_vod_resolve_rel_path(const char *app, const char *rel, char *out, size_t out_cap)
{
    const zms_vod_config *cfg = zms_vod_config_get();
    size_t n;

    if (!app || !rel || !out || out_cap == 0 || !zms_vod_rel_path_safe(rel)) {
        return 0;
    }
    n = (size_t)snprintf(out, out_cap, "%s/%s/%s", cfg->record_root, app, rel);
    return n > 0 && n < out_cap;
}

int zms_vod_m3u8_rel_to_mp4_stream(const char *m3u8_rel, char *stream_out, size_t out_cap)
{
    size_t n;

    if (!m3u8_rel || !stream_out || out_cap == 0 || !zms_vod_rel_path_safe(m3u8_rel)) {
        return 0;
    }
    n = strlen(m3u8_rel);
    if (n < 6) {
        return 0;
    }
    if (strcmp(m3u8_rel + n - 5, ".m3u8") != 0) {
        return 0;
    }
    if (n - 5 >= out_cap) {
        return 0;
    }
    memcpy(stream_out, m3u8_rel, n - 5);
    stream_out[n - 5] = '\0';
    if (strlen(stream_out) + 4 >= out_cap) {
        return 0;
    }
    strcat(stream_out, ".mp4");
    return stream_out[0] != '\0';
}

int zms_vod_mp4_stream_to_m3u8_rel(const char *mp4_stream, char *m3u8_rel, size_t rel_cap)
{
    size_t n;

    if (!mp4_stream || !m3u8_rel || rel_cap == 0) {
        return 0;
    }
    n = strlen(mp4_stream);
    if (n < 5 || strcmp(mp4_stream + n - 4, ".mp4") != 0) {
        return 0;
    }
    if (n - 4 + 5 >= rel_cap) {
        return 0;
    }
    memcpy(m3u8_rel, mp4_stream, n - 4);
    m3u8_rel[n - 4] = '\0';
    strcat(m3u8_rel, ".m3u8");
    return m3u8_rel[0] != '\0';
}

int zms_vod_infer_stream_from_ts(const char *app, const char *ts_rel, char *stream_out,
                                 size_t out_cap)
{
    const zms_vod_config *cfg = zms_vod_config_get();
    char dir_rel[ZMS_STREAM_MAX];
    char dir_path[ZMS_VOD_DIR_PATH_MAX];
    char m3u8_name[ZMS_STREAM_MAX];
    char m3u8_rel[ZMS_VOD_M3U8_REL_MAX];
    char *slash;
    int found = 0;

    if (!app || !ts_rel || !stream_out || out_cap == 0 || !zms_vod_rel_path_safe(ts_rel)) {
        return 0;
    }

    strncpy(dir_rel, ts_rel, sizeof(dir_rel) - 1);
    dir_rel[sizeof(dir_rel) - 1] = '\0';
    slash = strrchr(dir_rel, '/');
    if (slash) {
        *slash = '\0';
    }

    if (dir_rel[0]) {
        size_t n = (size_t)snprintf(dir_path, sizeof(dir_path), "%s/%s/%s", cfg->record_root, app,
                                    dir_rel);
        if (n == 0 || n >= sizeof(dir_path)) {
            return 0;
        }
    } else {
        size_t n = (size_t)snprintf(dir_path, sizeof(dir_path), "%s/%s", cfg->record_root, app);
        if (n == 0 || n >= sizeof(dir_path)) {
            return 0;
        }
    }

#if defined(_WIN32)
    {
        char pattern[ZMS_CFG_PATH_MAX + 8];
        struct _finddata_t fd;
        intptr_t h;

        snprintf(pattern, sizeof(pattern), "%s/*.m3u8", dir_path);
        h = _findfirst(pattern, &fd);
        if (h == -1) {
            return 0;
        }
        do {
            if (fd.name[0] == '.') {
                continue;
            }
            if (found) {
                break;
            }
            strncpy(m3u8_name, fd.name, sizeof(m3u8_name) - 1);
            m3u8_name[sizeof(m3u8_name) - 1] = '\0';
            found = 1;
        } while (_findnext(h, &fd) == 0);
        _findclose(h);
    }
#else
    {
        DIR *d;
        struct dirent *ent;

        d = opendir(dir_path);
        if (!d) {
            return 0;
        }
        while ((ent = readdir(d)) != NULL) {
            size_t nlen;
            if (ent->d_name[0] == '.') {
                continue;
            }
            nlen = strlen(ent->d_name);
            if (nlen < 6 || strcmp(ent->d_name + nlen - 5, ".m3u8") != 0) {
                continue;
            }
            if (found) {
                found = 0;
                break;
            }
            strncpy(m3u8_name, ent->d_name, sizeof(m3u8_name) - 1);
            m3u8_name[sizeof(m3u8_name) - 1] = '\0';
            found = 1;
        }
        closedir(d);
    }
#endif
    if (!found || !m3u8_name[0]) {
        return 0;
    }
    if (dir_rel[0]) {
        size_t n = (size_t)snprintf(m3u8_rel, sizeof(m3u8_rel), "%s/%s", dir_rel, m3u8_name);
        if (n == 0 || n >= sizeof(m3u8_rel)) {
            return 0;
        }
    } else {
        size_t n = (size_t)snprintf(m3u8_rel, sizeof(m3u8_rel), "%s", m3u8_name);
        if (n == 0 || n >= sizeof(m3u8_rel)) {
            return 0;
        }
    }
    return zms_vod_m3u8_rel_to_mp4_stream(m3u8_rel, stream_out, out_cap);
}

void zms_vod_stream_basename(const char *stream_in, char *base_out, size_t out_cap)
{
    char tmp[ZMS_STREAM_MAX];

    if (!base_out || out_cap == 0) {
        return;
    }
    base_out[0] = '\0';
    if (!stream_in) {
        return;
    }
    strncpy(tmp, stream_in, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    vod_strip_media_suffix(tmp);
    strncpy(base_out, tmp, out_cap - 1);
    base_out[out_cap - 1] = '\0';
}

typedef struct zms_vod_resolve_ctx {
    const char *app;
    const char *canon;
    char *out;
    size_t out_cap;
    int found;
} zms_vod_resolve_ctx;

static int vod_resolve_candidate(const char *app, const char *rel, char *out, size_t out_cap)
{
    char path[ZMS_CFG_PATH_MAX];

    if (!rel || !rel[0] || !zms_vod_resolve_rel_path(app, rel, path, sizeof(path))) {
        return 0;
    }
    if (!file_exists(path)) {
        return 0;
    }
    snprintf(out, out_cap, "%s", path);
    return 1;
}

static int vod_resolve_registered_ext_cb(const zms_vod_format_desc *fmt, const char *ext,
                                         void *user)
{
    zms_vod_resolve_ctx *ctx = (zms_vod_resolve_ctx *)user;
    char base[ZMS_STREAM_MAX];

    (void)fmt;
    if (!ctx || !ext || !ext[0] || vod_has_media_ext(ctx->canon)) {
        return 0;
    }
    strncpy(base, ctx->canon, sizeof(base) - 1);
    base[sizeof(base) - 1] = '\0';
    if (strlen(base) + strlen(ext) >= sizeof(base)) {
        return 0;
    }
    strcat(base, ext);
    if (vod_resolve_candidate(ctx->app, base, ctx->out, ctx->out_cap)) {
        ctx->found = 1;
        return 1;
    }
    return 0;
}

int zms_vod_is_record_app(const char *app)
{
    const zms_vod_config *cfg = zms_vod_config_get();
    return app && cfg && cfg->record_app[0] && strcmp(app, cfg->record_app) == 0;
}

int zms_vod_resolve_file_path(const char *app, const char *stream, char *out, size_t out_cap)
{
    char canon[ZMS_STREAM_MAX];
    zms_vod_resolve_ctx ctx;

    if (!app || !stream || !out || out_cap == 0) {
        return 0;
    }

    strncpy(canon, stream, sizeof(canon) - 1);
    canon[sizeof(canon) - 1] = '\0';
    vod_strip_rtmp_type_prefix(canon);

    if (zms_vod_stream_is_vod_flv_wrap_suffix(canon)) {
        char disk[ZMS_STREAM_MAX];

        strncpy(disk, canon, sizeof(disk) - 1);
        disk[sizeof(disk) - 1] = '\0';
        vod_strip_vod_flv_wrap_suffix(disk);
        if (vod_resolve_candidate(app, disk, out, out_cap)) {
            return 1;
        }
        return 0;
    }

    if (zms_vod_stream_is_native_flv_file(canon)) {
        return vod_resolve_candidate(app, canon, out, out_cap);
    }

    if (vod_resolve_candidate(app, canon, out, out_cap)) {
        return 1;
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.app = app;
    ctx.canon = canon;
    ctx.out = out;
    ctx.out_cap = out_cap;
    zms_vod_format_foreach_extension(vod_resolve_registered_ext_cb, &ctx);
    if (ctx.found) {
        return 1;
    }

    zms_vod_canonical_stream(stream, canon, sizeof(canon));
    return vod_resolve_candidate(app, canon, out, out_cap);
}

int zms_media_source_is_vod(const zms_media_source *s)
{
    return s && s->vod_buffer && s->publish_origin == ZMS_ORIGIN_MP4_VOD;
}

zms_media_source *zms_vod_source_open(const char *app, const char *stream, const char *file_path,
                                      ztk_poller *poller)
{
    zms_vod_reader_opts opts;
    zms_vod_reader *reader;
    zms_media_source *src;
    char resolved[ZMS_CFG_PATH_MAX];
    char norm_stream[ZMS_STREAM_MAX];
    const char *path = file_path;

    if (!app || !stream || !poller) {
        return NULL;
    }

    zms_vod_canonical_stream(stream, norm_stream, sizeof(norm_stream));
    if (!norm_stream[0]) {
        return NULL;
    }

    if (!path || !path[0]) {
        if (!zms_vod_resolve_file_path(app, norm_stream, resolved, sizeof(resolved))) {
            ztk_warn("vod: file not found app=%s stream=%s root=%s", app, norm_stream,
                     g_vod_cfg.record_root);
            return NULL;
        }
        path = resolved;
    }

    src = zms_media_source_register_vod(ZMS_SCHEMA_RTMP, app, norm_stream);
    if (!src) {
        return NULL;
    }

    memset(&opts, 0, sizeof(opts));
    opts.file_path = path;
    opts.app = app;
    opts.stream = norm_stream;
    opts.speed = 1.0;
    opts.loop = g_vod_cfg.file_repeat;
    opts.fifo = src->vod_buffer;
    opts.source = src;

    reader = zms_vod_reader_open(&opts);
    if (!reader) {
        zms_media_source_unregister_vod(src);
        return NULL;
    }

    src->publisher_ctx = reader;
    zms_media_source_set_publisher(src, reader, NULL);
    zms_media_event_publish(src, ZMS_ORIGIN_MP4_VOD);
    zms_vod_reader_bind_poller(reader, poller);
    ztk_info("vod: started %s -> %s/%s", path, app, norm_stream);
    return src;
}

void zms_vod_source_stop(zms_media_source *src)
{
    zms_vod_reader *reader;

    if (!src || !zms_media_source_is_vod(src)) {
        return;
    }
    reader = (zms_vod_reader *)src->publisher_ctx;
    if (reader) {
        zms_media_event_publish_fini(src, ZMS_ORIGIN_MP4_VOD);
        src->publisher_ctx = NULL;
        zms_vod_reader_close(reader);
    }
    zms_media_source_unregister_vod(src);
}

zms_media_source *zms_vod_source_find_or_open(const char *schema, const char *app,
                                              const char *stream, ztk_poller *poller)
{
    zms_media_source *s;
    const zms_vod_config *cfg;
    char canon[ZMS_STREAM_MAX];

    if (!app || !stream || !stream[0]) {
        return NULL;
    }

    s = zms_media_source_find_api(schema, app, stream);
    if (s) {
        return s;
    }

    zms_vod_canonical_stream(stream, canon, sizeof(canon));
    if (canon[0] && strcmp(canon, stream) != 0) {
        s = zms_media_source_find_api(schema, app, canon);
        if (s) {
            return s;
        }
    }

    cfg = zms_vod_config_get();
    if (strcmp(app, cfg->record_app) != 0) {
        return NULL;
    }
    if (!poller) {
        poller = zms_media_events_poller();
    }
    if (!poller) {
        return NULL;
    }
    return zms_vod_source_open(app, canon[0] ? canon : stream, NULL, poller);
}

void zms_vod_source_prepare_play(zms_media_source *src)
{
    if (!src || !src->publisher_ctx) {
        return;
    }
    zms_vod_reader_prepare_play((zms_vod_reader *)src->publisher_ctx);
}

uint64_t zms_vod_source_seek_ms(zms_media_source *src, uint64_t ms)
{
    if (!src || !src->publisher_ctx) {
        return 0;
    }
    return zms_vod_reader_seek_ms((zms_vod_reader *)src->publisher_ctx, ms);
}

uint64_t zms_vod_source_seek_for_reader(zms_media_source *src, zms_vod_buffer_reader *rd,
                                        uint64_t ms)
{
    uint64_t actual = ms;
    int readers;

    if (!src || !src->publisher_ctx) {
        return 0;
    }
    if (rd && zms_vod_buffer_reader_seek_ms(rd, ms, &actual)) {
        ztk_info("vod: fifo seek reader -> %llu ms", (unsigned long long)actual);
        return actual;
    }
    readers = zms_media_source_reader_count(src);
    if (readers > 1) {
        ztk_info("vod: demux seek %llu ms (%d readers share fifo)", (unsigned long long)ms,
                 readers);
    }
    actual = zms_vod_reader_seek_ms((zms_vod_reader *)src->publisher_ctx, ms);
    if (rd) {
        zms_vod_buffer_reader_seek_beginning(rd);
        zms_vod_buffer_reader_seek_video_key(rd);
    }
    return actual;
}

uint64_t zms_vod_source_duration_ms(const zms_media_source *src)
{
    if (!src || !src->publisher_ctx) {
        return 0;
    }
    return zms_vod_reader_duration_ms((const zms_vod_reader *)src->publisher_ctx);
}

int zms_vod_source_file_path(const zms_media_source *src, char *out, size_t out_cap)
{
    const char *path;

    if (!src || !out || out_cap == 0 || !src->publisher_ctx) {
        return 0;
    }
    path = zms_vod_reader_file_path((const zms_vod_reader *)src->publisher_ctx);
    if (!path || !path[0]) {
        return 0;
    }
    strncpy(out, path, out_cap - 1);
    out[out_cap - 1] = '\0';
    return 1;
}

uint64_t zms_vod_probe_duration_ms(const char *app, const char *stream)
{
    char path[ZMS_CFG_PATH_MAX];

    if (!zms_vod_resolve_file_path(app, stream, path, sizeof(path))) {
        return 0;
    }
    return zms_vod_format_probe_duration_ms(path);
}

const zms_vod_flv_index *zms_vod_source_flv_index(const zms_media_source *src)
{
    if (!src || !src->publisher_ctx) {
        return NULL;
    }
    return zms_vod_reader_flv_index((zms_vod_reader *)src->publisher_ctx);
}
