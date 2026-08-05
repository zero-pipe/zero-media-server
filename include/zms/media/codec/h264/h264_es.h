#ifndef ZMS_CODEC_H264_ES_H
#define ZMS_CODEC_H264_ES_H

#include "zms/zms_export.h"
#include "ztk/ztk_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

ZMS_API int zms_h264_annexb_extract_sps_pps(const uint8_t *annexb, size_t len, const uint8_t **sps,
                                            size_t *sps_len, const uint8_t **pps, size_t *pps_len);

/** ES 头部为 Annex-B 起始码（区分 0x00000001 AVCC 长度前缀）。 */
ZMS_API int zms_h264_es_is_annexb(const uint8_t *data, size_t len);

/** Annex-B 或 AVCC 长度前缀 elementary stream。 */
ZMS_API int zms_h264_es_extract_sps_pps(const uint8_t *data, size_t len, const uint8_t **sps,
                                        size_t *sps_len, const uint8_t **pps, size_t *pps_len);

/** 原始 AVCDecoderConfigurationRecord（MPEG-TS PMT esinfo），非 FLV 封装。 */
ZMS_API int zms_h264_avcc_extract_sps_pps(const uint8_t *avcc, size_t len, const uint8_t **sps,
                                          size_t *sps_len, const uint8_t **pps, size_t *pps_len);

ZMS_API int zms_h264_es_has_slice(const uint8_t *data, size_t len);

/** annex-B ES 中首个 VCL NAL 的 first_mb_in_slice 置位（新访问单元）。 */
ZMS_API int zms_h264_annexb_first_slice(const uint8_t *annexb, size_t len);

/** ES 开始新 H264 访问单元时为真（MPEG-TS PES 边界提示）。 */
ZMS_API int zms_h264_es_starts_access_unit(const uint8_t *es, size_t len);

ZMS_API ztk_err_t zms_h264_es_to_annexb(const uint8_t *data, size_t len, uint8_t *out, size_t cap,
                                        size_t *out_len);

/** IDR 或 non-IDR I 切片（GOP 起播/sync） */
ZMS_API int zms_h264_annexb_is_sync_key(const uint8_t *annexb, size_t len);

/** 点播起播：access unit 须含 NAL type 5 (IDR) */
ZMS_API int zms_h264_annexb_is_idr(const uint8_t *annexb, size_t len);

ZMS_API ztk_err_t zms_h264_annexb_prepend_sps_pps(const uint8_t *sps, size_t sps_len,
                                                  const uint8_t *pps, size_t pps_len,
                                                  const uint8_t *body, size_t body_len,
                                                  uint8_t *out, size_t cap, size_t *out_len);

/** RTSP/RTP：仅保留 VCL NAL（type 1–5），参数集由 SDP sprop 提供 */
ZMS_API ztk_err_t zms_h264_annexb_copy_vcl(const uint8_t *annexb, size_t len, uint8_t *out,
                                           size_t cap, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_CODEC_H264_ES_H */
