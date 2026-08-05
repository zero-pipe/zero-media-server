#ifndef ZMS_SESSION_RTSP_SPLITTER_H
#define ZMS_SESSION_RTSP_SPLITTER_H

#include "zms/zms_export.h"
#include "ztk/ztk_errno.h"
#include "rtsp_parser.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*zms_rtsp_on_message_cb)(const zms_rtsp_message *msg, void *user);
typedef void (*zms_rtsp_on_rtp_cb)(uint8_t channel, const uint8_t *data, size_t len, void *user);

typedef struct zms_rtsp_splitter zms_rtsp_splitter;

typedef struct zms_rtsp_splitter_opts {
    zms_rtsp_on_message_cb on_message;
    zms_rtsp_on_rtp_cb on_rtp;
    void *user;
} zms_rtsp_splitter_opts;

ZMS_API zms_rtsp_splitter *zms_rtsp_splitter_create(const zms_rtsp_splitter_opts *opts);
ZMS_API void zms_rtsp_splitter_destroy(zms_rtsp_splitter *s);
ZMS_API void zms_rtsp_splitter_enable_rtp(zms_rtsp_splitter *s, int on);
ZMS_API ztk_err_t zms_rtsp_splitter_input(zms_rtsp_splitter *s, const void *data, size_t len);
ZMS_API void zms_rtsp_splitter_reset(zms_rtsp_splitter *s);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_SESSION_RTSP_SPLITTER_H */
