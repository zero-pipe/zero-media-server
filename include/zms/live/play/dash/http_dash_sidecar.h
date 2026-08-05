#ifndef ZMS_LIVE_EGRESS_DASH_SIDECAR_H
#define ZMS_LIVE_EGRESS_DASH_SIDECAR_H

#include "zms/engine/stream/stream_hub.h"
#include "zms/zms_export.h"
#include "ztk/poller/poller.h"

#ifdef __cplusplus
extern "C" {
#endif

ZMS_API void zms_http_dash_ensure_recorder(zms_media_source *src, ztk_poller *poller);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_LIVE_EGRESS_DASH_SIDECAR_H */
