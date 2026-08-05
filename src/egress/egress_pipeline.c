/**
 * @file egress_pipeline.c
 * @brief 出站管线：RTSP/RTP 与 FLV/RTMP 播放路径。
 *
 * Copyright (c) zero-media-server
 */
#include "zms/egress/egress_pipeline.h"
#include "zms/egress/egress_live_policy.h"
#include "zms/vod/io/vod_reader.h"
#include "zms/media/codec/h265/h265_es.h"
#include "zms/engine/frame.h"
#include "zms/engine/gop/gop_queue.h"
#include "zms/engine/media_clock.h"
#include "zms/engine/media/media_limits.h"
#include "zms/engine/stream/stream_hub.h"
#include "zms/egress/egress_pacing.h"
#include "zms/engine/stream/stream_stats.h"
#include "zms/media/container/flv/flv_tag_pack.h"
#include "zms/media/container/flv/flv_types.h"
#include "zms/egress/egress_clock.h"
#include "zms/util/buf_pool.h"
#include "zms/util/log_throttle.h"
#include "zms/vod/io/vod_buffer.h"
#include "ztk/poller/poller.h"
#include "ztk/util/buf.h"
#include "ztk/util/log.h"
#include <stdlib.h>
#include <string.h>

#define PLAYBACK_FLV_BUDGET_VOD_SEEK ZMS_EGRESS_FRAME_BUDGET_CATCHUP
#define PLAYBACK_VOD_TAG_BUF_CAP 65536u
#define PLAYBACK_FLV_OUTBOX_CAP 256u
#define PLAYBACK_FLV_OUTBOX_FLUSH 32u

typedef struct zms_egress_flv_out_slot {
    uint8_t msg_type;
    uint32_t tag_dts_ms;
    ztk_buf *buf;
} zms_egress_flv_out_slot;

static inline size_t egress_flv_tag_cap(const zms_gop_slot *slot)
{
    size_t es = slot ? slot->len : 0;
    return es + 64u;
}

struct zms_egress_pipeline {
    zms_wire_format_id wire;
    zms_container_id container;
    zms_egress_source *reader;
    zms_egress_wire_cb on_wire;
    void *wire_user;
    zms_rtp_muxer *rtsp_mux;
    int rtsp_mux_owned;
    zms_egress_flv_bind flv;
    int has_flv;
    uint8_t *tag_buf;
    size_t tag_cap;
    uint8_t *es_buf;
    size_t es_cap;
    ztk_poller *io_poller;
    zms_egress_flv_out_slot *flv_out;
    unsigned flv_out_cap;
    size_t flv_out_r;
    size_t flv_out_w;
    size_t flv_out_n;
};

static void *egress_slot_resize(zms_egress_pipeline *p, uint8_t **data, size_t *cap, size_t len)
{
    if (!data || !cap || len == 0) {
        return NULL;
    }
    if (p && p->io_poller) {
        return zms_buf_pool_slot_resize_poller(data, cap, len, p->io_poller);
    }
    return zms_buf_pool_slot_resize(data, cap, len);
}

static void egress_slot_clear(zms_egress_pipeline *p, uint8_t **data, size_t *cap)
{
    if (!data) {
        return;
    }
    if (p && p->io_poller) {
        zms_buf_pool_slot_clear_poller(data, cap, p->io_poller);
    } else {
        zms_buf_pool_slot_clear(data, cap);
    }
}

static int flv_video_armed(const zms_egress_flv_bind *flv)
{
    return flv && flv->video_armed && *flv->video_armed;
}

static void flv_set_video_armed(const zms_egress_flv_bind *flv, int v)
{
    if (flv && flv->video_armed) {
        *flv->video_armed = v;
    }
}

/*
 * 首个视频关键帧时初始化时间线原点、时钟 epoch 并打日志。
 * 仅当 flv_video_armed 为 false 时每会话调用一次。
 */
static void egress_flv_on_first_video(const zms_egress_flv_bind *flv, zms_egress_clock *clk,
                                      uint32_t dts_ms, unsigned session_no, size_t es_len)
{
    flv_set_video_armed(flv, 1);
    if (flv->timeline) {
        zms_mux_av_timeline_lock_origin(flv->timeline, dts_ms);
    }
    if (clk) {
        (void)zms_egress_clock_lock_epoch(clk, dts_ms);
    }
    if (session_no) {
        ztk_debug("[egress] first_video session=%u ts=%u es=%u", session_no, (unsigned)dts_ms,
                  (unsigned)es_len);
    }
}

/** 热路径走会话 tag 槽（buf_pool），避免每帧 4K 栈缓冲。 */
static uint8_t *egress_tag_buf(zms_egress_pipeline *p, size_t need)
{
    if (!p || need == 0) {
        return NULL;
    }
    if (!egress_slot_resize(p, &p->tag_buf, &p->tag_cap, need)) {
        return NULL;
    }
    return p->tag_buf;
}

