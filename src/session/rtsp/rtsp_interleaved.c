#include "zms/media/container/container_dispatcher.h"
#include "rtp-over-rtsp.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    zms_container_demux_opts cfg;
    struct rtp_over_rtsp_t rtp;
} rtsp_ic_ctx;

static void rtsp_ic_onrtp(void *param, uint8_t channel, const void *data, uint16_t bytes)
{
    rtsp_ic_ctx *c = (rtsp_ic_ctx *)param;
    zms_container_packet pkt;

    if (!c || !c->cfg.on_packet || !data || bytes == 0) {
        return;
    }
    memset(&pkt, 0, sizeof(pkt));
    pkt.kind = ZMS_CONTAINER_PKT_RTP;
    pkt.channel = channel;
    pkt.data = (const uint8_t *)data;
    pkt.len = bytes;
    c->cfg.on_packet(&pkt, c->cfg.user);
}

static void *rtsp_ic_create(const zms_container_demux_opts *opts)
{
    rtsp_ic_ctx *c;

    if (!opts || !opts->on_packet) {
        return NULL;
    }
    c = (rtsp_ic_ctx *)calloc(1, sizeof(*c));
    if (!c) {
        return NULL;
    }
    c->cfg = *opts;
    memset(&c->rtp, 0, sizeof(c->rtp));
    c->rtp.onrtp = rtsp_ic_onrtp;
    c->rtp.param = c;
    return c;
}

static void rtsp_ic_destroy(void *ctx)
{
    rtsp_ic_ctx *c = (rtsp_ic_ctx *)ctx;
    if (!c) {
        return;
    }
    free(c->rtp.data);
    free(c);
}

static ztk_err_t rtsp_ic_feed(void *ctx, const uint8_t *buf, size_t len)
{
    rtsp_ic_ctx *c = (rtsp_ic_ctx *)ctx;
    const uint8_t *p;
    const uint8_t *end;

    if (!c || (!buf && len)) {
        return ZTK_ERR_INVALID;
    }
    if (len == 0) {
        return ZTK_OK;
    }

    /* librtsp 状态机跨调用续跑；空闲时跳过前导垃圾字节。 */
    if (c->rtp.state == 0 && buf[0] != '$') {
        const uint8_t *mark = (const uint8_t *)memchr(buf, '$', len);
        if (!mark) {
            return ZTK_OK;
        }
        buf = mark;
        len -= (size_t)(mark - (const uint8_t *)buf);
    }

    end = buf + len;
    p = rtp_over_rtsp(&c->rtp, buf, end);
    (void)p;
    return ZTK_OK;
}

static ztk_err_t rtsp_ic_input_tag(void *ctx, uint8_t type_id, const uint8_t *body, size_t len,
                                   uint32_t tag_dts_ms)
{
    (void)ctx;
    (void)type_id;
    (void)body;
    (void)len;
    (void)tag_dts_ms;
    return ZTK_ERR_NOT_IMPL;
}

const zms_container_demuxer_ops zms_container_rtsp_interleaved_ops = {
    ZMS_CONTAINER_RTSP_INTERLEAVED,
    "rtsp-interleaved",
    rtsp_ic_create,
    rtsp_ic_destroy,
    rtsp_ic_feed,
    rtsp_ic_input_tag,
};
