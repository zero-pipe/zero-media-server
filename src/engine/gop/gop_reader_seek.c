/**
 * @file gop_reader_seek.c
 * @brief GOP reader 定位策略：seek live 边缘、GOP 头关键帧、live/RTSP IDR
 *        解码起点等。从 gop_queue.c 拆分；仅操作队列/读者内部状态
 *
 * Copyright (c) zero-media-server
 */
#include "zms/engine/gop/gop_queue.h"
#include "engine/gop/gop_queue_internal.h"
#include "zms/media/codec/h264/h264_es.h"
#include "zms/media/codec/h265/h265_es.h"

void zms_gop_reader_seek_live(zms_gop_reader *rd)
{
    zms_gop_queue *r;
    size_t target;

    if (!rd || !rd->ring) {
        return;
    }
    r = rd->ring;

    frame_mux_clear(rd);
    ztk_mutex_lock(r->mu);

    /* 稳定 snap/resync：只向前裁剪 lag，禁止回退到 GOP 头 */
    if (r->write_idx > rd->read_idx) {
        size_t es_lag = r->write_idx - rd->read_idx;

        if (es_lag > ZMS_GOP_QUEUE_PLAY_MAX_LAG) {
            target = r->write_idx - ZMS_GOP_QUEUE_PLAY_MAX_LAG;
            if (target > rd->read_idx) {
                rd->read_idx = target;
            }
        }
    }
    if (r->write_idx > 0 && rd->read_idx >= r->write_idx) {
        rd->read_idx = r->write_idx - 1;
    }

    ztk_mutex_unlock(r->mu);
}

/**
 * 持锁将 rd->read_idx 钳制到 live 窗口内最近的 sync 帧。
 * lag 超过 ZMS_GOP_QUEUE_PLAY_MAX_LAG 时，在 [write_idx-MAX_LAG, end) 内
 * 向前扫描最后一个 sync 帧；找不到则截断到 write_idx-1。
 * 须在 r->mu 持锁期间调用。
 */
static void seek_clamp_lag(zms_gop_queue *r, zms_gop_reader *rd, size_t end)
{
    if (rd->read_idx >= end && end > 0) {
        rd->read_idx = end - 1;
    }
    if (r->write_idx > rd->read_idx) {
        size_t lag = r->write_idx - rd->read_idx;
        if (lag > ZMS_GOP_QUEUE_PLAY_MAX_LAG) {
            size_t target = r->write_idx - ZMS_GOP_QUEUE_PLAY_MAX_LAG;
            for (size_t j = target; j < end; ++j) {
                if (ring_slot_video_sync(&r->slots[j % ZMS_GOP_QUEUE_CAPACITY])) {
                    rd->read_idx = j;
                    break;
                }
            }
        }
    }
    if (r->write_idx > 0 && rd->read_idx >= r->write_idx) {
        rd->read_idx = r->write_idx - 1;
    }
}

void zms_gop_reader_seek_gop_key(zms_gop_reader *rd)
{
    if (!rd || !rd->ring) {
        return;
    }
    zms_gop_queue *r = rd->ring;
    size_t gop;
    size_t end;

    frame_mux_clear(rd);
    ztk_mutex_lock(r->mu);
    if (r->write_idx == 0) {
        rd->read_idx = 0;
        ztk_mutex_unlock(r->mu);
        return;
    }
    if (r->gop_count == 0) {
        ztk_mutex_unlock(r->mu);
        zms_gop_reader_seek_live(rd);
        return;
    }
    /* gop_count > 0 已确定（上文分支保证） */
    gop = r->gop_start[r->gop_count - 1];
    end = r->write_idx;
    rd->read_idx = gop;

    /* 正向扫描：优先取 GOP 头起的第一个 sync 帧 */
    for (size_t i = gop; i < end; ++i) {
        if (ring_slot_video_sync(&r->slots[i % ZMS_GOP_QUEUE_CAPACITY])) {
            rd->read_idx = i;
            seek_clamp_lag(r, rd, end);
            ztk_mutex_unlock(r->mu);
            return;
        }
    }

    /* 回退扫描：GOP 范围内无 sync 帧，反向找最后一个 */
    for (size_t i = end > 0 ? end - 1 : 0;;) {
        if (ring_slot_video_sync(&r->slots[i % ZMS_GOP_QUEUE_CAPACITY])) {
            rd->read_idx = i;
            break;
        }
        if (i <= gop || i == 0) {
            break;
        }
        i--;
    }
    seek_clamp_lag(r, rd, end);
    ztk_mutex_unlock(r->mu);
}

