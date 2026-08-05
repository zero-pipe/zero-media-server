#include "zms/media/codec/h264/h264_es.h"
#include "mpeg4-avc.h"
#include <string.h>

static int h264_nal_type(uint8_t b)
{
    return b & 0x1f;
}

static int h264_bit_get(const uint8_t *nal, size_t len, size_t *bitpos)
{
    if (*bitpos >= len * 8) {
        return 0;
    }
    size_t byte = *bitpos / 8;
    int off = 7 - (int)(*bitpos % 8);
    (*bitpos)++;
    return (nal[byte] >> off) & 1;
}

static unsigned h264_read_ue(const uint8_t *nal, size_t len, size_t *bitpos)
{
    int zeros = 0;

    while (!h264_bit_get(nal, len, bitpos) && *bitpos < len * 8) {
        zeros++;
    }
    unsigned val = 0;
    int i;

    for (i = 0; i < zeros; ++i) {
        val = (val << 1) | (unsigned)h264_bit_get(nal, len, bitpos);
    }
    return val + (1u << zeros) - 1u;
}

static int h264_nal_is_intra_i(const uint8_t *nal, size_t len)
{
    size_t bitpos;

    if (!nal || len < 3) {
        return 0;
    }
    if (h264_nal_type(nal[0]) == 5) {
        return 1;
    }
    if (h264_nal_type(nal[0]) != 1) {
        return 0;
    }
    bitpos = 8;
    (void)h264_read_ue(nal, len, &bitpos);
    {
        unsigned st = h264_read_ue(nal, len, &bitpos);
        unsigned mod = st % 5u;

        return mod == 2u || mod == 4u;
    }
}

typedef struct {
    int found;
} h264_scan_ctx;

typedef struct {
    const uint8_t *sps;
    const uint8_t *pps;
    size_t sps_len;
    size_t pps_len;
    int got;
} h264_param_ctx;

typedef struct {
    uint8_t *out;
    size_t cap;
    size_t pos;
    int err;
} h264_vcl_ctx;

static void h264_scan_sync_key(void *param, const uint8_t *nalu, size_t bytes)
{
    h264_scan_ctx *c = (h264_scan_ctx *)param;
    int t;

    if (!c || c->found || !nalu || bytes == 0) {
        return;
    }
    t = h264_nal_type(nalu[0]);
    if (t >= 1 && t <= 5 && h264_nal_is_intra_i(nalu, bytes)) {
        c->found = 1;
    }
}

static void h264_scan_idr(void *param, const uint8_t *nalu, size_t bytes)
{
    h264_scan_ctx *c = (h264_scan_ctx *)param;

    if (!c || c->found || !nalu || bytes == 0) {
        return;
    }
    if (h264_nal_type(nalu[0]) == 5) {
        c->found = 1;
    }
}

static void h264_scan_params(void *param, const uint8_t *nalu, size_t bytes)
{
    h264_param_ctx *c = (h264_param_ctx *)param;
    int t;

    if (!c || !nalu || bytes == 0) {
        return;
    }
    t = h264_nal_type(nalu[0]);
    if (t == 7) {
        c->sps = nalu;
        c->sps_len = bytes;
        c->got |= 1;
    } else if (t == 8) {
        c->pps = nalu;
        c->pps_len = bytes;
        c->got |= 2;
    }
}

static void h264_append_vcl(void *param, const uint8_t *nalu, size_t bytes)
{
    h264_vcl_ctx *c = (h264_vcl_ctx *)param;
    int t;

    if (!c || c->err || !nalu || bytes == 0) {
        return;
    }
    t = h264_nal_type(nalu[0]);
    if (t < 1 || t > 5) {
        return;
    }
    if (c->pos + 4 + bytes > c->cap) {
        c->err = 1;
        return;
    }
    c->out[c->pos++] = 0;
    c->out[c->pos++] = 0;
    c->out[c->pos++] = 0;
    c->out[c->pos++] = 1;
    memcpy(c->out + c->pos, nalu, bytes);
    c->pos += bytes;
}

static uint32_t h264_avcc_read_be_size(const uint8_t *p, int nalu_bytes)
{
    uint32_t n = 0;
    int i;

    for (i = 0; i < nalu_bytes; ++i) {
        n = (n << 8) + p[i];
    }
    return n;
}

static int h264_avcc_sample_valid(const uint8_t *p, size_t len, int nalu_bytes)
{
    size_t off = 0;

    if (!p || nalu_bytes < 1 || nalu_bytes > 4 || len < (size_t)nalu_bytes) {
        return 0;
    }
    while (off + (size_t)nalu_bytes <= len) {
        uint32_t n = h264_avcc_read_be_size(p + off, nalu_bytes);
        off += (size_t)nalu_bytes;
        if (n == 0 || off + n > len) {
            return 0;
        }
        off += n;
    }
    return off == len;
}

