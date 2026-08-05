#include "webrtc/whep/whep_play_session.h"
#include "webrtc/session/webrtc_media_gateway.h"
#include "webrtc/session/webrtc_media_internal.h"
#include "webrtc/session/webrtc_session_internal.h"
#include "zms/media/codec/codec_id.h"
#include "zms/egress/egress_pipeline.h"
#include "zms/media/codec/h264/h264_over_rtmp.h"
#include "zms/engine/gop/gop_queue.h"
#include "zms/engine/media/media_limits.h"
#include "zms/engine/stream/stream_hub.h"
#include "zms/egress/egress_clock.h"
#include "zms/session/rtp/rtcp.h"
#include "zms/media/wire/rtp_packet.h"
#include "zms/webrtc/webrtc_srtp.h"
#include "zms/egress/egress_live_policy.h"
#include "zms/egress/rtp/rtp_muxer.h"
#include "zms/egress/rtp/rtp_play_pump.h"
#include "webrtc/session/webrtc_ice_internal.h"
#include "ztk/net/udp_server.h"
#include "ztk/util/buf.h"
#include "ztk/util/log.h"
#include "ztk/util/timer.h"
#include "ztk/poller/poller.h"
#include "ztk/platform.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#endif
static uint32_t webrtc_ipv4_be(const char *ip)
{
    struct in_addr addr;
    if (!ip || !ip[0]) {
        return htonl(0x7f000001u);
    }
    if (inet_pton(AF_INET, ip, &addr) != 1) {
        return htonl(0x7f000001u);
    }
    return addr.s_addr;
}

static void webrtc_send_udp(zms_webrtc_session *s, const uint8_t *data, size_t len)
{
    if (!s || !data || len == 0) {
        return;
    }
    (void)zms_webrtc_session_send_udp(s, data, len);
}

static void webrtc_play_gateway_cfg(const zms_webrtc_session *s, zms_webrtc_gateway_ingest_cfg *cfg)
{
    if (!s || !cfg) {
        return;
    }
    zms_webrtc_gateway_ingest_cfg_defaults(cfg);
    cfg->wire_video_pt = s->video_pt;
    cfg->wire_audio_pt = s->audio_pt;
    cfg->answer_has_video = s->answer_has_video;
    cfg->answer_has_audio = s->answer_has_audio;
}
#if defined(ZMS_HAVE_WEBRTC_DTLS) && ZMS_HAVE_WEBRTC_DTLS
static struct zms_webrtc_srtp *g_play_srtp_verify;
static char g_play_srtp_verify_id[ZMS_WEBRTC_SESSION_ID_LEN];
static void webrtc_play_srtp_verify_reset(void)
{
    zms_webrtc_srtp_destroy(g_play_srtp_verify);
    g_play_srtp_verify = NULL;
    g_play_srtp_verify_id[0] = '\0';
}
/** 自测 protect→unprotect（服务端写密钥，同 DTLS server 上 SRS send_key）。 */
static void webrtc_play_srtp_roundtrip_check(zms_webrtc_session *s, const uint8_t *plain,
                                             size_t plain_len, size_t prot_len)
{
    uint8_t server_key[ZMS_WEBRTC_SRTP_KEY_LEN];
    uint8_t server_salt[ZMS_WEBRTC_SRTP_SALT_LEN];
    size_t check_len;
    static unsigned tests;
    if (!s || !s->dtls || !plain || plain_len == 0 || prot_len == 0 || tests >= 4u) {
        return;
    }
    if (!g_play_srtp_verify ||
        strncmp(g_play_srtp_verify_id, s->id, sizeof(g_play_srtp_verify_id)) != 0) {
        webrtc_play_srtp_verify_reset();
        g_play_srtp_verify = zms_webrtc_srtp_create();
        if (!g_play_srtp_verify ||
            zms_webrtc_dtls_export_server_srtp(s->dtls, server_key, server_salt) != 0) {
            return;
        }
        if (zms_webrtc_srtp_init_recv(g_play_srtp_verify, server_key, server_salt) != 0) {
            return;
        }
        if (zms_webrtc_session_io_buf_ensure(s) == 0) {
            zms_webrtc_srtp_bind_scratch(g_play_srtp_verify, s->io_buf, s->io_cap);
        }
        strncpy(g_play_srtp_verify_id, s->id, sizeof(g_play_srtp_verify_id) - 1);
    }
    if (prot_len > ZMS_WEBRTC_PLAY_RTP_SLOT_MAX || !s->io_buf) {
        return;
    }
    {
        uint8_t prot_tmp[ZMS_WEBRTC_PLAY_RTP_SLOT_MAX];
        memcpy(prot_tmp, s->io_buf, prot_len);
        check_len = prot_len;
        if (zms_webrtc_srtp_unprotect(g_play_srtp_verify, prot_tmp, &check_len) != 0) {
            ++tests;
            ztk_warn(
                "[webrtc] play srtp selftest fail id=%s step=unprotect prot=%zu (send continues)",
                s->id, prot_len);
            return;
        }
        if (check_len != plain_len || memcmp(prot_tmp, plain, plain_len) != 0) {
            ++tests;
            ztk_warn("[webrtc] play srtp selftest fail id=%s step=mismatch plain=%zu got=%zu",
                     s->id, plain_len, check_len);
            return;
        }
    }
    ++tests;
    ztk_info("[webrtc] play srtp selftest ok id=%s pt=%u seq=%u ssrc=%u plain=%zu prot=%zu", s->id,
             (unsigned)zms_rtp_payload_type(plain, plain_len),
             (unsigned)(((uint16_t)plain[2] << 8) | plain[3]),
             (unsigned)(((uint32_t)plain[8] << 24) | ((uint32_t)plain[9] << 16) |
                        ((uint32_t)plain[10] << 8) | plain[11]),
             plain_len, prot_len);
}
#else
static void webrtc_play_srtp_verify_reset(void) {}
#endif
static void webrtc_on_pli(struct zms_webrtc_session *s)
{
    if (!s || !s->play.readers.gop) {
        return;
    }
    zms_gop_reader_seek_gop_key(s->play.readers.gop);
    if (s->play_rtp_muxer) {
        zms_egress_live_snap_gop(s->play.readers.gop, s->play_rtp_muxer);
    }
}

