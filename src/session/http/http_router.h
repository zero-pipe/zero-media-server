#ifndef ZMS_SRC_SESSION_HTTP_ROUTER_H
#define ZMS_SRC_SESSION_HTTP_ROUTER_H

#include "zms/session/http/http_request_reader.h"
#include "ztk/ztk_errno.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zms_http_session zms_http_session;

typedef int (*zms_http_route_match_fn)(const zms_http_request *req);
typedef void (*zms_http_route_handle_fn)(zms_http_session *sess, const zms_http_request *req);

typedef struct zms_http_route_ops {
    const char *name;
    zms_http_route_match_fn match;
    zms_http_route_handle_fn handle;
} zms_http_route_ops;

void zms_http_route_register(const zms_http_route_ops *ops);
void zms_http_routes_register_all(void);
int zms_http_router_dispatch(zms_http_session *sess, const zms_http_request *req);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_SRC_SESSION_HTTP_ROUTER_H */
