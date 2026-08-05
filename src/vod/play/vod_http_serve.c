#include "session/http/http_session_internal.h"
#include "ztk/platform.h"
#include "session/http/http_router.h"
#include "zms/ops/api/webhook/webhook_client.h"
#include "zms/egress/egress_mp4_recorder.h"
#include "zms/engine/media_event.h"
#include "zms/engine/stream/stream_hub.h"
#include "zms/vod/io/vod_thread_pool.h"
#include "zms/vod/play/vod_flv_muxer.h"
#include "zms/vod/vod_flv_cache.h"
#include "zms/vod/vod_flv_index.h"
#include "zms/vod/vod_hls.h"
#include "zms/vod/play/vod_play_lane.h"
#include "zms/vod/io/vod_source.h"
#include "ztk/net/tcp_server.h"
#include "ztk/poller/poller.h"
#include "ztk/util/buf.h"
#include "ztk/util/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <sys/stat.h>
#else
#include <sys/stat.h>
#endif

static uint64_t range_bytes_to_seek_ms(const char *range, uint64_t dur_ms, uint64_t est_bytes,
                                       const zms_vod_flv_index *idx)
{
    const char *p;
    unsigned long long byte_start = 0;
    size_t i;

    if (!range || !range[0]) {
        return 0;
    }
    p = strstr(range, "bytes=");
    if (!p) {
        return 0;
    }
    p += 6;
    byte_start = strtoull(p, NULL, 10);
    if (byte_start == 0) {
        return 0;
    }

    if (idx && idx->count > 1) {
        for (i = 1; i < idx->count; ++i) {
            if (idx->filepositions[i] > (double)byte_start) {
                return (uint64_t)(idx->times[i] * 1000.0 + 0.5);
            }
        }
        if (dur_ms > 0) {
            return dur_ms > 1000 ? dur_ms - 1000 : 0;
        }
    }

    if (!dur_ms) {
        return 0;
    }
    if (!est_bytes) {
        est_bytes = dur_ms * 125ULL;
    }
    if (byte_start >= est_bytes) {
        byte_start = (unsigned long long)(est_bytes * 95ULL / 100ULL);
    }
    return (uint64_t)((double)byte_start / (double)est_bytes * (double)dur_ms);
}

static uint64_t vod_est_seek_bytes(const char *app, const char *stream, uint64_t dur_ms)
{
    char path[512];
    struct stat st;

    if (zms_vod_resolve_file_path(app, stream, path, sizeof(path)) == 0 && stat(path, &st) == 0 &&
        st.st_size > 0) {
        return (uint64_t)st.st_size;
    }
    return dur_ms > 0 ? dur_ms * 125ULL : 0;
}

static uint64_t vod_flv_resolve_seek_ms(zms_media_source *src, const char *app, const char *stream,
                                        uint64_t seek_ms, const char *range_hdr)
{
    if (seek_ms || !range_hdr || !range_hdr[0]) {
        return seek_ms;
    }

    {
        const zms_vod_flv_index *flv_idx = zms_vod_source_flv_index(src);
        uint64_t vod_dur_ms = zms_vod_source_duration_ms(src);
        uint64_t est_bytes;

        if (!vod_dur_ms) {
            vod_dur_ms = zms_vod_probe_duration_ms(app, stream);
        }
        est_bytes = flv_idx && flv_idx->filesize > 0 ? (uint64_t)flv_idx->filesize
                                                     : vod_est_seek_bytes(app, stream, vod_dur_ms);
        return range_bytes_to_seek_ms(range_hdr, vod_dur_ms, est_bytes, flv_idx);
    }
}

typedef struct zms_vod_flv_cache_ctx {
    zms_http_session *hs;
    zms_media_source *src;
    char app[ZMS_APP_MAX];
    char stream[ZMS_STREAM_MAX];
    char flv_cache[512];
    char range_hdr[256];
    uint64_t seek_ms;
    ztk_err_t cache_err;
} zms_vod_flv_cache_ctx;

static int http_sess_alive(zms_http_session *hs)
{
    return hs && hs->tcp && ztk_tcp_session_user(hs->tcp) == hs;
}

static ztk_err_t vod_flv_start_hdr(void *muxer, int has_audio, int has_video, uint8_t *out,
                                   size_t cap, size_t *out_len)
{
    return zms_vod_flv_muxer_start((zms_vod_flv_muxer *)muxer, has_audio, has_video, out, cap,
                                   out_len);
}

