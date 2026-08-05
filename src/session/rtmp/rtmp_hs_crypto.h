#ifndef ZMS_SRC_SESSION_RTMP_HS_CRYPTO_H
#define ZMS_SRC_SESSION_RTMP_HS_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

void zms_hs_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *data, size_t data_len,
                        uint8_t out[32]);

#endif /* ZMS_SRC_SESSION_RTMP_HS_CRYPTO_H */
