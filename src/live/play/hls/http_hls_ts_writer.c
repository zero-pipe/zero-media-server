/**
 * @file http_hls_ts_writer.c
 * @brief HLS 录制 MPEG-TS mux 路径：extradata/PMT 设置与按 codec 喂入。
 *        覆盖 H.264/H.265/H.266/AV1/VPx 视频与 AAC/Opus 音频；自 http_hls_segmenter.c 拆出，
 *        由录制 tick 循环编排。
 *
 * Copyright (c) zero-media-server
 */
#include "live/play/hls/http_hls_segmenter_internal.h"
#include "zms/media/codec/h264/h264_es.h"
#include "zms/media/codec/h265/h265_es.h"
#include "zms/media/codec/h266/h266_over_rtmp.h"
#include "zms/media/codec/av1/av1_over_rtmp.h"
#include "zms/media/codec/vpx/vpx_over_rtmp.h"
#include "zms/media/codec/aac/aac_over_rtp.h"
#include "zms/media/codec/aac/aac_over_rtmp.h"
#include "zms/media/codec/h264/h264_over_rtmp.h"
#include "zms/media/container/flv/flv_tag_probe.h"
#include "zms/media/container/mpegts/mpegts_mux_feed.h"
#include "webm-vpx.h"
#include "ztk/util/log.h"
#include <string.h>

static void hls_ts_set_extradata(zms_http_hls_segmenter *rec, int stream_type, const void *extra,
                                 size_t len)
{
    if (rec && rec->ts_mux_ops && rec->ts_mux && rec->ts_mux_ops->set_extradata) {
        rec->ts_mux_ops->set_extradata(rec->ts_mux, stream_type, extra, len);
    }
}

static void hls_ts_flush(zms_http_hls_segmenter *rec, int stream_type, int64_t pts_ms)
{
    if (rec && rec->ts_mux_ops && rec->ts_mux && rec->ts_mux_ops->flush) {
        rec->ts_mux_ops->flush(rec->ts_mux, stream_type, pts_ms);
    }
}

static ztk_err_t hls_ts_input(zms_http_hls_segmenter *rec, int stream_type, const void *data,
                              size_t len, int64_t pts_ms, int64_t dts_ms, int flags)
{
    int64_t seg_ms;
    ztk_err_t err;

    if (!rec || !rec->ts_mux_ops || !rec->ts_mux || !rec->ts_mux_ops->write_frame) {
        return ZTK_ERR_INVALID;
    }
    if (!data || len == 0) {
        return ZTK_ERR_INVALID;
    }
    seg_ms = (int64_t)(rec->seg_duration_sec * 1000.f);
    if (seg_ms <= 0) {
        seg_ms = 2000;
    }
    /* 帧间大间隙：先收口当前分片。 */
    if (rec->last_mux_dts_ms > 0 && dts_ms > (int64_t)rec->last_mux_dts_ms + seg_ms) {
        hls_ts_flush(rec, stream_type, (int64_t)rec->last_mux_dts_ms);
        rec->ts_seg_open = 0;
    }
    /* 无关键帧时 libhls 不会按时长切段；超过 2×target 强制 flush，避免直播列表冻结。 */
    if (rec->ts_seg_open &&
        dts_ms > (int64_t)rec->ts_seg_origin_dts_ms + seg_ms * 2) {
        hls_ts_flush(rec, stream_type, (int64_t)rec->last_mux_dts_ms);
        rec->ts_seg_open = 0;
    }
    if (!rec->ts_seg_open) {
        rec->ts_seg_origin_dts_ms = (uint64_t)dts_ms;
        rec->ts_seg_open = 1;
    }
    err = rec->ts_mux_ops->write_frame(rec->ts_mux, stream_type, data, len, pts_ms, dts_ms, flags);
    if (err != ZTK_OK) {
        ztk_warn("HLS ts mux write_frame failed: stream=0x%x len=%u dts=%lld", stream_type,
                 (unsigned)len, (long long)dts_ms);
    }
    return err;
}

