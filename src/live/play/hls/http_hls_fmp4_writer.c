/**
 * @file http_hls_fmp4_writer.c
 * @brief HLS 录制 fMP4（CMAF）mux 路径：轨道设置与按 codec 喂入。
 *        覆盖 VP8/VP9/AV1 视频与 AAC/Opus 音频；从 http_hls_segmenter.c 拆出，
 *        由 recorder tick 循环调度。
 *
 * Copyright (c) zero-media-server
 */
#include "live/play/hls/http_hls_segmenter_internal.h"
#include "zms/media/codec/aac/aac_over_rtp.h"
#include "zms/media/codec/aac/aac_over_rtmp.h"
#include "zms/media/codec/av1/av1_over_rtmp.h"
#include "zms/media/container/flv/flv_tag_probe.h"
#include "webm-vpx.h"
#include "ztk/util/buf.h"
#include "ztk/util/log.h"
#include <string.h>

int hls_fmp4_segment_cb(void *param, const void *data, size_t bytes, int64_t pts_ms, int64_t dts_ms,
                        int64_t duration_ms)
{
    zms_http_hls_segmenter *rec = (zms_http_hls_segmenter *)param;
    int r;

    (void)pts_ms;
    r = hls_media_segment_cb(param, data, bytes, pts_ms, dts_ms, duration_ms);
    if (r == 0 && rec) {
        uint64_t end = (uint64_t)dts_ms + (uint64_t)(duration_ms > 0 ? duration_ms : 0);

        if (end < rec->last_mux_dts_ms) {
            end = rec->last_mux_dts_ms;
        }
        rec->fmp4_seg_origin_dts_ms = (uint32_t)end;
    }
    return r;
}

static int hls_fmp4_seg_duration_ms(const zms_http_hls_segmenter *rec)
{
    int64_t ms;

    if (!rec) {
        return 2000;
    }
    ms = (int64_t)(rec->seg_duration_sec * 1000.f);
    return ms > 0 ? (int)ms : 2000;
}

static void hls_fmp4_flush_at(zms_http_hls_segmenter *rec, uint32_t dts_ms)
{
    if (!rec || !rec->fmp4_mux || !rec->video_armed || rec->fmp4_video_track < 0) {
        return;
    }
    (void)zms_hls_fmp4_write_frame(rec->fmp4_mux, rec->fmp4_video_track, NULL, 0, (int64_t)dts_ms,
                                   (int64_t)dts_ms, ZMS_HLS_FMP4_FLAG_SEGMENT_DISABLE);
    rec->fmp4_seg_origin_dts_ms = dts_ms;
}

static void hls_fmp4_maybe_flush_on_key(zms_http_hls_segmenter *rec, uint32_t rel_dts_ms,
                                        int keyframe)
{
    uint32_t dur_ms;

    if (!rec || !keyframe) {
        return;
    }
    dur_ms = (uint32_t)hls_fmp4_seg_duration_ms(rec);
    if (rel_dts_ms < rec->fmp4_seg_origin_dts_ms + dur_ms) {
        return;
    }
    hls_fmp4_flush_at(rec, rel_dts_ms);
}

/** 首个 m3u8 请求时强制落盘，避免 live 起播等满 2s 时间轴才出首片。 */
void hls_fmp4_serve_flush(zms_http_hls_segmenter *rec)
{
    if (!rec || !rec->fmp4_mux || !rec->video_armed || rec->fmp4_video_track < 0 || !rec->maker) {
        return;
    }
    if (zms_http_hls_playlist_segment_count(rec->maker) > 0) {
        return;
    }
    if ((uint32_t)rec->last_mux_dts_ms <= rec->fmp4_seg_origin_dts_ms) {
        return;
    }
    hls_fmp4_flush_at(rec, (uint32_t)rec->last_mux_dts_ms);
}

