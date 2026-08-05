#include "zms/ops/service/config.h"
#include "zms/egress/egress_pacing.h"
#include "zms/engine/gop/gop_limits.h"
#include "ztk/util/log.h"
#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── 字符串辅助 ───────────────────────────────────────────────────────── */

static void trim(char *s)
{
    if (!s) {
        return;
    }
    char *p = s;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
        ++p;
    }
    if (p != s) {
        memmove(s, p, strlen(p) + 1);
    }
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) {
        s[--n] = '\0';
    }
}

static int parse_bool(const char *v)
{
    if (!v || !v[0]) {
        return 0;
    }
    if (v[0] == '0' || v[0] == 'f' || v[0] == 'F' || v[0] == 'n' || v[0] == 'N') {
        return 0;
    }
    return 1;
}

static int parse_log_level(const char *v)
{
    if (!v) {
        return ZTK_LOG_INFO;
    }
    if (strcmp(v, "trace") == 0 || strcmp(v, "0") == 0) {
        return ZTK_LOG_TRACE;
    }
    if (strcmp(v, "debug") == 0 || strcmp(v, "1") == 0) {
        return ZTK_LOG_DEBUG;
    }
    if (strcmp(v, "warn") == 0 || strcmp(v, "2") == 0) {
        return ZTK_LOG_WARN;
    }
    if (strcmp(v, "error") == 0 || strcmp(v, "3") == 0) {
        return ZTK_LOG_ERROR;
    }
    return ZTK_LOG_INFO;
}

/* ── 表驱动字段描述 ───────────────────────────────────────────────────────── */

typedef enum {
    ZMS_CFG_F_STR,   /* strncpy */
    ZMS_CFG_F_INT,   /* atoi */
    ZMS_CFG_F_UINT,  /* (unsigned)atoi */
    ZMS_CFG_F_FLOAT, /* (float)atof */
    ZMS_CFG_F_BOOL,  /* parse_bool */
    ZMS_CFG_F_CUSTOM /* 自定义解析函数 */
} zms_cfg_ftype;

typedef struct zms_cfg_field {
    const char *key;
    const char *alias; /* 旧驼峰别名，NULL 表示无别名 */
    zms_cfg_ftype type;
    size_t offset;   /* offsetof(zms_config, section.field) */
    size_t str_size; /* type==STR 时为字符串缓冲区大小 */
    void (*custom_fn)(zms_config *, const char *);
} zms_cfg_field;

/* 写入辅助 */
static void field_apply(zms_config *cfg, const zms_cfg_field *f, const char *val)
{
    void *ptr = (char *)cfg + f->offset;
    switch (f->type) {
    case ZMS_CFG_F_STR:
        strncpy((char *)ptr, val, f->str_size - 1);
        ((char *)ptr)[f->str_size - 1] = '\0';
        trim((char *)ptr);
        break;
    case ZMS_CFG_F_INT:
        *(int *)ptr = atoi(val);
        break;
    case ZMS_CFG_F_UINT:
        *(unsigned *)ptr = (unsigned)atoi(val);
        break;
    case ZMS_CFG_F_FLOAT:
        *(float *)ptr = (float)atof(val);
        break;
    case ZMS_CFG_F_BOOL:
        *(int *)ptr = parse_bool(val);
        break;
    case ZMS_CFG_F_CUSTOM:
        if (f->custom_fn) {
            f->custom_fn(cfg, val);
        }
        break;
    }
}

/* CUSTOM 解析函数 */
static void parse_log_level_fn(zms_config *cfg, const char *val)
{
    cfg->general.log_level = parse_log_level(val);
}

static void parse_buf_pool_mode_fn(zms_config *cfg, const char *val)
{
    if (strcmp(val, "global") == 0) {
        cfg->general.buf_pool_mode = ZMS_BUF_POOL_MODE_GLOBAL;
    } else if (strcmp(val, "per_poller") == 0 || strcmp(val, "perPoller") == 0) {
        cfg->general.buf_pool_mode = ZMS_BUF_POOL_MODE_PER_POLLER;
    } else {
        cfg->general.buf_pool_mode = ZMS_BUF_POOL_MODE_HYBRID;
    }
}

