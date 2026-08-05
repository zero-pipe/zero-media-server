#include "session/rtmp/rtmp_protocol_internal.h"
#include "zms/util/buf_pool.h"
#include "ztk/util/log.h"
#include <stdlib.h>
#include <string.h>

static zms_rtmp_cs_state *get_cs(zms_rtmp_protocol *p, uint32_t csid)
{
    zms_rtmp_cs_state *c;

    if (csid >= ZMS_RTMP_MAX_CS) {
        return NULL;
    }
    c = p->cs[csid];
    if (c) {
        return c;
    }
    c = (zms_rtmp_cs_state *)calloc(1, sizeof(*c));
    if (!c) {
        return NULL;
    }
    c->csid = csid;
    p->cs[csid] = c;
    return c;
}

void rtmp_chunk_cs_reset_all(zms_rtmp_protocol *p)
{
    int i;

    if (!p) {
        return;
    }
    for (i = 0; i < (int)ZMS_RTMP_MAX_CS; ++i) {
        if (p->cs[i]) {
            p->cs[i]->msg_len = 0;
            p->cs[i]->body_off = 0;
        }
    }
}

static ztk_err_t ensure_body(zms_rtmp_protocol *p, zms_rtmp_cs_state *c, size_t need)
{
    size_t cap;

    if (c->body_cap >= need) {
        return ZTK_OK;
    }
    cap = need < 256 ? 256 : need;
    if (p && p->io_poller) {
        if (!zms_buf_pool_slot_resize_poller(&c->body, &c->body_cap, cap, p->io_poller)) {
            return ZTK_ERR_NOMEM;
        }
    } else if (!zms_buf_pool_slot_resize(&c->body, &c->body_cap, cap)) {
        return ZTK_ERR_NOMEM;
    }
    return ZTK_OK;
}

static void dispatch_msg(zms_rtmp_protocol *p, zms_rtmp_cs_state *c)
{
    if (c->body_off < c->msg_len) {
        return;
    }

    if (c->type_id == ZMS_RTMP_MSG_VIDEO || c->type_id == ZMS_RTMP_MSG_AUDIO) {
        static unsigned disp_cnt;
        if (disp_cnt < 100) {
            ztk_info("RTMP dispatch csid=%u type=%u len=%u tag_dts_ms=%u", (unsigned)c->csid,
                     (unsigned)c->type_id, (unsigned)c->msg_len, (unsigned)c->tag_dts_ms);
        }
        ++disp_cnt;
    }

    if (c->type_id == ZMS_RTMP_MSG_SET_CHUNK && c->msg_len >= 4) {
        p->in_chunk_size = rtmp_read_be32(c->body);
        if (p->in_chunk_size == 0) {
            p->in_chunk_size = ZMS_RTMP_DEFAULT_CHUNK;
        }
        ztk_info("RTMP peer SetChunkSize=%u", (unsigned)p->in_chunk_size);
    }

    zms_rtmp_chunk chunk = {
        .type_id = c->type_id,
        .stream_id = c->stream_id,
        .tag_dts_ms = c->tag_dts_ms,
        .body = c->body,
        .body_size = c->msg_len,
    };
    if (c->type_id == ZMS_RTMP_MSG_CMD && p->opts.on_cmd && c->body && c->msg_len > 0) {
        zms_amf_value *cmd = NULL;
        size_t off = 0;
        if (zms_amf_decode(c->body, c->msg_len, &cmd, &off) == ZTK_OK && cmd &&
            zms_amf_value_type(cmd) == ZMS_AMF_STRING) {
            static unsigned cmd_cnt;
            if (cmd_cnt < 20) {
                ztk_info("RTMP cmd csid=%u len=%u: %s", (unsigned)c->csid, (unsigned)c->msg_len,
                         zms_amf_value_str(cmd));
            }
            ++cmd_cnt;
            double trans = 0;
            if (off < c->msg_len) {
                zms_amf_value *tid = NULL;
                size_t tid_len = 0;
                if (zms_amf_decode(c->body + off, c->msg_len - off, &tid, &tid_len) == ZTK_OK &&
                    tid) {
                    if (zms_amf_value_type(tid) == ZMS_AMF_NUMBER) {
                        trans = zms_amf_value_num(tid);
                    }
                    zms_amf_value_free(tid);
                    off += tid_len;
                }
            }
            p->opts.on_cmd(zms_amf_value_str(cmd), trans, c->body + off, c->msg_len - off,
                           p->opts.user);
        }
        zms_amf_value_free(cmd);
    } else if (p->opts.on_chunk) {
        p->opts.on_chunk(&chunk, p->opts.user);
    }
    c->last_msg_len = c->msg_len;
    c->last_type_id = c->type_id;
    c->last_stream_id = c->stream_id;
    c->last_tag_dts_ms = c->tag_dts_ms;
    c->last_tag_dts_field = c->tag_dts_field;
    c->body_off = 0;
    c->msg_len = 0;
}

