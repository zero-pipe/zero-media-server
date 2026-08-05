#include "zms/ops/api/webhook/webhook_client.h"
#include "zms/ops/api/json/media_json.h"
#include "zms/engine/stream/stream_hub.h"
#include "zms/engine/media_event.h"
#include "zms/version.h"
#include "ztk/net/tcp_server.h"
#include "ztk/net/socket.h"
#include "ztk/util/log.h"
#include "ztk/poller/poller.h"
#include "ztk/platform.h"
#include "ztk/util/timer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET zms_sock_t;
#define ZMS_SOCK_INVALID INVALID_SOCKET
#define zms_sock_close(s) closesocket(s)
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int zms_sock_t;
#define ZMS_SOCK_INVALID (-1)
#define zms_sock_close(s) close(s)
#endif

#define ZMS_DEFAULT_VHOST "__defaultVhost__"
#define ZMS_HOOK_BODY_MAX 8192

static ztk_poller *g_poller;
static zms_hook_config g_hook;
static int g_webhook_initialized;
static uint64_t g_hook_index;
static char g_play_host[64];
static unsigned g_play_http_port;

static void queue_hook(const char *url, const char *body, const zms_media_tuple *tuple,
                       int none_reader);

typedef struct zms_webhook_task {
    char url[ZMS_CFG_HOOK_URL_MAX];
    char body[ZMS_HOOK_BODY_MAX];
    zms_media_tuple tuple;
    int none_reader;
    int retry_left;
} zms_webhook_task;

