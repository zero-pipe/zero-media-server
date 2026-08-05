#include "zms/media/container/mpegts/mpegts_demuxer.h"
#include "zms/media/container/container_dispatcher.h"
#include "zms/media/codec/h264/h264_es.h"
#include "zms/media/codec/h265/h265_es.h"
#include "zms/engine/media_clock.h"
#include "zms/engine/media/media_limits.h"
#include "mpeg-proto.h"
#include "mpeg-ts.h"
#include "mpeg-types.h"
#include "mpeg4-avc.h"
#include "ztk/util/log.h"
#include <stdlib.h>
#include <string.h>

#define MPEGTS_PKT_SIZE 188
#define MPEGTS_SYNC_BYTE 0x47
#define MPEGTS_SEG_CAP ZMS_MPEGTS_AU_MAX
#define MPEGTS_SNIFF_CAP ZMS_MPEGTS_AU_MAX

struct zms_mpegts_demuxer {
    zms_mpegts_demuxer_opts opts;
    struct ts_demuxer_t *ts;
    zms_frame frame;
    uint8_t seg[MPEGTS_SEG_CAP];
    size_t seg_len;
    uint8_t payload_acc[MPEGTS_SNIFF_CAP];
    size_t payload_acc_len;
    uint8_t sniff_sps[512];
    size_t sniff_sps_len;
    uint8_t sniff_pps[256];
    size_t sniff_pps_len;
    uint8_t sniff_roll[256];
    size_t sniff_roll_len;
    int h264_ps_applied;
    unsigned ts_pkts;
    unsigned es_frames;
    int logged_no_es;
    int logged_no_ps;
    uint8_t h264_au[MPEGTS_SEG_CAP];
    size_t h264_au_len;
    uint32_t h264_au_dts_ms;
    uint32_t h264_au_pts_ms;
    int h264_au_active;
    int h264_au_idr_flag;
    /** TS 丢包/损坏后：P/B 需新 IDR（H.264 参考模型）。 */
    int h264_need_idr;
    uint8_t h264_frag[ZMS_MPEGTS_AU_MAX];
};

static void mpegts_h264_au_reset(zms_mpegts_demuxer *d)
{
    if (!d) {
        return;
    }
    d->h264_au_len = 0;
    d->h264_au_active = 0;
    d->h264_au_idr_flag = 0;
}

static void mpegts_emit_es_frame(zms_mpegts_demuxer *d, zms_codec_id codec, const uint8_t *data,
                                 size_t bytes, uint32_t pts_ms, uint32_t dts_ms, int flags)
{
    int key = 0;
    const uint8_t *emit = data;

    if (!d || !d->opts.on_frame || !data || bytes == 0) {
        return;
    }

    if (bytes <= sizeof(d->payload_acc)) {
        memcpy(d->payload_acc, data, bytes);
        emit = d->payload_acc;
    }

    d->frame.codec = codec;
    d->frame.track = zms_codec_track_type(codec);
    d->frame.pts_ms = pts_ms;
    d->frame.dts_ms = dts_ms;
    if (d->frame.dts_ms == 0 && d->frame.pts_ms != 0) {
        d->frame.dts_ms = d->frame.pts_ms;
    }
    if (d->frame.pts_ms == 0 && d->frame.dts_ms != 0) {
        d->frame.pts_ms = d->frame.dts_ms;
    }

    if (flags & MPEG_FLAG_IDR_FRAME) {
        key = 1;
    } else if (codec == ZMS_CODEC_H264) {
        key = zms_h264_es_is_annexb(emit, bytes) ? zms_h264_annexb_is_idr(emit, bytes) : 0;
    } else if (codec == ZMS_CODEC_H265) {
        key = zms_h265_annexb_is_sync_key(emit, bytes);
    } else {
        key = 0;
    }

    d->frame.keyframe = key;
    d->frame.config_frame = 0;
    d->frame.drop_able = 0;
    d->frame.data = (uint8_t *)emit;
    d->frame.size = bytes;
    d->frame.owned = 0;
    d->opts.on_frame(&d->frame, d->opts.user);
}

