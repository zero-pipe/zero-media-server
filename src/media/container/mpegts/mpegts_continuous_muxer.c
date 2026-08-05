/**
 * @file mpegts_continuous_muxer.c
 * @brief 连续 MPEG-TS mux 后端（zms_container_muxer_ops，基于 zmk libmpeg / mpeg_ts_*）。
 *        与分段 HLS 后端（mpegts_segment_muxer.c / libhls）不同，本实现输出一路连续 TS，
 *        供 SRT 出站使用；通过容器 mux 抽象与 HLS 共用同一套 codec-feed。
 *
 * Copyright (c) zero-media-server
 */
#include "zms/media/container/mpegts/mpegts_mux_feed.h"
#include "zms/engine/media_clock.h"
#include "mpeg-ts.h"
#include <stdlib.h>
#include <string.h>

#define TS_CONT_MAX_STREAMS 8

typedef struct {
    int stream_type;
    int stream_id;
} ts_cont_stream;

typedef struct {
    zms_container_mux_opts cfg;
    void *ts;
    ts_cont_stream streams[TS_CONT_MAX_STREAMS];
    int nstreams;
} ts_cont_ctx;

static void *ts_cont_pkt_alloc(void *param, size_t bytes)
{
    (void)param;
    return malloc(bytes);
}

static void ts_cont_pkt_free(void *param, void *packet)
{
    (void)param;
    free(packet);
}

static int ts_cont_on_write(void *param, const void *packet, size_t bytes)
{
    ts_cont_ctx *c = (ts_cont_ctx *)param;

    if (!c || !c->cfg.on_segment || !packet || bytes == 0) {
        return 0;
    }
    return c->cfg.on_segment(c->cfg.user, packet, bytes, 0, 0, 0);
}

static void *ts_cont_create(const zms_container_mux_opts *opts)
{
    ts_cont_ctx *c;
    struct mpeg_ts_func_t func;

    if (!opts || !opts->on_segment) {
        return NULL;
    }
    c = (ts_cont_ctx *)calloc(1, sizeof(*c));
    if (!c) {
        return NULL;
    }
    c->cfg = *opts;
    memset(&func, 0, sizeof(func));
    func.alloc = ts_cont_pkt_alloc;
    func.free = ts_cont_pkt_free;
    func.write = ts_cont_on_write;
    c->ts = mpeg_ts_create(&func, c);
    if (!c->ts) {
        free(c);
        return NULL;
    }
    return c;
}

static void ts_cont_destroy(void *ctx)
{
    ts_cont_ctx *c = (ts_cont_ctx *)ctx;

    if (!c) {
        return;
    }
    if (c->ts) {
        mpeg_ts_destroy(c->ts);
    }
    free(c);
}

static int ts_cont_find_stream(const ts_cont_ctx *c, int stream_type)
{
    for (int i = 0; i < c->nstreams; ++i) {
        if (c->streams[i].stream_type == stream_type) {
            return c->streams[i].stream_id;
        }
    }
    return -1;
}

static void ts_cont_set_extradata(void *ctx, int stream_type, const void *extra, size_t len)
{
    ts_cont_ctx *c = (ts_cont_ctx *)ctx;
    int sid;

    if (!c || !c->ts || !extra || len == 0) {
        return;
    }
    if (ts_cont_find_stream(c, stream_type) > 0) {
        return;
    }
    if (c->nstreams >= TS_CONT_MAX_STREAMS) {
        return;
    }
    sid = mpeg_ts_add_stream(c->ts, stream_type, extra, len);
    if (sid > 0) {
        c->streams[c->nstreams].stream_type = stream_type;
        c->streams[c->nstreams].stream_id = sid;
        c->nstreams++;
    }
}

static ztk_err_t ts_cont_input(void *ctx, int stream_type, const void *data, size_t len,
                               int64_t pts_ms, int64_t dts_ms, int flags)
{
    ts_cont_ctx *c = (ts_cont_ctx *)ctx;
    int sid;
    int key;

    if (!c || !c->ts || !data || len == 0) {
        return ZTK_ERR_INVALID;
    }
    sid = ts_cont_find_stream(c, stream_type);
    if (sid <= 0) {
        return ZTK_ERR_INVALID;
    }
    key = (flags & ZMS_CONTAINER_MUX_FLAG_KEYFRAME) ? 1 : 0;
    {
        int64_t pts_90k = (int64_t)zms_ms_to_mpegts_90k((uint32_t)pts_ms);
        int64_t dts_90k = (int64_t)zms_ms_to_mpegts_90k((uint32_t)dts_ms);

        if (mpeg_ts_write(c->ts, sid, key, pts_90k, dts_90k, data, len) != 0) {
            return ZTK_ERR_INVALID;
        }
    }
    return ZTK_OK;
}

static void ts_cont_flush(void *ctx, int stream_type, int64_t pts_ms)
{
    (void)ctx;
    (void)stream_type;
    (void)pts_ms;
}

const zms_container_muxer_ops zms_container_mpegts_continuous_muxer_ops = {
    ZMS_CONTAINER_MPEGTS,  "mpegts-continuous", ts_cont_create, ts_cont_destroy,
    ts_cont_set_extradata, ts_cont_input,       ts_cont_flush,
};
