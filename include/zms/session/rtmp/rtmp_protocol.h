#ifndef ZMS_SESSION_RTMP_PROTOCOL_H
#define ZMS_SESSION_RTMP_PROTOCOL_H

#include "rtmp_amf.h"
#include "rtmp.h"
#include "zms/zms_export.h"
#include "ztk/ztk_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ztk_poller;

typedef struct zms_rtmp_chunk zms_rtmp_chunk;

typedef void (*zms_rtmp_on_chunk_cb)(const zms_rtmp_chunk *chunk, void *user);
typedef void (*zms_rtmp_on_cmd_cb)(const char *cmd, double trans_id, const uint8_t *amf_rest,
                                   size_t amf_rest_len, void *user);

typedef struct zms_rtmp_protocol zms_rtmp_protocol;

typedef struct zms_rtmp_protocol_opts {
    zms_rtmp_on_chunk_cb on_chunk;
    zms_rtmp_on_cmd_cb on_cmd;
    void *user;
} zms_rtmp_protocol_opts;

typedef struct zms_rtmp_chunk {
    uint8_t type_id;
    uint32_t stream_id;
    uint32_t tag_dts_ms;
    const uint8_t *body;
    size_t body_size;
} zms_rtmp_chunk;

ZMS_API zms_rtmp_protocol *zms_rtmp_protocol_create(const zms_rtmp_protocol_opts *opts);
/** 握手已完成（客户端侧），直接进入 chunk 解析 */
ZMS_API zms_rtmp_protocol *zms_rtmp_protocol_create_established(const zms_rtmp_protocol_opts *opts);
ZMS_API void zms_rtmp_protocol_destroy(zms_rtmp_protocol *p);
/** 绑定会话 poller：rx_buf / cs.body 走 poller 本地池（须在首次 input 前调用） */
ZMS_API void zms_rtmp_protocol_set_poller(zms_rtmp_protocol *p, struct ztk_poller *poller);
ZMS_API ztk_err_t zms_rtmp_protocol_input(zms_rtmp_protocol *p, const void *data, size_t len);
ZMS_API ztk_err_t zms_rtmp_protocol_send_handshake_s0s1s2(zms_rtmp_protocol *p, uint8_t *out,
                                                          size_t cap, size_t *out_len);
ZMS_API ztk_err_t zms_rtmp_protocol_send_invoke(zms_rtmp_protocol *p, const char *cmd,
                                                double trans_id, uint32_t msg_stream_id,
                                                const uint8_t *amf_body, size_t amf_len,
                                                uint8_t *out, size_t cap, size_t *out_len);
ZMS_API ztk_err_t zms_rtmp_protocol_send_chunk(zms_rtmp_protocol *p, uint8_t type_id,
                                               uint32_t stream_id, uint32_t tag_dts_ms,
                                               const void *body, size_t len, uint8_t *out,
                                               size_t cap, size_t *out_len);
ZMS_API int zms_rtmp_protocol_handshake_pending(const zms_rtmp_protocol *p);
ZMS_API int zms_rtmp_protocol_handshake_complete(const zms_rtmp_protocol *p);
ZMS_API ztk_err_t zms_rtmp_protocol_send_server_init(zms_rtmp_protocol *p, uint8_t *out, size_t cap,
                                                     size_t *out_len);
ZMS_API void zms_rtmp_protocol_set_out_chunk_size(zms_rtmp_protocol *p, uint32_t size);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_SESSION_RTMP_PROTOCOL_H */