static void vod_flv_cache_build_blocking(void *user)
{
    zms_vod_flv_cache_ctx *job = (zms_vod_flv_cache_ctx *)user;
    job->cache_err = zms_vod_flv_cache_ensure(job->app, job->stream, job->src, NULL);
}

static void zms_http_vod_flv_start_transcode(zms_http_session *hs, zms_media_source *src,
                                             const char *app, const char *stream, uint64_t seek_ms,
                                             const char *range_hdr);

static void vod_flv_cache_build_on_io(void *user)
{
    zms_vod_flv_cache_ctx *job = (zms_vod_flv_cache_ctx *)user;

    if (!http_sess_alive(job->hs)) {
        free(job);
        return;
    }
    if (job->cache_err == ZTK_OK && zms_vod_flv_cache_valid(job->app, job->stream)) {
        zms_http_response_send_vod_flv_file(job->hs, job->src, job->flv_cache,
                                            job->range_hdr[0] ? job->range_hdr : NULL,
                                            job->seek_ms);
    } else {
        uint64_t seek = vod_flv_resolve_seek_ms(job->src, job->app, job->stream, job->seek_ms,
                                                job->range_hdr[0] ? job->range_hdr : NULL);
        zms_http_vod_flv_start_transcode(job->hs, job->src, job->app, job->stream, seek, NULL);
    }
    free(job);
}

static void zms_http_vod_flv_start_transcode(zms_http_session *hs, zms_media_source *src,
                                             const char *app, const char *stream, uint64_t seek_ms,
                                             const char *range_hdr)
{
    ztk_poller *pol;
    uint64_t vod_dur_ms = 0;
    uint64_t play_ms = 0;
    size_t vcfg_len = 0;
    size_t acfg_len = 0;
    const uint8_t *vcfg = NULL;
    const uint8_t *acfg = NULL;
    char hdr[384];
    int n;

    pol = hs->tcp ? ztk_tcp_session_poller(hs->tcp) : NULL;
    if (!pol) {
        zms_http_response_send_error(hs, 503, "Service Unavailable");
        return;
    }
    hs->vod_lane = zms_vod_play_lane_open(src, pol);
    if (!hs->vod_lane) {
        ztk_warn("HTTP-FLV vod lane open failed: app=%s stream=%s", app, stream);
        zms_http_response_send_error(hs, 404, "Not Found");
        return;
    }
    if (seek_ms) {
        play_ms = zms_vod_play_lane_seek_ms(hs->vod_lane, seek_ms);
    } else {
        play_ms = zms_vod_play_lane_prepare(hs->vod_lane, 0);
    }
    zms_vod_play_lane_prefill(hs->vod_lane);
    vod_dur_ms = zms_vod_source_duration_ms(src);
    if (!vod_dur_ms) {
        vod_dur_ms = zms_vod_probe_duration_ms(app, stream);
    }
    vcfg = zms_vod_play_lane_video_config(hs->vod_lane, &vcfg_len);
    acfg = zms_vod_play_lane_audio_config(hs->vod_lane, &acfg_len);
    hs->vod_muxer =
        zms_vod_flv_muxer_create_reader(src, zms_vod_play_lane_buffer_reader(hs->vod_lane));
    if (!hs->vod_muxer) {
        zms_vod_play_lane_close(hs->vod_lane);
        hs->vod_lane = NULL;
        zms_http_response_send_error(hs, 500, "Internal Server Error");
        return;
    }

    {
        const zms_vod_flv_index *flv_idx = zms_vod_source_flv_index(src);
        zms_vod_flv_index_view *flv_view =
            flv_idx ? zms_vod_flv_index_view_create(flv_idx, play_ms) : NULL;
        zms_vod_flv_muxer_configure(hs->vod_muxer, vcfg, vcfg_len, acfg, acfg_len, vod_dur_ms);
        zms_vod_flv_muxer_set_index_view(hs->vod_muxer, flv_view);
        zms_vod_flv_muxer_set_http_realtime_pace(hs->vod_muxer, 1);
        zms_vod_flv_muxer_seek(hs->vod_muxer,
                               play_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)play_ms);
    }
    zms_vod_flv_muxer_bind_poller(hs->vod_muxer, pol);
    zms_vod_flv_muxer_bind_source(hs->vod_muxer, src);
    hs->source = src;
    ztk_info("HTTP-FLV vod play: app=%s stream=%s seek=%llu ms dur=%llu ms", app, stream,
             (unsigned long long)play_ms, (unsigned long long)vod_dur_ms);

    {
        zms_media_tuple tuple;
        zms_media_tuple_from_source(src, &tuple);
        if (!zms_webhook_allow_play(&tuple, "http-flv", hs->tcp, NULL)) {
            zms_http_response_send_error(hs, 403, "Forbidden");
            return;
        }
    }
    zms_media_source_reader_add(src);
    hs->play_start_ms = ztk_monotonic_ms();
    zms_media_event_play(src, "http-flv");
    hs->reader_attached = 1;
    hs->play_event = "http-flv";

    if (!vod_dur_ms) {
        vod_dur_ms = zms_vod_probe_duration_ms(app, stream);
    }
    n = snprintf(hdr, sizeof(hdr),
                 "HTTP/1.1 200 OK\r\n"
                 "Connection: close\r\n"
                 "Content-Type: video/x-flv\r\n"
                 "Access-Control-Allow-Origin: *\r\n"
                 "Accept-Ranges: none\r\n"
                 "X-Vod-Duration-Ms: %llu\r\n"
                 "X-Vod-Seek-Query: start=<ms>|seek=<sec>\r\n\r\n",
                 (unsigned long long)vod_dur_ms);
    ztk_tcp_session_send(hs->tcp, hdr, (size_t)n);

    zms_http_flv_emit_header_and_stream(hs, src->has_audio, src->has_video, vod_flv_start_hdr,
                                        hs->vod_muxer);
}

