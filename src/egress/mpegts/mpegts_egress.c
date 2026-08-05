/**
 * @file mpegts_egress.c
 * @brief 基于 gop_queue 的直播 MPEG-TS 出站泵（SRT / HTTP-TS）。
 */
#include "zms/egress/mpegts/mpegts_egress.h"
#include "zms/media/container/mpegts/mpegts_mux_feed.h"
#include "zms/egress/egress_sidecar_param_sets.h"
#include "zms/media/codec/aac/aac_over_rtmp.h"
#include "zms/media/codec/aac/aac_config.h"
#include "zms/media/codec/codec_id.h"
#include "zms/media/codec/h264/h264_es.h"
#include "zms/media/codec/h265/h265_es.h"
#include "zms/media/codec/h264/h264_over_rtmp.h"
#include "zms/media/container/flv/flv_tag_probe.h"
#include "zms/egress/egress_source.h"
#include "zms/engine/media/media_limits.h"
#include "zms/engine/stream/stream_stats.h"
#include "mpeg-proto.h"
#include "ztk/util/log.h"
#include <stdlib.h>
#include <string.h>

#define TS_SEND_INIT_CAP (256u * 1024u)
#define TS_PUMP_FRAMES 64
#define TS_MUX_SKEW_MS 500u

struct zms_mpegts_egress {
    zms_media_source *source;
    zms_egress_source *play;
    const zms_container_muxer_ops *ops;
    void *mux;
    int vstream;
    int astream;
    int sent_video_cfg;
    int sent_audio_cfg;
    int video_armed;
    uint32_t video_origin_ms;
    int logged_video;
    int logged_audio;
    zms_mux_av_timeline mux_av;
    zms_sidecar_param_sets params;
    zms_codec_id video_codec;
    zms_aac_config aac;
    int have_aac;
    int enable_audio;
    uint8_t *mux_buf;
    size_t mux_buf_cap;
    uint8_t *adts;
    size_t adts_cap;
    uint8_t hevc_esinfo[64];
    size_t hevc_esinfo_len;
    uint8_t hvcc_buf[256];
    size_t hvcc_len;
    uint8_t *send_buf;
    size_t send_len;
    size_t send_off;
    size_t send_cap;
};

static int ts_append_send(zms_mpegts_egress *m, const void *packet, size_t bytes)
{
    size_t need;

    if (!m || !packet || bytes == 0) {
        return 0;
    }
    if (m->send_off > 0 && m->send_off >= m->send_len) {
        m->send_off = 0;
        m->send_len = 0;
    } else if (m->send_off > 0) {
        memmove(m->send_buf, m->send_buf + m->send_off, m->send_len - m->send_off);
        m->send_len -= m->send_off;
        m->send_off = 0;
    }
    need = m->send_len + bytes;
    if (need > m->send_cap) {
        size_t ncap = m->send_cap ? m->send_cap : TS_SEND_INIT_CAP;
        uint8_t *p;
        while (ncap < need) {
            ncap *= 2;
        }
        p = (uint8_t *)realloc(m->send_buf, ncap);
        if (!p) {
            return -1;
        }
        m->send_buf = p;
        m->send_cap = ncap;
    }
    memcpy(m->send_buf + m->send_len, packet, bytes);
    m->send_len += bytes;
    return 0;
}

static int srt_ts_on_data(void *user, const void *data, size_t bytes, int64_t pts_ms,
                          int64_t dts_ms, int64_t duration_ms)
{
    (void)pts_ms;
    (void)dts_ms;
    (void)duration_ms;
    return ts_append_send((zms_mpegts_egress *)user, data, bytes);
}

static void count_egress(zms_mpegts_egress *m, size_t n)
{
    if (m && m->source && n > 0) {
        zms_media_stats_on_egress(m->source, n);
    }
}

static void ensure_mux_buf(zms_mpegts_egress *m, size_t need)
{
    if (!m || need <= m->mux_buf_cap) {
        return;
    }
    {
        uint8_t *p = (uint8_t *)realloc(m->mux_buf, need);
        if (p) {
            m->mux_buf = p;
            m->mux_buf_cap = need;
        }
    }
}

