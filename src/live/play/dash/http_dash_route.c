#include "session/http/http_router.h"
#include "session/http/http_session_internal.h"
#include "zms/engine/stream/stream_hub.h"
#include "zms/vod/io/vod_source.h"
#include "ztk/util/log.h"
#include <string.h>

static int dash_route_match(const zms_http_request *req)
{
    char app[ZMS_APP_MAX];
    char stream[ZMS_STREAM_MAX];
    char file[128];

    return req && zms_http_route_parse_dash_path(req->path, app, stream, file, sizeof(file));
}

static void dash_route_handle(zms_http_session *hs, const zms_http_request *req)
{
    zms_media_source *src;
    char app[ZMS_APP_MAX];
    char stream[ZMS_STREAM_MAX];
    char file[128];

    if (strcmp(req->method, "GET") != 0) {
        zms_http_response_send_error(hs, 405, "Method Not Allowed");
        return;
    }
    if (!zms_http_route_parse_dash_path(req->path, app, stream, file, sizeof(file))) {
        zms_http_response_send_error(hs, 404, "Not Found");
        return;
    }
    src = zms_media_source_find_for_play(ZMS_SCHEMA_RTMP, app, stream);
    if (!src || zms_media_source_is_vod(src) || !src->gop_queue) {
        ztk_warn("DASH live 404: no live source app=%s stream=%s src=%p vod=%d ring=%p", app,
                 stream, (void *)src, src ? zms_media_source_is_vod(src) : 0,
                 src ? (void *)src->gop_queue : NULL);
        zms_http_response_send_error(hs, 404, "Not Found");
        return;
    }
    zms_http_live_dash_serve(hs, src, app, stream, file);
}

static const zms_http_route_ops k_dash_route = {
    .name = "dash",
    .match = dash_route_match,
    .handle = dash_route_handle,
};

void zms_http_dash_routes_register(void)
{
    zms_http_route_register(&k_dash_route);
}
