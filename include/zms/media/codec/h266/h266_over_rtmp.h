#ifndef ZMS_CODEC_H266_H266_OVER_RTMP_H
#define ZMS_CODEC_H266_H266_OVER_RTMP_H

#include "zms/zms_export.h"
#include "ztk/ztk_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

ZMS_API int zms_h266_over_rtmp_config_extradata(const uint8_t *data, size_t len,
                                                const uint8_t **vvcc, size_t *vvcc_len);

ZMS_API int zms_h266_video_config_vvcc(const uint8_t *cfg, size_t cfg_len, const uint8_t **vvcc,
                                       size_t *vvcc_len);

ZMS_API ztk_err_t zms_h266_flv_sequence_header(const uint8_t *cfg, size_t cfg_len, uint8_t *out,
                                               size_t cap, size_t *out_len);

ZMS_API ztk_err_t zms_h266_over_rtmp_pack_es(const uint8_t *video_cfg, size_t cfg_len,
                                             const uint8_t *annexb, size_t len, int key,
                                             uint8_t *out, size_t cap, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_CODEC_H266_H266_OVER_RTMP_H */
