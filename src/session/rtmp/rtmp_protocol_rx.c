#include "session/rtmp/rtmp_protocol_internal.h"
#include "zms/util/buf_pool.h"
#include <string.h>

ztk_err_t rtmp_rx_append(zms_rtmp_protocol *p, const uint8_t *data, size_t len)
{
    size_t need;
    size_t cap;

    if (!p || len == 0) {
        return ZTK_OK;
    }
    need = p->rx_len + len;
    if (need > p->rx_cap) {
        cap = need < 4096 ? 4096 : need * 2;
        if (p->io_poller) {
            if (!zms_buf_pool_slot_resize_poller(&p->rx_buf, &p->rx_cap, cap, p->io_poller)) {
                return ZTK_ERR_NOMEM;
            }
        } else if (!zms_buf_pool_slot_resize(&p->rx_buf, &p->rx_cap, cap)) {
            return ZTK_ERR_NOMEM;
        }
    }
    memcpy(p->rx_buf + p->rx_len, data, len);
    p->rx_len += len;
    return ZTK_OK;
}

void rtmp_rx_consume(zms_rtmp_protocol *p, size_t n)
{
    if (!p || n == 0) {
        return;
    }
    if (n >= p->rx_len) {
        p->rx_len = 0;
        return;
    }
    memmove(p->rx_buf, p->rx_buf + n, p->rx_len - n);
    p->rx_len -= n;
}
