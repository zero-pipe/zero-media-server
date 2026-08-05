#ifndef ZMS_CONTAINER_FLV_TAG_FRAMER_H
#define ZMS_CONTAINER_FLV_TAG_FRAMER_H

/**
 * FLV 字节流 → 完整 tag 拆包（framing），不解析 ES。
 */
#include "zms/zms_export.h"
#include "ztk/ztk_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zms_flv_tag_framer zms_flv_tag_framer;

typedef void (*zms_flv_on_tag)(uint8_t type, const uint8_t *body, size_t len, uint32_t tag_dts_ms,
                               void *user);

ZMS_API zms_flv_tag_framer *zms_flv_tag_framer_create(void);
ZMS_API void zms_flv_tag_framer_destroy(zms_flv_tag_framer *s);
ZMS_API void zms_flv_tag_framer_reset(zms_flv_tag_framer *s);
ZMS_API ztk_err_t zms_flv_tag_framer_feed(zms_flv_tag_framer *s, const uint8_t *data, size_t len,
                                          zms_flv_on_tag on_tag, void *user);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_CONTAINER_FLV_TAG_FRAMER_H */