void zms_http_vod_flv_start(zms_http_session *hs, zms_media_source *src, const char *app,
                            const char *stream, uint64_t seek_ms, const char *range_hdr)
{
    ztk_poller *pol;
    char flv_cache[512];
    char disk_flv[512];

    if (!hs || !src) {
        zms_http_response_send_error(hs, 404, "Not Found");
        return;
    }

    /* 磁盘原生 .flv：直接 Range/文件下发（ZLMediaKit 路径），避免 demux→remux + Accept-Ranges: none */
    if (zms_vod_stream_is_native_flv_file(stream) &&
        zms_vod_resolve_file_path(app, stream, disk_flv, sizeof(disk_flv))) {
        if (!seek_ms && range_hdr && range_hdr[0]) {
            seek_ms = vod_flv_resolve_seek_ms(src, app, stream, 0, range_hdr);
        }
        ztk_info("HTTP-FLV vod file: %s seek=%llu ms%s", disk_flv, (unsigned long long)seek_ms,
                 (range_hdr && range_hdr[0]) ? " (Range)" : "");
        zms_http_response_send_vod_flv_file(hs, src, disk_flv, range_hdr, seek_ms);
        return;
    }

    pol = hs->tcp ? ztk_tcp_session_poller(hs->tcp) : NULL;
    if (pol && zms_vod_flv_cache_path(app, stream, flv_cache, sizeof(flv_cache))) {
        if (zms_vod_flv_cache_valid(app, stream)) {
            zms_http_response_send_vod_flv_file(hs, src, flv_cache, range_hdr, seek_ms);
            return;
        }
        if (zms_vod_thread_pool_enabled()) {
            zms_vod_flv_cache_ctx *job = (zms_vod_flv_cache_ctx *)calloc(1, sizeof(*job));
            if (job) {
                job->hs = hs;
                job->src = src;
                strncpy(job->app, app, sizeof(job->app) - 1);
                strncpy(job->stream, stream, sizeof(job->stream) - 1);
                strncpy(job->flv_cache, flv_cache, sizeof(job->flv_cache) - 1);
                if (range_hdr && range_hdr[0]) {
                    strncpy(job->range_hdr, range_hdr, sizeof(job->range_hdr) - 1);
                }
                job->seek_ms = seek_ms;
                if (zms_vod_thread_pool_run(pol, vod_flv_cache_build_blocking,
                                            vod_flv_cache_build_on_io, job) == ZTK_OK) {
                    return;
                }
                free(job);
            }
        }
        if (zms_vod_flv_cache_ensure(app, stream, src, pol) == ZTK_OK &&
            zms_vod_flv_cache_valid(app, stream)) {
            zms_http_response_send_vod_flv_file(hs, src, flv_cache, range_hdr, seek_ms);
            return;
        }
    }

    seek_ms = vod_flv_resolve_seek_ms(src, app, stream, seek_ms, range_hdr);
    zms_http_vod_flv_start_transcode(hs, src, app, stream, seek_ms, NULL);
}

