#include "zms/session/codec_filter.h"
#include "zms/engine/gop/gop_queue.h"
#include "zms/engine/media_event.h"
#include "zms/media/container/flv/flv_tag_probe.h"
#include "ztk/util/log.h"
#include <stdio.h>
#include <string.h>

static zms_codec_id cap_video_codec(const zms_media_source *s)
{
    size_t len = 0;
    const uint8_t *cfg;

    if (!s) {
        return ZMS_CODEC_INVALID;
    }
    if (s->video.ready && s->video.codec != ZMS_CODEC_INVALID) {
        return s->video.codec;
    }
    if (s->gop_queue) {
        cfg = zms_gop_queue_video_config(s->gop_queue, &len);
        if (cfg && len > 0) {
            return zms_flv_tag_video_codec(cfg, len);
        }
    }
    return ZMS_CODEC_INVALID;
}

static zms_codec_id cap_audio_codec(const zms_media_source *s)
{
    size_t len = 0;
    const uint8_t *cfg;

    if (!s) {
        return ZMS_CODEC_INVALID;
    }
    if (s->audio.ready && s->audio.codec != ZMS_CODEC_INVALID) {
        return s->audio.codec;
    }
    if (s->gop_queue) {
        cfg = zms_gop_queue_audio_config(s->gop_queue, &len);
        if (cfg && len > 0) {
            return zms_flv_tag_audio_codec(cfg, len);
        }
    }
    return ZMS_CODEC_INVALID;
}

static const zms_codec_id k_cap_rtsp_video[] = {
    ZMS_CODEC_H264, ZMS_CODEC_H265, ZMS_CODEC_H266, ZMS_CODEC_AV1, ZMS_CODEC_VP8, ZMS_CODEC_VP9,
};

static const zms_codec_id k_cap_rtsp_audio[] = {
    ZMS_CODEC_AAC,
    ZMS_CODEC_G711A,
    ZMS_CODEC_G711U,
    ZMS_CODEC_OPUS,
};

static const zms_codec_id k_cap_flv_video[] = {
    ZMS_CODEC_H264,
    ZMS_CODEC_H265,
    ZMS_CODEC_AV1,
};

static const zms_codec_id k_cap_flv_audio[] = {
    ZMS_CODEC_AAC,
    ZMS_CODEC_G711A,
    ZMS_CODEC_G711U,
};

static const zms_codec_id k_cap_webrtc_video[] = {
    ZMS_CODEC_H264, ZMS_CODEC_H265, ZMS_CODEC_AV1, ZMS_CODEC_VP8, ZMS_CODEC_VP9,
};

/* 浏览器 WebRTC 播放侧不支持 AAC；源为 AAC 时应过滤音轨、仅视频播放 */
static const zms_codec_id k_cap_webrtc_audio[] = {
    ZMS_CODEC_OPUS,
};

static const zms_codec_id k_cap_ts_video[] = {
    ZMS_CODEC_H264,
    ZMS_CODEC_H265,
};

static const zms_codec_id k_cap_ts_audio[] = {
    ZMS_CODEC_AAC,
    ZMS_CODEC_G711A,
    ZMS_CODEC_G711U,
};

static const zms_codec_id k_cap_hls_video[] = {
    ZMS_CODEC_H264, ZMS_CODEC_H265, ZMS_CODEC_H266, ZMS_CODEC_AV1, ZMS_CODEC_VP8, ZMS_CODEC_VP9,
};

static const zms_codec_id k_cap_hls_audio[] = {
    ZMS_CODEC_AAC,
    ZMS_CODEC_OPUS,
};

typedef struct {
    const zms_codec_id *video;
    size_t video_n;
    const zms_codec_id *audio;
    size_t audio_n;
    const char *hint;
} cap_codec_set;

static int cap_codec_in_list(zms_codec_id codec, const zms_codec_id *list, size_t n)
{
    size_t i;

    if (codec == ZMS_CODEC_INVALID) {
        return 1;
    }
    for (i = 0; i < n; ++i) {
        if (list[i] == codec) {
            return 1;
        }
    }
    return 0;
}