static uint32_t egress_flv_out_ts_ex(const zms_egress_flv_bind *flv, const zms_gop_slot *slot,
                                     uint8_t type_id)
{
    zms_track_type track = (type_id == ZMS_FLV_TAG_VIDEO) ? ZMS_TRACK_VIDEO : ZMS_TRACK_AUDIO;

    if (flv && flv->timeline && slot) {
        return zms_mux_av_timeline_pts(flv->timeline, track, slot->dts_ms);
    }
    return slot ? slot->dts_ms : 0;
}

static int egress_flv_store_tag(zms_egress_pipeline *p, uint8_t type_id, uint32_t ts,
                                const uint8_t *body, size_t len, zms_egress_flv_tag *out)
{
    if (!p || !body || len == 0 || !out) {
        return 0;
    }
    if (!egress_slot_resize(p, &p->tag_buf, &p->tag_cap, len)) {
        return 0;
    }
    memcpy(p->tag_buf, body, len);
    out->msg_type = type_id;
    out->tag_dts_ms = ts;
    out->body = p->tag_buf;
    out->len = len;
    return 1;
}

static void egress_flv_outbox_clear(zms_egress_pipeline *p)
{
    size_t i;

    if (!p || !p->flv_out) {
        return;
    }
    for (i = 0; i < p->flv_out_cap; ++i) {
        if (p->flv_out[i].buf) {
            ztk_buf_unref(p->flv_out[i].buf);
        }
        p->flv_out[i].buf = NULL;
    }
    p->flv_out_r = p->flv_out_w = p->flv_out_n = 0;
}

static int egress_flv_outbox_init(zms_egress_pipeline *p)
{
    if (!p || p->flv_out) {
        return 0;
    }
    p->flv_out_cap = (unsigned)PLAYBACK_FLV_OUTBOX_CAP;
    p->flv_out = (zms_egress_flv_out_slot *)calloc(p->flv_out_cap, sizeof(*p->flv_out));
    return p->flv_out ? 0 : -1;
}

static int egress_flv_outbox_push(zms_egress_pipeline *p, uint8_t type_id, uint32_t ts,
                                  const uint8_t *body, size_t len)
{
    zms_egress_flv_out_slot *slot;
    ztk_buf *buf;
    void *dst;

    if (!p || !body || len == 0 || !p->flv_out) {
        return -1;
    }
    if (p->flv_out_n >= p->flv_out_cap) {
        slot = &p->flv_out[p->flv_out_r];
        if (slot->buf) {
            ztk_buf_unref(slot->buf);
        }
        slot->buf = NULL;
        p->flv_out_r = (p->flv_out_r + 1) % p->flv_out_cap;
        p->flv_out_n--;
    }
    buf = p->io_poller ? ztk_buf_alloc_local(p->io_poller, len) : ztk_buf_alloc(len);
    if (!buf) {
        return -1;
    }
    dst = (void *)ztk_buf_data(buf);
    memcpy(dst, body, len);
    ztk_buf_set_len(buf, len);
    slot = &p->flv_out[p->flv_out_w];
    if (slot->buf) {
        ztk_buf_unref(slot->buf);
    }
    slot->msg_type = type_id;
    slot->tag_dts_ms = ts;
    slot->buf = buf;
    p->flv_out_w = (p->flv_out_w + 1) % p->flv_out_cap;
    p->flv_out_n++;
    return 0;
}

int zms_egress_pipeline_flush_flv_outbox(zms_egress_pipeline *p, int tag_budget)
{
    const zms_egress_flv_bind *flv;

    if (!p || tag_budget <= 0 || !p->flv_out || p->flv_out_n == 0) {
        return 0;
    }
    flv = p->has_flv ? &p->flv : NULL;
    if (!flv || !flv->on_tag) {
        return 0;
    }

    {
        int sent = 0;
        while (tag_budget-- > 0 && p->flv_out_n > 0) {
            zms_egress_flv_out_slot *slot = &p->flv_out[p->flv_out_r];

            if (slot->buf) {
                size_t blen = ztk_buf_len(slot->buf);

                if (flv->source) {
                    zms_media_stats_on_egress(flv->source, blen);
                }
                flv->on_tag(slot->msg_type, slot->tag_dts_ms,
                            (const uint8_t *)ztk_buf_data(slot->buf), blen, flv->user);
                ztk_buf_unref(slot->buf);
                slot->buf = NULL;
            }
            p->flv_out_r = (p->flv_out_r + 1) % p->flv_out_cap;
            p->flv_out_n--;
            sent++;
        }
        return sent;
    }
}

size_t zms_egress_pipeline_flv_outbox_pending(const zms_egress_pipeline *p)
{
    return p ? p->flv_out_n : 0;
}

