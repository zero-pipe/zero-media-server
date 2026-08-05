/**
 * @file rtp_muxer.c
 * @brief RTP 出站：gop_queue 槽位经 librtsp rtsp_muxer 打成 RTP。
 *
 * Play-clock / epoch 在 ZMS；打包走 media-server rtsp_muxer + librtp。
 */
#include "zms/egress/rtp/rtp_muxer.h"

#include "ztk/ztk_errno.h"

#include "zms/engine/frame.h"

#include "zms/media/container/flv/flv_tag_probe.h"
#include "zms/media/codec/aac/aac_over_rtp.h"
#include "zms/media/codec/h264/h264_over_rtmp.h"
#include "zms/media/codec/h264/h264_es.h"
#include "zms/media/codec/h265/h265_es.h"
#include "zms/media/codec/av1/av1_over_rtmp.h"

#include "rtsp-muxer.h"
#include "rtp-profile.h"
#include "mov-format.h"

#include <stdlib.h>
#include <string.h>

#include "egress/rtp/rtp_muxer_internal.h"

static uint32_t mux_video_clock_hz(const zms_rtp_muxer *m);
static uint32_t mux_audio_clock_hz(const zms_rtp_muxer *m);

static void mux_emit(zms_rtp_muxer *m, zms_rtp_mux_track track, const uint8_t *rtp, size_t len)

{
    if (!m || !rtp || len < 8) {
        return;
    }

    uint32_t rtp_ts =
        ((uint32_t)rtp[4] << 24) | ((uint32_t)rtp[5] << 16) | ((uint32_t)rtp[6] << 8) | rtp[7];

    if (track == ZMS_RTP_MUX_TRACK_VIDEO) {
        m->stats.video_last_rtp_ts = rtp_ts;

        m->stats.video_pkt_count++;

        m->stats.video_octet_count += (uint32_t)len;
        m->v_seq = (uint16_t)(((uint16_t)rtp[2] << 8) | rtp[3]) + 1u;

    } else {
        m->stats.audio_last_rtp_ts = rtp_ts;

        m->stats.audio_pkt_count++;

        m->stats.audio_octet_count += (uint32_t)len;
        m->a_seq = (uint16_t)(((uint16_t)rtp[2] << 8) | rtp[3]) + 1u;
    }

    if (m->on_rtp) {
        m->on_rtp(track, rtp, len, m->user);
    }
}

static int zms_rtsp_onpacket(void *param, int pid, const void *data, int bytes, uint32_t timestamp,
                             int flags)
{
    zms_rtp_muxer *m = (zms_rtp_muxer *)param;
    zms_rtp_mux_track trk;

    (void)timestamp;
    (void)flags;
    if (!m || !data || bytes <= 0) {
        return 0;
    }
    if (pid == m->v_pid) {
        trk = ZMS_RTP_MUX_TRACK_VIDEO;
    } else if (pid == m->a_pid) {
        trk = ZMS_RTP_MUX_TRACK_AUDIO;
    } else {
        return 0;
    }
    mux_emit(m, trk, (const uint8_t *)data, (size_t)bytes);
    return 0;
}

static void zms_rtsp_drop_video(zms_rtp_muxer *m)
{
    if (!m) {
        return;
    }
    m->v_pid = -1;
    m->v_mid = -1;
    m->video_codec = ZMS_CODEC_INVALID;
}

/** recreate 前快照 librtsp seq，保持 seq 单调递增。 */
static void zms_rtsp_snapshot_seq(zms_rtp_muxer *m)
{
    uint16_t seq;
    uint32_t ts;
    const char *sdp;
    int sdp_len;

    if (!m || !m->zms) {
        return;
    }
    if (m->v_pid >= 0 && rtsp_muxer_getinfo(m->zms, m->v_pid, &seq, &ts, &sdp, &sdp_len) == 0) {
        m->v_seq = seq;
    }
    if (m->a_pid >= 0 && rtsp_muxer_getinfo(m->zms, m->a_pid, &seq, &ts, &sdp, &sdp_len) == 0) {
        m->a_seq = seq;
    }
}

