#include "session/rtmp/rtmp_protocol_internal.h"
#include "session/rtmp/rtmp_handshake.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void hs_random(uint8_t *out, size_t len)
{
    static const char pat[] = "simple-rtmp-server-ZMS";
    for (size_t i = 0; i < len; ++i) {
        out[i] = (uint8_t)pat[i % (sizeof(pat) - 1)] ^ (uint8_t)(rand() & 0xff);
    }
}

void rtmp_handshake_make_s1(uint8_t *s1)
{
    uint32_t t = (uint32_t)time(NULL);
    s1[0] = (uint8_t)((t >> 24) & 0xff);
    s1[1] = (uint8_t)((t >> 16) & 0xff);
    s1[2] = (uint8_t)((t >> 8) & 0xff);
    s1[3] = (uint8_t)(t & 0xff);
    memset(s1 + 4, 0, 4);
    hs_random(s1 + 8, ZMS_RTMP_HS_BODY - 8);
}

void rtmp_handshake_release(zms_rtmp_protocol *p)
{
    if (!p) {
        return;
    }
    free(p->c1);
    free(p->s1);
    free(p->s2);
    free(p->hs_buf);
    p->c1 = NULL;
    p->s1 = NULL;
    p->s2 = NULL;
    p->hs_buf = NULL;
}

static ztk_err_t hs_ensure(zms_rtmp_protocol *p)
{
    if (p->c1 && p->s1 && p->s2 && p->hs_buf) {
        return ZTK_OK;
    }
    if (!p->c1) {
        p->c1 = (uint8_t *)malloc(ZMS_RTMP_HS_BODY);
    }
    if (!p->s1) {
        p->s1 = (uint8_t *)malloc(ZMS_RTMP_HS_BODY);
    }
    if (!p->s2) {
        p->s2 = (uint8_t *)malloc(ZMS_RTMP_HS_BODY);
    }
    if (!p->hs_buf) {
        p->hs_buf = (uint8_t *)malloc(1 + ZMS_RTMP_HS_BODY);
    }
    if (!p->c1 || !p->s1 || !p->s2 || !p->hs_buf) {
        rtmp_handshake_release(p);
        return ZTK_ERR_NOMEM;
    }
    return ZTK_OK;
}

ztk_err_t rtmp_handshake_input(zms_rtmp_protocol *p, const uint8_t *data, size_t len,
                               size_t *consumed)
{
    size_t off = 0;
    if (!p || !data) {
        return ZTK_ERR_INVALID;
    }
    if (p->hs_state == ZMS_RTMP_HS_STATE_DONE) {
        if (consumed) {
            *consumed = 0;
        }
        return ZTK_OK;
    }

    if (hs_ensure(p) != ZTK_OK) {
        return ZTK_ERR_NOMEM;
    }

    while (off < len) {
        size_t need = 0;
        if (p->hs_state == ZMS_RTMP_HS_STATE_WAIT_C0C1) {
            need = 1 + ZMS_RTMP_HS_BODY;
        } else if (p->hs_state == ZMS_RTMP_HS_STATE_WAIT_C2) {
            need = ZMS_RTMP_HS_BODY;
        }

        size_t copy = need - p->hs_off;
        if (copy > len - off) {
            copy = len - off;
        }
        memcpy(p->hs_buf + p->hs_off, data + off, copy);
        p->hs_off += copy;
        off += copy;

        if (p->hs_state == ZMS_RTMP_HS_STATE_WAIT_C0C1 && p->hs_off >= need) {
            memcpy(p->c1, p->hs_buf + 1, ZMS_RTMP_HS_BODY);
            p->hs_complex = zms_rtmp_hs_detect_complex(p->c1);
            if (p->hs_complex && !zms_rtmp_hs_build_complex(p->c1, p->s1, p->s2)) {
                p->hs_complex = 0;
            }
            p->hs_state = ZMS_RTMP_HS_STATE_WAIT_C2;
            p->hs_off = 0;
            p->hs_need_reply = 1;
        } else if (p->hs_state == ZMS_RTMP_HS_STATE_WAIT_C2 && p->hs_off >= need) {
            if (p->hs_complex) {
                (void)zms_rtmp_hs_validate_c2(p->s1, p->hs_buf);
            } else {
                (void)memcmp(p->hs_buf, p->s1, ZMS_RTMP_HS_BODY);
            }
            p->hs_state = ZMS_RTMP_HS_STATE_DONE;
            p->hs_off = 0;
            p->hs_init_pending = 1;
            /* C2 已校验完，握手缓冲可释放（S0S1S2 已在 need_reply 时发出） */
            rtmp_handshake_release(p);
        }
    }

    if (consumed) {
        *consumed = off;
    }
    return ZTK_OK;
}
