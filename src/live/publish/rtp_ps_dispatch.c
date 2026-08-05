#include "zms/session/session_dispatcher.h"

static const zms_session_dispatch_ops k_rtp_ps_dispatch = {
    .name = ZMS_SESSION_RTP_PS,
    .on_play_live = NULL,
    .on_play_vod = NULL,
    .on_publish = NULL,
    .on_teardown = NULL,
};

void zms_rtp_ps_register(void)
{
    static int registered; /* 启动阶段，单线程 */

    if (registered) {
        return;
    }
    registered = 1;
    zms_session_dispatch_register(&k_rtp_ps_dispatch);
}
