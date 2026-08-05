#include "session/rtsp/rtsp_session_internal.h"
#include "ztk/platform.h"
#include "zms/egress/egress_live_policy.h"
#include "zms/session/session_dispatcher.h"
#include "zms/session/play_binding.h"
#include "zms/engine/stream/stream_hub.h"
#include "zms/media/codec/g711/g711_over_rtp.h"
#include "zms/media/codec/av1/av1_over_rtmp.h"
#include "zms/vod/io/vod_thread_pool.h"
#include "zms/vod/io/vod_reader.h"
#include "zms/vod/io/vod_buffer.h"
#include "zms/vod/play/vod_play_lane.h"
#include "zms/vod/io/vod_source.h"
#include "zms/ops/api/webhook/webhook_client.h"
#include "zms/engine/media_clock.h"
#include "zms/engine/media_event.h"
#include "zms/engine/gop/gop_queue.h"
#include "zms/session/rtp/rtcp.h"
#include "zms/session/rtsp/rtsp_parser.h"
#include "zms/session/rtsp/rtsp_session_auth.h"
#include "ztk/poller/poller.h"
#include "ztk/util/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void zms_rtsp_session_fill_play_ctx(zms_rtsp_session *rs, zms_rtp_play_run_ctx *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    if (!rs) {
        return;
    }
    ctx->mux = rs->play_rtp_muxer;
    ctx->egress = rs->egress_pipe;
    ctx->sender = rs->play_sender;
    ctx->gop_reader = rs->gop_reader;
    ctx->vod_reader = rs->vod_reader;
    ctx->vod_lane = rs->vod_lane;
    ctx->source = rs->source;
    ctx->close_pending = &rs->close_pending;
    ctx->destroy_scheduled = &rs->destroy_scheduled;
    ctx->play_config_pending = &rs->play_config_pending;
    ctx->play_live_catchup = &rs->play_live_catchup;
    ctx->play_lag_resync_ms = &rs->play_lag_resync_ms;
    ctx->session_no = rs->session_no;
}

zms_rtsp_play_rtp_info_args zms_rtsp_session_rtp_info_args(zms_rtsp_session *rs, uint32_t anchor_ms,
                                                           int vod_linear)
{
    zms_rtsp_play_rtp_info_args args;

    memset(&args, 0, sizeof(args));
    if (!rs) {
        return args;
    }
    args.host = rs->peer_ip[0] ? rs->peer_ip : "127.0.0.1";
    args.app = rs->app;
    args.stream = rs->stream;
    args.source = rs->source;
    args.mux = rs->play_rtp_muxer;
    args.video_clock_hz = rs->video_clock_hz;
    args.audio_clock_hz = rs->audio_clock_hz;
    args.audio_rate = rs->audio_rate;
    args.anchor_ms = anchor_ms;
    args.vod_linear_rtp = vod_linear;
    return args;
}

void zms_rtsp_session_egress_close(zms_rtsp_session *rs)
{
    zms_play_binding bind;

    if (!rs) {
        return;
    }
    memset(&bind, 0, sizeof(bind));
    bind.source = &rs->source;
    bind.play = &rs->play;
    bind.gop_reader = &rs->gop_reader;
    bind.vod_reader = &rs->vod_reader;
    bind.vod_lane = &rs->vod_lane;
    bind.reader_attached = &rs->play_reader_attached;
    bind.play_start_ms = &rs->play_start_ms;
    bind.player = "rtsp";
    zms_play_binding_close_readers(&bind);
}

static void zms_rtsp_session_play_pump(zms_rtsp_session *rs, int budget, int flush)
{
    zms_rtp_play_run_ctx ctx;

    if (!rs) {
        return;
    }
    zms_rtsp_session_fill_play_ctx(rs, &ctx);
    (void)zms_rtp_play_run_pump(&ctx, budget, flush);
    zms_rtsp_session_play_try_rtcp_boot(rs);
}

