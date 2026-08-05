/**
 * @file gop_queue.c
 * @brief 直播 GOP 缓存与多读者挂接/seek 实现。
 *
 * Copyright (c) zero-media-server
 */
#include "zms/engine/gop/gop_queue.h"
#include "engine/gop/gop_queue_internal.h"
#include "zms/engine/media/media_limits.h"
#include "zms/media/codec/h264/h264_es.h"
#include "zms/media/codec/h265/h265_es.h"
#include "zms/media/container/flv/flv_tag_probe.h"
#include "zms/util/buf_pool.h"
#include "ztk/thread/sync.h"
#include "ztk/util/buf.h"
#include "ztk/util/mpsc.h"
#include "ztk/util/log.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER)
#include <windows.h>
#endif

static size_t oldest_idx(const zms_gop_queue *r);
static size_t beginning_idx(const zms_gop_queue *r);

int ring_slot_video_sync(const zms_frame_slot *slot)
{
    if (!slot || !slot->pub.data || slot->pub.len == 0 || slot->pub.config_frame) {
        return 0;
    }
    if (slot->pub.track != ZMS_TRACK_VIDEO) {
        return 0;
    }
    if (slot->pub.codec == ZMS_CODEC_H265) {
        /* 接受 IDR 与 CRA（type 16-21）：libx265/FFmpeg 常发 CRA_NUT */
        return zms_h265_annexb_is_sync_key(slot->pub.data, slot->pub.len);
    }
    if (slot->pub.keyframe) {
        return 1;
    }
    if (slot->pub.codec == ZMS_CODEC_H264) {
        return zms_h264_annexb_is_sync_key(slot->pub.data, slot->pub.len);
    }
    return zms_gop_slot_is_egress_sync(&slot->pub);
}

#define ZMS_GOP_QUEUE_WAKE_CAP 64u
#define ZMS_GOP_QUEUE_READER_WAKE_CAP 8u
/** 持锁快照唤醒目标；溢出时仍持锁 push。 */
#define ZMS_GOP_QUEUE_SIGNAL_SNAP 128

static unsigned g_default_target_gops = ZMS_GOP_QUEUE_TARGET_GOPS;
static unsigned g_default_cache_ms = ZMS_GOP_QUEUE_DEFAULT_CACHE_MS;

static void zms_gop_barrier(void)
{
#if defined(_MSC_VER)
    MemoryBarrier();
#elif defined(__GNUC__) || defined(__clang__)
    __sync_synchronize();
#else
    /* 尽力而为 */
#endif
}

static void ring_cpu_pause(void)
{
#if defined(_MSC_VER)
    YieldProcessor();
#elif defined(__i386__) || defined(__x86_64__)
    __asm__ __volatile__("pause");
#else
    /* 空操作 */
#endif
}

static unsigned clamp_target_gops(unsigned n)
{
    if (n < 1u) {
        return 1u;
    }
    if (n > ZMS_GOP_QUEUE_MAX_GOP) {
        return ZMS_GOP_QUEUE_MAX_GOP;
    }
    return n;
}

void zms_gop_queue_set_default_target_gops(unsigned n)
{
    g_default_target_gops = clamp_target_gops(n);
}

void zms_gop_queue_set_target_gops(zms_gop_queue *r, unsigned n)
{
    if (!r) {
        return;
    }
    n = clamp_target_gops(n);
    ztk_mutex_lock(r->mu);
    r->target_gops = n;
    ztk_mutex_unlock(r->mu);
}

void zms_gop_queue_set_default_cache_ms(unsigned ms)
{
    g_default_cache_ms = ms;
}

void zms_gop_queue_set_cache_ms(zms_gop_queue *r, unsigned ms)
{
    if (!r) {
        return;
    }
    ztk_mutex_lock(r->mu);
    r->cache_ms = ms;
    ztk_mutex_unlock(r->mu);
}

static void reader_unref_held(zms_gop_reader *rd)
{
    if (!rd || !rd->held_ref) {
        return;
    }
    ztk_buf_unref(rd->held_ref);
    rd->held_ref = NULL;
}

void frame_mux_clear(zms_gop_reader *rd)
{
    if (!rd) {
        return;
    }
    for (int i = 0; i < rd->mux_cnt; ++i) {
        if (rd->mux[i].buf) {
            ztk_buf_unref(rd->mux[i].buf);
        }
        rd->mux[i].buf = NULL;
    }
    rd->mux_cnt = 0;
}

