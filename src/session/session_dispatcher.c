#include "zms/session/session_dispatcher.h"
#include "zms/session/codec_filter.h"
#include "zms/vod/io/vod_source.h"
#include <string.h>

#define ZMS_SESSION_DISPATCH_MAX 12

static const zms_session_dispatch_ops *g_dispatch[ZMS_SESSION_DISPATCH_MAX];
static int g_dispatch_count;
static int g_session_dispatch_all_registered;

void zms_rtmp_register(void);
void zms_rtsp_register(void);
void zms_http_register(void);
void zms_http_ts_register(void);
#if defined(ZMS_ENABLE_WEBRTC)
void zms_webrtc_register(void);
#endif
void zms_rtp_ps_register(void);
#if defined(ZMS_HAVE_SRT)
void zms_srt_register(void);
#endif

void zms_session_dispatch_register(const zms_session_dispatch_ops *ops)
{
    int i;

    if (!ops || !ops->name || !ops->name[0]) {
        return;
    }
    for (i = 0; i < g_dispatch_count; ++i) {
        if (g_dispatch[i] && strcmp(g_dispatch[i]->name, ops->name) == 0) {
            g_dispatch[i] = ops;
            return;
        }
    }
    if (g_dispatch_count >= ZMS_SESSION_DISPATCH_MAX) {
        return;
    }
    g_dispatch[g_dispatch_count++] = ops;
}

void zms_session_dispatch_register_all(void)
{
    if (g_session_dispatch_all_registered) {
        return;
    }
    g_session_dispatch_all_registered = 1;
    zms_rtmp_register();
    zms_rtsp_register();
    zms_http_register();
    zms_http_ts_register();
#if defined(ZMS_ENABLE_WEBRTC)
    zms_webrtc_register();
#endif
    zms_rtp_ps_register();
#if defined(ZMS_HAVE_SRT)
    zms_srt_register();
#endif
}

const zms_session_dispatch_ops *zms_session_dispatch_find(const char *name)
{
    int i;

    if (!name || !name[0]) {
        return NULL;
    }
    zms_session_dispatch_register_all();
    for (i = 0; i < g_dispatch_count; ++i) {
        if (g_dispatch[i] && strcmp(g_dispatch[i]->name, name) == 0) {
            return g_dispatch[i];
        }
    }
    return NULL;
}

ztk_err_t zms_session_play_open_live(zms_egress_source *play, zms_media_source *src,
                                     zms_session_live_mode mode)
{
    if (!play || !src) {
        return ZTK_ERR_INVALID;
    }
    switch (mode) {
    case ZMS_SESSION_LIVE_GOP:
        return zms_egress_source_open_live_gop(src, play);
    case ZMS_SESSION_LIVE_KEY:
        return zms_egress_source_open_live_key(src, play);
    case ZMS_SESSION_LIVE_EDGE:
        return zms_egress_source_open_live(src, 1, play);
    default:
        return ZTK_ERR_INVALID;
    }
}

ztk_err_t zms_session_play_open_vod(zms_egress_source *play, zms_media_source *src,
                                    uint64_t seek_ms)
{
    ztk_err_t err;

    if (!play || !src) {
        return ZTK_ERR_INVALID;
    }
    err = zms_egress_source_open_vod(src, play);
    if (err != ZTK_OK) {
        return err;
    }
    if (seek_ms > 0) {
        (void)zms_vod_source_seek_for_reader(src, play->readers.vod, seek_ms);
    }
    return ZTK_OK;
}

void zms_session_play_close(zms_egress_source *play)
{
    if (play) {
        zms_egress_source_close(play);
    }
}

ztk_err_t zms_session_attach_play(const char *protocol, void *session, zms_media_source *src,
                                  const zms_session_play_opts *opts)
{
    const zms_session_dispatch_ops *ops;

    if (!protocol || !session || !src) {
        return ZTK_ERR_INVALID;
    }
    ops = zms_session_dispatch_find(protocol);
    if (!ops) {
        return ZTK_ERR_INVALID;
    }
    if (zms_media_source_is_vod(src)) {
        if (!ops->on_play_vod) {
            return ZTK_ERR_INVALID;
        }
        return ops->on_play_vod(session, src, opts);
    }
    if (!ops->on_play_live) {
        return ZTK_ERR_INVALID;
    }
    {
        zms_session_cap_role role = zms_session_capability_play_role(protocol);
        if (zms_session_capability_check_source(role, src) != ZTK_OK) {
            zms_session_capability_log_reject(opts && opts->player ? opts->player : protocol, src,
                                              role);
            return ZTK_ERR_NOT_IMPL;
        }
    }
    return ops->on_play_live(session, src, opts);
}

void zms_session_detach_play(const char *protocol, void *session)
{
    const zms_session_dispatch_ops *ops;

    if (!protocol || !session) {
        return;
    }
    ops = zms_session_dispatch_find(protocol);
    if (ops && ops->on_teardown) {
        ops->on_teardown(session);
    }
}

ztk_err_t zms_session_attach_publish(const char *protocol, void *session, zms_media_source *src,
                                     const zms_session_publish_opts *opts)
{
    const zms_session_dispatch_ops *ops;

    if (!protocol || !session || !src) {
        return ZTK_ERR_INVALID;
    }
    ops = zms_session_dispatch_find(protocol);
    if (!ops || !ops->on_publish) {
        return ZTK_ERR_INVALID;
    }
    return ops->on_publish(session, src, opts);
}
