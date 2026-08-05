#ifndef ZMS_CODEC_BITSTREAM_ANNEXB_H
#define ZMS_CODEC_BITSTREAM_ANNEXB_H

/**
 * @file annexb.h
 * @brief Annex-B NAL 查找（H.26x 字节流辅助）。
 */
#include "zms/zms_export.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

ZMS_API const uint8_t *zms_annexb_find_nal(const uint8_t *p, const uint8_t *end, size_t *nal_len);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_CODEC_BITSTREAM_ANNEXB_H */
