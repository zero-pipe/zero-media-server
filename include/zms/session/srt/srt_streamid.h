#ifndef ZMS_SESSION_SRT_STREAMID_H
#define ZMS_SESSION_SRT_STREAMID_H

#include "zms/zms_export.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum zms_srt_stream_mode {
    ZMS_SRT_MODE_PUBLISH = 0,
    ZMS_SRT_MODE_PLAY = 1,
    ZMS_SRT_MODE_INVALID = 2,
} zms_srt_stream_mode;

/** 解析 ZLM 风格 streamid（如 #!::r=live/test,m=publish）。 */
ZMS_API int zms_srt_streamid_parse(const char *streamid, char *app, char *stream,
                                   zms_srt_stream_mode *mode);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_SESSION_SRT_STREAMID_H */