static void egress_flv_dispatch_tag(zms_egress_pipeline *p, const zms_egress_flv_bind *flv,
                                    uint8_t type_id, uint32_t ts, const uint8_t *body, size_t len)
{
    if (!flv || !body || len == 0) {
        return;
    }
    if (p && p->wire == ZMS_WIRE_FORMAT_RTMP && p->flv_out) {
        if (egress_flv_outbox_push(p, type_id, ts, body, len) == 0) {
            return;
        }
    }
    if (flv->source) {
        zms_media_stats_on_egress(flv->source, len);
    }
    if (flv->on_tag) {
        flv->on_tag(type_id, ts, body, len, flv->user);
    }
}

static ztk_err_t egress_mux_flv_tag(const zms_egress_flv_bind *flv, const zms_gop_slot *slot,
                                    uint32_t pts_ms, uint8_t *tag_buf, size_t tag_cap,
                                    size_t *tag_len, uint8_t *type_id)
{
    zms_flv_tag_pack_req req;
    zms_flv_tag_pack_out out;
    ztk_err_t err;

    (void)flv;
    if (!slot || !tag_buf || !tag_len || !type_id) {
        return ZTK_ERR_INVALID;
    }
    req.slot = slot;
    req.pts_ms = pts_ms;
    req.buf = tag_buf;
    req.cap = tag_cap;
    if (flv && flv->source) {
        req.video_cfg = zms_media_source_video_config(flv->source, &req.video_cfg_len);
        if (!req.video_cfg) {
            req.video_cfg_len = 0;
        }
    }
    err = zms_flv_tag_pack(&req, &out);
    if (err == ZTK_OK) {
        *tag_len = out.tag_len;
        *type_id = out.rtmp_msg_type;
    } else if (err == ZTK_ERR_NOT_IMPL && slot) {
        zms_log_warn_throttle("egress:flv_codec_unsupported", ZMS_LOG_THROTTLE_WARN_MS,
                              "[egress] flv_codec_unsupported track=%d codec=%s", (int)slot->track,
                              zms_codec_name(slot->codec));
    }
    return err;
}

zms_egress_pipeline *zms_egress_pipeline_create(const zms_egress_pipeline_opts *opts)
{
    zms_egress_pipeline *p;
    const zms_egress_rtsp_bind *rtsp;

    if (!opts || !opts->reader) {
        return NULL;
    }
    rtsp = opts->rtsp;
    if (opts->wire == ZMS_WIRE_FORMAT_RTP) {
        if (!rtsp) {
            return NULL;
        }
        if (!rtsp->mux && (!rtsp->opts || !rtsp->on_rtp)) {
            return NULL;
        }
    }
    if (opts->wire == ZMS_WIRE_FORMAT_RTMP && (!opts->flv || !opts->flv->on_tag)) {
        return NULL;
    }
    if (opts->wire == ZMS_WIRE_FORMAT_HTTP_FLV && !opts->flv) {
        return NULL;
    }

    p = (zms_egress_pipeline *)calloc(1, sizeof(*p));
    if (!p) {
        return NULL;
    }
    p->wire = opts->wire;
    p->container = opts->container;
    p->reader = opts->reader;
    p->on_wire = opts->on_wire;
    p->wire_user = opts->user;
    if (opts->wire == ZMS_WIRE_FORMAT_RTP && rtsp) {
        if (rtsp->mux) {
            p->rtsp_mux = rtsp->mux;
            p->rtsp_mux_owned = 0;
        } else {
            p->rtsp_mux = zms_rtp_muxer_create(rtsp->opts, rtsp->on_rtp, rtsp->user);
            p->rtsp_mux_owned = p->rtsp_mux ? 1 : 0;
            if (!p->rtsp_mux) {
                free(p);
                return NULL;
            }
        }
    }
    if (opts->flv) {
        p->flv = *opts->flv;
        p->has_flv = 1;
        if (opts->wire == ZMS_WIRE_FORMAT_RTMP && egress_flv_outbox_init(p) != 0) {
            zms_egress_pipeline_destroy(p);
            return NULL;
        }
    }
    return p;
}

void zms_egress_pipeline_destroy(zms_egress_pipeline *p)
{
    if (!p) {
        return;
    }
    if (p->rtsp_mux_owned) {
        zms_rtp_muxer_destroy(p->rtsp_mux);
    }
    p->rtsp_mux = NULL;
    egress_flv_outbox_clear(p);
    free(p->flv_out);
    p->flv_out = NULL;
    egress_slot_clear(p, &p->tag_buf, &p->tag_cap);
    egress_slot_clear(p, &p->es_buf, &p->es_cap);
    free(p);
}

void zms_egress_pipeline_bind_poller(zms_egress_pipeline *p, ztk_poller *pol)
{
    if (p) {
        p->io_poller = pol;
    }
}

zms_rtp_muxer *zms_egress_pipeline_rtsp_mux(const zms_egress_pipeline *p)
{
    return p ? p->rtsp_mux : NULL;
}

