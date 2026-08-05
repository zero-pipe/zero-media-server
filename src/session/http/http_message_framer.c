#include "zms/session/http/http_message_framer.h"
#include "ztk/util/byte_buf.h"
#include <stdlib.h>
#include <string.h>

#define ZMS_HTTP_DEFAULT_MAX_CACHE (4U * 1024U * 1024U)

struct zms_http_message_framer {
    zms_http_message_framer_opts opts;
    ztk_byte_buf *remain;
    intptr_t content_len;
    size_t remain_data_size;
    size_t max_cache_size;
};

const char *zms_http_message_search_tail(const char *data, size_t len)
{
    for (size_t i = 0; i + 3 < len; ++i) {
        if (data[i] == '\r' && data[i + 1] == '\n' && data[i + 2] == '\r' && data[i + 3] == '\n') {
            return data + i + 4;
        }
    }
    return NULL;
}

static const char *search_packet_tail(zms_http_message_framer *s, const char *data, size_t len)
{
    if (s->opts.on_search_tail) {
        return s->opts.on_search_tail(data, len, s->opts.user);
    }
    return zms_http_message_search_tail(data, len);
}

zms_http_message_framer *zms_http_message_framer_create(const zms_http_message_framer_opts *opts)
{
    zms_http_message_framer *s = (zms_http_message_framer *)calloc(1, sizeof(*s));
    if (!s) {
        return NULL;
    }
    if (opts) {
        s->opts = *opts;
    }
    s->max_cache_size =
        s->opts.max_cache_size ? s->opts.max_cache_size : ZMS_HTTP_DEFAULT_MAX_CACHE;
    s->remain = ztk_byte_buf_create(0);
    if (!s->remain) {
        free(s);
        return NULL;
    }
    return s;
}

void zms_http_message_framer_destroy(zms_http_message_framer *s)
{
    if (!s) {
        return;
    }
    ztk_byte_buf_destroy(s->remain);
    free(s);
}

void zms_http_message_framer_reset(zms_http_message_framer *s)
{
    if (!s) {
        return;
    }
    s->content_len = 0;
    s->remain_data_size = 0;
    ztk_byte_buf_clear(s->remain);
}

size_t zms_http_message_framer_remain_size(const zms_http_message_framer *s)
{
    return s ? s->remain_data_size : 0;
}

ztk_err_t zms_http_message_framer_input(zms_http_message_framer *s, const void *data, size_t len)
{
    if (!s || !data || len == 0) {
        return ZTK_ERR_INVALID;
    }

    size_t cached = ztk_byte_buf_size(s->remain);
    if (cached + len > s->max_cache_size) {
        zms_http_message_framer_reset(s);
        return ZTK_ERR_BUFFER_TOO_SMALL;
    }

    const char *ptr = (const char *)data;
    if (cached > 0) {
        if (ztk_byte_buf_append(s->remain, data, len) != ZTK_OK) {
            return ZTK_ERR_NOMEM;
        }
        ptr = ztk_byte_buf_data(s->remain);
        len = ztk_byte_buf_size(s->remain);
    }

split_packet:
    const char *packet_data = ptr;
    s->remain_data_size = len;

    while (s->content_len == 0 && s->remain_data_size > 0) {
        const char *index = search_packet_tail(s, ptr, s->remain_data_size);
        if (!index || index == ptr) {
            break;
        }
        if (index < ptr || index > ptr + s->remain_data_size) {
            return ZTK_ERR_INVALID;
        }

        const char *header_ptr = ptr;
        size_t header_size = (size_t)(index - ptr);
        ptr = index;
        s->remain_data_size = len - (size_t)(ptr - packet_data);

        if (s->opts.on_header) {
            s->content_len = s->opts.on_header(header_ptr, header_size, s->opts.user);
        } else {
            s->content_len = 0;
        }
    }

    if (s->remain_data_size <= 0) {
        ztk_byte_buf_clear(s->remain);
        return ZTK_OK;
    }

    if (s->content_len == 0) {
        ztk_byte_buf_clear(s->remain);
        if (ztk_byte_buf_append(s->remain, ptr, s->remain_data_size) != ZTK_OK) {
            return ZTK_ERR_NOMEM;
        }
        return ZTK_OK;
    }

    if (s->content_len > 0) {
        if (s->remain_data_size < (size_t)s->content_len) {
            ztk_byte_buf_clear(s->remain);
            if (ztk_byte_buf_append(s->remain, ptr, s->remain_data_size) != ZTK_OK) {
                return ZTK_ERR_NOMEM;
            }
            return ZTK_OK;
        }
        if (s->opts.on_content) {
            s->opts.on_content(ptr, (size_t)s->content_len, s->opts.user);
        }
        s->remain_data_size -= (size_t)s->content_len;
        ptr += (size_t)s->content_len;
        s->content_len = 0;

        if (s->remain_data_size > 0) {
            ztk_byte_buf_clear(s->remain);
            if (ztk_byte_buf_append(s->remain, ptr, s->remain_data_size) != ZTK_OK) {
                return ZTK_ERR_NOMEM;
            }
            ptr = ztk_byte_buf_data(s->remain);
            len = ztk_byte_buf_size(s->remain);
            goto split_packet;
        }
        ztk_byte_buf_clear(s->remain);
        return ZTK_OK;
    }

    if (s->opts.on_content) {
        s->opts.on_content(ptr, s->remain_data_size, s->opts.user);
    }
    ztk_byte_buf_clear(s->remain);
    s->remain_data_size = 0;
    return ZTK_OK;
}
