#ifndef ZMS_VOD_EGRESS_FLV_MUXER_H
#define ZMS_VOD_EGRESS_FLV_MUXER_H

struct zms_media_source;
struct zms_vod_buffer_reader;
struct zms_vod_flv_index;
struct zms_egress_clock;
#include "zms/vod/vod_flv_index.h"
#include "zms/zms_export.h"
#include "ztk/ztk_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zms_vod_flv_muxer zms_vod_flv_muxer;

typedef struct zms_vod_flv_rtmp_media {
    uint8_t msg_type;
    uint32_t tag_dts_ms;
} zms_vod_flv_rtmp_media;

/** 点播（共享 vod_buffer reader） */
ZMS_API zms_vod_flv_muxer *zms_vod_flv_muxer_create(struct zms_media_source *src);
/** 外部传入 vod_buffer_reader（如 vod_play_lane） */
ZMS_API zms_vod_flv_muxer *zms_vod_flv_muxer_create_reader(struct zms_media_source *src,
                                                           struct zms_vod_buffer_reader *rd);
ZMS_API void zms_vod_flv_muxer_bind_source(zms_vod_flv_muxer *m, struct zms_media_source *src);
struct ztk_poller;
ZMS_API void zms_vod_flv_muxer_bind_poller(zms_vod_flv_muxer *m, struct ztk_poller *pol);
ZMS_API void zms_vod_flv_muxer_destroy(zms_vod_flv_muxer *m);
ZMS_API ztk_err_t zms_vod_flv_muxer_start(zms_vod_flv_muxer *m, int has_audio, int has_video,
                                          uint8_t *out, size_t cap, size_t *out_len);

ZMS_API void zms_vod_flv_muxer_configure(zms_vod_flv_muxer *m, const uint8_t *video_cfg,
                                         size_t video_len, const uint8_t *audio_cfg,
                                         size_t audio_len, uint64_t duration_ms);
ZMS_API void zms_vod_flv_muxer_set_index_view(zms_vod_flv_muxer *m, zms_vod_flv_index_view *view);
ZMS_API void zms_vod_flv_muxer_set_index_full(zms_vod_flv_muxer *m,
                                              const struct zms_vod_flv_index *idx);
ZMS_API void zms_vod_flv_muxer_set_http_realtime_pace(zms_vod_flv_muxer *m, int on);
ZMS_API void zms_vod_flv_muxer_seek(zms_vod_flv_muxer *m, uint32_t ms);
ZMS_API void zms_vod_flv_muxer_skip_bootstrap(zms_vod_flv_muxer *m);
ZMS_API void zms_vod_flv_muxer_bind_play_clock(zms_vod_flv_muxer *m, struct zms_egress_clock *clk);
ZMS_API void zms_vod_flv_muxer_set_catchup(zms_vod_flv_muxer *m, int catchup);
ZMS_API int zms_vod_flv_muxer_video_armed(const zms_vod_flv_muxer *m);
ZMS_API int zms_vod_flv_muxer_catchup(const zms_vod_flv_muxer *m);

ZMS_API int zms_vod_flv_muxer_next_rtmp_media(zms_vod_flv_muxer *m, zms_vod_flv_rtmp_media *meta,
                                              uint8_t *body, size_t body_cap, size_t *body_len);
ZMS_API int zms_vod_flv_muxer_next(zms_vod_flv_muxer *m, uint8_t *out, size_t cap, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_VOD_EGRESS_FLV_MUXER_H */
