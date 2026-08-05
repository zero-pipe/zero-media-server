#ifndef ZMS_API_JSON_MEDIA_JSON_H
#define ZMS_API_JSON_MEDIA_JSON_H

#include "zms/engine/stream/stream_hub.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zms_json_buf {
    char *buf;
    size_t cap;
    size_t len;
} zms_json_buf;

int zms_json_buf_append(zms_json_buf *jb, const char *fmt, ...);
int zms_media_source_is_online(const zms_media_source *s);
/** 列表 JSON（不含外层数组）；ports 可为 NULL（使用 zms_media_events_server_ports） */
int zms_json_append_media_item(zms_json_buf *jb, zms_media_source *s, int *first,
                               const zms_media_server_ports *ports);
/** getMediaInfo：{"code":0, ...fields} */
int zms_json_write_media_info(zms_json_buf *jb, zms_media_source *s,
                              const zms_media_server_ports *ports);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_API_JSON_MEDIA_JSON_H */
