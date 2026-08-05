#include "zms/webrtc/webrtc_whep.h"
#include "session/http/http_router.h"
#include "session/http/http_session_internal.h"
#include "webrtc/session/webrtc_session_internal.h"
#include "zms/webrtc/webrtc_service.h"
#include "zms/ops/service/config.h"
#include "ztk/net/tcp_server.h"
#include "ztk/util/log.h"
#include <stdio.h>
#include <string.h>

static int webrtc_http_enabled(const zms_http_session *hs)
{
    if (!hs || !hs->server || !hs->server->cfg) {
        return 1;
    }
    return hs->server->cfg->webrtc.enable;
}

static void whip_copy_query_value(char *dst, size_t dst_sz, const char *val, size_t val_len)
{
    size_t n;

    if (!dst || dst_sz == 0) {
        return;
    }
    n = val_len < dst_sz - 1 ? val_len : dst_sz - 1;
    if (n > 0) {
        memcpy(dst, val, n);
    }
    dst[n] = '\0';
}

static int whip_query_key_is(const char *key, size_t key_len, const char *name)
{
    size_t n = strlen(name);

    return key_len == n && memcmp(key, name, n) == 0;
}

static int whip_parse_query(const char *path, char *app, char *stream)
{
    const char *q;

    if (app) {
        app[0] = '\0';
    }
    if (stream) {
        stream[0] = '\0';
    }
    if (!path) {
        return 0;
    }
    q = strchr(path, '?');
    if (!q) {
        return 0;
    }
    ++q;
    while (*q) {
        const char *amp = strchr(q, '&');
        const char *eq = strchr(q, '=');
        const char *val;
        size_t key_len;
        size_t val_len;

        if (!eq || (amp && eq >= amp)) {
            if (!amp) {
                break;
            }
            q = amp + 1;
            continue;
        }
        key_len = (size_t)(eq - q);
        val = eq + 1;
        val_len = amp ? (size_t)(amp - val) : strlen(val);
        if (whip_query_key_is(q, key_len, "app") && app) {
            whip_copy_query_value(app, ZMS_APP_MAX, val, val_len);
        } else if (whip_query_key_is(q, key_len, "stream") && stream) {
            whip_copy_query_value(stream, ZMS_STREAM_MAX, val, val_len);
        }
        if (!amp) {
            break;
        }
        q = amp + 1;
    }
    return app && app[0] && stream && stream[0];
}

static int webrtc_whip_route_match(const zms_http_request *req)
{
    if (!req || !req->path[0]) {
        return 0;
    }
    return strncmp(req->path, "/index/api/whip", 15) == 0;
}

static int webrtc_api_path_id(const char *path, const char *api_prefix, char *id, size_t id_cap)
{
    const char *p;
    const char *q;
    size_t n;

    if (!path || !api_prefix || !id || id_cap == 0) {
        return 0;
    }
    n = strlen(api_prefix);
    if (strncmp(path, api_prefix, n) != 0) {
        return 0;
    }
    p = path + n;
    if (p[0] != '/' || !p[1]) {
        return 0;
    }
    ++p;
    q = strchr(p, '?');
    if (q) {
        size_t len = (size_t)(q - p);
        if (len == 0 || len >= id_cap) {
            return 0;
        }
        memcpy(id, p, len);
        id[len] = '\0';
        return id[0] != '\0';
    }
    if (strlen(p) >= id_cap) {
        return 0;
    }
    strcpy(id, p);
    return id[0] != '\0';
}

