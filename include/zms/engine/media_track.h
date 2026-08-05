#ifndef ZMS_ENGINE_TRACK_MEDIA_TRACK_H
#define ZMS_ENGINE_TRACK_MEDIA_TRACK_H

#include "zms/engine/frame.h"
#include "zms/zms_export.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZMS_TRACK_CTRL_MAX 128
#define ZMS_TRACK_FMTP_MAX 256

/** 已解析的 SDP / RTSP 媒体轨（客户端 RECORD / 播放）。 */
typedef struct zms_media_track {
    zms_track_type type;
    zms_codec_id codec;
    int payload_type;
    int sample_rate;
    int channels;
    int ready;
    uint8_t interleaved_rtp;
    uint8_t interleaved_rtcp;
    char control[ZMS_TRACK_CTRL_MAX];
    char fmtp[ZMS_TRACK_FMTP_MAX];
} zms_media_track;

#ifdef __cplusplus
}
#endif

#endif /* ZMS_ENGINE_TRACK_MEDIA_TRACK_H */
