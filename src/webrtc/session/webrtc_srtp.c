#include "webrtc/session/webrtc_media_internal.h"
#include "zms/engine/media/media_limits.h"
#include "zms/webrtc/webrtc_srtp.h"
#include "zms/media/wire/rtp_packet.h"
#include "ztk/util/log.h"
#include "ztk/platform.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#endif
#if defined(ZMS_HAVE_WEBRTC_DTLS) && ZMS_HAVE_WEBRTC_DTLS
#include <openssl/evp.h>
#include <openssl/hmac.h>
struct zms_webrtc_srtp {
    uint8_t enc_key[ZMS_WEBRTC_SRTP_KEY_LEN];
    uint8_t auth_key[20];
    uint8_t session_salt[ZMS_WEBRTC_SRTP_SALT_LEN];
    uint8_t *auth_send_buf;
    uint8_t *auth_recv_buf;
    size_t auth_buf_cap;
    EVP_CIPHER_CTX *ecb;
    uint16_t last_seq;
    uint32_t roc;
    uint32_t srtcp_index;
    int ready;
};

static void srtp_kdf(const uint8_t *master_key, const uint8_t *master_salt, uint8_t label,
                     uint8_t *out, size_t out_len)
{
    EVP_CIPHER_CTX *ctx;
    uint8_t iv[16];
    uint8_t block[16];
    size_t off = 0;
    int n;
    memset(iv, 0, sizeof(iv));
    memcpy(iv, master_salt, ZMS_WEBRTC_SRTP_SALT_LEN);
    iv[7] ^= label;
    ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        return;
    }
    if (EVP_EncryptInit_ex(ctx, EVP_aes_128_ecb(), NULL, master_key, NULL) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return;
    }
    EVP_CIPHER_CTX_set_padding(ctx, 0);
    while (off < out_len) {
        if (EVP_EncryptUpdate(ctx, block, &n, iv, 16) != 1) {
            break;
        }
        memcpy(out + off, block, out_len - off >= 16 ? 16 : out_len - off);
        off += out_len - off >= 16 ? 16 : out_len - off;
        for (int i = 15; i >= 0; --i) {
            if (++iv[i] != 0) {
                break;
            }
        }
    }
    EVP_CIPHER_CTX_free(ctx);
}

static void srtp_seq_update(struct zms_webrtc_srtp *s, uint16_t seq)
{
    if (!s->ready) {
        s->last_seq = seq;
        s->roc = 0;
        s->ready = 1;
        return;
    }
    if (seq < s->last_seq && s->last_seq - seq > 0x8000) {
        ++s->roc;
    }
    s->last_seq = seq;
}

static uint32_t srtp_index(const struct zms_webrtc_srtp *s, uint16_t seq)
{
    return (s->roc << 16) | seq;
}

static int srtp_aes_ecb_init(struct zms_webrtc_srtp *s)
{
    if (!s) {
        return -1;
    }
    if (!s->ecb) {
        s->ecb = EVP_CIPHER_CTX_new();
        if (!s->ecb) {
            return -1;
        }
    }
    if (EVP_EncryptInit_ex(s->ecb, EVP_aes_128_ecb(), NULL, s->enc_key, NULL) != 1) {
        return -1;
    }
    EVP_CIPHER_CTX_set_padding(s->ecb, 0);
    return 0;
}

static int srtp_aes_icm_crypt(struct zms_webrtc_srtp *s, uint32_t ssrc, uint32_t index,
                              uint8_t *data, size_t len)
{
    uint8_t counter[16];
    uint8_t keystream[16];
    size_t i, pos = 0;
    int out_len = 0;
    uint32_t roc = index >> 16;
    uint16_t seq = (uint16_t)(index & 0xffffu);
    if (!s || !data || len == 0) {
        return -1;
    }
    if (srtp_aes_ecb_init(s) != 0) {
        return -1;
    }
    memset(counter, 0, sizeof(counter));
    counter[4] = (uint8_t)(ssrc >> 24);
    counter[5] = (uint8_t)(ssrc >> 16);
    counter[6] = (uint8_t)(ssrc >> 8);
    counter[7] = (uint8_t)(ssrc);
    counter[8] = (uint8_t)(roc >> 24);
    counter[9] = (uint8_t)(roc >> 16);
    counter[10] = (uint8_t)(roc >> 8);
    counter[11] = (uint8_t)(roc);
    counter[12] = (uint8_t)(seq >> 8);
    counter[13] = (uint8_t)(seq);
    for (i = 0; i < ZMS_WEBRTC_SRTP_SALT_LEN; ++i) {
        counter[i] ^= s->session_salt[i];
    }
    while (pos < len) {
        uint16_t block_index = (uint16_t)(pos >> 4);
        counter[14] = (uint8_t)(block_index >> 8);
        counter[15] = (uint8_t)(block_index);
        if (EVP_EncryptUpdate(s->ecb, keystream, &out_len, counter, 16) != 1 || out_len != 16) {
            return -1;
        }
        for (i = 0; i < 16 && pos < len; ++i, ++pos) {
            data[pos] ^= keystream[i];
        }
    }
    return 0;
}

