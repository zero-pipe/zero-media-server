#include "zms/media/codec/h266/h266_over_rtmp.h"
#include "zms/media/container/flv/flv_tag_probe.h"
#include "mpeg4-vvc.h"
#include "flv-header.h"
#include "flv-proto.h"
#include <string.h>

static int h266_write_video_hdr(uint8_t *out, size_t cap, int key, int seq_hdr)
{
    struct flv_video_tag_header_t video;
    int n;

    memset(&video, 0, sizeof(video));
    video.codecid = FLV_VIDEO_H266;
    video.enhanced_rtmp = 1;
    video.keyframe = key ? FLV_VIDEO_KEY_FRAME : FLV_VIDEO_INTER_FRAME;
    video.avpacket = seq_hdr ? FLV_SEQUENCE_HEADER : FLV_AVPACKET;
    n = flv_video_tag_header_write(&video, out, cap);
    return n;
}

static const uint8_t *locate_vvcc(const uint8_t *data, size_t len, size_t *vvcc_len)
{
    struct flv_video_tag_header_t vh;
    int hdr;

    if (!data || !vvcc_len || len < 2) {
        return NULL;
    }
    if (zms_flv_tag_video_codec(data, len) == ZMS_CODEC_H266) {
        hdr = flv_video_tag_header_read(&vh, data, len);
        if (hdr > 0 && vh.avpacket == FLV_SEQUENCE_HEADER && (size_t)hdr < len) {
            *vvcc_len = len - (size_t)hdr;
            return data + hdr;
        }
    }
    *vvcc_len = len;
    return data;
}

int zms_h266_over_rtmp_config_extradata(const uint8_t *data, size_t len, const uint8_t **vvcc,
                                        size_t *vvcc_len)
{
    if (!data || len < 2 || !vvcc || !vvcc_len) {
        return 0;
    }
    *vvcc = locate_vvcc(data, len, vvcc_len);
    return *vvcc && *vvcc_len > 0;
}

int zms_h266_video_config_vvcc(const uint8_t *cfg, size_t cfg_len, const uint8_t **vvcc,
                               size_t *vvcc_len)
{
    return zms_h266_over_rtmp_config_extradata(cfg, cfg_len, vvcc, vvcc_len);
}

ztk_err_t zms_h266_flv_sequence_header(const uint8_t *cfg, size_t cfg_len, uint8_t *out, size_t cap,
                                       size_t *out_len)
{
    struct mpeg4_vvc_t vvc;
    const uint8_t *vvcc = NULL;
    size_t vvcc_len = 0;
    int hdr;
    int n;

    if (!cfg || cfg_len < 2 || !out || !out_len) {
        return ZTK_ERR_INVALID;
    }

    if (zms_flv_tag_video_codec(cfg, cfg_len) == ZMS_CODEC_H266) {
        struct flv_video_tag_header_t vh;
        int rh = flv_video_tag_header_read(&vh, cfg, cfg_len);
        if (rh > 0 && vh.avpacket == FLV_SEQUENCE_HEADER && cfg_len <= cap) {
            memcpy(out, cfg, cfg_len);
            *out_len = cfg_len;
            return ZTK_OK;
        }
    }

    if (!zms_h266_video_config_vvcc(cfg, cfg_len, &vvcc, &vvcc_len) || !vvcc || vvcc_len < 7) {
        return ZTK_ERR_INVALID;
    }

    memset(&vvc, 0, sizeof(vvc));
    if (mpeg4_vvc_decoder_configuration_record_load(vvcc, vvcc_len, &vvc) <= 0) {
        return ZTK_ERR_INVALID;
    }

    hdr = h266_write_video_hdr(out, cap, 1, 1);
    if (hdr < 0) {
        return ZTK_ERR_BUFFER_TOO_SMALL;
    }
    n = mpeg4_vvc_decoder_configuration_record_save(&vvc, out + hdr, cap - (size_t)hdr);
    if (n <= 0) {
        return ZTK_ERR_INVALID;
    }
    *out_len = (size_t)hdr + (size_t)n;
    return ZTK_OK;
}

ztk_err_t zms_h266_over_rtmp_pack_es(const uint8_t *video_cfg, size_t cfg_len,
                                     const uint8_t *annexb, size_t len, int key, uint8_t *out,
                                     size_t cap, size_t *out_len)
{
    struct mpeg4_vvc_t vvc;
    const uint8_t *vvcc = NULL;
    size_t vvcc_len = 0;
    int vcl = 0;
    int update = 0;
    int hdr;
    int n;

    if (!annexb || !out || len == 0) {
        return ZTK_ERR_INVALID;
    }
    if (cap < 8) {
        return ZTK_ERR_BUFFER_TOO_SMALL;
    }

    memset(&vvc, 0, sizeof(vvc));
    if (video_cfg && zms_h266_video_config_vvcc(video_cfg, cfg_len, &vvcc, &vvcc_len) && vvcc &&
        vvcc_len >= 7) {
        (void)mpeg4_vvc_decoder_configuration_record_load(vvcc, vvcc_len, &vvc);
    }

    hdr = h266_write_video_hdr(out, cap, key, 0);
    if (hdr < 0) {
        return ZTK_ERR_BUFFER_TOO_SMALL;
    }
    n = h266_annexbtomp4(&vvc, annexb, len, out + hdr, cap - (size_t)hdr, &vcl, &update);
    if (n <= 0) {
        return ZTK_ERR_INVALID;
    }
    if (out_len) {
        *out_len = (size_t)hdr + (size_t)n;
    }
    return ZTK_OK;
}