static int frame_mux_pick_best(const zms_gop_reader *rd, uint32_t max_skew_ms)
{
    int idx_v = -1;
    int idx_a = -1;
    uint32_t min_v = UINT32_MAX;
    uint32_t min_a = UINT32_MAX;

    for (int i = 0; i < rd->mux_cnt; ++i) {
        if (rd->mux[i].pub.track == ZMS_TRACK_AUDIO) {
            if (rd->mux[i].pub.dts_ms < min_a) {
                min_a = rd->mux[i].pub.dts_ms;
                idx_a = i;
            }
        } else {
            if (rd->mux[i].pub.dts_ms < min_v) {
                min_v = rd->mux[i].pub.dts_ms;
                idx_v = i;
            }
        }
    }

    if (idx_v >= 0 && idx_a >= 0) {
        if (min_a + max_skew_ms < min_v) {
            return idx_a;
        }
        if (min_v + max_skew_ms < min_a) {
            return idx_v;
        }
        return (min_a <= min_v) ? idx_a : idx_v;
    }
    if (idx_a >= 0) {
        return idx_a;
    }
    if (idx_v >= 0) {
        return idx_v;
    }
    return -1;
}

static void free_slot(zms_frame_slot *p)
{
    if (!p) {
        return;
    }
    p->seq++;
    zms_gop_barrier();
    if (p->buf) {
        ztk_buf_unref(p->buf);
    }
    p->buf = NULL;
    p->pub.data = NULL;
    p->pub.len = 0;
    zms_gop_barrier();
    p->seq++;
}

/** 等待无锁读者结束观察（迷你 RCU grace）。调用方已持 mu。 */
static void ring_wait_quiescent(zms_gop_queue *r)
{
    unsigned spins = 0;

    if (!r) {
        return;
    }
    for (;;) {
        int busy = 0;
        int i;

        for (i = 0; i < r->reader_count; ++i) {
            zms_gop_reader *rd = r->readers[i];
            if (rd && rd->in_cs) {
                busy = 1;
                break;
            }
        }
        if (!busy) {
            return;
        }
        ring_cpu_pause();
        if (++spins > 4000000u) {
            /* 极慢读者 CS——继续；try_ref 已缓解多数竞态。 */
            return;
        }
    }
}

static void slot_publish(zms_gop_queue *r, zms_frame_slot *slot, ztk_buf *buf, const zms_frame *frame,
                         uint32_t dts_ms, uint32_t pts_ms)
{
    ztk_buf *old;

    if (!r || !slot || !buf || !frame) {
        return;
    }
    old = (slot->buf == buf) ? NULL : slot->buf;
    slot->seq++;
    zms_gop_barrier();
    slot->buf = buf;
    slot->pub.codec = frame->codec;
    slot->pub.track = frame->track;
    slot->pub.keyframe = frame->keyframe;
    slot->pub.config_frame = frame->config_frame;
    slot->pub.drop_able = frame->drop_able;
    slot->pub.dts_ms = dts_ms;
    slot->pub.pts_ms = pts_ms;
    slot->pub.data = (uint8_t *)ztk_buf_data(buf);
    slot->pub.len = ztk_buf_len(buf);
    zms_gop_barrier();
    slot->seq++;
    if (old) {
        ring_wait_quiescent(r);
        ztk_buf_unref(old);
    }
}

static void publish_oldest(zms_gop_queue *r)
{
    size_t oldest;

    if (!r) {
        return;
    }
    oldest = oldest_idx(r);
    zms_gop_barrier();
    r->oldest_seq = oldest;
}

static void ring_post_wake(zms_gop_queue *r)
{
    if (!r || !r->wake_q) {
        return;
    }
    (void)ztk_mpsc_push(r->wake_q, r);
}

static void ring_signal_readers(zms_gop_queue *r, size_t seq, ztk_mpsc_queue **snaps, int *snap_n,
                                int snap_cap)
{
    int i;

    if (!r || !snaps || !snap_n) {
        return;
    }
    *snap_n = 0;
    for (i = 0; i < r->reader_count; ++i) {
        zms_gop_reader *rd = r->readers[i];

        if (!rd) {
            continue;
        }
        rd->wake_pending = 1;
        if (!rd->wake_q) {
            continue;
        }
        if (*snap_n < snap_cap) {
            snaps[(*snap_n)++] = rd->wake_q;
        } else {
            /* 少见：读者数超 snap 容量——持锁 push。 */
            (void)ztk_mpsc_push(rd->wake_q, (void *)(uintptr_t)seq);
        }
    }
}

static void ring_flush_reader_wakes(ztk_mpsc_queue **snaps, int snap_n, size_t seq)
{
    int i;

    for (i = 0; i < snap_n; ++i) {
        if (snaps[i]) {
            (void)ztk_mpsc_push(snaps[i], (void *)(uintptr_t)seq);
        }
    }
}

static int ring_reader_register(zms_gop_queue *r, zms_gop_reader *rd)
{
    if (!r || !rd) {
        return -1;
    }
    if (r->reader_count >= r->reader_cap) {
        /* 动态扩容：每次翻倍，最小初始容量 ZMS_GOP_QUEUE_READERS_INIT_CAP */
        int new_cap = r->reader_cap > 0 ? r->reader_cap * 2 : (int)ZMS_GOP_QUEUE_READERS_INIT_CAP;
        zms_gop_reader **buf =
            (zms_gop_reader **)realloc(r->readers, (size_t)new_cap * sizeof(r->readers[0]));
        if (!buf) {
            ztk_warn("gop_queue: reader registry grow failed (count=%d)", r->reader_count);
            return -1;
        }
        r->readers = buf;
        r->reader_cap = new_cap;
    }
    r->readers[r->reader_count++] = rd;
    return 0;
}

