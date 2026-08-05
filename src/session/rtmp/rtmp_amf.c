#include "zms/session/rtmp/rtmp_amf.h"
#include <stdlib.h>
#include <string.h>

struct zms_amf_value {
    zms_amf_type type;
    char *str;
    double num;
    int boolean;
    struct zms_amf_pair {
        char key[64];
        zms_amf_value *val;
    } *pairs;
    unsigned pair_count;
};

static zms_amf_value *amf_new(zms_amf_type t)
{
    zms_amf_value *v = (zms_amf_value *)calloc(1, sizeof(*v));
    if (v) {
        v->type = t;
    }
    return v;
}

void zms_amf_value_free(zms_amf_value *v)
{
    if (!v) {
        return;
    }
    free(v->str);
    for (unsigned i = 0; i < v->pair_count; ++i) {
        zms_amf_value_free(v->pairs[i].val);
    }
    free(v->pairs);
    free(v);
}

static ztk_err_t decode_one(const uint8_t *data, size_t len, size_t *off, zms_amf_value **out);

static ztk_err_t read_string(const uint8_t *data, size_t len, size_t *off, char **out)
{
    if (*off + 2 > len) {
        return ZTK_ERR_INVALID;
    }
    uint16_t slen = (uint16_t)((data[*off] << 8) | data[*off + 1]);
    *off += 2;
    if (*off + slen > len) {
        return ZTK_ERR_INVALID;
    }
    char *s = (char *)malloc((size_t)slen + 1);
    if (!s) {
        return ZTK_ERR_NOMEM;
    }
    if (slen) {
        memcpy(s, data + *off, slen);
    }
    s[slen] = '\0';
    *off += slen;
    *out = s;
    return ZTK_OK;
}

static ztk_err_t decode_one(const uint8_t *data, size_t len, size_t *off, zms_amf_value **out)
{
    if (*off >= len) {
        return ZTK_ERR_INVALID;
    }
    uint8_t t = data[(*off)++];
    zms_amf_value *v = amf_new((zms_amf_type)t);
    if (!v) {
        return ZTK_ERR_NOMEM;
    }

    switch (t) {
    case ZMS_AMF_NUMBER:
        if (*off + 8 > len) {
            zms_amf_value_free(v);
            return ZTK_ERR_INVALID;
        }
        {
            static const double k_endian_probe = 1.0;
            const uint8_t *p = data + *off;
            uint8_t *dst = (uint8_t *)&v->num;

            if (*(const char *)&k_endian_probe == 0) {
                dst[0] = p[7];
                dst[1] = p[6];
                dst[2] = p[5];
                dst[3] = p[4];
                dst[4] = p[3];
                dst[5] = p[2];
                dst[6] = p[1];
                dst[7] = p[0];
            } else {
                memcpy(&v->num, p, 8);
            }
        }
        *off += 8;
        break;
    case ZMS_AMF_BOOLEAN:
        if (*off + 1 > len) {
            zms_amf_value_free(v);
            return ZTK_ERR_INVALID;
        }
        v->boolean = data[*off++] ? 1 : 0;
        break;
    case ZMS_AMF_STRING:
        if (read_string(data, len, off, &v->str) != ZTK_OK) {
            zms_amf_value_free(v);
            return ZTK_ERR_INVALID;
        }
        break;
    case ZMS_AMF_NULL:
        break;
    case 0x06: /* AMF0_UNDEFINED */
        break;
    case 0x0c: { /* AMF0_LONG_STRING */
        if (*off + 4 > len) {
            zms_amf_value_free(v);
            return ZTK_ERR_INVALID;
        }
        uint32_t slen = ((uint32_t)data[*off] << 24) | ((uint32_t)data[*off + 1] << 16) |
                        ((uint32_t)data[*off + 2] << 8) | (uint32_t)data[*off + 3];
        *off += 4;
        if (*off + slen > len) {
            zms_amf_value_free(v);
            return ZTK_ERR_INVALID;
        }
        v->str = (char *)malloc((size_t)slen + 1);
        if (!v->str) {
            zms_amf_value_free(v);
            return ZTK_ERR_NOMEM;
        }
        if (slen) {
            memcpy(v->str, data + *off, slen);
        }
        v->str[slen] = '\0';
        *off += slen;
        v->type = ZMS_AMF_STRING;
        break;
    }
    case ZMS_AMF_STRICT_ARRAY: {
        if (*off + 4 > len) {
            zms_amf_value_free(v);
            return ZTK_ERR_INVALID;
        }
        uint32_t count = ((uint32_t)data[*off] << 24) | ((uint32_t)data[*off + 1] << 16) |
                         ((uint32_t)data[*off + 2] << 8) | (uint32_t)data[*off + 3];
        *off += 4;
        for (uint32_t i = 0; i < count; ++i) {
            zms_amf_value *elem = NULL;
            if (decode_one(data, len, off, &elem) != ZTK_OK) {
                zms_amf_value_free(v);
                return ZTK_ERR_INVALID;
            }
            zms_amf_value_free(elem);
        }
        v->type = ZMS_AMF_NULL;
        break;
    }
    case ZMS_AMF_OBJECT:
    case ZMS_AMF_ECMA_ARRAY:
        if (t == ZMS_AMF_ECMA_ARRAY) {
            if (*off + 4 > len) {
                zms_amf_value_free(v);
                return ZTK_ERR_INVALID;
            }
            *off += 4;
        }
        for (;;) {
            if (*off + 3 > len) {
                zms_amf_value_free(v);
                return ZTK_ERR_INVALID;
            }
            uint16_t klen = (uint16_t)((data[*off] << 8) | data[*off + 1]);
            *off += 2;
            if (klen == 0) {
                if (*off + 1 > len || data[*off] != 9) {
                    zms_amf_value_free(v);
                    return ZTK_ERR_INVALID;
                }
                *off += 1;
                break;
            }
            if (*off + klen > len) {
                zms_amf_value_free(v);
                return ZTK_ERR_INVALID;
            }
            char key[64];
            size_t copy = klen < sizeof(key) - 1 ? klen : sizeof(key) - 1;
            memcpy(key, data + *off, copy);
            key[copy] = '\0';
            *off += klen;
            zms_amf_value *child = NULL;
            if (decode_one(data, len, off, &child) != ZTK_OK) {
                zms_amf_value_free(v);
                return ZTK_ERR_INVALID;
            }
            v->pairs =
                (struct zms_amf_pair *)realloc(v->pairs, (v->pair_count + 1) * sizeof(*v->pairs));
            if (!v->pairs) {
                zms_amf_value_free(child);
                zms_amf_value_free(v);
                return ZTK_ERR_NOMEM;
            }
            strncpy(v->pairs[v->pair_count].key, key, sizeof(v->pairs[0].key) - 1);
            v->pairs[v->pair_count].val = child;
            v->pair_count++;
        }
        break;
    default:
        zms_amf_value_free(v);
        return ZTK_ERR_NOT_IMPL;
    }
    *out = v;
    return ZTK_OK;
}

