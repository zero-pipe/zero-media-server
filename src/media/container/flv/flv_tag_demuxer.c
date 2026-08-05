#include "zms/media/container/flv/flv_tag_demuxer.h"
#include "flv-demuxer.h"
#include "flv-proto.h"
#include <stdlib.h>
#include <string.h>

struct zms_flv_tag_demuxer {
    zms_flv_tag_demuxer_opts opts;
    flv_demuxer_t *flv;
    zms_frame frame;
};

static int on_flv_es(void *param, int codec, const void *data, size_t bytes, uint32_t pts,
                     uint32_t dts, int flags)
{
    zms_flv_tag_demuxer *d = (zms_flv_tag_demuxer *)param;
    if (!d || !d->opts.on_frame || !data || bytes == 0) {
        return 0;
    }

    if (codec == FLV_VIDEO_H264 || codec == FLV_VIDEO_H265 || codec == FLV_VIDEO_H266 ||
        codec == FLV_VIDEO_AV1 || codec == FLV_VIDEO_VP8 || codec == FLV_VIDEO_VP9) {
        if (codec == FLV_VIDEO_H265) {
            d->frame.codec = ZMS_CODEC_H265;
        } else if (codec == FLV_VIDEO_H266) {
            d->frame.codec = ZMS_CODEC_H266;
        } else if (codec == FLV_VIDEO_AV1) {
            d->frame.codec = ZMS_CODEC_AV1;
        } else if (codec == FLV_VIDEO_VP8) {
            d->frame.codec = ZMS_CODEC_VP8;
        } else if (codec == FLV_VIDEO_VP9) {
            d->frame.codec = ZMS_CODEC_VP9;
        } else {
            d->frame.codec = ZMS_CODEC_H264;
        }
        d->frame.track = ZMS_TRACK_VIDEO;
        d->frame.keyframe = flags ? 1 : 0;
    } else if (codec == FLV_AUDIO_AAC || codec == FLV_AUDIO_OPUS) {
        d->frame.codec = (codec == FLV_AUDIO_OPUS) ? ZMS_CODEC_OPUS : ZMS_CODEC_AAC;
        d->frame.track = ZMS_TRACK_AUDIO;
        d->frame.keyframe = 0;
    } else if (codec == FLV_AUDIO_G711A || codec == FLV_AUDIO_G711U) {
        d->frame.codec = (codec == FLV_AUDIO_G711A) ? ZMS_CODEC_G711A : ZMS_CODEC_G711U;
        d->frame.track = ZMS_TRACK_AUDIO;
        d->frame.keyframe = 0;
    } else if (codec == FLV_VIDEO_AVCC || codec == FLV_VIDEO_HVCC || codec == FLV_VIDEO_VVCC ||
               codec == FLV_VIDEO_AV1C || codec == FLV_AUDIO_ASC) {
        return 0;
    } else {
        return 0;
    }

    d->frame.pts_ms = pts;
    d->frame.dts_ms = dts;
    d->frame.data = (uint8_t *)data;
    d->frame.size = bytes;
    d->frame.owned = 0;
    d->opts.on_frame(&d->frame, d->opts.user);
    return 0;
}

zms_flv_tag_demuxer *zms_flv_tag_demuxer_create(const zms_flv_tag_demuxer_opts *opts)
{
    if (!opts || !opts->on_frame) {
        return NULL;
    }
    zms_flv_tag_demuxer *d = (zms_flv_tag_demuxer *)calloc(1, sizeof(*d));
    if (!d) {
        return NULL;
    }
    d->opts = *opts;
    d->flv = flv_demuxer_create(on_flv_es, d);
    if (!d->flv) {
        free(d);
        return NULL;
    }
    zms_frame_init(&d->frame);
    return d;
}

void zms_flv_tag_demuxer_destroy(zms_flv_tag_demuxer *d)
{
    if (!d) {
        return;
    }
    if (d->flv) {
        flv_demuxer_destroy(d->flv);
    }
    zms_frame_clear(&d->frame);
    free(d);
}

ztk_err_t zms_flv_tag_demuxer_input(zms_flv_tag_demuxer *d, uint8_t type_id, const void *data,
                                    size_t len, uint32_t tag_dts_ms)
{
    if (!d || !d->flv || !data || len == 0) {
        return ZTK_ERR_INVALID;
    }
    int type = FLV_TYPE_AUDIO;
    if (type_id == 9) {
        type = FLV_TYPE_VIDEO;
    } else if (type_id == 18) {
        type = FLV_TYPE_SCRIPT;
    }
    if (flv_demuxer_input(d->flv, type, data, len, tag_dts_ms) != 0) {
        return ZTK_ERR_INVALID;
    }
    return ZTK_OK;
}
