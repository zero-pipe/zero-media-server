/**
 * @file live_pull_proxy.c
 * @brief 拉流代理写入本地 media_source。
 *
 * Copyright (c) zero-media-server
 */
#include "zms/live/proxy/live_pull_proxy.h"
#include "zms/ops/service/pull_ssl.h"
#include "ztk/net/ssl.h"
#include <stdio.h>
#include "zms/engine/media_event.h"
#include "zms/engine/stream/stream_hub.h"
#include "zms/engine/stream/stream_url.h"
#include "zms/live/ingest/common/stream_ingest.h"
#include "zms/session/rtsp/rtsp_client.h"
#include "zms/session/rtmp/rtmp_client.h"
#include "zms/session/http/http_flv_client.h"
#include "zms/session/http/http_hls_client.h"
#include "zms/session/rtmp/rtmp.h"
#include "ztk/poller/poller_pool.h"
#include "ztk/thread/sync.h"
#include "ztk/util/log.h"
#include "ztk/util/timer.h"
#include <stdlib.h>
#include <string.h>

static void trim_fmtp_token(char *s)
{
    if (!s) {
        return;
    }
    for (char *p = s; *p; ++p) {
        if (*p == ';' || *p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
            *p = '\0';
            break;
        }
    }
}

static int base64_val(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

static size_t base64_decode(const char *in, uint8_t *out, size_t cap)
{
    size_t o = 0;
    int val[4];
    while (*in) {
        int n = 0;
        for (; n < 4 && *in; ++in) {
            if (*in == '=' || *in == ' ' || *in == '\r' || *in == '\n' || *in == '\t') {
                continue;
            }
            val[n] = base64_val(*in);
            if (val[n] < 0) {
                return o;
            }
            ++n;
        }
        if (n < 2) {
            break;
        }
        if (o < cap) {
            out[o++] = (uint8_t)((val[0] << 2) | (val[1] >> 4));
        }
        if (n > 2 && o < cap) {
            out[o++] = (uint8_t)(((val[1] & 15) << 4) | (val[2] >> 2));
        }
        if (n > 3 && o < cap) {
            out[o++] = (uint8_t)(((val[2] & 3) << 6) | val[3]);
        }
    }
    return o;
}

static void apply_h264_sprop(zms_live_ingest *ch, const char *fmtp)
{
    if (!ch || !fmtp) {
        return;
    }
    const char *sprop = strstr(fmtp, "sprop-parameter-sets=");
    if (!sprop) {
        return;
    }
    sprop += 21;
    const char *comma = strchr(sprop, ',');
    if (!comma) {
        return;
    }

    char sps_b64[384], pps_b64[384];
    size_t slen = (size_t)(comma - sprop);
    if (slen >= sizeof(sps_b64)) {
        slen = sizeof(sps_b64) - 1;
    }
    memcpy(sps_b64, sprop, slen);
    sps_b64[slen] = '\0';
    trim_fmtp_token(sps_b64);
    strncpy(pps_b64, comma + 1, sizeof(pps_b64) - 1);
    pps_b64[sizeof(pps_b64) - 1] = '\0';
    trim_fmtp_token(pps_b64);

    uint8_t sps[256], pps[256];
    size_t sps_len = base64_decode(sps_b64, sps, sizeof(sps));
    size_t pps_len = base64_decode(pps_b64, pps, sizeof(pps));
    if (!sps_len || !pps_len) {
        return;
    }

    if (zms_live_ingest_set_h264_sps_pps(ch, sps, sps_len, pps, pps_len) == ZTK_OK) {
        ztk_debug("live_pull_proxy: applied H264 sprop SPS=%u PPS=%u", (unsigned)sps_len,
                  (unsigned)pps_len);
    }
}

static int proxy_fmtp_extract_b64(const char *fmtp, const char *key, char *out, size_t cap)
{
    char needle[32];
    const char *p;
    const char *end;
    size_t n;

    if (!fmtp || !key || !out || cap == 0) {
        return 0;
    }
    snprintf(needle, sizeof(needle), "%s=", key);
    p = strstr(fmtp, needle);
    if (!p) {
        return 0;
    }
    p += strlen(needle);
    end = p;
    while (*end && *end != ';' && *end != ' ' && *end != '\r' && *end != '\n') {
        ++end;
    }
    n = (size_t)(end - p);
    if (n == 0 || n >= cap) {
        return 0;
    }
    memcpy(out, p, n);
    out[n] = '\0';
    trim_fmtp_token(out);
    return out[0] != '\0';
}

static void apply_h265_sprop(zms_live_ingest *ch, const char *fmtp)
{
    char vps_b64[512], sps_b64[512], pps_b64[512];
    uint8_t vps[512], sps[512], pps[512];
    size_t vps_len = 0, sps_len = 0, pps_len = 0;

    if (!ch || !fmtp) {
        return;
    }
    vps_b64[0] = sps_b64[0] = pps_b64[0] = '\0';
    (void)proxy_fmtp_extract_b64(fmtp, "sprop-vps", vps_b64, sizeof(vps_b64));
    if (!proxy_fmtp_extract_b64(fmtp, "sprop-sps", sps_b64, sizeof(sps_b64))) {
        return;
    }
    if (!proxy_fmtp_extract_b64(fmtp, "sprop-pps", pps_b64, sizeof(pps_b64))) {
        return;
    }
    if (vps_b64[0]) {
        vps_len = base64_decode(vps_b64, vps, sizeof(vps));
    }
    sps_len = base64_decode(sps_b64, sps, sizeof(sps));
    pps_len = base64_decode(pps_b64, pps, sizeof(pps));
    if (!sps_len || !pps_len) {
        return;
    }
    if (zms_live_ingest_set_h265_vps_sps_pps(ch, vps_len ? vps : NULL, vps_len, sps, sps_len, pps,
                                             pps_len) == ZTK_OK) {
        ztk_debug("live_pull_proxy: applied H265 sprop VPS=%u SPS=%u PPS=%u", (unsigned)vps_len,
                  (unsigned)sps_len, (unsigned)pps_len);
    }
}

static ztk_err_t proxy_do_play(zms_live_pull_proxy *p);

static void proxy_publisher_kick(void *ctx, int force)
{
    zms_live_pull_proxy *p = (zms_live_pull_proxy *)ctx;
    (void)force;
    if (p) {
        zms_live_pull_proxy_stop(p);
    }
}

typedef enum {
    PROXY_RTSP = 0,
    PROXY_RTMP,
    PROXY_HTTP_FLV,
    PROXY_HLS,
} proxy_scheme;

typedef struct zms_live_pull_proxy_ops zms_live_pull_proxy_ops;

typedef struct zms_live_pull_proxy_reg_entry {
    zms_live_pull_proxy *player;
    char key[192];
    struct zms_live_pull_proxy_reg_entry *next;
} zms_live_pull_proxy_reg_entry;

static zms_live_pull_proxy_reg_entry *g_proxy_registry;
static ztk_mutex *g_proxy_registry_mtx;

static void proxy_registry_init(void)
{
    if (!g_proxy_registry_mtx) {
        g_proxy_registry_mtx = ztk_mutex_create(ZTK_MUTEX_NORMAL);
    }
}

struct zms_live_pull_proxy {
    zms_live_pull_proxy_opts opts;
    char reg_key[192];
    char pull_url[768];
    char app[ZMS_APP_MAX];
    char stream[ZMS_STREAM_MAX];
    zms_live_ingest *ingress;
    proxy_scheme scheme;
    const zms_live_pull_proxy_ops *ops;
    zms_rtsp_client *rtsp;
    zms_rtmp_client *rtmp;
    zms_http_flv_client *http_flv;
    zms_http_hls_client *hls;
    int started;
    int retry_count;
    int reconnect_delay_ms;
    int failed_count;
    int logged_rtmp_media;
    ztk_poller_timer *retry_timer;
};

struct zms_live_pull_proxy_ops {
    proxy_scheme scheme;
    const char *name;
    ztk_err_t (*create)(zms_live_pull_proxy *p, ztk_poller *poller, ztk_ssl_ctx *ssl_ctx);
    ztk_err_t (*play)(zms_live_pull_proxy *p);
    void (*stop)(zms_live_pull_proxy *p);
    void (*destroy)(zms_live_pull_proxy *p);
};

static ztk_err_t proxy_rtsp_create(zms_live_pull_proxy *p, ztk_poller *poller,
                                   ztk_ssl_ctx *ssl_ctx);
static ztk_err_t proxy_rtmp_create(zms_live_pull_proxy *p, ztk_poller *poller,
                                   ztk_ssl_ctx *ssl_ctx);
static ztk_err_t proxy_http_flv_create(zms_live_pull_proxy *p, ztk_poller *poller,
                                       ztk_ssl_ctx *ssl_ctx);
static ztk_err_t proxy_hls_create(zms_live_pull_proxy *p, ztk_poller *poller, ztk_ssl_ctx *ssl_ctx);
static ztk_err_t proxy_rtsp_play(zms_live_pull_proxy *p);
static ztk_err_t proxy_rtmp_play(zms_live_pull_proxy *p);
static ztk_err_t proxy_http_flv_play(zms_live_pull_proxy *p);
static ztk_err_t proxy_hls_play(zms_live_pull_proxy *p);
static void proxy_rtsp_stop(zms_live_pull_proxy *p);
static void proxy_rtmp_stop(zms_live_pull_proxy *p);
static void proxy_http_flv_stop(zms_live_pull_proxy *p);
static void proxy_hls_stop(zms_live_pull_proxy *p);
static void proxy_rtsp_destroy(zms_live_pull_proxy *p);
static void proxy_rtmp_destroy(zms_live_pull_proxy *p);
static void proxy_http_flv_destroy(zms_live_pull_proxy *p);
static void proxy_hls_destroy(zms_live_pull_proxy *p);

static const zms_live_pull_proxy_ops k_proxy_pull_ops[] = {
    {PROXY_RTSP, "rtsp", proxy_rtsp_create, proxy_rtsp_play, proxy_rtsp_stop, proxy_rtsp_destroy},
    {PROXY_RTMP, "rtmp", proxy_rtmp_create, proxy_rtmp_play, proxy_rtmp_stop, proxy_rtmp_destroy},
    {PROXY_HTTP_FLV, "http-flv", proxy_http_flv_create, proxy_http_flv_play, proxy_http_flv_stop,
     proxy_http_flv_destroy},
    {PROXY_HLS, "hls", proxy_hls_create, proxy_hls_play, proxy_hls_stop, proxy_hls_destroy},
};

static const zms_live_pull_proxy_ops *proxy_pull_ops_find(proxy_scheme scheme)
{
    size_t i;

    for (i = 0; i < sizeof(k_proxy_pull_ops) / sizeof(k_proxy_pull_ops[0]); ++i) {
        if (k_proxy_pull_ops[i].scheme == scheme) {
            return &k_proxy_pull_ops[i];
        }
    }
    return NULL;
}

static void proxy_cancel_retry(zms_live_pull_proxy *p)
{
    if (p && p->retry_timer) {
        ztk_poller_timer_cancel(p->retry_timer);
        p->retry_timer = NULL;
    }
}

static void strip_path_query(char *s)
{
    if (!s) {
        return;
    }
    char *q = strchr(s, '?');
    if (q) {
        *q = '\0';
    }
    char *h = strchr(s, '#');
    if (h) {
        *h = '\0';
    }
}

static const char *pull_url_path_start(const char *url)
{
    if (!url) {
        return NULL;
    }
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    const char *at = strchr(p, '@');
    if (at) {
        p = at + 1;
    }
    p = strchr(p, '/');
    if (!p || !p[1]) {
        return NULL;
    }
    return p + 1;
}

static int proxy_stream_is_auto(const char *stream)
{
    if (!stream || !stream[0]) {
        return 1;
    }
    return strcmp(stream, "auto") == 0 || strcmp(stream, "AUTO") == 0;
}

int zms_live_pull_proxy_path_tail(const char *pull_url, char *tail, size_t tail_cap)
{
    if (!tail || tail_cap == 0) {
        return -1;
    }
    tail[0] = '\0';
    const char *p = pull_url_path_start(pull_url);
    if (!p) {
        return 0;
    }
    const char *slash = strchr(p, '/');
    if (!slash) {
        tail[0] = '\0';
        return 0;
    }
    strncpy(tail, slash + 1, tail_cap - 1);
    tail[tail_cap - 1] = '\0';
    strip_path_query(tail);
    zms_media_path_strip_play_suffix(tail);
    return 0;
}

int zms_live_pull_proxy_build_stream(const char *pull_url, const char *prefix, char *stream,
                                     size_t stream_cap)
{
    char tail[ZMS_STREAM_MAX];
    int n;

    if (!stream || stream_cap == 0) {
        return -1;
    }
    stream[0] = '\0';
    if (zms_live_pull_proxy_path_tail(pull_url, tail, sizeof(tail)) != 0 || !tail[0]) {
        return -1;
    }
    if (prefix && prefix[0]) {
        n = snprintf(stream, stream_cap, "%s/%s", prefix, tail);
    } else {
        n = snprintf(stream, stream_cap, "%s", tail);
    }
    if (n < 0 || (size_t)n >= stream_cap) {
        stream[stream_cap - 1] = '\0';
        return -1;
    }
    return 0;
}

static int path_suffix_ci(const char *path, size_t len, const char *suf)
{
    size_t slen;

    if (!path || !suf || len == 0) {
        return 0;
    }
    slen = strlen(suf);
    if (len < slen) {
        return 0;
    }
#ifdef _WIN32
    return _strnicmp(path + len - slen, suf, slen) == 0;
#else
    return strncasecmp(path + len - slen, suf, slen) == 0;
#endif
}

static int is_hls_pull_url(const char *url)
{
    const char *path;
    const char *slash;
    const char *q;
    size_t n;

    if (!url) {
        return 0;
    }
    path = strstr(url, "://");
    path = path ? path + 3 : url;
    slash = strchr(path, '/');
    if (!slash) {
        return 0;
    }
    q = strchr(slash, '?');
    n = q ? (size_t)(q - slash) : strlen(slash);
    if (path_suffix_ci(slash, n, ".m3u8")) {
        return 1;
    }
    return 0;
}

static proxy_scheme detect_scheme(const char *url)
{
    if (!url) {
        return PROXY_RTSP;
    }
    if (strncmp(url, "rtmp://", 7) == 0 || strncmp(url, "rtmps://", 8) == 0) {
        return PROXY_RTMP;
    }
    if (strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0) {
        if (is_hls_pull_url(url)) {
            return PROXY_HLS;
        }
        return PROXY_HTTP_FLV;
    }
    return PROXY_RTSP;
}

static int pull_url_needs_tls(const char *url)
{
    return url && (strncmp(url, "rtmps://", 8) == 0 || strncmp(url, "rtsps://", 8) == 0 ||
                   strncmp(url, "https://", 8) == 0);
}

static ztk_ssl_ctx *proxy_resolve_ssl_ctx(const zms_live_pull_proxy_opts *opts)
{
    if (opts && opts->ssl_ctx) {
        return opts->ssl_ctx;
    }
    return zms_pull_ssl_ctx(NULL);
}

#define ZMS_DEFAULT_VHOST "__defaultVhost__"

void zms_live_pull_proxy_make_key(const char *vhost, const char *app, const char *stream, char *key,
                                  size_t key_cap)
{
    if (!key || key_cap == 0) {
        return;
    }
    const char *vh = (vhost && vhost[0]) ? vhost : ZMS_DEFAULT_VHOST;
    snprintf(key, key_cap, "%s/%s/%s", vh, app ? app : "", stream ? stream : "");
}

static void proxy_registry_add(zms_live_pull_proxy *p)
{
    if (!p || !p->reg_key[0]) {
        return;
    }
    proxy_registry_init();
    zms_live_pull_proxy_reg_entry *e = (zms_live_pull_proxy_reg_entry *)calloc(1, sizeof(*e));
    if (!e) {
        return;
    }
    e->player = p;
    strncpy(e->key, p->reg_key, sizeof(e->key) - 1);
    ztk_mutex_lock(g_proxy_registry_mtx);
    e->next = g_proxy_registry;
    g_proxy_registry = e;
    ztk_mutex_unlock(g_proxy_registry_mtx);
}

static void proxy_registry_remove(zms_live_pull_proxy *p)
{
    zms_live_pull_proxy_reg_entry **pp;

    if (!p) {
        return;
    }
    proxy_registry_init();
    ztk_mutex_lock(g_proxy_registry_mtx);
    pp = &g_proxy_registry;
    while (*pp) {
        if ((*pp)->player == p) {
            zms_live_pull_proxy_reg_entry *dead = *pp;
            *pp = dead->next;
            ztk_mutex_unlock(g_proxy_registry_mtx);
            free(dead);
            return;
        }
        pp = &(*pp)->next;
    }
    ztk_mutex_unlock(g_proxy_registry_mtx);
}

zms_live_pull_proxy *zms_live_pull_proxy_find_by_key(const char *key)
{
    zms_live_pull_proxy *found = NULL;

    if (!key) {
        return NULL;
    }
    proxy_registry_init();
    ztk_mutex_lock(g_proxy_registry_mtx);
    for (zms_live_pull_proxy_reg_entry *e = g_proxy_registry; e; e = e->next) {
        if (strcmp(e->key, key) == 0) {
            found = e->player;
            break;
        }
    }
    ztk_mutex_unlock(g_proxy_registry_mtx);
    return found;
}

int zms_live_pull_proxy_foreach(zms_live_pull_proxy_visit_cb cb, void *user)
{
    int n = 0;

    if (!cb) {
        return 0;
    }
    proxy_registry_init();
    ztk_mutex_lock(g_proxy_registry_mtx);
    for (zms_live_pull_proxy_reg_entry *e = g_proxy_registry; e; e = e->next) {
        if (cb(e->key, e->player, user) != 0) {
            break;
        }
        ++n;
    }
    ztk_mutex_unlock(g_proxy_registry_mtx);
    return n;
}

static ztk_poller *resolve_poller(const zms_live_pull_proxy_opts *opts)
{
    if (opts->poller) {
        return opts->poller;
    }
    if (opts->poller_pool) {
        return ztk_poller_pool_get(opts->poller_pool, 0);
    }
    return NULL;
}

static void proxy_try_publish(zms_live_pull_proxy *p)
{
    zms_media_source *src;

    if (!p || !p->ingress) {
        return;
    }
    src = zms_live_ingest_source(p->ingress);
    if (!src || src->publishing) {
        return;
    }
    if (!src->has_video || !src->video.ready) {
        return;
    }
    ztk_debug("live_pull_proxy publish: %s/%s tracks ready (video=%d audio=%d)", p->app, p->stream,
              src->has_video, src->has_audio);
    zms_media_event_publish(src, ZMS_ORIGIN_PULL);
}

static void proxy_on_ready(void *user)
{
    zms_live_pull_proxy *p = (zms_live_pull_proxy *)user;
    p->failed_count = 0;
    proxy_cancel_retry(p);
    ztk_info("live_pull_proxy ready: %s/%s from %s", p->app, p->stream, p->pull_url);
    {
        zms_media_source *src = zms_live_ingest_source(p->ingress);
        if (src) {
            zms_media_source_set_publisher(src, p, proxy_publisher_kick);
        }
    }
    proxy_try_publish(p);
    if (p->opts.on_ready) {
        p->opts.on_ready(p->opts.user);
    }
}

static void proxy_on_track(const zms_media_track *track, void *user)
{
    zms_live_pull_proxy *p = (zms_live_pull_proxy *)user;
    if (!p || !p->ingress || !track) {
        return;
    }
    if (track->codec == ZMS_CODEC_H264 && track->fmtp[0]) {
        apply_h264_sprop(p->ingress, track->fmtp);
    } else if (track->codec == ZMS_CODEC_H265 && track->fmtp[0]) {
        apply_h265_sprop(p->ingress, track->fmtp);
    }
    if (track->codec == ZMS_CODEC_AAC && track->fmtp[0]) {
        const char *cfg = strstr(track->fmtp, "config=");
        if (!cfg) {
            cfg = strstr(track->fmtp, "Config=");
        }
        if (cfg) {
            char hex[128];
            strncpy(hex, cfg + 7, sizeof(hex) - 1);
            hex[sizeof(hex) - 1] = '\0';
            trim_fmtp_token(hex);
            if (hex[0]) {
                (void)zms_live_ingest_set_aac_config_hex(p->ingress, hex);
            }
        }
    }
    if (track->type == ZMS_TRACK_AUDIO &&
        (track->codec == ZMS_CODEC_G711A || track->codec == ZMS_CODEC_G711U)) {
        int rate = track->sample_rate > 0 ? track->sample_rate : 8000;
        zms_live_ingest_set_audio_codec(p->ingress, track->codec, (uint32_t)rate);
        zms_live_ingest_set_rtp_clocks(p->ingress, 90000, track->codec, (uint32_t)rate);
    }
    proxy_try_publish(p);
}

static void proxy_on_frame(const zms_frame *frame, void *user)
{
    zms_live_pull_proxy *p = (zms_live_pull_proxy *)user;
    if (!p || !p->ingress || !frame) {
        return;
    }
    (void)zms_live_ingest_input_frame(p->ingress, frame);
    proxy_try_publish(p);
}

static void proxy_on_rtmp_media(uint8_t msg_type, uint32_t tag_dts_ms, const void *body, size_t len,
                                void *user)
{
    zms_live_pull_proxy *p = (zms_live_pull_proxy *)user;
    if (!p || !p->ingress || !body || len == 0) {
        return;
    }
    if (!p->logged_rtmp_media) {
        p->logged_rtmp_media = 1;
        ztk_debug("live_pull_proxy first RTMP media type=%u len=%u", (unsigned)msg_type,
                  (unsigned)len);
    }
    if (msg_type == ZMS_RTMP_MSG_VIDEO) {
        (void)zms_live_ingest_input_rtmp_video(p->ingress, tag_dts_ms, body, len);
    } else if (msg_type == ZMS_RTMP_MSG_AUDIO) {
        (void)zms_live_ingest_input_rtmp_audio(p->ingress, tag_dts_ms, body, len);
    }
    proxy_try_publish(p);
}

static uint64_t proxy_retry_task(void *user)
{
    zms_live_pull_proxy *p = (zms_live_pull_proxy *)user;
    p->retry_timer = NULL;
    if (!p || !p->started) {
        return 0;
    }
    ztk_info("live_pull_proxy retry #%d pull=%s", p->failed_count + 1, p->pull_url);
    if (p->ingress) {
        zms_live_ingest_reset_upstream(p->ingress);
    }
    p->logged_rtmp_media = 0;
    if (proxy_do_play(p) != ZTK_OK) {
        return (uint64_t)p->reconnect_delay_ms;
    }
    return 0;
}

static void proxy_schedule_retry(zms_live_pull_proxy *p)
{
    if (!p || !p->started) {
        return;
    }
    if (p->retry_count >= 0 && p->failed_count >= p->retry_count) {
        return;
    }
    ++p->failed_count;
    proxy_cancel_retry(p);
    p->retry_timer =
        ztk_poller_do_delay(p->opts.poller, (uint64_t)p->reconnect_delay_ms, proxy_retry_task, p);
}

static void proxy_on_error(ztk_err_t err, void *user)
{
    zms_live_pull_proxy *p = (zms_live_pull_proxy *)user;
    ztk_warn("live_pull_proxy error: %s/%s err=%d", p->app, p->stream, (int)err);
    if (p->ops && p->ops->stop) {
        p->ops->stop(p);
    }
    if (p->opts.on_error) {
        p->opts.on_error(err, p->opts.user);
    }
    proxy_schedule_retry(p);
}

static ztk_err_t proxy_do_play(zms_live_pull_proxy *p)
{
    if (!p) {
        return ZTK_ERR_INVALID;
    }
    return p->ops && p->ops->play ? p->ops->play(p) : ZTK_ERR_INVALID;
}

static ztk_err_t proxy_rtsp_play(zms_live_pull_proxy *p)
{
    return p && p->rtsp ? zms_rtsp_client_play(p->rtsp) : ZTK_ERR_INVALID;
}

static ztk_err_t proxy_rtmp_play(zms_live_pull_proxy *p)
{
    return p && p->rtmp ? zms_rtmp_client_play(p->rtmp) : ZTK_ERR_INVALID;
}

static ztk_err_t proxy_http_flv_play(zms_live_pull_proxy *p)
{
    return p && p->http_flv ? zms_http_flv_client_play(p->http_flv) : ZTK_ERR_INVALID;
}

static ztk_err_t proxy_hls_play(zms_live_pull_proxy *p)
{
    if (!p || !p->hls) {
        return ZTK_ERR_INVALID;
    }
    (void)zms_http_hls_client_play(p->hls);
    return ZTK_OK;
}

static void proxy_rtsp_stop(zms_live_pull_proxy *p)
{
    if (p && p->rtsp) {
        zms_rtsp_client_stop(p->rtsp);
    }
}

static void proxy_rtmp_stop(zms_live_pull_proxy *p)
{
    if (p && p->rtmp) {
        zms_rtmp_client_stop(p->rtmp);
    }
}

static void proxy_http_flv_stop(zms_live_pull_proxy *p)
{
    if (p && p->http_flv) {
        zms_http_flv_client_stop(p->http_flv);
    }
}

static void proxy_hls_stop(zms_live_pull_proxy *p)
{
    if (p && p->hls) {
        zms_http_hls_client_stop(p->hls);
    }
}

static void proxy_rtsp_destroy(zms_live_pull_proxy *p)
{
    if (p) {
        zms_rtsp_client_destroy(p->rtsp);
    }
}

static void proxy_rtmp_destroy(zms_live_pull_proxy *p)
{
    if (p) {
        zms_rtmp_client_destroy(p->rtmp);
    }
}

static void proxy_http_flv_destroy(zms_live_pull_proxy *p)
{
    if (p) {
        zms_http_flv_client_destroy(p->http_flv);
    }
}

static void proxy_hls_destroy(zms_live_pull_proxy *p)
{
    if (p) {
        zms_http_hls_client_destroy(p->hls);
    }
}

static ztk_err_t proxy_rtsp_create(zms_live_pull_proxy *p, ztk_poller *poller, ztk_ssl_ctx *ssl_ctx)
{
    zms_rtsp_client_opts copts;

    if (!p) {
        return ZTK_ERR_INVALID;
    }
    memset(&copts, 0, sizeof(copts));
    copts.poller = poller;
    copts.url = p->pull_url;
    copts.ssl_ctx = ssl_ctx;
    copts.rtp_mode = p->opts.rtp_mode;
    copts.retry_count = 0;
    copts.reconnect_delay_ms = 0;
    copts.on_ready = proxy_on_ready;
    copts.on_track = proxy_on_track;
    copts.on_frame = proxy_on_frame;
    copts.on_error = proxy_on_error;
    copts.user = p;
    p->rtsp = zms_rtsp_client_create(&copts);
    return p->rtsp ? ZTK_OK : ZTK_ERR_NOMEM;
}

static ztk_err_t proxy_rtmp_create(zms_live_pull_proxy *p, ztk_poller *poller, ztk_ssl_ctx *ssl_ctx)
{
    zms_rtmp_client_opts copts;

    if (!p) {
        return ZTK_ERR_INVALID;
    }
    memset(&copts, 0, sizeof(copts));
    copts.poller = poller;
    copts.url = p->pull_url;
    copts.ssl_ctx = ssl_ctx;
    copts.on_ready = proxy_on_ready;
    copts.on_media = proxy_on_rtmp_media;
    copts.on_error = proxy_on_error;
    copts.user = p;
    p->rtmp = zms_rtmp_client_create(&copts);
    return p->rtmp ? ZTK_OK : ZTK_ERR_NOMEM;
}

static ztk_err_t proxy_http_flv_create(zms_live_pull_proxy *p, ztk_poller *poller,
                                       ztk_ssl_ctx *ssl_ctx)
{
    zms_http_flv_client_opts copts;

    if (!p) {
        return ZTK_ERR_INVALID;
    }
    memset(&copts, 0, sizeof(copts));
    copts.poller = poller;
    copts.url = p->pull_url;
    copts.ssl_ctx = ssl_ctx;
    copts.on_ready = proxy_on_ready;
    copts.on_media = proxy_on_rtmp_media;
    copts.on_error = proxy_on_error;
    copts.user = p;
    p->http_flv = zms_http_flv_client_create(&copts);
    return p->http_flv ? ZTK_OK : ZTK_ERR_NOMEM;
}

static ztk_err_t proxy_hls_create(zms_live_pull_proxy *p, ztk_poller *poller, ztk_ssl_ctx *ssl_ctx)
{
    zms_http_hls_client_opts copts;

    if (!p) {
        return ZTK_ERR_INVALID;
    }
    memset(&copts, 0, sizeof(copts));
    copts.poller = poller;
    copts.url = p->pull_url;
    copts.ssl_ctx = ssl_ctx;
    copts.on_ready = proxy_on_ready;
    copts.on_frame = proxy_on_frame;
    copts.on_error = proxy_on_error;
    copts.user = p;
    p->hls = zms_http_hls_client_create(&copts);
    return p->hls ? ZTK_OK : ZTK_ERR_NOMEM;
}

zms_live_pull_proxy *zms_live_pull_proxy_create(const zms_live_pull_proxy_opts *opts)
{
    if (!opts || !opts->pull_url) {
        return NULL;
    }
    ztk_poller *poller = resolve_poller(opts);
    if (!poller) {
        return NULL;
    }

    zms_live_pull_proxy *p = (zms_live_pull_proxy *)calloc(1, sizeof(*p));
    if (!p) {
        return NULL;
    }
    p->opts = *opts;
    p->opts.poller = poller;
    strncpy(p->pull_url, opts->pull_url, sizeof(p->pull_url) - 1);
    p->scheme = detect_scheme(p->pull_url);
    p->ops = proxy_pull_ops_find(p->scheme);
    if (!p->ops) {
        free(p);
        return NULL;
    }

    if (opts->app && opts->app[0]) {
        strncpy(p->app, opts->app, sizeof(p->app) - 1);
    } else {
        strncpy(p->app, "live", sizeof(p->app) - 1);
    }

    if (!proxy_stream_is_auto(opts->stream) && opts->stream && opts->stream[0]) {
        strncpy(p->stream, opts->stream, sizeof(p->stream) - 1);
    } else if (zms_live_pull_proxy_build_stream(p->pull_url, opts->proxy_prefix, p->stream,
                                                sizeof(p->stream)) != 0 ||
               !p->stream[0]) {
        ztk_error("live_pull_proxy: derive stream name failed pull=%s", p->pull_url);
        free(p);
        return NULL;
    }
    ztk_debug("live_pull_proxy register %s/%s pull=%s", p->app, p->stream, p->pull_url);

    p->retry_count = opts->retry_count;
    if (p->retry_count == 0) {
        p->retry_count = -1;
    }
    p->reconnect_delay_ms = opts->reconnect_delay_ms > 0 ? opts->reconnect_delay_ms : 2000;

    p->ingress = zms_live_ingest_create(p->app, p->stream, NULL);
    if (!p->ingress) {
        free(p);
        return NULL;
    }
    zms_live_ingest_set_poller(p->ingress, poller);
    {
        zms_media_source *src = zms_live_ingest_source(p->ingress);
        char tail[ZMS_STREAM_MAX];
        if (src && zms_live_pull_proxy_path_tail(p->pull_url, tail, sizeof(tail)) == 0 && tail[0]) {
            strncpy(src->stream_requested, tail, sizeof(src->stream_requested) - 1);
            src->stream_requested[sizeof(src->stream_requested) - 1] = '\0';
        }
    }

    ztk_ssl_ctx *ssl_ctx = pull_url_needs_tls(p->pull_url) ? proxy_resolve_ssl_ctx(opts) : NULL;
    if (pull_url_needs_tls(p->pull_url) && !ssl_ctx) {
        zms_live_ingest_destroy(p->ingress);
        free(p);
        return NULL;
    }

    if (p->ops->create(p, poller, ssl_ctx) != ZTK_OK) {
        zms_live_ingest_destroy(p->ingress);
        free(p);
        return NULL;
    }
    zms_live_pull_proxy_make_key(NULL, p->app, p->stream, p->reg_key, sizeof(p->reg_key));
    proxy_registry_add(p);
    return p;
}

void zms_live_pull_proxy_destroy(zms_live_pull_proxy *p)
{
    if (!p) {
        return;
    }
    proxy_registry_remove(p);
    proxy_cancel_retry(p);
    zms_live_pull_proxy_stop(p);
    if (p->ops && p->ops->destroy) {
        p->ops->destroy(p);
    }
    zms_live_ingest_destroy(p->ingress);
    free(p);
}

ztk_err_t zms_live_pull_proxy_start(zms_live_pull_proxy *p)
{
    if (!p) {
        return ZTK_ERR_INVALID;
    }
    if (p->started) {
        return ZTK_OK;
    }
    ztk_debug("live_pull_proxy start pull=%s -> %s/%s", p->pull_url, p->app, p->stream);
    p->started = 1;
    p->failed_count = 0;
    return proxy_do_play(p);
}

void zms_live_pull_proxy_stop(zms_live_pull_proxy *p)
{
    if (!p || !p->started) {
        return;
    }
    p->started = 0;
    proxy_cancel_retry(p);
    {
        zms_media_source *src = zms_live_ingest_source(p->ingress);
        if (src) {
            zms_media_source_clear_publisher(src, p);
        }
        if (src && src->publishing) {
            zms_media_event_publish_fini(src, ZMS_ORIGIN_PULL);
        }
    }
    if (p->ops && p->ops->stop) {
        p->ops->stop(p);
    }
    if (p->ingress) {
        zms_live_ingest_reset(p->ingress);
    }
}

zms_media_source *zms_live_pull_proxy_source(zms_live_pull_proxy *p)
{
    return p ? zms_live_ingest_source(p->ingress) : NULL;
}

zms_live_ingest *zms_live_pull_proxy_ingress(zms_live_pull_proxy *p)
{
    return p ? p->ingress : NULL;
}

const char *zms_live_pull_proxy_app(const zms_live_pull_proxy *p)
{
    return p ? p->app : "";
}

const char *zms_live_pull_proxy_stream(const zms_live_pull_proxy *p)
{
    return p ? p->stream : "";
}