static int schema_allowed(const char *schema)
{
    if (!schema || !schema[0]) {
        return 0;
    }
    if (!g_hook.stream_changed_schemas[0]) {
        return 1;
    }
    char buf[ZMS_CFG_SCHEMA_FILTER_MAX];
    strncpy(buf, g_hook.stream_changed_schemas, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
#ifdef _WIN32
    char *ctx = NULL;
    for (char *tok = strtok_s(buf, "/", &ctx); tok; tok = strtok_s(NULL, "/", &ctx)) {
#else
    for (char *tok = strtok(buf, "/"); tok; tok = strtok(NULL, "/")) {
#endif
        while (*tok == ' ' || *tok == '\t') {
            ++tok;
        }
        if (strcmp(tok, schema) == 0) {
            return 1;
        }
    }
    return 0;
}

static int json_append_common(char *body, size_t cap, size_t *len, const char *prefix)
{
    uint64_t idx = ++g_hook_index;
    int n = snprintf(body + *len, cap - *len, "%s\"mediaServerId\":\"%s\",\"hook_index\":%llu",
                     prefix ? prefix : "", g_hook.media_server_id, (unsigned long long)idx);
    if (n < 0 || (size_t)n >= cap - *len) {
        return -1;
    }
    *len += (size_t)n;
    return 0;
}

static int build_stream_changed_regist(zms_media_source *src, int regist, char *body, size_t cap)
{
    if (!src || !body) {
        return -1;
    }
    size_t len = 0;
    if (regist) {
        char item[4096];
        zms_json_buf jb = {item, sizeof(item), 0};
        int first = 1;
        if (zms_json_append_media_item(&jb, src, &first, NULL) != 0 || jb.len < 2) {
            return -1;
        }
        int n = snprintf(body, cap, "{%.*s", (int)(jb.len - 2), item + 1);
        if (n < 0 || (size_t)n >= cap) {
            return -1;
        }
        len = (size_t)n;
    } else {
        int n = snprintf(body, cap,
                         "{\"schema\":\"%s\",\"vhost\":\"%s\",\"app\":\"%s\",\"stream\":\"%s\"",
                         src->schema, ZMS_DEFAULT_VHOST, src->app, src->stream);
        if (n < 0 || (size_t)n >= cap) {
            return -1;
        }
        len = (size_t)n;
    }
    if (json_append_common(body, cap, &len, ",") != 0) {
        return -1;
    }
    int n = snprintf(body + len, cap - len, ",\"regist\":%s}", regist ? "true" : "false");
    if (n < 0 || (size_t)n >= cap - len) {
        return -1;
    }
    return 0;
}

static int build_tuple_body(const zms_media_tuple *t, const char *player_schema, char *body,
                            size_t cap)
{
    if (!t || !body) {
        return -1;
    }
    const char *proto = player_schema && player_schema[0] ? player_schema : t->schema;
    int n = snprintf(body, cap,
                     "{\"schema\":\"%s\",\"protocol\":\"%s\",\"vhost\":\"%s\",\"app\":\"%s\","
                     "\"stream\":\"%s\",\"params\":\"\"",
                     t->schema, proto, ZMS_DEFAULT_VHOST, t->app, t->stream);
    if (n < 0 || (size_t)n >= cap) {
        return -1;
    }
    size_t len = (size_t)n;
    if (json_append_common(body, cap, &len, ",") != 0) {
        return -1;
    }
    n = snprintf(body + len, cap - len, "}");
    if (n < 0 || (size_t)n >= cap - len) {
        return -1;
    }
    return 0;
}

typedef struct zms_webhook_parsed_url {
    char host[256];
    uint16_t port;
    char path[512];
} zms_webhook_parsed_url;

static int parse_http_url(const char *url, zms_webhook_parsed_url *out)
{
    if (!url || !out || strncmp(url, "http://", 7) != 0) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    const char *p = url + 7;
    const char *slash = strchr(p, '/');
    char *colon = NULL;
    size_t host_len;
    if (slash) {
        host_len = (size_t)(slash - p);
        strncpy(out->path, slash, sizeof(out->path) - 1);
    } else {
        host_len = strlen(p);
        strncpy(out->path, "/", sizeof(out->path) - 1);
    }
    if (host_len >= sizeof(out->host)) {
        return -1;
    }
    memcpy(out->host, p, host_len);
    out->host[host_len] = '\0';
    colon = strchr(out->host, ':');
    out->port = 80;
    if (colon) {
        *colon = '\0';
        out->port = (uint16_t)atoi(colon + 1);
        if (out->port == 0) {
            out->port = 80;
        }
    }
    if (!out->path[0]) {
        strncpy(out->path, "/", sizeof(out->path) - 1);
    }
    return 0;
}

static int set_sock_timeout(zms_sock_t fd, int timeout_ms)
{
#if defined(_WIN32)
    DWORD tv = (DWORD)timeout_ms;
    return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv)) == 0 ? 0 : -1;
#else
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

static int http_post_sync(const char *url, const char *json_body, char *resp, size_t resp_cap)
{
    zms_webhook_parsed_url pu;
    if (parse_http_url(url, &pu) != 0) {
        return -1;
    }

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)pu.port);

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(pu.host, port_str, &hints, &res) != 0 || !res) {
        return -1;
    }

    zms_sock_t fd = ZMS_SOCK_INVALID;
    int ok = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd == ZMS_SOCK_INVALID) {
            continue;
        }
        if (connect(fd, ai->ai_addr, (int)ai->ai_addrlen) != 0) {
            zms_sock_close(fd);
            fd = ZMS_SOCK_INVALID;
            continue;
        }
        break;
    }
    freeaddrinfo(res);
    if (fd == ZMS_SOCK_INVALID) {
        return -1;
    }

    int timeout_ms = (int)(g_hook.timeout_sec * 1000.f);
    if (timeout_ms <= 0) {
        timeout_ms = 10000;
    }
    set_sock_timeout(fd, timeout_ms);

    char req[ZMS_HOOK_BODY_MAX + 1024];
    size_t body_len = json_body ? strlen(json_body) : 0;
    int req_len =
        snprintf(req, sizeof(req),
                 "POST %s HTTP/1.1\r\n"
                 "Host: %s:%u\r\n"
                 "Content-Type: application/json\r\n"
                 "Content-Length: %zu\r\n"
                 "Connection: close\r\n"
                 "\r\n"
                 "%s",
                 pu.path, pu.host, (unsigned)pu.port, body_len, json_body ? json_body : "");
    if (req_len < 0 || (size_t)req_len >= sizeof(req)) {
        zms_sock_close(fd);
        return -1;
    }

