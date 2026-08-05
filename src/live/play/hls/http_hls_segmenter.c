#include "zms/live/play/hls/http_hls_segmenter.h"
#include "zms/live/play/hls/http_hls_sidecar.h"
#include "zms/egress/egress_sidecar_param_sets.h"
#include "zms/media/codec/codec_id.h"
#include "zms/media/container/container_dispatcher.h"
#include "zms/engine/gop/gop_queue.h"
#include "zms/engine/frame.h"
#include "zms/engine/media_clock.h"
#include "zms/engine/media/media_limits.h"
#include "zms/egress/egress_segment_recorder.h"
#include "ztk/util/buf.h"
#include "ztk/util/log.h"
#include "ztk/util/timer.h"
#include "ztk/thread/sync.h"
#include "ztk/platform.h"
#include <stdlib.h>
#include <string.h>

#include "live/play/hls/http_hls_segmenter_internal.h"

int hls_media_segment_cb(void *param, const void *data, size_t bytes, int64_t pts_ms,
                         int64_t dts_ms, int64_t duration_ms)
{
    zms_http_hls_segmenter *rec = (zms_http_hls_segmenter *)param;
    ztk_buf *buf;
    uint64_t dur_ms;

    if (!rec || !rec->maker || !data || bytes == 0) {
        return 0;
    }
    if (duration_ms < 0) {
        duration_ms = 0;
    }
    dur_ms = (uint64_t)duration_ms;
    if (dur_ms == 0) {
        dur_ms = 2000;
    }
    if (dur_ms > 60000u) {
        dur_ms = 2000;
    }

    buf = ztk_buf_alloc(bytes);
    if (!buf) {
        return -1;
    }
    memcpy((void *)ztk_buf_data(buf), data, bytes);
    ztk_buf_set_len(buf, bytes);
    if (zms_http_hls_playlist_push_segment_buf(rec->maker, buf, pts_ms, dts_ms, dur_ms, 0) !=
        ZTK_OK) {
        ztk_buf_unref(buf);
        return -1;
    }
    rec->ts_seg_open = 0;
    {
        char latest[64];

        if (rec->src &&
            zms_http_hls_playlist_latest_segment(rec->maker, latest, sizeof(latest), NULL)) {
            ztk_info("HLS live segment cut: %s/%s %s dur=%llu bytes=%u", rec->src->app,
                     rec->src->stream, latest, (unsigned long long)dur_ms, (unsigned)bytes);
        }
    }
    return 0;
}

static int hls_source_needs_fmp4(const zms_media_source *src)
{
    if (!src || !src->video.ready) {
        return 0;
    }
    switch (src->video.codec) {
    case ZMS_CODEC_VP8:
    case ZMS_CODEC_VP9:
    case ZMS_CODEC_AV1:
    case ZMS_CODEC_H266:
        return 1;
    default:
        return 0;
    }
}

void zms_http_hls_segmenter_default_opts(zms_http_hls_segmenter_opts *opts)
{
    if (!opts) {
        return;
    }
    opts->segment_duration_sec = 2.f;
    opts->segment_count = 3;
    opts->enable_audio = 1;
}

static void zms_http_hls_segmenter_tick_nolock(zms_http_hls_segmenter *rec);

static void hls_segmenter_tick_cb(void *user)
{
    zms_http_hls_segmenter *rec = (zms_http_hls_segmenter *)user;

    if (!rec || !rec->mu) {
        return;
    }
    ztk_mutex_lock(rec->mu);
    if (!rec->closing) {
        zms_http_hls_segmenter_tick_nolock(rec);
    }
    ztk_mutex_unlock(rec->mu);
}

