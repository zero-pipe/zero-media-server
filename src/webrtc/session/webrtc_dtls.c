#include "webrtc/session/webrtc_media_internal.h"
#include "ztk/util/log.h"
#include <stdlib.h>
#include <string.h>

#if defined(ZMS_HAVE_WEBRTC_DTLS) && ZMS_HAVE_WEBRTC_DTLS

#include <openssl/bio.h>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

struct zms_webrtc_dtls {
    SSL *ssl;
    BIO *rbio;
    BIO *wbio;
    int connected;
    int is_client;
};

static SSL_CTX *g_dtls_ctx;
static SSL_CTX *g_dtls_cli_ctx;
static char g_dtls_fingerprint[128];

static X509 *webrtc_dtls_make_cert(EVP_PKEY *pkey)
{
    X509 *x509;
    BIGNUM *rnd;
    X509_NAME *name;

    x509 = X509_new();
    rnd = BN_new();
    name = X509_NAME_new();
    if (!x509 || !rnd || !name) {
        goto fail;
    }
    if (!X509_set_version(x509, 2) || !X509_set_pubkey(x509, pkey)) {
        goto fail;
    }
    if (!BN_pseudo_rand(rnd, 64, 0, 0) || !BN_to_ASN1_INTEGER(rnd, X509_get_serialNumber(x509))) {
        goto fail;
    }
    if (!X509_NAME_add_entry_by_NID(name, NID_commonName, MBSTRING_UTF8, (unsigned char *)"zms", -1,
                                    -1, 0) ||
        !X509_set_subject_name(x509, name) || !X509_set_issuer_name(x509, name) ||
        !X509_gmtime_adj(X509_get_notBefore(x509), -86400) ||
        !X509_gmtime_adj(X509_get_notAfter(x509), 86400 * 365) ||
        !X509_sign(x509, pkey, EVP_sha256())) {
        goto fail;
    }
    BN_free(rnd);
    X509_NAME_free(name);
    return x509;
fail:
    BN_free(rnd);
    X509_NAME_free(name);
    X509_free(x509);
    return NULL;
}

static int webrtc_dtls_fingerprint_from_x509(const X509 *x509, char *out, size_t cap)
{
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int n = 0;
    unsigned int i;

    if (!x509 || !out || cap < 3) {
        return -1;
    }
    if (!X509_digest(x509, EVP_sha256(), digest, &n) || n == 0) {
        return -1;
    }
    if (cap < (size_t)n * 3) {
        return -1;
    }
    for (i = 0; i < n; ++i) {
        snprintf(out + i * 3, 4, "%02X:", digest[i]);
    }
    out[n * 3 - 1] = '\0';
    return 0;
}

static int webrtc_dtls_ctx_use_cert(SSL_CTX *ctx, X509 *x509, EVP_PKEY *pkey)
{
    if (!ctx || !x509 || !pkey) {
        return -1;
    }
    if (SSL_CTX_use_certificate(ctx, x509) != 1 || SSL_CTX_use_PrivateKey(ctx, pkey) != 1 ||
        SSL_CTX_check_private_key(ctx) != 1) {
        return -1;
    }
    return 0;
}

static int webrtc_dtls_init_ctx(SSL_CTX **ctx_out, int as_client, X509 *x509, EVP_PKEY *pkey)
{
    SSL_CTX *ctx;

    ctx = SSL_CTX_new(as_client ? DTLS_client_method() : DTLS_server_method());
    if (!ctx) {
        return -1;
    }
    SSL_CTX_set_options(ctx, SSL_OP_NO_SSLv3 | SSL_OP_NO_TLSv1 | SSL_OP_NO_TLSv1_1);
    SSL_CTX_set_read_ahead(ctx, 1);
    SSL_CTX_set_cipher_list(ctx, "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256");
    if (SSL_CTX_set_tlsext_use_srtp(ctx, "SRTP_AES128_CM_SHA1_80") != 0) {
        ztk_warn("[webrtc] DTLS-SRTP profile not available");
    }
    if (webrtc_dtls_ctx_use_cert(ctx, x509, pkey) != 0) {
        SSL_CTX_free(ctx);
        return -1;
    }
    *ctx_out = ctx;
    return 0;
}

