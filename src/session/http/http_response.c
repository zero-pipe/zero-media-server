#include "session/http/http_session_internal.h"
#include "ztk/platform.h"
#include "zms/ops/api/webhook/webhook_client.h"
#include "zms/engine/media_event.h"
#include "zms/vod/io/vod_thread_pool.h"
#include "zms/vod/io/vod_source.h"
#include "zms/vod/vod_flv_index.h"
#include "ztk/poller/poller.h"
#include "ztk/util/log.h"
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <sys/stat.h>
#define zms_http_fseek _fseeki64
#else
#include <strings.h>
#include <sys/stat.h>
#define zms_http_fseek fseeko
#endif

void zms_http_response_send_error(zms_http_session *hs, int code, const char *reason)
{
    char hdr[256];
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 %d %s\r\n"
                     "Connection: close\r\n"
                     "Access-Control-Allow-Origin: *\r\n"
                     "Content-Length: 0\r\n\r\n",
                     code, reason);
    ztk_tcp_session_send(hs->tcp, hdr, (size_t)n);
    ztk_tcp_session_flush(hs->tcp);
    hs->state = ZMS_HTTP_SESSION_STATE_IDLE;
}

void zms_http_response_send_json(zms_http_session *hs, int status, const char *body,
                                 size_t body_len)
{
    char hdr[256];
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 %d %s\r\n"
                     "Connection: close\r\n"
                     "Content-Type: application/json; charset=utf-8\r\n"
                     "Access-Control-Allow-Origin: *\r\n"
                     "Content-Length: %zu\r\n\r\n",
                     status, status == 200 ? "OK" : "Error", body_len);
    ztk_tcp_session_send(hs->tcp, hdr, (size_t)n);
    if (body_len) {
        ztk_tcp_session_send(hs->tcp, body, body_len);
    }
    ztk_tcp_session_flush(hs->tcp);
    hs->state = ZMS_HTTP_SESSION_STATE_IDLE;
}

void zms_http_response_send_bytes(zms_http_session *hs, int status, const char *ctype,
                                  const void *body, size_t body_len)
{
    char hdr[320];
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 %d %s\r\n"
                     "Connection: close\r\n"
                     "Content-Type: %s\r\n"
                     "Access-Control-Allow-Origin: *\r\n"
                     "Cache-Control: no-cache\r\n"
                     "Content-Length: %zu\r\n\r\n",
                     status, status == 200 ? "OK" : "Not Found", ctype, body_len);
    ztk_tcp_session_send(hs->tcp, hdr, (size_t)n);
    if (body_len && body) {
        ztk_tcp_session_send(hs->tcp, body, body_len);
    }
    ztk_tcp_session_flush(hs->tcp);
    hs->state = ZMS_HTTP_SESSION_STATE_IDLE;
}

void zms_http_response_send_bytes_buf(zms_http_session *hs, int status, const char *ctype,
                                      ztk_buf *body)
{
    char hdr[320];
    size_t body_len = body ? ztk_buf_len(body) : 0;
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 %d %s\r\n"
                     "Connection: close\r\n"
                     "Content-Type: %s\r\n"
                     "Access-Control-Allow-Origin: *\r\n"
                     "Cache-Control: no-cache\r\n"
                     "Content-Length: %zu\r\n\r\n",
                     status, status == 200 ? "OK" : "Not Found", ctype, body_len);
    ztk_tcp_session_send(hs->tcp, hdr, (size_t)n);
    if (body_len > 0) {
        ztk_tcp_session_send_buf(hs->tcp, body);
    }
    ztk_tcp_session_flush(hs->tcp);
    hs->state = ZMS_HTTP_SESSION_STATE_IDLE;
}

