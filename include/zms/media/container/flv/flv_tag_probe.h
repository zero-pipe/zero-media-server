#ifndef ZMS_CONTAINER_FLV_TAG_PROBE_H
#define ZMS_CONTAINER_FLV_TAG_PROBE_H

/**
 * FLV tag body 只读探测：codec 识别、seq header / raw 分类、AAC ASC 定位。
 * 不负责 ES 解复用（见 flv_tag_demuxer）或 codec 打包（见 {codec}_over_rtmp）。
 */
#include "zms/media/codec/codec_id.h"
#include "zms/zms_export.h"
#include "ztk/ztk_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** FLV VideoTag / AudioTag 的 AVCPacketType / AACPacketType 语义。 */
typedef enum {
    ZMS_FLV_TAG_PKT_INVALID = -1,
    ZMS_FLV_TAG_PKT_RAW = 0,
    ZMS_FLV_TAG_PKT_SEQ_HEADER = 1,
    ZMS_FLV_TAG_PKT_END_OF_SEQ = 2,
    ZMS_FLV_TAG_PKT_OTHER = 3,
} zms_flv_tag_packet_kind;

ZMS_API zms_flv_tag_packet_kind zms_flv_tag_video_packet_kind(const uint8_t *tag, size_t len);
ZMS_API zms_flv_tag_packet_kind zms_flv_tag_audio_packet_kind(const uint8_t *tag, size_t len);

ZMS_API zms_codec_id zms_flv_tag_video_codec(const uint8_t *tag, size_t len);
ZMS_API zms_codec_id zms_flv_tag_audio_codec(const uint8_t *tag, size_t len);

/** avcC/hvcc 或 FLV seq header：识别 video config 的 codec。 */
ZMS_API zms_codec_id zms_flv_video_config_codec(const uint8_t *cfg, size_t len);

/** AAC sequence-header tag：指向 AudioSpecificConfig 字节。 */
ZMS_API ztk_err_t zms_flv_tag_aac_seq_header_asc(const uint8_t *tag, size_t len,
                                                 const uint8_t **asc, size_t *asc_len);

/** 剥 FLV audio tag 头，得到 AAC/G711 ES。 */
ZMS_API ztk_err_t zms_flv_tag_audio_to_es(const uint8_t *body, size_t len, const uint8_t **es,
                                          size_t *es_len, zms_codec_id *codec);

/** onMetaData videocodecid：H.264=7，Enhanced H.265='hvc1'(1752589105) */
ZMS_API double zms_flv_metadata_videocodecid(zms_codec_id id);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_CONTAINER_FLV_TAG_PROBE_H */
