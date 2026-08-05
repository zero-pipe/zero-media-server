/**
 * @file frame.c
 * @brief zms_frame 生命周期与 GOP/同步判定辅助。
 *
 * Copyright (c) zero-media-server
 */
#include "zms/engine/frame.h"
#include "zms/engine/gop/gop_queue.h"
#include "zms/media/codec/h264/h264_es.h"
#include "zms/media/codec/h265/h265_es.h"
#include "zms/media/codec/frame_policy/frame_codec_policy.h"
#include <stdlib.h>
#include <string.h>

void zms_frame_init(zms_frame *f)
{
    if (!f) {
        return;
    }
    memset(f, 0, sizeof(*f));
}

void zms_frame_clear(zms_frame *f)
{
    if (!f) {
        return;
    }
    if (f->owned && f->data) {
        free(f->data);
    }
    zms_frame_init(f);
}

ztk_err_t zms_frame_reserve(zms_frame *f, size_t cap)
{
    if (!f) {
        return ZTK_ERR_INVALID;
    }
    if (cap <= f->capacity) {
        return ZTK_OK;
    }
    uint8_t *p = (uint8_t *)realloc(f->owned ? f->data : NULL, cap);
    if (!p) {
        return ZTK_ERR_NOMEM;
    }
    f->data = p;
    f->capacity = cap;
    f->owned = 1;
    return ZTK_OK;
}

void zms_frame_refresh_key_from_es(zms_frame *f)
{
    const zms_frame_codec_ops *ops;

    if (!f || f->track != ZMS_TRACK_VIDEO || f->config_frame || !f->data || f->size == 0) {
        return;
    }
    ops = zms_frame_codec_find(f->codec);
    if (ops && ops->is_keyframe && ops->is_keyframe(f)) {
        f->keyframe = 1;
    }
}

int zms_frame_video_gop_marker(const zms_frame *f)
{
    const zms_frame_codec_ops *ops;
    if (!f || f->track != ZMS_TRACK_VIDEO) {
        return 0;
    }
    ops = zms_frame_codec_find(f->codec);
    if (ops && ops->is_keyframe && ops->is_config_frame) {
        return ops->is_keyframe(f) || ops->is_config_frame(f);
    }
    return f->keyframe || f->config_frame;
}

int zms_gop_queue_storable(const zms_frame *f, int cache_started, int has_video)
{
    const zms_frame_codec_ops *ops;
    if (!f) {
        return 0;
    }
    ops = zms_frame_codec_find(f->codec);
    if (ops && ops->gop_queue_storable) {
        return ops->gop_queue_storable(f, cache_started, has_video);
    }
    if (f->track == ZMS_TRACK_AUDIO) {
        return has_video ? cache_started : 1;
    }
    if (f->drop_able) {
        return cache_started;
    }
    if (zms_frame_video_gop_marker(f)) {
        return 1;
    }
    return cache_started;
}

int zms_gop_queue_new_gop(const zms_frame *f, int video_key_pos, int has_video)
{
    const zms_frame_codec_ops *ops;
    if (!f) {
        return 0;
    }
    ops = zms_frame_codec_find(f->codec);
    if (ops && ops->gop_queue_new_gop) {
        return ops->gop_queue_new_gop(f, video_key_pos, has_video);
    }
    if (f->track == ZMS_TRACK_AUDIO) {
        return has_video ? 0 : 1;
    }
    (void)video_key_pos;
    if (f->config_frame) {
        return 0;
    }
    return zms_frame_video_gop_marker(f);
}

int zms_frame_is_egress_sync(const zms_frame *f)
{
    const zms_frame_codec_ops *ops;
    if (!f || f->track != ZMS_TRACK_VIDEO || f->config_frame || !f->data || f->size == 0) {
        return 0;
    }
    if (f->keyframe) {
        return 1;
    }
    ops = zms_frame_codec_find(f->codec);
    return ops && ops->is_keyframe && ops->is_keyframe(f);
}