static void http_vod_hls_attach_play(zms_http_session *hs, zms_media_source *src,
                                     const char *player)
{
    zms_media_tuple tuple;

    if (!hs || !src) {
        return;
    }
    zms_media_tuple_from_source(src, &tuple);
    if (!zms_webhook_allow_play(&tuple, player, hs->tcp, NULL)) {
        return;
    }
    zms_media_source_reader_add(src);
    hs->play_start_ms = ztk_monotonic_ms();
    zms_media_event_play(src, player);
    hs->reader_attached = 1;
    hs->play_event = player;
    hs->source = src;
}

typedef struct vod_hls_m3u8_job {
    zms_http_session *hs;
    zms_media_source *src;
    char app[ZMS_APP_MAX];
    char stream[ZMS_STREAM_MAX];
    char rel_path[256];
    char disk_path[512];
    int need_ensure;
    int ok;
    uint8_t *body;
    size_t body_len;
} vod_hls_m3u8_job;

typedef struct zms_vod_hls_seg_ctx {
    zms_http_session *hs;
    zms_media_source *src;
    char app[ZMS_APP_MAX];
    char play_stream[ZMS_STREAM_MAX];
    char rel_path[256];
    char disk_path[512];
    uint64_t seg_no;
    ztk_err_t err;
} zms_vod_hls_seg_ctx;

static void http_send_vod_hls_m3u8_finish(zms_http_session *hs, zms_media_source *src,
                                          const char *app, const char *rel_path,
                                          const char *disk_path, const void *body, size_t body_len)
{
    size_t n;
    zms_media_tuple tuple;

    if (!hs || !body || body_len == 0 || body_len >= hs->send_cap) {
        zms_http_response_send_error(hs, 404, "Not Found");
        return;
    }
    memcpy(hs->send_buf, body, body_len);
    hs->send_buf[body_len] = '\0';
    n = zms_http_route_rewrite_vod_hls_m3u8((char *)hs->send_buf, hs->send_cap, app, rel_path);
    if (src) {
        zms_media_tuple_from_source(src, &tuple);
        if (!zms_webhook_allow_play(&tuple, "hls", hs->tcp, NULL)) {
            zms_http_response_send_error(hs, 403, "Forbidden");
            return;
        }
        http_vod_hls_attach_play(hs, src, "hls");
    }
    ztk_info("HLS vod m3u8: app=%s stream=%s path=%s bytes=%u", app, src ? src->stream : "",
             disk_path, (unsigned)n);
    zms_http_response_send_bytes(hs, 200, "application/vnd.apple.mpegurl", hs->send_buf, n);
}

static void vod_hls_m3u8_blocking(void *user)
{
    vod_hls_m3u8_job *job = (vod_hls_m3u8_job *)user;
    FILE *fp;
    size_t n;
    uint8_t *buf;
    struct stat st;

    job->ok = 0;
    job->body = NULL;
    job->body_len = 0;

    if (job->need_ensure && job->src) {
        if (zms_vod_hls_ensure_playlist(job->src) != ZTK_OK) {
            return;
        }
        if (!zms_vod_hls_resolve_playlist(job->app, job->stream, job->disk_path,
                                          sizeof(job->disk_path))) {
            return;
        }
    }

    if (stat(job->disk_path, &st) != 0 || st.st_size <= 0 || st.st_size >= 4 * 1024 * 1024) {
        return;
    }
    fp = fopen(job->disk_path, "rb");
    if (!fp) {
        return;
    }
    n = (size_t)st.st_size;
    buf = (uint8_t *)malloc(n);
    if (!buf) {
        fclose(fp);
        return;
    }
    if (fread(buf, 1, n, fp) != n) {
        fclose(fp);
        free(buf);
        return;
    }
    fclose(fp);
    job->body = buf;
    job->body_len = n;
    job->ok = 1;
}

static void vod_hls_m3u8_on_io(void *user)
{
    vod_hls_m3u8_job *job = (vod_hls_m3u8_job *)user;

    if (!http_sess_alive(job->hs)) {
        free(job->body);
        free(job);
        return;
    }
    if (!job->ok) {
        zms_http_response_send_error(job->hs, 404, "Not Found");
        free(job->body);
        free(job);
        return;
    }
    http_send_vod_hls_m3u8_finish(job->hs, job->src, job->app, job->rel_path, job->disk_path,
                                  job->body, job->body_len);
    free(job->body);
    free(job);
}

