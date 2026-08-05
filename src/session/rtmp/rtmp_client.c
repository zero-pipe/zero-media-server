#include "zms/session/rtmp/rtmp_client.h"
#include "zms/engine/media/media_limits.h"
#include "zms/engine/stream/stream_hub.h"
#include "zms/session/rtmp/rtmp_protocol.h"
#include "zms/session/rtmp/rtmp_amf.h"
#include "session/rtmp/rtmp_handshake.h"
#include "zms/ops/service/zms_have_tls.h"
#include "zms/util/buf_pool.h"
#include "ztk/net/tcp_client.h"
#include "ztk/net/tls_client.h"
#include "ztk/util/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RTMP_HS_BODY ZMS_RTMP_HS_SIZE
#define RTMP_HS_REPLY (1 + RTMP_HS_BODY * 2)

typedef enum {
    CL_HS = 0,
    CL_CONNECT,
    CL_CREATE,
    CL_PLAY,
    CL_PLAYING,
} zms_rtsp_client_state;

typedef struct zms_rtmp_url {
    char host[256];
    uint16_t port;
    char app[ZMS_APP_MAX];
    char stream[ZMS_STREAM_MAX];
    char tc_url[512];
    const char *scheme;
    int use_tls;
} zms_rtmp_url;

struct zms_rtmp_client {
    zms_rtmp_client_opts opts;
    ztk_tcp_client *tcp;
    ztk_tls_client *tls;
    zms_rtmp_protocol *proto;
    zms_rtmp_url url;
    zms_rtsp_client_state state;
    double trans_id;
    uint32_t stream_id;
    int stopping;
    int ready_sent;
    int hs_create_sent;
    int hs_play_sent;
    /** 握手收包缓冲（1+S1+S2）；握手完成后释放 */
    uint8_t *hs_buf;
    size_t hs_off;
    uint8_t *send_buf;
    size_t send_cap;
};

static void merge_slash_in_app(char *app, char *stream)
{
    char *slash = strchr(app, '/');
    if (!slash) {
        return;
    }
    *slash = '\0';
    if (!slash[1]) {
        return;
    }
    if (stream[0]) {
        char buf[ZMS_STREAM_MAX];
        snprintf(buf, sizeof(buf), "%s/%s", slash + 1, stream);
        strncpy(stream, buf, sizeof(stream) - 1);
        stream[sizeof(stream) - 1] = '\0';
    } else {
        strncpy(stream, slash + 1, ZMS_STREAM_MAX - 1);
        stream[sizeof(stream) - 1] = '\0';
    }
}

/** URL path → librtmp connect app + play（对 zms_rtmp_session_find_play_source 的入参约定） */
static void rtmp_pull_wire_names(char *app, char *stream)
{
    char *slash;

    if (!app || !stream || !stream[0]) {
        return;
    }
    slash = strchr(stream, '/');
    if (!slash || !slash[1]) {
        return;
    }
    {
        char connect_app[ZMS_APP_MAX];
        size_t prefix_len = (size_t)(slash - stream);
        int n = snprintf(connect_app, sizeof(connect_app), "%s/%.*s", app, (int)prefix_len, stream);
        if (n <= 0 || (size_t)n >= sizeof(connect_app)) {
            return;
        }
        strncpy(app, connect_app, ZMS_APP_MAX - 1);
        app[ZMS_APP_MAX - 1] = '\0';
    }
    memmove(stream, slash + 1, strlen(slash + 1) + 1);
}

ztk_err_t zms_rtmp_parse_pull_url(const char *url, char *connect_app, size_t app_cap,
                                  char *play_name, size_t play_cap)
{
    char app[ZMS_APP_MAX];
    char stream[ZMS_STREAM_MAX];

    if (!url || !connect_app || !play_name || app_cap == 0 || play_cap == 0) {
        return ZTK_ERR_INVALID;
    }
    app[0] = stream[0] = '\0';
    zms_media_split_path(url, app, stream);
    merge_slash_in_app(app, stream);
    if (!app[0] || !stream[0]) {
        return ZTK_ERR_INVALID;
    }
    rtmp_pull_wire_names(app, stream);
    strncpy(connect_app, app, app_cap - 1);
    connect_app[app_cap - 1] = '\0';
    strncpy(play_name, stream, play_cap - 1);
    play_name[play_cap - 1] = '\0';
    return ZTK_OK;
}

