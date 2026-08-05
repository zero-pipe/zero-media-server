#ifndef ZMS_VOD_FLV_INDEX_H
#define ZMS_VOD_FLV_INDEX_H

#include "zms/zms_export.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zms_vod_flv_index {
    double *times;
    double *filepositions;
    size_t count;
    double filesize;
    size_t metadata_bytes;
} zms_vod_flv_index;

/** HTTP 会话内相对索引起点（filepositions 相对本响应 byte 0） */
typedef struct zms_vod_flv_index_view {
    double *times;
    double *filepositions;
    size_t count;
    double filesize;
} zms_vod_flv_index_view;

ZMS_API void zms_vod_flv_index_free(zms_vod_flv_index *idx);
/** 按 MP4 sample 表估算 FLV keyframes 索引（times/filepositions 供 HTTP-FLV seek）。
 *  @param mp4_demux MP4 容器 demux ctx（仅 MP4 有效），经 container/mp4 sample 迭代器遍历。 */
ZMS_API zms_vod_flv_index *zms_vod_flv_index_build(void *mp4_demux, size_t video_cfg_len,
                                                   size_t audio_cfg_len, size_t metadata_bytes);
ZMS_API double zms_vod_flv_index_byte_at_ms(const zms_vod_flv_index *idx, uint64_t play_ms);
ZMS_API zms_vod_flv_index_view *zms_vod_flv_index_view_create(const zms_vod_flv_index *idx,
                                                              uint64_t play_ms);
ZMS_API void zms_vod_flv_index_view_free(zms_vod_flv_index_view *view);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_VOD_FLV_INDEX_H */
