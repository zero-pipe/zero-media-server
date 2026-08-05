#include "session/rtsp/rtsp_session_internal.h"
#include "ztk/util/log.h"
#include <stdlib.h>

static const zms_rtsp_method_fn k_rtsp_method_fns[ZMS_RTSP_UNKNOWN] = {
    [ZMS_RTSP_OPTIONS] = zms_rtsp_session_handle_options,
    [ZMS_RTSP_DESCRIBE] = zms_rtsp_session_handle_describe,
    [ZMS_RTSP_SETUP] = zms_rtsp_session_handle_setup,
    [ZMS_RTSP_PLAY] = zms_rtsp_session_handle_play,
    [ZMS_RTSP_PAUSE] = zms_rtsp_session_handle_pause,
    [ZMS_RTSP_TEARDOWN] = zms_rtsp_session_handle_teardown,
    [ZMS_RTSP_GET_PARAMETER] = zms_rtsp_session_handle_get_parameter,
    [ZMS_RTSP_ANNOUNCE] = zms_rtsp_session_handle_announce,
    [ZMS_RTSP_RECORD] = zms_rtsp_session_handle_record,
};

void zms_rtsp_session_on_message(const zms_rtsp_message *msg, void *user)
{
    zms_rtsp_session *rs = (zms_rtsp_session *)user;
    if (!rs || !msg) {
        return;
    }

    const char *cseq = zms_rtsp_message_get(msg, "CSeq");
    rs->cseq = cseq ? (unsigned)atoi(cseq) : 0;

    if (msg->is_response) {
        return;
    }

    if (msg->method >= 0 && msg->method < ZMS_RTSP_UNKNOWN && k_rtsp_method_fns[msg->method]) {
        k_rtsp_method_fns[msg->method](rs, msg);
        return;
    }
    zms_rtsp_session_send_resp(rs, 501, "Not Implemented", NULL, NULL, 0);
}
