#ifndef ZMS_CONTAINER_HLS_FMP4_H
#define ZMS_CONTAINER_HLS_FMP4_H

/**
 * @file hls_fmp4.h
 * @brief fMP4（CMAF）分片 muxer 门面（基于 zmk libhls hls_fmp4 API）。
 *
 * 使播放/业务层无需包含 `hls-fmp4.h` 与 `mov-format.h`：按 zms_codec_id 添加轨，
 * 以 ZMS flag 位喂样本，门面映射到底层 MOV_OBJECT_* / MOV_AV_FLAG_*。
 */
#include "zms/zms_export.h"
#include "zms/media/codec/codec_id.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zms_hls_fmp4 zms_hls_fmp4;

/** 分片数据 sink（一条 init/媒体 fragment）。@return 0 成功。 */
typedef int (*zms_hls_fmp4_segment_cb)(void *param, const void *data, size_t bytes, int64_t pts_ms,
                                       int64_t dts_ms, int64_t duration_ms);

/** zms_hls_fmp4_write_frame() 的样本标志。 */
#define ZMS_HLS_FMP4_FLAG_KEYFRAME 0x01
#define ZMS_HLS_FMP4_FLAG_SEGMENT_DISABLE 0x02

/** @param segment_ms 分片时长（毫秒）；0 → 每个视频关键帧一片。 */
ZMS_API zms_hls_fmp4 *zms_hls_fmp4_create(int64_t segment_ms, zms_hls_fmp4_segment_cb cb,
                                          void *param);
ZMS_API void zms_hls_fmp4_destroy(zms_hls_fmp4 *m);

/** 为 @p codec 添加视频轨。@return 轨 id（>=0），<0 错误。 */
ZMS_API int zms_hls_fmp4_add_video(zms_hls_fmp4 *m, zms_codec_id codec, int width, int height,
                                   const void *extra, size_t extra_len);

/** 为 @p codec 添加音频轨。@return 轨 id（>=0），<0 错误。 */
ZMS_API int zms_hls_fmp4_add_audio(zms_hls_fmp4 *m, zms_codec_id codec, int channels,
                                   int bits_per_sample, int sample_rate, const void *extra,
                                   size_t extra_len);

/** 写入一帧样本（H.264/H.265 需 length-prefixed）。@param flags ZMS_HLS_FMP4_FLAG_*。@return 0 成功。*/
ZMS_API int zms_hls_fmp4_write_frame(zms_hls_fmp4 *m, int track, const void *data, size_t bytes,
                                     int64_t pts_ms, int64_t dts_ms, int flags);

/** 序列化 init 分片。@return 写入字节数（>0），<0 错误。 */
ZMS_API int zms_hls_fmp4_init_segment(zms_hls_fmp4 *m, void *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_CONTAINER_HLS_FMP4_H */
