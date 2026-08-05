#include "zms/session/session_dispatcher.h"

/**
 * HTTP-FLV 桩：play 挂接打开 egress_source；直播/点播 FLV muxer 负责线格式。
 */
static ztk_err_t http_protocol_on_play_live(void *session, zms_media_source *src,
                                            const zms_session_play_opts *opts)
{
    zms_egress_source *play = (zms_egress_source *)session;
    (void)opts;
    if (!play || !src) {
        return ZTK_ERR_INVALID;
    }
    return zms_session_play_open_live(play, src, ZMS_SESSION_LIVE_GOP);
}

static ztk_err_t http_protocol_on_play_vod(void *session, zms_media_source *src,
                                           const zms_session_play_opts *opts)
{
    zms_egress_source *play = (zms_egress_source *)session;
    uint64_t seek_ms = opts ? opts->seek_ms : 0;
    if (!play || !src) {
        return ZTK_ERR_INVALID;
    }
    return zms_session_play_open_vod(play, src, seek_ms);
}

static void http_protocol_on_teardown(void *session)
{
    zms_egress_source *play = (zms_egress_source *)session;
    zms_session_play_close(play);
}

static const zms_session_dispatch_ops k_http_dispatch = {
    .name = ZMS_SESSION_HTTP_FLV,
    .on_play_live = http_protocol_on_play_live,
    .on_play_vod = http_protocol_on_play_vod,
    .on_publish = NULL,
    .on_teardown = http_protocol_on_teardown,
};

void zms_http_register(void)
{
    static int registered; /* 启动阶段，单线程 */
    if (registered) {
        return;
    }
    registered = 1;
    zms_session_dispatch_register(&k_http_dispatch);
}