static void cs_restore_from_last(zms_rtmp_cs_state *c)
{
    if (c->last_msg_len == 0) {
        return;
    }
    c->msg_len = c->last_msg_len;
    c->type_id = c->last_type_id;
    c->stream_id = c->last_stream_id;
    c->tag_dts_ms = c->last_tag_dts_ms;
    c->tag_dts_field = c->last_tag_dts_field;
    c->body_off = 0;
}

ztk_err_t rtmp_chunk_parse(zms_rtmp_protocol *p, const uint8_t *data, size_t len, size_t *consumed)
{
    static const size_t HEADER_LENGTH[] = {12, 8, 4, 1};
    const uint8_t *ptr = data;
    const uint8_t *end = data + len;

    while (ptr < end) {
        if (ptr + 1 > end) {
            break;
        }

        uint8_t b0 = *ptr;
        uint8_t fmt = b0 >> 6;
        uint32_t csid = b0 & 0x3f;
        size_t offset = 0;

        if (csid == 0) {
            if (ptr + 2 > end) {
                break;
            }
            csid = 64 + (uint32_t)ptr[1];
            offset = 1;
        } else if (csid == 1) {
            if (ptr + 3 > end) {
                break;
            }
            csid = 64 + (uint32_t)ptr[2] + ((uint32_t)ptr[1] << 8);
            offset = 2;
        }

        if (fmt > 3) {
            break;
        }

        size_t header_len = HEADER_LENGTH[fmt];
        if ((size_t)(end - ptr) < header_len + offset) {
            break;
        }

        zms_rtmp_cs_state *c = get_cs(p, csid);
        if (!c) {
            break;
        }
        c->csid = csid;

        if (c->body_off == 0 && c->msg_len == 0) {
            cs_restore_from_last(c);
            c->is_abs_stamp = 0;
        }

        const uint8_t *h = ptr + offset;
        switch (header_len) {
        case 12:
            c->is_abs_stamp = 1;
            c->stream_id = rtmp_read_le32(h + 8);
            /* fallthrough */
        case 8:
            c->msg_len = rtmp_read_be24(h + 4);
            c->type_id = h[7];
            c->body_off = 0;
            /* fallthrough */
        case 4:
            c->tag_dts_field = rtmp_read_be24(h + 1);
            break;
        default:
            break;
        }

        uint32_t header_tag_dts = c->tag_dts_field;
        if (c->tag_dts_field == 0xffffff) {
            if ((size_t)(end - ptr) < header_len + offset + 4) {
                break;
            }
            header_tag_dts = rtmp_read_be32(ptr + offset + header_len);
            offset += 4;
        }

        if (c->msg_len < c->body_off) {
            return ZTK_ERR_INVALID;
        }

        size_t more = c->msg_len - c->body_off;
        if (more > p->in_chunk_size) {
            more = p->in_chunk_size;
        }

        if ((size_t)(end - ptr) < header_len + offset + more) {
            break;
        }

        if (more > 0) {
            if (ensure_body(p, c, c->body_off + more) != ZTK_OK) {
                return ZTK_ERR_NOMEM;
            }
            memcpy(c->body + c->body_off, ptr + offset + header_len, more);
            c->body_off += more;
        }

        ptr += offset + header_len + more;

        if (c->body_off >= c->msg_len) {
            c->tag_dts_ms = header_tag_dts + (c->is_abs_stamp ? 0 : c->tag_dts_ms);
            dispatch_msg(p, c);
        }
    }

    if (consumed) {
        *consumed = (size_t)(ptr - data);
    }
    return ZTK_OK;
}