static zms_http_hls_segmenter *hls_segmenter_create_common(zms_media_source *src,
                                                           const zms_http_hls_segmenter_opts *o)
{
    zms_http_hls_segmenter *rec = (zms_http_hls_segmenter *)calloc(1, sizeof(*rec));
    if (!rec) {
        return NULL;
    }

    zms_http_hls_playlist_opts mopts = {
        .segment_duration_sec = o->segment_duration_sec,
        .segment_count = o->segment_count,
    };
    rec->src = src;
    rec->use_fmp4 = hls_source_needs_fmp4(src);
    mopts.use_fmp4 = rec->use_fmp4;
    rec->maker = zms_http_hls_playlist_create(&mopts);
    if (!rec->maker) {
        free(rec);
        return NULL;
    }

    int64_t seg_ms = (int64_t)(o->segment_duration_sec * 1000.f);
    if (seg_ms <= 0) {
        seg_ms = 2000;
    }
    if (rec->use_fmp4) {
        rec->fmp4_mux = zms_hls_fmp4_create(seg_ms, hls_fmp4_segment_cb, rec);
        if (!rec->fmp4_mux) {
            zms_http_hls_playlist_destroy(rec->maker);
            free(rec);
            return NULL;
        }
        rec->fmp4_video_track = -1;
        rec->fmp4_audio_track = -1;
    } else {
        zms_container_mux_opts mcfg;

        memset(&mcfg, 0, sizeof(mcfg));
        mcfg.id = ZMS_CONTAINER_MPEGTS;
        mcfg.segment_duration_ms = seg_ms;
        mcfg.on_segment = hls_media_segment_cb;
        mcfg.user = rec;
        rec->ts_mux_ops = zms_container_muxer_find(ZMS_CONTAINER_MPEGTS);
        rec->ts_mux =
            rec->ts_mux_ops && rec->ts_mux_ops->create ? rec->ts_mux_ops->create(&mcfg) : NULL;
        if (!rec->ts_mux) {
            zms_http_hls_playlist_destroy(rec->maker);
            free(rec);
            return NULL;
        }
    }

    rec->enable_audio = o->enable_audio;
    rec->seg_duration_sec = o->segment_duration_sec > 0.f ? o->segment_duration_sec : 2.f;
    rec->mu = ztk_mutex_create(0);
    if (!rec->mu) {
        zms_http_hls_segmenter_destroy(rec);
        return NULL;
    }
    zms_mux_av_timeline_reset(&rec->mux_av);

    rec->annexb_cap = 256 * 1024;
    rec->annexb = (uint8_t *)malloc(rec->annexb_cap);
    rec->mux_buf_cap = rec->annexb_cap;
    rec->mux_buf = (uint8_t *)malloc(rec->mux_buf_cap);
    rec->adts_cap = 8 * 1024;
    rec->adts = (uint8_t *)malloc(rec->adts_cap);
    if (!rec->annexb || !rec->mux_buf || !rec->adts) {
        zms_http_hls_segmenter_destroy(rec);
        return NULL;
    }
    return rec;
}

zms_http_hls_segmenter *zms_http_hls_segmenter_create(zms_media_source *src,
                                                      const zms_http_hls_segmenter_opts *opts)
{
    zms_http_hls_segmenter_opts def;
    zms_http_hls_segmenter *rec;
    const zms_http_hls_segmenter_opts *o;

    if (!src || !src->gop_queue) {
        return NULL;
    }
    zms_http_hls_segmenter_default_opts(&def);
    o = opts ? opts : &def;

    rec = hls_segmenter_create_common(src, o);
    if (!rec) {
        return NULL;
    }
    rec->reader = zms_gop_reader_attach(src->gop_queue);
    if (!rec->reader) {
        zms_http_hls_segmenter_destroy(rec);
        return NULL;
    }
    zms_gop_reader_seek_live_key(rec->reader);
    (void)zms_media_source_segment_rec_set(src, ZMS_SEGMENT_REC_HLS, rec);
    ztk_info("HLS recorder start (%s): %s/%s", rec->use_fmp4 ? "fMP4" : "gop_queue", src->app,
             src->stream);
    return rec;
}