static void zms_rtsp_recreate(zms_rtp_muxer *m)
{
    if (!m) {
        return;
    }
    if (m->zms) {
        rtsp_muxer_destroy(m->zms);
    }
    m->zms = rtsp_muxer_create(zms_rtsp_onpacket, m);
    m->v_pid = m->a_pid = m->v_mid = m->a_mid = -1;
    m->video_codec = ZMS_CODEC_INVALID;
}

static int zms_rtsp_codec_supported(zms_codec_id codec, zms_rtp_mux_track trk)
{
    if (trk == ZMS_RTP_MUX_TRACK_VIDEO) {
        return codec == ZMS_CODEC_H264 || codec == ZMS_CODEC_H265 || codec == ZMS_CODEC_AV1 ||
               codec == ZMS_CODEC_VP8 || codec == ZMS_CODEC_VP9 || codec == ZMS_CODEC_H266;
    }
    return codec == ZMS_CODEC_AAC || codec == ZMS_CODEC_G711A || codec == ZMS_CODEC_G711U ||
           codec == ZMS_CODEC_OPUS;
}

static int zms_ensure_video(zms_rtp_muxer *m, zms_codec_id codec)
{
    const char *enc;
    int payload;

    if (!m || !m->zms) {
        return -1;
    }
    if (m->v_mid >= 0 && m->video_codec == codec) {
        return 0;
    }
    if (m->v_mid >= 0) {
        zms_rtsp_recreate(m);
    }
    if (codec == ZMS_CODEC_H264) {
        enc = "H264";
        payload = RTP_PAYLOAD_H264;
    } else if (codec == ZMS_CODEC_H265) {
        enc = "HEVC";
        payload = RTP_PAYLOAD_H265;
    } else if (codec == ZMS_CODEC_AV1) {
        enc = "AV1";
        payload = RTP_PAYLOAD_AV1;
    } else if (codec == ZMS_CODEC_VP8) {
        enc = "VP8";
        payload = RTP_PAYLOAD_VP8;
    } else if (codec == ZMS_CODEC_VP9) {
        enc = "VP9";
        payload = RTP_PAYLOAD_VP9;
    } else if (codec == ZMS_CODEC_H266) {
        enc = "H266";
        payload = RTP_PAYLOAD_H266;
    } else {
        return -1;
    }
    {
        const void *vextra = NULL;
        int vextra_size = 0;

        if (codec == ZMS_CODEC_H264) {
            if (m->h264_avcc_len == 0) {
                (void)mux_h264_build_avcc_fallback(m);
            }
            if (m->h264_avcc_len == 0 || !m->h264_avcc) {
                return -1;
            }
            vextra = m->h264_avcc;
            vextra_size = (int)m->h264_avcc_len;
        } else if (codec == ZMS_CODEC_H265) {
            if (m->hevc_hvcc_len == 0) {
                (void)mux_hevc_build_hvcc_fallback(m);
            }
            if (m->hevc_hvcc_len == 0 || !m->hevc_hvcc) {
                return -1;
            }
            vextra = m->hevc_hvcc;
            vextra_size = (int)m->hevc_hvcc_len;
        } else if (codec == ZMS_CODEC_AV1) {
            if (m->av1_av1c_len == 0 || !m->av1_av1c) {
                return -1;
            }
            vextra = m->av1_av1c;
            vextra_size = (int)m->av1_av1c_len;
        }
        m->v_pid = rtsp_muxer_add_payload(m->zms, "RTP/AVP", (int)mux_video_clock_hz(m),
                                          m->opts.video_pt ? m->opts.video_pt : 96, enc, m->v_seq,
                                          m->opts.video_ssrc, 0, vextra, vextra_size);
    }
    if (m->v_pid < 0) {
        return -1;
    }
    m->v_mid = rtsp_muxer_add_media(m->zms, m->v_pid, payload, NULL, 0);
    if (m->v_mid < 0) {
        return -1;
    }
    m->video_codec = codec;
    return 0;
}

