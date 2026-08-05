#ifndef ZMS_CODEC_H265_OVER_RTMP_H
#define ZMS_CODEC_H265_OVER_RTMP_H

#include "zms/media/codec/h265/h265_es.h"
#include "zms/zms_export.h"
#include "ztk/ztk_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** raw hvcC 或 FLV sequence header → hvcC 指针 */
ZMS_API int zms_rtmp_hevc_extradata(const uint8_t *data, size_t len, const uint8_t **hvcc,
                                    size_t *hvcc_len);

/** ring/RTSP 存 raw hvcC 时，生成 Enhanced RTMP video tag body（sequence header） */
ZMS_API ztk_err_t zms_h265_flv_sequence_header(const uint8_t *cfg, size_t cfg_len, uint8_t *out,
                                               size_t cap, size_t *out_len);

/** video_config 预载 hvcC 后打包 VCL（Enhanced RTMP） */
ZMS_API ztk_err_t zms_h265_over_rtmp_pack_es(const uint8_t *video_cfg, size_t cfg_len,
                                             const uint8_t *annexb, size_t len, int key,
                                             uint8_t *out, size_t cap, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_CODEC_H265_OVER_RTMP_H */
