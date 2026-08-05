#ifndef ZMS_CODEC_VPX_VPX_OVER_RTMP_H
#define ZMS_CODEC_VPX_VPX_OVER_RTMP_H

#include "zms/media/codec/codec_id.h"
#include "zms/zms_export.h"
#include "ztk/ztk_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

ZMS_API int zms_vpx_over_rtmp_config_extradata(zms_codec_id codec, const uint8_t *data, size_t len,
                                               const uint8_t **vpxc, size_t *vpxc_len);

ZMS_API ztk_err_t zms_vpx_flv_sequence_header(zms_codec_id codec, const uint8_t *cfg,
                                              size_t cfg_len, uint8_t *out, size_t cap,
                                              size_t *out_len);

ZMS_API ztk_err_t zms_vpx_over_rtmp_pack_es(zms_codec_id codec, const uint8_t *video_cfg,
                                            size_t cfg_len, const uint8_t *es, size_t len, int key,
                                            uint8_t *out, size_t cap, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_CODEC_VPX_VPX_OVER_RTMP_H */
