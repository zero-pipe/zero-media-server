#ifndef ZMS_CODEC_AAC_AAC_OVER_RTMP_H
#define ZMS_CODEC_AAC_AAC_OVER_RTMP_H

#include "zms/zms_export.h"
#include "ztk/ztk_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

ZMS_API int zms_rtmp_aac_extradata(const uint8_t *data, size_t len, const uint8_t **asc,
                                   size_t *asc_len);
ZMS_API ztk_err_t zms_rtmp_aac_seq_header(const uint8_t *asc, size_t asc_len, uint8_t *out,
                                          size_t cap, size_t *out_len);
ZMS_API ztk_err_t zms_rtmp_aac_frame(const uint8_t *aac, size_t len, uint32_t tag_dts_ms,
                                     uint8_t *out, size_t cap, size_t *out_len);

ZMS_API int zms_aac_parse_asc(const uint8_t *asc, size_t len, int *sample_rate, int *channels);
/** 解析 ADTS 固定头（7+ 字节）；非 ADTS 或无效时返回 0。 */
ZMS_API int zms_aac_adts_parse(const uint8_t *adts, size_t len, int *sample_rate, int *channels);
ZMS_API int zms_aac_build_config_hex(int sample_rate, int channels, char *hex, size_t hex_cap);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_CODEC_AAC_AAC_OVER_RTMP_H */