static void webrtc_on_rtcp_block(const uint8_t *block, size_t block_len, void *user)
{
    zms_webrtc_session *s = (zms_webrtc_session *)user;
    if (!s || !block || block_len == 0) {
        return;
    }
    if (zms_webrtc_rtcp_is_psfb_pli(block, block_len) ||
        zms_webrtc_rtcp_is_rtpfb_nack(block, block_len)) {
        webrtc_on_pli(s);
    }
}

static int webrtc_play_send_srtcp_sr(zms_webrtc_session *s, int send_video, int send_audio)
{
    const zms_rtp_muxer_stats *st;
    const zms_egress_clock *clk;
    uint8_t rtcp[64];
    size_t n;
    uint32_t ntp_sec = 0;
    uint32_t ntp_frac = 0;
    uint32_t vhz = 90000;
    uint32_t ahz = 48000;
    struct zms_webrtc_srtp *srtp;
    int sent = 0;
    if (!s || !s->play_rtp_muxer || !s->peer_known) {
        return 0;
    }
    st = zms_rtp_muxer_get_stats(s->play_rtp_muxer);
    clk = zms_rtp_muxer_play_clock(s->play_rtp_muxer);
    if (!st) {
        return 0;
    }
    if (send_video && s->source && s->source->has_video && st->video_pkt_count > 0) {
        if (clk) {
            zms_egress_clock_sr_ntp(clk, st->video_last_rtp_ts, vhz, &ntp_sec, &ntp_frac);
        }
        n = zms_webrtc_rtcp_build_sr(rtcp, sizeof(rtcp), 1, ntp_sec, ntp_frac,
                                     st->video_last_rtp_ts, st->video_pkt_count,
                                     st->video_octet_count);
        srtp = s->video_srtp;
        if (n && srtp && zms_webrtc_srtcp_protect(srtp, rtcp, &n, sizeof(rtcp)) == 0 &&
            zms_webrtc_session_send_udp(s, rtcp, n) == 0) {
            sent = 1;
        } else if (n && srtp) {
            ztk_warn("[webrtc] srtcp video SR fail id=%s", s->id);
        }
    }
    if (send_audio && s->source && s->source->has_audio && s->answer_has_audio &&
        st->audio_pkt_count > 0) {
        if (clk) {
            zms_egress_clock_sr_ntp(clk, st->audio_last_rtp_ts, ahz, &ntp_sec, &ntp_frac);
        }
        n = zms_webrtc_rtcp_build_sr(rtcp, sizeof(rtcp), 2, ntp_sec, ntp_frac,
                                     st->audio_last_rtp_ts, st->audio_pkt_count,
                                     st->audio_octet_count);
        srtp = s->audio_srtp;
        if (n && srtp && zms_webrtc_srtcp_protect(srtp, rtcp, &n, sizeof(rtcp)) == 0 &&
            zms_webrtc_session_send_udp(s, rtcp, n) == 0) {
            sent = 1;
        } else if (n && srtp) {
            ztk_warn("[webrtc] srtcp audio SR fail id=%s", s->id);
        }
    }
    return sent;
}
/** 对齐 RTSP rtsp_play_try_rtcp_boot：浏览器首 RTP 后需 SRTCP SR。 */
static void webrtc_play_try_rtcp_boot(zms_webrtc_session *s)
{
    const zms_rtp_muxer_stats *st;
    int send_v = 0;
    int send_a = 0;
    if (!s || !s->play_rtp_muxer || !s->source) {
        return;
    }
    st = zms_rtp_muxer_get_stats(s->play_rtp_muxer);
    if (!st) {
        return;
    }
    if (s->source->has_video && !s->rtcp_video_sr_sent && st->video_pkt_count > 0) {
        send_v = 1;
    }
    if (s->source->has_audio && s->answer_has_audio && !s->rtcp_audio_sr_sent &&
        st->audio_pkt_count > 0) {
        send_a = 1;
    }
    if (!send_v && !send_a) {
        return;
    }
    if (webrtc_play_send_srtcp_sr(s, send_v, send_a)) {
        if (send_v) {
            s->rtcp_video_sr_sent = 1;
        }
        if (send_a) {
            s->rtcp_audio_sr_sent = 1;
        }
        ztk_info("[webrtc] play srtcp boot id=%s video=%d audio=%d v_pkts=%u", s->id, send_v,
                 send_a, st->video_pkt_count);
    }
}

