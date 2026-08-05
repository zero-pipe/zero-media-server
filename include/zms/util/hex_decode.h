#ifndef ZMS_UTIL_HEX_DECODE_H
#define ZMS_UTIL_HEX_DECODE_H

#include "zms/zms_export.h"
#include "ztk/ztk_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

ZMS_API ztk_err_t zms_hex_decode(const char *hex, uint8_t *out, size_t cap, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_UTIL_HEX_DECODE_H */