int zms_frame_is_decode_start(const zms_frame *f)
{
    if (!f || f->track != ZMS_TRACK_VIDEO || f->config_frame || !f->data || f->size == 0) {
        return 0;
    }
    if (f->codec == ZMS_CODEC_H265) {
        /* 接受 IDR 与 CRA（type 16-21）：libx265/FFmpeg 常发 CRA_NUT */
        return zms_h265_annexb_is_sync_key(f->data, f->size);
    }
    if (f->codec == ZMS_CODEC_H264) {
        return f->keyframe || zms_h264_annexb_is_sync_key(f->data, f->size);
    }
    return f->keyframe || zms_frame_is_egress_sync(f);
}

int zms_frame_is_play_start(const zms_frame *f)
{
    return zms_frame_is_decode_start(f);
}

void zms_gop_queue_timeline_from_frame(const zms_frame *f, uint32_t *dts_ms, uint32_t *pts_ms)
{
    uint32_t dts;
    uint32_t pts;

    if (!dts_ms || !pts_ms) {
        return;
    }
    if (!f) {
        *dts_ms = 0;
        *pts_ms = 0;
        return;
    }
    dts = (uint32_t)f->dts_ms;
    pts = (uint32_t)f->pts_ms;
    if (pts == 0) {
        pts = dts;
    }
    *dts_ms = dts;
    *pts_ms = (pts != dts) ? pts : 0;
}

uint32_t zms_gop_slot_dts_ms(const zms_gop_slot *slot)
{
    return slot ? slot->dts_ms : 0;
}

uint32_t zms_gop_slot_pts_ms(const zms_gop_slot *slot)
{
    if (!slot) {
        return 0;
    }
    return slot->pts_ms ? slot->pts_ms : slot->dts_ms;
}

int zms_gop_slot_is_egress_sync(const zms_gop_slot *slot)
{
    zms_frame f;
    if (!slot) {
        return 0;
    }
    zms_frame_init(&f);
    f.data = slot->data;
    f.size = slot->len;
    f.codec = slot->codec;
    f.track = slot->track;
    f.keyframe = slot->keyframe;
    f.config_frame = slot->config_frame;
    return zms_frame_is_egress_sync(&f);
}

void zms_gop_slot_refresh_sync_key(zms_gop_slot *slot)
{
    if (slot && zms_gop_slot_is_egress_sync(slot)) {
        slot->keyframe = 1;
    }
}

int zms_gop_slot_is_decode_start(const zms_gop_slot *slot)
{
    zms_frame f;
    if (!slot) {
        return 0;
    }
    zms_frame_init(&f);
    f.data = slot->data;
    f.size = slot->len;
    f.codec = slot->codec;
    f.track = slot->track;
    f.keyframe = slot->keyframe;
    f.config_frame = slot->config_frame;
    return zms_frame_is_decode_start(&f);
}

void zms_gop_slot_refresh_decode_key(zms_gop_slot *slot)
{
    if (slot && zms_gop_slot_is_decode_start(slot)) {
        slot->keyframe = 1;
    }
}

int zms_gop_slot_is_play_start(const zms_gop_slot *slot)
{
    zms_frame f;
    if (!slot) {
        return 0;
    }
    zms_frame_init(&f);
    f.data = slot->data;
    f.size = slot->len;
    f.codec = slot->codec;
    f.track = slot->track;
    f.keyframe = slot->keyframe;
    f.config_frame = slot->config_frame;
    return zms_frame_is_play_start(&f);
}

void zms_gop_slot_refresh_play_key(zms_gop_slot *slot)
{
    if (slot && zms_gop_slot_is_play_start(slot)) {
        slot->keyframe = 1;
    }
}

ztk_err_t zms_frame_assign(zms_frame *f, const void *data, size_t len, int copy)
{
    if (!f || (!data && len)) {
        return ZTK_ERR_INVALID;
    }
    if (copy) {
        ztk_err_t err = zms_frame_reserve(f, len);
        if (err != ZTK_OK) {
            return err;
        }
        memcpy(f->data, data, len);
        f->size = len;
        f->owned = 1;
    } else {
        zms_frame_clear(f);
        f->data = (uint8_t *)(uintptr_t)data;
        f->size = len;
        f->owned = 0;
    }
    return ZTK_OK;
}
