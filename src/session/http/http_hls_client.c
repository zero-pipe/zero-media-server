#include "zms/session/http/http_hls_client.h"
#include "zms/ops/service/zms_have_tls.h"
#include "zms/media/codec/codec_id.h"
#include "zms/engine/media_clock.h"
#include "mpeg-ts.h"
#include "mpeg-types.h"
#include "zms/session/http/http_parser.h"
#include "zms/session/http/http_message_framer.h"
#include "ztk/net/tcp_client.h"
#include "ztk/net/tls_client.h"
#include "ztk/util/log.h"
#include "ztk/util/timer.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HLS_SEG_MAX 128
#define HLS_BODY_MAX (32U * 1024U * 1024U)
#define HLS_URL_MAX 2048

typedef struct zms_http_hls_url {
    char host[256];
    uint16_t port;
    char path[ZMS_HTTP_URL_MAX];
    int use_tls;
} zms_http_hls_url;

typedef struct zms_http_hls_segment {
    char uri[HLS_URL_MAX];
    double duration;
} zms_http_hls_segment;

typedef enum {
    HLS_REQ_M3U8 = 0,
    HLS_REQ_TS,
} hls_req_kind;

typedef enum {
    BODY_CL = 0,
    BODY_CHUNKED,
    BODY_UNTIL_CLOSE,
} http_body_mode;

typedef struct zms_http_hls_chunk_dec {
    int in_chunk;
    size_t chunk_left;
    char line[32];
    size_t line_len;
} zms_http_hls_chunk_dec;

struct zms_http_hls_client {
    zms_http_hls_client_opts opts;
    zms_http_hls_url base_url;
    char playlist_url[HLS_URL_MAX];
    char fetch_url[HLS_URL_MAX];
    hls_req_kind req_kind;
    ztk_tcp_client *tcp;
    ztk_tls_client *tls;
    zms_http_message_framer *http;
    zms_http_parser http_hdr;
    http_body_mode body_mode;
    zms_http_hls_chunk_dec chunk;
    uint8_t *body;
    size_t body_len;
    size_t body_cap;
    zms_http_hls_segment segs[HLS_SEG_MAX];
    size_t seg_count;
    size_t seg_pos;
    int live;
    int is_master_pending;
    double target_duration;
    char last_seg_uri[HLS_URL_MAX];
    struct ts_demuxer_t *ts_demuxer;
    ztk_poller_timer *refresh_timer;
    size_t expect_body;
    int stopping;
    int ready_sent;
    int http_active;
    int pending_soft_retry;
    int connect_retries;
    char req_buf[4096];
};

static void on_http_done(zms_http_hls_client *c, ztk_err_t err);
static void start_next_segment(zms_http_hls_client *c);
static uint64_t refresh_task(void *user);
static void hls_soft_retry(zms_http_hls_client *c, uint64_t delay_ms);
static void client_close_http(zms_http_hls_client *c);

static zms_codec_id codecid_to_ZMS(int codecid)
{
    return zms_codec_from_mpeg_psi(codecid);
}

static void client_fail(zms_http_hls_client *c, ztk_err_t err)
{
    if (!c || c->stopping) {
        return;
    }
    client_close_http(c);
    ztk_warn("hls_pull fatal err=%d kind=%d url=%s body=%u", (int)err, (int)c->req_kind,
             c->fetch_url, (unsigned)c->body_len);
    if (c->opts.on_error) {
        c->opts.on_error(err, c->opts.user);
    }
}

static void cancel_refresh(zms_http_hls_client *c)
{
    if (c && c->refresh_timer) {
        ztk_poller_timer_cancel(c->refresh_timer);
        c->refresh_timer = NULL;
    }
}

static void client_close_http(zms_http_hls_client *c)
{
    if (!c) {
        return;
    }
    c->http_active = 0;
#if ZMS_HAVE_PULL_TLS
    if (c->tls) {
        ztk_tls_client_close(c->tls);
        return;
    }
#endif
    if (c->tcp) {
        ztk_tcp_client_close(c->tcp);
    }
    if (c->http) {
        zms_http_message_framer_reset(c->http);
    }
}

