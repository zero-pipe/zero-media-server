#ifndef ZMS_CODEC_G711_G711_OVER_RTP_H
#define ZMS_CODEC_G711_G711_OVER_RTP_H

#include "zms/media/codec/codec_id.h"
#include "zms/zms_export.h"
#include "ztk/ztk_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

ZMS_API uint8_t zms_g711_over_rtp_default_pt(zms_codec_id codec);
ZMS_API zms_codec_id zms_g711_codec_from_rtp_pt(uint8_t pt);
ZMS_API ztk_err_t zms_g711_over_rtp_unpack_payload(const uint8_t *payload, size_t len,
                                                   const uint8_t **g711, size_t *g711_len);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_CODEC_G711_G711_OVER_RTP_H */
