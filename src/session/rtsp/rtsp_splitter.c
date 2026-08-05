#include "zms/session/rtsp/rtsp_splitter.h"
#include "zms/media/container/container_dispatcher.h"
#include "zms/media/wire/rtp_packet.h"
#include "zms/session/http/http_message_framer.h"
#include <stdlib.h>
#include <string.h>

#define ZMS_RTSP_SPLITTER_MAX_CACHE (256U * 1024U)

struct zms_rtsp_splitter {
    zms_rtsp_splitter_opts opts;
    zms_http_message_framer *inner;
    zms_rtsp_message msg;
    int enable_rtp;
    int is_rtp_packet;
};

static const char *search_rtsp_tail_l(zms_rtsp_splitter *s, const char *data, size_t len)
{
    if (s->enable_rtp && len > 0 && (uint8_t)data[0] == '$') {
        if (len < ZMS_RTSP_INTERLEAVED_HDR) {
            return NULL;
        }
        uint16_t plen = (uint16_t)(((uint8_t)data[2] << 8) | (uint8_t)data[3]);
        size_t total = ZMS_RTSP_INTERLEAVED_HDR + plen;
        if (len < total) {
            return NULL;
        }
        s->is_rtp_packet = 1;
        return data + total;
    }

    s->is_rtp_packet = 0;
    return zms_http_message_search_tail(data, len);
}

static const char *on_search_tail(const char *data, size_t len, void *user)
{
    zms_rtsp_splitter *s = (zms_rtsp_splitter *)user;
    const char *ret = search_rtsp_tail_l(s, data, len);
    if (ret) {
        return ret;
    }

    if (len > ZMS_RTSP_SPLITTER_MAX_CACHE) {
        const char *mark = (const char *)memchr(data, '$', len);
        if (!mark) {
            return NULL;
        }
        return mark;
    }
    return NULL;
}

static void dispatch_interleaved(zms_rtsp_splitter *s, const uint8_t *data, size_t len)
{
    zms_container_packet pkt;
    if (zms_container_rtsp_interleaved_parse(data, len, &pkt) != ZTK_OK) {
        return;
    }
    if (s->opts.on_rtp) {
        s->opts.on_rtp(pkt.channel, pkt.data, pkt.len, s->opts.user);
    }
}

static void dispatch_rtsp_message(zms_rtsp_splitter *s)
{
    if (s->opts.on_message) {
        s->opts.on_message(&s->msg, s->opts.user);
    }
    memset(&s->msg, 0, sizeof(s->msg));
}

static intptr_t on_rtsp_header(const char *header, size_t header_len, void *user)
{
    zms_rtsp_splitter *s = (zms_rtsp_splitter *)user;
    if (!s) {
        return 0;
    }

    if (s->is_rtp_packet) {
        dispatch_interleaved(s, (const uint8_t *)header, header_len);
        return 0;
    }

    if (header_len == 4 && memcmp(header, "\r\n\r\n", 4) == 0) {
        return 0;
    }

    memset(&s->msg, 0, sizeof(s->msg));
    if (zms_rtsp_message_parse_header(header, header_len, &s->msg) != ZTK_OK) {
        if (s->enable_rtp) {
            return 0;
        }
        return 0;
    }

    int clen = zms_rtsp_message_content_length(&s->msg);
    if (clen <= 0) {
        dispatch_rtsp_message(s);
        return 0;
    }
    return (intptr_t)clen;
}

static void on_rtsp_content(const char *data, size_t len, void *user)
{
    zms_rtsp_splitter *s = (zms_rtsp_splitter *)user;
    if (!s) {
        return;
    }
    s->msg.body = data;
    s->msg.body_len = len;
    dispatch_rtsp_message(s);
}

zms_rtsp_splitter *zms_rtsp_splitter_create(const zms_rtsp_splitter_opts *opts)
{
    if (!opts) {
        return NULL;
    }
    zms_rtsp_splitter *s = (zms_rtsp_splitter *)calloc(1, sizeof(*s));
    if (!s) {
        return NULL;
    }
    s->opts = *opts;

    zms_http_message_framer_opts iopts = {
        .on_header = on_rtsp_header,
        .on_content = on_rtsp_content,
        .user = s,
        .max_cache_size = ZMS_RTSP_SPLITTER_MAX_CACHE,
        .on_search_tail = on_search_tail,
    };
    s->inner = zms_http_message_framer_create(&iopts);
    if (!s->inner) {
        free(s);
        return NULL;
    }
    return s;
}

void zms_rtsp_splitter_destroy(zms_rtsp_splitter *s)
{
    if (!s) {
        return;
    }
    zms_http_message_framer_destroy(s->inner);
    free(s);
}

void zms_rtsp_splitter_enable_rtp(zms_rtsp_splitter *s, int on)
{
    if (s) {
        s->enable_rtp = on ? 1 : 0;
    }
}

void zms_rtsp_splitter_reset(zms_rtsp_splitter *s)
{
    if (!s) {
        return;
    }
    zms_http_message_framer_reset(s->inner);
    memset(&s->msg, 0, sizeof(s->msg));
    s->is_rtp_packet = 0;
}

ztk_err_t zms_rtsp_splitter_input(zms_rtsp_splitter *s, const void *data, size_t len)
{
    if (!s || !s->inner) {
        return ZTK_ERR_INVALID;
    }
    if (!data && len) {
        return ZTK_ERR_INVALID;
    }
    if (len == 0) {
        return ZTK_OK;
    }

    ztk_err_t err = zms_http_message_framer_input(s->inner, data, len);
    if (err == ZTK_ERR_BUFFER_TOO_SMALL && s->enable_rtp) {
        zms_rtsp_splitter_reset(s);
        const char *d = (const char *)data;
        const char *mark = (const char *)memchr(d, '$', len);
        if (mark && mark > d) {
            return zms_http_message_framer_input(s->inner, mark, len - (size_t)(mark - d));
        }
        return ZTK_ERR_IO;
    }
    return err;
}