void zms_http_hls_segmenter_destroy(zms_http_hls_segmenter *rec)
{
    zms_media_source *src;
    zms_http_hls_playlist *maker;
    zms_gop_reader *reader;
    const zms_container_muxer_ops *ts_mux_ops;
    void *ts_mux;
    ztk_mutex *mu;
    int video_armed;
    uint64_t last_mux_dts_ms;
    zms_codec_id video_codec;

    zms_hls_fmp4 *fmp4_mux;
    int use_fmp4;

    if (!rec) {
        return;
    }

    if (rec->timer) {
        ztk_timer_stop(rec->timer);
        rec->timer = NULL;
    }

    video_armed = rec->video_armed;
    last_mux_dts_ms = rec->last_mux_dts_ms;
    video_codec = rec->video_codec;
    ts_mux_ops = rec->ts_mux_ops;
    ts_mux = rec->ts_mux;
    fmp4_mux = rec->fmp4_mux;
    use_fmp4 = rec->use_fmp4;
    reader = rec->reader;
    maker = rec->maker;
    src = rec->src;
    mu = rec->mu;

    if (mu) {
        ztk_mutex_lock(mu);
        rec->closing = 1;
        if (src) {
            (void)zms_media_source_segment_rec_set(src, ZMS_SEGMENT_REC_HLS, NULL);
        }
        rec->ts_mux = NULL;
        rec->ts_mux_ops = NULL;
        rec->fmp4_mux = NULL;
        rec->reader = NULL;
        ztk_mutex_unlock(mu);
    } else if (src) {
        (void)zms_media_source_segment_rec_set(src, ZMS_SEGMENT_REC_HLS, NULL);
    }

    if (fmp4_mux) {
        zms_hls_fmp4_destroy(fmp4_mux);
    }
    if (ts_mux && ts_mux_ops) {
        if (video_armed) {
            int psi = hls_video_psi(video_codec);
            if (ts_mux_ops->flush) {
                ts_mux_ops->flush(ts_mux, psi, (int64_t)last_mux_dts_ms);
            }
        }
        if (ts_mux_ops->destroy) {
            ts_mux_ops->destroy(ts_mux);
        }
    }
    if (reader) {
        zms_gop_reader_detach(reader);
    }
    if (maker) {
        zms_http_hls_playlist_destroy(maker);
    }
    free(rec->annexb);
    free(rec->mux_buf);
    free(rec->adts);
    zms_sidecar_param_sets_clear(&rec->params);
    ztk_mutex_destroy(mu);
    free(rec);
}

zms_http_hls_playlist *zms_http_hls_segmenter_playlist(zms_http_hls_segmenter *rec)
{
    if (!rec || rec->closing) {
        return NULL;
    }
    return rec->maker;
}

static ztk_poller *hls_segmenter_timer_poller(ztk_poller *fallback)
{
    ztk_poller *main_pol = zms_http_hls_main_poller();
    return main_pol ? main_pol : fallback;
}

void zms_http_hls_segmenter_bind_timer(zms_http_hls_segmenter *rec, ztk_poller *poller)
{
    ztk_poller *bind_pol;

    if (!rec) {
        return;
    }
    bind_pol = hls_segmenter_timer_poller(poller);
    if (!bind_pol || !rec->mu) {
        return;
    }
    ztk_mutex_lock(rec->mu);
    rec->timer_poller = bind_pol;
    if (rec->timer) {
        ztk_timer_stop(rec->timer);
        rec->timer = NULL;
    }
    if (!rec->closing) {
        rec->timer = ztk_timer_start(bind_pol, 10, 1, hls_segmenter_tick_cb, rec);
    }
    ztk_mutex_unlock(rec->mu);
}

void zms_http_hls_segmenter_http_enter(zms_http_hls_segmenter *rec)
{
    if (!rec || !rec->mu) {
        return;
    }
    ztk_mutex_lock(rec->mu);
    rec->http_serve_depth++;
    ztk_mutex_unlock(rec->mu);
}

void zms_http_hls_segmenter_http_leave(zms_http_hls_segmenter *rec)
{
    if (!rec || !rec->mu) {
        return;
    }
    ztk_mutex_lock(rec->mu);
    if (rec->http_serve_depth > 0) {
        --rec->http_serve_depth;
    }
    ztk_mutex_unlock(rec->mu);
}

void zms_http_hls_segmenter_touch_hls(zms_http_hls_segmenter *rec)
{
    if (rec) {
        rec->last_hls_req_ms = ztk_monotonic_ms();
    }
}

