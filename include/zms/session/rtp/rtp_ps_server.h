#ifndef ZMS_SESSION_RTP_PS_SERVER_H
#define ZMS_SESSION_RTP_PS_SERVER_H

/**
 * @file rtp_ps_server.h
 * @brief GB28181 媒体面：UDP/TCP RTP/PS 接入槽（信令在 Go 侧）。
 */
#include "zms/engine/stream/stream_hub.h"
#include "zms/zms_export.h"
#include "ztk/ztk_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ztk_poller;
struct ztk_poller_pool;
typedef struct ztk_poller ztk_poller;
typedef struct ztk_poller_pool ztk_poller_pool;

#define ZMS_RTP_PS_DEFAULT_PT 96

/** 对齐 ZLM openRtpServer tcp_mode：0=UDP，1=TCP被动(平台listen)，2=TCP主动(平台connect) */
#define ZMS_RTP_PS_TCP_UDP 0
#define ZMS_RTP_PS_TCP_PASSIVE 1
#define ZMS_RTP_PS_TCP_ACTIVE 2

typedef struct zms_rtp_ps_server_slot zms_rtp_ps_server_slot;

typedef struct zms_rtp_ps_server_open_opts {
    ztk_poller *poller;
    ztk_poller_pool *poller_pool;
    const char *vhost;
    const char *app;
    const char *stream;
    /** 0 = kernel picks ephemeral port */
    uint16_t port;
    int payload_type;
    /** 0/1/2 ZMS_RTP_PS_TCP_* */
    int tcp_mode;
/** 非零时仅接受此 SSRC 的 RTP */
    uint32_t ssrc;
    int enable_ssrc_filter;
} zms_rtp_ps_server_open_opts;

ZMS_API void zms_rtp_ps_server_make_key(const char *vhost, const char *app, const char *stream,
                                        char *key, size_t key_cap);

ZMS_API ztk_err_t zms_rtp_ps_server_open(const zms_rtp_ps_server_open_opts *opts,
                                         zms_rtp_ps_server_slot **out_slot, uint16_t *out_port);

/** TCP-ACTIVE：open OK 后平台主动连接摄像头 media 地址（对齐 ZLM connectRtpServer） */
ZMS_API ztk_err_t zms_rtp_ps_server_connect(ztk_poller *poller, const char *vhost, const char *app,
                                            const char *stream, const char *host, uint16_t port);

ZMS_API void zms_rtp_ps_server_close(zms_rtp_ps_server_slot *slot);

ZMS_API zms_rtp_ps_server_slot *zms_rtp_ps_server_find_by_key(const char *key);

ZMS_API zms_media_source *zms_rtp_ps_server_source(const zms_rtp_ps_server_slot *slot);

ZMS_API uint16_t zms_rtp_ps_server_port(const zms_rtp_ps_server_slot *slot);

ZMS_API const char *zms_rtp_ps_server_app(const zms_rtp_ps_server_slot *slot);
ZMS_API const char *zms_rtp_ps_server_stream(const zms_rtp_ps_server_slot *slot);

ZMS_API int zms_rtp_ps_server_payload_type(const zms_rtp_ps_server_slot *slot);

typedef int (*zms_rtp_ps_server_visit_fn)(const char *key, zms_rtp_ps_server_slot *slot,
                                          void *user);
ZMS_API int zms_rtp_ps_server_foreach(zms_rtp_ps_server_visit_fn fn, void *user);

void zms_rtp_ps_register(void);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_SESSION_RTP_PS_SERVER_H */
