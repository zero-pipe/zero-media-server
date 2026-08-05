#include "webrtc/session/webrtc_media_internal.h"
#include "zms/session/rtp/rtcp.h"
#include "zms/media/wire/rtp_packet.h"
#include <string.h>

int zms_webrtc_rtcp_is_psfb_pli(const uint8_t *data, size_t len)
{
    if (!data || len < 12) {
        return 0;
    }
    if ((data[0] & 0xC0) != 0x80) {
        return 0;
    }
    if (data[1] != 206) {
        return 0;
    }
    if ((data[0] & 0x1F) != 1) {
        return 0;
    }
    return 1;
}

int zms_webrtc_rtcp_is_rtpfb_nack(const uint8_t *data, size_t len)
{
    if (!data || len < 12) {
        return 0;
    }
    if ((data[0] & 0xC0) != 0x80) {
        return 0;
    }
    if (data[1] != 205) {
        return 0;
    }
    if ((data[0] & 0x1F) != 1) {
        return 0;
    }
    return 1;
}

size_t zms_webrtc_rtcp_build_sr(uint8_t *out, size_t cap, uint32_t ssrc, uint32_t ntp_sec,
                                uint32_t ntp_frac, uint32_t rtp_ts, uint32_t packet_count,
                                uint32_t octet_count)
{
    return zms_rtcp_build_sr(out, cap, ssrc, ntp_sec, ntp_frac, rtp_ts, packet_count, octet_count);
}

void zms_webrtc_rtcp_for_each_compound(const uint8_t *data, size_t len,
                                       void (*cb)(const uint8_t *block, size_t block_len,
                                                  void *user),
                                       void *user)
{
    size_t off = 0;

    if (!data || !cb) {
        return;
    }
    while (off + 4 <= len) {
        uint16_t blk_len = (uint16_t)(((uint16_t)data[off + 2] << 8) | data[off + 3]);
        size_t total = ((size_t)blk_len + 1u) * 4u;
        if (total < 4 || off + total > len) {
            break;
        }
        cb(data + off, total, user);
        off += total;
    }
}
