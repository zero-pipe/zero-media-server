#ifndef ZMS_LIVE_INGEST_COMMON_PROTOCOL_OPTS_H
#define ZMS_LIVE_INGEST_COMMON_PROTOCOL_OPTS_H

#include "zms/zms_export.h"
#include "zms/ops/service/config.h" /* zms_protocol_config */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 直播推流协议选项，与 config.ini [protocol] 节字段一一对应。
 * zms_protocol_opts 为 zms_protocol_config 的别名，内存布局相同。
 */
typedef zms_protocol_config zms_protocol_opts;

/** [protocol] 默认值初始化 opts（enable_rtmp/rtsp/audio=1，其余为 0） */
ZMS_API void zms_protocol_opts_default(zms_protocol_opts *opts);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_LIVE_INGEST_COMMON_PROTOCOL_OPTS_H */