static int http_send_vod_hls_m3u8_async(zms_http_session *hs, zms_media_source *src,
                                        const char *app, const char *stream, const char *rel_path,
                                        const char *disk_path, int need_ensure)
{
    ztk_poller *pol;
    vod_hls_m3u8_job *job;

    if (!zms_vod_thread_pool_enabled() || !hs) {
        return 0;
    }
    pol = hs->tcp ? ztk_tcp_session_poller(hs->tcp) : NULL;
    if (!pol) {
        return 0;
    }

    job = (vod_hls_m3u8_job *)calloc(1, sizeof(*job));
    if (!job) {
        return 0;
    }
    job->hs = hs;
    job->src = src;
    job->need_ensure = need_ensure;
    strncpy(job->app, app, sizeof(job->app) - 1);
    if (stream && stream[0]) {
        strncpy(job->stream, stream, sizeof(job->stream) - 1);
    }
    strncpy(job->rel_path, rel_path, sizeof(job->rel_path) - 1);
    strncpy(job->disk_path, disk_path, sizeof(job->disk_path) - 1);

    if (zms_vod_thread_pool_run(pol, vod_hls_m3u8_blocking, vod_hls_m3u8_on_io, job) != ZTK_OK) {
        free(job);
        return 0;
    }
    return 1;
}

static void http_send_vod_hls_m3u8(zms_http_session *hs, zms_media_source *src, const char *app,
                                   const char *rel_path, const char *disk_path)
{
    vod_hls_m3u8_job sync;

    if (http_send_vod_hls_m3u8_async(hs, src, app, NULL, rel_path, disk_path, 0)) {
        return;
    }

    memset(&sync, 0, sizeof(sync));
    sync.hs = hs;
    sync.src = src;
    strncpy(sync.app, app, sizeof(sync.app) - 1);
    strncpy(sync.rel_path, rel_path, sizeof(sync.rel_path) - 1);
    strncpy(sync.disk_path, disk_path, sizeof(sync.disk_path) - 1);
    vod_hls_m3u8_blocking(&sync);
    if (!sync.ok) {
        free(sync.body);
        zms_http_response_send_error(hs, 404, "Not Found");
        return;
    }
    http_send_vod_hls_m3u8_finish(hs, src, app, rel_path, disk_path, sync.body, sync.body_len);
    free(sync.body);
}

static void vod_hls_seg_blocking(void *user)
{
    zms_vod_hls_seg_ctx *job = (zms_vod_hls_seg_ctx *)user;

    job->err = ZTK_ERR_INVALID;
    if (!job->src) {
        return;
    }
    job->err = zms_vod_hls_ensure_segment(job->src, job->seg_no, NULL);
    if (job->err != ZTK_OK) {
        return;
    }
    if (!zms_vod_hls_segment_path(job->app, job->play_stream, job->seg_no, job->disk_path,
                                  sizeof(job->disk_path))) {
        job->err = ZTK_ERR_INVALID;
    }
}

static void vod_hls_seg_on_io(void *user)
{
    zms_vod_hls_seg_ctx *job = (zms_vod_hls_seg_ctx *)user;

    if (!http_sess_alive(job->hs)) {
        free(job);
        return;
    }
    if (job->err != ZTK_OK || !job->disk_path[0]) {
        ztk_warn("HLS vod segment gen failed: app=%s stream=%s rel=%s", job->app, job->play_stream,
                 job->rel_path);
        zms_http_response_send_error(job->hs, 404, "Not Found");
        free(job);
        return;
    }
    ztk_info("HLS vod ts (generated): app=%s stream=%s rel=%s path=%s", job->app, job->play_stream,
             job->rel_path, job->disk_path);
    zms_http_response_send_static_file(job->hs, job->src, job->disk_path, "video/mp2t", "hls");
    free(job);
}

static int zms_http_vod_hls_serve_segment_async(zms_http_session *hs, zms_media_source *src,
                                                const char *app, const char *play_stream,
                                                const char *rel_path, uint64_t seg_no)
{
    ztk_poller *pol;
    zms_vod_hls_seg_ctx *job;

    if (!zms_vod_thread_pool_enabled() || !hs || !src) {
        return 0;
    }
    pol = hs->tcp ? ztk_tcp_session_poller(hs->tcp) : NULL;
    if (!pol) {
        return 0;
    }

    job = (zms_vod_hls_seg_ctx *)calloc(1, sizeof(*job));
    if (!job) {
        return 0;
    }
    job->hs = hs;
    job->src = src;
    job->seg_no = seg_no;
    strncpy(job->app, app, sizeof(job->app) - 1);
    strncpy(job->play_stream, play_stream, sizeof(job->play_stream) - 1);
    strncpy(job->rel_path, rel_path, sizeof(job->rel_path) - 1);

    if (zms_vod_thread_pool_run(pol, vod_hls_seg_blocking, vod_hls_seg_on_io, job) != ZTK_OK) {
        free(job);
        return 0;
    }
    return 1;
}

