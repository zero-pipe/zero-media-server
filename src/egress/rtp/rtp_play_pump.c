#include "zms/engine/media_clock.h"
#include "zms/egress/egress_pipeline.h"
#include "zms/egress/rtp/rtp_play_pump.h"
#include "zms/egress/egress_clock.h"

#include "zms/media/container/flv/flv_tag_probe.h"

#include "zms/media/codec/h264/h264_es.h"
#include "zms/media/codec/h265/h265_es.h"
#include "zms/media/codec/av1/av1_over_rtmp.h"
#include "zms/egress/egress_live_policy.h"
#include "zms/egress/rtp/rtp_muxer.h"
#include "zms/engine/gop/gop_queue.h"
#include "zms/vod/io/vod_buffer.h"
#include "zms/vod/play/vod_play_lane.h"

#include "ztk/util/buf.h"
#include "ztk/util/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ZMS_RTP_PLAY_SLOT_MAX 2048u
#define ZMS_RTP_PLAY_PACE_LEAD_MS 120u

typedef struct {
    uint8_t ch;
    ztk_buf *buf;
} rtsp_play_slot;

struct zms_rtp_play_sender {
    zms_rtp_play_write_fn write;
    void *user;
    ztk_poller *poller;
    rtsp_play_slot *slots;
    unsigned cap;
    size_t r;
    size_t w;
    size_t n;
    unsigned session_no;
};

static int g_play_sender_flush_depth;

static int play_pump_abort(const zms_rtp_play_pump *p)
{
    if (!p) {
        return 1;
    }
    if (p->destroy_flag && *p->destroy_flag) {
        return 1;
    }
    if (p->close_flag && *p->close_flag) {
        return 1;
    }
    return 0;
}

static size_t rtsp_play_pump_pending(const zms_rtp_play_pump *p)
{
    if (!p) {
        return 0;
    }
    if (p->vod_reader) {
        return zms_vod_buffer_reader_lag(p->vod_reader);
    }
    if (p->gop_reader) {
        return zms_gop_reader_lag(p->gop_reader);
    }
    return 0;
}

zms_rtp_play_sender *zms_rtp_play_sender_create(zms_rtp_play_write_fn write, void *user,
                                                unsigned cap)
{
    if (!write || cap == 0) {
        cap = ZMS_RTP_PLAY_RTPQ_CAP;
    }
    zms_rtp_play_sender *s = (zms_rtp_play_sender *)calloc(1, sizeof(*s));
    if (!s) {
        return NULL;
    }
    s->slots = (rtsp_play_slot *)calloc(cap, sizeof(rtsp_play_slot));
    if (!s->slots) {
        free(s);
        return NULL;
    }
    s->write = write;
    s->user = user;
    s->cap = cap;
    return s;
}

void zms_rtp_play_sender_destroy(zms_rtp_play_sender *s)
{
    size_t i;

    if (!s) {
        return;
    }
    if (s->slots) {
        for (i = 0; i < s->cap; ++i) {
            if (s->slots[i].buf) {
                ztk_buf_unref(s->slots[i].buf);
            }
        }
        free(s->slots);
    }
    free(s);
}

void zms_rtp_play_sender_reset(zms_rtp_play_sender *s)
{
    size_t i;

    if (!s) {
        return;
    }
    if (s->slots) {
        for (i = 0; i < s->cap; ++i) {
            if (s->slots[i].buf) {
                ztk_buf_unref(s->slots[i].buf);
                s->slots[i].buf = NULL;
            }
        }
    }
    s->r = s->w = s->n = 0;
}

size_t zms_rtp_play_sender_pending(const zms_rtp_play_sender *s)
{
    return s ? s->n : 0;
}

void zms_rtp_play_sender_set_session_no(zms_rtp_play_sender *s, unsigned session_no)
{
    if (s) {
        s->session_no = session_no;
    }
}

void zms_rtp_play_sender_set_poller(zms_rtp_play_sender *s, ztk_poller *poller)
{
    if (s) {
        s->poller = poller;
    }
}