int zms_webrtc_dtls_global_init(void)
{
    EVP_PKEY *pkey;
    X509 *x509;

    if (g_dtls_ctx) {
        return 0;
    }
#if OPENSSL_VERSION_NUMBER >= 0x10100000L
    OPENSSL_init_ssl(OPENSSL_INIT_SSL_DEFAULT, NULL);
#endif
    pkey = EVP_PKEY_new();
    if (!pkey) {
        return -1;
    }
    {
        EC_KEY *ec = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
        if (!ec || !EC_KEY_generate_key(ec) || !EVP_PKEY_assign_EC_KEY(pkey, ec)) {
            EC_KEY_free(ec);
            EVP_PKEY_free(pkey);
            return -1;
        }
    }
    x509 = webrtc_dtls_make_cert(pkey);
    if (!x509) {
        EVP_PKEY_free(pkey);
        return -1;
    }
    if (webrtc_dtls_init_ctx(&g_dtls_ctx, 0, x509, pkey) != 0 ||
        webrtc_dtls_init_ctx(&g_dtls_cli_ctx, 1, x509, pkey) != 0) {
        SSL_CTX_free(g_dtls_ctx);
        SSL_CTX_free(g_dtls_cli_ctx);
        g_dtls_ctx = NULL;
        g_dtls_cli_ctx = NULL;
        X509_free(x509);
        EVP_PKEY_free(pkey);
        return -1;
    }
    if (webrtc_dtls_fingerprint_from_x509(x509, g_dtls_fingerprint, sizeof(g_dtls_fingerprint)) !=
        0) {
        strncpy(g_dtls_fingerprint,
                "00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:"
                "00:00:00:00:00",
                sizeof(g_dtls_fingerprint) - 1);
    }
    X509_free(x509);
    EVP_PKEY_free(pkey);
    ztk_info("[webrtc] DTLS certificate ready fingerprint=%s", g_dtls_fingerprint);
    return 0;
}

void zms_webrtc_dtls_global_fini(void)
{
    if (g_dtls_cli_ctx) {
        SSL_CTX_free(g_dtls_cli_ctx);
        g_dtls_cli_ctx = NULL;
    }
    if (g_dtls_ctx) {
        SSL_CTX_free(g_dtls_ctx);
        g_dtls_ctx = NULL;
    }
    g_dtls_fingerprint[0] = '\0';
}

const char *zms_webrtc_dtls_fingerprint(void)
{
    return g_dtls_fingerprint[0] ? g_dtls_fingerprint
                                 : "00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:"
                                   "00:00:00:00:00:00:00:00:00:00:00";
}

zms_webrtc_dtls *zms_webrtc_dtls_create(void)
{
    zms_webrtc_dtls *d;

    if (!g_dtls_ctx && zms_webrtc_dtls_global_init() != 0) {
        return NULL;
    }
    d = (zms_webrtc_dtls *)calloc(1, sizeof(*d));
    if (!d) {
        return NULL;
    }
    d->rbio = BIO_new(BIO_s_mem());
    d->wbio = BIO_new(BIO_s_mem());
    d->ssl = SSL_new(g_dtls_ctx);
    if (!d->ssl || !d->rbio || !d->wbio) {
        zms_webrtc_dtls_destroy(d);
        return NULL;
    }
    SSL_set_bio(d->ssl, d->rbio, d->wbio);
    SSL_set_accept_state(d->ssl);
    SSL_set_mtu(d->ssl, 1200);
    return d;
}

zms_webrtc_dtls *zms_webrtc_dtls_create_client(void)
{
    zms_webrtc_dtls *d;

    if (!g_dtls_cli_ctx && zms_webrtc_dtls_global_init() != 0) {
        return NULL;
    }
    d = (zms_webrtc_dtls *)calloc(1, sizeof(*d));
    if (!d) {
        return NULL;
    }
    d->rbio = BIO_new(BIO_s_mem());
    d->wbio = BIO_new(BIO_s_mem());
    d->ssl = SSL_new(g_dtls_cli_ctx);
    if (!d->ssl || !d->rbio || !d->wbio) {
        zms_webrtc_dtls_destroy(d);
        return NULL;
    }
    d->is_client = 1;
    SSL_set_bio(d->ssl, d->rbio, d->wbio);
    SSL_set_connect_state(d->ssl);
    SSL_set_mtu(d->ssl, 1200);
    return d;
}

static int webrtc_dtls_handshake_step(zms_webrtc_dtls *d)
{
    int r;
    int err;

    r = d->is_client ? SSL_connect(d->ssl) : SSL_accept(d->ssl);
    if (r == 1) {
        d->connected = 1;
        ztk_info("[webrtc] DTLS handshake complete role=%s", d->is_client ? "client" : "server");
        return 1;
    }
    err = SSL_get_error(d->ssl, r);
    if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
        ztk_warn("[webrtc] DTLS %s err=%d ssl_err=%d", d->is_client ? "connect" : "accept", r, err);
        return -1;
    }
    return 0;
}

static int webrtc_dtls_flush_out(zms_webrtc_dtls *d, uint8_t *out, size_t out_cap, size_t *out_len);

int zms_webrtc_dtls_kick(zms_webrtc_dtls *d, uint8_t *out, size_t out_cap, size_t *out_len)
{
    int st;

    if (!d || !out_len || d->connected || !d->is_client) {
        return 0;
    }
    *out_len = 0;
    st = webrtc_dtls_handshake_step(d);
    if (st < 0) {
        return -1;
    }
    webrtc_dtls_flush_out(d, out, out_cap, out_len);
    return d->connected ? 1 : 0;
}

