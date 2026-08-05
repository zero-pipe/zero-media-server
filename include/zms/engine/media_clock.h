#ifndef ZMS_ENGINE_CLOCK_MEDIA_CLOCK_H
#define ZMS_ENGINE_CLOCK_MEDIA_CLOCK_H

/**
 * @file media_clock.h
 * @brief 入站时间线归一化与出站时钟换算。
 *
 * 直播推流时间戳在 channel 经 @ref zms_track_stamp 与 @ref zms_media_timeline 修订一次。
 * 出站将 ring 毫秒映射为协议时钟（RTP、FLV/RTMP、MPEG-TS PCR、RTCP NTP），不再重跑 DeltaStamp。
 *
 * Copyright (c) zero-media-server
 */
#include "zms/media/codec/codec_id.h"
#include "zms/zms_export.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zms_time_base {
    uint32_t num;
    uint32_t den;
} zms_time_base;

ZMS_API zms_time_base zms_time_base_ms(void);
ZMS_API zms_time_base zms_time_base_from_hz(uint32_t clock_hz);

/** 将 @a ts 从 @a from_tb 重标度到 @a to_tb（整数运算，av_rescale_q 风格）。 */
ZMS_API uint64_t zms_ts_rescale_q(uint64_t ts, zms_time_base from_tb, zms_time_base to_tb);

/** 按 rtpmap 时钟（Hz）将 RTP 时间戳换算为毫秒。 */
ZMS_API uint32_t zms_rtp_clock_to_ms(uint32_t rtp_ts, uint32_t clock_hz);
ZMS_API uint32_t zms_ms_to_rtp_clock(uint32_t ms, uint32_t clock_hz);

/** MPEG-TS / PCR 刻度，90 kHz（ITU-T H.222.0）。 */
ZMS_API uint64_t zms_ms_to_mpegts_90k(uint32_t ms);
ZMS_API uint32_t zms_mpegts_90k_to_ms(int64_t ticks_90k);

/** 由编解码与音频采样率得到标称帧时长。 */
ZMS_API uint32_t zms_codec_frame_duration_ms(zms_codec_id codec, uint32_t sample_rate_hz);

/**
 * 每轨 DeltaStamp，可选跨轨 sync master。
 * 仅用于入站。
 */
typedef struct zms_track_stamp {
    uint32_t max_delta_ms;
    int64_t last_stamp;
    int64_t last_delta;
    int64_t relative_ms;
    int64_t last_dts_in;
    int need_sync;
    struct zms_track_stamp *sync_master;
} zms_track_stamp;

ZMS_API void zms_track_stamp_reset(zms_track_stamp *s);
ZMS_API void zms_track_stamp_sync_to(zms_track_stamp *s, zms_track_stamp *master, int count);

/** @return 增量钳制与 A/V 同步后的相对毫秒。 */
ZMS_API uint32_t zms_track_stamp_revise(zms_track_stamp *s, uint64_t raw_ms);

/** 轻量流时钟：由原始 PTS 间隔推断帧时长。 */
typedef struct zms_stream_clock {
    zms_track_type track;
    uint32_t frame_dur_ms;
    int started;
    uint64_t last_raw_ms;
    uint32_t last_pts_ms;
    uint32_t learned_delta_ms;
} zms_stream_clock;

ZMS_API void zms_stream_clock_reset(zms_stream_clock *c);
ZMS_API void zms_stream_clock_init(zms_stream_clock *c, zms_track_type track, zms_codec_id codec,
                                   uint32_t sample_rate_hz);

/** @param raw_ms 入站 PTS（毫秒），来自 RTP 重标度或 RTMP tag。 */
ZMS_API uint32_t zms_stream_clock_advance(zms_stream_clock *c, uint64_t raw_ms);

/** 直播入站时间线：视频为主轨，音频经链接 stamp 跟随。 */
typedef struct zms_media_timeline {
    zms_track_stamp vst;
    zms_track_stamp ast;
    int stamp_linked;
    int av_clamp_disabled;
    /** MPEG-TS：demux PTS/DTS 已在共用轴上为毫秒；跳过 DeltaStamp 零原点。 */
    int linear_ms;
    zms_stream_clock video;
    zms_stream_clock audio;
    uint32_t video_clock_hz;
    uint32_t audio_clock_hz;
    zms_codec_id audio_codec;
} zms_media_timeline;

ZMS_API void zms_media_timeline_reset(zms_media_timeline *tl);
ZMS_API void zms_media_timeline_set_video(zms_media_timeline *tl, zms_codec_id codec,
                                          uint32_t clock_hz);
ZMS_API void zms_media_timeline_set_audio(zms_media_timeline *tl, zms_codec_id codec,
                                          uint32_t clock_hz);

ZMS_API uint32_t zms_media_timeline_video(zms_media_timeline *tl, uint64_t raw_ms);
ZMS_API uint32_t zms_media_timeline_audio(zms_media_timeline *tl, uint64_t raw_ms);

ZMS_API void zms_media_timeline_link_stamps(zms_media_timeline *tl);
/** 关闭 A/V relative_ms 钳制（MPEG-TS PES 原点可能不同；RTP 路径保持默认）。 */
ZMS_API void zms_media_timeline_set_av_clamp(zms_media_timeline *tl, int enabled);

/** 每轨减去自身首 PTS + 单调 DTS（单轨 FLV/HLS mux）。 */
typedef struct zms_mux_timeline {
    uint32_t origin_ms;
    int origin_set;
    uint32_t last_out_ms;
} zms_mux_timeline;

ZMS_API void zms_mux_timeline_reset(zms_mux_timeline *m);
ZMS_API uint32_t zms_mux_timeline_pts(zms_mux_timeline *m, uint32_t ring_pts_ms);

/**
 * 共享 FLV/MPEG-TS 时间线：各轨减去各自首 PTS，
 * 再强制全局单调 DTS（VLC 等播放器要求）。
 */
typedef struct zms_mux_av_timeline {
    uint32_t origin_ms[2];
    int origin_set[2];
    uint32_t last_out_ms[2];
    uint32_t shared_origin_ms;
    int use_shared_origin;
} zms_mux_av_timeline;

ZMS_API void zms_mux_av_timeline_reset(zms_mux_av_timeline *m);

/** 将双轨锚定到同一 ring PTS（如首个视频关键帧）。 */
ZMS_API void zms_mux_av_timeline_lock_origin(zms_mux_av_timeline *m, uint32_t origin_ms);

/** 追帧重定原点：移动 origin 但不重置 last_out_ms。 */
ZMS_API void zms_mux_av_timeline_shift_origin(zms_mux_av_timeline *m, uint32_t origin_ms);
ZMS_API uint32_t zms_mux_av_timeline_pts(zms_mux_av_timeline *m, zms_track_type track,
                                         uint32_t ring_pts_ms);

/** Unix 纪元以来的墙钟毫秒（RTCP SR）。 */
ZMS_API uint64_t zms_wall_ms(void);

/** 由墙钟毫秒生成 RFC 3550 NTP 时间戳（RTCP SR）。 */
ZMS_API void zms_wall_ms_to_ntp(uint64_t wall_ms, uint32_t *ntp_sec, uint32_t *ntp_frac);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_ENGINE_CLOCK_MEDIA_CLOCK_H */
