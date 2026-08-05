#include "zms/session/http/http_flv_client.h"
#include "zms/ops/service/zms_have_tls.h"
#include "zms/media/container/flv/flv_tag_demuxer.h"
#include "zms/media/container/flv/flv_tag_framer.h"
#include "zms/session/rtmp/rtmp.h"
#include "zms/media/container/flv/flv_tag_probe.h"
#include "zms/session/http/http_parser.h"
#include "zms/session/http/http_message_framer.h"
#include "ztk/net/tcp_client.h"
#include "ztk/net/tls_client.h"
#include "ztk/util/log.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FLV_IO_BUF (256 * 1024)
typedef struct zms_http_flv_url {
    char host[256];
    uint16_t port;
    char path[ZMS_HTTP_URL_MAX];
    int use_tls;
} zms_http_flv_url;

typedef enum {
    BODY_CL = 0,
    BODY_CHUNKED,
    BODY_UNTIL_CLOSE,
} http_body_mode;

typedef struct zms_http_flv_chunk_dec {
    int in_chunk;
    size_t chunk_left;
    char line[32];
    size_t line_len;
} zms_http_flv_chunk_dec;

struct zms_http_flv_client {
    zms_http_flv_client_opts opts;
    zms_http_flv_url url;
    ztk_tcp_client *tcp;
    ztk_tls_client *tls;
    zms_http_message_framer *http;
    zms_flv_tag_demuxer *demux;
    zms_http_parser http_hdr;
    http_body_mode body_mode;
    zms_http_flv_chunk_dec chunk;
    zms_flv_tag_framer *flv_bs;
    int stopping;
    int ready_sent;
    char req_buf[4096];
};

static void client_fail(zms_http_flv_client *c, ztk_err_t err)
{
    if (!c || c->stopping) {
        return;
    }
    if (c->opts.on_error) {
        c->opts.on_error(err, c->opts.user);
    }
}

static void client_send(zms_http_flv_client *c, const void *data, size_t len)
{
    if (!c || !data || !len) {
        return;
    }
#if ZMS_HAVE_PULL_TLS
    if (c->tls) {
        ztk_tls_client_send(c->tls, data, len);
        return;
    }
#endif
    if (c->tcp) {
        ztk_tcp_client_send(c->tcp, data, len);
    }
}

static ztk_err_t client_connect(zms_http_flv_client *c)
{
    if (!c) {
        return ZTK_ERR_INVALID;
    }
#if ZMS_HAVE_PULL_TLS
    if (c->tls) {
        return ztk_tls_client_connect(c->tls, c->url.host, c->url.port);
    }
#endif
    if (c->tcp) {
        return ztk_tcp_client_connect(c->tcp, c->url.host, c->url.port);
    }
    return ZTK_ERR_STATE;
}

static void client_close(zms_http_flv_client *c)
{
    if (!c) {
        return;
    }
#if ZMS_HAVE_PULL_TLS
    if (c->tls) {
        ztk_tls_client_close(c->tls);
        return;
    }
#endif
    if (c->tcp) {
        ztk_tcp_client_close(c->tcp);
    }
}

static ztk_err_t parse_http_flv_url(const char *url, zms_http_flv_url *out)
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

static void send_http_get(zms_http_flv_client *c)
{
    char host_line[320];
    if ((!c->url.use_tls && c->url.port != 80) || (c->url.use_tls && c->url.port != 443)) {
        snprintf(host_line, sizeof(host_line), "%s:%u", c->url.host, (unsigned)c->url.port);
    } else {
        snprintf(host_line, sizeof(host_line), "%s", c->url.host);
    }

    int n = snprintf(c->req_buf, sizeof(c->req_buf),
                     "GET %s HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "Connection: keep-alive\r\n"
                     "User-Agent: ZMS-HttpFlv/1.0\r\n"
                     "Accept: */*\r\n"
                     "\r\n",
                     c->url.path[0] ? c->url.path : "/", host_line);
    if (n > 0 && (size_t)n < sizeof(c->req_buf)) {
        client_send(c, c->req_buf, (size_t)n);
    }
}

