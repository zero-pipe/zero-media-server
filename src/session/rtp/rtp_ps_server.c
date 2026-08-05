#include "zms/session/rtp/rtp_ps_server.h"
#include "session/rtp/rtp_ps_server_internal.h"
#include "zms/ops/api/webhook/webhook_client.h"
#include "zms/egress/egress_mp4_recorder.h"
#include "zms/engine/media_event.h"
#include "zms/media/wire/rtp_packet.h"
#include "zms/live/ingest/common/stream_ingest.h"
#include "zms/util/buf_pool.h"
#include "rtp-payload.h"
#include "ztk/net/tcp_client.h"
#include "ztk/net/tcp_server.h"
#include "ztk/net/udp_server.h"
#include "ztk/net/socket.h"
#include "ztk/util/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ZMS_RTP_PS_RECV_BUF 65536
#define ZMS_RTP_PS_TCP_ASM_INIT 65536

typedef struct zms_rtp_ps_server_reg_entry {
    char key[256];
    zms_rtp_ps_server_slot *slot;
    struct zms_rtp_ps_server_reg_entry *next;
} zms_rtp_ps_server_reg_entry;

static zms_rtp_ps_server_reg_entry *g_rtp_ps_registry;
static unsigned g_rtp_ps_session_no;

static void rtp_ps_reg_add(zms_rtp_ps_server_slot *slot)
{
    zms_rtp_ps_server_reg_entry *e;

    if (!slot || !slot->key[0]) {
        return;
    }
    e = (zms_rtp_ps_server_reg_entry *)calloc(1, sizeof(*e));
    if (!e) {
        return;
    }
    strncpy(e->key, slot->key, sizeof(e->key) - 1);
    e->slot = slot;
    e->next = g_rtp_ps_registry;
    g_rtp_ps_registry = e;
}

static void rtp_ps_reg_remove(zms_rtp_ps_server_slot *slot)
{
    zms_rtp_ps_server_reg_entry **pp = &g_rtp_ps_registry;

    if (!slot) {
        return;
    }
    while (*pp) {
        if ((*pp)->slot == slot) {
            zms_rtp_ps_server_reg_entry *dead = *pp;
            *pp = dead->next;
            free(dead);
            return;
        }
        pp = &(*pp)->next;
    }
}

void zms_rtp_ps_server_make_key(const char *vhost, const char *app, const char *stream, char *key,
                                size_t key_cap)
{
    const char *vh = (vhost && vhost[0]) ? vhost : "__defaultVhost__";
    if (!key || key_cap == 0) {
        return;
    }
    snprintf(key, key_cap, "%s/%s/%s", vh, app && app[0] ? app : "live",
             stream && stream[0] ? stream : "");
}

zms_rtp_ps_server_slot *zms_rtp_ps_server_find_by_key(const char *key)
{
    if (!key || !key[0]) {
        return NULL;
    }
    for (zms_rtp_ps_server_reg_entry *e = g_rtp_ps_registry; e; e = e->next) {
        if (strcmp(e->key, key) == 0) {
            return e->slot;
        }
    }
    return NULL;
}

zms_rtp_ps_server_slot *zms_rtp_ps_server_slot_from_udp(ztk_udp_server *udp)
{
    for (zms_rtp_ps_server_reg_entry *e = g_rtp_ps_registry; e; e = e->next) {
        if (e->slot && e->slot->udp == udp) {
            return e->slot;
        }
    }
    return NULL;
}

zms_media_source *zms_rtp_ps_server_source(const zms_rtp_ps_server_slot *slot)
{
    return slot ? slot->source : NULL;
}

uint16_t zms_rtp_ps_server_port(const zms_rtp_ps_server_slot *slot)
{
    return slot ? slot->port : 0;
}

const char *zms_rtp_ps_server_app(const zms_rtp_ps_server_slot *slot)
{
    return slot ? slot->app : "";
}

const char *zms_rtp_ps_server_stream(const zms_rtp_ps_server_slot *slot)
{
    return slot ? slot->stream : "";
}