static void client_mark_playing(zms_rtmp_client *c)
{
    if (!c || c->stopping) {
        return;
    }
    if (c->state != CL_PLAYING) {
        c->state = CL_PLAYING;
    }
    if (!c->ready_sent && c->opts.on_ready) {
        c->ready_sent = 1;
        c->opts.on_ready(c->opts.user);
    }
}

static void client_fail(zms_rtmp_client *c, ztk_err_t err)
{
    if (!c || c->stopping) {
        return;
    }
    c->state = CL_HS;
    if (c->opts.on_error) {
        c->opts.on_error(err, c->opts.user);
    }
}

static ztk_err_t client_send(zms_rtmp_client *c, const void *data, size_t len)
{
    if (!c || !data || !len) {
        return ZTK_ERR_INVALID;
    }
#if ZMS_HAVE_PULL_TLS
    if (c->tls) {
        return ztk_tls_client_send(c->tls, data, len);
    }
#endif
    if (c->tcp) {
        return ztk_tcp_client_send(c->tcp, data, len);
    }
    return ZTK_ERR_STATE;
}

static ztk_err_t client_connect(zms_rtmp_client *c)
{
    if (!c) {
        return ZTK_ERR_INVALID;
    }
#if ZMS_HAVE_PULL_TLS
    if (c->tls) {
        return ztk_tls_client_connect(c->tls, c->url.host, c->url.port);
    }
#endif
    if (c->tcp) {
        return ztk_tcp_client_connect(c->tcp, c->url.host, c->url.port);
    }
    return ZTK_ERR_STATE;
}

static void client_close(zms_rtmp_client *c)
{
    if (!c) {
        return;
    }
#if ZMS_HAVE_PULL_TLS
    if (c->tls) {
        ztk_tls_client_close(c->tls);
        return;
    }
#endif
    if (c->tcp) {
        ztk_tcp_client_close(c->tcp);
    }
}

static ztk_err_t client_ensure_send_buf(zms_rtmp_client *c)
{
    if (!c) {
        return ZTK_ERR_INVALID;
    }
    if (c->send_cap >= ZMS_MEDIA_IO_BUF_SIZE && c->send_buf) {
        return ZTK_OK;
    }
    if (!zms_buf_pool_slot_resize(&c->send_buf, &c->send_cap, ZMS_MEDIA_IO_BUF_SIZE)) {
        return ZTK_ERR_NOMEM;
    }
    return ZTK_OK;
}

static ztk_err_t append_chunk_msg(zms_rtmp_client *c, uint8_t type_id, uint32_t stream_id,
                                  uint32_t tag_dts_ms, const void *body, size_t body_len,
                                  size_t *pos)
{
    size_t n = 0;
    if (!c || !c->proto || !pos || client_ensure_send_buf(c) != ZTK_OK || *pos >= c->send_cap) {
        return ZTK_ERR_NOMEM;
    }
    ztk_err_t err =
        zms_rtmp_protocol_send_chunk(c->proto, type_id, stream_id, tag_dts_ms, body, body_len,
                                     c->send_buf + *pos, c->send_cap - *pos, &n);
    if (err != ZTK_OK) {
        return err;
    }
    *pos += n;
    return ZTK_OK;
}