size_t zms_egress_pipeline_gop_lag(const zms_egress_pipeline *p)
{
    zms_gop_reader *rd;

    if (!p || !p->reader) {
        return 0;
    }
    rd = p->reader->readers.gop;
    return rd ? zms_gop_reader_lag(rd) : 0;
}

void zms_egress_pipeline_jump_live(zms_egress_pipeline *p)
{
    if (p && p->rtsp_mux) {
        zms_rtp_muxer_jump_live(p->rtsp_mux);
    }
}

void zms_egress_pipeline_snap_live(zms_egress_pipeline *p)
{
    zms_gop_reader *rd;

    if (!p || !p->reader) {
        return;
    }
    rd = p->reader->readers.gop;
    zms_egress_live_snap_gop(rd, p->rtsp_mux);
}

/** FLV 直播：tag 1.4 peek pacing + lag catchup（见 play_pacing.h）。 */
static int egress_flv_live_step(zms_egress_pipeline *p, zms_egress_flv_tag *out,
                                int read_timeout_ms, unsigned session_no,
                                const zms_egress_live_state *live)
{
    zms_gop_reader *rd;
    const zms_egress_flv_bind *flv;
    zms_egress_clock *clk;
    zms_gop_slot peek;
    zms_gop_slot slot;
    size_t lag;
    int catchup;
    int catchup_done;
    int pace;
    uint8_t *tag_buf;
    size_t tag_len = 0;
    size_t tag_cap;
    uint8_t type_id = 0;
    uint32_t out_ts;

    if (!p || !p->reader || !p->has_flv) {
        return 0;
    }
    if (p->wire != ZMS_WIRE_FORMAT_RTMP && p->wire != ZMS_WIRE_FORMAT_HTTP_FLV) {
        return 0;
    }
    flv = &p->flv;
    rd = p->reader->readers.gop;
    if (!rd || (!flv->on_tag && !out)) {
        return 0;
    }
    if (read_timeout_ms <= 0) {
        read_timeout_ms = 500;
    }

    clk = flv->play_clk;
    if (clk && zms_egress_clock_is_paused(clk)) {
        return 0;
    }

    lag = zms_gop_reader_lag(rd);
    catchup = zms_egress_live_lag_catchup(lag);
    catchup_done = live && live->live_catchup_done && *live->live_catchup_done;
    /* GOP catchup 后 lag<=16 时启用 pacing。 */
    pace = catchup_done && !catchup;

    if (zms_egress_live_peek_paced(rd, &peek, clk, pace, read_timeout_ms) <= 0) {
        return 0;
    }

    if (zms_gop_reader_read_muxed(rd, &slot, read_timeout_ms) <= 0) {
        return 0;
    }
    if (slot.len == 0 || !slot.data) {
        return 0;
    }

    if (slot.track == ZMS_TRACK_VIDEO) {
        zms_gop_slot_refresh_play_key(&slot);
        if (!flv_video_armed(flv)) {
            if (!zms_gop_slot_is_play_start(&slot)) {
                return 2;
            }
            egress_flv_on_first_video(flv, clk, slot.dts_ms, session_no, slot.len);
        }
    } else if (!flv_video_armed(flv)) {
        return 2;
    }

    if (clk && !zms_egress_clock_epoch_locked(clk)) {
        (void)zms_egress_clock_lock_epoch(clk, slot.dts_ms);
    }

    tag_cap = egress_flv_tag_cap(&slot);
    tag_buf = egress_tag_buf(p, tag_cap);
    if (!tag_buf) {
        return 0;
    }
    if (egress_mux_flv_tag(flv, &slot, zms_gop_slot_pts_ms(&slot), tag_buf, tag_cap, &tag_len,
                           &type_id) != ZTK_OK) {
        return 0;
    }

    out_ts = egress_flv_out_ts_ex(flv, &slot, type_id);
    if (out) {
        if (!egress_flv_store_tag(p, type_id, out_ts, tag_buf, tag_len, out)) {
            return 0;
        }
    } else {
        egress_flv_dispatch_tag(p, flv, type_id, out_ts, tag_buf, tag_len);
    }

    if (catchup && zms_gop_reader_lag(rd) <= ZMS_EGRESS_CATCHUP_LAG && clk) {
        zms_egress_clock_sync_wall(clk, slot.dts_ms);
    }
    return 1;
}