static void hls_fmp4_build_opus_extra(int channels, int sample_rate, uint8_t *out, size_t cap,
                                      size_t *out_len)
{
    uint32_t sr;

    if (!out || cap < 19 || !out_len) {
        return;
    }
    memset(out, 0, 19);
    memcpy(out, "OpusHead", 8);
    out[8] = 1;
    out[9] = (uint8_t)(channels > 0 ? channels : 2);
    out[10] = 0x38;
    out[11] = 0x01;
    sr = sample_rate > 0 ? (uint32_t)sample_rate : 48000u;
    out[12] = (uint8_t)(sr & 0xff);
    out[13] = (uint8_t)((sr >> 8) & 0xff);
    out[14] = (uint8_t)((sr >> 16) & 0xff);
    out[15] = (uint8_t)((sr >> 24) & 0xff);
    *out_len = 19;
}

static int hls_aac_is_adts(const uint8_t *es, size_t es_len)
{
    return es && es_len >= 7 && es[0] == 0xff && (es[1] & 0xf0) == 0xf0;
}

static uint32_t hls_fmp4_aac_au_duration_ms(const zms_http_hls_segmenter *rec)
{
    int sr;

    if (!rec || !rec->have_aac) {
        return 23u;
    }
    sr = zms_aac_config_sample_rate(&rec->aac);
    if (sr <= 0) {
        sr = 48000;
    }
    return (1024u * 1000u + (uint32_t)sr - 1u) / (uint32_t)sr;
}

static void hls_fmp4_maybe_init(zms_http_hls_segmenter *rec)
{
    uint8_t init[8192];
    int init_len;
    ztk_buf *buf;

    if (!rec || rec->fmp4_init_done || !rec->fmp4_mux || !rec->maker || rec->fmp4_video_track < 0) {
        return;
    }
    if (rec->enable_audio && rec->fmp4_audio_track < 0 && rec->src && rec->src->has_audio &&
        rec->src->audio.ready &&
        (rec->src->audio.codec == ZMS_CODEC_OPUS || rec->src->audio.codec == ZMS_CODEC_AAC)) {
        return;
    }
    init_len = zms_hls_fmp4_init_segment(rec->fmp4_mux, init, sizeof(init));
    if (init_len <= 0) {
        return;
    }
    buf = ztk_buf_alloc((size_t)init_len);
    if (!buf) {
        return;
    }
    memcpy((void *)ztk_buf_data(buf), init, (size_t)init_len);
    ztk_buf_set_len(buf, (size_t)init_len);
    if (zms_http_hls_playlist_store_init_segment(rec->maker, buf) != ZTK_OK) {
        ztk_buf_unref(buf);
        return;
    }
    rec->fmp4_init_done = 1;
    ztk_info("HLS recorder: fMP4 init segment ready len=%d", init_len);
}

static void hls_fmp4_ensure_vpx_track(zms_http_hls_segmenter *rec, zms_codec_id vc,
                                      const uint8_t *keyframe, size_t len)
{
    uint8_t vpxc[32];
    struct webm_vpx_t vpx;
    int w = 0;
    int h = 0;
    int n;

    if (!rec || !rec->fmp4_mux || rec->fmp4_video_track >= 0 || !keyframe || len == 0) {
        return;
    }
    if (vc != ZMS_CODEC_VP8 && vc != ZMS_CODEC_VP9) {
        return;
    }
    memset(&vpx, 0, sizeof(vpx));
    if (vc == ZMS_CODEC_VP9) {
        if (webm_vpx_codec_configuration_record_from_vp9(&vpx, &w, &h, keyframe, len) < 0) {
            return;
        }
    } else if (webm_vpx_codec_configuration_record_from_vp8(&vpx, &w, &h, keyframe, len) < 0) {
        return;
    }
    n = webm_vpx_codec_configuration_record_save(&vpx, vpxc, sizeof(vpxc));
    if (n <= 0) {
        return;
    }
    rec->fmp4_video_track = zms_hls_fmp4_add_video(rec->fmp4_mux, vc, w > 0 ? w : 640,
                                                   h > 0 ? h : 480, vpxc, (size_t)n);
    if (rec->fmp4_video_track >= 0) {
        rec->video_codec = vc;
        rec->sent_video_cfg = 1;
        ztk_info("HLS recorder: %s fMP4 video track ready", zms_codec_name(vc));
        hls_fmp4_maybe_init(rec);
    }
}

