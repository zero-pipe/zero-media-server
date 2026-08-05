#ifndef ZMS_CONTAINER_FLV_WIRE_H
#define ZMS_CONTAINER_FLV_WIRE_H

/**
 * @file flv_wire.h
 * @brief FLV 线格式辅助：文件头与完整 tag（libflv）。
 */
#include "zms/zms_export.h"
#include "ztk/ztk_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 写 FLV 文件头 + PreviousTagSize0。 */
ZMS_API ztk_err_t zms_flv_write_header(uint8_t *out, size_t cap, size_t *out_len, int has_audio,
                                       int has_video);

/** 写 11 字节 tag 头 + body + PreviousTagSizeN（type_id：8/9/18）。 */
ZMS_API ztk_err_t zms_flv_write_tag(uint8_t *out, size_t cap, size_t *out_len, uint8_t type_id,
                                    uint32_t tag_dts_ms, const void *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_CONTAINER_FLV_WIRE_H */
