#include "session/rtsp/rtsp_session_internal.h"
#include "zms/session/rtsp/rtsp_session_auth.h"
#include "zms/engine/stream/stream_hub.h"
#include "zms/vod/io/vod_source.h"
#include "ztk/util/log.h"
#include <string.h>

void zms_rtsp_session_handle_describe(zms_rtsp_session *rs, const zms_rtsp_message *msg)
{
    if (!rs || !msg) {
        return;
    }
    if (!zms_rtsp_auth_check(rs, msg)) {
        return;
    }
    zms_rtsp_session_parse_url(msg->url, rs->app, rs->stream);
    rs->source = zms_media_source_find_for_play(ZMS_SCHEMA_RTSP, rs->app, rs->stream);
    if (!rs->source || (!zms_media_source_is_vod(rs->source) &&
                        !zms_media_source_use_gop_queue_play(rs->source))) {
        ztk_warn("RTSP DESCRIBE 404: stream not found app=%s stream=%s url=%s (active_sources=%d)",
                 rs->app, rs->stream, msg->url, zms_media_source_count());
        zms_rtsp_session_send_resp(rs, 404, "Not Found", NULL, NULL, 0);
        return;
    }
    char sdp[4096];
    zms_rtsp_session_build_sdp(rs, sdp, sizeof(sdp));
    ztk_info("RTSP DESCRIBE 200: app=%s stream=%s video=%d audio=%d sdp_bytes=%u", rs->app,
             rs->stream, rs->source->has_video, rs->source->has_audio, (unsigned)strlen(sdp));
    zms_rtsp_session_send_resp(rs, 200, "OK", "Content-Type: application/sdp\r\n", sdp,
                               strlen(sdp));
}