static void ensure_adts_buf(zms_mpegts_egress *m, size_t need)
{
    if (!m || need <= m->adts_cap) {
        return;
    }
    {
        uint8_t *p = (uint8_t *)realloc(m->adts, need);
        if (p) {
            m->adts = p;
            m->adts_cap = need;
        }
    }
}

/* 连续后端 sink：mux 后 AU 直达 TS 容器 mux ops。 */
static ztk_err_t srt_es_sink(void *user, int stream_type, const void *data, size_t len,
                             int64_t pts_ms, int64_t dts_ms, int flags)
{
    zms_mpegts_egress *m = (zms_mpegts_egress *)user;

    if (!m || !m->mux) {
        return ZTK_ERR_INVALID;
    }
    return m->ops->write_frame(m->mux, stream_type, data, len, pts_ms, dts_ms, flags);
}

static void srt_feed_view(zms_mpegts_egress *m, zms_mpegts_mux_feed_view *f)
{
    memset(f, 0, sizeof(*f));
    f->sink = srt_es_sink;
    f->sink_user = m;
    f->params = &m->params;
    f->mux_av = &m->mux_av;
    f->aac = &m->aac;
    f->mux_buf = m->mux_buf;
    f->mux_buf_cap = m->mux_buf_cap;
    f->adts = m->adts;
    f->adts_cap = m->adts_cap;
    f->video_armed = &m->video_armed;
}

static void feed_video_cfg(zms_mpegts_egress *m, const uint8_t *cfg, size_t clen)
{
    zms_codec_id vc;

    if (!m || !m->mux || !cfg || clen < 2) {
        return;
    }
    vc = zms_flv_video_config_codec(cfg, clen);
    if (vc == ZMS_CODEC_INVALID && m->source && m->source->video.codec != ZMS_CODEC_INVALID) {
        vc = m->source->video.codec;
    }
    m->video_codec = vc;
    (void)zms_sidecar_cache_rtmp_video_cfg(&m->params, cfg, clen);

    if (vc == ZMS_CODEC_H264) {
        const uint8_t *avcc = NULL;
        size_t avcc_len = 0;
        if (zms_rtmp_avc_extradata(cfg, clen, &avcc, &avcc_len) && avcc && avcc_len > 0) {
            m->ops->set_extradata(m->mux, PSI_STREAM_H264, avcc, avcc_len);
            m->vstream = PSI_STREAM_H264;
        }
    } else if (vc == ZMS_CODEC_H265) {
        const uint8_t *hvcc = NULL;
        size_t hvcc_len = 0;
        if (zms_h265_video_config_hvcc(cfg, clen, &hvcc, &hvcc_len)) {
            m->hvcc_len = 0;
            if (hvcc && hvcc_len > 0 && hvcc_len <= sizeof(m->hvcc_buf)) {
                memcpy(m->hvcc_buf, hvcc, hvcc_len);
                m->hvcc_len = hvcc_len;
            }
            m->hevc_esinfo_len = 0;
            if (hvcc && hvcc_len > 0) {
                int es_len =
                    zms_mpegts_hevc_esinfo(hvcc, hvcc_len, m->hevc_esinfo, sizeof(m->hevc_esinfo));
                if (es_len > 0) {
                    m->hevc_esinfo_len = (size_t)es_len;
                }
            }
            if (m->hevc_esinfo_len > 0) {
                m->ops->set_extradata(m->mux, PSI_STREAM_H265, m->hevc_esinfo, m->hevc_esinfo_len);
                m->vstream = PSI_STREAM_H265;
            } else if (hvcc && hvcc_len > 0) {
                m->ops->set_extradata(m->mux, PSI_STREAM_H265, hvcc, hvcc_len);
                m->vstream = PSI_STREAM_H265;
            }
        }
    }
}

static void feed_audio_cfg(zms_mpegts_egress *m, const uint8_t *cfg, size_t clen)
{
    const uint8_t *asc = NULL;
    size_t asc_len = 0;

    if (!m || !m->mux || !cfg || clen < 2) {
        return;
    }
    if (zms_flv_tag_audio_codec(cfg, clen) != ZMS_CODEC_AAC || clen < 4) {
        return;
    }
    if (!zms_rtmp_aac_extradata(cfg, clen, &asc, &asc_len)) {
        return;
    }
    zms_aac_config_set_defaults(&m->aac, 44100, 2);
    if (zms_aac_config_load_asc(&m->aac, asc, asc_len)) {
        m->have_aac = 1;
    }
    m->ops->set_extradata(m->mux, PSI_STREAM_AAC, asc, asc_len);
    m->astream = PSI_STREAM_AAC;
}

