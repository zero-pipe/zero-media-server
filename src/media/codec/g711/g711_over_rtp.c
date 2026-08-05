#include "zms/media/codec/g711/g711_over_rtp.h"
#include "zms/media/codec/payload/payload_registry.h"
#include "zms/engine/media_clock.h"
#include "rtp-demuxer.h"
#include "rtp-profile.h"
#include <stdlib.h>
#include <string.h>

uint8_t zms_g711_over_rtp_default_pt(zms_codec_id codec)
{
    if (codec == ZMS_CODEC_G711U) {
        return RTP_PAYLOAD_PCMU;
    }
    return RTP_PAYLOAD_PCMA;
}

zms_codec_id zms_g711_codec_from_rtp_pt(uint8_t pt)
{
    if (pt == RTP_PAYLOAD_PCMU) {
        return ZMS_CODEC_G711U;
    }
    if (pt == RTP_PAYLOAD_PCMA) {
        return ZMS_CODEC_G711A;
    }
    return ZMS_CODEC_INVALID;
}

ztk_err_t zms_g711_over_rtp_unpack_payload(const uint8_t *payload, size_t len, const uint8_t **g711,
                                           size_t *g711_len)
{
    if (!payload || len == 0 || !g711 || !g711_len) {
        return ZTK_ERR_INVALID;
    }
    *g711 = payload;
    *g711_len = len;
    return ZTK_OK;
}

typedef struct {
    zms_payload_demux_opts payload_opts;
    zms_codec_id codec;
    struct rtp_demuxer_t *demuxer;
} zms_g711_over_rtp_demux_ctx;

static int g711_over_rtp_onpacket(void *param, const void *packet, int bytes, uint32_t timestamp,
                                  int flags)
{
    zms_g711_over_rtp_demux_ctx *ctx = (zms_g711_over_rtp_demux_ctx *)param;
    const uint8_t *g711 = NULL;
    size_t g711_len = 0;
    zms_frame frame;
    uint32_t hz;

    (void)flags;
    if (!ctx || !packet || bytes <= 0) {
        return 0;
    }
    if (zms_g711_over_rtp_unpack_payload((const uint8_t *)packet, (size_t)bytes, &g711,
                                         &g711_len) != ZTK_OK ||
        !g711) {
        return 0;
    }
    hz = ctx->payload_opts.rtp_clock_hz > 0 ? ctx->payload_opts.rtp_clock_hz : 8000u;
    zms_frame_init(&frame);
    frame.data = (uint8_t *)g711;
    frame.size = g711_len;
    frame.codec = ctx->codec;
    frame.track = ZMS_TRACK_AUDIO;
    frame.dts_ms = frame.pts_ms = zms_rtp_clock_to_ms(timestamp, hz);
    if (ctx->payload_opts.on_frame) {
        ctx->payload_opts.on_frame(&frame, ctx->payload_opts.user);
    }
    return 0;
}

static void *g711_over_rtp_demux_create(const zms_payload_demux_opts *opts)
{
    zms_g711_over_rtp_demux_ctx *ctx;
    const char *encoding;
    int pt;
    int rate;

    if (!opts || !opts->on_frame) {
        return NULL;
    }
    if (opts->codec != ZMS_CODEC_G711A && opts->codec != ZMS_CODEC_G711U) {
        return NULL;
    }
    ctx = (zms_g711_over_rtp_demux_ctx *)calloc(1, sizeof(*ctx));
    if (!ctx) {
        return NULL;
    }
    ctx->payload_opts = *opts;
    ctx->codec = opts->codec;
    rate = opts->rtp_clock_hz > 0 ? (int)opts->rtp_clock_hz : 8000;
    if (opts->codec == ZMS_CODEC_G711U) {
        encoding = "PCMU";
        pt = 0;
    } else {
        encoding = "PCMA";
        pt = 8;
    }
    ctx->demuxer = rtp_demuxer_create(200, rate, pt, encoding, g711_over_rtp_onpacket, ctx);
    if (!ctx->demuxer) {
        free(ctx);
        return NULL;
    }
    return ctx;
}

static void g711_over_rtp_demux_destroy(void *ctx)
{
    zms_g711_over_rtp_demux_ctx *demux = (zms_g711_over_rtp_demux_ctx *)ctx;
    if (!demux) {
        return;
    }
    rtp_demuxer_destroy(&demux->demuxer);
    free(demux);
}

static ztk_err_t g711_over_rtp_demux_input_rtp(void *ctx, const zms_rtp_packet *pkt, zms_frame *out)
{
    zms_g711_over_rtp_demux_ctx *demux = (zms_g711_over_rtp_demux_ctx *)ctx;
    (void)out;
    if (!demux || !demux->demuxer || !pkt || !pkt->data || pkt->size < 12) {
        return ZTK_ERR_INVALID;
    }
    return rtp_demuxer_input(demux->demuxer, pkt->data, (int)pkt->size) >= 0 ? ZTK_OK
                                                                             : ZTK_ERR_INVALID;
}

const zms_payload_demux_ops zms_g711a_over_rtp_demux_ops = {
    ZMS_CODEC_G711A,
    ZMS_WIRE_FORMAT_RTP,
    "g711a_over_rtp",
    g711_over_rtp_demux_create,
    g711_over_rtp_demux_destroy,
    g711_over_rtp_demux_input_rtp,
};

const zms_payload_demux_ops zms_g711u_over_rtp_demux_ops = {
    ZMS_CODEC_G711U,
    ZMS_WIRE_FORMAT_RTP,
    "g711u_over_rtp",
    g711_over_rtp_demux_create,
    g711_over_rtp_demux_destroy,
    g711_over_rtp_demux_input_rtp,
};