#if defined(_WIN32)
    if (send(fd, req, req_len, 0) != req_len) {
#else
    if (send(fd, req, (size_t)req_len, 0) != (ssize_t)req_len) {
#endif
        zms_sock_close(fd);
        return -1;
    }

    if (resp && resp_cap > 0) {
        resp[0] = '\0';
        size_t total = 0;
        for (;;) {
            char buf[1024];
#if defined(_WIN32)
            int n = recv(fd, buf, (int)sizeof(buf), 0);
#else
            ssize_t n = recv(fd, buf, sizeof(buf), 0);
#endif
            if (n <= 0) {
                break;
            }
            if (total + (size_t)n >= resp_cap) {
                n = (int)(resp_cap - total - 1);
            }
            memcpy(resp + total, buf, (size_t)n);
            total += (size_t)n;
            resp[total] = '\0';
            if (total >= resp_cap - 1) {
                break;
            }
        }
    }

    zms_sock_close(fd);
    ok = 0;
    return ok;
}

static uint64_t hook_retry_fire(void *user);

/* zms_media_origin_str() 定义于 media_event.h */

static void fill_peer(ztk_tcp_session *tcp, char *ip, size_t ip_cap, uint16_t *port, char *id,
                      size_t id_cap)
{
    if (ip && ip_cap) {
        ip[0] = '\0';
    }
    if (port) {
        *port = 0;
    }
    if (id && id_cap) {
        snprintf(id, id_cap, "-");
    }
    if (!tcp) {
        return;
    }
    ztk_socket *sock = ztk_tcp_session_socket(tcp);
    if (!sock) {
        return;
    }
    if (ip && ip_cap) {
        (void)ztk_socket_get_peer(sock, ip, ip_cap, port);
    }
    if (id && id_cap) {
        snprintf(id, id_cap, "%p", (void *)tcp);
    }
}

static const char *http_body_start(const char *resp)
{
    if (!resp) {
        return "";
    }
    const char *p = strstr(resp, "\r\n\r\n");
    return p ? p + 4 : resp;
}

static int hook_json_code_ok(const char *resp)
{
    const char *body = http_body_start(resp);
    const char *code = strstr(body, "\"code\"");
    if (!code) {
        return 0;
    }
    code = strchr(code, ':');
    if (!code) {
        return 0;
    }
    ++code;
    while (*code == ' ' || *code == '\t') {
        ++code;
    }
    return (int)strtol(code, NULL, 10) == 0;
}

/** 解析 JSON 布尔字段：true / 1 → 1 */
static int hook_json_bool_field(const char *resp, const char *key)
{
    char pat[64];
    const char *body;
    const char *p;

    if (!resp || !key || !key[0]) {
        return 0;
    }
    body = http_body_start(resp);
    if (snprintf(pat, sizeof(pat), "\"%s\"", key) >= (int)sizeof(pat)) {
        return 0;
    }
    p = strstr(body, pat);
    if (!p) {
        return 0;
    }
    p = strchr(p + strlen(pat), ':');
    if (!p) {
        return 0;
    }
    ++p;
    while (*p == ' ' || *p == '\t') {
        ++p;
    }
    if (strncmp(p, "true", 4) == 0 || *p == '1') {
        return 1;
    }
    return 0;
}

static int hook_auth_post(const char *url, const char *json_body)
{
    char resp[4096];
    resp[0] = '\0';
    if (http_post_sync(url, json_body, resp, sizeof(resp)) != 0) {
        return 0;
    }
    return hook_json_code_ok(resp);
}