static int play_sender_push(zms_rtp_play_sender *s, uint8_t ch, const uint8_t *data, size_t len)
{
    rtsp_play_slot *slot;
    ztk_buf *buf;
    void *dst;

    if (!s || !data || len == 0 || len > ZMS_RTP_PLAY_SLOT_MAX) {
        return -1;
    }
    if (s->n >= s->cap) {
        rtsp_play_slot *drop = &s->slots[s->r];
        if (drop->buf) {
            ztk_buf_unref(drop->buf);
        }
        drop->buf = NULL;
        s->r = (s->r + 1) % s->cap;
        s->n--;
        if (s->session_no) {
            ztk_warn("[rtsp] rtpq_drop_oldest session=%u cap=%u", s->session_no, s->cap);
        }
    }
    buf = ztk_buf_alloc_local(s->poller, len);
    if (!buf) {
        return -1;
    }
    dst = (void *)ztk_buf_data(buf);
    memcpy(dst, data, len);
    ztk_buf_set_len(buf, len);
    slot = &s->slots[s->w];
    slot->ch = ch;
    slot->buf = buf;
    s->w = (s->w + 1) % s->cap;
    s->n++;
    return 0;
}

static void play_sender_flush_inner(zms_rtp_play_sender *s, int budget, int close_pending)
{
    if (!s || !s->write || budget <= 0 || close_pending) {
        return;
    }
    while (budget-- > 0 && s->n > 0) {
        rtsp_play_slot *slot = &s->slots[s->r];
        if (slot->buf) {
            s->write(s->user, slot->ch, (const uint8_t *)ztk_buf_data(slot->buf),
                     ztk_buf_len(slot->buf));
            ztk_buf_unref(slot->buf);
            slot->buf = NULL;
        }
        s->r = (s->r + 1) % s->cap;
        s->n--;
    }
}

void zms_rtp_play_sender_flush(zms_rtp_play_sender *s, int budget, int close_pending)
{
    if (!s || budget <= 0 || g_play_sender_flush_depth > 0) {
        return;
    }
    g_play_sender_flush_depth++;
    play_sender_flush_inner(s, budget, close_pending);
    g_play_sender_flush_depth--;
}

void zms_rtp_play_sender_submit(zms_rtp_play_sender *s, int queued, uint8_t channel,
                                const uint8_t *payload, size_t len)
{
    if (!s || !payload || len == 0) {
        return;
    }
    if (!queued || (s->n == 0 && g_play_sender_flush_depth == 0)) {
        if (s->write) {
            s->write(s->user, channel, payload, len);
        }
        return;
    }
    if (play_sender_push(s, channel, payload, len) != 0) {
        if (s->write) {
            s->write(s->user, channel, payload, len);
        }
    } else if (s->n >= ZMS_RTP_PLAY_RTPQ_HIGH_WATER) {
        zms_rtp_play_sender_flush(s, (int)ZMS_RTP_PLAY_RTPQ_HIGH_WATER, 0);
    }
}

static void rtsp_play_send_stream_config(zms_rtp_muxer *mux, const zms_media_source *src,
                                         zms_vod_play_lane *lane, uint32_t anchor_ms)
{
    size_t vcfg_len = 0;
    const uint8_t *vcfg = NULL;

    if (!mux || !src) {
        return;
    }
    if (lane) {
        vcfg = zms_vod_play_lane_video_config(lane, &vcfg_len);
    }
    if ((!vcfg || vcfg_len <= 5) && src) {
        vcfg = zms_media_source_video_config(src, &vcfg_len);
    }
    if (vcfg && vcfg_len > 5) {
        zms_codec_id vc = zms_flv_video_config_codec(vcfg, vcfg_len);
        if (vc == ZMS_CODEC_INVALID && src->video.codec != ZMS_CODEC_INVALID) {
            vc = src->video.codec;
        }
        if (vc == ZMS_CODEC_H264) {
            zms_rtp_muxer_send_avc_config(mux, vcfg, vcfg_len, anchor_ms);
        } else if (vc == ZMS_CODEC_H265) {
            zms_rtp_muxer_send_hevc_config(mux, vcfg, vcfg_len, anchor_ms);
        } else if (vc == ZMS_CODEC_AV1) {
            zms_rtp_muxer_send_av1_config(mux, vcfg, vcfg_len, anchor_ms);
        }
    }
}

