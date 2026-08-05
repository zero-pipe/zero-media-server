#include "zms/session/rtmp/rtmp_protocol.h"
#include "session/rtmp/rtmp_protocol_internal.h"
#include "zms/util/buf_pool.h"
#include "ztk/util/log.h"
#include <stdlib.h>
#include <string.h>

zms_rtmp_protocol *zms_rtmp_protocol_create(const zms_rtmp_protocol_opts *opts)
{
    zms_rtmp_protocol *p = (zms_rtmp_protocol *)calloc(1, sizeof(*p));
    if (!p) {
        return NULL;
    }
    if (opts) {
        p->opts = *opts;
    }
    p->in_chunk_size = ZMS_RTMP_DEFAULT_CHUNK;
    p->out_chunk_size = ZMS_RTMP_DEFAULT_CHUNK;
    p->hs_state = ZMS_RTMP_HS_STATE_WAIT_C0C1;
    return p;
}

zms_rtmp_protocol *zms_rtmp_protocol_create_established(const zms_rtmp_protocol_opts *opts)
{
    zms_rtmp_protocol *p = zms_rtmp_protocol_create(opts);
    if (!p) {
        return NULL;
    }
    p->hs_state = ZMS_RTMP_HS_STATE_DONE;
    p->hs_off = 0;
    p->hs_need_reply = 0;
    p->hs_init_pending = 0;
    return p;
}

static void rtmp_proto_slot_clear(zms_rtmp_protocol *p, uint8_t **data, size_t *cap)
{
    if (!data) {
        return;
    }
    if (p && p->io_poller) {
        zms_buf_pool_slot_clear_poller(data, cap, p->io_poller);
    } else {
        zms_buf_pool_slot_clear(data, cap);
    }
}

void zms_rtmp_protocol_set_poller(zms_rtmp_protocol *p, struct ztk_poller *poller)
{
    if (!p) {
        return;
    }
    p->io_poller = poller;
}

void zms_rtmp_protocol_destroy(zms_rtmp_protocol *p)
{
    int i;

    if (!p) {
        return;
    }
    for (i = 0; i < ZMS_RTMP_MAX_CS; ++i) {
        if (p->cs[i]) {
            rtmp_proto_slot_clear(p, &p->cs[i]->body, &p->cs[i]->body_cap);
            free(p->cs[i]);
            p->cs[i] = NULL;
        }
    }
    rtmp_proto_slot_clear(p, &p->rx_buf, &p->rx_cap);
    rtmp_handshake_release(p);
    free(p);
}

ztk_err_t zms_rtmp_protocol_send_handshake_s0s1s2(zms_rtmp_protocol *p, uint8_t *out, size_t cap,
                                                  size_t *out_len)
{
    if (!p || !out || cap < 1 + ZMS_RTMP_HS_BODY * 2) {
        return ZTK_ERR_INVALID;
    }
    if (!p->s1 || (!p->hs_complex && !p->c1) || (p->hs_complex && !p->s2)) {
        return ZTK_ERR_STATE;
    }
    if (!p->hs_complex) {
        rtmp_handshake_make_s1(p->s1);
    }
    out[0] = 3;
    memcpy(out + 1, p->s1, ZMS_RTMP_HS_BODY);
    memcpy(out + 1 + ZMS_RTMP_HS_BODY, p->hs_complex ? p->s2 : p->c1, ZMS_RTMP_HS_BODY);
    if (out_len) {
        *out_len = 1 + ZMS_RTMP_HS_BODY * 2;
    }
    return ZTK_OK;
}

ztk_err_t zms_rtmp_protocol_input(zms_rtmp_protocol *p, const void *data, size_t len)
{
    if (!p || !data) {
        return ZTK_ERR_INVALID;
    }
    const uint8_t *d = (const uint8_t *)data;
    size_t off = 0;

    if (p->hs_state != ZMS_RTMP_HS_STATE_DONE) {
        size_t hs_used = 0;
        ztk_err_t err = rtmp_handshake_input(p, d, len, &hs_used);
        if (err != ZTK_OK) {
            return err;
        }
        off = hs_used;
        if (p->hs_state != ZMS_RTMP_HS_STATE_DONE) {
            return ZTK_OK;
        }
    }

    if (off < len) {
        if (rtmp_rx_append(p, d + off, len - off) != ZTK_OK) {
            return ZTK_ERR_NOMEM;
        }
    }
    while (p->rx_len > 0) {
        size_t used = 0;
        ztk_err_t err = rtmp_chunk_parse(p, p->rx_buf, p->rx_len, &used);
        if (err != ZTK_OK) {
            ztk_warn("RTMP parse_chunks failed err=%d rx_len=%u used=%u", (int)err,
                     (unsigned)p->rx_len, (unsigned)used);
            rtmp_chunk_cs_reset_all(p);
            p->rx_len = 0;
            return ZTK_OK;
        }
        if (used == 0) {
            break;
        }
        rtmp_rx_consume(p, used);
    }
    return ZTK_OK;
}

ztk_err_t zms_rtmp_protocol_send_chunk(zms_rtmp_protocol *p, uint8_t type_id, uint32_t stream_id,
                                       uint32_t tag_dts_ms, const void *body, size_t len,
                                       uint8_t *out, size_t cap, size_t *out_len)
{
    return rtmp_chunk_send(p, type_id, stream_id, tag_dts_ms, body, len, out, cap, out_len);
}

int zms_rtmp_protocol_handshake_pending(const zms_rtmp_protocol *p)
{
    if (!p) {
        return 0;
    }
    if (p->hs_need_reply) {
        ((zms_rtmp_protocol *)p)->hs_need_reply = 0;
        return 1;
    }
    return 0;
}

int zms_rtmp_protocol_handshake_complete(const zms_rtmp_protocol *p)
{
    if (!p) {
        return 0;
    }
    if (p->hs_init_pending) {
        ((zms_rtmp_protocol *)p)->hs_init_pending = 0;
        return 1;
    }
    return 0;
}

ztk_err_t zms_rtmp_protocol_send_server_init(zms_rtmp_protocol *p, uint8_t *out, size_t cap,
                                             size_t *out_len)
{
    return rtmp_control_send_server_init(p, out, cap, out_len);
}

void zms_rtmp_protocol_set_out_chunk_size(zms_rtmp_protocol *p, uint32_t size)
{
    if (!p) {
        return;
    }
    p->out_chunk_size = size ? size : ZMS_RTMP_DEFAULT_CHUNK;
}

ztk_err_t zms_rtmp_protocol_send_invoke(zms_rtmp_protocol *p, const char *cmd, double trans_id,
                                        uint32_t msg_stream_id, const uint8_t *amf_body,
                                        size_t amf_len, uint8_t *out, size_t cap, size_t *out_len)
{
    if (!p || !cmd || !out || !out_len) {
        return ZTK_ERR_INVALID;
    }
    uint8_t buf[4096];
    size_t pos = 0;
    pos += zms_amf_encode_string(buf + pos, sizeof(buf) - pos, cmd);
    pos += zms_amf_encode_number(buf + pos, sizeof(buf) - pos, trans_id);
    pos += zms_amf_encode_null(buf + pos, sizeof(buf) - pos);
    if (amf_body && amf_len) {
        if (pos + amf_len > sizeof(buf)) {
            return ZTK_ERR_NOMEM;
        }
        memcpy(buf + pos, amf_body, amf_len);
        pos += amf_len;
    }
    return zms_rtmp_protocol_send_chunk(p, ZMS_RTMP_MSG_CMD, msg_stream_id, 0, buf, pos, out, cap,
                                        out_len);
}