static int parse_http_range(const char *range, uint64_t file_size, uint64_t *start, uint64_t *end)
{
    const char *p;
    unsigned long long a = 0;
    unsigned long long b = 0;

    if (start) {
        *start = 0;
    }
    if (end) {
        *end = file_size > 0 ? file_size - 1 : 0;
    }
    if (!range || !range[0] || file_size == 0) {
        return 0;
    }
    p = strstr(range, "bytes=");
    if (!p) {
        return 0;
    }
    p += 6;
    if (*p == '-') {
        a = strtoull(p + 1, NULL, 10);
        if (a >= file_size) {
            a = 0;
        } else {
            a = file_size - a;
        }
        b = file_size - 1;
    } else {
        a = strtoull(p, NULL, 10);
        p = strchr(p, '-');
        if (p && p[1]) {
            b = strtoull(p + 1, NULL, 10);
        } else {
            b = file_size - 1;
        }
    }
    if (b >= file_size) {
        b = file_size - 1;
    }
    if (a > b) {
        return 0;
    }
    if (start) {
        *start = (uint64_t)a;
    }
    if (end) {
        *end = (uint64_t)b;
    }
    return 1;
}

static int http_sess_alive(zms_http_session *hs)
{
    return hs && hs->tcp && ztk_tcp_session_user(hs->tcp) == hs;
}

typedef struct zms_http_vod_file_open_ctx {
    zms_http_session *hs;
    zms_media_source *src;
    char path[512];
    char range_hdr[256];
    uint64_t query_seek_ms;
    int head_only;
    int is_mp4;
    int prep_ok;
    int http_code;
    const char *http_reason;
    FILE *fp;
    uint64_t file_size;
    uint64_t start;
    uint64_t end;
    int has_range;
} zms_http_vod_file_open_ctx;

static void http_file_done(zms_http_session *hs);
static void flush_file(zms_http_session *hs);

static void vod_file_open_prep_blocking(void *user)
{
    zms_http_vod_file_open_ctx *ctx = (zms_http_vod_file_open_ctx *)user;
    struct stat st;

    ctx->prep_ok = 0;
    ctx->fp = NULL;
    if (stat(ctx->path, &st) != 0 || st.st_size <= 0) {
        ctx->http_code = 404;
        ctx->http_reason = "Not Found";
        return;
    }
    ctx->file_size = (uint64_t)st.st_size;
    ctx->end = ctx->file_size - 1;
    ctx->start = 0;
    ctx->has_range = 0;

    if (ctx->is_mp4) {
        if (ctx->range_hdr[0]) {
            ctx->has_range =
                parse_http_range(ctx->range_hdr, ctx->file_size, &ctx->start, &ctx->end);
        }
        if (!ctx->head_only) {
            ctx->fp = fopen(ctx->path, "rb");
            if (!ctx->fp) {
                ctx->http_code = 404;
                ctx->http_reason = "Not Found";
                return;
            }
            if (ctx->has_range && zms_http_fseek(ctx->fp, (long long)ctx->start, SEEK_SET) != 0) {
                fclose(ctx->fp);
                ctx->fp = NULL;
                ctx->http_code = 416;
                ctx->http_reason = "Range Not Satisfiable";
                return;
            }
        }
        ctx->prep_ok = 1;
        return;
    }

    if (ctx->file_size < 256) {
        ctx->http_code = 404;
        ctx->http_reason = "Not Found";
        return;
    }

    if (ctx->query_seek_ms > 0 && (!ctx->range_hdr[0])) {
        const zms_vod_flv_index *idx = zms_vod_source_flv_index(ctx->src);
        ctx->start = (uint64_t)zms_vod_flv_index_byte_at_ms(idx, ctx->query_seek_ms);
        if (ctx->start >= ctx->file_size) {
            ctx->start = 0;
        }
        ctx->has_range = 1;
    } else if (ctx->range_hdr[0]) {
        ctx->has_range = parse_http_range(ctx->range_hdr, ctx->file_size, &ctx->start, &ctx->end);
    }

    ctx->fp = fopen(ctx->path, "rb");
    if (!ctx->fp) {
        ctx->http_code = 404;
        ctx->http_reason = "Not Found";
        return;
    }
    if (zms_http_fseek(ctx->fp, (long long)ctx->start, SEEK_SET) != 0) {
        fclose(ctx->fp);
        ctx->fp = NULL;
        ctx->http_code = 416;
        ctx->http_reason = "Range Not Satisfiable";
        return;
    }
    ctx->prep_ok = 1;
}

