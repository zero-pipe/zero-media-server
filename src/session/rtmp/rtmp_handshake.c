#include "session/rtmp/rtmp_handshake.h"
#include "session/rtmp/rtmp_hs_crypto.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define HS_SIZE 1536
#define HS_BLOCK 764

static const uint8_t k_fp_key[] = {
    'G',  'e',  'n',  'u',  'i',  'n',  'e',  ' ',  'A',  'd',  'o',  'b',  'e',  ' ',  'F',  'l',
    'a',  's',  'h',  ' ',  'P',  'l',  'a',  'y',  'e',  'r',  ' ',  '0',  '0',  '1',  0xF0, 0xEE,
    0xC2, 0x4A, 0x80, 0x68, 0xBE, 0xE8, 0x2E, 0x00, 0xD0, 0xD1, 0x02, 0x9E, 0x7E, 0x57, 0x6E, 0xEC,
    0x5D, 0x2D, 0x29, 0x80, 0x6F, 0xAB, 0x93, 0xB8, 0xE6, 0x36, 0xCF, 0xEB, 0x31, 0xAE,
};

static const uint8_t k_fms_key[] = {
    'G',  'e',  'n',  'u',  'i',  'n',  'e',  ' ',  'A',  'd',  'o',  'b',  'e',  ' ',
    'F',  'l',  'a',  's',  'h',  ' ',  'M',  'e',  'd',  'i',  'a',  ' ',  'S',  'e',
    'r',  'v',  'e',  'r',  ' ',  '0',  '0',  '1',  0xF0, 0xEE, 0xC2, 0x4A, 0x80, 0x68,
    0xBE, 0xE8, 0x2E, 0x00, 0xD0, 0xD1, 0x02, 0x9E, 0x7E, 0x57, 0x6E, 0xEC, 0x5D, 0x2D,
    0x29, 0x80, 0x6F, 0xAB, 0x93, 0xB8, 0xE6, 0x36, 0xCF, 0xEB, 0x31, 0xAE,
};

static void hs_random(uint8_t *out, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        out[i] = (uint8_t)(rand() & 0xff);
    }
}

static int digest_valid_offset(int32_t off)
{
    uint8_t *p = (uint8_t *)&off;
    return ((int)p[0] + p[1] + p[2] + p[3]) % (HS_BLOCK - 32 - 4);
}

static int key_valid_offset(int32_t off)
{
    uint8_t *p = (uint8_t *)&off;
    return ((int)p[0] + p[1] + p[2] + p[3]) % (HS_BLOCK - 128 - 4);
}

typedef struct {
    int32_t offset;
    uint8_t *r0;
    int r0_len;
    uint8_t digest[32];
    uint8_t *r1;
    int r1_len;
} hs_digest;

typedef struct {
    int32_t offset;
    uint8_t *r0;
    int r0_len;
    uint8_t key[128];
    uint8_t *r1;
    int r1_len;
} hs_key;

typedef struct {
    uint32_t time;
    uint32_t version;
    hs_digest digest;
    hs_key key;
    int schema; /* 0=key-digest, 1=digest-key */
} hs_c1s1;

static void digest_free(hs_digest *d)
{
    free(d->r0);
    free(d->r1);
    d->r0 = d->r1 = NULL;
    d->r0_len = d->r1_len = 0;
}

static void key_free(hs_key *k)
{
    free(k->r0);
    free(k->r1);
    k->r0 = k->r1 = NULL;
    k->r0_len = k->r1_len = 0;
}

static void hs_free(hs_c1s1 *p)
{
    digest_free(&p->digest);
    key_free(&p->key);
}