int zms_webrtc_srtp_init_send(zms_webrtc_srtp *ctx, const uint8_t *key, const uint8_t *salt)
{
    if (!ctx || !key || !salt) {
        return -1;
    }
    memset(ctx, 0, sizeof(*ctx));
    srtp_kdf(key, salt, 0x00, ctx->enc_key, sizeof(ctx->enc_key));
    srtp_kdf(key, salt, 0x01, ctx->auth_key, sizeof(ctx->auth_key));
    srtp_kdf(key, salt, 0x02, ctx->session_salt, sizeof(ctx->session_salt));
    return srtp_aes_ecb_init(ctx);
}

zms_webrtc_srtp *zms_webrtc_srtp_create(void)
{
    return (zms_webrtc_srtp *)calloc(1, sizeof(zms_webrtc_srtp));
}

void zms_webrtc_srtp_destroy(zms_webrtc_srtp *ctx)
{
    if (!ctx) {
        return;
    }
    if (ctx->ecb) {
        EVP_CIPHER_CTX_free(ctx->ecb);
        ctx->ecb = NULL;
    }
    free(ctx);
}

void zms_webrtc_srtp_bind_scratch(zms_webrtc_srtp *ctx, uint8_t *buf, size_t cap)
{
    if (!ctx || !buf || cap < ZMS_WEBRTC_PLAY_CRYPT_BYTES) {
        return;
    }
    ctx->auth_send_buf = buf;
    ctx->auth_recv_buf = buf;
    ctx->auth_buf_cap = cap;
}
/* RFC 3550 头 + CSRC + 扩展：SRTP 仅加密/认证其后字节。 */
static size_t srtp_plain_rtp_prefix_len(const uint8_t *rtp, size_t len)
{
    size_t off = zms_rtp_payload_offset(rtp, len);
    return off > 0 ? off : 0;
}

static void srtp_diag(const char *step, size_t len, size_t hdr_len, size_t body_len,
                      const struct zms_webrtc_srtp *ctx)
{
#if defined(ZMS_WEBRTC_SRTP_DIAG)
    static unsigned n;
    if (n >= 24u) {
        return;
    }
    ++n;
    ztk_info("[webrtc] srtp_diag tid=%llu step=%s len=%zu hdr=%zu body=%zu ctx=%p ecb=%p ready=%d",
             (unsigned long long)ztk_thread_self_id(), step ? step : "?", len, hdr_len, body_len,
             (const void *)ctx, ctx ? (void *)ctx->ecb : NULL, ctx ? ctx->ready : -1);
#else
    (void)step;
    (void)len;
    (void)hdr_len;
    (void)body_len;
    (void)ctx;
#endif
}
/* OpenSSL HMAC 写出完整摘要（SHA-1 为 20 字节）；SRTP 仅用前 10 字节。 */
static int srtp_hmac_tag(const uint8_t *auth_key, size_t auth_key_len, const uint8_t *data,
                         size_t data_len, uint8_t out_tag[ZMS_WEBRTC_SRTP_TAG_LEN])
{
    unsigned int digest_len = EVP_MAX_MD_SIZE;
    uint8_t digest[EVP_MAX_MD_SIZE];
    if (!auth_key || !data || !out_tag) {
        return -1;
    }
    if (!HMAC(EVP_sha1(), auth_key, (int)auth_key_len, data, data_len, digest, &digest_len) ||
        digest_len < ZMS_WEBRTC_SRTP_TAG_LEN) {
        return -1;
    }
    memcpy(out_tag, digest, ZMS_WEBRTC_SRTP_TAG_LEN);
    return 0;
}