static void http_send_vod_mp4_finish(zms_http_session *hs, zms_media_source *src, int head_only,
                                     FILE *fp, uint64_t file_size, uint64_t start, uint64_t end,
                                     int has_range)
{
    uint64_t body_len;
    char hdr[512];
    int hn;

    if (src) {
        zms_media_tuple tuple;
        zms_media_tuple_from_source(src, &tuple);
        if (!zms_webhook_allow_play(&tuple, "http-mp4", hs->tcp, NULL)) {
            if (fp) {
                fclose(fp);
            }
            zms_http_response_send_error(hs, 403, "Forbidden");
            return;
        }
        zms_media_source_reader_add(src);
        hs->play_start_ms = ztk_monotonic_ms();
        zms_media_event_play(src, "http-mp4");
        hs->reader_attached = 1;
        hs->play_event = "http-mp4";
        hs->source = src;
    }

    body_len = has_range ? (end - start + 1) : file_size;

    if (has_range) {
        hn = snprintf(hdr, sizeof(hdr),
                      "HTTP/1.1 206 Partial Content\r\n"
                      "Connection: close\r\n"
                      "Content-Type: video/mp4\r\n"
                      "Accept-Ranges: bytes\r\n"
                      "Access-Control-Allow-Origin: *\r\n"
                      "Content-Range: bytes %llu-%llu/%llu\r\n"
                      "Content-Length: %llu\r\n\r\n",
                      (unsigned long long)start, (unsigned long long)end,
                      (unsigned long long)file_size, (unsigned long long)body_len);
    } else {
        hn = snprintf(hdr, sizeof(hdr),
                      "HTTP/1.1 200 OK\r\n"
                      "Connection: close\r\n"
                      "Content-Type: video/mp4\r\n"
                      "Accept-Ranges: bytes\r\n"
                      "Access-Control-Allow-Origin: *\r\n"
                      "Content-Length: %llu\r\n\r\n",
                      (unsigned long long)file_size);
    }
    ztk_tcp_session_send(hs->tcp, hdr, (size_t)hn);
    ztk_tcp_session_flush(hs->tcp);

    if (head_only) {
        http_file_done(hs);
        return;
    }

    hs->file_fp = fp;
    hs->file_remain = body_len;
    hs->state = ZMS_HTTP_SESSION_STATE_FILE_SENDING;
    flush_file(hs);
}

static void http_send_vod_flv_finish(zms_http_session *hs, zms_media_source *src, FILE *fp,
                                     uint64_t file_size, uint64_t start, uint64_t end,
                                     int has_range)
{
    uint64_t body_len = (size_t)(end - start + 1);
    char hdr[512];
    int hn;

    {
        zms_media_tuple tuple;
        zms_media_tuple_from_source(src, &tuple);
        if (!zms_webhook_allow_play(&tuple, "http-flv", hs->tcp, NULL)) {
            fclose(fp);
            zms_http_response_send_error(hs, 403, "Forbidden");
            return;
        }
    }
    zms_media_source_reader_add(src);
    hs->play_start_ms = ztk_monotonic_ms();
    zms_media_event_play(src, "http-flv");
    hs->reader_attached = 1;
    hs->play_event = "http-flv";
    hs->source = src;

    if (has_range) {
        hn = snprintf(hdr, sizeof(hdr),
                      "HTTP/1.1 206 Partial Content\r\n"
                      "Connection: close\r\n"
                      "Content-Type: video/x-flv\r\n"
                      "Accept-Ranges: bytes\r\n"
                      "Access-Control-Allow-Origin: *\r\n"
                      "Content-Range: bytes %llu-%llu/%llu\r\n"
                      "Content-Length: %llu\r\n\r\n",
                      (unsigned long long)start, (unsigned long long)end,
                      (unsigned long long)file_size, (unsigned long long)body_len);
    } else {
        hn = snprintf(hdr, sizeof(hdr),
                      "HTTP/1.1 200 OK\r\n"
                      "Connection: close\r\n"
                      "Content-Type: video/x-flv\r\n"
                      "Accept-Ranges: bytes\r\n"
                      "Access-Control-Allow-Origin: *\r\n"
                      "Content-Length: %llu\r\n\r\n",
                      (unsigned long long)file_size);
        body_len = file_size;
        fseek(fp, 0, SEEK_SET);
    }
    ztk_tcp_session_send(hs->tcp, hdr, (size_t)hn);
    ztk_tcp_session_flush(hs->tcp);
    hs->file_fp = fp;
    hs->file_remain = body_len;
    hs->state = ZMS_HTTP_SESSION_STATE_FILE_SENDING;
    flush_file(hs);
}

