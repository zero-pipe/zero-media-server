#include "zms/live/play/dash/http_dash_segmenter.h"
#include "zms/live/play/dash/http_dash_playlist.h"
#include "zms/live/play/hls/http_hls_sidecar.h"
#include "zms/egress/egress_sidecar_param_sets.h"
#include "zms/media/codec/aac/aac_over_rtmp.h"
#include "zms/media/codec/aac/aac_over_rtp.h"
#include "zms/media/codec/vpx/vpx_over_rtmp.h"
#include "zms/media/codec/av1/av1_over_rtmp.h"
#include "zms/media/codec/codec_id.h"
#include "zms/util/buf_pool.h"
#include "zms/media/codec/h264/h264_es.h"
#include "zms/media/codec/h264/h264_config.h"
#include "zms/media/codec/h265/h265_es.h"
#include "zms/media/codec/h265/h265_config.h"
#include "zms/engine/media_clock.h"
#include "zms/engine/gop/gop_queue.h"
#include "zms/egress/egress_pacing.h"
#include "zms/egress/egress_segment_recorder.h"
#include "zms/engine/stream/stream_hub.h"
#include "zms/media/codec/h264/h264_over_rtmp.h"
#include "zms/media/container/flv/flv_tag_probe.h"
#include "zms/media/codec/aac/aac_config.h"
#include "dash-mpd.h"
#include "mov-format.h"
#include "webm-vpx.h"
#include "ztk/util/log.h"
#include "ztk/util/timer.h"
#include "ztk/thread/sync.h"
#include <stdlib.h>
#include <string.h>

struct zms_http_dash_segmenter {
    zms_media_source *src;
    zms_http_dash_playlist *maker;
    dash_mpd_t *mpd;
    zms_gop_reader *reader;
    ztk_mutex *mu;
    ztk_timer *timer;
    ztk_poller *timer_poller;
    int http_serve_depth;
    int closing;
    int enable_audio;
    int sent_video_cfg;
    int sent_audio_cfg;
    int video_armed;
    int adapt_video;
    int adapt_audio;
    zms_avc_config avc;
    zms_hevc_config hevc;
    zms_aac_config aac;
    int have_aac;
    uint8_t *mux_buf;
    size_t mux_buf_cap;
    uint8_t *annexb;
    size_t annexb_cap;
    zms_sidecar_param_sets params;
    zms_mux_av_timeline mux_av;
    zms_codec_id video_codec;
    uint32_t last_video_ring_dts_ms;
};

void zms_http_dash_segmenter_default_opts(zms_http_dash_segmenter_opts *opts)
{
    if (!opts) {
        return;
    }
    opts->segment_duration_sec = 2.f;
    opts->segment_count = 24;
    opts->enable_audio = 1;
}

static void dash_segmenter_tick_nolock(zms_http_dash_segmenter *rec);

static void dash_segmenter_tick_cb(void *user)
{
    zms_http_dash_segmenter *rec = (zms_http_dash_segmenter *)user;

    if (!rec || !rec->mu) {
        return;
    }
    ztk_mutex_lock(rec->mu);
    if (!rec->closing) {
        dash_segmenter_tick_nolock(rec);
    }
    ztk_mutex_unlock(rec->mu);
}

static uint32_t dash_mux_dts_ms(zms_http_dash_segmenter *rec, zms_track_type track,
                                uint32_t ring_dts_ms)
{
    return zms_mux_av_timeline_pts(&rec->mux_av, track, ring_dts_ms);
}

static int dash_aac_is_adts(const uint8_t *es, size_t es_len)
{
    return es && es_len >= 7 && es[0] == 0xff && (es[1] & 0xf0) == 0xf0;
}