static int mpegts_h264_au_append(zms_mpegts_demuxer *d, const uint8_t *data, size_t bytes)
{
    const uint8_t *src = data;
    size_t src_len = bytes;
    size_t out_len = 0;

    if (!d || !data || bytes == 0) {
        return 0;
    }
    if (!zms_h264_es_is_annexb(data, bytes)) {
        if (zms_h264_es_to_annexb(data, bytes, d->h264_frag, sizeof(d->h264_frag), &out_len) !=
                ZTK_OK ||
            out_len == 0) {
            return -1;
        }
        src = d->h264_frag;
        src_len = out_len;
    }
    if (d->h264_au_len + src_len > sizeof(d->h264_au)) {
        static int logged_au_overflow;
        if (!logged_au_overflow++) {
            ztk_warn("[mpegts] h264_au_overflow have=%u add=%u", (unsigned)d->h264_au_len,
                     (unsigned)src_len);
        }
        mpegts_h264_au_reset(d);
        return -1;
    }
    memcpy(d->h264_au + d->h264_au_len, src, src_len);
    d->h264_au_len += src_len;
    return 0;
}

static void mpegts_h264_au_emit(zms_mpegts_demuxer *d)
{
    const uint8_t *data;
    size_t bytes;
    const uint8_t *emit_data;
    size_t emit_len;
    int idr;
    uint32_t dts_ms;
    uint32_t pts_ms;

    if (!d || !d->opts.on_frame || !d->h264_au_active || d->h264_au_len == 0) {
        return;
    }

    data = d->h264_au;
    bytes = d->h264_au_len;
    emit_data = data;
    emit_len = bytes;

    idr = zms_h264_annexb_is_idr(data, bytes) || d->h264_au_idr_flag;

    if (idr) {
        d->h264_need_idr = 0;
    } else if (d->h264_need_idr || !zms_h264_es_has_slice(data, bytes) ||
               !zms_h264_es_starts_access_unit(data, bytes)) {
        mpegts_h264_au_reset(d);
        return;
    }

    if (idr && d->sniff_sps_len > 0 && d->sniff_pps_len > 0) {
        const uint8_t *sps = NULL, *pps = NULL;
        size_t sps_len = 0, pps_len = 0;
        if (!zms_h264_annexb_extract_sps_pps(data, bytes, &sps, &sps_len, &pps, &pps_len)) {
            size_t prep_len = 0;
            if (zms_h264_annexb_prepend_sps_pps(d->sniff_sps, d->sniff_sps_len, d->sniff_pps,
                                                d->sniff_pps_len, data, bytes, d->payload_acc,
                                                sizeof(d->payload_acc), &prep_len) == ZTK_OK &&
                prep_len > 0) {
                emit_data = d->payload_acc;
                emit_len = prep_len;
            }
        }
    }

    dts_ms = d->h264_au_dts_ms;
    pts_ms = d->h264_au_pts_ms;
    if (pts_ms < dts_ms) {
        pts_ms = dts_ms;
    }

    if (emit_len <= sizeof(d->payload_acc) && emit_data != d->payload_acc) {
        memcpy(d->payload_acc, emit_data, emit_len);
        emit_data = d->payload_acc;
    }

    d->frame.codec = ZMS_CODEC_H264;
    d->frame.track = ZMS_TRACK_VIDEO;
    d->frame.pts_ms = pts_ms;
    d->frame.dts_ms = dts_ms;
    d->frame.keyframe = idr ? 1 : 0;
    d->frame.config_frame = 0;
    d->frame.drop_able = 0;
    d->frame.data = (uint8_t *)emit_data;
    d->frame.size = emit_len;
    d->frame.owned = 0;
    d->opts.on_frame(&d->frame, d->opts.user);
    mpegts_h264_au_reset(d);
}