static void webrtc_play_send_rtcp_sr(zms_webrtc_session *s)
{
    if (!s || !s->play_rtp_muxer) {
        return;
    }
    (void)webrtc_play_send_srtcp_sr(s, 1, 1);
}

static void webrtc_play_rtp_q_reset(zms_webrtc_session *s)
{
    size_t i;
    if (!s || !s->play_rtp_q) {
        return;
    }
    for (i = 0; i < ZMS_WEBRTC_PLAY_RTPQ_CAP; ++i) {
        if (s->play_rtp_q[i].buf) {
            ztk_buf_unref(s->play_rtp_q[i].buf);
        }
        s->play_rtp_q[i].buf = NULL;
    }
    s->play_rtp_q_r = 0;
    s->play_rtp_q_w = 0;
    s->play_rtp_q_n = 0;
}

static int webrtc_play_rtp_q_ensure(zms_webrtc_session *s)
{
    if (!s) {
        return -1;
    }
    if (s->play_rtp_q) {
        return 0;
    }
    s->play_rtp_q =
        (zms_webrtc_play_rtp_slot *)calloc(ZMS_WEBRTC_PLAY_RTPQ_CAP, sizeof(*s->play_rtp_q));
    return s->play_rtp_q ? 0 : -1;
}

static void webrtc_play_rtp_q_free(zms_webrtc_session *s)
{
    if (!s) {
        return;
    }
    webrtc_play_rtp_q_reset(s);
    free(s->play_rtp_q);
    s->play_rtp_q = NULL;
}

static void webrtc_play_diag(zms_webrtc_session *s, const char *tag, const char *detail)
{
#if defined(ZMS_WEBRTC_PLAY_DIAG)
    if (!s || !tag) {
        return;
    }
    ztk_info("[webrtc] diag id=%s tid=%llu poller=%d q=%u tag=%s %s", s->id,
             (unsigned long long)ztk_thread_self_id(),
             s->poller ? ztk_poller_is_current_thread(s->poller) : -1, s->play_rtp_q_n, tag,
             detail ? detail : "");
#else
    (void)s;
    (void)tag;
    (void)detail;
#endif
}

static void webrtc_play_trace(zms_webrtc_session *s, const char *phase, unsigned extra)
{
#if defined(ZMS_WEBRTC_PLAY_TRACE)
    if (!s || !phase) {
        return;
    }
    ztk_info("[webrtc] trace id=%s step=%u phase=%s extra=%u q=%u", s->id, ++s->play_trace_seq,
             phase, extra, s->play_rtp_q_n);
#else
    (void)s;
    (void)phase;
    (void)extra;
#endif
}

static int webrtc_play_rtp_q_push(zms_webrtc_session *s, zms_rtp_mux_track track,
                                  const uint8_t *rtp, size_t len)
{
    zms_webrtc_play_rtp_slot *slot;
    ztk_buf *buf;
    void *dst;
    if (!s || !rtp || len < 12 || len > ZMS_WEBRTC_PLAY_RTP_SLOT_MAX || !s->poller) {
        return -1;
    }
    if (webrtc_play_rtp_q_ensure(s) != 0) {
        return -1;
    }
    if (s->play_rtp_q_n >= ZMS_WEBRTC_PLAY_RTPQ_CAP) {
        zms_webrtc_play_rtp_slot *drop = &s->play_rtp_q[s->play_rtp_q_r % ZMS_WEBRTC_PLAY_RTPQ_CAP];
        if (drop->buf) {
            ztk_buf_unref(drop->buf);
        }
        drop->buf = NULL;
        s->play_rtp_q_r = (s->play_rtp_q_r + 1) % ZMS_WEBRTC_PLAY_RTPQ_CAP;
        --s->play_rtp_q_n;
        ztk_warn("[webrtc] rtpq_drop_oldest id=%s session=%u", s->id, s->session_no);
    }
    buf = ztk_buf_alloc_local(s->poller, len);
    if (!buf) {
        return -1;
    }
    dst = (void *)ztk_buf_data(buf);
    memcpy(dst, rtp, len);
    ztk_buf_set_len(buf, len);
    slot = &s->play_rtp_q[s->play_rtp_q_w % ZMS_WEBRTC_PLAY_RTPQ_CAP];
    if (slot->buf) {
        ztk_buf_unref(slot->buf);
    }
    slot->track = (uint8_t)track;
    slot->buf = buf;
    s->play_rtp_q_w = (s->play_rtp_q_w + 1) % ZMS_WEBRTC_PLAY_RTPQ_CAP;
    ++s->play_rtp_q_n;
    return 0;
}