int zms_egress_pipeline_pump_flv_live(zms_egress_pipeline *p, int frame_budget, int read_timeout_ms,
                                      unsigned session_no, const zms_egress_live_state *live)
{
    zms_gop_reader *rd;
    const zms_egress_flv_bind *flv;
    size_t lag;
    int frames = 0;
    int pump_budget;

    if (!p || !p->reader || !p->has_flv || p->wire != ZMS_WIRE_FORMAT_RTMP) {
        return 0;
    }
    flv = &p->flv;
    rd = p->reader->readers.gop;
    if (!rd || !flv->on_tag) {
        return 0;
    }

    if (read_timeout_ms <= 0) {
        read_timeout_ms = 500;
    }

    {
        int woke;
        int epoch0;
        int catchup0;

        lag = zms_gop_reader_lag(rd);
        woke = zms_gop_reader_drain_wake(rd);
        epoch0 = flv->play_clk && zms_egress_clock_epoch_locked(flv->play_clk);
        catchup0 = live && live->live_catchup_done && *live->live_catchup_done;
        if (!zms_egress_live_should_pump(rd, woke, catchup0, epoch0)) {
            (void)zms_egress_pipeline_flush_flv_outbox(p, (int)PLAYBACK_FLV_OUTBOX_FLUSH);
            return frames;
        }
        if (frame_budget <= 0) {
            frame_budget =
                zms_egress_live_pump_budget(epoch0, catchup0, lag, (int)ZMS_EGRESS_FLV_BUDGET_LIVE);
        }
    }
    pump_budget = frame_budget;
    if (live && zms_egress_live_flv_prep(rd, live, session_no, &pump_budget)) {
        (void)zms_egress_pipeline_flush_flv_outbox(p, (int)PLAYBACK_FLV_OUTBOX_FLUSH);
        return frames;
    }

    while (pump_budget-- > 0) {
        int n = egress_flv_live_step(p, NULL, read_timeout_ms, session_no, live);

        if (n == 1) {
            ++frames;
        } else if (n <= 0) {
            break;
        }
    }
    if (!flv_video_armed(flv) && frames == 0) {
        zms_gop_reader_seek_gop_key(rd);
    }

    (void)zms_egress_pipeline_flush_flv_outbox(p, (int)PLAYBACK_FLV_OUTBOX_FLUSH);
    return frames;
}

static int egress_flv_pull_one_live(zms_egress_pipeline *p, zms_egress_flv_tag *out,
                                    int read_timeout_ms, unsigned session_no,
                                    const zms_egress_live_state *live)
{
    zms_gop_reader *rd;
    const zms_egress_flv_bind *flv;

    if (!p || !p->reader || !p->has_flv || !out) {
        return 0;
    }
    if (p->wire != ZMS_WIRE_FORMAT_RTMP && p->wire != ZMS_WIRE_FORMAT_HTTP_FLV) {
        return 0;
    }
    flv = &p->flv;
    rd = p->reader->readers.gop;
    if (!rd) {
        return 0;
    }
    if (read_timeout_ms <= 0) {
        read_timeout_ms = 500;
    }

    if (live && zms_egress_live_flv_prep(rd, live, session_no, NULL)) {
        return 0;
    }

    {
        int n;
        int skip_budget = (int)ZMS_EGRESS_FRAME_BUDGET_CATCHUP;

        while (skip_budget-- > 0) {
            n = egress_flv_live_step(p, out, read_timeout_ms, session_no, live);
            if (n == 1) {
                return 1;
            }
            if (n <= 0) {
                return 0;
            }
        }
    }
    return 0;
}

int zms_egress_pipeline_pull_flv_live(zms_egress_pipeline *p, zms_egress_flv_tag *out,
                                      int read_timeout_ms, unsigned session_no,
                                      const zms_egress_live_state *live)
{
    return egress_flv_pull_one_live(p, out, read_timeout_ms, session_no, live);
}