static ztk_err_t append_invoke(zms_rtmp_client *c, const char *cmd, double trans_id,
                               uint32_t stream_id, const uint8_t *body, size_t body_len,
                               size_t *pos)
{
    size_t n = 0;
    if (!c || !c->proto || !pos || client_ensure_send_buf(c) != ZTK_OK || *pos >= c->send_cap) {
        return ZTK_ERR_NOMEM;
    }
    ztk_err_t err =
        zms_rtmp_protocol_send_invoke(c->proto, cmd, trans_id, stream_id, body, body_len,
                                      c->send_buf + *pos, c->send_cap - *pos, &n);
    if (err != ZTK_OK) {
        return err;
    }
    *pos += n;
    return ZTK_OK;
}

static void send_invoke(zms_rtmp_client *c, const char *cmd, double trans_id, uint32_t stream_id,
                        const uint8_t *body, size_t body_len)
{
    size_t pos = 0;
    if (append_invoke(c, cmd, trans_id, stream_id, body, body_len, &pos) != ZTK_OK) {
        ztk_warn("rtmp_client send_invoke %s failed", cmd);
        return;
    }
    if (client_send(c, c->send_buf, pos) != ZTK_OK) {
        ztk_warn("rtmp_client send %s tcp failed", cmd);
    }
}

static size_t amf_key(uint8_t *out, size_t cap, const char *key)
{
    size_t klen = key ? strlen(key) : 0;
    if (cap < 2 + klen) {
        return 0;
    }
    out[0] = (uint8_t)((klen >> 8) & 0xff);
    out[1] = (uint8_t)(klen & 0xff);
    if (klen) {
        memcpy(out + 2, key, klen);
    }
    return 2 + klen;
}

static ztk_err_t parse_rtmp_url(const char *url, zms_rtmp_url *out)
{
    if (!url || !out) {
        return ZTK_ERR_INVALID;
    }
    memset(out, 0, sizeof(*out));

    const char *p = url;
    if (strncmp(p, "rtmps://", 8) == 0) {
        out->use_tls = 1;
        out->scheme = "rtmps";
        out->port = 443;
        p += 8;
    } else if (strncmp(p, "rtmp://", 7) == 0) {
        out->scheme = "rtmp";
        out->port = 1935;
        p += 7;
    } else {
        return ZTK_ERR_INVALID;
    }

    const char *at = strchr(p, '@');
    if (at) {
        p = at + 1;
    }

    const char *slash = strchr(p, '/');
    const char *host_end = slash ? slash : p + strlen(p);
    const char *colon = NULL;
    for (const char *c = p; c < host_end; ++c) {
        if (*c == ':') {
            colon = c;
            break;
        }
    }
    size_t host_len = colon ? (size_t)(colon - p) : (size_t)(host_end - p);
    if (host_len == 0 || host_len >= sizeof(out->host)) {
        return ZTK_ERR_INVALID;
    }
    memcpy(out->host, p, host_len);
    out->host[host_len] = '\0';
    if (colon) {
        out->port = (uint16_t)atoi(colon + 1);
        if (out->port == 0) {
            out->port = out->use_tls ? 443 : 1935;
        }
    }

    if (zms_rtmp_parse_pull_url(url, out->app, sizeof(out->app), out->stream,
                                sizeof(out->stream)) != ZTK_OK) {
        return ZTK_ERR_INVALID;
    }

    snprintf(out->tc_url, sizeof(out->tc_url), "%s://%s:%u/%s", out->scheme, out->host, out->port,
             out->app);
    return ZTK_OK;
}

static void write_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)((v >> 24) & 0xff);
    p[1] = (uint8_t)((v >> 16) & 0xff);
    p[2] = (uint8_t)((v >> 8) & 0xff);
    p[3] = (uint8_t)(v & 0xff);
}

