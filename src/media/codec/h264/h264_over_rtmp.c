#include "zms/media/codec/h264/h264_over_rtmp.h"
#include "zms/media/codec/h264/h264_sps.h"
#include "mpeg4-avc.h"
#include "flv-header.h"
#include "flv-proto.h"
#include <stdio.h>
#include <string.h>

static int h264_write_video_hdr(uint8_t *out, size_t cap, int key, int seq_hdr)
{
    struct flv_video_tag_header_t video;
    int n;

    memset(&video, 0, sizeof(video));
    video.codecid = FLV_VIDEO_H264;
    video.keyframe = key ? FLV_VIDEO_KEY_FRAME : FLV_VIDEO_INTER_FRAME;
    video.avpacket = seq_hdr ? FLV_SEQUENCE_HEADER : FLV_AVPACKET;
    n = flv_video_tag_header_write(&video, out, cap);
    return n;
}

static const uint8_t *locate_avcc(const uint8_t *data, size_t len, size_t *avcc_len)
{
    if (!data || !avcc_len || len < 7) {
        return NULL;
    }
    if (data[0] == 0x17 && data[1] == 0x00) {
        if (len > 5 && data[5] == 1) {
            *avcc_len = len - 5;
            return data + 5;
        }
        if (len > 2 && data[2] == 1) {
            *avcc_len = len - 2;
            return data + 2;
        }
    }
    if ((data[0] & 0x0f) == 7 && data[1] == 0 && len > 5) {
        *avcc_len = len - 5;
        return data + 5;
    }
    if (data[0] == 1) {
        *avcc_len = len;
        return data;
    }
    return NULL;
}

static void mpeg4_avc_bind_param_sets(struct mpeg4_avc_t *avc, const uint8_t *sps, size_t sps_len,
                                      const uint8_t *pps, size_t pps_len)
{
    memset(avc, 0, sizeof(*avc));
    avc->nalu = 4; /* lengthSizeMinusOne=3，标准 4 字节 NAL 长度前缀 */
    if (sps_len >= 4) {
        avc->profile = sps[1];
        avc->compatibility = sps[2];
        avc->level = sps[3];
    }
    avc->nb_sps = 1;
    avc->sps[0].data = (uint8_t *)sps;
    avc->sps[0].bytes = (uint16_t)sps_len;
    avc->nb_pps = 1;
    avc->pps[0].data = (uint8_t *)pps;
    avc->pps[0].bytes = (uint16_t)pps_len;
}

ztk_err_t zms_rtmp_avc_seq_header(const uint8_t *sps, size_t sps_len, const uint8_t *pps,
                                  size_t pps_len, uint8_t *out, size_t cap, size_t *out_len)
{
    struct mpeg4_avc_t avc;
    int n;

    if (!sps || !pps || !out || sps_len == 0 || pps_len == 0) {
        return ZTK_ERR_INVALID;
    }
    if (cap < 6) {
        return ZTK_ERR_BUFFER_TOO_SMALL;
    }

    int hdr;

    mpeg4_avc_bind_param_sets(&avc, sps, sps_len, pps, pps_len);
    hdr = h264_write_video_hdr(out, cap, 1, 1);
    if (hdr < 0) {
        return ZTK_ERR_BUFFER_TOO_SMALL;
    }
    n = mpeg4_avc_decoder_configuration_record_save(&avc, out + hdr, cap - (size_t)hdr);
    if (n <= 0) {
        return ZTK_ERR_INVALID;
    }
    if (out_len) {
        *out_len = (size_t)hdr + (size_t)n;
    }
    return ZTK_OK;
}

static int pick_best_sps(struct mpeg4_avc_t *avc, const uint8_t **sps, size_t *sps_len)
{
    int best = -1;
    int best_area = 0;
    int i;

    for (i = 0; i < (int)avc->nb_sps; ++i) {
        zms_h264_sps_info info;
        int area;

        if (!avc->sps[i].data || avc->sps[i].bytes == 0) {
            continue;
        }
        if (zms_h264_sps_parse(avc->sps[i].data, avc->sps[i].bytes, &info)) {
            area = info.width * info.height;
            if (area > best_area) {
                best_area = area;
                best = i;
            }
        } else if (best < 0) {
            best = i;
        }
    }
    if (best < 0) {
        return 0;
    }
    *sps = avc->sps[best].data;
    *sps_len = avc->sps[best].bytes;
    return 1;
}

