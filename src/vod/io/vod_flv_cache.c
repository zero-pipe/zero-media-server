#include "zms/vod/vod_flv_cache.h"
#include "zms/vod/vod_flv_index.h"
#include "zms/vod/play/vod_play_lane.h"
#include "zms/vod/io/vod_source.h"
#include "zms/vod/play/vod_flv_muxer.h"
#include "ztk/util/log.h"
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <sys/stat.h>
#else
#include <sys/stat.h>
#endif

static int file_mtime(const char *path, time_t *out)
{
    struct stat st;
    if (!path || !out || stat(path, &st) != 0) {
        return 0;
    }
    *out = st.st_mtime;
    return 1;
}

int zms_vod_flv_cache_path(const char *app, const char *stream, char *out, size_t out_cap)
{
    char media_path[512];
    char *dot;
    size_t n;

    if (!app || !stream || !out || out_cap == 0) {
        return 0;
    }
    if (!zms_vod_resolve_file_path(app, stream, media_path, sizeof(media_path))) {
        return 0;
    }
    n = strlen(media_path);
    if (n + 1 >= out_cap) {
        return 0;
    }
    memcpy(out, media_path, n + 1);
    dot = strrchr(out, '.');
    if (!dot || strcmp(dot, ".mp4") != 0) {
        return 0;
    }
    if (strlen(out) + 4 >= out_cap) {
        return 0;
    }
    strcat(out, ".flv");
    return 1;
}

int zms_vod_flv_cache_valid(const char *app, const char *stream)
{
    char media_path[512];
    char flv[512];
    time_t media_t = 0;
    time_t flv_t = 0;
    struct stat st;

    if (!zms_vod_resolve_file_path(app, stream, media_path, sizeof(media_path))) {
        return 0;
    }
    if (!zms_vod_flv_cache_path(app, stream, flv, sizeof(flv))) {
        return 0;
    }
    if (!file_mtime(media_path, &media_t) || !file_mtime(flv, &flv_t)) {
        return 0;
    }
    if (stat(flv, &st) != 0 || st.st_size < 256) {
        return 0;
    }
    return flv_t >= media_t;
}

static ztk_err_t mux_lane_to_file(zms_vod_play_lane *lane, zms_media_source *src, FILE *fp)
{
    zms_vod_flv_muxer *muxer;
    const zms_vod_flv_index *idx;
    size_t vlen = 0;
    size_t alen = 0;
    uint64_t dur_ms;
    uint8_t stack[256 * 1024];
    size_t cap = sizeof(stack);
    uint8_t *buf = stack;
    uint8_t *heap = NULL;
    size_t flv_len = 0;
    int idle;
    int loops = 0;

    if (!lane || !src || !fp) {
        return ZTK_ERR_INVALID;
    }

    dur_ms = zms_vod_source_duration_ms(src);
    idx = zms_vod_source_flv_index(src);
    muxer = zms_vod_flv_muxer_create_reader(src, zms_vod_play_lane_buffer_reader(lane));
    if (!muxer) {
        return ZTK_ERR_NOMEM;
    }

    zms_vod_flv_muxer_configure(muxer, zms_vod_play_lane_video_config(lane, &vlen), vlen,
                                zms_vod_play_lane_audio_config(lane, &alen), alen, dur_ms);
    zms_vod_flv_muxer_set_index_full(muxer, idx);
    zms_vod_flv_muxer_set_http_realtime_pace(muxer, 0);
    zms_vod_flv_muxer_seek(muxer, 0);
    zms_vod_flv_muxer_bind_source(muxer, src);

    if (zms_vod_flv_muxer_start(muxer, src->has_audio, src->has_video, buf, cap, &flv_len) !=
            ZTK_OK ||
        flv_len == 0) {
        zms_vod_flv_muxer_destroy(muxer);
        return ZTK_ERR_INVALID;
    }
    if (fwrite(buf, 1, flv_len, fp) != flv_len) {
        zms_vod_flv_muxer_destroy(muxer);
        return ZTK_ERR_IO;
    }

    idle = 0;
    while (idle < 64 && loops < 5000000) {
        size_t n = 0;
        int r = zms_vod_flv_muxer_next(muxer, buf, cap, &n);
        if (r > 0 && n > 0) {
            if (fwrite(buf, 1, n, fp) != n) {
                zms_vod_flv_muxer_destroy(muxer);
                return ZTK_ERR_IO;
            }
            idle = 0;
        } else {
            idle++;
        }
        if (zms_vod_reader_pump(zms_vod_play_lane_reader(lane)) > 0) {
            idle = 0;
        }
        loops++;
    }

    zms_vod_flv_muxer_destroy(muxer);
    return idle >= 64 ? ZTK_OK : ZTK_ERR_IO;
}

ztk_err_t zms_vod_flv_cache_ensure(const char *app, const char *stream, zms_media_source *src,
                                   ztk_poller *pol)
{
    char flv[512];
    char tmp[520];
    zms_vod_play_lane *lane;
    FILE *fp;
    ztk_err_t err;

    if (!app || !stream || !src) {
        return ZTK_ERR_INVALID;
    }
    if (zms_vod_flv_cache_valid(app, stream)) {
        return ZTK_OK;
    }
    if (!zms_vod_flv_cache_path(app, stream, flv, sizeof(flv))) {
        return ZTK_ERR_INVALID;
    }

    snprintf(tmp, sizeof(tmp), "%s.tmp", flv);
    ztk_info("vod flv cache: building %s (first HTTP play may take a while)", flv);

    lane = zms_vod_play_lane_open(src, pol);
    if (!lane) {
        return ZTK_ERR_NOMEM;
    }
    zms_vod_play_lane_prepare(lane, 0);
    zms_vod_play_lane_set_pump_hold(lane, 1);

    fp = fopen(tmp, "wb");
    if (!fp) {
        zms_vod_play_lane_close(lane);
        return ZTK_ERR_IO;
    }

    err = mux_lane_to_file(lane, src, fp);
    fclose(fp);
    zms_vod_play_lane_close(lane);

    if (err != ZTK_OK) {
        remove(tmp);
        ztk_warn("vod flv cache: build failed %s", flv);
        return err;
    }
    remove(flv);
    if (rename(tmp, flv) != 0) {
        remove(tmp);
        return ZTK_ERR_IO;
    }
    ztk_info("vod flv cache: ready %s", flv);
    return ZTK_OK;
}
