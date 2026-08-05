#include "zms/session/http/websocket/websocket_handshake.h"
#include "zms/util/sha1.h"
#include "base64.h"
#include <stdio.h>
#include <string.h>

#define ZMS_WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

int zms_ws_handshake_accept(const char *sec_websocket_key, char *accept_out, size_t accept_cap)
{
    zms_sha1_ctx ctx;
    uint8_t digest[20];
    char concat[128];
    size_t n;

    if (!sec_websocket_key || !sec_websocket_key[0] || !accept_out || accept_cap < 29) {
        return -1;
    }
    n = snprintf(concat, sizeof(concat), "%s%s", sec_websocket_key, ZMS_WS_GUID);
    if (n >= sizeof(concat)) {
        return -1;
    }
    zms_sha1_init(&ctx);
    zms_sha1_update(&ctx, concat, n);
    zms_sha1_final(&ctx, digest);
    if (base64_encode(accept_out, digest, 20) == 0) {
        return -1;
    }
    return 0;
}

size_t zms_ws_handshake_response(char *out, size_t cap, const char *accept)
{
    if (!out || cap == 0 || !accept || !accept[0]) {
        return 0;
    }
    return (size_t)snprintf(out, cap,
                            "HTTP/1.1 101 Switching Protocols\r\n"
                            "Upgrade: websocket\r\n"
                            "Connection: Upgrade\r\n"
                            "Sec-WebSocket-Accept: %s\r\n"
                            "Access-Control-Allow-Origin: *\r\n\r\n",
                            accept);
}
