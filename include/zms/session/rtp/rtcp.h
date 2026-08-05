#ifndef ZMS_SESSION_RTP_RTCP_H
#define ZMS_SESSION_RTP_RTCP_H

#include "zms/zms_export.h"
#include "ztk/ztk_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zms_rtcp_sr {
    uint32_t ssrc;
    uint32_t ntp_sec;
    uint32_t ntp_frac;
    uint32_t rtp_ts;
    uint32_t packet_count;
    uint32_t octet_count;
} zms_rtcp_sr;

typedef struct zms_rtcp_rr {
    uint32_t ssrc;
    uint32_t fraction_lost;
    uint32_t cumulative_lost;
    uint32_t highest_seq;
    uint32_t jitter;
    uint32_t lsr;
    uint32_t dlsr;
} zms_rtcp_rr;

ZMS_API ztk_err_t zms_rtcp_parse_sr(const uint8_t *data, size_t len, zms_rtcp_sr *out);
ZMS_API ztk_err_t zms_rtcp_parse_rr(const uint8_t *data, size_t len, zms_rtcp_rr *out);
ZMS_API size_t zms_rtcp_build_sr(uint8_t *out, size_t cap, uint32_t ssrc, uint32_t ntp_sec,
                                 uint32_t ntp_frac, uint32_t rtp_ts, uint32_t packet_count,
                                 uint32_t octet_count);
ZMS_API size_t zms_rtcp_build_rr(uint8_t *out, size_t cap, uint32_t ssrc, uint32_t sender_ssrc,
                                 uint32_t fraction_lost, uint32_t highest_seq, uint32_t jitter);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_SESSION_RTP_RTCP_H */
