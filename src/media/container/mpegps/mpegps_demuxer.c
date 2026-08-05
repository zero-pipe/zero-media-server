#include "zms/media/container/mpegps/mpegps_demuxer.h"
#include "zms/media/codec/codec_id.h"
#include "zms/media/codec/h264/h264_es.h"
#include "zms/media/codec/h265/h265_es.h"
#include "zms/engine/media_clock.h"
#include "zms/engine/media/media_limits.h"
#include "mpeg-proto.h"
#include "mpeg-ps.h"
#include "ztk/util/log.h"
#include <stdlib.h>
#include <string.h>

#define MPEGPS_ES_CAP ZMS_MPEGTS_AU_MAX

struct zms_mpegps_demuxer {
    zms_mpegps_demuxer_opts opts;
    struct ps_demuxer_t *ps;
    zms_frame frame;
    uint8_t es_buf[MPEGPS_ES_CAP];
    uint8_t sniff_sps[512];
    size_t sniff_sps_len;
    uint8_t sniff_pps[256];
    size_t sniff_pps_len;
    int h264_ps_applied;
};

static zms_codec_id mpegps_psi_to_codec(int psi)
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
    default:
        return ZMS_CODEC_INVALID;
    }
}

static void mpegps_try_apply_h264_ps(zms_mpegps_demuxer *d, const uint8_t *sps, size_t sps_len,
                                     const uint8_t *pps, size_t pps_len)
{
    if (!d || !d->opts.on_h264_ps || !sps || !pps || sps_len == 0 || pps_len == 0) {
        return;
    }
    d->opts.on_h264_ps(sps, sps_len, pps, pps_len, d->opts.user);
    d->h264_ps_applied = 1;
}

static void mpegps_try_apply_cached_ps(zms_mpegps_demuxer *d)
{
    if (!d || !d->sniff_sps_len || !d->sniff_pps_len) {
        return;
    }
    mpegps_try_apply_h264_ps(d, d->sniff_sps, d->sniff_sps_len, d->sniff_pps, d->sniff_pps_len);
}

static void mpegps_cache_ps_from_es(zms_mpegps_demuxer *d, const uint8_t *data, size_t len)
{
    const uint8_t *sps = NULL, *pps = NULL;
    size_t sps_len = 0, pps_len = 0;

    if (!d || !data || len == 0) {
        return;
    }
    if (!zms_h264_es_extract_sps_pps(data, len, &sps, &sps_len, &pps, &pps_len)) {
        return;
    }
    if (sps_len == 0 || sps_len > sizeof(d->sniff_sps) || pps_len == 0 ||
        pps_len > sizeof(d->sniff_pps)) {
        return;
    }
    /* SPS/PPS 未变化则跳过；变化时再交给 ingest（由其判断是否采纳） */
    if (d->h264_ps_applied && sps_len == d->sniff_sps_len && pps_len == d->sniff_pps_len &&
        memcmp(d->sniff_sps, sps, sps_len) == 0 && memcmp(d->sniff_pps, pps, pps_len) == 0) {
        return;
    }
    memcpy(d->sniff_sps, sps, sps_len);
    d->sniff_sps_len = sps_len;
    memcpy(d->sniff_pps, pps, pps_len);
    d->sniff_pps_len = pps_len;
    mpegps_try_apply_cached_ps(d);
}

static void mpegps_emit_es(zms_mpegps_demuxer *d, zms_codec_id codec, const uint8_t *data,
                           size_t bytes, uint32_t pts_ms, uint32_t dts_ms, int flags)
{
    int key = 0;

    if (!d || !d->opts.on_frame || !data || bytes == 0 || codec == ZMS_CODEC_INVALID) {
        return;
    }
    if (bytes > sizeof(d->es_buf)) {
        /* 超过静态缓冲上限（ZMS_MPEGTS_AU_MAX = 512KB）；4K/8K IDR 帧理论上不超过此值，
         * 但若实际命中则丢帧并记录 warn，以便排查编码器配置异常或带宽攻击 */
        ztk_warn("[mpegps] es frame too large bytes=%zu cap=%zu codec=%d pts_ms=%u dropped", bytes,
                 sizeof(d->es_buf), (int)codec, pts_ms);
        return;
    }

    memcpy(d->es_buf, data, bytes);
    if (codec == ZMS_CODEC_H264) {
        mpegps_cache_ps_from_es(d, d->es_buf, bytes);
        key = (flags & MPEG_FLAG_IDR_FRAME) ? 1 : zms_h264_annexb_is_idr(d->es_buf, bytes);
    } else if (codec == ZMS_CODEC_H265) {
        key = (flags & MPEG_FLAG_IDR_FRAME) ? 1 : zms_h265_annexb_is_sync_key(d->es_buf, bytes);
    } else {
        key = (flags & MPEG_FLAG_IDR_FRAME) ? 1 : 0;
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
    d->frame.keyframe = key;
    d->frame.config_frame = 0;
    d->frame.drop_able = 0;
    d->frame.data = d->es_buf;
    d->frame.size = bytes;
    d->frame.owned = 0;
    d->opts.on_frame(&d->frame, d->opts.user);
}

static int mpegps_on_packet(void *param, int stream, int codecid, int flags, int64_t pts,
                            int64_t dts, const void *data, size_t bytes)
{
    zms_mpegps_demuxer *d = (zms_mpegps_demuxer *)param;
    zms_codec_id codec;
    uint32_t pts_ms;
    uint32_t dts_ms;

    (void)stream;
    if (!d || !data || bytes == 0) {
        return 0;
    }
    codec = mpegps_psi_to_codec(codecid);
    if (codec == ZMS_CODEC_INVALID) {
        return 0;
    }
    pts_ms = zms_mpegts_90k_to_ms(pts);
    dts_ms = zms_mpegts_90k_to_ms(dts);
    mpegps_emit_es(d, codec, (const uint8_t *)data, bytes, pts_ms, dts_ms, flags);
    return 0;
}

zms_mpegps_demuxer *zms_mpegps_demuxer_create(const zms_mpegps_demuxer_opts *opts)
{
    zms_mpegps_demuxer *d;

    if (!opts || !opts->on_frame) {
        return NULL;
    }
    d = (zms_mpegps_demuxer *)calloc(1, sizeof(*d));
    if (!d) {
        return NULL;
    }
    d->opts = *opts;
    d->ps = ps_demuxer_create(mpegps_on_packet, d);
    if (!d->ps) {
        free(d);
        return NULL;
    }
    return d;
}

void zms_mpegps_demuxer_destroy(zms_mpegps_demuxer *d)
{
    if (!d) {
        return;
    }
    if (d->ps) {
        ps_demuxer_destroy(d->ps);
    }
    free(d);
}

ztk_err_t zms_mpegps_demuxer_feed(zms_mpegps_demuxer *d, const uint8_t *data, size_t len)
{
    int n;

    if (!d || !d->ps || !data || len == 0) {
        return ZTK_ERR_INVALID;
    }
    n = ps_demuxer_input(d->ps, data, len);
    if (n < 0) {
        return ZTK_ERR_INVALID;
    }
    return ZTK_OK;
}

ztk_err_t zms_mpegps_demuxer_flush(zms_mpegps_demuxer *d)
{
    (void)d;
    return ZTK_OK;
}
