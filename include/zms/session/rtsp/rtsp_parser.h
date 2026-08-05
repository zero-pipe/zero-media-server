#ifndef ZMS_SESSION_RTSP_PARSER_H
#define ZMS_SESSION_RTSP_PARSER_H

#include "zms/zms_export.h"
#include "ztk/ztk_errno.h"
#include "rtsp.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZMS_RTSP_HDR_MAX 32
#define ZMS_RTSP_KEY_MAX 64
#define ZMS_RTSP_VAL_MAX 512
#define ZMS_RTSP_LINE_MAX 2048

typedef struct zms_rtsp_header {
    char key[ZMS_RTSP_KEY_MAX];
    char value[ZMS_RTSP_VAL_MAX];
} zms_rtsp_header;

typedef struct zms_rtsp_message {
    int is_response;
    zms_rtsp_method method;
    int status_code;
    char version[16];
    char url[512];
    char reason[128];
    zms_rtsp_header headers[ZMS_RTSP_HDR_MAX];
    unsigned header_count;
    const char *body;
    size_t body_len;
} zms_rtsp_message;

/** 仅解析头（至 \\r\\r\n\\r\\r\n），不含 body */
ZMS_API ztk_err_t zms_rtsp_message_parse_header(const char *data, size_t len,
                                                zms_rtsp_message *msg);
ZMS_API ztk_err_t zms_rtsp_message_parse(const char *data, size_t len, zms_rtsp_message *msg,
                                         size_t *consumed);
ZMS_API const char *zms_rtsp_message_get(const zms_rtsp_message *msg, const char *key);
ZMS_API int zms_rtsp_message_content_length(const zms_rtsp_message *msg);
ZMS_API ztk_err_t zms_rtsp_message_build_request(char *out, size_t cap, zms_rtsp_method method,
                                                 const char *url, unsigned cseq,
                                                 const char *session, const char *extra_headers,
                                                 const char *body, size_t body_len,
                                                 size_t *out_len);
ZMS_API ztk_err_t zms_rtsp_message_build_response(char *out, size_t cap, int status,
                                                  const char *reason, unsigned cseq,
                                                  const char *session, const char *extra_headers,
                                                  const char *body, size_t body_len,
                                                  size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_SESSION_RTSP_PARSER_H */
