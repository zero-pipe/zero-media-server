#include "zms/media/container/flv/flv_tag_pack.h"
#include "zms/media/container/flv/flv_types.h"
#include "zms/media/codec/codec_id.h"
#include "zms/media/codec/aac/aac_over_rtmp.h"
#include "zms/media/codec/aac/aac_over_rtp.h"
#include "zms/media/codec/h264/h264_over_rtmp.h"
#include "zms/media/codec/h265/h265_over_rtmp.h"
#include "zms/media/codec/av1/av1_over_rtmp.h"
#include "zms/media/codec/h266/h266_over_rtmp.h"
#include "zms/media/codec/vpx/vpx_over_rtmp.h"
#include "flv-muxer.h"
#include <stdlib.h>
#include <string.h>

struct zms_flv_pack_ctx {
    uint8_t *buf;
    size_t cap;
    size_t len;
    uint8_t msg_type;
    int ok;
};

static int zms_flv_on_tag(void *param, int type, const void *data, size_t bytes, uint32_t timestamp)
{
    struct zms_flv_pack_ctx *ctx = (struct zms_flv_pack_ctx *)param;

    (void)timestamp;
    if (!ctx || !data || bytes == 0) {
        return -1;
    }
    if (bytes > ctx->cap) {
        ctx->ok = 0;
        return -1;
    }
    memcpy(ctx->buf, data, bytes);
    ctx->len = bytes;
    ctx->msg_type = (uint8_t)type;
    ctx->ok = 1;
    return 0;
}

static ztk_err_t zms_flv_pack_video(const zms_flv_tag_pack_req *req, zms_flv_tag_pack_out *out)
{
    const zms_gop_slot *slot = req->slot;
    int key = slot->keyframe ? 1 : 0;

    if (slot->codec == ZMS_CODEC_H264) {
        return zms_h264_over_rtmp_pack_es(req->video_cfg, req->video_cfg_len, slot->data, slot->len,
                                          key, req->buf, req->cap, &out->tag_len);
    }
    if (slot->codec == ZMS_CODEC_H265) {
        return zms_h265_over_rtmp_pack_es(req->video_cfg, req->video_cfg_len, slot->data, slot->len,
                                          key, req->buf, req->cap, &out->tag_len);
    }
    if (slot->codec == ZMS_CODEC_H266) {
        return zms_h266_over_rtmp_pack_es(req->video_cfg, req->video_cfg_len, slot->data, slot->len,
                                          key, req->buf, req->cap, &out->tag_len);
    }
    if (slot->codec == ZMS_CODEC_AV1) {
        return zms_av1_over_rtmp_pack_es(req->video_cfg, req->video_cfg_len, slot->data, slot->len,
                                         key, req->buf, req->cap, &out->tag_len);
    }
    if (slot->codec == ZMS_CODEC_VP8 || slot->codec == ZMS_CODEC_VP9) {
        return zms_vpx_over_rtmp_pack_es(slot->codec, req->video_cfg, req->video_cfg_len,
                                         slot->data, slot->len, key, req->buf, req->cap,
                                         &out->tag_len);
    }
    return ZTK_ERR_NOT_IMPL;
}

static ztk_err_t zms_flv_pack_audio_aac(const zms_gop_slot *slot, uint8_t *buf, size_t cap,
                                        size_t *tag_len, uint8_t *msg_type)
{
    const uint8_t *raw = slot->data;
    size_t raw_len = slot->len;
    ztk_err_t err;

    if (!slot || !slot->data || slot->len == 0 || !buf || !tag_len || !msg_type) {
        return ZTK_ERR_INVALID;
    }

    err = zms_aac_es_to_raw(slot->data, slot->len, &raw, &raw_len);
    if (err != ZTK_OK || raw_len == 0) {
        return ZTK_ERR_INVALID;
    }
    err = zms_rtmp_aac_frame(raw, raw_len, slot->dts_ms, buf, cap, tag_len);
    if (err == ZTK_OK) {
        *msg_type = (uint8_t)ZMS_FLV_TAG_AUDIO;
    }
    return err;
}

static ztk_err_t zms_flv_pack_audio(flv_muxer_t *mux, struct zms_flv_pack_ctx *ctx,
                                    const zms_gop_slot *slot, uint32_t pts_ms, size_t *tag_len,
                                    uint8_t *msg_type)
{
    int rc;
    uint32_t dts = slot->dts_ms;

    pts_ms = pts_ms ? pts_ms : dts;
    ctx->ok = 0;
    ctx->len = 0;

    if (slot->codec == ZMS_CODEC_AAC) {
        return zms_flv_pack_audio_aac(slot, ctx->buf, ctx->cap, tag_len, msg_type);
    } else if (slot->codec == ZMS_CODEC_G711A) {
        rc = flv_muxer_g711a(mux, slot->data, slot->len, pts_ms, dts);
    } else if (slot->codec == ZMS_CODEC_G711U) {
        rc = flv_muxer_g711u(mux, slot->data, slot->len, pts_ms, dts);
    } else if (slot->codec == ZMS_CODEC_OPUS) {
        rc = flv_muxer_opus(mux, slot->data, slot->len, pts_ms, dts);
    } else {
        return ZTK_ERR_NOT_IMPL;
    }

    if (rc != 0 || !ctx->ok || ctx->len == 0) {
        return ZTK_ERR_INVALID;
    }
    *tag_len = ctx->len;
    *msg_type = (uint8_t)ZMS_FLV_TAG_AUDIO;
    if (ctx->msg_type == (uint8_t)ZMS_FLV_TAG_AUDIO) {
        *msg_type = (uint8_t)ZMS_FLV_TAG_AUDIO;
    }
    return ZTK_OK;
}

ztk_err_t zms_flv_tag_pack(const zms_flv_tag_pack_req *req, zms_flv_tag_pack_out *out)
{
    flv_muxer_t *mux = NULL;
    ztk_err_t err;
    struct zms_flv_pack_ctx ctx;

    if (!req || !req->slot || !req->buf || !out) {
        return ZTK_ERR_INVALID;
    }

    out->tag_len = 0;
    out->rtmp_msg_type = 0;

    if (req->slot->track == ZMS_TRACK_VIDEO) {
        err = zms_flv_pack_video(req, out);
        if (err == ZTK_OK) {
            out->rtmp_msg_type = (uint8_t)ZMS_FLV_TAG_VIDEO;
        }
        return err;
    }

    if (req->slot->track != ZMS_TRACK_AUDIO) {
        return ZTK_ERR_INVALID;
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.buf = req->buf;
    ctx.cap = req->cap;
    mux = flv_muxer_create(zms_flv_on_tag, &ctx);
    if (!mux) {
        return ZTK_ERR_NOMEM;
    }
    err = zms_flv_pack_audio(mux, &ctx, req->slot, req->pts_ms, &out->tag_len, &out->rtmp_msg_type);
    flv_muxer_destroy(mux);
    return err;
}
