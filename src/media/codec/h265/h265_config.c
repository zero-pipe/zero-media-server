/**
 * @file h265_config.c
 * @brief H.265 配置门面实现（封装 zmk libflv mpeg4_hevc_t）。
 *
 * Copyright (c) zero-media-server
 */
#include "zms/media/codec/h265/h265_config.h"
#include "mpeg4-hevc.h"
#include <string.h>

typedef char zms_hevc_config_size_check
    [(sizeof(struct mpeg4_hevc_t) <= sizeof(((zms_hevc_config *)0)->opaque)) ? 1 : -1];

static struct mpeg4_hevc_t *hevc_of(zms_hevc_config *c)
{
    return (struct mpeg4_hevc_t *)(void *)c->opaque.bytes;
}

static const struct mpeg4_hevc_t *hevc_const(const zms_hevc_config *c)
{
    return (const struct mpeg4_hevc_t *)(const void *)c->opaque.bytes;
}

int zms_hevc_config_load_record(zms_hevc_config *c, const uint8_t *hvcc, size_t len)
{
    if (!c || !hvcc || len == 0) {
        return -1;
    }
    return mpeg4_hevc_decoder_configuration_record_load(hvcc, len, hevc_of(c));
}

int zms_hevc_config_length_size(const zms_hevc_config *c)
{
    return c ? (int)hevc_const(c)->lengthSizeMinusOne + 1 : 0;
}

int zms_hevc_config_mp4_to_annexb(const zms_hevc_config *c, const uint8_t *mp4, size_t len,
                                  uint8_t *out, size_t cap)
{
    if (!c || !mp4 || len == 0 || !out) {
        return -1;
    }
    return h265_mp4toannexb(hevc_const(c), mp4, len, out, cap);
}

int zms_hevc_config_annexb_to_mp4(zms_hevc_config *c, const uint8_t *annexb, size_t len,
                                  uint8_t *out, size_t cap, int *vcl, int *update)
{
    int v = 0;
    int u = 0;
    int n;

    if (!c || !annexb || len == 0 || !out) {
        return -1;
    }
    n = h265_annexbtomp4(hevc_of(c), annexb, len, out, cap, &v, &u);
    if (vcl) {
        *vcl = v;
    }
    if (update) {
        *update = u;
    }
    return n;
}

int zms_hevc_nalu_to_annexb(int length_size, const uint8_t *mp4, size_t len, uint8_t *out,
                            size_t cap)
{
    struct mpeg4_hevc_t hevc;

    if (!mp4 || len == 0 || !out) {
        return -1;
    }
    memset(&hevc, 0, sizeof(hevc));
    if (length_size > 0) {
        hevc.lengthSizeMinusOne = (uint8_t)(length_size - 1);
    }
    return h265_mp4toannexb(&hevc, mp4, len, out, cap);
}