static int build_publish_auth_body(zms_media_source *src, zms_media_origin origin, const char *ip,
                                   uint16_t port, const char *id, char *body, size_t cap)
{
    if (!src || !body) {
        return -1;
    }
    int n = snprintf(
        body, cap,
        "{\"schema\":\"%s\",\"protocol\":\"%s\",\"vhost\":\"%s\",\"app\":\"%s\",\"stream\":\"%s\","
        "\"params\":\"\","
        "\"ip\":\"%s\",\"port\":%u,\"id\":\"%s\",\"originType\":%d,\"originTypeStr\":\"%s\"",
        src->schema, src->schema, ZMS_DEFAULT_VHOST, src->app, src->stream, ip ? ip : "",
        (unsigned)port, id ? id : "", (int)origin, zms_media_origin_str(origin));
    if (n < 0 || (size_t)n >= cap) {
        return -1;
    }
    size_t len = (size_t)n;
    if (json_append_common(body, cap, &len, ",") != 0) {
        return -1;
    }
    n = snprintf(body + len, cap - len, "}");
    if (n < 0 || (size_t)n >= cap - len) {
        return -1;
    }
    return 0;
}

int zms_webhook_allow_publish(zms_media_source *src, zms_media_origin origin, ztk_tcp_session *tcp,
                              const char *id)
{
    char resp[4096];

    if (!src) {
        return 0;
    }
    src->enable_mp4 = 0;
    if (!g_webhook_initialized || !g_hook.on_publish[0]) {
        return 1;
    }
    char ip[64];
    uint16_t port = 0;
    char sid[64];
    fill_peer(tcp, ip, sizeof(ip), &port, sid, sizeof(sid));
    if (id && id[0]) {
        strncpy(sid, id, sizeof(sid) - 1);
    }
    char body[ZMS_HOOK_BODY_MAX];
    if (build_publish_auth_body(src, origin, ip, port, sid, body, sizeof(body)) != 0) {
        ztk_warn("webhook on_publish build body failed app=%s stream=%s", src->app, src->stream);
        return 0;
    }
    resp[0] = '\0';
    if (http_post_sync(g_hook.on_publish, body, resp, sizeof(resp)) != 0) {
        ztk_warn("webhook on_publish HTTP failed url=%s app=%s stream=%s", g_hook.on_publish,
                 src->app, src->stream);
        return 0;
    }
    if (!hook_json_code_ok(resp)) {
        ztk_warn("webhook on_publish rejected app=%s stream=%s resp=%.180s", src->app, src->stream,
                 resp);
        return 0;
    }
    src->enable_mp4 = hook_json_bool_field(resp, "enable_mp4");
    if (src->enable_mp4) {
        ztk_info("webhook enable_mp4=1 app=%s stream=%s", src->app, src->stream);
    }
    return 1;
}

int zms_webhook_allow_play(const zms_media_tuple *tuple, const char *player_schema,
                           ztk_tcp_session *tcp, const char *id)
{
    if (!g_webhook_initialized || !g_hook.on_play[0]) {
        return 1;
    }
    if (!tuple) {
        return 0;
    }
    char ip[64];
    uint16_t port = 0;
    char sid[64];
    fill_peer(tcp, ip, sizeof(ip), &port, sid, sizeof(sid));
    if (id && id[0]) {
        strncpy(sid, id, sizeof(sid) - 1);
    }
    char body[ZMS_HOOK_BODY_MAX];
    const char *proto = player_schema && player_schema[0] ? player_schema : tuple->schema;
    int n = snprintf(body, sizeof(body),
                     "{\"schema\":\"%s\",\"protocol\":\"%s\",\"vhost\":\"%s\",\"app\":\"%s\","
                     "\"stream\":\"%s\",\"params\":\"\","
                     "\"ip\":\"%s\",\"port\":%u,\"id\":\"%s\"",
                     tuple->schema, proto, ZMS_DEFAULT_VHOST, tuple->app, tuple->stream, ip,
                     (unsigned)port, sid);
    if (n < 0 || (size_t)n >= sizeof(body)) {
        return 0;
    }
    size_t len = (size_t)n;
    if (json_append_common(body, sizeof(body), &len, ",") != 0) {
        return 0;
    }
    if ((size_t)snprintf(body + len, sizeof(body) - len, "}") >= sizeof(body) - len) {
        return 0;
    }
    return hook_auth_post(g_hook.on_play, body);
}

