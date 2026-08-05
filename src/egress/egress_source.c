/**
 * @file egress_source.c
 * @brief 为出站挂接直播 GOP / 点播 play-buffer 读者。
 */
#include "zms/egress/egress_source.h"
#include "zms/vod/io/vod_source.h"
#include <string.h>

void zms_egress_source_init(zms_egress_source *p)
{
    if (p) {
        memset(p, 0, sizeof(*p));
    }
}

void zms_egress_source_close(zms_egress_source *p)
{
    if (!p) {
        return;
    }
    zms_media_subscriber_detach(&p->readers);
    memset(p, 0, sizeof(*p));
}

ztk_err_t zms_egress_source_open_live_gop(zms_media_source *src, zms_egress_source *p)
{
    if (!src || !p) {
        return ZTK_ERR_INVALID;
    }
    zms_egress_source_close(p);
    p->source = src;
    p->is_live = 1;
    return zms_media_source_subscribe_gop(src, &p->readers);
}

ztk_err_t zms_egress_source_open_live_key(zms_media_source *src, zms_egress_source *p)
{
    if (!src || !p) {
        return ZTK_ERR_INVALID;
    }
    zms_egress_source_close(p);
    p->source = src;
    p->is_live = 1;
    if (zms_media_source_subscribe(src, 0, &p->readers) != ZTK_OK) {
        return ZTK_ERR_STATE;
    }
    if (p->readers.gop) {
        zms_gop_reader_seek_live_idr(p->readers.gop);
    }
    return ZTK_OK;
}

ztk_err_t zms_egress_source_open_live(zms_media_source *src, int seek_live, zms_egress_source *p)
{
    if (!src || !p) {
        return ZTK_ERR_INVALID;
    }
    zms_egress_source_close(p);
    p->source = src;
    p->is_live = 1;
    return zms_media_source_subscribe(src, seek_live, &p->readers);
}

ztk_err_t zms_egress_source_open_vod(zms_media_source *src, zms_egress_source *p)
{
    if (!src || !p) {
        return ZTK_ERR_INVALID;
    }
    zms_egress_source_close(p);
    p->source = src;
    p->is_live = 0;
    return zms_media_source_subscribe_vod(src, &p->readers);
}

int zms_egress_source_read_muxed(zms_egress_source *p, zms_gop_slot *slot, int timeout_ms)
{
    if (!p || !slot) {
        return -1;
    }
    if (p->readers.gop) {
        return zms_gop_reader_read_muxed(p->readers.gop, slot, timeout_ms);
    }
    if (p->readers.vod) {
        return zms_vod_buffer_reader_read_muxed(p->readers.vod, slot);
    }
    (void)timeout_ms;
    return 0;
}
