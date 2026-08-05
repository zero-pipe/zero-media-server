#include "zms/vod/vod_flv_metadata.h"
#include "zms/vod/vod_flv_index.h"
#include "zms/vod/io/vod_source.h"
#include "zms/engine/stream/stream_hub.h"
#include "zms/media/container/flv/flv_tag_probe.h"
#include "zms/media/codec/codec_id.h"
#include "zms/session/rtmp/rtmp_amf.h"
#include "zms/session/rtmp/rtmp.h"
#include <stdlib.h>
#include <string.h>

static size_t amf_encode_key(uint8_t *out, size_t cap, const char *key)
{
    size_t klen = key ? strlen(key) : 0;

    if (cap < 2 + klen) {
        return 0;
    }
    out[0] = (uint8_t)((klen >> 8) & 0xff);
    out[1] = (uint8_t)(klen & 0xff);
    if (klen) {
        memcpy(out + 2, key, klen);
    }
    return 2 + klen;
}

static size_t amf_encode_bool(uint8_t *out, size_t cap, int v)
{
    if (cap < 2) {
        return 0;
    }
    out[0] = ZMS_AMF_BOOLEAN;
    out[1] = v ? 1 : 0;
    return 2;
}

size_t zms_vod_flv_metadata_encode(const zms_media_source *src, uint8_t *amf, size_t cap,
                                   double duration_override_sec, const zms_vod_flv_index *idx,
                                   const zms_vod_flv_index_view *view)
{
    double width = 0.0, height = 0.0, fps = 0.0;
    double video_codec = 7.0, audio_codec = 10.0;
    double audio_rate = 44100.0;
    double stereo = 1.0;
    double duration_sec = duration_override_sec;
    zms_codec_id vc = ZMS_CODEC_INVALID;
    const double *kf_times = NULL;
    const double *kf_pos = NULL;
    size_t kf_count = 0;
    double kf_filesize = 0.0;
    size_t pos = 0;

    if (!amf || cap == 0) {
        return 0;
    }

    if (view && view->count > 0) {
        kf_times = view->times;
        kf_pos = view->filepositions;
        kf_count = view->count;
        kf_filesize = view->filesize;
    } else if (idx && idx->count > 0) {
        kf_times = idx->times;
        kf_pos = idx->filepositions;
        kf_count = idx->count;
        kf_filesize = idx->filesize;
    }

    if (src) {
        if (duration_sec <= 0.0 && zms_media_source_is_vod(src)) {
            duration_sec = zms_vod_source_duration_ms(src) / 1000.0;
            if (duration_sec <= 0.0) {
                duration_sec = zms_vod_probe_duration_ms(src->app, src->stream) / 1000.0;
            }
        }
        if (src->video.ready) {
            width = (double)src->video.width;
            height = (double)src->video.height;
            fps = (double)src->video.fps;
            vc = src->video.codec;
        }
        if (vc == ZMS_CODEC_INVALID && src->gop_queue) {
            size_t vlen = 0;
            const uint8_t *vcfg = zms_media_source_video_config(src, &vlen);
            if (vcfg && vlen) {
                vc = zms_flv_tag_video_codec(vcfg, vlen);
            }
        }
        video_codec = zms_flv_metadata_videocodecid(vc != ZMS_CODEC_INVALID ? vc : ZMS_CODEC_H264);
        if (src->audio.ready) {
            audio_rate = (double)src->audio.sample_rate;
            stereo = src->audio.channels > 1 ? 1.0 : 0.0;
        }
    }

    pos += zms_amf_encode_string(amf + pos, cap - pos, "onMetaData");
    amf[pos++] = ZMS_AMF_OBJECT;
    pos += amf_encode_key(amf + pos, cap - pos, "duration");
    pos += zms_amf_encode_number(amf + pos, cap - pos, duration_sec);
    if (duration_sec > 0.0) {
        pos += amf_encode_key(amf + pos, cap - pos, "canSeekToEnd");
        pos += amf_encode_bool(amf + pos, cap - pos, 1);
    }
    if (kf_filesize > 0.0) {
        pos += amf_encode_key(amf + pos, cap - pos, "filesize");
        pos += zms_amf_encode_number(amf + pos, cap - pos, kf_filesize);
    }
    pos += amf_encode_key(amf + pos, cap - pos, "width");
    pos += zms_amf_encode_number(amf + pos, cap - pos, width);
    pos += amf_encode_key(amf + pos, cap - pos, "height");
    pos += zms_amf_encode_number(amf + pos, cap - pos, height);
    if (fps > 0.0) {
        pos += amf_encode_key(amf + pos, cap - pos, "framerate");
        pos += zms_amf_encode_number(amf + pos, cap - pos, fps);
    }
    pos += amf_encode_key(amf + pos, cap - pos, "videocodecid");
    pos += zms_amf_encode_number(amf + pos, cap - pos, video_codec);
    pos += amf_encode_key(amf + pos, cap - pos, "audiocodecid");
    pos += zms_amf_encode_number(amf + pos, cap - pos, audio_codec);
    pos += amf_encode_key(amf + pos, cap - pos, "audiosamplerate");
    pos += zms_amf_encode_number(amf + pos, cap - pos, audio_rate);
    pos += amf_encode_key(amf + pos, cap - pos, "stereo");
    pos += zms_amf_encode_number(amf + pos, cap - pos, stereo);
    if (kf_count > 0) {
        pos += amf_encode_key(amf + pos, cap - pos, "keyframes");
        if (pos >= cap) {
            return pos;
        }
        amf[pos++] = ZMS_AMF_OBJECT;
        pos += amf_encode_key(amf + pos, cap - pos, "filepositions");
        pos += zms_amf_encode_strict_array_numbers(amf + pos, cap - pos, kf_pos, kf_count);
        pos += amf_encode_key(amf + pos, cap - pos, "times");
        pos += zms_amf_encode_strict_array_numbers(amf + pos, cap - pos, kf_times, kf_count);
        pos += zms_amf_encode_object_end(amf + pos, cap - pos);
    }
    pos += zms_amf_encode_object_end(amf + pos, cap - pos);
    return pos;
}

size_t zms_vod_flv_metadata_body_size(const zms_media_source *src, double duration_sec,
                                      const zms_vod_flv_index *idx)
{
    uint8_t stack[16384];
    uint8_t *heap = NULL;
    size_t cap = sizeof(stack);
    size_t need;

    if (idx && idx->count > 400) {
        cap = 8192 + idx->count * 24;
    }
    need = zms_vod_flv_metadata_encode(src, stack, cap, duration_sec, idx, NULL);
    if (need > 0 && need <= cap) {
        return need;
    }
    cap = (need > cap ? need : cap) + 256;
    heap = (uint8_t *)malloc(cap);
    if (!heap) {
        return 0;
    }
    need = zms_vod_flv_metadata_encode(src, heap, cap, duration_sec, idx, NULL);
    free(heap);
    return need;
}