void zms_http_vod_hls_serve(zms_http_session *hs, zms_media_source *src, const char *app,
                            const char *stream, const char *rel_path)
{
    char disk_path[512];
    char play_stream[ZMS_STREAM_MAX];
    ztk_poller *pol;
    size_t flen;
    const char *mp4_stream = stream;

    if (!hs || !app || !rel_path || !rel_path[0]) {
        return;
    }

    flen = strlen(rel_path);
    if (flen >= 5 && strcmp(rel_path + flen - 5, ".m3u8") == 0) {
        struct stat st;
        int have_file = 0;

        if (zms_vod_resolve_rel_path(app, rel_path, disk_path, sizeof(disk_path)) &&
            stat(disk_path, &st) == 0 && st.st_size > 16) {
            have_file = 1;
        }
        if (!have_file) {
            if (!mp4_stream || !mp4_stream[0] || !src) {
                ztk_warn("HLS vod m3u8 404: app=%s rel=%s", app, rel_path);
                zms_http_response_send_error(hs, 404, "Not Found");
                return;
            }
            if (http_send_vod_hls_m3u8_async(hs, src, app, mp4_stream, rel_path, disk_path, 1)) {
                return;
            }
            if (zms_vod_hls_ensure_playlist(src) != ZTK_OK ||
                !zms_vod_hls_resolve_playlist(app, mp4_stream, disk_path, sizeof(disk_path))) {
                ztk_warn("HLS vod m3u8 404: app=%s rel=%s", app, rel_path);
                zms_http_response_send_error(hs, 404, "Not Found");
                return;
            }
        }
        http_send_vod_hls_m3u8(hs, src, app, rel_path, disk_path);
        return;
    }

    {
        const char *dot = strrchr(rel_path, '.');
        const char *base = rel_path;
        const char *slash = strrchr(rel_path, '/');
        unsigned long long seg_no = 0;

        if (!dot || strcmp(dot, ".ts") != 0) {
            zms_http_response_send_error(hs, 404, "Not Found");
            return;
        }
        if (slash) {
            base = slash + 1;
        }

        play_stream[0] = '\0';
        if (mp4_stream && mp4_stream[0]) {
            strncpy(play_stream, mp4_stream, sizeof(play_stream) - 1);
        } else {
            (void)zms_vod_infer_stream_from_ts(app, rel_path, play_stream, sizeof(play_stream));
        }
        if (!src && play_stream[0]) {
            src = zms_media_source_find_for_play(ZMS_SCHEMA_RTMP, app, play_stream);
        }

        if (zms_vod_hls_resolve_segment_file(app, play_stream, rel_path, disk_path,
                                             sizeof(disk_path))) {
            ztk_info("HLS vod ts: app=%s rel=%s path=%s", app, rel_path, disk_path);
            zms_http_response_send_static_file(hs, src, disk_path, "video/mp2t", "hls");
            return;
        }

        if (play_stream[0] && zms_vod_hls_has_static_pack(app, play_stream)) {
            ztk_warn("HLS vod static segment 404: app=%s stream=%s rel=%s", app, play_stream,
                     rel_path);
            zms_http_response_send_error(hs, 404, "Not Found");
            return;
        }

        if (!src || !play_stream[0]) {
            ztk_warn("HLS vod ts 404: app=%s rel=%s (no source)", app, rel_path);
            zms_http_response_send_error(hs, 404, "Not Found");
            return;
        }

        pol = hs->tcp ? ztk_tcp_session_poller(hs->tcp) : NULL;
        seg_no = strtoull(base, NULL, 10);
        if (zms_http_vod_hls_serve_segment_async(hs, src, app, play_stream, rel_path, seg_no)) {
            return;
        }
        if (!pol || zms_vod_hls_ensure_segment(src, seg_no, pol) != ZTK_OK ||
            !zms_vod_hls_segment_path(app, play_stream, seg_no, disk_path, sizeof(disk_path))) {
            ztk_warn("HLS vod segment gen failed: app=%s stream=%s rel=%s", app, play_stream,
                     rel_path);
            zms_http_response_send_error(hs, 404, "Not Found");
            return;
        }
        ztk_info("HLS vod ts (generated): app=%s stream=%s rel=%s path=%s", app, play_stream,
                 rel_path, disk_path);
        zms_http_response_send_static_file(hs, src, disk_path, "video/mp2t", "hls");
    }
}

