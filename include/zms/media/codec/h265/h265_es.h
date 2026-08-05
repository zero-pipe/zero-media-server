#ifndef ZMS_CODEC_H265_ES_H
#define ZMS_CODEC_H265_ES_H

#include "zms/zms_export.h"
#include "ztk/ztk_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** BLA / IDR / CRA 等随机接入点（GOP 起播/sync，NAL type 16–21） */
ZMS_API int zms_h265_annexb_is_sync_key(const uint8_t *annexb, size_t len);

/** 点播起播：access unit 须含 IDR 类 NAL（type 19–20） */
ZMS_API int zms_h265_annexb_is_idr(const uint8_t *annexb, size_t len);

ZMS_API int zms_h265_annexb_extract_vps_sps_pps(const uint8_t *annexb, size_t len,
                                                const uint8_t **vps, size_t *vps_len,
                                                const uint8_t **sps, size_t *sps_len,
                                                const uint8_t **pps, size_t *pps_len);

/** RTSP/RTP：仅保留 VCL NAL（type 0–31），去掉 AUD/SEI/参数集等 */
ZMS_API ztk_err_t zms_h265_annexb_copy_vcl(const uint8_t *annexb, size_t len, uint8_t *out,
                                           size_t cap, size_t *out_len);

/** sync 帧前 prepend VPS/SPS/PPS，再拼接 VCL-only AU（RFC7798 单 AU 多 RTP） */
ZMS_API ztk_err_t zms_h265_annexb_build_rtp_au(const uint8_t *vps, size_t vps_len,
                                               const uint8_t *sps, size_t sps_len,
                                               const uint8_t *pps, size_t pps_len,
                                               const uint8_t *annexb, size_t len,
                                               int prepend_params, uint8_t *out, size_t cap,
                                               size_t *out_len);

/** hvcC extradata → VPS/SPS/PPS 裸 NAL */
ZMS_API int zms_h265_hvcc_param_sets(const uint8_t *hvcc, size_t hvcc_len, const uint8_t **vps,
                                     size_t *vps_len, const uint8_t **sps, size_t *sps_len,
                                     const uint8_t **pps, size_t *pps_len);

/** video_config：raw hvcC 或 legacy FLV sequence header → VPS/SPS/PPS */
ZMS_API int zms_h265_video_config_param_sets(const uint8_t *cfg, size_t cfg_len,
                                             const uint8_t **vps, size_t *vps_len,
                                             const uint8_t **sps, size_t *sps_len,
                                             const uint8_t **pps, size_t *pps_len);

/** video_config → raw hvcC 指针（raw 或从 FLV header 剥离） */
ZMS_API int zms_h265_video_config_hvcc(const uint8_t *cfg, size_t cfg_len, const uint8_t **hvcc,
                                       size_t *hvcc_len);

ZMS_API ztk_err_t zms_h265_hvcc_from_annexb(const uint8_t *annexb, size_t len, uint8_t *out,
                                            size_t cap, size_t *out_len);
ZMS_API ztk_err_t zms_h265_hvcc_from_param_sets(const uint8_t *vps, size_t vps_len,
                                                const uint8_t *sps, size_t sps_len,
                                                const uint8_t *pps, size_t pps_len, uint8_t *out,
                                                size_t cap, size_t *out_len);

ZMS_API ztk_err_t zms_h265_param_sets_to_annexb(const uint8_t *vps, size_t vps_len,
                                                const uint8_t *sps, size_t sps_len,
                                                const uint8_t *pps, size_t pps_len, uint8_t *out,
                                                size_t cap, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_CODEC_H265_ES_H */
