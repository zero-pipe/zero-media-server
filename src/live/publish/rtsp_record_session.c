#include "session/rtsp/rtsp_session_internal.h"
#include "zms/session/session_dispatcher.h"
#include "zms/media/codec/payload/payload_registry.h"
#include "zms/ops/api/webhook/webhook_client.h"
#include "zms/egress/egress_mp4_recorder.h"
#include "zms/engine/media_event.h"
#include "zms/engine/media/media_limits.h"
#include "zms/session/rtp/rtp_receiver.h"
#include "zms/media/wire/rtp_packet.h"
#include "ztk/util/log.h"
#include "ztk/net/tcp_server.h"
#include <stdio.h>
#include <string.h>

static void record_input_rtp_track(zms_rtsp_session *rs, int track_idx, const zms_rtp_packet *pkt)
{
    const zms_media_track *t;
    zms_codec_id codec;
    uint32_t clock_hz;

    if (!rs || !rs->ingress || !pkt || track_idx < 0 ||
        (unsigned)track_idx >= rs->publish_sdp.track_count) {
        return;
    }

    t = &rs->publish_sdp.tracks[track_idx];
    if (t->payload_type >= 0 && pkt->hdr.pt != (uint8_t)t->payload_type) {
        return;
    }

    codec = t->codec;
    if (codec == ZMS_CODEC_INVALID && t->type == ZMS_TRACK_AUDIO) {
        codec = ZMS_CODEC_AAC;
    }
    if (codec == ZMS_CODEC_INVALID) {
        return;
    }
    if (!zms_payload_demux_find(codec, ZMS_WIRE_FORMAT_RTP)) {
        ztk_warn("RTSP RECORD: unsupported codec=%d on track %d", (int)codec, track_idx);
        return;
    }

    clock_hz = (uint32_t)(t->sample_rate > 0 ? t->sample_rate : 0);
    if (!clock_hz) {
        clock_hz = t->type == ZMS_TRACK_VIDEO ? 90000u : 44100u;
    }

    if (codec == ZMS_CODEC_AAC) {
        (void)zms_live_ingest_ensure_aac_config(rs->ingress, (int)clock_hz, rs->audio_channels);
        rs->audio_rate = (int)clock_hz;
        rs->audio_clock_hz = clock_hz;
    }
    if (codec == ZMS_CODEC_G711A || codec == ZMS_CODEC_G711U) {
        rs->audio_rate = (int)clock_hz;
        rs->audio_clock_hz = clock_hz;
        rs->audio_codec = codec;
    }
    if (codec == ZMS_CODEC_OPUS) {
        rs->audio_rate = (int)clock_hz;
        rs->audio_clock_hz = clock_hz;
        rs->audio_codec = ZMS_CODEC_OPUS;
    }

    if (!rs->record_pipeline) {
        return;
    }
    zms_demux_pipeline_set_track(rs->record_pipeline, track_idx, codec, clock_hz);
    (void)zms_demux_pipeline_input_rtp(rs->record_pipeline, track_idx, pkt);
}

static void on_record_sorted_rtp(const zms_rtp_packet *pkt, int track_index, void *user)
{
    zms_rtsp_session *rs = (zms_rtsp_session *)user;
    if (!rs || !pkt) {
        return;
    }
    zms_rtsp_session_record_on_rtp_track(rs, track_index, pkt);
}

void zms_rtsp_session_record_receiver_ensure(zms_rtsp_session *rs)
{
    if (!rs || rs->record_receiver || !rs->ingress) {
        return;
    }
    unsigned n = rs->publish_sdp.track_count ? rs->publish_sdp.track_count : 2;
    int jitter_ms = rs->rtp_mode == ZMS_RTSP_RTP_UDP ? ZMS_RTP_JITTER_MS_UDP_DEFAULT
                                                     : ZMS_RTP_JITTER_MS_TCP_DEFAULT;
    zms_rtp_receiver_opts ropts = {
        .max_track = n,
        .jitter_slots = ZMS_RTP_JITTER_SLOTS_DEFAULT,
        .on_sorted = on_record_sorted_rtp,
        .user = rs,
        .jitter_ms = jitter_ms,
    };
    rs->record_receiver = zms_rtp_receiver_create(&ropts);
    if (rs->record_receiver) {
        ztk_info("RTSP RECORD rtp reorder: tracks=%u jitter=%dms transport=%s", n, jitter_ms,
                 rs->rtp_mode == ZMS_RTSP_RTP_UDP ? "udp" : "tcp");
    }
}

void zms_rtsp_session_record_input_rtp_raw(zms_rtsp_session *rs, int track_idx, const uint8_t *data,
                                           size_t len)
{
    if (!rs || track_idx < 0 || !data || len < 12) {
        return;
    }

    zms_rtsp_session_record_receiver_ensure(rs);
    if (rs->record_receiver) {
        (void)zms_rtp_receiver_input(rs->record_receiver, track_idx, data, len);
        return;
    }

    zms_rtp_packet pkt;
    if (zms_rtp_parse(data, len, &pkt) == ZTK_OK) {
        zms_rtsp_session_record_on_rtp_track(rs, track_idx, &pkt);
    }
}

