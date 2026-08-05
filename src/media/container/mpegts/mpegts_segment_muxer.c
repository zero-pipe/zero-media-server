/**
 * @file mpegts_segment_muxer.c
 * @brief MPEG-TS 分段 muxer 后端，实现 zms_container_muxer_ops vtable。
 *
 * 包装 zero-media-kit libhls 的 hls_media_t，将 ES 帧写入 libhls，
 * 由 libhls 按 segment_duration_ms 自动切段并通过 on_segment 回调吐出 TS 分段。
 * 供 HLS 分段录制路径使用（http_hls_segmenter → vod_hls）。
 *
 * 对应的连续流版本见 mpegts_continuous_muxer.c（基于 libmpeg，用于 SRT 输出）。
 *
 * 注册名：ZMS_CONTAINER_MPEGTS / "mpegts"
 * 全局实例：zms_container_mpegts_muxer_ops
 */
#include "zms/media/container/container_dispatcher.h"
#include "hls-media.h"
#include <stdlib.h>

typedef struct {
    zms_container_mux_opts cfg;
    hls_media_t *hls;
} mpegts_mux_ctx;

static int mpegts_segment_bridge(void *param, const void *data, size_t bytes, int64_t pts_ms,
                                 int64_t dts_ms, int64_t duration)
{
    mpegts_mux_ctx *c = (mpegts_mux_ctx *)param;

    if (!c || !c->cfg.on_segment) {
        return 0;
    }
    return c->cfg.on_segment(c->cfg.user, data, bytes, pts_ms, dts_ms, duration);
}

static void *mpegts_mux_create(const zms_container_mux_opts *opts)
{
    mpegts_mux_ctx *c;

    if (!opts || !opts->on_segment) {
        return NULL;
    }
    c = (mpegts_mux_ctx *)calloc(1, sizeof(*c));
    if (!c) {
        return NULL;
    }
    c->cfg = *opts;
    c->hls = hls_media_create(opts->segment_duration_ms, mpegts_segment_bridge, c);
    if (!c->hls) {
        free(c);
        return NULL;
    }
    return c;
}

static void mpegts_mux_destroy(void *ctx)
{
    mpegts_mux_ctx *c = (mpegts_mux_ctx *)ctx;

    if (!c) {
        return;
    }
    if (c->hls) {
        hls_media_destroy(c->hls);
    }
    free(c);
}

static void mpegts_mux_set_extradata(void *ctx, int stream_type, const void *extra, size_t len)
{
    mpegts_mux_ctx *c = (mpegts_mux_ctx *)ctx;

    if (!c || !c->hls || !extra || len == 0) {
        return;
    }
    (void)hls_media_add_stream(c->hls, stream_type, extra, len);
}

static ztk_err_t mpegts_mux_write_frame(void *ctx, int stream_type, const void *data, size_t len,
                                        int64_t pts_ms, int64_t dts_ms, int flags)
{
    mpegts_mux_ctx *c = (mpegts_mux_ctx *)ctx;
    int hls_flags = 0;

    if (!c || !c->hls) {
        return ZTK_ERR_INVALID;
    }
    if (flags & ZMS_CONTAINER_MUX_FLAG_KEYFRAME) {
        hls_flags |= HLS_FLAGS_KEYFRAME;
    }
    if (hls_media_input(c->hls, stream_type, data, len, pts_ms, dts_ms, hls_flags) != 0) {
        return ZTK_ERR_INVALID;
    }
    return ZTK_OK;
}

static void mpegts_mux_flush(void *ctx, int stream_type, int64_t pts_ms)
{
    mpegts_mux_ctx *c = (mpegts_mux_ctx *)ctx;

    if (!c || !c->hls) {
        return;
    }
    (void)hls_media_input(c->hls, stream_type, NULL, 0, pts_ms, pts_ms, 0);
}

const zms_container_muxer_ops zms_container_mpegts_muxer_ops = {
    ZMS_CONTAINER_MPEGTS,     "mpegts",
    mpegts_mux_create,        mpegts_mux_destroy,
    mpegts_mux_set_extradata, mpegts_mux_write_frame,
    mpegts_mux_flush,
};
