#ifndef ZMS_SESSION_HTTP_WEBSOCKET_WS_HANDSHAKE_H
#define ZMS_SESSION_HTTP_WEBSOCKET_WS_HANDSHAKE_H

#include "zms/zms_export.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** RFC6455 accept key = base64(sha1(key + GUID))。out 29 字节 + NUL。 */
ZMS_API int zms_ws_handshake_accept(const char *sec_websocket_key, char *accept_out,
                                    size_t accept_cap);

/** 101 Switching Protocols 响应体（仅头）。返回写入长度或 0。 */
ZMS_API size_t zms_ws_handshake_response(char *out, size_t cap, const char *accept);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_SESSION_HTTP_WEBSOCKET_WS_HANDSHAKE_H */