static int webrtc_play_send_rtp_item_impl(zms_webrtc_session *s, uint8_t track, const uint8_t *rtp,
                                          size_t len)
{
    struct zms_webrtc_srtp *srtp;
    zms_webrtc_gateway_ingest_cfg gw;
    size_t out_len;
    size_t cap;
    int is_video = track == (uint8_t)ZMS_RTP_MUX_TRACK_VIDEO;
    uint8_t *plain_copy;
    if (!s || !rtp || len < 12 || !s->io_buf || s->io_cap < ZMS_WEBRTC_PLAY_CRYPT_BYTES ||
        len + ZMS_WEBRTC_SRTP_TAG_LEN > s->io_cap) {
        return -1;
    }
    cap = s->io_cap;
    srtp = is_video ? s->video_srtp : s->audio_srtp;
    memcpy(s->io_buf, rtp, len);
    webrtc_play_gateway_cfg(s, &gw);
    zms_webrtc_gateway_remap_pt_egress(s->io_buf, len, &gw, is_video);
    out_len = len;
    if (!srtp) {
        webrtc_play_diag(s, "send_pre", "srtp null");
        return -1;
    }
    plain_copy = s->io_buf + ZMS_WEBRTC_PLAY_RTP_SLOT_MAX;
    if (len + ZMS_WEBRTC_SRTP_TAG_LEN <= cap && len <= ZMS_WEBRTC_PLAY_RTP_SLOT_MAX &&
        plain_copy + len <= s->io_buf + cap) {
        memcpy(plain_copy, s->io_buf, len);
    }
    if (zms_webrtc_srtp_protect(srtp, s->io_buf, &out_len, cap) != 0) {
        static unsigned fail_log;
        if (fail_log < 8) {
            ++fail_log;
            ztk_warn("[webrtc] srtp protect fail id=%s track=%u pt=%u len=%zu", s->id,
                     (unsigned)track, (unsigned)zms_rtp_payload_type(s->io_buf, len), len);
        }
        return -1;
    }
#if defined(ZMS_HAVE_WEBRTC_DTLS) && ZMS_HAVE_WEBRTC_DTLS
    if (is_video && len <= ZMS_WEBRTC_PLAY_RTP_SLOT_MAX && len + ZMS_WEBRTC_SRTP_TAG_LEN <= cap) {
        webrtc_play_srtp_roundtrip_check(s, plain_copy, len, out_len);
    }
#endif
    if (zms_webrtc_session_send_udp(s, s->io_buf, out_len) != 0) {
        return -1;
    }
    if (is_video) {
        static unsigned first_v;
        const uint8_t *plain = len <= ZMS_WEBRTC_PLAY_RTP_SLOT_MAX ? plain_copy : s->io_buf;
        if (first_v < 8) {
            ++first_v;
            ztk_info("[webrtc] play srtp out id=%s pt=%u seq=%u ssrc=%u len=%zu->%zu peer=%s:%u",
                     s->id, (unsigned)zms_rtp_payload_type(plain, len),
                     (unsigned)(((uint16_t)plain[2] << 8) | plain[3]),
                     (unsigned)(((uint32_t)plain[8] << 24) | ((uint32_t)plain[9] << 16) |
                                ((uint32_t)plain[10] << 8) | plain[11]),
                     len, out_len, s->peer_ip, (unsigned)s->peer_port);
        }
    }
    return 0;
}

static int webrtc_play_send_rtp_item(zms_webrtc_session *s, uint8_t track, const uint8_t *rtp,
                                     size_t len)
{
#if defined(_WIN32)
    __try {
        return webrtc_play_send_rtp_item_impl(s, track, rtp, len);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ztk_warn("[webrtc] FAULT send id=%s code=0x%08x track=%u len=%zu", s ? s->id : "?",
                 (unsigned)GetExceptionCode(), (unsigned)track, len);
        return -1;
    }
#else
    return webrtc_play_send_rtp_item_impl(s, track, rtp, len);
#endif
}

static unsigned webrtc_play_rtp_q_flush(zms_webrtc_session *s, unsigned max_send)
{
    unsigned sent = 0;
    if (!s || !s->play_rtp_q) {
        return 0;
    }
    while (max_send > 0 && s->play_rtp_q_n > 0) {
        zms_webrtc_play_rtp_slot *slot = &s->play_rtp_q[s->play_rtp_q_r % ZMS_WEBRTC_PLAY_RTPQ_CAP];
        if (slot->buf) {
            if (webrtc_play_send_rtp_item(s, slot->track, (const uint8_t *)ztk_buf_data(slot->buf),
                                          ztk_buf_len(slot->buf)) == 0) {
                ++sent;
            }
            ztk_buf_unref(slot->buf);
            slot->buf = NULL;
        }
        s->play_rtp_q_r = (s->play_rtp_q_r + 1) % ZMS_WEBRTC_PLAY_RTPQ_CAP;
        --s->play_rtp_q_n;
        --max_send;
    }
    if (sent > 0) {
        webrtc_play_try_rtcp_boot(s);
    }
    return sent;
}