void zms_webhook_server_started(const zms_config *cfg)
{
    if (!g_webhook_initialized || !g_hook.on_server_started[0] || !cfg) {
        return;
    }
    char body[ZMS_HOOK_BODY_MAX];
    size_t len = 0;
    int n = snprintf(body, sizeof(body),
                     "{\"version\":\"%s\",\"rtmp.port\":%u,\"rtsp.port\":%u,\"http.port\":%u",
                     ZMS_VERSION_STRING, cfg->rtmp.port, cfg->rtsp.port, cfg->http.port);
    if (n < 0 || (size_t)n >= sizeof(body)) {
        return;
    }
    len = (size_t)n;
    if (json_append_common(body, sizeof(body), &len, ",") != 0) {
        return;
    }
    snprintf(body + len, sizeof(body) - len, "}");
    queue_hook(g_hook.on_server_started, body, NULL, 0);
}

static int hook_response_close_stream(const char *resp)
{
    if (!resp) {
        return 0;
    }
    const char *p = strstr(resp, "\"close\"");
    if (!p) {
        return 0;
    }
    p = strchr(p, ':');
    if (!p) {
        return 0;
    }
    ++p;
    while (*p == ' ' || *p == '\t') {
        ++p;
    }
    return strncmp(p, "true", 4) == 0;
}

static void hook_task_run(void *user)
{
    zms_webhook_task *t = (zms_webhook_task *)user;
    if (!t) {
        return;
    }
    if (!g_webhook_initialized || !g_poller) {
        free(t);
        return;
    }

    char resp[4096];
    resp[0] = '\0';
    int err = http_post_sync(t->url, t->body, resp, sizeof(resp));
    if (err != 0) {
        ztk_warn("webhook POST failed: %s", t->url);
        if (t->retry_left > 0 && g_poller) {
            --t->retry_left;
            int delay_ms = (int)(g_hook.retry_delay_sec * 1000.f);
            if (delay_ms < 0) {
                delay_ms = 0;
            }
            if (ztk_poller_do_delay(g_poller, (uint64_t)delay_ms, hook_retry_fire, t)) {
                return;
            }
        }
        free(t);
        return;
    }

    if (t->none_reader && hook_response_close_stream(resp)) {
        zms_media_source *src =
            zms_media_source_find(t->tuple.schema, t->tuple.app, t->tuple.stream);
        if (!src) {
            src = zms_media_source_find_api(t->tuple.schema, t->tuple.app, t->tuple.stream);
        }
        if (src) {
            ztk_warn("webhook close stream on none_reader: %s/%s", t->tuple.app, t->tuple.stream);
            zms_media_source_close(src, 0);
        }
    }
    free(t);
}

/* delay 回调须为 C 函数指针（修正上文 lambda） */

static uint64_t hook_retry_fire(void *user)
{
    hook_task_run(user);
    return 0;
}

static void queue_hook(const char *url, const char *body, const zms_media_tuple *tuple,
                       int none_reader)
{
    if (!url || !url[0] || !body || !g_poller) {
        return;
    }
    zms_webhook_task *t = (zms_webhook_task *)calloc(1, sizeof(*t));
    if (!t) {
        return;
    }
    strncpy(t->url, url, sizeof(t->url) - 1);
    strncpy(t->body, body, sizeof(t->body) - 1);
    t->retry_left = g_hook.retry_count;
    t->none_reader = none_reader;
    if (tuple) {
        t->tuple = *tuple;
    }
    if (ztk_poller_async(g_poller, hook_task_run, t, 0) != ZTK_OK) {
        free(t);
    }
}

void zms_webhook_init(ztk_poller *poller, const zms_config *cfg)
{
    g_poller = poller;
    g_webhook_initialized = 0;
    g_play_host[0] = '\0';
    g_play_http_port = 0;
    if (!cfg) {
        return;
    }
    g_hook = cfg->hook;
    g_webhook_initialized = g_hook.enable && poller != NULL;
    if (cfg->general.extern_ip[0]) {
        strncpy(g_play_host, cfg->general.extern_ip, sizeof(g_play_host) - 1);
    } else {
        strncpy(g_play_host, "127.0.0.1", sizeof(g_play_host) - 1);
    }
    g_play_host[sizeof(g_play_host) - 1] = '\0';
    g_play_http_port = cfg->http.port ? cfg->http.port : 80;
    ztk_info("webhook init enable=%d on_publish=%s mediaServerId=%s play_base=http://%s:%u",
             g_webhook_initialized, g_hook.on_publish[0] ? g_hook.on_publish : "(none)",
             g_hook.media_server_id[0] ? g_hook.media_server_id : "(none)", g_play_host,
             g_play_http_port);
}

