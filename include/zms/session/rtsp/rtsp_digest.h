#ifndef ZMS_SESSION_RTSP_DIGEST_H
#define ZMS_SESSION_RTSP_DIGEST_H

#include "zms/zms_export.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum zms_rtsp_auth_scheme {
    ZMS_RTSP_AUTH_NONE = 0,
    ZMS_RTSP_AUTH_DIGEST,
    ZMS_RTSP_AUTH_BASIC,
} zms_rtsp_auth_scheme;

/** 解析 WWW-Authenticate 头（可含 "Digest ..." 前缀） */
ZMS_API zms_rtsp_auth_scheme zms_rtsp_digest_parse_www_auth(const char *hdr, char *realm,
                                                            size_t realm_cap, char *nonce,
                                                            size_t nonce_cap);

/**
 * 生成 Authorization 行（不含 "Authorization: " 前缀与 CRLF）。
 * ANSI 密码：response = md5(md5(user:realm:pass):nonce:md5(method:url))
 */
ZMS_API int zms_rtsp_digest_build_authorization(const char *method, const char *url,
                                                const char *user, const char *pass,
                                                const char *realm, const char *nonce, char *out,
                                                size_t out_cap);

/** 校验客户端 Authorization（Digest / Basic） */
ZMS_API int zms_rtsp_digest_verify(const char *method, const char *url, const char *user,
                                   const char *pass, const char *realm, const char *nonce,
                                   const char *authorization);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_SESSION_RTSP_DIGEST_H */
