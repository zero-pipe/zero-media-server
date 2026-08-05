#ifndef ZMS_CONTAINER_FLV_LIVE_MUXER_H
#define ZMS_CONTAINER_FLV_LIVE_MUXER_H

struct zms_media_source;
#include "zms/zms_export.h"
#include "ztk/ztk_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zms_flv_live_muxer zms_flv_live_muxer;

/** HTTP-FLV 直播：play 经 protocol_dispatch，媒体帧经 egress_pipeline */
ZMS_API zms_flv_live_muxer *zms_flv_live_muxer_create(struct zms_media_source *src);
ZMS_API void zms_flv_live_muxer_bind_source(zms_flv_live_muxer *m, struct zms_media_source *src);
struct ztk_poller;
ZMS_API void zms_flv_live_muxer_bind_poller(zms_flv_live_muxer *m, struct ztk_poller *pol);
ZMS_API void zms_flv_live_muxer_destroy(zms_flv_live_muxer *m);
ZMS_API ztk_err_t zms_flv_live_muxer_start(zms_flv_live_muxer *m, int has_audio, int has_video,
                                           uint8_t *out, size_t cap, size_t *out_len);
ZMS_API int zms_flv_live_muxer_next(zms_flv_live_muxer *m, uint8_t *out, size_t cap,
                                    size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_CONTAINER_FLV_LIVE_MUXER_H */