static ztk_err_t pack_connect_amf(zms_rtmp_client *c, uint8_t *amf, size_t cap, size_t *out_len)
{
    size_t pos = 0;
    if (!c || !amf || !out_len) {
        return ZTK_ERR_INVALID;
    }
    amf[pos++] = ZMS_AMF_OBJECT;
    pos += amf_key(amf + pos, cap - pos, "app");
    pos += zms_amf_encode_string(amf + pos, cap - pos, c->url.app);
    pos += amf_key(amf + pos, cap - pos, "tcUrl");
    pos += zms_amf_encode_string(amf + pos, cap - pos, c->url.tc_url);
    pos += amf_key(amf + pos, cap - pos, "flashVer");
    pos += zms_amf_encode_string(amf + pos, cap - pos, "FMLE/3.0 (compatible; ZMS)");
    pos += amf_key(amf + pos, cap - pos, "capabilities");
    pos += zms_amf_encode_number(amf + pos, cap - pos, 15.0);
    pos += amf_key(amf + pos, cap - pos, "videoFunction");
    pos += zms_amf_encode_number(amf + pos, cap - pos, 1.0);
    pos += amf_key(amf + pos, cap - pos, "audioCodecs");
    pos += zms_amf_encode_number(amf + pos, cap - pos, 1024.0);
    pos += amf_key(amf + pos, cap - pos, "videoCodecs");
    pos += zms_amf_encode_number(amf + pos, cap - pos, 128.0);
    pos += zms_amf_encode_object_end(amf + pos, cap - pos);
    *out_len = pos;
    return ZTK_OK;
}

static void send_create_stream(zms_rtmp_client *c)
{
    if (!c || c->hs_create_sent) {
        return;
    }
    c->hs_create_sent = 1;
    uint8_t amf[8];
    size_t pos = zms_amf_encode_null(amf, sizeof(amf));
    c->trans_id = 2.0;
    send_invoke(c, "createStream", c->trans_id, 0, amf, pos);
    c->state = CL_CREATE;
    ztk_info("rtmp_client -> createStream");
}

static void send_play(zms_rtmp_client *c)
{
    if (!c || c->hs_play_sent) {
        return;
    }
    c->hs_play_sent = 1;
    uint8_t amf[256];
    size_t pos = 0;
    pos += zms_amf_encode_string(amf + pos, sizeof(amf) - pos, c->url.stream);
    pos += zms_amf_encode_number(amf + pos, sizeof(amf) - pos, -2000.0);
    c->trans_id = 3.0;
    send_invoke(c, "play", c->trans_id, c->stream_id, amf, pos);
    c->state = CL_PLAY;
    ztk_info("rtmp_client play: app=%s stream=%s net_stream=%u", c->url.app, c->url.stream,
             (unsigned)c->stream_id);
}

static void on_chunk(const zms_rtmp_chunk *chunk, void *user);
static void on_cmd(const char *cmd, double trans_id, const uint8_t *amf_rest, size_t amf_rest_len,
                   void *user);

static void begin_rtmp_session(zms_rtmp_client *c)
{
    zms_rtmp_protocol_opts popts = {on_chunk, on_cmd, c};
    uint8_t body[8];
    uint8_t amf[768];
    size_t amf_len = 0;
    size_t pos = 0;

    c->proto = zms_rtmp_protocol_create_established(&popts);
    if (!c->proto) {
        client_fail(c, ZTK_ERR_NOMEM);
        return;
    }
    zms_rtmp_protocol_set_poller(c->proto, c->opts.poller);
    c->hs_create_sent = 0;
    c->hs_play_sent = 0;

    write_be32(body, 4096);
    if (append_chunk_msg(c, ZMS_RTMP_MSG_SET_CHUNK, 0, 0, body, 4, &pos) != ZTK_OK) {
        goto fail;
    }
    zms_rtmp_protocol_set_out_chunk_size(c->proto, 4096);

    write_be32(body, 5000000);
    if (append_chunk_msg(c, ZMS_RTMP_MSG_WIN_SIZE, 0, 0, body, 4, &pos) != ZTK_OK) {
        goto fail;
    }

    body[0] = 2;
    write_be32(body + 1, 5000000);
    if (append_chunk_msg(c, ZMS_RTMP_MSG_SET_PEER_BW, 0, 0, body, 5, &pos) != ZTK_OK) {
        goto fail;
    }

    if (pack_connect_amf(c, amf, sizeof(amf), &amf_len) != ZTK_OK) {
        goto fail;
    }
    c->trans_id = 1.0;
    if (append_invoke(c, "connect", c->trans_id, 0, amf, amf_len, &pos) != ZTK_OK) {
        goto fail;
    }

    if (client_send(c, c->send_buf, pos) != ZTK_OK) {
        goto fail;
    }
    c->state = CL_CONNECT;
    ztk_info("rtmp_client -> connect app=%s tcUrl=%s bytes=%u", c->url.app, c->url.tc_url,
             (unsigned)pos);
    return;

fail:
    ztk_warn("rtmp_client begin_rtmp_session pack/send failed");
    client_fail(c, ZTK_ERR_IO);
}