/* ── section 字段表 ────────────────────────────────────────────────────── */

#define OFF(section, field) offsetof(zms_config, section.field)
#define STSZ(section, field) sizeof(((zms_config *)0)->section.field)

/* [general] */
static const zms_cfg_field k_general[] = {
    {"log_file", "logFile", ZMS_CFG_F_STR, OFF(general, log_file), STSZ(general, log_file), NULL},
    {"log_level", "logLevel", ZMS_CFG_F_CUSTOM, 0, 0, parse_log_level_fn},
    {"log_max_mb", "logMaxMB", ZMS_CFG_F_UINT, OFF(general, log_max_mb), 0, NULL},
    {"log_keep_count", "logKeepCount", ZMS_CFG_F_INT, OFF(general, log_keep_count), 0, NULL},
    {"poller_threads", "pollerThreads", ZMS_CFG_F_UINT, OFF(general, poller_threads), 0, NULL},
    {"stream_none_reader_delay_ms", "streamNoneReaderDelayMS", ZMS_CFG_F_INT,
     OFF(general, stream_none_reader_delay_ms), 0, NULL},
    {"buf_pool_enable", "bufPoolEnable", ZMS_CFG_F_BOOL, OFF(general, buf_pool_enable), 0, NULL},
    {"buf_pool_mode", "bufPoolMode", ZMS_CFG_F_CUSTOM, 0, 0, parse_buf_pool_mode_fn},
    {"buf_pool_max", "bufPoolMaxPerBucket", ZMS_CFG_F_UINT, OFF(general, buf_pool_max_per_bucket),
     0, NULL},
    {"gop_target_gops", "gopTargetGops", ZMS_CFG_F_UINT, OFF(general, gop_target_gops), 0, NULL},
    {"gop_cache_sec", "gopCacheSec", ZMS_CFG_F_UINT, OFF(general, gop_cache_sec), 0, NULL},
    {"vod_work_threads", "vodWorkThreads", ZMS_CFG_F_UINT, OFF(general, vod_work_threads), 0, NULL},
    {"extern_ip", "externIP", ZMS_CFG_F_STR, OFF(general, extern_ip), STSZ(general, extern_ip),
     NULL},
    {"listen_host", "listenHost", ZMS_CFG_F_STR, OFF(general, listen_host),
     STSZ(general, listen_host), NULL},
    /* media_server_id 属于 hook 但习惯放 general section */
    {"media_server_id", "mediaServerId", ZMS_CFG_F_STR, OFF(hook, media_server_id),
     STSZ(hook, media_server_id), NULL},
    {NULL, NULL, ZMS_CFG_F_STR, 0, 0, NULL}};

/* [rtmp] */
static const zms_cfg_field k_rtmp[] = {{"port", NULL, ZMS_CFG_F_UINT, OFF(rtmp, port), 0, NULL},
                                       {NULL, NULL, ZMS_CFG_F_STR, 0, 0, NULL}};

/* [rtsp] */
static const zms_cfg_field k_rtsp[] = {
    {"port", NULL, ZMS_CFG_F_UINT, OFF(rtsp, port), 0, NULL},
    {"auth_user", NULL, ZMS_CFG_F_STR, OFF(rtsp, auth_user), STSZ(rtsp, auth_user), NULL},
    {"auth_pass", NULL, ZMS_CFG_F_STR, OFF(rtsp, auth_pass), STSZ(rtsp, auth_pass), NULL},
    {"test_reject_tcp_setup", NULL, ZMS_CFG_F_BOOL, OFF(rtsp, test_reject_tcp_setup), 0, NULL},
    {NULL, NULL, ZMS_CFG_F_STR, 0, 0, NULL}};

/* [http] */
static const zms_cfg_field k_http[] = {{"port", NULL, ZMS_CFG_F_UINT, OFF(http, port), 0, NULL},
                                       {NULL, NULL, ZMS_CFG_F_STR, 0, 0, NULL}};