static void live_ensure_h265_params(zms_mpegts_egress *m)
{
    size_t clen = 0;
    const uint8_t *cfg;

    if (!m || m->params.sps_len > 0) {
        return;
    }
    if (m->hvcc_len > 0) {
        (void)zms_sidecar_cache_rtmp_video_cfg(&m->params, m->hvcc_buf, m->hvcc_len);
    }
    if (m->params.sps_len > 0) {
        return;
    }
    if (!m->source || !m->source->gop_queue) {
        return;
    }
    cfg = zms_gop_queue_video_config(m->source->gop_queue, &clen);
    if (cfg && clen) {
        (void)zms_sidecar_cache_rtmp_video_cfg(&m->params, cfg, clen);
    }
}

/* feed_h264_es / feed_h265_es：play-start 门控与关键帧刷新在
 * pump_ring 中完成；此处仅设置共享 feed view、跑公共 mux，
 * 并维护 SRT video-origin 记账（用于丢弃 pre-roll 音频）。
 * H.264/H.265/AAC mux 逻辑在 mpegts_mux_feed。 */
static void feed_h264_es(zms_mpegts_egress *m, const uint8_t *annexb, size_t len, uint32_t dts_ms,
                         int keyframe)
{
    zms_mpegts_mux_feed_view f;
    int was_armed;

    if (!m || !m->mux || m->vstream <= 0 || !annexb || len < 4) {
        return;
    }
    ensure_mux_buf(m, ZMS_MPEGTS_AU_MAX);
    was_armed = m->video_armed;
    srt_feed_view(m, &f);
    zms_mpegts_feed_h264(&f, annexb, len, dts_ms, keyframe);
    if (!was_armed && m->video_armed) {
        m->video_origin_ms = dts_ms;
    }
}

static void feed_h265_es(zms_mpegts_egress *m, const uint8_t *annexb, size_t len, uint32_t dts_ms,
                         int keyframe)
{
    zms_mpegts_mux_feed_view f;
    int was_armed;

    if (!m || !m->mux || m->vstream <= 0 || !annexb || len < 4) {
        return;
    }
    live_ensure_h265_params(m);
    ensure_mux_buf(m, ZMS_MPEGTS_AU_MAX);
    was_armed = m->video_armed;
    srt_feed_view(m, &f);
    zms_mpegts_feed_h265(&f, annexb, len, dts_ms, keyframe);
    if (!was_armed && m->video_armed) {
        m->video_origin_ms = dts_ms;
    }
}

static void feed_audio_es(zms_mpegts_egress *m, const uint8_t *es, size_t es_len, uint32_t dts_ms,
                          zms_codec_id codec)
{
    zms_mpegts_mux_feed_view f;

    if (!m || !m->enable_audio || !es || es_len == 0 || !m->video_armed) {
        return;
    }
    if (codec != ZMS_CODEC_AAC) {
        return;
    }
    if (!m->have_aac && m->source && m->source->gop_queue) {
        size_t clen = 0;
        const uint8_t *cfg = zms_gop_queue_audio_config(m->source->gop_queue, &clen);
        if (cfg && clen) {
            feed_audio_cfg(m, cfg, clen);
            m->sent_audio_cfg = 1;
        }
    }
    if (!m->have_aac || m->astream <= 0) {
        return;
    }
    if (m->video_origin_ms && dts_ms < m->video_origin_ms) {
        return;
    }
    srt_feed_view(m, &f);
    zms_mpegts_feed_aac(&f, m->astream, es, es_len, dts_ms);
}