void zms_rtsp_session_record_payload_teardown(zms_rtsp_session *rs)
{
    if (!rs) {
        return;
    }
    if (rs->record_receiver) {
        zms_rtp_receiver_destroy(rs->record_receiver);
        rs->record_receiver = NULL;
    }
}

void zms_rtsp_session_handle_record(zms_rtsp_session *rs, const zms_rtsp_message *msg)
{
    if (!rs || !msg) {
        return;
    }
    const char *sid = zms_rtsp_message_get(msg, "Session");
    if (!rs->ingress || !rs->source || !rs->source->gop_queue) {
        zms_rtsp_session_send_resp(rs, 404, "Not Found", NULL, NULL, 0);
        return;
    }
    if (sid && !zms_rtsp_session_match(rs, sid)) {
        zms_rtsp_session_send_resp(rs, 454, "Session Not Found", NULL, NULL, 0);
        return;
    }
    if (!zms_rtsp_session_record_any_track_setup(rs)) {
        zms_rtsp_session_send_resp(rs, 455, "Method Not Valid in This State", NULL, NULL, 0);
        return;
    }
    rs->logged_record = 0;
    rs->logged_record_h264 = 0;
    zms_rtsp_session_record_receiver_ensure(rs);
    if (rs->rtp_mode != ZMS_RTSP_RTP_UDP) {
        zms_rtsp_splitter_enable_rtp(rs->splitter, 1);
    }
    rs->mode = ZMS_RTSP_SESSION_MODE_RECORD;
    {
        char extra[512];
        snprintf(extra, sizeof(extra),
                 "Session: %s;timeout=60\r\n"
                 "Range: npt=0.000-\r\n",
                 rs->session_id);
        zms_rtsp_session_send_resp(rs, 200, "OK", extra, NULL, 0);
    }
    ztk_info("RTSP RECORD 200: app=%s stream=%s tracks=%u transport=%s", rs->app, rs->stream,
             rs->publish_sdp.track_count, rs->rtp_mode == ZMS_RTSP_RTP_UDP ? "udp" : "tcp");
    if (rs->source) {
        if (!zms_webhook_allow_publish(rs->source, ZMS_ORIGIN_RTSP_PUSH, rs->tcp, NULL)) {
            ztk_warn("RTSP RECORD denied by hook: app=%s stream=%s", rs->app, rs->stream);
            zms_rtsp_session_send_resp(rs, 403, "Forbidden", NULL, NULL, 0);
            return;
        }
        {
            zms_session_publish_opts pubcfg;

            memset(&pubcfg, 0, sizeof(pubcfg));
            pubcfg.schema = ZMS_SESSION_RTSP;
            if (zms_session_attach_publish(ZMS_SESSION_RTSP, rs, rs->source, &pubcfg) != ZTK_OK) {
                ztk_warn("RTSP RECORD attach publish failed: app=%s stream=%s", rs->app,
                         rs->stream);
                zms_rtsp_session_send_resp(rs, 500, "Internal Server Error", NULL, NULL, 0);
                return;
            }
        }
        zms_media_event_publish(rs->source, ZMS_ORIGIN_RTSP_PUSH);
        zms_media_source_set_publisher(rs->source, rs, zms_rtsp_session_publisher_kick);
        if (rs->source->enable_mp4) {
            ztk_poller *pol = rs->tcp ? ztk_tcp_session_poller(rs->tcp) : NULL;
            (void)zms_mp4_recorder_start(rs->source, pol);
        }
    }
}

void zms_rtsp_session_record_on_rtp_track(zms_rtsp_session *rs, int tidx, const zms_rtp_packet *pkt)
{
    if (!rs || !pkt || tidx < 0) {
        return;
    }
    rs->record_rtp_count++;
    if (rs->record_rtp_count <= 3 || rs->record_rtp_count % 300 == 0) {
        ztk_info("RTSP RECORD rtp #%u track=%d pt=%u transport=%s", rs->record_rtp_count, tidx,
                 (unsigned)pkt->hdr.pt, rs->rtp_mode == ZMS_RTSP_RTP_UDP ? "udp" : "tcp");
    }
    if (!rs->logged_record) {
        rs->logged_record = 1;
        ztk_info("RTSP RECORD first RTP: track=%d pt=%u bytes=%u transport=%s", tidx,
                 (unsigned)pkt->hdr.pt, (unsigned)pkt->payload_size,
                 rs->rtp_mode == ZMS_RTSP_RTP_UDP ? "udp" : "tcp");
    }
    record_input_rtp_track(rs, tidx, pkt);
}

void zms_rtsp_session_record_on_rtp(zms_rtsp_session *rs, uint8_t channel,
                                    const zms_rtp_packet *pkt)
{
    int tidx = zms_rtsp_session_record_track_by_channel(rs, channel);
    if (tidx < 0 || !pkt) {
        return;
    }
    zms_rtsp_session_record_on_rtp_track(rs, tidx, pkt);
}