static void webrtc_whip_delete(zms_http_session *hs, const zms_http_request *req)
{
    char id[ZMS_WEBRTC_SESSION_ID_LEN];
    zms_webrtc_session *sess;

    if (!hs || !req) {
        return;
    }
    if (!webrtc_http_enabled(hs)) {
        zms_http_response_send_error(hs, 503, "Service Unavailable");
        return;
    }
    if (!webrtc_api_path_id(req->path, "/index/api/whip", id, sizeof(id))) {
        zms_http_response_send_error(hs, 404, "Not Found");
        return;
    }
    sess = zms_webrtc_session_find(id);
    if (!sess || sess->mode != ZMS_WEBRTC_SESSION_PUBLISH) {
        zms_http_response_send_error(hs, 404, "Not Found");
        return;
    }
    ztk_info("[webrtc] WHIP DELETE id=%s", id);
    zms_webrtc_session_destroy(sess);
    zms_http_response_send_bytes(hs, 200, "text/plain", "", 0);
}

static void webrtc_whip_post(zms_http_session *hs, const zms_http_request *req)
{
    zms_webrtc_session *sess;
    char app[ZMS_APP_MAX];
    char stream[ZMS_STREAM_MAX];
    char answer[4096];
    char location[128];
    size_t answer_len = 0;
    ztk_poller *pol;

    if (!hs || !req) {
        return;
    }
    if (!webrtc_http_enabled(hs)) {
        zms_http_response_send_error(hs, 503, "Service Unavailable");
        return;
    }
    if (!zms_webrtc_service_instance()) {
        zms_http_response_send_error(hs, 503, "Service Unavailable");
        return;
    }
    if (!whip_parse_query(req->path, app, stream) || !req->body || req->body_len == 0) {
        zms_http_response_send_error(hs, 400, "Bad Request");
        return;
    }

    pol = zms_webrtc_service_resolve_poller(hs);
    if (!pol) {
        zms_http_response_send_error(hs, 500, "Internal Server Error");
        return;
    }

    sess = zms_webrtc_session_create_publish(app, stream, pol);
    if (!sess) {
        zms_http_response_send_error(hs, 503, "Service Unavailable");
        return;
    }

    if (zms_webrtc_session_build_publish_answer(sess, req->body, req->body_len, answer,
                                                sizeof(answer), &answer_len) != 0) {
        zms_webrtc_session_destroy(sess);
        zms_http_response_send_error(hs, 400, "Bad Request");
        return;
    }

    snprintf(location, sizeof(location), "/index/api/whip/%s", sess->id);
    ztk_info("[webrtc] WHIP answer app=%s stream=%s id=%s port=%u", app, stream, sess->id,
             (unsigned)sess->port);

    {
        char hdr[512];
        int hn = snprintf(hdr, sizeof(hdr),
                          "HTTP/1.1 201 Created\r\n"
                          "Content-Type: application/sdp\r\n"
                          "Location: %s\r\n"
                          "Access-Control-Allow-Origin: *\r\n"
                          "Content-Length: %zu\r\n\r\n",
                          location, answer_len);
        ztk_tcp_session_send(hs->tcp, hdr, (size_t)hn);
        ztk_tcp_session_send(hs->tcp, answer, answer_len);
        ztk_tcp_session_flush(hs->tcp);
    }
}

void zms_webrtc_whip_handle(zms_http_session *hs, const zms_http_request *req)
{
    if (!hs || !req || !req->method[0]) {
        zms_http_response_send_error(hs, 405, "Method Not Allowed");
        return;
    }
    if (strcmp(req->method, "POST") == 0) {
        webrtc_whip_post(hs, req);
        return;
    }
    if (strcmp(req->method, "DELETE") == 0) {
        webrtc_whip_delete(hs, req);
        return;
    }
    zms_http_response_send_error(hs, 405, "Method Not Allowed");
}

static void webrtc_whip_route_handle(zms_http_session *hs, const zms_http_request *req)
{
    zms_webrtc_whip_handle(hs, req);
}

static const zms_http_route_ops k_webrtc_whip_route = {
    .name = "webrtc-whip",
    .match = webrtc_whip_route_match,
    .handle = webrtc_whip_route_handle,
};

void zms_webrtc_whip_routes_register(void)
{
    zms_http_route_register(&k_webrtc_whip_route);
}
