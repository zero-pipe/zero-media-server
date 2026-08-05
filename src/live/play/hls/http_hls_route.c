#include "session/http/http_router.h"
#include "session/http/http_session_internal.h"
#include "zms/engine/stream/stream_hub.h"
#include "zms/vod/io/vod_source.h"
#include <string.h>

static int hls_route_match(const zms_http_request *req)
{
    char app[ZMS_APP_MAX];
    char stream[ZMS_STREAM_MAX];
    char hls_file[128];

    return req && zms_http_route_parse_hls_path(req->path, app, stream, hls_file, sizeof(hls_file));
}

static void hls_route_handle(zms_http_session *hs, const zms_http_request *req)
{
    zms_media_source *src;
    char app[ZMS_APP_MAX];
    char stream[ZMS_STREAM_MAX];
    char hls_file[128];

    if (strcmp(req->method, "GET") != 0) {
        zms_http_response_send_error(hs, 405, "Method Not Allowed");
        return;
    }
    if (!zms_http_route_parse_hls_path(req->path, app, stream, hls_file, sizeof(hls_file))) {
        zms_http_response_send_error(hs, 404, "Not Found");
        return;
    }
    src = zms_media_source_find_for_play(ZMS_SCHEMA_RTMP, app, stream);
    if (!src) {
        zms_http_response_send_error(hs, 404, "Not Found");
        return;
    }
    if (zms_media_source_is_vod(src)) {
        zms_http_vod_hls_serve(hs, src, app, stream, hls_file);
    } else {
        zms_http_live_hls_serve(hs, src, app, stream, hls_file);
    }
}

static const zms_http_route_ops k_hls_route = {
    .name = "hls",
    .match = hls_route_match,
    .handle = hls_route_handle,
};

void zms_http_hls_routes_register(void)
{
    zms_http_route_register(&k_hls_route);
}