static int zms_ensure_audio(zms_rtp_muxer *m)
{
    const char *enc = "mpeg4-generic";
    int payload = RTP_PAYLOAD_MP4A;
    int rate = m->opts.audio_rate > 0 ? m->opts.audio_rate : 44100;

    if (!m || !m->zms) {
        return -1;
    }
    if (m->a_mid >= 0) {
        return 0;
    }
    if (m->opts.audio_codec == ZMS_CODEC_G711A) {
        enc = "PCMA";
        payload = RTP_PAYLOAD_PCMA;
        rate = 8000;
    } else if (m->opts.audio_codec == ZMS_CODEC_G711U) {
        enc = "PCMU";
        payload = RTP_PAYLOAD_PCMU;
        rate = 8000;
    } else if (m->opts.audio_codec == ZMS_CODEC_OPUS) {
        enc = "opus";
        payload = RTP_PAYLOAD_OPUS;
        rate = m->opts.audio_rate > 0 ? m->opts.audio_rate : 48000;
    }
    {
        const void *extra = NULL;
        int extra_size = 0;
        int apt;

        if (m->opts.audio_codec == ZMS_CODEC_G711A || m->opts.audio_codec == ZMS_CODEC_G711U) {
            apt = (int)m->opts.audio_pt; /* PT 0 (PCMU) 合法；勿用真假性判断 */
        } else {
            apt = m->opts.audio_pt ? (int)m->opts.audio_pt : 97;
        }

        if (m->opts.audio_codec == ZMS_CODEC_AAC) {
            if (m->aac_asc_len == 0 || !m->aac_asc) {
                return -1;
            }
            extra = m->aac_asc;
            extra_size = (int)m->aac_asc_len;
        }
        m->a_pid = rtsp_muxer_add_payload(m->zms, "RTP/AVP", rate, apt, enc, m->a_seq,
                                          m->opts.audio_ssrc, 0, extra, extra_size);
    }
    if (m->a_pid < 0) {
        return -1;
    }
    m->a_mid = rtsp_muxer_add_media(m->zms, m->a_pid, payload, NULL, 0);
    return m->a_mid >= 0 ? 0 : -1;
}

static int zms_pack_bits(zms_rtp_muxer *m, const void *data, size_t len, zms_codec_id codec,
                         zms_rtp_mux_track trk, int64_t dts_ms, uint32_t rtp_ts, int keyframe)
{
    int mid;
    int pid;
    int flags = keyframe ? MOV_AV_FLAG_KEYFREAME : 0;

    if (!m || !data || len == 0) {
        return -1;
    }
    if (trk == ZMS_RTP_MUX_TRACK_VIDEO) {
        if (zms_ensure_video(m, codec) != 0) {
            return -1;
        }
        mid = m->v_mid;
        pid = m->v_pid;
    } else {
        if (zms_ensure_audio(m) != 0) {
            return -1;
        }
        mid = m->a_mid;
        pid = m->a_pid;
    }
    if (rtsp_muxer_sync_timeline(m->zms, pid, dts_ms, rtp_ts) != 0) {
        return -1;
    }
    if (rtsp_muxer_input(m->zms, mid, dts_ms, dts_ms, data, (int)len, flags) != 0) {
        return -1;
    }
    return 1;
}

static uint32_t mux_video_clock_hz(const zms_rtp_muxer *m)
{
    return m && m->opts.video_clock_hz > 0 ? m->opts.video_clock_hz : 90000u;
}

static uint32_t mux_audio_clock_hz(const zms_rtp_muxer *m)
{
    if (!m) {
        return 44100u;
    }
    if (m->opts.audio_clock_hz > 0) {
        return m->opts.audio_clock_hz;
    }
    return (uint32_t)(m->opts.audio_rate > 0 ? m->opts.audio_rate : 44100);
}

static int mux_rtsp_video_play_start(const zms_frame *f)
{
    return zms_frame_is_play_start(f);
}

static int mux_pack_frame(zms_rtp_muxer *m, const zms_frame *frame, zms_rtp_mux_track trk)

