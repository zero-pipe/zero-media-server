/**
 * @file vod_buffer.c
 * @brief 点播播放缓冲与每会话读取者。
 *
 * Copyright (c) zero-media-server
 */
#include "zms/vod/io/vod_buffer.h"
#include "zms/media/codec/h264/h264_es.h"
#include "zms/media/codec/h265/h265_es.h"
#include "zms/engine/frame.h"
#include "zms/util/buf_pool.h"
#include "ztk/poller/poller.h"
#include "ztk/thread/sync.h"
#include "ztk/util/log.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t *data;
    size_t cap;
    size_t len;
    zms_codec_id codec;
    zms_track_type track;
    uint32_t dts_ms;
    uint32_t pts_ms;
    int keyframe;
    int config_frame;
} vod_slot;

struct zms_vod_buffer {
    vod_slot *slots;
    size_t cap;
    uint64_t write_seq;
    uint64_t consumed_seq;
    uint8_t *video_config;
    size_t video_config_len;
    size_t video_config_cap;
    uint8_t *audio_config;
    size_t audio_config_len;
    size_t audio_config_cap;
    ztk_mutex *mtx;
};

struct zms_vod_buffer_reader {
    zms_vod_buffer *fifo;
    uint64_t read_seq;
};

static int set_blob(uint8_t **buf, size_t *len, size_t *cap, const void *data, size_t data_len)
{
    if (!buf || !len || !cap) {
        return -1;
    }
    if (data_len == 0) {
        *len = 0;
        return 0;
    }
    if (!zms_buf_pool_slot_resize(buf, cap, data_len)) {
        return -1;
    }
    memcpy(*buf, data, data_len);
    *len = data_len;
    return 0;
}

static void slot_clear(vod_slot *s)
{
    if (!s) {
        return;
    }
    zms_buf_pool_slot_clear(&s->data, &s->cap);
    s->len = 0;
    s->codec = ZMS_CODEC_INVALID;
    s->track = ZMS_TRACK_INVALID;
    s->dts_ms = 0;
    s->pts_ms = 0;
    s->keyframe = 0;
    s->config_frame = 0;
}

zms_vod_buffer *zms_vod_buffer_create(size_t cap)
{
    zms_vod_buffer *f;

    if (cap == 0) {
        cap = ZMS_VOD_BUFFER_DEFAULT_CAP;
    }
    f = (zms_vod_buffer *)calloc(1, sizeof(*f));
    if (!f) {
        return NULL;
    }
    f->slots = (vod_slot *)calloc(cap, sizeof(vod_slot));
    if (!f->slots) {
        free(f);
        return NULL;
    }
    f->cap = cap;
    f->mtx = ztk_mutex_create(ZTK_MUTEX_NORMAL);
    if (!f->mtx) {
        free(f->slots);
        free(f);
        return NULL;
    }
    return f;
}

void zms_vod_buffer_destroy(zms_vod_buffer *fifo)
{
    size_t i;

    if (!fifo) {
        return;
    }
    if (fifo->mtx) {
        ztk_mutex_lock(fifo->mtx);
    }
    for (i = 0; i < fifo->cap; ++i) {
        slot_clear(&fifo->slots[i]);
    }
    free(fifo->slots);
    free(fifo->video_config);
    free(fifo->audio_config);
    if (fifo->mtx) {
        ztk_mutex_unlock(fifo->mtx);
        ztk_mutex_destroy(fifo->mtx);
    }
    free(fifo);
}

static uint64_t min_read_seq_locked(const zms_vod_buffer *fifo)
{
    return fifo ? fifo->consumed_seq : 0;
}