static int h264_es_is_annexb_sample(const uint8_t *p, size_t len, int avcc_nalu_bytes)
{
    if (!p || len < 3) {
        return 0;
    }
    if (p[0] == 0 && p[1] == 0 && p[2] == 1) {
        return 1;
    }
    if (len >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) {
        if (avcc_nalu_bytes > 0 && h264_avcc_sample_valid(p, len, avcc_nalu_bytes)) {
            return 0;
        }
        return 1;
    }
    return 0;
}

static int h264_avcc_nalu_bytes(const uint8_t *data, size_t len)
{
    int nalu;

    for (nalu = 4; nalu >= 1; --nalu) {
        if (h264_avcc_sample_valid(data, len, nalu)) {
            return nalu;
        }
    }
    return 0;
}

typedef struct {
    const uint8_t *sps;
    const uint8_t *pps;
    size_t sps_len;
    size_t pps_len;
    int got;
} h264_avcc_param_ctx;

static void h264_avcc_scan_nal(void *param, const uint8_t *nal, size_t bytes)
{
    h264_avcc_param_ctx *c = (h264_avcc_param_ctx *)param;
    int t;

    if (!c || !nal || bytes == 0) {
        return;
    }
    t = h264_nal_type(nal[0]);
    if (t == 7) {
        c->sps = nal;
        c->sps_len = bytes;
        c->got |= 1;
    } else if (t == 8) {
        c->pps = nal;
        c->pps_len = bytes;
        c->got |= 2;
    }
}

static int h264_avcc_es_extract_sps_pps(const uint8_t *data, size_t len, const uint8_t **sps,
                                        size_t *sps_len, const uint8_t **pps, size_t *pps_len)
{
    h264_avcc_param_ctx ctx;
    int nalu_bytes;
    size_t off = 0;

    if (!data || len < 5 || !sps || !sps_len || !pps || !pps_len) {
        return 0;
    }
    nalu_bytes = h264_avcc_nalu_bytes(data, len);
    if (nalu_bytes <= 0) {
        return 0;
    }
    memset(&ctx, 0, sizeof(ctx));
    while (off + (size_t)nalu_bytes <= len) {
        uint32_t n = h264_avcc_read_be_size(data + off, nalu_bytes);
        const uint8_t *nal;

        off += (size_t)nalu_bytes;
        if (n == 0 || off + n > len) {
            break;
        }
        nal = data + off;
        h264_avcc_scan_nal(&ctx, nal, n);
        off += n;
    }
    *sps = ctx.sps;
    *pps = ctx.pps;
    *sps_len = ctx.sps_len;
    *pps_len = ctx.pps_len;
    return ctx.got == 3;
}

static int h264_avcc_es_has_slice(const uint8_t *data, size_t len)
{
    int nalu_bytes;
    size_t off = 0;

    if (!data || len < 5) {
        return 0;
    }
    nalu_bytes = h264_avcc_nalu_bytes(data, len);
    if (nalu_bytes <= 0) {
        return 0;
    }
    while (off + (size_t)nalu_bytes <= len) {
        uint32_t n = h264_avcc_read_be_size(data + off, nalu_bytes);
        int t;

        off += (size_t)nalu_bytes;
        if (n == 0 || off + n > len) {
            break;
        }
        t = h264_nal_type(data[off]);
        if (t >= 1 && t <= 5) {
            return 1;
        }
        off += n;
    }
    return 0;
}

static int h264_annexb_has_nal_type(const uint8_t *annexb, size_t len, int want_type)
{
    const uint8_t *end = annexb + len;
    const uint8_t *p = annexb;

    if (!annexb || len < 5) {
        return 0;
    }
    while (p < end) {
        size_t start = 0;
        const uint8_t *nal;

        if (p + 4 <= end && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) {
            start = 4;
        } else if (p[0] == 0 && p[1] == 0 && p[2] == 1) {
            start = 3;
        } else {
            ++p;
            continue;
        }
        nal = p + start;
        if (nal >= end) {
            break;
        }
        if ((nal[0] & 0x1f) == want_type) {
            return 1;
        }
        p = nal + 1;
    }
    return 0;
}

int zms_h264_es_is_annexb(const uint8_t *data, size_t len)
{
    return h264_es_is_annexb_sample(data, len, 0);
}