void zms_http_vod_mp4_serve(zms_http_session *hs, zms_media_source *src, const char *app,
                            const char *stream, const char *range_hdr, int head_only)
{
    char mp4_path[512];

    if (!hs || !src || !app || !stream) {
        return;
    }

    if (!zms_vod_resolve_file_path(app, stream, mp4_path, sizeof(mp4_path))) {
        zms_http_response_send_error(hs, 404, "Not Found");
        return;
    }

    ztk_info("HTTP-MP4 vod: app=%s stream=%s path=%s%s", app, stream, mp4_path,
             head_only ? " (HEAD)" : "");
    zms_http_response_send_vod_mp4_file(hs, src, mp4_path, range_hdr, head_only);
}

static int vod_hls_route_match(const zms_http_request *req)
{
    char app[ZMS_APP_MAX];
    char stream[ZMS_STREAM_MAX];
    char hls_file[128];

    return req &&
           zms_http_route_parse_vod_hls_path(req->path, app, stream, hls_file, sizeof(hls_file));
}

static void vod_hls_route_handle(zms_http_session *hs, const zms_http_request *req)
{
    zms_media_source *src;
    char app[ZMS_APP_MAX];
    char stream[ZMS_STREAM_MAX];
    char hls_file[128];

    if (strcmp(req->method, "GET") != 0) {
        zms_http_response_send_error(hs, 405, "Method Not Allowed");
        return;
    }
    if (!zms_http_route_parse_vod_hls_path(req->path, app, stream, hls_file, sizeof(hls_file))) {
        zms_http_response_send_error(hs, 404, "Not Found");
        return;
    }
    src = stream[0] ? zms_media_source_find_for_play(ZMS_SCHEMA_RTMP, app, stream) : NULL;
    if (!src) {
        char disk[512];
        struct stat st;
        if (!zms_vod_resolve_rel_path(app, hls_file, disk, sizeof(disk)) || stat(disk, &st) != 0 ||
            st.st_size <= 0) {
            if (!stream[0] || !zms_vod_hls_has_static_pack(app, stream)) {
                zms_http_response_send_error(hs, 404, "Not Found");
                return;
            }
        }
    }
    zms_http_vod_hls_serve(hs, src, app, stream, hls_file);
}

static int vod_mp4_route_match(const zms_http_request *req)
{
    char app[ZMS_APP_MAX];
    char stream[ZMS_STREAM_MAX];

    return req && zms_http_route_parse_mp4_path(req->path, app, stream);
}

static void vod_mp4_route_handle(zms_http_session *hs, const zms_http_request *req)
{
    zms_media_source *src;
    char app[ZMS_APP_MAX];
    char stream[ZMS_STREAM_MAX];

    if (!req || (strcmp(req->method, "GET") != 0 && strcmp(req->method, "HEAD") != 0)) {
        zms_http_response_send_error(hs, 405, "Method Not Allowed");
        return;
    }
    if (!zms_http_route_parse_mp4_path(req->path, app, stream)) {
        zms_http_response_send_error(hs, 404, "Not Found");
        return;
    }
    src = zms_media_source_find_for_play(ZMS_SCHEMA_RTMP, app, stream);
    if (!src || !zms_media_source_is_vod(src)) {
        zms_http_response_send_error(hs, 404, "Not Found");
        return;
    }
    if (hs->state == ZMS_HTTP_SESSION_STATE_STREAMING ||
        hs->state == ZMS_HTTP_SESSION_STATE_FILE_SENDING ||
        hs->state == ZMS_HTTP_SESSION_STATE_HLS_SENDING) {
        zms_http_session_stop_stream(hs);
    }
    zms_http_vod_mp4_serve(hs, src, app, stream, req->range, strcmp(req->method, "HEAD") == 0);
}

/** 云录像直出：/record/.../*.mp4 → ./www/record/...（HTTP-MP4 + Range，无需 loadMP4File） */
static int record_file_mp4_route_match(const zms_http_request *req)
{
    size_t n;
    size_t i;

    if (!req || !req->path || strncmp(req->path, "/record/", 8) != 0) {
        return 0;
    }
    n = strlen(req->path);
    if (n < 12) {
        return 0;
    }
    for (i = 0; i < 4; ++i) {
        char a = req->path[n - 4 + i];
        char b = ".mp4"[i];
        if (a >= 'A' && a <= 'Z') {
            a = (char)(a - 'A' + 'a');
        }
        if (a != b) {
            return 0;
        }
    }
    return 1;
}

