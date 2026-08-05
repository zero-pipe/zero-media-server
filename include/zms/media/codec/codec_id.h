#ifndef ZMS_CODEC_CODEC_ID_H
#define ZMS_CODEC_CODEC_ID_H

/**
 * 生产路径编解码 ID（H.264 / H.265 + AAC）。
 */
#include "zms/zms_export.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum zms_track_type {
    ZMS_TRACK_INVALID = 0,
    ZMS_TRACK_VIDEO = 1,
    ZMS_TRACK_AUDIO = 2,
} zms_track_type;

typedef enum zms_codec_id {
    ZMS_CODEC_INVALID = 0,
    ZMS_CODEC_H264 = 1,
    ZMS_CODEC_H265 = 2,
    ZMS_CODEC_AAC = 3,
    ZMS_CODEC_OPUS = 4,
    ZMS_CODEC_G711A = 5,
    ZMS_CODEC_G711U = 6,
    ZMS_CODEC_AV1 = 7,
    ZMS_CODEC_VP8 = 8,
    ZMS_CODEC_VP9 = 9,
    ZMS_CODEC_H266 = 10,
} zms_codec_id;

typedef struct zms_codec_meta {
    zms_codec_id id;
    zms_track_type track;
    const char *name;
    const char *sdp_mime;
    int mpeg_psi_id;
} zms_codec_meta;

ZMS_API const zms_codec_meta *zms_codec_meta_get(zms_codec_id id);
ZMS_API zms_codec_id zms_codec_id_from_name(const char *name);
ZMS_API const char *zms_codec_name(zms_codec_id id);
ZMS_API zms_track_type zms_codec_track_type(zms_codec_id id);
/** @return @a id 的 MPEG-TS stream_type；未走 TS 则为 0。 */
ZMS_API int zms_codec_mpeg_psi(zms_codec_id id);
/** @return MPEG-TS stream_type 对应的 zms_codec_id（含 _subset/AAC-LATM 变体），或 ZMS_CODEC_INVALID。 */
ZMS_API zms_codec_id zms_codec_from_mpeg_psi(int psi);
ZMS_API unsigned zms_codec_count(void);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_CODEC_CODEC_ID_H */