int zms_h264_es_extract_sps_pps(const uint8_t *data, size_t len, const uint8_t **sps,
                                size_t *sps_len, const uint8_t **pps, size_t *pps_len)
{
    struct mpeg4_avc_t avc;

    if (!data || len == 0 || !sps || !sps_len || !pps || !pps_len) {
        return 0;
    }
    if (zms_h264_es_is_annexb(data, len)) {
        return zms_h264_annexb_extract_sps_pps(data, len, sps, sps_len, pps, pps_len);
    }
    if (h264_avcc_es_extract_sps_pps(data, len, sps, sps_len, pps, pps_len)) {
        return 1;
    }
    memset(&avc, 0, sizeof(avc));
    if (mpeg4_avc_from_nalu(data, len, &avc) > 0 && avc.nb_sps > 0 && avc.nb_pps > 0 &&
        avc.sps[0].data && avc.pps[0].data) {
        *sps = avc.sps[0].data;
        *sps_len = avc.sps[0].bytes;
        *pps = avc.pps[0].data;
        *pps_len = avc.pps[0].bytes;
        return 1;
    }
    return 0;
}

int zms_h264_es_has_slice(const uint8_t *data, size_t len)
{
    if (!data || len < 5) {
        return 0;
    }
    if (zms_h264_es_is_annexb(data, len)) {
        return h264_annexb_has_nal_type(data, len, 1) || h264_annexb_has_nal_type(data, len, 5);
    }
    return h264_avcc_es_has_slice(data, len);
}

ztk_err_t zms_h264_es_to_annexb(const uint8_t *data, size_t len, uint8_t *out, size_t cap,
                                size_t *out_len)
{
    struct mpeg4_avc_t avc;
    int nalu_bytes;
    int n;

    if (!out || !out_len) {
        return ZTK_ERR_INVALID;
    }
    *out_len = 0;
    if (!data || len == 0) {
        return ZTK_ERR_INVALID;
    }
    if (zms_h264_es_is_annexb(data, len)) {
        if (len > cap) {
            return ZTK_ERR_BUFFER_TOO_SMALL;
        }
        if (out != data) {
            memcpy(out, data, len);
        }
        *out_len = len;
        return ZTK_OK;
    }
    nalu_bytes = h264_avcc_nalu_bytes(data, len);
    if (nalu_bytes <= 0) {
        return ZTK_ERR_INVALID;
    }
    memset(&avc, 0, sizeof(avc));
    avc.nalu = (uint8_t)nalu_bytes;
    n = h264_mp4toannexb(&avc, data, len, out, cap);
    if (n <= 0) {
        return ZTK_ERR_INVALID;
    }
    *out_len = (size_t)n;
    return ZTK_OK;
}

static size_t annexb_write_nal(uint8_t *out, size_t cap, size_t pos, const uint8_t *nal, size_t len)
{
    if (!nal || len == 0 || pos + 4 + len > cap) {
        return 0;
    }
    out[pos++] = 0;
    out[pos++] = 0;
    out[pos++] = 0;
    out[pos++] = 1;
    memcpy(out + pos, nal, len);
    return pos + len;
}

int zms_h264_annexb_is_sync_key(const uint8_t *annexb, size_t len)
{
    h264_scan_ctx ctx;

    if (!annexb || len < 4) {
        return 0;
    }
    memset(&ctx, 0, sizeof(ctx));
    mpeg4_h264_annexb_nalu(annexb, len, h264_scan_sync_key, &ctx);
    return ctx.found;
}

int zms_h264_annexb_is_idr(const uint8_t *annexb, size_t len)
{
    h264_scan_ctx ctx;

    if (!annexb || len < 4) {
        return 0;
    }
    memset(&ctx, 0, sizeof(ctx));
    mpeg4_h264_annexb_nalu(annexb, len, h264_scan_idr, &ctx);
    return ctx.found;
}

typedef struct {
    int found;
    int first_mb;
} h264_first_mb_ctx;

static void h264_scan_first_vcl(void *param, const uint8_t *nalu, size_t bytes)
{
    h264_first_mb_ctx *c = (h264_first_mb_ctx *)param;
    int t;

    if (!c || c->found || !nalu || bytes == 0) {
        return;
    }
    t = h264_nal_type(nalu[0]);
    if (t >= 1 && t <= 5) {
        c->found = 1;
        c->first_mb = (bytes >= 2 && (nalu[1] & 0x80)) != 0;
    }
}

int zms_h264_annexb_first_slice(const uint8_t *annexb, size_t len)
{
    h264_first_mb_ctx ctx;

    if (!annexb || len < 4) {
        return 1;
    }
    memset(&ctx, 0, sizeof(ctx));
    mpeg4_h264_annexb_nalu(annexb, len, h264_scan_first_vcl, &ctx);
    return !ctx.found || ctx.first_mb;
}

int zms_h264_es_starts_access_unit(const uint8_t *es, size_t len)
{
    if (!es || len < 4) {
        return 1;
    }
    if (!zms_h264_es_has_slice(es, len)) {
        return 0;
    }
    if (!zms_h264_es_is_annexb(es, len)) {
        return 1;
    }
    return zms_h264_annexb_first_slice(es, len);
}