static int srtp_protect_impl(zms_webrtc_srtp *ctx, uint8_t *rtp, size_t *len_io, size_t cap)
{
    uint16_t seq;
    uint32_t ssrc;
    uint32_t index;
    size_t hdr_len;
    size_t body_len;
    uint8_t tag[ZMS_WEBRTC_SRTP_TAG_LEN];
    uint32_t roc;
    if (!ctx || !rtp || !len_io || *len_io < 12) {
        return -1;
    }
    if (*len_io + ZMS_WEBRTC_SRTP_TAG_LEN > cap) {
        return -1;
    }
    hdr_len = srtp_plain_rtp_prefix_len(rtp, *len_io);
    if (hdr_len == 0 || hdr_len > *len_io || hdr_len < 12) {
        return -1;
    }
    body_len = *len_io - hdr_len;
    srtp_diag("hdr_ok", *len_io, hdr_len, body_len, ctx);
    if (body_len > 0 && (hdr_len + body_len > cap || hdr_len + body_len > *len_io)) {
        return -1;
    }
    seq = (uint16_t)(((uint16_t)rtp[2] << 8) | rtp[3]);
    ssrc = ((uint32_t)rtp[8] << 24) | ((uint32_t)rtp[9] << 16) | ((uint32_t)rtp[10] << 8) | rtp[11];
    srtp_seq_update(ctx, seq);
    index = srtp_index(ctx, seq);
    roc = index >> 16;
    if (body_len > 0) {
        srtp_diag("icm_in", *len_io, hdr_len, body_len, ctx);
        if (srtp_aes_icm_crypt(ctx, ssrc, index, rtp + hdr_len, body_len) != 0) {
            return -1;
        }
        srtp_diag("icm_ok", *len_io, hdr_len, body_len, ctx);
    }
    if (*len_io + 4 > ctx->auth_buf_cap || !ctx->auth_send_buf) {
        return -1;
    }
    memcpy(ctx->auth_send_buf, rtp, *len_io);
    ctx->auth_send_buf[*len_io + 0] = (uint8_t)(roc >> 24);
    ctx->auth_send_buf[*len_io + 1] = (uint8_t)(roc >> 16);
    ctx->auth_send_buf[*len_io + 2] = (uint8_t)(roc >> 8);
    ctx->auth_send_buf[*len_io + 3] = (uint8_t)(roc);
    srtp_diag("hmac_in", *len_io, hdr_len, body_len, ctx);
    if (srtp_hmac_tag(ctx->auth_key, sizeof(ctx->auth_key), ctx->auth_send_buf, *len_io + 4, tag) !=
        0) {
        return -1;
    }
    srtp_diag("hmac_ok", *len_io, hdr_len, body_len, ctx);
    memcpy(rtp + *len_io, tag, ZMS_WEBRTC_SRTP_TAG_LEN);
    *len_io += ZMS_WEBRTC_SRTP_TAG_LEN;
    srtp_diag("done", *len_io, hdr_len, body_len, ctx);
    return 0;
}

int zms_webrtc_srtp_protect(zms_webrtc_srtp *ctx, uint8_t *rtp, size_t *len_io, size_t cap)
{
#if defined(_WIN32)
    __try {
        return srtp_protect_impl(ctx, rtp, len_io, cap);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ztk_warn("[webrtc] FAULT srtp_protect code=0x%08x ctx=%p len=%zu cap=%zu",
                 (unsigned)GetExceptionCode(), (void *)ctx, len_io ? *len_io : 0u, cap);
        return -1;
    }
#else
    return srtp_protect_impl(ctx, rtp, len_io, cap);
#endif
}

int zms_webrtc_srtp_init_recv(zms_webrtc_srtp *ctx, const uint8_t *key, const uint8_t *salt)
{
    return zms_webrtc_srtp_init_send(ctx, key, salt);
}