static int egress_flv_vod_step(zms_egress_pipeline *p, const zms_flv_vod_egress_bind *vcfg,
                               zms_egress_flv_tag *out, int *catchup_io, unsigned session_no)
{
    const zms_egress_flv_bind *flv;
    zms_vod_buffer_reader *vod_rd;
    zms_egress_clock *clk;
    int catchup;
    zms_gop_slot slot;
    size_t tag_len = 0;
    uint8_t type_id = 0;
    uint8_t *tag_buf;
    size_t tag_cap;
    uint32_t out_ts;

    if (!p || !p->has_flv || !vcfg || !vcfg->vod_rd) {
        return 0;
    }
    if (p->wire != ZMS_WIRE_FORMAT_RTMP && p->wire != ZMS_WIRE_FORMAT_HTTP_FLV) {
        return 0;
    }
    flv = &p->flv;
    vod_rd = vcfg->vod_rd;
    clk = vcfg->play_clk ? vcfg->play_clk : flv->play_clk;
    catchup = catchup_io && *catchup_io > 0;

    if (clk && zms_egress_clock_is_paused(clk)) {
        return 0;
    }
    if (zms_vod_buffer_reader_peek_muxed_es(vod_rd, &slot, vcfg->es_buf, vcfg->es_cap,
                                            p->io_poller) <= 0) {
        return 0;
    }
    if (slot.len == 0 || !slot.data) {
        zms_vod_buffer_reader_advance(vod_rd);
        return 0;
    }
    if (slot.track == ZMS_TRACK_VIDEO) {
        zms_gop_slot_refresh_sync_key(&slot);
    }
    if (slot.track == ZMS_TRACK_VIDEO) {
        if (!flv_video_armed(flv)) {
            if (!slot.keyframe) {
                zms_vod_buffer_reader_advance(vod_rd);
                return 0;
            }
            flv_set_video_armed(flv, 1);
        }
    } else if (!flv_video_armed(flv)) {
        zms_vod_buffer_reader_advance(vod_rd);
        return 0;
    }
    if (!catchup && vcfg && vcfg->pace_when_locked && clk && zms_egress_clock_epoch_locked(clk) &&
        !zms_egress_clock_media_due(clk, slot.dts_ms, ZMS_EGRESS_PACE_LEAD_MS)) {
        return 0;
    }

    tag_cap = egress_flv_tag_cap(&slot);
    if (tag_cap > PLAYBACK_VOD_TAG_BUF_CAP && tag_cap > slot.len + 65536u) {
        tag_cap = slot.len + 65536u;
    }
    tag_buf = egress_tag_buf(p, tag_cap);
    if (!tag_buf) {
        zms_vod_buffer_reader_advance(vod_rd);
        return 0;
    }
    if (egress_mux_flv_tag(flv, &slot, zms_gop_slot_pts_ms(&slot), tag_buf, tag_cap, &tag_len,
                           &type_id) != ZTK_OK) {
        zms_vod_buffer_reader_advance(vod_rd);
        return 0;
    }
    zms_vod_buffer_reader_advance(vod_rd);
    out_ts = slot.dts_ms;

    if (out) {
        if (!egress_flv_store_tag(p, type_id, out_ts, tag_buf, tag_len, out)) {
            return 0;
        }
    } else {
        egress_flv_dispatch_tag(p, flv, type_id, out_ts, tag_buf, tag_len);
    }

    if (catchup && vcfg && vcfg->pace_when_locked &&
        zms_vod_buffer_reader_lag(vod_rd) <= ZMS_EGRESS_CATCHUP_LAG) {
        zms_egress_clock_rebase(clk, slot.dts_ms);
        catchup = 0;
        if (catchup_io) {
            *catchup_io = 0;
        }
    }
    return 1;
}

int zms_egress_pipeline_pull_flv_vod(zms_egress_pipeline *p, const zms_flv_vod_egress_bind *vcfg,
                                     zms_egress_flv_tag *out, unsigned session_no)
{
    if (!out) {
        return 0;
    }
    return egress_flv_vod_step(p, vcfg, out, vcfg ? vcfg->catchup_left : NULL, session_no);
}

int zms_egress_pipeline_pump_flv_vod(zms_egress_pipeline *p, const zms_flv_vod_egress_bind *vcfg,
                                     int frame_budget, unsigned session_no)
{
    int tags = 0;
    int pump_budget;

    if (!p || !p->has_flv || !vcfg || !vcfg->vod_rd) {
        return 0;
    }
    if (p->wire != ZMS_WIRE_FORMAT_RTMP && p->wire != ZMS_WIRE_FORMAT_HTTP_FLV) {
        return 0;
    }

    if (frame_budget <= 0) {
        int catchup = vcfg->catchup_left && *vcfg->catchup_left > 0;
        frame_budget =
            catchup ? (int)ZMS_EGRESS_FRAME_BUDGET_CATCHUP : (int)ZMS_EGRESS_FLV_BUDGET_VOD;
    }
    pump_budget = frame_budget;

    while (pump_budget-- > 0) {
        if (egress_flv_vod_step(p, vcfg, NULL, vcfg->catchup_left, session_no) <= 0) {
            break;
        }
        ++tags;
    }

    if (vcfg->catchup_left && *vcfg->catchup_left > 0) {
        *vcfg->catchup_left = (pump_budget >= 0 ? pump_budget : 0);
    }
    (void)zms_egress_pipeline_flush_flv_outbox(p, (int)PLAYBACK_FLV_OUTBOX_FLUSH);
    return tags;
}

