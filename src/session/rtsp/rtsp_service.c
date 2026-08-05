#include "zms/session/rtsp/rtsp_service.h"
#include "zms/session/session_dispatcher.h"
#include "zms/session/play_binding.h"
#include "session/rtsp/rtsp_session_internal.h"
#include "zms/engine/media_event.h"
#include "zms/live/ingest/common/stream_ingest.h"
#include "zms/engine/stream/stream_hub.h"
#include "zms/media/wire/rtp_packet.h"
#include "ztk/net/tcp_server.h"
#include "ztk/poller/poller.h"
#include "ztk/util/timer.h"
#include "ztk/poller/poller_pool.h"
#include "zms/engine/zms_atomic.h"
#include "zms/util/buf_pool.h"
#include "ztk/util/log.h"
#include <stdlib.h>
#include <string.h>

static uint64_t g_rtsp_session_serial;
/** SDP o= 对外公告地址；由 zms_rtsp_service_create 写入，session_create_user 读取 */
static char g_rtsp_advertise_host[64];

struct zms_rtsp_service {
    ztk_tcp_server *tcp;
};

void zms_rtsp_session_publisher_kick(void *ctx, int force)
{
    zms_rtsp_session *rs = (zms_rtsp_session *)ctx;
    (void)force;
    if (rs && rs->tcp) {
        ztk_tcp_session_close(rs->tcp);
    }
}

void zms_rtsp_session_on_rtp(uint8_t channel, const uint8_t *data, size_t len, void *user)
{
    zms_rtsp_session *rs = (zms_rtsp_session *)user;
    if (!rs || !data || len < 8) {
        return;
    }

    if (rs->mode == ZMS_RTSP_SESSION_MODE_PLAY) {
        if (zms_rtp_is_rtcp(data, len)) {
            zms_rtsp_session_play_on_rtcp(rs, data, len);
        }
        return;
    }

    if (rs->mode != ZMS_RTSP_SESSION_MODE_RECORD || len < 12) {
        return;
    }
    if (zms_rtp_is_rtcp(data, len)) {
        return;
    }

    int tidx = zms_rtsp_session_record_track_by_channel(rs, channel);
    if (tidx < 0) {
        return;
    }
    zms_rtsp_session_record_input_rtp_raw(rs, tidx, data, len);
}

void zms_rtsp_session_teardown(zms_rtsp_session *rs)
{
    zms_media_source *src;
    int record;
    zms_play_binding bind;

    if (!rs) {
        return;
    }
    src = rs->source;
    record = (rs->mode == ZMS_RTSP_SESSION_MODE_RECORD);

    zms_session_detach_play(ZMS_SESSION_RTSP, rs);
    memset(&bind, 0, sizeof(bind));
    bind.source = &rs->source;
    bind.gop_reader = &rs->gop_reader;
    bind.reader_attached = &rs->play_reader_attached;
    bind.play_start_ms = &rs->play_start_ms;
    bind.player = "rtsp";
    if (bind.gop_reader) {
        *bind.gop_reader = NULL;
    }
    zms_play_binding_reader_stop(&bind);
    if (record && rs->splitter) {
        zms_rtsp_splitter_enable_rtp(rs->splitter, 0);
    }
    if (record && rs->ingress) {
        zms_live_ingest_reset(rs->ingress);
    }
    if (record && src) {
        zms_media_source_clear_publisher(src, rs);
        zms_media_event_publish_fini(src, ZMS_ORIGIN_RTSP_PUSH);
    }
}

static void zms_rtsp_session_close(zms_rtsp_session *rs)
{
    if (!rs || !rs->tcp || rs->destroy_scheduled) {
        return;
    }
    ztk_tcp_session *tcp = rs->tcp;
    rs->tcp = NULL;
    ztk_tcp_session_close(tcp);
}

static void zms_rtsp_session_destroy_task(void *user);

static uint64_t zms_rtsp_session_destroy_delayed(void *user)
{
    zms_rtsp_session_destroy_task(user);
    return 0;
}

