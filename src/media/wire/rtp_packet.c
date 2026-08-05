#include "zms/media/wire/rtp_packet.h"
#include <string.h>

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

ztk_err_t zms_rtp_parse(const uint8_t *data, size_t len, zms_rtp_packet *out)
{
    if (!data || !out || len < ZMS_RTP_HDR_SIZE) {
        return ZTK_ERR_INVALID;
    }

    memset(out, 0, sizeof(*out));
    out->data = data;
    out->size = len;

    uint8_t b0 = data[0];
    out->hdr.version = (b0 >> 6) & 0x03;
    out->hdr.padding = (b0 >> 5) & 0x01;
    out->hdr.extension = (b0 >> 4) & 0x01;
    out->hdr.csrc_count = b0 & 0x0f;

    uint8_t b1 = data[1];
    out->hdr.marker = (b1 >> 7) & 0x01;
    out->hdr.pt = b1 & 0x7f;
    out->hdr.seq = be16(data + 2);
    out->hdr.timestamp = be32(data + 4);
    out->hdr.ssrc = be32(data + 8);

    if (out->hdr.version != 2) {
        return ZTK_ERR_INVALID;
    }

    size_t off = ZMS_RTP_HDR_SIZE + (size_t)out->hdr.csrc_count * 4;
    if (off > len) {
        return ZTK_ERR_INVALID;
    }

    if (out->hdr.extension) {
        if (off + 4 > len) {
            return ZTK_ERR_INVALID;
        }
        uint16_t ext_len = be16(data + off + 2);
        off += 4 + (size_t)ext_len * 4;
        if (off > len) {
            return ZTK_ERR_INVALID;
        }
    }

    out->payload = data + off;
    out->payload_size = len - off;

    if (out->hdr.padding && out->payload_size > 0) {
        uint8_t pad = data[len - 1];
        if (pad == 0 || pad > out->payload_size) {
            return ZTK_ERR_INVALID;
        }
        out->payload_size -= pad;
    }
    return ZTK_OK;
}

size_t zms_rtp_payload_offset(const uint8_t *data, size_t len)
{
    zms_rtp_packet pkt;
    if (zms_rtp_parse(data, len, &pkt) != ZTK_OK) {
        return 0;
    }
    return (size_t)(pkt.payload - data);
}

uint8_t zms_rtp_payload_type(const uint8_t *data, size_t len)
{
    if (!data || len < 2) {
        return 0;
    }
    return (uint8_t)(data[1] & 0x7f);
}

void zms_rtp_set_payload_type(uint8_t *data, size_t len, uint8_t pt)
{
    if (!data || len < 2) {
        return;
    }
    data[1] = (uint8_t)((data[1] & 0x80) | (pt & 0x7f));
}

int zms_rtp_is_rtcp(const uint8_t *data, size_t len)
{
    uint8_t pt;

    if (!data || len < 2) {
        return 0;
    }
    if ((data[0] & 0xc0) != 0x80) {
        return 0;
    }
    pt = zms_rtp_payload_type(data, len);
    return pt >= 192 && pt <= 223;
}

size_t zms_rtsp_interleaved_write(uint8_t *out, size_t cap, uint8_t channel, const void *payload,
                                  size_t len)
{
    if (!out || cap < ZMS_RTSP_INTERLEAVED_HDR + len) {
        return 0;
    }
    out[0] = '$';
    out[1] = channel;
    out[2] = (uint8_t)((len >> 8) & 0xff);
    out[3] = (uint8_t)(len & 0xff);
    if (payload && len) {
        memcpy(out + 4, payload, len);
    }
    return ZMS_RTSP_INTERLEAVED_HDR + len;
}

ztk_err_t zms_rtsp_interleaved_read(const uint8_t *data, size_t len, uint8_t *channel,
                                    const uint8_t **payload, size_t *payload_len)
{
    if (!data || len < ZMS_RTSP_INTERLEAVED_HDR || data[0] != '$') {
        return ZTK_ERR_INVALID;
    }
    uint16_t plen = (uint16_t)((data[2] << 8) | data[3]);
    if (len < ZMS_RTSP_INTERLEAVED_HDR + plen) {
        return ZTK_ERR_AGAIN;
    }
    if (channel) {
        *channel = data[1];
    }
    if (payload) {
        *payload = data + ZMS_RTSP_INTERLEAVED_HDR;
    }
    if (payload_len) {
        *payload_len = plen;
    }
    return ZTK_OK;
}

uint32_t zms_rtp90_ts_to_ms(uint32_t rtp_ts)
{
    return rtp_ts / 90u;
}

uint32_t zms_rtp90_ms_to_ts(uint32_t ms)
{
    return ms * 90u;
}
