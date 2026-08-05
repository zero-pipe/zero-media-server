#ifndef ZMS_SESSION_CODEC_FILTER_H
#define ZMS_SESSION_CODEC_FILTER_H

#include "zms/media/codec/codec_id.h"
#include "zms/engine/stream/stream_hub.h"
#include "zms/zms_export.h"
#include "ztk/ztk_errno.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum zms_session_cap_role {
    ZMS_PROTO_CAP_RTSP_PLAY = 0,
    ZMS_PROTO_CAP_RTSP_PUBLISH,
    ZMS_PROTO_CAP_RTMP_PLAY,
    ZMS_PROTO_CAP_RTMP_PUBLISH,
    ZMS_PROTO_CAP_HTTP_FLV_PLAY,
    ZMS_PROTO_CAP_HTTP_TS_PLAY,
    ZMS_PROTO_CAP_WEBRTC_PLAY,
    ZMS_PROTO_CAP_WEBRTC_PUBLISH,
    ZMS_PROTO_CAP_SRT_PLAY,
    ZMS_PROTO_CAP_SRT_PUBLISH,
    ZMS_PROTO_CAP_RTP_PS_PUBLISH,
    ZMS_PROTO_CAP_HLS_PLAY,
    ZMS_PROTO_CAP_DASH_PLAY,
} zms_session_cap_role;

/** 按音视频 codec 检查能力；无轨时传 ZMS_CODEC_INVALID*/
ZMS_API ztk_err_t zms_session_capability_check(zms_session_cap_role role, zms_codec_id video,
                                               zms_codec_id audio, int has_video, int has_audio);

/** source 轨道/config 读取 codec 后检查 */
ZMS_API ztk_err_t zms_session_capability_check_source(zms_session_cap_role role,
                                                      const zms_media_source *src);

/** 推流入站：按 source schema / publish_origin 选择 publish 能力位 */
ZMS_API ztk_err_t zms_session_capability_check_publish_source(const zms_media_source *src);

ZMS_API zms_session_cap_role zms_session_capability_play_role(const char *player);
ZMS_API zms_session_cap_role zms_session_capability_publish_role(const zms_media_source *src);

ZMS_API const char *zms_session_capability_role_name(zms_session_cap_role role);
ZMS_API const char *zms_session_capability_allowed_hint(zms_session_cap_role role);

ZMS_API void zms_session_capability_log_reject(const char *player, const zms_media_source *src,
                                               zms_session_cap_role role);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_SESSION_CODEC_FILTER_H */