static void rtp_play_on_snap(void *user)
{
    zms_rtp_play_pump *p = (zms_rtp_play_pump *)user;

    if (!p) {
        return;
    }
    if (p->sender) {
        zms_rtp_play_sender_reset(p->sender);
    }
    if (p->mux && p->source) {
        rtsp_play_send_stream_config(p->mux, p->source, NULL, 0);
    }
}

static void rtp_play_on_slow_consumer(void *user)
{
    zms_rtp_play_pump *p = (zms_rtp_play_pump *)user;

    if (!p || !p->close_flag) {
        return;
    }
    *(int *)p->close_flag = 1;
}

static const char *rtsp_play_attach_kind_slot(const zms_gop_slot *slot)
{
    if (!slot || slot->track != ZMS_TRACK_VIDEO || !slot->data || slot->len < 5) {
        return "none";
    }
    if (slot->codec == ZMS_CODEC_H265) {
        if (zms_h265_annexb_is_idr(slot->data, slot->len)) {
            return "IDR";
        }
        if (zms_h265_annexb_is_sync_key(slot->data, slot->len)) {
            return "CRA";
        }
        return "VCL";
    }
    if (slot->codec == ZMS_CODEC_H264) {
        if (zms_h264_annexb_is_idr(slot->data, slot->len) || slot->keyframe) {
            return "IDR";
        }
        if (zms_h264_annexb_is_sync_key(slot->data, slot->len)) {
            return "sync";
        }
        return "VCL";
    }
    if (slot->codec == ZMS_CODEC_AV1) {
        if (slot->keyframe || zms_av1_obu_has_sequence_header(slot->data, slot->len)) {
            return "key";
        }
        return "VCL";
    }
    return slot->keyframe ? "key" : "VCL";
}

int zms_rtp_play_bootstrap_live(zms_rtp_muxer *mux, const zms_media_source *src,
                                zms_gop_reader *reader, unsigned session_no)
{
    size_t lag = 0;

    if (!mux || !src || !src->gop_queue) {
        return 0;
    }

    rtsp_play_send_stream_config(mux, src, NULL, 0);

    if (reader) {
        zms_egress_live_seek_rtsp_attach(reader);
        lag = zms_gop_reader_lag(reader);
    }

    if (session_no) {
        zms_gop_slot at;

        if (reader && zms_gop_reader_slot_at_read(reader, &at) && at.track == ZMS_TRACK_VIDEO) {
            const char *kind = rtsp_play_attach_kind_slot(&at);

            if (kind[0] == 'V' || kind[0] == 'n') {
                ztk_debug("[rtsp] bootstrap_wait session=%u attach=%s ts=%u lag=%zu", session_no,
                          kind, (unsigned)at.dts_ms, lag);
            } else {
                ztk_debug("[rtsp] bootstrap_ready session=%u attach=%s ts=%u lag=%zu", session_no,
                          kind, (unsigned)at.dts_ms, lag);
            }
        } else {
            ztk_debug("[rtsp] bootstrap_no_video session=%u lag=%zu", session_no, lag);
        }
    }

    return 1;
}

int zms_rtp_play_bootstrap_live_edge(zms_rtp_muxer *mux, const zms_media_source *src,
                                     zms_gop_reader *reader, unsigned session_no)
{
    size_t lag = 0;

    if (!mux || !src || !src->gop_queue) {
        return 0;
    }

    rtsp_play_send_stream_config(mux, src, NULL, 0);

    if (reader) {
        zms_gop_reader_seek_live_idr(reader);
        lag = zms_gop_reader_lag(reader);
    }

    if (session_no) {
        zms_gop_slot at;

        if (reader && zms_gop_reader_slot_at_read(reader, &at) && at.track == ZMS_TRACK_VIDEO) {
            const char *kind = rtsp_play_attach_kind_slot(&at);

            if (kind[0] == 'V' || kind[0] == 'n') {
                ztk_debug("[rtsp] bootstrap_edge_wait session=%u attach=%s ts=%u lag=%zu",
                          session_no, kind, (unsigned)at.dts_ms, lag);
            } else {
                ztk_debug("[rtsp] bootstrap_edge_ready session=%u attach=%s ts=%u lag=%zu",
                          session_no, kind, (unsigned)at.dts_ms, lag);
            }
        } else {
            ztk_debug("[rtsp] bootstrap_edge_no_video session=%u lag=%zu", session_no, lag);
        }
    }

    return 1;
}

