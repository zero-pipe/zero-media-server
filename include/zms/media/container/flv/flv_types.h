#ifndef ZMS_CONTAINER_FLV_TYPES_H
#define ZMS_CONTAINER_FLV_TYPES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum zms_flv_tag_type {
    ZMS_FLV_TAG_AUDIO = 8,
    ZMS_FLV_TAG_VIDEO = 9,
    ZMS_FLV_TAG_SCRIPT = 18,
} zms_flv_tag_type;

typedef struct zms_flv_tag_header {
    uint8_t tag_type;
    uint32_t data_size;
    uint32_t tag_dts_ms;
    uint32_t stream_id;
} zms_flv_tag_header;

typedef struct zms_flv_tag_view {
    zms_flv_tag_header header;
    const uint8_t *body;
    size_t body_size;
} zms_flv_tag_view;

#ifdef __cplusplus
}
#endif

#endif /* ZMS_CONTAINER_FLV_TYPES_H */
