#include "zms/util/md5.h"
#include <stdio.h>
#include <string.h>

typedef struct {
    uint32_t state[4];
    uint32_t count[2];
    uint8_t buffer[64];
} md5_ctx;

static uint32_t zms_md5_f(uint32_t x, uint32_t y, uint32_t z)
{
    return (x & y) | (~x & z);
}

static uint32_t zms_md5_g(uint32_t x, uint32_t y, uint32_t z)
{
    return (x & z) | (y & ~z);
}

static uint32_t zms_md5_h(uint32_t x, uint32_t y, uint32_t z)
{
    return x ^ y ^ z;
}

static uint32_t zms_md5_i(uint32_t x, uint32_t y, uint32_t z)
{
    return y ^ (x | ~z);
}

static uint32_t rol(uint32_t v, unsigned n)
{
    return (v << n) | (v >> (32 - n));
}

static void md5_step(uint32_t *a, uint32_t b, uint32_t c, uint32_t d, uint32_t k, uint32_t s,
                     uint32_t t, uint32_t (*fn)(uint32_t, uint32_t, uint32_t))
{
    *a += fn(b, c, d) + k + t;
    *a = rol(*a, s);
    *a += b;
}

static void md5_decode(uint32_t *out, const uint8_t *in)
{
    for (int i = 0; i < 16; ++i) {
        out[i] = (uint32_t)in[i * 4] | ((uint32_t)in[i * 4 + 1] << 8) |
                 ((uint32_t)in[i * 4 + 2] << 16) | ((uint32_t)in[i * 4 + 3] << 24);
    }
}

static void md5_transform(uint32_t state[4], const uint8_t block[64])
{
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], blk[16];
    md5_decode(blk, block);

    md5_step(&a, b, c, d, blk[0], 7, 0xd76aa478u, zms_md5_f);
    md5_step(&d, a, b, c, blk[1], 12, 0xe8c7b756u, zms_md5_f);
    md5_step(&c, d, a, b, blk[2], 17, 0x242070dbu, zms_md5_f);
    md5_step(&b, c, d, a, blk[3], 22, 0xc1bdceeeu, zms_md5_f);
    md5_step(&a, b, c, d, blk[4], 7, 0xf57c0fafu, zms_md5_f);
    md5_step(&d, a, b, c, blk[5], 12, 0x4787c62au, zms_md5_f);
    md5_step(&c, d, a, b, blk[6], 17, 0xa8304613u, zms_md5_f);
    md5_step(&b, c, d, a, blk[7], 22, 0xfd469501u, zms_md5_f);
    md5_step(&a, b, c, d, blk[8], 7, 0x698098d8u, zms_md5_f);
    md5_step(&d, a, b, c, blk[9], 12, 0x8b44f7afu, zms_md5_f);
    md5_step(&c, d, a, b, blk[10], 17, 0xffff5bb1u, zms_md5_f);
    md5_step(&b, c, d, a, blk[11], 22, 0x895cd7beu, zms_md5_f);
    md5_step(&a, b, c, d, blk[12], 7, 0x6b901122u, zms_md5_f);
    md5_step(&d, a, b, c, blk[13], 12, 0xfd987193u, zms_md5_f);
    md5_step(&c, d, a, b, blk[14], 17, 0xa679438eu, zms_md5_f);
    md5_step(&b, c, d, a, blk[15], 22, 0x49b40821u, zms_md5_f);

    md5_step(&a, b, c, d, blk[1], 5, 0xf61e2562u, zms_md5_g);
    md5_step(&d, a, b, c, blk[6], 9, 0xc040b340u, zms_md5_g);
    md5_step(&c, d, a, b, blk[11], 14, 0x265e5a51u, zms_md5_g);
    md5_step(&b, c, d, a, blk[0], 20, 0xe9b6c7aau, zms_md5_g);
    md5_step(&a, b, c, d, blk[5], 5, 0xd62f105du, zms_md5_g);
    md5_step(&d, a, b, c, blk[10], 9, 0x02441453u, zms_md5_g);
    md5_step(&c, d, a, b, blk[15], 14, 0xd8a1e681u, zms_md5_g);
    md5_step(&b, c, d, a, blk[4], 20, 0xe7d3fbc8u, zms_md5_g);
    md5_step(&a, b, c, d, blk[9], 5, 0x21e1cde6u, zms_md5_g);
    md5_step(&d, a, b, c, blk[14], 9, 0xc33707d6u, zms_md5_g);
    md5_step(&c, d, a, b, blk[3], 14, 0xf4d50d87u, zms_md5_g);
    md5_step(&b, c, d, a, blk[8], 20, 0x455a14edu, zms_md5_g);
    md5_step(&a, b, c, d, blk[13], 5, 0xa9e3e905u, zms_md5_g);
    md5_step(&d, a, b, c, blk[2], 9, 0xfcefa3f8u, zms_md5_g);
    md5_step(&c, d, a, b, blk[7], 14, 0x676f02d9u, zms_md5_g);
    md5_step(&b, c, d, a, blk[12], 20, 0x8d2a4c8au, zms_md5_g);

    md5_step(&a, b, c, d, blk[5], 4, 0xfffa3942u, zms_md5_h);
    md5_step(&d, a, b, c, blk[8], 11, 0x8771f681u, zms_md5_h);
    md5_step(&c, d, a, b, blk[11], 16, 0x6d9d6122u, zms_md5_h);
    md5_step(&b, c, d, a, blk[14], 23, 0xfde5380cu, zms_md5_h);
    md5_step(&a, b, c, d, blk[1], 4, 0xa4beea44u, zms_md5_h);
    md5_step(&d, a, b, c, blk[4], 11, 0x4bdecfa9u, zms_md5_h);
    md5_step(&c, d, a, b, blk[7], 16, 0xf6bb4b60u, zms_md5_h);
    md5_step(&b, c, d, a, blk[10], 23, 0xbebfbc70u, zms_md5_h);
    md5_step(&a, b, c, d, blk[13], 4, 0x289b7ec6u, zms_md5_h);
    md5_step(&d, a, b, c, blk[0], 11, 0xeaa127fau, zms_md5_h);
    md5_step(&c, d, a, b, blk[3], 16, 0xd4ef3085u, zms_md5_h);
    md5_step(&b, c, d, a, blk[6], 23, 0x04881d05u, zms_md5_h);
    md5_step(&a, b, c, d, blk[9], 4, 0xd9d4d039u, zms_md5_h);
    md5_step(&d, a, b, c, blk[12], 11, 0xe6db99e5u, zms_md5_h);
    md5_step(&c, d, a, b, blk[15], 16, 0x1fa27cf8u, zms_md5_h);
    md5_step(&b, c, d, a, blk[2], 23, 0xc4ac5665u, zms_md5_h);

    md5_step(&a, b, c, d, blk[0], 6, 0xf4292244u, zms_md5_i);
    md5_step(&d, a, b, c, blk[7], 10, 0x432aff97u, zms_md5_i);
    md5_step(&c, d, a, b, blk[14], 15, 0xab9423a7u, zms_md5_i);
    md5_step(&b, c, d, a, blk[5], 21, 0xfc93a039u, zms_md5_i);
    md5_step(&a, b, c, d, blk[12], 6, 0x655b59c3u, zms_md5_i);
    md5_step(&d, a, b, c, blk[3], 10, 0x8f0ccc92u, zms_md5_i);
    md5_step(&c, d, a, b, blk[10], 15, 0xffeff47du, zms_md5_i);
    md5_step(&b, c, d, a, blk[1], 21, 0x85845dd1u, zms_md5_i);
    md5_step(&a, b, c, d, blk[8], 6, 0x6fa87e4fu, zms_md5_i);
    md5_step(&d, a, b, c, blk[15], 10, 0xfe2ce6e0u, zms_md5_i);
    md5_step(&c, d, a, b, blk[6], 15, 0xa3014314u, zms_md5_i);
    md5_step(&b, c, d, a, blk[13], 21, 0x4e0811a1u, zms_md5_i);
    md5_step(&a, b, c, d, blk[4], 6, 0xf7537e82u, zms_md5_i);
    md5_step(&d, a, b, c, blk[11], 10, 0xbd3af235u, zms_md5_i);
    md5_step(&c, d, a, b, blk[2], 15, 0x2ad7d2bbu, zms_md5_i);
    md5_step(&b, c, d, a, blk[9], 21, 0xeb86d391u, zms_md5_i);

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