static void body_reset(zms_http_hls_client *c)
{
    c->body_len = 0;
    memset(&c->chunk, 0, sizeof(c->chunk));
}

static ztk_err_t body_append(zms_http_hls_client *c, const void *data, size_t len)
{
    if (!c || !data || len == 0) {
        return ZTK_OK;
    }
    if (c->body_len + len > HLS_BODY_MAX) {
        return ZTK_ERR_BUFFER_TOO_SMALL;
    }
    if (c->body_len + len > c->body_cap) {
        size_t cap = c->body_cap ? c->body_cap : 65536;
        while (cap < c->body_len + len) {
            cap *= 2;
        }
        uint8_t *p = (uint8_t *)realloc(c->body, cap);
        if (!p) {
            return ZTK_ERR_NOMEM;
        }
        c->body = p;
        c->body_cap = cap;
    }
    memcpy(c->body + c->body_len, data, len);
    c->body_len += len;
    return ZTK_OK;
}

static ztk_err_t parse_http_url(const char *url, zms_http_hls_url *out)
{
    if (!url || !out) {
        return ZTK_ERR_INVALID;
    }
    memset(out, 0, sizeof(*out));
    const char *p = url;
    if (strncmp(p, "https://", 8) == 0) {
        out->use_tls = 1;
        out->port = 443;
        p += 8;
    } else if (strncmp(p, "http://", 7) == 0) {
        out->port = 80;
        p += 7;
    } else {
        return ZTK_ERR_INVALID;
    }
    const char *slash = strchr(p, '/');
    const char *host_end = slash ? slash : p + strlen(p);
    const char *colon = NULL;
    for (const char *c = p; c < host_end; ++c) {
        if (*c == ':') {
            colon = c;
            break;
        }
    }
    size_t host_len = colon ? (size_t)(colon - p) : (size_t)(host_end - p);
    if (host_len == 0 || host_len >= sizeof(out->host)) {
        return ZTK_ERR_INVALID;
    }
    memcpy(out->host, p, host_len);
    out->host[host_len] = '\0';
    if (colon) {
        out->port = (uint16_t)atoi(colon + 1);
        if (out->port == 0) {
            out->port = out->use_tls ? 443 : 80;
        }
    }
    if (slash) {
        strncpy(out->path, slash, sizeof(out->path) - 1);
    } else {
        strncpy(out->path, "/", sizeof(out->path) - 1);
    }
    return ZTK_OK;
}

static size_t hls_url_prefix_len(const char *scheme, const char *host, uint16_t port)
{
    char port_buf[8];
    int pn = snprintf(port_buf, sizeof(port_buf), "%u", (unsigned)port);
    size_t port_len = pn > 0 ? (size_t)pn : 1;

    return strlen(scheme) + 3 + strlen(host) + 1 + port_len;
}

static void resolve_hls_url(const char *base, const char *rel, char *out, size_t cap)
{
    if (!rel || !rel[0] || !out || cap == 0) {
        return;
    }
    if (strncmp(rel, "http://", 7) == 0 || strncmp(rel, "https://", 8) == 0) {
        strncpy(out, rel, cap - 1);
        out[cap - 1] = '\0';
        return;
    }
    char scheme[16] = "http";
    char host[256] = "";
    uint16_t port = 80;
    const char *p = base;
    if (strncmp(p, "https://", 8) == 0) {
        strcpy(scheme, "https");
        port = 443;
        p += 8;
    } else if (strncmp(p, "http://", 7) == 0) {
        p += 7;
    }
    const char *slash = strchr(p, '/');
    const char *host_end = slash ? slash : p + strlen(p);
    const char *colon = strchr(p, ':');
    if (colon && colon < host_end) {
        size_t hl = (size_t)(colon - p);
        if (hl < sizeof(host)) {
            memcpy(host, p, hl);
            host[hl] = '\0';
        }
        port = (uint16_t)atoi(colon + 1);
    } else {
        size_t hl = (size_t)(host_end - p);
        if (hl < sizeof(host)) {
            memcpy(host, p, hl);
            host[hl] = '\0';
        }
    }
    char dir[ZMS_HTTP_URL_MAX];
    if (slash) {
        strncpy(dir, slash, sizeof(dir) - 1);
    } else {
        strcpy(dir, "/");
    }
    char *last = strrchr(dir, '/');
    if (last) {
        last[1] = '\0';
    } else {
        strcat(dir, "/");
    }

    {
        size_t prefix = hls_url_prefix_len(scheme, host, port);
        size_t path_max = cap > prefix ? cap - prefix : 0;

        if (rel[0] == '/') {
            size_t rel_len = strlen(rel);
            if (rel_len > path_max) {
                rel_len = path_max;
            }
            snprintf(out, cap, "%s://%s:%u%.*s", scheme, host, (unsigned)port, (int)rel_len, rel);
        } else {
            char path[ZMS_HTTP_URL_MAX];
            size_t path_len;

            snprintf(path, sizeof(path), "%s%s", dir, rel);
            path_len = strlen(path);
            if (path_len > path_max) {
                path_len = path_max;
            }
            snprintf(out, cap, "%s://%s:%u%.*s", scheme, host, (unsigned)port, (int)path_len, path);
        }
    }
}

