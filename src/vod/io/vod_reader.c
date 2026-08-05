#include "zms/vod/io/vod_reader.h"
#include "zms/vod/io/mp4_vod_reader.h"
#include "zms/vod/vod_flv_index.h"
#include <stdlib.h>
#include <string.h>

struct zms_vod_reader {
    zms_mp4_reader *impl;
};

zms_vod_reader *zms_vod_reader_open(const zms_vod_reader_opts *opts)
{
    zms_mp4_reader_opts mp4_opts;
    zms_vod_reader *r;

    if (!opts) {
        return NULL;
    }
    r = (zms_vod_reader *)calloc(1, sizeof(*r));
    if (!r) {
        return NULL;
    }
    memset(&mp4_opts, 0, sizeof(mp4_opts));
    mp4_opts.file_path = opts->file_path;
    mp4_opts.app = opts->app;
    mp4_opts.stream = opts->stream;
    mp4_opts.fifo = opts->fifo;
    mp4_opts.source = opts->source;
    mp4_opts.speed = opts->speed;
    mp4_opts.loop = opts->loop;
    mp4_opts.fifo_high_water = opts->fifo_high_water;
    mp4_opts.owner_ctx = r;
    r->impl = zms_mp4_reader_open(&mp4_opts);
    if (!r->impl) {
        free(r);
        return NULL;
    }
    return r;
}

void zms_vod_reader_close(zms_vod_reader *r)
{
    if (!r) {
        return;
    }
    zms_mp4_reader_close(r->impl);
    free(r);
}

void zms_vod_reader_bind_poller(zms_vod_reader *r, ztk_poller *poller)
{
    if (r) {
        zms_mp4_reader_bind_poller(r->impl, poller);
    }
}

void zms_vod_reader_bind_poller_lite(zms_vod_reader *r, ztk_poller *poller)
{
    if (r) {
        zms_mp4_reader_bind_poller_lite(r->impl, poller);
    }
}

void zms_vod_reader_prefill(zms_vod_reader *r)
{
    if (r) {
        zms_mp4_reader_prefill(r->impl);
    }
}

void zms_vod_reader_prepare_play(zms_vod_reader *r)
{
    if (r) {
        zms_mp4_reader_prepare_play(r->impl);
    }
}

uint64_t zms_vod_reader_seek_ms(zms_vod_reader *r, uint64_t ms)
{
    return r ? zms_mp4_reader_seek_ms(r->impl, ms) : 0;
}

zms_media_source *zms_vod_reader_source(zms_vod_reader *r)
{
    return r ? zms_mp4_reader_source(r->impl) : NULL;
}

const char *zms_vod_reader_file_path(const zms_vod_reader *r)
{
    return r ? zms_mp4_reader_file_path(r->impl) : NULL;
}

uint64_t zms_vod_reader_duration_ms(const zms_vod_reader *r)
{
    return r ? zms_mp4_reader_duration_ms(r->impl) : 0;
}

const zms_vod_flv_index *zms_vod_reader_flv_index(zms_vod_reader *r)
{
    return r ? zms_mp4_reader_flv_index(r->impl) : NULL;
}

void zms_vod_reader_set_pump_hold(zms_vod_reader *r, int hold)
{
    if (r) {
        zms_mp4_reader_set_pump_hold(r->impl, hold);
    }
}

int zms_vod_reader_pump(zms_vod_reader *r)
{
    return r ? zms_mp4_reader_pump(r->impl) : 0;
}
