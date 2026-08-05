#include "zms/media/codec/frame_policy/frame_codec_policy.h"
#include "zms/media/codec/bitstream/annexb.h"
#include "zms/media/codec/h264/h264_es.h"
#include "zms/media/codec/h265/h265_es.h"
#include "zms/media/codec/av1/av1_over_rtmp.h"
#include <stddef.h>

#define ZMS_FRAME_CODEC_SLOT_MAX 32

static const zms_frame_codec_ops *g_es[ZMS_FRAME_CODEC_SLOT_MAX];

static int default_is_config(const zms_frame *f)
{
    return f && f->config_frame;
}

static int default_is_key(const zms_frame *f)
{
    return f && f->keyframe;
}

static int default_ring_storable(const zms_frame *f, int cache_started, int has_video)
{
    if (!f) {
        return 0;
    }
    if (f->track == ZMS_TRACK_AUDIO) {
        return has_video ? cache_started : 1;
    }
    if (f->drop_able) {
        return cache_started;
    }
    if (f->keyframe || f->config_frame) {
        return 1;
    }
    return cache_started;
}

static int default_ring_new_gop(const zms_frame *f, int video_key_pos, int has_video)
{
    (void)video_key_pos;
    if (!f) {
        return 0;
    }
    if (f->track == ZMS_TRACK_AUDIO) {
        return has_video ? 0 : 1;
    }
    if (f->config_frame) {
        return 0;
    }
    return f->keyframe;
}

static const zms_frame_codec_ops k_default_es = {ZMS_CODEC_INVALID,     "default",
                                                 default_is_config,     default_is_key,
                                                 default_ring_storable, default_ring_new_gop};

static int annexb_nal_type(const uint8_t *data, size_t len)
{
    size_t nlen = 0;
    const uint8_t *nal = zms_annexb_find_nal(data, data + len, &nlen);
    if (!nal || nlen == 0) {
        return -1;
    }
    return nal[0] & 0x1f;
}

static int h264_is_config(const zms_frame *f)
{
    int t;
    if (!f) {
        return 0;
    }
    if (f->config_frame) {
        return 1;
    }
    if (f->codec != ZMS_CODEC_H264 || !f->data || f->size < 4) {
        return 0;
    }
    t = annexb_nal_type(f->data, f->size);
    return t == 7 || t == 8;
}

static int h264_is_key(const zms_frame *f)
{
    if (!f) {
        return 0;
    }
    if (f->keyframe) {
        return 1;
    }
    if (f->codec == ZMS_CODEC_H264 && f->data && f->size >= 4) {
        return zms_h264_annexb_is_sync_key(f->data, f->size);
    }
    return 0;
}

static int h265_nal_type_hevc(const uint8_t *annexb, size_t len)
{
    size_t nlen = 0;
    const uint8_t *nal;

    if (!annexb || len < 5) {
        return -1;
    }
    nal = zms_annexb_find_nal(annexb, annexb + len, &nlen);
    if (!nal || nlen == 0) {
        return -1;
    }
    return (nal[0] >> 1) & 0x3f;
}

static int h265_is_config(const zms_frame *f)
{
    int t;

    if (!f) {
        return 0;
    }
    if (f->config_frame) {
        return 1;
    }
    if (f->codec != ZMS_CODEC_H265 || !f->data || f->size < 5) {
        return 0;
    }
    t = h265_nal_type_hevc(f->data, f->size);
    return t == 32 || t == 33 || t == 34;
}

static int h265_is_key(const zms_frame *f)
{
    if (!f || f->codec != ZMS_CODEC_H265 || !f->data || f->size == 0) {
        return 0;
    }
    /* 接受 IDR 与 CRA（type 16-21）：libx265/FFmpeg 常发 CRA_NUT */
    return zms_h265_annexb_is_sync_key(f->data, f->size);
}

/** 每个视频 sync 点开启新 GOP（对齐 SRS/ZLM gop_queue：按 IDR 滚动缓存）。*/
static int h264_ring_new_gop(const zms_frame *f, int video_key_pos, int has_video)
{
    (void)video_key_pos;
    if (!f) {
        return 0;
    }
    if (f->track == ZMS_TRACK_AUDIO) {
        return has_video ? 0 : 1;
    }
    if (f->config_frame) {
        return 0;
    }
    return h264_is_key(f);
}

static int h265_ring_new_gop(const zms_frame *f, int video_key_pos, int has_video)
{
    (void)video_key_pos;
    if (!f) {
        return 0;
    }
    if (f->track == ZMS_TRACK_AUDIO) {
        return has_video ? 0 : 1;
    }
    if (f->config_frame) {
        return 0;
    }
    return h265_is_key(f);
}

static int vp8_es_is_key(const zms_frame *f)
{
    if (!f || !f->data || f->size < 1) {
        return f && f->keyframe;
    }
    if (f->keyframe) {
        return 1;
    }
    return (f->data[0] & 0x01) == 0;
}

static int vp9_es_is_key(const zms_frame *f)
{
    static const uint8_t sync[] = {0x49, 0x83, 0x42};
    const uint8_t *p;

    if (!f || !f->data || f->size < 4) {
        return f && f->keyframe;
    }
    if (f->keyframe) {
        return 1;
    }
    p = f->data;
    if ((p[0] >> 6) != 0x02) {
        return 0;
    }
    return p[1] == sync[0] && p[2] == sync[1] && p[3] == sync[2];
}

