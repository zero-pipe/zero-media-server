#include "zms/media/codec/aac/aac_over_rtp.h"
#include "zms/media/codec/payload/payload_registry.h"
#include "zms/engine/media_clock.h"
#include "rtp-demuxer.h"
#include "rtp-payload.h"
#include <stdlib.h>
#include <string.h>

/* --- 无状态 parse --- */

static int adts_sync(const uint8_t *p, size_t left)
{
    return left >= 7 && p[0] == 0xff && (p[1] & 0xf0) == 0xf0;
}

/* protection_absent=1 为 7 字节头；0 则 7 字节 + 2 字节 CRC 后接 raw AU。 */
static size_t adts_hdr_len(const uint8_t *p, size_t left)
{
    if (!adts_sync(p, left)) {
        return 0;
    }
    return (p[1] & 0x01) ? 7u : 9u;
}

static size_t adts_frame_length(const uint8_t *p, size_t left)
{
    size_t flen;

    if (!adts_sync(p, left)) {
        return 0;
    }
    flen = ((size_t)(p[3] & 0x03) << 11) | ((size_t)p[4] << 3) | ((size_t)(p[5] & 0xe0) >> 5);
    if (flen < 7 || flen > 8192) {
        return 0;
    }
    return flen;
}

/** PES ADTS 帧完整覆盖 payload（CC gap 专用，比 foreach 探针更严） */
int zms_aac_es_strict_valid(const uint8_t *es, size_t len)
{
    size_t off = 0;

    if (!es || len < 7 || !adts_sync(es, len)) {
        return 0;
    }
    while (off + 7 <= len) {
        size_t hdr = adts_hdr_len(es + off, len - off);
        size_t flen = adts_frame_length(es + off, len - off);
        if (hdr < 7 || flen < hdr || off + flen > len) {
            return 0;
        }
        off += flen;
    }
    return off == len;
}

ztk_err_t zms_aac_es_foreach_frame(const uint8_t *es, size_t len, zms_aac_au_cb cb, void *user)
{
    if (!es || len == 0 || !cb) {
        return ZTK_ERR_INVALID;
    }

    if (adts_sync(es, len)) {
        size_t off = 0;
        unsigned frames = 0;
        while (off + 7 <= len) {
            size_t hdr = adts_hdr_len(es + off, len - off);
            size_t flen = adts_frame_length(es + off, len - off);
            if (hdr < 7 || flen < hdr || off + flen > len) {
                break;
            }
            if (cb(es + off + hdr, flen - hdr, user) != 0) {
                break;
            }
            off += flen;
            frames++;
        }
        if (frames > 0) {
            return ZTK_OK;
        }
    }

    return cb(es, len, user) == 0 ? ZTK_OK : ZTK_ERR_INVALID;
}

ztk_err_t zms_aac_es_to_raw(const uint8_t *es, size_t len, const uint8_t **raw, size_t *raw_len)
{
    if (!es || len == 0 || !raw || !raw_len) {
        return ZTK_ERR_INVALID;
    }

    if (adts_sync(es, len)) {
        size_t hdr = adts_hdr_len(es, len);
        size_t flen = adts_frame_length(es, len);
        if (hdr >= 7 && flen >= hdr && flen <= len) {
            *raw = es + hdr;
            *raw_len = flen - hdr;
            return ZTK_OK;
        }
    }

    *raw = es;
    *raw_len = len;
    return ZTK_OK;
}

/* --- 有状态 demuxer --- */

struct zms_aac_over_rtp_demuxer {
    struct rtp_demuxer_t *rtp_demuxer;
    zms_aac_over_rtp_on_frame_cb on_frame_cb;
    void *user;
    uint32_t last_dts;
    uint32_t rtp_clock_hz;
};

static int librtp_aac_packet(void *param, const void *packet, int bytes, uint32_t timestamp,
                             int flags)
{
    struct zms_aac_over_rtp_demuxer *d = (struct zms_aac_over_rtp_demuxer *)param;
    uint32_t hz;
    uint32_t stamp;

    if (!d || !d->on_frame_cb || !packet || bytes <= 0) {
        return 0;
    }
    if (flags & RTP_PAYLOAD_FLAG_PACKET_LOST) {
        return 0;
    }

    hz = d->rtp_clock_hz > 0 ? d->rtp_clock_hz : 44100u;
    stamp = zms_rtp_clock_to_ms(timestamp, hz);
    d->on_frame_cb((const uint8_t *)packet, (size_t)bytes, stamp, d->user);
    d->last_dts = stamp;
    return 0;
}

