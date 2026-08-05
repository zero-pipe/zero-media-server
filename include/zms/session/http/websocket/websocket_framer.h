#ifndef ZMS_SESSION_HTTP_WEBSOCKET_WS_FRAMER_H
#define ZMS_SESSION_HTTP_WEBSOCKET_WS_FRAMER_H

#include "zms/zms_export.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 服务端发往客户端的二进制帧头长度（载荷未掩码）。 */
ZMS_API size_t zms_ws_framer_server_binary_hdr(uint8_t *hdr, size_t cap, size_t payload_len);

/** 解析客户端帧；close 时调 on_close，ping 时回 pong。返回已消费字节数。 */
typedef void (*zms_ws_on_close_fn)(void *user);
typedef int (*zms_ws_send_fn)(void *user, const void *data, size_t len);
ZMS_API size_t zms_ws_framer_consume_client(const uint8_t *data, size_t len, zms_ws_send_fn send,
                                            zms_ws_on_close_fn on_close, void *user);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_SESSION_HTTP_WEBSOCKET_WS_FRAMER_H */