static void hls_fmp4_ensure_av1_track(zms_http_hls_segmenter *rec, const uint8_t *obu, size_t len)
{
    uint8_t av1c[2048];
    int w = 0;
    int h = 0;
    int n;

    if (!rec || !rec->fmp4_mux || rec->fmp4_video_track >= 0 || !obu || len == 0) {
        return;
    }
    n = zms_av1_extradata_from_obu(obu, len, av1c, sizeof(av1c), &w, &h);
    if (n <= 0) {
        return;
    }
    rec->fmp4_video_track = zms_hls_fmp4_add_video(rec->fmp4_mux, ZMS_CODEC_AV1, w > 0 ? w : 640,
                                                   h > 0 ? h : 480, av1c, (size_t)n);
    if (rec->fmp4_video_track >= 0) {
        rec->video_codec = ZMS_CODEC_AV1;
        rec->sent_video_cfg = 1;
        ztk_info("HLS recorder: AV1 fMP4 video track ready");
        hls_fmp4_maybe_init(rec);
    }
}

void hls_fmp4_ensure_aac_track(zms_http_hls_segmenter *rec, const uint8_t *cfg, size_t clen)
{
    const uint8_t *asc = NULL;
    size_t asc_len = 0;
    int sr = 44100;
    int ch = 2;
    int track;

    if (!rec || !rec->fmp4_mux || rec->fmp4_audio_track >= 0 || !cfg || clen < 4) {
        return;
    }
    if (zms_flv_tag_audio_codec(cfg, clen) != ZMS_CODEC_AAC) {
        return;
    }
    if (!zms_rtmp_aac_extradata(cfg, clen, &asc, &asc_len) || !asc || asc_len == 0) {
        return;
    }
    zms_aac_config_set_defaults(&rec->aac, 44100, 2);
    if (!zms_aac_config_load_asc(&rec->aac, asc, asc_len)) {
        return;
    }
    rec->have_aac = 1;
    sr = zms_aac_config_sample_rate(&rec->aac);
    if (sr <= 0) {
        sr = rec->aac_sr > 0 ? rec->aac_sr : 48000;
    }
    ch = zms_aac_config_channels(&rec->aac);
    if (ch <= 0) {
        ch = 2;
    }
    track = zms_hls_fmp4_add_audio(rec->fmp4_mux, ZMS_CODEC_AAC, ch, 16, sr, asc, asc_len);
    if (track >= 0) {
        rec->fmp4_audio_track = track;
        rec->sent_audio_cfg = 1;
        ztk_info("HLS recorder: AAC fMP4 audio track ready rate=%d ch=%d", sr, ch);
        hls_fmp4_maybe_init(rec);
    }
}

void hls_fmp4_ensure_opus_track(zms_http_hls_segmenter *rec)
{
    uint8_t opus_head[32];
    size_t head_len = 0;
    int ch = 2;
    int sr = 48000;

    if (!rec || !rec->fmp4_mux || rec->fmp4_audio_track >= 0 || !rec->enable_audio) {
        return;
    }
    if (rec->src && rec->src->audio.ready) {
        ch = rec->src->audio.channels > 0 ? rec->src->audio.channels : 2;
        sr = rec->src->audio.sample_rate > 0 ? rec->src->audio.sample_rate : 48000;
    }
    hls_fmp4_build_opus_extra(ch, sr, opus_head, sizeof(opus_head), &head_len);
    if (head_len == 0) {
        return;
    }
    rec->fmp4_audio_track =
        zms_hls_fmp4_add_audio(rec->fmp4_mux, ZMS_CODEC_OPUS, ch, 16, sr, opus_head, head_len);
    if (rec->fmp4_audio_track >= 0) {
        rec->sent_audio_cfg = 1;
        ztk_info("HLS recorder: Opus fMP4 audio track ready rate=%d ch=%d", sr, ch);
        hls_fmp4_maybe_init(rec);
    }
}