{
    uint32_t rtp_ts;
    int video_key = 0;

    if (!m || !frame || !frame->data || frame->size == 0) {
        return -1;
    }

    if (frame->track == ZMS_TRACK_VIDEO) {
        if (!frame->config_frame && !m->video_key_out) {
            zms_frame probe = *frame;
            if (!mux_rtsp_video_play_start(&probe)) {
                return 0;
            }
        }

        video_key = frame->keyframe;
        if (!video_key && frame->data && frame->size > 0) {
            zms_frame probe = *frame;
            if (mux_rtsp_video_play_start(&probe)) {
                video_key = 1;
            }
        }

        if (frame->config_frame) {
            if (frame->codec == ZMS_CODEC_H265) {
                (void)mux_hevc_refresh_params_from_au(m, frame->data, frame->size);
                return 0;
            }
            /* 缓存 H264 SPS/PPS 供内联注入（Annex-B config 路径） */
            if (frame->codec == ZMS_CODEC_H264 && frame->data && frame->size > 0) {
                const uint8_t *sps_p = NULL, *pps_p = NULL;
                size_t sps_l = 0, pps_l = 0;
                if (zms_h264_annexb_extract_sps_pps(frame->data, frame->size, &sps_p, &sps_l,
                                                    &pps_p, &pps_l) &&
                    sps_p && pps_p) {
                    mux_h264_cache_store(m, sps_p, sps_l, pps_p, pps_l);
                }
            }
            rtp_ts = m->abs_rtp_ts ? zms_ms_to_rtp_clock(m->vod_seek_ms, mux_video_clock_hz(m)) : 0;
        } else {
            if (!m->video_key_out) {
                if (!zms_egress_clock_epoch_locked(&m->clk)) {
                    (void)zms_egress_clock_lock_epoch(&m->clk, (uint32_t)frame->dts_ms);
                } else if (m->abs_rtp_ts && m->clk.epoch_ms != (uint32_t)frame->dts_ms) {
                    zms_egress_clock_rebase(&m->clk, (uint32_t)frame->dts_ms);
                }
            }
            if (!zms_egress_clock_epoch_locked(&m->clk)) {
                return 0;
            }
            /* RTP ts 跟解码时间线 (DTS)，非 PTS；B 帧 PTS 会倒退。 */
            rtp_ts = m->abs_rtp_ts
                         ? zms_ms_to_rtp_clock((uint32_t)frame->dts_ms, mux_video_clock_hz(m))
                         : zms_egress_clock_rtp_ts(&m->clk, (uint32_t)frame->dts_ms,
                                                   mux_video_clock_hz(m));
        }

    } else {
        if (!zms_egress_clock_epoch_locked(&m->clk)) {
            return 0;
        }

        rtp_ts = m->abs_rtp_ts ? zms_ms_to_rtp_clock((uint32_t)frame->dts_ms, mux_audio_clock_hz(m))
                               : zms_egress_clock_rtp_ts(&m->clk, (uint32_t)frame->dts_ms,
                                                         mux_audio_clock_hz(m));
    }

    if (!zms_rtsp_codec_supported(frame->codec, trk)) {
        return -1;
    }

    if (frame->codec == ZMS_CODEC_H265 && frame->track == ZMS_TRACK_VIDEO && !frame->config_frame) {
        zms_frame pack;
        uint8_t *vcl = NULL;
        uint8_t *prep = NULL;
        size_t vcl_len = 0;
        size_t prep_len = 0;
        int r;

        if (frame->data && frame->size > 0) {
            (void)mux_hevc_refresh_params_from_au(m, frame->data, frame->size);
        }

        pack = *frame;
        if (video_key && m->hevc_sps && m->hevc_sps_len > 0 && m->hevc_pps && m->hevc_pps_len > 0) {
            size_t need = frame->size + m->hevc_vps_len + m->hevc_sps_len + m->hevc_pps_len + 64u;

            prep = (uint8_t *)malloc(need);
            if (prep &&
                zms_h265_annexb_build_rtp_au(m->hevc_vps_len ? m->hevc_vps : NULL, m->hevc_vps_len,
                                             m->hevc_sps, m->hevc_sps_len, m->hevc_pps,
                                             m->hevc_pps_len, frame->data, frame->size, 1, prep,
                                             need, &prep_len) == ZTK_OK &&
                prep_len > 0) {
                pack.data = prep;
                pack.size = prep_len;
            } else {
                free(prep);
                prep = NULL;
            }
        }
        if (!prep) {
            vcl = (uint8_t *)malloc(frame->size);
            if (!vcl) {
                return -1;
            }
            if (zms_h265_annexb_copy_vcl(frame->data, frame->size, vcl, frame->size, &vcl_len) !=
                    ZTK_OK ||
                vcl_len == 0) {
                free(vcl);
                return -1;
            }
            pack.data = vcl;
            pack.size = vcl_len;
        }
        r = zms_pack_bits(m, pack.data, pack.size, frame->codec, trk, (int64_t)frame->dts_ms,
                          rtp_ts, video_key);
        free(vcl);
        free(prep);
        if (r < 0) {
            return -1;
        }
        if (video_key) {
            m->video_key_out = 1;
        }
        return r;
    }
    /* H264 RTP：仅 VCL (type 1)。VPS/SPS/PPS 走 SDP sprop；单独发 RTP NAL
     * 会让 FFmpeg 产出无 PTS 的 AVPacket，破坏 MPEG-TS mux
     *（"first pts and dts value must be set"）。 */
    if (frame->codec == ZMS_CODEC_H264 && trk == ZMS_RTP_MUX_TRACK_VIDEO && !frame->config_frame) {
        zms_frame pack;
        uint8_t *vcl = NULL;
        size_t vcl_len = 0;
        int r;

        vcl = (uint8_t *)malloc(frame->size);
        if (!vcl) {
            return -1;
        }
        if (zms_h264_annexb_copy_vcl(frame->data, frame->size, vcl, frame->size, &vcl_len) !=
                ZTK_OK ||
            vcl_len == 0) {
            free(vcl);
            return -1;
        }
        pack = *frame;
        pack.data = vcl;
        pack.size = vcl_len;
        r = zms_pack_bits(m, pack.data, pack.size, frame->codec, trk, (int64_t)frame->dts_ms,
                          rtp_ts, video_key);
        free(vcl);
        if (r < 0) {
            return -1;
        }
        if (video_key) {
            m->video_key_out = 1;
        }
        return r;
    }

    {
        const uint8_t *pack_data = frame->data;
        size_t pack_len = frame->size;
        uint8_t aac_scratch[8192];

        if (frame->codec == ZMS_CODEC_AV1 && trk == ZMS_RTP_MUX_TRACK_VIDEO &&
            !frame->config_frame) {
            (void)mux_av1_try_cache_av1c_from_obu(m, frame->data, frame->size);
        }
        if (frame->codec == ZMS_CODEC_AAC && trk == ZMS_RTP_MUX_TRACK_AUDIO) {
            const uint8_t *raw = frame->data;
            size_t raw_len = frame->size;

            if (zms_aac_es_to_raw(frame->data, frame->size, &raw, &raw_len) == ZTK_OK &&
                raw != frame->data) {
                if (raw_len == 0 || raw_len > sizeof(aac_scratch)) {
                    return -1;
                }
                memcpy(aac_scratch, raw, raw_len);
                pack_data = aac_scratch;
                pack_len = raw_len;
            }
        }
        if (zms_pack_bits(m, pack_data, pack_len, frame->codec, trk, (int64_t)frame->dts_ms, rtp_ts,
                          video_key) < 0) {
            return -1;
        }
    }
    if (trk == ZMS_RTP_MUX_TRACK_VIDEO && video_key && !frame->config_frame) {
        m->video_key_out = 1;
    }
    return 1;
}

