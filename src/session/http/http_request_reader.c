#include "zms/session/http/http_request_reader.h"
#include "zms/session/http/http_parser.h"
#include "zms/session/http/http_message_framer.h"
#if !defined(_WIN32)
#include <strings.h>
#endif
#include <stdlib.h>
#include <string.h>

#define ZMS_HTTP_MAX_BODY (64U * 1024U)

struct zms_http_request_reader {
    zms_http_request_reader_opts opts;
    zms_http_message_framer *inner;
    zms_http_parser parser;
    char path_buf[ZMS_HTTP_URL_MAX + ZMS_HTTP_URL_MAX];
};

static void emit_request(zms_http_request_reader *s)
{
    if (!s || !s->opts.on_request) {
        return;
    }

    zms_http_request req;
    memset(&req, 0, sizeof(req));
    strncpy(req.method, s->parser.method, sizeof(req.method) - 1);

    zms_http_parser_full_url(&s->parser, s->path_buf, sizeof(s->path_buf));
    strncpy(req.path, s->path_buf, sizeof(req.path) - 1);

    const char *host = zms_http_parser_header(&s->parser, "Host");
    if (!host[0]) {
        host = zms_http_parser_header(&s->parser, "host");
    }
    strncpy(req.host, host, sizeof(req.host) - 1);

    {
        const char *range = zms_http_parser_header(&s->parser, "Range");
        if (!range[0]) {
            range = zms_http_parser_header(&s->parser, "range");
        }
        strncpy(req.range, range, sizeof(req.range) - 1);
    }

    req.body = s->parser.content;
    req.body_len = s->parser.content_len;

    {
        const char *upgrade = zms_http_parser_header(&s->parser, "Upgrade");
        const char *ws_key = zms_http_parser_header(&s->parser, "Sec-WebSocket-Key");
#if defined(_WIN32)
        req.ws_upgrade = upgrade && upgrade[0] && _stricmp(upgrade, "websocket") == 0;
#else
        req.ws_upgrade = upgrade && upgrade[0] && strcasecmp(upgrade, "websocket") == 0;
#endif
        if (ws_key && ws_key[0]) {
            strncpy(req.ws_key, ws_key, sizeof(req.ws_key) - 1);
        }
    }

    s->opts.on_request(&req, s->opts.user);
}

static intptr_t on_packet_header(const char *header, size_t header_len, void *user)
{
    zms_http_request_reader *s = (zms_http_request_reader *)user;
    if (!s) {
        return 0;
    }

    zms_http_parser_clear(&s->parser);
    if (zms_http_parser_parse_header(&s->parser, header, header_len) != ZTK_OK) {
        return 0;
    }

    const char *cl = zms_http_parser_header(&s->parser, "Content-Length");
    if (cl[0]) {
        unsigned long long n = 0;
        for (const char *p = cl; *p; ++p) {
            if (*p < '0' || *p > '9') {
                return 0;
            }
            n = n * 10ULL + (unsigned long long)(*p - '0');
            if (n > ZMS_HTTP_MAX_BODY) {
                return 0;
            }
        }
        if (n > 0) {
            return (intptr_t)n;
        }
    }

    emit_request(s);
    zms_http_parser_clear(&s->parser);
    return 0;
}

static void on_packet_content(const char *data, size_t len, void *user)
{
    zms_http_request_reader *s = (zms_http_request_reader *)user;
    if (!s) {
        return;
    }
    if (zms_http_parser_set_content(&s->parser, data, len) != ZTK_OK) {
        return;
    }
    emit_request(s);
    zms_http_parser_clear(&s->parser);
}

zms_http_request_reader *zms_http_request_reader_create(const zms_http_request_reader_opts *opts)
{
    zms_http_request_reader *s = (zms_http_request_reader *)calloc(1, sizeof(*s));
    if (!s) {
        return NULL;
    }
    if (opts) {
        s->opts = *opts;
    }

    zms_http_message_framer_opts iopts = {
        .on_header = on_packet_header,
        .on_content = on_packet_content,
        .user = s,
        .max_cache_size = 0,
    };
    s->inner = zms_http_message_framer_create(&iopts);
    if (!s->inner) {
        free(s);
        return NULL;
    }
    return s;
}

void zms_http_request_reader_destroy(zms_http_request_reader *s)
{
    if (!s) {
        return;
    }
    zms_http_message_framer_destroy(s->inner);
    zms_http_parser_clear(&s->parser);
    free(s);
}

void zms_http_request_reader_reset(zms_http_request_reader *s)
{
    if (!s) {
        return;
    }
    zms_http_message_framer_reset(s->inner);
    zms_http_parser_clear(&s->parser);
}

ztk_err_t zms_http_request_reader_input(zms_http_request_reader *s, const void *data, size_t len)
{
    if (!s || !s->inner) {
        return ZTK_ERR_INVALID;
    }
    return zms_http_message_framer_input(s->inner, data, len);
}