void hls_fmp4_feed_vpx(zms_http_hls_segmenter *rec, zms_codec_id vc, const uint8_t *es, size_t len,
                       uint32_t dts_ms, int keyframe)
{
    uint32_t rel_dts_ms;
    int flags;

    if (!rec || !rec->fmp4_mux || !es || len == 0) {
        return;
    }
    if (keyframe) {
        hls_fmp4_ensure_vpx_track(rec, vc, es, len);
    }
    if (rec->fmp4_video_track < 0) {
        return;
    }
    if (!rec->video_armed) {
        if (!keyframe) {
            return;
        }
        rec->video_armed = 1;
        zms_mux_av_timeline_lock_origin(&rec->mux_av, dts_ms);
        rec->fmp4_seg_origin_dts_ms = hls_mux_dts_ms(rec, ZMS_TRACK_VIDEO, dts_ms);
        ztk_info("HLS recorder: first %s key dts_ms=%u (fMP4)", zms_codec_name(vc),
                 (unsigned)dts_ms);
    }
    rel_dts_ms = hls_mux_dts_ms(rec, ZMS_TRACK_VIDEO, dts_ms);
    rec->last_mux_dts_ms = rel_dts_ms;
    rec->last_video_dts_ms = rel_dts_ms;
    flags = ZMS_HLS_FMP4_FLAG_SEGMENT_DISABLE;
    if (keyframe) {
        flags |= ZMS_HLS_FMP4_FLAG_KEYFRAME;
    }
    (void)zms_hls_fmp4_write_frame(rec->fmp4_mux, rec->fmp4_video_track, es, len,
                                   (int64_t)rel_dts_ms, (int64_t)rel_dts_ms, flags);
    hls_fmp4_maybe_flush_on_key(rec, rel_dts_ms, keyframe);
}

typedef struct {
    zms_http_hls_segmenter *rec;
    uint32_t base_dts_ms;
    unsigned idx;
    uint32_t au_dur_ms;
} hls_fmp4_aac_ctx;

static int hls_fmp4_feed_aac_au(const uint8_t *au, size_t len, void *user)
{
    hls_fmp4_aac_ctx *ctx = (hls_fmp4_aac_ctx *)user;
    uint32_t rel_dts_ms;
    const uint8_t *raw = au;
    size_t raw_len = len;
    uint32_t ats;

    if (!ctx || !ctx->rec || !au || len == 0) {
        return -1;
    }
    if (zms_aac_es_to_raw(au, len, &raw, &raw_len) != ZTK_OK || !raw || raw_len == 0) {
        return -1;
    }
    ats = ctx->base_dts_ms + ctx->idx * ctx->au_dur_ms;
    if (ctx->rec->last_video_dts_ms > 0 && ats > ctx->rec->last_video_dts_ms + 4000u) {
        return 0;
    }
    rel_dts_ms = hls_mux_dts_ms(ctx->rec, ZMS_TRACK_AUDIO, ats);
    ctx->rec->last_mux_dts_ms = rel_dts_ms;
    (void)zms_hls_fmp4_write_frame(ctx->rec->fmp4_mux, ctx->rec->fmp4_audio_track, raw, raw_len,
                                   (int64_t)rel_dts_ms, (int64_t)rel_dts_ms,
                                   ZMS_HLS_FMP4_FLAG_SEGMENT_DISABLE);
    ctx->idx++;
    return 0;
}

