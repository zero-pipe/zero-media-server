#ifndef ZMS_SRC_SESSION_RTMP_HANDSHAKE_H
#define ZMS_SRC_SESSION_RTMP_HANDSHAKE_H

#include <stdint.h>

#define ZMS_RTMP_HS_SIZE 1536

int zms_rtmp_hs_detect_complex(const uint8_t c1[ZMS_RTMP_HS_SIZE]);
int zms_rtmp_hs_build_complex(const uint8_t c1[ZMS_RTMP_HS_SIZE], uint8_t s1[ZMS_RTMP_HS_SIZE],
                              uint8_t s2[ZMS_RTMP_HS_SIZE]);
int zms_rtmp_hs_validate_c2(const uint8_t s1[ZMS_RTMP_HS_SIZE], const uint8_t c2[ZMS_RTMP_HS_SIZE]);

#endif /* ZMS_SRC_SESSION_RTMP_HANDSHAKE_H */
