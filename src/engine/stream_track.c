#include "zms/engine/stream_track.h"
#include "zms/media/codec/h264/h264_sps.h"
#include "zms/media/codec/h264/h264_over_rtmp.h"
#include "zms/media/codec/aac/aac_over_rtmp.h"
#include "zms/media/container/flv/flv_tag_probe.h"
#include "zms/live/ingest/common/stream_ingest.h"
#include "ztk/util/log.h"
#include <stdio.h>
#include <string.h>

void zms_video_track_clear(zms_video_track *t)
{
    if (!t) {
        return;
    }
    memset(t, 0, sizeof(*t));
}

void zms_audio_track_clear(zms_audio_track *t)
{
    if (!t) {
        return;
    }
    memset(t, 0, sizeof(*t));
}

int zms_video_size_is_suspicious(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0) {
        return 1;
    }
    /* 仅识别常见占位量级，不作为业务分辨率下限 */
    return width < 320u || height < 240u;
}

int zms_video_size_should_replace(uint32_t cur_w, uint32_t cur_h, uint32_t new_w, uint32_t new_h)
{
    if (new_w == 0 || new_h == 0) {
        return 0;
    }
    if (cur_w == new_w && cur_h == new_h) {
        return 0;
    }
    if (cur_w == 0 || cur_h == 0) {
        return 1;
    }
    /* 已有可信尺寸时，拒绝被占位 SPS 回退；其余以新 SPS 为准 */
    if (!zms_video_size_is_suspicious(cur_w, cur_h) &&
        zms_video_size_is_suspicious(new_w, new_h)) {
        return 0;
    }
    return 1;
}

ztk_err_t zms_video_track_from_avc(zms_video_track *t, const uint8_t *data, size_t len)
{
    if (!t || !data || len < 5) {
        return ZTK_ERR_INVALID;
    }

    const uint8_t *sps = NULL, *pps = NULL;
    size_t sps_len = 0, pps_len = 0;
    if (!zms_rtmp_avc_extract_sps_pps(data, len, &sps, &sps_len, &pps, &pps_len)) {
        return ZTK_ERR_INVALID;
    }

    zms_video_track_clear(t);
    t->codec = ZMS_CODEC_H264;
    if (!zms_rtmp_avc_profile_level_id(data, len, t->profile_level_id)) {
        snprintf(t->profile_level_id, sizeof(t->profile_level_id), "42001f");
    }

    zms_h264_sps_info sps_info;
    if (zms_h264_sps_parse(sps, sps_len, &sps_info)) {
        t->width = sps_info.width;
        t->height = sps_info.height;
        t->fps = sps_info.fps;
    }
    t->ready = 1;
    if (t->width >= ZMS_VIDEO_WIDTH_MIN_VALID && t->height > 0) {
        ztk_info("track video: %dx%d fps=%.2f profile=%s", t->width, t->height, (double)t->fps,
                 t->profile_level_id);
    }
    return ZTK_OK;
}

ztk_err_t zms_audio_track_from_asc(zms_audio_track *t, const uint8_t *asc, size_t len)
{
    if (!t || !asc || len == 0) {
        return ZTK_ERR_INVALID;
    }

    zms_audio_track_clear(t);
    t->codec = ZMS_CODEC_AAC;
    if (!zms_aac_parse_asc(asc, len, &t->sample_rate, &t->channels)) {
        t->sample_rate = 44100;
        t->channels = 2;
    }

    char *p = t->asc_hex;
    for (size_t i = 0; i < len && (size_t)(p - t->asc_hex) < sizeof(t->asc_hex) - 3; ++i) {
        p += snprintf(p, (size_t)(t->asc_hex + sizeof(t->asc_hex) - p), "%02x", asc[i]);
    }
    t->ready = 1;
    ztk_info("track audio: codec=%d rate=%d ch=%d asc=%s", (int)t->codec, t->sample_rate,
             t->channels, t->asc_hex);
    return ZTK_OK;
}

ztk_err_t zms_audio_track_from_g711(zms_audio_track *t, zms_codec_id codec, int sample_rate,
                                    int channels)
{
    if (!t || (codec != ZMS_CODEC_G711A && codec != ZMS_CODEC_G711U)) {
        return ZTK_ERR_INVALID;
    }
    zms_audio_track_clear(t);
    t->codec = codec;
    t->sample_rate = sample_rate > 0 ? sample_rate : 8000;
    t->channels = channels > 0 ? channels : 1;
    t->ready = 1;
    ztk_info("track audio: %s rate=%d ch=%d", zms_codec_name(codec), t->sample_rate, t->channels);
    return ZTK_OK;
}

ztk_err_t zms_audio_track_from_rtmp(zms_audio_track *t, const uint8_t *data, size_t len)
{
    if (!t || !data || len < 2) {
        return ZTK_ERR_INVALID;
    }

    zms_codec_id codec = zms_flv_tag_audio_codec(data, len);
    if (codec == ZMS_CODEC_G711A || codec == ZMS_CODEC_G711U) {
        return zms_audio_track_from_g711(t, codec, 8000, 1);
    }
    if (codec != ZMS_CODEC_AAC || len < 4) {
        return ZTK_ERR_INVALID;
    }
    return zms_audio_track_from_asc(t, data + 2, len - 2);
}