static int hls_psi_is_video(int stream_type)
{
    return zms_codec_track_type(zms_codec_from_mpeg_psi(stream_type)) == ZMS_TRACK_VIDEO;
}

/* 分段后端 sink：记录末时间戳（分片 flush + HTTP 服务时序），再将 mux AU 交给 HLS TS mux ops。 */
static ztk_err_t hls_es_sink(void *user, int stream_type, const void *data, size_t len,
                             int64_t pts_ms, int64_t dts_ms, int flags)
{
    zms_http_hls_segmenter *rec = (zms_http_hls_segmenter *)user;

    rec->last_mux_dts_ms = (uint64_t)pts_ms;
    if (hls_psi_is_video(stream_type)) {
        rec->last_video_dts_ms = (uint32_t)pts_ms;
    }
    return hls_ts_input(rec, stream_type, data, len, pts_ms, dts_ms, flags);
}

static void hls_ts_feed_view(zms_http_hls_segmenter *rec, zms_mpegts_mux_feed_view *f)
{
    memset(f, 0, sizeof(*f));
    f->sink = hls_es_sink;
    f->sink_user = rec;
    f->params = &rec->params;
    f->mux_av = &rec->mux_av;
    f->aac = &rec->aac;
    f->mux_buf = rec->mux_buf;
    f->mux_buf_cap = rec->mux_buf_cap;
    f->adts = rec->adts;
    f->adts_cap = rec->adts_cap;
    f->video_armed = &rec->video_armed;
}

int hls_video_psi(zms_codec_id codec)
{
    /* 中央 codec 元数据（codec_id.c）持有 codec→stream_type 映射；
     * 新视频 codec 仅在其表增一行，勿在此再加 switch。 */
    int psi = zms_codec_mpeg_psi(codec);
    return psi ? psi : zms_codec_mpeg_psi(ZMS_CODEC_H264);
}

static const uint8_t k_hls_opus_default_extradata[30] = {
    'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, 0, 0, 0, 0, 0, 0,
    0,   0,   0,   0,   0,   0,   0,   0,   0, 0, 0, 0, 0, 0, 0,
};

static const uint8_t k_hls_opus_coupled_stream_cnt[9] = {1, 0, 1, 1, 2, 2, 2, 3, 3};
static const uint8_t k_hls_opus_stream_cnt[9] = {1, 1, 1, 2, 2, 3, 4, 4, 5};
static const uint8_t k_hls_opus_channel_map[8][8] = {
    {0},
    {0, 1},
    {0, 2, 1},
    {0, 1, 2, 3},
    {0, 4, 1, 2, 3},
    {0, 4, 1, 2, 3, 5},
    {0, 4, 1, 2, 3, 5, 6},
    {0, 6, 1, 2, 3, 4, 5, 7},
};

static int hls_build_vpx_extradata(zms_codec_id vc, const uint8_t *keyframe, size_t len,
                                   uint8_t *out, size_t cap)
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
    n = webm_vpx_codec_configuration_record_save(&vpx, out, cap);
    return n > 0 ? n : -1;
}

static void hls_build_opus_extradata(int channels, int sample_rate, uint8_t *out, size_t cap,
                                     size_t *out_len)
{
    uint8_t ch;
    uint32_t sr;

    if (!out || cap < sizeof(k_hls_opus_default_extradata) || !out_len) {
        return;
    }
    ch = (uint8_t)(channels > 0 && channels <= 8 ? channels : 2);
    memcpy(out, k_hls_opus_default_extradata, sizeof(k_hls_opus_default_extradata));
    out[9] = ch;
    out[18] = ch > 2 ? 1 : (ch == 2 ? 255 : 0);
    out[19] = k_hls_opus_stream_cnt[ch];
    out[20] = k_hls_opus_coupled_stream_cnt[ch];
    memcpy(out + 21, k_hls_opus_channel_map[ch - 1], ch);
    sr = sample_rate > 0 ? (uint32_t)sample_rate : 48000u;
    out[12] = (uint8_t)(sr & 0xff);
    out[13] = (uint8_t)((sr >> 8) & 0xff);
    out[14] = (uint8_t)((sr >> 16) & 0xff);
    out[15] = (uint8_t)((sr >> 24) & 0xff);
    *out_len = sizeof(k_hls_opus_default_extradata);
}

