#include "zms/vod/play/vod_play_lane.h"
#include "zms/vod/io/vod_source.h"
#include "zms/engine/stream/stream_hub.h"
#include "ztk/util/log.h"
#include <stdlib.h>
#include <string.h>

#define VOD_LANE_BUFFER_CAP 256u
#define VOD_LANE_FIFO_HW 128u

struct zms_vod_play_lane {
    zms_vod_reader *reader;
    zms_vod_buffer *fifo;
    zms_vod_buffer_reader *fifo_rd;
    zms_vod_reader *pub_hold;
    zms_media_source *source;
};

static void vod_lane_pub_hold(zms_media_source *src, zms_vod_reader *pub)
{
    if (!src || !pub) {
        return;
    }
    zms_media_source_registry_lock();
    if (++src->vod_play_lane_ref == 1) {
        zms_vod_reader_set_pump_hold(pub, 1);
    }
    zms_media_source_registry_unlock();
}

static void vod_lane_pub_release(zms_media_source *src, zms_vod_reader *pub)
{
    if (!src || !pub) {
        return;
    }
    zms_media_source_registry_lock();
    if (src->vod_play_lane_ref > 0 && --src->vod_play_lane_ref == 0) {
        zms_vod_reader_set_pump_hold(pub, 0);
    }
    zms_media_source_registry_unlock();
}

static void vod_lane_align_reader(zms_vod_play_lane *lane)
{
    if (!lane || !lane->fifo_rd) {
        return;
    }
    zms_vod_buffer_reader_seek_beginning(lane->fifo_rd);
    zms_vod_buffer_reader_seek_video_key(lane->fifo_rd);
}

zms_vod_play_lane *zms_vod_play_lane_open(const zms_media_source *src, ztk_poller *poller)
{
    zms_vod_reader_opts opts;
    zms_vod_play_lane *lane;
    char path[512];

    if (!src) {
        return NULL;
    }
    /* loadMP4File 等显式路径优先；经典 www/{app}/{stream} 布局作回退 */
    if (!zms_vod_source_file_path(src, path, sizeof(path)) &&
        !zms_vod_resolve_file_path(src->app, src->stream, path, sizeof(path))) {
        ztk_warn("vod lane: file not found %s/%s", src->app, src->stream);
        return NULL;
    }

    lane = (zms_vod_play_lane *)calloc(1, sizeof(*lane));
    if (!lane) {
        return NULL;
    }

    lane->fifo = zms_vod_buffer_create(VOD_LANE_BUFFER_CAP);
    if (!lane->fifo) {
        goto fail;
    }

    memset(&opts, 0, sizeof(opts));
    opts.file_path = path;
    opts.app = src->app;
    opts.stream = src->stream;
    opts.fifo = lane->fifo;
    opts.source = (zms_media_source *)src;
    opts.speed = 1.0;
    opts.fifo_high_water = VOD_LANE_FIFO_HW;

    lane->reader = zms_vod_reader_open(&opts);
    if (!lane->reader) {
        goto fail;
    }

    if (poller) {
        zms_vod_reader_bind_poller_lite(lane->reader, poller);
    }
    lane->fifo_rd = zms_vod_buffer_reader_attach(lane->fifo, 1);
    if (!lane->fifo_rd) {
        goto fail;
    }

    if (src->publisher_ctx && src->publisher_ctx != lane->reader) {
        lane->pub_hold = (zms_vod_reader *)src->publisher_ctx;
        lane->source = (zms_media_source *)src;
        vod_lane_pub_hold(lane->source, lane->pub_hold);
    }

    ztk_info("vod lane: open %s -> %s/%s", path, src->app, src->stream);
    return lane;

fail:
    zms_vod_play_lane_close(lane);
    return NULL;
}

void zms_vod_play_lane_close(zms_vod_play_lane *lane)
{
    if (!lane) {
        return;
    }
    if (lane->pub_hold) {
        vod_lane_pub_release(lane->source, lane->pub_hold);
        lane->pub_hold = NULL;
        lane->source = NULL;
    }
    zms_vod_buffer_reader_detach(lane->fifo_rd);
    lane->fifo_rd = NULL;
    zms_vod_reader_close(lane->reader);
    lane->reader = NULL;
    zms_vod_buffer_destroy(lane->fifo);
    lane->fifo = NULL;
    free(lane);
}

zms_vod_buffer_reader *zms_vod_play_lane_buffer_reader(zms_vod_play_lane *lane)
{
    return lane ? lane->fifo_rd : NULL;
}

zms_vod_reader *zms_vod_play_lane_reader(zms_vod_play_lane *lane)
{
    return lane ? lane->reader : NULL;
}

uint64_t zms_vod_play_lane_seek_ms(zms_vod_play_lane *lane, uint64_t ms)
{
    if (!lane || !lane->reader) {
        return 0;
    }
    ms = zms_vod_reader_seek_ms(lane->reader, ms);
    vod_lane_align_reader(lane);
    return ms;
}

uint64_t zms_vod_play_lane_prepare(zms_vod_play_lane *lane, uint64_t start_ms)
{
    return zms_vod_play_lane_seek_ms(lane, start_ms);
}

void zms_vod_play_lane_prefill(zms_vod_play_lane *lane)
{
    if (lane && lane->reader) {
        zms_vod_reader_prefill(lane->reader);
    }
}

void zms_vod_play_lane_align_reader(zms_vod_play_lane *lane)
{
    vod_lane_align_reader(lane);
}

void zms_vod_play_lane_set_pump_hold(zms_vod_play_lane *lane, int hold)
{
    if (lane && lane->reader) {
        zms_vod_reader_set_pump_hold(lane->reader, hold);
    }
}

void zms_vod_play_lane_demux_fill(zms_vod_play_lane *lane, int max_pumps)
{
    int i;
    size_t target_lag = 48;

    if (!lane || !lane->reader || !lane->fifo_rd || max_pumps <= 0) {
        return;
    }
    for (i = 0; i < max_pumps; ++i) {
        if (zms_vod_buffer_reader_lag(lane->fifo_rd) >= target_lag) {
            break;
        }
        if (zms_vod_reader_pump(lane->reader) <= 0) {
            break;
        }
    }
}

const uint8_t *zms_vod_play_lane_video_config(const zms_vod_play_lane *lane, size_t *len)
{
    return lane && lane->fifo ? zms_vod_buffer_video_config(lane->fifo, len) : NULL;
}

const uint8_t *zms_vod_play_lane_audio_config(const zms_vod_play_lane *lane, size_t *len)
{
    return lane && lane->fifo ? zms_vod_buffer_audio_config(lane->fifo, len) : NULL;
}
