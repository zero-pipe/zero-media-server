#include "zms/vod/io/vod_format.h"
#include <string.h>

#if defined(_WIN32)
#define zms_vod_format_stricmp _stricmp
#else
#include <strings.h>
#define zms_vod_format_stricmp strcasecmp
#endif

#define ZMS_VOD_FORMAT_MAX 16

static const zms_vod_format_desc *g_formats[ZMS_VOD_FORMAT_MAX];
static int g_format_count;
static int g_vod_format_builtins_registered;

static const char *path_ext(const char *path)
{
    const char *dot;
    const char *slash;
    const char *backslash;

    if (!path) {
        return NULL;
    }
    dot = strrchr(path, '.');
    if (!dot || !dot[1]) {
        return NULL;
    }
    slash = strrchr(path, '/');
    backslash = strrchr(path, '\\');
    if ((slash && dot < slash) || (backslash && dot < backslash)) {
        return NULL;
    }
    return dot;
}

static int ext_eq(const char *a, const char *b)
{
    if (!a || !b) {
        return 0;
    }
    return zms_vod_format_stricmp(a, b) == 0;
}

void zms_vod_format_register(const zms_vod_format_desc *fmt)
{
    int i;

    if (!fmt || !fmt->name || !fmt->extensions) {
        return;
    }
    for (i = 0; i < g_format_count; ++i) {
        if (g_formats[i] && strcmp(g_formats[i]->name, fmt->name) == 0) {
            g_formats[i] = fmt;
            return;
        }
    }
    if (g_format_count < ZMS_VOD_FORMAT_MAX) {
        g_formats[g_format_count++] = fmt;
    }
}

static uint64_t probe_mov_family_duration(const char *path)
{
    return zms_container_mp4_file_duration_ms(path);
}

static uint64_t probe_flv_duration(const char *path)
{
    return zms_container_flv_file_duration_ms(path);
}

void zms_vod_format_register_all(void)
{
    static const char *const mov_exts[] = {".mp4", ".mov", ".m4v", NULL};
    static const char *const mkv_exts[] = {".mkv", ".mka", NULL};
    static const char *const flv_exts[] = {".flv", NULL};
    static const zms_vod_format_desc mov = {
        "mov-family", ZMS_CONTAINER_MP4, mov_exts, ".mp4", 1, 0, probe_mov_family_duration,
    };
    static const zms_vod_format_desc mkv = {
        "mkv", ZMS_CONTAINER_MKV, mkv_exts, NULL, 1, 0, probe_mov_family_duration,
    };
    static const zms_vod_format_desc flv = {
        "flv-file", ZMS_CONTAINER_FLV_FILE, flv_exts, NULL, 0, 1, probe_flv_duration,
    };

    if (g_vod_format_builtins_registered) {
        return;
    }
    g_vod_format_builtins_registered = 1;
    zms_vod_format_register(&mov);
    zms_vod_format_register(&mkv);
    zms_vod_format_register(&flv);
}

static void ensure_builtins(void)
{
    if (!g_vod_format_builtins_registered) {
        zms_vod_format_register_all();
    }
}

const zms_vod_format_desc *zms_vod_format_find_by_extension(const char *ext)
{
    int i;

    ensure_builtins();
    if (!ext || !ext[0]) {
        return NULL;
    }
    for (i = 0; i < g_format_count; ++i) {
        const char *const *e;
        if (!g_formats[i] || !g_formats[i]->extensions) {
            continue;
        }
        for (e = g_formats[i]->extensions; *e; ++e) {
            if (ext_eq(ext, *e)) {
                return g_formats[i];
            }
        }
    }
    return NULL;
}

const zms_vod_format_desc *zms_vod_format_find_by_path(const char *path)
{
    return zms_vod_format_find_by_extension(path_ext(path));
}

int zms_vod_format_foreach_extension(zms_vod_format_visit_cb cb, void *user)
{
    int i;
    int n = 0;

    ensure_builtins();
    if (!cb) {
        return 0;
    }
    for (i = 0; i < g_format_count; ++i) {
        const char *const *e;
        if (!g_formats[i] || !g_formats[i]->extensions) {
            continue;
        }
        for (e = g_formats[i]->extensions; *e; ++e) {
            ++n;
            if (cb(g_formats[i], *e, user) != 0) {
                return n;
            }
        }
    }
    return n;
}

const char *zms_vod_format_default_extension(void)
{
    int i;

    ensure_builtins();
    for (i = 0; i < g_format_count; ++i) {
        if (g_formats[i] && g_formats[i]->default_extension) {
            return g_formats[i]->default_extension;
        }
    }
    return ".mp4";
}

int zms_vod_format_stream_has_supported_ext(const char *stream)
{
    return zms_vod_format_find_by_path(stream) != NULL;
}

int zms_vod_format_stream_is_native_file(const char *stream)
{
    const zms_vod_format_desc *fmt = zms_vod_format_find_by_path(stream);
    return fmt && fmt->native_file_send;
}

int zms_vod_format_stream_is_flv_wrap_suffix(const char *stream)
{
    size_t n;
    char disk_ext[32];
    const char *dot;
    const zms_vod_format_desc *fmt;

    if (!stream) {
        return 0;
    }
    n = strlen(stream);
    if (n < 8 || zms_vod_format_stricmp(stream + n - 4, ".flv") != 0) {
        return 0;
    }
    dot = strrchr(stream, '.');
    if (!dot || dot == stream) {
        return 0;
    }
    {
        const char *prev = dot - 1;
        while (prev > stream && *prev != '/' && *prev != '\\' && *prev != '.') {
            --prev;
        }
        if (*prev != '.') {
            return 0;
        }
        if ((size_t)(dot - prev) >= sizeof(disk_ext)) {
            return 0;
        }
        memcpy(disk_ext, prev, (size_t)(dot - prev));
        disk_ext[dot - prev] = '\0';
    }
    fmt = zms_vod_format_find_by_extension(disk_ext);
    return fmt && fmt->can_flv_wrap;
}

uint64_t zms_vod_format_probe_duration_ms(const char *path)
{
    const zms_vod_format_desc *fmt = zms_vod_format_find_by_path(path);
    if (!fmt || !fmt->probe_duration_ms) {
        return 0;
    }
    return fmt->probe_duration_ms(path);
}