/* [api] api.secret 逻辑上属于 http */
static const zms_cfg_field k_api[] = {
    {"secret", NULL, ZMS_CFG_F_STR, OFF(http, api_secret), STSZ(http, api_secret), NULL},
    {NULL, NULL, ZMS_CFG_F_STR, 0, 0, NULL}};

/* [srt] */
static const zms_cfg_field k_srt[] = {{"port", NULL, ZMS_CFG_F_UINT, OFF(srt, port), 0, NULL},
                                      {NULL, NULL, ZMS_CFG_F_STR, 0, 0, NULL}};

/* [webrtc] */
static const zms_cfg_field k_webrtc[] = {
    {"enable", NULL, ZMS_CFG_F_BOOL, OFF(webrtc, enable), 0, NULL},
    {"port_min", NULL, ZMS_CFG_F_UINT, OFF(webrtc, port_min), 0, NULL},
    {"port_max", NULL, ZMS_CFG_F_UINT, OFF(webrtc, port_max), 0, NULL},
    /* advertise_host / externIP 覆盖 general.extern_ip（保持旧行为） */
    {"advertise_host", "externIP", ZMS_CFG_F_STR, OFF(general, extern_ip), STSZ(general, extern_ip),
     NULL},
    {NULL, NULL, ZMS_CFG_F_STR, 0, 0, NULL}};

/* [protocol] */
static const zms_cfg_field k_protocol[] = {
    {"enable_rtmp", NULL, ZMS_CFG_F_BOOL, OFF(protocol, enable_rtmp), 0, NULL},
    {"enable_rtsp", NULL, ZMS_CFG_F_BOOL, OFF(protocol, enable_rtsp), 0, NULL},
    {"enable_srt", NULL, ZMS_CFG_F_BOOL, OFF(protocol, enable_srt), 0, NULL},
    {"enable_hls", NULL, ZMS_CFG_F_BOOL, OFF(protocol, enable_hls), 0, NULL},
    {"enable_audio", NULL, ZMS_CFG_F_BOOL, OFF(protocol, enable_audio), 0, NULL},
    {"modify_stamp", "modifyStamp", ZMS_CFG_F_INT, OFF(protocol, modify_stamp), 0, NULL},
    {NULL, NULL, ZMS_CFG_F_STR, 0, 0, NULL}};

/* [hls] */
static const zms_cfg_field k_hls[] = {
    {"seg_dur", "segDur", ZMS_CFG_F_FLOAT, OFF(hls, segment_duration_sec), 0, NULL},
    {"seg_num", "segNum", ZMS_CFG_F_UINT, OFF(hls, segment_count), 0, NULL},
    {NULL, NULL, ZMS_CFG_F_STR, 0, 0, NULL}};

/* [proxy] */
static const zms_cfg_field k_proxy[] = {
    {"proxy_pull", "pull_url", ZMS_CFG_F_STR, OFF(proxy, pull), STSZ(proxy, pull), NULL},
    {"proxy_app", "app", ZMS_CFG_F_STR, OFF(proxy, app), STSZ(proxy, app), NULL},
    {"proxy_stream", "stream", ZMS_CFG_F_STR, OFF(proxy, stream), STSZ(proxy, stream), NULL},
    {"proxy_prefix", "prefix", ZMS_CFG_F_STR, OFF(proxy, prefix), STSZ(proxy, prefix), NULL},
    {"retry_count", NULL, ZMS_CFG_F_INT, OFF(proxy, retry_count), 0, NULL},
    {"reconnect_delay_ms", NULL, ZMS_CFG_F_INT, OFF(proxy, reconnect_delay_ms), 0, NULL},
    {NULL, NULL, ZMS_CFG_F_STR, 0, 0, NULL}};

