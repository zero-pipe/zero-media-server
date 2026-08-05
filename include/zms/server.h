#ifndef ZMS_SERVER_H
#define ZMS_SERVER_H

/**
 * @file server.h
 * @brief ZMS 媒体服务器门面（Stable SDK 主入口之一）。
 *
 * 嵌入推荐：#include "zms/sdk.h"（本头由 sdk.h 包含）。
 * 分层：docs/api-tiers.md · 指南：docs/integrator-guide.md
 *
 * 典型用法：
 *   zms_server *s = zms_server_create_from_ini("config.ini");
 *   zms_server_set_hooks(s, &hooks);
 *   zms_server_start(s);
 *   zms_server_run(s, -1);   // 阻塞直到 Ctrl+C / request_stop
 *   zms_server_stop(s);
 *   zms_server_destroy(s);
 *
 * 业务侧优先只依赖 sdk.h / 本头；协议与 GOP 等头属 Advanced/Internal。
 */
#include "zms/engine/media_event.h"
#include "zms/ops/service/config.h"
#include "zms/zms_export.h"
#include "ztk/ztk_errno.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zms_server zms_server;

/** 与 zms_media_events 对齐的生命周期回调（可只填需要的字段）。 */
typedef zms_media_events zms_server_hooks;

/**
 * 从 INI 创建服务器实例（未 start）。
 * @param config_path  可为 NULL（使用内置默认配置）
 * @param log_override 非空时覆盖 [general] log_file
 */
ZMS_API zms_server *zms_server_create_from_ini(const char *config_path, const char *log_override);

/** 使用已填充的 zms_config 创建（调用方保留 cfg 所有权；内部会拷贝一份）。 */
ZMS_API zms_server *zms_server_create(const zms_config *cfg);

ZMS_API void zms_server_destroy(zms_server *s);

/** 在 start 前设置；传 NULL 清除。返回 0 成功。 */
ZMS_API int zms_server_set_hooks(zms_server *s, const zms_server_hooks *hooks);

/** 启动监听（RTMP/HTTP/RTSP/SRT 等按配置与编译选项）。 */
ZMS_API ztk_err_t zms_server_start(zms_server *s);

/** 停止协议服务（可再次 start 前需 destroy 重建；当前实现为一次性生命周期）。 */
ZMS_API void zms_server_stop(zms_server *s);

/**
 * 阻塞运行直到 zms_server_request_stop() 或默认信号（若已 install）。
 * @param seconds  <0 一直跑；>=0 最多跑 seconds 秒（仍可被 stop 打断）
 */
ZMS_API void zms_server_run(zms_server *s, int seconds);

/** 请求 run() 返回（线程安全：仅置标志）。 */
ZMS_API void zms_server_request_stop(zms_server *s);

/** 安装 Ctrl+C / SIGTERM 等，内部调用 request_stop。可在 create 后、run 前调用。 */
ZMS_API void zms_server_install_default_signals(zms_server *s);

/** 只读配置（start 前后均可；勿长期保存指针跨 destroy）。 */
ZMS_API const zms_config *zms_server_config(const zms_server *s);

/** 是否已 start。 */
ZMS_API int zms_server_is_running(const zms_server *s);

/**
 * 向 stdout 打印推流/播放 URL 提示（demo / 联调用）。
 * start 成功后调用。
 */
ZMS_API void zms_server_print_endpoints(const zms_server *s);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_SERVER_H */
