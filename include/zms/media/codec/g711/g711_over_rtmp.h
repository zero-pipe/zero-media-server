#ifndef ZMS_CODEC_G711_G711_OVER_RTMP_H
#define ZMS_CODEC_G711_G711_OVER_RTMP_H

/** G.711 PCM over RTMP/FLV audio tag body。 */
#include "zms/media/codec/codec_id.h"
#include "zms/zms_export.h"
#include "ztk/ztk_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

ZMS_API ztk_err_t zms_g711_over_rtmp_pack_es(zms_codec_id codec, const uint8_t *g711, size_t len,
                                             uint32_t tag_dts_ms, uint8_t *out, size_t cap,
                                             size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_CODEC_G711_G711_OVER_RTMP_H */