int zms_rtp_ps_server_payload_type(const zms_rtp_ps_server_slot *slot)
{
    return slot ? slot->payload_type : ZMS_RTP_PS_DEFAULT_PT;
}

int zms_rtp_ps_server_foreach(zms_rtp_ps_server_visit_fn fn, void *user)
{
    int n = 0;
    if (!fn) {
        return 0;
    }
    for (zms_rtp_ps_server_reg_entry *e = g_rtp_ps_registry; e; e = e->next) {
        if (e->slot && fn(e->key, e->slot, user) != 0) {
            break;
        }
        ++n;
    }
    return n;
}

static void rtp_ps_slot_finish_publish(zms_rtp_ps_server_slot *slot)
{
    if (!slot || slot->publish_started || !slot->source) {
        return;
    }
    if (!zms_webhook_allow_publish(slot->source, ZMS_ORIGIN_RTP_PS_PUSH, NULL, NULL)) {
        ztk_warn("[rtp-ps] publish denied by hook app=%s stream=%s", slot->app, slot->stream);
        if (slot->source) {
            zms_media_source_close(slot->source, 1);
        }
        return;
    }
    zms_media_event_publish(slot->source, ZMS_ORIGIN_RTP_PS_PUSH);
    zms_media_source_set_publisher(slot->source, slot, NULL);
    if (slot->source->enable_mp4) {
        (void)zms_mp4_recorder_start(slot->source, slot->poller);
    }
    slot->publish_started = 1;
    ztk_info("[rtp-ps] publish ready app=%s stream=%s port=%u tcp_mode=%d enable_mp4=%d", slot->app,
             slot->stream, (unsigned)slot->port, slot->tcp_mode, slot->source->enable_mp4);
}

static void rtp_ps_on_ps_frame(const zms_frame *frame, void *user)
{
    zms_rtp_ps_server_slot *slot = (zms_rtp_ps_server_slot *)user;

    if (!slot || !slot->ingress || !frame || frame->size == 0) {
        return;
    }
    if (zms_live_ingest_input_frame(slot->ingress, frame) != ZTK_OK) {
        return;
    }
    if (frame->track == ZMS_TRACK_VIDEO && frame->keyframe) {
        rtp_ps_slot_finish_publish(slot);
    }
}

static void rtp_ps_on_h264_ps(const uint8_t *sps, size_t sps_len, const uint8_t *pps,
                              size_t pps_len, void *user)
{
    zms_rtp_ps_server_slot *slot = (zms_rtp_ps_server_slot *)user;

    if (!slot || !slot->ingress) {
        return;
    }
    (void)zms_live_ingest_set_h264_sps_pps(slot->ingress, sps, sps_len, pps, pps_len);
}

static void *rtp_ps_payload_alloc(void *param, int bytes)
{
    zms_rtp_ps_server_slot *slot = (zms_rtp_ps_server_slot *)param;

    if (!slot || bytes <= 0) {
        return NULL;
    }
    if (!zms_buf_pool_slot_resize(&slot->rtp_ps_assembly, &slot->rtp_ps_assembly_cap,
                                  (size_t)bytes)) {
        return NULL;
    }
    return slot->rtp_ps_assembly;
}

static void rtp_ps_payload_free(void *param, void *packet)
{
    (void)param;
    (void)packet;
}

static int rtp_ps_payload_packet(void *param, const void *packet, int bytes, uint32_t timestamp,
                                 int flags)
{
    zms_rtp_ps_server_slot *slot = (zms_rtp_ps_server_slot *)param;

    (void)timestamp;
    if (!slot || !slot->ps_demuxer || !packet || bytes <= 0) {
        return 0;
    }
    if (flags & RTP_PAYLOAD_FLAG_PACKET_LOST) {
        return 0;
    }
    return zms_mpegps_demuxer_feed(slot->ps_demuxer, (const uint8_t *)packet, (size_t)bytes) ==
                   ZTK_OK
               ? 0
               : -1;
}