static void vod_file_open_prep_finish(zms_http_vod_file_open_ctx *ctx, int free_ctx)
{
    if (!http_sess_alive(ctx->hs)) {
        if (ctx->fp) {
            fclose(ctx->fp);
        }
        if (free_ctx) {
            free(ctx);
        }
        return;
    }
    if (!ctx->prep_ok) {
        if (ctx->fp) {
            fclose(ctx->fp);
        }
        zms_http_response_send_error(ctx->hs, ctx->http_code ? ctx->http_code : 404,
                                     ctx->http_reason ? ctx->http_reason : "Not Found");
        if (free_ctx) {
            free(ctx);
        }
        return;
    }
    if (ctx->is_mp4) {
        http_send_vod_mp4_finish(ctx->hs, ctx->src, ctx->head_only, ctx->fp, ctx->file_size,
                                 ctx->start, ctx->end, ctx->has_range);
    } else {
        http_send_vod_flv_finish(ctx->hs, ctx->src, ctx->fp, ctx->file_size, ctx->start, ctx->end,
                                 ctx->has_range);
    }
    if (free_ctx) {
        free(ctx);
    }
}

static void vod_file_open_prep_on_io(void *user)
{
    vod_file_open_prep_finish((zms_http_vod_file_open_ctx *)user, 1);
}

static int vod_file_open_async(zms_http_session *hs, zms_media_source *src, const char *path,
                               const char *range_hdr, uint64_t query_seek_ms, int head_only,
                               int is_mp4)
{
    ztk_poller *pol;
    zms_http_vod_file_open_ctx *ctx;

    if (!zms_vod_thread_pool_enabled() || !hs || !path || !path[0]) {
        return 0;
    }
    pol = hs->tcp ? ztk_tcp_session_poller(hs->tcp) : NULL;
    if (!pol) {
        return 0;
    }

    ctx = (zms_http_vod_file_open_ctx *)calloc(1, sizeof(*ctx));
    if (!ctx) {
        return 0;
    }
    ctx->hs = hs;
    ctx->src = src;
    strncpy(ctx->path, path, sizeof(ctx->path) - 1);
    if (range_hdr && range_hdr[0]) {
        strncpy(ctx->range_hdr, range_hdr, sizeof(ctx->range_hdr) - 1);
    }
    ctx->query_seek_ms = query_seek_ms;
    ctx->head_only = head_only;
    ctx->is_mp4 = is_mp4;

    if (zms_vod_thread_pool_run(pol, vod_file_open_prep_blocking, vod_file_open_prep_on_io, ctx) !=
        ZTK_OK) {
        free(ctx);
        return 0;
    }
    return 1;
}

typedef struct zms_http_static_file_ctx {
    zms_http_session *hs;
    zms_media_source *src;
    char path[512];
    char ctype[96];
    char player[32];
    int prep_ok;
    FILE *fp;
    uint64_t file_size;
} zms_http_static_file_ctx;

static void http_send_static_finish(zms_http_session *hs, zms_media_source *src, const char *ctype,
                                    const char *player, FILE *fp, uint64_t file_size)
{
    char hdr[512];
    int hn;

    if (src) {
        zms_media_tuple tuple;
        zms_media_tuple_from_source(src, &tuple);
        if (!zms_webhook_allow_play(&tuple, player, hs->tcp, NULL)) {
            fclose(fp);
            zms_http_response_send_error(hs, 403, "Forbidden");
            return;
        }
        zms_media_source_reader_add(src);
        hs->play_start_ms = ztk_monotonic_ms();
        zms_media_event_play(src, player);
        hs->reader_attached = 1;
        hs->play_event = player;
        hs->source = src;
    }
    hn = snprintf(hdr, sizeof(hdr),
                  "HTTP/1.1 200 OK\r\n"
                  "Connection: close\r\n"
                  "Content-Type: %s\r\n"
                  "Access-Control-Allow-Origin: *\r\n"
                  "Cache-Control: no-cache\r\n"
                  "Content-Length: %llu\r\n\r\n",
                  ctype, (unsigned long long)file_size);
    ztk_tcp_session_send(hs->tcp, hdr, (size_t)hn);
    ztk_tcp_session_flush(hs->tcp);
    hs->file_fp = fp;
    hs->file_remain = file_size;
    hs->state = ZMS_HTTP_SESSION_STATE_FILE_SENDING;
    flush_file(hs);
}

