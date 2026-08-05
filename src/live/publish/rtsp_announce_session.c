#include "session/rtsp/rtsp_session_internal.h"
#include "zms/engine/gop/gop_queue.h"
#include "zms/session/rtsp/rtsp_sdp.h"
#include "ztk/util/log.h"
#include <stdio.h>
#include <string.h>

static void trim_fmtp_token(char *s)
{
    if (!s) {
        return;
    }
    for (char *p = s; *p; ++p) {
        if (*p == ';' || *p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
            *p = '\0';
            break;
        }
    }
}

static const char k_b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int base64_val(char c)
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

static size_t base64_decode(const char *in, uint8_t *out, size_t cap)
{
    size_t o = 0;
    int val[4];
    while (*in) {
        int n = 0;
        for (; n < 4 && *in; ++in) {
            if (*in == '=' || *in == ' ' || *in == '\r' || *in == '\n' || *in == '\t') {
                continue;
            }
            val[n] = base64_val(*in);
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

static void apply_h264_sprop(zms_rtsp_session *rs, const char *fmtp)
{
    if (!rs || !rs->ingress || !fmtp) {
        return;
    }
    const char *sprop = strstr(fmtp, "sprop-parameter-sets=");
    if (!sprop) {
        return;
    }
    sprop += 21;
    const char *comma = strchr(sprop, ',');
    if (!comma) {
        return;
    }

    char sps_b64[384], pps_b64[384];
    size_t slen = (size_t)(comma - sprop);
    if (slen >= sizeof(sps_b64)) {
        slen = sizeof(sps_b64) - 1;
    }
    memcpy(sps_b64, sprop, slen);
    sps_b64[slen] = '\0';
    trim_fmtp_token(sps_b64);
    strncpy(pps_b64, comma + 1, sizeof(pps_b64) - 1);
    pps_b64[sizeof(pps_b64) - 1] = '\0';
    trim_fmtp_token(pps_b64);

    uint8_t sps[256], pps[256];
    size_t sps_len = base64_decode(sps_b64, sps, sizeof(sps));
    size_t pps_len = base64_decode(pps_b64, pps, sizeof(pps));
    if (!sps_len || !pps_len) {
        return;
    }

    (void)zms_live_ingest_set_h264_sps_pps(rs->ingress, sps, sps_len, pps, pps_len);
    ztk_info("RTSP ANNOUNCE: applied H264 sprop SPS=%u PPS=%u", (unsigned)sps_len,
             (unsigned)pps_len);
}

static int fmtp_extract_b64(const char *fmtp, const char *key, char *out, size_t cap)
{
    char needle[32];
    const char *p;
    const char *end;
    size_t n;

    if (!fmtp || !key || !out || cap == 0) {
        return 0;
    }
    snprintf(needle, sizeof(needle), "%s=", key);
    p = strstr(fmtp, needle);
    if (!p) {
        return 0;
    }
    p += strlen(needle);
    end = p;
    while (*end && *end != ';' && *end != ' ' && *end != '\r' && *end != '\n') {
        ++end;
    }
    n = (size_t)(end - p);
    if (n == 0 || n >= cap) {
        return 0;
    }
    memcpy(out, p, n);
    out[n] = '\0';
    trim_fmtp_token(out);
    return out[0] != '\0';
}

static void apply_h265_sprop(zms_rtsp_session *rs, const char *fmtp)
{
    char vps_b64[512], sps_b64[512], pps_b64[512];
    uint8_t vps[512], sps[512], pps[512];
    size_t vps_len = 0, sps_len = 0, pps_len = 0;

    if (!rs || !rs->ingress || !fmtp) {
        return;
    }
    vps_b64[0] = sps_b64[0] = pps_b64[0] = '\0';
    (void)fmtp_extract_b64(fmtp, "sprop-vps", vps_b64, sizeof(vps_b64));
    if (!fmtp_extract_b64(fmtp, "sprop-sps", sps_b64, sizeof(sps_b64))) {
        return;
    }
    if (!fmtp_extract_b64(fmtp, "sprop-pps", pps_b64, sizeof(pps_b64))) {
        return;
    }
    if (vps_b64[0]) {
        vps_len = base64_decode(vps_b64, vps, sizeof(vps));
    }
    sps_len = base64_decode(sps_b64, sps, sizeof(sps));
    pps_len = base64_decode(pps_b64, pps, sizeof(pps));
    if (!sps_len || !pps_len) {
        return;
    }
    if (zms_live_ingest_set_h265_vps_sps_pps(rs->ingress, vps_len ? vps : NULL, vps_len, sps,
                                             sps_len, pps, pps_len) == ZTK_OK) {
        ztk_info("RTSP ANNOUNCE: applied H265 sprop VPS=%u SPS=%u PPS=%u", (unsigned)vps_len,
                 (unsigned)sps_len, (unsigned)pps_len);
    }
}

static void apply_announce_sdp(zms_rtsp_session *rs, const char *sdp, size_t sdp_len)
{
    if (!rs) {
        return;
    }
    memset(&rs->publish_sdp, 0, sizeof(rs->publish_sdp));
    memset(rs->publish_track_setup, 0, sizeof(rs->publish_track_setup));
    rs->video_clock_hz = 90000;
    rs->audio_clock_hz = 44100;
    if (!sdp || !sdp_len || zms_sdp_parse(sdp, sdp_len, &rs->publish_sdp) != ZTK_OK) {
        ztk_warn("RTSP ANNOUNCE: SDP parse failed app=%s stream=%s", rs->app, rs->stream);
        return;
    }

    for (unsigned i = 0; i < rs->publish_sdp.track_count; ++i) {
        const zms_media_track *t = &rs->publish_sdp.tracks[i];
        if (t->type == ZMS_TRACK_VIDEO) {
            if (rs->source) {
                rs->source->has_video = 1;
                rs->source->video.codec = t->codec;
                rs->source->video.ready = 1;
            }
            rs->video_clock_hz = (uint32_t)(t->sample_rate > 0 ? t->sample_rate : 90000);
            if (t->codec != ZMS_CODEC_H264 && t->codec != ZMS_CODEC_H265 &&
                t->codec != ZMS_CODEC_AV1 && t->codec != ZMS_CODEC_VP8 &&
                t->codec != ZMS_CODEC_VP9 && t->codec != ZMS_CODEC_H266) {
                ztk_warn("RTSP ANNOUNCE: unsupported video codec=%d", (int)t->codec);
                continue;
            }
            if (t->fmtp[0]) {
                if (t->codec == ZMS_CODEC_H265) {
                    apply_h265_sprop(rs, t->fmtp);
                } else if (t->codec == ZMS_CODEC_H264) {
                    apply_h264_sprop(rs, t->fmtp);
                }
            }
            if (t->interleaved_rtp == 0) {
                rs->video_rtp_ch = (uint8_t)(i * 2);
            }
        } else if (t->type == ZMS_TRACK_AUDIO) {
            if (rs->source) {
                rs->source->has_audio = 1;
            }
            rs->audio_rate = t->sample_rate > 0 ? t->sample_rate : 44100;
            rs->audio_channels = t->channels > 0 ? t->channels : 2;
            if (t->codec == ZMS_CODEC_G711A || t->codec == ZMS_CODEC_G711U) {
                rs->audio_codec = t->codec;
                rs->audio_rate = t->sample_rate > 0 ? t->sample_rate : 8000;
                rs->audio_channels = t->channels > 0 ? t->channels : 1;
                rs->audio_clock_hz = (uint32_t)rs->audio_rate;
            } else if (t->codec == ZMS_CODEC_OPUS) {
                rs->audio_codec = ZMS_CODEC_OPUS;
                rs->audio_rate = t->sample_rate > 0 ? t->sample_rate : 48000;
                rs->audio_clock_hz = (uint32_t)rs->audio_rate;
            } else if (t->codec != ZMS_CODEC_AAC) {
                ztk_warn("RTSP ANNOUNCE: unsupported audio codec=%d", (int)t->codec);
                continue;
            } else {
                rs->audio_codec = ZMS_CODEC_AAC;
                rs->audio_clock_hz = (uint32_t)(t->sample_rate > 0 ? t->sample_rate : 44100);
            }
            if (t->codec == ZMS_CODEC_AAC && (t->type == ZMS_TRACK_AUDIO && t->fmtp[0])) {
                const char *cfg = strstr(t->fmtp, "config=");
                if (!cfg) {
                    cfg = strstr(t->fmtp, "Config=");
                }
                if (cfg && rs->ingress) {
                    char hex[128];
                    strncpy(hex, cfg + 7, sizeof(hex) - 1);
                    hex[sizeof(hex) - 1] = '\0';
                    trim_fmtp_token(hex);
                    if (hex[0]) {
                        (void)zms_live_ingest_set_aac_config_hex(rs->ingress, hex);
                    }
                }
            }
            if (rs->source && rs->audio_codec != ZMS_CODEC_INVALID) {
                rs->source->audio.codec = rs->audio_codec;
                rs->source->audio.sample_rate = rs->audio_rate;
                rs->source->audio.channels = rs->audio_channels;
                rs->source->audio.ready = 1;
            }
            if (t->interleaved_rtp == 0 && i > 0) {
                rs->audio_rtp_ch = (uint8_t)(i * 2);
            } else if (i == 1) {
                rs->audio_rtp_ch = 2;
            }
        }
        ztk_info("RTSP ANNOUNCE track[%u]: type=%d codec=%d pt=%d rate=%d control=%s", i,
                 (int)t->type, (int)t->codec, t->payload_type, t->sample_rate, t->control);
    }
    if (rs->ingress) {
        zms_live_ingest_set_rtp_clocks(rs->ingress, rs->video_clock_hz, rs->audio_codec,
                                       rs->audio_clock_hz);
    }
    if (rs->ingress && rs->source && rs->source->has_audio && rs->audio_codec == ZMS_CODEC_AAC) {
        (void)zms_live_ingest_ensure_aac_config(rs->ingress, rs->audio_rate, rs->audio_channels);
    }
}

void zms_rtsp_session_handle_announce(zms_rtsp_session *rs, const zms_rtsp_message *msg)
{
    if (!rs || !msg) {
        return;
    }
    zms_rtsp_session_parse_url(msg->url, rs->app, rs->stream);
    if (!rs->app[0] || !rs->stream[0]) {
        zms_rtsp_session_send_resp(rs, 403, "Forbidden", "Content-Type: text/plain\r\n",
                                   "invalid rtsp push url", 20);
        return;
    }
    char client_stream[ZMS_STREAM_MAX];
    strncpy(client_stream, rs->stream, sizeof(client_stream) - 1);
    rs->ingress = zms_live_ingest_create_publish(rs->app, rs->stream, NULL);
    if (rs->ingress) {
        zms_live_ingest_set_poller(rs->ingress, rs->poller);
    }
    rs->source = rs->ingress ? zms_live_ingest_source(rs->ingress) : NULL;
    if (!rs->source || !rs->source->gop_queue) {
        zms_live_ingest_destroy(rs->ingress);
        rs->ingress = NULL;
        zms_rtsp_session_send_resp(rs, 500, "Internal Server Error", NULL, NULL, 0);
        return;
    }
    if (msg->body && msg->body_len) {
        apply_announce_sdp(rs, msg->body, msg->body_len);
    }
    if (rs->publish_sdp.track_count == 0) {
        zms_live_ingest_destroy(rs->ingress);
        rs->ingress = NULL;
        rs->source = NULL;
        zms_rtsp_session_send_resp(rs, 403, "Forbidden", "Content-Type: text/plain\r\n",
                                   "no valid track in sdp", 21);
        return;
    }
    strncpy(rs->content_base, msg->url, sizeof(rs->content_base) - 1);
    if (rs->source) {
        strncpy(rs->stream, rs->source->stream, sizeof(rs->stream) - 1);
    }
    ztk_info("RTSP ANNOUNCE 200: app=%s client_stream=%s stream_id=%s tracks=%u", rs->app,
             client_stream, rs->stream, rs->publish_sdp.track_count);
    zms_rtsp_session_send_resp(rs, 200, "OK", NULL, NULL, 0);
}