static void webrtc_play_load_h264_ps(zms_webrtc_session *s)
{
    const uint8_t *sps = NULL;
    const uint8_t *pps = NULL;
    size_t sps_len = 0;
    size_t pps_len = 0;
    size_t vcfg_len = 0;
    const uint8_t *vcfg;
    if (!s) {
        return;
    }
    s->play_h264_sps_len = 0;
    s->play_h264_pps_len = 0;
    s->play_h264_seq_bias = 0;
    if (!s->source || s->video_codec != ZMS_CODEC_H264) {
        return;
    }
    vcfg = zms_media_source_video_config(s->source, &vcfg_len);
    if (vcfg && vcfg_len &&
        zms_rtmp_avc_extract_sps_pps(vcfg, vcfg_len, &sps, &sps_len, &pps, &pps_len)) {
        goto store;
    }
    if (s->source->gop_queue) {
        vcfg = zms_gop_queue_video_config(s->source->gop_queue, &vcfg_len);
        if (vcfg && vcfg_len &&
            zms_rtmp_avc_extract_sps_pps(vcfg, vcfg_len, &sps, &sps_len, &pps, &pps_len)) {
            goto store;
        }
    }
    return;
store:
    if (!sps || !sps_len || sps_len > sizeof(s->play_h264_sps) || !pps || !pps_len ||
        pps_len > sizeof(s->play_h264_pps)) {
        return;
    }
    memcpy(s->play_h264_sps, sps, sps_len);
    memcpy(s->play_h264_pps, pps, pps_len);
    s->play_h264_sps_len = sps_len;
    s->play_h264_pps_len = pps_len;
    ztk_info("[webrtc] play h264 stap ps id=%s sps=%zu pps=%zu", s->id, sps_len, pps_len);
}

static int webrtc_rtp_is_h264_idr_fu_start(const uint8_t *rtp, size_t len)
{
    size_t off;
    uint8_t fu_ind;
    uint8_t fu_hdr;
    if (!rtp || len < 14) {
        return 0;
    }
    off = zms_rtp_payload_offset(rtp, len);
    if (off == 0 || off + 2 > len) {
        return 0;
    }
    fu_ind = rtp[off];
    fu_hdr = rtp[off + 1];
    if ((fu_ind & 0x1fu) != 28u) {
        return 0;
    }
    if ((fu_hdr & 0x80u) == 0) {
        return 0;
    }
    if ((fu_hdr & 0x1fu) != 5u) {
        return 0;
    }
    return 1;
}

static void webrtc_rtp_bump_seq(uint8_t *rtp, size_t len, uint16_t bias)
{
    uint16_t seq;
    if (!rtp || len < 4 || bias == 0) {
        return;
    }
    seq = (uint16_t)(((uint16_t)rtp[2] << 8) | rtp[3]);
    seq = (uint16_t)(seq + bias);
    rtp[2] = (uint8_t)(seq >> 8);
    rtp[3] = (uint8_t)(seq & 0xffu);
}

static int webrtc_play_build_h264_stap_a(const zms_webrtc_session *s, const uint8_t *tpl,
                                         size_t tpl_len, uint8_t *out, size_t cap, size_t *out_len)
{
    size_t pay_len;
    uint8_t stap_hdr;
    size_t pos;
    if (!s || !tpl || tpl_len < 12 || !out || !out_len || s->play_h264_sps_len == 0 ||
        s->play_h264_pps_len == 0) {
        return -1;
    }
    pay_len = 1 + 2 + s->play_h264_sps_len + 2 + s->play_h264_pps_len;
    if (12 + pay_len > cap) {
        return -1;
    }
    memcpy(out, tpl, 12);
    webrtc_rtp_bump_seq(out, 12, s->play_h264_seq_bias);
    out[1] = (uint8_t)((out[1] & 0x80u) | (tpl[1] & 0x7fu)); /* STAP-A 上 marker=0 */
    stap_hdr = (uint8_t)((s->play_h264_sps[0] & 0x60u) | 24u);
    pos = 12;
    out[pos++] = stap_hdr;
    out[pos++] = (uint8_t)(s->play_h264_sps_len >> 8);
    out[pos++] = (uint8_t)(s->play_h264_sps_len);
    memcpy(out + pos, s->play_h264_sps, s->play_h264_sps_len);
    pos += s->play_h264_sps_len;
    out[pos++] = (uint8_t)(s->play_h264_pps_len >> 8);
    out[pos++] = (uint8_t)(s->play_h264_pps_len);
    memcpy(out + pos, s->play_h264_pps, s->play_h264_pps_len);
    pos += s->play_h264_pps_len;
    *out_len = pos;
    return 0;
}

static void webrtc_on_mux_rtp(zms_rtp_mux_track track, const uint8_t *rtp, size_t len, void *user)
{
    zms_webrtc_session *s = (zms_webrtc_session *)user;
    uint8_t wire[ZMS_WEBRTC_PLAY_RTP_SLOT_MAX];
    size_t wire_len = len;
    uint8_t stap[ZMS_WEBRTC_PLAY_H264_STAP_MAX];
    size_t stap_len = 0;
    if (!s || !rtp || len < 12 || len > sizeof(wire)) {
        return;
    }
    if (track == ZMS_RTP_MUX_TRACK_VIDEO && s->video_codec == ZMS_CODEC_H264 &&
        s->play_h264_sps_len > 0 && webrtc_rtp_is_h264_idr_fu_start(rtp, len)) {
        if (webrtc_play_build_h264_stap_a(s, rtp, len, stap, sizeof(stap), &stap_len) == 0) {
            static unsigned stap_log;
            (void)webrtc_play_rtp_q_push(s, track, stap, stap_len);
            ++s->play_h264_seq_bias;
            if (stap_log < 8) {
                ++stap_log;
                ztk_info("[webrtc] play h264 stap-a id=%s seq=%u ts=%u bias=%u len=%zu", s->id,
                         (unsigned)(((uint16_t)stap[2] << 8) | stap[3]),
                         (unsigned)(((uint32_t)stap[4] << 24) | ((uint32_t)stap[5] << 16) |
                                    ((uint32_t)stap[6] << 8) | stap[7]),
                         (unsigned)s->play_h264_seq_bias, stap_len);
            }
        }
    }
    memcpy(wire, rtp, len);
    if (track == ZMS_RTP_MUX_TRACK_VIDEO && s->play_h264_seq_bias > 0) {
        webrtc_rtp_bump_seq(wire, wire_len, s->play_h264_seq_bias);
    }
    (void)webrtc_play_rtp_q_push(s, track, wire, wire_len);
}

