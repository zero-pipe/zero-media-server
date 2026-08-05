#include "live/publish/webrtc/webrtc_whip_ingress.h"
#include "webrtc/whep/whep_play_session.h"
#include "webrtc/session/webrtc_media_gateway.h"
#include "webrtc/session/webrtc_media_internal.h"
#include "webrtc/session/webrtc_session_internal.h"
#include "zms/engine/media_event.h"
#include "zms/media/codec/payload/payload_registry.h"
#include "zms/live/ingest/common/stream_ingest.h"
#include "zms/engine/media/media_limits.h"
#include "zms/media/wire/rtp_packet.h"
#include "zms/webrtc/webrtc_srtp.h"
#include "ztk/util/log.h"
#include <stdlib.h>
#include <string.h>
static unsigned g_whip_srtp_ok;
static unsigned g_whip_srtp_fail;
typedef struct zms_webrtc_whip_ingress_ctx {
    zms_webrtc_session *session;
    zms_webrtc_gateway_ingest_cfg gw;
    const zms_payload_demux_ops *v_ops;
    const zms_payload_demux_ops *a_ops;
    void *v_demux;
    void *a_demux;
} zms_webrtc_whip_ingress_ctx;
static void whip_store_h264_nal(zms_webrtc_session *s, const uint8_t *nal, size_t nlen)
{
    int type;
    if (!s || !nal || nlen == 0) {
        return;
    }
    type = nal[0] & 0x1f;
    if (type == 7 && nlen <= sizeof(s->whip_h264_sps)) {
        memcpy(s->whip_h264_sps, nal, nlen);
        s->whip_h264_sps_len = nlen;
    } else if (type == 8 && nlen <= sizeof(s->whip_h264_pps)) {
        memcpy(s->whip_h264_pps, nal, nlen);
        s->whip_h264_pps_len = nlen;
    }
}

static void whip_try_apply_h264_ps(zms_webrtc_session *s)
{
    if (!s || !s->ingest) {
        return;
    }
    if (s->source && s->source->video.ready) {
        return;
    }
    if (s->whip_h264_sps_len == 0 || s->whip_h264_pps_len == 0) {
        return;
    }
    if (zms_live_ingest_set_h264_sps_pps(s->ingest, s->whip_h264_sps, s->whip_h264_sps_len,
                                         s->whip_h264_pps, s->whip_h264_pps_len) == ZTK_OK) {
        ztk_info("[webrtc] WHIP applied inband H264 SPS=%zu PPS=%zu app=%s stream=%s",
                 s->whip_h264_sps_len, s->whip_h264_pps_len, s->app, s->stream);
    }
}
/** WHIP 客户端常将 SPS/PPS 作为独立单 NAL RTP 包发送（offer 无 sprop）。 */
static void whip_collect_h264_rtp_ps(zms_webrtc_session *s, const uint8_t *payload, size_t size)
{
    size_t off;
    if (!s || !payload || size < 2 || s->video_codec != ZMS_CODEC_H264) {
        return;
    }
    if (s->source && s->source->video.ready) {
        return;
    }
    off = 0;
    while (off < size) {
        int type = payload[off] & 0x1f;
        if (type >= 1 && type <= 23) {
            whip_store_h264_nal(s, payload + off, size - off);
            break;
        }
        if (type == 24) {
            size_t p = off + 1;
            while (p + 2 <= size) {
                size_t nlen = ((size_t)payload[p] << 8) | payload[p + 1];
                p += 2;
                if (nlen == 0 || p + nlen > size) {
                    break;
                }
                whip_store_h264_nal(s, payload + p, nlen);
                p += nlen;
            }
            break;
        }
        break;
    }
    whip_try_apply_h264_ps(s);
}

static void whip_on_frame(const zms_frame *frame, void *user)
{
    zms_webrtc_whip_ingress_ctx *ctx = (zms_webrtc_whip_ingress_ctx *)user;
    if (!ctx || !ctx->session || !ctx->session->ingest || !frame || !frame->data ||
        frame->size == 0) {
        return;
    }
    (void)zms_live_ingest_input_frame(ctx->session->ingest, frame);
}

static void *whip_create_demux(zms_codec_id codec, uint8_t pt, uint32_t clock_hz,
                               zms_webrtc_whip_ingress_ctx *ctx,
                               const zms_payload_demux_ops **ops_out)
{
    const zms_payload_demux_ops *ops;
    zms_payload_demux_opts opts;
    ops = zms_payload_demux_find(codec, ZMS_WIRE_FORMAT_RTP);
    if (!ops || !ops->create) {
        return NULL;
    }
    memset(&opts, 0, sizeof(opts));
    opts.codec = codec;
    opts.wire = ZMS_WIRE_FORMAT_RTP;
    opts.rtp_clock_hz = clock_hz;
    opts.payload_type = pt;
    opts.on_frame = whip_on_frame;
    opts.user = ctx;
    if (ops_out) {
        *ops_out = ops;
    }
    return ops->create(&opts);
}

