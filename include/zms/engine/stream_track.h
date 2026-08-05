#ifndef ZMS_ENGINE_TRACK_STREAM_TRACK_H
#define ZMS_ENGINE_TRACK_STREAM_TRACK_H

#include "zms/engine/frame.h"
#include "zms/zms_export.h"
#include "ztk/ztk_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 直播流视频元数据（来自 AVC config / SPS）。 */
typedef struct zms_video_track {
    zms_codec_id codec;
    int width;
    int height;
    float fps;
    char profile_level_id[16];
    int ready;
} zms_video_track;

/** 直播流音频元数据（来自 AAC ASC / RTMP audio config）。 */
typedef struct zms_audio_track {
    zms_codec_id codec;
    int sample_rate;
    int channels;
    char asc_hex[64];
    int ready;
} zms_audio_track;

ZMS_API void zms_video_track_clear(zms_video_track *t);
ZMS_API void zms_audio_track_clear(zms_audio_track *t);
ZMS_API ztk_err_t zms_video_track_from_avc(zms_video_track *t, const uint8_t *data, size_t len);
ZMS_API ztk_err_t zms_audio_track_from_rtmp(zms_audio_track *t, const uint8_t *data, size_t len);
ZMS_API ztk_err_t zms_audio_track_from_asc(zms_audio_track *t, const uint8_t *asc, size_t len);
ZMS_API ztk_err_t zms_audio_track_from_g711(zms_audio_track *t, zms_codec_id codec, int sample_rate,
                                            int channels);

/**
 * 尺寸是否明显不可信（常见厂商占位 SPS，如 208x64 / 208x96）。
 * 仅用于决定「后续码流 SPS 可否覆盖」，不强制任何业务分辨率。
 */
ZMS_API int zms_video_size_is_suspicious(uint32_t width, uint32_t height);

/**
 * 是否采纳 new 分辨率。
 * 原则：以码流 SPS 为准；仅拒绝「已有可信尺寸被占位 SPS 回退覆盖」。
 */
ZMS_API int zms_video_size_should_replace(uint32_t cur_w, uint32_t cur_h, uint32_t new_w,
                                          uint32_t new_h);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_ENGINE_TRACK_STREAM_TRACK_H */