static void hls_ensure_vpx_extradata(zms_http_hls_segmenter *rec, zms_codec_id vc,
                                     const uint8_t *keyframe, size_t len)
{
    uint8_t vpxc[32];
    int psi;
    int n;

    if (!rec || rec->sent_video_cfg || !rec->ts_mux || !keyframe || len == 0) {
        return;
    }
    if (vc != ZMS_CODEC_VP8 && vc != ZMS_CODEC_VP9) {
        return;
    }
    n = hls_build_vpx_extradata(vc, keyframe, len, vpxc, sizeof(vpxc));
    if (n <= 0) {
        return;
    }
    psi = hls_video_psi(vc);
    hls_ts_set_extradata(rec, psi, vpxc, (size_t)n);
    rec->video_codec = vc;
    rec->sent_video_cfg = 1;
    ztk_info("HLS recorder: %s TS extradata ready (keyframe)", zms_codec_name(vc));
}

void hls_ensure_opus_extradata(zms_http_hls_segmenter *rec)
{
    uint8_t opus_head[32];
    size_t head_len = 0;
    int ch = 2;
    int sr = 48000;

    if (!rec || rec->sent_audio_cfg || !rec->ts_mux || !rec->enable_audio) {
        return;
    }
    if (rec->src && rec->src->audio.ready) {
        ch = rec->src->audio.channels > 0 ? rec->src->audio.channels : 2;
        sr = rec->src->audio.sample_rate > 0 ? rec->src->audio.sample_rate : 48000;
    }
    hls_build_opus_extradata(ch, sr, opus_head, sizeof(opus_head), &head_len);
    if (head_len == 0) {
        return;
    }
    hls_ts_set_extradata(rec, zms_codec_mpeg_psi(ZMS_CODEC_OPUS), opus_head, head_len);
    rec->sent_audio_cfg = 1;
    ztk_info("HLS recorder: Opus TS extradata ready rate=%d ch=%d", sr, ch);
}

static zms_codec_id hls_psi_to_video_codec(int psi)
{
    zms_codec_id c = zms_codec_from_mpeg_psi(psi);
    return c != ZMS_CODEC_INVALID ? c : ZMS_CODEC_H264;
}

void feed_raw_video_es(zms_http_hls_segmenter *rec, int psi, const uint8_t *es, size_t len,
                       uint32_t dts_ms, int keyframe)
{
    uint32_t rel_dts_ms;

    if (!rec || !rec->ts_mux || !es || len == 0) {
        return;
    }
    if (keyframe) {
        hls_ensure_vpx_extradata(rec, hls_psi_to_video_codec(psi), es, len);
    }
    if (!rec->video_armed) {
        if (!keyframe) {
            return;
        }
        rec->video_armed = 1;
        zms_mux_av_timeline_lock_origin(&rec->mux_av, dts_ms);
        ztk_info("HLS recorder: first video sync psi=0x%x at dts_ms=%u (gop_queue)", psi,
                 (unsigned)dts_ms);
    }
    rel_dts_ms = hls_mux_dts_ms(rec, ZMS_TRACK_VIDEO, dts_ms);
    rec->last_mux_dts_ms = rel_dts_ms;
    rec->last_video_dts_ms = rel_dts_ms;
    (void)hls_ts_input(rec, psi, es, len, (int64_t)rel_dts_ms, (int64_t)rel_dts_ms,
                       keyframe ? ZMS_CONTAINER_MUX_FLAG_KEYFRAME : 0);
}

