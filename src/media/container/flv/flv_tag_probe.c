#include "zms/media/container/flv/flv_tag_probe.h"
#include "flv-header.h"
#include "flv-proto.h"
#include "mpeg4-avc.h"
#include "mpeg4-hevc.h"
#include <string.h>

zms_codec_id zms_flv_tag_video_codec(const uint8_t *tag, size_t len)
{
    struct flv_video_tag_header_t v;

    if (!tag || len < 1) {
        return ZMS_CODEC_INVALID;
    }
    if (tag[0] & 0x80) {
        if (len >= 5) {
            if (memcmp(tag + 1, "avc1", 4) == 0) {
                return ZMS_CODEC_H264;
            }
            if (memcmp(tag + 1, "hvc1", 4) == 0 || memcmp(tag + 1, "hev1", 4) == 0) {
                return ZMS_CODEC_H265;
            }
            uint32_t fourcc = FLV_VIDEO_FOURCC(tag[1], tag[2], tag[3], tag[4]);
            if (fourcc == FLV_VIDEO_FOURCC_HEVC) {
                return ZMS_CODEC_H265;
            }
            if (fourcc == FLV_VIDEO_FOURCC_AVC) {
                return ZMS_CODEC_H264;
            }
            if (fourcc == FLV_VIDEO_FOURCC_VP8) {
                return ZMS_CODEC_VP8;
            }
            if (fourcc == FLV_VIDEO_FOURCC_VP9) {
                return ZMS_CODEC_VP9;
            }
            if (fourcc == FLV_VIDEO_FOURCC_AV1) {
                return ZMS_CODEC_AV1;
            }
            if (fourcc == FLV_VIDEO_FOURCC_VVC) {
                return ZMS_CODEC_H266;
            }
        }
        return ZMS_CODEC_INVALID;
    }
    if (flv_video_tag_header_read(&v, tag, len) < 0) {
        return ZMS_CODEC_INVALID;
    }
    if (v.codecid == FLV_VIDEO_H264) {
        return ZMS_CODEC_H264;
    }
    if (v.codecid == FLV_VIDEO_H265) {
        return ZMS_CODEC_H265;
    }
    if (v.codecid == FLV_VIDEO_AV1) {
        return ZMS_CODEC_AV1;
    }
    if (v.codecid == FLV_VIDEO_H266) {
        return ZMS_CODEC_H266;
    }
    return ZMS_CODEC_INVALID;
}

zms_codec_id zms_flv_video_config_codec(const uint8_t *cfg, size_t len)
{
    struct mpeg4_hevc_t hevc;
    struct mpeg4_avc_t avc;
    zms_codec_id id;

    id = zms_flv_tag_video_codec(cfg, len);
    if (id != ZMS_CODEC_INVALID) {
        return id;
    }
    if (!cfg || len < 7 || cfg[0] != 1) {
        return ZMS_CODEC_INVALID;
    }
    memset(&hevc, 0, sizeof(hevc));
    if (mpeg4_hevc_decoder_configuration_record_load(cfg, len, &hevc) > 0) {
        return ZMS_CODEC_H265;
    }
    memset(&avc, 0, sizeof(avc));
    if (mpeg4_avc_decoder_configuration_record_load(cfg, len, &avc) > 0) {
        return ZMS_CODEC_H264;
    }
    return ZMS_CODEC_INVALID;
}

double zms_flv_metadata_videocodecid(zms_codec_id id)
{
    if (id == ZMS_CODEC_H265) {
        return (double)FLV_VIDEO_FOURCC_HEVC;
    }
    if (id == ZMS_CODEC_H266) {
        return (double)FLV_VIDEO_FOURCC_VVC;
    }
    if (id == ZMS_CODEC_AV1) {
        return (double)FLV_VIDEO_FOURCC_AV1;
    }
    if (id == ZMS_CODEC_VP8) {
        return (double)FLV_VIDEO_FOURCC_VP8;
    }
    if (id == ZMS_CODEC_VP9) {
        return (double)FLV_VIDEO_FOURCC_VP9;
    }
    if (id == ZMS_CODEC_H264) {
        return 7.0;
    }
    return 7.0;
}