static size_t avcc_to_annexb(const uint8_t *body, size_t len, uint8_t *out, size_t cap)

{
    if (len < 5) {
        return 0;
    }

    const uint8_t *p = body + 5;

    const uint8_t *end = body + len;

    size_t pos = 0;

    while (p + 4 <= end) {
        uint32_t nlen =
            ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];

        p += 4;

        if (nlen == 0 || p + nlen > end) {
            break;
        }

        if (pos + 4 + nlen > cap) {
            break;
        }

        out[pos++] = 0;

        out[pos++] = 0;

        out[pos++] = 0;

        out[pos++] = 1;

        memcpy(out + pos, p, nlen);

        pos += nlen;

        p += nlen;
    }

    return pos;
}

zms_rtp_muxer *zms_rtp_muxer_create(const zms_rtp_muxer_opts *opts, zms_rtp_mux_on_rtp on_rtp,
                                    void *user)

{
    if (!opts || !on_rtp) {
        return NULL;
    }

    zms_rtp_muxer *m = (zms_rtp_muxer *)calloc(1, sizeof(*m));

    if (!m) {
        return NULL;
    }

    m->opts = *opts;
    m->opts.audio_extra = NULL;
    m->opts.audio_extra_len = 0;
    m->opts.video_extra = NULL;
    m->opts.video_extra_len = 0;
    if (opts->audio_extra && opts->audio_extra_len > 0) {
        (void)mux_aac_store_asc(m, opts->audio_extra, opts->audio_extra_len);
    }
    if (opts->video_extra && opts->video_extra_len > 0) {
        mux_av1_store_av1c(m, opts->video_extra, opts->video_extra_len);
    }

    m->v_seq = opts->video_seq ? opts->video_seq : 1;

    m->a_seq = opts->audio_seq ? opts->audio_seq : 1;

    m->on_rtp = on_rtp;

    m->user = user;

    m->v_pid = m->a_pid = m->v_mid = m->a_mid = -1;

    m->video_codec = ZMS_CODEC_INVALID;

    m->zms = rtsp_muxer_create(zms_rtsp_onpacket, m);

    if (!m->zms) {
        mux_codec_cache_release(m);
        free(m);
        return NULL;
    }

    zms_egress_clock_init(&m->clk);

    return m;
}

