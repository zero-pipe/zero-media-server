#include "zms/session/http/websocket/websocket_framer.h"
#include <string.h>

size_t zms_ws_framer_server_binary_hdr(uint8_t *hdr, size_t cap, size_t payload_len)
{
    if (!hdr || cap < 2) {
        return 0;
    }
    hdr[0] = 0x82;
    if (payload_len < 126) {
        hdr[1] = (uint8_t)payload_len;
        return 2;
    }
    if (payload_len <= 0xFFFF) {
        if (cap < 4) {
            return 0;
        }
        hdr[1] = 126;
        hdr[2] = (uint8_t)((payload_len >> 8) & 0xFF);
        hdr[3] = (uint8_t)(payload_len & 0xFF);
        return 4;
    }
    if (cap < 10) {
        return 0;
    }
    hdr[1] = 127;
    hdr[2] = hdr[3] = hdr[4] = hdr[5] = 0;
    hdr[6] = (uint8_t)((payload_len >> 24) & 0xFF);
    hdr[7] = (uint8_t)((payload_len >> 16) & 0xFF);
    hdr[8] = (uint8_t)((payload_len >> 8) & 0xFF);
    hdr[9] = (uint8_t)(payload_len & 0xFF);
    return 10;
}

static size_t ws_skip_frame(const uint8_t *data, size_t len, uint8_t *opcode_out,
                            size_t *payload_len_out)
{
    size_t off = 0;
    uint8_t b0, b1;
    uint64_t plen;
    size_t hlen;

    if (!data || len < 2) {
        return 0;
    }
    b0 = data[0];
    b1 = data[1];
    if (opcode_out) {
        *opcode_out = b0 & 0x0F;
    }
    off = 2;
    plen = b1 & 0x7F;
    if (plen == 126) {
        if (len < 4) {
            return 0;
        }
        plen = ((uint64_t)data[2] << 8) | data[3];
        off = 4;
    } else if (plen == 127) {
        if (len < 10) {
            return 0;
        }
        plen = 0;
        for (int i = 0; i < 8; ++i) {
            plen = (plen << 8) | data[2 + i];
        }
        off = 10;
    }
    if (b1 & 0x80) {
        off += 4;
    }
    hlen = off;
    if (len < hlen + plen) {
        return 0;
    }
    if (payload_len_out) {
        *payload_len_out = (size_t)plen;
    }
    return hlen + (size_t)plen;
}

size_t zms_ws_framer_consume_client(const uint8_t *data, size_t len, zms_ws_send_fn send,
                                    zms_ws_on_close_fn on_close, void *user)
{
    size_t total = 0;

    while (total < len) {
        uint8_t opcode = 0;
        size_t plen = 0;
        size_t consumed = ws_skip_frame(data + total, len - total, &opcode, &plen);

        if (consumed == 0) {
            break;
        }
        if (opcode == 0x8) {
            if (on_close) {
                on_close(user);
            }
            total += consumed;
            break;
        }
        if (opcode == 0x9 && send) {
            uint8_t pong[2] = {0x8A, 0x00};
            send(user, pong, 2);
        }
        total += consumed;
    }
    return total;
}
