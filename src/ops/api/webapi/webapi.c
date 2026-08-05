#include "zms/ops/api/webapi/webapi.h"
#include "zms/live/proxy/live_pull_proxy.h"
#include "zms/session/rtp/rtp_ps_server.h"
#include "zms/ops/service/config.h"
#include "zms/ops/service/pull_ssl.h"
#include "zms/util/buf_pool.h"
#include "zms/version.h"
#include "zms/ops/api/json/media_json.h"
#include "zms/engine/stream/stream_hub.h"
#include "zms/engine/media_event.h"
#include "zms/vod/io/vod_source.h"
#include "zms/egress/egress_mp4_recorder.h"
#include "ztk/net/socket.h"
#include "ztk/platform.h"
#include "ztk/util/log.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

typedef struct zms_webapi_query_map {
    char schema[ZMS_SCHEMA_MAX];
    char vhost[64];
    char app[ZMS_APP_MAX];
    char stream[ZMS_STREAM_MAX];
    char stream_id[ZMS_STREAM_MAX];
    char port[16];
    char secret[128];
    char force[8];
    char url[ZMS_CFG_URL_MAX];
    char key[256];
    char rtp_transport[16];
    char proxy_prefix[64];
    char pt[8];
    char ssrc[16];
    char tcp_mode[8];
    char dst_url[128];
    char dst_port[16];
    char file_path[ZMS_CFG_PATH_MAX];
    char stamp[32];
    char speed[16];
} zms_webapi_query_map;

typedef struct zms_webapi_json_buf {
    char *buf;
    size_t cap;
    size_t len;
} zms_webapi_json_buf;

/** 服务标记启动时的单调 ms（0=未设置）。 */
static uint64_t g_webapi_boot_ms;

void zms_web_api_note_boot(void)
{
    if (g_webapi_boot_ms == 0) {
        g_webapi_boot_ms = ztk_monotonic_ms();
    }
}

static uint64_t webapi_uptime_sec(void)
{
    uint64_t now;

    if (g_webapi_boot_ms == 0) {
        return 0;
    }
    now = ztk_monotonic_ms();
    return now > g_webapi_boot_ms ? (now - g_webapi_boot_ms) / 1000ull : 0;
}

static double buf_pool_hit_rate(const ztk_buf_pool_stats *st)
{
    uint64_t denom;

    if (!st) {
        return 0.0;
    }
    denom = st->acquire_hit + st->acquire_miss;
    if (denom == 0) {
        return 0.0;
    }
    return (double)st->acquire_hit * 100.0 / (double)denom;
}

static void split_path_query(const char *path_with_query, char *path, size_t path_cap, char *query,
                             size_t query_cap)
{
    path[0] = query[0] = '\0';
    if (!path_with_query) {
        return;
    }
    const char *q = strchr(path_with_query, '?');
    if (q) {
        size_t plen = (size_t)(q - path_with_query);
        if (plen >= path_cap) {
            plen = path_cap - 1;
        }
        memcpy(path, path_with_query, plen);
        path[plen] = '\0';
        strncpy(query, q + 1, query_cap - 1);
    } else {
        strncpy(path, path_with_query, path_cap - 1);
    }
}

static void url_decode_inplace(char *s)
{
    if (!s) {
        return;
    }
    char *w = s;
    for (char *r = s; *r; ++r) {
        if (*r == '+') {
            *w++ = ' ';
        } else if (*r == '%' && r[1] && r[2]) {
            char hex[3] = {r[1], r[2], '\0'};
            *w++ = (char)strtol(hex, NULL, 16);
            r += 2;
        } else {
            *w++ = *r;
        }
    }
    *w = '\0';
}