int zms_rtp_play_bootstrap_vod(zms_rtp_muxer *mux, const zms_media_source *src,
                               zms_vod_buffer_reader *reader, unsigned session_no,
                               zms_vod_play_lane *lane, uint32_t anchor_ms)
{
    (void)reader;
    if (!mux || !src) {
        return 0;
    }
    if (!anchor_ms) {
        anchor_ms = zms_rtp_muxer_vod_anchor_ms(mux);
    }
    rtsp_play_send_stream_config(mux, src, lane, anchor_ms);
    if (session_no) {
        ztk_debug("[rtsp] bootstrap_vod session=%u anchor_ms=%u lane=%d", session_no,
                  (unsigned)anchor_ms, lane ? 1 : 0);
    }
    return 1;
}

int zms_rtp_play_pump_run(zms_rtp_play_pump *p, int frame_budget, int rtp_flush_budget)
{
    if (!p || !p->mux || !p->sender || play_pump_abort(p)) {
        return 0;
    }

    if (frame_budget <= 0) {
        frame_budget = (int)ZMS_RTP_PLAY_FRAME_BUDGET_LIVE;
    }
    if (rtp_flush_budget <= 0) {
        rtp_flush_budget = ZMS_RTP_PLAY_RTP_FLUSH;
    }

    int frames = 0;
    int pump_budget = frame_budget;
    size_t rtpq_before = zms_rtp_play_sender_pending(p->sender);

    if (p->egress && (p->vod_reader || p->gop_reader)) {
        zms_egress_live_state live_st;
        const zms_egress_live_state *live = NULL;

        if (p->gop_reader && !p->vod_reader) {
            memset(&live_st, 0, sizeof(live_st));
            live_st.live_catchup_done = p->live_catchup_done;
            live_st.live_resync_at_ms = p->live_resync_at_ms;
            live_st.on_snap = rtp_play_on_snap;
            live_st.snap_user = p;
            live_st.on_slow_consumer = rtp_play_on_slow_consumer;
            live_st.slow_consumer_user = p;
            live = &live_st;
        }
        frames = zms_egress_pipeline_pump_rtsp(p->egress, p->vod_reader, p->vod_demux, pump_budget,
                                               500, p->session_no, live);
        if (!play_pump_abort(p)) {
            zms_rtp_play_sender_flush(p->sender, rtp_flush_budget, 0);
        }
        if (frames > 0 && p->session_no) {
            const zms_rtp_muxer_stats *st = zms_rtp_muxer_get_stats(p->mux);
            size_t pending = rtsp_play_pump_pending(p);
            if (st && p->vod_reader && zms_rtp_muxer_catchup_on(p->mux)) {
                ztk_debug("[rtsp] play_pump_catchup session=%u frames=%d rtpq=%zu->%zu lag=%zu "
                          "v_pkts=%u a_pkts=%u",
                          p->session_no, frames, rtpq_before,
                          zms_rtp_play_sender_pending(p->sender), pending, st->video_pkt_count,
                          st->audio_pkt_count);
            } else if (st && !p->vod_reader &&
                       (pending > ZMS_GOP_QUEUE_PLAY_MAX_LAG ||
                        frames >= (int)ZMS_RTP_PLAY_FRAME_BUDGET_LIVE)) {
                ztk_debug("[rtsp] play_pump session=%u frames=%d rtpq=%zu->%zu lag=%zu v_pkts=%u "
                          "a_pkts=%u",
                          p->session_no, frames, rtpq_before,
                          zms_rtp_play_sender_pending(p->sender), pending, st->video_pkt_count,
                          st->audio_pkt_count);
            }
        }
        return frames;
    }

    return 0;
}