static int is_config_flv_body(uint8_t type, const uint8_t *body, size_t len)
{
    if (!body || len < 2) {
        return 1;
    }
    if (type == ZMS_RTMP_MSG_VIDEO) {
        zms_codec_id vc = zms_flv_tag_video_codec(body, len);
        if (vc == ZMS_CODEC_H264 || vc == ZMS_CODEC_H265) {
            zms_flv_tag_packet_kind pt = zms_flv_tag_video_packet_kind(body, len);
            return pt == ZMS_FLV_TAG_PKT_SEQ_HEADER || pt == ZMS_FLV_TAG_PKT_END_OF_SEQ;
        }
    }
    if (type == ZMS_RTMP_MSG_AUDIO) {
        if (zms_flv_tag_audio_codec(body, len) == ZMS_CODEC_AAC) {
            return zms_flv_tag_audio_packet_kind(body, len) == ZMS_FLV_TAG_PKT_SEQ_HEADER;
        }
    }
    return 0;
}

static void maybe_ready(zms_http_flv_client *c, uint8_t type, const uint8_t *body, size_t len)
{
    if (c->ready_sent || !c->opts.on_ready) {
        return;
    }
    if (type != ZMS_RTMP_MSG_AUDIO && type != ZMS_RTMP_MSG_VIDEO) {
        return;
    }
    if (is_config_flv_body(type, body, len)) {
        return;
    }
    c->ready_sent = 1;
    c->opts.on_ready(c->opts.user);
}

static void on_demux_frame(const zms_frame *frame, void *user)
{
    zms_http_flv_client *c = (zms_http_flv_client *)user;
    if (!c || c->stopping || !frame) {
        return;
    }
    if (c->opts.on_frame) {
        c->opts.on_frame(frame, c->opts.user);
    }
}

static void feed_tag(zms_http_flv_client *c, uint8_t type, const uint8_t *body, size_t len,
                     uint32_t ts)
{
    uint8_t msg;

    if (!c || !body || !len) {
        return;
    }
    maybe_ready(c, type, body, len);
    msg = (type == ZMS_RTMP_MSG_VIDEO)   ? ZMS_RTMP_MSG_VIDEO
          : (type == ZMS_RTMP_MSG_AUDIO) ? ZMS_RTMP_MSG_AUDIO
                                         : ZMS_RTMP_MSG_DATA;
    if (c->opts.on_media) {
        c->opts.on_media(msg, ts, body, len, c->opts.user);
        return;
    }
    if (c->demux) {
        (void)zms_flv_tag_demuxer_input(c->demux, msg, body, len, ts);
    }
}

static void flv_reset(zms_http_flv_client *c)
{
    if (c && c->flv_bs) {
        zms_flv_tag_framer_reset(c->flv_bs);
    }
}

static void on_flv_byte_tag(uint8_t type, const uint8_t *body, size_t len, uint32_t ts_ms,
                            void *user)
{
    zms_http_flv_client *c = (zms_http_flv_client *)user;
    if (!c || c->stopping) {
        return;
    }
    feed_tag(c, type, body, len, ts_ms);
}

static void flv_feed(zms_http_flv_client *c, const uint8_t *data, size_t len)
{
    ztk_err_t err;
    if (!c || !c->flv_bs || !data || len == 0) {
        return;
    }
    err = zms_flv_tag_framer_feed(c->flv_bs, data, len, on_flv_byte_tag, c);
    if (err == ZTK_ERR_INVALID) {
        client_fail(c, ZTK_ERR_INVALID);
    } else if (err == ZTK_ERR_NOMEM || err == ZTK_ERR_BUFFER_TOO_SMALL) {
        client_fail(c, ZTK_ERR_NOMEM);
    }
}

