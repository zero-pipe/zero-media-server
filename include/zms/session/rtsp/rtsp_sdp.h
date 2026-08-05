#ifndef ZMS_SESSION_RTSP_SDP_H
#define ZMS_SESSION_RTSP_SDP_H

#include "zms/zms_export.h"
#include "ztk/ztk_errno.h"
#include "zms/engine/media_track.h"
#include "zms/engine/stream/stream_limits.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 别名：SDP 轨槽与 engine track-slot 上限共用。 */
#define ZMS_SDP_TRACK_MAX ZMS_TRACK_SLOT_MAX

typedef struct zms_sdp_session {
    zms_media_track tracks[ZMS_SDP_TRACK_MAX];
    unsigned track_count;
    char session_control[ZMS_TRACK_CTRL_MAX];
} zms_sdp_session;

ZMS_API ztk_err_t zms_sdp_parse(const char *sdp, size_t len, zms_sdp_session *out);
ZMS_API const zms_media_track *zms_sdp_track_at(const zms_sdp_session *s, unsigned index);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_SESSION_RTSP_SDP_H */