int zms_rtsp_parse_range_npt_ms(const char *range_hdr, uint64_t *out_ms, int *out_now)
{
    char npt[64];
    const char *p, *dash;
    size_t n;
    unsigned h, m, s;
    double sec_d;

    if (out_now) {
        *out_now = 0;
    }
    if (!out_ms) {
        return 0;
    }
    *out_ms = 0;
    if (!range_hdr || !range_hdr[0]) {
        return 0;
    }
    p = range_hdr;
    while (*p == ' ' || *p == '\t') {
        ++p;
    }
    if ((p[0] != 'n' && p[0] != 'N') || (p[1] != 'p' && p[1] != 'P') ||
        (p[2] != 't' && p[2] != 'T') || p[3] != '=') {
        return 0;
    }
    p += 4;
    if ((p[0] == 'n' || p[0] == 'N') && (p[1] == 'o' || p[1] == 'O') &&
        (p[2] == 'w' || p[2] == 'W')) {
        if (out_now) {
            *out_now = 1;
        }
        return 1;
    }
    dash = strchr(p, '-');
    if (!dash) {
        dash = p + strlen(p);
    }
    n = (size_t)(dash - p);
    if (n >= sizeof(npt)) {
        n = sizeof(npt) - 1;
    }
    memcpy(npt, p, n);
    npt[n] = '\0';
    while (n > 0 && (npt[n - 1] == ' ' || npt[n - 1] == '\t')) {
        npt[--n] = '\0';
    }
    if (strchr(npt, ':')) {
        const char *dot;
        double frac = 0.0;
        if (sscanf(npt, "%u:%u:%u", &h, &m, &s) < 3) {
            return 0;
        }
        dot = strchr(npt, '.');
        if (dot) {
            frac = strtod(dot, NULL);
        }
        *out_ms = (uint64_t)h * 3600000ULL + (uint64_t)m * 60000ULL + (uint64_t)s * 1000ULL +
                  (uint64_t)(frac * 1000.0 + 0.5);
        return 1;
    }
    sec_d = strtod(npt, NULL);
    if (sec_d < 0.0) {
        sec_d = 0.0;
    }
    *out_ms = (uint64_t)(sec_d * 1000.0 + 0.5);
    return 1;
}

double zms_rtsp_parse_play_scale(const char *hdr, double current)
{
    double v;
    char *end;

    if (!hdr || !hdr[0]) {
        return current > 0.0 ? current : 1.0;
    }
    v = strtod(hdr, &end);
    if (end == hdr || v <= 0.0 || v > 16.0) {
        return current > 0.0 ? current : 1.0;
    }
    return v;
}

uint32_t zms_rtp_play_vod_anchor_ms(zms_vod_buffer_reader *rd, uint32_t seek_ms)
{
    zms_gop_slot slot;

    if (rd && zms_vod_buffer_reader_peek_muxed(rd, &slot)) {
        return slot.dts_ms;
    }
    return seek_ms;
}

void zms_rtsp_play_format_rtp_info(const zms_rtsp_play_rtp_info_args *args, char *rtp_info,
                                   size_t cap)
{
    uint32_t vhz;
    uint32_t ahz;
    uint32_t vrtp;
    uint32_t artp;
    uint16_t vseq;
    uint16_t aseq;
    const char *host;
    const char *app;
    const char *stream;

    if (!rtp_info || cap == 0) {
        return;
    }
    rtp_info[0] = '\0';
    if (!args || !args->source || !args->mux) {
        return;
    }
    host = (args->host && args->host[0]) ? args->host : "127.0.0.1";
    app = args->app ? args->app : "";
    stream = args->stream ? args->stream : "";
    vhz = args->video_clock_hz > 0 ? args->video_clock_hz : 90000u;
    ahz = args->audio_clock_hz > 0 ? args->audio_clock_hz
                                   : (uint32_t)(args->audio_rate > 0 ? args->audio_rate : 44100);
    if (args->vod_linear_rtp) {
        vrtp = (uint32_t)((uint64_t)args->anchor_ms * (uint64_t)vhz / 1000u);
        artp = (uint32_t)((uint64_t)args->anchor_ms * (uint64_t)ahz / 1000u);
    } else {
        vrtp = zms_ms_to_rtp_clock(args->anchor_ms, vhz);
        artp = zms_ms_to_rtp_clock(args->anchor_ms, ahz);
    }
    vseq = zms_rtp_muxer_video_seq(args->mux);
    aseq = zms_rtp_muxer_audio_seq(args->mux);
    if (args->source->has_video && args->source->has_audio) {
        snprintf(rtp_info, cap,
                 "RTP-Info: "
                 "url=rtsp://%s:554/%s/%s/trackID=0;seq=%u;rtptime=%u,"
                 "url=rtsp://%s:554/%s/%s/trackID=1;seq=%u;rtptime=%u\r\n",
                 host, app, stream, (unsigned)vseq, (unsigned)vrtp, host, app, stream,
                 (unsigned)aseq, (unsigned)artp);
    } else if (args->source->has_video) {
        snprintf(rtp_info, cap, "RTP-Info: url=rtsp://%s:554/%s/%s/trackID=0;seq=%u;rtptime=%u\r\n",
                 host, app, stream, (unsigned)vseq, (unsigned)vrtp);
    } else if (args->source->has_audio) {
        snprintf(rtp_info, cap, "RTP-Info: url=rtsp://%s:554/%s/%s/trackID=1;seq=%u;rtptime=%u\r\n",
                 host, app, stream, (unsigned)aseq, (unsigned)artp);
    }
}