static ztk_err_t rtp_ps_slot_create_media(zms_rtp_ps_server_slot *slot, const char *app,
                                          const char *stream)
{
    zms_mpegps_demuxer_opts psopts;
    struct rtp_payload_t rtp_handler;

    if (!slot || !app || !app[0] || !stream || !stream[0]) {
        return ZTK_ERR_INVALID;
    }

    slot->ingress = zms_live_ingest_create_publish_schema(ZMS_SCHEMA_RTP_PS, app, stream, NULL);
    if (slot->ingress) {
        zms_live_ingest_set_poller(slot->ingress, slot->poller);
    }
    slot->source = slot->ingress ? zms_live_ingest_source(slot->ingress) : NULL;
    if (!slot->source) {
        return ZTK_ERR_STATE;
    }

    zms_live_ingest_set_rtp_clocks(slot->ingress, 90000, ZMS_CODEC_AAC, 90000);
    zms_live_ingest_set_stamp_max_delta(slot->ingress, 10000);
    zms_live_ingest_set_stamp_av_clamp(slot->ingress, 0);
    zms_live_ingest_set_timeline_linear_ms(slot->ingress, 1);
    zms_live_ingest_set_defer_gop_vcfg(slot->ingress, 1);

    memset(&psopts, 0, sizeof(psopts));
    psopts.on_frame = rtp_ps_on_ps_frame;
    psopts.on_h264_ps = rtp_ps_on_h264_ps;
    psopts.user = slot;
    slot->ps_demuxer = zms_mpegps_demuxer_create(&psopts);
    if (!slot->ps_demuxer) {
        zms_live_ingest_destroy(slot->ingress);
        slot->ingress = NULL;
        slot->source = NULL;
        return ZTK_ERR_NOMEM;
    }

    memset(&rtp_handler, 0, sizeof(rtp_handler));
    rtp_handler.alloc = rtp_ps_payload_alloc;
    rtp_handler.free = rtp_ps_payload_free;
    rtp_handler.packet = rtp_ps_payload_packet;
    slot->rtp_ps_decoder = rtp_payload_decode_create(slot->payload_type, "PS", &rtp_handler, slot);
    if (!slot->rtp_ps_decoder) {
        zms_mpegps_demuxer_destroy(slot->ps_demuxer);
        slot->ps_demuxer = NULL;
        zms_live_ingest_destroy(slot->ingress);
        slot->ingress = NULL;
        slot->source = NULL;
        return ZTK_ERR_NOMEM;
    }

    return ZTK_OK;
}

static void rtp_ps_slot_teardown_media(zms_rtp_ps_server_slot *slot)
{
    zms_media_source *src;

    if (!slot) {
        return;
    }
    src = slot->source;
    if (slot->rtp_ps_decoder) {
        rtp_payload_decode_destroy(slot->rtp_ps_decoder);
        slot->rtp_ps_decoder = NULL;
    }
    if (slot->ps_demuxer) {
        zms_mpegps_demuxer_destroy(slot->ps_demuxer);
        slot->ps_demuxer = NULL;
    }
    zms_buf_pool_slot_clear(&slot->rtp_ps_assembly, &slot->rtp_ps_assembly_cap);
    if (slot->ingress) {
        zms_live_ingest_reset(slot->ingress);
    }
    if (src) {
        zms_media_source_clear_publisher(src, slot);
        zms_media_event_publish_fini(src, ZMS_ORIGIN_RTP_PS_PUSH);
    }
    if (slot->ingress) {
        zms_live_ingest_destroy(slot->ingress);
        slot->ingress = NULL;
    }
    slot->source = NULL;
    slot->publish_started = 0;
}

