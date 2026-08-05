/**
 * @file hls_fmp4.c
 * @brief fMP4 分片 muxer 门面实现（封装 zmk libhls hls_fmp4）。
 *
 * Copyright (c) zero-media-server
 */
#include "zms/media/container/hls/hls_fmp4.h"
#include "hls-fmp4.h"
#include "mov-format.h"

static uint8_t mov_object_of_codec(zms_codec_id codec)
{
    switch (codec) {
    case ZMS_CODEC_H264:
        return MOV_OBJECT_H264;
    case ZMS_CODEC_H265:
        return MOV_OBJECT_H265;
    case ZMS_CODEC_AV1:
        return MOV_OBJECT_AV1;
    case ZMS_CODEC_VP9:
        return MOV_OBJECT_VP9;
    case ZMS_CODEC_VP8:
        return MOV_OBJECT_VP8;
    case ZMS_CODEC_AAC:
        return MOV_OBJECT_AAC;
    case ZMS_CODEC_OPUS:
        return MOV_OBJECT_OPUS;
    default:
        return 0;
    }
}

static int mov_flags_of(int flags)
{
    int out = 0;

    if (flags & ZMS_HLS_FMP4_FLAG_KEYFRAME) {
        out |= MOV_AV_FLAG_KEYFREAME;
    }
    if (flags & ZMS_HLS_FMP4_FLAG_SEGMENT_DISABLE) {
        out |= MOV_AV_FLAG_SEGMENT_DISABLE;
    }
    return out;
}

zms_hls_fmp4 *zms_hls_fmp4_create(int64_t segment_ms, zms_hls_fmp4_segment_cb cb, void *param)
{
    return (zms_hls_fmp4 *)hls_fmp4_create(segment_ms, (hls_fmp4_handler)cb, param);
}

void zms_hls_fmp4_destroy(zms_hls_fmp4 *m)
{
    hls_fmp4_destroy((hls_fmp4_t *)m);
}

int zms_hls_fmp4_add_video(zms_hls_fmp4 *m, zms_codec_id codec, int width, int height,
                           const void *extra, size_t extra_len)
{
    return hls_fmp4_add_video((hls_fmp4_t *)m, mov_object_of_codec(codec), width, height, extra,
                              extra_len);
}

int zms_hls_fmp4_add_audio(zms_hls_fmp4 *m, zms_codec_id codec, int channels, int bits_per_sample,
                           int sample_rate, const void *extra, size_t extra_len)
{
    return hls_fmp4_add_audio((hls_fmp4_t *)m, mov_object_of_codec(codec), channels,
                              bits_per_sample, sample_rate, extra, extra_len);
}

int zms_hls_fmp4_write_frame(zms_hls_fmp4 *m, int track, const void *data, size_t bytes,
                             int64_t pts_ms, int64_t dts_ms, int flags)
{
    return hls_fmp4_input((hls_fmp4_t *)m, track, data, bytes, pts_ms, dts_ms, mov_flags_of(flags));
}

int zms_hls_fmp4_init_segment(zms_hls_fmp4 *m, void *out, size_t cap)
{
    return hls_fmp4_init_segment((hls_fmp4_t *)m, out, cap);
}