static char *trim_line(char *s)
{
    while (*s && (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')) {
        ++s;
    }
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) {
        s[--n] = '\0';
    }
    return s;
}

static int uri_is_ts(const char *uri)
{
    if (!uri) {
        return 0;
    }
    size_t n = strlen(uri);
#ifdef _WIN32
    if (n >= 3 && _stricmp(uri + n - 3, ".ts") == 0)
#else
    if (n >= 3 && strcasecmp(uri + n - 3, ".ts") == 0)
#endif
        return 1;
    if (strstr(uri, ".m3u8") || strstr(uri, ".m4s") || strstr(uri, ".mp4")) {
        return 0;
    }
    return 1;
}

static ztk_err_t parse_m3u8(zms_http_hls_client *c, const char *text, size_t len)
{
    if (!c || !text) {
        return ZTK_ERR_INVALID;
    }
    c->seg_count = 0;
    c->live = 1;
    c->target_duration = 2.0;
    double pending_dur = 0;
    int wait_uri = 0;
    int saw_stream_inf = 0;

    char *buf = (char *)malloc(len + 1);
    if (!buf) {
        return ZTK_ERR_NOMEM;
    }
    memcpy(buf, text, len);
    buf[len] = '\0';

    for (char *line = buf, *next = NULL; line; line = next) {
        next = strchr(line, '\n');
        if (next) {
            *next++ = '\0';
        }
        line = trim_line(line);
        if (!line[0]) {
            continue;
        }
        if (line[0] != '#') {
            if (saw_stream_inf) {
                resolve_hls_url(c->playlist_url, line, c->fetch_url, sizeof(c->fetch_url));
                saw_stream_inf = 0;
                c->is_master_pending = 1;
                free(buf);
                return ZTK_OK;
            }
            if (wait_uri && c->seg_count < HLS_SEG_MAX && uri_is_ts(line)) {
                zms_http_hls_segment *seg = &c->segs[c->seg_count++];
                resolve_hls_url(c->playlist_url, line, seg->uri, sizeof(seg->uri));
                seg->duration = pending_dur > 0 ? pending_dur : c->target_duration;
                wait_uri = 0;
                pending_dur = 0;
            }
            continue;
        }
        if (strncmp(line, "#EXT-X-ENDLIST", 14) == 0) {
            c->live = 0;
        } else if (strncmp(line, "#EXT-X-TARGETDURATION:", 22) == 0) {
            c->target_duration = atof(line + 22);
        } else if (strncmp(line, "#EXTINF:", 8) == 0) {
            pending_dur = atof(line + 8);
            wait_uri = 1;
        } else if (strncmp(line, "#EXT-X-STREAM-INF:", 18) == 0) {
            saw_stream_inf = 1;
        }
    }
    free(buf);
    return ZTK_OK;
}