uint32_t hls_mux_dts_ms(zms_http_hls_segmenter *rec, zms_track_type track, uint32_t ring_dts_ms)
{
    return zms_mux_av_timeline_pts(&rec->mux_av, track, ring_dts_ms);
}

static void zms_http_hls_segmenter_tick_nolock(zms_http_hls_segmenter *rec)
{
    zms_gop_slot slot;

    if (!rec || rec->closing || !rec->src) {
        return;
    }
    if (!rec->reader || !rec->src->gop_queue) {
        return;
    }

    if (rec->use_fmp4) {
        if (!rec->sent_audio_cfg && rec->enable_audio && rec->src) {
            size_t clen = 0;
            const uint8_t *cfg = zms_gop_queue_audio_config(rec->src->gop_queue, &clen);
            if (cfg && clen) {
                hls_fmp4_ensure_aac_track(rec, cfg, clen);
            } else if (rec->src->audio.ready && rec->src->audio.codec == ZMS_CODEC_OPUS) {
                hls_fmp4_ensure_opus_track(rec);
            }
        }
        for (int n = 0; n < ZMS_SEGMENT_REC_TICK_FRAMES &&
                        zms_gop_reader_read_muxed(rec->reader, &slot, 0) > 0;
             ++n) {
            if (!slot.data || slot.len == 0 || slot.config_frame) {
                continue;
            }
            if (slot.track == ZMS_TRACK_VIDEO) {
                zms_gop_slot_refresh_play_key(&slot);
                if (slot.codec == ZMS_CODEC_VP8 || slot.codec == ZMS_CODEC_VP9) {
                    hls_fmp4_feed_vpx(rec, slot.codec, slot.data, slot.len, slot.dts_ms,
                                      slot.keyframe);
                } else if (slot.codec == ZMS_CODEC_AV1) {
                    hls_fmp4_feed_av1(rec, slot.data, slot.len, slot.dts_ms, slot.keyframe);
                }
            } else if (slot.track == ZMS_TRACK_AUDIO) {
                if (slot.codec == ZMS_CODEC_OPUS) {
                    hls_fmp4_feed_opus(rec, slot.data, slot.len, slot.dts_ms);
                } else if (slot.codec == ZMS_CODEC_AAC) {
                    hls_fmp4_feed_aac(rec, slot.data, slot.len, slot.dts_ms);
                }
            }
        }
        return;
    }

    if (!rec->sent_video_cfg) {
        size_t clen = 0;
        const uint8_t *cfg = zms_gop_queue_video_config(rec->src->gop_queue, &clen);
        if (cfg && clen) {
            feed_video_tag_cfg(rec, cfg, clen);
            rec->sent_video_cfg = 1;
        }
    }
    if (!rec->sent_audio_cfg && rec->enable_audio) {
        size_t clen = 0;
        const uint8_t *cfg = zms_gop_queue_audio_config(rec->src->gop_queue, &clen);
        if (cfg && clen) {
            feed_audio_tag_cfg(rec, cfg, clen);
            if (!rec->sent_audio_cfg) {
                rec->sent_audio_cfg = 1;
            }
        } else if (rec->src && rec->src->audio.ready && rec->src->audio.codec == ZMS_CODEC_OPUS) {
            hls_ensure_opus_extradata(rec);
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
                feed_h265_video_es(rec, slot.data, slot.len, slot.dts_ms, slot.keyframe);
            } else if (slot.codec == ZMS_CODEC_H264) {
                feed_h264_video_es(rec, slot.data, slot.len, slot.dts_ms, slot.keyframe);
            } else if (slot.codec == ZMS_CODEC_H266) {
                feed_raw_video_es(rec, hls_video_psi(ZMS_CODEC_H266), slot.data, slot.len,
                                  slot.dts_ms, slot.keyframe);
            } else if (slot.codec == ZMS_CODEC_AV1) {
                feed_raw_video_es(rec, hls_video_psi(ZMS_CODEC_AV1), slot.data, slot.len,
                                  slot.dts_ms, slot.keyframe);
            } else if (slot.codec == ZMS_CODEC_VP8) {
                feed_raw_video_es(rec, hls_video_psi(ZMS_CODEC_VP8), slot.data, slot.len,
                                  slot.dts_ms, slot.keyframe);
            } else if (slot.codec == ZMS_CODEC_VP9) {
                feed_raw_video_es(rec, hls_video_psi(ZMS_CODEC_VP9), slot.data, slot.len,
                                  slot.dts_ms, slot.keyframe);
            }
        } else if (slot.track == ZMS_TRACK_AUDIO) {
            feed_audio_es(rec, slot.data, slot.len, slot.dts_ms, slot.codec);
        }
    }
}