static void webrtc_whip_start_media(zms_webrtc_session *s)
{
    zms_webrtc_whip_ingress_ctx *ctx;
    uint8_t key[ZMS_WEBRTC_SRTP_KEY_LEN];
    uint8_t salt[ZMS_WEBRTC_SRTP_SALT_LEN];
    if (!s || s->media_started) {
        return;
    }
#if defined(ZMS_HAVE_WEBRTC_DTLS) && ZMS_HAVE_WEBRTC_DTLS
    if (!s->dtls || zms_webrtc_dtls_export_client_srtp(s->dtls, key, salt) != 0) {
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
    zms_webrtc_srtp_init_recv(s->video_srtp, key, salt);
    zms_webrtc_srtp_init_recv(s->audio_srtp, key, salt);
    if (zms_webrtc_session_io_buf_ensure(s) != 0) {
        return;
    }
    zms_webrtc_srtp_bind_scratch(s->video_srtp, s->io_buf, s->io_cap);
    zms_webrtc_srtp_bind_scratch(s->audio_srtp, s->io_buf, s->io_cap);
#else
    return;
#endif
    ctx = (zms_webrtc_whip_ingress_ctx *)calloc(1, sizeof(*ctx));
    if (!ctx) {
        return;
    }
    ctx->session = s;
    zms_webrtc_gateway_ingest_cfg_defaults(&ctx->gw);
    ctx->gw.answer_has_video = s->answer_has_video;
    ctx->gw.answer_has_audio = s->answer_has_audio;
    ctx->gw.wire_video_pt = s->video_pt;
    ctx->gw.wire_audio_pt = s->audio_pt;
    if (s->answer_has_video && s->video_codec != ZMS_CODEC_INVALID) {
        ctx->v_demux =
            whip_create_demux(s->video_codec, ctx->gw.canon_video_pt, 90000u, ctx, &ctx->v_ops);
        if (!ctx->v_demux) {
            free(ctx);
            return;
        }
    }
    if (s->answer_has_audio && s->audio_codec != ZMS_CODEC_INVALID) {
        uint32_t ahz = s->audio_rate > 0 ? (uint32_t)s->audio_rate : 48000u;
        ctx->a_demux =
            whip_create_demux(s->audio_codec, ctx->gw.canon_audio_pt, ahz, ctx, &ctx->a_ops);
        if (!ctx->a_demux) {
            if (ctx->v_ops && ctx->v_demux) {
                ctx->v_ops->destroy(ctx->v_demux);
            }
            free(ctx);
            return;
        }
    }
    if (s->ingest) {
        zms_codec_id ac = s->answer_has_audio ? s->audio_codec : ZMS_CODEC_INVALID;
        uint32_t ahz = s->audio_rate > 0 ? (uint32_t)s->audio_rate : 48000u;
        zms_live_ingest_set_rtp_clocks(s->ingest, 90000, ac, ahz);
    }
    s->whip_ingress = ctx;
    s->media_started = 1;
    zms_webrtc_publish_apply_offer_h264_sprop(s);
    if (s->source && !s->source->publishing) {
        zms_media_event_publish(s->source, ZMS_ORIGIN_WEBRTC_PUSH);
    }
    ztk_info("[webrtc] WHIP publish media ready id=%s video=%s audio=%s app=%s stream=%s", s->id,
             zms_codec_name(s->video_codec), zms_codec_name(s->audio_codec), s->app, s->stream);
}

static void webrtc_whip_on_rtp(zms_webrtc_session *s, uint8_t *rtp, size_t len,
                               struct zms_webrtc_srtp *srtp, int is_video)
{
    zms_webrtc_whip_ingress_ctx *ctx;
    zms_rtp_packet pkt;
    const zms_payload_demux_ops *ops;
    void *demux;
    if (!s || !rtp || len < 12 || !srtp) {
        return;
    }
    ctx = (zms_webrtc_whip_ingress_ctx *)s->whip_ingress;
    if (!ctx) {
        return;
    }
    if (zms_webrtc_srtp_unprotect(srtp, rtp, &len) != 0) {
        ++g_whip_srtp_fail;
        if (g_whip_srtp_fail <= 5 || (g_whip_srtp_fail % 500) == 0) {
            ztk_warn("[webrtc] SRTP unprotect fail total=%u", g_whip_srtp_fail);
        }
        return;
    }
    ++g_whip_srtp_ok;
    zms_webrtc_gateway_remap_pt(rtp, len, &ctx->gw, is_video);
    if (zms_rtp_parse(rtp, len, &pkt) != ZTK_OK) {
        return;
    }
    if (g_whip_srtp_ok <= 8 && pkt.payload && pkt.payload_size > 0) {
        ztk_info("[webrtc] SRTP ok #%u seq=%u ext=%u pt=%u pay0=%02x pay1=%02x len=%zu",
                 g_whip_srtp_ok, pkt.hdr.seq, pkt.hdr.extension, pkt.hdr.pt, pkt.payload[0],
                 pkt.payload_size > 1 ? pkt.payload[1] : 0, pkt.payload_size);
    }
    if (is_video && pkt.payload && pkt.payload_size > 0) {
        whip_collect_h264_rtp_ps(s, pkt.payload, pkt.payload_size);
    }
    if (is_video) {
        ops = ctx->v_ops;
        demux = ctx->v_demux;
    } else {
        ops = ctx->a_ops;
        demux = ctx->a_demux;
    }
    if (ops && demux && ops->input_rtp) {
        (void)ops->input_rtp(demux, &pkt, NULL);
    }
}

static void webrtc_whip_on_srtcp(zms_webrtc_session *s, const uint8_t *wire, size_t wire_len)
{
    size_t plain_len;
    uint8_t *rtcp_slot;
    if (!s || !wire || wire_len < 8 || !s->io_buf || wire_len > s->io_cap) {
        return;
    }
    rtcp_slot = s->io_buf + ZMS_WEBRTC_PLAY_RTP_SLOT_MAX;
    if (wire_len > s->io_cap - ZMS_WEBRTC_PLAY_RTP_SLOT_MAX) {
        return;
    }
    memcpy(rtcp_slot, wire, wire_len);
    plain_len = wire_len;
    if (zms_webrtc_srtcp_unprotect(s->video_srtp, rtcp_slot, &plain_len) != 0) {
        if (zms_webrtc_rtcp_is_psfb_pli(wire, wire_len) && s->ingest) {
            zms_live_ingest_reset_upstream(s->ingest);
        }
        return;
    }
    if (zms_webrtc_rtcp_is_psfb_pli(rtcp_slot, plain_len) && s->ingest) {
        zms_live_ingest_reset_upstream(s->ingest);
    }
}

void zms_webrtc_whip_ingress_on_udp(zms_webrtc_session *s, const void *data, size_t len)
{
    const uint8_t *pkt = (const uint8_t *)data;
    zms_webrtc_gateway_pkt_kind kind;
    zms_webrtc_whip_ingress_ctx *ctx;
    int track;
    uint8_t pt;
    if (!s || !pkt || len == 0) {
        return;
    }
    kind = zms_webrtc_gateway_classify(pkt, len);
    if (kind == ZMS_WEBRTC_GATEWAY_STUN || kind == ZMS_WEBRTC_GATEWAY_DTLS) {
        if (!s->dtls_ready) {
            (void)zms_webrtc_play_on_stun_dtls(s, data, len);
            if (s->dtls_ready) {
                webrtc_whip_start_media(s);
            }
        } else {
            (void)zms_webrtc_play_on_stun_dtls(s, data, len);
        }
        return;
    }
    if (!s->dtls_ready || !s->media_started) {
        return;
    }
    ctx = (zms_webrtc_whip_ingress_ctx *)s->whip_ingress;
    if (kind == ZMS_WEBRTC_GATEWAY_SRTCP) {
        if (!s->io_buf || len > s->io_cap) {
            return;
        }
        webrtc_whip_on_srtcp(s, pkt, len);
        return;
    }
    if (kind != ZMS_WEBRTC_GATEWAY_SRTP) {
        return;
    }
    if (!s->io_buf || len + ZMS_WEBRTC_PLAY_RTP_SLOT_MAX > s->io_cap) {
        return;
    }
    pt = zms_rtp_payload_type(pkt, len);
    track = ctx ? zms_webrtc_gateway_match_media_track(&ctx->gw, pt) : -1;
    if (track < 0) {
        return;
    }
    {
        uint8_t *rtp_slot = s->io_buf + ZMS_WEBRTC_PLAY_RTP_SLOT_MAX;
        memcpy(rtp_slot, pkt, len);
        webrtc_whip_on_rtp(s, rtp_slot, len, track ? s->video_srtp : s->audio_srtp, track);
    }
}

void zms_webrtc_whip_ingress_stop(zms_webrtc_session *s)
{
    zms_webrtc_whip_ingress_ctx *ctx;
    if (!s) {
        return;
    }
    ctx = (zms_webrtc_whip_ingress_ctx *)s->whip_ingress;
    if (ctx) {
        if (ctx->v_ops && ctx->v_demux) {
            ctx->v_ops->destroy(ctx->v_demux);
        }
        if (ctx->a_ops && ctx->a_demux) {
            ctx->a_ops->destroy(ctx->a_demux);
        }
        free(ctx);
        s->whip_ingress = NULL;
    }
    zms_webrtc_play_stop(s);
    if (s->ingest) {
        if (s->source && s->source->publishing) {
            zms_media_event_publish_fini(s->source, ZMS_ORIGIN_WEBRTC_PUSH);
        }
        zms_live_ingest_destroy(s->ingest);
        s->ingest = NULL;
        s->source = NULL;
    }
}
