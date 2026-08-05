#ifndef ZMS_UTIL_CRYPTO_SHA1_H
#define ZMS_UTIL_CRYPTO_SHA1_H

#include "zms/zms_export.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zms_sha1_ctx {
    uint32_t state[5];
    uint32_t count[2];
    uint8_t buffer[64];
} zms_sha1_ctx;

ZMS_API void zms_sha1_init(zms_sha1_ctx *ctx);
ZMS_API void zms_sha1_update(zms_sha1_ctx *ctx, const void *data, size_t len);
ZMS_API void zms_sha1_final(zms_sha1_ctx *ctx, uint8_t digest[20]);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_UTIL_CRYPTO_SHA1_H */