static void webrtc_play_arm_mux(zms_webrtc_session *s)
{
    zms_rtp_muxer_opts opts;
    zms_rtp_play_mux_opts ocfg;
    zms_rtp_play_mux_handle opened;
    if (!s || s->play_rtp_muxer) {
        return;
    }
    memset(&opts, 0, sizeof(opts));
    opts.video_clock_hz = 90000;
    opts.audio_clock_hz = 48000;
    opts.audio_rate = 48000;
    opts.audio_codec = ZMS_CODEC_INVALID;
    if (s->answer_has_audio) {
        if (s->audio_codec == ZMS_CODEC_OPUS) {
            opts.audio_codec = ZMS_CODEC_OPUS;
        } else if (s->audio_codec != ZMS_CODEC_INVALID) {
            /* 非 Opus 不送音轨，避免 AAC 进 WebRTC mux */
            ztk_info("[webrtc] mux drop non-opus audio=%s id=%s", zms_codec_name(s->audio_codec),
                     s->id);
            opts.audio_codec = ZMS_CODEC_INVALID;
        }
    }
    if (opts.audio_codec == ZMS_CODEC_OPUS) {
        opts.audio_rate = s->audio_rate > 0 ? s->audio_rate : 48000;
        opts.audio_clock_hz = (uint32_t)opts.audio_rate;
    }
    opts.video_pt = s->video_pt ? s->video_pt : 96;
    opts.audio_pt = s->audio_pt ? s->audio_pt : 97;
    opts.video_ssrc = 1;
    opts.audio_ssrc = 2;
    opts.video_seq = 1;
    opts.audio_seq = 1;
    memset(&ocfg, 0, sizeof(ocfg));
    ocfg.mux_opts = opts;
    ocfg.reader = &s->play;
    ocfg.on_rtp = webrtc_on_mux_rtp;
    ocfg.user = s;
    if (zms_rtp_play_mux_create(&opened, &ocfg) != ZTK_OK || !opened.mux) {
        return;
    }
    s->egress_pipe = opened.egress;
    s->play_rtp_muxer = opened.mux;
    zms_rtp_muxer_arm_play(s->play_rtp_muxer);
    (void)zms_rtp_play_bootstrap_live(s->play_rtp_muxer, s->source, s->play.readers.gop, s->session_no);
    if (s->play.readers.gop) {
        zms_gop_reader_seek_live_idr(s->play.readers.gop);
        if (s->play_rtp_muxer) {
            zms_egress_live_snap_gop(s->play.readers.gop, s->play_rtp_muxer);
        }
    }
    ztk_info("[webrtc] media mux ready id=%s", s->id);
}

static void webrtc_play_pump_work_impl(void *user)
{
    zms_webrtc_session *s = (zms_webrtc_session *)user;
    zms_egress_live_state live;
    int frame_budget;
    if (!s) {
        return;
    }
    s->play_pump_armed = 0;
    if (!s->play_rtp_muxer || !s->play.readers.gop) {
        return;
    }
    /* 与 zms_rtp_play_pump_run 同模式：单 tick 内 egress pump 再 SRTP/UDP flush。 */
    frame_budget =
        s->live_catchup_done ? (int)ZMS_RTP_PLAY_FRAME_BUDGET_LIVE : (int)ZMS_RTP_PLAY_FRAME_BUDGET;
    if (s->play_rtp_q_n + 16u >= ZMS_WEBRTC_PLAY_RTPQ_CAP) {
        return;
    }
    memset(&live, 0, sizeof(live));
    live.live_catchup_done = &s->live_catchup_done;
    live.pump_budget_cap = 0;
    (void)zms_egress_pipeline_pump_live(s->egress_pipe, frame_budget, 500, s->session_no, &live);
    if (s->play_rtp_q_n > 0) {
        unsigned flush_left = s->live_catchup_done ? (unsigned)ZMS_WEBRTC_PLAY_RTP_FLUSH
                                                   : (unsigned)ZMS_RTP_PLAY_RTP_FLUSH;
        while (s->play_rtp_q_n > 0 && flush_left > 0) {
            unsigned sent = webrtc_play_rtp_q_flush(s, flush_left);
            if (sent == 0) {
                break;
            }
            if (sent >= flush_left) {
                break;
            }
            flush_left -= sent;
        }
    }
    if (++s->rtcp_tick >= 250) {
        s->rtcp_tick = 0;
        webrtc_play_send_rtcp_sr(s);
    }
}

