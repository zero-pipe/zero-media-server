#ifndef ZMS_VOD_CODEC_EXT_H
#define ZMS_VOD_CODEC_EXT_H

/**
 * 点播编解码扩展模板。
 */
#include "zms/zms_export.h"
#include "ztk/ztk_errno.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZMS_VOD_CODEC_HEVC "hevc"

typedef struct zms_vod_codec_ops {
    const char *name;
    int (*probe_mp4)(const char *file_path);
    ztk_err_t (*lane_prepare)(void *ctx);
    void (*lane_destroy)(void *ctx);
} zms_vod_codec_ops;

ZMS_API void zms_vod_codec_ext_register(const zms_vod_codec_ops *ops);
ZMS_API void zms_vod_codec_ext_register_all(void);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_VOD_CODEC_EXT_H */
