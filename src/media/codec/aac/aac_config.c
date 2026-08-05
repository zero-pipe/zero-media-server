/**
 * @file aac_config.c
 * @brief AAC 配置门面实现（封装 zmk libflv mpeg4_aac_t）。
 *
 * Copyright (c) zero-media-server
 */
#include "zms/media/codec/aac/aac_config.h"
#include "mpeg4-aac.h"
#include <string.h>

/* 不透明存储须足够大且对齐，以容纳 lib 结构体。 */
typedef char zms_aac_config_size_check
    [(sizeof(struct mpeg4_aac_t) <= sizeof(((zms_aac_config *)0)->opaque)) ? 1 : -1];

static struct mpeg4_aac_t *aac_of(zms_aac_config *c)
{
    return (struct mpeg4_aac_t *)(void *)c->opaque.bytes;
}

static const struct mpeg4_aac_t *aac_const(const zms_aac_config *c)
{
    return (const struct mpeg4_aac_t *)(const void *)c->opaque.bytes;
}

void zms_aac_config_set_defaults(zms_aac_config *c, int sample_rate, int channels)
{
    struct mpeg4_aac_t *a;
    int freq_idx;
    int sr = sample_rate > 0 ? sample_rate : 44100;
    int ch = channels > 0 ? channels : 2;

    if (!c) {
        return;
    }
    a = aac_of(c);
    memset(a, 0, sizeof(*a));
    a->profile = MPEG4_AAC_LC;
    freq_idx = mpeg4_aac_audio_frequency_from(sr);
    a->sampling_frequency_index = freq_idx >= 0 ? (uint8_t)freq_idx : (uint8_t)MPEG4_AAC_44100;
    a->channel_configuration = (uint8_t)ch;
    a->channels = (uint8_t)ch;
    a->sampling_frequency = (uint32_t)sr;
    a->extension_frequency = (uint32_t)sr;
}

int zms_aac_config_load_asc(zms_aac_config *c, const uint8_t *asc, size_t len)
{
    if (!c || !asc || len == 0) {
        return 0;
    }
    return mpeg4_aac_audio_specific_config_load(asc, len, aac_of(c)) > 0 ? 1 : 0;
}

size_t zms_aac_config_save_asc(const zms_aac_config *c, uint8_t *out, size_t cap)
{
    int n;

    if (!c || !out) {
        return 0;
    }
    n = mpeg4_aac_audio_specific_config_save(aac_const(c), out, cap);
    return n > 0 ? (size_t)n : 0;
}

int zms_aac_config_adts_header(const zms_aac_config *c, uint16_t payload_len, uint8_t *out,
                               size_t cap)
{
    if (!c || !out) {
        return -1;
    }
    return mpeg4_aac_adts_save(aac_const(c), payload_len, out, cap);
}

int zms_aac_config_sample_rate(const zms_aac_config *c)
{
    const struct mpeg4_aac_t *a;

    if (!c) {
        return -1;
    }
    a = aac_const(c);
    if (a->sampling_frequency > 0) {
        return (int)a->sampling_frequency;
    }
    return mpeg4_aac_audio_frequency_to((enum mpeg4_aac_frequency)a->sampling_frequency_index);
}

int zms_aac_config_channels(const zms_aac_config *c)
{
    return c ? (int)aac_const(c)->channel_configuration : 0;
}