static int digest_parse(hs_digest *d, const uint8_t *block)
{
    d->offset = (int32_t)((uint32_t)block[0] << 24 | (uint32_t)block[1] << 16 |
                          (uint32_t)block[2] << 8 | block[3]);
    int vo = digest_valid_offset(d->offset);
    if (vo < 0 || vo > HS_BLOCK - 36) {
        return 0;
    }
    d->r0_len = vo;
    if (d->r0_len) {
        d->r0 = (uint8_t *)malloc((size_t)d->r0_len);
        if (!d->r0) {
            return 0;
        }
        memcpy(d->r0, block + 4, (size_t)d->r0_len);
    }
    memcpy(d->digest, block + 4 + d->r0_len, 32);
    d->r1_len = HS_BLOCK - 4 - d->r0_len - 32;
    if (d->r1_len > 0) {
        d->r1 = (uint8_t *)malloc((size_t)d->r1_len);
        if (!d->r1) {
            return 0;
        }
        memcpy(d->r1, block + 4 + d->r0_len + 32, (size_t)d->r1_len);
    }
    return 1;
}

static int key_parse(hs_key *k, const uint8_t *block)
{
    k->offset =
        (int32_t)((uint32_t)block[HS_BLOCK - 4] << 24 | (uint32_t)block[HS_BLOCK - 3] << 16 |
                  (uint32_t)block[HS_BLOCK - 2] << 8 | block[HS_BLOCK - 1]);
    int vo = key_valid_offset(k->offset);
    if (vo < 0 || vo > HS_BLOCK - 132) {
        return 0;
    }
    k->r0_len = vo;
    if (k->r0_len) {
        k->r0 = (uint8_t *)malloc((size_t)k->r0_len);
        if (!k->r0) {
            return 0;
        }
        memcpy(k->r0, block, (size_t)k->r0_len);
    }
    memcpy(k->key, block + k->r0_len, 128);
    k->r1_len = HS_BLOCK - k->r0_len - 128 - 4;
    if (k->r1_len > 0) {
        k->r1 = (uint8_t *)malloc((size_t)k->r1_len);
        if (!k->r1) {
            return 0;
        }
        memcpy(k->r1, block + k->r0_len + 128, (size_t)k->r1_len);
    }
    return 1;
}

static int hs_parse(hs_c1s1 *p, const uint8_t *buf, int schema)
{
    memset(p, 0, sizeof(*p));
    p->schema = schema;
    p->time =
        ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) | ((uint32_t)buf[2] << 8) | buf[3];
    p->version =
        ((uint32_t)buf[4] << 24) | ((uint32_t)buf[5] << 16) | ((uint32_t)buf[6] << 8) | buf[7];
    if (schema == 0) {
        if (!key_parse(&p->key, buf + 8)) {
            return 0;
        }
        if (!digest_parse(&p->digest, buf + 8 + HS_BLOCK)) {
            return 0;
        }
    } else {
        if (!digest_parse(&p->digest, buf + 8)) {
            return 0;
        }
        if (!key_parse(&p->key, buf + 8 + HS_BLOCK)) {
            return 0;
        }
    }
    return 1;
}

static size_t digest_copy(const hs_digest *d, uint8_t *out, int with_digest)
{
    size_t pos = 0;
    out[pos++] = (uint8_t)((d->offset >> 24) & 0xff);
    out[pos++] = (uint8_t)((d->offset >> 16) & 0xff);
    out[pos++] = (uint8_t)((d->offset >> 8) & 0xff);
    out[pos++] = (uint8_t)(d->offset & 0xff);
    if (d->r0_len > 0) {
        memcpy(out + pos, d->r0, (size_t)d->r0_len);
    }
    pos += (size_t)d->r0_len;
    if (with_digest) {
        memcpy(out + pos, d->digest, 32);
        pos += 32;
    }
    if (d->r1_len > 0) {
        memcpy(out + pos, d->r1, (size_t)d->r1_len);
        pos += (size_t)d->r1_len;
    }
    return pos;
}

static size_t key_copy(const hs_key *k, uint8_t *out)
{
    size_t pos = 0;
    if (k->r0_len > 0) {
        memcpy(out + pos, k->r0, (size_t)k->r0_len);
    }
    pos += (size_t)k->r0_len;
    memcpy(out + pos, k->key, 128);
    pos += 128;
    if (k->r1_len > 0) {
        memcpy(out + pos, k->r1, (size_t)k->r1_len);
        pos += (size_t)k->r1_len;
    }
    out[pos++] = (uint8_t)((k->offset >> 24) & 0xff);
    out[pos++] = (uint8_t)((k->offset >> 16) & 0xff);
    out[pos++] = (uint8_t)((k->offset >> 8) & 0xff);
    out[pos++] = (uint8_t)(k->offset & 0xff);
    return pos;
}