ztk_err_t zms_vod_buffer_write(zms_vod_buffer *fifo, const zms_frame *frame)
{
    vod_slot *s;
    uint64_t seq;
    size_t oldest;

    if (!fifo || !frame || !frame->data || frame->size == 0) {
        return ZTK_ERR_INVALID;
    }

    ztk_mutex_lock(fifo->mtx);
    if (fifo->write_seq > fifo->consumed_seq &&
        (fifo->write_seq - fifo->consumed_seq) >= fifo->cap) {
        ztk_mutex_unlock(fifo->mtx);
        return ZTK_ERR_BUFFER_TOO_SMALL;
    }

    seq = fifo->write_seq++;
    s = &fifo->slots[seq % fifo->cap];
    slot_clear(s);
    if (frame->owned && frame->data && frame->capacity >= frame->size) {
        s->data = frame->data;
        s->cap = frame->capacity;
        s->len = frame->size;
    } else {
        if (!zms_buf_pool_slot_resize(&s->data, &s->cap, frame->size)) {
            fifo->write_seq--;
            ztk_mutex_unlock(fifo->mtx);
            return ZTK_ERR_NOMEM;
        }
        memcpy(s->data, frame->data, frame->size);
        s->len = frame->size;
    }
    s->codec = frame->codec;
    s->track = frame->track;
    zms_gop_queue_timeline_from_frame(frame, &s->dts_ms, &s->pts_ms);
    s->keyframe = frame->keyframe;
    s->config_frame = frame->config_frame;

    oldest = (fifo->write_seq > fifo->cap) ? (fifo->write_seq - fifo->cap) : 0;
    if (oldest > 0) {
        vod_slot *drop = &fifo->slots[oldest % fifo->cap];
        if (drop->data && oldest + fifo->cap <= fifo->write_seq) {
            slot_clear(drop);
        }
    }
    ztk_mutex_unlock(fifo->mtx);
    return ZTK_OK;
}

ztk_err_t zms_vod_buffer_set_video_config(zms_vod_buffer *fifo, const void *data, size_t len)
{
    if (!fifo) {
        return ZTK_ERR_INVALID;
    }
    ztk_mutex_lock(fifo->mtx);
    if (set_blob(&fifo->video_config, &fifo->video_config_len, &fifo->video_config_cap, data,
                 len) != 0) {
        ztk_mutex_unlock(fifo->mtx);
        return ZTK_ERR_NOMEM;
    }
    ztk_mutex_unlock(fifo->mtx);
    return ZTK_OK;
}

ztk_err_t zms_vod_buffer_set_audio_config(zms_vod_buffer *fifo, const void *data, size_t len)
{
    if (!fifo) {
        return ZTK_ERR_INVALID;
    }
    ztk_mutex_lock(fifo->mtx);
    if (set_blob(&fifo->audio_config, &fifo->audio_config_len, &fifo->audio_config_cap, data,
                 len) != 0) {
        ztk_mutex_unlock(fifo->mtx);
        return ZTK_ERR_NOMEM;
    }
    ztk_mutex_unlock(fifo->mtx);
    return ZTK_OK;
}

void zms_vod_buffer_reset(zms_vod_buffer *fifo)
{
    size_t i;

    if (!fifo) {
        return;
    }
    ztk_mutex_lock(fifo->mtx);
    for (i = 0; i < fifo->cap; ++i) {
        slot_clear(&fifo->slots[i]);
    }
    fifo->write_seq = 0;
    fifo->consumed_seq = 0;
    ztk_mutex_unlock(fifo->mtx);
}

void zms_vod_buffer_reader_seek_beginning(zms_vod_buffer_reader *rd)
{
    if (!rd || !rd->fifo) {
        return;
    }
    ztk_mutex_lock(rd->fifo->mtx);
    rd->read_seq =
        (rd->fifo->write_seq > rd->fifo->cap) ? (rd->fifo->write_seq - rd->fifo->cap) : 0;
    ztk_mutex_unlock(rd->fifo->mtx);
}

const uint8_t *zms_vod_buffer_video_config(const zms_vod_buffer *fifo, size_t *len)
{
    if (!fifo) {
        return NULL;
    }
    if (len) {
        *len = fifo->video_config_len;
    }
    return fifo->video_config_len > 0 ? fifo->video_config : NULL;
}

const uint8_t *zms_vod_buffer_audio_config(const zms_vod_buffer *fifo, size_t *len)
{
    if (!fifo) {
        return NULL;
    }
    if (len) {
        *len = fifo->audio_config_len;
    }
    return fifo->audio_config_len > 0 ? fifo->audio_config : NULL;
}

size_t zms_vod_buffer_pending(const zms_vod_buffer *fifo)
{
    size_t n;

    if (!fifo) {
        return 0;
    }
    ztk_mutex_lock(fifo->mtx);
    n = (fifo->write_seq > fifo->consumed_seq) ? (size_t)(fifo->write_seq - fifo->consumed_seq) : 0;
    ztk_mutex_unlock(fifo->mtx);
    return n;
}

int zms_vod_buffer_has_h264_idr(const zms_vod_buffer *fifo)
{
    return zms_vod_buffer_has_video_sync_key(fifo);
}

