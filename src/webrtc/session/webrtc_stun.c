#include "webrtc/session/webrtc_media_internal.h"
#include <openssl/hmac.h>
#include <string.h>

#define STUN_MAGIC 0x2112A442u
#define STUN_ATTR_XOR_MAPPED 0x0020u
#define STUN_ATTR_MESSAGE_INTEGRITY 0x0008u
#define STUN_ATTR_FINGERPRINT 0x8028u
#define STUN_FINGERPRINT_XOR 0x5354554eu

static uint16_t stun_read_u16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void stun_write_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xff);
}

static void stun_write_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v & 0xff);
}

static uint32_t stun_read_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint32_t stun_crc32(const uint8_t *buf, size_t len)
{
    uint32_t crc = 0xffffffffu;
    size_t i;
    int b;

    for (i = 0; i < len; ++i) {
        crc ^= buf[i];
        for (b = 0; b < 8; ++b) {
            crc = (crc >> 1) ^ (0xedb88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return ~crc;
}

int zms_webrtc_stun_is_binding_req(const uint8_t *data, size_t len)
{
    uint16_t msg_type;
    uint16_t msg_len;

    if (!data || len < 20) {
        return 0;
    }
    if ((data[0] & 0xC0) != 0x00) {
        return 0;
    }
    msg_type = stun_read_u16(data);
    if (msg_type != 0x0001) {
        return 0;
    }
    msg_len = stun_read_u16(data + 2);
    if ((size_t)msg_len + 20 > len) {
        return 0;
    }
    if (stun_read_u32(data + 4) != STUN_MAGIC) {
        return 0;
    }
    return 1;
}

size_t zms_webrtc_stun_binding_reply(const uint8_t *req, size_t req_len, uint8_t *out,
                                     size_t out_cap, uint32_t xor_ip_be, uint16_t xor_port,
                                     const char *pwd)
{
    uint16_t xor_port_val;
    uint32_t xor_ip_val;
    size_t body_len;
    size_t mi_off;
    size_t total;
    unsigned int hmac_len = 20;
    uint32_t fp;

    if (!req || req_len < 20 || !out || out_cap < 64 || !pwd || !pwd[0]) {
        return 0;
    }
    if (!zms_webrtc_stun_is_binding_req(req, req_len)) {
        return 0;
    }

    memset(out, 0, out_cap);
    stun_write_u16(out, 0x0101);
    stun_write_u32(out + 4, STUN_MAGIC);
    memcpy(out + 8, req + 8, 12);

    stun_write_u16(out + 20, STUN_ATTR_XOR_MAPPED);
    stun_write_u16(out + 22, 8);
    out[24] = 0x00;
    out[25] = 0x01;
    xor_port_val = xor_port ^ (uint16_t)(STUN_MAGIC >> 16);
    stun_write_u16(out + 26, xor_port_val);
    xor_ip_val = xor_ip_be ^ STUN_MAGIC;
    stun_write_u32(out + 28, xor_ip_val);

    mi_off = 32;
    stun_write_u16(out + mi_off, STUN_ATTR_MESSAGE_INTEGRITY);
    stun_write_u16(out + mi_off + 2, 20);

    body_len = 12u + 24u;
    stun_write_u16(out + 2, (uint16_t)body_len);
    memset(out + mi_off + 4, 0, 20);
    if (!HMAC(EVP_sha1(), pwd, (int)strlen(pwd), out, mi_off + 4 + 20, out + mi_off + 4,
              &hmac_len) ||
        hmac_len != 20) {
        return 0;
    }

    total = 20u + body_len;
    stun_write_u16(out + total, STUN_ATTR_FINGERPRINT);
    stun_write_u16(out + total + 2, 4);
    fp = stun_crc32(out, total) ^ STUN_FINGERPRINT_XOR;
    stun_write_u32(out + total + 4, fp);
    body_len += 8u;
    stun_write_u16(out + 2, (uint16_t)body_len);
    return total + 8u;
}

int zms_webrtc_packet_is_dtls(const uint8_t *data, size_t len)
{
    if (!data || len < 1) {
        return 0;
    }
    return data[0] >= 20 && data[0] <= 64;
}

int zms_webrtc_packet_is_rtp(const uint8_t *data, size_t len)
{
    if (!data || len < 12) {
        return 0;
    }
    if ((data[0] & 0xC0) != 0x80) {
        return 0;
    }
    return 1;
}