void zms_gop_reader_seek_live_key(zms_gop_reader *rd)
{
    zms_gop_queue *r;
    size_t end;
    size_t start;

    if (!rd || !rd->ring) {
        return;
    }
    r = rd->ring;

    frame_mux_clear(rd);
    ztk_mutex_lock(r->mu);
    end = r->write_idx;
    if (end == 0) {
        rd->read_idx = 0;
        ztk_mutex_unlock(r->mu);
        return;
    }

    /* attach(use_gop)：从最后一个 GOP 头起找首个 sync，不向后回退到更早 GOP */
    if (r->gop_count > 0) {
        start = r->gop_start[r->gop_count - 1];
    } else if (end > ZMS_GOP_QUEUE_PLAY_MAX_LAG) {
        start = end - ZMS_GOP_QUEUE_PLAY_MAX_LAG;
    } else {
        start = 0;
    }

    rd->read_idx = start;
    for (size_t i = start; i < end; ++i) {
        if (ring_slot_video_sync(&r->slots[i % ZMS_GOP_QUEUE_CAPACITY])) {
            rd->read_idx = i;
        }
    }
    if (rd->read_idx >= end) {
        rd->read_idx = end - 1;
    }

    ztk_mutex_unlock(r->mu);
}

static int ring_slot_is_live_idr(const zms_gop_slot *pub)
{
    if (!pub || pub->config_frame || pub->track != ZMS_TRACK_VIDEO || !pub->data || pub->len < 5) {
        return 0;
    }
    if (pub->codec == ZMS_CODEC_H265) {
        return zms_h265_annexb_is_idr(pub->data, pub->len);
    }
    if (pub->codec == ZMS_CODEC_H264) {
        return pub->keyframe || zms_h264_annexb_is_idr(pub->data, pub->len);
    }
    return pub->keyframe || zms_gop_slot_is_decode_start(pub);
}

static void seek_live_idr_apply_pick(zms_gop_queue *r, zms_gop_reader *rd, size_t end, size_t start,
                                     size_t pick)
{
    size_t near;

    rd->read_idx = pick;
    if (end <= pick || end - pick <= ZMS_GOP_QUEUE_PLAY_MAX_LAG) {
        return;
    }
    near = end - ZMS_GOP_QUEUE_PLAY_MAX_LAG;
    if (near < start) {
        near = start;
    }
    for (size_t i = near; i < end; ++i) {
        const zms_gop_slot *pub = &r->slots[i % ZMS_GOP_QUEUE_CAPACITY].pub;

        if (ring_slot_is_live_idr(pub)) {
            rd->read_idx = i;
        }
    }
    /* GOP 超过 live 窗口且无 IDR：勿停在远 IDR（会按实时泄洪数百帧） */
    if (rd->read_idx == pick && end > pick) {
        size_t sync_pick = (size_t)-1;

        for (size_t i = near; i < end; ++i) {
            if (ring_slot_video_sync(&r->slots[i % ZMS_GOP_QUEUE_CAPACITY])) {
                sync_pick = i;
            }
        }
        if (sync_pick != (size_t)-1) {
            rd->read_idx = sync_pick;
        } else if (end > 0) {
            rd->read_idx = end - 1;
        }
    }
}

