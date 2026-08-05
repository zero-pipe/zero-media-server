#include "zms/session/rtmp/rtmp_amf3_decode.h"
#include "zms/session/rtmp/rtmp_amf.h"
#include <stdio.h>
#include <string.h>

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;
} amf3_buf;

static int u29_read(amf3_buf *b, uint32_t *out)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        if (b->pos >= b->len) {
            return -1;
        }
        uint8_t byte = b->data[b->pos++];
        if (i < 3) {
            v = (v << 7) | (byte & 0x7f);
            if ((byte & 0x80) == 0) {
                *out = v;
                return 0;
            }
        } else {
            *out = (v << 8) | byte;
            return 0;
        }
    }
    return -1;
}

static int string_read(amf3_buf *b, char *out, size_t out_cap)
{
    uint32_t u = 0;
    if (u29_read(b, &u) != 0) {
        return -1;
    }
    if (u & 1) {
        return 0;
    }
    uint32_t slen = u >> 1;
    if (b->pos + slen > b->len) {
        return -1;
    }
    if (out && out_cap) {
        size_t copy = slen < out_cap - 1 ? slen : out_cap - 1;
        memcpy(out, b->data + b->pos, copy);
        out[copy] = '\0';
    }
    b->pos += slen;
    return 0;
}

static int amf3_skip_value(amf3_buf *b);

static int amf3_read_object(amf3_buf *b, char *app, size_t app_cap, char *tc_url, size_t tc_cap)
{
    if (b->pos >= b->len || b->data[b->pos++] != 0x0a) {
        return -1;
    }

    uint32_t ref = 0;
    if (u29_read(b, &ref) != 0) {
        return -1;
    }
    if ((ref & 1) == 0) {
        return 0;
    }

    int external = (ref & 2) != 0;
    int dynamic = (ref & 4) != 0;
    unsigned member_count = (unsigned)(ref >> 4);

    if (external) {
        return -1;
    }

    if (string_read(b, NULL, 0) != 0) {
        return -1;
    }

    for (unsigned i = 0; i < member_count; ++i) {
        if (amf3_skip_value(b) != 0) {
            return -1;
        }
    }

    if (!dynamic) {
        return 0;
    }

    for (;;) {
        char key[64];
        if (string_read(b, key, sizeof(key)) != 0) {
            return -1;
        }
        if (!key[0]) {
            break;
        }
        if (b->pos >= b->len) {
            return -1;
        }
        uint8_t vt = b->data[b->pos];
        if (vt == 0x06) {
            b->pos++;
            char val[768];
            if (string_read(b, val, sizeof(val)) != 0) {
                return -1;
            }
            if (strcmp(key, "app") == 0 && app && app_cap) {
                strncpy(app, val, app_cap - 1);
            } else if (strcmp(key, "tcUrl") == 0 && tc_url && tc_cap) {
                strncpy(tc_url, val, tc_cap - 1);
            }
        } else if (amf3_skip_value(b) != 0) {
            return -1;
        }
    }
    return 0;
}

static int amf3_skip_value(amf3_buf *b)
{
    if (b->pos >= b->len) {
        return -1;
    }
    uint8_t t = b->data[b->pos++];
    switch (t) {
    case 0x00:
    case 0x01:
    case 0x02:
    case 0x03:
        return 0;
    case 0x04:
        return u29_read(b, NULL);
    case 0x05:
        return (b->pos + 8 <= b->len) ? ((b->pos += 8), 0) : -1;
    case 0x06:
        return string_read(b, NULL, 0);
    case 0x08:
        return u29_read(b, NULL);
    case 0x09: {
        uint32_t ref = 0;
        if (u29_read(b, &ref) != 0) {
            return -1;
        }
        if ((ref & 1) == 0) {
            return 0;
        }
        int dense = (ref & 2) != 0;
        if (!dense && string_read(b, NULL, 0) != 0) {
            return -1;
        }
        uint32_t n = 0;
        if (u29_read(b, &n) != 0) {
            return -1;
        }
        for (uint32_t i = 0; i < n; ++i) {
            if (string_read(b, NULL, 0) != 0) {
                return -1;
            }
            if (amf3_skip_value(b) != 0) {
                return -1;
            }
        }
        if (!dense) {
            for (;;) {
                char k[64];
                if (string_read(b, k, sizeof(k)) != 0) {
                    return -1;
                }
                if (!k[0]) {
                    break;
                }
                if (amf3_skip_value(b) != 0) {
                    return -1;
                }
            }
        }
        return 0;
    }
    case 0x0a:
        return amf3_read_object(b, NULL, 0, NULL, 0);
    default:
        return -1;
    }
}

static int try_amf3_connect(amf3_buf *b, char *app, size_t app_cap, char *tc_url, size_t tc_cap)
{
    while (b->pos < b->len) {
        if (b->data[b->pos] == 0x11) {
            b->pos++;
            if (amf3_read_object(b, app, app_cap, tc_url, tc_cap) == 0) {
                return (app && app[0]) || (tc_url && tc_url[0]);
            }
        } else if (b->data[b->pos] == 0x0a) {
            if (amf3_read_object(b, app, app_cap, tc_url, tc_cap) == 0) {
                return (app && app[0]) || (tc_url && tc_url[0]);
            }
        } else if (b->data[b->pos] == 0x05) {
            b->pos++;
        } else {
            break;
        }
    }
    return 0;
}