static void static_file_prep_blocking(void *user)
{
    zms_http_static_file_ctx *ctx = (zms_http_static_file_ctx *)user;
    struct stat st;

    ctx->prep_ok = 0;
    ctx->fp = NULL;
    if (stat(ctx->path, &st) != 0 || st.st_size <= 0) {
        return;
    }
    ctx->file_size = (uint64_t)st.st_size;
    ctx->fp = fopen(ctx->path, "rb");
    if (!ctx->fp) {
        return;
    }
    ctx->prep_ok = 1;
}

static void static_file_prep_finish(zms_http_static_file_ctx *ctx, int free_ctx)
{
    if (!http_sess_alive(ctx->hs)) {
        if (ctx->fp) {
            fclose(ctx->fp);
        }
        if (free_ctx) {
            free(ctx);
        }
        return;
    }
    if (!ctx->prep_ok) {
        if (ctx->fp) {
            fclose(ctx->fp);
        }
        zms_http_response_send_error(ctx->hs, 404, "Not Found");
        if (free_ctx) {
            free(ctx);
        }
        return;
    }
    http_send_static_finish(ctx->hs, ctx->src, ctx->ctype, ctx->player, ctx->fp, ctx->file_size);
    if (free_ctx) {
        free(ctx);
    }
}

static void static_file_prep_on_io(void *user)
{
    static_file_prep_finish((zms_http_static_file_ctx *)user, 1);
}

static int static_file_open_async(zms_http_session *hs, zms_media_source *src, const char *path,
                                  const char *ctype, const char *player)
{
    ztk_poller *pol;
    zms_http_static_file_ctx *ctx;

    if (!zms_vod_thread_pool_enabled() || !hs || !path || !path[0]) {
        return 0;
    }
    pol = hs->tcp ? ztk_tcp_session_poller(hs->tcp) : NULL;
    if (!pol) {
        return 0;
    }

    ctx = (zms_http_static_file_ctx *)calloc(1, sizeof(*ctx));
    if (!ctx) {
        return 0;
    }
    ctx->hs = hs;
    ctx->src = src;
    strncpy(ctx->path, path, sizeof(ctx->path) - 1);
    if (ctype && ctype[0]) {
        strncpy(ctx->ctype, ctype, sizeof(ctx->ctype) - 1);
    }
    if (player && player[0]) {
        strncpy(ctx->player, player, sizeof(ctx->player) - 1);
    }

    if (zms_vod_thread_pool_run(pol, static_file_prep_blocking, static_file_prep_on_io, ctx) !=
        ZTK_OK) {
        free(ctx);
        return 0;
    }
    return 1;
}

static void http_file_done(zms_http_session *hs)
{
    if (!hs) {
        return;
    }
    if (hs->file_fp) {
        fclose(hs->file_fp);
        hs->file_fp = NULL;
    }
    hs->file_remain = 0;
    if (hs->reader_attached && hs->source) {
        zms_media_source_reader_remove(hs->source);
        zms_media_event_stop(hs->source, hs->play_event ? hs->play_event : "http-flv",
                             hs->play_start_ms);
        hs->reader_attached = 0;
        hs->play_start_ms = 0;
    }
    hs->play_event = NULL;
    hs->source = NULL;
    hs->state = ZMS_HTTP_SESSION_STATE_IDLE;
}

