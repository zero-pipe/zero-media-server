#include "webrtc/session/webrtc_session_internal.h"
#include "webrtc/session/webrtc_media_internal.h"
#include "webrtc/session/webrtc_ice_internal.h"
#include "zms/live/ingest/common/stream_ingest.h"
#include "zms/media/codec/codec_id.h"
#include "zms/media/codec/h264/h264_es.h"
#include "zms/media/codec/h264/h264_over_rtmp.h"
#include "zms/engine/gop/gop_queue.h"
#include "zms/engine/stream/stream_hub.h"
#include "zms/webrtc/webrtc_service.h"
#include "zms/session/rtsp/rtsp_sdp.h"
#include "sdp.h"
#include "sdp-a-fmtp.h"
#include "sdp-a-rtpmap.h"
#include "ztk/util/log.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#define webrtc_strcasecmp _stricmp
#else
#include <strings.h>
#define webrtc_strcasecmp strcasecmp
#endif

static int webrtc_sdp_append(char *answer, size_t answer_cap, size_t n, const char *fmt, ...)
{
    va_list ap;
    int w;
    size_t room;

    if (!answer || !fmt || n >= answer_cap) {
        return -1;
    }
    room = answer_cap - n;
    va_start(ap, fmt);
    w = vsnprintf(answer + n, room, fmt, ap);
    va_end(ap);
    if (w < 0 || (size_t)w >= room) {
        return -1;
    }
    return (int)(n + (size_t)w);
}

typedef struct webrtc_neg_track {
    zms_codec_id codec;
    uint8_t pt;
    int rate;
    int channels;
} webrtc_neg_track;

static void webrtc_rand_credential(char *out, size_t cap, size_t want)
{
    static const char k_alph[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    size_t i;

    if (!out || cap == 0 || want == 0) {
        return;
    }
    if (want >= cap) {
        want = cap - 1;
    }
    for (i = 0; i < want; ++i) {
        out[i] = k_alph[(unsigned)rand() % (sizeof(k_alph) - 1)];
    }
    out[want] = '\0';
}

static int webrtc_parse_remote_ice(sdp_t *sdp, char *ufrag, size_t ufrag_cap, char *pwd,
                                   size_t pwd_cap)
{
    const char *v;
    int i, n;

    if (!sdp) {
        return -1;
    }
    v = sdp_attribute_find(sdp, "ice-ufrag");
    if (!v || !v[0]) {
        n = sdp_media_count(sdp);
        for (i = 0; i < n; ++i) {
            v = sdp_media_attribute_find(sdp, i, "ice-ufrag");
            if (v && v[0]) {
                break;
            }
        }
    }
    if (!v || !v[0]) {
        return -1;
    }
    strncpy(ufrag, v, ufrag_cap - 1);
    ufrag[ufrag_cap - 1] = '\0';

    v = sdp_attribute_find(sdp, "ice-pwd");
    if (!v || !v[0]) {
        n = sdp_media_count(sdp);
        for (i = 0; i < n; ++i) {
            v = sdp_media_attribute_find(sdp, i, "ice-pwd");
            if (v && v[0]) {
                break;
            }
        }
    }
    if (!v || !v[0]) {
        return -1;
    }
    strncpy(pwd, v, pwd_cap - 1);
    pwd[pwd_cap - 1] = '\0';
    return 0;
}

static int webrtc_media_index(sdp_t *sdp, const char *media_type)
{
    int i, n;

    if (!sdp || !media_type) {
        return -1;
    }
    n = sdp_media_count(sdp);
    for (i = 0; i < n; ++i) {
        if (sdp_media_type(sdp, i) && strcmp(sdp_media_type(sdp, i), media_type) == 0) {
            return i;
        }
    }
    return -1;
}

static int webrtc_offer_has_media(sdp_t *sdp, const char *media_type)
{
    return webrtc_media_index(sdp, media_type) >= 0;
}

static zms_codec_id webrtc_codec_from_encoding(const char *encoding)
{
    char lower[32];
    size_t i;

    if (!encoding || !encoding[0]) {
        return ZMS_CODEC_INVALID;
    }
    for (i = 0; encoding[i] && i + 1 < sizeof(lower); ++i) {
        lower[i] = (char)tolower((unsigned char)encoding[i]);
    }
    lower[i] = '\0';
    if (strstr(lower, "h264") || strstr(lower, "avc")) {
        return ZMS_CODEC_H264;
    }
    if (strstr(lower, "h265") || strstr(lower, "hevc")) {
        return ZMS_CODEC_H265;
    }
    if (strstr(lower, "av1") || strstr(lower, "av01")) {
        return ZMS_CODEC_AV1;
    }
    if (strstr(lower, "vp8")) {
        return ZMS_CODEC_VP8;
    }
    if (strstr(lower, "vp9")) {
        return ZMS_CODEC_VP9;
    }
    if (strstr(lower, "mpeg4-generic") || strstr(lower, "aac") || strstr(lower, "mp4a")) {
        return ZMS_CODEC_AAC;
    }
    if (strstr(lower, "opus")) {
        return ZMS_CODEC_OPUS;
    }
    if (strstr(lower, "rtx") || strstr(lower, "red") || strstr(lower, "ulpfec") ||
        strstr(lower, "flexfec")) {
        return ZMS_CODEC_INVALID;
    }
    return ZMS_CODEC_INVALID;
}

static int webrtc_codec_negotiable(zms_codec_id codec, int is_video, int for_play)
{
    if (is_video) {
        if (codec == ZMS_CODEC_H264 || codec == ZMS_CODEC_VP8 || codec == ZMS_CODEC_VP9) {
            return 1;
        }
        if (for_play && (codec == ZMS_CODEC_H265 || codec == ZMS_CODEC_AV1)) {
            return 1;
        }
        return 0;
    }
    /* 浏览器 WebRTC 播放/推流音轨以 Opus 为主；AAC 不可协商 */
    (void)for_play;
    return codec == ZMS_CODEC_OPUS;
}

static void webrtc_on_media_rtpmap_log(void *param, const char *name, const char *value)
{
    const char *tag = (const char *)param;
    int pt = 0;
    int rate = 0;
    char enc[16];
    char params[64];

    if (!name || !value || strcmp(name, "rtpmap") != 0) {
        return;
    }
    enc[0] = params[0] = '\0';
    if (sdp_a_rtpmap(value, &pt, enc, &rate, params) == 0) {
        ztk_debug("[webrtc] sdp %s rtpmap pt=%d enc=%s rate=%d", tag ? tag : "?", pt, enc, rate);
    } else {
        ztk_debug("[webrtc] sdp %s rtpmap parse fail value=[%s]", tag ? tag : "?", value);
    }
}

static void webrtc_log_media_rtpmaps(sdp_t *sdp, int media_idx, const char *tag)
{
    if (!sdp || media_idx < 0 || !tag) {
        return;
    }
    ztk_debug("[webrtc] sdp %s media=%s idx=%d", tag, sdp_media_type(sdp, media_idx), media_idx);
    sdp_media_attribute_list(sdp, media_idx, "rtpmap", webrtc_on_media_rtpmap_log, (void *)tag);
}

typedef struct {
    int for_play;
    int is_video;
    zms_codec_id prefer;
    int want_pt;
    webrtc_neg_track *out;
    int done;
} webrtc_neg_pick_ctx;

static void webrtc_pick_rtpmap_cb(void *param, const char *name, const char *value)
{
    webrtc_neg_pick_ctx *pick = (webrtc_neg_pick_ctx *)param;
    webrtc_neg_track cand;
    int pt = 0;
    int rate = 0;
    char enc[16];
    char params[64];

    if (!pick || pick->done || !pick->out || !name || !value || strcmp(name, "rtpmap") != 0) {
        return;
    }
    enc[0] = params[0] = '\0';
    if (sdp_a_rtpmap(value, &pt, enc, &rate, params) != 0) {
        return;
    }
    if (pick->want_pt >= 0 && pt != pick->want_pt) {
        return;
    }
    cand.codec = webrtc_codec_from_encoding(enc);
    if (cand.codec == ZMS_CODEC_INVALID) {
        return;
    }
    if (!webrtc_codec_negotiable(cand.codec, pick->is_video, pick->for_play)) {
        return;
    }
    if (pick->for_play && pick->prefer != ZMS_CODEC_INVALID && cand.codec != pick->prefer) {
        return;
    }
    cand.pt = (uint8_t)pt;
    cand.rate = rate > 0 ? rate : (cand.codec == ZMS_CODEC_OPUS ? 48000 : 90000);
    cand.channels = 2;
    if (params[0]) {
        int ch = atoi(params);
        if (ch > 0) {
            cand.channels = ch;
        }
    }
    *pick->out = cand;
    pick->done = 1;
}

static const char k_webrtc_b64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t webrtc_base64_encode(const uint8_t *in, size_t in_len, char *out, size_t cap)
{
    size_t i = 0, o = 0;

    while (i + 2 < in_len && o + 4 < cap) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
        out[o++] = k_webrtc_b64[(v >> 18) & 63];
        out[o++] = k_webrtc_b64[(v >> 12) & 63];
        out[o++] = k_webrtc_b64[(v >> 6) & 63];
        out[o++] = k_webrtc_b64[v & 63];
        i += 3;
    }
    if (i < in_len && o + 4 < cap) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < in_len) {
            v |= (uint32_t)in[i + 1] << 8;
        }
        out[o++] = k_webrtc_b64[(v >> 18) & 63];
        out[o++] = k_webrtc_b64[(v >> 12) & 63];
        out[o++] = (i + 1 < in_len) ? k_webrtc_b64[(v >> 6) & 63] : '=';
        out[o++] = '=';
    }
    if (o < cap) {
        out[o] = '\0';
    }
    return o;
}