static void parse_query(const char *query, zms_webapi_query_map *m)
{
    if (!query || !m) {
        return;
    }
    char buf[1024];
    strncpy(buf, query, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
#ifdef _WIN32
    char *ctx = NULL;
    for (char *tok = strtok_s(buf, "&", &ctx); tok; tok = strtok_s(NULL, "&", &ctx)) {
#else
    for (char *tok = strtok(buf, "&"); tok; tok = strtok(NULL, "&")) {
#endif
        char *eq = strchr(tok, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        char *key = tok;
        char *val = eq + 1;
        url_decode_inplace(key);
        url_decode_inplace(val);
        if (strcmp(key, "schema") == 0) {
            strncpy(m->schema, val, sizeof(m->schema) - 1);
        } else if (strcmp(key, "vhost") == 0) {
            strncpy(m->vhost, val, sizeof(m->vhost) - 1);
        } else if (strcmp(key, "app") == 0) {
            strncpy(m->app, val, sizeof(m->app) - 1);
        } else if (strcmp(key, "stream") == 0) {
            strncpy(m->stream, val, sizeof(m->stream) - 1);
        } else if (strcmp(key, "stream_id") == 0) {
            strncpy(m->stream_id, val, sizeof(m->stream_id) - 1);
        } else if (strcmp(key, "port") == 0) {
            strncpy(m->port, val, sizeof(m->port) - 1);
        } else if (strcmp(key, "secret") == 0) {
            strncpy(m->secret, val, sizeof(m->secret) - 1);
        } else if (strcmp(key, "force") == 0) {
            strncpy(m->force, val, sizeof(m->force) - 1);
        } else if (strcmp(key, "url") == 0) {
            strncpy(m->url, val, sizeof(m->url) - 1);
        } else if (strcmp(key, "key") == 0) {
            strncpy(m->key, val, sizeof(m->key) - 1);
        } else if (strcmp(key, "rtp_transport") == 0) {
            strncpy(m->rtp_transport, val, sizeof(m->rtp_transport) - 1);
        } else if (strcmp(key, "proxy_prefix") == 0) {
            strncpy(m->proxy_prefix, val, sizeof(m->proxy_prefix) - 1);
        } else if (strcmp(key, "pt") == 0) {
            strncpy(m->pt, val, sizeof(m->pt) - 1);
        } else if (strcmp(key, "ssrc") == 0) {
            strncpy(m->ssrc, val, sizeof(m->ssrc) - 1);
        } else if (strcmp(key, "tcp_mode") == 0) {
            strncpy(m->tcp_mode, val, sizeof(m->tcp_mode) - 1);
        } else if (strcmp(key, "dst_url") == 0) {
            strncpy(m->dst_url, val, sizeof(m->dst_url) - 1);
        } else if (strcmp(key, "dst_port") == 0) {
            strncpy(m->dst_port, val, sizeof(m->dst_port) - 1);
        } else if (strcmp(key, "file_path") == 0) {
            strncpy(m->file_path, val, sizeof(m->file_path) - 1);
        } else if (strcmp(key, "stamp") == 0) {
            strncpy(m->stamp, val, sizeof(m->stamp) - 1);
        } else if (strcmp(key, "speed") == 0) {
            strncpy(m->speed, val, sizeof(m->speed) - 1);
        }
    }
}

static int proxy_stream_is_auto(const char *stream)
{
    if (!stream || !stream[0]) {
        return 1;
    }
    return strcmp(stream, "auto") == 0;
}

static zms_rtsp_rtp_mode parse_rtp_transport_param(const char *s)
{
    if (!s || !s[0]) {
        return ZMS_RTSP_RTP_TCP;
    }
#ifdef _WIN32
    if (_stricmp(s, "udp") == 0) {
        return ZMS_RTSP_RTP_UDP;
    }
    if (_stricmp(s, "auto") == 0) {
        return ZMS_RTSP_RTP_AUTO;
    }
#else
    if (strcasecmp(s, "udp") == 0) {
        return ZMS_RTSP_RTP_UDP;
    }
    if (strcasecmp(s, "auto") == 0) {
        return ZMS_RTSP_RTP_AUTO;
    }
#endif
    return ZMS_RTSP_RTP_TCP;
}

static int is_localhost_peer(ztk_tcp_session *tcp)
{
    if (!tcp) {
        return 0;
    }
    ztk_socket *sock = ztk_tcp_session_socket(tcp);
    if (!sock) {
        return 0;
    }
    char ip[64];
    if (ztk_socket_get_peer(sock, ip, sizeof(ip), NULL) != ZTK_OK) {
        return 0;
    }
    return strcmp(ip, "127.0.0.1") == 0 || strcmp(ip, "::1") == 0;
}

static int check_secret(ztk_tcp_session *tcp, const char *cfg_secret, const char *req_secret)
{
    if (!cfg_secret || !cfg_secret[0]) {
        return 1;
    }
    if (is_localhost_peer(tcp) && (!req_secret || !req_secret[0])) {
        return 1;
    }
    return req_secret && strcmp(req_secret, cfg_secret) == 0;
}

static int jb_append(zms_webapi_json_buf *jb, const char *fmt, ...)
{
    zms_json_buf zjb = {jb->buf, jb->cap, jb->len};
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(zjb.buf + zjb.len, zjb.cap - zjb.len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= zjb.cap - zjb.len) {
        return -1;
    }
    jb->len = zjb.len + (size_t)n;
    return 0;
}

static zms_media_server_ports ports_from_cfg(const zms_config *cfg)
{
    zms_media_server_ports p = {0};
    if (cfg) {
        p.rtmp = cfg->rtmp.port;
        p.rtsp = cfg->rtsp.port;
        p.http = cfg->http.port;
        p.srt = cfg->srt.port;
    }
    return p;
}

static int append_media_json(zms_media_source *s, zms_webapi_json_buf *jb, int *first,
                             const zms_media_server_ports *ports)
{
    zms_json_buf zjb = {jb->buf, jb->cap, jb->len};
    int r = zms_json_append_media_item(&zjb, s, first, ports);
    jb->len = zjb.len;
    return r;
}

typedef struct zms_webapi_list_ctx {
    zms_webapi_json_buf *jb;
    int first;
    zms_media_server_ports ports;
} zms_webapi_list_ctx;

static int list_visit(zms_media_source *src, void *user)
{
    zms_webapi_list_ctx *ctx = (zms_webapi_list_ctx *)user;
    return append_media_json(src, ctx->jb, &ctx->first, &ctx->ports);
}

static size_t respond_json(int code, const char *msg, const char *extra, char *body,
                           size_t body_cap)
{
    if (extra && extra[0]) {
        return (size_t)snprintf(body, body_cap, "{\"code\":%d,\"msg\":\"%s\",%s}", code, msg,
                                extra);
    }
    return (size_t)snprintf(body, body_cap, "{\"code\":%d,\"msg\":\"%s\"}", code, msg);
}

static size_t handle_get_media_list(const zms_webapi_query_map *q, zms_webapi_json_buf *jb,
                                    const zms_config *cfg)
{
    zms_media_source_filter filter = {
        .schema = q->schema[0] ? q->schema : NULL,
        .app = q->app[0] ? q->app : NULL,
        .stream = q->stream[0] ? q->stream : NULL,
    };
    zms_webapi_list_ctx ctx = {jb, 1, ports_from_cfg(cfg)};
    if (jb_append(jb, "{\"code\":0,\"data\":[") != 0) {
        return 0;
    }
    zms_media_source_foreach(list_visit, &ctx, &filter);
    if (jb_append(jb, "]}") != 0) {
        return 0;
    }
    return jb->len;
}

static size_t handle_close_stream(const zms_webapi_query_map *q, char *body, size_t body_cap,
                                  int *http_status)
{
    if (!q->app[0] || !q->stream[0]) {
        *http_status = 200;
        return respond_json(ZMS_API_INVALID_ARGS, "missing app/stream", NULL, body, body_cap);
    }
    const char *schema = q->schema[0] ? q->schema : ZMS_SCHEMA_RTMP;
    zms_media_source *src = zms_media_source_find_api(schema, q->app, q->stream);
    if (!src || !zms_media_source_is_online(src)) {
        *http_status = 200;
        return respond_json(ZMS_API_NOT_FOUND, "can not find the stream", NULL, body, body_cap);
    }
    int force = q->force[0] && q->force[0] != '0';
    int ok = zms_media_source_close(src, force);
    *http_status = 200;
    if (ok) {
        return respond_json(ZMS_API_SUCCESS, "success", "\"result\":0", body, body_cap);
    }
    return respond_json(ZMS_API_OTHER_FAILED, "close failed", "\"result\":-1", body, body_cap);
}

static size_t handle_get_media_info(const zms_webapi_query_map *q, zms_webapi_json_buf *jb,
                                    const zms_config *cfg)
{
    if (!q->schema[0] || !q->app[0] || !q->stream[0]) {
        return 0;
    }
    zms_media_source *src = zms_media_source_find_api(q->schema, q->app, q->stream);
    if (!src || !zms_media_source_is_online(src)) {
        return 0;
    }
    zms_json_buf zjb = {jb->buf, jb->cap, jb->len};
    zms_media_server_ports ports = ports_from_cfg(cfg);
    if (zms_json_write_media_info(&zjb, src, &ports) != 0) {
        return 0;
    }
    jb->len = zjb.len;
    return jb->len;
}

typedef struct zms_webapi_close_streams_ctx {
    const zms_webapi_query_map *q;
    int count_hit;
    int count_closed;
    int force;
} zms_webapi_close_streams_ctx;

static int close_streams_visit(zms_media_source *src, void *user)
{
    zms_webapi_close_streams_ctx *ctx = (zms_webapi_close_streams_ctx *)user;
    ++ctx->count_hit;
    if (zms_media_source_close(src, ctx->force)) {
        ++ctx->count_closed;
    }
    return 0;
}

static size_t handle_close_streams(const zms_webapi_query_map *q, char *body, size_t body_cap,
                                   int *http_status)
{
    zms_webapi_close_streams_ctx ctx = {q, 0, 0, q->force[0] && q->force[0] != '0'};
    zms_media_source_filter filter = {
        .schema = q->schema[0] ? q->schema : NULL,
        .app = q->app[0] ? q->app : NULL,
        .stream = q->stream[0] ? q->stream : NULL,
    };
    zms_media_source_foreach(close_streams_visit, &ctx, &filter);
    *http_status = 200;
    return (size_t)snprintf(body, body_cap, "{\"code\":0,\"count_hit\":%d,\"count_closed\":%d}",
                            ctx.count_hit, ctx.count_closed);
}

static size_t handle_get_server_config(char *body, size_t body_cap, int *http_status)
{
    *http_status = 200;
    return (size_t)snprintf(body, body_cap,
                            "{\"code\":0,\"data\":["
                            "[\"api.apiDebug\",\"0\"],"
                            "[\"general.mediaServerId\",\"zms\"],"
                            "[\"protocol.enable_rtmp\",\"1\"],"
                            "[\"protocol.enable_rtsp\",\"1\"],"
                            "[\"protocol.enable_hls\",\"1\"]"
                            "]}");
}

static size_t handle_get_api_list(char *body, size_t body_cap)
{
    return (size_t)snprintf(body, body_cap,
                            "{\"code\":0,\"data\":["
                            "\"/index/api/getApiList\","
                            "\"/index/api/version\","
                            "\"/index/api/health\","
                            "\"/index/api/getBufPoolStats\","
                            "\"/index/api/getStatistic\","
                            "\"/index/api/getServerConfig\","
                            "\"/index/api/getMediaList\","
                            "\"/index/api/getMediaInfo\","
                            "\"/index/api/isMediaOnline\","
                            "\"/index/api/close_stream\","
                            "\"/index/api/close_streams\","
                            "\"/index/api/addStreamProxy\","
                            "\"/index/api/delStreamProxy\","
                            "\"/index/api/listStreamProxy\","
                            "\"/index/api/openRtpServer\","
                            "\"/index/api/connectRtpServer\","
                            "\"/index/api/closeRtpServer\","
                            "\"/index/api/listRtpServer\","
                            "\"/index/api/loadMP4File\","
                            "\"/index/api/downloadFile\","
                            "\"/index/api/deleteRecordFile\","
                            "\"/index/api/seekRecordStamp\","
                            "\"/index/api/setRecordSpeed\""
                            "]}");
}

typedef struct zms_webapi_rtp_ps_list_ctx {
    zms_webapi_json_buf *jb;
    int first;
} zms_webapi_rtp_ps_list_ctx;

static int rtp_ps_list_visit(const char *key, zms_rtp_ps_server_slot *slot, void *user)
{
    zms_webapi_rtp_ps_list_ctx *ctx = (zms_webapi_rtp_ps_list_ctx *)user;

    if (!ctx || !ctx->jb || !slot) {
        return 0;
    }
    if (!ctx->first && jb_append(ctx->jb, ",") != 0) {
        return -1;
    }
    ctx->first = 0;
    if (jb_append(
            ctx->jb, "{\"key\":\"%s\",\"app\":\"%s\",\"stream\":\"%s\",\"port\":%u,\"pt\":%d}", key,
            zms_rtp_ps_server_app(slot), zms_rtp_ps_server_stream(slot),
            (unsigned)zms_rtp_ps_server_port(slot), zms_rtp_ps_server_payload_type(slot)) != 0) {
        return -1;
    }
    return 0;
}

static size_t handle_list_rtp_server(zms_webapi_json_buf *jb)
{
    if (jb_append(jb, "{\"code\":0,\"data\":[") != 0) {
        return 0;
    }
    zms_webapi_rtp_ps_list_ctx ctx = {jb, 1};
    zms_rtp_ps_server_foreach(rtp_ps_list_visit, &ctx);
    if (jb_append(jb, "]}") != 0) {
        return 0;
    }
    return jb->len;
}

static size_t handle_open_rtp_server(const zms_webapi_query_map *q, const zms_web_api_opts *opts,
                                     char *body, size_t body_cap, int *http_status)
{
    zms_rtp_ps_server_open_opts ropts;
    zms_rtp_ps_server_slot *slot = NULL;
    uint16_t bound_port = 0;
    const char *app;
    const char *stream;
    char key[256];

    *http_status = 200;
    if (!opts || !opts->poller) {
        return respond_json(ZMS_API_OTHER_FAILED, "no poller for openRtpServer", NULL, body,
                            body_cap);
    }

    app = q->app[0] ? q->app : "live";
    stream = q->stream[0] ? q->stream : (q->stream_id[0] ? q->stream_id : NULL);
    if (!stream) {
        return respond_json(ZMS_API_INVALID_ARGS, "missing stream", NULL, body, body_cap);
    }

    memset(&ropts, 0, sizeof(ropts));
    ropts.poller = opts->poller;
    ropts.vhost = q->vhost[0] ? q->vhost : NULL;
    ropts.app = app;
    ropts.stream = stream;
    ropts.port = q->port[0] ? (uint16_t)atoi(q->port) : 0;
    ropts.payload_type = q->pt[0] ? atoi(q->pt) : ZMS_RTP_PS_DEFAULT_PT;
    if (q->ssrc[0]) {
        ropts.ssrc = (uint32_t)strtoul(q->ssrc, NULL, 10);
        ropts.enable_ssrc_filter = 1;
    }
    ropts.tcp_mode = q->tcp_mode[0] ? atoi(q->tcp_mode) : ZMS_RTP_PS_TCP_UDP;
    ropts.poller_pool = opts->poller_pool;

    if (zms_rtp_ps_server_open(&ropts, &slot, &bound_port) != ZTK_OK || !slot) {
        return respond_json(ZMS_API_OTHER_FAILED, "openRtpServer failed", NULL, body, body_cap);
    }

    zms_rtp_ps_server_make_key(ropts.vhost, app, stream, key, sizeof(key));
    ztk_info(
        "[GB28181 zms 2/6] openRtpServer app=%s stream=%s port=%u pt=%d tcp_mode=%d ssrc_filter=%s",
        app, stream, (unsigned)bound_port, zms_rtp_ps_server_payload_type(slot), ropts.tcp_mode,
        q->ssrc[0] ? q->ssrc : "off");
    return (size_t)snprintf(body, body_cap,
                            "{\"code\":0,\"data\":{\"key\":\"%s\",\"app\":\"%s\",\"stream\":\"%s\","
                            "\"port\":%u,\"pt\":%d}}",
                            key, app, stream, (unsigned)bound_port,
                            zms_rtp_ps_server_payload_type(slot));
}

static size_t handle_connect_rtp_server(const zms_webapi_query_map *q, const zms_web_api_opts *opts,
                                        char *body, size_t body_cap, int *http_status)
{
    const char *app;
    const char *stream;
    const char *host;
    uint16_t port;

    *http_status = 200;
    if (!opts || !opts->poller) {
        return respond_json(ZMS_API_OTHER_FAILED, "no poller for connectRtpServer", NULL, body,
                            body_cap);
    }

    app = q->app[0] ? q->app : "live";
    stream = q->stream[0] ? q->stream : (q->stream_id[0] ? q->stream_id : NULL);
    host = q->dst_url[0] ? q->dst_url : NULL;
    port = q->dst_port[0] ? (uint16_t)atoi(q->dst_port) : 0;
    if (!stream || !host || port == 0) {
        return respond_json(ZMS_API_INVALID_ARGS, "missing stream/dst_url/dst_port", NULL, body,
                            body_cap);
    }

    if (zms_rtp_ps_server_connect(opts->poller, q->vhost[0] ? q->vhost : NULL, app, stream, host,
                                  port) != ZTK_OK) {
        return respond_json(ZMS_API_OTHER_FAILED, "connectRtpServer failed", NULL, body, body_cap);
    }

    ztk_debug("[GB28181 zms 3/6] connectRtpServer app=%s stream=%s -> %s:%u", app, stream, host,
              (unsigned)port);
    return (size_t)snprintf(body, body_cap, "{\"code\":0,\"msg\":\"success\"}");
}

static size_t handle_close_rtp_server(const zms_webapi_query_map *q, char *body, size_t body_cap,
                                      int *http_status)
{
    zms_rtp_ps_server_slot *slot = NULL;
    int flag = 0;

    *http_status = 200;
    if (q->key[0]) {
        slot = zms_rtp_ps_server_find_by_key(q->key);
    } else if (q->app[0] && (q->stream[0] || q->stream_id[0])) {
        char key[256];
        const char *stream = q->stream[0] ? q->stream : q->stream_id;
        zms_rtp_ps_server_make_key(q->vhost[0] ? q->vhost : NULL, q->app, stream, key, sizeof(key));
        slot = zms_rtp_ps_server_find_by_key(key);
    }
    if (slot) {
        zms_rtp_ps_server_close(slot);
        flag = 1;
    }
    return (size_t)snprintf(body, body_cap, "{\"code\":0,\"data\":{\"flag\":%s}}",
                            flag ? "true" : "false");
}

static size_t handle_is_media_online(const zms_webapi_query_map *q, char *body, size_t body_cap,
                                     int *http_status)
{
    *http_status = 200;
    if (!q->schema[0] || !q->app[0] || !q->stream[0]) {
        return respond_json(ZMS_API_INVALID_ARGS, "missing schema/app/stream", NULL, body,
                            body_cap);
    }
    zms_media_source *src = zms_media_source_find_api(q->schema, q->app, q->stream);
    int online = src && zms_media_source_is_online(src);
    return (size_t)snprintf(body, body_cap, "{\"code\":0,\"online\":%s}",
                            online ? "true" : "false");
}

static size_t handle_version(char *body, size_t body_cap, int *http_status)
{
    *http_status = 200;
    return (size_t)snprintf(body, body_cap, "{\"code\":0,\"data\":{\"version\":\"%s\"}}",
                            ZMS_VERSION_STRING);
}

typedef struct zms_webapi_health_ctx {
    int media_source;
    int total_reader;
    size_t max_gop_queue_lag;
    size_t total_pending;
    uint64_t total_dropped;
} zms_webapi_health_ctx;

static int health_visit(zms_media_source *src, void *user)
{
    zms_webapi_health_ctx *ctx = (zms_webapi_health_ctx *)user;
    zms_media_stats_view st;

    if (!zms_media_source_is_online(src)) {
        return 0;
    }
    ++ctx->media_source;
    ctx->total_reader += src->reader_count;
    zms_media_stats_fill(src, src->gop_queue, &st);
    ctx->total_pending += st.gop_queue_pending;
    ctx->total_dropped += st.dropped_frames;
    if (st.gop_queue_max_lag > ctx->max_gop_queue_lag) {
        ctx->max_gop_queue_lag = st.gop_queue_max_lag;
    }
    return 0;
}

/** 进程健康：无需 secret（负载均衡探活）；含环滞后与全局池摘要 */
static size_t handle_health(char *body, size_t body_cap, int *http_status)
{
    zms_webapi_health_ctx ctx;
    ztk_buf_pool_stats st;
    int have_pool;

    memset(&ctx, 0, sizeof(ctx));
    memset(&st, 0, sizeof(st));
    zms_media_source_foreach(health_visit, &ctx, NULL);
    have_pool = zms_buf_pool_get_stats(&st);
    *http_status = 200;
    return (size_t)snprintf(
        body, body_cap,
        "{\"code\":0,\"data\":{"
        "\"status\":\"ok\","
        "\"version\":\"%s\","
        "\"uptimeSec\":%llu,"
        "\"MediaSource\":%d,"
        "\"totalReaderCount\":%d,"
        "\"totalFrameRingPending\":%zu,"
        "\"maxFrameRingLag\":%zu,"
        "\"totalDroppedFrames\":%llu,"
        "\"bufPool\":{"
        "\"enabled\":%s,"
        "\"hit\":%llu,\"miss\":%llu,\"oversize\":%llu,"
        "\"cached\":%llu,\"dropped\":%llu,"
        "\"hitRate\":%.2f"
        "}}}",
        ZMS_VERSION_STRING, (unsigned long long)webapi_uptime_sec(), ctx.media_source,
        ctx.total_reader, ctx.total_pending, ctx.max_gop_queue_lag,
        (unsigned long long)ctx.total_dropped, have_pool ? "true" : "false",
        (unsigned long long)st.acquire_hit, (unsigned long long)st.acquire_miss,
        (unsigned long long)st.acquire_oversize, (unsigned long long)st.release_cached,
        (unsigned long long)st.release_dropped, buf_pool_hit_rate(&st));
}

static size_t handle_get_buf_pool_stats(char *body, size_t body_cap, int *http_status)
{
    ztk_buf_pool_stats st;
    int have_pool;

    memset(&st, 0, sizeof(st));
    have_pool = zms_buf_pool_get_stats(&st);
    *http_status = 200;
    if (!have_pool) {
        return (size_t)snprintf(body, body_cap,
                                "{\"code\":0,\"data\":{\"enabled\":false,"
                                "\"note\":\"global pool off or not hybrid/global mode\"}}");
    }
    return (size_t)snprintf(body, body_cap,
                            "{\"code\":0,\"data\":{"
                            "\"enabled\":true,"
                            "\"mode\":%d,"
                            "\"hit\":%llu,\"miss\":%llu,\"oversize\":%llu,"
                            "\"cached\":%llu,\"dropped\":%llu,"
                            "\"hitRate\":%.2f"
                            "}}",
                            zms_buf_pool_mode(), (unsigned long long)st.acquire_hit,
                            (unsigned long long)st.acquire_miss,
                            (unsigned long long)st.acquire_oversize,
                            (unsigned long long)st.release_cached,
                            (unsigned long long)st.release_dropped, buf_pool_hit_rate(&st));
}

typedef struct zms_webapi_stat_ctx {
    int media_source;
    int total_reader;
    uint64_t total_ingress_bytes;
    uint64_t total_egress_bytes;
    int64_t total_bytes_speed;
    int64_t total_egress_speed;
    size_t max_gop_queue_lag;
    size_t total_pending;
    size_t max_pending;
    size_t total_gop_count;
    uint64_t total_dropped;
} zms_webapi_stat_ctx;

static int stat_visit(zms_media_source *src, void *user)
{
    zms_webapi_stat_ctx *ctx = (zms_webapi_stat_ctx *)user;
    zms_media_stats_view st;

    if (!zms_media_source_is_online(src)) {
        return 0;
    }
    ++ctx->media_source;
    ctx->total_reader += src->reader_count;
    zms_media_stats_fill(src, src->gop_queue, &st);
    ctx->total_ingress_bytes += st.ingress_bytes;
    ctx->total_egress_bytes += st.egress_bytes;
    ctx->total_bytes_speed += st.bytes_speed;
    ctx->total_egress_speed += st.egress_speed;
    ctx->total_pending += st.gop_queue_pending;
    ctx->total_gop_count += st.gop_queue_gop_count;
    ctx->total_dropped += st.dropped_frames;
    if (st.gop_queue_max_lag > ctx->max_gop_queue_lag) {
        ctx->max_gop_queue_lag = st.gop_queue_max_lag;
    }
    if (st.gop_queue_pending > ctx->max_pending) {
        ctx->max_pending = st.gop_queue_pending;
    }
    return 0;
}

static size_t handle_get_statistic(char *body, size_t body_cap, int *http_status)
{
    zms_webapi_stat_ctx ctx;
    ztk_buf_pool_stats pst;
    int have_pool;

    memset(&ctx, 0, sizeof(ctx));
    memset(&pst, 0, sizeof(pst));
    zms_media_source_foreach(stat_visit, &ctx, NULL);
    have_pool = zms_buf_pool_get_stats(&pst);
    *http_status = 200;
    return (size_t)snprintf(
        body, body_cap,
        "{\"code\":0,\"data\":{"
        "\"uptimeSec\":%llu,"
        "\"MediaSource\":%d,"
        "\"totalReaderCount\":%d,"
        "\"totalBytes\":%llu,"
        "\"bytesSpeed\":%lld,"
        "\"totalEgressBytes\":%llu,"
        "\"egressSpeed\":%lld,"
        "\"totalFrameRingPending\":%zu,"
        "\"maxFrameRingPending\":%zu,"
        "\"maxFrameRingLag\":%zu,"
        "\"totalGopCount\":%zu,"
        "\"totalDroppedFrames\":%llu,"
        "\"bufPool\":{"
        "\"enabled\":%s,"
        "\"hit\":%llu,\"miss\":%llu,\"oversize\":%llu,"
        "\"cached\":%llu,\"dropped\":%llu,"
        "\"hitRate\":%.2f"
        "}}}",
        (unsigned long long)webapi_uptime_sec(), ctx.media_source, ctx.total_reader,
        (unsigned long long)ctx.total_ingress_bytes, (long long)ctx.total_bytes_speed,
        (unsigned long long)ctx.total_egress_bytes, (long long)ctx.total_egress_speed,
        ctx.total_pending, ctx.max_pending, ctx.max_gop_queue_lag, ctx.total_gop_count,
        (unsigned long long)ctx.total_dropped, have_pool ? "true" : "false",
        (unsigned long long)pst.acquire_hit, (unsigned long long)pst.acquire_miss,
        (unsigned long long)pst.acquire_oversize, (unsigned long long)pst.release_cached,
        (unsigned long long)pst.release_dropped, buf_pool_hit_rate(&pst));
}

typedef struct zms_webapi_proxy_list_ctx {
    zms_webapi_json_buf *jb;
    int first;
} zms_webapi_proxy_list_ctx;

static int proxy_list_visit(const char *key, zms_live_pull_proxy *p, void *user)
{
    zms_webapi_proxy_list_ctx *ctx = (zms_webapi_proxy_list_ctx *)user;
    (void)p;
    if (jb_append(ctx->jb, "%s\"%s\"", ctx->first ? "" : ",", key) != 0) {
        return -1;
    }
    ctx->first = 0;
    return 0;
}

static size_t handle_list_stream_proxy(zms_webapi_json_buf *jb)
{
    if (jb_append(jb, "{\"code\":0,\"data\":[") != 0) {
        return 0;
    }
    zms_webapi_proxy_list_ctx ctx = {jb, 1};
    zms_live_pull_proxy_foreach(proxy_list_visit, &ctx);
    if (jb_append(jb, "]}") != 0) {
        return 0;
    }
    return jb->len;
}

static size_t handle_add_stream_proxy(const zms_webapi_query_map *q, const zms_web_api_opts *opts,
                                      char *body, size_t body_cap, int *http_status)
{
    *http_status = 200;
    if (!opts || !opts->poller) {
        return respond_json(ZMS_API_OTHER_FAILED, "no poller for addStreamProxy", NULL, body,
                            body_cap);
    }
    if (!q->url[0]) {
        return respond_json(ZMS_API_INVALID_ARGS, "missing url", NULL, body, body_cap);
    }

    const zms_config *cfg = opts->cfg;
    const char *app = q->app[0] ? q->app : (cfg && cfg->proxy.app[0] ? cfg->proxy.app : "live");
    const char *prefix = q->proxy_prefix[0]
                             ? q->proxy_prefix
                             : (cfg && cfg->proxy.prefix[0] ? cfg->proxy.prefix : NULL);

    char stream_auto[ZMS_STREAM_MAX];
    const char *stream = q->stream;
    if (proxy_stream_is_auto(q->stream)) {
        if (zms_live_pull_proxy_build_stream(q->url, prefix, stream_auto, sizeof(stream_auto)) !=
            0) {
            return respond_json(ZMS_API_INVALID_ARGS, "derive stream name failed", NULL, body,
                                body_cap);
        }
        stream = stream_auto;
    }

    char key[256];
    zms_live_pull_proxy_make_key(q->vhost[0] ? q->vhost : NULL, app, stream, key, sizeof(key));
    zms_live_pull_proxy *old = zms_live_pull_proxy_find_by_key(key);
    if (old) {
        zms_live_pull_proxy_destroy(old);
    }

    zms_live_pull_proxy_opts popts = {
        .poller = opts->poller,
        .pull_url = q->url,
        .ssl_ctx = zms_pull_ssl_ctx(cfg),
        .app = app,
        .stream = stream,
        .proxy_prefix = prefix,
        .rtp_mode = parse_rtp_transport_param(q->rtp_transport),
        .retry_count = -1,
        .reconnect_delay_ms = 3000,
    };
    zms_live_pull_proxy *proxy = zms_live_pull_proxy_create(&popts);
    if (!proxy || zms_live_pull_proxy_start(proxy) != ZTK_OK) {
        zms_live_pull_proxy_destroy(proxy);
        return respond_json(ZMS_API_OTHER_FAILED, "addStreamProxy failed", NULL, body, body_cap);
    }
    return (size_t)snprintf(
        body, body_cap, "{\"code\":0,\"data\":{\"key\":\"%s\",\"app\":\"%s\",\"stream\":\"%s\"}}",
        key, zms_live_pull_proxy_app(proxy), zms_live_pull_proxy_stream(proxy));
}

static size_t handle_del_stream_proxy(const zms_webapi_query_map *q, char *body, size_t body_cap,
                                      int *http_status)
{
    *http_status = 200;
    if (!q->key[0]) {
        return respond_json(ZMS_API_INVALID_ARGS, "missing key", NULL, body, body_cap);
    }
    zms_live_pull_proxy *p = zms_live_pull_proxy_find_by_key(q->key);
    int flag = 0;
    if (p) {
        zms_live_pull_proxy_destroy(p);
        flag = 1;
    }
    return (size_t)snprintf(body, body_cap, "{\"code\":0,\"data\":{\"flag\":%s}}",
                            flag ? "true" : "false");
}

static size_t handle_load_mp4_file(const zms_webapi_query_map *q, const zms_web_api_opts *opts,
                                   char *body, size_t body_cap, int *http_status)
{
    zms_media_source *src;
    ztk_poller *pol;
    const char *app;
    const char *stream;

    *http_status = 200;
    app = q->app[0] ? q->app : "mp4_record";
    stream = q->stream[0] ? q->stream : NULL;
    if (!stream || !q->file_path[0]) {
        return respond_json(ZMS_API_INVALID_ARGS, "missing stream/file_path", NULL, body, body_cap);
    }
    if (!zms_mp4_recorder_path_under_root(q->file_path)) {
        return respond_json(ZMS_API_INVALID_ARGS, "file_path not under www/record or www", NULL,
                            body, body_cap);
    }
    {
        struct stat st;
        if (stat(q->file_path, &st) != 0 || st.st_size <= 0) {
            return respond_json(ZMS_API_NOT_FOUND, "file not found", NULL, body, body_cap);
        }
    }
    pol = opts && opts->poller ? opts->poller : zms_media_events_poller();
    if (!pol) {
        return respond_json(ZMS_API_OTHER_FAILED, "no poller", NULL, body, body_cap);
    }
    src = zms_media_source_find_api(ZMS_SCHEMA_RTMP, app, stream);
    if (src && zms_media_source_is_vod(src)) {
        return respond_json(ZMS_API_SUCCESS, "success", NULL, body, body_cap);
    }
    src = zms_vod_source_open(app, stream, q->file_path, pol);
    if (!src) {
        return respond_json(ZMS_API_OTHER_FAILED, "loadMP4File failed", NULL, body, body_cap);
    }
    return respond_json(ZMS_API_SUCCESS, "success", NULL, body, body_cap);
}

static size_t handle_delete_record_file(const zms_webapi_query_map *q, char *body, size_t body_cap,
                                        int *http_status)
{
    *http_status = 200;
    if (!q->file_path[0]) {
        return respond_json(ZMS_API_INVALID_ARGS, "missing file_path", NULL, body, body_cap);
    }
    if (!zms_mp4_recorder_path_under_root(q->file_path)) {
        return respond_json(ZMS_API_INVALID_ARGS, "file_path not under www/record or www", NULL,
                            body, body_cap);
    }
    if (remove(q->file_path) != 0) {
        return respond_json(ZMS_API_OTHER_FAILED, "delete failed", NULL, body, body_cap);
    }
    return respond_json(ZMS_API_SUCCESS, "success", NULL, body, body_cap);
}

static size_t handle_seek_record_stamp(const zms_webapi_query_map *q, char *body, size_t body_cap,
                                       int *http_status)
{
    zms_media_source *src;
    const char *app;
    const char *stream;
    uint64_t stamp_ms;
    uint64_t actual;

    *http_status = 200;
    app = q->app[0] ? q->app : "mp4_record";
    stream = q->stream[0] ? q->stream : NULL;
    if (!stream || !q->stamp[0]) {
        return respond_json(ZMS_API_INVALID_ARGS, "missing stream/stamp", NULL, body, body_cap);
    }
    stamp_ms = (uint64_t)strtod(q->stamp, NULL);
    src = zms_media_source_find_api(q->schema[0] ? q->schema : ZMS_SCHEMA_RTMP, app, stream);
    if (!src) {
        src = zms_media_source_find_api(ZMS_SCHEMA_RTMP, app, stream);
    }
    if (!src || !zms_media_source_is_vod(src)) {
        return respond_json(ZMS_API_NOT_FOUND, "can not find the stream", NULL, body, body_cap);
    }
    actual = zms_vod_source_seek_ms(src, stamp_ms);
    return (size_t)snprintf(body, body_cap, "{\"code\":0,\"msg\":\"success\",\"data\":%llu}",
                            (unsigned long long)actual);
}

static size_t handle_set_record_speed(const zms_webapi_query_map *q, char *body, size_t body_cap,
                                      int *http_status)
{
    (void)q;
    *http_status = 200;
    return respond_json(ZMS_API_SUCCESS, "success", NULL, body, body_cap);
}

size_t zms_web_api_handle(ztk_tcp_session *tcp, const char *method, const char *path_with_query,
                          const zms_web_api_opts *opts, int *http_status, char *body,
                          size_t body_cap)
{
    if (!http_status || !body || body_cap < 64) {
        return 0;
    }
    *http_status = 500;
    body[0] = '\0';

    if (!method || (strcmp(method, "GET") != 0 && strcmp(method, "POST") != 0)) {
        *http_status = 405;
        return respond_json(ZMS_API_INVALID_ARGS, "method not allowed", NULL, body, body_cap);
    }

    char path[512];
    char query[1024];
    split_path_query(path_with_query, path, sizeof(path), query, sizeof(query));

    zms_webapi_query_map q;
    memset(&q, 0, sizeof(q));
    parse_query(query, &q);

    /* 探活：不要求 secret，便于负载均衡 */
    if (strcmp(path, "/index/api/health") == 0) {
        return handle_health(body, body_cap, http_status);
    }

    const char *cfg_secret = opts ? opts->api_secret : NULL;
    if (!check_secret(tcp, cfg_secret, q.secret)) {
        *http_status = 200;
        return respond_json(ZMS_API_AUTH_FAILED, "auth failed", NULL, body, body_cap);
    }

    if (strcmp(path, "/index/api/getApiList") == 0 || strcmp(path, "/index/") == 0 ||
        strcmp(path, "/index") == 0) {
        *http_status = 200;
        return handle_get_api_list(body, body_cap);
    }

    if (strcmp(path, "/index/api/getMediaList") == 0) {
        zms_webapi_json_buf jb = {body, body_cap, 0};
        size_t n = handle_get_media_list(&q, &jb, opts ? opts->cfg : NULL);
        if (n == 0) {
            *http_status = 500;
            return respond_json(ZMS_API_OTHER_FAILED, "response too large", NULL, body, body_cap);
        }
        *http_status = 200;
        return n;
    }

    if (strcmp(path, "/index/api/close_stream") == 0) {
        return handle_close_stream(&q, body, body_cap, http_status);
    }

    if (strcmp(path, "/index/api/getMediaInfo") == 0) {
        zms_webapi_json_buf jb = {body, body_cap, 0};
        size_t n = handle_get_media_info(&q, &jb, opts ? opts->cfg : NULL);
        if (n == 0) {
            *http_status = 200;
            return respond_json(ZMS_API_NOT_FOUND, "can not find the stream", NULL, body, body_cap);
        }
        *http_status = 200;
        return n;
    }

    if (strcmp(path, "/index/api/close_streams") == 0) {
        return handle_close_streams(&q, body, body_cap, http_status);
    }

    if (strcmp(path, "/index/api/isMediaOnline") == 0) {
        return handle_is_media_online(&q, body, body_cap, http_status);
    }

    if (strcmp(path, "/index/api/version") == 0) {
        return handle_version(body, body_cap, http_status);
    }

    if (strcmp(path, "/index/api/getBufPoolStats") == 0) {
        return handle_get_buf_pool_stats(body, body_cap, http_status);
    }

    if (strcmp(path, "/index/api/getStatistic") == 0) {
        return handle_get_statistic(body, body_cap, http_status);
    }

    if (strcmp(path, "/index/api/getServerConfig") == 0) {
        return handle_get_server_config(body, body_cap, http_status);
    }

    if (strcmp(path, "/index/api/addStreamProxy") == 0) {
        return handle_add_stream_proxy(&q, opts, body, body_cap, http_status);
    }

    if (strcmp(path, "/index/api/delStreamProxy") == 0) {
        return handle_del_stream_proxy(&q, body, body_cap, http_status);
    }

    if (strcmp(path, "/index/api/listStreamProxy") == 0) {
        zms_webapi_json_buf jb = {body, body_cap, 0};
        size_t n = handle_list_stream_proxy(&jb);
        if (n == 0) {
            *http_status = 500;
            return respond_json(ZMS_API_OTHER_FAILED, "response too large", NULL, body, body_cap);
        }
        *http_status = 200;
        return n;
    }

    if (strcmp(path, "/index/api/openRtpServer") == 0) {
        return handle_open_rtp_server(&q, opts, body, body_cap, http_status);
    }

    if (strcmp(path, "/index/api/connectRtpServer") == 0) {
        return handle_connect_rtp_server(&q, opts, body, body_cap, http_status);
    }

    if (strcmp(path, "/index/api/closeRtpServer") == 0) {
        return handle_close_rtp_server(&q, body, body_cap, http_status);
    }

    if (strcmp(path, "/index/api/listRtpServer") == 0) {
        zms_webapi_json_buf jb = {body, body_cap, 0};
        size_t n = handle_list_rtp_server(&jb);
        if (n == 0) {
            *http_status = 500;
            return respond_json(ZMS_API_OTHER_FAILED, "response too large", NULL, body, body_cap);
        }
        *http_status = 200;
        return n;
    }

    if (strcmp(path, "/index/api/loadMP4File") == 0) {
        return handle_load_mp4_file(&q, opts, body, body_cap, http_status);
    }

    if (strcmp(path, "/index/api/deleteRecordFile") == 0) {
        return handle_delete_record_file(&q, body, body_cap, http_status);
    }

    if (strcmp(path, "/index/api/seekRecordStamp") == 0) {
        return handle_seek_record_stamp(&q, body, body_cap, http_status);
    }

    if (strcmp(path, "/index/api/setRecordSpeed") == 0) {
        return handle_set_record_speed(&q, body, body_cap, http_status);
    }

    /* downloadFile 由 HTTP 路由层直接流式发送文件 */
    if (strcmp(path, "/index/api/downloadFile") == 0) {
        *http_status = 200;
        return respond_json(ZMS_API_OTHER_FAILED, "use http handler for download", NULL, body,
                            body_cap);
    }

    *http_status = 404;
    return respond_json(ZMS_API_NOT_FOUND, "api not found", NULL, body, body_cap);
}
