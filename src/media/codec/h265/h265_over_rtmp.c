#include "zms/media/codec/h265/h265_over_rtmp.h"
#include "zms/media/container/flv/flv_tag_probe.h"
#include "mpeg4-hevc.h"
#include "flv-header.h"
#include "flv-proto.h"
#include <string.h>

int zms_rtmp_hevc_extradata(const uint8_t *data, size_t len, const uint8_t **hvcc, size_t *hvcc_len)
{
    return zms_h265_video_config_hvcc(data, len, hvcc, hvcc_len);
}

static int h265_write_video_hdr(uint8_t *out, size_t cap, int key, int seq_hdr)
{
    struct flv_video_tag_header_t video;
    int n;

    memset(&video, 0, sizeof(video));
    video.codecid = FLV_VIDEO_H265;
    video.enhanced_rtmp = 1;
    video.keyframe = key ? FLV_VIDEO_KEY_FRAME : FLV_VIDEO_INTER_FRAME;
    video.avpacket = seq_hdr ? FLV_SEQUENCE_HEADER : FLV_AVPACKET;
    n = flv_video_tag_header_write(&video, out, cap);
    return n;
}

ztk_err_t zms_h265_over_rtmp_pack_es(const uint8_t *video_cfg, size_t cfg_len,
                                     const uint8_t *annexb, size_t len, int key, uint8_t *out,
                                     size_t cap, size_t *out_len)
{
    struct mpeg4_hevc_t hevc;
    const uint8_t *hvcc = NULL;
    size_t hvcc_len = 0;
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

    memset(&hevc, 0, sizeof(hevc));
    if (video_cfg && zms_rtmp_hevc_extradata(video_cfg, cfg_len, &hvcc, &hvcc_len) && hvcc &&
        hvcc_len >= 7) {
        (void)mpeg4_hevc_decoder_configuration_record_load(hvcc, hvcc_len, &hevc);
    }

    hdr = h265_write_video_hdr(out, cap, key, 0);
    if (hdr < 0) {
        return ZTK_ERR_BUFFER_TOO_SMALL;
    }
    n = h265_annexbtomp4(&hevc, annexb, len, out + hdr, cap - (size_t)hdr, &vcl, &update);
    if (n <= 0) {
        return ZTK_ERR_INVALID;
    }
    if (out_len) {
        *out_len = (size_t)hdr + (size_t)n;
    }
    return ZTK_OK;
}

ztk_err_t zms_h265_flv_sequence_header(const uint8_t *cfg, size_t cfg_len, uint8_t *out, size_t cap,
                                       size_t *out_len)
{
    struct flv_video_tag_header_t vh;
    struct mpeg4_hevc_t hevc;
    const uint8_t *hvcc = NULL;
    size_t hvcc_len = 0;
    int hdr;
    int n;

    if (!cfg || cfg_len < 7 || !out || !out_len) {
        return ZTK_ERR_INVALID;
    }

    if (zms_flv_tag_video_codec(cfg, cfg_len) == ZMS_CODEC_H265) {
        memset(&vh, 0, sizeof(vh));
        hdr = flv_video_tag_header_read(&vh, cfg, cfg_len);
        if (hdr > 0 && vh.avpacket == FLV_SEQUENCE_HEADER && cfg_len <= cap) {
            memcpy(out, cfg, cfg_len);
            *out_len = cfg_len;
            return ZTK_OK;
        }
    }

    if (!zms_h265_video_config_hvcc(cfg, cfg_len, &hvcc, &hvcc_len) || !hvcc || hvcc_len < 7) {
        return ZTK_ERR_INVALID;
    }

    memset(&hevc, 0, sizeof(hevc));
    if (mpeg4_hevc_decoder_configuration_record_load(hvcc, hvcc_len, &hevc) <= 0) {
        return ZTK_ERR_INVALID;
    }

    memset(&vh, 0, sizeof(vh));
    vh.codecid = FLV_VIDEO_H265;
    vh.enhanced_rtmp = 1;
    vh.avpacket = FLV_SEQUENCE_HEADER;
    vh.keyframe = FLV_VIDEO_KEY_FRAME;
    hdr = flv_video_tag_header_write(&vh, out, cap);
    if (hdr < 0) {
        return ZTK_ERR_BUFFER_TOO_SMALL;
    }
    n = mpeg4_hevc_decoder_configuration_record_save(&hevc, out + hdr, cap - (size_t)hdr);
    if (n <= 0) {
        return ZTK_ERR_INVALID;
    }
    *out_len = (size_t)hdr + (size_t)n;
    return ZTK_OK;
}