static void dash_feed_video_cfg(zms_http_dash_segmenter *rec, const uint8_t *cfg, size_t clen)
{
    const uint8_t *extra = NULL;
    size_t extra_len = 0;
    int w = 0;
    int h = 0;

    if (!rec || !rec->mpd || !cfg || clen < 2) {
        return;
    }
    rec->video_codec = zms_flv_video_config_codec(cfg, clen);
    if (rec->video_codec == ZMS_CODEC_INVALID && rec->src &&
        rec->src->video.codec != ZMS_CODEC_INVALID) {
        rec->video_codec = rec->src->video.codec;
    }
    if (rec->video_codec == ZMS_CODEC_H264 &&
        zms_rtmp_avc_extradata(cfg, clen, &extra, &extra_len) && extra && extra_len > 0) {
        if (rec->src) {
            w = (int)rec->src->video.width;
            h = (int)rec->src->video.height;
        }
        (void)zms_avc_config_load_record(&rec->avc, extra, extra_len);
        (void)zms_sidecar_cache_rtmp_video_cfg(&rec->params, cfg, clen);
        rec->adapt_video =
            dash_mpd_add_video_adaptation_set(rec->mpd, zms_http_dash_playlist_prefix(rec->maker),
                                              MOV_OBJECT_H264, w, h, extra, extra_len);
    } else if (rec->video_codec == ZMS_CODEC_H265 &&
               zms_h265_video_config_hvcc(cfg, clen, &extra, &extra_len) && extra &&
               extra_len > 0) {
        if (rec->src) {
            w = (int)rec->src->video.width;
            h = (int)rec->src->video.height;
        }
        (void)zms_hevc_config_load_record(&rec->hevc, extra, extra_len);
        (void)zms_sidecar_cache_rtmp_video_cfg(&rec->params, cfg, clen);
        rec->adapt_video =
            dash_mpd_add_video_adaptation_set(rec->mpd, zms_http_dash_playlist_prefix(rec->maker),
                                              MOV_OBJECT_H265, w, h, extra, extra_len);
        if (rec->adapt_video >= 0) {
            ztk_info("DASH recorder: H265 adaptation set ready w=%d h=%d hvcc=%u", w, h,
                     (unsigned)extra_len);
        }
    } else if ((rec->video_codec == ZMS_CODEC_VP8 || rec->video_codec == ZMS_CODEC_VP9) &&
               clen >= 8) {
        const uint8_t *vpxc = NULL;
        size_t vpxc_len = 0;
        uint8_t mov_obj = rec->video_codec == ZMS_CODEC_VP9 ? MOV_OBJECT_VP9 : MOV_OBJECT_VP8;

        if (zms_vpx_over_rtmp_config_extradata(rec->video_codec, cfg, clen, &vpxc, &vpxc_len) &&
            vpxc && vpxc_len >= 8) {
            if (rec->src) {
                w = (int)rec->src->video.width;
                h = (int)rec->src->video.height;
            }
            rec->adapt_video = dash_mpd_add_video_adaptation_set(
                rec->mpd, zms_http_dash_playlist_prefix(rec->maker), mov_obj, w > 0 ? w : 640,
                h > 0 ? h : 480, vpxc, vpxc_len);
            if (rec->adapt_video >= 0) {
                ztk_info("DASH recorder: %s adaptation set ready w=%d h=%d",
                         zms_codec_name(rec->video_codec), w, h);
            }
        }
    } else if (rec->video_codec == ZMS_CODEC_AV1) {
        const uint8_t *av1c = NULL;
        size_t av1c_len = 0;

        if (zms_av1_over_rtmp_config_extradata(cfg, clen, &av1c, &av1c_len) && av1c &&
            av1c_len >= 4) {
            if (rec->src) {
                w = (int)rec->src->video.width;
                h = (int)rec->src->video.height;
            }
            rec->adapt_video = dash_mpd_add_video_adaptation_set(
                rec->mpd, zms_http_dash_playlist_prefix(rec->maker), MOV_OBJECT_AV1,
                w > 0 ? w : 640, h > 0 ? h : 480, av1c, av1c_len);
            if (rec->adapt_video >= 0) {
                ztk_info("DASH recorder: AV1 adaptation set ready w=%d h=%d", w, h);
            }
        }
    }
}

static int dash_build_vpx_extra(zms_codec_id vc, const uint8_t *keyframe, size_t len, uint8_t *out,
                                size_t cap, int *width, int *height)
{
    struct webm_vpx_t vpx;
    int w = 0;
    int h = 0;
    int n;

    if (!keyframe || len < 10 || !out || cap < 8) {
        return -1;
    }
    memset(&vpx, 0, sizeof(vpx));
    if (vc == ZMS_CODEC_VP9) {
        if (webm_vpx_codec_configuration_record_from_vp9(&vpx, &w, &h, keyframe, len) < 0) {
            return -1;
        }
    } else if (vc == ZMS_CODEC_VP8) {
        if (webm_vpx_codec_configuration_record_from_vp8(&vpx, &w, &h, keyframe, len) < 0) {
            return -1;
        }
    } else {
        return -1;
    }
    if (width) {
        *width = w;
    }
    if (height) {
        *height = h;
    }
    n = webm_vpx_codec_configuration_record_save(&vpx, out, cap);
    return n > 0 ? n : -1;
}