static void webrtc_h264_profile_lower(char *profile)
{
    char *p;

    if (!profile || !profile[0]) {
        return;
    }
    for (p = profile; *p; ++p) {
        *p = (char)tolower((unsigned char)*p);
    }
}

static void webrtc_h264_profile_from_sps(const uint8_t *sps, size_t sps_len, char *profile,
                                         size_t cap)
{
    if (!profile || cap == 0) {
        return;
    }
    profile[0] = '\0';
    if (!sps || sps_len < 4) {
        return;
    }
    snprintf(profile, cap, "%02x%02x%02x", sps[1], sps[2], sps[3]);
    webrtc_h264_profile_lower(profile);
}

typedef struct {
    int want_pt;
    const char *found;
} webrtc_fmtp_pick_ctx;

static void webrtc_pick_fmtp_cb(void *param, const char *name, const char *value)
{
    webrtc_fmtp_pick_ctx *ctx = (webrtc_fmtp_pick_ctx *)param;
    int fmt;

    if (!ctx || !name || !value || strcmp(name, "fmtp") != 0) {
        return;
    }
    fmt = atoi(value);
    if (fmt == ctx->want_pt) {
        ctx->found = value;
    }
}

static const char *webrtc_sdp_fmtp_for_pt(sdp_t *sdp, int media_idx, int pt)
{
    webrtc_fmtp_pick_ctx ctx;

    memset(&ctx, 0, sizeof(ctx));
    ctx.want_pt = pt;
    sdp_media_attribute_list(sdp, media_idx, "fmtp", webrtc_pick_fmtp_cb, &ctx);
    return ctx.found;
}

static int webrtc_h264_profile_score(const char *offer_plid, const char *want_plid, int pkt_mode)
{
    int score = 0;
    char offer[16];
    char want[16];

    if (pkt_mode != 1) {
        score -= 8;
    }
    if (!offer_plid || !offer_plid[0]) {
        return score;
    }
    snprintf(offer, sizeof(offer), "%s", offer_plid);
    webrtc_h264_profile_lower(offer);
    if (!want_plid || !want_plid[0]) {
        return score + 1;
    }
    snprintf(want, sizeof(want), "%s", want_plid);
    webrtc_h264_profile_lower(want);
    if (strcmp(offer, want) == 0) {
        return score + 32;
    }
    if (offer[0] == want[0] && offer[1] == want[1]) {
        return score + 16;
    }
    return score + 1;
}

typedef struct {
    sdp_t *sdp;
    int media_idx;
    const zms_webrtc_session *sess;
    char want_plid[16];
    webrtc_neg_track best;
    int best_score;
    int have_best;
} webrtc_h264_neg_ctx;

static void webrtc_negotiate_h264_cb(void *param, const char *name, const char *value)
{
    webrtc_h264_neg_ctx *ctx = (webrtc_h264_neg_ctx *)param;
    webrtc_neg_track cand;
    struct sdp_a_fmtp_h264_t h264;
    int pt = 0, fmt = 0, rate = 0, score;
    char enc[16];
    char params[64];

    if (!ctx || !name || !value || strcmp(name, "rtpmap") != 0) {
        return;
    }
    enc[0] = params[0] = '\0';
    if (sdp_a_rtpmap(value, &pt, enc, &rate, params) != 0) {
        return;
    }
    if (webrtc_codec_from_encoding(enc) != ZMS_CODEC_H264) {
        return;
    }
    memset(&h264, 0, sizeof(h264));
    {
        const char *fmtp = webrtc_sdp_fmtp_for_pt(ctx->sdp, ctx->media_idx, pt);
        char line[512];

        if (fmtp) {
            snprintf(line, sizeof(line), "%d %s", pt, fmtp);
            if (sdp_a_fmtp_h264(line, &fmt, &h264) != 0) {
                memset(&h264, 0, sizeof(h264));
            }
        }
    }
    score = webrtc_h264_profile_score(
        (h264.flags & SDP_A_FMTP_H264_PROFILE_LEVEL_ID) ? h264.profile_level_id : NULL,
        ctx->want_plid[0] ? ctx->want_plid : NULL,
        (h264.flags & SDP_A_FMTP_H264_PACKETIZATION_MODE) ? h264.packetization_mode : 1);
    if (ctx->have_best && score <= ctx->best_score) {
        return;
    }
    cand.codec = ZMS_CODEC_H264;
    cand.pt = (uint8_t)pt;
    cand.rate = 90000;
    cand.channels = 1;
    ctx->best = cand;
    ctx->best_score = score;
    ctx->have_best = 1;
}