static void mpegts_h264_on_es(zms_mpegts_demuxer *d, const uint8_t *data, size_t bytes,
                              int64_t pts_90k, int64_t dts_90k, int flags)
{
    uint32_t dts_ms;
    uint32_t pts_ms;

    if (!d || !data || bytes == 0) {
        return;
    }

    dts_ms = zms_mpegts_90k_to_ms(dts_90k);
    pts_ms = zms_mpegts_90k_to_ms(pts_90k);
    if (dts_ms == 0 && pts_ms != 0) {
        dts_ms = pts_ms;
    }
    if (pts_ms == 0 && dts_ms != 0) {
        pts_ms = dts_ms;
    }

    /* libmpeg h26x demux 每次回调交付完整 AU；勿按 dts 合并。 */
    mpegts_h264_au_reset(d);
    if (mpegts_h264_au_append(d, data, bytes) != 0) {
        return;
    }

    d->h264_au_active = 1;
    d->h264_au_dts_ms = dts_ms;
    d->h264_au_pts_ms = pts_ms;
    d->h264_au_idr_flag = (flags & MPEG_FLAG_IDR_FRAME) ? 1 : 0;
    mpegts_h264_au_emit(d);
}

static zms_codec_id mpegts_psi_to_codec(int psi)
{
    switch (psi) {
    case PSI_STREAM_H264:
        return ZMS_CODEC_H264;
    case PSI_STREAM_H265:
    case PSI_STREAM_H265_subset:
        return ZMS_CODEC_H265;
    case PSI_STREAM_H266:
    case PSI_STREAM_H266_subset:
        return ZMS_CODEC_H266;
    case PSI_STREAM_AAC:
    case PSI_STREAM_MPEG4_AAC:
        return ZMS_CODEC_AAC;
    case PSI_STREAM_AUDIO_OPUS:
        return ZMS_CODEC_OPUS;
    case PSI_STREAM_AUDIO_G711A:
        return ZMS_CODEC_G711A;
    case PSI_STREAM_AUDIO_G711U:
        return ZMS_CODEC_G711U;
    case PSI_STREAM_VP8:
        return ZMS_CODEC_VP8;
    case PSI_STREAM_VP9:
        return ZMS_CODEC_VP9;
    case PSI_STREAM_AV1:
        return ZMS_CODEC_AV1;
    default:
        return ZMS_CODEC_INVALID;
    }
}

static void mpegts_try_apply_h264_ps(zms_mpegts_demuxer *d, const uint8_t *sps, size_t sps_len,
                                     const uint8_t *pps, size_t pps_len, const char *via)
{
    if (!d || !d->opts.on_h264_ps || !sps || !pps || sps_len == 0 || pps_len == 0) {
        return;
    }
    d->opts.on_h264_ps(sps, sps_len, pps, pps_len, d->opts.user);
    d->h264_ps_applied = 1;
    ztk_debug("[mpegts] h264_ps via=%s sps=%u pps=%u", via, (unsigned)sps_len, (unsigned)pps_len);
}

static void mpegts_try_apply_cached_ps(zms_mpegts_demuxer *d, const char *via)
{
    if (!d || !d->sniff_sps_len || !d->sniff_pps_len) {
        return;
    }
    mpegts_try_apply_h264_ps(d, d->sniff_sps, d->sniff_sps_len, d->sniff_pps, d->sniff_pps_len,
                             via);
}

static void mpegts_cache_annexb_ps(zms_mpegts_demuxer *d, const uint8_t *annexb, size_t len);

static void mpegts_cache_ps_from_es(zms_mpegts_demuxer *d, const uint8_t *data, size_t len,
                                    const char *via)
{
    const uint8_t *sps = NULL, *pps = NULL;
    size_t sps_len = 0, pps_len = 0;

    if (!d || !data || len == 0) {
        return;
    }
    if (zms_h264_es_extract_sps_pps(data, len, &sps, &sps_len, &pps, &pps_len)) {
        if (sps_len == 0 || sps_len > sizeof(d->sniff_sps) || pps_len == 0 ||
            pps_len > sizeof(d->sniff_pps)) {
            return;
        }
        if (d->h264_ps_applied && sps_len == d->sniff_sps_len && pps_len == d->sniff_pps_len &&
            memcmp(d->sniff_sps, sps, sps_len) == 0 && memcmp(d->sniff_pps, pps, pps_len) == 0) {
            return;
        }
        memcpy(d->sniff_sps, sps, sps_len);
        d->sniff_sps_len = sps_len;
        memcpy(d->sniff_pps, pps, pps_len);
        d->sniff_pps_len = pps_len;
    } else {
        mpegts_cache_annexb_ps(d, data, len);
    }
    mpegts_try_apply_cached_ps(d, via);
}