void feed_video_tag_cfg(zms_http_hls_segmenter *rec, const uint8_t *cfg, size_t clen)
{
    zms_codec_id vc;

    if (!rec || !cfg || clen < 2) {
        return;
    }
    vc = zms_flv_video_config_codec(cfg, clen);
    if (vc == ZMS_CODEC_INVALID && rec->src && rec->src->video.codec != ZMS_CODEC_INVALID) {
        vc = rec->src->video.codec;
    }
    rec->video_codec = vc;
    if (vc == ZMS_CODEC_H264) {
        const uint8_t *avcc = NULL;
        size_t avcc_len = 0;
        (void)zms_sidecar_cache_rtmp_video_cfg(&rec->params, cfg, clen);
        if (rec->ts_mux && zms_rtmp_avc_extradata(cfg, clen, &avcc, &avcc_len)) {
            hls_ts_set_extradata(rec, zms_codec_mpeg_psi(ZMS_CODEC_H264), avcc, avcc_len);
        }
    } else if (vc == ZMS_CODEC_H265) {
        const uint8_t *hvcc = NULL;
        size_t hvcc_len = 0;
        (void)zms_sidecar_cache_rtmp_video_cfg(&rec->params, cfg, clen);
        if (rec->ts_mux && zms_h265_video_config_hvcc(cfg, clen, &hvcc, &hvcc_len)) {
            rec->hevc_esinfo_len = 0;
            if (hvcc && hvcc_len > 0) {
                int es_len = zms_mpegts_hevc_esinfo(hvcc, hvcc_len, rec->hevc_esinfo,
                                                    sizeof(rec->hevc_esinfo));
                if (es_len > 0) {
                    rec->hevc_esinfo_len = (size_t)es_len;
                }
            }
            if (rec->hevc_esinfo_len > 0) {
                hls_ts_set_extradata(rec, zms_codec_mpeg_psi(ZMS_CODEC_H265), rec->hevc_esinfo,
                                     rec->hevc_esinfo_len);
            } else {
                hls_ts_set_extradata(rec, zms_codec_mpeg_psi(ZMS_CODEC_H265), hvcc, hvcc_len);
            }
        }
    } else if (vc == ZMS_CODEC_H266) {
        const uint8_t *vvcc = NULL;
        size_t vvcc_len = 0;
        (void)zms_sidecar_cache_rtmp_video_cfg(&rec->params, cfg, clen);
        if (rec->ts_mux && zms_h266_video_config_vvcc(cfg, clen, &vvcc, &vvcc_len) && vvcc &&
            vvcc_len > 0) {
            hls_ts_set_extradata(rec, zms_codec_mpeg_psi(ZMS_CODEC_H266), vvcc, vvcc_len);
        }
    } else if (vc == ZMS_CODEC_AV1) {
        const uint8_t *av1c = NULL;
        size_t av1c_len = 0;
        (void)zms_sidecar_cache_rtmp_video_cfg(&rec->params, cfg, clen);
        if (rec->ts_mux && zms_av1_over_rtmp_config_extradata(cfg, clen, &av1c, &av1c_len) &&
            av1c && av1c_len > 0) {
            hls_ts_set_extradata(rec, zms_codec_mpeg_psi(ZMS_CODEC_AV1), av1c, av1c_len);
        }
    } else if (vc == ZMS_CODEC_VP8 || vc == ZMS_CODEC_VP9) {
        const uint8_t *vpxc = NULL;
        size_t vpxc_len = 0;
        int psi = hls_video_psi(vc);
        (void)zms_sidecar_cache_rtmp_video_cfg(&rec->params, cfg, clen);
        if (rec->ts_mux && zms_vpx_over_rtmp_config_extradata(vc, cfg, clen, &vpxc, &vpxc_len) &&
            vpxc && vpxc_len > 0) {
            hls_ts_set_extradata(rec, psi, vpxc, vpxc_len);
        }
    }
}

/* feed_h264_video_es / feed_h265_video_es：H.264/H.265 mux 编排
 *（arming、SPS/PPS 前置、sync 检测、时间线）在 mpegts_mux_feed，
 * 与 SRT 连续路径共享。HLS sink 保留分片 flush 记账。 */
