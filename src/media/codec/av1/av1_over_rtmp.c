#include "zms/media/codec/av1/av1_over_rtmp.h"
#include "zms/media/container/flv/flv_tag_probe.h"
#include "aom-av1.h"
#include "flv-header.h"
#include "flv-proto.h"
#include <string.h>

static int av1_write_video_hdr(uint8_t *out, size_t cap, int key, int seq_hdr)
{
    struct flv_video_tag_header_t video;
    int n;

    memset(&video, 0, sizeof(video));
    video.codecid = FLV_VIDEO_AV1;
    video.enhanced_rtmp = 1;
    video.keyframe = key ? FLV_VIDEO_KEY_FRAME : FLV_VIDEO_INTER_FRAME;
    video.avpacket = seq_hdr ? FLV_SEQUENCE_HEADER : FLV_PACKET_TYPE_CODED_FRAMES_X;
    n = flv_video_tag_header_write(&video, out, cap);
    return n;
}

static const uint8_t *locate_av1c(const uint8_t *data, size_t len, size_t *av1c_len)
{
    struct flv_video_tag_header_t vh;
    int hdr;

    if (!data || !av1c_len || len < 2) {
        return NULL;
    }
    if (zms_flv_tag_video_codec(data, len) == ZMS_CODEC_AV1) {
        hdr = flv_video_tag_header_read(&vh, data, len);
        if (hdr > 0 && vh.avpacket == FLV_SEQUENCE_HEADER && (size_t)hdr < len) {
            *av1c_len = len - (size_t)hdr;
            return data + hdr;
        }
    }
    *av1c_len = len;
    return data;
}

int zms_av1_over_rtmp_config_extradata(const uint8_t *data, size_t len, const uint8_t **av1c,
                                       size_t *av1c_len)
{
    if (!data || len < 2 || !av1c || !av1c_len) {
        return 0;
    }
    *av1c = locate_av1c(data, len, av1c_len);
    return *av1c && *av1c_len > 0;
}

ztk_err_t zms_av1_flv_sequence_header(const uint8_t *cfg, size_t cfg_len, uint8_t *out, size_t cap,
                                      size_t *out_len)
{
    struct aom_av1_t av1;
    const uint8_t *av1c = NULL;
    size_t av1c_len = 0;
    int hdr;
    int n;

    if (!cfg || cfg_len < 2 || !out || !out_len) {
        return ZTK_ERR_INVALID;
    }

    if (zms_flv_tag_video_codec(cfg, cfg_len) == ZMS_CODEC_AV1) {
        struct flv_video_tag_header_t vh;
        int rh = flv_video_tag_header_read(&vh, cfg, cfg_len);
        if (rh > 0 && vh.avpacket == FLV_SEQUENCE_HEADER && cfg_len <= cap) {
            memcpy(out, cfg, cfg_len);
            *out_len = cfg_len;
            return ZTK_OK;
        }
    }

    if (!zms_av1_over_rtmp_config_extradata(cfg, cfg_len, &av1c, &av1c_len) || !av1c ||
        av1c_len < 4) {
        return ZTK_ERR_INVALID;
    }

    memset(&av1, 0, sizeof(av1));
    if (aom_av1_codec_configuration_record_load(av1c, av1c_len, &av1) <= 0) {
        return ZTK_ERR_INVALID;
    }

    hdr = av1_write_video_hdr(out, cap, 1, 1);
    if (hdr < 0) {
        return ZTK_ERR_BUFFER_TOO_SMALL;
    }
    n = aom_av1_codec_configuration_record_save(&av1, out + hdr, cap - (size_t)hdr);
    if (n <= 0) {
        return ZTK_ERR_INVALID;
    }
    *out_len = (size_t)hdr + (size_t)n;
    return ZTK_OK;
}

ztk_err_t zms_av1_over_rtmp_pack_es(const uint8_t *video_cfg, size_t cfg_len, const uint8_t *obu,
                                    size_t len, int key, uint8_t *out, size_t cap, size_t *out_len)
{
    int hdr;

    (void)video_cfg;
    (void)cfg_len;
    if (!obu || !out || len == 0) {
        return ZTK_ERR_INVALID;
    }
    if (cap < len + 8) {
        return ZTK_ERR_BUFFER_TOO_SMALL;
    }

    hdr = av1_write_video_hdr(out, cap, key, 0);
    if (hdr < 0) {
        return ZTK_ERR_BUFFER_TOO_SMALL;
    }
    memcpy(out + hdr, obu, len);
    if (out_len) {
        *out_len = (size_t)hdr + len;
    }
    return ZTK_OK;
}

int zms_av1_extradata_from_obu(const uint8_t *obu, size_t len, uint8_t *out, size_t cap, int *width,
                               int *height)
{
    struct aom_av1_t av1;
    int n;

    if (!obu || len < 2 || !out || cap < 4) {
        return -1;
    }
    memset(&av1, 0, sizeof(av1));
    if (aom_av1_codec_configuration_record_init(&av1, obu, len) != 0) {
        return -1;
    }
    n = aom_av1_codec_configuration_record_save(&av1, out, cap);
    if (n <= 0) {
        return -1;
    }
    if (width) {
        *width = av1.width > 0 ? (int)av1.width : 640;
    }
    if (height) {
        *height = av1.height > 0 ? (int)av1.height : 480;
    }
    return n;
}

typedef struct {
    int found;
} av1_seq_hdr_ctx;

static int av1_seq_hdr_handler(void *param, const uint8_t *obu, size_t bytes)
{
    av1_seq_hdr_ctx *ctx = (av1_seq_hdr_ctx *)param;
    uint8_t type;

    (void)bytes;
    if (!ctx || !obu) {
        return 0;
    }
    type = (obu[0] >> 3) & 0x0F;
    if (type == 1) {
        ctx->found = 1;
    }
    return 0;
}

int zms_av1_obu_has_sequence_header(const uint8_t *obu, size_t len)
{
    av1_seq_hdr_ctx ctx;

    if (!obu || len < 2) {
        return 0;
    }
    memset(&ctx, 0, sizeof(ctx));
    if (aom_av1_obu_split(obu, len, av1_seq_hdr_handler, &ctx) != 0) {
        return 0;
    }
    return ctx.found;
}

int zms_av1_obu_is_sync_key(const uint8_t *obu, size_t len)
{
    struct aom_av1_t av1;

    if (!obu || len < 2) {
        return 0;
    }
    if (!zms_av1_obu_has_sequence_header(obu, len)) {
        return 0;
    }
    memset(&av1, 0, sizeof(av1));
    return aom_av1_codec_configuration_record_init(&av1, obu, len) == 0 && av1.width > 0 &&
           av1.height > 0;
}
