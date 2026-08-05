#include "zms/media/container/container_dispatcher.h"
#include "zms/media/container/flv/flv_tag_framer.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    zms_container_demux_opts cfg;
    zms_flv_tag_framer *flv;
} flv_tag_ctx;

static void flv_tag_on_byte(uint8_t type, const uint8_t *body, size_t len, uint32_t tag_dts_ms,
                            void *user)
{
    flv_tag_ctx *c = (flv_tag_ctx *)user;
    zms_container_packet pkt;

    if (!c || !body || len == 0) {
        return;
    }
    memset(&pkt, 0, sizeof(pkt));
    pkt.kind = ZMS_CONTAINER_PKT_FLV_TAG;
    pkt.flv_type_id = type;
    pkt.tag_dts_ms = tag_dts_ms;
    pkt.data = body;
    pkt.len = len;
    if (c->cfg.on_packet) {
        c->cfg.on_packet(&pkt, c->cfg.user);
    }
}

static void *flv_tag_create(const zms_container_demux_opts *opts)
{
    flv_tag_ctx *c;

    if (!opts || !opts->on_packet) {
        return NULL;
    }
    c = (flv_tag_ctx *)calloc(1, sizeof(*c));
    if (!c) {
        return NULL;
    }
    c->cfg = *opts;
    c->flv = zms_flv_tag_framer_create();
    if (!c->flv) {
        free(c);
        return NULL;
    }
    return c;
}

static void flv_tag_destroy(void *ctx)
{
    flv_tag_ctx *c = (flv_tag_ctx *)ctx;
    if (!c) {
        return;
    }
    zms_flv_tag_framer_destroy(c->flv);
    free(c);
}

static ztk_err_t flv_tag_feed(void *ctx, const uint8_t *buf, size_t len)
{
    flv_tag_ctx *c = (flv_tag_ctx *)ctx;
    if (!c || !c->flv || !buf || len == 0) {
        return ZTK_ERR_INVALID;
    }
    return zms_flv_tag_framer_feed(c->flv, buf, len, flv_tag_on_byte, c);
}

static ztk_err_t flv_tag_input_tag(void *ctx, uint8_t type_id, const uint8_t *body, size_t len,
                                   uint32_t tag_dts_ms)
{
    flv_tag_ctx *c = (flv_tag_ctx *)ctx;
    zms_container_packet pkt;

    if (!c || !body || len == 0) {
        return ZTK_ERR_INVALID;
    }
    memset(&pkt, 0, sizeof(pkt));
    pkt.kind = ZMS_CONTAINER_PKT_FLV_TAG;
    pkt.flv_type_id = type_id;
    pkt.tag_dts_ms = tag_dts_ms;
    pkt.data = body;
    pkt.len = len;
    if (c->cfg.on_packet) {
        c->cfg.on_packet(&pkt, c->cfg.user);
    }
    return ZTK_OK;
}

const zms_container_demuxer_ops zms_container_flv_tag_ops = {
    ZMS_CONTAINER_FLV_TAG, "flv-tag",    flv_tag_create,
    flv_tag_destroy,       flv_tag_feed, flv_tag_input_tag,
};
