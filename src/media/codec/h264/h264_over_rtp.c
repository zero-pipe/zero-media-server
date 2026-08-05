#include "zms/media/codec/h264/h264_over_rtp.h"
#include "zms/engine/media_clock.h"
#include "zms/util/log_throttle.h"
#include "rtp-demuxer.h"
#include "rtp-payload.h"
#include <stdlib.h>
#include <string.h>

static const uint8_t k_annexb_prefix[4] = {0, 0, 0, 1};

struct zms_h264_over_rtp_demuxer {
    zms_h264_over_rtp_demuxer_opts opts;
    struct rtp_demuxer_t *rtp_demuxer;
    zms_frame frame;
    zms_frame au;
    uint32_t au_pts_ms;
    int au_active;
    int au_has_vcl;
    uint32_t rtp_clock_hz;
    int pending_marker;
};

static int h264_type(uint8_t nal)
{
    return nal & 0x1f;
}

static int is_vcl_nal(int type)
{
    return type >= 1 && type <= 5;
}

static int is_key_nal(const uint8_t *data, size_t len)
{
    if (!data || len == 0) {
        return 0;
    }
    const uint8_t *nal = data;
    size_t nlen = len;
    if (len >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1) {
        nal = data + 4;
        nlen = len - 4;
    }
    if (nlen == 0) {
        return 0;
    }
    return h264_type(nal[0]) == 5;
}

static int is_first_mb_vcl(const uint8_t *nal, size_t len)
{
    return is_vcl_nal(h264_type(nal[0])) && len >= 2 && (nal[1] & 0x80);
}

static void au_clear(zms_h264_over_rtp_demuxer *d)
{
    d->au.size = 0;
    d->au_active = 0;
    d->au_has_vcl = 0;
}