static void mpegts_sniff_on_nal(void *param, const uint8_t *nalu, size_t bytes)
{
    zms_mpegts_demuxer *d = (zms_mpegts_demuxer *)param;
    int t;

    if (!d || !nalu || bytes == 0) {
        return;
    }
    t = nalu[0] & 0x1f;
    if (t == 7 && bytes <= sizeof(d->sniff_sps)) {
        memcpy(d->sniff_sps, nalu, bytes);
        d->sniff_sps_len = bytes;
    } else if (t == 8 && bytes <= sizeof(d->sniff_pps)) {
        memcpy(d->sniff_pps, nalu, bytes);
        d->sniff_pps_len = bytes;
    }
}

static void mpegts_cache_annexb_ps(zms_mpegts_demuxer *d, const uint8_t *annexb, size_t len)
{
    if (!d || !annexb || len < 5) {
        return;
    }

    mpeg4_h264_annexb_nalu(annexb, len, mpegts_sniff_on_nal, d);
    mpegts_try_apply_cached_ps(d, "annexb scan");
}

static void mpegts_roll_append(zms_mpegts_demuxer *d, const uint8_t *data, size_t len)
{
    if (!d || !data || len == 0) {
        return;
    }
    if (len >= sizeof(d->sniff_roll)) {
        memcpy(d->sniff_roll, data + len - sizeof(d->sniff_roll), sizeof(d->sniff_roll));
        d->sniff_roll_len = sizeof(d->sniff_roll);
        return;
    }
    if (d->sniff_roll_len + len > sizeof(d->sniff_roll)) {
        size_t drop = d->sniff_roll_len + len - sizeof(d->sniff_roll);
        memmove(d->sniff_roll, d->sniff_roll + drop, d->sniff_roll_len - drop);
        d->sniff_roll_len -= drop;
    }
    memcpy(d->sniff_roll + d->sniff_roll_len, data, len);
    d->sniff_roll_len += len;
}

static void mpegts_payload_acc_append(zms_mpegts_demuxer *d, const uint8_t *data, size_t len)
{
    if (!d || !data || len == 0) {
        return;
    }
    if (len >= MPEGTS_SNIFF_CAP) {
        memcpy(d->payload_acc, data + len - MPEGTS_SNIFF_CAP, MPEGTS_SNIFF_CAP);
        d->payload_acc_len = MPEGTS_SNIFF_CAP;
        return;
    }
    if (d->payload_acc_len + len > MPEGTS_SNIFF_CAP) {
        size_t drop = d->payload_acc_len + len - MPEGTS_SNIFF_CAP;
        memmove(d->payload_acc, d->payload_acc + drop, d->payload_acc_len - drop);
        d->payload_acc_len -= drop;
    }
    memcpy(d->payload_acc + d->payload_acc_len, data, len);
    d->payload_acc_len += len;
}

static size_t mpegts_ts_payload_offset(const uint8_t *pkt)
{
    size_t off = 4;
    int afc;

    if (!pkt || pkt[0] != MPEGTS_SYNC_BYTE) {
        return MPEGTS_PKT_SIZE;
    }
    afc = (pkt[3] >> 4) & 3;
    if (afc & 2) {
        if (off >= MPEGTS_PKT_SIZE) {
            return MPEGTS_PKT_SIZE;
        }
        off += 1u + pkt[4];
    }
    if (!(afc & 1)) {
        return MPEGTS_PKT_SIZE;
    }
    if (off > MPEGTS_PKT_SIZE) {
        return MPEGTS_PKT_SIZE;
    }
    return off;
}