/* [record] */
static const zms_cfg_field k_record[] = {
    {"app", "appName", ZMS_CFG_F_STR, OFF(record, app), STSZ(record, app), NULL},
    {"root", "mp4_save_path", ZMS_CFG_F_STR, OFF(record, root), STSZ(record, root), NULL},
    {"file_repeat", "fileRepeat", ZMS_CFG_F_BOOL, OFF(record, file_repeat), 0, NULL},
    {"mp4_max_second", "mp4MaxSecond", ZMS_CFG_F_INT, OFF(record, mp4_max_second), 0, NULL},
    {NULL, NULL, ZMS_CFG_F_STR, 0, 0, NULL}};

/* [play] 所有字段属于 play.pacing */
#define POFF(field) offsetof(zms_config, play.pacing.field)
static const zms_cfg_field k_play[] = {
    {"ring_max_lag", "ringMaxLag", ZMS_CFG_F_UINT, POFF(ring_max_lag), 0, NULL},
    {"catchup_lag", "catchupLag", ZMS_CFG_F_UINT, POFF(catchup_lag), 0, NULL},
    {"frame_budget_live", "frameBudgetLive", ZMS_CFG_F_UINT, POFF(frame_budget_live), 0, NULL},
    {"frame_budget_catchup", "frameBudgetCatchup", ZMS_CFG_F_UINT, POFF(frame_budget_catchup), 0,
     NULL},
    {"burst_budget", "burstBudget", ZMS_CFG_F_UINT, POFF(burst_budget), 0, NULL},
    {"flv_budget_live", "flvBudgetLive", ZMS_CFG_F_UINT, POFF(flv_budget_live), 0, NULL},
    {"flv_budget_vod", "flvBudgetVod", ZMS_CFG_F_UINT, POFF(flv_budget_vod), 0, NULL},
    {"pace_lead_ms", "paceLeadMs", ZMS_CFG_F_UINT, POFF(pace_lead_ms), 0, NULL},
    {"resync_lag", "resyncLag", ZMS_CFG_F_UINT, POFF(resync_lag), 0, NULL},
    {"resync_lag_max", "resyncLagMax", ZMS_CFG_F_UINT, POFF(resync_lag_max), 0, NULL},
    {"resync_cooldown_ms", "resyncCooldownMs", ZMS_CFG_F_UINT, POFF(resync_cooldown_ms), 0, NULL},
    {"slow_consumer_kick_lag", "slowConsumerKickLag", ZMS_CFG_F_UINT, POFF(slow_consumer_kick_lag),
     0, NULL},
    {"vod_catchup_start", "vodCatchupStart", ZMS_CFG_F_UINT, POFF(vod_catchup_start), 0, NULL},
    {"vod_catchup_seek", "vodCatchupSeek", ZMS_CFG_F_UINT, POFF(vod_catchup_seek), 0, NULL},
    {"vod_prefill_lag", "vodPrefillLag", ZMS_CFG_F_UINT, POFF(vod_prefill_lag), 0, NULL},
    {NULL, NULL, ZMS_CFG_F_STR, 0, 0, NULL}};
#undef POFF

/* [hook] */
#define HOFF(field) offsetof(zms_config, hook.field)
#define HSSZ(field) sizeof(((zms_config *)0)->hook.field)
static const zms_cfg_field k_hook[] = {
    {"enable", NULL, ZMS_CFG_F_BOOL, HOFF(enable), 0, NULL},
    {"timeout_sec", "timeoutSec", ZMS_CFG_F_FLOAT, HOFF(timeout_sec), 0, NULL},
    {"retry", NULL, ZMS_CFG_F_INT, HOFF(retry_count), 0, NULL},
    {"retry_delay", NULL, ZMS_CFG_F_FLOAT, HOFF(retry_delay_sec), 0, NULL},
    {"on_publish", NULL, ZMS_CFG_F_STR, HOFF(on_publish), HSSZ(on_publish), NULL},
    {"on_stream_changed", NULL, ZMS_CFG_F_STR, HOFF(on_stream_changed), HSSZ(on_stream_changed),
     NULL},
    {"on_server_started", NULL, ZMS_CFG_F_STR, HOFF(on_server_started), HSSZ(on_server_started),
     NULL},
    {"on_stream_none_reader", NULL, ZMS_CFG_F_STR, HOFF(on_stream_none_reader),
     HSSZ(on_stream_none_reader), NULL},
    {"on_play", NULL, ZMS_CFG_F_STR, HOFF(on_play), HSSZ(on_play), NULL},
    {"on_play_stop", NULL, ZMS_CFG_F_STR, HOFF(on_play_stop), HSSZ(on_play_stop), NULL},
    {"on_record_mp4", NULL, ZMS_CFG_F_STR, HOFF(on_record_mp4), HSSZ(on_record_mp4), NULL},
    {"stream_changed_schemas", NULL, ZMS_CFG_F_STR, HOFF(stream_changed_schemas),
     HSSZ(stream_changed_schemas), NULL},
    {NULL, NULL, ZMS_CFG_F_STR, 0, 0, NULL}};
