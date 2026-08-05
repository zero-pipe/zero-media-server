#include "zms/media/codec/g711/g711_over_rtp.h"
#include "zms/session/rtsp/rtsp_sdp.h"
#include "sdp.h"
#include "sdp-a-rtpmap.h"
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static zms_codec_id codec_from_encoding(const char *encoding)
{
    if (!encoding || !encoding[0]) {
        return ZMS_CODEC_INVALID;
    }
    char lower[32];
    size_t i = 0;
    for (; encoding[i] && i + 1 < sizeof(lower); ++i) {
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
    if (strstr(lower, "h266") || strstr(lower, "vvc") || strstr(lower, "hev2")) {
        return ZMS_CODEC_H266;
    }
    if (strstr(lower, "mpeg4-generic") || strstr(lower, "aac") || strstr(lower, "mp4a")) {
        return ZMS_CODEC_AAC;
    }
    if (strstr(lower, "opus")) {
        return ZMS_CODEC_OPUS;
    }
    if (strstr(lower, "pcma") || strstr(lower, "g711a") || strstr(lower, "alaw")) {
        return ZMS_CODEC_G711A;
    }
    if (strstr(lower, "pcmu") || strstr(lower, "g711u") || strstr(lower, "ulaw") ||
        strstr(lower, "mu-law")) {
        return ZMS_CODEC_G711U;
    }
    return ZMS_CODEC_INVALID;
}

static zms_track_type track_from_media_type(const char *media)
{
    if (!media) {
        return ZMS_TRACK_INVALID;
    }
    if (strcmp(media, "video") == 0) {
        return ZMS_TRACK_VIDEO;
    }
    if (strcmp(media, "audio") == 0) {
        return ZMS_TRACK_AUDIO;
    }
    return ZMS_TRACK_INVALID;
}

typedef struct {
    zms_media_track *track;
    int found;
} rtpmap_ctx;

static void on_media_rtpmap(void *param, const char *name, const char *value)
{
    rtpmap_ctx *ctx = (rtpmap_ctx *)param;
    if (!ctx || !ctx->track || !name || !value || strcmp(name, "rtpmap") != 0) {
        return;
    }

    int pt = 0, rate = 90000;
    char enc[32];
    char ch[64];
    enc[0] = ch[0] = '\0';
    if (sdp_a_rtpmap(value, &pt, enc, &rate, ch) != 0) {
        return;
    }

    zms_codec_id codec = codec_from_encoding(enc);
    if (codec == ZMS_CODEC_INVALID) {
        return;
    }

    if (ctx->found && ctx->track->codec != ZMS_CODEC_INVALID && codec != ctx->track->codec) {
        return;
    }

    ctx->track->payload_type = pt;
    ctx->track->sample_rate = rate > 0 ? rate : 90000;
    ctx->track->codec = codec;
    if (ch[0]) {
        int c = atoi(ch);
        if (c > 0) {
            ctx->track->channels = c;
        }
    }
    ctx->found = 1;
}

static void fill_media_track(zms_media_track *cur, sdp_t *s, int media_idx)
{
    const char *mtype = sdp_media_type(s, media_idx);
    cur->type = track_from_media_type(mtype);
    cur->sample_rate = (cur->type == ZMS_TRACK_AUDIO) ? 44100 : 90000;
    cur->channels = (cur->type == ZMS_TRACK_AUDIO) ? 2 : 1;

    int fmts[16];
    int nfmt = sdp_media_formats(s, media_idx, fmts, (int)(sizeof(fmts) / sizeof(fmts[0])));
    if (nfmt > 0) {
        cur->payload_type = fmts[0];
    }

    rtpmap_ctx ctx = {.track = cur, .found = 0};
    sdp_media_attribute_list(s, media_idx, "rtpmap", on_media_rtpmap, &ctx);

    const char *ctrl = sdp_media_attribute_find(s, media_idx, "control");
    if (ctrl && ctrl[0]) {
        strncpy(cur->control, ctrl, sizeof(cur->control) - 1);
    }

    const char *fmtp = sdp_media_attribute_find(s, media_idx, "fmtp");
    if (fmtp && fmtp[0]) {
        const char *p = strchr(fmtp, ' ');
        if (p) {
            strncpy(cur->fmtp, p + 1, sizeof(cur->fmtp) - 1);
        } else {
            strncpy(cur->fmtp, fmtp, sizeof(cur->fmtp) - 1);
        }
    }

    /* RFC 3551 静态 payload（PCMU=0、PCMA=8）在 publisher SDP 中常省略 a=rtpmap。 */
    if (cur->codec == ZMS_CODEC_INVALID && nfmt > 0) {
        zms_codec_id g711 = zms_g711_codec_from_rtp_pt((uint8_t)cur->payload_type);
        if (g711 != ZMS_CODEC_INVALID) {
            cur->codec = g711;
            cur->sample_rate = 8000;
            cur->channels = 1;
        }
    }
}

ztk_err_t zms_sdp_parse(const char *sdp, size_t len, zms_sdp_session *out)
{
    if (!sdp || !out || len == 0) {
        return ZTK_ERR_INVALID;
    }

    memset(out, 0, sizeof(*out));
    sdp_t *s = sdp_parse(sdp, (int)len);
    if (!s) {
        return ZTK_ERR_INVALID;
    }

    const char *sess_ctrl = sdp_attribute_find(s, "control");
    if (sess_ctrl && sess_ctrl[0]) {
        strncpy(out->session_control, sess_ctrl, sizeof(out->session_control) - 1);
    }

    int nm = sdp_media_count(s);
    for (int i = 0; i < nm && out->track_count < ZMS_SDP_TRACK_MAX; ++i) {
        zms_media_track *cur = &out->tracks[out->track_count];
        memset(cur, 0, sizeof(*cur));
        fill_media_track(cur, s, i);
        if (cur->type == ZMS_TRACK_INVALID || cur->codec == ZMS_CODEC_INVALID) {
            continue;
        }
        out->track_count++;
    }

    sdp_destroy(s);

    if (out->track_count == 0) {
        return ZTK_ERR_INVALID;
    }

    for (unsigned i = 0; i < out->track_count; ++i) {
        zms_media_track *t = &out->tracks[i];
        if (t->control[0] == '\0') {
            snprintf(t->control, sizeof(t->control), "track%u", i + 1);
        }
        t->interleaved_rtp = (uint8_t)(i * 2);
        t->interleaved_rtcp = (uint8_t)(i * 2 + 1);
        t->ready = 1;
    }

    return ZTK_OK;
}

const zms_media_track *zms_sdp_track_at(const zms_sdp_session *s, unsigned index)
{
    if (!s || index >= s->track_count) {
        return NULL;
    }
    return &s->tracks[index];
}
