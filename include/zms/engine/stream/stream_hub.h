#ifndef ZMS_ENGINE_STREAM_HUB_H
#define ZMS_ENGINE_STREAM_HUB_H

/**
 * @file stream_hub.h
 * @brief 流注册表：app/stream 标识、轨道与共享缓冲。
 *
 * 直播 source 拥有 @ref zms_gop_queue；点播 source 使用 @ref zms_vod_buffer。
 * 同一 source 上二者互斥。
 *
 * Copyright (c) zero-media-server
 */
#include "zms/engine/stream/stream_limits.h"
#include "zms/engine/gop/gop_queue.h"
#include "zms/engine/stream/stream_stats.h"
#include "zms/engine/media_track.h"
#include "zms/engine/stream_track.h"
#include "zms/zms_export.h"
#include "ztk/ztk_errno.h"
#include <stddef.h>

struct zms_vod_buffer;

#ifdef __cplusplus
extern "C" {
#endif

#define ZMS_SCHEMA_RTMP "rtmp"
#define ZMS_SCHEMA_RTSP "rtsp"
#define ZMS_SCHEMA_SRT "srt"
#define ZMS_SCHEMA_RTP_PS "rtp-ps"

typedef struct zms_media_source {
    char schema[ZMS_SCHEMA_MAX];
    char app[ZMS_APP_MAX];
    char stream[ZMS_STREAM_MAX];
    /** 客户端推流使用的 stream 名（register_publish 时与 stream 不同） */
    char stream_requested[ZMS_STREAM_MAX];
    zms_gop_queue *gop_queue;
    struct zms_vod_buffer *vod_buffer;
    int has_video;
    int has_audio;
    zms_video_track video;
    zms_audio_track audio;
    int reader_count;
    int vod_play_lane_ref;
    int publishing;
    int publish_origin;
    uint64_t create_stamp_ms;
    zms_media_stream_stats stats;
    void *publisher_ctx;
    void (*publisher_kick)(void *ctx, int force);
    /** 直播切片 sidecar 袋（HLS/DASH）；由 egress_segment_recorder 持有。 */
    void *segment_sidecar;
    /** on_publish 响应 enable_mp4；非 0 时启动直播 MP4 录制器 */
    int enable_mp4;
    /** 直播 MP4 录制器（egress_mp4_recorder）；停推时 finalize */
    void *mp4_recorder;
} zms_media_source;

typedef struct zms_media_server_ports {
    unsigned rtmp;
    unsigned rtsp;
    unsigned http;
    unsigned srt;
} zms_media_server_ports;

typedef struct zms_media_source_filter {
    const char *schema;
    const char *app;
    const char *stream;
} zms_media_source_filter;

typedef int (*zms_media_source_visit_fn)(zms_media_source *src, void *user);

ZMS_API void zms_media_source_registry_init(void);
/** poller 线程下保护注册表与 reader_count；media 模块内部使用 */
ZMS_API void zms_media_source_registry_lock(void);
ZMS_API void zms_media_source_registry_unlock(void);
ZMS_API int zms_media_source_count(void);
ZMS_API zms_media_source *zms_media_source_find(const char *schema, const char *app,
                                                const char *stream);
/** API 查流：schema=rtsp 时回退到 rtmp 枢纽 */
ZMS_API zms_media_source *zms_media_source_find_api(const char *schema, const char *app,
                                                    const char *stream);
/** 播放查流（直播：等价于 zms_media_source_find_api） */
ZMS_API zms_media_source *zms_media_source_find_for_play(const char *schema, const char *app,
                                                         const char *stream);
ZMS_API void zms_media_source_foreach(zms_media_source_visit_fn fn, void *user,
                                      const zms_media_source_filter *filter);
ZMS_API void zms_media_source_set_publisher(zms_media_source *s, void *ctx,
                                            void (*kick)(void *ctx, int force));
ZMS_API void zms_media_source_clear_publisher(zms_media_source *s, void *ctx);
ZMS_API int zms_media_source_close(zms_media_source *s, int force);
/** 精确 stream 名注册（代理/测试）；同名校验并 kick 旧推流 */
ZMS_API zms_media_source *zms_media_source_register(const char *schema, const char *app,
                                                    const char *stream);
/** 点播注册：仅 vod_buffer，无 gop_queue */
ZMS_API zms_media_source *zms_media_source_register_vod(const char *schema, const char *app,
                                                        const char *stream);
ZMS_API void zms_media_source_unregister_vod(zms_media_source *s);
/** 推流注册：在 stream 后自动加 _id，不抢占同路流 */
ZMS_API zms_media_source *zms_media_source_register_publish(const char *schema, const char *app,
                                                            const char *stream_requested);
ZMS_API void zms_media_source_clear(zms_media_source *s);
ZMS_API void zms_media_source_unregister(zms_media_source *s);
ZMS_API void zms_media_source_log_registry(const zms_media_server_ports *ports);

/** 播放 config：来自 gop_queue */
ZMS_API const uint8_t *zms_media_source_video_config(const zms_media_source *s, size_t *len);
ZMS_API const uint8_t *zms_media_source_audio_config(const zms_media_source *s, size_t *len);

/** 播放是否可走 gop_queue（H.264/H.265 + AAC） */
ZMS_API int zms_media_source_use_gop_queue_play(const zms_media_source *s);

/** 播放订阅者（live=gop_queue reader；vod=vod_buffer reader） */
typedef struct zms_media_subscriber {
    zms_gop_reader *gop;
    struct zms_vod_buffer_reader *vod;
} zms_media_subscriber;

ZMS_API void zms_media_subscriber_detach(zms_media_subscriber *r);
/** @param seek_live 1=live 边缘（write-48，可能无 IDR）；0=从头（VOD） */
ZMS_API ztk_err_t zms_media_source_subscribe(zms_media_source *s, int seek_live,
                                             zms_media_subscriber *out);
/** 直播秒开：subscribe + seek_gop_key（RTMP/HTTP-FLV/RTSP seek_live 区分） */
ZMS_API ztk_err_t zms_media_source_subscribe_gop(zms_media_source *s, zms_media_subscriber *out);
/** 点播：attach vod_buffer reader（from_beginning=1） */
ZMS_API ztk_err_t zms_media_source_subscribe_vod(zms_media_source *s, zms_media_subscriber *out);

/**
 * 统一路径规则：首段为 app，app/ 之后整段为 stream（媒体源 id，可含 /）。
 * @param path_or_url 完整 URL 或路径（可带 / 前缀）
 */
ZMS_API void zms_media_split_path(const char *path_or_url, char *app, char *stream);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_ENGINE_STREAM_HUB_H */