void zms_gop_reader_seek_live_idr(zms_gop_reader *rd)
{
    zms_gop_queue *r;
    size_t end;
    size_t start;
    size_t idr_pick = (size_t)-1;
    size_t sync_pick = (size_t)-1;

    if (!rd || !rd->ring) {
        return;
    }
    r = rd->ring;
    frame_mux_clear(rd);
    ztk_mutex_lock(r->mu);
    end = r->write_idx;
    if (end == 0) {
        rd->read_idx = 0;
        ztk_mutex_unlock(r->mu);
        return;
    }

    if (r->gop_count > 0) {
        start = r->gop_start[r->gop_count - 1];
    } else if (end > ZMS_GOP_QUEUE_PLAY_MAX_LAG) {
        start = end - ZMS_GOP_QUEUE_PLAY_MAX_LAG;
    } else {
        start = 0;
    }

    /* 优先：当前 GOP 内最后一个 IDR（不限近 live 窗口） */
    for (size_t i = start; i < end; ++i) {
        const zms_gop_slot *pub = &r->slots[i % ZMS_GOP_QUEUE_CAPACITY].pub;

        if (ring_slot_is_live_idr(pub)) {
            idr_pick = i;
        }
    }

    if (idr_pick != (size_t)-1) {
        seek_live_idr_apply_pick(r, rd, end, start, idr_pick);
    } else {
        size_t near_start = start;

        /* 次选：live 窗口内最后一个 CRA/sync（低 lag 起播） */
        if (end > near_start && end - near_start > ZMS_GOP_QUEUE_PLAY_MAX_LAG) {
            near_start = end - ZMS_GOP_QUEUE_PLAY_MAX_LAG;
        }
        for (size_t i = near_start; i < end; ++i) {
            if (ring_slot_video_sync(&r->slots[i % ZMS_GOP_QUEUE_CAPACITY])) {
                sync_pick = i;
            }
        }
        if (sync_pick != (size_t)-1) {
            rd->read_idx = sync_pick;
        } else {
            /* 再次：整 GOP 的 CRA/sync（须在 lag 上限内，否则停在 live 边缘） */
            for (size_t i = start; i < near_start; ++i) {
                if (ring_slot_video_sync(&r->slots[i % ZMS_GOP_QUEUE_CAPACITY])) {
                    sync_pick = i;
                }
            }
            if (sync_pick != (size_t)-1 && end - sync_pick <= ZMS_GOP_QUEUE_PLAY_MAX_LAG) {
                rd->read_idx = sync_pick;
            } else {
                rd->read_idx = end - 1;
            }
        }
    }

    ztk_mutex_unlock(r->mu);
}

void zms_gop_reader_seek_rtsp_idr(zms_gop_reader *rd)
{
    zms_gop_queue *r;
    size_t end;
    size_t start;
    size_t idr_pick = (size_t)-1;

    if (!rd || !rd->ring) {
        return;
    }
    r = rd->ring;
    frame_mux_clear(rd);
    ztk_mutex_lock(r->mu);
    end = r->write_idx;
    if (end == 0) {
        rd->read_idx = 0;
        ztk_mutex_unlock(r->mu);
        return;
    }

    if (r->gop_count > 0) {
        start = r->gop_start[r->gop_count - 1];
    } else if (end > ZMS_GOP_QUEUE_PLAY_MAX_LAG) {
        start = end - ZMS_GOP_QUEUE_PLAY_MAX_LAG;
    } else {
        start = 0;
    }

    for (size_t i = start; i < end; ++i) {
        if (ring_slot_is_live_idr(&r->slots[i % ZMS_GOP_QUEUE_CAPACITY].pub)) {
            idr_pick = i;
        }
    }

    if (idr_pick != (size_t)-1) {
        seek_live_idr_apply_pick(r, rd, end, start, idr_pick);
    } else if (end > 0) {
        rd->read_idx = end - 1;
    }

    ztk_mutex_unlock(r->mu);
}

void zms_gop_reader_seek_decode_start(zms_gop_reader *rd)
{
    zms_gop_queue *r;
    size_t end;

    if (!rd || !rd->ring) {
        return;
    }
    r = rd->ring;
    frame_mux_clear(rd);
    ztk_mutex_lock(r->mu);
    end = r->write_idx;
    if (end == 0 || rd->read_idx >= end) {
        ztk_mutex_unlock(r->mu);
        return;
    }
    for (size_t i = rd->read_idx; i < end; ++i) {
        if (zms_gop_slot_is_decode_start(&r->slots[i % ZMS_GOP_QUEUE_CAPACITY].pub)) {
            rd->read_idx = i;
            break;
        }
    }
    ztk_mutex_unlock(r->mu);
}