zms_aac_over_rtp_demuxer *zms_aac_over_rtp_demuxer_create(const zms_aac_over_rtp_demuxer_opts *opts)
{
    int jitter;
    int pt;

    if (!opts || !opts->on_frame_cb) {
        return NULL;
    }

    struct zms_aac_over_rtp_demuxer *d = (struct zms_aac_over_rtp_demuxer *)calloc(1, sizeof(*d));
    if (!d) {
        return NULL;
    }

    d->on_frame_cb = opts->on_frame_cb;
    d->user = opts->user;
    d->rtp_clock_hz = opts->rtp_clock_hz > 0 ? opts->rtp_clock_hz : 44100u;

    jitter = opts->jitter_ms > 0 ? opts->jitter_ms : 200;
    pt = opts->payload_type > 0 ? opts->payload_type : 97;
    d->rtp_demuxer = rtp_demuxer_create(jitter, (int)d->rtp_clock_hz, pt, "mpeg4-generic",
                                        (rtp_demuxer_onpacket)librtp_aac_packet, d);
    if (!d->rtp_demuxer) {
        free(d);
        return NULL;
    }
    return d;
}

void zms_aac_over_rtp_demuxer_destroy(zms_aac_over_rtp_demuxer *d)
{
    if (!d) {
        return;
    }
    rtp_demuxer_destroy(&d->rtp_demuxer);
    free(d);
}

ztk_err_t zms_aac_over_rtp_demuxer_input_rtp(zms_aac_over_rtp_demuxer *d, const zms_rtp_packet *pkt)
{
    int r;

    if (!d || !d->rtp_demuxer || !pkt || !pkt->data || pkt->size < 12) {
        return ZTK_ERR_INVALID;
    }

    r = rtp_demuxer_input(d->rtp_demuxer, pkt->data, (int)pkt->size);
    return r >= 0 ? ZTK_OK : ZTK_ERR_INVALID;
}

typedef struct {
    zms_payload_demux_opts payload_opts;
    zms_aac_over_rtp_demuxer *demuxer;
} zms_aac_over_rtp_demux_ctx;

static void aac_over_rtp_demux_bridge(const uint8_t *aac, size_t len, uint64_t dts_ms, void *user)
{
    zms_aac_over_rtp_demux_ctx *ctx = (zms_aac_over_rtp_demux_ctx *)user;
    zms_frame frame;

    if (!ctx || !ctx->payload_opts.on_frame || !aac || len == 0) {
        return;
    }
    zms_frame_init(&frame);
    frame.data = (uint8_t *)aac;
    frame.size = len;
    frame.codec = ZMS_CODEC_AAC;
    frame.track = ZMS_TRACK_AUDIO;
    frame.dts_ms = frame.pts_ms = dts_ms;
    ctx->payload_opts.on_frame(&frame, ctx->payload_opts.user);
}

static void *aac_over_rtp_demux_create(const zms_payload_demux_opts *opts)
{
    zms_aac_over_rtp_demux_ctx *ctx;
    zms_aac_over_rtp_demuxer_opts demuxer_opts;

    if (!opts || !opts->on_frame) {
        return NULL;
    }
    ctx = (zms_aac_over_rtp_demux_ctx *)calloc(1, sizeof(*ctx));
    if (!ctx) {
        return NULL;
    }
    ctx->payload_opts = *opts;
    memset(&demuxer_opts, 0, sizeof(demuxer_opts));
    demuxer_opts.on_frame_cb = aac_over_rtp_demux_bridge;
    demuxer_opts.user = ctx;
    demuxer_opts.rtp_clock_hz = opts->rtp_clock_hz > 0 ? opts->rtp_clock_hz : 44100u;
    demuxer_opts.payload_type = opts->payload_type;
    ctx->demuxer = zms_aac_over_rtp_demuxer_create(&demuxer_opts);
    if (!ctx->demuxer) {
        free(ctx);
        return NULL;
    }
    return ctx;
}

static void aac_over_rtp_demux_destroy(void *ctx)
{
    zms_aac_over_rtp_demux_ctx *demux = (zms_aac_over_rtp_demux_ctx *)ctx;
    if (!demux) {
        return;
    }
    zms_aac_over_rtp_demuxer_destroy(demux->demuxer);
    free(demux);
}

static ztk_err_t aac_over_rtp_demux_input_rtp(void *ctx, const zms_rtp_packet *pkt, zms_frame *out)
{
    zms_aac_over_rtp_demux_ctx *demux = (zms_aac_over_rtp_demux_ctx *)ctx;
    (void)out;
    if (!demux || !demux->demuxer) {
        return ZTK_ERR_INVALID;
    }
    return zms_aac_over_rtp_demuxer_input_rtp(demux->demuxer, pkt);
}

const zms_payload_demux_ops zms_aac_over_rtp_demux_ops = {
    ZMS_CODEC_AAC,
    ZMS_WIRE_FORMAT_RTP,
    "aac_over_rtp",
    aac_over_rtp_demux_create,
    aac_over_rtp_demux_destroy,
    aac_over_rtp_demux_input_rtp,
};