void zms_rtp_muxer_destroy(zms_rtp_muxer *m)

{
    if (!m) {
        return;
    }

    if (m->zms) {
        rtsp_muxer_destroy(m->zms);
    }
    mux_codec_cache_release(m);
    free(m);
}

void zms_rtp_muxer_reset(zms_rtp_muxer *m)

{
    if (!m) {
        return;
    }

    memset(&m->stats, 0, sizeof(m->stats));

    m->video_key_out = 0;

    m->v_seq = m->opts.video_seq ? m->opts.video_seq : 1;

    m->a_seq = m->opts.audio_seq ? m->opts.audio_seq : 1;

    zms_egress_clock_reset(&m->clk);

    zms_rtsp_recreate(m);

    mux_hevc_cache_clear(m);
}

void zms_rtp_muxer_arm_play(zms_rtp_muxer *m)
{
    if (!m) {
        return;
    }
    zms_egress_clock_arm(&m->clk);
    m->video_key_out = 0;
    /* 直播 PLAY：RTP timestamp = media dts_ms（勿归零重基）。
     * ts 归零会导致 FFmpeg RTSP→MPEGTS 拷贝首包报错
     * "first pts and dts value must be set"。 */
    m->abs_rtp_ts = 1;
    m->clk.abs_rtp_ts = 1;
    m->vod_seek_ms = 0;
}

void zms_rtp_muxer_jump_live(zms_rtp_muxer *m)
{
    if (!m) {
        return;
    }
    m->video_key_out = 0;
    zms_egress_clock_unlock(&m->clk);
    zms_rtsp_drop_video(m);
    /* 保留 hevc_vps/sps/pps，snap 后下一 IDR 仍可 insertConfigFrame。 */
}

const zms_egress_clock *zms_rtp_muxer_play_clock(const zms_rtp_muxer *m)
{
    return m ? &m->clk : NULL;
}

zms_egress_clock *zms_rtp_muxer_play_clock_mut(zms_rtp_muxer *m)
{
    return m ? &m->clk : NULL;
}

uint16_t zms_rtp_muxer_video_seq(const zms_rtp_muxer *m)
{
    return m ? m->v_seq : 0;
}

uint16_t zms_rtp_muxer_audio_seq(const zms_rtp_muxer *m)
{
    return m ? m->a_seq : 0;
}

const zms_rtp_muxer_stats *zms_rtp_muxer_get_stats(const zms_rtp_muxer *m)

{
    return m ? &m->stats : NULL;
}

int zms_rtp_muxer_input_slot(zms_rtp_muxer *m, const zms_gop_slot *slot)

{
    zms_frame frame;

    zms_rtp_mux_track trk;

    if (!m || !slot || !slot->data || slot->len == 0) {
        return -1;
    }

    if (!zms_rtsp_codec_supported(slot->codec, slot->track == ZMS_TRACK_VIDEO
                                                   ? ZMS_RTP_MUX_TRACK_VIDEO
                                                   : ZMS_RTP_MUX_TRACK_AUDIO)) {
        return -1;
    }

    zms_frame_init(&frame);

    frame.data = slot->data;

    frame.size = slot->len;

    frame.codec = slot->codec;

    frame.track = slot->track;

    frame.keyframe = slot->keyframe;
    frame.config_frame = slot->config_frame;

    frame.dts_ms = slot->dts_ms;
    frame.pts_ms = zms_gop_slot_pts_ms(slot);

    trk = slot->track == ZMS_TRACK_VIDEO ? ZMS_RTP_MUX_TRACK_VIDEO : ZMS_RTP_MUX_TRACK_AUDIO;

    return mux_pack_frame(m, &frame, trk);
}