static void mpegts_sniff_ts_packet_payload(zms_mpegts_demuxer *d, const uint8_t *pkt)
{
    size_t off, pay_len;
    uint8_t scratch[512];

    if (!d || d->h264_ps_applied || !pkt || pkt[0] != MPEGTS_SYNC_BYTE) {
        return;
    }
    off = mpegts_ts_payload_offset(pkt);
    if (off >= MPEGTS_PKT_SIZE) {
        return;
    }
    pay_len = MPEGTS_PKT_SIZE - off;
    mpegts_payload_acc_append(d, pkt + off, pay_len);
    mpegts_cache_annexb_ps(d, d->payload_acc, d->payload_acc_len);
    mpegts_try_apply_cached_ps(d, "TS payload acc");
    if (d->h264_ps_applied) {
        return;
    }
    if (d->sniff_roll_len > 0) {
        size_t prefix = d->sniff_roll_len;
        size_t chunk = pay_len;
        if (prefix + chunk > sizeof(scratch)) {
            chunk = sizeof(scratch) - prefix;
        }
        memcpy(scratch, d->sniff_roll, prefix);
        memcpy(scratch + prefix, pkt + off, chunk);
        mpegts_cache_annexb_ps(d, scratch, prefix + chunk);
        mpegts_try_apply_cached_ps(d, "TS payload roll");
    }
    mpegts_cache_annexb_ps(d, pkt + off, pay_len);
    mpegts_try_apply_cached_ps(d, "TS payload");
    mpegts_roll_append(d, pkt + off, pay_len);
}

static void mpegts_on_ts_stream(void *param, int stream, int codecid, const void *extra, int bytes,
                                int finish)
{
    const uint8_t *sps = NULL, *pps = NULL;
    size_t sps_len = 0, pps_len = 0;
    zms_mpegts_demuxer *d = (zms_mpegts_demuxer *)param;

    (void)stream;
    (void)finish;
    if (!d) {
        return;
    }
    if (codecid == PSI_STREAM_H264 && extra && bytes > 0 &&
        zms_h264_avcc_extract_sps_pps((const uint8_t *)extra, (size_t)bytes, &sps, &sps_len, &pps,
                                      &pps_len)) {
        mpegts_try_apply_h264_ps(d, sps, sps_len, pps, pps_len, "PMT esinfo");
    }
}

static int mpegts_on_packet(void *param, int program, int stream, int codecid, int flags,
                            int64_t pts_90k, int64_t dts_90k, const void *data, size_t bytes)
{
    zms_mpegts_demuxer *d = (zms_mpegts_demuxer *)param;
    zms_codec_id codec;

    (void)program;
    (void)stream;
    if (!d || !d->opts.on_frame || !data || bytes == 0) {
        return 0;
    }

    codec = mpegts_psi_to_codec(codecid);
    if (codec == ZMS_CODEC_INVALID) {
        static int logged_unknown;
        if (!logged_unknown++) {
            ztk_warn("[mpegts] unsupported_stream_type=0x%02x", codecid & 0xff);
        }
        return 0;
    }

    if (flags & MPEG_FLAG_PACKET_LOST) {
        if (codec == ZMS_CODEC_H264) {
            mpegts_h264_au_reset(d);
        }
    }
    if (flags & MPEG_FLAG_PACKET_CORRUPT) {
        if (codec == ZMS_CODEC_H264) {
            mpegts_h264_au_reset(d);
            d->h264_need_idr = 1;
            return 0;
        }
        if (codec == ZMS_CODEC_H265 || codec == ZMS_CODEC_H266) {
            return 0;
        }
    }

    d->es_frames++;

    if (codec == ZMS_CODEC_H264) {
        mpegts_cache_ps_from_es(d, (const uint8_t *)data, bytes, "ES frame");
        mpegts_cache_annexb_ps(d, (const uint8_t *)data, bytes);
        mpegts_h264_on_es(d, (const uint8_t *)data, bytes, pts_90k, dts_90k, flags);
        return 0;
    }

    {
        uint32_t pts_ms = zms_mpegts_90k_to_ms(pts_90k);
        uint32_t dts_ms = zms_mpegts_90k_to_ms(dts_90k);
        if (dts_ms == 0 && pts_ms != 0) {
            dts_ms = pts_ms;
        }
        if (pts_ms == 0 && dts_ms != 0) {
            pts_ms = dts_ms;
        }
        mpegts_emit_es_frame(d, codec, (const uint8_t *)data, bytes, pts_ms, dts_ms, flags);
    }
    return 0;
}