static void flush_file(zms_http_session *hs)
{
    size_t chunk;
    int i;

    if (!hs || hs->state != ZMS_HTTP_SESSION_STATE_FILE_SENDING || !hs->file_fp || !hs->tcp) {
        return;
    }

    for (i = 0; i < 32 && hs->file_remain > 0; ++i) {
        if (ztk_tcp_session_out_pending(hs->tcp) > 256 * 1024) {
            break;
        }
        ztk_tcp_session_flush(hs->tcp);

        chunk = hs->file_remain > hs->send_cap ? hs->send_cap : (size_t)hs->file_remain;
        if (fread(hs->send_buf, 1, chunk, hs->file_fp) != chunk) {
            break;
        }
        ztk_tcp_session_send(hs->tcp, hs->send_buf, chunk);
        hs->file_remain -= chunk;
    }
    ztk_tcp_session_flush(hs->tcp);

    if (hs->file_remain == 0) {
        http_file_done(hs);
    }
}

static void http_hls_send_done(zms_http_session *hs)
{
    if (!hs) {
        return;
    }
    hs->hls_send_len = 0;
    hs->hls_send_off = 0;
    if (hs->tcp) {
        ztk_tcp_session_close(hs->tcp);
    }
    hs->state = ZMS_HTTP_SESSION_STATE_IDLE;
}

static void start_hls_body_send(zms_http_session *hs, const char *ctype, size_t body_len)
{
    char hdr[320];
    int n;

    if (!hs || !hs->tcp || body_len == 0) {
        return;
    }
    n = snprintf(hdr, sizeof(hdr),
                 "HTTP/1.1 200 OK\r\n"
                 "Connection: close\r\n"
                 "Content-Type: %s\r\n"
                 "Access-Control-Allow-Origin: *\r\n"
                 "Cache-Control: no-cache\r\n"
                 "Content-Length: %zu\r\n\r\n",
                 ctype, body_len);
    ztk_tcp_session_send(hs->tcp, hdr, (size_t)n);
    hs->hls_send_off = 0;
    hs->hls_send_len = body_len;
    hs->state = ZMS_HTTP_SESSION_STATE_HLS_SENDING;
    zms_http_session_flush(hs);
}

static void flush_hls(zms_http_session *hs)
{
    size_t chunk;
    size_t remain;
    int i;

    if (!hs || hs->state != ZMS_HTTP_SESSION_STATE_HLS_SENDING || !hs->tcp || !hs->send_buf) {
        return;
    }

    for (i = 0; i < 32; ++i) {
        remain = hs->hls_send_len > hs->hls_send_off ? hs->hls_send_len - hs->hls_send_off : 0;
        if (remain == 0) {
            break;
        }
        if (ztk_tcp_session_out_pending(hs->tcp) > 256 * 1024) {
            break;
        }
        ztk_tcp_session_flush(hs->tcp);

        chunk = remain > 256 * 1024 ? 256 * 1024 : remain;
        ztk_tcp_session_send(hs->tcp, hs->send_buf + hs->hls_send_off, chunk);
        hs->hls_send_off += chunk;
    }
    ztk_tcp_session_flush(hs->tcp);

    if (hs->hls_send_off >= hs->hls_send_len) {
        http_hls_send_done(hs);
    }
}

void zms_http_session_flush(zms_http_session *hs)
{
    if (!hs) {
        return;
    }
    if (hs->state == ZMS_HTTP_SESSION_STATE_STREAMING) {
        zms_http_session_stream_flush(hs);
    } else if (hs->state == ZMS_HTTP_SESSION_STATE_FILE_SENDING) {
        flush_file(hs);
    } else if (hs->state == ZMS_HTTP_SESSION_STATE_HLS_SENDING) {
        flush_hls(hs);
    }
}

void zms_http_response_send_static_file(zms_http_session *hs, zms_media_source *src,
                                        const char *path, const char *ctype, const char *player)
{
    zms_http_static_file_ctx sync;

    if (!hs || !path || !path[0]) {
        return;
    }
    if (static_file_open_async(hs, src, path, ctype, player)) {
        return;
    }

    memset(&sync, 0, sizeof(sync));
    sync.hs = hs;
    sync.src = src;
    strncpy(sync.path, path, sizeof(sync.path) - 1);
    if (ctype && ctype[0]) {
        strncpy(sync.ctype, ctype, sizeof(sync.ctype) - 1);
    } else {
        strncpy(sync.ctype, "application/octet-stream", sizeof(sync.ctype) - 1);
    }
    if (player && player[0]) {
        strncpy(sync.player, player, sizeof(sync.player) - 1);
    }
    static_file_prep_blocking(&sync);
    static_file_prep_finish(&sync, 0);
}