#undef HOFF
#undef HSSZ

/* [ssl] */
static const zms_cfg_field k_ssl[] = {
    {"ca_file", "caFile", ZMS_CFG_F_STR, OFF(ssl, ca_file), STSZ(ssl, ca_file), NULL},
    {"verify_peer", "verifyPeer", ZMS_CFG_F_BOOL, OFF(ssl, verify_peer), 0, NULL},
    {"client_cert", NULL, ZMS_CFG_F_STR, OFF(ssl, client_cert), STSZ(ssl, client_cert), NULL},
    {"client_key", NULL, ZMS_CFG_F_STR, OFF(ssl, client_key), STSZ(ssl, client_key), NULL},
    {NULL, NULL, ZMS_CFG_F_STR, 0, 0, NULL}};

/* [format] */
static const zms_cfg_field k_format[] = {{"flv_tag_pack", NULL, ZMS_CFG_F_STR,
                                          OFF(format, flv_tag_pack), STSZ(format, flv_tag_pack),
                                          NULL},
                                         {NULL, NULL, ZMS_CFG_F_STR, 0, 0, NULL}};

#undef OFF
#undef STSZ

/* ── section 字段表映射 ─────────────────────────────────────────────────── */

typedef struct {
    const char *section;
    const zms_cfg_field *fields;
} zms_section_map;

static const zms_section_map k_sections[] = {
    {"general", k_general}, {"rtmp", k_rtmp},   {"rtsp", k_rtsp},     {"http", k_http},
    {"api", k_api},         {"srt", k_srt},     {"webrtc", k_webrtc}, {"protocol", k_protocol},
    {"hls", k_hls},         {"proxy", k_proxy}, {"record", k_record}, {"play", k_play},
    {"hook", k_hook},       {"ssl", k_ssl},     {"format", k_format}, {NULL, NULL}};

/* ── apply_kv：查表写入 ───────────────────────────────────────────────────── */

static void apply_kv(zms_config *cfg, const char *section, const char *key, const char *val)
{
    if (!cfg || !section || !key || !val) {
        return;
    }
    for (const zms_section_map *sm = k_sections; sm->section; ++sm) {
        if (strcmp(sm->section, section) != 0) {
            continue;
        }
        for (const zms_cfg_field *f = sm->fields; f->key; ++f) {
            if (strcmp(f->key, key) == 0 || (f->alias && strcmp(f->alias, key) == 0)) {
                field_apply(cfg, f, val);
                return;
            }
        }
        return; /* section 匹配 key 未找到，不继续搜其他 section */
    }
}

/* ── 公开 API ────────────────────────────────────────────────────────────── */