static int on_ts_packet(void *param, int program, int stream, int codecid, int flags,
                        int64_t pts_90k, int64_t dts_90k, const void *data, size_t bytes)
{
    (void)program;
    (void)stream;
    zms_http_hls_client *c = (zms_http_hls_client *)param;
    if (!c || c->stopping || !data || bytes == 0) {
        return 0;
    }

    zms_codec_id codec = codecid_to_ZMS(codecid);
    if (codec == ZMS_CODEC_INVALID) {
        return 0;
    }

    if (!c->ready_sent && c->opts.on_ready) {
        c->ready_sent = 1;
        c->opts.on_ready(c->opts.user);
    }

    if (!c->opts.on_frame) {
        return 0;
    }

    zms_frame frame;
    zms_frame_init(&frame);
    frame.codec = codec;
    frame.track =
        (codec == ZMS_CODEC_AAC || codec == ZMS_CODEC_OPUS) ? ZMS_TRACK_AUDIO : ZMS_TRACK_VIDEO;
    frame.data = (uint8_t *)data;
    frame.size = bytes;
    frame.dts_ms = zms_mpegts_90k_to_ms(dts_90k);
    frame.pts_ms = zms_mpegts_90k_to_ms(pts_90k);
    frame.keyframe = (flags & MPEG_FLAG_IDR_FRAME) ? 1 : 0;
    frame.owned = 0;
    c->opts.on_frame(&frame, c->opts.user);
    return 0;
}

static ztk_err_t demux_ts_segment(zms_http_hls_client *c)
{
    if (!c->ts_demuxer || !c->body) {
        return ZTK_ERR_INVALID;
    }
    if (c->body_len < 188) {
        ztk_warn("hls_pull ts too short len=%u url=%s", (unsigned)c->body_len, c->fetch_url);
        return ZTK_OK;
    }
    for (size_t i = 0; i + 188 <= c->body_len; i += 188) {
        if (c->body[i] != 0x47) {
            continue;
        }
        (void)ts_demuxer_input(c->ts_demuxer, c->body + i, 188);
    }
    (void)ts_demuxer_flush(c->ts_demuxer);
    return ZTK_OK;
}

static uint64_t refresh_task(void *user)
{
    zms_http_hls_client *c = (zms_http_hls_client *)user;
    c->refresh_timer = NULL;
    if (!c || c->stopping || !c->live) {
        return 0;
    }
    ztk_info("hls_pull refresh m3u8 %s", c->playlist_url);
    c->req_kind = HLS_REQ_M3U8;
    strncpy(c->fetch_url, c->playlist_url, sizeof(c->fetch_url) - 1);
    body_reset(c);
    client_close_http(c);
    start_next_segment(c);
    return 0;
}

static void hls_soft_retry(zms_http_hls_client *c, uint64_t delay_ms)
{
    if (!c || c->stopping) {
        return;
    }
    if (!c->live) {
        c->live = 1;
    }
    cancel_refresh(c);
    client_close_http(c);
    c->http_active = 0;
    body_reset(c);
    if (delay_ms < 500) {
        delay_ms = 500;
    }
    c->refresh_timer = ztk_poller_do_delay(c->opts.poller, delay_ms, refresh_task, c);
}

static void schedule_refresh(zms_http_hls_client *c)
{
    if (!c || !c->live || c->stopping) {
        return;
    }
    cancel_refresh(c);
    uint64_t ms = (uint64_t)(c->target_duration * 500.0);
    if (ms < 1000) {
        ms = 1000;
    }
    if (ms > 10000) {
        ms = 10000;
    }
    c->refresh_timer = ztk_poller_do_delay(c->opts.poller, ms, refresh_task, c);
}

