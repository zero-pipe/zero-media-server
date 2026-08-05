#include "zms/util/sha1.h"
#include <string.h>

#define SHA1_ROL(v, n) (((v) << (n)) | ((v) >> (32 - (n))))

static void sha1_transform(uint32_t state[5], const uint8_t block[64])
{
    uint32_t w[80];
    uint32_t a, b, c, d, e, f, k, t;
    int i;

    for (i = 0; i < 16; ++i) {
        w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
    }
    for (i = 16; i < 80; ++i) {
        w[i] = SHA1_ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    a = state[0];
    b = state[1];
    c = state[2];
    d = state[3];
    e = state[4];

    for (i = 0; i < 80; ++i) {
        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999U;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1U;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCU;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6U;
        }
        t = SHA1_ROL(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = SHA1_ROL(b, 30);
        b = a;
        a = t;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

void zms_sha1_init(zms_sha1_ctx *ctx)
{
    if (!ctx) {
        return;
    }
    ctx->state[0] = 0x67452301U;
    ctx->state[1] = 0xEFCDAB89U;
    ctx->state[2] = 0x98BADCFEU;
    ctx->state[3] = 0x10325476U;
    ctx->state[4] = 0xC3D2E1F0U;
    ctx->count[0] = ctx->count[1] = 0;
}

void zms_sha1_update(zms_sha1_ctx *ctx, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t i, idx, part;

    if (!ctx || !data || len == 0) {
        return;
    }

    idx = (ctx->count[0] >> 3) & 63;
    ctx->count[0] += (uint32_t)(len << 3);
    if (ctx->count[0] < (uint32_t)(len << 3)) {
        ctx->count[1]++;
    }
    ctx->count[1] += (uint32_t)(len >> 29);

    part = 64 - idx;
    if (len >= part) {
        memcpy(&ctx->buffer[idx], p, part);
        sha1_transform(ctx->state, ctx->buffer);
        for (i = part; i + 63 < len; i += 64) {
            sha1_transform(ctx->state, p + i);
        }
        idx = 0;
    } else {
        i = 0;
    }
    memcpy(&ctx->buffer[idx], p + i, len - i);
}

void zms_sha1_final(zms_sha1_ctx *ctx, uint8_t digest[20])
{
    uint8_t finalcount[8];
    uint8_t c = 0x80;
    int i;

    if (!ctx || !digest) {
        return;
    }

    for (i = 0; i < 8; ++i) {
        finalcount[i] = (uint8_t)((ctx->count[(i >= 4) ? 1 : 0] >> ((3 - (i & 3)) * 8)) & 255);
    }
    zms_sha1_update(ctx, &c, 1);
    while ((ctx->count[0] & 504) != 448) {
        zms_sha1_update(ctx, &c, 1);
    }
    zms_sha1_update(ctx, finalcount, 8);

    for (i = 0; i < 20; ++i) {
        digest[i] = (uint8_t)((ctx->state[i >> 2] >> ((3 - (i & 3)) * 8)) & 255);
    }
}