static void ring_reader_unregister(zms_gop_queue *r, zms_gop_reader *rd)
{
    int i;

    if (!r || !rd) {
        return;
    }
    for (i = 0; i < r->reader_count; ++i) {
        if (r->readers[i] != rd) {
            continue;
        }
        if (i + 1 < r->reader_count) {
            memmove(&r->readers[i], &r->readers[i + 1],
                    (size_t)(r->reader_count - i - 1) * sizeof(r->readers[0]));
        }
        r->reader_count--;
        return;
    }
}

static size_t beginning_idx(const zms_gop_queue *r)
{
    if (r->count > 0 && r->write_idx >= r->count) {
        return r->write_idx - r->count;
    }
    return 0;
}

static size_t oldest_idx(const zms_gop_queue *r)
{
    if (r->gop_count > 0) {
        return r->gop_start[0];
    }
    if (r->count > 0) {
        return r->write_idx - r->count;
    }
    return r->write_idx;
}

/**
 * 无锁 slot 观察（RCU 式）。
 * @return 1 成功（out_buf 已引用），0 重试/busy，-1 空 slot 跳过。
 */
static int reader_try_take_slot(zms_gop_reader *rd, size_t idx, zms_gop_slot *out, ztk_buf **out_buf)
{
    zms_gop_queue *r;
    zms_frame_slot *slot;
    uint32_t s1;
    uint32_t s2;
    ztk_buf *buf;
    zms_gop_slot pub;

    if (!rd || !rd->ring || !out || !out_buf) {
        return 0;
    }
    r = rd->ring;
    slot = &r->slots[idx % ZMS_GOP_QUEUE_CAPACITY];

    rd->in_cs = 1;
    zms_gop_barrier();

    s1 = slot->seq;
    if (s1 & 1u) {
        rd->in_cs = 0;
        return 0;
    }
    zms_gop_barrier();
    buf = slot->buf;
    pub = slot->pub;
    zms_gop_barrier();
    s2 = slot->seq;
    if (s1 != s2 || (s2 & 1u)) {
        rd->in_cs = 0;
        return 0;
    }
    if (!buf || pub.len == 0) {
        rd->in_cs = 0;
        return -1;
    }
    if (!ztk_buf_try_ref(buf)) {
        rd->in_cs = 0;
        return 0;
    }
    zms_gop_barrier();
    s2 = slot->seq;
    if (s1 != s2) {
        ztk_buf_unref(buf);
        rd->in_cs = 0;
        return 0;
    }
    rd->in_cs = 0;

    pub.data = (uint8_t *)ztk_buf_data(buf);
    pub.len = ztk_buf_len(buf);
    *out = pub;
    *out_buf = buf;
    return 1;
}

static int frame_mux_fill_one_locked(zms_gop_reader *rd)
{
    zms_gop_queue *r;
    size_t oldest;
    zms_frame_slot *slot;
    zms_gop_frame_mux_item *it;
    ztk_buf *buf;

    if (!rd || !rd->ring) {
        return 0;
    }
    r = rd->ring;

    ztk_mutex_lock(r->mu);
    oldest = rd->from_beginning ? beginning_idx(r) : oldest_idx(r);
    if (rd->read_idx < oldest) {
        size_t snap = oldest;

        if (r->gop_count > 0) {
            size_t gop = r->gop_start[r->gop_count - 1];
            if (gop >= oldest) {
                snap = gop;
            }
        }
        rd->read_idx = snap;
        for (size_t i = snap; i < r->write_idx; ++i) {
            if (ring_slot_video_sync(&r->slots[i % ZMS_GOP_QUEUE_CAPACITY])) {
                rd->read_idx = i;
                break;
            }
        }
    }
    if (rd->read_idx >= r->write_idx) {
        ztk_mutex_unlock(r->mu);
        return 0;
    }

    slot = &r->slots[rd->read_idx % ZMS_GOP_QUEUE_CAPACITY];
    rd->read_idx++;
    if (!slot->buf || slot->pub.len == 0) {
        ztk_mutex_unlock(r->mu);
        return 2;
    }

    buf = ztk_buf_ref(slot->buf);
    it = &rd->mux[rd->mux_cnt];
    it->pub = slot->pub;
    it->buf = buf;
    it->pub.data = (uint8_t *)ztk_buf_data(buf);
    it->pub.len = ztk_buf_len(buf);
    rd->mux_cnt++;
    ztk_mutex_unlock(r->mu);
    return 1;
}