static void pump_ring(zms_mpegts_egress *m)
{
    zms_gop_slot slot;

    if (!m || !m->play || !m->play->readers.gop || !m->source || !m->source->gop_queue) {
        return;
    }

    if (!m->sent_video_cfg) {
        size_t clen = 0;
        const uint8_t *cfg = zms_gop_queue_video_config(m->source->gop_queue, &clen);
        if (cfg && clen) {
            feed_video_cfg(m, cfg, clen);
            m->sent_video_cfg = 1;
        }
    }
    if (!m->sent_audio_cfg && m->enable_audio) {
        size_t clen = 0;
        const uint8_t *cfg = zms_gop_queue_audio_config(m->source->gop_queue, &clen);
        if (cfg && clen) {
            feed_audio_cfg(m, cfg, clen);
            m->sent_audio_cfg = 1;
        }
    }

    for (int n = 0;
         n < TS_PUMP_FRAMES && zms_egress_source_read_muxed(m->play, &slot, TS_MUX_SKEW_MS) > 0;
         ++n) {
        if (!slot.data || slot.len == 0 || slot.config_frame) {
            continue;
        }
        if (slot.track == ZMS_TRACK_VIDEO) {
            zms_gop_slot_refresh_play_key(&slot);
            if (!m->video_armed && !zms_gop_slot_is_play_start(&slot)) {
                continue;
            }
            if (slot.codec == ZMS_CODEC_H265) {
                feed_h265_es(m, slot.data, slot.len, slot.dts_ms, slot.keyframe);
            } else {
                feed_h264_es(m, slot.data, slot.len, slot.dts_ms, slot.keyframe);
            }
        } else if (slot.track == ZMS_TRACK_AUDIO) {
            feed_audio_es(m, slot.data, slot.len, slot.dts_ms, slot.codec);
        }
    }
}

zms_mpegts_egress *zms_mpegts_egress_create(zms_media_source *src, zms_egress_source *play)
{
    zms_container_mux_opts mcfg;
    zms_mpegts_egress *m;

    if (!src || !play || !src->gop_queue) {
        return NULL;
    }
    m = (zms_mpegts_egress *)calloc(1, sizeof(*m));
    if (!m) {
        return NULL;
    }
    m->source = src;
    m->play = play;
    m->enable_audio = src->has_audio;
    zms_mux_av_timeline_reset(&m->mux_av);
    ensure_mux_buf(m, ZMS_MPEGTS_AU_MAX);
    ensure_adts_buf(m, ZMS_MPEGTS_ADTS_MAX);
    m->send_buf = (uint8_t *)malloc(TS_SEND_INIT_CAP);
    if (!m->send_buf) {
        zms_mpegts_egress_destroy(m);
        return NULL;
    }
    m->send_cap = TS_SEND_INIT_CAP;
    m->ops = &zms_container_mpegts_continuous_muxer_ops;
    memset(&mcfg, 0, sizeof(mcfg));
    mcfg.id = ZMS_CONTAINER_MPEGTS;
    mcfg.segment_duration_ms = 0;
    mcfg.on_segment = srt_ts_on_data;
    mcfg.user = m;
    m->mux = m->ops->create ? m->ops->create(&mcfg) : NULL;
    if (!m->mux) {
        zms_mpegts_egress_destroy(m);
        return NULL;
    }
    return m;
}

void zms_mpegts_egress_destroy(zms_mpegts_egress *m)
{
    if (!m) {
        return;
    }
    if (m->ops && m->mux) {
        m->ops->destroy(m->mux);
    }
    free(m->mux_buf);
    free(m->adts);
    free(m->send_buf);
    free(m);
}

int zms_mpegts_egress_next(zms_mpegts_egress *m, uint8_t *out, size_t cap, size_t *out_len)
{
    size_t avail;
    size_t n;

    if (!m || !out || !out_len || cap == 0) {
        return -1;
    }
    *out_len = 0;

    if (m->send_off < m->send_len) {
        avail = m->send_len - m->send_off;
        n = avail < cap ? avail : cap;
        if (cap >= 1316 && n >= 1316) {
            n = 1316;
        }
        memcpy(out, m->send_buf + m->send_off, n);
        m->send_off += n;
        *out_len = n;
        count_egress(m, n);
        return 1;
    }

    pump_ring(m);

    if (m->send_off < m->send_len) {
        avail = m->send_len - m->send_off;
        n = avail < cap ? avail : cap;
        if (cap >= 1316 && n >= 1316) {
            n = 1316;
        }
        memcpy(out, m->send_buf + m->send_off, n);
        m->send_off += n;
        *out_len = n;
        count_egress(m, n);
        return 1;
    }
    return 0;
}