void zms_webhook_fini(void)
{
    g_webhook_initialized = 0;
    if (g_poller) {
        ztk_poller_wake(g_poller);
        ztk_poller_process_pending(g_poller);
        ztk_sleep_ms(50);
        ztk_poller_process_pending(g_poller);
    }
    g_poller = NULL;
}

int zms_webhook_none_reader_configured(void)
{
    return g_webhook_initialized && g_hook.on_stream_none_reader[0] != '\0';
}

void zms_webhook_on_stream_register(zms_media_source *src, zms_media_origin origin)
{
    (void)origin;
    if (!g_webhook_initialized || !g_hook.on_stream_changed[0] || !src) {
        return;
    }
    if (!schema_allowed(src->schema)) {
        return;
    }
    char body[ZMS_HOOK_BODY_MAX];
    if (build_stream_changed_regist(src, 1, body, sizeof(body)) != 0) {
        return;
    }
    ztk_debug("[GB28181 zms 6/6] hook on_stream_changed regist=1 app=%s stream=%s -> %s", src->app,
              src->stream, g_hook.on_stream_changed);
    queue_hook(g_hook.on_stream_changed, body, NULL, 0);
}

void zms_webhook_on_stream_unregister(zms_media_source *src, zms_media_origin origin)
{
    (void)origin;
    if (!g_webhook_initialized || !g_hook.on_stream_changed[0] || !src) {
        return;
    }
    if (!schema_allowed(src->schema)) {
        return;
    }
    char body[ZMS_HOOK_BODY_MAX];
    if (build_stream_changed_regist(src, 0, body, sizeof(body)) != 0) {
        return;
    }
    queue_hook(g_hook.on_stream_changed, body, NULL, 0);
}

void zms_webhook_on_play(const zms_media_tuple *tuple, const char *player_schema)
{
    if (!g_webhook_initialized || !g_hook.on_play[0] || !tuple) {
        return;
    }
    char body[ZMS_HOOK_BODY_MAX];
    if (build_tuple_body(tuple, player_schema, body, sizeof(body)) != 0) {
        return;
    }
    queue_hook(g_hook.on_play, body, NULL, 0);
}

void zms_webhook_on_play_stop(const zms_media_tuple *tuple, const char *player_schema,
                              uint64_t duration_ms)
{
    if (!g_webhook_initialized || !g_hook.on_play_stop[0] || !tuple) {
        return;
    }
    char body[ZMS_HOOK_BODY_MAX];
    if (build_tuple_body(tuple, player_schema, body, sizeof(body)) != 0) {
        return;
    }
    /* 已知时在闭合花括号前追加 duration_ms。 */
    if (duration_ms > 0) {
        size_t len = strlen(body);
        if (len > 0 && body[len - 1] == '}') {
            char extra[48];
            int n = snprintf(extra, sizeof(extra), ",\"durationMs\":%llu}",
                             (unsigned long long)duration_ms);
            if (n > 0 && len - 1 + (size_t)n < sizeof(body)) {
                body[len - 1] = '\0'; /* 去掉闭合花括号 */
                strncat(body, extra, sizeof(body) - len);
            }
        }
    }
    queue_hook(g_hook.on_play_stop, body, NULL, 0);
}

void zms_webhook_on_none_reader(const zms_media_tuple *tuple)
{
    if (!g_webhook_initialized || !g_hook.on_stream_none_reader[0] || !tuple) {
        return;
    }
    char body[ZMS_HOOK_BODY_MAX];
    if (build_tuple_body(tuple, NULL, body, sizeof(body)) != 0) {
        return;
    }
    queue_hook(g_hook.on_stream_none_reader, body, tuple, 1);
}