static int frame_mux_fill_one(zms_gop_reader *rd)
{
    zms_gop_queue *r;
    size_t idx;
    zms_gop_slot pub;
    ztk_buf *buf = NULL;
    zms_gop_frame_mux_item *it;
    int n;

    if (!rd || !rd->ring) {
        return 0;
    }
    r = rd->ring;

    if (rd->read_idx >= r->write_seq) {
        return 0;
    }
    if (rd->from_beginning || rd->read_idx < r->oldest_seq) {
        return frame_mux_fill_one_locked(rd);
    }

    idx = rd->read_idx;
    n = reader_try_take_slot(rd, idx, &pub, &buf);
    if (n == 1) {
        if (idx < r->oldest_seq) {
            ztk_buf_unref(buf);
            return frame_mux_fill_one_locked(rd);
        }
        rd->read_idx = idx + 1;
        it = &rd->mux[rd->mux_cnt];
        it->pub = pub;
        it->buf = buf;
        rd->mux_cnt++;
        return 1;
    }
    if (n == -1) {
        rd->read_idx = idx + 1;
        return 2;
    }
    return frame_mux_fill_one_locked(rd);
}

static int frame_mux_fill(zms_gop_reader *rd)
{
    if (!rd) {
        return 0;
    }
    while (rd->mux_cnt < ZMS_FRAME_MUX_CAP) {
        int n = frame_mux_fill_one(rd);

        if (n == 2) {
            continue;
        }
        if (n <= 0) {
            return rd->mux_cnt > 0 ? rd->mux_cnt : n;
        }
    }
    return rd->mux_cnt;
}

static void reset_gop(zms_gop_queue *r)
{
    r->gop_count = 0;
    r->cache_started = 0;
    r->video_key_pos = 0;
    r->has_video = 0;
    r->last_ts_v = 0;
    r->last_ts_a = 0;
}

static void pop_front_gop(zms_gop_queue *r)
{
    if (r->gop_count == 0) {
        return;
    }
    size_t drop_begin = r->gop_start[0];
    size_t drop_end = (r->gop_count > 1) ? r->gop_start[1] : r->write_idx;
    if (drop_end > drop_begin) {
        size_t dropped = drop_end - drop_begin;
        /* 按 abs_idx % CAP 回收 slot；write_idx 回绕时 dropped 可能 >512，需钳制 count */
        if (dropped <= r->count) {
            r->count -= dropped;
        } else {
            r->count = 0;
        }
    }
    if (r->gop_count > 1) {
        memmove(r->gop_start, r->gop_start + 1, (r->gop_count - 1) * sizeof(r->gop_start[0]));
        memmove(r->gop_dts, r->gop_dts + 1, (r->gop_count - 1) * sizeof(r->gop_dts[0]));
    }
    r->gop_count--;
    publish_oldest(r);
}

static void trim_gops_for_time(zms_gop_queue *r, uint32_t now_dts)
{
    if (!r || r->cache_ms == 0) {
        return;
    }
    while (r->gop_count > 1) {
        uint32_t oldest = r->gop_dts[0];
        if (now_dts < oldest) {
            break; /* 时间线重置/回绕——保留 */
        }
        if ((now_dts - oldest) <= r->cache_ms) {
            break;
        }
        pop_front_gop(r);
    }
}

/** 新 GOP 边界：更新 key/config 相关游标（含 _video_key_pos）。 */
static void on_new_gop(zms_gop_queue *r, uint32_t dts_ms)
{
    /* gop_count == 0：首个 GOP；否则在 gop_count-1 处开新段。
     * set_video_config 可能把 cache_started 置 1，此时仍按新 gop 处理。 */
    if (!r->cache_started || r->gop_count == 0) {
        r->cache_started = 1;
        r->gop_start[0] = r->write_idx;
        r->gop_dts[0] = dts_ms;
        r->gop_count = 1;
        publish_oldest(r);
        return;
    }
    size_t cur = r->gop_start[r->gop_count - 1];
    if (r->write_idx > cur) {
        while (r->gop_count >= ZMS_GOP_QUEUE_MAX_GOP ||
               (r->gop_count >= r->target_gops && r->count >= ZMS_GOP_QUEUE_CAPACITY)) {
            pop_front_gop(r);
        }
        trim_gops_for_time(r, dts_ms);
        r->gop_start[r->gop_count] = r->write_idx;
        r->gop_dts[r->gop_count] = dts_ms;
        r->gop_count++;
    }
}

