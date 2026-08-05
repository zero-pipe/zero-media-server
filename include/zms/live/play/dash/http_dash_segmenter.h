#ifndef ZMS_LIVE_EGRESS_DASH_SEGMENTER_H
#define ZMS_LIVE_EGRESS_DASH_SEGMENTER_H

/**
 * Live DASH sidecar：gop_queue → libdash dash_mpd + fMP4 分片（内存环）。
 */
#include "zms/egress/egress_segment_recorder.h"
#include "zms/engine/stream/stream_hub.h"
#include "zms/zms_export.h"
#include "ztk/poller/poller.h"
#include "ztk/util/buf.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zms_http_dash_segmenter_opts {
    float segment_duration_sec;
    unsigned segment_count;
    int enable_audio;
} zms_http_dash_segmenter_opts;

typedef struct zms_http_dash_playlist zms_http_dash_playlist;
typedef struct zms_http_dash_segmenter zms_http_dash_segmenter;

ZMS_API void zms_http_dash_segmenter_default_opts(zms_http_dash_segmenter_opts *opts);
ZMS_API zms_http_dash_segmenter *
zms_http_dash_segmenter_create(zms_media_source *src, const zms_http_dash_segmenter_opts *opts);
ZMS_API void zms_http_dash_segmenter_destroy(zms_http_dash_segmenter *rec);
ZMS_API zms_http_dash_playlist *zms_http_dash_segmenter_playlist(zms_http_dash_segmenter *rec);
ZMS_API void zms_http_dash_segmenter_bind_timer(zms_http_dash_segmenter *rec, ztk_poller *poller);
/** HTTP 服务期间暂停 timer（depth>0）；结束后仅在主 poller 上恢复 */
ZMS_API void zms_http_dash_segmenter_http_enter(zms_http_dash_segmenter *rec);
ZMS_API void zms_http_dash_segmenter_http_leave(zms_http_dash_segmenter *rec, ztk_poller *poller);
ZMS_API void zms_http_dash_segmenter_tick(zms_http_dash_segmenter *rec);
/** 持锁 pump + 生成 MPD，避免与 timer tick 并发更新 libdash 时间轴 */
ZMS_API ztk_err_t zms_http_dash_segmenter_serve_mpd(zms_http_dash_segmenter *rec, char *out,
                                                    size_t cap, size_t *out_len, int max_ticks,
                                                    int *has_media);
/** 持锁 pump + 拷贝分片 */
ZMS_API ztk_err_t zms_http_dash_segmenter_copy_segment(zms_http_dash_segmenter *rec,
                                                       const char *name, uint8_t *buf, size_t cap,
                                                       size_t *out_len, int max_ticks);
ZMS_API ztk_err_t zms_http_dash_segmenter_ref_segment(zms_http_dash_segmenter *rec,
                                                      const char *name, ztk_buf **out_buf,
                                                      size_t *out_len, int max_ticks);

ZMS_API const zms_segment_recorder_ops *zms_http_dash_segment_recorder_ops(void);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_LIVE_EGRESS_DASH_SEGMENTER_H */
