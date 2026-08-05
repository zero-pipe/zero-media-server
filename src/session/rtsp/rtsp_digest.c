#include "zms/session/rtsp/rtsp_digest.h"
#include "zms/util/md5.h"
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define strncasecmp _strnicmp
#define strcasecmp _stricmp
#endif

static const char *skip_scheme(const char *hdr)
{
    if (!hdr) {
        return "";
    }
    if (strncasecmp(hdr, "Digest ", 7) == 0) {
        return hdr + 7;
    }
    if (strncasecmp(hdr, "Basic ", 6) == 0) {
        return hdr + 6;
    }
    return hdr;
}

static int extract_quoted(const char *hdr, const char *key, char *out, size_t cap)
{
    char pattern[32];
    snprintf(pattern, sizeof(pattern), "%s=\"", key);
    const char *p = strstr(hdr, pattern);
    if (!p) {
        return -1;
    }
    p += strlen(pattern);
    const char *end = strchr(p, '"');
    if (!end) {
        return -1;
    }
    size_t n = (size_t)(end - p);
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(out, p, n);
    out[n] = '\0';
    return 0;
}

zms_rtsp_auth_scheme zms_rtsp_digest_parse_www_auth(const char *hdr, char *realm, size_t realm_cap,
                                                    char *nonce, size_t nonce_cap)
{
    if (realm && realm_cap) {
        realm[0] = '\0';
    }
    if (nonce && nonce_cap) {
        nonce[0] = '\0';
    }
    if (!hdr || !hdr[0]) {
        return ZMS_RTSP_AUTH_NONE;
    }

    if (strncasecmp(hdr, "Basic", 5) == 0) {
        extract_quoted(skip_scheme(hdr), "realm", realm, realm_cap);
        return ZMS_RTSP_AUTH_BASIC;
    }

    const char *p = skip_scheme(hdr);
    if (strstr(p, "realm=") == NULL) {
        return ZMS_RTSP_AUTH_NONE;
    }
    extract_quoted(p, "realm", realm, realm_cap);
    if (nonce && nonce_cap) {
        extract_quoted(p, "nonce", nonce, nonce_cap);
    }
    return ZMS_RTSP_AUTH_DIGEST;
}

static void md5_join3_hex(const char *a, const char *b, const char *c, char out[33])
{
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s:%s:%s", a ? a : "", b ? b : "", c ? c : "");
    zms_md5_hex_str(buf, out);
}

static void md5_method_url_hex(const char *method, const char *url, char out[33])
{
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s:%s", method ? method : "", url ? url : "");
    zms_md5_hex_str(buf, out);
}

int zms_rtsp_digest_build_authorization(const char *method, const char *url, const char *user,
                                        const char *pass, const char *realm, const char *nonce,
                                        char *out, size_t out_cap)
{
    if (!method || !url || !user || !pass || !realm || !nonce || !out || out_cap < 64) {
        return -1;
    }

    char ha1[33], ha2[33], resp[33];
    md5_join3_hex(user, realm, pass, ha1);
    md5_method_url_hex(method, url, ha2);
    md5_join3_hex(ha1, nonce, ha2, resp);

    int n = snprintf(out, out_cap,
                     "Authorization: Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", "
                     "uri=\"%s\", response=\"%s\"\r\n",
                     user, realm, nonce, url, resp);
    return (n > 0 && (size_t)n < out_cap) ? n : -1;
}

static int parse_digest_auth(const char *auth, char *user, size_t user_cap, char *resp,
                             size_t resp_cap)
{
    if (!auth || strncasecmp(auth, "Digest ", 7) != 0) {
        return -1;
    }
    const char *p = auth + 7;
    if (extract_quoted(p, "username", user, user_cap) != 0) {
        return -1;
    }
    return extract_quoted(p, "response", resp, resp_cap);
}

static int parse_basic_auth(const char *auth, char *user, size_t user_cap, char *pass,
                            size_t pass_cap)
{
    (void)user_cap;
    (void)pass_cap;
    if (!auth || strncasecmp(auth, "Basic ", 6) != 0) {
        return -1;
    }
    /* 简化：仅用于测试，完整 base64 解码 */
    (void)auth;
    (void)user;
    (void)pass;
    return -1;
}

int zms_rtsp_digest_verify(const char *method, const char *url, const char *user, const char *pass,
                           const char *realm, const char *nonce, const char *authorization)
{
    if (!authorization || !user || !pass || !realm || !method || !url || !nonce) {
        return 0;
    }
    if (strncasecmp(authorization, "Digest ", 7) != 0) {
        return 0;
    }
    char u[128], got[33], ha1[33], ha2[33], expect[33];
    if (parse_digest_auth(authorization, u, sizeof(u), got, sizeof(got)) != 0) {
        return 0;
    }
    if (strcmp(u, user) != 0) {
        return 0;
    }
    md5_join3_hex(user, realm, pass, ha1);
    md5_method_url_hex(method, url, ha2);
    md5_join3_hex(ha1, nonce, ha2, expect);
    return strcmp(got, expect) == 0;
}