static void md5_init(md5_ctx *ctx)
{
    ctx->count[0] = ctx->count[1] = 0;
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xefcdab89;
    ctx->state[2] = 0x98badcfe;
    ctx->state[3] = 0x10325476;
}

static void md5_update(md5_ctx *ctx, const uint8_t *input, size_t len)
{
    size_t i, index = (size_t)((ctx->count[0] >> 3) & 0x3f);
    if ((ctx->count[0] += (uint32_t)(len << 3)) < (uint32_t)(len << 3)) {
        ctx->count[1]++;
    }
    ctx->count[1] += (uint32_t)(len >> 29);
    size_t part = 64 - index;
    if (len >= part) {
        memcpy(&ctx->buffer[index], input, part);
        md5_transform(ctx->state, ctx->buffer);
        for (i = part; i + 63 < len; i += 64) {
            md5_transform(ctx->state, &input[i]);
        }
        index = 0;
    } else {
        i = 0;
    }
    memcpy(&ctx->buffer[index], &input[i], len - i);
}

static void md5_final(md5_ctx *ctx, uint8_t digest[16])
{
    static const uint8_t pad[64] = {0x80};
    uint8_t bits[8];
    size_t index = (size_t)((ctx->count[0] >> 3) & 0x3f);
    size_t pad_len = (index < 56) ? (56 - index) : (120 - index);
    for (int i = 0; i < 8; ++i) {
        bits[i] = (uint8_t)((ctx->count[i >> 2] >> ((i & 3) * 8)) & 0xff);
    }
    md5_update(ctx, pad, pad_len);
    md5_update(ctx, bits, 8);
    for (int i = 0; i < 4; ++i) {
        digest[i * 4] = (uint8_t)(ctx->state[i] & 0xff);
        digest[i * 4 + 1] = (uint8_t)((ctx->state[i] >> 8) & 0xff);
        digest[i * 4 + 2] = (uint8_t)((ctx->state[i] >> 16) & 0xff);
        digest[i * 4 + 3] = (uint8_t)((ctx->state[i] >> 24) & 0xff);
    }
}

void zms_md5(const uint8_t *data, size_t len, uint8_t digest[16])
{
    md5_ctx ctx;
    md5_init(&ctx);
    if (data && len) {
        md5_update(&ctx, data, len);
    }
    md5_final(&ctx, digest);
}

void zms_md5_hex(const uint8_t *data, size_t len, char out[33])
{
    uint8_t d[16];
    zms_md5(data, len, d);
    for (int i = 0; i < 16; ++i) {
        sprintf(out + i * 2, "%02x", d[i]);
    }
    out[32] = '\0';
}

void zms_md5_hex_str(const char *s, char out[33])
{
    zms_md5_hex((const uint8_t *)(s ? s : ""), s ? strlen(s) : 0, out);
}