static void webrtc_play_pump_work(void *user)
{
#if defined(_WIN32)
    zms_webrtc_session *s = (zms_webrtc_session *)user;
    __try {
        webrtc_play_pump_work_impl(user);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (s) {
            s->play_pump_armed = 0;
        }
        ztk_warn("[webrtc] FAULT pump id=%s code=0x%08x step=%u q=%u", s ? s->id : "?",
                 (unsigned)GetExceptionCode(), s ? s->play_trace_seq : 0u,
                 s ? s->play_rtp_q_n : 0u);
    }
#else
    webrtc_play_pump_work_impl(user);
#endif
}

static uint64_t webrtc_play_pump_timer_impl(void *user)
{
    zms_webrtc_session *s = (zms_webrtc_session *)user;
    if (!s || !s->poller) {
        return 0;
    }
    webrtc_play_pump_work(s);
    return s->play_rtp_q_n > 0 ? 1 : 5;
}

static uint64_t webrtc_play_pump_timer(void *user)
{
#if defined(_WIN32)
    zms_webrtc_session *s = (zms_webrtc_session *)user;
    __try {
        return webrtc_play_pump_timer_impl(user);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ztk_warn("[webrtc] FAULT id=%s code=0x%08x step=%u q=%u", s ? s->id : "?",
                 (unsigned)GetExceptionCode(), s ? s->play_trace_seq : 0u,
                 s ? s->play_rtp_q_n : 0u);
        return 5;
    }
#else
    return webrtc_play_pump_timer_impl(user);
#endif
}

static void webrtc_play_start_media(zms_webrtc_session *s)
{
    uint8_t key[ZMS_WEBRTC_SRTP_KEY_LEN];
    uint8_t salt[ZMS_WEBRTC_SRTP_SALT_LEN];
    if (!s || s->media_started || s->mode != ZMS_WEBRTC_SESSION_PLAY) {
        return;
    }
#if defined(ZMS_HAVE_WEBRTC_DTLS) && ZMS_HAVE_WEBRTC_DTLS
    if (!s->dtls || zms_webrtc_dtls_export_server_srtp(s->dtls, key, salt) != 0) {
        return;
    }
    if (!s->video_srtp) {
        s->video_srtp = zms_webrtc_srtp_create();
    }
    if (!s->audio_srtp) {
        s->audio_srtp = zms_webrtc_srtp_create();
    }
    if (!s->video_srtp || !s->audio_srtp) {
        return;
    }
    zms_webrtc_srtp_init_send(s->video_srtp, key, salt);
    zms_webrtc_srtp_init_send(s->audio_srtp, key, salt);
#else
    (void)key;
    (void)salt;
    return;
#endif
    webrtc_play_rtp_q_free(s);
    if (webrtc_play_rtp_q_ensure(s) != 0) {
        return;
    }
    if (zms_webrtc_session_io_buf_ensure(s) != 0) {
        return;
    }
    zms_webrtc_srtp_bind_scratch(s->video_srtp, s->io_buf, s->io_cap);
    zms_webrtc_srtp_bind_scratch(s->audio_srtp, s->io_buf, s->io_cap);
    webrtc_play_arm_mux(s);
    if (!s->play_rtp_muxer) {
        return;
    }
    webrtc_play_load_h264_ps(s);
    s->media_started = 1;
    s->live_catchup_done = 1;
    if (s->poller && !s->pump_timer) {
        s->pump_timer = ztk_poller_do_delay(s->poller, 0, webrtc_play_pump_timer, s);
    }
}

static uint64_t webrtc_play_deferred_start(void *user)
{
    webrtc_play_start_media((zms_webrtc_session *)user);
    return 0;
}

static ztk_err_t webrtc_handle_stun(zms_webrtc_session *s, const uint8_t *data, size_t len)
{
    uint8_t reply[128];
    size_t reply_len;
    uint32_t mapped_ip;
    if (!s->local_pwd[0]) {
        return ZTK_ERR_INVALID;
    }
    if (!zms_webrtc_stun_is_binding_req(data, len)) {
        return ZTK_ERR_INVALID;
    }
    mapped_ip = webrtc_ipv4_be(s->peer_ip);
    reply_len = zms_webrtc_stun_binding_reply(data, len, reply, sizeof(reply), mapped_ip,
                                              s->peer_port, s->local_pwd);
    if (reply_len == 0) {
        return ZTK_ERR_INVALID;
    }
    webrtc_send_udp(s, reply, reply_len);
    if (s->mode == ZMS_WEBRTC_SESSION_PUBLISH) {
        zms_webrtc_session_try_dtls_client(s);
    }
    return ZTK_OK;
}

static struct zms_webrtc_dtls *webrtc_session_get_dtls(zms_webrtc_session *s)
{
#if defined(ZMS_HAVE_WEBRTC_DTLS) && ZMS_HAVE_WEBRTC_DTLS
    if (!s->dtls) {
        if (s->dtls_as_client) {
            s->dtls = zms_webrtc_dtls_create_client();
        } else {
            s->dtls = zms_webrtc_dtls_create();
        }
    }
#endif
    return s->dtls;
}

