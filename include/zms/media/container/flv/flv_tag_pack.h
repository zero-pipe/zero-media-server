#ifndef ZMS_CONTAINER_FLV_TAG_PACK_H
#define ZMS_CONTAINER_FLV_TAG_PACK_H

/**
 * @file flv_tag_pack.h
 * @brief 将 ES 环形槽位打包为 FLV/RTMP tag body。
 */
#include "zms/engine/gop/gop_queue.h"
#include "zms/zms_export.h"
#include "ztk/ztk_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zms_flv_tag_pack_req {
    const zms_gop_slot *slot;
    uint32_t pts_ms;
    /** 可选 video config（raw AVCC/hvcC / FLV seq header），供 VCL 打包。 */
    const uint8_t *video_cfg;
    size_t video_cfg_len;
    uint8_t *buf;
    size_t cap;
} zms_flv_tag_pack_req;

typedef struct zms_flv_tag_pack_out {
    size_t tag_len;
    uint8_t rtmp_msg_type;
} zms_flv_tag_pack_out;

/** 将一条 ES ring 槽位编码为 FLV/RTMP tag body。 */
ZMS_API ztk_err_t zms_flv_tag_pack(const zms_flv_tag_pack_req *req, zms_flv_tag_pack_out *out);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_CONTAINER_FLV_TAG_PACK_H */
