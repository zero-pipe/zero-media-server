#ifndef ZMS_SESSION_RTMP_SESSION_H
#define ZMS_SESSION_RTMP_SESSION_H

#include "zms/zms_export.h"
#include "ztk/ztk_errno.h"
#include "ztk/net/tcp_server.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zms_rtmp_session zms_rtmp_session;

typedef struct zms_rtmp_session_opts {
    ztk_tcp_session *tcp;
} zms_rtmp_session_opts;

ZMS_API zms_rtmp_session *zms_rtmp_session_create(const zms_rtmp_session_opts *opts);
ZMS_API void zms_rtmp_session_destroy(zms_rtmp_session *s);
ZMS_API void zms_rtmp_session_on_recv(zms_rtmp_session *s, const void *data, size_t len);
ZMS_API void zms_rtmp_session_on_error(zms_rtmp_session *s);
ZMS_API void zms_rtmp_session_schedule_destroy(zms_rtmp_session *s, ztk_tcp_session *session);
void zms_rtmp_session_on_manager(zms_rtmp_session *s);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_SESSION_RTMP_SESSION_H */