int zms_vod_buffer_has_video_sync_key(const zms_vod_buffer *fifo)
{
    uint64_t seq;
    uint64_t oldest;

    if (!fifo) {
        return 0;
    }
    ztk_mutex_lock(fifo->mtx);
    if (fifo->write_seq == 0) {
        ztk_mutex_unlock(fifo->mtx);
        return 0;
    }
    oldest = (fifo->write_seq > fifo->cap) ? (fifo->write_seq - fifo->cap) : 0;
    for (seq = oldest; seq < fifo->write_seq; ++seq) {
        const vod_slot *s = &fifo->slots[seq % fifo->cap];
        if (s->track != ZMS_TRACK_VIDEO || !s->data || s->len == 0) {
            continue;
        }
        if (s->codec == ZMS_CODEC_H264 && (zms_h264_annexb_is_idr(s->data, s->len) ||
                                           zms_h264_annexb_is_sync_key(s->data, s->len))) {
            ztk_mutex_unlock(fifo->mtx);
            return 1;
        }
        if (s->codec == ZMS_CODEC_H265 && (zms_h265_annexb_is_idr(s->data, s->len) ||
                                           zms_h265_annexb_is_sync_key(s->data, s->len))) {
            ztk_mutex_unlock(fifo->mtx);
            return 1;
        }
    }
    ztk_mutex_unlock(fifo->mtx);
    return 0;
}

zms_vod_buffer_reader *zms_vod_buffer_reader_attach(zms_vod_buffer *fifo, int from_beginning)
{
    zms_vod_buffer_reader *rd;

    if (!fifo) {
        return NULL;
    }
    rd = (zms_vod_buffer_reader *)calloc(1, sizeof(*rd));
    if (!rd) {
        return NULL;
    }
    rd->fifo = fifo;
    ztk_mutex_lock(fifo->mtx);
    if (from_beginning) {
        rd->read_seq = (fifo->write_seq > fifo->cap) ? (fifo->write_seq - fifo->cap) : 0;
    } else {
        rd->read_seq = fifo->write_seq;
    }
    ztk_mutex_unlock(fifo->mtx);
    return rd;
}

void zms_vod_buffer_reader_detach(zms_vod_buffer_reader *rd)
{
    free(rd);
}

static void vod_slot_to_ring_slot(const vod_slot *s, zms_gop_slot *slot)
{
    if (!slot) {
        return;
    }
    memset(slot, 0, sizeof(*slot));
    if (!s) {
        return;
    }
    slot->codec = s->codec;
    slot->track = s->track;
    slot->dts_ms = s->dts_ms;
    slot->pts_ms = s->pts_ms;
    slot->keyframe = s->keyframe;
    slot->config_frame = s->config_frame;
    slot->data = s->data;
    slot->len = s->len;
}

static int vod_buffer_reader_slot_at(zms_vod_buffer_reader *rd, zms_gop_slot *slot, int advance)
{
    const vod_slot *s;
    uint64_t seq;

    if (!rd || !rd->fifo || !slot) {
        return 0;
    }

    ztk_mutex_lock(rd->fifo->mtx);
    if (rd->read_seq >= rd->fifo->write_seq) {
        ztk_mutex_unlock(rd->fifo->mtx);
        return 0;
    }
    seq = rd->read_seq;
    if (advance) {
        rd->read_seq++;
        if (rd->read_seq > rd->fifo->consumed_seq) {
            rd->fifo->consumed_seq = rd->read_seq;
        }
    }
    s = &rd->fifo->slots[seq % rd->fifo->cap];
    if (!s->data || s->len == 0) {
        ztk_mutex_unlock(rd->fifo->mtx);
        return 0;
    }
    vod_slot_to_ring_slot(s, slot);
    ztk_mutex_unlock(rd->fifo->mtx);
    return 1;
}

int zms_vod_buffer_reader_peek_muxed_es(zms_vod_buffer_reader *rd, zms_gop_slot *slot,
                                        uint8_t **es_buf, size_t *es_cap, ztk_poller *pol)
{
    const vod_slot *s;
    uint64_t seq;

    if (!rd || !rd->fifo || !slot || !es_buf || !es_cap) {
        return 0;
    }

    ztk_mutex_lock(rd->fifo->mtx);
    if (rd->read_seq >= rd->fifo->write_seq) {
        ztk_mutex_unlock(rd->fifo->mtx);
        return 0;
    }
    seq = rd->read_seq;
    s = &rd->fifo->slots[seq % rd->fifo->cap];
    if (!s->data || s->len == 0) {
        ztk_mutex_unlock(rd->fifo->mtx);
        return 0;
    }
    if (pol) {
        if (!zms_buf_pool_slot_resize_poller(es_buf, es_cap, s->len, pol)) {
            ztk_mutex_unlock(rd->fifo->mtx);
            return 0;
        }
    } else if (!zms_buf_pool_slot_resize(es_buf, es_cap, s->len)) {
        ztk_mutex_unlock(rd->fifo->mtx);
        return 0;
    }
    memcpy(*es_buf, s->data, s->len);
    vod_slot_to_ring_slot(s, slot);
    slot->data = *es_buf;
    slot->len = s->len;
    ztk_mutex_unlock(rd->fifo->mtx);
    return 1;
}