ztk_err_t zms_rtp_play_mux_create(zms_rtp_play_mux_handle *out, const zms_rtp_play_mux_opts *opts)
{
    zms_egress_pipeline_opts ecfg;
    zms_egress_rtsp_bind bind;

    if (!out || !opts || !opts->reader || !opts->on_rtp) {
        return ZTK_ERR_INVALID;
    }
    memset(out, 0, sizeof(*out));
    memset(&ecfg, 0, sizeof(ecfg));
    memset(&bind, 0, sizeof(bind));
    bind.opts = &opts->mux_opts;
    bind.on_rtp = opts->on_rtp;
    bind.user = opts->user;
    ecfg.wire = ZMS_WIRE_FORMAT_RTP;
    ecfg.reader = opts->reader;
    ecfg.rtsp = &bind;
    out->egress = zms_egress_pipeline_create(&ecfg);
    out->mux = out->egress ? zms_egress_pipeline_rtsp_mux(out->egress) : NULL;
    return out->mux ? ZTK_OK : ZTK_ERR_STATE;
}

void zms_rtp_play_mux_destroy(zms_rtp_play_mux_handle *h)
{
    if (!h) {
        return;
    }
    zms_egress_pipeline_destroy(h->egress);
    h->egress = NULL;
    h->mux = NULL;
}

int zms_rtp_play_frame_budget(const zms_rtp_play_run_ctx *ctx)
{
    if (!ctx || !ctx->mux) {
        return (int)ZMS_RTP_PLAY_FRAME_BUDGET_LIVE;
    }
    if (zms_rtp_muxer_catchup_on(ctx->mux) && ctx->vod_reader) {
        return (int)ZMS_RTP_PLAY_FRAME_BUDGET_SEEK;
    }
    if (ctx->vod_reader && zms_vod_buffer_reader_lag(ctx->vod_reader) > ZMS_RTP_PLAY_CATCHUP_LAG) {
        return (int)ZMS_RTP_PLAY_FRAME_BUDGET;
    }
    if (ctx->gop_reader) {
        const zms_egress_clock *clk = zms_rtp_muxer_play_clock(ctx->mux);
        size_t lag = zms_gop_reader_lag(ctx->gop_reader);
        int epoch_on = clk && zms_egress_clock_epoch_locked(clk);
        int catchup = ctx->play_live_catchup ? *ctx->play_live_catchup : 0;

        return zms_egress_live_pump_budget(epoch_on, catchup, lag, 0);
    }
    return (int)ZMS_RTP_PLAY_FRAME_BUDGET_LIVE;
}

int zms_rtp_play_kick_budget(const zms_rtp_play_run_ctx *ctx)
{
    if (!ctx) {
        return (int)ZMS_RTP_PLAY_FRAME_BUDGET;
    }
    if (ctx->vod_reader && ctx->mux && zms_rtp_muxer_catchup_on(ctx->mux)) {
        return (int)ZMS_RTP_PLAY_FRAME_BUDGET_SEEK;
    }
    if (ctx->gop_reader && !ctx->vod_reader) {
        return (int)ZMS_RTP_PLAY_FRAME_BUDGET_LIVE;
    }
    return (int)ZMS_RTP_PLAY_FRAME_BUDGET;
}

