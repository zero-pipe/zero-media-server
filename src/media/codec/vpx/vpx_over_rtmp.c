#include "zms/media/codec/vpx/vpx_over_rtmp.h"
#include "zms/media/container/flv/flv_tag_probe.h"
#include "webm-vpx.h"
#include "flv-header.h"
#include "flv-proto.h"
#include <string.h>

static uint32_t vpx_fourcc(zms_codec_id codec)
{
    if (codec == ZMS_CODEC_VP9) {
        return FLV_VIDEO_FOURCC_VP9;
    }
    return FLV_VIDEO_FOURCC_VP8;
}

static int vpx_write_video_hdr(uint8_t *out, size_t cap, uint32_t fourcc, int key, int seq_hdr)
{
    if (!out || cap < 5) {
        return -1;
    }
    out[0] = (uint8_t)(0x80 | (key ? FLV_VIDEO_KEY_FRAME : FLV_VIDEO_INTER_FRAME) << 4 |
                       (seq_hdr ? FLV_SEQUENCE_HEADER : FLV_PACKET_TYPE_CODED_FRAMES_X));
    out[1] = (uint8_t)((fourcc >> 24) & 0xff);
    out[2] = (uint8_t)((fourcc >> 16) & 0xff);
    out[3] = (uint8_t)((fourcc >> 8) & 0xff);
    out[4] = (uint8_t)(fourcc & 0xff);
    return 5;
}

static const uint8_t *locate_vpxc(const uint8_t *data, size_t len, zms_codec_id codec,
                                  size_t *vpxc_len)
{
    struct flv_video_tag_header_t vh;
    int hdr;

    if (!data || !vpxc_len || len < 2) {
        return NULL;
    }
    if (zms_flv_tag_video_codec(data, len) == codec) {
        hdr = flv_video_tag_header_read(&vh, data, len);
        if (hdr > 0 && vh.avpacket == FLV_SEQUENCE_HEADER && (size_t)hdr < len) {
            *vpxc_len = len - (size_t)hdr;
            return data + hdr;
        }
    }
    *vpxc_len = len;
    return data;
}

int zms_vpx_over_rtmp_config_extradata(zms_codec_id codec, const uint8_t *data, size_t len,
                                       const uint8_t **vpxc, size_t *vpxc_len)
{
    if (!data || len < 2 || !vpxc || !vpxc_len) {
        return 0;
    }
    if (codec != ZMS_CODEC_VP8 && codec != ZMS_CODEC_VP9) {
        return 0;
    }
    *vpxc = locate_vpxc(data, len, codec, vpxc_len);
    return *vpxc && *vpxc_len > 0;
}

ztk_err_t zms_vpx_flv_sequence_header(zms_codec_id codec, const uint8_t *cfg, size_t cfg_len,
                                      uint8_t *out, size_t cap, size_t *out_len)
{
    struct webm_vpx_t vpx;
    const uint8_t *vpxc = NULL;
    size_t vpxc_len = 0;
    uint32_t fourcc;
    int hdr;
    int n;

    if (!cfg || cfg_len < 2 || !out || !out_len) {
        return ZTK_ERR_INVALID;
    }
    if (codec != ZMS_CODEC_VP8 && codec != ZMS_CODEC_VP9) {
        return ZTK_ERR_INVALID;
    }

    if (zms_flv_tag_video_codec(cfg, cfg_len) == codec && cfg_len <= cap) {
        struct flv_video_tag_header_t vh;
        int rh = flv_video_tag_header_read(&vh, cfg, cfg_len);
        if (rh > 0 && vh.avpacket == FLV_SEQUENCE_HEADER) {
            memcpy(out, cfg, cfg_len);
            *out_len = cfg_len;
            return ZTK_OK;
        }
    }

    if (!zms_vpx_over_rtmp_config_extradata(codec, cfg, cfg_len, &vpxc, &vpxc_len) || !vpxc ||
        vpxc_len < 4) {
        return ZTK_ERR_INVALID;
    }

    memset(&vpx, 0, sizeof(vpx));
    if (webm_vpx_codec_configuration_record_load(vpxc, vpxc_len, &vpx) <= 0) {
        return ZTK_ERR_INVALID;
    }

    fourcc = vpx_fourcc(codec);
    hdr = vpx_write_video_hdr(out, cap, fourcc, 1, 1);
    if (hdr < 0) {
        return ZTK_ERR_BUFFER_TOO_SMALL;
    }
    n = webm_vpx_codec_configuration_record_save(&vpx, out + hdr, cap - (size_t)hdr);
    if (n <= 0) {
        return ZTK_ERR_INVALID;
    }
    *out_len = (size_t)hdr + (size_t)n;
    return ZTK_OK;
}

ztk_err_t zms_vpx_over_rtmp_pack_es(zms_codec_id codec, const uint8_t *video_cfg, size_t cfg_len,
                                    const uint8_t *es, size_t len, int key, uint8_t *out,
                                    size_t cap, size_t *out_len)
{
    uint32_t fourcc;
    int hdr;

    (void)video_cfg;
    (void)cfg_len;
    if (codec != ZMS_CODEC_VP8 && codec != ZMS_CODEC_VP9) {
        return ZTK_ERR_INVALID;
    }
    if (!es || !out || len == 0) {
        return ZTK_ERR_INVALID;
    }
    if (cap < len + 8) {
        return ZTK_ERR_BUFFER_TOO_SMALL;
    }

    fourcc = vpx_fourcc(codec);
    hdr = vpx_write_video_hdr(out, cap, fourcc, key, 0);
    if (hdr < 0) {
        return ZTK_ERR_BUFFER_TOO_SMALL;
    }
    memcpy(out + hdr, es, len);
    if (out_len) {
        *out_len = (size_t)hdr + len;
    }
    return ZTK_OK;
}
