#ifndef ZMS_CODEC_H264_SPS_H
#define ZMS_CODEC_H264_SPS_H

#include "zms/zms_export.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zms_h264_sps_info {
    int width;
    int height;
    float fps;
} zms_h264_sps_info;

/** 解析 H264 SPS NAL（含 0x67 头字节）。成功返回 1。 */
ZMS_API int zms_h264_sps_parse(const uint8_t *sps, size_t len, zms_h264_sps_info *info);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_CODEC_H264_SPS_H */