void zms_rtsp_session_play_tick(zms_rtsp_session *rs)
{
    zms_rtp_play_run_ctx ctx;
    int budget;

    if (!rs || rs->close_pending || rs->destroy_scheduled || rs->play_paused ||
        rs->mode != ZMS_RTSP_SESSION_MODE_PLAY || !rs->play_rtp_muxer) {
        return;
    }
    zms_rtsp_session_fill_play_ctx(rs, &ctx);
    budget = zms_rtp_play_frame_budget(&ctx);
    zms_rtsp_session_play_pump(rs, budget, ZMS_RTP_PLAY_RTP_FLUSH);
    if (rs->close_pending && !rs->destroy_scheduled) {
        zms_rtsp_session_schedule_destroy(rs, rs->tcp);
    }
}

void zms_rtsp_session_play_kick(zms_rtsp_session *rs)
{
    zms_rtp_play_run_ctx ctx;
    int budget;

    if (!rs || rs->close_pending || rs->destroy_scheduled || rs->play_paused ||
        rs->mode != ZMS_RTSP_SESSION_MODE_PLAY || !rs->play_rtp_muxer) {
        return;
    }
    if (!rs->play_boot_sent) {
        if (rs->source && zms_media_source_is_vod(rs->source)) {
            (void)zms_rtp_play_bootstrap_vod(rs->play_rtp_muxer, rs->source, rs->vod_reader,
                                             rs->session_no, rs->vod_lane, 0);
        } else {
            (void)zms_rtp_play_bootstrap_live(rs->play_rtp_muxer, rs->source, rs->gop_reader,
                                              rs->session_no);
        }
        rs->play_boot_sent = 1;
        rs->play_config_pending = 0;
        if (rs->vod_reader) {
            rs->play_live_catchup = 1;
        } else {
            rs->play_live_catchup = 0;
        }
        if (rs->play_rtp_muxer && rs->vod_reader && !zms_rtp_muxer_catchup_on(rs->play_rtp_muxer)) {
            zms_rtp_muxer_set_catchup_budget(rs->play_rtp_muxer, 1,
                                             (int)ZMS_RTP_VOD_CATCHUP_FRAMES_START);
        }
    }
    zms_rtsp_session_fill_play_ctx(rs, &ctx);
    budget = zms_rtp_play_kick_budget(&ctx);
    zms_rtsp_session_play_pump(rs, budget, ZMS_RTP_PLAY_RTP_FLUSH);
    if (rs->tcp) {
        ztk_tcp_session_flush(rs->tcp);
    }
}

static void on_mux_rtp(zms_rtp_mux_track track, const uint8_t *rtp, size_t len, void *user)
{
    zms_rtsp_session *rs = (zms_rtsp_session *)user;
    const zms_rtp_muxer_stats *st;

    if (!rs || rs->close_pending || rs->destroy_scheduled ||
        rs->mode != ZMS_RTSP_SESSION_MODE_PLAY || !rtp || len == 0) {
        return;
    }
    if (rs->source) {
        zms_media_stats_on_egress(rs->source, len);
    }
    st = zms_rtp_muxer_get_stats(rs->play_rtp_muxer);
    if (track == ZMS_RTP_MUX_TRACK_VIDEO) {
        if (st && st->video_pkt_count == 1) {
            ztk_info("RTSP #%u first RTP: ch=%u bytes=%u transport=%s", rs->session_no,
                     (unsigned)rs->video_rtp_ch, (unsigned)len,
                     rs->rtp_mode == ZMS_RTSP_RTP_UDP ? "udp" : "tcp");
        }
        if (st && rs->vod_reader && rs->source && zms_media_source_is_vod(rs->source)) {
            uint32_t vhz = rs->video_clock_hz > 0 ? rs->video_clock_hz : 90000u;
            rs->play_seek_ms = zms_rtp_clock_to_ms(st->video_last_rtp_ts, vhz);
        }
        zms_rtp_play_sender_submit(rs->play_sender, 1, rs->video_rtp_ch, rtp, len);
    } else {
        if (st && st->audio_pkt_count == 1) {
            ztk_info("RTSP #%u first audio RTP: ch=%u bytes=%u transport=%s", rs->session_no,
                     (unsigned)rs->audio_rtp_ch, (unsigned)len,
                     rs->rtp_mode == ZMS_RTSP_RTP_UDP ? "udp" : "tcp");
        }
        zms_rtp_play_sender_submit(rs->play_sender, 1, rs->audio_rtp_ch, rtp, len);
    }
    zms_rtsp_session_play_try_rtcp_boot(rs);
}