static void on_http_done(zms_http_hls_client *c, ztk_err_t err)
{
    client_close_http(c);
    c->http_active = 0;
    if (err != ZTK_OK) {
        client_fail(c, err);
        return;
    }

    if (c->pending_soft_retry) {
        c->pending_soft_retry = 0;
        hls_soft_retry(c, 1000);
        return;
    }

    if (c->req_kind == HLS_REQ_M3U8) {
        if (parse_m3u8(c, (const char *)c->body, c->body_len) != ZTK_OK) {
            if (c->live) {
                ztk_warn("hls_pull m3u8 parse failed, retry %s", c->playlist_url);
                hls_soft_retry(c, 1000);
                return;
            }
            client_fail(c, ZTK_ERR_INVALID);
            return;
        }
        if (c->is_master_pending) {
            c->is_master_pending = 0;
            strncpy(c->playlist_url, c->fetch_url, sizeof(c->playlist_url) - 1);
            if (parse_http_url(c->fetch_url, &c->base_url) != ZTK_OK) {
                client_fail(c, ZTK_ERR_INVALID);
                return;
            }
            c->req_kind = HLS_REQ_M3U8;
            body_reset(c);
            start_next_segment(c);
            return;
        }
        ztk_info("hls_pull m3u8 parsed: segs=%u live=%d body=%u url=%s", (unsigned)c->seg_count,
                 c->live, (unsigned)c->body_len, c->playlist_url);
        if (c->seg_count == 0) {
            if (c->live) {
                schedule_refresh(c);
                return;
            }
            client_fail(c, ZTK_ERR_INVALID);
            return;
        }
        if (c->seg_pos >= c->seg_count) {
            c->seg_pos = c->live ? (c->seg_count > 3 ? c->seg_count - 3 : 0) : 0;
        }
        c->req_kind = HLS_REQ_TS;
        start_next_segment(c);
        return;
    }

    (void)demux_ts_segment(c);
    ztk_info("hls_pull ts done len=%u pos=%u/%u url=%s", (unsigned)c->body_len,
             (unsigned)c->seg_pos, (unsigned)c->seg_count, c->fetch_url);
    strncpy(c->last_seg_uri, c->fetch_url, sizeof(c->last_seg_uri) - 1);
    ++c->seg_pos;
    start_next_segment(c);
}

static void hls_begin_http_fetch(zms_http_hls_client *c, const zms_http_hls_url *u)
{
#if ZMS_HAVE_PULL_TLS
    if (c->tls) {
        ztk_err_t err = ztk_tls_client_connect(c->tls, u->host, u->port);
        if (err != ZTK_OK) {
            ztk_warn("hls_pull tls connect %s:%u err=%d", u->host, (unsigned)u->port, (int)err);
        }
        if (err != ZTK_OK) {
            hls_soft_retry(c, 1000);
        }
        return;
    }
#endif
    if (!c->tcp) {
        return;
    }
    ztk_err_t err = ztk_tcp_client_connect(c->tcp, u->host, u->port);
    if (err != ZTK_OK) {
        ztk_warn("hls_pull tcp connect %s:%u err=%d (is upstream :8080 running?)", u->host,
                 (unsigned)u->port, (int)err);
        hls_soft_retry(c, 1000);
    }
}

static void start_next_segment(zms_http_hls_client *c)
{
    if (!c || c->stopping) {
        return;
    }

    if (c->req_kind == HLS_REQ_M3U8 || c->seg_count == 0) {
        c->req_kind = HLS_REQ_M3U8;
        strncpy(c->fetch_url, c->playlist_url, sizeof(c->fetch_url) - 1);
        goto do_fetch;
    }

    if (c->seg_pos >= c->seg_count) {
        if (c->live) {
            schedule_refresh(c);
        }
        return;
    }

    {
        const zms_http_hls_segment *seg = &c->segs[c->seg_pos];
        if (strcmp(seg->uri, c->last_seg_uri) == 0) {
            ++c->seg_pos;
            if (c->seg_pos >= c->seg_count) {
                if (c->live) {
                    schedule_refresh(c);
                }
                return;
            }
            seg = &c->segs[c->seg_pos];
        }
        c->req_kind = HLS_REQ_TS;
        strncpy(c->fetch_url, seg->uri, sizeof(c->fetch_url) - 1);
    }

do_fetch: {
    zms_http_hls_url u;
    if (parse_http_url(c->fetch_url, &u) != ZTK_OK) {
        client_fail(c, ZTK_ERR_INVALID);
        return;
    }
    client_close_http(c);
    body_reset(c);
    hls_begin_http_fetch(c, &u);
}
}

