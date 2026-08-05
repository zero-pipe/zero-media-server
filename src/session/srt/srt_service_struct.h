#ifndef ZMS_SRC_SESSION_SRT_SERVICE_STRUCT_H
#define ZMS_SRC_SESSION_SRT_SERVICE_STRUCT_H

#include "ztk/poller/poller.h"
#include <stdint.h>

#ifdef ZMS_HAVE_SRT
#include <srt/srt.h>
#else
typedef int SRTSOCKET;
#endif

typedef struct ztk_timer ztk_timer;

typedef struct zms_srt_session zms_srt_session;

typedef struct zms_srt_service {
    ztk_poller *poller;
    ztk_timer *io_timer;
    int epoll_id;
    SRTSOCKET listen_sock;
    volatile int running;
    uint16_t port;
    int session_serial;
    zms_srt_session *sessions;
} zms_srt_service;

#endif /* ZMS_SRC_SESSION_SRT_SERVICE_STRUCT_H */