static ztk_err_t ring_commit_frame(zms_gop_queue *r, const zms_frame *frame, ztk_buf *owned_buf)
{
    zms_frame_slot *slot;
    uint32_t dts_ms;
    uint32_t pts_ms;
    ztk_mpsc_queue *wake_snaps[ZMS_GOP_QUEUE_SIGNAL_SNAP];
    int wake_snap_n = 0;
    size_t seq;

    if (!r || !frame || !owned_buf || ztk_buf_len(owned_buf) == 0) {
        return ZTK_ERR_INVALID;
    }

    zms_gop_queue_timeline_from_frame(frame, &dts_ms, &pts_ms);

    ztk_mutex_lock(r->mu);

    if (frame->track == ZMS_TRACK_VIDEO && !frame->config_frame) {
        r->has_video = 1;
    }

    if (!zms_gop_queue_storable(frame, r->cache_started, r->has_video)) {
        ztk_mutex_unlock(r->mu);
        ztk_buf_unref(owned_buf);
        return ZTK_OK;
    }
    if (zms_gop_queue_new_gop(frame, r->video_key_pos, r->has_video)) {
        on_new_gop(r, dts_ms);
    }

    while (r->count >= ZMS_GOP_QUEUE_CAPACITY && r->gop_count > 0) {
        pop_front_gop(r);
    }

    slot = &r->slots[r->write_idx % ZMS_GOP_QUEUE_CAPACITY];
    slot_publish(r, slot, owned_buf, frame, dts_ms, pts_ms);

    r->write_idx++;
    if (r->count < ZMS_GOP_QUEUE_CAPACITY) {
        r->count++;
    }

    if (!frame->drop_able) {
        if (frame->track == ZMS_TRACK_VIDEO) {
            r->video_key_pos = zms_frame_video_gop_marker(frame);
        } else if (!r->has_video) {
            r->video_key_pos = 1;
        }
    }

    if (r->count >= ZMS_GOP_QUEUE_CAPACITY && r->gop_count > 1) {
        pop_front_gop(r);
    }
    trim_gops_for_time(r, dts_ms);

    zms_gop_barrier();
    r->write_seq = r->write_idx;
    seq = r->write_seq;
    ring_signal_readers(r, seq, wake_snaps, &wake_snap_n, ZMS_GOP_QUEUE_SIGNAL_SNAP);
    ztk_mutex_unlock(r->mu);
    ring_flush_reader_wakes(wake_snaps, wake_snap_n, seq);
    ring_post_wake(r);
    return ZTK_OK;
}

static ztk_err_t set_blob(uint8_t **dst, size_t *dst_len, size_t *dst_cap, const void *data,
                          size_t len)
{
    zms_buf_pool_slot_clear(dst, dst_cap);
    *dst_len = 0;
    if (!data || len == 0) {
        return ZTK_OK;
    }
    if (!zms_buf_pool_slot_resize(dst, dst_cap, len)) {
        return ZTK_ERR_NOMEM;
    }
    memcpy(*dst, data, len);
    *dst_len = len;
    return ZTK_OK;
}

zms_gop_queue *zms_gop_queue_create(void)
{
    zms_gop_queue *r = (zms_gop_queue *)calloc(1, sizeof(*r));
    if (!r) {
        return NULL;
    }
    r->readers = (zms_gop_reader **)malloc(ZMS_GOP_QUEUE_READERS_INIT_CAP * sizeof(r->readers[0]));
    if (!r->readers) {
        free(r);
        return NULL;
    }
    r->reader_cap = (int)ZMS_GOP_QUEUE_READERS_INIT_CAP;
    r->mu = ztk_mutex_create(ZTK_MUTEX_NORMAL);
    if (!r->mu) {
        free(r->readers);
        free(r);
        return NULL;
    }
    r->wake_q = ztk_mpsc_create(ZMS_GOP_QUEUE_WAKE_CAP);
    if (!r->wake_q) {
        ztk_mutex_destroy(r->mu);
        free(r->readers);
        free(r);
        return NULL;
    }
    r->target_gops = g_default_target_gops;
    r->cache_ms = g_default_cache_ms;
    r->oldest_seq = 0;
    return r;
}

int zms_gop_queue_drain_wake(zms_gop_queue *r)
{
    if (!r || !r->wake_q) {
        return 0;
    }
    return ztk_mpsc_pop(r->wake_q) != NULL;
}

int zms_gop_reader_drain_wake(const zms_gop_reader *rd)
{
    zms_gop_reader *mut;
    int got = 0;

    if (!rd) {
        return 0;
    }
    if (rd->wake_pending) {
        mut = (zms_gop_reader *)rd;
        mut->wake_pending = 0;
        got = 1;
    }
    if (rd->wake_q) {
        while (ztk_mpsc_pop(rd->wake_q) != NULL) {
            got = 1;
        }
    }
    return got;
}

static void reader_wake_destroy(zms_gop_reader *rd)
{
    if (!rd || !rd->wake_q) {
        return;
    }
    ztk_mpsc_destroy(rd->wake_q);
    rd->wake_q = NULL;
}

static zms_gop_reader *reader_alloc(zms_gop_queue *r, size_t read_idx, int from_beginning)
{
    zms_gop_reader *rd;

    if (!r) {
        return NULL;
    }
    rd = (zms_gop_reader *)calloc(1, sizeof(*rd));
    if (!rd) {
        return NULL;
    }
    rd->wake_q = ztk_mpsc_create(ZMS_GOP_QUEUE_READER_WAKE_CAP);
    if (!rd->wake_q) {
        free(rd);
        return NULL;
    }
    rd->ring = r;
    rd->read_idx = read_idx;
    rd->from_beginning = from_beginning;
    return rd;
}