void zms_http_hls_segmenter_lock(zms_http_hls_segmenter *rec)
{
    if (rec && rec->mu) {
        ztk_mutex_lock(rec->mu);
    }
}

void zms_http_hls_segmenter_unlock(zms_http_hls_segmenter *rec)
{
    if (rec && rec->mu) {
        ztk_mutex_unlock(rec->mu);
    }
}

void zms_http_hls_segmenter_tick_locked(zms_http_hls_segmenter *rec)
{
    zms_http_hls_segmenter_tick_nolock(rec);
}

void zms_http_hls_segmenter_tick(zms_http_hls_segmenter *rec)
{
    if (!rec || !rec->mu) {
        return;
    }
    ztk_mutex_lock(rec->mu);
    if (!rec->closing) {
        zms_http_hls_segmenter_tick_nolock(rec);
    }
    ztk_mutex_unlock(rec->mu);
}

/**
 * 仅在尚无分片时于 m3u8 请求路径上 pump（与 DASH MPD 策略一致）。
 * 已有分片后依赖 10ms timer；每次 m3u8 狂抽 gop_reader 会抽空交织窗口，导致切段停滞。
 */
static void hls_segmenter_serve_pump_m3u8(zms_http_hls_segmenter *rec)
{
    int i;

    if (!rec || !rec->mu) {
        return;
    }
    ztk_mutex_lock(rec->mu);
    if (!rec->closing) {
        for (i = 0; i < ZMS_SEGMENT_REC_SERVE_MAX_TICKS; ++i) {
            zms_http_hls_segmenter_tick_nolock(rec);
        }
        if (rec->use_fmp4) {
            hls_fmp4_serve_flush(rec);
        }
    }
    ztk_mutex_unlock(rec->mu);
}

ztk_err_t zms_http_hls_segmenter_serve_m3u8(zms_http_hls_segmenter *rec, char *out, size_t cap,
                                            size_t *out_len, int max_ticks, int *seg_count)
{
    zms_http_hls_playlist *maker;
    ztk_err_t err;

    (void)max_ticks;
    if (seg_count) {
        *seg_count = 0;
    }
    if (!rec || !rec->mu || !out || !out_len) {
        return ZTK_ERR_INVALID;
    }

    maker = rec->maker;
    if (!maker) {
        return ZTK_ERR_INVALID;
    }

    /* 已有分片时勿在每次 m3u8 轮询时 pump：高频刷新会抽空 reader，切段停滞（同 DASH）。 */
    if (zms_http_hls_playlist_segment_count(maker) <= 0) {
        hls_segmenter_serve_pump_m3u8(rec);
    }

    ztk_mutex_lock(rec->mu);
    if (rec->closing) {
        ztk_mutex_unlock(rec->mu);
        return ZTK_ERR_INVALID;
    }
    err = zms_http_hls_playlist_build_m3u8(maker, out, cap, out_len);
    if (err == ZTK_OK && seg_count) {
        *seg_count = zms_http_hls_playlist_segment_count(maker);
    }
    if (err == ZTK_OK) {
        zms_http_hls_segmenter_touch_hls(rec);
    }
    ztk_mutex_unlock(rec->mu);
    return err;
}