static const cap_codec_set *cap_sets_for_role(zms_session_cap_role role)
{
    static const cap_codec_set rtsp = {
        k_cap_rtsp_video,
        sizeof(k_cap_rtsp_video) / sizeof(k_cap_rtsp_video[0]),
        k_cap_rtsp_audio,
        sizeof(k_cap_rtsp_audio) / sizeof(k_cap_rtsp_audio[0]),
        "H264/H265/H266/AV1/VP8/VP9 + AAC/G711/Opus (any combo)",
    };
    static const cap_codec_set flv = {
        k_cap_flv_video,
        sizeof(k_cap_flv_video) / sizeof(k_cap_flv_video[0]),
        k_cap_flv_audio,
        sizeof(k_cap_flv_audio) / sizeof(k_cap_flv_audio[0]),
        "H264/H265/AV1 + AAC/G711 (any combo)",
    };
    static const cap_codec_set webrtc = {
        k_cap_webrtc_video,
        sizeof(k_cap_webrtc_video) / sizeof(k_cap_webrtc_video[0]),
        k_cap_webrtc_audio,
        sizeof(k_cap_webrtc_audio) / sizeof(k_cap_webrtc_audio[0]),
        "H264/H265/AV1/VP8/VP9 + Opus (AAC 等不支持则自动无声播放)",
    };
    static const cap_codec_set ts = {
        k_cap_ts_video,
        sizeof(k_cap_ts_video) / sizeof(k_cap_ts_video[0]),
        k_cap_ts_audio,
        sizeof(k_cap_ts_audio) / sizeof(k_cap_ts_audio[0]),
        "H264/H265 + AAC/G711 (any combo)",
    };
    static const cap_codec_set hls = {
        k_cap_hls_video,
        sizeof(k_cap_hls_video) / sizeof(k_cap_hls_video[0]),
        k_cap_hls_audio,
        sizeof(k_cap_hls_audio) / sizeof(k_cap_hls_audio[0]),
        "H264/H265/H266/AV1/VP8/VP9 + AAC/Opus (any combo)",
    };

    switch (role) {
    case ZMS_PROTO_CAP_RTSP_PLAY:
    case ZMS_PROTO_CAP_RTSP_PUBLISH:
        return &rtsp;
    case ZMS_PROTO_CAP_RTMP_PLAY:
    case ZMS_PROTO_CAP_RTMP_PUBLISH:
    case ZMS_PROTO_CAP_HTTP_FLV_PLAY:
        return &flv;
    case ZMS_PROTO_CAP_WEBRTC_PLAY:
    case ZMS_PROTO_CAP_WEBRTC_PUBLISH:
        return &webrtc;
    case ZMS_PROTO_CAP_HTTP_TS_PLAY:
    case ZMS_PROTO_CAP_SRT_PLAY:
    case ZMS_PROTO_CAP_SRT_PUBLISH:
    case ZMS_PROTO_CAP_RTP_PS_PUBLISH:
        return &ts;
    case ZMS_PROTO_CAP_HLS_PLAY:
    case ZMS_PROTO_CAP_DASH_PLAY:
        return &hls;
    default:
        return NULL;
    }
}

static int cap_combo_ok(zms_session_cap_role role, zms_codec_id vc, zms_codec_id ac, int has_video,
                        int has_audio)
{
    const cap_codec_set *set = cap_sets_for_role(role);

    if (!set) {
        return 0;
    }
    if (has_video && vc != ZMS_CODEC_INVALID && !cap_codec_in_list(vc, set->video, set->video_n)) {
        return 0;
    }
    if (has_audio && ac != ZMS_CODEC_INVALID && !cap_codec_in_list(ac, set->audio, set->audio_n)) {
        return 0;
    }
    return 1;
}

ztk_err_t zms_session_capability_check(zms_session_cap_role role, zms_codec_id video,
                                       zms_codec_id audio, int has_video, int has_audio)
{
    if (!has_video && !has_audio) {
        return ZTK_ERR_INVALID;
    }
    if (!cap_combo_ok(role, video, audio, has_video, has_audio)) {
        return ZTK_ERR_NOT_IMPL;
    }
    return ZTK_OK;
}

ztk_err_t zms_session_capability_check_source(zms_session_cap_role role,
                                              const zms_media_source *src)
{
    zms_codec_id vc = ZMS_CODEC_INVALID;
    zms_codec_id ac = ZMS_CODEC_INVALID;
    int hv = 0;
    int ha = 0;

    if (!src) {
        return ZTK_ERR_INVALID;
    }
    hv = src->has_video;
    ha = src->has_audio;
    if (hv) {
        vc = cap_video_codec(src);
    }
    if (ha) {
        ac = cap_audio_codec(src);
    }
    if (!hv && !ha) {
        /* WHIP：首 SRTP 后才出现 track；answer 已协商 H264/Opus。 */
        if (src->publishing && src->gop_queue && src->publish_origin == ZMS_ORIGIN_WEBRTC_PUSH) {
            return ZTK_OK;
        }
        return ZTK_ERR_STATE;
    }
    /* WebRTC 播放：AAC/G711 等浏览器不支持的音轨直接丢掉，允许纯视频 */
    if (role == ZMS_PROTO_CAP_WEBRTC_PLAY && ha && ac != ZMS_CODEC_INVALID) {
        const cap_codec_set *set = cap_sets_for_role(role);
        if (set && !cap_codec_in_list(ac, set->audio, set->audio_n)) {
            ztk_info("[capability] webrtc-play filter audio=%s -> video-only app=%s stream=%s",
                     zms_codec_name(ac), src->app, src->stream);
            ha = 0;
            ac = ZMS_CODEC_INVALID;
        }
    }
    return zms_session_capability_check(role, vc, ac, hv, ha);
}