static ztk_err_t webrtc_handle_dtls(zms_webrtc_session *s, const uint8_t *data, size_t len)
{
    uint8_t out[ZMS_WEBRTC_PLAY_DTLS_IO];
    size_t out_len = 0;
    int st;
#if !defined(ZMS_HAVE_WEBRTC_DTLS) || !ZMS_HAVE_WEBRTC_DTLS
    (void)data;
    (void)len;
    (void)out;
    return ZTK_ERR_NOT_IMPL;
#else
    if (!s->dtls) {
        s->dtls = webrtc_session_get_dtls(s);
    }
    if (!s->dtls) {
        return ZTK_ERR_STATE;
    }
    st = zms_webrtc_dtls_input(s->dtls, data, len, out, sizeof(out), &out_len);
    if (out_len > 0) {
        webrtc_send_udp(s, out, out_len);
    }
    if (st < 0) {
        return ZTK_ERR_STATE;
    }
    if (st == 1 && !s->dtls_ready) {
        s->dtls_ready = 1;
        if (s->mode == ZMS_WEBRTC_SESSION_PLAY && s->poller) {
            (void)ztk_poller_do_delay(s->poller, 0, webrtc_play_deferred_start, s);
        }
    }
    return ZTK_OK;
#endif
}

ztk_err_t zms_webrtc_play_on_stun_dtls(struct zms_webrtc_session *s, const void *data, size_t len)
{
    const uint8_t *pkt = (const uint8_t *)data;
    if (!s || !pkt || len == 0) {
        return ZTK_ERR_INVALID;
    }
    if (zms_webrtc_stun_is_binding_req(pkt, len)) {
        return webrtc_handle_stun(s, pkt, len);
    }
    if (zms_webrtc_packet_is_dtls(pkt, len)) {
        static unsigned dtls_log;
        if (dtls_log < 8) {
            ++dtls_log;
            ztk_info("[webrtc] DTLS in id=%s len=%zu peer=%s:%u", s->id, len, s->peer_ip,
                     (unsigned)s->peer_port);
        }
        return webrtc_handle_dtls(s, pkt, len);
    }
    return ZTK_ERR_INVALID;
}

void zms_webrtc_play_input(struct zms_webrtc_session *s, const void *data, size_t len)
{
    const uint8_t *pkt = (const uint8_t *)data;
    zms_webrtc_gateway_pkt_kind kind;
    if (!s || !pkt || len == 0) {
        return;
    }
    if (zms_webrtc_session_io_buf_ensure(s) != 0) {
        return;
    }
    kind = zms_webrtc_gateway_classify(pkt, len);
    if (kind == ZMS_WEBRTC_GATEWAY_SRTCP) {
        size_t rtcp_len = len;
        if (rtcp_len > s->io_cap || !s->io_buf) {
            return;
        }
        memcpy(s->io_buf, pkt, rtcp_len);
        if (zms_webrtc_srtcp_unprotect(s->video_srtp, s->io_buf, &rtcp_len) == 0) {
            zms_webrtc_rtcp_for_each_compound(s->io_buf, rtcp_len, webrtc_on_rtcp_block, s);
            return;
        }
        rtcp_len = len;
        memcpy(s->io_buf, pkt, rtcp_len);
        if (s->audio_srtp && zms_webrtc_srtcp_unprotect(s->audio_srtp, s->io_buf, &rtcp_len) == 0) {
            zms_webrtc_rtcp_for_each_compound(s->io_buf, rtcp_len, webrtc_on_rtcp_block, s);
            return;
        }
        zms_webrtc_rtcp_for_each_compound(pkt, len, webrtc_on_rtcp_block, s);
        return;
    }
    if (kind == ZMS_WEBRTC_GATEWAY_DTLS) {
        (void)webrtc_handle_dtls(s, pkt, len);
    }
}

void zms_webrtc_play_stop(struct zms_webrtc_session *s)
{
    if (!s) {
        return;
    }
    webrtc_play_srtp_verify_reset();
    s->play_pump_armed = 0;
    webrtc_play_rtp_q_free(s);
    if (s->pump_timer) {
        ztk_poller_timer_cancel(s->pump_timer);
        s->pump_timer = NULL;
    }
    if (s->egress_pipe) {
        zms_rtp_play_mux_handle h = {s->egress_pipe, s->play_rtp_muxer};
        zms_rtp_play_mux_destroy(&h);
        s->egress_pipe = NULL;
        s->play_rtp_muxer = NULL;
    }
#if defined(ZMS_HAVE_WEBRTC_DTLS) && ZMS_HAVE_WEBRTC_DTLS
    if (s->dtls) {
        zms_webrtc_dtls_destroy(s->dtls);
        s->dtls = NULL;
    }
    zms_webrtc_srtp_destroy(s->video_srtp);
    s->video_srtp = NULL;
    zms_webrtc_srtp_destroy(s->audio_srtp);
    s->audio_srtp = NULL;
#endif
    s->media_started = 0;
    s->dtls_ready = 0;
    s->rtcp_video_sr_sent = 0;
    s->rtcp_audio_sr_sent = 0;
}