static void rtp_ps_feed_rtp_packet(zms_rtp_ps_server_slot *slot, const uint8_t *data, size_t len,
                                   const char *peer_ip, uint16_t peer_port, int is_first_raw)
{
    zms_rtp_packet rtp;

    if (!slot || !data || len < ZMS_RTP_HDR_SIZE) {
        return;
    }
    if (is_first_raw) {
        ztk_debug(
            "[GB28181 zms 4/6] first RTP/PS TCP from %s:%u len=%zu app=%s stream=%s expect_pt=%d",
            peer_ip ? peer_ip : "?", (unsigned)peer_port, len, slot->app, slot->stream,
            slot->payload_type);
    }
    if (zms_rtp_is_rtcp(data, len)) {
        return;
    }
    if (zms_rtp_parse(data, len, &rtp) != ZTK_OK) {
        return;
    }
    if (slot->payload_type >= 0 && rtp.hdr.pt != (uint8_t)slot->payload_type) {
        if (slot->recv_pkts <= 3) {
            ztk_debug("[GB28181 zms] drop RTP pt=%u expect=%d app=%s stream=%s",
                      (unsigned)rtp.hdr.pt, slot->payload_type, slot->app, slot->stream);
        }
        return;
    }
    if (slot->enable_ssrc_filter && rtp.hdr.ssrc != slot->ssrc_filter) {
        if (slot->recv_pkts <= 3) {
            ztk_debug("[GB28181 zms] drop RTP ssrc=%u filter=%u app=%s stream=%s",
                      (unsigned)rtp.hdr.ssrc, (unsigned)slot->ssrc_filter, slot->app, slot->stream);
        }
        return;
    }
    if (!slot->rtp_ps_decoder) {
        return;
    }
    slot->recv_pkts++;
    if (slot->recv_pkts == 1) {
        ztk_info("[GB28181 zms 5/6] first RTP/PS app=%s stream=%s pt=%u ssrc=%u len=%zu from %s:%u "
                 "tcp_mode=%d",
                 slot->app, slot->stream, (unsigned)rtp.hdr.pt, (unsigned)rtp.hdr.ssrc, len,
                 peer_ip ? peer_ip : "?", (unsigned)peer_port, slot->tcp_mode);
    }
    (void)rtp_payload_decode_input(slot->rtp_ps_decoder, data, (int)len);
}

static int rtp_ps_tcp_asm_grow(zms_rtp_ps_server_slot *slot, size_t need)
{
    size_t cap;
    uint8_t *p;

    if (!slot) {
        return 0;
    }
    cap = slot->tcp_asm_cap ? slot->tcp_asm_cap : ZMS_RTP_PS_TCP_ASM_INIT;
    while (cap < need) {
        cap *= 2;
    }
    if (cap <= slot->tcp_asm_cap) {
        return 1;
    }
    p = (uint8_t *)realloc(slot->tcp_asm, cap);
    if (!p) {
        return 0;
    }
    slot->tcp_asm = p;
    slot->tcp_asm_cap = cap;
    return 1;
}

/** RFC4571：16 位 BE 长度 + RTP 包（GB28181 TCP/RTP/AVP） */
static void rtp_ps_tcp_feed(zms_rtp_ps_server_slot *slot, const void *data, size_t len,
                            const char *peer_ip, uint16_t peer_port)
{
    const uint8_t *in = (const uint8_t *)data;
    size_t off = 0;
    int first_raw = (slot->recv_pkts == 0 && slot->raw_udp_pkts == 0);

    if (!slot || !data || len == 0) {
        return;
    }
    slot->raw_udp_pkts++;
    if (!rtp_ps_tcp_asm_grow(slot, slot->tcp_asm_len + len)) {
        return;
    }
    memcpy(slot->tcp_asm + slot->tcp_asm_len, in, len);
    slot->tcp_asm_len += len;

    for (;;) {
        size_t remain = slot->tcp_asm_len;
        uint16_t pkt_len;

        if (remain < 2) {
            break;
        }
        pkt_len = (uint16_t)(((uint16_t)slot->tcp_asm[0] << 8) | slot->tcp_asm[1]);
        if (pkt_len < ZMS_RTP_HDR_SIZE || pkt_len > 65535) {
            ztk_warn("[GB28181 zms] bad TCP RTP length=%u app=%s stream=%s, resync",
                     (unsigned)pkt_len, slot->app, slot->stream);
            slot->tcp_asm_len = 0;
            break;
        }
        if (remain < (size_t)2 + pkt_len) {
            break;
        }
        rtp_ps_feed_rtp_packet(slot, slot->tcp_asm + 2, pkt_len, peer_ip, peer_port, first_raw);
        first_raw = 0;
        memmove(slot->tcp_asm, slot->tcp_asm + 2 + pkt_len, remain - 2 - pkt_len);
        slot->tcp_asm_len = remain - 2 - pkt_len;
    }
}