zms_session_cap_role zms_session_capability_play_role(const char *player)
{
    if (!player || !player[0]) {
        return ZMS_PROTO_CAP_RTMP_PLAY;
    }
    if (strcmp(player, "rtsp") == 0) {
        return ZMS_PROTO_CAP_RTSP_PLAY;
    }
    if (strcmp(player, "http-flv") == 0 || strcmp(player, "ws-flv") == 0) {
        return ZMS_PROTO_CAP_HTTP_FLV_PLAY;
    }
    if (strcmp(player, "http-ts") == 0) {
        return ZMS_PROTO_CAP_HTTP_TS_PLAY;
    }
    if (strcmp(player, "webrtc") == 0 || strcmp(player, "whep") == 0) {
        return ZMS_PROTO_CAP_WEBRTC_PLAY;
    }
    if (strcmp(player, "srt") == 0) {
        return ZMS_PROTO_CAP_SRT_PLAY;
    }
    if (strcmp(player, "hls") == 0) {
        return ZMS_PROTO_CAP_HLS_PLAY;
    }
    if (strcmp(player, "dash") == 0) {
        return ZMS_PROTO_CAP_DASH_PLAY;
    }
    return ZMS_PROTO_CAP_RTMP_PLAY;
}

zms_session_cap_role zms_session_capability_publish_role(const zms_media_source *src)
{
    if (!src) {
        return ZMS_PROTO_CAP_RTMP_PUBLISH;
    }
    if (src->publish_origin == ZMS_ORIGIN_SRT_PUSH) {
        return ZMS_PROTO_CAP_SRT_PUBLISH;
    }
    if (src->publish_origin == ZMS_ORIGIN_RTP_PS_PUSH) {
        return ZMS_PROTO_CAP_RTP_PS_PUBLISH;
    }
    if (src->publish_origin == ZMS_ORIGIN_RTSP_PUSH) {
        return ZMS_PROTO_CAP_RTSP_PUBLISH;
    }
    if (src->publish_origin == ZMS_ORIGIN_WEBRTC_PUSH) {
        return ZMS_PROTO_CAP_WEBRTC_PUBLISH;
    }
    if (strcmp(src->schema, ZMS_SCHEMA_SRT) == 0) {
        return ZMS_PROTO_CAP_SRT_PUBLISH;
    }
    if (strcmp(src->schema, ZMS_SCHEMA_RTP_PS) == 0) {
        return ZMS_PROTO_CAP_RTP_PS_PUBLISH;
    }
    if (strcmp(src->schema, ZMS_SCHEMA_RTSP) == 0) {
        return ZMS_PROTO_CAP_RTSP_PUBLISH;
    }
    return ZMS_PROTO_CAP_RTMP_PUBLISH;
}

ztk_err_t zms_session_capability_check_publish_source(const zms_media_source *src)
{
    return zms_session_capability_check_source(zms_session_capability_publish_role(src), src);
}

const char *zms_session_capability_role_name(zms_session_cap_role role)
{
    switch (role) {
    case ZMS_PROTO_CAP_RTSP_PLAY:
        return "rtsp-play";
    case ZMS_PROTO_CAP_RTSP_PUBLISH:
        return "rtsp-publish";
    case ZMS_PROTO_CAP_RTMP_PLAY:
        return "rtmp-play";
    case ZMS_PROTO_CAP_RTMP_PUBLISH:
        return "rtmp-publish";
    case ZMS_PROTO_CAP_HTTP_FLV_PLAY:
        return "http-flv-play";
    case ZMS_PROTO_CAP_HTTP_TS_PLAY:
        return "http-ts-play";
    case ZMS_PROTO_CAP_WEBRTC_PLAY:
        return "webrtc-play";
    case ZMS_PROTO_CAP_WEBRTC_PUBLISH:
        return "webrtc-publish";
    case ZMS_PROTO_CAP_SRT_PLAY:
        return "srt-play";
    case ZMS_PROTO_CAP_SRT_PUBLISH:
        return "srt-publish";
    case ZMS_PROTO_CAP_RTP_PS_PUBLISH:
        return "rtp-ps-publish";
    case ZMS_PROTO_CAP_HLS_PLAY:
        return "hls-play";
    case ZMS_PROTO_CAP_DASH_PLAY:
        return "dash-play";
    default:
        return "unknown";
    }
}

const char *zms_session_capability_allowed_hint(zms_session_cap_role role)
{
    const cap_codec_set *set = cap_sets_for_role(role);

    return set && set->hint ? set->hint : "";
}

void zms_session_capability_log_reject(const char *player, const zms_media_source *src,
                                       zms_session_cap_role role)
{
    zms_codec_id vc = ZMS_CODEC_INVALID;
    zms_codec_id ac = ZMS_CODEC_INVALID;

    if (!src) {
        return;
    }
    if (src->has_video) {
        vc = cap_video_codec(src);
    }
    if (src->has_audio) {
        ac = cap_audio_codec(src);
    }
    ztk_warn("[capability] reject %s app=%s stream=%s video=%s audio=%s allowed=%s",
             player && player[0] ? player : zms_session_capability_role_name(role), src->app,
             src->stream, vc != ZMS_CODEC_INVALID ? zms_codec_name(vc) : "-",
             ac != ZMS_CODEC_INVALID ? zms_codec_name(ac) : "-",
             zms_session_capability_allowed_hint(role));
}