int zms_rtp_muxer_input_rtmp(zms_rtp_muxer *m, uint8_t type_id, uint32_t tag_dts_ms,
                             const uint8_t *data, size_t len,

                             uint8_t *scratch, size_t scratch_cap)

{
    if (!m || !data || len == 0) {
        return -1;
    }

    if (type_id == 9) {
        zms_codec_id vc = zms_flv_tag_video_codec(data, len);

        zms_frame frame;

        size_t need = len + 64;

        uint8_t *conv = scratch;

        size_t cap = scratch_cap;

        uint8_t *heap = NULL;

        if (len < 5 || data[1] != 1 || !zms_rtsp_codec_supported(vc, ZMS_RTP_MUX_TRACK_VIDEO)) {
            return 0;
        }

        if (!conv || need > cap) {
            heap = (uint8_t *)malloc(need);

            if (!heap) {
                return -1;
            }

            conv = heap;

            cap = need;
        }

        size_t annexb_len = avcc_to_annexb(data, len, conv, cap);

        int key = (data[0] & 0xf0) == 0x10;

        if (annexb_len == 0) {
            free(heap);

            return 0;
        }

        zms_frame_init(&frame);

        frame.data = conv;

        frame.size = annexb_len;

        frame.codec = vc;

        frame.track = ZMS_TRACK_VIDEO;

        frame.keyframe = key;

        frame.dts_ms = frame.pts_ms = tag_dts_ms;

        int r = mux_pack_frame(m, &frame, ZMS_RTP_MUX_TRACK_VIDEO);

        free(heap);

        return r;
    }

    if (type_id == 8) {
        const uint8_t *es = NULL;

        size_t es_len = 0;

        zms_codec_id ac = ZMS_CODEC_INVALID;

        zms_frame frame;

        if (!m->video_key_out) {
            return 0;
        }

        if (zms_flv_tag_audio_to_es(data, len, &es, &es_len, &ac) != ZTK_OK || !es || es_len == 0) {
            return 0;
        }

        if (!zms_rtsp_codec_supported(ac, ZMS_RTP_MUX_TRACK_AUDIO)) {
            return 0;
        }

        zms_frame_init(&frame);

        frame.data = (uint8_t *)es;

        frame.size = es_len;

        frame.codec = ac;

        frame.track = ZMS_TRACK_AUDIO;

        frame.dts_ms = frame.pts_ms = tag_dts_ms;

        return mux_pack_frame(m, &frame, ZMS_RTP_MUX_TRACK_AUDIO);
    }

    return 0;
}

void zms_rtp_muxer_set_catchup(zms_rtp_muxer *m, int on)
{
    zms_rtp_muxer_set_catchup_budget(m, on, 256);
}

void zms_rtp_muxer_set_catchup_budget(zms_rtp_muxer *m, int on, int max_frames)
{
    if (!m) {
        return;
    }
    m->catchup = on ? 1 : 0;
    if (!on) {
        m->catchup_left = 0;
        return;
    }
    m->catchup_left = max_frames > 0 ? max_frames : 256;
}

int zms_rtp_muxer_catchup_on(const zms_rtp_muxer *m)
{
    return m && m->catchup;
}

int zms_rtp_muxer_awaiting_video_key(const zms_rtp_muxer *m)
{
    return m && !m->video_key_out;
}

void zms_rtp_muxer_catchup_frame(zms_rtp_muxer *m)
{
    if (!m || !m->catchup) {
        return;
    }
    if (--m->catchup_left <= 0) {
        m->catchup = 0;
    }
}