void zms_http_response_send_vod_flv_file(zms_http_session *hs, zms_media_source *src,
                                         const char *flv_path, const char *range_hdr,
                                         uint64_t query_seek_ms)
{
    zms_http_vod_file_open_ctx sync;

    if (!hs || !flv_path || !flv_path[0]) {
        return;
    }
    if (vod_file_open_async(hs, src, flv_path, range_hdr, query_seek_ms, 0, 0)) {
        return;
    }

    memset(&sync, 0, sizeof(sync));
    sync.hs = hs;
    sync.src = src;
    strncpy(sync.path, flv_path, sizeof(sync.path) - 1);
    if (range_hdr && range_hdr[0]) {
        strncpy(sync.range_hdr, range_hdr, sizeof(sync.range_hdr) - 1);
    }
    sync.query_seek_ms = query_seek_ms;
    vod_file_open_prep_blocking(&sync);
    vod_file_open_prep_finish(&sync, 0);
}

void zms_http_response_send_vod_mp4_file(zms_http_session *hs, zms_media_source *src,
                                         const char *mp4_path, const char *range_hdr, int head_only)
{
    zms_http_vod_file_open_ctx sync;

    if (!hs || !mp4_path || !mp4_path[0]) {
        return;
    }
    if (vod_file_open_async(hs, src, mp4_path, range_hdr, 0, head_only, 1)) {
        return;
    }

    memset(&sync, 0, sizeof(sync));
    sync.hs = hs;
    sync.src = src;
    strncpy(sync.path, mp4_path, sizeof(sync.path) - 1);
    if (range_hdr && range_hdr[0]) {
        strncpy(sync.range_hdr, range_hdr, sizeof(sync.range_hdr) - 1);
    }
    sync.head_only = head_only;
    sync.is_mp4 = 1;
    vod_file_open_prep_blocking(&sync);
    vod_file_open_prep_finish(&sync, 0);
}

void zms_http_response_send_hls_body(zms_http_session *hs, const char *ctype, size_t body_len)
{
    start_hls_body_send(hs, ctype, body_len);
}

void zms_http_response_send_download_file(zms_http_session *hs, const char *path)
{
    struct stat st;
    FILE *fp;
    char hdr[768];
    const char *base;
    char fname[256];
    int hn;
    size_t i;

    if (!hs || !path || !path[0]) {
        return;
    }
    if (stat(path, &st) != 0 || st.st_size <= 0) {
        zms_http_response_send_error(hs, 404, "Not Found");
        return;
    }
    fp = fopen(path, "rb");
    if (!fp) {
        zms_http_response_send_error(hs, 404, "Not Found");
        return;
    }
    base = path;
    for (i = 0; path[i]; ++i) {
        if (path[i] == '/' || path[i] == '\\') {
            base = path + i + 1;
        }
    }
    strncpy(fname, base && base[0] ? base : "download.bin", sizeof(fname) - 1);
    fname[sizeof(fname) - 1] = '\0';
    hn = snprintf(hdr, sizeof(hdr),
                  "HTTP/1.1 200 OK\r\n"
                  "Connection: close\r\n"
                  "Content-Type: application/octet-stream\r\n"
                  "Content-Disposition: attachment; filename=\"%s\"\r\n"
                  "Access-Control-Allow-Origin: *\r\n"
                  "Cache-Control: no-cache\r\n"
                  "Content-Length: %llu\r\n\r\n",
                  fname, (unsigned long long)st.st_size);
    ztk_tcp_session_send(hs->tcp, hdr, (size_t)hn);
    ztk_tcp_session_flush(hs->tcp);
    hs->file_fp = fp;
    hs->file_remain = (uint64_t)st.st_size;
    hs->state = ZMS_HTTP_SESSION_STATE_FILE_SENDING;
    flush_file(hs);
}