int zms_rtmp_avc_extract_sps_pps(const uint8_t *data, size_t len, const uint8_t **sps,
                                 size_t *sps_len, const uint8_t **pps, size_t *pps_len)
{
    struct mpeg4_avc_t avc;
    size_t avcc_len = 0;
    const uint8_t *avcc;

    if (!sps || !sps_len || !pps || !pps_len) {
        return 0;
    }
    avcc = locate_avcc(data, len, &avcc_len);
    if (!avcc || avcc_len < 7) {
        return 0;
    }
    memset(&avc, 0, sizeof(avc));
    if (mpeg4_avc_decoder_configuration_record_load(avcc, avcc_len, &avc) <= 0) {
        return 0;
    }
    if (!pick_best_sps(&avc, sps, sps_len) || avc.nb_pps == 0 || !avc.pps[0].data ||
        avc.pps[0].bytes == 0) {
        return 0;
    }
    *pps = avc.pps[0].data;
    *pps_len = avc.pps[0].bytes;
    return 1;
}

int zms_rtmp_avc_extradata(const uint8_t *data, size_t len, const uint8_t **avcc, size_t *avcc_len)
{
    if (!avcc || !avcc_len) {
        return 0;
    }
    *avcc = locate_avcc(data, len, avcc_len);
    return *avcc && *avcc_len > 0;
}

int zms_rtmp_avc_profile_level_id(const uint8_t *data, size_t len, char profile[16])
{
    struct mpeg4_avc_t avc;
    size_t avcc_len = 0;
    const uint8_t *avcc;

    if (!profile) {
        return 0;
    }
    avcc = locate_avcc(data, len, &avcc_len);
    if (!avcc || avcc_len < 4) {
        return 0;
    }
    memset(&avc, 0, sizeof(avc));
    if (mpeg4_avc_decoder_configuration_record_load(avcc, avcc_len, &avc) <= 0) {
        return 0;
    }
    snprintf(profile, 16, "%02x%02x%02x", avc.profile, avc.compatibility, avc.level);
    return 1;
}

ztk_err_t zms_h264_over_rtmp_pack_es(const uint8_t *video_cfg, size_t cfg_len,
                                     const uint8_t *annexb, size_t len, int key, uint8_t *out,
                                     size_t cap, size_t *out_len)
{
    struct mpeg4_avc_t avc;
    const uint8_t *avcc = NULL;
    size_t avcc_len = 0;
    int vcl = 0;
    int update = 0;
    int hdr;
    int n;

    if (!annexb || !out || len == 0) {
        return ZTK_ERR_INVALID;
    }
    if (cap < 6) {
        return ZTK_ERR_BUFFER_TOO_SMALL;
    }

    memset(&avc, 0, sizeof(avc));
    if (video_cfg && zms_rtmp_avc_extradata(video_cfg, cfg_len, &avcc, &avcc_len) && avcc &&
        avcc_len >= 7) {
        (void)mpeg4_avc_decoder_configuration_record_load(avcc, avcc_len, &avc);
    }

    hdr = h264_write_video_hdr(out, cap, key, 0);
    if (hdr < 0) {
        return ZTK_ERR_BUFFER_TOO_SMALL;
    }
    n = h264_annexbtomp4(&avc, annexb, len, out + hdr, cap - (size_t)hdr, &vcl, &update);
    if (n <= 0) {
        return ZTK_ERR_INVALID;
    }
    if (out_len) {
        *out_len = (size_t)hdr + (size_t)n;
    }
    return ZTK_OK;
}

ztk_err_t zms_rtmp_h264_annexb(const uint8_t *annexb, size_t len, uint32_t tag_dts_ms, int key,
                               uint8_t *out, size_t cap, size_t *out_len)
{
    (void)tag_dts_ms;
    return zms_h264_over_rtmp_pack_es(NULL, 0, annexb, len, key, out, cap, out_len);
}

ztk_err_t zms_rtmp_video_tag_to_annexb(const uint8_t *body, size_t len, uint8_t *out, size_t cap,
                                       size_t *out_len, int *keyframe)
{
    struct mpeg4_avc_t avc;
    struct flv_video_tag_header_t v;
    int hdr;
    int n;

    if (!body || len < 5 || !out || !out_len) {
        return ZTK_ERR_INVALID;
    }
    hdr = flv_video_tag_header_read(&v, body, len);
    if (hdr < 0 || v.avpacket != FLV_AVPACKET) {
        return ZTK_ERR_INVALID;
    }
    if (keyframe) {
        *keyframe = (v.keyframe == FLV_VIDEO_KEY_FRAME) ? 1 : 0;
    }

    memset(&avc, 0, sizeof(avc));
    n = h264_mp4toannexb(&avc, body + hdr, len - (size_t)hdr, out, cap);
    if (n <= 0) {
        return ZTK_ERR_INVALID;
    }
    *out_len = (size_t)n;
    return ZTK_OK;
}