zms_mpegts_demuxer *zms_mpegts_demuxer_create(const zms_mpegts_demuxer_opts *opts)
{
    zms_mpegts_demuxer *d;
    struct ts_demuxer_notify_t notify;

    if (!opts || !opts->on_frame) {
        return NULL;
    }
    d = (zms_mpegts_demuxer *)calloc(1, sizeof(*d));
    if (!d) {
        return NULL;
    }
    d->opts = *opts;
    zms_frame_init(&d->frame);
    d->ts = ts_demuxer_create(mpegts_on_packet, d);
    if (!d->ts) {
        free(d);
        return NULL;
    }
    memset(&notify, 0, sizeof(notify));
    notify.onstream = mpegts_on_ts_stream;
    ts_demuxer_set_notify(d->ts, &notify, d);
    return d;
}

void zms_mpegts_demuxer_destroy(zms_mpegts_demuxer *d)
{
    if (!d) {
        return;
    }
    if (d->ts) {
        ts_demuxer_destroy(d->ts);
    }
    zms_frame_clear(&d->frame);
    free(d);
}

static ztk_err_t mpegts_feed_ts_packet(zms_mpegts_demuxer *d, const uint8_t *pkt)
{
    if (!d || !d->ts || !pkt) {
        return ZTK_ERR_INVALID;
    }
    if (pkt[0] != MPEGTS_SYNC_BYTE) {
        return ZTK_OK;
    }
    mpegts_sniff_ts_packet_payload(d, pkt);
    (void)ts_demuxer_input(d->ts, pkt, MPEGTS_PKT_SIZE);
    d->ts_pkts++;
    if (!d->logged_no_ps && d->ts_pkts >= 50 && !d->h264_ps_applied) {
        d->logged_no_ps = 1;
        ztk_warn("[mpegts] no_sps_pps ts_pkts=%u sniff_sps=%u sniff_pps=%u es=%u", d->ts_pkts,
                 (unsigned)d->sniff_sps_len, (unsigned)d->sniff_pps_len, d->es_frames);
    }
    if (!d->logged_no_es && d->ts_pkts >= 200 && d->es_frames == 0) {
        d->logged_no_es = 1;
        ztk_warn("[mpegts] no_es ts_pkts=%u sniff_sps=%u sniff_pps=%u", d->ts_pkts,
                 (unsigned)d->sniff_sps_len, (unsigned)d->sniff_pps_len);
    }
    return ZTK_OK;
}

static void mpegts_seg_drop_prefix(zms_mpegts_demuxer *d, size_t n)
{
    if (!d || n == 0) {
        return;
    }
    if (n >= d->seg_len) {
        d->seg_len = 0;
        return;
    }
    memmove(d->seg, d->seg + n, d->seg_len - n);
    d->seg_len -= n;
}

static void mpegts_seg_resync(zms_mpegts_demuxer *d)
{
    const uint8_t *sync;

    if (!d || d->seg_len == 0) {
        return;
    }
    sync = (const uint8_t *)memchr(d->seg, MPEGTS_SYNC_BYTE, d->seg_len);
    if (!sync) {
        if (d->seg_len > MPEGTS_PKT_SIZE * 4) {
            d->seg_len = 0;
        }
        return;
    }
    if (sync > d->seg) {
        mpegts_seg_drop_prefix(d, (size_t)(sync - d->seg));
    }
}

static int mpegts_packet_confirmed(const uint8_t *buf, size_t len, size_t off)
{
    if (off + MPEGTS_PKT_SIZE > len || buf[off] != MPEGTS_SYNC_BYTE) {
        return 0;
    }
    if (off + MPEGTS_PKT_SIZE * 2 <= len) {
        return buf[off + MPEGTS_PKT_SIZE] == MPEGTS_SYNC_BYTE;
    }
    return 0;
}