void zms_rtp_muxer_begin_vod_seek(zms_rtp_muxer *m, uint32_t seek_ms)
{
    if (!m) {
        return;
    }
    m->stats.video_pkt_count = 0;
    m->stats.audio_pkt_count = 0;
    m->stats.video_octet_count = 0;
    m->stats.audio_octet_count = 0;
    m->stats.video_last_rtp_ts = 0;
    m->stats.audio_last_rtp_ts = 0;
    /* 会话中途 seek 仍保持 seq 单调（ffplay/VLC 遇 seq 回绕会丢包）。 */
    zms_rtsp_snapshot_seq(m);
    zms_rtsp_recreate(m);
    mux_hevc_cache_clear(m);
    m->abs_rtp_ts = 1;
    m->video_key_out = 0;
    m->clk.paused = 0;
    m->clk.pause_wall_ms = 0;
    zms_rtp_muxer_arm_play(m);
    m->vod_seek_ms = seek_ms;
    zms_egress_clock_rebase(&m->clk, seek_ms);
}

uint32_t zms_rtp_muxer_vod_anchor_ms(const zms_rtp_muxer *m)
{
    return m && m->abs_rtp_ts ? m->vod_seek_ms : 0;
}

void zms_rtp_muxer_set_play_scale(zms_rtp_muxer *m, double scale)
{
    if (!m) {
        return;
    }
    zms_egress_clock_set_scale(&m->clk, scale);
}

void zms_rtp_muxer_pause_play(zms_rtp_muxer *m)
{
    if (!m) {
        return;
    }
    zms_egress_clock_pause(&m->clk);
}

void zms_rtp_muxer_resume_play(zms_rtp_muxer *m)
{
    if (!m) {
        return;
    }
    zms_egress_clock_resume(&m->clk);
}

void zms_rtp_muxer_send_avc_config(zms_rtp_muxer *m, const uint8_t *avcc, size_t avcc_len,
                                   uint32_t anchor_ms)
{
    const uint8_t *sps = NULL, *pps = NULL;
    size_t sps_len = 0, pps_len = 0;

    (void)anchor_ms;
    if (!m || !avcc || avcc_len <= 5 || zms_flv_tag_video_codec(avcc, avcc_len) != ZMS_CODEC_H264) {
        return;
    }
    if (!zms_rtmp_avc_extract_sps_pps(avcc, avcc_len, &sps, &sps_len, &pps, &pps_len)) {
        return;
    }
    mux_h264_store_avcc(m, avcc, avcc_len);
    /* 每个 IDR 前缓存 SPS/PPS 供内联注入（RFC 6184 §8.4）。
     * 勿发 ts=0 config RTP：SPS/PPS 已在 SDP sprop-parameter-sets；
     * 再发会让 MPEG-TS mux 把独立 SPS/PPS 当无效 AU。 */
    mux_h264_cache_store(m, sps, sps_len, pps, pps_len);
}

void zms_rtp_muxer_send_hevc_config(zms_rtp_muxer *m, const uint8_t *video_cfg, size_t cfg_len,
                                    uint32_t anchor_ms)
{
    const uint8_t *vps = NULL, *sps = NULL, *pps = NULL;
    size_t vps_len = 0, sps_len = 0, pps_len = 0;

    (void)anchor_ms;
    if (!m || !video_cfg || cfg_len <= 5) {
        return;
    }
    if (!zms_h265_video_config_param_sets(video_cfg, cfg_len, &vps, &vps_len, &sps, &sps_len, &pps,
                                          &pps_len)) {
        return;
    }
    mux_hevc_store_hvcc(m, video_cfg, cfg_len);
    /* SDP sprop + 首关键帧 insertConfigFrame；PLAY 不发 ts=0 config RTP。 */
    mux_hevc_cache_store(m, vps, vps_len, sps, sps_len, pps, pps_len);
}

void zms_rtp_muxer_send_av1_config(zms_rtp_muxer *m, const uint8_t *video_cfg, size_t cfg_len,
                                   uint32_t anchor_ms)
{
    const uint8_t *av1c = NULL;
    size_t av1c_len = 0;

    (void)anchor_ms;
    if (!m || !video_cfg || cfg_len <= 5) {
        return;
    }
    if (zms_flv_tag_video_codec(video_cfg, cfg_len) != ZMS_CODEC_AV1) {
        return;
    }
    if (!zms_av1_over_rtmp_config_extradata(video_cfg, cfg_len, &av1c, &av1c_len) || !av1c ||
        av1c_len == 0) {
        return;
    }
    mux_av1_store_av1c(m, av1c, av1c_len);
}
