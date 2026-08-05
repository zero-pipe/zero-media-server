#include "zms/media/codec/g711/g711_over_rtmp.h"
#include "flv-proto.h"
#include <string.h>

static uint8_t g711_flv_audio_hdr(zms_codec_id codec)
{
    uint8_t fmt = (codec == ZMS_CODEC_G711U) ? FLV_AUDIO_G711U : FLV_AUDIO_G711A;
    return (uint8_t)(fmt | (FLV_SOUND_RATE_5500 << 2) | (FLV_SOUND_BIT_16 << 1) |
                     FLV_SOUND_CHANNEL_MONO);
}

ztk_err_t zms_g711_over_rtmp_pack_es(zms_codec_id codec, const uint8_t *g711, size_t len,
                                     uint32_t tag_dts_ms, uint8_t *out, size_t cap, size_t *out_len)
{
    (void)tag_dts_ms;
    if (!g711 || len == 0 || !out || (codec != ZMS_CODEC_G711A && codec != ZMS_CODEC_G711U)) {
        return ZTK_ERR_INVALID;
    }
    if (cap < 1 + len) {
        return ZTK_ERR_BUFFER_TOO_SMALL;
    }
    out[0] = g711_flv_audio_hdr(codec);
    memcpy(out + 1, g711, len);
    if (out_len) {
        *out_len = 1 + len;
    }
    return ZTK_OK;
}