static void dash_build_opus_extra(int channels, int sample_rate, uint8_t *out, size_t cap,
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

static void dash_ensure_av1_adaptation(zms_http_dash_segmenter *rec, const uint8_t *obu, size_t len)
{
    uint8_t av1c[2048];
    int w = 0;
    int h = 0;
    int n;

    if (!rec || !rec->mpd || rec->adapt_video >= 0 || !obu || len == 0) {
        return;
    }
    n = zms_av1_extradata_from_obu(obu, len, av1c, sizeof(av1c), &w, &h);
    if (n <= 0) {
        return;
    }
    rec->video_codec = ZMS_CODEC_AV1;
    rec->adapt_video = dash_mpd_add_video_adaptation_set(
        rec->mpd, zms_http_dash_playlist_prefix(rec->maker), MOV_OBJECT_AV1, w > 0 ? w : 640,
        h > 0 ? h : 480, av1c, (size_t)n);
    if (rec->adapt_video >= 0) {
        rec->sent_video_cfg = 1;
        ztk_info("DASH recorder: AV1 adaptation set ready w=%d h=%d (keyframe)", w, h);
    }
}

static void dash_ensure_vpx_adaptation(zms_http_dash_segmenter *rec, zms_codec_id vc,
                                       const uint8_t *keyframe, size_t len)
{
    uint8_t vpxc[32];
    int w = 0;
    int h = 0;
    int n;
    uint8_t mov_obj;

    if (!rec || !rec->mpd || rec->adapt_video >= 0 || !keyframe || len == 0) {
        return;
    }
    n = dash_build_vpx_extra(vc, keyframe, len, vpxc, sizeof(vpxc), &w, &h);
    if (n <= 0) {
        return;
    }
    mov_obj = vc == ZMS_CODEC_VP9 ? MOV_OBJECT_VP9 : MOV_OBJECT_VP8;
    rec->video_codec = vc;
    rec->adapt_video = dash_mpd_add_video_adaptation_set(
        rec->mpd, zms_http_dash_playlist_prefix(rec->maker), mov_obj, w > 0 ? w : 640,
        h > 0 ? h : 480, vpxc, (size_t)n);
    if (rec->adapt_video >= 0) {
        rec->sent_video_cfg = 1;
        ztk_info("DASH recorder: %s adaptation set ready w=%d h=%d (keyframe)", zms_codec_name(vc),
                 w, h);
    }
}

static void dash_ensure_opus_adaptation(zms_http_dash_segmenter *rec)
{
    uint8_t opus_head[32];
    size_t head_len = 0;
    int ch = 2;
    int sr = 48000;

    if (!rec || !rec->mpd || rec->adapt_audio >= 0 || !rec->enable_audio) {
        return;
    }
    if (rec->src && rec->src->audio.ready) {
        ch = rec->src->audio.channels > 0 ? rec->src->audio.channels : 2;
        sr = rec->src->audio.sample_rate > 0 ? rec->src->audio.sample_rate : 48000;
    }
    dash_build_opus_extra(ch, sr, opus_head, sizeof(opus_head), &head_len);
    rec->adapt_audio =
        dash_mpd_add_audio_adaptation_set(rec->mpd, zms_http_dash_playlist_prefix(rec->maker),
                                          MOV_OBJECT_OPUS, ch, 16, sr, opus_head, head_len);
    if (rec->adapt_audio >= 0) {
        rec->sent_audio_cfg = 1;
        ztk_info("DASH recorder: Opus adaptation set ready rate=%d ch=%d", sr, ch);
    }
}

static void dash_feed_audio_cfg(zms_http_dash_segmenter *rec, const uint8_t *cfg, size_t clen)
{
    const uint8_t *asc = NULL;
    size_t asc_len = 0;
    int sr = 44100;
    int ch = 2;
    zms_codec_id ac;

    if (!rec || !rec->mpd || !cfg || clen < 2) {
        return;
    }
    ac = zms_flv_tag_audio_codec(cfg, clen);
    if (ac == ZMS_CODEC_OPUS) {
        dash_ensure_opus_adaptation(rec);
        return;
    }
    if (ac != ZMS_CODEC_AAC || clen < 4) {
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
    ch = zms_aac_config_channels(&rec->aac);
    if (ch <= 0) {
        ch = 2;
    }
    rec->adapt_audio =
        dash_mpd_add_audio_adaptation_set(rec->mpd, zms_http_dash_playlist_prefix(rec->maker),
                                          MOV_OBJECT_AAC, ch, 16, sr, asc, asc_len);
}

static void dash_feed_h264(zms_http_dash_segmenter *rec, const uint8_t *annexb, size_t len,
                           uint32_t dts_ms, int keyframe)
{
    int vcl = 0;
    int update = 0;
    int n;
    int64_t rel_dts_ms;
    int flags;
    int sync;
    int idr;

    if (!rec || rec->adapt_video < 0 || !annexb || len < 4) {
        return;
    }
    sync = keyframe || zms_h264_annexb_is_sync_key(annexb, len);
    idr = keyframe || zms_h264_annexb_is_idr(annexb, len);
    if (!rec->video_armed) {
        if (!sync) {
            return;
        }
        rec->video_armed = 1;
        zms_mux_av_timeline_lock_origin(&rec->mux_av, dts_ms);
        ztk_info("DASH recorder: first H264 IDR ts=%u", (unsigned)dts_ms);
    }
    if (!zms_buf_pool_slot_resize(&rec->mux_buf, &rec->mux_buf_cap, len + 65536u)) {
        return;
    }
    /* fMP4 参数集在 init 段；media 段勿 prepend SPS/PPS（TS/HLS 习惯，会导致 DASH 解码失败） */
    n = zms_avc_config_annexb_to_mp4(&rec->avc, annexb, len, rec->mux_buf, rec->mux_buf_cap, &vcl,
                                     &update);
    if (n <= 0) {
        return;
    }
    rel_dts_ms = (int64_t)dash_mux_dts_ms(rec, ZMS_TRACK_VIDEO, dts_ms);
    rec->last_video_ring_dts_ms = dts_ms;
    /* 仅在 IDR 切段；sync_key（含非 IDR）会导致约 500ms 碎段，ffplay 追帧卡顿 */
    flags = idr ? MOV_AV_FLAG_KEYFREAME : 0;
    (void)dash_mpd_input(rec->mpd, rec->adapt_video, rec->mux_buf, (size_t)n, rel_dts_ms,
                         rel_dts_ms, flags);
}

static void dash_feed_h265(zms_http_dash_segmenter *rec, const uint8_t *annexb, size_t len,
                           uint32_t dts_ms, int keyframe)
{
    int vcl = 0;
    int update = 0;
    int n;
    int64_t rel_dts_ms;
    int flags;
    int sync;
    int idr;
    const uint8_t *mux_ptr = annexb;
    size_t mux_len = len;

    if (!rec || rec->adapt_video < 0 || !annexb || len < 4) {
        return;
    }
    sync = keyframe || zms_h265_annexb_is_sync_key(annexb, len);
    idr = keyframe || zms_h265_annexb_is_idr(annexb, len);
    if (!rec->video_armed) {
        if (!sync) {
            return;
        }
        rec->video_armed = 1;
        zms_mux_av_timeline_lock_origin(&rec->mux_av, dts_ms);
        ztk_info("DASH recorder: first H265 sync ts=%u", (unsigned)dts_ms);
    }
    if (!zms_buf_pool_slot_resize(&rec->mux_buf, &rec->mux_buf_cap, len + 65536u)) {
        return;
    }
    if (sync && rec->params.sps_len > 0 && rec->params.pps_len > 0) {
        size_t full_len = 0;
        if (zms_h265_annexb_build_rtp_au(rec->params.vps_len ? rec->params.vps : NULL,
                                         rec->params.vps_len, rec->params.sps, rec->params.sps_len,
                                         rec->params.pps, rec->params.pps_len, annexb, len, 1,
                                         rec->annexb, rec->annexb_cap, &full_len) == ZTK_OK &&
            full_len > 0) {
            mux_ptr = rec->annexb;
            mux_len = full_len;
        }
    } else {
        size_t vcl_len = 0;
        if (zms_h265_annexb_copy_vcl(annexb, len, rec->annexb, rec->annexb_cap, &vcl_len) ==
                ZTK_OK &&
            vcl_len > 0) {
            mux_ptr = rec->annexb;
            mux_len = vcl_len;
        }
    }
    n = zms_hevc_config_annexb_to_mp4(&rec->hevc, mux_ptr, mux_len, rec->mux_buf, rec->mux_buf_cap,
                                      &vcl, &update);
    if (n <= 0) {
        return;
    }
    rel_dts_ms = (int64_t)dash_mux_dts_ms(rec, ZMS_TRACK_VIDEO, dts_ms);
    rec->last_video_ring_dts_ms = dts_ms;
    flags = idr ? MOV_AV_FLAG_KEYFREAME : 0;
    (void)dash_mpd_input(rec->mpd, rec->adapt_video, rec->mux_buf, (size_t)n, rel_dts_ms,
                         rel_dts_ms, flags);
}

typedef struct {
    zms_http_dash_segmenter *rec;
    uint32_t base_dts_ms;
    unsigned idx;
    uint32_t au_dur_ms;
} dash_aac_feed_ctx;

static uint32_t dash_aac_au_duration_ms(const zms_http_dash_segmenter *rec)
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

static int dash_feed_aac_au(const uint8_t *au, size_t len, void *user)
{
    dash_aac_feed_ctx *ctx = (dash_aac_feed_ctx *)user;
    int64_t rel_dts_ms;
    const uint8_t *raw = au;
    size_t raw_len = len;

    if (!ctx || !ctx->rec || !au || len == 0) {
        return -1;
    }
    if (zms_aac_es_to_raw(au, len, &raw, &raw_len) != ZTK_OK || !raw || raw_len == 0) {
        return -1;
    }
    {
        uint32_t ats = ctx->base_dts_ms + ctx->idx * ctx->au_dur_ms;

        /* 音频 fmp4 勿超前视频太多，否则播放 A-V 严重错位 */
        if (ctx->rec->last_video_ring_dts_ms > 0 &&
            ats > ctx->rec->last_video_ring_dts_ms + 4000u) {
            return 0;
        }
        rel_dts_ms = (int64_t)dash_mux_dts_ms(ctx->rec, ZMS_TRACK_AUDIO, ats);
    }
    (void)dash_mpd_input(ctx->rec->mpd, ctx->rec->adapt_audio, raw, raw_len, rel_dts_ms, rel_dts_ms,
                         0);
    ctx->idx++;
    return 0;
}

static void dash_feed_opus(zms_http_dash_segmenter *rec, const uint8_t *es, size_t es_len,
                           uint32_t dts_ms)
{
    int64_t rel_dts_ms;

    if (!rec || rec->adapt_audio < 0 || !rec->enable_audio || !rec->video_armed || !es ||
        es_len == 0) {
        return;
    }
    if (rec->last_video_ring_dts_ms > 0 && dts_ms > rec->last_video_ring_dts_ms + 4000u) {
        return;
    }
    rel_dts_ms = (int64_t)dash_mux_dts_ms(rec, ZMS_TRACK_AUDIO, dts_ms);
    (void)dash_mpd_input(rec->mpd, rec->adapt_audio, es, es_len, rel_dts_ms, rel_dts_ms, 0);
}

static void dash_feed_av1(zms_http_dash_segmenter *rec, const uint8_t *es, size_t len,
                          uint32_t dts_ms, int keyframe)
{
    int64_t rel_dts_ms;
    int flags;

    if (!rec || !es || len == 0) {
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
    dash_ensure_av1_adaptation(rec, es, len);
    if (rec->adapt_video < 0) {
        return;
    }
    if (!rec->video_armed) {
        if (!keyframe) {
            return;
        }
        rec->video_armed = 1;
        zms_mux_av_timeline_lock_origin(&rec->mux_av, dts_ms);
        ztk_info("DASH recorder: first AV1 key ts=%u", (unsigned)dts_ms);
    }
    rel_dts_ms = (int64_t)dash_mux_dts_ms(rec, ZMS_TRACK_VIDEO, dts_ms);
    rec->last_video_ring_dts_ms = dts_ms;
    flags = keyframe ? MOV_AV_FLAG_KEYFREAME : 0;
    (void)dash_mpd_input(rec->mpd, rec->adapt_video, es, len, rel_dts_ms, rel_dts_ms, flags);
}

static void dash_feed_vpx(zms_http_dash_segmenter *rec, zms_codec_id vc, const uint8_t *es,
                          size_t len, uint32_t dts_ms, int keyframe)
{
    int64_t rel_dts_ms;
    int flags;

    if (!rec || !es || len == 0) {
        return;
    }
    if (!keyframe) {
        zms_gop_slot slot;
        memset(&slot, 0, sizeof(slot));
        slot.codec = vc;
        slot.track = ZMS_TRACK_VIDEO;
        slot.data = (uint8_t *)es;
        slot.len = len;
        slot.keyframe = keyframe;
        zms_gop_slot_refresh_play_key(&slot);
        keyframe = slot.keyframe;
    }
    dash_ensure_vpx_adaptation(rec, vc, es, len);
    if (rec->adapt_video < 0) {
        return;
    }
    if (!rec->video_armed) {
        if (!keyframe) {
            return;
        }
        rec->video_armed = 1;
        zms_mux_av_timeline_lock_origin(&rec->mux_av, dts_ms);
        ztk_info("DASH recorder: first %s key ts=%u", zms_codec_name(vc), (unsigned)dts_ms);
    }
    rel_dts_ms = (int64_t)dash_mux_dts_ms(rec, ZMS_TRACK_VIDEO, dts_ms);
    rec->last_video_ring_dts_ms = dts_ms;
    flags = keyframe ? MOV_AV_FLAG_KEYFREAME : 0;
    (void)dash_mpd_input(rec->mpd, rec->adapt_video, es, len, rel_dts_ms, rel_dts_ms, flags);
}

static void dash_feed_aac(zms_http_dash_segmenter *rec, const uint8_t *es, size_t es_len,
                          uint32_t dts_ms)
{
    dash_aac_feed_ctx ctx;

    if (!rec || rec->adapt_audio < 0 || !rec->enable_audio || !rec->video_armed || !es ||
        es_len == 0) {
        return;
    }
    if (!rec->have_aac) {
        return;
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.rec = rec;
    ctx.base_dts_ms = dts_ms;
    ctx.au_dur_ms = dash_aac_au_duration_ms(rec);
    if (dash_aac_is_adts(es, es_len)) {
        (void)zms_aac_es_foreach_frame(es, es_len, dash_feed_aac_au, &ctx);
    } else {
        (void)dash_feed_aac_au(es, es_len, &ctx);
    }
}

zms_http_dash_segmenter *zms_http_dash_segmenter_create(zms_media_source *src,
                                                        const zms_http_dash_segmenter_opts *opts)
{
    zms_http_dash_segmenter_opts def;
    zms_http_dash_segmenter *rec;
    zms_http_dash_playlist_opts mopts;
    const zms_http_dash_segmenter_opts *o;

    if (!src || !src->gop_queue) {
        return NULL;
    }
    zms_http_dash_segmenter_default_opts(&def);
    o = opts ? opts : &def;

    rec = (zms_http_dash_segmenter *)calloc(1, sizeof(*rec));
    if (!rec) {
        return NULL;
    }

    memset(&mopts, 0, sizeof(mopts));
    mopts.segment_duration_sec = o->segment_duration_sec;
    mopts.segment_count = o->segment_count;
    mopts.prefix = src->stream;
    rec->maker = zms_http_dash_playlist_create(&mopts);
    rec->mpd = rec->maker ? zms_http_dash_playlist_mpd(rec->maker) : NULL;
    if (!rec->maker || !rec->mpd) {
        zms_http_dash_segmenter_destroy(rec);
        return NULL;
    }

    rec->src = src;
    rec->enable_audio = o->enable_audio;
    rec->adapt_video = rec->adapt_audio = -1;
    rec->mu = ztk_mutex_create(0);
    rec->reader = zms_gop_reader_attach(src->gop_queue);
    if (!rec->mu || !rec->reader) {
        zms_http_dash_segmenter_destroy(rec);
        return NULL;
    }
    zms_gop_reader_seek_live_key(rec->reader);
    zms_mux_av_timeline_reset(&rec->mux_av);
    rec->mux_buf_cap = 256 * 1024;
    rec->mux_buf = (uint8_t *)malloc(rec->mux_buf_cap);
    rec->annexb_cap = rec->mux_buf_cap;
    rec->annexb = (uint8_t *)malloc(rec->annexb_cap);
    if (!rec->mux_buf || !rec->annexb) {
        zms_http_dash_segmenter_destroy(rec);
        return NULL;
    }
    (void)zms_media_source_segment_rec_set(src, ZMS_SEGMENT_REC_DASH, rec);
    ztk_info("DASH recorder start: %s/%s", src->app, src->stream);
    return rec;
}

void zms_http_dash_segmenter_destroy(zms_http_dash_segmenter *rec)
{
    zms_media_source *src;
    zms_http_dash_playlist *maker;
    zms_gop_reader *reader;
    ztk_mutex *mu;

    if (!rec) {
        return;
    }
    if (rec->timer) {
        ztk_timer_stop(rec->timer);
        rec->timer = NULL;
    }
    src = rec->src;
    maker = rec->maker;
    reader = rec->reader;
    mu = rec->mu;
    if (mu) {
        ztk_mutex_lock(mu);
        rec->closing = 1;
        if (src) {
            (void)zms_media_source_segment_rec_set(src, ZMS_SEGMENT_REC_DASH, NULL);
        }
        rec->maker = NULL;
        rec->mpd = NULL;
        rec->reader = NULL;
        rec->mu = NULL;
        ztk_mutex_unlock(mu);
    } else if (src) {
        (void)zms_media_source_segment_rec_set(src, ZMS_SEGMENT_REC_DASH, NULL);
    }
    if (reader) {
        zms_gop_reader_detach(reader);
    }
    if (maker) {
        zms_http_dash_playlist_destroy(maker);
    }
    free(rec->mux_buf);
    free(rec->annexb);
    zms_sidecar_param_sets_clear(&rec->params);
    ztk_mutex_destroy(mu);
    free(rec);
}

zms_http_dash_playlist *zms_http_dash_segmenter_playlist(zms_http_dash_segmenter *rec)
{
    return rec ? rec->maker : NULL;
}

static ztk_poller *dash_segmenter_timer_poller(ztk_poller *fallback)
{
    ztk_poller *main_pol = zms_http_hls_main_poller();
    return main_pol ? main_pol : fallback;
}

void zms_http_dash_segmenter_bind_timer(zms_http_dash_segmenter *rec, ztk_poller *poller)
{
    ztk_poller *bind_pol;

    if (!rec) {
        return;
    }
    bind_pol = dash_segmenter_timer_poller(poller);
    if (!bind_pol) {
        return;
    }
    if (rec->mu) {
        ztk_mutex_lock(rec->mu);
    }
    rec->timer_poller = bind_pol;
    if (rec->timer) {
        ztk_timer_stop(rec->timer);
        rec->timer = NULL;
    }
    if (!rec->closing) {
        rec->timer = ztk_timer_start(bind_pol, 10, 1, dash_segmenter_tick_cb, rec);
    }
    if (rec->mu) {
        ztk_mutex_unlock(rec->mu);
    }
}

void zms_http_dash_segmenter_http_enter(zms_http_dash_segmenter *rec)
{
    if (!rec || !rec->mu) {
        return;
    }
    ztk_mutex_lock(rec->mu);
    rec->http_serve_depth++;
    ztk_mutex_unlock(rec->mu);
}

void zms_http_dash_segmenter_http_leave(zms_http_dash_segmenter *rec, ztk_poller *poller)
{
    (void)poller;
    if (!rec || !rec->mu) {
        return;
    }
    ztk_mutex_lock(rec->mu);
    if (rec->http_serve_depth > 0) {
        --rec->http_serve_depth;
    }
    ztk_mutex_unlock(rec->mu);
}

void zms_http_dash_segmenter_touch(zms_http_dash_segmenter *rec)
{
    if (rec) {
        zms_http_dash_segmenter_tick(rec);
    }
}

/** 仅在 MPD 请求时轻量推进；分片响应后不 pump，避免覆盖播放器仍请求的缓存 */
static void dash_segmenter_serve_pump_mpd(zms_http_dash_segmenter *rec)
{
    int i;

    if (!rec || !rec->mu) {
        return;
    }
    ztk_mutex_lock(rec->mu);
    if (!rec->closing) {
        for (i = 0; i < ZMS_SEGMENT_REC_SERVE_MAX_TICKS; ++i) {
            dash_segmenter_tick_nolock(rec);
        }
    }
    ztk_mutex_unlock(rec->mu);
}

static void dash_segmenter_tick_nolock(zms_http_dash_segmenter *rec)
{
    zms_gop_slot slot;

    if (!rec) {
        return;
    }
    if (rec->closing || !rec->src || !rec->reader || !rec->mpd) {
        return;
    }
    if (!rec->sent_video_cfg || rec->adapt_video < 0) {
        size_t clen = 0;
        const uint8_t *cfg = zms_gop_queue_video_config(rec->src->gop_queue, &clen);
        if (cfg && clen) {
            dash_feed_video_cfg(rec, cfg, clen);
            if (rec->adapt_video >= 0) {
                rec->sent_video_cfg = 1;
            }
        } else if (rec->src && rec->src->video.ready &&
                   rec->src->video.codec != ZMS_CODEC_INVALID) {
            rec->video_codec = rec->src->video.codec;
        }
    }
    if (rec->enable_audio && (!rec->sent_audio_cfg || rec->adapt_audio < 0)) {
        size_t clen = 0;
        const uint8_t *cfg = zms_gop_queue_audio_config(rec->src->gop_queue, &clen);
        if (cfg && clen) {
            dash_feed_audio_cfg(rec, cfg, clen);
            if (rec->adapt_audio >= 0) {
                rec->sent_audio_cfg = 1;
            }
        } else if (rec->src && rec->src->audio.ready && rec->src->audio.codec == ZMS_CODEC_OPUS) {
            dash_ensure_opus_adaptation(rec);
        }
    }
    for (int n = 0;
         n < ZMS_SEGMENT_REC_TICK_FRAMES && zms_gop_reader_read_muxed(rec->reader, &slot, 0) > 0;
         ++n) {
        if (!slot.data || slot.len == 0 || slot.config_frame) {
            continue;
        }
        if (slot.track == ZMS_TRACK_VIDEO) {
            zms_gop_slot_refresh_play_key(&slot);
            if (slot.codec == ZMS_CODEC_H265) {
                dash_feed_h265(rec, slot.data, slot.len, slot.dts_ms, slot.keyframe);
            } else if (slot.codec == ZMS_CODEC_H264) {
                dash_feed_h264(rec, slot.data, slot.len, slot.dts_ms, slot.keyframe);
            } else if (slot.codec == ZMS_CODEC_VP8 || slot.codec == ZMS_CODEC_VP9) {
                dash_feed_vpx(rec, slot.codec, slot.data, slot.len, slot.dts_ms, slot.keyframe);
            } else if (slot.codec == ZMS_CODEC_AV1) {
                dash_feed_av1(rec, slot.data, slot.len, slot.dts_ms, slot.keyframe);
            }
        } else if (slot.track == ZMS_TRACK_AUDIO) {
            if (slot.codec == ZMS_CODEC_OPUS) {
                dash_feed_opus(rec, slot.data, slot.len, slot.dts_ms);
            } else {
                dash_feed_aac(rec, slot.data, slot.len, slot.dts_ms);
            }
        }
    }
}

void zms_http_dash_segmenter_tick(zms_http_dash_segmenter *rec)
{
    if (!rec || !rec->mu) {
        return;
    }
    ztk_mutex_lock(rec->mu);
    if (!rec->closing) {
        dash_segmenter_tick_nolock(rec);
    }
    ztk_mutex_unlock(rec->mu);
}

ztk_err_t zms_http_dash_segmenter_serve_mpd(zms_http_dash_segmenter *rec, char *out, size_t cap,
                                            size_t *out_len, int max_ticks, int *has_media)
{
    zms_http_dash_playlist *maker;
    ztk_err_t err;

    (void)max_ticks;
    if (has_media) {
        *has_media = 0;
    }
    if (!rec || !rec->mu || !out || !out_len) {
        return ZTK_ERR_INVALID;
    }
    maker = rec->maker;
    if (!maker) {
        return ZTK_ERR_INVALID;
    }

    /* 已有分片时勿在每次 MPD 轮询时 pump：ffplay 高频刷新会抽空 reader，导致 MPD 停滞 */
    if (!zms_http_dash_playlist_has_media_segment(maker)) {
        dash_segmenter_serve_pump_mpd(rec);
    }

    if (!zms_http_dash_playlist_has_media_segment(maker)) {
        return ZTK_ERR_INVALID;
    }

    err = zms_http_dash_playlist_build_mpd(maker, out, cap, out_len);
    if (err == ZTK_OK && has_media) {
        *has_media = 1;
    }
    return err;
}

ztk_err_t zms_http_dash_segmenter_copy_segment(zms_http_dash_segmenter *rec, const char *name,
                                               uint8_t *buf, size_t cap, size_t *out_len,
                                               int max_ticks)
{
    zms_http_dash_playlist *maker;
    int i;

    if (!rec || !rec->mu || !name || !buf || !out_len) {
        return ZTK_ERR_INVALID;
    }
    maker = rec->maker;
    if (!maker) {
        return ZTK_ERR_INVALID;
    }
    if (max_ticks <= 0) {
        max_ticks = ZMS_SEGMENT_REC_SERVE_MAX_TICKS;
    }

    if (zms_http_dash_playlist_copy_segment(maker, name, buf, cap, out_len) == ZTK_OK &&
        *out_len > 0) {
        return ZTK_OK;
    }

    for (i = 0; i < max_ticks; ++i) {
        ztk_mutex_lock(rec->mu);
        if (rec->closing) {
            ztk_mutex_unlock(rec->mu);
            return ZTK_ERR_INVALID;
        }
        dash_segmenter_tick_nolock(rec);
        ztk_mutex_unlock(rec->mu);
        if (zms_http_dash_playlist_copy_segment(maker, name, buf, cap, out_len) == ZTK_OK &&
            *out_len > 0) {
            return ZTK_OK;
        }
    }
    *out_len = 0;
    return ZTK_ERR_INVALID;
}

ztk_err_t zms_http_dash_segmenter_ref_segment(zms_http_dash_segmenter *rec, const char *name,
                                              ztk_buf **out_buf, size_t *out_len, int max_ticks)
{
    zms_http_dash_playlist *maker;
    int i;

    if (!rec || !rec->mu || !name || !out_buf || !out_len) {
        return ZTK_ERR_INVALID;
    }
    maker = rec->maker;
    if (!maker) {
        return ZTK_ERR_INVALID;
    }
    if (max_ticks <= 0) {
        max_ticks = ZMS_SEGMENT_REC_SERVE_MAX_TICKS;
    }

    if (zms_http_dash_playlist_ref_segment(maker, name, out_buf, out_len) == ZTK_OK &&
        *out_len > 0) {
        return ZTK_OK;
    }

    for (i = 0; i < max_ticks; ++i) {
        ztk_mutex_lock(rec->mu);
        if (rec->closing) {
            ztk_mutex_unlock(rec->mu);
            return ZTK_ERR_INVALID;
        }
        dash_segmenter_tick_nolock(rec);
        ztk_mutex_unlock(rec->mu);
        if (zms_http_dash_playlist_ref_segment(maker, name, out_buf, out_len) == ZTK_OK &&
            *out_len > 0) {
            return ZTK_OK;
        }
    }
    *out_len = 0;
    return ZTK_ERR_INVALID;
}

static ztk_err_t dash_ops_create_live(zms_media_source *src, const void *opts, void **out_rec)
{
    zms_http_dash_segmenter *rec;

    if (!out_rec) {
        return ZTK_ERR_INVALID;
    }
    rec = zms_http_dash_segmenter_create(src, (const zms_http_dash_segmenter_opts *)opts);
    if (!rec) {
        return ZTK_ERR_INVALID;
    }
    *out_rec = rec;
    return ZTK_OK;
}

static void dash_ops_destroy(void *rec)
{
    zms_http_dash_segmenter_destroy((zms_http_dash_segmenter *)rec);
}

static void dash_ops_bind_timer(void *rec, ztk_poller *poller)
{
    zms_http_dash_segmenter_bind_timer((zms_http_dash_segmenter *)rec, poller);
}

static void dash_ops_tick(void *rec)
{
    zms_http_dash_segmenter_tick((zms_http_dash_segmenter *)rec);
}

static void dash_ops_touch(void *rec)
{
    zms_http_dash_segmenter_touch((zms_http_dash_segmenter *)rec);
}

const zms_segment_recorder_ops *zms_http_dash_segment_recorder_ops(void)
{
    static const zms_segment_recorder_ops k_dash_ops = {
        .name = ZMS_SEGMENT_REC_DASH,
        .create_live = dash_ops_create_live,
        .destroy = dash_ops_destroy,
        .bind_timer = dash_ops_bind_timer,
        .tick = dash_ops_tick,
        .touch = dash_ops_touch,
    };
    return &k_dash_ops;
}