void zms_rtsp_session_play_mux_destroy(zms_rtsp_session *rs)
{
    zms_rtp_play_mux_handle opened;

    if (!rs) {
        return;
    }
    opened.egress = rs->egress_pipe;
    opened.mux = rs->play_rtp_muxer;
    zms_rtp_play_mux_destroy(&opened);
    rs->egress_pipe = NULL;
    rs->play_rtp_muxer = NULL;
}

void zms_rtsp_session_play_mux_create(zms_rtsp_session *rs)
{
    zms_rtp_muxer_opts opts;
    zms_rtp_play_mux_opts ocfg;
    zms_rtp_play_mux_handle opened;

    if (!rs) {
        return;
    }
    zms_rtsp_session_play_mux_destroy(rs);
    memset(&opts, 0, sizeof(opts));
    opts.video_clock_hz = rs->video_clock_hz > 0 ? rs->video_clock_hz : 90000u;
    opts.audio_clock_hz = rs->audio_clock_hz;
    opts.audio_rate = rs->audio_rate;
    opts.audio_codec = rs->audio_codec;
    opts.video_pt = 96;
    opts.audio_pt = (rs->audio_codec == ZMS_CODEC_G711A || rs->audio_codec == ZMS_CODEC_G711U)
                        ? zms_g711_over_rtp_default_pt(rs->audio_codec)
                        : 97;
    opts.video_ssrc = rs->video_rtp_ssrc;
    opts.audio_ssrc = rs->audio_rtp_ssrc;
    opts.video_seq = 1;
    opts.audio_seq = 1;
    if (rs->source && rs->audio_codec == ZMS_CODEC_AAC) {
        size_t acfg_len = 0;
        const uint8_t *acfg = zms_media_source_audio_config(rs->source, &acfg_len);

        if (acfg && acfg_len > 2) {
            opts.audio_extra = acfg + 2;
            opts.audio_extra_len = acfg_len - 2;
        }
    }
    if (rs->source && rs->source->has_video && rs->source->video.codec == ZMS_CODEC_AV1) {
        size_t vcfg_len = 0;
        const uint8_t *vcfg = zms_media_source_video_config(rs->source, &vcfg_len);
        const uint8_t *av1c = NULL;
        size_t av1c_len = 0;

        if (vcfg && zms_av1_over_rtmp_config_extradata(vcfg, vcfg_len, &av1c, &av1c_len) && av1c &&
            av1c_len > 0) {
            opts.video_extra = av1c;
            opts.video_extra_len = av1c_len;
        }
    }
    memset(&ocfg, 0, sizeof(ocfg));
    ocfg.mux_opts = opts;
    ocfg.reader = &rs->play;
    ocfg.on_rtp = on_mux_rtp;
    ocfg.user = rs;
    if (zms_rtp_play_mux_create(&opened, &ocfg) == ZTK_OK) {
        rs->egress_pipe = opened.egress;
        rs->play_rtp_muxer = opened.mux;
        zms_rtp_muxer_arm_play(rs->play_rtp_muxer);
    }
}