void zms_gop_queue_destroy(zms_gop_queue *r)
{
    if (!r) {
        return;
    }
    for (size_t i = 0; i < ZMS_GOP_QUEUE_CAPACITY; ++i) {
        free_slot(&r->slots[i]);
    }
    zms_buf_pool_slot_clear(&r->video_config, &r->video_config_cap);
    zms_buf_pool_slot_clear(&r->audio_config, &r->audio_config_cap);
    if (r->wake_q) {
        ztk_mpsc_destroy(r->wake_q);
    }
    ztk_mutex_destroy(r->mu);
    free(r->readers);
    free(r);
}

size_t zms_gop_queue_pending_count(const zms_gop_queue *r)
{
    if (!r) {
        return 0;
    }
    size_t n;
    ztk_mutex_lock(r->mu);
    n = r->count;
    ztk_mutex_unlock(r->mu);
    return n;
}

size_t zms_gop_queue_gop_count(const zms_gop_queue *r)
{
    size_t n = 0;

    if (!r) {
        return 0;
    }
    ztk_mutex_lock(r->mu);
    n = r->gop_count;
    ztk_mutex_unlock(r->mu);
    return n;
}

void zms_gop_queue_clear(zms_gop_queue *r)
{
    if (!r) {
        return;
    }
    ztk_mutex_lock(r->mu);
    for (size_t i = 0; i < ZMS_GOP_QUEUE_CAPACITY; ++i) {
        free_slot(&r->slots[i]);
    }
    r->write_idx = 0;
    r->write_seq = 0;
    r->oldest_seq = 0;
    r->count = 0;
    reset_gop(r);
    zms_buf_pool_slot_clear(&r->video_config, &r->video_config_cap);
    r->video_config_len = 0;
    zms_buf_pool_slot_clear(&r->audio_config, &r->audio_config_cap);
    r->audio_config_len = 0;
    ztk_mutex_unlock(r->mu);
}

ztk_buf *zms_gop_queue_alloc_write(zms_gop_queue *r, size_t size)
{
    ztk_buf *buf = NULL;
    zms_frame_slot *slot;

    if (!r || size == 0) {
        return NULL;
    }

    ztk_mutex_lock(r->mu);
    slot = &r->slots[r->write_idx % ZMS_GOP_QUEUE_CAPACITY];
    if (slot->buf && ztk_buf_cap(slot->buf) >= size && ztk_buf_refcnt(slot->buf) == 1) {
        buf = slot->buf;
    }
    ztk_mutex_unlock(r->mu);

    if (!buf) {
        buf = ztk_buf_alloc(size);
    }
    return buf;
}

ztk_err_t zms_gop_queue_write(zms_gop_queue *r, const zms_frame *frame)
{
    zms_frame norm;
    ztk_buf *buf;
    void *dst;

    if (!r || !frame || !frame->data || frame->size == 0) {
        return ZTK_ERR_INVALID;
    }

    norm = *frame;
    zms_frame_refresh_key_from_es(&norm);
    frame = &norm;

    buf = zms_gop_queue_alloc_write(r, frame->size);
    if (!buf) {
        return ZTK_ERR_NOMEM;
    }
    dst = (void *)ztk_buf_data(buf);
    memcpy(dst, frame->data, frame->size);
    ztk_buf_set_len(buf, frame->size);
    return ring_commit_frame(r, frame, buf);
}

ztk_err_t zms_gop_queue_write_buf(zms_gop_queue *r, ztk_buf *buf, const zms_frame *frame)
{
    zms_frame norm;

    if (!r || !buf || !frame) {
        return ZTK_ERR_INVALID;
    }

    norm = *frame;
    zms_frame_refresh_key_from_es(&norm);
    return ring_commit_frame(r, &norm, buf);
}

ztk_err_t zms_gop_queue_set_video_config(zms_gop_queue *r, const void *data, size_t len)
{
    if (!r) {
        return ZTK_ERR_INVALID;
    }
    ztk_mutex_lock(r->mu);
    ztk_err_t err =
        set_blob(&r->video_config, &r->video_config_len, &r->video_config_cap, data, len);
    if (err == ZTK_OK) {
        r->cache_started = 1;
        r->has_video = 1;
    }
    ztk_mutex_unlock(r->mu);
    return err;
}

ztk_err_t zms_gop_queue_set_audio_config(zms_gop_queue *r, const void *data, size_t len)
{
    if (!r) {
        return ZTK_ERR_INVALID;
    }
    ztk_mutex_lock(r->mu);
    ztk_err_t err =
        set_blob(&r->audio_config, &r->audio_config_len, &r->audio_config_cap, data, len);
    ztk_mutex_unlock(r->mu);
    return err;
}

const uint8_t *zms_gop_queue_video_config(const zms_gop_queue *r, size_t *len)
{
    if (!r) {
        return NULL;
    }
    ztk_mutex_lock(r->mu);
    if (len) {
        *len = r->video_config_len;
    }
    const uint8_t *p = r->video_config;
    ztk_mutex_unlock(r->mu);
    return p;
}

const uint8_t *zms_gop_queue_audio_config(const zms_gop_queue *r, size_t *len)
{
    if (!r) {
        return NULL;
    }
    ztk_mutex_lock(r->mu);
    if (len) {
        *len = r->audio_config_len;
    }
    const uint8_t *p = r->audio_config;
    ztk_mutex_unlock(r->mu);
    return p;
}

