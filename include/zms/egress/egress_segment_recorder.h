#ifndef ZMS_EGRESS_SEGMENT_RECORDER_H
#define ZMS_EGRESS_SEGMENT_RECORDER_H

/**
 * @file egress_segment_recorder.h
 * @brief 直播分片 sidecar 注册表（HLS / DASH）。
 *
 * 附着于 gop_queue。VOD 切片位于 vod/play（如 vod_hls）。
 */
#include "zms/engine/stream/stream_hub.h"
#include "zms/zms_export.h"
#include "ztk/ztk_errno.h"

struct ztk_poller;
typedef struct ztk_poller ztk_poller;

#ifdef __cplusplus
extern "C" {
#endif

#define ZMS_SEGMENT_REC_HLS "hls"
#define ZMS_SEGMENT_REC_DASH "dash"

/** 每次 HTTP 服务 pump 的最大 tick 数（缩短持锁时间）。 */
#define ZMS_SEGMENT_REC_SERVE_MAX_TICKS 8
/** 每个 recorder 定时 tick 从 gop_queue mux 的帧数。 */
#define ZMS_SEGMENT_REC_TICK_FRAMES 32

typedef struct zms_segment_recorder_ops {
    const char *name;
    /** 仅直播；VOD 须返回 ZTK_ERR_INVALID。 */
    ztk_err_t (*create_live)(zms_media_source *src, const void *opts, void **out_rec);
    void (*destroy)(void *rec);
    void (*bind_timer)(void *rec, ztk_poller *poller);
    void (*tick)(void *rec);
    /** HTTP 拉流保活（滑动窗口）。 */
    void (*touch)(void *rec);
} zms_segment_recorder_ops;

ZMS_API void zms_segment_recorder_register(const zms_segment_recorder_ops *ops);
/** 注册内建 HLS/DASH ops — 仅由 zms_modules_register_all 调用。 */
ZMS_API void zms_segment_recorder_register_builtins(void);

ZMS_API const zms_segment_recorder_ops *zms_segment_recorder_find_ops(const char *name);

ZMS_API void *zms_media_source_segment_rec_get(const zms_media_source *src, const char *name);
ZMS_API ztk_err_t zms_media_source_segment_rec_set(zms_media_source *src, const char *name,
                                                   void *rec);

ZMS_API ztk_err_t zms_segment_recorder_create_live(zms_media_source *src, const char *name,
                                                   const void *opts);
ZMS_API void zms_segment_recorder_destroy_live(zms_media_source *src, const char *name);
ZMS_API void zms_segment_recorder_destroy_all(zms_media_source *src);

ZMS_API ztk_err_t zms_segment_recorder_ensure_live(zms_media_source *src, const char *name,
                                                   ztk_poller *poller, const void *opts);
ZMS_API void zms_segment_recorder_bind_timer(zms_media_source *src, const char *name,
                                             ztk_poller *poller);
ZMS_API void zms_segment_recorder_tick(zms_media_source *src, const char *name);
ZMS_API void zms_segment_recorder_touch(zms_media_source *src, const char *name);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_EGRESS_SEGMENT_RECORDER_H */
