#ifndef ZMS_SRC_SESSION_SRT_POLLER_H
#define ZMS_SRC_SESSION_SRT_POLLER_H

#include "srt_service_internal.h"

struct zms_srt_service;

void zms_srt_poller_init(struct zms_srt_service *srv);
void zms_srt_poller_fini(struct zms_srt_service *srv);

zms_srt_session *zms_srt_session_find(struct zms_srt_service *srv, SRTSOCKET sock);
void zms_srt_session_link(struct zms_srt_service *srv, zms_srt_session *sess);
void zms_srt_session_unlink(struct zms_srt_service *srv, zms_srt_session *sess);

zms_srt_session *zms_srt_session_accept(struct zms_srt_service *srv, SRTSOCKET client,
                                        const char *streamid);

#endif /* ZMS_SRC_SESSION_SRT_POLLER_H */