int zms_rtp_play_run_pump(zms_rtp_play_run_ctx *ctx, int budget, int flush)
{
    zms_rtp_play_pump pump;

    if (!ctx) {
        return 0;
    }
    if (ctx->vod_lane) {
        zms_vod_play_lane_demux_fill(ctx->vod_lane, 32);
    }
    memset(&pump, 0, sizeof(pump));
    pump.mux = ctx->mux;
    pump.egress = ctx->egress;
    pump.sender = ctx->sender;
    pump.gop_reader = ctx->gop_reader;
    pump.vod_reader = ctx->vod_reader;
    pump.vod_demux = ctx->vod_lane ? zms_vod_play_lane_reader(ctx->vod_lane) : NULL;
    pump.source = ctx->source;
    pump.close_flag = ctx->close_pending;
    pump.destroy_flag = ctx->destroy_scheduled;
    pump.config_pending = ctx->play_config_pending;
    pump.live_catchup_done = ctx->play_live_catchup;
    pump.live_resync_at_ms = ctx->play_lag_resync_ms;
    pump.session_no = ctx->session_no;
    if (ctx->vod_lane) {
        zms_vod_play_lane_set_pump_hold(ctx->vod_lane, 1);
    }
    budget = zms_rtp_play_pump_run(&pump, budget, flush);
    if (ctx->vod_lane) {
        zms_vod_play_lane_set_pump_hold(ctx->vod_lane, 0);
    }
    return budget;
}

void zms_rtp_play_apply_vod_seek(zms_rtp_play_vod_seek_state *st)
{
    uint32_t anchor_ms;

    if (!st || !st->mux) {
        return;
    }
    if (st->is_replay && st->did_seek) {
        if (st->sender) {
            zms_rtp_play_sender_reset(st->sender);
        }
    } else if (st->is_replay && st->was_paused) {
        if (st->sender) {
            zms_rtp_play_sender_reset(st->sender);
        }
    }
    anchor_ms = (uint32_t)st->seek_ms;
    if (st->did_seek && st->vod_reader) {
        anchor_ms = zms_rtp_play_vod_anchor_ms(st->vod_reader, anchor_ms);
    }
    if (st->did_seek || !st->is_replay) {
        zms_rtp_muxer_begin_vod_seek(st->mux, anchor_ms);
    } else if (st->was_paused) {
        zms_rtp_muxer_resume_play(st->mux);
    }
    zms_rtp_muxer_set_play_scale(st->mux, st->scale);
    if (st->did_seek || st->was_paused) {
        /* Replay seek：关闭 catchup，避免 RTP ts 突发拖慢 RTCP SR（VLC 卡住）。 */
        if (st->is_replay && st->did_seek) {
            zms_rtp_muxer_set_catchup_budget(st->mux, 0, 0);
        } else {
            int catchup = (int)ZMS_RTP_VOD_CATCHUP_FRAMES_START;
            zms_rtp_muxer_set_catchup_budget(st->mux, 1, catchup);
        }
    }
}

void zms_rtsp_play_format_vod_play_200(char *extra, size_t extra_cap, const char *session_id,
                                       double scale, uint64_t seek_ms, double vod_dur_sec,
                                       const char *rtp_info)
{
    char range_hdr[64];
    char scale_hdr[32];

    if (!extra || extra_cap == 0) {
        return;
    }
    scale_hdr[0] = '\0';
    if (scale != 1.0) {
        snprintf(scale_hdr, sizeof(scale_hdr), "Scale: %.3f\r\n", scale);
    }
    if (vod_dur_sec > 0.0) {
        snprintf(range_hdr, sizeof(range_hdr), "Range: npt=%.3f-%.3f\r\n", seek_ms / 1000.0,
                 vod_dur_sec);
    } else {
        snprintf(range_hdr, sizeof(range_hdr), "Range: npt=%.3f-\r\n", seek_ms / 1000.0);
    }
    snprintf(extra, extra_cap,
             "Session: %s;timeout=60\r\n"
             "%s"
             "%s"
             "%s",
             session_id ? session_id : "", range_hdr, scale_hdr, rtp_info ? rtp_info : "");
}

void zms_rtsp_play_format_live_play_200(char *extra, size_t extra_cap, const char *session_id,
                                        const char *rtp_info)
{
    if (!extra || extra_cap == 0) {
        return;
    }
    snprintf(extra, extra_cap,
             "Session: %s;timeout=60\r\n"
             "Range: npt=0.000-\r\n"
             "%s",
             session_id ? session_id : "", rtp_info ? rtp_info : "");
}