void zms_config_default(zms_config *cfg)
{
    if (!cfg) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));

    /* [general] */
    strncpy(cfg->general.log_file, "zms_media_server.log", sizeof(cfg->general.log_file) - 1);
    cfg->general.log_level = ZTK_LOG_INFO;
    cfg->general.log_max_mb = 0;
    cfg->general.log_keep_count = 5;
    cfg->general.poller_threads = 2;
    cfg->general.stream_none_reader_delay_ms = 20000;
    strncpy(cfg->general.listen_host, "0.0.0.0", sizeof(cfg->general.listen_host) - 1);
    strncpy(cfg->general.extern_ip, "127.0.0.1", sizeof(cfg->general.extern_ip) - 1);
    cfg->general.buf_pool_enable = 1;
    cfg->general.buf_pool_mode = ZMS_BUF_POOL_MODE_HYBRID;
    cfg->general.buf_pool_max_per_bucket = ZMS_BUF_POOL_MAX_PER_BUCKET_DEFAULT;
    cfg->general.gop_target_gops = ZMS_GOP_QUEUE_TARGET_GOPS;
    cfg->general.gop_cache_sec = 0;
    cfg->general.vod_work_threads = 2;

    /* [rtmp/rtsp/http/srt] */
    cfg->rtmp.port = 1935;
    cfg->rtsp.port = 554;
    cfg->srt.port = 9000;
    cfg->http.port = 8080;

    /* [protocol] */
    cfg->protocol.enable_rtmp = 1;
    cfg->protocol.enable_rtsp = 1;
    cfg->protocol.enable_srt = 1;
    cfg->protocol.enable_hls = 1;
    cfg->protocol.enable_audio = 1;
    cfg->protocol.modify_stamp = 0;

    /* [proxy] */
    strncpy(cfg->proxy.app, "live", sizeof(cfg->proxy.app) - 1);
    cfg->proxy.retry_count = -1;
    cfg->proxy.reconnect_delay_ms = 2000;

    /* [hls] */
    cfg->hls.segment_duration_sec = 2.f;
    cfg->hls.segment_count = 3;

    /* [hook] */
    cfg->hook.enable = 0;
    cfg->hook.timeout_sec = 10.f;
    cfg->hook.retry_count = 1;
    cfg->hook.retry_delay_sec = 3.f;
    strncpy(cfg->hook.stream_changed_schemas, "rtsp/rtmp/srt/rtp-ps/webrtc",
            sizeof(cfg->hook.stream_changed_schemas) - 1);

    /* [record] */
    strncpy(cfg->record.app, "vod", sizeof(cfg->record.app) - 1);
    strncpy(cfg->record.root, "./www/record", sizeof(cfg->record.root) - 1);
    cfg->record.file_repeat = 0;
    cfg->record.mp4_max_second = 180;

    /* [ssl] */
    cfg->ssl.verify_peer = 1;

    /* [webrtc] */
    cfg->webrtc.enable = 1;
    cfg->webrtc.port_min = 50000;
    cfg->webrtc.port_max = 60000;

    /* [format] */
    strncpy(cfg->format.flv_tag_pack, "zms", sizeof(cfg->format.flv_tag_pack) - 1);

    /* [play] */
    zms_egress_pacing_defaults(&cfg->play.pacing);
}

ztk_err_t zms_config_load_ini(zms_config *cfg, const char *path)
{
    if (!cfg || !path) {
        return ZTK_ERR_INVALID;
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        ztk_warn("config: cannot open %s", path);
        return ZTK_ERR_IO;
    }

    char line[1024];
    char section[64] = "general";
    while (fgets(line, sizeof(line), fp)) {
        trim(line);
        if (!line[0] || line[0] == '#' || line[0] == ';') {
            continue;
        }
        if (line[0] == '[') {
            char *end = strchr(line, ']');
            if (!end) {
                continue;
            }
            *end = '\0';
            strncpy(section, line + 1, sizeof(section) - 1);
            section[sizeof(section) - 1] = '\0';
            trim(section);
            continue;
        }
        char *eq = strchr(line, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        trim(key);
        trim(val);
        apply_kv(cfg, section, key, val);
    }
    fclose(fp);
    ztk_info("config loaded: %s (rtmp=%u rtsp=%u http=%u poller=%u)", path, cfg->rtmp.port,
             cfg->rtsp.port, cfg->http.port, cfg->general.poller_threads);
    return ZTK_OK;
}
