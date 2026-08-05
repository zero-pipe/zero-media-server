#ifndef ZMS_SRC_SESSION_RTMP_PROTOCOL_INTERNAL_H
#define ZMS_SRC_SESSION_RTMP_PROTOCOL_INTERNAL_H

#include "zms/session/rtmp/rtmp_amf.h"
#include "zms/session/rtmp/rtmp_protocol.h"
#include "ztk/ztk_errno.h"
#include <stddef.h>
#include <stdint.h>

typedef enum zms_rtmp_hs_state {
    ZMS_RTMP_HS_STATE_WAIT_C0C1,
    ZMS_RTMP_HS_STATE_WAIT_C2,
    ZMS_RTMP_HS_STATE_DONE,
} zms_rtmp_hs_state;

#define ZMS_RTMP_MAX_CS 320
#define ZMS_RTMP_HS_BODY 1536

typedef struct zms_rtmp_cs_state {
    uint8_t *body;
    size_t body_cap;
    size_t body_off;
    uint32_t csid;
    uint32_t tag_dts_ms;
    uint32_t tag_dts_field;
    uint32_t msg_len;
    uint32_t stream_id;
    uint32_t last_msg_len;
    uint32_t last_stream_id;
    uint32_t last_tag_dts_ms;
    uint32_t last_tag_dts_field;
    int is_abs_stamp;
    uint8_t type_id;
    uint8_t last_type_id;
} zms_rtmp_cs_state;

struct ztk_poller;

struct zms_rtmp_protocol {
    zms_rtmp_protocol_opts opts;
    struct ztk_poller *io_poller; /**< 有则 rx/cs body 走本地无锁池 */
    uint8_t *rx_buf;
    size_t rx_cap;
    size_t rx_len;
    size_t hs_off;
    size_t in_chunk_size;
    size_t out_chunk_size;
    /** 按 csid 懒分配；未用槽为 NULL（避免每连接 ~30KB 稠密表） */
    zms_rtmp_cs_state *cs[ZMS_RTMP_MAX_CS];
    int hs_state;
    int hs_need_reply;
    int hs_init_pending;
    int hs_complex;
    /**
     * 握手工作区（懒分配，DONE 后释放）：
     * c1 / s1 / s2 各 1536，hs_buf 为 1+1536。create_established 永不分配。
     */
    uint8_t *c1;
    uint8_t *s1;
    uint8_t *s2;
    uint8_t *hs_buf;
};

uint32_t rtmp_read_le32(const uint8_t *p);
uint32_t rtmp_read_be24(const uint8_t *p);
uint32_t rtmp_read_be32(const uint8_t *p);
void rtmp_write_be24(uint8_t *p, uint32_t v);
void rtmp_write_be32(uint8_t *p, uint32_t v);

ztk_err_t rtmp_rx_append(zms_rtmp_protocol *p, const uint8_t *data, size_t len);
void rtmp_rx_consume(zms_rtmp_protocol *p, size_t n);

/** 消费握手字节流；consumed 为已处理长度；返回 ZTK_OK 且 hs_state==ZMS_RTMP_HS_STATE_DONE 时握手结束 */
ztk_err_t rtmp_handshake_input(zms_rtmp_protocol *p, const uint8_t *data, size_t len,
                               size_t *consumed);
void rtmp_handshake_make_s1(uint8_t *s1);
/** 握手完成后释放 c1/s1/s2/hs_buf（可重复调用） */
void rtmp_handshake_release(zms_rtmp_protocol *p);

ztk_err_t rtmp_chunk_parse(zms_rtmp_protocol *p, const uint8_t *data, size_t len, size_t *consumed);
void rtmp_chunk_cs_reset_all(zms_rtmp_protocol *p);

ztk_err_t rtmp_chunk_send(zms_rtmp_protocol *p, uint8_t type_id, uint32_t stream_id,
                          uint32_t tag_dts_ms, const void *body, size_t len, uint8_t *out,
                          size_t cap, size_t *out_len);

ztk_err_t rtmp_control_send_server_init(zms_rtmp_protocol *p, uint8_t *out, size_t cap,
                                        size_t *out_len);

#endif /* ZMS_SRC_SESSION_RTMP_PROTOCOL_INTERNAL_H */