static size_t gop_queue_copy_blob(zms_gop_queue *r, uint8_t **blob, size_t *blob_len, uint8_t *buf,
                                  size_t cap)
{
    size_t n = 0;

    if (!r || !buf || !cap) {
        return 0;
    }
    ztk_mutex_lock(r->mu);
    n = blob_len ? *blob_len : 0;
    if (blob && *blob && n > 0 && n <= cap) {
        memcpy(buf, *blob, n);
    } else {
        n = 0;
    }
    ztk_mutex_unlock(r->mu);
    return n;
}

size_t zms_gop_queue_copy_video_config(zms_gop_queue *r, uint8_t *buf, size_t cap)
{
    if (!r) {
        return 0;
    }
    return gop_queue_copy_blob(r, &r->video_config, &r->video_config_len, buf, cap);
}

size_t zms_gop_queue_copy_audio_config(zms_gop_queue *r, uint8_t *buf, size_t cap)
{
    if (!r) {
        return 0;
    }
    return gop_queue_copy_blob(r, &r->audio_config, &r->audio_config_len, buf, cap);
}

zms_codec_id zms_gop_queue_audio_codec(const zms_gop_queue *r)
{
    if (!r || r->audio_config_len == 0) {
        return ZMS_CODEC_INVALID;
    }
    return zms_flv_tag_audio_codec(r->audio_config, r->audio_config_len);
}

zms_gop_reader *zms_gop_reader_attach_beginning(zms_gop_queue *r)
{
    zms_gop_reader *rd;

    if (!r) {
        return NULL;
    }
    ztk_mutex_lock(r->mu);
    rd = reader_alloc(r, beginning_idx(r), 1);
    if (!rd || ring_reader_register(r, rd) != 0) {
        ztk_mutex_unlock(r->mu);
        reader_wake_destroy(rd);
        free(rd);
        return NULL;
    }
    ztk_mutex_unlock(r->mu);
    return rd;
}

zms_gop_reader *zms_gop_reader_attach(zms_gop_queue *r)
{
    zms_gop_reader *rd;

    if (!r) {
        return NULL;
    }
    ztk_mutex_lock(r->mu);
    rd = reader_alloc(r, oldest_idx(r), 0);
    if (!rd || ring_reader_register(r, rd) != 0) {
        ztk_mutex_unlock(r->mu);
        reader_wake_destroy(rd);
        free(rd);
        return NULL;
    }
    ztk_mutex_unlock(r->mu);
    return rd;
}

int zms_gop_reader_count(const zms_gop_queue *r)
{
    int n = 0;

    if (!r) {
        return 0;
    }
    ztk_mutex_lock(r->mu);
    n = r->reader_count;
    ztk_mutex_unlock(r->mu);
    return n;
}

void zms_gop_reader_detach(zms_gop_reader *rd)
{
    zms_gop_queue *r;

    if (!rd) {
        return;
    }
    r = rd->ring;
    if (r) {
        ztk_mutex_lock(r->mu);
        ring_reader_unregister(r, rd);
        ztk_mutex_unlock(r->mu);
    }
    frame_mux_clear(rd);
    reader_unref_held(rd);
    reader_wake_destroy(rd);
    free(rd);
}

int zms_gop_reader_slot_at_read(const zms_gop_reader *rd, zms_gop_slot *out)
{
    zms_gop_queue *r;
    const zms_frame_slot *slot;

    if (!rd || !rd->ring || !out) {
        return 0;
    }
    r = rd->ring;
    if (rd->read_idx >= r->write_seq) {
        return 0;
    }
    ztk_mutex_lock(r->mu);
    if (rd->read_idx >= r->write_idx) {
        ztk_mutex_unlock(r->mu);
        return 0;
    }
    slot = &r->slots[rd->read_idx % ZMS_GOP_QUEUE_CAPACITY];
    *out = slot->pub;
    ztk_mutex_unlock(r->mu);
    return out->data && out->len > 0;
}

int zms_gop_reader_at_decode_start(const zms_gop_reader *rd)
{
    zms_gop_queue *r;
    int ok = 0;

    if (!rd || !rd->ring) {
        return 0;
    }
    r = rd->ring;
    if (rd->read_idx >= r->write_seq) {
        return 0;
    }
    ztk_mutex_lock(r->mu);
    if (rd->read_idx < r->write_idx) {
        ok = zms_gop_slot_is_decode_start(&r->slots[rd->read_idx % ZMS_GOP_QUEUE_CAPACITY].pub);
    }
    ztk_mutex_unlock(r->mu);
    return ok;
}

size_t zms_gop_reader_lag(const zms_gop_reader *rd)
{
    size_t w;
    size_t lag = 0;

    if (!rd || !rd->ring) {
        return 0;
    }
    w = rd->ring->write_seq;
    if (w > rd->read_idx) {
        lag = w - rd->read_idx;
    }
    return lag;
}