static size_t digest_write(const hs_digest *d, uint8_t *block, int with_digest)
{
    memset(block, 0, HS_BLOCK);
    return digest_copy(d, block, with_digest);
}

static size_t key_write(const hs_key *k, uint8_t *block)
{
    memset(block, 0, HS_BLOCK);
    return key_copy(k, block);
}

static int hs_joined(const hs_c1s1 *p, uint8_t *out, int with_digest)
{
    size_t n = 0;
    out[n++] = (uint8_t)((p->time >> 24) & 0xff);
    out[n++] = (uint8_t)((p->time >> 16) & 0xff);
    out[n++] = (uint8_t)((p->time >> 8) & 0xff);
    out[n++] = (uint8_t)(p->time & 0xff);
    out[n++] = (uint8_t)((p->version >> 24) & 0xff);
    out[n++] = (uint8_t)((p->version >> 16) & 0xff);
    out[n++] = (uint8_t)((p->version >> 8) & 0xff);
    out[n++] = (uint8_t)(p->version & 0xff);

    if (p->schema == 0) {
        n += key_copy(&p->key, out + n);
        n += digest_copy(&p->digest, out + n, with_digest);
    } else {
        n += digest_copy(&p->digest, out + n, with_digest);
        n += key_copy(&p->key, out + n);
    }
    return (int)n;
}

static int hs_dump(const hs_c1s1 *p, uint8_t *out)
{
    size_t n = 0;
    out[n++] = (uint8_t)((p->time >> 24) & 0xff);
    out[n++] = (uint8_t)((p->time >> 16) & 0xff);
    out[n++] = (uint8_t)((p->time >> 8) & 0xff);
    out[n++] = (uint8_t)(p->time & 0xff);
    out[n++] = (uint8_t)((p->version >> 24) & 0xff);
    out[n++] = (uint8_t)((p->version >> 16) & 0xff);
    out[n++] = (uint8_t)((p->version >> 8) & 0xff);
    out[n++] = (uint8_t)(p->version & 0xff);

    uint8_t block[HS_BLOCK];
    if (p->schema == 0) {
        key_write(&p->key, block);
        memcpy(out + n, block, HS_BLOCK);
        n += HS_BLOCK;
        digest_write(&p->digest, block, 1);
        memcpy(out + n, block, HS_BLOCK);
        n += HS_BLOCK;
    } else {
        digest_write(&p->digest, block, 1);
        memcpy(out + n, block, HS_BLOCK);
        n += HS_BLOCK;
        key_write(&p->key, block);
        memcpy(out + n, block, HS_BLOCK);
        n += HS_BLOCK;
    }
    return (int)n;
}

static int hs_calc_digest(const hs_c1s1 *p, const uint8_t *key, size_t key_len, uint8_t digest[32])
{
    uint8_t joined[HS_SIZE - 32];
    if (hs_joined(p, joined, 0) != (int)sizeof(joined)) {
        return 0;
    }
    zms_hs_hmac_sha256(key, key_len, joined, sizeof(joined), digest);
    return 1;
}

static int hs_validate_c1(const hs_c1s1 *p)
{
    uint8_t calc[32];
    if (!hs_calc_digest(p, k_fp_key, 30, calc)) {
        return 0;
    }
    return memcmp(calc, p->digest.digest, 32) == 0;
}

static void digest_init_random(hs_digest *d)
{
    digest_free(d);
    d->offset = (int32_t)rand();
    int vo = digest_valid_offset(d->offset);
    d->r0_len = vo;
    if (d->r0_len) {
        d->r0 = (uint8_t *)malloc((size_t)d->r0_len);
        if (d->r0) {
            hs_random(d->r0, (size_t)d->r0_len);
        }
    }
    d->r1_len = HS_BLOCK - 4 - d->r0_len - 32;
    if (d->r1_len > 0) {
        d->r1 = (uint8_t *)malloc((size_t)d->r1_len);
        if (d->r1) {
            hs_random(d->r1, (size_t)d->r1_len);
        }
    }
}

