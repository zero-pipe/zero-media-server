#ifndef ZMS_LIVE_EGRESS_HLS_SIDECAR_H
#define ZMS_LIVE_EGRESS_HLS_SIDECAR_H

/** HLS 全局 init：推流时自动切片，HTTP 同端口分发 m3u8/ts */
#include "zms/engine/media_event.h"
#include "zms/zms_export.h"
#include "ztk/poller/poller.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zms_http_hls_opts {
    int enable;
    float segment_duration_sec;
    unsigned segment_count;
    int enable_audio;
} zms_http_hls_opts;

ZMS_API void zms_http_hls_default_opts(zms_http_hls_opts *opts);
/** 包装 media_events_set，在推流/断流时自动启停 HLS 切片 */
ZMS_API void zms_http_hls_init(ztk_poller *poller, const zms_http_hls_opts *hls,
                               const zms_media_events *events, int none_reader_delay_ms);
ZMS_API void zms_http_hls_fini(void);

/** HLS 切片 timer 绑到 zms_http_hls_init 的主 poller；@p poller 已忽略（E2-5） */
ZMS_API void zms_http_hls_bind_recorder_timer(zms_media_source *src, ztk_poller *poller);
ZMS_API ztk_poller *zms_http_hls_main_poller(void);
/** 直播 HLS 懒创建；timer 始终绑主 poller，@p poller 已忽略 */
ZMS_API void zms_http_hls_ensure_recorder(zms_media_source *src, ztk_poller *poller);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_LIVE_EGRESS_HLS_SIDECAR_H */
