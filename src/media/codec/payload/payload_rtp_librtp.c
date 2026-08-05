/**
 * librtp rtp_demuxer 通用 payload demux（Opus / VP8 / VP9 / H266） */
#include "zms/media/codec/payload/payload_registry.h"
#include "zms/engine/media_clock.h"
#include "rtp-demuxer.h"
#include "rtp-payload.h"
#include <stdlib.h>
#include <string.h>

struct zms_payload_rtp_librtp_ctx {
    zms_payload_demux_opts opts;
    zms_codec_id codec;
    const char *encoding;
    int default_pt;
    int annexb_out;
    struct rtp_demuxer_t *demuxer;
    zms_frame frame;
};

static int vp8_payload_is_key(const uint8_t *data, int bytes)
{
    if (!data || bytes < 1) {
        return 0;
    }
    return (data[0] & 0x01) == 0;
}

static int vp9_payload_is_key(const uint8_t *data, int bytes)
{
    static const uint8_t sync[] = {0x49, 0x83, 0x42};

    if (!data || bytes < 4) {
        return 0;
    }
    if ((data[0] >> 6) != 0x02) {
        return 0;
    }
    return data[1] == sync[0] && data[2] == sync[1] && data[3] == sync[2];
}

static int librtp_onpacket(void *param, const void *packet, int bytes, uint32_t timestamp,
                           int flags)
{
    struct zms_payload_rtp_librtp_ctx *d = (struct zms_payload_rtp_librtp_ctx *)param;
    uint32_t hz;
    uint32_t pts;
    size_t need;
    const uint8_t *src = (const uint8_t *)packet;

    if (!d || !packet || bytes <= 0) {
        return 0;
    }
    if (flags & RTP_PAYLOAD_FLAG_PACKET_LOST) {
        return 0;
    }

    hz = d->opts.rtp_clock_hz > 0 ? d->opts.rtp_clock_hz : 90000u;
    if (d->codec == ZMS_CODEC_OPUS) {
        hz = d->opts.rtp_clock_hz > 0 ? d->opts.rtp_clock_hz : 48000u;
    }
    pts = zms_rtp_clock_to_ms(timestamp, hz);

    need = (size_t)bytes;
    if (d->annexb_out) {
        need += 4u;
    }

    if (zms_frame_reserve(&d->frame, need) != ZTK_OK) {
        return 0;
    }

    if (d->annexb_out) {
        d->frame.data[0] = 0;
        d->frame.data[1] = 0;
        d->frame.data[2] = 0;
        d->frame.data[3] = 1;
        memcpy(d->frame.data + 4, src, (size_t)bytes);
        d->frame.size = (size_t)bytes + 4u;
    } else {
        memcpy(d->frame.data, src, (size_t)bytes);
        d->frame.size = (size_t)bytes;
    }

    d->frame.codec = d->codec;
    d->frame.track = d->codec == ZMS_CODEC_OPUS ? ZMS_TRACK_AUDIO : ZMS_TRACK_VIDEO;
    d->frame.config_frame = 0;
    if (d->codec == ZMS_CODEC_VP8) {
        d->frame.keyframe = vp8_payload_is_key(d->frame.data, (int)d->frame.size);
    } else if (d->codec == ZMS_CODEC_VP9) {
        d->frame.keyframe = vp9_payload_is_key(d->frame.data, (int)d->frame.size);
    } else {
        d->frame.keyframe = 0;
    }
    d->frame.pts_ms = pts;
    d->frame.dts_ms = pts;
    d->frame.owned = 1;

    if (d->opts.on_frame) {
        d->opts.on_frame(&d->frame, d->opts.user);
    }
    return 0;
}

static void *librtp_demux_create(const zms_payload_demux_opts *opts, zms_codec_id codec,
                                 const char *encoding, int default_pt, int annexb_out)
{
    struct zms_payload_rtp_librtp_ctx *d;
    uint32_t hz;

    if (!opts || !opts->on_frame || !encoding) {
        return NULL;
    }
    d = (struct zms_payload_rtp_librtp_ctx *)calloc(1, sizeof(*d));
    if (!d) {
        return NULL;
    }
    d->opts = *opts;
    d->codec = codec;
    d->encoding = encoding;
    d->default_pt = default_pt;
    d->annexb_out = annexb_out;
    zms_frame_init(&d->frame);

    int pt;

    hz = opts->rtp_clock_hz;
    if (!hz) {
        hz = codec == ZMS_CODEC_OPUS ? 48000u : 90000u;
    }
    pt = opts->payload_type > 0 ? opts->payload_type : default_pt;
    d->demuxer = rtp_demuxer_create(200, (int)hz, pt, encoding, librtp_onpacket, d);
    if (!d->demuxer) {
        free(d);
        return NULL;
    }
    return d;
}

static void librtp_demux_destroy(void *ctx)
{
    struct zms_payload_rtp_librtp_ctx *d = (struct zms_payload_rtp_librtp_ctx *)ctx;
    if (!d) {
        return;
    }
    rtp_demuxer_destroy(&d->demuxer);
    zms_frame_clear(&d->frame);
    free(d);
}

static ztk_err_t librtp_demux_input_rtp(void *ctx, const zms_rtp_packet *pkt, zms_frame *out)
{
    struct zms_payload_rtp_librtp_ctx *d = (struct zms_payload_rtp_librtp_ctx *)ctx;
    (void)out;
    if (!d || !d->demuxer || !pkt || !pkt->data || pkt->size < 12) {
        return ZTK_ERR_INVALID;
    }
    return rtp_demuxer_input(d->demuxer, pkt->data, (int)pkt->size) >= 0 ? ZTK_OK : ZTK_ERR_INVALID;
}

#define LIBRTP_PAYLOAD_DEMUX(VAR, CODEC, ENC, PT, ANNEXB, NAME)                  \
    static void *VAR##_create(const zms_payload_demux_opts *opts)                \
    {                                                                            \
        return librtp_demux_create(opts, CODEC, ENC, PT, ANNEXB);                \
    }                                                                            \
    const zms_payload_demux_ops VAR = {CODEC,        ZMS_WIRE_FORMAT_RTP,  NAME, \
                                       VAR##_create, librtp_demux_destroy, librtp_demux_input_rtp}

LIBRTP_PAYLOAD_DEMUX(zms_opus_over_rtp_demux_ops, ZMS_CODEC_OPUS, "opus", 111, 0, "opus_over_rtp");
LIBRTP_PAYLOAD_DEMUX(zms_vp8_over_rtp_demux_ops, ZMS_CODEC_VP8, "VP8", 96, 0, "vp8_over_rtp");
LIBRTP_PAYLOAD_DEMUX(zms_vp9_over_rtp_demux_ops, ZMS_CODEC_VP9, "VP9", 96, 0, "vp9_over_rtp");
LIBRTP_PAYLOAD_DEMUX(zms_h266_over_rtp_demux_ops, ZMS_CODEC_H266, "H266", 96, 1, "h266_over_rtp");
