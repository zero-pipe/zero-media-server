/**
 * @file h264_config.c
 * @brief H.264 配置门面实现（封装 zmk libflv mpeg4_avc_t）。
 *
 * Copyright (c) zero-media-server
 */
#include "zms/media/codec/h264/h264_config.h"
#include "mpeg4-avc.h"
#include <string.h>

typedef char zms_avc_config_size_check
    [(sizeof(struct mpeg4_avc_t) <= sizeof(((zms_avc_config *)0)->opaque)) ? 1 : -1];

static struct mpeg4_avc_t *avc_of(zms_avc_config *c)
{
    return (struct mpeg4_avc_t *)(void *)c->opaque.bytes;
}

static const struct mpeg4_avc_t *avc_const(const zms_avc_config *c)
{
    return (const struct mpeg4_avc_t *)(const void *)c->opaque.bytes;
}

int zms_avc_config_load_record(zms_avc_config *c, const uint8_t *avcc, size_t len)
{
    if (!c || !avcc || len == 0) {
        return -1;
    }
    return mpeg4_avc_decoder_configuration_record_load(avcc, len, avc_of(c));
}

int zms_avc_config_load_annexb(zms_avc_config *c, const uint8_t *annexb, size_t len)
{
    if (!c || !annexb || len == 0) {
        return -1;
    }
    return mpeg4_avc_from_nalu(annexb, len, avc_of(c));
}

int zms_avc_config_nalu_length(const zms_avc_config *c)
{
    return c ? (int)avc_const(c)->nalu : 0;
}

int zms_avc_config_mp4_to_annexb(const zms_avc_config *c, const uint8_t *mp4, size_t len,
                                 uint8_t *out, size_t cap)
{
    if (!c || !mp4 || len == 0 || !out) {
        return -1;
    }
    return h264_mp4toannexb(avc_const(c), mp4, len, out, cap);
}

int zms_avc_config_annexb_to_mp4(zms_avc_config *c, const uint8_t *annexb, size_t len, uint8_t *out,
                                 size_t cap, int *vcl, int *update)
{
    int v = 0;
    int u = 0;
    int n;

    if (!c || !annexb || len == 0 || !out) {
        return -1;
    }
    n = h264_annexbtomp4(avc_of(c), annexb, len, out, cap, &v, &u);
    if (vcl) {
        *vcl = v;
    }
    if (update) {
        *update = u;
    }
    return n;
}

int zms_avc_nalu_to_annexb(int nalu_length, const uint8_t *mp4, size_t len, uint8_t *out,
                           size_t cap)
{
    struct mpeg4_avc_t avc;

    if (!mp4 || len == 0 || !out) {
        return -1;
    }
    memset(&avc, 0, sizeof(avc));
    if (nalu_length > 0) {
        avc.nalu = (uint8_t)nalu_length;
    }
    return h264_mp4toannexb(&avc, mp4, len, out, cap);
}