static void send_http_get(zms_http_hls_client *c, const zms_http_hls_url *u)
{
    char host_line[320];
    if ((!u->use_tls && u->port != 80) || (u->use_tls && u->port != 443)) {
        snprintf(host_line, sizeof(host_line), "%s:%u", u->host, (unsigned)u->port);
    } else {
        snprintf(host_line, sizeof(host_line), "%s", u->host);
    }

    int n = snprintf(c->req_buf, sizeof(c->req_buf),
                     "GET %s HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "Connection: close\r\n"
                     "User-Agent: ZMS-HlsPull/1.0\r\n"
                     "Accept: */*\r\n"
                     "\r\n",
                     u->path[0] ? u->path : "/", host_line);
    if (n > 0 && (size_t)n < sizeof(c->req_buf)) {
#if ZMS_HAVE_PULL_TLS
        if (c->tls) {
            ztk_tls_client_send(c->tls, c->req_buf, (size_t)n);
            return;
        }
#endif
        if (c->tcp) {
            ztk_tcp_client_send(c->tcp, c->req_buf, (size_t)n);
        }
    }
}

static int str_icontains(const char *hay, const char *needle)
{
    if (!hay || !needle) {
        return 0;
    }
    size_t nlen = strlen(needle);
    for (const char *p = hay; *p; ++p) {
        size_t i = 0;
        while (i < nlen && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
            ++i;
        }
        if (i == nlen) {
            return 1;
        }
    }
    return 0;
}

static int chunk_line_complete(zms_http_hls_chunk_dec *d, size_t *out_size)
{
    for (size_t i = 0; i < d->line_len; ++i) {
        if (d->line[i] == '\r' || d->line[i] == '\n') {
            d->line[i] = '\0';
            *out_size = (size_t)strtoul(d->line, NULL, 16);
            return 1;
        }
    }
    return 0;
}

static intptr_t on_http_header(const char *header, size_t header_len, void *user)
{
    zms_http_hls_client *c = (zms_http_hls_client *)user;
    if (!c) {
        return 0;
    }

    zms_http_parser_clear(&c->http_hdr);
    if (zms_http_parser_parse_header(&c->http_hdr, header, header_len) != ZTK_OK) {
        return 0;
    }
    if (!c->http_hdr.is_response) {
        ztk_warn("hls_pull not an HTTP response (first=%s)", c->http_hdr.method);
        client_fail(c, ZTK_ERR_INVALID);
        return 0;
    }

    int status = atoi(c->http_hdr.url);
    if (status != 200 && status != 206) {
        if (c->req_kind == HLS_REQ_M3U8) {
            ztk_warn("hls_pull m3u8 HTTP %d (wait upstream HLS) %s", status, c->fetch_url);
            c->pending_soft_retry = 1;
        } else {
            ztk_warn("hls_pull segment HTTP %d %s, refresh playlist", status, c->fetch_url);
            c->pending_soft_retry = 1;
            c->req_kind = HLS_REQ_M3U8;
            strncpy(c->fetch_url, c->playlist_url, sizeof(c->fetch_url) - 1);
        }
    }

    const char *te = zms_http_parser_header(&c->http_hdr, "Transfer-Encoding");
    const char *cl = zms_http_parser_header(&c->http_hdr, "Content-Length");
    memset(&c->chunk, 0, sizeof(c->chunk));
    c->expect_body = 0;

    if (te[0] && str_icontains(te, "chunked")) {
        c->body_mode = BODY_CHUNKED;
        return -1;
    }
    if (cl[0]) {
        unsigned long long n = 0;
        for (const char *p = cl; *p; ++p) {
            if (*p < '0' || *p > '9') {
                break;
            }
            n = n * 10ULL + (unsigned long long)(*p - '0');
        }
        if (n > 0 && n <= HLS_BODY_MAX) {
            c->body_mode = BODY_CL;
            c->expect_body = (size_t)n;
            return (intptr_t)n;
        }
    }
    c->body_mode = BODY_UNTIL_CLOSE;
    return -1;
}