ztk_err_t zms_amf_decode(const uint8_t *data, size_t len, zms_amf_value **out, size_t *consumed)
{
    size_t off = 0;
    ztk_err_t err = decode_one(data, len, &off, out);
    if (err != ZTK_OK) {
        return err;
    }
    if (consumed) {
        *consumed = off;
    }
    return ZTK_OK;
}

zms_amf_type zms_amf_value_type(const zms_amf_value *v)
{
    return v ? v->type : ZMS_AMF_NULL;
}

const char *zms_amf_value_str(const zms_amf_value *v)
{
    return v && v->str ? v->str : "";
}

double zms_amf_value_num(const zms_amf_value *v)
{
    return v ? v->num : 0;
}

const zms_amf_value *zms_amf_object_get(const zms_amf_value *obj, const char *key)
{
    if (!obj || !key) {
        return NULL;
    }
    for (unsigned i = 0; i < obj->pair_count; ++i) {
        if (strcmp(obj->pairs[i].key, key) == 0) {
            return obj->pairs[i].val;
        }
    }
    return NULL;
}

size_t zms_amf_encode_string(uint8_t *out, size_t cap, const char *s)
{
    size_t slen = s ? strlen(s) : 0;
    if (cap < 3 + slen) {
        return 0;
    }
    out[0] = ZMS_AMF_STRING;
    out[1] = (uint8_t)((slen >> 8) & 0xff);
    out[2] = (uint8_t)(slen & 0xff);
    if (slen) {
        memcpy(out + 3, s, slen);
    }
    return 3 + slen;
}

size_t zms_amf_encode_strict_array_numbers(uint8_t *out, size_t cap, const double *vals,
                                           size_t count)
{
    size_t pos;
    size_t i;

    if (!out || count > 0x7fffffffu) {
        return 0;
    }
    if (cap < 5 + count * 9) {
        return 0;
    }
    out[0] = ZMS_AMF_STRICT_ARRAY;
    out[1] = (uint8_t)((count >> 24) & 0xff);
    out[2] = (uint8_t)((count >> 16) & 0xff);
    out[3] = (uint8_t)((count >> 8) & 0xff);
    out[4] = (uint8_t)(count & 0xff);
    pos = 5;
    for (i = 0; i < count; ++i) {
        size_t n = zms_amf_encode_number(out + pos, cap - pos, vals ? vals[i] : 0.0);
        if (n == 0) {
            return 0;
        }
        pos += n;
    }
    return pos;
}

size_t zms_amf_encode_number(uint8_t *out, size_t cap, double n)
{
    static const double k_endian_probe = 1.0;
    const uint8_t *src;

    if (cap < 9) {
        return 0;
    }
    out[0] = ZMS_AMF_NUMBER;
    src = (const uint8_t *)&n;
    if (*(const char *)&k_endian_probe == 0) {
        out[1] = src[7];
        out[2] = src[6];
        out[3] = src[5];
        out[4] = src[4];
        out[5] = src[3];
        out[6] = src[2];
        out[7] = src[1];
        out[8] = src[0];
    } else {
        memcpy(out + 1, &n, 8);
    }
    return 9;
}

size_t zms_amf_encode_null(uint8_t *out, size_t cap)
{
    if (cap < 1) {
        return 0;
    }
    out[0] = ZMS_AMF_NULL;
    return 1;
}

size_t zms_amf_encode_object_end(uint8_t *out, size_t cap)
{
    if (cap < 3) {
        return 0;
    }
    out[0] = 0;
    out[1] = 0;
    out[2] = 9;
    return 3;
}