static int webrtc_negotiate_h264_track(sdp_t *sdp, const zms_webrtc_session *sess,
                                       webrtc_neg_track *out)
{
    webrtc_h264_neg_ctx ctx;
    int media_idx;

    if (!sdp || !out) {
        return -1;
    }
    media_idx = webrtc_media_index(sdp, "video");
    if (media_idx < 0) {
        return -1;
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.sdp = sdp;
    ctx.media_idx = media_idx;
    ctx.sess = sess;
    ctx.best_score = -999;
    if (sess && sess->source && sess->source->video.ready &&
        sess->source->video.profile_level_id[0]) {
        snprintf(ctx.want_plid, sizeof(ctx.want_plid), "%s", sess->source->video.profile_level_id);
    }
    sdp_media_attribute_list(sdp, media_idx, "rtpmap", webrtc_negotiate_h264_cb, &ctx);
    if (!ctx.have_best) {
        return -1;
    }
    *out = ctx.best;
    ztk_info("[webrtc] h264 pt=%u profile=%s score=%d", (unsigned)out->pt,
             ctx.want_plid[0] ? ctx.want_plid : "?", ctx.best_score);
    return 0;
}

static int webrtc_h264_extract_ps(const zms_webrtc_session *s, const uint8_t **sps, size_t *sps_len,
                                  const uint8_t **pps, size_t *pps_len)
{
    size_t vcfg_len = 0;
    const uint8_t *vcfg;

    if (!s || !sps || !sps_len || !pps || !pps_len) {
        return 0;
    }
    *sps = *pps = NULL;
    *sps_len = *pps_len = 0;
    if (s->source) {
        vcfg = zms_media_source_video_config(s->source, &vcfg_len);
        if (vcfg && vcfg_len &&
            zms_rtmp_avc_extract_sps_pps(vcfg, vcfg_len, sps, sps_len, pps, pps_len)) {
            return 1;
        }
        if (s->source->gop_queue) {
            vcfg = zms_gop_queue_video_config(s->source->gop_queue, &vcfg_len);
            if (vcfg && vcfg_len &&
                zms_rtmp_avc_extract_sps_pps(vcfg, vcfg_len, sps, sps_len, pps, pps_len)) {
                return 1;
            }
        }
        /* WHEP 播放已持有该队列 gop reader；此处勿 attach_beginning。 */
        if (s->play.readers.gop) {
            return 0;
        }
        if (s->source->gop_queue) {
            zms_gop_reader *rd = zms_gop_reader_attach_beginning(s->source->gop_queue);
            zms_gop_slot slot;

            if (rd) {
                while (zms_gop_reader_read(rd, &slot) > 0) {
                    if (slot.track != ZMS_TRACK_VIDEO || !slot.data || slot.len == 0) {
                        continue;
                    }
                    if (zms_h264_es_extract_sps_pps(slot.data, slot.len, sps, sps_len, pps,
                                                    pps_len)) {
                        zms_gop_reader_detach(rd);
                        return 1;
                    }
                    if (slot.keyframe) {
                        break;
                    }
                }
                zms_gop_reader_detach(rd);
            }
        }
    }
    return 0;
}

static void webrtc_h264_load_fmtp(const zms_webrtc_session *s, char *profile, size_t profile_cap,
                                  char *sprop, size_t sprop_cap)
{
    const uint8_t *sps = NULL, *pps = NULL;
    size_t sps_len = 0, pps_len = 0;
    char sps_b64[512], pps_b64[256];
    size_t vcfg_len = 0;
    const uint8_t *vcfg;

    if (profile && profile_cap) {
        profile[0] = '\0';
    }
    if (sprop && sprop_cap) {
        sprop[0] = '\0';
    }
    if (s && s->source && s->source->video.ready && s->source->video.profile_level_id[0] &&
        profile && profile_cap) {
        snprintf(profile, profile_cap, "%s", s->source->video.profile_level_id);
    }
    if (webrtc_h264_extract_ps(s, &sps, &sps_len, &pps, &pps_len)) {
        if (profile && profile_cap && (!profile[0] || strncmp(profile, "000000", 6) == 0)) {
            webrtc_h264_profile_from_sps(sps, sps_len, profile, profile_cap);
        }
        if (sps && sps_len && pps && pps_len && sprop && sprop_cap) {
            webrtc_base64_encode(sps, sps_len, sps_b64, sizeof(sps_b64));
            webrtc_base64_encode(pps, pps_len, pps_b64, sizeof(pps_b64));
            snprintf(sprop, sprop_cap, "%s,%s", sps_b64, pps_b64);
        }
    }
    if (profile && profile_cap && !profile[0] && s && s->source) {
        vcfg = zms_media_source_video_config(s->source, &vcfg_len);
        if (vcfg && vcfg_len && !zms_rtmp_avc_profile_level_id(vcfg, vcfg_len, profile)) {
            snprintf(profile, profile_cap, "42001f");
        }
    }
    if (profile && profile_cap && profile[0]) {
        webrtc_h264_profile_lower(profile);
    } else if (profile && profile_cap) {
        snprintf(profile, profile_cap, "42001f");
    }
    ztk_info("[webrtc] h264 fmtp profile=%s sprop=%s len=%zu", profile ? profile : "?",
             (sprop && sprop[0]) ? "yes" : "no", (sprop && sprop[0]) ? strlen(sprop) : 0);
}

static int webrtc_negotiate_track(sdp_t *sdp, const char *media_type, zms_codec_id prefer,
                                  int for_play, const zms_webrtc_session *sess,
                                  webrtc_neg_track *out)
{
    int media_idx, i, n, fmts[16];
    webrtc_neg_pick_ctx pick;

    if (!sdp || !media_type || !out) {
        return -1;
    }
    if (for_play && prefer == ZMS_CODEC_H264 && strcmp(media_type, "video") == 0) {
        return webrtc_negotiate_h264_track(sdp, sess, out);
    }
    media_idx = webrtc_media_index(sdp, media_type);
    if (media_idx < 0) {
        return -1;
    }
    n = sdp_media_formats(sdp, media_idx, fmts, (int)(sizeof(fmts) / sizeof(fmts[0])));
    if (n <= 0) {
        return -1;
    }

    memset(&pick, 0, sizeof(pick));
    pick.for_play = for_play;
    pick.is_video = strcmp(media_type, "video") == 0;
    pick.prefer = prefer;
    pick.out = out;

    if (for_play && prefer != ZMS_CODEC_INVALID) {
        for (i = 0; i < n; ++i) {
            pick.done = 0;
            pick.want_pt = fmts[i];
            sdp_media_attribute_list(sdp, media_idx, "rtpmap", webrtc_pick_rtpmap_cb, &pick);
            if (pick.done) {
                return 0;
            }
        }
        pick.done = 0;
        pick.want_pt = -1;
        sdp_media_attribute_list(sdp, media_idx, "rtpmap", webrtc_pick_rtpmap_cb, &pick);
        if (pick.done) {
            return 0;
        }
    } else {
        pick.want_pt = -1;
        sdp_media_attribute_list(sdp, media_idx, "rtpmap", webrtc_pick_rtpmap_cb, &pick);
        if (pick.done) {
            return 0;
        }
    }

    webrtc_log_media_rtpmaps(sdp, media_idx, media_type);
    ztk_warn("[webrtc] negotiate failed media=%s fmts=%d", media_type, n);
    for (i = 0; i < n; ++i) {
        ztk_warn("[webrtc]   fmt[%d]=%d", i, fmts[i]);
    }
    return -1;
}

static zms_codec_id webrtc_prefer_video_codec(const zms_webrtc_session *s)
{
    if (s && s->source && s->source->video.ready && s->source->video.codec != ZMS_CODEC_INVALID) {
        return s->source->video.codec;
    }
    return ZMS_CODEC_H264;
}

static zms_codec_id webrtc_prefer_audio_codec(const zms_webrtc_session *s)
{
    if (s && s->source && s->source->audio.ready && s->source->audio.codec != ZMS_CODEC_INVALID) {
        /* 仅当源流本身是 Opus 时才协商音轨；AAC 等在 negotiate 前过滤 */
        if (s->source->audio.codec == ZMS_CODEC_OPUS) {
            return ZMS_CODEC_OPUS;
        }
        return ZMS_CODEC_INVALID;
    }
    return ZMS_CODEC_OPUS;
}

static int webrtc_source_audio_playable(const zms_webrtc_session *s)
{
    if (!s || !s->source || !s->source->has_audio) {
        return 0;
    }
    if (s->source->audio.ready && s->source->audio.codec != ZMS_CODEC_INVALID) {
        return s->source->audio.codec == ZMS_CODEC_OPUS;
    }
    /* 音轨未就绪时不强行带音频，避免 AAC 源误协商 */
    return 0;
}

static void webrtc_session_save_layout(zms_webrtc_session *mut, sdp_t *sdp)
{
    int ai, vi;
    const char *mid;

    ai = webrtc_media_index(sdp, "audio");
    vi = webrtc_media_index(sdp, "video");
    mut->offer_audio_before_video = (ai >= 0 && vi >= 0 && ai < vi) ? 1 : 0;
    if (vi >= 0) {
        mid = sdp_media_attribute_find(sdp, vi, "mid");
        snprintf(mut->video_mid, sizeof(mut->video_mid), "%s", mid && mid[0] ? mid : "0");
    } else {
        mut->video_mid[0] = '\0';
    }
    if (ai >= 0) {
        mid = sdp_media_attribute_find(sdp, ai, "mid");
        snprintf(mut->audio_mid, sizeof(mut->audio_mid), "%s", mid && mid[0] ? mid : "1");
    } else {
        mut->audio_mid[0] = '\0';
    }
}

static int webrtc_session_negotiate(zms_webrtc_session *mut, sdp_t *sdp, int for_play)
{
    webrtc_neg_track v, a;

    mut->offer_has_video = webrtc_offer_has_media(sdp, "video");
    mut->offer_has_audio = webrtc_offer_has_media(sdp, "audio");
    mut->answer_has_video = mut->offer_has_video;
    mut->answer_has_audio = mut->offer_has_audio;
    mut->video_codec = ZMS_CODEC_INVALID;
    mut->audio_codec = ZMS_CODEC_INVALID;
    mut->audio_rate = 48000;
    mut->audio_channels = 2;

    if (mut->answer_has_video) {
        if (webrtc_negotiate_track(sdp, "video",
                                   for_play ? webrtc_prefer_video_codec(mut) : ZMS_CODEC_INVALID,
                                   for_play, mut, &v) != 0) {
            return -1;
        }
        mut->video_codec = v.codec;
        mut->video_pt = v.pt;
    }
    if (mut->answer_has_audio && for_play && !webrtc_source_audio_playable(mut)) {
        zms_codec_id src_ac = ZMS_CODEC_INVALID;
        if (mut->source && mut->source->audio.ready) {
            src_ac = mut->source->audio.codec;
        }
        ztk_info("[webrtc] filter unsupported audio=%s, video-only play app=%s stream=%s",
                 zms_codec_name(src_ac), mut->app, mut->stream);
        mut->answer_has_audio = 0;
        mut->audio_codec = ZMS_CODEC_INVALID;
        mut->audio_pt = 0;
    }
    if (mut->answer_has_audio) {
        if (webrtc_negotiate_track(sdp, "audio",
                                   for_play ? webrtc_prefer_audio_codec(mut) : ZMS_CODEC_INVALID,
                                   for_play, mut, &a) != 0) {
            if (for_play) {
                ztk_info("[webrtc] audio negotiate failed, video-only play app=%s stream=%s",
                         mut->app, mut->stream);
                mut->answer_has_audio = 0;
                mut->audio_codec = ZMS_CODEC_INVALID;
                mut->audio_pt = 0;
            } else {
                return -1;
            }
        } else {
            mut->audio_codec = a.codec;
            mut->audio_pt = a.pt;
            mut->audio_rate = a.rate > 0 ? a.rate : 48000;
            mut->audio_channels = a.channels > 0 ? a.channels : 2;
        }
    }
    webrtc_session_save_layout(mut, sdp);
    return 0;
}

static int webrtc_base64_val(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

static size_t webrtc_base64_decode(const char *in, uint8_t *out, size_t cap)
{
    size_t o = 0;
    int val[4];

    if (!in || !out) {
        return 0;
    }
    while (*in) {
        int n = 0;
        for (; n < 4 && *in; ++in) {
            if (*in == '=' || *in == ' ' || *in == '\r' || *in == '\n' || *in == '\t') {
                continue;
            }
            val[n] = webrtc_base64_val(*in);
            if (val[n] < 0) {
                return o;
            }
            ++n;
        }
        if (n < 2) {
            break;
        }
        if (o < cap) {
            out[o++] = (uint8_t)((val[0] << 2) | (val[1] >> 4));
        }
        if (n > 2 && o < cap) {
            out[o++] = (uint8_t)(((val[1] & 15) << 4) | (val[2] >> 2));
        }
        if (n > 3 && o < cap) {
            out[o++] = (uint8_t)(((val[2] & 3) << 6) | val[3]);
        }
    }
    return o;
}

static int webrtc_publish_store_offer_h264_sprop(zms_webrtc_session *s, sdp_t *sdp)
{
    int media_idx;
    const char *fmtp;
    const char *params;
    const char *sprop;
    char sps_b64[384];
    char pps_b64[384];
    char *comma;

    if (!s || !sdp || s->video_codec != ZMS_CODEC_H264) {
        return -1;
    }
    media_idx = webrtc_media_index(sdp, "video");
    if (media_idx < 0) {
        return -1;
    }
    fmtp = webrtc_sdp_fmtp_for_pt(sdp, media_idx, s->video_pt ? s->video_pt : 96);
    if (!fmtp) {
        return -1;
    }
    params = fmtp;
    if (atoi(fmtp) == (s->video_pt ? s->video_pt : 96)) {
        const char *sp = strchr(fmtp, ' ');
        if (sp && sp[1]) {
            params = sp + 1;
        }
    }
    sprop = strstr(params, "sprop-parameter-sets=");
    if (!sprop) {
        return -1;
    }
    sprop += 21;
    snprintf(sps_b64, sizeof(sps_b64), "%s", sprop);
    comma = strchr(sps_b64, ';');
    if (comma) {
        *comma = '\0';
    }
    comma = strchr(sps_b64, ',');
    if (!comma) {
        return -1;
    }
    *comma = '\0';
    snprintf(pps_b64, sizeof(pps_b64), "%s", comma + 1);
    s->whip_h264_sps_len =
        webrtc_base64_decode(sps_b64, s->whip_h264_sps, sizeof(s->whip_h264_sps));
    s->whip_h264_pps_len =
        webrtc_base64_decode(pps_b64, s->whip_h264_pps, sizeof(s->whip_h264_pps));
    if (!s->whip_h264_sps_len || !s->whip_h264_pps_len) {
        return -1;
    }
    s->whip_h264_have_sprop = 1;
    return 0;
}

void zms_webrtc_publish_apply_offer_h264_sprop(zms_webrtc_session *s)
{
    if (!s || !s->ingest || !s->whip_h264_have_sprop) {
        return;
    }
    if (zms_live_ingest_set_h264_sps_pps(s->ingest, s->whip_h264_sps, s->whip_h264_sps_len,
                                         s->whip_h264_pps, s->whip_h264_pps_len) == ZTK_OK) {
        ztk_info("[webrtc] WHIP applied offer sprop SPS=%zu PPS=%zu app=%s stream=%s",
                 s->whip_h264_sps_len, s->whip_h264_pps_len, s->app, s->stream);
    }
}

static int webrtc_offer_setup_passive(sdp_t *sdp)
{
    const char *v;
    int i, n;

    if (!sdp) {
        return 0;
    }
    v = sdp_attribute_find(sdp, "setup");
    if (v && v[0]) {
        return webrtc_strcasecmp(v, "passive") == 0;
    }
    n = sdp_media_count(sdp);
    for (i = 0; i < n; ++i) {
        v = sdp_media_attribute_find(sdp, i, "setup");
        if (v && v[0]) {
            return webrtc_strcasecmp(v, "passive") == 0;
        }
    }
    return 0;
}

static const char *webrtc_dtls_setup_answer(const zms_webrtc_session *s)
{
    return s && s->dtls_as_client ? "active" : "passive";
}

static int webrtc_snprintf_bundle(char *buf, size_t cap, const zms_webrtc_session *s)
{
    int bundle_audio = s->answer_has_audio || s->offer_has_audio;

    if (s->answer_has_video && bundle_audio) {
        if (s->offer_audio_before_video) {
            return snprintf(buf, cap, "a=group:BUNDLE %s %s\r\n", s->audio_mid, s->video_mid);
        }
        return snprintf(buf, cap, "a=group:BUNDLE %s %s\r\n", s->video_mid, s->audio_mid);
    }
    if (s->answer_has_video) {
        return snprintf(buf, cap, "a=group:BUNDLE %s\r\n", s->video_mid);
    }
    if (s->answer_has_audio) {
        return snprintf(buf, cap, "a=group:BUNDLE %s\r\n", s->audio_mid);
    }
    return snprintf(buf, cap, "a=group:BUNDLE 0\r\n");
}

static int webrtc_append_rejected_audio_play(char *answer, size_t answer_cap, size_t n,
                                             const zms_webrtc_session *s, const char *mid)
{
    const char *fp = zms_webrtc_dtls_fingerprint();

    if (!s || !mid || !mid[0] || !fp || !fp[0]) {
        return -1;
    }
    return (int)n + snprintf(answer + n, answer_cap > n ? answer_cap - n : 0,
                             "m=audio 0 UDP/TLS/RTP/SAVPF 0\r\n"
                             "c=IN IP4 0.0.0.0\r\n"
                             "a=rtcp:9 IN IP4 0.0.0.0\r\n"
                             "a=ice-ufrag:%s\r\n"
                             "a=ice-pwd:%s\r\n"
                             "a=ice-lite\r\n"
                             "a=fingerprint:sha-256 %s\r\n"
                             "a=setup:passive\r\n"
                             "a=mid:%s\r\n"
                             "a=inactive\r\n"
                             "a=rtcp-mux\r\n",
                             s->local_ufrag, s->local_pwd, fp, mid);
}

static int webrtc_append_video_play(char *answer, size_t answer_cap, size_t n,
                                    const zms_webrtc_session *s, const char *host, const char *fp,
                                    const webrtc_neg_track *v, const char *mid)
{
    char profile[16];
    char sprop[1536];
    char fmtp_line[1792];

    switch (v->codec) {
    case ZMS_CODEC_VP8:
        return (int)n + snprintf(answer + n, answer_cap > n ? answer_cap - n : 0,
                                 "m=video %u UDP/TLS/RTP/SAVPF %u\r\n"
                                 "c=IN IP4 %s\r\n"
                                 "a=rtcp:9 IN IP4 0.0.0.0\r\n"
                                 "a=ice-ufrag:%s\r\n"
                                 "a=ice-pwd:%s\r\n"
                                 "a=ice-lite\r\n"
                                 "a=fingerprint:sha-256 %s\r\n"
                                 "a=setup:passive\r\n"
                                 "a=mid:%s\r\n"
                                 "a=sendonly\r\n"
                                 "a=rtcp-mux\r\n"
                                 "a=rtpmap:%u VP8/90000\r\n"
                                 "a=rtcp-fb:%u nack\r\n"
                                 "a=rtcp-fb:%u nack pli\r\n"
                                 "a=ssrc:1 cname:zms\r\n"
                                 "a=ssrc:1 msid:zms zmsv\r\n"
                                 "a=ssrc:1 mslabel:zms\r\n"
                                 "a=ssrc:1 label:zmsv\r\n"
                                 "a=candidate:1 1 udp 2130706431 %s %u typ host generation 0\r\n",
                                 (unsigned)s->port, (unsigned)v->pt, host, s->local_ufrag,
                                 s->local_pwd, fp, mid, (unsigned)v->pt, (unsigned)v->pt,
                                 (unsigned)v->pt, host, (unsigned)s->port);
    case ZMS_CODEC_VP9:
        return (int)n + snprintf(answer + n, answer_cap > n ? answer_cap - n : 0,
                                 "m=video %u UDP/TLS/RTP/SAVPF %u\r\n"
                                 "c=IN IP4 %s\r\n"
                                 "a=rtcp:9 IN IP4 0.0.0.0\r\n"
                                 "a=ice-ufrag:%s\r\n"
                                 "a=ice-pwd:%s\r\n"
                                 "a=ice-lite\r\n"
                                 "a=fingerprint:sha-256 %s\r\n"
                                 "a=setup:passive\r\n"
                                 "a=mid:%s\r\n"
                                 "a=sendonly\r\n"
                                 "a=rtcp-mux\r\n"
                                 "a=rtpmap:%u VP9/90000\r\n"
                                 "a=rtcp-fb:%u nack\r\n"
                                 "a=rtcp-fb:%u nack pli\r\n"
                                 "a=ssrc:1 cname:zms\r\n"
                                 "a=ssrc:1 msid:zms zmsv\r\n"
                                 "a=ssrc:1 mslabel:zms\r\n"
                                 "a=ssrc:1 label:zmsv\r\n"
                                 "a=candidate:1 1 udp 2130706431 %s %u typ host generation 0\r\n",
                                 (unsigned)s->port, (unsigned)v->pt, host, s->local_ufrag,
                                 s->local_pwd, fp, mid, (unsigned)v->pt, (unsigned)v->pt,
                                 (unsigned)v->pt, host, (unsigned)s->port);
    case ZMS_CODEC_H264:
    default: {
        int r;

        webrtc_h264_load_fmtp(s, profile, sizeof(profile), sprop, sizeof(sprop));
        if (sprop[0]) {
            snprintf(fmtp_line, sizeof(fmtp_line),
                     "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=%s;sprop-"
                     "parameter-sets=%s",
                     profile, sprop);
        } else {
            snprintf(fmtp_line, sizeof(fmtp_line),
                     "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=%s", profile);
        }
        r = webrtc_sdp_append(answer, answer_cap, n,
                              "m=video %u UDP/TLS/RTP/SAVPF %u\r\n"
                              "c=IN IP4 %s\r\n"
                              "a=rtcp:9 IN IP4 0.0.0.0\r\n"
                              "a=ice-ufrag:%s\r\n"
                              "a=ice-pwd:%s\r\n"
                              "a=ice-lite\r\n"
                              "a=fingerprint:sha-256 %s\r\n"
                              "a=setup:passive\r\n"
                              "a=mid:%s\r\n"
                              "a=sendonly\r\n"
                              "a=rtcp-mux\r\n"
                              "a=rtpmap:%u H264/90000\r\n"
                              "a=fmtp:%u %s\r\n"
                              "a=rtcp-fb:%u nack\r\n"
                              "a=rtcp-fb:%u nack pli\r\n"
                              "a=ssrc:1 cname:zms\r\n"
                              "a=ssrc:1 msid:zms zmsv\r\n"
                              "a=ssrc:1 mslabel:zms\r\n"
                              "a=ssrc:1 label:zmsv\r\n"
                              "a=candidate:1 1 udp 2130706431 %s %u typ host generation 0\r\n",
                              (unsigned)s->port, (unsigned)v->pt, host, s->local_ufrag,
                              s->local_pwd, fp, mid, (unsigned)v->pt, (unsigned)v->pt, fmtp_line,
                              (unsigned)v->pt, (unsigned)v->pt, host, (unsigned)s->port);
        if (r < 0) {
            ztk_warn("[webrtc] SDP video H264 truncated app=%s stream=%s", s->app, s->stream);
        }
        return r;
    }
    }
}

static int webrtc_append_video_publish(char *answer, size_t answer_cap, size_t n,
                                       const zms_webrtc_session *s, const char *host,
                                       const char *fp, const webrtc_neg_track *v, const char *mid)
{
    switch (v->codec) {
    case ZMS_CODEC_VP8:
        return (int)n + snprintf(answer + n, answer_cap > n ? answer_cap - n : 0,
                                 "m=video %u UDP/TLS/RTP/SAVPF %u\r\n"
                                 "c=IN IP4 %s\r\n"
                                 "a=ice-ufrag:%s\r\n"
                                 "a=ice-pwd:%s\r\n"
                                 "a=ice-lite\r\n"
                                 "a=fingerprint:sha-256 %s\r\n"
                                 "a=setup:%s\r\n"
                                 "a=mid:%s\r\n"
                                 "a=recvonly\r\n"
                                 "a=rtcp-mux\r\n"
                                 "a=rtpmap:%u VP8/90000\r\n"
                                 "a=candidate:1 1 udp 2130706431 %s %u typ host generation 0\r\n",
                                 (unsigned)s->port, (unsigned)v->pt, host, s->local_ufrag,
                                 s->local_pwd, fp, webrtc_dtls_setup_answer(s), mid,
                                 (unsigned)v->pt, host, (unsigned)s->port);
    case ZMS_CODEC_VP9:
        return (int)n + snprintf(answer + n, answer_cap > n ? answer_cap - n : 0,
                                 "m=video %u UDP/TLS/RTP/SAVPF %u\r\n"
                                 "c=IN IP4 %s\r\n"
                                 "a=ice-ufrag:%s\r\n"
                                 "a=ice-pwd:%s\r\n"
                                 "a=ice-lite\r\n"
                                 "a=fingerprint:sha-256 %s\r\n"
                                 "a=setup:%s\r\n"
                                 "a=mid:%s\r\n"
                                 "a=recvonly\r\n"
                                 "a=rtcp-mux\r\n"
                                 "a=rtpmap:%u VP9/90000\r\n"
                                 "a=candidate:1 1 udp 2130706431 %s %u typ host generation 0\r\n",
                                 (unsigned)s->port, (unsigned)v->pt, host, s->local_ufrag,
                                 s->local_pwd, fp, webrtc_dtls_setup_answer(s), mid,
                                 (unsigned)v->pt, host, (unsigned)s->port);
    case ZMS_CODEC_H264:
    default:
        return (int)n +
               snprintf(answer + n, answer_cap > n ? answer_cap - n : 0,
                        "m=video %u UDP/TLS/RTP/SAVPF %u\r\n"
                        "c=IN IP4 %s\r\n"
                        "a=ice-ufrag:%s\r\n"
                        "a=ice-pwd:%s\r\n"
                        "a=ice-lite\r\n"
                        "a=fingerprint:sha-256 %s\r\n"
                        "a=setup:%s\r\n"
                        "a=mid:%s\r\n"
                        "a=recvonly\r\n"
                        "a=rtcp-mux\r\n"
                        "a=rtpmap:%u H264/90000\r\n"
                        "a=fmtp:%u "
                        "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f\r\n"
                        "a=candidate:1 1 udp 2130706431 %s %u typ host generation 0\r\n",
                        (unsigned)s->port, (unsigned)v->pt, host, s->local_ufrag, s->local_pwd, fp,
                        webrtc_dtls_setup_answer(s), mid, (unsigned)v->pt, (unsigned)v->pt, host,
                        (unsigned)s->port);
    }
}

static int webrtc_append_audio_play(char *answer, size_t answer_cap, size_t n,
                                    const zms_webrtc_session *s, const char *host, const char *fp,
                                    const webrtc_neg_track *a, const char *asc_hex, const char *mid)
{
    (void)asc_hex;
    if (!a || a->codec != ZMS_CODEC_OPUS) {
        ztk_warn("[webrtc] refuse non-opus audio answer codec=%s",
                 a ? zms_codec_name(a->codec) : "-");
        return -1;
    }
    return (int)n + snprintf(answer + n, answer_cap > n ? answer_cap - n : 0,
                             "m=audio %u UDP/TLS/RTP/SAVPF %u\r\n"
                             "c=IN IP4 %s\r\n"
                             "a=rtcp:9 IN IP4 0.0.0.0\r\n"
                             "a=ice-ufrag:%s\r\n"
                             "a=ice-pwd:%s\r\n"
                             "a=ice-lite\r\n"
                             "a=fingerprint:sha-256 %s\r\n"
                             "a=setup:passive\r\n"
                             "a=mid:%s\r\n"
                             "a=sendonly\r\n"
                             "a=rtcp-mux\r\n"
                             "a=rtpmap:%u opus/%d/2\r\n"
                             "a=fmtp:%u minptime=10;useinbandfec=1\r\n"
                             "a=ssrc:2 cname:zms\r\n"
                             "a=candidate:1 1 udp 2130706431 %s %u typ host generation 0\r\n",
                             (unsigned)s->port, (unsigned)a->pt, host, s->local_ufrag,
                             s->local_pwd, fp, mid, (unsigned)a->pt,
                             a->rate > 0 ? a->rate : 48000, (unsigned)a->pt, host,
                             (unsigned)s->port);
}

static int webrtc_append_audio_publish(char *answer, size_t answer_cap, size_t n,
                                       const zms_webrtc_session *s, const char *host,
                                       const char *fp, const webrtc_neg_track *a, const char *mid)
{
    if (a->codec == ZMS_CODEC_OPUS) {
        return (int)n + snprintf(answer + n, answer_cap > n ? answer_cap - n : 0,
                                 "m=audio %u UDP/TLS/RTP/SAVPF %u\r\n"
                                 "c=IN IP4 %s\r\n"
                                 "a=ice-ufrag:%s\r\n"
                                 "a=ice-pwd:%s\r\n"
                                 "a=ice-lite\r\n"
                                 "a=fingerprint:sha-256 %s\r\n"
                                 "a=setup:%s\r\n"
                                 "a=mid:%s\r\n"
                                 "a=recvonly\r\n"
                                 "a=rtcp-mux\r\n"
                                 "a=rtpmap:%u opus/%d/2\r\n"
                                 "a=fmtp:%u minptime=10;useinbandfec=1\r\n"
                                 "a=candidate:1 1 udp 2130706431 %s %u typ host generation 0\r\n",
                                 (unsigned)s->port, (unsigned)a->pt, host, s->local_ufrag,
                                 s->local_pwd, fp, webrtc_dtls_setup_answer(s), mid,
                                 (unsigned)a->pt, a->rate > 0 ? a->rate : 48000, (unsigned)a->pt,
                                 host, (unsigned)s->port);
    }
    return (int)n + snprintf(answer + n, answer_cap > n ? answer_cap - n : 0,
                             "m=audio %u UDP/TLS/RTP/SAVPF %u\r\n"
                             "c=IN IP4 %s\r\n"
                             "a=ice-ufrag:%s\r\n"
                             "a=ice-pwd:%s\r\n"
                             "a=ice-lite\r\n"
                             "a=fingerprint:sha-256 %s\r\n"
                             "a=setup:%s\r\n"
                             "a=mid:%s\r\n"
                             "a=recvonly\r\n"
                             "a=rtcp-mux\r\n"
                             "a=rtpmap:%u mpeg4-generic/48000/2\r\n"
                             "a=fmtp:%u "
                             "streamtype=5;profile-level-id=1;mode=AAC-hbr;sizelength=13;"
                             "indexlength=3;indexdeltalength=3\r\n"
                             "a=candidate:1 1 udp 2130706431 %s %u typ host generation 0\r\n",
                             (unsigned)s->port, (unsigned)a->pt, host, s->local_ufrag, s->local_pwd,
                             fp, webrtc_dtls_setup_answer(s), mid, (unsigned)a->pt, (unsigned)a->pt,
                             host, (unsigned)s->port);
}

static int webrtc_append_play_media(char *answer, size_t answer_cap, size_t n,
                                    const zms_webrtc_session *s, const char *host, const char *fp,
                                    const webrtc_neg_track *v, const webrtc_neg_track *a,
                                    const char *asc_hex)
{
    int r;

    if (s->offer_audio_before_video) {
        if (s->answer_has_audio) {
            r = webrtc_append_audio_play(answer, answer_cap, n, s, host, fp, a, asc_hex,
                                         s->audio_mid);
            if (r < 0 || (size_t)r >= answer_cap) {
                return -1;
            }
            n = (size_t)r;
        } else if (s->offer_has_audio) {
            r = webrtc_append_rejected_audio_play(answer, answer_cap, n, s, s->audio_mid);
            if (r < 0 || (size_t)r >= answer_cap) {
                return -1;
            }
            n = (size_t)r;
        }
        if (s->answer_has_video) {
            r = webrtc_append_video_play(answer, answer_cap, n, s, host, fp, v, s->video_mid);
            if (r < 0 || (size_t)r >= answer_cap) {
                return -1;
            }
            n = (size_t)r;
        }
    } else {
        if (s->answer_has_video) {
            r = webrtc_append_video_play(answer, answer_cap, n, s, host, fp, v, s->video_mid);
            if (r < 0 || (size_t)r >= answer_cap) {
                ztk_warn("[webrtc] SDP play video section failed app=%s stream=%s r=%d cap=%zu",
                         s->app, s->stream, r, answer_cap);
                return -1;
            }
            n = (size_t)r;
            ztk_info("[webrtc] SDP play video ok off=%zu app=%s stream=%s", n, s->app, s->stream);
        }
        if (s->answer_has_audio) {
            r = webrtc_append_audio_play(answer, answer_cap, n, s, host, fp, a, asc_hex,
                                         s->audio_mid);
            if (r < 0 || (size_t)r >= answer_cap) {
                ztk_warn(
                    "[webrtc] SDP play audio section failed app=%s stream=%s r=%d cap=%zu asc=%s",
                    s->app, s->stream, r, answer_cap, asc_hex[0] ? asc_hex : "-");
                return -1;
            }
            n = (size_t)r;
            ztk_info("[webrtc] SDP play audio ok off=%zu app=%s stream=%s", n, s->app, s->stream);
        } else if (s->offer_has_audio) {
            r = webrtc_append_rejected_audio_play(answer, answer_cap, n, s, s->audio_mid);
            if (r < 0 || (size_t)r >= answer_cap) {
                return -1;
            }
            n = (size_t)r;
        }
    }
    return (int)n;
}

static int webrtc_append_publish_media(char *answer, size_t answer_cap, size_t n,
                                       const zms_webrtc_session *s, const char *host,
                                       const char *fp, const webrtc_neg_track *v,
                                       const webrtc_neg_track *a)
{
    if (s->offer_audio_before_video) {
        if (s->answer_has_audio) {
            n = (size_t)webrtc_append_audio_publish(answer, answer_cap, n, s, host, fp, a,
                                                    s->audio_mid);
        }
        if (s->answer_has_video) {
            n = (size_t)webrtc_append_video_publish(answer, answer_cap, n, s, host, fp, v,
                                                    s->video_mid);
        }
    } else {
        if (s->answer_has_video) {
            n = (size_t)webrtc_append_video_publish(answer, answer_cap, n, s, host, fp, v,
                                                    s->video_mid);
        }
        if (s->answer_has_audio) {
            n = (size_t)webrtc_append_audio_publish(answer, answer_cap, n, s, host, fp, a,
                                                    s->audio_mid);
        }
    }
    return (int)n;
}

int zms_webrtc_session_build_answer(const zms_webrtc_session *s, const char *offer,
                                    size_t offer_len, char *answer, size_t answer_cap,
                                    size_t *answer_len)
{
    sdp_t *sdp;
    int n;
    const char *host;
    const char *fp;
    char bundle_line[64];
    char asc_hex[128];
    webrtc_neg_track v, a;
    zms_webrtc_session *mut = (zms_webrtc_session *)s;

    if (!s || !offer || offer_len == 0 || !answer || answer_cap == 0) {
        return -1;
    }
    host = zms_webrtc_service_advertise_host(zms_webrtc_service_instance());
    fp = zms_webrtc_dtls_fingerprint();
    sdp = sdp_parse(offer, (int)offer_len);
    if (!sdp) {
        return -1;
    }
    if (webrtc_parse_remote_ice(sdp, mut->remote_ufrag, sizeof(mut->remote_ufrag), mut->remote_pwd,
                                sizeof(mut->remote_pwd)) != 0) {
        sdp_destroy(sdp);
        return -1;
    }
    webrtc_rand_credential(mut->local_ufrag, sizeof(mut->local_ufrag), 8);
    webrtc_rand_credential(mut->local_pwd, sizeof(mut->local_pwd), 22);
    if (webrtc_session_negotiate(mut, sdp, 1) != 0) {
        ztk_warn("[webrtc] WHEP codec negotiate failed app=%s stream=%s", s->app, s->stream);
        sdp_destroy(sdp);
        return -1;
    }
    sdp_destroy(sdp);
    asc_hex[0] = '\0';
    /* play 仅 Opus；AAC 已在 negotiate 阶段过滤，无需再取 ASC */

    v.codec = mut->video_codec;
    v.pt = mut->video_pt;
    a.codec = mut->audio_codec;
    a.pt = mut->audio_pt;
    a.rate = mut->audio_rate;
    a.channels = mut->audio_channels;

    webrtc_snprintf_bundle(bundle_line, sizeof(bundle_line), mut);
    n = snprintf(answer, answer_cap,
                 "v=0\r\n"
                 "o=- %llu 2 IN IP4 %s\r\n"
                 "s=-\r\n"
                 "t=0 0\r\n"
                 "%s"
                 "a=msid-semantic: WMS *\r\n",
                 (unsigned long long)time(NULL), host, bundle_line);
    n = webrtc_append_play_media(answer, answer_cap, (size_t)n, s, host, fp, &v, &a, asc_hex);
    if (n <= 0 || (size_t)n >= answer_cap) {
        ztk_warn("[webrtc] answer too large app=%s stream=%s len=%d cap=%zu", s->app, s->stream, n,
                 answer_cap);
        return -1;
    }
    ztk_info("[webrtc] WHEP SDP body ready len=%d video=%d audio=%d app=%s stream=%s", n,
             mut->answer_has_video, mut->answer_has_audio, s->app, s->stream);
    if (mut->ice) {
        if (!mut->remote_ufrag[0] || !mut->remote_pwd[0] || !mut->local_ufrag[0] ||
            !mut->local_pwd[0] || !host || !host[0]) {
            ztk_warn("[webrtc] ICE setup skipped: missing auth/host app=%s stream=%s", s->app,
                     s->stream);
            return -1;
        }
        if (zms_webrtc_ice_setup(mut->ice, offer, offer_len, mut->local_ufrag, mut->local_pwd,
                                 mut->remote_ufrag, mut->remote_pwd, host, s->port) != 0) {
            ztk_warn("[webrtc] ICE setup failed app=%s stream=%s", s->app, s->stream);
            return -1;
        }
    }
    if (answer_len) {
        *answer_len = (size_t)n;
    }
    ztk_info("[webrtc] WHEP answer video=%s audio=%s app=%s stream=%s",
             zms_codec_name(mut->video_codec),
             mut->answer_has_audio ? zms_codec_name(mut->audio_codec) : "none", s->app, s->stream);
    return 0;
}

int zms_webrtc_session_build_publish_answer(const zms_webrtc_session *s, const char *offer,
                                            size_t offer_len, char *answer, size_t answer_cap,
                                            size_t *answer_len)
{
    sdp_t *sdp;
    int n;
    const char *host;
    const char *fp;
    char bundle_line[64];
    webrtc_neg_track v, a;
    zms_webrtc_session *mut = (zms_webrtc_session *)s;

    if (!s || !offer || offer_len == 0 || !answer || answer_cap == 0) {
        return -1;
    }
    host = zms_webrtc_service_advertise_host(zms_webrtc_service_instance());
    fp = zms_webrtc_dtls_fingerprint();
    sdp = sdp_parse(offer, (int)offer_len);
    if (!sdp) {
        return -1;
    }
    if (webrtc_parse_remote_ice(sdp, mut->remote_ufrag, sizeof(mut->remote_ufrag), mut->remote_pwd,
                                sizeof(mut->remote_pwd)) != 0) {
        sdp_destroy(sdp);
        return -1;
    }
    webrtc_rand_credential(mut->local_ufrag, sizeof(mut->local_ufrag), 8);
    webrtc_rand_credential(mut->local_pwd, sizeof(mut->local_pwd), 22);
    if (webrtc_session_negotiate(mut, sdp, 0) != 0) {
        ztk_warn("[webrtc] WHIP codec negotiate failed app=%s stream=%s", s->app, s->stream);
        sdp_destroy(sdp);
        return -1;
    }
    mut->dtls_as_client = webrtc_offer_setup_passive(sdp) ? 1 : 0;
    (void)webrtc_publish_store_offer_h264_sprop(mut, sdp);
    sdp_destroy(sdp);

    v.codec = mut->video_codec;
    v.pt = mut->video_pt;
    a.codec = mut->audio_codec;
    a.pt = mut->audio_pt;
    a.rate = mut->audio_rate;
    a.channels = mut->audio_channels;

    webrtc_snprintf_bundle(bundle_line, sizeof(bundle_line), mut);
    n = snprintf(answer, answer_cap,
                 "v=0\r\n"
                 "o=- %llu 2 IN IP4 %s\r\n"
                 "s=-\r\n"
                 "t=0 0\r\n"
                 "%s"
                 "a=msid-semantic: WMS *\r\n",
                 (unsigned long long)time(NULL), host, bundle_line);
    n = webrtc_append_publish_media(answer, answer_cap, (size_t)n, s, host, fp, &v, &a);
    if (n <= 0 || (size_t)n >= answer_cap) {
        return -1;
    }
    if (mut->ice &&
        zms_webrtc_ice_setup(mut->ice, offer, offer_len, mut->local_ufrag, mut->local_pwd,
                             mut->remote_ufrag, mut->remote_pwd, host, s->port) != 0) {
        return -1;
    }
    if (answer_len) {
        *answer_len = (size_t)n;
    }
    ztk_info("[webrtc] WHIP answer video=%s audio=%s dtls=%s app=%s stream=%s",
             zms_codec_name(mut->video_codec), zms_codec_name(mut->audio_codec),
             mut->dtls_as_client ? "client" : "server", s->app, s->stream);
    return 0;
}
