#ifndef ZMS_VOD_EGRESS_PLAY_LANE_H
#define ZMS_VOD_EGRESS_PLAY_LANE_H

/**
 * 单观众点播 lane：独占 vod_reader + vod_buffer，互不干扰。
 * RTSP 每会话一条 lane；媒体源 publisher 仍用共享 vod_buffer 做注册元数据。
 */
#include "zms/engine/stream/stream_hub.h"
#include "zms/vod/io/vod_buffer.h"
#include "zms/vod/io/vod_reader.h"
#include "zms/zms_export.h"
#include "ztk/poller/poller.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zms_vod_play_lane zms_vod_play_lane;

/** @param poller 可为 NULL（仅同步 pump，用于阻塞线程上的 FLV cache） */
ZMS_API zms_vod_play_lane *zms_vod_play_lane_open(const zms_media_source *src, ztk_poller *poller);
ZMS_API void zms_vod_play_lane_close(zms_vod_play_lane *lane);

ZMS_API zms_vod_buffer_reader *zms_vod_play_lane_buffer_reader(zms_vod_play_lane *lane);
ZMS_API zms_vod_reader *zms_vod_play_lane_reader(zms_vod_play_lane *lane);
ZMS_API uint64_t zms_vod_play_lane_seek_ms(zms_vod_play_lane *lane, uint64_t ms);
ZMS_API uint64_t zms_vod_play_lane_prepare(zms_vod_play_lane *lane, uint64_t start_ms);

ZMS_API void zms_vod_play_lane_prefill(zms_vod_play_lane *lane);
/** prefill 后重新对齐 fifo reader 到首个视频关键帧 */
ZMS_API void zms_vod_play_lane_align_reader(zms_vod_play_lane *lane);
ZMS_API void zms_vod_play_lane_set_pump_hold(zms_vod_play_lane *lane, int hold);
/** RTSP/RTMP 出站排空期间持续填充 vod_buffer（不遵守 pump_hold）。 */
ZMS_API void zms_vod_play_lane_demux_fill(zms_vod_play_lane *lane, int max_pumps);
ZMS_API const uint8_t *zms_vod_play_lane_video_config(const zms_vod_play_lane *lane, size_t *len);
ZMS_API const uint8_t *zms_vod_play_lane_audio_config(const zms_vod_play_lane *lane, size_t *len);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_VOD_EGRESS_PLAY_LANE_H */