int zms_webrtc_srtp_unprotect(zms_webrtc_srtp *ctx, uint8_t *rtp, size_t *len_io)
{
    uint16_t seq;
    uint32_t ssrc;
    uint32_t index;
    size_t hdr_len;
    size_t body_len;
    size_t pkt_len;
    uint8_t tag[ZMS_WEBRTC_SRTP_TAG_LEN];
    uint32_t roc;
    if (!ctx || !rtp || !len_io || *len_io < 12 + ZMS_WEBRTC_SRTP_TAG_LEN) {
        return -1;
    }
    pkt_len = *len_io - ZMS_WEBRTC_SRTP_TAG_LEN;
    hdr_len = srtp_plain_rtp_prefix_len(rtp, pkt_len);
    if (hdr_len == 0) {
        return -1;
    }
    body_len = pkt_len - hdr_len;
    seq = (uint16_t)(((uint16_t)rtp[2] << 8) | rtp[3]);
    ssrc = ((uint32_t)rtp[8] << 24) | ((uint32_t)rtp[9] << 16) | ((uint32_t)rtp[10] << 8) | rtp[11];
    srtp_seq_update(ctx, seq);
    index = srtp_index(ctx, seq);
    roc = index >> 16;
    if (!ctx->auth_recv_buf || ctx->auth_buf_cap < pkt_len + 4) {
        return -1;
    }
    memcpy(ctx->auth_recv_buf, rtp, pkt_len);
    ctx->auth_recv_buf[pkt_len + 0] = (uint8_t)(roc >> 24);
    ctx->auth_recv_buf[pkt_len + 1] = (uint8_t)(roc >> 16);
    ctx->auth_recv_buf[pkt_len + 2] = (uint8_t)(roc >> 8);
    ctx->auth_recv_buf[pkt_len + 3] = (uint8_t)(roc);
    if (srtp_hmac_tag(ctx->auth_key, sizeof(ctx->auth_key), ctx->auth_recv_buf, pkt_len + 4, tag) !=
            0 ||
        memcmp(tag, rtp + pkt_len, ZMS_WEBRTC_SRTP_TAG_LEN) != 0) {
        return -1;
    }
    if (body_len > 0 && srtp_aes_icm_crypt(ctx, ssrc, index, rtp + hdr_len, body_len) != 0) {
        return -1;
    }
    *len_io = pkt_len;
    return 0;
}

static int srtcp_aes_icm_crypt(struct zms_webrtc_srtp *s, uint32_t ssrc, uint32_t srtcp_index,
                               uint8_t *data, size_t len)
{
    uint8_t counter[16];
    uint8_t keystream[16];
    size_t i, pos = 0;
    int out_len = 0;
    if (!s || !data || len == 0) {
        return -1;
    }
    if (srtp_aes_ecb_init(s) != 0) {
        return -1;
    }
    memset(counter, 0, sizeof(counter));
    counter[4] = (uint8_t)(ssrc >> 24);
    counter[5] = (uint8_t)(ssrc >> 16);
    counter[6] = (uint8_t)(ssrc >> 8);
    counter[7] = (uint8_t)(ssrc);
    counter[8] = (uint8_t)(srtcp_index >> 24);
    counter[9] = (uint8_t)(srtcp_index >> 16);
    counter[10] = (uint8_t)(srtcp_index >> 8);
    counter[11] = (uint8_t)(srtcp_index);
    for (i = 0; i < ZMS_WEBRTC_SRTP_SALT_LEN; ++i) {
        counter[i] ^= s->session_salt[i];
    }
    while (pos < len) {
        uint16_t block_index = (uint16_t)(pos >> 4);
        counter[14] = (uint8_t)(block_index >> 8);
        counter[15] = (uint8_t)(block_index);
        if (EVP_EncryptUpdate(s->ecb, keystream, &out_len, counter, 16) != 1 || out_len != 16) {
            return -1;
        }
        for (i = 0; i < 16 && pos < len; ++i, ++pos) {
            data[pos] ^= keystream[i];
        }
    }
    return 0;
}
/** RFC 3711 SRTCP：明文 RTCP + 4 字节 index + 10 字节 auth tag。 */
int zms_webrtc_srtcp_unprotect(zms_webrtc_srtp *ctx, uint8_t *rtcp, size_t *len_io)
{
    size_t total;
    size_t plain_len;
    size_t enc_len;
    uint32_t ssrc;
    uint32_t index_word;
    uint32_t srtcp_index;
    uint8_t tag[ZMS_WEBRTC_SRTP_TAG_LEN];
    if (!ctx || !rtcp || !len_io || *len_io < 8 + 4 + ZMS_WEBRTC_SRTP_TAG_LEN) {
        return -1;
    }
    total = *len_io;
    plain_len = total - 4 - ZMS_WEBRTC_SRTP_TAG_LEN;
    if (plain_len < 8) {
        return -1;
    }
    enc_len = plain_len - 8;
    index_word = ((uint32_t)rtcp[plain_len] << 24) | ((uint32_t)rtcp[plain_len + 1] << 16) |
                 ((uint32_t)rtcp[plain_len + 2] << 8) | rtcp[plain_len + 3];
    srtcp_index = index_word & 0x7fffffffu;
    ssrc =
        ((uint32_t)rtcp[4] << 24) | ((uint32_t)rtcp[5] << 16) | ((uint32_t)rtcp[6] << 8) | rtcp[7];
    if (srtp_hmac_tag(ctx->auth_key, sizeof(ctx->auth_key), rtcp, plain_len + 4, tag) != 0 ||
        memcmp(tag, rtcp + plain_len + 4, ZMS_WEBRTC_SRTP_TAG_LEN) != 0) {
        return -1;
    }
    if (enc_len > 0 && srtcp_aes_icm_crypt(ctx, ssrc, srtcp_index, rtcp + 8, enc_len) != 0) {
        return -1;
    }
    *len_io = plain_len;
    return 0;
}