int zms_egress_pipeline_pump_vod(zms_egress_pipeline *p, zms_vod_buffer_reader *vod_rd,
                                 zms_vod_reader *vod_demux, int frame_budget, unsigned session_no)
{
    const zms_egress_clock *clk;
    int catchup;
    int frames = 0;
    int pump_budget;

    if (!p || !p->rtsp_mux || p->wire != ZMS_WIRE_FORMAT_RTP || !vod_rd) {
        return 0;
    }

    if (frame_budget <= 0) {
        frame_budget = (int)ZMS_EGRESS_FRAME_BUDGET_LIVE;
    }
    pump_budget = frame_budget;
    catchup = zms_rtp_muxer_catchup_on(p->rtsp_mux);
    if (catchup && pump_budget < (int)ZMS_EGRESS_FRAME_BUDGET_CATCHUP) {
        pump_budget = (int)ZMS_EGRESS_FRAME_BUDGET_CATCHUP;
    }

    while (pump_budget-- > 0) {
        zms_gop_slot slot;
        uint8_t *es_buf;
        size_t es_cap;
        int muxed;

        es_buf = p->es_buf;
        es_cap = p->es_cap;
        if (zms_vod_buffer_reader_peek_muxed_es(vod_rd, &slot, &es_buf, &es_cap, p->io_poller) <=
            0) {
            int refilled = 0;
            if (vod_demux) {
                int i;
                for (i = 0; i < 8; ++i) {
                    if (zms_vod_buffer_reader_lag(vod_rd) >= 48) {
                        break;
                    }
                    if (zms_vod_reader_pump(vod_demux) <= 0) {
                        break;
                    }
                    ++refilled;
                }
            }
            if (refilled > 0) {
                continue;
            }
            if (session_no && zms_vod_buffer_reader_lag(vod_rd) > 0) {
                ztk_warn("[egress] vod_peek_fail session=%u lag=%zu need_cap=%zu", session_no,
                         zms_vod_buffer_reader_lag(vod_rd), es_cap);
            }
            break;
        }
        p->es_buf = es_buf;
        p->es_cap = es_cap;

        if (slot.len == 0 || !slot.data) {
            zms_vod_buffer_reader_advance(vod_rd);
            continue;
        }
        clk = zms_rtp_muxer_play_clock(p->rtsp_mux);
        catchup = zms_rtp_muxer_catchup_on(p->rtsp_mux);
        if (!catchup && clk && zms_egress_clock_epoch_locked(clk) &&
            !zms_egress_clock_media_due(clk, slot.dts_ms, ZMS_EGRESS_PACE_LEAD_MS)) {
            break;
        }
        if (slot.track == ZMS_TRACK_VIDEO) {
            zms_gop_slot_refresh_sync_key(&slot);
        }
        {
            int log_first_v = session_no && slot.track == ZMS_TRACK_VIDEO &&
                              zms_rtp_muxer_awaiting_video_key(p->rtsp_mux);

            muxed = zms_rtp_muxer_input_slot(p->rtsp_mux, &slot);
            if (muxed > 0) {
                zms_vod_buffer_reader_advance(vod_rd);
                ++frames;
                if (catchup) {
                    zms_rtp_muxer_catchup_frame(p->rtsp_mux);
                    catchup = zms_rtp_muxer_catchup_on(p->rtsp_mux);
                    if (!catchup) {
                        /* VOD RTP ts 跟文件 dts_ms；catchup 后按 seek_ms / RTCP SR 重基 epoch。 */
                        zms_egress_clock *clk_mut = zms_rtp_muxer_play_clock_mut(p->rtsp_mux);
                        if (clk_mut) {
                            zms_egress_clock_sync_wall(clk_mut, slot.dts_ms);
                        }
                    }
                }
                if (log_first_v) {
                    ztk_debug("[egress] vod_first_video session=%u es=%u ts=%u key=%d", session_no,
                              (unsigned)slot.len, (unsigned)slot.dts_ms, slot.keyframe);
                }
            } else if (muxed == 0) {
                if (session_no && slot.track == ZMS_TRACK_VIDEO && slot.keyframe) {
                    ztk_debug("[egress] vod_mux_skip session=%u es=%u ts=%u key=%d", session_no,
                              (unsigned)slot.len, (unsigned)slot.dts_ms, slot.keyframe);
                }
                zms_vod_buffer_reader_advance(vod_rd);
            } else {
                if (session_no && slot.track == ZMS_TRACK_VIDEO) {
                    ztk_warn("[egress] vod_mux_fail session=%u codec=%d es=%u ts=%u key=%d",
                             session_no, (int)slot.codec, (unsigned)slot.len, (unsigned)slot.dts_ms,
                             slot.keyframe);
                }
                break;
            }
        }
    }
    return frames;
}

