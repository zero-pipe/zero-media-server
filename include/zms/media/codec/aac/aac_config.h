#ifndef ZMS_CODEC_AAC_AAC_CONFIG_H
#define ZMS_CODEC_AAC_AAC_CONFIG_H

/**
 * @file aac_config.h
 * @brief AAC 配置门面（基于 zmk libflv mpeg4_aac_t 解析器）。
 *
 * 使业务/播放/容器层无需包含 `mpeg4-aac.h`：调用方按值持有 `zms_aac_config`，
 * 播种默认值 / 加载 AudioSpecificConfig，再序列化 ASC 或 ADTS 头而无需触碰原始库结构体。
 */
#include "zms/zms_export.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 按值不透明包装 `struct mpeg4_aac_t`。字节存储留足余量；
 * aac_config.c 静态断言可容纳底层库结构体。
 */
typedef struct zms_aac_config {
    union {
        uint8_t bytes[160];
        uint64_t align_;
    } opaque;
} zms_aac_config;

/** 重置为 @p sample_rate / @p channels 的 AAC-LC 默认（0 → 44100 / 2）。 */
ZMS_API void zms_aac_config_set_defaults(zms_aac_config *c, int sample_rate, int channels);

/** 加载 AudioSpecificConfig。@return 1 成功，0 失败。 */
ZMS_API int zms_aac_config_load_asc(zms_aac_config *c, const uint8_t *asc, size_t len);

/** 将 AudioSpecificConfig 序列化到 @p out。@return 写入字节数，或 0。 */
ZMS_API size_t zms_aac_config_save_asc(const zms_aac_config *c, uint8_t *out, size_t cap);

/** 为 @p payload_len 字节载荷写 ADTS 头。@return 头字节数（>=7），或 <0。 */
ZMS_API int zms_aac_config_adts_header(const zms_aac_config *c, uint16_t payload_len, uint8_t *out,
                                       size_t cap);

/** @return 解码采样率（Hz），未知则 <=0。 */
ZMS_API int zms_aac_config_sample_rate(const zms_aac_config *c);

/** @return 声道数（channel_configuration），未设置则为 0。 */
ZMS_API int zms_aac_config_channels(const zms_aac_config *c);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_CODEC_AAC_AAC_CONFIG_H */
