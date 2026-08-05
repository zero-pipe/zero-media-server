#ifndef ZMS_ENGINE_STREAM_URL_H
#define ZMS_ENGINE_STREAM_URL_H

#include "zms/engine/stream/stream_hub.h"
#include "zms/zms_export.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZMS_MEDIA_URL_MAX 256

/** 推流/播放 URL（生产环境 stream 名通常带随机后缀，通过 API 查询） */
typedef struct zms_media_urls {
    char origin[ZMS_MEDIA_URL_MAX];
    char rtmp_play[ZMS_MEDIA_URL_MAX];
    char rtsp_play[ZMS_MEDIA_URL_MAX];
    char http_flv[ZMS_MEDIA_URL_MAX];
    char http_ts[ZMS_MEDIA_URL_MAX];
    char http_mp4[ZMS_MEDIA_URL_MAX];
    char hls[ZMS_MEDIA_URL_MAX];
    char dash[ZMS_MEDIA_URL_MAX];
} zms_media_urls;

ZMS_API void zms_media_urls_build(zms_media_urls *out, const zms_media_source *src,
                                  const zms_media_server_ports *ports);

/** 推流注册后打印完整推拉地址（写入日志） */
ZMS_API void zms_media_urls_log_publish(const zms_media_source *src,
                                        const zms_media_server_ports *ports);

/** 单路 URL 明细（registry 周期 dump 用） */
ZMS_API void zms_media_urls_log_stream(const zms_media_source *src,
                                       const zms_media_server_ports *ports);

/** 去掉 HTTP 拉流路径上的 *.{mp4|mkv|mov|m4v}.flv / .flv / .m3u8 / .mpd / .mp4，得到协议无关的 stream */
ZMS_API void zms_media_path_strip_play_suffix(char *path);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_ENGINE_STREAM_URL_H */