static void on_http_content(const char *data, size_t len, void *user)
{
    zms_http_hls_client *c = (zms_http_hls_client *)user;
    if (!c || c->stopping) {
        return;
    }
    if (c->body_mode == BODY_CHUNKED) {
        size_t off = 0;
        while (off < len) {
            if (!c->chunk.in_chunk) {
                while (off < len) {
                    if (c->chunk.line_len + 1 >= sizeof(c->chunk.line)) {
                        client_fail(c, ZTK_ERR_INVALID);
                        return;
                    }
                    c->chunk.line[c->chunk.line_len++] = (char)data[off++];
                    size_t sz = 0;
                    if (!chunk_line_complete(&c->chunk, &sz)) {
                        continue;
                    }
                    c->chunk.line_len = 0;
                    if (sz == 0) {
                        on_http_done(c, ZTK_OK);
                        return;
                    }
                    c->chunk.chunk_left = sz;
                    c->chunk.in_chunk = 1;
                    break;
                }
                continue;
            }
            size_t take = c->chunk.chunk_left;
            if (take > len - off) {
                take = len - off;
            }
            if (body_append(c, (const uint8_t *)data + off, take) != ZTK_OK) {
                client_fail(c, ZTK_ERR_IO);
                return;
            }
            off += take;
            c->chunk.chunk_left -= take;
            if (c->chunk.chunk_left == 0) {
                c->chunk.in_chunk = 0;
            }
        }
        return;
    }
    if (body_append(c, data, len) != ZTK_OK) {
        client_fail(c, ZTK_ERR_IO);
        return;
    }
    if (c->body_mode == BODY_CL && c->expect_body > 0 && c->body_len >= c->expect_body) {
        on_http_done(c, ZTK_OK);
    }
}

static void on_transport_data(zms_http_hls_client *c, const void *data, size_t len)
{
    if (!c->http || !c->http_active) {
        return;
    }
    ztk_err_t err = zms_http_message_framer_input(c->http, data, len);
    if (err != ZTK_OK) {
        ztk_warn("hls_pull http split err=%d url=%s", (int)err, c->fetch_url);
        hls_soft_retry(c, 1000);
    }
}

static void on_io_connected(zms_http_hls_client *c)
{
    zms_http_hls_url u;
    if (parse_http_url(c->fetch_url, &u) != ZTK_OK) {
        client_fail(c, ZTK_ERR_INVALID);
        return;
    }
    c->http_active = 1;
    c->connect_retries = 0;
    zms_http_message_framer_reset(c->http);
    body_reset(c);
    ztk_info("hls_pull connected, GET %s", c->fetch_url);
    send_http_get(c, &u);
}

static void on_io_closed(zms_http_hls_client *c)
{
    if (!c->http_active) {
        return;
    }
    c->http_active = 0;
    if (c->body_mode == BODY_CL && c->expect_body > 0 && c->body_len < c->expect_body) {
        ztk_warn("hls_pull short body %u/%u url=%s", (unsigned)c->body_len,
                 (unsigned)c->expect_body, c->fetch_url);
        c->pending_soft_retry = 1;
    }
    on_http_done(c, ZTK_OK);
}

static void on_tcp_connect(ztk_tcp_client *tcp, void *user)
{
    (void)tcp;
    on_io_connected((zms_http_hls_client *)user);
}

static void on_tcp_recv(ztk_tcp_client *tcp, const void *data, size_t len, void *user)
{
    (void)tcp;
    on_transport_data((zms_http_hls_client *)user, data, len);
}

static void on_tcp_error(ztk_tcp_client *tcp, void *user)
{
    (void)tcp;
    zms_http_hls_client *c = (zms_http_hls_client *)user;
    if (!c || c->stopping) {
        return;
    }
    if (c->http_active && c->body_len > 0) {
        on_io_closed(c);
        return;
    }
    c->http_active = 0;
    ++c->connect_retries;
    if (c->connect_retries <= 3 || (c->connect_retries % 10) == 0) {
        ztk_warn("hls_pull tcp closed/err (retry %d) ready=%d url=%s", c->connect_retries,
                 c->ready_sent, c->fetch_url);
    }
    hls_soft_retry(c, c->ready_sent ? 500 : 1000);
}

static void on_tls_connect(ztk_tls_client *tls, void *user)
{
    (void)tls;
    on_io_connected((zms_http_hls_client *)user);
}

static void on_tls_recv(ztk_tls_client *tls, const void *data, size_t len, void *user)
{
    (void)tls;
    on_transport_data((zms_http_hls_client *)user, data, len);
}

