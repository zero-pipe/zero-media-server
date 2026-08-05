#include "zms/media/codec/h265/h265_over_rtp.h"
#include "zms/media/codec/payload/payload_registry.h"
#include "zms/engine/media_clock.h"
#include "rtp-demuxer.h"
#include "rtp-payload.h"
#include <stdlib.h>
#include <string.h>

static const uint8_t k_annexb_prefix[4] = {0, 0, 0, 1};

struct zms_h265_over_rtp_demuxer {
    zms_h265_over_rtp_demuxer_opts opts;
    struct rtp_demuxer_t *rtp_demuxer;
    zms_frame frame;
    zms_frame au;
    uint32_t au_pts_ms;
    int au_active;
    int au_has_vcl;
    uint32_t rtp_clock_hz;
    int pending_marker;
};

static int hevc_type(uint8_t nal0)
{
    return (nal0 >> 1) & 0x3f;
}

static int is_vcl(int t)
{
    return t < 32;
}

static int is_key_nal(const uint8_t *data, size_t len)
{
    if (!data || len < 5) {
        return 0;
    }
    const uint8_t *nal = data;
    if (len >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1) {
        nal = data + 4;
    }
    int t = hevc_type(nal[0]);
    return t >= 16 && t <= 21;
}

static int is_first_slice(const uint8_t *nal, size_t len)
{
    int t = hevc_type(nal[0]);
    return is_vcl(t) && len >= 2 && (nal[1] & 0x80);
}

static int h265_rtp_fu_end(const uint8_t *data, size_t size)
{
    if (!data || size < 3) {
        return 0;
    }
    if (hevc_type(data[0]) != 49) {
        return 0;
    }
    return (data[2] & 0x40) != 0;
}

static void au_clear(struct zms_h265_over_rtp_demuxer *d)
{
    d->au.size = 0;
    d->au_active = 0;
    d->au_has_vcl = 0;
}

static int au_append_nal(struct zms_h265_over_rtp_demuxer *d, const uint8_t *nal, size_t len,
                         uint32_t pts_ms)
{
    if (!d->au_active) {
        d->au_pts_ms = pts_ms;
        d->au_active = 1;
    }
    zms_frame_reserve(&d->au, d->au.size + len + 4);
    memcpy(d->au.data + d->au.size, k_annexb_prefix, 4);
    memcpy(d->au.data + d->au.size + 4, nal, len);
    d->au.size += len + 4;
    return is_key_nal(nal, len);
}

static int au_flush(struct zms_h265_over_rtp_demuxer *d)
{
    if (!d || !d->opts.on_frame_cb || d->au.size == 0) {
        return 0;
    }
    d->frame.codec = ZMS_CODEC_H265;
    d->frame.track = ZMS_TRACK_VIDEO;
    d->frame.keyframe = is_key_nal(d->au.data, d->au.size);
    d->frame.pts_ms = d->au_pts_ms;
    d->frame.dts_ms = d->au_pts_ms;
    d->frame.data = d->au.data;
    d->frame.size = d->au.size;
    d->frame.owned = 0;
    d->opts.on_frame_cb(&d->frame, d->opts.user);
    int key = d->frame.keyframe;
    au_clear(d);
    return key;
}

static int on_vcl(struct zms_h265_over_rtp_demuxer *d, const uint8_t *ptr, size_t size,
                  uint32_t pts)
{
    int first = is_first_slice(ptr, size);
    int key = 0;
    if (first && d->au_active && d->au_has_vcl) {
        key = au_flush(d);
    }
    if (first) {
        key = au_append_nal(d, ptr, size, pts) || key;
    } else if (d->au_active) {
        key = au_append_nal(d, ptr, size, d->au_pts_ms) || key;
    } else {
        key = au_append_nal(d, ptr, size, pts) || key;
    }
    d->au_has_vcl = 1;
    return key;
}

static int librtp_h265_packet(void *param, const void *packet, int bytes, uint32_t timestamp,
                              int flags)
{
    struct zms_h265_over_rtp_demuxer *d = (struct zms_h265_over_rtp_demuxer *)param;
    const uint8_t *ptr = (const uint8_t *)packet;
    if (!d || !ptr || bytes <= 0) {
        return 0;
    }
    if (flags & RTP_PAYLOAD_FLAG_PACKET_LOST) {
        au_clear(d);
        return 0;
    }
    uint32_t hz = d->rtp_clock_hz > 0 ? d->rtp_clock_hz : 90000u;
    uint32_t pts = zms_rtp_clock_to_ms(timestamp, hz);
    int t = hevc_type(ptr[0]);
    if (!is_vcl(t)) {
        zms_frame_reserve(&d->frame, (size_t)bytes + 4);
        memcpy(d->frame.data, k_annexb_prefix, 4);
        memcpy(d->frame.data + 4, ptr, (size_t)bytes);
        d->frame.size = (size_t)bytes + 4;
        d->frame.codec = ZMS_CODEC_H265;
        d->frame.track = ZMS_TRACK_VIDEO;
        d->frame.keyframe = 0;
        d->frame.pts_ms = pts;
        d->frame.dts_ms = pts;
        if (d->opts.on_frame_cb) {
            d->opts.on_frame_cb(&d->frame, d->opts.user);
        }
        d->frame.size = 0;
        return 0;
    }
    (void)on_vcl(d, ptr, (size_t)bytes, pts);
    return 0;
}

