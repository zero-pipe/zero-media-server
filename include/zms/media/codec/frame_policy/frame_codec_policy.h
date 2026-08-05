#ifndef ZMS_CODEC_FRAME_CODEC_POLICY_H
#define ZMS_CODEC_FRAME_CODEC_POLICY_H

#include "zms/media/codec/codec_id.h"
#include "zms/engine/frame.h"
#include "zms/zms_export.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zms_frame_codec_ops {
    zms_codec_id id;
    const char *name;
    int (*is_config_frame)(const zms_frame *f);
    int (*is_keyframe)(const zms_frame *f);
    int (*gop_queue_storable)(const zms_frame *f, int cache_started, int has_video);
    int (*gop_queue_new_gop)(const zms_frame *f, int video_key_pos, int has_video);
} zms_frame_codec_ops;

ZMS_API void zms_frame_codec_register(const zms_frame_codec_ops *ops);
ZMS_API const zms_frame_codec_ops *zms_frame_codec_find(zms_codec_id id);
ZMS_API void zms_frame_codec_register_all(void);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_CODEC_FRAME_CODEC_POLICY_H */