ztk_err_t zms_http_hls_segmenter_copy_segment(zms_http_hls_segmenter *rec, const char *name,
                                              uint8_t *buf, size_t cap, size_t *out_len,
                                              int max_ticks)
{
    zms_http_hls_playlist *maker;
    int i;

    if (!rec || !rec->mu || !name || !buf || !out_len) {
        return ZTK_ERR_INVALID;
    }
    if (max_ticks <= 0) {
        max_ticks = ZMS_SEGMENT_REC_SERVE_MAX_TICKS;
    }

    maker = rec->maker;
    if (!maker) {
        return ZTK_ERR_INVALID;
    }

    if (zms_http_hls_playlist_copy_segment(maker, name, buf, cap, out_len) == ZTK_OK &&
        *out_len > 0) {
        zms_http_hls_segmenter_touch_hls(rec);
        return ZTK_OK;
    }

    for (i = 0; i < max_ticks; ++i) {
        ztk_mutex_lock(rec->mu);
        if (rec->closing) {
            ztk_mutex_unlock(rec->mu);
            return ZTK_ERR_INVALID;
        }
        zms_http_hls_segmenter_tick_nolock(rec);
        ztk_mutex_unlock(rec->mu);
        if (zms_http_hls_playlist_copy_segment(maker, name, buf, cap, out_len) == ZTK_OK &&
            *out_len > 0) {
            zms_http_hls_segmenter_touch_hls(rec);
            return ZTK_OK;
        }
    }
    *out_len = 0;
    return ZTK_ERR_INVALID;
}

ztk_err_t zms_http_hls_segmenter_ref_segment(zms_http_hls_segmenter *rec, const char *name,
                                             ztk_buf **out_buf, size_t *out_len, int max_ticks)
{
    zms_http_hls_playlist *maker;
    int i;

    if (!rec || !rec->mu || !name || !out_buf || !out_len) {
        return ZTK_ERR_INVALID;
    }
    if (max_ticks <= 0) {
        max_ticks = ZMS_SEGMENT_REC_SERVE_MAX_TICKS;
    }

    maker = rec->maker;
    if (!maker) {
        return ZTK_ERR_INVALID;
    }

    if (zms_http_hls_playlist_ref_segment(maker, name, out_buf, out_len) == ZTK_OK &&
        *out_len > 0) {
        zms_http_hls_segmenter_touch_hls(rec);
        return ZTK_OK;
    }

    for (i = 0; i < max_ticks; ++i) {
        ztk_mutex_lock(rec->mu);
        if (rec->closing) {
            ztk_mutex_unlock(rec->mu);
            return ZTK_ERR_INVALID;
        }
        zms_http_hls_segmenter_tick_nolock(rec);
        ztk_mutex_unlock(rec->mu);
        if (zms_http_hls_playlist_ref_segment(maker, name, out_buf, out_len) == ZTK_OK &&
            *out_len > 0) {
            zms_http_hls_segmenter_touch_hls(rec);
            return ZTK_OK;
        }
    }
    *out_len = 0;
    return ZTK_ERR_INVALID;
}

static ztk_err_t hls_ops_create_live(zms_media_source *src, const void *opts, void **out_rec)
{
    zms_http_hls_segmenter *rec;

    if (!out_rec) {
        return ZTK_ERR_INVALID;
    }
    rec = zms_http_hls_segmenter_create(src, (const zms_http_hls_segmenter_opts *)opts);
    if (!rec) {
        return ZTK_ERR_INVALID;
    }
    *out_rec = rec;
    return ZTK_OK;
}

static void hls_ops_destroy(void *rec)
{
    zms_http_hls_segmenter_destroy((zms_http_hls_segmenter *)rec);
}

static void hls_ops_bind_timer(void *rec, ztk_poller *poller)
{
    zms_http_hls_segmenter_bind_timer((zms_http_hls_segmenter *)rec, poller);
}

static void hls_ops_tick(void *rec)
{
    zms_http_hls_segmenter_tick((zms_http_hls_segmenter *)rec);
}

static void hls_ops_touch(void *rec)
{
    zms_http_hls_segmenter_touch_hls((zms_http_hls_segmenter *)rec);
}

static const zms_segment_recorder_ops k_hls_ops = {
    .name = ZMS_SEGMENT_REC_HLS,
    .create_live = hls_ops_create_live,
    .destroy = hls_ops_destroy,
    .bind_timer = hls_ops_bind_timer,
    .tick = hls_ops_tick,
    .touch = hls_ops_touch,
};

const zms_segment_recorder_ops *zms_http_hls_segment_recorder_ops(void)
{
    return &k_hls_ops;
}
