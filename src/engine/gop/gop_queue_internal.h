#ifndef ZMS_SRC_CORE_GOP_GOP_QUEUE_INTERNAL_H
#define ZMS_SRC_CORE_GOP_GOP_QUEUE_INTERNAL_H

/**
 * @file gop_queue_internal.h
 * @brief 直播 GOP 队列及其读者的私有布局（跨文件共享）。
 *        队列/读者核心（gop_queue.c）与读者定位模块（gop_reader_seek.c）共享。
 *        非公开 API。
 */
#include "zms/engine/gop/gop_queue.h"
#include "zms/engine/gop/gop_limits.h"
#include "zms/engine/media/media_limits.h"
#include "ztk/thread/sync.h"
#include "ztk/util/buf.h"
#include "ztk/util/mpsc.h"
#include <stddef.h>
#include <stdint.h>

#define ZMS_FRAME_MUX_CAP 24

/** ring / reader 暂存：公开 gop_slot 视图 + 持有的 ztk_buf 引用。 */
typedef struct zms_frame_slot {
    zms_gop_slot pub;
    ztk_buf *buf;
    /** Seqlock：偶数=稳定，奇数=写者更新中（RCU 式读者）。 */
    volatile uint32_t seq;
} zms_frame_slot;

/** 别名：reader A/V 交错暂存与 ring slot 同布局。 */
typedef zms_frame_slot zms_gop_frame_mux_item;

struct zms_gop_queue {
    zms_frame_slot slots[ZMS_GOP_QUEUE_CAPACITY];
    size_t write_idx;
    volatile size_t write_seq;
    /** 逻辑最旧绝对下标；无锁读者落后时 snap 对齐。 */
    volatile size_t oldest_seq;
    size_t count;
    size_t gop_start[ZMS_GOP_QUEUE_MAX_GOP];
    uint32_t gop_dts[ZMS_GOP_QUEUE_MAX_GOP]; /* 各 GOP 起点 dts_ms */
    size_t gop_count;
    unsigned target_gops; /* 压力下保留 GOP 数；默认 TARGET_GOPS */
    unsigned cache_ms;    /* 0=关闭时间裁剪；否则丢弃超此时长的 GOP */
    int cache_started;
    uint32_t last_ts_v;
    uint32_t last_ts_a;
    uint8_t *video_config;
    size_t video_config_len;
    size_t video_config_cap;
    uint8_t *audio_config;
    size_t audio_config_len;
    size_t audio_config_cap;
    int video_key_pos;
    int has_video;
    zms_gop_reader **readers; /* 动态数组，按需扩容 */
    int reader_count;
    int reader_cap;
    ztk_mutex *mu;
    ztk_mpsc_queue *wake_q;
};

struct zms_gop_reader {
    zms_gop_queue *ring;
    size_t read_idx;
    int from_beginning;
    volatile int wake_pending;
    /** 无锁临界区观察 slot 指针期间非零。 */
    volatile int in_cs;
    ztk_mpsc_queue *wake_q;
    ztk_buf *held_ref;
    zms_gop_frame_mux_item mux[ZMS_FRAME_MUX_CAP];
    int mux_cnt;
};

/* 核心与 seek 模块共享。 */
int ring_slot_video_sync(const zms_frame_slot *slot);
void frame_mux_clear(zms_gop_reader *rd);

#endif /* ZMS_SRC_CORE_GOP_GOP_QUEUE_INTERNAL_H */