size_t zms_gop_queue_max_reader_lag(const zms_gop_queue *r)
{
    size_t max_lag = 0;
    int i;

    if (!r) {
        return 0;
    }
    ztk_mutex_lock(r->mu);
    for (i = 0; i < r->reader_count; ++i) {
        const zms_gop_reader *rd = r->readers[i];
        size_t lag;
        size_t w;

        if (!rd) {
            continue;
        }
        w = r->write_seq;
        lag = (w > rd->read_idx) ? (w - rd->read_idx) : 0;
        if (lag > max_lag) {
            max_lag = lag;
        }
    }
    ztk_mutex_unlock(r->mu);
    return max_lag;
}

int zms_gop_reader_read(zms_gop_reader *rd, zms_gop_slot *out)
{
    zms_gop_queue *r;

    if (!rd || !out) {
        return -1;
    }
    r = rd->ring;
    if (!r) {
        return 0;
    }

    for (;;) {
        size_t w;
        size_t oldest;
        size_t idx;
        zms_frame_slot *slot;
        ztk_buf *buf;
        zms_gop_slot pub;
        int n;

        if (rd->read_idx >= r->write_seq) {
            return 0;
        }

        /* 直播快路径：RCU 式观察，不持 mu。 */
        if (!rd->from_beginning && rd->read_idx >= r->oldest_seq) {
            idx = rd->read_idx;
            n = reader_try_take_slot(rd, idx, &pub, &buf);
            if (n == 1) {
                if (idx < r->oldest_seq) {
                    ztk_buf_unref(buf);
                } else {
                    rd->read_idx = idx + 1;
                    reader_unref_held(rd);
                    rd->held_ref = buf;
                    *out = pub;
                    return 1;
                }
            } else if (n == -1) {
                rd->read_idx = idx + 1;
                continue;
            }
            /* busy/竞态 → 持锁路径 */
        }

        ztk_mutex_lock(r->mu);
        oldest = rd->from_beginning ? beginning_idx(r) : oldest_idx(r);
        if (rd->read_idx < oldest) {
            rd->read_idx = oldest;
        }

        w = r->write_idx;
        if (rd->read_idx >= w) {
            ztk_mutex_unlock(r->mu);
            return 0;
        }

        idx = rd->read_idx++;
        slot = &r->slots[idx % ZMS_GOP_QUEUE_CAPACITY];
        if (!slot->buf || slot->pub.len == 0) {
            ztk_mutex_unlock(r->mu);
            continue;
        }

        buf = ztk_buf_ref(slot->buf);
        out->codec = slot->pub.codec;
        out->track = slot->pub.track;
        out->dts_ms = slot->pub.dts_ms;
        out->pts_ms = slot->pub.pts_ms;
        out->keyframe = slot->pub.keyframe;
        out->config_frame = slot->pub.config_frame;
        out->drop_able = slot->pub.drop_able;
        out->len = slot->pub.len;
        ztk_mutex_unlock(r->mu);
        reader_unref_held(rd);
        rd->held_ref = buf;
        out->data = (uint8_t *)ztk_buf_data(buf);
        return 1;
    }
}

int zms_gop_reader_peek_muxed(zms_gop_reader *rd, zms_gop_slot *out, uint32_t max_skew_ms)
{
    int best;

    if (!rd || !out) {
        return -1;
    }
    if (max_skew_ms == 0) {
        max_skew_ms = ZMS_GOP_QUEUE_MUX_MAX_SKEW_MS;
    }

    if (rd->mux_cnt == 0) {
        int n = frame_mux_fill(rd);
        if (n <= 0) {
            return n;
        }
    }

    best = frame_mux_pick_best(rd, max_skew_ms);
    if (best < 0) {
        frame_mux_clear(rd);
        return 0;
    }

    {
        const zms_gop_frame_mux_item *it = &rd->mux[best];
        *out = it->pub;
        out->data = it->pub.data;
        out->len = it->pub.len;
    }
    return 1;
}

int zms_gop_reader_read_muxed(zms_gop_reader *rd, zms_gop_slot *out, uint32_t max_skew_ms)
{
    if (!rd || !out) {
        return -1;
    }
    if (max_skew_ms == 0) {
        max_skew_ms = ZMS_GOP_QUEUE_MUX_MAX_SKEW_MS;
    }

    if (rd->mux_cnt == 0) {
        int n = frame_mux_fill(rd);
        if (n <= 0) {
            return n;
        }
    }

    int best = frame_mux_pick_best(rd, max_skew_ms);
    if (best < 0) {
        frame_mux_clear(rd);
        return 0;
    }
    zms_gop_frame_mux_item *it = &rd->mux[best];

    *out = it->pub;
    out->data = it->pub.data;
    out->len = it->pub.len;
    reader_unref_held(rd);
    rd->held_ref = it->buf;
    it->buf = NULL;

    for (int i = best + 1; i < rd->mux_cnt; ++i) {
        rd->mux[i - 1] = rd->mux[i];
    }
    rd->mux_cnt--;
    rd->mux[rd->mux_cnt].buf = NULL;
    return 1;
}