static void rtp_ps_on_udp_packet(ztk_udp_server *srv, const char *peer_ip, uint16_t peer_port,
                                 const void *data, size_t len, void *user)
{
    zms_rtp_ps_server_slot *slot = (zms_rtp_ps_server_slot *)user;

    (void)srv;
    if (!slot || !data || len < ZMS_RTP_HDR_SIZE) {
        return;
    }
    slot->raw_udp_pkts++;
    if (slot->raw_udp_pkts == 1) {
        ztk_debug("[GB28181 zms 4/6] first UDP from %s:%u len=%zu app=%s stream=%s expect_pt=%d "
                  "ssrc_filter=%s",
                  peer_ip ? peer_ip : "?", (unsigned)peer_port, len, slot->app, slot->stream,
                  slot->payload_type, slot->enable_ssrc_filter ? "on" : "off");
    }
    rtp_ps_feed_rtp_packet(slot, (const uint8_t *)data, len, peer_ip, peer_port, 0);
}

static void rtp_ps_tcp_session_close(zms_rtp_ps_server_slot *slot)
{
    if (!slot) {
        return;
    }
    if (slot->tcp_sess) {
        ztk_tcp_session_close(slot->tcp_sess);
        slot->tcp_sess = NULL;
    }
    if (slot->tcp_cli) {
        ztk_tcp_client_close(slot->tcp_cli);
        ztk_tcp_client_destroy(slot->tcp_cli);
        slot->tcp_cli = NULL;
    }
    slot->tcp_asm_len = 0;
}

static void rtp_ps_tcp_on_recv(ztk_tcp_session *session, const void *data, size_t len, void *user)
{
    zms_rtp_ps_server_slot *slot = (zms_rtp_ps_server_slot *)user;
    (void)session;
    if (!slot) {
        return;
    }
    rtp_ps_tcp_feed(slot, data, len, slot->tcp_peer_ip[0] ? slot->tcp_peer_ip : "?",
                    slot->tcp_peer_port ? slot->tcp_peer_port : 0);
}

static void rtp_ps_tcp_on_error(ztk_tcp_session *session, void *user)
{
    zms_rtp_ps_server_slot *slot = (zms_rtp_ps_server_slot *)user;
    (void)session;
    if (!slot) {
        return;
    }
    ztk_debug("[GB28181 zms] RTP/PS TCP session closed app=%s stream=%s", slot->app, slot->stream);
    slot->tcp_sess = NULL;
}

static void rtp_ps_tcp_client_on_connect(ztk_tcp_client *client, void *user)
{
    zms_rtp_ps_server_slot *slot = (zms_rtp_ps_server_slot *)user;
    (void)client;
    if (!slot) {
        return;
    }
    ztk_debug("[GB28181 zms 3/6] TCP-ACTIVE connected -> %s:%u app=%s stream=%s", slot->tcp_peer_ip,
              (unsigned)slot->tcp_peer_port, slot->app, slot->stream);
}

static void rtp_ps_tcp_client_on_recv(ztk_tcp_client *client, const void *data, size_t len,
                                      void *user)
{
    zms_rtp_ps_server_slot *slot = (zms_rtp_ps_server_slot *)user;
    (void)client;
    if (!slot) {
        return;
    }
    rtp_ps_tcp_feed(slot, data, len, slot->tcp_peer_ip, slot->tcp_peer_port);
}

static void rtp_ps_tcp_client_on_error(ztk_tcp_client *client, void *user)
{
    zms_rtp_ps_server_slot *slot = (zms_rtp_ps_server_slot *)user;
    (void)client;
    if (!slot) {
        return;
    }
    ztk_warn("[GB28181 zms] TCP-ACTIVE error app=%s stream=%s -> %s:%u", slot->app, slot->stream,
             slot->tcp_peer_ip, (unsigned)slot->tcp_peer_port);
    slot->tcp_cli = NULL;
}