void hls_fmp4_feed_aac(zms_http_hls_segmenter *rec, const uint8_t *es, size_t es_len,
                       uint32_t dts_ms)
{
    hls_fmp4_aac_ctx ctx;

    if (!rec || !rec->fmp4_mux || rec->fmp4_audio_track < 0 || !rec->enable_audio ||
        !rec->video_armed || !es || es_len == 0) {
        return;
    }
    if (!rec->have_aac) {
        return;
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.rec = rec;
    ctx.base_dts_ms = dts_ms;
    ctx.au_dur_ms = hls_fmp4_aac_au_duration_ms(rec);
    if (hls_aac_is_adts(es, es_len)) {
        (void)zms_aac_es_foreach_frame(es, es_len, hls_fmp4_feed_aac_au, &ctx);
    } else {
        (void)hls_fmp4_feed_aac_au(es, es_len, &ctx);
    }
}

void hls_fmp4_feed_av1(zms_http_hls_segmenter *rec, const uint8_t *es, size_t len, uint32_t dts_ms,
                       int keyframe)
{
    uint32_t rel_dts_ms;
    int flags;

    if (!rec || !rec->fmp4_mux || !es || len == 0) {
        return;
    }
    if (!keyframe) {
        zms_gop_slot slot;
        memset(&slot, 0, sizeof(slot));
        slot.codec = ZMS_CODEC_AV1;
        slot.track = ZMS_TRACK_VIDEO;
        slot.data = (uint8_t *)es;
        slot.len = len;
        slot.keyframe = keyframe;
        zms_gop_slot_refresh_play_key(&slot);
        keyframe = slot.keyframe;
    }
    if (keyframe) {
        hls_fmp4_ensure_av1_track(rec, es, len);
    }
    if (rec->fmp4_video_track < 0) {
        return;
    }
    if (!rec->video_armed) {
        if (!keyframe) {
            return;
        }
        rec->video_armed = 1;
        zms_mux_av_timeline_lock_origin(&rec->mux_av, dts_ms);
        rec->fmp4_seg_origin_dts_ms = hls_mux_dts_ms(rec, ZMS_TRACK_VIDEO, dts_ms);
        ztk_info("HLS recorder: first AV1 key dts_ms=%u (fMP4)", (unsigned)dts_ms);
    }
    rel_dts_ms = hls_mux_dts_ms(rec, ZMS_TRACK_VIDEO, dts_ms);
    rec->last_mux_dts_ms = rel_dts_ms;
    rec->last_video_dts_ms = rel_dts_ms;
    flags = ZMS_HLS_FMP4_FLAG_SEGMENT_DISABLE;
    if (keyframe) {
        flags |= ZMS_HLS_FMP4_FLAG_KEYFRAME;
    }
    (void)zms_hls_fmp4_write_frame(rec->fmp4_mux, rec->fmp4_video_track, es, len,
                                   (int64_t)rel_dts_ms, (int64_t)rel_dts_ms, flags);
    hls_fmp4_maybe_flush_on_key(rec, rel_dts_ms, keyframe);
}

void hls_fmp4_feed_opus(zms_http_hls_segmenter *rec, const uint8_t *es, size_t len, uint32_t dts_ms)
{
    uint32_t rel_dts_ms;

    if (!rec || !rec->fmp4_mux || !rec->enable_audio || !es || len == 0) {
        return;
    }
    hls_fmp4_ensure_opus_track(rec);
    if (rec->fmp4_audio_track < 0 || !rec->video_armed) {
        return;
    }
    rel_dts_ms = hls_mux_dts_ms(rec, ZMS_TRACK_AUDIO, dts_ms);
    rec->last_mux_dts_ms = rel_dts_ms;
    (void)zms_hls_fmp4_write_frame(rec->fmp4_mux, rec->fmp4_audio_track, es, len,
                                   (int64_t)rel_dts_ms, (int64_t)rel_dts_ms,
                                   ZMS_HLS_FMP4_FLAG_SEGMENT_DISABLE);
}
