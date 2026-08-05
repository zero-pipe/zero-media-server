#ifndef ZMS_SESSION_RTSP_H
#define ZMS_SESSION_RTSP_H

#include "zms/zms_export.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZMS_RTSP_DEFAULT_PORT 554u

typedef enum zms_rtsp_transport {
    ZMS_RTSP_TRANSPORT_TCP = 0,
    ZMS_RTSP_TRANSPORT_UDP = 1,
    ZMS_RTSP_TRANSPORT_MULTICAST = 2,
} zms_rtsp_transport;

typedef enum zms_rtsp_method {
    ZMS_RTSP_OPTIONS = 0,
    ZMS_RTSP_DESCRIBE,
    ZMS_RTSP_SETUP,
    ZMS_RTSP_PLAY,
    ZMS_RTSP_PAUSE,
    ZMS_RTSP_TEARDOWN,
    ZMS_RTSP_GET_PARAMETER,
    ZMS_RTSP_SET_PARAMETER,
    ZMS_RTSP_ANNOUNCE,
    ZMS_RTSP_RECORD,
    ZMS_RTSP_UNKNOWN,
} zms_rtsp_method;

#ifdef __cplusplus
}
#endif

#endif /* ZMS_SESSION_RTSP_H */
