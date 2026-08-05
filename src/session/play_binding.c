/**
 * @file play_binding.c
 * @brief 播放侧 reader 记账与 lane/egress_source 统一拆除。
 */
#include "zms/session/play_binding.h"
#include "zms/session/session_dispatcher.h"
#include "zms/engine/media_event.h"
#include "zms/vod/play/vod_play_lane.h"
#include "ztk/platform.h"

void zms_play_binding_reader_start(zms_play_binding *b, zms_media_source *src, const char *player)
{
    if (!b || !src || !b->reader_attached) {
        return;
    }
    if (*b->reader_attached) {
        return;
    }
    if (b->source) {
        *b->source = src;
    }
    zms_media_source_reader_add(src);
    *b->reader_attached = 1;
    if (b->play_start_ms) {
        *b->play_start_ms = ztk_monotonic_ms();
    }
    if (player) {
        b->player = player;
    }
    zms_media_event_play(src, b->player ? b->player : "play");
}

void zms_play_binding_reader_stop(zms_play_binding *b)
{
    zms_media_source *src;
    const char *player;
    uint64_t start_ms;

    if (!b || !b->reader_attached || !*b->reader_attached) {
        return;
    }
    src = (b->source && *b->source) ? *b->source : NULL;
    player = b->player ? b->player : "play";
    start_ms = b->play_start_ms ? *b->play_start_ms : 0;
    if (src) {
        zms_media_source_reader_remove(src);
        zms_media_event_stop(src, player, start_ms);
    }
    *b->reader_attached = 0;
    if (b->play_start_ms) {
        *b->play_start_ms = 0;
    }
}

void zms_play_binding_close_readers(zms_play_binding *b)
{
    if (!b) {
        return;
    }
    /*
     * close 前先置空别名：VOD lane 可能持有曾挂到 play.readers.vod 的 buffer reader
     *（RTMP/RTSP）。拆除时勿释放该 reader。
     */
    if (b->gop_reader) {
        *b->gop_reader = NULL;
    }
    if (b->vod_reader) {
        *b->vod_reader = NULL;
    }
    if (b->play) {
        b->play->readers.gop = NULL;
        b->play->readers.vod = NULL;
        zms_session_play_close(b->play);
    }
    if (b->vod_lane && *b->vod_lane) {
        zms_vod_play_lane_close(*b->vod_lane);
        *b->vod_lane = NULL;
    }
}

void zms_play_binding_close(zms_play_binding *b, int clear_source)
{
    if (!b) {
        return;
    }
    zms_play_binding_close_readers(b);
    zms_play_binding_reader_stop(b);
    if (clear_source && b->source) {
        *b->source = NULL;
    }
}

void zms_play_binding_demux_fill(zms_play_binding *b, int max_pumps)
{
    if (!b || !b->vod_lane || !*b->vod_lane || max_pumps <= 0) {
        return;
    }
    zms_vod_play_lane_demux_fill(*b->vod_lane, max_pumps);
}