void feed_h264_video_es(zms_http_hls_segmenter *rec, const uint8_t *annexb, size_t len,
                        uint32_t dts_ms, int keyframe)
{
    zms_mpegts_mux_feed_view f;

    if (!rec || !rec->ts_mux) {
        return;
    }
    hls_ts_feed_view(rec, &f);
    zms_mpegts_feed_h264(&f, annexb, len, dts_ms, keyframe);
}

void feed_h265_video_es(zms_http_hls_segmenter *rec, const uint8_t *annexb, size_t len,
                        uint32_t dts_ms, int keyframe)
{
    zms_mpegts_mux_feed_view f;

    if (!rec || !rec->ts_mux) {
        return;
    }
    hls_ts_feed_view(rec, &f);
    zms_mpegts_feed_h265(&f, annexb, len, dts_ms, keyframe);
}

void feed_audio_tag_cfg(zms_http_hls_segmenter *rec, const uint8_t *cfg, size_t clen)
{
    const uint8_t *asc = NULL;
    size_t asc_len = 0;
    zms_codec_id ac;

    if (!rec || !cfg || clen < 2) {
        return;
    }
    ac = zms_flv_tag_audio_codec(cfg, clen);
    if (ac == ZMS_CODEC_OPUS) {
        if (rec->ts_mux && clen > 0) {
            hls_ts_set_extradata(rec, zms_codec_mpeg_psi(ZMS_CODEC_OPUS), cfg, clen);
            rec->sent_audio_cfg = 1;
        }
        return;
    }
    if (ac != ZMS_CODEC_AAC || clen < 4) {
        return;
    }
    if (!zms_rtmp_aac_extradata(cfg, clen, &asc, &asc_len)) {
        return;
    }
    {
        (void)zms_aac_parse_asc(asc, asc_len, &rec->aac_sr, &rec->aac_ch);
        zms_aac_config_set_defaults(&rec->aac, 44100, 2);
        if (zms_aac_config_load_asc(&rec->aac, asc, asc_len)) {
            rec->have_aac = 1;
        }
        if (rec->ts_mux) {
            hls_ts_set_extradata(rec, zms_codec_mpeg_psi(ZMS_CODEC_AAC), asc, asc_len);
        }
    }
}

void feed_audio_es(zms_http_hls_segmenter *rec, const uint8_t *es, size_t es_len, uint32_t dts_ms,
                   zms_codec_id codec)
{
    zms_mpegts_mux_feed_view f;

    if (!rec || !rec->ts_mux || !rec->enable_audio || !es || es_len == 0) {
        return;
    }
    if (codec == ZMS_CODEC_OPUS) {
        hls_ensure_opus_extradata(rec);
        if (!rec->video_armed) {
            return;
        }
        {
            uint32_t rel_dts_ms = hls_mux_dts_ms(rec, ZMS_TRACK_AUDIO, dts_ms);
            rec->last_mux_dts_ms = rel_dts_ms;
            (void)hls_ts_input(rec, zms_codec_mpeg_psi(ZMS_CODEC_OPUS), es, es_len,
                               (int64_t)rel_dts_ms, (int64_t)rel_dts_ms, 0);
        }
        return;
    }
    if (codec != ZMS_CODEC_AAC) {
        return;
    }
    if (!rec->video_armed) {
        return;
    }

    if (!rec->have_aac && rec->src && rec->src->gop_queue) {
        size_t clen = 0;
        const uint8_t *cfg = zms_gop_queue_audio_config(rec->src->gop_queue, &clen);
        if (cfg && clen) {
            feed_audio_tag_cfg(rec, cfg, clen);
            rec->sent_audio_cfg = 1;
        }
    }
    if (!rec->have_aac) {
        return;
    }

    hls_ts_feed_view(rec, &f);
    zms_mpegts_feed_aac(&f, zms_codec_mpeg_psi(ZMS_CODEC_AAC), es, es_len, dts_ms);
}