static void hs_random(uint8_t *buf, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        buf[i] = (uint8_t)(rand() & 0xff);
    }
}

static void client_hs_release(zms_rtmp_client *c)
{
    if (!c) {
        return;
    }
    free(c->hs_buf);
    c->hs_buf = NULL;
    c->hs_off = 0;
}

static void send_c0c1(zms_rtmp_client *c)
{
    uint8_t out[1 + RTMP_HS_BODY];
    uint8_t c1[RTMP_HS_BODY];

    if (!c->hs_buf) {
        c->hs_buf = (uint8_t *)malloc(RTMP_HS_REPLY);
        if (!c->hs_buf) {
            client_fail(c, ZTK_ERR_NOMEM);
            return;
        }
    }
    out[0] = 3;
    hs_random(c1, RTMP_HS_BODY);
    memcpy(out + 1, c1, RTMP_HS_BODY);
    client_send(c, out, sizeof(out));
    c->hs_off = 0;
}

static void try_finish_hs(zms_rtmp_client *c)
{
    uint8_t s1[RTMP_HS_BODY];
    uint8_t c2[RTMP_HS_BODY];

    if (!c->hs_buf || c->hs_off < RTMP_HS_REPLY) {
        return;
    }
    memcpy(s1, c->hs_buf + 1, RTMP_HS_BODY);
    if (zms_rtmp_hs_detect_complex(s1)) {
        memcpy(c2, c->hs_buf + 1 + RTMP_HS_BODY, RTMP_HS_BODY);
    } else {
        memcpy(c2, s1, RTMP_HS_BODY);
    }
    client_send(c, c2, sizeof(c2));
    client_hs_release(c);
    begin_rtmp_session(c);
}

