#include "zms/media/container/container_dispatcher.h"
#include "zms/media/wire/rtp_packet.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define ZMS_CONTAINER_SLOT_MAX 16

extern const zms_container_demuxer_ops zms_container_rtsp_interleaved_ops;
extern const zms_container_demuxer_ops zms_container_flv_tag_ops;
extern const zms_container_demuxer_ops zms_container_flv_file_ops;
extern const zms_container_demuxer_ops zms_container_mp4_ops;
extern const zms_container_demuxer_ops zms_container_mkv_ops;
extern const zms_container_demuxer_ops zms_container_mpegts_demuxer_ops;
extern const zms_container_muxer_ops zms_container_mpegts_muxer_ops;

static const zms_container_demuxer_ops *g_demux[ZMS_CONTAINER_SLOT_MAX];
static const zms_container_muxer_ops *g_mux[ZMS_CONTAINER_SLOT_MAX];
static int g_container_dispatch_all_registered;

static const char *k_names[] = {
    "invalid", "rtsp-interleaved", "flv-tag", "mpegts", "mp4", "mkv", "flv-file",
};

static void zms_container_dispatch_mark_ready(void)
{
    g_container_dispatch_all_registered = 1;
}

const char *zms_container_name(zms_container_id id)
{
    if ((unsigned)id >= sizeof(k_names) / sizeof(k_names[0])) {
        return "invalid";
    }
    return k_names[id];
}

void zms_container_register_demuxer(const zms_container_demuxer_ops *ops)
{
    if (!ops || ops->id <= 0 || ops->id >= ZMS_CONTAINER_SLOT_MAX) {
        return;
    }
    g_demux[ops->id] = ops;
}

const zms_container_demuxer_ops *zms_container_demuxer_find(zms_container_id id)
{
    if (!g_container_dispatch_all_registered) {
        zms_container_dispatch_register_all();
    }
    if (id <= 0 || id >= ZMS_CONTAINER_SLOT_MAX) {
        return NULL;
    }
    return g_demux[id];
}

void zms_container_register_muxer(const zms_container_muxer_ops *ops)
{
    if (!ops || ops->id <= 0 || ops->id >= ZMS_CONTAINER_SLOT_MAX) {
        return;
    }
    g_mux[ops->id] = ops;
}

const zms_container_muxer_ops *zms_container_muxer_find(zms_container_id id)
{
    if (!g_container_dispatch_all_registered) {
        zms_container_dispatch_register_all();
    }
    if (id <= 0 || id >= ZMS_CONTAINER_SLOT_MAX) {
        return NULL;
    }
    return g_mux[id];
}

void zms_container_dispatch_register_all(void)
{
    static int registered; /* 启动阶段，单线程 */

    if (registered) {
        return;
    }
    registered = 1;
    zms_container_register_demuxer(&zms_container_rtsp_interleaved_ops);
    zms_container_register_demuxer(&zms_container_flv_tag_ops);
    zms_container_register_demuxer(&zms_container_flv_file_ops);
    zms_container_register_demuxer(&zms_container_mp4_ops);
    zms_container_register_demuxer(&zms_container_mkv_ops);
    zms_container_register_demuxer(&zms_container_mpegts_demuxer_ops);
    zms_container_register_muxer(&zms_container_mpegts_muxer_ops);
    zms_container_dispatch_mark_ready();
}

ztk_err_t zms_container_rtsp_interleaved_parse(const uint8_t *data, size_t len,
                                               zms_container_packet *out)
{
    uint8_t channel;
    const uint8_t *payload;
    size_t plen;

    if (!out) {
        return ZTK_ERR_INVALID;
    }
    memset(out, 0, sizeof(*out));
    if (zms_rtsp_interleaved_read(data, len, &channel, &payload, &plen) != ZTK_OK) {
        return ZTK_ERR_INVALID;
    }
    out->kind = ZMS_CONTAINER_PKT_RTP;
    out->channel = channel;
    out->data = payload;
    out->len = plen;
    return ZTK_OK;
}