static int av1_es_is_key(const zms_frame *f)
{
    if (!f || !f->data || f->size < 2) {
        return f && f->keyframe;
    }
    if (f->keyframe) {
        return 1;
    }
    return zms_av1_obu_has_sequence_header(f->data, f->size);
}

static int vpx_ring_storable(const zms_frame *f, int cache_started, int has_video,
                             int (*is_key)(const zms_frame *))
{
    if (!f) {
        return 0;
    }
    if (f->track == ZMS_TRACK_AUDIO) {
        return has_video ? cache_started : 1;
    }
    if (f->drop_able) {
        return cache_started;
    }
    if (is_key && is_key(f)) {
        return 1;
    }
    return cache_started;
}

static int vp8_ring_storable(const zms_frame *f, int cache_started, int has_video)
{
    return vpx_ring_storable(f, cache_started, has_video, vp8_es_is_key);
}

static int vp9_ring_storable(const zms_frame *f, int cache_started, int has_video)
{
    return vpx_ring_storable(f, cache_started, has_video, vp9_es_is_key);
}

static int vpx_ring_new_gop(const zms_frame *f, int video_key_pos, int has_video,
                            int (*is_key)(const zms_frame *))
{
    (void)has_video;
    (void)video_key_pos;
    if (!f) {
        return 0;
    }
    if (f->track == ZMS_TRACK_AUDIO) {
        return 0;
    }
    if (f->config_frame) {
        return 0;
    }
    return is_key && is_key(f);
}

static int vp8_ring_new_gop(const zms_frame *f, int video_key_pos, int has_video)
{
    return vpx_ring_new_gop(f, video_key_pos, has_video, vp8_es_is_key);
}

static int vp9_ring_new_gop(const zms_frame *f, int video_key_pos, int has_video)
{
    return vpx_ring_new_gop(f, video_key_pos, has_video, vp9_es_is_key);
}

static int av1_ring_storable(const zms_frame *f, int cache_started, int has_video)
{
    return vpx_ring_storable(f, cache_started, has_video, av1_es_is_key);
}

static int av1_ring_new_gop(const zms_frame *f, int video_key_pos, int has_video)
{
    return vpx_ring_new_gop(f, video_key_pos, has_video, av1_es_is_key);
}

static const zms_frame_codec_ops k_h264_es = {
    ZMS_CODEC_H264, "h264", h264_is_config, h264_is_key, default_ring_storable, h264_ring_new_gop};
static const zms_frame_codec_ops k_h265_es = {
    ZMS_CODEC_H265, "h265", h265_is_config, h265_is_key, default_ring_storable, h265_ring_new_gop};
static const zms_frame_codec_ops k_aac_es = {ZMS_CODEC_AAC,         "aac",
                                             default_is_config,     default_is_key,
                                             default_ring_storable, default_ring_new_gop};
static const zms_frame_codec_ops k_g711_es = {ZMS_CODEC_G711A,       "g711",
                                              default_is_config,     default_is_key,
                                              default_ring_storable, default_ring_new_gop};
static const zms_frame_codec_ops k_av1_es = {
    ZMS_CODEC_AV1, "av1", default_is_config, av1_es_is_key, av1_ring_storable, av1_ring_new_gop};
static const zms_frame_codec_ops k_opus_es = {ZMS_CODEC_OPUS,        "opus",
                                              default_is_config,     default_is_key,
                                              default_ring_storable, default_ring_new_gop};
static const zms_frame_codec_ops k_vp8_es = {
    ZMS_CODEC_VP8, "vp8", default_is_config, vp8_es_is_key, vp8_ring_storable, vp8_ring_new_gop};
static const zms_frame_codec_ops k_vp9_es = {
    ZMS_CODEC_VP9, "vp9", default_is_config, vp9_es_is_key, vp9_ring_storable, vp9_ring_new_gop};
static const zms_frame_codec_ops k_h266_es = {ZMS_CODEC_H266,        "h266",
                                              default_is_config,     default_is_key,
                                              default_ring_storable, default_ring_new_gop};

void zms_frame_codec_register(const zms_frame_codec_ops *ops)
{
    if (!ops || ops->id <= 0 || ops->id >= ZMS_FRAME_CODEC_SLOT_MAX) {
        return;
    }
    g_es[ops->id] = ops;
}

const zms_frame_codec_ops *zms_frame_codec_find(zms_codec_id id)
{
    if (id <= 0 || id >= ZMS_FRAME_CODEC_SLOT_MAX) {
        return &k_default_es;
    }
    if (!g_es[id]) {
        return &k_default_es;
    }
    return g_es[id];
}

void zms_frame_codec_register_all(void)
{
    zms_frame_codec_register(&k_h264_es);
    zms_frame_codec_register(&k_h265_es);
    zms_frame_codec_register(&k_aac_es);
    zms_frame_codec_register(&k_g711_es);
    zms_frame_codec_register(&k_av1_es);
    zms_frame_codec_register(&k_opus_es);
    zms_frame_codec_register(&k_vp8_es);
    zms_frame_codec_register(&k_vp9_es);
    zms_frame_codec_register(&k_h266_es);
    {
        static const zms_frame_codec_ops g711u = {ZMS_CODEC_G711U,       "g711u",
                                                  default_is_config,     default_is_key,
                                                  default_ring_storable, default_ring_new_gop};
        zms_frame_codec_register(&g711u);
    }
}
