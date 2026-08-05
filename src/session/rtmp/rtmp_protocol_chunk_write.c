#include "session/rtmp/rtmp_protocol_internal.h"
#include <string.h>

static size_t write_basic(uint8_t *out, size_t cap, uint8_t fmt, uint32_t csid)
{
    if (csid < 64) {
        if (cap < 1) {
            return 0;
        }
        out[0] = (fmt << 6) | (uint8_t)csid;
        return 1;
    }
    if (csid < 320) {
        if (cap < 2) {
            return 0;
        }
        out[0] = (fmt << 6);
        out[1] = (uint8_t)(csid - 64);
        return 2;
    }
    if (cap < 3) {
        return 0;
    }
    uint32_t v = csid - 64;
    out[0] = (fmt << 6) | 1;
    out[1] = (uint8_t)(v & 0xff);
    out[2] = (uint8_t)((v >> 8) & 0xff);
    return 3;
}

ztk_err_t rtmp_chunk_send(zms_rtmp_protocol *p, uint8_t type_id, uint32_t stream_id,
                          uint32_t tag_dts_ms, const void *body, size_t len, uint8_t *out,
                          size_t cap, size_t *out_len)
{
    if (!p || !out || !out_len) {
        return ZTK_ERR_INVALID;
    }
    uint32_t csid = (type_id == ZMS_RTMP_MSG_CMD || type_id == ZMS_RTMP_MSG_WIN_SIZE ||
                     type_id == ZMS_RTMP_MSG_SET_PEER_BW || type_id == ZMS_RTMP_MSG_USER_CONTROL ||
                     type_id == ZMS_RTMP_MSG_SET_CHUNK)
                        ? ZMS_RTMP_CHUNK_SYSTEM
                        : ZMS_RTMP_CHUNK_MEDIA;
    size_t chunk_sz = p->out_chunk_size ? p->out_chunk_size : ZMS_RTMP_DEFAULT_CHUNK;
    size_t pos = 0;
    const uint8_t *b = (const uint8_t *)body;
    size_t left = len;
    int first = 1;
    int use_ext = tag_dts_ms >= 0xffffff;
    uint32_t hdr_tag_dts = use_ext ? 0xffffffu : tag_dts_ms;
    while (left > 0 || (first && len == 0)) {
        size_t hdr = write_basic(out + pos, cap - pos, first ? 0 : 3, csid);
        if (hdr == 0) {
            return ZTK_ERR_NOMEM;
        }
        pos += hdr;
        if (first) {
            if (pos + 11 > cap) {
                return ZTK_ERR_NOMEM;
            }
            rtmp_write_be24(out + pos, hdr_tag_dts);
            rtmp_write_be24(out + pos + 3, (uint32_t)len);
            out[pos + 6] = type_id;
            rtmp_write_be32(out + pos + 7, stream_id);
            pos += 11;
            if (use_ext) {
                if (pos + 4 > cap) {
                    return ZTK_ERR_NOMEM;
                }
                rtmp_write_be32(out + pos, tag_dts_ms);
                pos += 4;
            }
            first = 0;
        } else if (use_ext) {
            if (pos + 4 > cap) {
                return ZTK_ERR_NOMEM;
            }
            rtmp_write_be32(out + pos, tag_dts_ms);
            pos += 4;
        }
        size_t chunk = left < chunk_sz ? left : chunk_sz;
        if (pos + chunk > cap) {
            return ZTK_ERR_NOMEM;
        }
        if (chunk) {
            memcpy(out + pos, b + (len - left), chunk);
        }
        pos += chunk;
        left -= chunk;
        if (left == 0) {
            break;
        }
    }
    *out_len = pos;
    return ZTK_OK;
}