static int au_append_nal(zms_h264_over_rtp_demuxer *d, const uint8_t *nal, size_t len,
                         uint32_t pts_ms)
{
    if (!d || !nal || len == 0) {
        return 0;
    }
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

static int au_count_vcl_nals(const uint8_t *data, size_t len)
{
    const uint8_t *p = data;
    const uint8_t *end = data + len;
    int count = 0;

    while (p + 3 < end) {
        size_t start = 0;
        if (p + 4 <= end && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) {
            start = 4;
        } else if (p[0] == 0 && p[1] == 0 && p[2] == 1) {
            start = 3;
        }
        if (start) {
            const uint8_t *nal = p + start;
            if (nal < end && is_vcl_nal(h264_type(nal[0]))) {
                ++count;
            }
            p = nal + 1;
        } else {
            ++p;
        }
    }
    return count;
}

static int au_flush(zms_h264_over_rtp_demuxer *d)
{
    if (!d || !d->opts.on_frame_cb || d->au.size == 0) {
        return 0;
    }
    zms_log_debug_throttle("h264_rtp_au_flush", 2000,
                           "h264 rtp au flush size=%zu vcl=%d key=%d pts=%u", d->au.size,
                           au_count_vcl_nals(d->au.data, d->au.size),
                           is_key_nal(d->au.data, d->au.size), d->au_pts_ms);
    d->frame.codec = ZMS_CODEC_H264;
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

static void emit_parameter_nal(zms_h264_over_rtp_demuxer *d, const uint8_t *ptr, size_t size,
                               uint32_t pts)
{
    zms_frame_reserve(&d->frame, size + 4);
    memcpy(d->frame.data, k_annexb_prefix, 4);
    memcpy(d->frame.data + 4, ptr, size);
    d->frame.size = size + 4;
    d->frame.codec = ZMS_CODEC_H264;
    d->frame.track = ZMS_TRACK_VIDEO;
    d->frame.keyframe = is_key_nal(d->frame.data, d->frame.size);
    d->frame.pts_ms = pts;
    d->frame.dts_ms = pts;
    if (d->opts.on_frame_cb) {
        d->opts.on_frame_cb(&d->frame, d->opts.user);
    }
    d->frame.size = 0;
}

static int on_vcl_nal(zms_h264_over_rtp_demuxer *d, const uint8_t *ptr, size_t size, uint32_t pts)
{
    int first = is_first_mb_vcl(ptr, size);
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

static int librtp_h264_packet(void *param, const void *packet, int bytes, uint32_t timestamp,
                              int flags)
{
    zms_h264_over_rtp_demuxer *d = (zms_h264_over_rtp_demuxer *)param;
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
    int type = h264_type(ptr[0]);

    if (!is_vcl_nal(type)) {
        emit_parameter_nal(d, ptr, (size_t)bytes, pts);
        return 0;
    }

    (void)on_vcl_nal(d, ptr, (size_t)bytes, pts);
    return 0;
}

zms_h264_over_rtp_demuxer *
zms_h264_over_rtp_demuxer_create(const zms_h264_over_rtp_demuxer_opts *opts)
{
    int jitter;
    int pt;

    if (!opts) {
        return NULL;
    }

    zms_h264_over_rtp_demuxer *d = (zms_h264_over_rtp_demuxer *)calloc(1, sizeof(*d));
    if (!d) {
        return NULL;
    }

    d->opts = *opts;
    d->rtp_clock_hz = opts->rtp_clock_hz > 0 ? opts->rtp_clock_hz : 90000u;
    zms_frame_init(&d->frame);
    zms_frame_init(&d->au);

    jitter = opts->jitter_ms > 0 ? opts->jitter_ms : 0;
    pt = opts->payload_type > 0 ? opts->payload_type : 96;
    d->rtp_demuxer = rtp_demuxer_create(jitter, (int)d->rtp_clock_hz, pt, "H264",
                                        (rtp_demuxer_onpacket)librtp_h264_packet, d);
    if (!d->rtp_demuxer) {
        zms_frame_clear(&d->frame);
        zms_frame_clear(&d->au);
        free(d);
        return NULL;
    }
    return d;
}

void zms_h264_over_rtp_demuxer_destroy(zms_h264_over_rtp_demuxer *d)
{
    if (!d) {
        return;
    }
    rtp_demuxer_destroy(&d->rtp_demuxer);
    zms_frame_clear(&d->frame);
    zms_frame_clear(&d->au);
    free(d);
}

ztk_err_t zms_h264_over_rtp_demuxer_input_rtp(zms_h264_over_rtp_demuxer *d,
                                              const zms_rtp_packet *pkt)
{
    int r;

    if (!d || !d->rtp_demuxer || !pkt || !pkt->data || pkt->size < 12) {
        return ZTK_ERR_INVALID;
    }

    d->pending_marker = pkt->hdr.marker;
    r = rtp_demuxer_input(d->rtp_demuxer, pkt->data, (int)pkt->size);
    /* 仅在 RTP M-bit 时 flush（AU 结束）。FU-A E-bit 勿 flush：
     * 仅标记分片 NAL 结束，非整 AU；多 slice 1080p 会一 slice 一 ring 帧导致花屏。 */
    if (d->pending_marker && d->au_active && d->au_has_vcl) {
        au_flush(d);
    }
    return r >= 0 ? ZTK_OK : ZTK_ERR_INVALID;
}

/* --- payload demux ops（注册表入口，见 payload/payload_rtp_register.c）--- */
static void *h264_over_rtp_demux_create(const zms_payload_demux_opts *opts)
{
    zms_h264_over_rtp_demuxer_opts demuxer_opts;

    if (!opts || !opts->on_frame) {
        return NULL;
    }
    memset(&demuxer_opts, 0, sizeof(demuxer_opts));
    demuxer_opts.on_frame_cb = (zms_h264_over_rtp_on_frame_cb)opts->on_frame;
    demuxer_opts.user = opts->user;
    demuxer_opts.rtp_clock_hz = opts->rtp_clock_hz > 0 ? opts->rtp_clock_hz : 90000u;
    demuxer_opts.payload_type = opts->payload_type;
    return zms_h264_over_rtp_demuxer_create(&demuxer_opts);
}

static void h264_over_rtp_demux_destroy(void *ctx)
{
    zms_h264_over_rtp_demuxer_destroy((zms_h264_over_rtp_demuxer *)ctx);
}

static ztk_err_t h264_over_rtp_demux_input_rtp(void *ctx, const zms_rtp_packet *pkt, zms_frame *out)
{
    (void)out;
    return zms_h264_over_rtp_demuxer_input_rtp((zms_h264_over_rtp_demuxer *)ctx, pkt);
}

const zms_payload_demux_ops zms_h264_over_rtp_demux_ops = {
    ZMS_CODEC_H264,
    ZMS_WIRE_FORMAT_RTP,
    "h264_over_rtp",
    h264_over_rtp_demux_create,
    h264_over_rtp_demux_destroy,
    h264_over_rtp_demux_input_rtp,
};
