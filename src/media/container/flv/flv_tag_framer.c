#include "zms/media/container/flv/flv_tag_framer.h"
#include "flv-header.h"
#include "flv-proto.h"
#include <stdlib.h>
#include <string.h>

#define FLV_TAG_BODY_MAX (4 * 1024 * 1024)

typedef enum {
    FLV_ST_HDR = 0,
    FLV_ST_PREV0,
    FLV_ST_TAG_HDR,
    FLV_ST_TAG_BODY,
    FLV_ST_TAG_PREV,
} flv_parse_st;

struct zms_flv_tag_framer {
    flv_parse_st st;
    uint8_t scratch[16];
    size_t scratch_len;
    struct flv_tag_header_t cur_tag;
    uint8_t *tag_body;
    size_t tag_body_cap;
};

zms_flv_tag_framer *zms_flv_tag_framer_create(void)
{
    return (zms_flv_tag_framer *)calloc(1, sizeof(zms_flv_tag_framer));
}

void zms_flv_tag_framer_destroy(zms_flv_tag_framer *s)
{
    if (!s) {
        return;
    }
    free(s->tag_body);
    free(s);
}

void zms_flv_tag_framer_reset(zms_flv_tag_framer *s)
{
    if (!s) {
        return;
    }
    s->st = FLV_ST_HDR;
    s->scratch_len = 0;
    memset(&s->cur_tag, 0, sizeof(s->cur_tag));
}

static ztk_err_t ensure_tag_buf(zms_flv_tag_framer *s, size_t need)
{
    if (need > FLV_TAG_BODY_MAX) {
        return ZTK_ERR_BUFFER_TOO_SMALL;
    }
    if (s->tag_body_cap >= need) {
        return ZTK_OK;
    }
    size_t cap = s->tag_body_cap ? s->tag_body_cap : 4096;
    while (cap < need) {
        cap *= 2;
    }
    uint8_t *p = (uint8_t *)realloc(s->tag_body, cap);
    if (!p) {
        return ZTK_ERR_NOMEM;
    }
    s->tag_body = p;
    s->tag_body_cap = cap;
    return ZTK_OK;
}

ztk_err_t zms_flv_tag_framer_feed(zms_flv_tag_framer *s, const uint8_t *data, size_t len,
                                  zms_flv_on_tag on_tag, void *user)
{
    size_t off = 0;

    if (!s || !data || len == 0) {
        return ZTK_ERR_INVALID;
    }

    while (off < len) {
        switch (s->st) {
        case FLV_ST_HDR: {
            size_t need = 9 - s->scratch_len;
            size_t copy = need < len - off ? need : len - off;
            memcpy(s->scratch + s->scratch_len, data + off, copy);
            s->scratch_len += copy;
            off += copy;
            if (s->scratch_len < 9) {
                return ZTK_OK;
            }
            struct flv_header_t hdr;
            if (flv_header_read(&hdr, s->scratch, 9) < 0) {
                return ZTK_ERR_INVALID;
            }
            s->scratch_len = 0;
            s->st = FLV_ST_PREV0;
            break;
        }
        case FLV_ST_PREV0: {
            size_t need = 4 - s->scratch_len;
            size_t copy = need < len - off ? need : len - off;
            memcpy(s->scratch + s->scratch_len, data + off, copy);
            s->scratch_len += copy;
            off += copy;
            if (s->scratch_len < 4) {
                return ZTK_OK;
            }
            s->scratch_len = 0;
            s->st = FLV_ST_TAG_HDR;
            break;
        }
        case FLV_ST_TAG_HDR: {
            size_t need = 11 - s->scratch_len;
            size_t copy = need < len - off ? need : len - off;
            memcpy(s->scratch + s->scratch_len, data + off, copy);
            s->scratch_len += copy;
            off += copy;
            if (s->scratch_len < 11) {
                return ZTK_OK;
            }
            if (flv_tag_header_read(&s->cur_tag, s->scratch, 11) < 0) {
                return ZTK_ERR_INVALID;
            }
            s->scratch_len = 0;
            if (s->cur_tag.size == 0) {
                s->st = FLV_ST_TAG_PREV;
                break;
            }
            if (ensure_tag_buf(s, s->cur_tag.size) != ZTK_OK) {
                return ZTK_ERR_NOMEM;
            }
            s->st = FLV_ST_TAG_BODY;
            break;
        }
        case FLV_ST_TAG_BODY: {
            size_t have = s->scratch_len;
            size_t need = s->cur_tag.size - have;
            size_t copy = need < len - off ? need : len - off;
            memcpy(s->tag_body + have, data + off, copy);
            s->scratch_len = have + copy;
            off += copy;
            if (s->scratch_len < s->cur_tag.size) {
                return ZTK_OK;
            }
            if (on_tag) {
                on_tag(s->cur_tag.type, s->tag_body, s->cur_tag.size, s->cur_tag.timestamp, user);
            }
            s->scratch_len = 0;
            s->st = FLV_ST_TAG_PREV;
            break;
        }
        case FLV_ST_TAG_PREV: {
            size_t need = 4 - s->scratch_len;
            size_t copy = need < len - off ? need : len - off;
            memcpy(s->scratch + s->scratch_len, data + off, copy);
            s->scratch_len += copy;
            off += copy;
            if (s->scratch_len < 4) {
                return ZTK_OK;
            }
            s->scratch_len = 0;
            s->st = FLV_ST_TAG_HDR;
            break;
        }
        default:
            return ZTK_ERR_INVALID;
        }
    }
    return ZTK_OK;
}
