#include "zms/live/play/hls/http_hls_sidecar.h"
#include "zms/live/play/dash/http_dash_sidecar.h"
#include "zms/live/play/hls/http_hls_segmenter.h"
#include "zms/egress/egress_segment_recorder.h"
#include "zms/engine/stream/stream_hub.h"
#include "zms/vod/vod_codec_ext.h"
#include "zms/vod/io/vod_source.h"
#include "ztk/util/log.h"
#include <string.h>

static zms_http_hls_opts g_hls;
static ztk_poller *g_poller;
static zms_media_events g_user_events;

void zms_http_hls_default_opts(zms_http_hls_opts *opts)
{
    if (!opts) {
        return;
    }
    opts->enable = 0;
    opts->segment_duration_sec = 2.f;
    opts->segment_count = 3;
    opts->enable_audio = 1;
}

static void hls_start_segmenter(const zms_media_tuple *tuple)
{
    zms_http_hls_segmenter_opts ropts;
    zms_media_source *src;

    if (!g_hls.enable || !tuple) {
        return;
    }
    src = zms_media_source_find(tuple->schema, tuple->app, tuple->stream);
    if (!src || zms_media_source_segment_rec_get(src, ZMS_SEGMENT_REC_HLS)) {
        return;
    }
    if (zms_media_source_is_vod(src)) {
        return;
    }

    ropts.segment_duration_sec = g_hls.segment_duration_sec;
    ropts.segment_count = g_hls.segment_count;
    ropts.enable_audio = g_hls.enable_audio;
    (void)zms_segment_recorder_create_live(src, ZMS_SEGMENT_REC_HLS, &ropts);
    if (g_poller) {
        zms_segment_recorder_bind_timer(src, ZMS_SEGMENT_REC_HLS, g_poller);
    }
}

void zms_http_hls_bind_recorder_timer(zms_media_source *src, ztk_poller *poller)
{
    (void)poller;
    if (!src) {
        return;
    }
    zms_segment_recorder_bind_timer(src, ZMS_SEGMENT_REC_HLS, g_poller);
}

ztk_poller *zms_http_hls_main_poller(void)
{
    return g_poller;
}

void zms_http_hls_ensure_recorder(zms_media_source *src, ztk_poller *poller)
{
    zms_http_hls_segmenter_opts ropts;
    ztk_err_t err;

    (void)poller;
    if (!src || zms_media_source_is_vod(src)) {
        return;
    }

    ropts.segment_duration_sec = g_hls.segment_duration_sec;
    ropts.segment_count = g_hls.segment_count;
    ropts.enable_audio = g_hls.enable_audio;
    err = zms_segment_recorder_ensure_live(src, ZMS_SEGMENT_REC_HLS, g_poller, &ropts);
    if (err != ZTK_OK) {
        ztk_warn("HLS ensure recorder failed: %s/%s err=%d", src->app, src->stream, (int)err);
    }
}

static void hls_stop_segmenter(const zms_media_tuple *tuple)
{
    zms_media_source *src;

    if (!tuple) {
        return;
    }
    src = zms_media_source_find(tuple->schema, tuple->app, tuple->stream);
    if (!src) {
        return;
    }
    zms_segment_recorder_destroy_live(src, ZMS_SEGMENT_REC_HLS);
}

static void dash_start_segmenter(const zms_media_tuple *tuple)
{
    zms_media_source *src;

    if (!tuple) {
        return;
    }
    src = zms_media_source_find(tuple->schema, tuple->app, tuple->stream);
    if (!src || zms_media_source_is_vod(src) || !src->gop_queue) {
        return;
    }
    zms_http_dash_ensure_recorder(src, g_poller);
}

static void on_publish_chain(const zms_media_tuple *tuple, zms_media_origin origin, void *user)
{
    (void)user;
    hls_start_segmenter(tuple);
    dash_start_segmenter(tuple);
    if (g_user_events.on_media_publish) {
        g_user_events.on_media_publish(tuple, origin, g_user_events.user);
    }
}

static void dash_stop_segmenter(const zms_media_tuple *tuple)
{
    zms_media_source *src;

    if (!tuple) {
        return;
    }
    src = zms_media_source_find(tuple->schema, tuple->app, tuple->stream);
    if (!src) {
        return;
    }
    zms_segment_recorder_destroy_live(src, ZMS_SEGMENT_REC_DASH);
}

static void on_publish_fini_chain(const zms_media_tuple *tuple, zms_media_origin origin, void *user)
{
    (void)user;
    hls_stop_segmenter(tuple);
    dash_stop_segmenter(tuple);
    if (g_user_events.on_media_publish_fini) {
        g_user_events.on_media_publish_fini(tuple, origin, g_user_events.user);
    }
}

void zms_http_hls_init(ztk_poller *poller, const zms_http_hls_opts *hls,
                       const zms_media_events *events, int none_reader_delay_ms)
{
    if (hls) {
        g_hls = *hls;
    } else {
        zms_http_hls_default_opts(&g_hls);
    }
    g_poller = poller;
    g_user_events = events ? *events : (zms_media_events){0};

    zms_segment_recorder_register_builtins();
    zms_vod_codec_ext_register_all();

    zms_media_events wrap = g_user_events;
    if (g_hls.enable) {
        wrap.on_media_publish = on_publish_chain;
        wrap.on_media_publish_fini = on_publish_fini_chain;
        ztk_info("HLS enabled: seg=%.1fs count=%u audio=%d", g_hls.segment_duration_sec,
                 g_hls.segment_count, g_hls.enable_audio);
    }
    zms_media_events_set(poller, &wrap, none_reader_delay_ms);
}

void zms_http_hls_fini(void)
{
    g_poller = NULL;
    zms_http_hls_default_opts(&g_hls);
    memset(&g_user_events, 0, sizeof(g_user_events));
}