zms_codec_id zms_flv_tag_audio_codec(const uint8_t *tag, size_t len)
{
    struct flv_audio_tag_header_t a;

    if (!tag || len == 0) {
        return ZMS_CODEC_INVALID;
    }
    if (flv_audio_tag_header_read(&a, tag, len) < 0) {
        return ZMS_CODEC_INVALID;
    }
    if (a.codecid == FLV_AUDIO_AAC) {
        return ZMS_CODEC_AAC;
    }
    if (a.codecid == FLV_AUDIO_OPUS) {
        return ZMS_CODEC_OPUS;
    }
    if (a.codecid == FLV_AUDIO_G711A) {
        return ZMS_CODEC_G711A;
    }
    if (a.codecid == FLV_AUDIO_G711U) {
        return ZMS_CODEC_G711U;
    }
    if (a.codecid == FLV_AUDIO_FOURCC && len >= 5 &&
        FLV_VIDEO_FOURCC(tag[1], tag[2], tag[3], tag[4]) == FLV_AUDIO_FOURCC_OPUS) {
        return ZMS_CODEC_OPUS;
    }
    return ZMS_CODEC_INVALID;
}

zms_flv_tag_packet_kind zms_flv_tag_video_packet_kind(const uint8_t *tag, size_t len)
{
    struct flv_video_tag_header_t v;

    if (!tag || len < 1) {
        return ZMS_FLV_TAG_PKT_INVALID;
    }
    if (flv_video_tag_header_read(&v, tag, len) < 0) {
        return ZMS_FLV_TAG_PKT_INVALID;
    }
    switch (v.avpacket) {
    case FLV_SEQUENCE_HEADER:
        return ZMS_FLV_TAG_PKT_SEQ_HEADER;
    case FLV_AVPACKET:
        return ZMS_FLV_TAG_PKT_RAW;
    case FLV_END_OF_SEQUENCE:
        return ZMS_FLV_TAG_PKT_END_OF_SEQ;
    default:
        return ZMS_FLV_TAG_PKT_OTHER;
    }
}

zms_flv_tag_packet_kind zms_flv_tag_audio_packet_kind(const uint8_t *tag, size_t len)
{
    struct flv_audio_tag_header_t a;
    zms_codec_id ac;

    if (!tag || len < 1) {
        return ZMS_FLV_TAG_PKT_INVALID;
    }
    if (flv_audio_tag_header_read(&a, tag, len) < 0) {
        return ZMS_FLV_TAG_PKT_INVALID;
    }
    ac = zms_flv_tag_audio_codec(tag, len);
    if (ac != ZMS_CODEC_AAC && ac != ZMS_CODEC_OPUS) {
        return ZMS_FLV_TAG_PKT_RAW;
    }
    return a.avpacket == FLV_SEQUENCE_HEADER ? ZMS_FLV_TAG_PKT_SEQ_HEADER : ZMS_FLV_TAG_PKT_RAW;
}

ztk_err_t zms_flv_tag_aac_seq_header_asc(const uint8_t *tag, size_t len, const uint8_t **asc,
                                         size_t *asc_len)
{
    struct flv_audio_tag_header_t a;
    int n;

    if (!tag || len < 2 || !asc || !asc_len) {
        return ZTK_ERR_INVALID;
    }
    n = flv_audio_tag_header_read(&a, tag, len);
    if (n < 0 || a.codecid != FLV_AUDIO_AAC || a.avpacket != FLV_SEQUENCE_HEADER) {
        return ZTK_ERR_INVALID;
    }
    if (len < (size_t)n + 1) {
        return ZTK_ERR_INVALID;
    }
    *asc = tag + n;
    *asc_len = len - (size_t)n;
    return ZTK_OK;
}

ztk_err_t zms_flv_tag_audio_to_es(const uint8_t *body, size_t len, const uint8_t **es,
                                  size_t *es_len, zms_codec_id *codec)
{
    if (!body || len < 2 || !es || !es_len) {
        return ZTK_ERR_INVALID;
    }

    zms_codec_id ac = zms_flv_tag_audio_codec(body, len);
    if (ac == ZMS_CODEC_G711A || ac == ZMS_CODEC_G711U) {
        if (len < 2) {
            return ZTK_ERR_INVALID;
        }
        if (codec) {
            *codec = ac;
        }
        *es = body + 1;
        *es_len = len - 1;
        return ZTK_OK;
    }
    if (ac != ZMS_CODEC_AAC) {
        return ZTK_ERR_INVALID;
    }
    if (codec) {
        *codec = ZMS_CODEC_AAC;
    }
    if (body[1] != 1) {
        return ZTK_ERR_INVALID;
    }
    *es = body + 2;
    *es_len = len - 2;
    return ZTK_OK;
}
