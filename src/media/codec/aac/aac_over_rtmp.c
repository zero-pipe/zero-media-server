#include "zms/media/codec/aac/aac_over_rtmp.h"
#include "zms/media/container/flv/flv_tag_probe.h"
#include "flv-header.h"
#include "flv-proto.h"
#include <stdio.h>
#include <string.h>

static const int k_aac_rates[] = {
    96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350,
};

int zms_rtmp_aac_extradata(const uint8_t *data, size_t len, const uint8_t **asc, size_t *asc_len)
{
    struct flv_audio_tag_header_t a;
    int hdr;

    if (!asc || !asc_len || !data || len < 3) {
        return 0;
    }
    if (zms_flv_tag_audio_codec(data, len) != ZMS_CODEC_AAC) {
        return 0;
    }
    hdr = flv_audio_tag_header_read(&a, data, len);
    if (hdr < 0 || a.avpacket != FLV_SEQUENCE_HEADER) {
        return 0;
    }
    *asc = data + (size_t)hdr;
    *asc_len = len - (size_t)hdr;
    return *asc_len > 0;
}

ztk_err_t zms_rtmp_aac_seq_header(const uint8_t *asc, size_t asc_len, uint8_t *out, size_t cap,
                                  size_t *out_len)
{
    if (!asc || asc_len == 0 || !out || cap < 2 + asc_len) {
        return ZTK_ERR_INVALID;
    }
    out[0] = 0xaf;
    out[1] = 0x00;
    memcpy(out + 2, asc, asc_len);
    if (out_len) {
        *out_len = 2 + asc_len;
    }
    return ZTK_OK;
}

ztk_err_t zms_rtmp_aac_frame(const uint8_t *aac, size_t len, uint32_t tag_dts_ms, uint8_t *out,
                             size_t cap, size_t *out_len)
{
    (void)tag_dts_ms;
    if (!aac || len == 0 || !out || cap < 2 + len) {
        return ZTK_ERR_INVALID;
    }
    out[0] = 0xaf;
    out[1] = 0x01;
    memcpy(out + 2, aac, len);
    if (out_len) {
        *out_len = 2 + len;
    }
    return ZTK_OK;
}

int zms_aac_build_config_hex(int sample_rate, int channels, char *hex, size_t hex_cap)
{
    if (!hex || hex_cap < 5) {
        return 0;
    }
    if (sample_rate <= 0) {
        sample_rate = 44100;
    }
    if (channels <= 0) {
        channels = 2;
    }
    int sr_idx = 4;
    for (int i = 0; i < (int)(sizeof(k_aac_rates) / sizeof(k_aac_rates[0])); ++i) {
        if (k_aac_rates[i] == sample_rate) {
            sr_idx = i;
            break;
        }
    }
    int n = snprintf(hex, hex_cap, "%02x%02x", (unsigned)(((2 << 3) | (sr_idx >> 1)) & 0xff),
                     (unsigned)((((sr_idx & 1) << 7) | (channels << 3)) & 0xff));
    return n >= 4 ? 1 : 0;
}

int zms_aac_adts_parse(const uint8_t *adts, size_t len, int *sample_rate, int *channels)
{
    int sr_idx;
    int ch;

    if (!adts || len < 7 || adts[0] != 0xff || (adts[1] & 0xf0) != 0xf0) {
        return 0;
    }
    sr_idx = (adts[2] & 0x3c) >> 2;
    ch = ((adts[2] & 0x01) << 2) | ((adts[3] & 0xc0) >> 6);
    if (sr_idx < 0 || sr_idx >= (int)(sizeof(k_aac_rates) / sizeof(k_aac_rates[0]))) {
        return 0;
    }
    if (ch <= 0 || ch > 7) {
        return 0;
    }
    if (sample_rate) {
        *sample_rate = k_aac_rates[sr_idx];
    }
    if (channels) {
        *channels = ch;
    }
    return 1;
}

int zms_aac_parse_asc(const uint8_t *asc, size_t len, int *sample_rate, int *channels)
{
    int sr_idx;
    int ch;

    if (!asc || len < 2) {
        return 0;
    }
    sr_idx = ((asc[0] & 7) << 1) | ((asc[1] >> 7) & 1);
    ch = (asc[1] >> 3) & 0x0f;
    if (sr_idx < 0 || sr_idx >= (int)(sizeof(k_aac_rates) / sizeof(k_aac_rates[0]))) {
        return 0;
    }
    if (ch == 0 || ch > 7) {
        return 0;
    }
    if (sample_rate) {
        *sample_rate = k_aac_rates[sr_idx];
    }
    if (channels) {
        *channels = ch;
    }
    return 1;
}