static ztk_err_t mpegts_seg_feed_packets(zms_mpegts_demuxer *d)
{
    size_t off = 0;

    if (!d) {
        return ZTK_ERR_INVALID;
    }

    mpegts_seg_resync(d);

    while (off + MPEGTS_PKT_SIZE <= d->seg_len) {
        if (d->seg[off] != MPEGTS_SYNC_BYTE) {
            const uint8_t *sync =
                (const uint8_t *)memchr(d->seg + off + 1, MPEGTS_SYNC_BYTE, d->seg_len - off - 1);
            if (!sync) {
                if (d->seg_len - off > MPEGTS_PKT_SIZE * 4) {
                    mpegts_seg_drop_prefix(d, off + 1);
                }
                break;
            }
            off = (size_t)(sync - d->seg);
            continue;
        }
        if (!mpegts_packet_confirmed(d->seg, d->seg_len, off)) {
            break;
        }
        (void)mpegts_feed_ts_packet(d, d->seg + off);
        off += MPEGTS_PKT_SIZE;
    }
    if (off > 0) {
        mpegts_seg_drop_prefix(d, off);
    }
    return ZTK_OK;
}

static void mpegts_seg_feed_tail(zms_mpegts_demuxer *d)
{
    if (!d || d->seg_len != MPEGTS_PKT_SIZE) {
        return;
    }
    if (d->seg[0] == MPEGTS_SYNC_BYTE) {
        (void)mpegts_feed_ts_packet(d, d->seg);
    }
    d->seg_len = 0;
}

ztk_err_t zms_mpegts_demuxer_feed(zms_mpegts_demuxer *d, const uint8_t *data, size_t len)
{
    if (!d || !d->ts || !data || len == 0) {
        return ZTK_ERR_INVALID;
    }

    if (len == MPEGTS_PKT_SIZE && data[0] == MPEGTS_SYNC_BYTE) {
        (void)mpegts_feed_ts_packet(d, data);
        return ZTK_OK;
    }

    /* SRT 常见：7×188 对齐块、seg 空——直接喂入，勿 resync/CC 误判间隙。 */
    if (d->seg_len == 0 && len >= MPEGTS_PKT_SIZE && (len % MPEGTS_PKT_SIZE) == 0 &&
        data[0] == MPEGTS_SYNC_BYTE) {
        size_t off = 0;

        while (off + MPEGTS_PKT_SIZE <= len) {
            if (data[off] != MPEGTS_SYNC_BYTE) {
                break;
            }
            (void)mpegts_feed_ts_packet(d, data + off);
            off += MPEGTS_PKT_SIZE;
        }
        if (off == len) {
            return ZTK_OK;
        }
    }

    if (d->seg_len + len > sizeof(d->seg)) {
        mpegts_seg_resync(d);
        if (d->seg_len + len > sizeof(d->seg)) {
            static int logged_overflow;
            if (!logged_overflow++) {
                ztk_warn("[mpegts] seg_overflow have=%u add=%u", (unsigned)d->seg_len,
                         (unsigned)len);
            }
            d->seg_len = 0;
        }
    }
    if (d->seg_len + len > sizeof(d->seg)) {
        return ZTK_ERR_BUFFER_TOO_SMALL;
    }

    memcpy(d->seg + d->seg_len, data, len);
    d->seg_len += len;
    return mpegts_seg_feed_packets(d);
}

ztk_err_t zms_mpegts_demuxer_flush(zms_mpegts_demuxer *d)
{
    if (!d || !d->ts) {
        return ZTK_ERR_INVALID;
    }
    mpegts_seg_feed_tail(d);
    (void)ts_demuxer_flush(d->ts);
    mpegts_h264_au_emit(d);
    return ZTK_OK;
}

typedef struct {
    int unused;
} mpegts_container_ctx;

static void *mpegts_container_create(const zms_container_demux_opts *opts)
{
    (void)opts;
    return calloc(1, sizeof(mpegts_container_ctx));
}

static void mpegts_container_destroy(void *ctx)
{
    free(ctx);
}

static ztk_err_t mpegts_container_feed(void *ctx, const uint8_t *buf, size_t len)
{
    (void)ctx;
    (void)buf;
    (void)len;
    return ZTK_OK;
}

const zms_container_demuxer_ops zms_container_mpegts_demuxer_ops = {
    ZMS_CONTAINER_MPEGTS,
    "mpegts",
    mpegts_container_create,
    mpegts_container_destroy,
    mpegts_container_feed,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
};