int zms_h264_annexb_extract_sps_pps(const uint8_t *annexb, size_t len, const uint8_t **sps,
                                    size_t *sps_len, const uint8_t **pps, size_t *pps_len)
{
    h264_param_ctx ctx;

    if (!annexb || len == 0 || !sps || !sps_len || !pps || !pps_len) {
        return 0;
    }
    memset(&ctx, 0, sizeof(ctx));
    mpeg4_h264_annexb_nalu(annexb, len, h264_scan_params, &ctx);
    *sps = ctx.sps;
    *pps = ctx.pps;
    *sps_len = ctx.sps_len;
    *pps_len = ctx.pps_len;
    return ctx.got == 3;
}

ztk_err_t zms_h264_annexb_prepend_sps_pps(const uint8_t *sps, size_t sps_len, const uint8_t *pps,
                                          size_t pps_len, const uint8_t *body, size_t body_len,
                                          uint8_t *out, size_t cap, size_t *out_len)
{
    if (!out || !out_len) {
        return ZTK_ERR_INVALID;
    }
    if (!body || body_len == 0) {
        *out_len = 0;
        return ZTK_ERR_INVALID;
    }
    {
        const uint8_t *bs = NULL;
        const uint8_t *bp = NULL;
        size_t bsl = 0;
        size_t bpl = 0;

        if (zms_h264_annexb_extract_sps_pps(body, body_len, &bs, &bsl, &bp, &bpl)) {
            if (body_len > cap) {
                return ZTK_ERR_BUFFER_TOO_SMALL;
            }
            memcpy(out, body, body_len);
            *out_len = body_len;
            return ZTK_OK;
        }
    }
    if (!sps || !pps || sps_len == 0 || pps_len == 0) {
        if (body_len > cap) {
            return ZTK_ERR_BUFFER_TOO_SMALL;
        }
        if (body != out) {
            memcpy(out, body, body_len);
        } else {
            memmove(out, body, body_len);
        }
        *out_len = body_len;
        return ZTK_OK;
    }

    {
        size_t prefix_max = (4 + sps_len) + (4 + pps_len);

        if (prefix_max + body_len > cap) {
            return ZTK_ERR_BUFFER_TOO_SMALL;
        }

        if (body == out || (body >= out && body < out + cap && body + body_len > out)) {
            memmove(out + prefix_max, body, body_len);
            body = out + prefix_max;
        }

        size_t pos = 0;

        pos = annexb_write_nal(out, cap, pos, sps, sps_len);
        if (pos == 0) {
            return ZTK_ERR_BUFFER_TOO_SMALL;
        }
        pos = annexb_write_nal(out, cap, pos, pps, pps_len);
        if (pos == 0) {
            return ZTK_ERR_BUFFER_TOO_SMALL;
        }
        if (body != out + prefix_max) {
            if (pos + body_len > cap) {
                return ZTK_ERR_BUFFER_TOO_SMALL;
            }
            memcpy(out + pos, body, body_len);
        }
        *out_len = pos + body_len;
        return ZTK_OK;
    }
}

int zms_h264_avcc_extract_sps_pps(const uint8_t *avcc, size_t len, const uint8_t **sps,
                                  size_t *sps_len, const uint8_t **pps, size_t *pps_len)
{
    struct mpeg4_avc_t avc;

    if (!avcc || len < 7 || !sps || !sps_len || !pps || !pps_len) {
        return 0;
    }
    memset(&avc, 0, sizeof(avc));
    if (mpeg4_avc_decoder_configuration_record_load(avcc, len, &avc) <= 0) {
        return 0;
    }
    if (avc.nb_sps == 0 || !avc.sps[0].data || avc.sps[0].bytes == 0) {
        return 0;
    }
    if (avc.nb_pps == 0 || !avc.pps[0].data || avc.pps[0].bytes == 0) {
        return 0;
    }
    *sps = avc.sps[0].data;
    *sps_len = avc.sps[0].bytes;
    *pps = avc.pps[0].data;
    *pps_len = avc.pps[0].bytes;
    return 1;
}

ztk_err_t zms_h264_annexb_copy_vcl(const uint8_t *annexb, size_t len, uint8_t *out, size_t cap,
                                   size_t *out_len)
{
    h264_vcl_ctx ctx;

    if (!out || !out_len) {
        return ZTK_ERR_INVALID;
    }
    *out_len = 0;
    if (!annexb || len < 5) {
        return ZTK_ERR_INVALID;
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.out = out;
    ctx.cap = cap;
    mpeg4_h264_annexb_nalu(annexb, len, h264_append_vcl, &ctx);
    if (ctx.err || ctx.pos == 0) {
        return ctx.err ? ZTK_ERR_BUFFER_TOO_SMALL : ZTK_ERR_INVALID;
    }
    *out_len = ctx.pos;
    return ZTK_OK;
}