static void rtp_ps_tcp_passive_on_recv(ztk_tcp_session *session, const void *data, size_t len,
                                       void *user)
{
    zms_rtp_ps_server_slot *slot = (zms_rtp_ps_server_slot *)user;
    ztk_socket *sock;
    char peer_ip[64];
    uint16_t peer_port = 0;

    if (!slot) {
        return;
    }
    if (slot->tcp_sess && slot->tcp_sess != session) {
        return;
    }
    if (!slot->tcp_sess) {
        slot->tcp_sess = session;
        sock = ztk_tcp_session_socket(session);
        if (sock) {
            (void)ztk_socket_get_peer(sock, peer_ip, sizeof(peer_ip), &peer_port);
        }
        strncpy(slot->tcp_peer_ip, peer_ip, sizeof(slot->tcp_peer_ip) - 1);
        slot->tcp_peer_port = peer_port;
        ztk_debug(
            "[GB28181 zms 3/6] TCP-PASSIVE accepted from %s:%u app=%s stream=%s listen_port=%u",
            peer_ip, (unsigned)peer_port, slot->app, slot->stream, (unsigned)slot->port);
    }
    rtp_ps_tcp_on_recv(session, data, len, slot);
}

static void rtp_ps_tcp_passive_on_error(ztk_tcp_session *session, void *user)
{
    zms_rtp_ps_server_slot *slot = (zms_rtp_ps_server_slot *)user;
    if (!slot) {
        return;
    }
    if (slot->tcp_sess == session) {
        slot->tcp_sess = NULL;
    }
    rtp_ps_tcp_on_error(session, slot);
}

static ztk_err_t rtp_ps_reserve_port_udp(uint16_t want_port, ztk_poller *poller, uint16_t *out_port)
{
    ztk_udp_server_opts_t uopts;
    static const ztk_udp_server_ops_t noop_ops = {NULL};
    ztk_udp_server *udp;

    memset(&uopts, 0, sizeof(uopts));
    uopts.host = "0.0.0.0";
    uopts.port = want_port;
    uopts.reuse = 1;
    uopts.poller = poller;
    uopts.ops = &noop_ops;
    udp = ztk_udp_server_create(&uopts);
    if (!udp || ztk_udp_server_start(udp) != ZTK_OK) {
        if (udp) {
            ztk_udp_server_destroy(udp);
        }
        return ZTK_ERR_STATE;
    }
    *out_port = ztk_udp_server_port(udp);
    ztk_udp_server_stop(udp);
    ztk_udp_server_destroy(udp);
    return ZTK_OK;
}

static ztk_err_t rtp_ps_tcp_passive_listen(zms_rtp_ps_server_slot *slot, uint16_t port)
{
    static const ztk_tcp_session_ops_t passive_ops = {
        rtp_ps_tcp_passive_on_recv,
        rtp_ps_tcp_passive_on_error,
        NULL,
    };
    ztk_tcp_server_opts_t topts;

    if (!slot || !slot->poller_pool) {
        return ZTK_ERR_INVALID;
    }
    memset(&topts, 0, sizeof(topts));
    topts.host = "0.0.0.0";
    topts.port = port;
    topts.backlog = 8;
    topts.poller_pool = slot->poller_pool;
    topts.session_ops = &passive_ops;
    topts.session_user = slot;
    slot->tcp_srv = ztk_tcp_server_create(&topts);
    if (!slot->tcp_srv || ztk_tcp_server_start(slot->tcp_srv) != ZTK_OK) {
        if (slot->tcp_srv) {
            ztk_tcp_server_destroy(slot->tcp_srv);
            slot->tcp_srv = NULL;
        }
        return ZTK_ERR_STATE;
    }
    slot->port = ztk_tcp_server_port(slot->tcp_srv);
    return ZTK_OK;
}

