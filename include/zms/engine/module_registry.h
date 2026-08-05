#ifndef ZMS_ENGINE_MODULE_REGISTRY_H
#define ZMS_ENGINE_MODULE_REGISTRY_H

/**
 * @file module_registry.h
 * @brief 内置 ZMS 模块注册表启动引导。
 *
 * ZMS 通过一组内建注册表粘合 ZTK 网络与 ZMK 媒体容器/编解码。
 * 进程启动时、服务协议会话或创建代理客户端之前调用一次。
 */
#include "zms/zms_export.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 注册全部内建 codec、payload、container、format 与分片模块。 */
ZMS_API void zms_modules_register_all(void);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_ENGINE_MODULE_REGISTRY_H */
