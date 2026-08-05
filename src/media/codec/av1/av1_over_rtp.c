#include "zms/media/codec/av1/av1_over_rtp.h"
#include "zms/media/codec/av1/av1_over_rtmp.h"
#include "zms/media/codec/payload/payload_registry.h"
#include "zms/engine/media_clock.h"
#include "rtp-demuxer.h"
#include "rtp-payload.h"
#include <stdlib.h>
#include <string.h>

struct zms_av1_over_rtp_demuxer {
    zms_av1_over_rtp_demuxer_opts opts;
    struct rtp_demuxer_t *rtp_demuxer;
    zms_frame frame;
};

static int librtp_av1_packet(void *param, const void *packet, int bytes, uint32_t timestamp,
                             int flags)
{
    struct zms_av1_over_rtp_demuxer *d = (struct zms_av1_over_rtp_demuxer *)param;
    uint32_t hz;
    uint32_t pts;

    if (!d || !packet || bytes <= 0) {
        return 0;
    }
    if (flags & RTP_PAYLOAD_FLAG_PACKET_LOST) {
        return 0;
    }

    hz = d->opts.rtp_clock_hz > 0 ? d->opts.rtp_clock_hz : 90000u;
    pts = zms_rtp_clock_to_ms(timestamp, hz);

    zms_frame_reserve(&d->frame, (size_t)bytes);
    memcpy(d->frame.data, packet, (size_t)bytes);
    d->frame.size = (size_t)bytes;
    d->frame.codec = ZMS_CODEC_AV1;
    d->frame.track = ZMS_TRACK_VIDEO;
    d->frame.keyframe = zms_av1_obu_has_sequence_header((const uint8_t *)packet, (size_t)bytes);
    d->frame.pts_ms = pts;
    d->frame.dts_ms = pts;
    d->frame.owned = 0;

    if (d->opts.on_frame_cb) {
        d->opts.on_frame_cb(&d->frame, d->opts.user);
    }
    return 0;
}

zms_av1_over_rtp_demuxer *zms_av1_over_rtp_demuxer_create(const zms_av1_over_rtp_demuxer_opts *opts)
{
    int jitter;
    int pt;

    if (!opts || !opts->on_frame_cb) {
        return NULL;
    }

    struct zms_av1_over_rtp_demuxer *d = (struct zms_av1_over_rtp_demuxer *)calloc(1, sizeof(*d));
    if (!d) {
        return NULL;
    }

    d->opts = *opts;
    if (d->opts.rtp_clock_hz == 0) {
        d->opts.rtp_clock_hz = 90000u;
    }
    zms_frame_init(&d->frame);

    jitter = opts->jitter_ms > 0 ? opts->jitter_ms : 200;
    pt = opts->payload_type > 0 ? opts->payload_type : 97;
    d->rtp_demuxer = rtp_demuxer_create(jitter, (int)d->opts.rtp_clock_hz, pt, "AV1",
                                        (rtp_demuxer_onpacket)librtp_av1_packet, d);
    if (!d->rtp_demuxer) {
        zms_frame_clear(&d->frame);
        free(d);
        return NULL;
    }
    return d;
}

void zms_av1_over_rtp_demuxer_destroy(zms_av1_over_rtp_demuxer *d)
{
    if (!d) {
        return;
    }
    rtp_demuxer_destroy(&d->rtp_demuxer);
    zms_frame_clear(&d->frame);
    free(d);
}

ztk_err_t zms_av1_over_rtp_demuxer_input_rtp(zms_av1_over_rtp_demuxer *d, const zms_rtp_packet *pkt)
{
    if (!d || !d->rtp_demuxer || !pkt || !pkt->data || pkt->size < 12) {
        return ZTK_ERR_INVALID;
    }
    return rtp_demuxer_input(d->rtp_demuxer, pkt->data, (int)pkt->size) >= 0 ? ZTK_OK
                                                                             : ZTK_ERR_INVALID;
}

static void *av1_over_rtp_demux_create(const zms_payload_demux_opts *opts)
{
    zms_av1_over_rtp_demuxer_opts demuxer_opts;

    if (!opts || !opts->on_frame) {
        return NULL;
    }
    memset(&demuxer_opts, 0, sizeof(demuxer_opts));
    demuxer_opts.on_frame_cb = (zms_av1_over_rtp_on_frame_cb)opts->on_frame;
    demuxer_opts.user = opts->user;
    demuxer_opts.rtp_clock_hz = opts->rtp_clock_hz > 0 ? opts->rtp_clock_hz : 90000u;
    return zms_av1_over_rtp_demuxer_create(&demuxer_opts);
}

static void av1_over_rtp_demux_destroy(void *ctx)
{
    zms_av1_over_rtp_demuxer_destroy((zms_av1_over_rtp_demuxer *)ctx);
}

static ztk_err_t av1_over_rtp_demux_input_rtp(void *ctx, const zms_rtp_packet *pkt, zms_frame *out)
{
    (void)out;
    return zms_av1_over_rtp_demuxer_input_rtp((zms_av1_over_rtp_demuxer *)ctx, pkt);
}

const zms_payload_demux_ops zms_av1_over_rtp_demux_ops = {
    ZMS_CODEC_AV1,
    ZMS_WIRE_FORMAT_RTP,
    "av1_over_rtp",
    av1_over_rtp_demux_create,
    av1_over_rtp_demux_destroy,
    av1_over_rtp_demux_input_rtp,
};