static ztk_err_t rtp_ps_udp_listen(zms_rtp_ps_server_slot *slot, uint16_t port)
{
    ztk_udp_server_opts_t uopts;
    static const ztk_udp_server_ops_t udp_ops = {rtp_ps_on_udp_packet};

    memset(&uopts, 0, sizeof(uopts));
    uopts.host = "0.0.0.0";
    uopts.port = port;
    uopts.reuse = 1;
    uopts.poller = slot->poller;
    uopts.ops = &udp_ops;
    uopts.user = slot;
    slot->udp = ztk_udp_server_create(&uopts);
    if (!slot->udp || ztk_udp_server_start(slot->udp) != ZTK_OK) {
        return ZTK_ERR_STATE;
    }
    slot->port = ztk_udp_server_port(slot->udp);
    return ZTK_OK;
}

static void rtp_ps_slot_destroy(zms_rtp_ps_server_slot *slot)
{
    if (!slot) {
        return;
    }
    rtp_ps_reg_remove(slot);
    rtp_ps_tcp_session_close(slot);
    if (slot->tcp_srv) {
        ztk_tcp_server_stop(slot->tcp_srv);
        ztk_tcp_server_destroy(slot->tcp_srv);
        slot->tcp_srv = NULL;
    }
    if (slot->udp) {
        ztk_udp_server_stop(slot->udp);
        ztk_udp_server_destroy(slot->udp);
        slot->udp = NULL;
    }
    free(slot->tcp_asm);
    slot->tcp_asm = NULL;
    slot->tcp_asm_cap = slot->tcp_asm_len = 0;
    rtp_ps_slot_teardown_media(slot);
    free(slot);
}

void zms_rtp_ps_server_close(zms_rtp_ps_server_slot *slot)
{
    rtp_ps_slot_destroy(slot);
}

ztk_err_t zms_rtp_ps_server_connect(ztk_poller *poller, const char *vhost, const char *app,
                                    const char *stream, const char *host, uint16_t port)
{
    zms_rtp_ps_server_slot *slot;
    char key[256];
    ztk_tcp_client_opts_t copts;
    static const ztk_tcp_client_ops_t cli_ops = {
        rtp_ps_tcp_client_on_connect,
        rtp_ps_tcp_client_on_recv,
        rtp_ps_tcp_client_on_error,
    };

    if (!poller || !app || !stream || !host || !host[0] || port == 0) {
        return ZTK_ERR_INVALID;
    }
    zms_rtp_ps_server_make_key(vhost, app, stream, key, sizeof(key));
    slot = zms_rtp_ps_server_find_by_key(key);
    if (!slot) {
        return ZTK_ERR_INVALID;
    }
    if (slot->tcp_mode != ZMS_RTP_PS_TCP_ACTIVE) {
        return ZTK_ERR_INVALID;
    }
    if (slot->tcp_cli || slot->tcp_sess) {
        return ZTK_ERR_STATE;
    }

    strncpy(slot->tcp_peer_ip, host, sizeof(slot->tcp_peer_ip) - 1);
    slot->tcp_peer_port = port;
    slot->poller = poller;

    memset(&copts, 0, sizeof(copts));
    copts.poller = poller;
    copts.ops = &cli_ops;
    copts.user = slot;
    slot->tcp_cli = ztk_tcp_client_create(&copts);
    if (!slot->tcp_cli) {
        return ZTK_ERR_NOMEM;
    }
    ztk_debug("[GB28181 zms 3/6] TCP-ACTIVE connecting -> %s:%u app=%s stream=%s", host,
              (unsigned)port, app, stream);
    if (ztk_tcp_client_connect(slot->tcp_cli, host, port) != ZTK_OK) {
        ztk_tcp_client_destroy(slot->tcp_cli);
        slot->tcp_cli = NULL;
        return ZTK_ERR_IO;
    }
    return ZTK_OK;
}

