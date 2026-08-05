#include "session/http/http_router.h"
#include <string.h>

#define ZMS_HTTP_ROUTE_MAX 16

static const zms_http_route_ops *g_routes[ZMS_HTTP_ROUTE_MAX];
static int g_route_count;

void zms_webapi_http_routes_register(void);
void zms_vod_http_routes_register(void);
void zms_http_hls_routes_register(void);
void zms_http_dash_routes_register(void);
void zms_http_flv_routes_register(void);
void zms_http_ts_routes_register(void);
#if defined(ZMS_ENABLE_WEBRTC)
void zms_webrtc_whep_routes_register(void);
void zms_webrtc_whip_routes_register(void);
#endif

void zms_http_route_register(const zms_http_route_ops *ops)
{
    int i;

    if (!ops || !ops->name || !ops->handle) {
        return;
    }
    for (i = 0; i < g_route_count; ++i) {
        if (g_routes[i] && strcmp(g_routes[i]->name, ops->name) == 0) {
            g_routes[i] = ops;
            return;
        }
    }
    if (g_route_count < ZMS_HTTP_ROUTE_MAX) {
        g_routes[g_route_count++] = ops;
    }
}

int zms_http_router_dispatch(zms_http_session *sess, const zms_http_request *req)
{
    int i;

    if (!sess || !req) {
        return 0;
    }
    zms_http_routes_register_all();
    for (i = 0; i < g_route_count; ++i) {
        const zms_http_route_ops *route = g_routes[i];
        if (route && (!route->match || route->match(req))) {
            route->handle(sess, req);
            return 1;
        }
    }
    return 0;
}

void zms_http_routes_register_all(void)
{
    static int registered; /* 启动阶段，单线程 */

    if (registered) {
        return;
    }
    registered = 1;
#if defined(ZMS_ENABLE_WEBRTC)
    zms_webrtc_whep_routes_register();
    zms_webrtc_whip_routes_register();
#endif
    zms_webapi_http_routes_register();
    zms_vod_http_routes_register();
    zms_http_hls_routes_register();
    zms_http_dash_routes_register();
    zms_http_ts_routes_register();
    zms_http_flv_routes_register();
}
