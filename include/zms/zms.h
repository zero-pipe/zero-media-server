#ifndef ZMS_H
#define ZMS_H

/**
 * @file zms.h
 * @brief Zero Media Server 全量伞头（demo / 内部方便用）。
 *
 * 嵌入与插件宿主请用 zms/sdk.h（Stable），不要依赖本头的「全家桶」包含。
 * Advanced 场景可优先使用领域伞头：zms/live.h、zms/vod.h、zms/player.h、zms/session.h。
 *
 * 分层：docs/api-tiers.md
 *
 * Copyright (c) zero-media-server
 */
#include "zms/version.h"
#include "zms/zms_export.h"
#include "zms/server.h"
#include "zms/engine/frame.h"
#include "zms/engine/module_registry.h"
#include "zms/engine/stream/stream_hub.h"
#include "zms/engine/media_event.h"
#include "zms/session.h"
#include "zms/live.h"
#include "zms/vod.h"
#include "zms/player.h"
#include "zms/ops/service/config.h"
#include "zms/ops/api/webapi/webapi.h"

#endif /* ZMS_H */
