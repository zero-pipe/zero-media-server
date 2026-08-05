#ifndef ZMS_ENGINE_MEDIA_EVENT_H
#define ZMS_ENGINE_MEDIA_EVENT_H

/**
 * 媒体生命周期事件（推流 / 播放 / 读者变更）。
 *
 * on_media_publish          流注册
 * on_media_publish_fini     流注销
 * on_media_play             开始播放
 * on_media_stop             停止播放
 * on_media_reader_changed   观众数变更
 * on_media_none_reader      无观众（触发延迟清理）
 */
#include "zms/engine/stream/stream_hub.h"
#include "zms/zms_export.h"

#ifdef __cplusplus
extern "C" {
#endif

struct ztk_poller;
typedef struct ztk_poller ztk_poller;

/** 推流来源类型 */
typedef enum {
    ZMS_ORIGIN_UNKNOWN = 0,
    ZMS_ORIGIN_RTMP_PUSH = 1,
    ZMS_ORIGIN_RTSP_PUSH = 2,
    ZMS_ORIGIN_SRT_PUSH = 4,
    ZMS_ORIGIN_PULL = 8,
    ZMS_ORIGIN_MP4_VOD = 16,
    ZMS_ORIGIN_RTP_PS_PUSH = 32,
    ZMS_ORIGIN_WEBRTC_PUSH = 64,
} zms_media_origin;

typedef struct zms_media_tuple {
    char schema[ZMS_SCHEMA_MAX];
    char app[ZMS_APP_MAX];
    char stream[ZMS_STREAM_MAX];
} zms_media_tuple;

typedef void (*zms_media_publish_cb)(const zms_media_tuple *tuple, zms_media_origin origin,
                                     void *user);
typedef void (*zms_media_publish_fini_cb)(const zms_media_tuple *tuple, zms_media_origin origin,
                                          void *user);
typedef void (*zms_media_play_cb)(const zms_media_tuple *tuple, const char *player_schema,
                                  void *user);
typedef void (*zms_media_stop_cb)(const zms_media_tuple *tuple, const char *player_schema,
                                  void *user);
typedef void (*zms_media_reader_changed_cb)(const zms_media_tuple *tuple, int total_reader_count,
                                            void *user);
typedef void (*zms_media_none_reader_cb)(const zms_media_tuple *tuple, void *user);

typedef struct zms_media_events {
    zms_media_publish_cb on_media_publish;
    zms_media_publish_fini_cb on_media_publish_fini;
    zms_media_play_cb on_media_play;
    zms_media_stop_cb on_media_stop;
    zms_media_reader_changed_cb on_media_reader_changed;
    zms_media_none_reader_cb on_media_none_reader;
    void *user;
} zms_media_events;

/** 将来源枚举转为小写下划线字符串（如 "rtmp_push"、"rtsp_push"）。 */
ZMS_API const char *zms_media_origin_str(zms_media_origin origin);

/** 注册全局事件回调；poller 用于无观众延迟（可为 NULL）。 */
ZMS_API void zms_media_events_set(ztk_poller *poller, const zms_media_events *events,
                                  int none_reader_delay_ms);
ZMS_API ztk_poller *zms_media_events_poller(void);

/** 服务监听端口（推流成功后日志 / API URL 打印）。 */
ZMS_API void zms_media_events_set_server_ports(const zms_media_server_ports *ports);
ZMS_API const zms_media_server_ports *zms_media_events_server_ports(void);

ZMS_API void zms_media_tuple_from_source(const zms_media_source *src, zms_media_tuple *tuple);

ZMS_API void zms_media_event_publish(zms_media_source *src, zms_media_origin origin);
ZMS_API void zms_media_event_publish_fini(zms_media_source *src, zms_media_origin origin);
ZMS_API void zms_media_event_play(zms_media_source *src, const char *player_schema);
/**
 * 通知出站会话已结束。
 * @param play_start_ms  播放开始的单调时间戳（ztk_monotonic_ms()）；
 *                       未知则传 0（webhook 将省略 duration_ms）。
 */
ZMS_API void zms_media_event_stop(zms_media_source *src, const char *player_schema,
                                  uint64_t play_start_ms);

/** 观众附着/脱离（触发 totalReaderCount / onReaderChanged）。 */
ZMS_API void zms_media_source_reader_add(zms_media_source *src);
ZMS_API void zms_media_source_reader_remove(zms_media_source *src);
ZMS_API int zms_media_source_reader_count(const zms_media_source *src);

/** 取消待处理的无观众定时器；poller 池关停前调用。 */
ZMS_API void zms_media_events_fini(void);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_ENGINE_MEDIA_EVENT_H */