static void zms_rtsp_session_destroy_task(void *user)
{
    zms_rtsp_destroy_job *job = (zms_rtsp_destroy_job *)user;
    zms_rtsp_session *rs = job ? job->rs : NULL;
    unsigned token = job ? job->token : 0;
    if (job) {
        free(job);
    }
    if (!rs) {
        return;
    }
    if (rs->destroy_token != token) {
        ztk_info("RTSP: destroy skip stale rs=%p token=%u cur=%u", (void *)rs, token,
                 rs->destroy_token);
        return;
    }
    {
        const zms_rtp_muxer_stats *st = zms_rtp_muxer_get_stats(rs->play_rtp_muxer);
        ztk_info("RTSP #%u: destroy rs=%p mode=%d rtpq=%zu v_pkts=%u a_pkts=%u", rs->session_no,
                 (void *)rs, rs->mode, zms_rtp_play_sender_pending(rs->play_sender),
                 st ? st->video_pkt_count : 0, st ? st->audio_pkt_count : 0);
    }
    zms_session_detach_play(ZMS_SESSION_RTSP, rs);
    zms_rtp_play_sender_destroy(rs->play_sender);
    rs->play_sender = NULL;
    zms_rtsp_session_play_mux_destroy(rs);
    zms_rtsp_session_udp_teardown(rs);
    zms_rtsp_session_record_payload_teardown(rs);
    zms_live_ingest_destroy(rs->ingress);
    zms_rtsp_splitter_destroy(rs->splitter);
    if (rs->poller) {
        zms_buf_pool_slot_clear_poller(&rs->resp_buf, &rs->resp_cap, rs->poller);
        zms_buf_pool_slot_clear_poller(&rs->play_interleaved_buf, &rs->play_interleaved_cap,
                                       rs->poller);
    } else {
        zms_buf_pool_slot_clear(&rs->resp_buf, &rs->resp_cap);
        zms_buf_pool_slot_clear(&rs->play_interleaved_buf, &rs->play_interleaved_cap);
    }
    free(rs);
}

void zms_rtsp_session_schedule_destroy(zms_rtsp_session *rs, ztk_tcp_session *session)
{
    ztk_poller *pol;
    zms_rtsp_destroy_job *job;

    if (!rs || rs->destroy_scheduled) {
        return;
    }
    ztk_info("RTSP #%u: schedule_destroy rs=%p mode=%d es=%d readers=%d rtpq=%zu tcp=%p",
             rs->session_no, (void *)rs, rs->mode, rs->gop_reader ? 1 : 0,
             rs->source ? zms_media_source_reader_count(rs->source) : 0,
             zms_rtp_play_sender_pending(rs->play_sender), (void *)rs->tcp);
    rs->destroy_scheduled = 1;
    rs->close_pending = 1;
    rs->tcp = NULL;
    rs->destroy_token++;
    zms_rtsp_session_teardown(rs);

    job = (zms_rtsp_destroy_job *)malloc(sizeof(*job));
    if (!job) {
        zms_rtsp_destroy_job stack_job = {rs, rs->destroy_token};
        zms_rtsp_session_destroy_task(&stack_job);
        return;
    }
    job->rs = rs;
    job->token = rs->destroy_token;

    pol = session ? ztk_tcp_session_poller(session) : rs->poller;
    if (pol && ztk_poller_do_delay(pol, 0, zms_rtsp_session_destroy_delayed, job)) {
        return;
    }
    zms_rtsp_session_destroy_task(job);
}

static void on_recv(ztk_tcp_session *session, const void *data, size_t len, void *user)
{
    zms_rtsp_session *rs = (zms_rtsp_session *)user;
    if (!rs || !rs->splitter || rs->destroy_scheduled) {
        return;
    }
    zms_rtsp_splitter_input(rs->splitter, data, len);
    if (rs->close_pending && !rs->destroy_scheduled) {
        zms_rtsp_session_close(rs);
    }
}

static void on_manager(ztk_tcp_session *session, void *user)
{
    (void)session;
    zms_rtsp_session *rs = (zms_rtsp_session *)user;
    if (!rs || !rs->tcp || rs->close_pending || rs->destroy_scheduled ||
        rs->mode != ZMS_RTSP_SESSION_MODE_PLAY) {
        return;
    }
    if (!rs->play_boot_sent) {
        zms_rtsp_session_play_kick(rs);
    } else {
        zms_rtsp_session_play_tick(rs);
    }
    if (rs->close_pending || rs->destroy_scheduled) {
        return;
    }
    if (++rs->play_rtcp_tick >= 250) {
        zms_rtsp_session_send_rtcp_srs(rs);
        rs->play_rtcp_tick = 0;
    }
    ztk_tcp_session_flush(rs->tcp);
}

