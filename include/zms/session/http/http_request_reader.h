#ifndef ZMS_SESSION_HTTP_REQUEST_READER_H
#define ZMS_SESSION_HTTP_REQUEST_READER_H

#include "zms/zms_export.h"
#include "ztk/ztk_errno.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zms_http_request {
    char method[16];
    char path[512];
    char host[128];
    char range[128];
    const char *body;
    size_t body_len;
    int ws_upgrade;
    char ws_key[64];
} zms_http_request;

typedef void (*zms_http_on_request_cb)(const zms_http_request *req, void *user);

typedef struct zms_http_request_reader zms_http_request_reader;

typedef struct zms_http_request_reader_opts {
    zms_http_on_request_cb on_request;
    void *user;
} zms_http_request_reader_opts;

ZMS_API zms_http_request_reader *
zms_http_request_reader_create(const zms_http_request_reader_opts *opts);
ZMS_API void zms_http_request_reader_destroy(zms_http_request_reader *s);
ZMS_API void zms_http_request_reader_reset(zms_http_request_reader *s);
ZMS_API ztk_err_t zms_http_request_reader_input(zms_http_request_reader *s, const void *data,
                                                size_t len);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_SESSION_HTTP_REQUEST_READER_H */