ztk_err_t zms_rtp_ps_server_open(const zms_rtp_ps_server_open_opts *opts,
                                 zms_rtp_ps_server_slot **out_slot, uint16_t *out_port)
{
    zms_rtp_ps_server_slot *slot;
    zms_rtp_ps_server_open_opts local;
    const char *app;
    const char *stream;
    char key[256];
    zms_rtp_ps_server_slot *old;
    ztk_err_t err;
    const char *mode_name;

    if (!opts || !opts->poller || !out_slot) {
        return ZTK_ERR_INVALID;
    }
    local = *opts;
    app = local.app && local.app[0] ? local.app : "live";
    stream = local.stream && local.stream[0] ? local.stream : NULL;
    if (!stream) {
        return ZTK_ERR_INVALID;
    }
    if (local.tcp_mode < 0) {
        local.tcp_mode = ZMS_RTP_PS_TCP_UDP;
    }
    if (local.tcp_mode == ZMS_RTP_PS_TCP_PASSIVE && !local.poller_pool) {
        return ZTK_ERR_INVALID;
    }

    zms_rtp_ps_server_make_key(local.vhost, app, stream, key, sizeof(key));
    old = zms_rtp_ps_server_find_by_key(key);
    if (old) {
        /* 双击点播/重复 openRtpServer：复用已有监听，避免 close+重建竞态崩溃 */
        if (old->tcp_mode == local.tcp_mode && old->port != 0 &&
            (local.port == 0 || local.port == old->port)) {
            ztk_info("[GB28181 zms 2/6] openRtpServer reuse key=%s port=%u pt=%d tcp_mode=%d "
                     "publish=%d",
                     key, (unsigned)old->port, old->payload_type, old->tcp_mode,
                     old->publish_started);
            *out_slot = old;
            if (out_port) {
                *out_port = old->port;
            }
            return ZTK_OK;
        }
        ztk_info("[GB28181 zms 2/6] openRtpServer replace key=%s old_port=%u -> new (mode %d->%d)",
                 key, (unsigned)old->port, old->tcp_mode, local.tcp_mode);
        zms_rtp_ps_server_close(old);
    }

    slot = (zms_rtp_ps_server_slot *)calloc(1, sizeof(*slot));
    if (!slot) {
        return ZTK_ERR_NOMEM;
    }
    strncpy(slot->key, key, sizeof(slot->key) - 1);
    strncpy(slot->app, app, sizeof(slot->app) - 1);
    strncpy(slot->stream, stream, sizeof(slot->stream) - 1);
    slot->poller = local.poller;
    slot->poller_pool = local.poller_pool;
    slot->tcp_mode = local.tcp_mode;
    slot->payload_type = local.payload_type > 0 ? local.payload_type : ZMS_RTP_PS_DEFAULT_PT;
    slot->ssrc_filter = local.ssrc;
    slot->enable_ssrc_filter = local.enable_ssrc_filter;

    if (rtp_ps_slot_create_media(slot, app, stream) != ZTK_OK) {
        free(slot);
        return ZTK_ERR_STATE;
    }

    switch (slot->tcp_mode) {
    case ZMS_RTP_PS_TCP_PASSIVE:
        err = rtp_ps_tcp_passive_listen(slot, local.port);
        mode_name = "TCP-PASSIVE listen";
        break;
    case ZMS_RTP_PS_TCP_ACTIVE:
        err = rtp_ps_reserve_port_udp(local.port, slot->poller, &slot->port);
        mode_name = "TCP-ACTIVE (await connectRtpServer)";
        break;
    default:
        err = rtp_ps_udp_listen(slot, local.port);
        mode_name = "UDP";
        slot->tcp_mode = ZMS_RTP_PS_TCP_UDP;
        break;
    }
    if (err != ZTK_OK) {
        rtp_ps_slot_destroy(slot);
        return err;
    }

    rtp_ps_reg_add(slot);
    ++g_rtp_ps_session_no;
    ztk_info("[GB28181 zms 2/6] openRtpServer #%u key=%s port=%u pt=%d mode=%s ssrc_filter=%s",
             g_rtp_ps_session_no, key, (unsigned)slot->port, slot->payload_type, mode_name,
             local.enable_ssrc_filter ? "on" : "off");

    *out_slot = slot;
    if (out_port) {
        *out_port = slot->port;
    }
    return ZTK_OK;
}