static void on_cmd(const char *cmd, double trans_id, const uint8_t *amf_rest, size_t amf_rest_len,
                   void *user)
{
    zms_rtmp_client *c = (zms_rtmp_client *)user;
    if (!c || c->stopping || !cmd) {
        return;
    }

    if (strcmp(cmd, "_error") == 0) {
        ztk_warn("rtmp_client _error state=%d trans=%.0f", (int)c->state, trans_id);
        client_fail(c, ZTK_ERR_IO);
        return;
    }

    if (strcmp(cmd, "_result") == 0) {
        if (c->state == CL_CONNECT) {
            ztk_info("rtmp_client connect ok trans=%.0f", trans_id);
            send_create_stream(c);
            return;
        }
        if (c->state == CL_CREATE) {
            zms_amf_value *v = NULL;
            size_t off = 0;
            c->stream_id = 1;
            while (amf_rest && off < amf_rest_len) {
                size_t used = 0;
                if (zms_amf_decode(amf_rest + off, amf_rest_len - off, &v, &used) != ZTK_OK || !v) {
                    break;
                }
                off += used;
                if (zms_amf_value_type(v) == ZMS_AMF_NUMBER) {
                    c->stream_id = (uint32_t)zms_amf_value_num(v);
                    zms_amf_value_free(v);
                    break;
                }
                zms_amf_value_free(v);
                v = NULL;
            }
            ztk_info("rtmp_client createStream ok net_stream=%u", (unsigned)c->stream_id);
            send_play(c);
            return;
        }
        ztk_warn("rtmp_client _result ignored state=%d trans=%.0f", (int)c->state, trans_id);
        return;
    }

    if (strcmp(cmd, "onBWDone") == 0) {
        if (!c->hs_create_sent) {
            send_create_stream(c);
        }
        return;
    }

    if (strcmp(cmd, "onStatus") == 0) {
        zms_amf_value *obj = NULL;
        size_t off = 0;
        if (!amf_rest || !amf_rest_len) {
            return;
        }
        if (zms_amf_decode(amf_rest, amf_rest_len, &obj, &off) != ZTK_OK) {
            zms_amf_value_free(obj);
            return;
        }
        if (obj && zms_amf_value_type(obj) == ZMS_AMF_NULL) {
            zms_amf_value_free(obj);
            obj = NULL;
            if (off >= amf_rest_len ||
                zms_amf_decode(amf_rest + off, amf_rest_len - off, &obj, &off) != ZTK_OK) {
                zms_amf_value_free(obj);
                return;
            }
        }
        if (!obj) {
            return;
        }
        const zms_amf_value *code = zms_amf_object_get(obj, "code");
        const char *code_str = code ? zms_amf_value_str(code) : "";
        if (strcmp(code_str, "NetStream.Play.Start") == 0 ||
            strcmp(code_str, "NetStream.Play.Reset") == 0) {
            ztk_info("rtmp_client onStatus: %s", code_str);
            client_mark_playing(c);
        } else if (strstr(code_str, "Play.Failed") || strstr(code_str, "StreamNotFound")) {
            ztk_warn("rtmp_client play failed: %s", code_str);
            client_fail(c, ZTK_ERR_IO);
        } else if (code_str[0] && c->state == CL_PLAY) {
            ztk_info("rtmp_client onStatus: %s", code_str);
        }
        zms_amf_value_free(obj);
        (void)trans_id;
        return;
    }

    if (strcmp(cmd, "onMetaData") == 0) {
        return;
    }
}

static void on_chunk(const zms_rtmp_chunk *chunk, void *user)
{
    zms_rtmp_client *c = (zms_rtmp_client *)user;
    if (!c || c->stopping || !chunk || !chunk->body || chunk->body_size == 0) {
        return;
    }
    if (chunk->type_id != ZMS_RTMP_MSG_AUDIO && chunk->type_id != ZMS_RTMP_MSG_VIDEO) {
        return;
    }
    if (c->state != CL_PLAYING) {
        client_mark_playing(c);
    }
    if (c->opts.on_media) {
        c->opts.on_media(chunk->type_id, chunk->tag_dts_ms, chunk->body, chunk->body_size,
                         c->opts.user);
    }
}

static void on_io_connected(zms_rtmp_client *c)
{
    c->state = CL_HS;
    c->hs_off = 0;
    send_c0c1(c);
}

static void on_transport_recv(zms_rtmp_client *c, const void *data, size_t len)
{
    if (!c || c->stopping || !data || len == 0) {
        return;
    }

    if (!c->proto) {
        const uint8_t *d = (const uint8_t *)data;
        size_t off = 0;
        if (!c->hs_buf) {
            return;
        }
        while (off < len) {
            size_t need = RTMP_HS_REPLY - c->hs_off;
            size_t copy = need < len - off ? need : len - off;
            memcpy(c->hs_buf + c->hs_off, d + off, copy);
            c->hs_off += copy;
            off += copy;
            try_finish_hs(c);
            if (c->proto) {
                break;
            }
        }
        if (c->proto && off < len) {
            zms_rtmp_protocol_input(c->proto, d + off, len - off);
        }
        return;
    }

    zms_rtmp_protocol_input(c->proto, data, len);
}

