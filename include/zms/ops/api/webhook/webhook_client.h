#ifndef ZMS_API_WEBHOOK_WEBHOOK_CLIENT_H
#define ZMS_API_WEBHOOK_WEBHOOK_CLIENT_H

/**
 * HTTP WebHook（对应 server/WebHook.cpp 子集，供 Java/WVP 对接）。
 *
 * 鉴权（同步 POST，要求响应 JSON code==0）：
 *   on_publish 推流鉴权（RTMP publish / RTSP RECORD / RTP-PS）
 *   on_play    播放鉴权（RTSP PLAY / RTMP play / HTTP-FLV）
 *
 * 通知（异步 POST）：
 *   on_stream_changed 流注册/注销 regist=true/false
 *   on_stream_none_reader 无人观看（响应可含 "close":true）
 *   on_play 若未用于鉴权，推流成功后仍会异步通知（鉴权与通知共用 URL 时仅鉴权）
 *   on_server_started 进程启动后上报
 */
#include "zms/ops/service/config.h"
#include "zms/zms_export.h"
#include "zms/engine/media_event.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ztk_poller;
struct ztk_tcp_session;
typedef struct ztk_poller ztk_poller;
typedef struct ztk_tcp_session ztk_tcp_session;

ZMS_API void zms_webhook_init(ztk_poller *poller, const zms_config *cfg);
ZMS_API void zms_webhook_fini(void);

/** 启动后调用一次（on_server_started） */
ZMS_API void zms_webhook_server_started(const zms_config *cfg);

/**
 * 推流鉴权：1=允许，0=拒绝（hook 未配置时恒为 1）。
 * @param id 会话标识，可为 NULL
 */
ZMS_API int zms_webhook_allow_publish(zms_media_source *src, zms_media_origin origin,
                                      ztk_tcp_session *tcp, const char *id);

/**
 * 播放鉴权：1=允许，0=拒绝
 */
ZMS_API int zms_webhook_allow_play(const zms_media_tuple *tuple, const char *player_schema,
                                   ztk_tcp_session *tcp, const char *id);

/** on_stream_changed regist=true */
ZMS_API void zms_webhook_on_stream_register(zms_media_source *src, zms_media_origin origin);
/** on_stream_changed regist=false */
ZMS_API void zms_webhook_on_stream_unregister(zms_media_source *src, zms_media_origin origin);

ZMS_API void zms_webhook_on_play(const zms_media_tuple *tuple, const char *player_schema);
/**
 * 播放会话断开通知（异步 POST，on_play_stop）。
 * @param duration_ms 会话持续时长（毫秒）；0 表示未知，字段将被省略
 */
ZMS_API void zms_webhook_on_play_stop(const zms_media_tuple *tuple, const char *player_schema,
                                      uint64_t duration_ms);
ZMS_API void zms_webhook_on_none_reader(const zms_media_tuple *tuple);

/** 直播 MP4 切片完成通知（异步 POST on_record_mp4） */
ZMS_API void zms_webhook_on_record_mp4(const char *app, const char *stream, const char *file_name,
                                       const char *file_path, const char *folder, int64_t file_size,
                                       int64_t start_time, float time_len);

/** 是否配置了 on_stream_none_reader（供 media_events 调度） */
ZMS_API int zms_webhook_none_reader_configured(void);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_API_WEBHOOK_WEBHOOK_CLIENT_H */
