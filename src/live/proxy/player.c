#include "zms/live/proxy/player.h"
#include "zms/engine/module_registry.h"
#include "zms/session/http/http_hls_client.h"
#include "zms/session/http/http_flv_client.h"
#include "zms/session/rtsp/rtsp_client.h"
#include <stdlib.h>
#include <string.h>

typedef enum zms_player_backend {
    PLAYER_BACKEND_INVALID = 0,
    PLAYER_BACKEND_RTSP,
    PLAYER_BACKEND_HTTP_FLV,
    PLAYER_BACKEND_HLS,
    PLAYER_BACKEND_RTMP_UNSUPPORTED,
} zms_player_backend;

struct zms_player {
    zms_player_opts opts;
    zms_player_backend backend;
    union {
        zms_rtsp_client *rtsp;
        zms_http_flv_client *http_flv;
        zms_http_hls_client *hls;
        void *ptr;
    } u;
};

static int starts_with(const char *s, const char *prefix)
{
    size_t n;

    if (!s || !prefix) {
        return 0;
    }
    n = strlen(prefix);
    return strncmp(s, prefix, n) == 0;
}

static int contains_token(const char *s, const char *token)
{
    return s && token && strstr(s, token) != NULL;
}

static zms_player_backend detect_backend(const char *url)
{
    if (starts_with(url, "rtsp://") || starts_with(url, "rtsps://")) {
        return PLAYER_BACKEND_RTSP;
    }
    if (starts_with(url, "rtmp://") || starts_with(url, "rtmps://")) {
        return PLAYER_BACKEND_RTMP_UNSUPPORTED;
    }
    if (starts_with(url, "http://") || starts_with(url, "https://")) {
        if (contains_token(url, ".m3u8")) {
            return PLAYER_BACKEND_HLS;
        }
        return PLAYER_BACKEND_HTTP_FLV;
    }
    return PLAYER_BACKEND_INVALID;
}

static void player_on_ready(void *user)
{
    zms_player *p = (zms_player *)user;
    if (p && p->opts.on_ready) {
        p->opts.on_ready(p->opts.user);
    }
}

static void player_on_track(const zms_media_track *track, void *user)
{
    zms_player *p = (zms_player *)user;
    if (p && p->opts.on_track) {
        p->opts.on_track(track, p->opts.user);
    }
}

static void player_on_frame(const zms_frame *frame, void *user)
{
    zms_player *p = (zms_player *)user;
    if (p && p->opts.on_frame) {
        p->opts.on_frame(frame, p->opts.user);
    }
}

static void player_on_error(ztk_err_t err, void *user)
{
    zms_player *p = (zms_player *)user;
    if (p && p->opts.on_error) {
        p->opts.on_error(err, p->opts.user);
    }
}

static int player_create_rtsp(zms_player *p)
{
    zms_rtsp_client_opts opts;

    memset(&opts, 0, sizeof(opts));
    opts.poller = p->opts.poller;
    opts.url = p->opts.url;
    opts.ssl_ctx = p->opts.ssl_ctx;
    opts.rtp_mode = p->opts.rtsp_rtp_mode;
    opts.retry_count = p->opts.retry_count;
    opts.reconnect_delay_ms = p->opts.reconnect_delay_ms;
    opts.on_ready = player_on_ready;
    opts.on_track = player_on_track;
    opts.on_frame = player_on_frame;
    opts.on_error = player_on_error;
    opts.user = p;
    p->u.rtsp = zms_rtsp_client_create(&opts);
    return p->u.rtsp != NULL;
}

static int player_create_http_flv(zms_player *p)
{
    zms_http_flv_client_opts opts;

    memset(&opts, 0, sizeof(opts));
    opts.poller = p->opts.poller;
    opts.url = p->opts.url;
    opts.ssl_ctx = p->opts.ssl_ctx;
    opts.on_ready = player_on_ready;
    opts.on_frame = player_on_frame;
    opts.on_error = player_on_error;
    opts.user = p;
    p->u.http_flv = zms_http_flv_client_create(&opts);
    return p->u.http_flv != NULL;
}