int zms_egress_pipeline_pump_live(zms_egress_pipeline *p, int frame_budget, int read_timeout_ms,
                                  unsigned session_no, const zms_egress_live_state *live)
{
    zms_gop_reader *rd;
    zms_gop_slot slot;
    zms_gop_slot peek;
    const zms_rtp_muxer_stats *st0;
    uint32_t v_pkts0;
    size_t lag;
    int frames = 0;
    int pump_budget;

    if (!p || !p->reader || !p->rtsp_mux || p->wire != ZMS_WIRE_FORMAT_RTP) {
        return 0;
    }

    rd = p->reader->readers.gop;
    if (!rd) {
        return 0;
    }

    {
        int woke;
        const zms_egress_clock *clk0;
        int epoch0;
        int catchup0;

        lag = zms_gop_reader_lag(rd);
        woke = zms_gop_reader_drain_wake(rd);
        clk0 = zms_rtp_muxer_play_clock(p->rtsp_mux);
        epoch0 = clk0 && zms_egress_clock_epoch_locked(clk0);
        catchup0 = live && live->live_catchup_done && *live->live_catchup_done;
        if (!zms_egress_live_should_pump(rd, woke, catchup0, epoch0)) {
            return frames;
        }
        if (frame_budget <= 0) {
            frame_budget = zms_egress_live_pump_budget(epoch0, catchup0, lag, 0);
        }
    }
    if (read_timeout_ms <= 0) {
        read_timeout_ms = 500;
    }

    pump_budget = frame_budget;
    st0 = zms_rtp_muxer_get_stats(p->rtsp_mux);
    v_pkts0 = st0 ? st0->video_pkt_count : 0;

    if (zms_egress_live_rtsp_prep(rd, p->rtsp_mux, live, session_no, &pump_budget)) {
        return frames;
    }

    while (pump_budget-- > 0) {
        const zms_egress_clock *clk;
        int lag_catchup;
        int pace;
        int pk;

        lag = zms_gop_reader_lag(rd);
        lag_catchup = zms_egress_live_lag_catchup(lag);
        pace = !lag_catchup;
        clk = zms_rtp_muxer_play_clock(p->rtsp_mux);

        pk = zms_egress_live_peek_paced(rd, &peek, clk, pace, read_timeout_ms);
        if (pk <= 0) {
            break;
        }

        if (zms_gop_reader_read_muxed(rd, &slot, read_timeout_ms) <= 0) {
            break;
        }
        if (slot.len == 0 || !slot.data) {
            continue;
        }

        if (slot.track == ZMS_TRACK_VIDEO) {
            zms_gop_slot_refresh_play_key(&slot);
        }

        lag = zms_gop_reader_lag(rd);
        lag_catchup = zms_egress_live_lag_catchup(lag);

        if (zms_rtp_muxer_input_slot(p->rtsp_mux, &slot) > 0) {
            ++frames;
            if (lag_catchup && zms_gop_reader_lag(rd) <= ZMS_EGRESS_CATCHUP_LAG) {
                zms_egress_clock *clk_mut = zms_rtp_muxer_play_clock_mut(p->rtsp_mux);
                if (clk_mut) {
                    zms_egress_clock_sync_wall(clk_mut, slot.dts_ms);
                }
            }
            if (session_no && v_pkts0 + (uint32_t)frames <= 3 && slot.track == ZMS_TRACK_VIDEO) {
                const char *kind = "VCL";
                if (slot.codec == ZMS_CODEC_H265) {
                    if (slot.data && slot.len >= 5) {
                        if (zms_h265_annexb_is_idr(slot.data, slot.len)) {
                            kind = "IDR";
                        } else if (zms_h265_annexb_is_sync_key(slot.data, slot.len)) {
                            kind = "CRA";
                        }
                    }
                } else if (slot.keyframe) {
                    kind = "key";
                }
                ztk_debug("[egress] play_attach session=%u codec=%d kind=%s es=%u ts=%u key=%d",
                          session_no, (int)slot.codec, kind, (unsigned)slot.len,
                          (unsigned)slot.dts_ms, slot.keyframe);
            }
        }
    }

    if (frames > 0 && session_no) {
        const zms_rtp_muxer_stats *st = zms_rtp_muxer_get_stats(p->rtsp_mux);
        size_t pending = zms_gop_reader_lag(rd);
        if (st &&
            (pending > ZMS_EGRESS_RING_MAX_LAG || frames >= (int)ZMS_EGRESS_FRAME_BUDGET_LIVE)) {
            ztk_debug("[egress] pump session=%u frames=%d lag=%zu v_pkts=%u a_pkts=%u", session_no,
                      frames, pending, st->video_pkt_count, st->audio_pkt_count);
        }
    }

    return frames;
}

int zms_egress_pipeline_pump_rtsp(zms_egress_pipeline *p, zms_vod_buffer_reader *vod_rd,
                                  zms_vod_reader *vod_demux, int frame_budget, int read_timeout_ms,
                                  unsigned session_no, const zms_egress_live_state *live)
{
    if (!p) {
        return 0;
    }
    if (vod_rd) {
        return zms_egress_pipeline_pump_vod(p, vod_rd, vod_demux, frame_budget, session_no);
    }
    return zms_egress_pipeline_pump_live(p, frame_budget, read_timeout_ms, session_no, live);
}

ztk_err_t zms_egress_pipeline_pump(zms_egress_pipeline *p, int timeout_ms)
{
    if (!p) {
        return ZTK_ERR_INVALID;
    }
    if (p->wire == ZMS_WIRE_FORMAT_RTMP) {
        (void)zms_egress_pipeline_pump_flv_live(p, ZMS_EGRESS_FLV_BUDGET_LIVE, timeout_ms, 0, NULL);
    } else {
        (void)zms_egress_pipeline_pump_live(p, (int)ZMS_EGRESS_FRAME_BUDGET_LIVE, timeout_ms, 0,
                                            NULL);
    }
    return ZTK_OK;
}