void zms_webrtc_dtls_destroy(zms_webrtc_dtls *d)
{
    if (!d) {
        return;
    }
    if (d->ssl) {
        SSL_free(d->ssl);
    } else {
        BIO_free(d->rbio);
        BIO_free(d->wbio);
    }
    free(d);
}

static int webrtc_dtls_flush_out(zms_webrtc_dtls *d, uint8_t *out, size_t out_cap, size_t *out_len)
{
    long pending;

    if (!d || !out_len) {
        return 0;
    }
    *out_len = 0;
    pending = BIO_pending(d->wbio);
    if (pending <= 0) {
        return 0;
    }
    if ((size_t)pending > out_cap) {
        pending = (long)out_cap;
    }
    *out_len = (size_t)BIO_read(d->wbio, out, (int)pending);
    return *out_len > 0;
}

int zms_webrtc_dtls_input(zms_webrtc_dtls *d, const uint8_t *pkt, size_t len, uint8_t *out,
                          size_t out_cap, size_t *out_len)
{
    int r;
    char drain[256];

    if (!d || !pkt || len == 0 || !out_len) {
        return -1;
    }
    *out_len = 0;
    if (BIO_write(d->rbio, pkt, (int)len) <= 0) {
        return -1;
    }

    if (!d->connected) {
        if (webrtc_dtls_handshake_step(d) < 0) {
            return -1;
        }
    } else {
        r = SSL_read(d->ssl, drain, (int)sizeof(drain));
        (void)r;
    }

    webrtc_dtls_flush_out(d, out, out_cap, out_len);
    if (d->connected) {
        return 1;
    }
    return 0;
}

int zms_webrtc_dtls_is_connected(const zms_webrtc_dtls *d)
{
    return d && d->connected;
}

int zms_webrtc_dtls_export_server_srtp(const zms_webrtc_dtls *d, uint8_t *key, uint8_t *salt)
{
    uint8_t material[60];

    if (!d || !d->connected || !key || !salt) {
        return -1;
    }
    if (SSL_export_keying_material(d->ssl, material, sizeof(material), "EXTRACTOR-dtls_srtp", 19,
                                   NULL, 0, 0) != 1) {
        return -1;
    }
    memcpy(key, material + 16, ZMS_WEBRTC_SRTP_KEY_LEN);
    memcpy(salt, material + 46, ZMS_WEBRTC_SRTP_SALT_LEN);
    return 0;
}

int zms_webrtc_dtls_export_client_srtp(const zms_webrtc_dtls *d, uint8_t *key, uint8_t *salt)
{
    uint8_t material[60];

    if (!d || !d->connected || !key || !salt) {
        return -1;
    }
    if (SSL_export_keying_material(d->ssl, material, sizeof(material), "EXTRACTOR-dtls_srtp", 19,
                                   NULL, 0, 0) != 1) {
        return -1;
    }
    memcpy(key, material + 0, ZMS_WEBRTC_SRTP_KEY_LEN);
    memcpy(salt, material + 32, ZMS_WEBRTC_SRTP_SALT_LEN);
    return 0;
}

#else /* !ZMS_HAVE_WEBRTC_DTLS */

int zms_webrtc_dtls_global_init(void)
{
    return 0;
}

void zms_webrtc_dtls_global_fini(void) {}

const char *zms_webrtc_dtls_fingerprint(void)
{
    return "00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:"
           "00:00:00";
}

zms_webrtc_dtls *zms_webrtc_dtls_create(void)
{
    return NULL;
}

zms_webrtc_dtls *zms_webrtc_dtls_create_client(void)
{
    return NULL;
}

void zms_webrtc_dtls_destroy(zms_webrtc_dtls *d)
{
    (void)d;
}

int zms_webrtc_dtls_kick(zms_webrtc_dtls *d, uint8_t *out, size_t out_cap, size_t *out_len)
{
    (void)d;
    (void)out;
    (void)out_cap;
    if (out_len) {
        *out_len = 0;
    }
    return -1;
}

int zms_webrtc_dtls_input(zms_webrtc_dtls *d, const uint8_t *pkt, size_t len, uint8_t *out,
                          size_t out_cap, size_t *out_len)
{
    (void)d;
    (void)pkt;
    (void)len;
    (void)out;
    (void)out_cap;
    if (out_len) {
        *out_len = 0;
    }
    return -1;
}

int zms_webrtc_dtls_is_connected(const zms_webrtc_dtls *d)
{
    (void)d;
    return 0;
}

int zms_webrtc_dtls_export_server_srtp(const zms_webrtc_dtls *d, uint8_t *key, uint8_t *salt)
{
    (void)d;
    (void)key;
    (void)salt;
    return -1;
}

int zms_webrtc_dtls_export_client_srtp(const zms_webrtc_dtls *d, uint8_t *key, uint8_t *salt)
{
    (void)d;
    (void)key;
    (void)salt;
    return -1;
}

#endif /* ZMS_HAVE_WEBRTC_DTLS */