static int amf0_scan_string_key(const uint8_t *data, size_t len, const char *key, char *out,
                                size_t out_cap)
{
    if (!data || !key || !out || out_cap == 0) {
        return 0;
    }
    size_t klen = strlen(key);
    for (size_t i = 0; i + 3 + klen < len; ++i) {
        if (data[i] != 0 || data[i + 1] != (uint8_t)((klen >> 8) & 0xff) ||
            data[i + 2] != (uint8_t)(klen & 0xff)) {
            continue;
        }
        if (memcmp(data + i + 3, key, klen) != 0) {
            continue;
        }
        size_t j = i + 3 + klen;
        if (j >= len || data[j] != 0x02) {
            continue;
        }
        j += 1;
        if (j + 2 > len) {
            continue;
        }
        uint16_t slen = (uint16_t)((data[j] << 8) | data[j + 1]);
        j += 2;
        if (j + slen > len) {
            continue;
        }
        size_t copy = slen < out_cap - 1 ? slen : out_cap - 1;
        memcpy(out, data + j, copy);
        out[copy] = '\0';
        return 1;
    }
    return 0;
}

static int amf0_extract_connect_scan(const uint8_t *data, size_t len, char *app, size_t app_cap,
                                     char *tc_url, size_t tc_cap)
{
    int got = 0;
    if (amf0_scan_string_key(data, len, "app", app, app_cap)) {
        got = 1;
    }
    if (amf0_scan_string_key(data, len, "tcUrl", tc_url, tc_cap)) {
        got = 1;
    }
    return got;
}

static zms_amf_value *amf0_decode_object_arg(const uint8_t *data, size_t len)
{
    size_t off = 0;
    while (off < len) {
        size_t step = 0;
        zms_amf_value *v = NULL;
        if (zms_amf_decode(data + off, len - off, &v, &step) != ZTK_OK || !v) {
            break;
        }
        off += step;
        zms_amf_type t = zms_amf_value_type(v);
        if (t == ZMS_AMF_OBJECT || t == ZMS_AMF_ECMA_ARRAY) {
            return v;
        }
        zms_amf_value_free(v);
    }
    return NULL;
}

int zms_amf_extract_connect(const uint8_t *data, size_t len, char *app, size_t app_cap,
                            char *tc_url, size_t tc_cap)
{
    if (!data || !len) {
        return 0;
    }
    if (app && app_cap) {
        app[0] = '\0';
    }
    if (tc_url && tc_cap) {
        tc_url[0] = '\0';
    }

    amf3_buf b = {data, len, 0};
    if (try_amf3_connect(&b, app, app_cap, tc_url, tc_cap)) {
        return 1;
    }

    for (size_t start = 0; start < len && start < 12; ++start) {
        if (data[start] == 0x05 || data[start] == 0x11) {
            continue;
        }
        if (data[start] != 0x03 && data[start] != 0x08) {
            continue;
        }
        zms_amf_value *try = NULL;
        size_t step = 0;
        if (zms_amf_decode(data + start, len - start, &try, &step) != ZTK_OK || !try) {
            continue;
        }
        zms_amf_type t = zms_amf_value_type(try);
        if (t == ZMS_AMF_OBJECT || t == ZMS_AMF_ECMA_ARRAY) {
            const zms_amf_value *av = zms_amf_object_get(try, "app");
            if (av && zms_amf_value_str(av)[0] && app && app_cap) {
                strncpy(app, zms_amf_value_str(av), app_cap - 1);
            }
            const zms_amf_value *tc = zms_amf_object_get(try, "tcUrl");
            if (tc && zms_amf_value_str(tc)[0] && tc_url && tc_cap) {
                strncpy(tc_url, zms_amf_value_str(tc), tc_cap - 1);
            }
            zms_amf_value_free(try);
            if ((app && app[0]) || (tc_url && tc_url[0])) {
                return 1;
            }
        } else {
            zms_amf_value_free(try);
        }
    }

    if (amf0_extract_connect_scan(data, len, app, app_cap, tc_url, tc_cap)) {
        return 1;
    }

    zms_amf_value *obj = amf0_decode_object_arg(data, len);
    if (!obj) {
        return 0;
    }
    const zms_amf_value *av = zms_amf_object_get(obj, "app");
    if (av && zms_amf_value_str(av)[0] && app && app_cap) {
        strncpy(app, zms_amf_value_str(av), app_cap - 1);
    }
    const zms_amf_value *tc = zms_amf_object_get(obj, "tcUrl");
    if (tc && zms_amf_value_str(tc)[0] && tc_url && tc_cap) {
        strncpy(tc_url, zms_amf_value_str(tc), tc_cap - 1);
    }
    zms_amf_value_free(obj);
    return (app && app[0]) || (tc_url && tc_url[0]);
}