static void record_file_mp4_route_handle(zms_http_session *hs, const zms_http_request *req)
{
    char disk[512];
    size_t n;

    if (!hs || !req || !req->path) {
        return;
    }
    if (strcmp(req->method, "GET") != 0 && strcmp(req->method, "HEAD") != 0) {
        zms_http_response_send_error(hs, 405, "Method Not Allowed");
        return;
    }
    if (strstr(req->path, "..") != NULL) {
        zms_http_response_send_error(hs, 400, "Bad Request");
        return;
    }
    n = (size_t)snprintf(disk, sizeof(disk), "./www%s", req->path);
    if (n == 0 || n >= sizeof(disk)) {
        zms_http_response_send_error(hs, 400, "Bad Request");
        return;
    }
    if (!zms_mp4_recorder_path_under_root(disk)) {
        zms_http_response_send_error(hs, 403, "Forbidden");
        return;
    }
    if (hs->state == ZMS_HTTP_SESSION_STATE_STREAMING ||
        hs->state == ZMS_HTTP_SESSION_STATE_FILE_SENDING ||
        hs->state == ZMS_HTTP_SESSION_STATE_HLS_SENDING) {
        zms_http_session_stop_stream(hs);
    }
    ztk_info("HTTP-MP4 record file: %s%s", disk, strcmp(req->method, "HEAD") == 0 ? " (HEAD)" : "");
    zms_http_response_send_vod_mp4_file(hs, NULL, disk, req->range, strcmp(req->method, "HEAD") == 0);
}

static const zms_http_route_ops k_vod_hls_route = {
    .name = "vod-hls",
    .match = vod_hls_route_match,
    .handle = vod_hls_route_handle,
};

static const zms_http_route_ops k_vod_mp4_route = {
    .name = "vod-mp4",
    .match = vod_mp4_route_match,
    .handle = vod_mp4_route_handle,
};

static const zms_http_route_ops k_record_file_mp4_route = {
    .name = "record-file-mp4",
    .match = record_file_mp4_route_match,
    .handle = record_file_mp4_route_handle,
};

static int vod_flv_route_match(const zms_http_request *req)
{
    zms_media_source *src;
    char app[ZMS_APP_MAX];
    char stream[ZMS_STREAM_MAX];
    uint64_t seek_ms = 0;

    if (!req) {
        return 0;
    }
    zms_http_route_parse_flv_path(req->path, app, stream, &seek_ms);
    if (!app[0] || !stream[0]) {
        return 0;
    }
    src = zms_media_source_find_for_play(ZMS_SCHEMA_RTMP, app, stream);
    return src && zms_media_source_is_vod(src);
}

static void vod_flv_route_handle(zms_http_session *hs, const zms_http_request *req)
{
    zms_media_source *src;
    char app[ZMS_APP_MAX];
    char stream[ZMS_STREAM_MAX];
    uint64_t seek_ms = 0;

    if (strcmp(req->method, "GET") != 0) {
        zms_http_response_send_error(hs, 405, "Method Not Allowed");
        return;
    }
    zms_http_route_parse_flv_path(req->path, app, stream, &seek_ms);
    if (!app[0] || !stream[0]) {
        zms_http_response_send_error(hs, 400, "Bad Request");
        return;
    }
    src = zms_media_source_find_for_play(ZMS_SCHEMA_RTMP, app, stream);
    if (!src || !zms_media_source_is_vod(src)) {
        zms_http_response_send_error(hs, 404, "Not Found");
        return;
    }
    if (hs->state == ZMS_HTTP_SESSION_STATE_STREAMING ||
        hs->state == ZMS_HTTP_SESSION_STATE_FILE_SENDING ||
        hs->state == ZMS_HTTP_SESSION_STATE_HLS_SENDING) {
        zms_http_session_stop_stream(hs);
    }
    zms_http_vod_flv_start(hs, src, app, stream, seek_ms, req->range);
}

static const zms_http_route_ops k_vod_flv_route = {
    .name = "vod-flv",
    .match = vod_flv_route_match,
    .handle = vod_flv_route_handle,
};

void zms_vod_http_routes_register(void)
{
    zms_http_route_register(&k_vod_hls_route);
    zms_http_route_register(&k_record_file_mp4_route);
    zms_http_route_register(&k_vod_mp4_route);
    zms_http_route_register(&k_vod_flv_route);
}