static void json_escape_path(const char *in, char *out, size_t out_cap)
{
    size_t o = 0;

    if (!out || out_cap == 0) {
        return;
    }
    out[0] = '\0';
    if (!in) {
        return;
    }
    for (; *in && o + 2 < out_cap; ++in) {
        char c = *in;
        if (c == '\\') {
            out[o++] = '/';
        } else if (c == '"') {
            if (o + 3 >= out_cap) {
                break;
            }
            out[o++] = '\\';
            out[o++] = '"';
        } else {
            out[o++] = c;
        }
    }
    out[o] = '\0';
}

static int record_file_to_http_rel(const char *file_path, char *out, size_t out_cap)
{
    char norm[ZMS_CFG_PATH_MAX];
    const char *p;
    size_t i;

    if (!file_path || !file_path[0] || !out || out_cap == 0) {
        return 0;
    }
    strncpy(norm, file_path, sizeof(norm) - 1);
    norm[sizeof(norm) - 1] = '\0';
    for (i = 0; norm[i]; ++i) {
        if (norm[i] == '\\') {
            norm[i] = '/';
        }
    }
    p = strstr(norm, "/record/");
    if (p) {
        p += 1; /* "record/..." */
    } else if (strncmp(norm, "record/", 7) == 0) {
        p = norm;
    } else if (strncmp(norm, "./record/", 9) == 0) {
        p = norm + 2;
    } else {
        return 0;
    }
    if (strstr(p, "..") != NULL) {
        return 0;
    }
    return (size_t)snprintf(out, out_cap, "%s", p) < out_cap;
}

void zms_webhook_on_record_mp4(const char *app, const char *stream, const char *file_name,
                               const char *file_path, const char *folder, int64_t file_size,
                               int64_t start_time, float time_len)
{
    char body[ZMS_HOOK_BODY_MAX];
    char esc_name[256];
    char esc_path[512];
    char esc_folder[512];
    char esc_url[768];
    char rel[ZMS_CFG_PATH_MAX];
    char url[ZMS_CFG_URL_MAX];
    size_t len = 0;
    int n;

    if (!g_webhook_initialized || !g_hook.on_record_mp4[0]) {
        return;
    }
    json_escape_path(file_name ? file_name : "", esc_name, sizeof(esc_name));
    json_escape_path(file_path ? file_path : "", esc_path, sizeof(esc_path));
    json_escape_path(folder ? folder : "", esc_folder, sizeof(esc_folder));
    url[0] = '\0';
    esc_url[0] = '\0';
    if (record_file_to_http_rel(file_path, rel, sizeof(rel)) && g_play_host[0] && g_play_http_port) {
        n = snprintf(url, sizeof(url), "http://%s:%u/%s", g_play_host, g_play_http_port, rel);
        if (n > 0 && (size_t)n < sizeof(url)) {
            json_escape_path(url, esc_url, sizeof(esc_url));
        } else {
            url[0] = '\0';
        }
    }
    n = snprintf(body, sizeof(body),
                 "{\"app\":\"%s\",\"stream\":\"%s\",\"file_name\":\"%s\",\"file_path\":\"%s\","
                 "\"folder\":\"%s\",\"file_size\":%lld,\"start_time\":%lld,\"time_len\":%.3f",
                 app ? app : "", stream ? stream : "", esc_name, esc_path, esc_folder,
                 (long long)file_size, (long long)start_time, (double)time_len);
    if (n < 0 || (size_t)n >= sizeof(body)) {
        return;
    }
    len = (size_t)n;
    if (esc_url[0]) {
        n = snprintf(body + len, sizeof(body) - len, ",\"url\":\"%s\",\"play_url\":\"%s\"", esc_url,
                     esc_url);
        if (n < 0 || (size_t)n >= sizeof(body) - len) {
            return;
        }
        len += (size_t)n;
    }
    if (json_append_common(body, sizeof(body), &len, ",") != 0) {
        return;
    }
    if ((size_t)snprintf(body + len, sizeof(body) - len, "}") >= sizeof(body) - len) {
        return;
    }
    queue_hook(g_hook.on_record_mp4, body, NULL, 0);
}