zms_h265_over_rtp_demuxer *
zms_h265_over_rtp_demuxer_create(const zms_h265_over_rtp_demuxer_opts *opts)
{
    int jitter;
    int pt;

    if (!opts) {
        return NULL;
    }
    struct zms_h265_over_rtp_demuxer *d = (struct zms_h265_over_rtp_demuxer *)calloc(1, sizeof(*d));
    if (!d) {
        return NULL;
    }
    d->opts = *opts;
    d->rtp_clock_hz = opts->rtp_clock_hz > 0 ? opts->rtp_clock_hz : 90000u;
    zms_frame_init(&d->frame);
    zms_frame_init(&d->au);

    jitter = opts->jitter_ms > 0 ? opts->jitter_ms : 200;
    pt = opts->payload_type > 0 ? opts->payload_type : 96;
    d->rtp_demuxer = rtp_demuxer_create(jitter, (int)d->rtp_clock_hz, pt, "H265",
                                        (rtp_demuxer_onpacket)librtp_h265_packet, d);
    if (!d->rtp_demuxer) {
        zms_frame_clear(&d->frame);
        zms_frame_clear(&d->au);
        free(d);
        return NULL;
    }
    return d;
}

void zms_h265_over_rtp_demuxer_destroy(zms_h265_over_rtp_demuxer *d)
{
    if (!d) {
        return;
    }
    rtp_demuxer_destroy(&d->rtp_demuxer);
    zms_frame_clear(&d->frame);
    zms_frame_clear(&d->au);
    free(d);
}

ztk_err_t zms_h265_over_rtp_demuxer_input_rtp(zms_h265_over_rtp_demuxer *d,
                                              const zms_rtp_packet *pkt)
{
    int fu_end;
    int r;

    if (!d || !d->rtp_demuxer || !pkt || !pkt->data || pkt->size < 12) {
        return ZTK_ERR_INVALID;
    }
    d->pending_marker = pkt->hdr.marker;
    fu_end = h265_rtp_fu_end(pkt->payload, pkt->payload_size);
    r = rtp_demuxer_input(d->rtp_demuxer, pkt->data, (int)pkt->size);
    if ((d->pending_marker || fu_end) && d->au_active && d->au_has_vcl) {
        au_flush(d);
    }
    return r >= 0 ? ZTK_OK : ZTK_ERR_INVALID;
}

static void *h265_over_rtp_demux_create(const zms_payload_demux_opts *opts)
{
    zms_h265_over_rtp_demuxer_opts demuxer_opts;

    if (!opts || !opts->on_frame) {
        return NULL;
    }
    memset(&demuxer_opts, 0, sizeof(demuxer_opts));
    demuxer_opts.on_frame_cb = (zms_h265_over_rtp_on_frame_cb)opts->on_frame;
    demuxer_opts.user = opts->user;
    demuxer_opts.rtp_clock_hz = opts->rtp_clock_hz > 0 ? opts->rtp_clock_hz : 90000u;
    return zms_h265_over_rtp_demuxer_create(&demuxer_opts);
}

static void h265_over_rtp_demux_destroy(void *ctx)
{
    zms_h265_over_rtp_demuxer_destroy((zms_h265_over_rtp_demuxer *)ctx);
}

static ztk_err_t h265_over_rtp_demux_input_rtp(void *ctx, const zms_rtp_packet *pkt, zms_frame *out)
{
    (void)out;
    return zms_h265_over_rtp_demuxer_input_rtp((zms_h265_over_rtp_demuxer *)ctx, pkt);
}

const zms_payload_demux_ops zms_h265_over_rtp_demux_ops = {
    ZMS_CODEC_H265,
    ZMS_WIRE_FORMAT_RTP,
    "h265_over_rtp",
    h265_over_rtp_demux_create,
    h265_over_rtp_demux_destroy,
    h265_over_rtp_demux_input_rtp,
};