int zms_vod_buffer_reader_read_muxed(zms_vod_buffer_reader *rd, zms_gop_slot *slot)
{
    return vod_buffer_reader_slot_at(rd, slot, 1);
}

int zms_vod_buffer_reader_peek_muxed(zms_vod_buffer_reader *rd, zms_gop_slot *slot)
{
    return vod_buffer_reader_slot_at(rd, slot, 0);
}

void zms_vod_buffer_reader_advance(zms_vod_buffer_reader *rd)
{
    if (!rd || !rd->fifo) {
        return;
    }
    ztk_mutex_lock(rd->fifo->mtx);
    if (rd->read_seq < rd->fifo->write_seq) {
        rd->read_seq++;
        if (rd->read_seq > rd->fifo->consumed_seq) {
            rd->fifo->consumed_seq = rd->read_seq;
        }
    }
    ztk_mutex_unlock(rd->fifo->mtx);
}

void zms_vod_buffer_reader_seek_video_key(zms_vod_buffer_reader *rd)
{
    uint64_t seq;

    if (!rd || !rd->fifo) {
        return;
    }
    ztk_mutex_lock(rd->fifo->mtx);
    for (seq = rd->read_seq; seq < rd->fifo->write_seq; ++seq) {
        const vod_slot *s = &rd->fifo->slots[seq % rd->fifo->cap];
        int sync = 0;

        if (s->track != ZMS_TRACK_VIDEO || !s->data || s->len == 0) {
            continue;
        }
        sync = s->keyframe;
        if (!sync && s->codec == ZMS_CODEC_H264) {
            sync = zms_h264_annexb_is_sync_key(s->data, s->len);
        } else if (!sync && s->codec == ZMS_CODEC_H265) {
            sync = zms_h265_annexb_is_sync_key(s->data, s->len);
        }
        if (!sync) {
            continue;
        }
        rd->read_seq = seq;
        ztk_mutex_unlock(rd->fifo->mtx);
        return;
    }
    ztk_mutex_unlock(rd->fifo->mtx);
}

int zms_vod_buffer_reader_seek_ms(zms_vod_buffer_reader *rd, uint64_t ms, uint64_t *out_ms)
{
    uint64_t seq, oldest;
    const vod_slot *s;

    if (out_ms) {
        *out_ms = ms;
    }
    if (!rd || !rd->fifo) {
        return 0;
    }
    ztk_mutex_lock(rd->fifo->mtx);
    if (rd->fifo->write_seq == 0) {
        ztk_mutex_unlock(rd->fifo->mtx);
        return 0;
    }
    oldest = (rd->fifo->write_seq > rd->fifo->cap) ? (rd->fifo->write_seq - rd->fifo->cap) : 0;
    for (seq = oldest; seq < rd->fifo->write_seq; ++seq) {
        s = &rd->fifo->slots[seq % rd->fifo->cap];
        if (!s->data || s->len == 0) {
            continue;
        }
        if ((uint64_t)s->dts_ms >= ms) {
            rd->read_seq = seq;
            if (out_ms) {
                *out_ms = s->dts_ms;
            }
            ztk_mutex_unlock(rd->fifo->mtx);
            zms_vod_buffer_reader_seek_video_key(rd);
            if (out_ms) {
                zms_gop_slot slot;
                if (zms_vod_buffer_reader_peek_muxed(rd, &slot)) {
                    *out_ms = slot.dts_ms;
                }
            }
            return 1;
        }
    }
    ztk_mutex_unlock(rd->fifo->mtx);
    return 0;
}

size_t zms_vod_buffer_reader_lag(const zms_vod_buffer_reader *rd)
{
    if (!rd || !rd->fifo) {
        return 0;
    }
    if (rd->fifo->write_seq <= rd->read_seq) {
        return 0;
    }
    return (size_t)(rd->fifo->write_seq - rd->read_seq);
}
