#ifndef ZMS_VOD_FLV_METADATA_H
#define ZMS_VOD_FLV_METADATA_H

#include "zms/zms_export.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct zms_media_source;
struct zms_vod_flv_index;
struct zms_vod_flv_index_view;

/** onMetaData script body 大小（含 keyframes） */
ZMS_API size_t zms_vod_flv_metadata_body_size(const struct zms_media_source *src,
                                              double duration_sec,
                                              const struct zms_vod_flv_index *idx);

/** 编码 onMetaData AMF body；返回 0 表示失败 */
ZMS_API size_t zms_vod_flv_metadata_encode(const struct zms_media_source *src, uint8_t *out,
                                           size_t cap, double duration_override_sec,
                                           const struct zms_vod_flv_index *idx,
                                           const struct zms_vod_flv_index_view *view);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_VOD_FLV_METADATA_H */
