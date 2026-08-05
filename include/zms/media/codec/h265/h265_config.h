#ifndef ZMS_CODEC_H265_CONFIG_H
#define ZMS_CODEC_H265_CONFIG_H

/**
 * @file h265_config.h
 * @brief H.265 参数集配置门面（基于 zmk libflv mpeg4_hevc_t）。
 *
 * 使业务/播放层无需包含 `mpeg4-hevc.h`。以稳定地址按值持有 `zms_hevc_config` 并传其指针；
 * 加载后切勿拷贝（库结构体内指针指向自有缓冲）。
 */
#include "zms/zms_export.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 按值不透明包装 `struct mpeg4_hevc_t`；h265_config.c 断言尺寸匹配。 */
typedef struct zms_hevc_config {
    union {
        uint8_t bytes[6144];
        uint64_t align_;
    } opaque;
} zms_hevc_config;

/** 解析 HEVCDecoderConfigurationRecord（hvcC）。@return >0 成功，<=0 错误。 */
ZMS_API int zms_hevc_config_load_record(zms_hevc_config *c, const uint8_t *hvcc, size_t len);

/** @return NAL 长度前缀字节数（1..4，== lengthSizeMinusOne+1）。 */
ZMS_API int zms_hevc_config_length_size(const zms_hevc_config *c);

/** hvcC 长度前缀样本 → Annex-B（按配置前置 VPS/SPS/PPS）。@return >0 字节数，<=0 错误。 */
ZMS_API int zms_hevc_config_mp4_to_annexb(const zms_hevc_config *c, const uint8_t *mp4, size_t len,
                                          uint8_t *out, size_t cap);

/** Annex-B AU → hvcC 长度前缀（更新配置参数集）。@p vcl/@p update 可选。@return >0 字节数。 */
ZMS_API int zms_hevc_config_annexb_to_mp4(zms_hevc_config *c, const uint8_t *annexb, size_t len,
                                          uint8_t *out, size_t cap, int *vcl, int *update);

/** 仅用长度前缀做裸 hvcC→Annex-B（无参数集），供格式探测。 */
ZMS_API int zms_hevc_nalu_to_annexb(int length_size, const uint8_t *mp4, size_t len, uint8_t *out,
                                    size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_CODEC_H265_CONFIG_H */