static int chunk_line_complete(zms_http_flv_chunk_dec *d, size_t *out_size)
{
    for (size_t i = 0; i < d->line_len; ++i) {
        if (d->line[i] == '\r' || d->line[i] == '\n') {
            d->line[i] = '\0';
            unsigned long sz = strtoul(d->line, NULL, 16);
            *out_size = (size_t)sz;
            return 1;
        }
    }
    return 0;
}

static void chunk_feed(zms_http_flv_client *c, const uint8_t *data, size_t len)
{
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
                    c->chunk.in_chunk = 0;
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
        flv_feed(c, data + off, take);
        off += take;
        c->chunk.chunk_left -= take;
        if (c->chunk.chunk_left == 0) {
            c->chunk.in_chunk = 0;
            while (off < len && (data[off] == '\r' || data[off] == '\n')) {
                ++off;
            }
        }
    }
}

static void body_feed(zms_http_flv_client *c, const void *data, size_t len)
{
    if (!c || !data || len == 0) {
        return;
    }
    const uint8_t *d = (const uint8_t *)data;
    if (c->body_mode == BODY_CHUNKED) {
        chunk_feed(c, d, len);
    } else {
        flv_feed(c, d, len);
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

static intptr_t on_http_header(const char *header, size_t header_len, void *user)
{
    zms_http_flv_client *c = (zms_http_flv_client *)user;
    if (!c) {
        return 0;
    }

    zms_http_parser_clear(&c->http_hdr);
    if (zms_http_parser_parse_header(&c->http_hdr, header, header_len) != ZTK_OK) {
        return 0;
    }

    if (!c->http_hdr.is_response) {
        ztk_warn("http-flv not an HTTP response (first=%s)", c->http_hdr.method);
        client_fail(c, ZTK_ERR_INVALID);
        return 0;
    }
    int status = atoi(c->http_hdr.url);
    if (status != 200 && status != 206) {
        ztk_warn("http-flv bad status %d", status);
        client_fail(c, ZTK_ERR_IO);
        return 0;
    }

    const char *ct = zms_http_parser_header(&c->http_hdr, "Content-Type");
    if (ct[0] && !str_icontains(ct, "flv") && !str_icontains(ct, "octet-stream")) {
        ztk_warn("http-flv unexpected content-type: %s", ct);
    }

    const char *te = zms_http_parser_header(&c->http_hdr, "Transfer-Encoding");
    const char *cl = zms_http_parser_header(&c->http_hdr, "Content-Length");
    memset(&c->chunk, 0, sizeof(c->chunk));
    flv_reset(c);

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
        if (n > 0) {
            c->body_mode = BODY_CL;
            return (intptr_t)n;
        }
    }
    c->body_mode = BODY_UNTIL_CLOSE;
    return -1;
}

static void on_http_content(const char *data, size_t len, void *user)
{
    zms_http_flv_client *c = (zms_http_flv_client *)user;
    if (!c || c->stopping) {
        return;
    }
    body_feed(c, data, len);
}

static void on_transport_data(zms_http_flv_client *c, const void *data, size_t len)
{
    if (!c->http) {
        return;
    }
    ztk_err_t err = zms_http_message_framer_input(c->http, data, len);
    if (err != ZTK_OK) {
        client_fail(c, err);
    }
}

static void on_io_connected(zms_http_flv_client *c)
{
    zms_http_message_framer_reset(c->http);
    zms_http_parser_clear(&c->http_hdr);
    memset(&c->chunk, 0, sizeof(c->chunk));
    flv_reset(c);
    c->ready_sent = 0;
    send_http_get(c);
}

static void on_tcp_connect(ztk_tcp_client *tcp, void *user)
{
    (void)tcp;
    on_io_connected((zms_http_flv_client *)user);
}

static void on_tcp_recv(ztk_tcp_client *tcp, const void *data, size_t len, void *user)
{
    (void)tcp;
    on_transport_data((zms_http_flv_client *)user, data, len);
}

static void on_tcp_error(ztk_tcp_client *tcp, void *user)
{
    (void)tcp;
    client_fail((zms_http_flv_client *)user, ZTK_ERR_IO);
}

static void on_tls_connect(ztk_tls_client *tls, void *user)
{
    (void)tls;
    on_io_connected((zms_http_flv_client *)user);
}

static void on_tls_recv(ztk_tls_client *tls, const void *data, size_t len, void *user)
{
    (void)tls;
    on_transport_data((zms_http_flv_client *)user, data, len);
}

static void on_tls_error(ztk_tls_client *tls, void *user)
{
    (void)tls;
    client_fail((zms_http_flv_client *)user, ZTK_ERR_IO);
}

zms_http_flv_client *zms_http_flv_client_create(const zms_http_flv_client_opts *opts)
{
    if (!opts || !opts->poller || !opts->url || (!opts->on_media && !opts->on_frame)) {
        return NULL;
    }

    zms_http_flv_client *c = (zms_http_flv_client *)calloc(1, sizeof(*c));
    if (!c) {
        return NULL;
    }
    c->opts = *opts;
    if (parse_http_flv_url(opts->url, &c->url) != ZTK_OK) {
        free(c);
        return NULL;
    }

    if (opts->on_frame) {
        zms_flv_tag_demuxer_opts dopts = {on_demux_frame, c};

        c->demux = zms_flv_tag_demuxer_create(&dopts);
        if (!c->demux) {
            free(c);
            return NULL;
        }
    }
    c->flv_bs = zms_flv_tag_framer_create();
    if (!c->flv_bs) {
        zms_flv_tag_demuxer_destroy(c->demux);
        free(c);
        return NULL;
    }

    zms_http_message_framer_opts sopts = {on_http_header, on_http_content, c};
    c->http = zms_http_message_framer_create(&sopts);
    if (!c->http) {
        zms_flv_tag_demuxer_destroy(c->demux);
        free(c);
        return NULL;
    }

    if (c->url.use_tls) {
#if !defined(ZTK_HAVE_OPENSSL) || !ZTK_HAVE_OPENSSL
        zms_http_flv_client_destroy(c);
        return NULL;
#else
        if (!opts->ssl_ctx) {
            zms_http_flv_client_destroy(c);
            return NULL;
        }
        ztk_tls_client_ops_t tops = {on_tls_connect, on_tls_recv, on_tls_error};
        ztk_tls_client_opts_t topts = {opts->poller, opts->ssl_ctx, &tops, c, c->url.host};
        c->tls = ztk_tls_client_create(&topts);
        if (!c->tls) {
            zms_http_flv_client_destroy(c);
            return NULL;
        }
#endif
    } else {
        ztk_tcp_client_ops_t tops = {on_tcp_connect, on_tcp_recv, on_tcp_error};
        ztk_tcp_client_opts_t topts = {opts->poller, &tops, c};
        c->tcp = ztk_tcp_client_create(&topts);
        if (!c->tcp) {
            zms_http_flv_client_destroy(c);
            return NULL;
        }
    }
    return c;
}

void zms_http_flv_client_destroy(zms_http_flv_client *c)
{
    if (!c) {
        return;
    }
    zms_http_flv_client_stop(c);
    ztk_tcp_client_destroy(c->tcp);
#if ZMS_HAVE_PULL_TLS
    ztk_tls_client_destroy(c->tls);
#endif
    zms_http_message_framer_destroy(c->http);
    zms_flv_tag_demuxer_destroy(c->demux);
    zms_flv_tag_framer_destroy(c->flv_bs);
    zms_http_parser_clear(&c->http_hdr);
    free(c);
}

ztk_err_t zms_http_flv_client_play(zms_http_flv_client *c)
{
    if (!c) {
        return ZTK_ERR_INVALID;
    }
    c->stopping = 0;
    c->ready_sent = 0;
    return client_connect(c);
}

void zms_http_flv_client_stop(zms_http_flv_client *c)
{
    if (!c) {
        return;
    }
    c->stopping = 1;
    client_close(c);
    if (c->http) {
        zms_http_message_framer_reset(c->http);
    }
    flv_reset(c);
    c->ready_sent = 0;
}