static void on_error(ztk_tcp_session *session, void *user)
{
    zms_rtsp_session *rs = (zms_rtsp_session *)user;
    if (!rs || rs->destroy_scheduled) {
        return;
    }
    zms_rtsp_session_schedule_destroy(rs, session);
}

static void *session_create_user(ztk_tcp_server *srv, ztk_tcp_session *session)
{
    (void)srv;
    zms_rtsp_session *rs = (zms_rtsp_session *)calloc(1, sizeof(*rs));
    if (!rs) {
        return NULL;
    }
    rs->session_no = (unsigned)(ZMS_ATOMIC_ADD64(&g_rtsp_session_serial, 1) + 1);
    rs->tcp = session;
    rs->poller = ztk_tcp_session_poller(session);
    if (g_rtsp_advertise_host[0]) {
        strncpy(rs->advertise_host, g_rtsp_advertise_host, sizeof(rs->advertise_host) - 1);
    }
    ztk_info("RTSP #%u: session create rs=%p", rs->session_no, (void *)rs);
    rs->audio_rate = 44100;
    rs->audio_channels = 2;
    rs->rtp_mode = ZMS_RTSP_RTP_TCP;
    rs->video_rtp_ssrc = 0x12345678u;
    rs->audio_rtp_ssrc = 0x87654321u;
    rs->play_scale = 1.0;
    zms_rtsp_udp_registry_init();
    zms_rtsp_splitter_opts opts = {zms_rtsp_session_on_message, zms_rtsp_session_on_rtp, rs};
    rs->splitter = zms_rtsp_splitter_create(&opts);
    if (!rs->splitter) {
        free(rs);
        return NULL;
    }
    rs->play_sender =
        zms_rtp_play_sender_create(zms_rtsp_session_write_interleaved, rs, ZMS_RTP_PLAY_RTPQ_CAP);
    if (!rs->play_sender) {
        zms_rtsp_splitter_destroy(rs->splitter);
        free(rs);
        return NULL;
    }
    zms_rtp_play_sender_set_session_no(rs->play_sender, rs->session_no);
    zms_rtp_play_sender_set_poller(rs->play_sender, rs->poller);
    return rs;
}

zms_rtsp_service *zms_rtsp_service_create(const zms_rtsp_service_opts *opts)
{
    if (!opts || !opts->poller_pool) {
        return NULL;
    }
    zms_session_dispatch_register_all();
    zms_rtsp_service *srv = (zms_rtsp_service *)calloc(1, sizeof(*srv));
    if (!srv) {
        return NULL;
    }
    memset(g_rtsp_advertise_host, 0, sizeof(g_rtsp_advertise_host));
    if (opts->advertise_host && opts->advertise_host[0]) {
        strncpy(g_rtsp_advertise_host, opts->advertise_host, sizeof(g_rtsp_advertise_host) - 1);
    }

    ztk_tcp_session_ops_t ops = {on_recv, on_error, on_manager};
    ztk_tcp_server_opts_t topts = {
        .host = opts->host ? opts->host : "0.0.0.0",
        .port = opts->port ? opts->port : 554,
        .backlog = 64,
        .poller_pool = opts->poller_pool,
        .session_ops = &ops,
        .session_create_user = session_create_user,
        .manager_interval_sec = 0.02f,
    };
    srv->tcp = ztk_tcp_server_create(&topts);
    if (!srv->tcp) {
        free(srv);
        return NULL;
    }
    return srv;
}

void zms_rtsp_service_destroy(zms_rtsp_service *srv)
{
    if (!srv) {
        return;
    }
    ztk_tcp_server_destroy(srv->tcp);
    free(srv);
}

ztk_err_t zms_rtsp_service_start(zms_rtsp_service *srv)
{
    if (!srv || !srv->tcp) {
        return ZTK_ERR_INVALID;
    }
    return ztk_tcp_server_start(srv->tcp);
}

void zms_rtsp_service_stop(zms_rtsp_service *srv)
{
    if (srv && srv->tcp) {
        ztk_tcp_server_stop(srv->tcp);
    }
}

uint16_t zms_rtsp_service_port(const zms_rtsp_service *srv)
{
    if (!srv || !srv->tcp) {
        return 0;
    }
    return ztk_tcp_server_port(srv->tcp);
}