static void on_tcp_connect(ztk_tcp_client *tcp, void *user)
{
    zms_rtmp_client *c = (zms_rtmp_client *)user;
    (void)tcp;
    on_io_connected(c);
}

static void on_tcp_recv(ztk_tcp_client *tcp, const void *data, size_t len, void *user)
{
    zms_rtmp_client *c = (zms_rtmp_client *)user;
    (void)tcp;
    on_transport_recv(c, data, len);
}

static void on_tcp_error(ztk_tcp_client *tcp, void *user)
{
    zms_rtmp_client *c = (zms_rtmp_client *)user;
    (void)tcp;
    client_fail(c, ZTK_ERR_IO);
}

static void on_tls_connect(ztk_tls_client *tls, void *user)
{
    zms_rtmp_client *c = (zms_rtmp_client *)user;
    (void)tls;
    on_io_connected(c);
}

static void on_tls_recv(ztk_tls_client *tls, const void *data, size_t len, void *user)
{
    zms_rtmp_client *c = (zms_rtmp_client *)user;
    (void)tls;
    on_transport_recv(c, data, len);
}

static void on_tls_error(ztk_tls_client *tls, void *user)
{
    zms_rtmp_client *c = (zms_rtmp_client *)user;
    (void)tls;
    client_fail(c, ZTK_ERR_IO);
}

zms_rtmp_client *zms_rtmp_client_create(const zms_rtmp_client_opts *opts)
{
    if (!opts || !opts->poller || !opts->url) {
        return NULL;
    }

    zms_rtmp_client *c = (zms_rtmp_client *)calloc(1, sizeof(*c));
    if (!c) {
        return NULL;
    }
    c->opts = *opts;
    if (parse_rtmp_url(opts->url, &c->url) != ZTK_OK) {
        free(c);
        return NULL;
    }

    if (c->url.use_tls) {
#if !defined(ZTK_HAVE_OPENSSL) || !ZTK_HAVE_OPENSSL
        free(c);
        return NULL;
#else
        if (!opts->ssl_ctx) {
            free(c);
            return NULL;
        }
        ztk_tls_client_ops_t tops = {on_tls_connect, on_tls_recv, on_tls_error};
        ztk_tls_client_opts_t topts = {opts->poller, opts->ssl_ctx, &tops, c, c->url.host};
        c->tls = ztk_tls_client_create(&topts);
        if (!c->tls) {
            free(c);
            return NULL;
        }
#endif
    } else {
        ztk_tcp_client_ops_t tops = {on_tcp_connect, on_tcp_recv, on_tcp_error};
        ztk_tcp_client_opts_t topts = {opts->poller, &tops, c};
        c->tcp = ztk_tcp_client_create(&topts);
        if (!c->tcp) {
            free(c);
            return NULL;
        }
    }
    return c;
}

void zms_rtmp_client_destroy(zms_rtmp_client *c)
{
    if (!c) {
        return;
    }
    zms_rtmp_client_stop(c);
    ztk_tcp_client_destroy(c->tcp);
#if ZMS_HAVE_PULL_TLS
    ztk_tls_client_destroy(c->tls);
#endif
    zms_rtmp_protocol_destroy(c->proto);
    zms_buf_pool_slot_clear(&c->send_buf, &c->send_cap);
    client_hs_release(c);
    free(c);
}

ztk_err_t zms_rtmp_client_play(zms_rtmp_client *c)
{
    if (!c) {
        return ZTK_ERR_INVALID;
    }
    c->stopping = 0;
    c->ready_sent = 0;
    c->hs_create_sent = 0;
    c->hs_play_sent = 0;
    c->stream_id = 0;
    return client_connect(c);
}

void zms_rtmp_client_stop(zms_rtmp_client *c)
{
    if (!c) {
        return;
    }
    c->stopping = 1;
    client_close(c);
    zms_rtmp_protocol_destroy(c->proto);
    c->proto = NULL;
    c->state = CL_HS;
    client_hs_release(c);
    c->ready_sent = 0;
}