static void key_init_random(hs_key *k)
{
    key_free(k);
    k->offset = (int32_t)rand();
    int vo = key_valid_offset(k->offset);
    k->r0_len = vo;
    if (k->r0_len) {
        k->r0 = (uint8_t *)malloc((size_t)k->r0_len);
        if (k->r0) {
            hs_random(k->r0, (size_t)k->r0_len);
        }
    }
    hs_random(k->key, sizeof(k->key));
    k->r1_len = HS_BLOCK - k->r0_len - 128 - 4;
    if (k->r1_len > 0) {
        k->r1 = (uint8_t *)malloc((size_t)k->r1_len);
        if (k->r1) {
            hs_random(k->r1, (size_t)k->r1_len);
        }
    }
}

static int hs_create_s1(hs_c1s1 *s1, const hs_c1s1 *c1)
{
    memset(s1, 0, sizeof(*s1));
    s1->schema = c1->schema;
    s1->time = (uint32_t)time(NULL);
    s1->version = 0x01000504;
    digest_init_random(&s1->digest);
    key_init_random(&s1->key);
    if (!hs_calc_digest(s1, k_fms_key, 36, s1->digest.digest)) {
        return 0;
    }
    return 1;
}

static int hs_create_s2(uint8_t s2[HS_SIZE], const hs_c1s1 *c1)
{
    hs_random(s2, HS_SIZE - 32);
    uint8_t temp[32];
    zms_hs_hmac_sha256(k_fms_key, 68, c1->digest.digest, 32, temp);
    zms_hs_hmac_sha256(temp, 32, s2, HS_SIZE - 32, s2 + HS_SIZE - 32);
    return 1;
}

int zms_rtmp_hs_detect_complex(const uint8_t c1[HS_SIZE])
{
    hs_c1s1 c1p = {0};
    if (hs_parse(&c1p, c1, 0) && hs_validate_c1(&c1p)) {
        hs_free(&c1p);
        return 1;
    }
    hs_free(&c1p);
    memset(&c1p, 0, sizeof(c1p));
    if (hs_parse(&c1p, c1, 1) && hs_validate_c1(&c1p)) {
        hs_free(&c1p);
        return 1;
    }
    hs_free(&c1p);
    return 0;
}

int zms_rtmp_hs_build_complex(const uint8_t c1[HS_SIZE], uint8_t s1[HS_SIZE], uint8_t s2[HS_SIZE])
{
    hs_c1s1 c1p = {0}, s1p = {0};
    int ok = 0;
    if (hs_parse(&c1p, c1, 0) && hs_validate_c1(&c1p)) {
        ok = 1;
    } else {
        hs_free(&c1p);
        memset(&c1p, 0, sizeof(c1p));
        if (!hs_parse(&c1p, c1, 1) || !hs_validate_c1(&c1p)) {
            goto out;
        }
        ok = 1;
    }
    if (!ok || !hs_create_s1(&s1p, &c1p)) {
        goto out;
    }
    if (!hs_dump(&s1p, s1)) {
        goto out;
    }
    if (!hs_create_s2(s2, &c1p)) {
        goto out;
    }
    hs_free(&s1p);
    hs_free(&c1p);
    return 1;
out:
    hs_free(&s1p);
    hs_free(&c1p);
    return 0;
}

int zms_rtmp_hs_validate_c2(const uint8_t s1[HS_SIZE], const uint8_t c2[HS_SIZE])
{
    hs_c1s1 s1p = {0};
    uint8_t temp[32], sig[32];
    if (!hs_parse(&s1p, s1, 0)) {
        hs_free(&s1p);
        memset(&s1p, 0, sizeof(s1p));
        if (!hs_parse(&s1p, s1, 1)) {
            hs_free(&s1p);
            return 0;
        }
    }
    zms_hs_hmac_sha256(k_fp_key, 62, s1p.digest.digest, 32, temp);
    zms_hs_hmac_sha256(temp, 32, c2, HS_SIZE - 32, sig);
    int ok = memcmp(sig, c2 + HS_SIZE - 32, 32) == 0;
    hs_free(&s1p);
    return ok;
}
