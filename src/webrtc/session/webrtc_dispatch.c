#include "zms/session/session_dispatcher.h"
static ztk_err_t webrtc_protocol_on_play_live(void *session, zms_media_source *src,
                                              const zms_session_play_opts *opts)
{
    zms_egress_source *play = (zms_egress_source *)session;
    (void)opts;
    if (!play || !src) {
        return ZTK_ERR_INVALID;
    }
    return zms_session_play_open_live(play, src, ZMS_SESSION_LIVE_GOP);
}

static void webrtc_protocol_on_teardown(void *session)
{
    zms_egress_source *play = (zms_egress_source *)session;
    zms_session_play_close(play);
}

static const zms_session_dispatch_ops k_webrtc_dispatch = {
    .name = ZMS_SESSION_WEBRTC,
    .on_play_live = webrtc_protocol_on_play_live,
    .on_play_vod = NULL,
    .on_publish = NULL,
    .on_teardown = webrtc_protocol_on_teardown,
};

void zms_webrtc_register(void)
{
    static int registered; /* 启动阶段，单线程 */
    if (registered) {
        return;
    }
    registered = 1;
    zms_session_dispatch_register(&k_webrtc_dispatch);
}
