#ifndef ZMS_SRC_LIVE_EGRESS_HLS_SEGMENTER_INTERNAL_H
#define ZMS_SRC_LIVE_EGRESS_HLS_SEGMENTER_INTERNAL_H

/**
 * @file http_hls_segmenter_internal.h
 * @brief HLS 录制器跨文件共享的私有状态与接口。
 *        供核心（http_hls_segmenter.c）及其 mux 写端（http_hls_fmp4_writer.c、TS 路径）使用；
 *        不属于公开 API。
 */
#include "zms/live/play/hls/http_hls_segmenter.h"
#include "zms/live/play/hls/http_hls_sidecar.h"
#include "zms/egress/egress_sidecar_param_sets.h"
#include "zms/media/container/container_dispatcher.h"
#include "zms/engine/gop/gop_queue.h"
#include "zms/engine/media_clock.h"
#include "zms/engine/stream/stream_hub.h"
#include "zms/media/codec/codec_id.h"
#include "zms/media/codec/aac/aac_config.h"
#include "zms/media/container/hls/hls_fmp4.h"
#include "ztk/thread/sync.h"
#include "ztk/util/timer.h"
#include "ztk/platform.h"
#include <stddef.h>
#include <stdint.h>

struct zms_http_hls_segmenter {
    zms_media_source *src;
    zms_http_hls_playlist *maker;
    const zms_container_muxer_ops *ts_mux_ops;
    void *ts_mux;
    zms_hls_fmp4 *fmp4_mux;
    int use_fmp4;
    int fmp4_video_track;
    int fmp4_audio_track;
    int fmp4_init_done;
    uint32_t fmp4_seg_origin_dts_ms;
    zms_gop_reader *reader;
    ztk_mutex *mu;
    int closing;
    int enable_audio;
    int sent_video_cfg;
    int sent_audio_cfg;
    int video_armed;
    float seg_duration_sec;
    uint64_t last_hls_req_ms;
    /** TS 当前打开分片的起点 mux dts；用于无关键帧时强制切段。 */
    uint64_t ts_seg_origin_dts_ms;
    int ts_seg_open;
    uint8_t *annexb;
    size_t annexb_cap;
    uint8_t *mux_buf;
    size_t mux_buf_cap;
    uint8_t *adts;
    size_t adts_cap;
    uint8_t hevc_esinfo[64];
    size_t hevc_esinfo_len;
    int aac_sr;
    int aac_ch;
    zms_aac_config aac;
    int have_aac;
    uint64_t last_mux_dts_ms;
    uint32_t last_video_dts_ms;
    zms_codec_id video_codec;
    zms_sidecar_param_sets params;
    ztk_timer *timer;
    ztk_poller *timer_poller;
    int http_serve_depth;
    zms_mux_av_timeline mux_av;
};

/* --- 与 mux writer 共享的核心辅助（定义于 http_hls_segmenter.c）--- */

/** 将 ring 解码时间戳映射到各 track 出站 (mux) 时间线。 */
uint32_t hls_mux_dts_ms(zms_http_hls_segmenter *rec, zms_track_type track, uint32_t ring_dts_ms);

/** 将成品媒体分片缓冲推入 HLS maker。 */
int hls_media_segment_cb(void *param, const void *data, size_t bytes, int64_t pts_ms,
                         int64_t dts_ms, int64_t duration_ms);

/* --- fMP4 writer 入口（定义于 hls_fmp4_writer.c）--- */

int hls_fmp4_segment_cb(void *param, const void *data, size_t bytes, int64_t pts_ms, int64_t dts_ms,
                        int64_t duration_ms);
void hls_fmp4_serve_flush(zms_http_hls_segmenter *rec);
void hls_fmp4_ensure_aac_track(zms_http_hls_segmenter *rec, const uint8_t *cfg, size_t clen);
void hls_fmp4_ensure_opus_track(zms_http_hls_segmenter *rec);
void hls_fmp4_feed_vpx(zms_http_hls_segmenter *rec, zms_codec_id vc, const uint8_t *es, size_t len,
                       uint32_t dts_ms, int keyframe);
void hls_fmp4_feed_aac(zms_http_hls_segmenter *rec, const uint8_t *es, size_t es_len,
                       uint32_t dts_ms);
void hls_fmp4_feed_av1(zms_http_hls_segmenter *rec, const uint8_t *es, size_t len, uint32_t dts_ms,
                       int keyframe);
void hls_fmp4_feed_opus(zms_http_hls_segmenter *rec, const uint8_t *es, size_t len,
                        uint32_t dts_ms);

/* --- MPEG-TS writer 入口（定义于 hls_ts_writer.c）--- */

/** @return 视频 codec 的 MPEG-TS stream_type（H.264 回退）。 */
int hls_video_psi(zms_codec_id codec);
void hls_ensure_opus_extradata(zms_http_hls_segmenter *rec);
void feed_raw_video_es(zms_http_hls_segmenter *rec, int psi, const uint8_t *es, size_t len,
                       uint32_t dts_ms, int keyframe);
void feed_video_tag_cfg(zms_http_hls_segmenter *rec, const uint8_t *cfg, size_t clen);
void feed_h264_video_es(zms_http_hls_segmenter *rec, const uint8_t *annexb, size_t len,
                        uint32_t dts_ms, int keyframe);
void feed_h265_video_es(zms_http_hls_segmenter *rec, const uint8_t *annexb, size_t len,
                        uint32_t dts_ms, int keyframe);
void feed_audio_tag_cfg(zms_http_hls_segmenter *rec, const uint8_t *cfg, size_t clen);
void feed_audio_es(zms_http_hls_segmenter *rec, const uint8_t *es, size_t es_len, uint32_t dts_ms,
                   zms_codec_id codec);

#endif /* ZMS_SRC_LIVE_EGRESS_HLS_SEGMENTER_INTERNAL_H */
