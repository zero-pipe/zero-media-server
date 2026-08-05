#ifndef ZMS_EGRESS_MPEGTS_EGRESS_H
#define ZMS_EGRESS_MPEGTS_EGRESS_H

/**
 * @file mpegts_egress.h
 * @brief 直播 MPEG-TS 出站（连续 188 字节 TS，供 SRT/HTTP-TS）。
 */
struct zms_media_source;
struct zms_egress_source;
#include "zms/zms_export.h"
#include "ztk/ztk_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zms_mpegts_egress zms_mpegts_egress;

/** 创建绑定媒体源 + egress_source 读者的直播 TS 出站。 */
ZMS_API zms_mpegts_egress *zms_mpegts_egress_create(struct zms_media_source *src,
                                                    struct zms_egress_source *play);
ZMS_API void zms_mpegts_egress_destroy(zms_mpegts_egress *m);

/**
 * 从 ring mux 并复制发送队列最多 @a cap 字节（优先 1316）。
 * @return 写入字节数，无数据/错误则 <=0
 */
ZMS_API int zms_mpegts_egress_next(zms_mpegts_egress *m, uint8_t *out, size_t cap, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_EGRESS_MPEGTS_EGRESS_H */
