#ifndef ZMS_CODEC_H264_OVER_RTMP_H
#define ZMS_CODEC_H264_OVER_RTMP_H

/**
 * H.264 Annex-B over RTMP/FLV media tag body：pack / unpack。
 * 见 Annex-B 工具于 zms/media/codec/h264/h264_es.h。
 */
#include "zms/media/codec/h264/h264_es.h"
#include "zms/zms_export.h"
#include "ztk/ztk_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

ZMS_API ztk_err_t zms_rtmp_avc_seq_header(const uint8_t *sps, size_t sps_len, const uint8_t *pps,
                                          size_t pps_len, uint8_t *out, size_t cap,
                                          size_t *out_len);
ZMS_API ztk_err_t zms_rtmp_h264_annexb(const uint8_t *annexb, size_t len, uint32_t tag_dts_ms,
                                       int key, uint8_t *out, size_t cap, size_t *out_len);
/** video_config 预载 AVCC 后打包 VCL（HTTP-FLV/RTMP play，seq header 已单独发送） */
ZMS_API ztk_err_t zms_h264_over_rtmp_pack_es(const uint8_t *video_cfg, size_t cfg_len,
                                             const uint8_t *annexb, size_t len, int key,
                                             uint8_t *out, size_t cap, size_t *out_len);
ZMS_API ztk_err_t zms_rtmp_video_tag_to_annexb(const uint8_t *body, size_t len, uint8_t *out,
                                               size_t cap, size_t *out_len, int *keyframe);
ZMS_API int zms_rtmp_avc_extract_sps_pps(const uint8_t *data, size_t len, const uint8_t **sps,
                                         size_t *sps_len, const uint8_t **pps, size_t *pps_len);
ZMS_API int zms_rtmp_avc_extradata(const uint8_t *data, size_t len, const uint8_t **avcc,
                                   size_t *avcc_len);
ZMS_API int zms_rtmp_avc_profile_level_id(const uint8_t *data, size_t len, char profile[16]);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_CODEC_H264_OVER_RTMP_H */