static int player_create_hls(zms_player *p)
{
    zms_http_hls_client_opts opts;

    memset(&opts, 0, sizeof(opts));
    opts.poller = p->opts.poller;
    opts.url = p->opts.url;
    opts.ssl_ctx = p->opts.ssl_ctx;
    opts.on_ready = player_on_ready;
    opts.on_frame = player_on_frame;
    opts.on_error = player_on_error;
    opts.user = p;
    p->u.hls = zms_http_hls_client_create(&opts);
    return p->u.hls != NULL;
}

zms_player *zms_player_create(const zms_player_opts *opts)
{
    zms_player *p;

    if (!opts || !opts->poller || !opts->url) {
        return NULL;
    }
    p = (zms_player *)calloc(1, sizeof(*p));
    if (!p) {
        return NULL;
    }
    p->opts = *opts;
    p->backend = detect_backend(opts->url);
    zms_modules_register_all();

    switch (p->backend) {
    case PLAYER_BACKEND_RTSP:
        if (!player_create_rtsp(p)) {
            goto fail;
        }
        break;
    case PLAYER_BACKEND_HTTP_FLV:
        if (!player_create_http_flv(p)) {
            goto fail;
        }
        break;
    case PLAYER_BACKEND_HLS:
        if (!player_create_hls(p)) {
            goto fail;
        }
        break;
    case PLAYER_BACKEND_RTMP_UNSUPPORTED:
        break;
    default:
        goto fail;
    }
    return p;

fail:
    zms_player_destroy(p);
    return NULL;
}

void zms_player_destroy(zms_player *p)
{
    if (!p) {
        return;
    }
    zms_player_stop(p);
    switch (p->backend) {
    case PLAYER_BACKEND_RTSP:
        zms_rtsp_client_destroy(p->u.rtsp);
        break;
    case PLAYER_BACKEND_HTTP_FLV:
        zms_http_flv_client_destroy(p->u.http_flv);
        break;
    case PLAYER_BACKEND_HLS:
        zms_http_hls_client_destroy(p->u.hls);
        break;
    default:
        break;
    }
    free(p);
}

ztk_err_t zms_player_play(zms_player *p)
{
    ztk_err_t err = ZTK_ERR_INVALID;

    if (!p) {
        return ZTK_ERR_INVALID;
    }
    switch (p->backend) {
    case PLAYER_BACKEND_RTSP:
        err = zms_rtsp_client_play(p->u.rtsp);
        break;
    case PLAYER_BACKEND_HTTP_FLV:
        err = zms_http_flv_client_play(p->u.http_flv);
        break;
    case PLAYER_BACKEND_HLS:
        err = zms_http_hls_client_play(p->u.hls);
        break;
    case PLAYER_BACKEND_RTMP_UNSUPPORTED:
        err = ZTK_ERR_NOT_IMPL;
        break;
    default:
        err = ZTK_ERR_INVALID;
        break;
    }
    if (err != ZTK_OK && p->opts.on_error) {
        p->opts.on_error(err, p->opts.user);
    }
    return err;
}

void zms_player_stop(zms_player *p)
{
    if (!p) {
        return;
    }
    switch (p->backend) {
    case PLAYER_BACKEND_RTSP:
        zms_rtsp_client_stop(p->u.rtsp);
        break;
    case PLAYER_BACKEND_HTTP_FLV:
        zms_http_flv_client_stop(p->u.http_flv);
        break;
    case PLAYER_BACKEND_HLS:
        zms_http_hls_client_stop(p->u.hls);
        break;
    default:
        break;
    }
}

const char *zms_player_protocol(const zms_player *p)
{
    if (!p) {
        return NULL;
    }
    switch (p->backend) {
    case PLAYER_BACKEND_RTSP:
        return "rtsp";
    case PLAYER_BACKEND_HTTP_FLV:
        return "http-flv";
    case PLAYER_BACKEND_HLS:
        return "hls";
    case PLAYER_BACKEND_RTMP_UNSUPPORTED:
        return "rtmp";
    default:
        return NULL;
    }
}
