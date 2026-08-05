#include "zms/session/rtp/rtcp.h"
#include <string.h>

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

ztk_err_t zms_rtcp_parse_sr(const uint8_t *data, size_t len, zms_rtcp_sr *out)
{
    if (!data || !out || len < 28) {
        return ZTK_ERR_INVALID;
    }
    if ((data[0] & 0xc0) != 0x80 || data[1] != 200) {
        return ZTK_ERR_INVALID;
    }

    memset(out, 0, sizeof(*out));
    out->ssrc = be32(data + 4);
    out->ntp_sec = be32(data + 8);
    out->ntp_frac = be32(data + 12);
    out->rtp_ts = be32(data + 16);
    out->packet_count = be32(data + 20);
    out->octet_count = be32(data + 24);
    return ZTK_OK;
}

ztk_err_t zms_rtcp_parse_rr(const uint8_t *data, size_t len, zms_rtcp_rr *out)
{
    if (!data || !out || len < 32) {
        return ZTK_ERR_INVALID;
    }
    if ((data[0] & 0xc0) != 0x80 || data[1] != 201) {
        return ZTK_ERR_INVALID;
    }

    memset(out, 0, sizeof(*out));
    out->ssrc = be32(data + 4);
    if (len >= 32) {
        out->fraction_lost = be32(data + 12);
        out->cumulative_lost = be32(data + 16) & 0xffffff;
        out->highest_seq = be32(data + 20);
        out->jitter = be32(data + 24);
        out->lsr = be32(data + 28);
    }
    return ZTK_OK;
}

size_t zms_rtcp_build_sr(uint8_t *out, size_t cap, uint32_t ssrc, uint32_t ntp_sec,
                         uint32_t ntp_frac, uint32_t rtp_ts, uint32_t packet_count,
                         uint32_t octet_count)
{
    if (!out || cap < 28) {
        return 0;
    }

    memset(out, 0, 28);
    out[0] = 0x80;
    out[1] = 200;
    out[2] = 0;
    out[3] = 6;
    put_be32(out + 4, ssrc);
    put_be32(out + 8, ntp_sec);
    put_be32(out + 12, ntp_frac);
    put_be32(out + 16, rtp_ts);
    put_be32(out + 20, packet_count);
    put_be32(out + 24, octet_count);
    return 28;
}

size_t zms_rtcp_build_rr(uint8_t *out, size_t cap, uint32_t ssrc, uint32_t sender_ssrc,
                         uint32_t fraction_lost, uint32_t highest_seq, uint32_t jitter)
{
    if (!out || cap < 32) {
        return 0;
    }

    memset(out, 0, 32);
    out[0] = 0x81; /* V=2, P=0, RC=1 */
    out[1] = 201;  /* RR */
    out[2] = 0;
    out[3] = 7; /* 长度（32 位字减 1） */

    put_be32(out + 4, ssrc);
    put_be32(out + 8, sender_ssrc);
    put_be32(out + 12, fraction_lost);
    put_be32(out + 16, highest_seq);
    put_be32(out + 20, jitter);
    /* lsr/dlsr 保持为零 */
    return 32;
}