int zms_webrtc_srtcp_protect(zms_webrtc_srtp *ctx, uint8_t *rtcp, size_t *len_io, size_t cap)
{
    size_t plain_len;
    size_t enc_len;
    uint32_t ssrc;
    uint32_t index_word;
    uint8_t tag[ZMS_WEBRTC_SRTP_TAG_LEN];
    if (!ctx || !rtcp || !len_io || *len_io < 8) {
        return -1;
    }
    plain_len = *len_io;
    if (plain_len + 4 + ZMS_WEBRTC_SRTP_TAG_LEN > cap) {
        return -1;
    }
    enc_len = plain_len - 8;
    ssrc =
        ((uint32_t)rtcp[4] << 24) | ((uint32_t)rtcp[5] << 16) | ((uint32_t)rtcp[6] << 8) | rtcp[7];
    ++ctx->srtcp_index;
    index_word = 0x80000000u | (ctx->srtcp_index & 0x7fffffffu);
    if (enc_len > 0 && srtcp_aes_icm_crypt(ctx, ssrc, ctx->srtcp_index, rtcp + 8, enc_len) != 0) {
        return -1;
    }
    rtcp[plain_len + 0] = (uint8_t)(index_word >> 24);
    rtcp[plain_len + 1] = (uint8_t)(index_word >> 16);
    rtcp[plain_len + 2] = (uint8_t)(index_word >> 8);
    rtcp[plain_len + 3] = (uint8_t)(index_word);
    if (srtp_hmac_tag(ctx->auth_key, sizeof(ctx->auth_key), rtcp, plain_len + 4, tag) != 0) {
        return -1;
    }
    memcpy(rtcp + plain_len + 4, tag, ZMS_WEBRTC_SRTP_TAG_LEN);
    *len_io = plain_len + 4 + ZMS_WEBRTC_SRTP_TAG_LEN;
    return 0;
}
#else  /* !ZMS_HAVE_WEBRTC_DTLS */
struct zms_webrtc_srtp {
    int dummy;
};

zms_webrtc_srtp *zms_webrtc_srtp_create(void)
{
    return NULL;
}

void zms_webrtc_srtp_destroy(zms_webrtc_srtp *ctx)
{
    free(ctx);
}

void zms_webrtc_srtp_bind_scratch(zms_webrtc_srtp *ctx, uint8_t *buf, size_t cap)
{
    (void)ctx;
    (void)buf;
    (void)cap;
}

int zms_webrtc_srtp_init_send(zms_webrtc_srtp *ctx, const uint8_t *key, const uint8_t *salt)
{
    (void)ctx;
    (void)key;
    (void)salt;
    return -1;
}

int zms_webrtc_srtp_protect(zms_webrtc_srtp *ctx, uint8_t *rtp, size_t *len_io, size_t cap)
{
    (void)ctx;
    (void)rtp;
    (void)len_io;
    (void)cap;
    return -1;
}

int zms_webrtc_srtp_init_recv(zms_webrtc_srtp *ctx, const uint8_t *key, const uint8_t *salt)
{
    (void)ctx;
    (void)key;
    (void)salt;
    return -1;
}

int zms_webrtc_srtp_unprotect(zms_webrtc_srtp *ctx, uint8_t *rtp, size_t *len_io)
{
    (void)ctx;
    (void)rtp;
    (void)len_io;
    return -1;
}

int zms_webrtc_srtcp_unprotect(zms_webrtc_srtp *ctx, uint8_t *rtcp, size_t *len_io)
{
    (void)ctx;
    (void)rtcp;
    (void)len_io;
    return -1;
}

int zms_webrtc_srtcp_protect(zms_webrtc_srtp *ctx, uint8_t *rtcp, size_t *len_io, size_t cap)
{
    (void)ctx;
    (void)rtcp;
    (void)len_io;
    (void)cap;
    return -1;
}
#endif /* ZMS_HAVE_WEBRTC_DTLS */
