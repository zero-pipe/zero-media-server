#ifndef ZMS_SDK_H
#define ZMS_SDK_H

/**
 * @file sdk.h
 * @brief ZMS Stable SDK 伞头（嵌入 / 插件宿主推荐唯一入口）。
 *
 * 只聚合稳定承诺的符号：版本、导出宏、服务器门面与 hooks/配置。
 * 不要在此头中继续 #include 协议/GOP/egress 等 Advanced 细节。
 *
 * 分层说明：docs/api-tiers.md
 * 嵌入指南：docs/integrator-guide.md
 */
#include "zms/version.h"
#include "zms/zms_export.h"
#include "zms/server.h"

#endif /* ZMS_SDK_H */