static void on_tls_error(ztk_tls_client *tls, void *user)
{
    (void)tls;
    on_tcp_error(NULL, user);
}

zms_http_hls_client *zms_http_hls_client_create(const zms_http_hls_client_opts *opts)
{
    if (!opts || !opts->poller || !opts->url || !opts->on_frame) {
        return NULL;
    }

    zms_http_hls_client *c = (zms_http_hls_client *)calloc(1, sizeof(*c));
    if (!c) {
        return NULL;
    }
    c->opts = *opts;
    if (parse_http_url(opts->url, &c->base_url) != ZTK_OK) {
        free(c);
        return NULL;
    }
    strncpy(c->playlist_url, opts->url, sizeof(c->playlist_url) - 1);

    c->ts_demuxer = ts_demuxer_create(on_ts_packet, c);
    if (!c->ts_demuxer) {
        free(c);
        return NULL;
    }

    zms_http_message_framer_opts sopts = {on_http_header, on_http_content, c, 16U * 1024U * 1024U,
                                          NULL};
    c->http = zms_http_message_framer_create(&sopts);
    if (!c->http) {
        ts_demuxer_destroy(c->ts_demuxer);
        free(c);
        return NULL;
    }

    if (c->base_url.use_tls) {
#if !defined(ZTK_HAVE_OPENSSL) || !ZTK_HAVE_OPENSSL
        zms_http_hls_client_destroy(c);
        return NULL;
#else
        if (!opts->ssl_ctx) {
            zms_http_hls_client_destroy(c);
            return NULL;
        }
        ztk_tls_client_ops_t tops = {on_tls_connect, on_tls_recv, on_tls_error};
        ztk_tls_client_opts_t topts = {opts->poller, opts->ssl_ctx, &tops, c, c->base_url.host};
        c->tls = ztk_tls_client_create(&topts);
        if (!c->tls) {
            zms_http_hls_client_destroy(c);
            return NULL;
        }
#endif
    } else {
        ztk_tcp_client_ops_t tops = {on_tcp_connect, on_tcp_recv, on_tcp_error};
        ztk_tcp_client_opts_t topts = {opts->poller, &tops, c};
        c->tcp = ztk_tcp_client_create(&topts);
        if (!c->tcp) {
            zms_http_hls_client_destroy(c);
            return NULL;
        }
    }
    return c;
}

void zms_http_hls_client_destroy(zms_http_hls_client *c)
{
    if (!c) {
        return;
    }
    zms_http_hls_client_stop(c);
    ztk_tcp_client_destroy(c->tcp);
#if ZMS_HAVE_PULL_TLS
    ztk_tls_client_destroy(c->tls);
#endif
    zms_http_message_framer_destroy(c->http);
    ts_demuxer_destroy(c->ts_demuxer);
    free(c->body);
    zms_http_parser_clear(&c->http_hdr);
    free(c);
}

ztk_err_t zms_http_hls_client_play(zms_http_hls_client *c)
{
    if (!c) {
        return ZTK_ERR_INVALID;
    }
    c->stopping = 0;
    c->ready_sent = 0;
    c->seg_pos = 0;
    c->last_seg_uri[0] = '\0';
    c->req_kind = HLS_REQ_M3U8;
    strncpy(c->fetch_url, c->playlist_url, sizeof(c->fetch_url) - 1);
    body_reset(c);
    c->http_active = 0;
    c->pending_soft_retry = 0;
    c->connect_retries = 0;
    /* 拉流代理按直出 HLS 处理；live 须在首次 m3u8 解析前为 1，否则 503 软重试会把 refresh_task 丢弃 */
    c->live = 1;
    ztk_info("hls_pull play %s", c->playlist_url);
    start_next_segment(c);
    return ZTK_OK;
}

void zms_http_hls_client_stop(zms_http_hls_client *c)
{
    if (!c) {
        return;
    }
    c->stopping = 1;
    cancel_refresh(c);
    client_close_http(c);
    c->http_active = 0;
    body_reset(c);
    c->ready_sent = 0;
    c->pending_soft_retry = 0;
    c->connect_retries = 0;
}