void zms_rtsp_session_handle_play(zms_rtsp_session *rs, const zms_rtsp_message *msg)
{
    if (!rs || !msg) {
        return;
    }
    if (!zms_rtsp_auth_check(rs, msg)) {
        return;
    }
    zms_rtsp_session_parse_url(msg->url, rs->app, rs->stream);
    if (!rs->source || (!rs->source->gop_queue && !rs->source->vod_buffer)) {
        rs->source = zms_media_source_find_for_play(ZMS_SCHEMA_RTSP, rs->app, rs->stream);
    }
    if (rs->source && zms_media_source_is_vod(rs->source)) {
        int is_replay = rs->play_reader_attached;
        int was_paused = rs->play_paused;
        int range_parsed = 0;
        int range_now = 0;
        int did_seek = 0;
        uint64_t seek_ms = 0;
        double scale;
        const char *range_req = zms_rtsp_message_get(msg, "Range");

        scale = zms_rtsp_parse_play_scale(zms_rtsp_message_get(msg, "Scale"), rs->play_scale);
        rs->play_scale = scale;
        rs->play_rtcp_tick = 0;
        rs->play_live_catchup = 0;
        rs->play_lag_resync_ms = 0;
        zms_rtsp_session_load_audio_params(rs);
        if (rs->source->has_audio && !rs->play_audio_setup) {
            ztk_warn("RTSP PLAY: audio track not SETUP 拒绝ffplay needs SETUP trackID=1 for sound");
        }
        {
            zms_media_tuple tuple;
            zms_media_tuple_from_source(rs->source, &tuple);
            if (!zms_webhook_allow_play(&tuple, "rtsp", rs->tcp, NULL)) {
                ztk_warn("RTSP PLAY denied by hook: app=%s stream=%s", rs->app, rs->stream);
                zms_rtsp_session_send_resp(rs, 403, "Forbidden", NULL, NULL, 0);
                return;
            }
        }
        zms_rtsp_splitter_enable_rtp(rs->splitter, 1);
        rs->play_paused = 0;
        rs->mode = ZMS_RTSP_SESSION_MODE_PLAY;

        if (range_req) {
            range_parsed = zms_rtsp_parse_range_npt_ms(range_req, &seek_ms, &range_now);
        }
        if (!is_replay) {
            if (!range_parsed || range_now) {
                seek_ms = 0;
            }
            did_seek = 1;
        } else if (range_parsed && !range_now) {
            did_seek = 1;
        } else {
            seek_ms = rs->play_seek_ms;
        }

        if (did_seek) {
            zms_session_play_opts pcfg;
            uint64_t req_ms = seek_ms;

            memset(&pcfg, 0, sizeof(pcfg));
            pcfg.player = ZMS_SESSION_RTSP;
            pcfg.seek_ms = seek_ms;
            if (is_replay && req_ms == rs->play_seek_ms) {
                ztk_info("RTSP #%u seek refresh: same pos %llu ms (skip lane re-seek)",
                         rs->session_no, (unsigned long long)req_ms);
            } else if (!rs->vod_lane && zms_vod_thread_pool_enabled()) {
                zms_rtsp_vod_play_ctx *job = (zms_rtsp_vod_play_ctx *)calloc(1, sizeof(*job));
                if (job) {
                    job->rs = rs;
                    job->src = rs->source;
                    job->seek_ms = seek_ms;
                    job->is_replay = is_replay;
                    job->was_paused = was_paused;
                    job->did_seek = did_seek;
                    job->range_parsed = range_parsed;
                    job->range_now = range_now;
                    job->scale = scale;
                    if (range_req && range_req[0]) {
                        strncpy(job->range_req, range_req, sizeof(job->range_req) - 1);
                    }
                    if (zms_rtsp_session_vod_open_try_async(job)) {
                        return;
                    }
                    free(job);
                }
                if (zms_session_attach_play(ZMS_SESSION_RTSP, rs, rs->source, &pcfg) != ZTK_OK) {
                    ztk_warn("RTSP PLAY 404: vod attach failed app=%s stream=%s", rs->app,
                             rs->stream);
                    zms_rtsp_session_send_resp(rs, 404, "Not Found", NULL, NULL, 0);
                    return;
                }
            } else if (zms_session_attach_play(ZMS_SESSION_RTSP, rs, rs->source, &pcfg) != ZTK_OK) {
                ztk_warn("RTSP PLAY 404: vod attach failed app=%s stream=%s", rs->app, rs->stream);
                zms_rtsp_session_send_resp(rs, 404, "Not Found", NULL, NULL, 0);
                return;
            }
            seek_ms = rs->play_seek_ms;
        } else if (!rs->vod_lane || !rs->vod_reader) {
            ztk_warn("RTSP PLAY 404: vod lane missing on replay app=%s stream=%s", rs->app,
                     rs->stream);
            zms_rtsp_session_send_resp(rs, 404, "Not Found", NULL, NULL, 0);
            return;
        } else {
            seek_ms = rs->play_seek_ms;
        }

        zms_rtsp_session_vod_play_continue(rs, is_replay, was_paused, did_seek, seek_ms, scale,
                                           range_req, range_parsed, range_now);
    } else if (rs->source && zms_media_source_use_gop_queue_play(rs->source)) {
        zms_session_play_opts pcfg;

        memset(&pcfg, 0, sizeof(pcfg));
        pcfg.player = ZMS_SESSION_RTSP;
        zms_session_detach_play(ZMS_SESSION_RTSP, rs);
        if (zms_session_attach_play(ZMS_SESSION_RTSP, rs, rs->source, &pcfg) != ZTK_OK) {
            rs->gop_reader = NULL;
        }
        rs->video_rtp_ssrc = 0x12345678;
        rs->audio_rtp_ssrc = 0x87654321;
        rs->play_rtcp_tick = 0;
        rs->play_boot_sent = 0;
        rs->play_rtcp_boot_sent = 0;
        rs->play_live_catchup = 0;
        rs->play_lag_resync_ms = 0;
        zms_rtp_play_sender_reset(rs->play_sender);
        zms_rtsp_session_load_audio_params(rs);
        if (rs->source->has_audio && !rs->play_audio_setup) {
            ztk_warn("RTSP PLAY: audio track not SETUP 拒绝ffplay needs SETUP trackID=1 for sound");
        }
        {
            zms_media_tuple tuple;
            zms_media_tuple_from_source(rs->source, &tuple);
            if (!zms_webhook_allow_play(&tuple, "rtsp", rs->tcp, NULL)) {
                ztk_warn("RTSP PLAY denied by hook: app=%s stream=%s", rs->app, rs->stream);
                zms_rtsp_session_send_resp(rs, 403, "Forbidden", NULL, NULL, 0);
                return;
            }
        }
        zms_rtsp_splitter_enable_rtp(rs->splitter, 1);
        rs->mode = ZMS_RTSP_SESSION_MODE_PLAY;
        zms_rtsp_session_play_mux_create(rs);
        {
            uint32_t anchor_ms = 0;
            zms_gop_slot at;

            (void)zms_rtp_play_bootstrap_live(rs->play_rtp_muxer, rs->source, rs->gop_reader,
                                              rs->session_no);
            rs->play_boot_sent = 1;
            rs->play_config_pending = 0;
            rs->play_live_catchup = 0;
            if (rs->gop_reader && zms_gop_reader_slot_at_read(rs->gop_reader, &at)) {
                anchor_ms = at.dts_ms;
            }
            ztk_info("RTSP #%u PLAY 200: app=%s stream=%s video=%d audio=%d es=%d reader=%s "
                     "transport=%s anchor_ms=%u",
                     rs->session_no, rs->app, rs->stream, rs->source->has_video,
                     rs->source->has_audio, rs->gop_reader ? 1 : 0,
                     rs->gop_reader ? "gop_queue" : "none",
                     rs->rtp_mode == ZMS_RTSP_RTP_UDP ? "udp" : "tcp", (unsigned)anchor_ms);
            {
                char extra[ZMS_RTSP_PLAY_EXTRA_MAX];
                char rtp_info[ZMS_RTSP_RTP_INFO_MAX];

                {
                    zms_rtsp_play_rtp_info_args rtp_args =
                        zms_rtsp_session_rtp_info_args(rs, anchor_ms, 0);
                    zms_rtsp_play_format_rtp_info(&rtp_args, rtp_info, sizeof(rtp_info));
                }
                zms_rtsp_play_format_live_play_200(extra, sizeof(extra), rs->session_id, rtp_info);
                zms_rtsp_session_send_resp(rs, 200, "OK", extra, NULL, 0);
            }
        }
        rs->play_config_pending = 0;
        if (rs->source && !rs->play_reader_attached) {
            zms_media_source_reader_add(rs->source);
            rs->play_start_ms = ztk_monotonic_ms();
            zms_media_event_play(rs->source, "rtsp");
            rs->play_reader_attached = 1;
        }
        zms_rtsp_session_play_kick(rs);
        if (rs->rtp_mode == ZMS_RTSP_RTP_UDP && rs->play_sender) {
            zms_rtp_play_sender_flush(rs->play_sender, (int)ZMS_RTP_PLAY_RTP_FLUSH, 0);
        }
    } else {
        ztk_warn("RTSP PLAY 404: stream not found app=%s stream=%s url=%s", rs->app, rs->stream,
                 msg->url);
        zms_rtsp_session_send_resp(rs, 404, "Not Found", NULL, NULL, 0);
    }
}
