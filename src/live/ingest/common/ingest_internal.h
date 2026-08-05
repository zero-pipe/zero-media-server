#ifndef ZMS_SRC_LIVE_INGEST_COMMON_INGEST_INTERNAL_H
#define ZMS_SRC_LIVE_INGEST_COMMON_INGEST_INTERNAL_H

#include "zms/live/ingest/common/stream_ingest.h"
#include "zms/live/ingest/common/protocol_opts.h"
#include "zms/engine/media_clock.h"
#include "zms/media/container/demux_pipeline.h"
#include "zms/media/container/flv/flv_tag_probe.h"
#include "zms/engine/media/media_limits.h"
#include "zms/util/buf_pool.h"
#include "ztk/util/buf.h"
#include "ztk/poller/poller.h"
#include <stddef.h>
#include <stdint.h>

struct zms_live_ingest {
    zms_media_source *source;
    struct ztk_poller *io_poller; /**< 会话钉住的 poller；有则工作区走本地无锁池 */
    int owns_source;
    int have_video_cfg;
    int have_audio_cfg;
    zms_media_timeline tl;
    zms_protocol_opts opt;
    /** codec 转换工作区（buf_pool，按需扩到 ZMS_LIVE_INGEST_WORK_BUF） */
    uint8_t *work_buf;
    size_t work_cap;
    uint8_t *large_buf;
    size_t large_cap;
    uint64_t last_v_raw_ms;
    uint64_t last_a_raw_ms;
    uint32_t last_v_pts;
    uint32_t last_a_pts;
    int last_v_valid;
    int last_a_valid;
    int defer_gop_vcfg;
    int gop_vcfg_applied;
    size_t video_cfg_len;
    uint32_t av_origin_ms;
    int av_origin_set;
    /** SRT linear_ms：TS CC 跳帧丢 P 后平滑 gop_queue dts。 */
    uint32_t tl_last_v_gop_norm;
    int tl_last_v_gop_valid;
    zms_demux_pipeline *rtmp_demux;
    struct {
        int active;
        uint32_t raw_tag_dts_ms;
        uint8_t msg_type;
    } rtmp_ingress;
    int rtmp_frame_got;
    struct {
        uint32_t raw_tag_dts_ms;
        uint32_t dts_ms;
        uint32_t pts_ms;
        int active;
        int key;
        uint8_t *buf;
        size_t len;
        size_t cap;
    } hevc_au;
};

static inline void *ingest_slot_resize(zms_live_ingest *ch, uint8_t **data, size_t *cap, size_t len)
{
    if (!ch || !data || !cap || len == 0) {
        return NULL;
    }
    if (ch->io_poller) {
        return zms_buf_pool_slot_resize_poller(data, cap, len, ch->io_poller);
    }
    return zms_buf_pool_slot_resize(data, cap, len);
}

static inline void ingest_slot_clear(zms_live_ingest *ch, uint8_t **data, size_t *cap)
{
    if (!data) {
        return;
    }
    if (ch && ch->io_poller) {
        zms_buf_pool_slot_clear_poller(data, cap, ch->io_poller);
    } else {
        zms_buf_pool_slot_clear(data, cap);
    }
}

uint32_t live_ingest_video_pts(zms_live_ingest *ch, uint32_t raw_ms);
uint32_t live_ingest_audio_pts(zms_live_ingest *ch, uint32_t raw_ms);
uint8_t *live_ingest_work_buf(zms_live_ingest *ch);
uint8_t *live_ingest_large_buf(zms_live_ingest *ch, size_t need);
void live_ingest_write_frame(zms_live_ingest *ch, const zms_frame *frame);
ztk_err_t live_ingest_write_frame_buf(zms_live_ingest *ch, ztk_buf *buf, const zms_frame *frame);
void live_ingest_set_video_config(zms_live_ingest *ch, const void *data, size_t len);
void live_ingest_set_audio_config(zms_live_ingest *ch, const void *data, size_t len);

#endif /* ZMS_SRC_LIVE_INGEST_COMMON_INGEST_INTERNAL_H */
