/**
 * @file container_dispatcher.h
 * @deprecated 请改用 container_registry.h。
 * 本头仅为向后兼容保留，未来版本将移除。
 */
#include "zms/media/container/container_registry.h"

/* 向后兼容别名：旧名称 → 新名称 */
#define zms_container_dispatch_register_all zms_container_register_all
#define zms_container_demux_register_all zms_container_register_all
