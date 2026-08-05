#ifndef ZMS_LIVE_EGRESS_HLS_SEGMENTER_H
#define ZMS_LIVE_EGRESS_HLS_SEGMENTER_H

/**
 * Live HLS 录制：从 gop_queue ES（H.264 Annex-B / AAC）→ TS 切片。
 * 点播 HLS 请用 vod_hls，不得使用本模块。
 */
#include "zms/engine/stream/stream_hub.h"
#include "zms/live/play/hls/http_hls_playlist.h"
#include "zms/zms_export.h"
#include "ztk/ztk_errno.h"

struct ztk_poller;
typedef struct ztk_poller ztk_poller;

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zms_http_hls_segmenter_opts {
    float segment_duration_sec;
    unsigned segment_count;
    int enable_audio;
} zms_http_hls_segmenter_opts;

typedef struct zms_http_hls_segmenter zms_http_hls_segmenter;

ZMS_API void zms_http_hls_segmenter_default_opts(zms_http_hls_segmenter_opts *opts);

/** 直播推流开始时创建；关流时 destroy。VOD 返回 NULL */
ZMS_API zms_http_hls_segmenter *
zms_http_hls_segmenter_create(zms_media_source *src, const zms_http_hls_segmenter_opts *opts);
ZMS_API void zms_http_hls_segmenter_destroy(zms_http_hls_segmenter *rec);

/** poller manager HTTP 定时路径调用，从 ring 拉取并切片 */
ZMS_API void zms_http_hls_segmenter_tick(zms_http_hls_segmenter *rec);

/** HTTP 服务路径：持锁期间拉 ring（调用方须已 zms_http_hls_segmenter_lock） */
ZMS_API void zms_http_hls_segmenter_tick_locked(zms_http_hls_segmenter *rec);

ZMS_API void zms_http_hls_segmenter_lock(zms_http_hls_segmenter *rec);
ZMS_API void zms_http_hls_segmenter_unlock(zms_http_hls_segmenter *rec);

ZMS_API zms_http_hls_playlist *zms_http_hls_segmenter_playlist(zms_http_hls_segmenter *rec);
ZMS_API void zms_http_hls_segmenter_bind_timer(zms_http_hls_segmenter *rec, ztk_poller *poller);

/** HTTP 服务期间暂停 timer（depth>0）；结束后在 poller 上恢复 */
ZMS_API void zms_http_hls_segmenter_http_enter(zms_http_hls_segmenter *rec);
ZMS_API void zms_http_hls_segmenter_http_leave(zms_http_hls_segmenter *rec);

/** 持锁 pump + 生成 m3u8，避免与 timer tick 并发写 ts mux / playlist */
ZMS_API ztk_err_t zms_http_hls_segmenter_serve_m3u8(zms_http_hls_segmenter *rec, char *out,
                                                    size_t cap, size_t *out_len, int max_ticks,
                                                    int *seg_count);
/** 持锁 pump + 拷贝分片 */
ZMS_API ztk_err_t zms_http_hls_segmenter_copy_segment(zms_http_hls_segmenter *rec, const char *name,
                                                      uint8_t *buf, size_t cap, size_t *out_len,
                                                      int max_ticks);
ZMS_API ztk_err_t zms_http_hls_segmenter_ref_segment(zms_http_hls_segmenter *rec, const char *name,
                                                     ztk_buf **out_buf, size_t *out_len,
                                                     int max_ticks);

/** 记录 HLS 请求时间（live 滑动窗口保活） */
ZMS_API void zms_http_hls_segmenter_touch_hls(zms_http_hls_segmenter *rec);

/** segment_recorder 插件 ops */
ZMS_API const struct zms_segment_recorder_ops *zms_http_hls_segment_recorder_ops(void);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_LIVE_EGRESS_HLS_SEGMENTER_H */
