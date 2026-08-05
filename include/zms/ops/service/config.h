#ifndef ZMS_SERVICE_CONFIG_H
#define ZMS_SERVICE_CONFIG_H

/**
 * 只读配置加载（INI 子集）。
 * 每个 INI section 对应一个子结构体；zms_config 是所有子结构体的聚合体。
 * 调用方通过 cfg->section.field 访问，如 cfg->rtmp.port、cfg->general.listen_host。
 */
#include "zms/engine/stream/stream_limits.h"
#include "zms/egress/egress_pacing.h"
#include "zms/zms_export.h"
#include "ztk/ztk_errno.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── 尺寸常量 ────────────────────────────────────────────────────────────── */
#define ZMS_CFG_PATH_MAX 512
#define ZMS_CFG_URL_MAX 768
#define ZMS_CFG_HOOK_URL_MAX 512
#define ZMS_CFG_SCHEMA_FILTER_MAX 128

/* ── [general] bufPoolMode 枚举 ──────────────────────────────────────────── */
#define ZMS_BUF_POOL_MAX_PER_BUCKET_DEFAULT 128u
#define ZMS_BUF_POOL_MODE_GLOBAL 0
#define ZMS_BUF_POOL_MODE_PER_POLLER 1
#define ZMS_BUF_POOL_MODE_HYBRID 2

/* ════════════════════════════════════════════════════════════════════════════
 * 子结构体：每个对应一个 INI section
 * ════════════════════════════════════════════════════════════════════════════ */

/** [general] */
typedef struct zms_general_config {
    char log_file[256];
    int log_level;       /**< ZTK_LOG_* */
    unsigned log_max_mb; /**< 0 = 不轮转 */
    int log_keep_count;  /**< 默认 5 */
    unsigned poller_threads;
    int stream_none_reader_delay_ms;
    /** 监听地址，默认 0.0.0.0（所有网卡）；可设为具体 IP 限定单网卡 */
    char listen_host[64];
    /** 对外公布 IP（WebRTC ICE candidate / RTSP SDP），对齐 ZLM externIP */
    char extern_ip[64];
    /** 缓冲池 */
    int buf_pool_enable;
    int buf_pool_mode; /**< ZMS_BUF_POOL_MODE_* */
    unsigned buf_pool_max_per_bucket;
    /**
     * 直播 GOP 缓存目标保留段数（压力下丢旧 GOP 的阈值）。
     * 0 = 使用编译默认 ZMS_GOP_QUEUE_TARGET_GOPS；典型 2–8。
     */
    unsigned gop_target_gops;
    /**
     * 直播 GOP 时间窗（秒）。0=关闭，仅按 gop_target_gops/容量裁剪。
     * 与 target_gops 同时生效：超时或超段数都会丢最旧 GOP（至少保留 1 段）。
     */
    unsigned gop_cache_sec;
    /** 点播磁盘阻塞线程数，0=I/O poller 上同步执行 */
    unsigned vod_work_threads;
} zms_general_config;

/** [rtmp] */
typedef struct zms_rtmp_config {
    unsigned port;
} zms_rtmp_config;

/** [rtsp] */
typedef struct zms_rtsp_config {
    unsigned port;
    char auth_user[64];
    char auth_pass[128];
    int test_reject_tcp_setup;
} zms_rtsp_config;

/** [http] api.secret 归入此处（HTTP 层鉴权） */
typedef struct zms_http_config {
    unsigned port;
    char api_secret[128];
} zms_http_config;

/** [srt] */
typedef struct zms_srt_config {
    unsigned port;
} zms_srt_config;

/** [webrtc] */
typedef struct zms_webrtc_config {
    int enable;
    unsigned port_min;
    unsigned port_max;
} zms_webrtc_config;

/** [protocol] 节；zms_protocol_opts 为其别名（见 protocol_opts.h） */
typedef struct zms_protocol_config {
    int enable_rtmp;
    int enable_rtsp;
    int enable_srt;
    int enable_hls;
    int enable_audio;
    /** 0=源时间戳 1=系统时间 2=相对时间（预留） */
    int modify_stamp;
} zms_protocol_config;

/** [hls] */
typedef struct zms_hls_config {
    float segment_duration_sec;
    unsigned segment_count;
} zms_hls_config;

/** [proxy] */
typedef struct zms_proxy_config {
    char pull[ZMS_CFG_URL_MAX];
    char app[ZMS_APP_MAX];
    /** 空或 auto：按 pull_url 自动生成 proxied/... */
    char stream[ZMS_STREAM_MAX];
    char prefix[64];
    int retry_count; /**< -1=无限 */
    int reconnect_delay_ms;
} zms_proxy_config;

/** [record] */
typedef struct zms_record_config {
    char app[ZMS_APP_MAX];
    char root[ZMS_CFG_PATH_MAX];
    int file_repeat;
    /** 直播 MP4 单文件最长秒数（默认 180）；到期后在关键帧切文件 */
    int mp4_max_second;
} zms_record_config;

/**
 * [play] 包装 zms_egress_pacing，保持 pacing 成员名一致。
 * 调用方可直接 &cfg->play.pacing 传给 zms_egress_pacing_init()。
 */
typedef struct zms_play_config {
    zms_egress_pacing pacing;
} zms_play_config;

/** [hook] */
typedef struct zms_hook_config {
    int enable;
    float timeout_sec;
    int retry_count;
    float retry_delay_sec;
    char media_server_id[64];
    char on_publish[ZMS_CFG_HOOK_URL_MAX];
    char on_stream_changed[ZMS_CFG_HOOK_URL_MAX];
    char on_stream_none_reader[ZMS_CFG_HOOK_URL_MAX];
    char on_play[ZMS_CFG_HOOK_URL_MAX];
    char on_play_stop[ZMS_CFG_HOOK_URL_MAX];
    char on_server_started[ZMS_CFG_HOOK_URL_MAX];
    /** 直播 MP4 切片落盘通知（对齐 ZLM on_record_mp4） */
    char on_record_mp4[ZMS_CFG_HOOK_URL_MAX];
    /** 留空表示不过滤；rtsp/rtmp */
    char stream_changed_schemas[ZMS_CFG_SCHEMA_FILTER_MAX];
} zms_hook_config;

/** [ssl] */
typedef struct zms_ssl_config {
    char ca_file[ZMS_CFG_PATH_MAX];
    int verify_peer;
    char client_cert[ZMS_CFG_PATH_MAX];
    char client_key[ZMS_CFG_PATH_MAX];
} zms_ssl_config;

/** [format] */
typedef struct zms_format_config {
    char flv_tag_pack[16]; /**< "native" | "zms" */
} zms_format_config;

/* ════════════════════════════════════════════════════════════════════════════
 * 顶层聚合：zms_config
 * ════════════════════════════════════════════════════════════════════════════ */
typedef struct zms_config {
    zms_general_config general;
    zms_rtmp_config rtmp;
    zms_rtsp_config rtsp;
    zms_http_config http;
    zms_srt_config srt;
    zms_webrtc_config webrtc;
    zms_protocol_config protocol;
    zms_hls_config hls;
    zms_proxy_config proxy;
    zms_record_config record;
    zms_play_config play;
    zms_hook_config hook;
    zms_ssl_config ssl;
    zms_format_config format;
} zms_config;

/* ── API ─────────────────────────────────────────────────────────────────── */
ZMS_API void zms_config_default(zms_config *cfg);
ZMS_API ztk_err_t zms_config_load_ini(zms_config *cfg, const char *path);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_SERVICE_CONFIG_H */
