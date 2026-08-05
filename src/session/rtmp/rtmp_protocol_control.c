#include "session/rtmp/rtmp_protocol_internal.h"

ztk_err_t rtmp_control_send_server_init(zms_rtmp_protocol *p, uint8_t *out, size_t cap,
                                        size_t *out_len)
{
    if (!p || !out || !out_len) {
        return ZTK_ERR_INVALID;
    }
    size_t pos = 0;
    size_t n = 0;
    uint8_t body[8];

    rtmp_write_be32(body, 4096);
    if (rtmp_chunk_send(p, ZMS_RTMP_MSG_SET_CHUNK, 0, 0, body, 4, out + pos, cap - pos, &n) !=
        ZTK_OK) {
        return ZTK_ERR_NOMEM;
    }
    pos += n;
    p->out_chunk_size = 4096;

    rtmp_write_be32(body, 5000000);
    if (rtmp_chunk_send(p, ZMS_RTMP_MSG_WIN_SIZE, 0, 0, body, 4, out + pos, cap - pos, &n) !=
        ZTK_OK) {
        return ZTK_ERR_NOMEM;
    }
    pos += n;

    body[0] = 2;
    rtmp_write_be32(body + 1, 5000000);
    if (rtmp_chunk_send(p, ZMS_RTMP_MSG_SET_PEER_BW, 0, 0, body, 5, out + pos, cap - pos, &n) !=
        ZTK_OK) {
        return ZTK_ERR_NOMEM;
    }
    pos += n;

    body[0] = 0;
    body[1] = 0;
    rtmp_write_be32(body + 2, 0);
    if (rtmp_chunk_send(p, ZMS_RTMP_MSG_USER_CONTROL, 0, 0, body, 6, out + pos, cap - pos, &n) !=
        ZTK_OK) {
        return ZTK_ERR_NOMEM;
    }
    pos += n;

    *out_len = pos;
    return ZTK_OK;
}
