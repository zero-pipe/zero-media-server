#include "media/container/flv/flv_file_muxer.h"
#include "zms/media/codec/aac/aac_over_rtmp.h"
#include "zms/media/codec/aac/aac_over_rtp.h"
#include "zms/media/codec/av1/av1_over_rtmp.h"
#include "zms/media/codec/codec_id.h"
#include "zms/media/codec/h265/h265_over_rtmp.h"
#include "zms/media/codec/h266/h266_over_rtmp.h"
#include "zms/media/codec/vpx/vpx_over_rtmp.h"
#include "zms/media/container/flv/flv_wire.h"
#include "zms/media/container/flv/flv_tag_probe.h"
#include <string.h>

typedef struct {
    zms_flv_mux_pending *pend;
    uint32_t base_tag_dts_ms;
    unsigned idx;
    int queued;
} flv_aac_split_ctx;

void zms_flv_mux_pending_clear(zms_flv_mux_pending *pend)
{
    int i;

    if (!pend) {
        return;
    }
    for (i = 0; i < ZMS_FLV_MUX_PENDING_MAX; ++i) {
        zms_buf_pool_slot_clear(&pend->pending_body[i], &pend->pending_body_cap[i]);
        pend->pending_len[i] = 0;
    }
    pend->pending_cnt = 0;
    pend->pending_emit = 0;
}

static int flv_queue_aac_tag(const uint8_t *au, size_t len, void *user)
{
    flv_aac_split_ctx *ctx = (flv_aac_split_ctx *)user;
    zms_flv_mux_pending *pend;
    uint32_t tag_dts_ms;
    size_t tag_len = 0;
    size_t need;
    int slot;

    if (!ctx || !ctx->pend || !au || len == 0 || ctx->pend->pending_cnt >= ZMS_FLV_MUX_PENDING_MAX) {
        return 0;
    }
    pend = ctx->pend;
    need = len + 2;
    if (need > ZMS_FLV_MUX_PENDING_BODY_MAX) {
        return 0;
    }
    slot = pend->pending_cnt;
    if (!zms_buf_pool_slot_resize(&pend->pending_body[slot], &pend->pending_body_cap[slot], need)) {
        return 0;
    }
    tag_dts_ms =
        ctx->base_tag_dts_ms + ctx->idx * (pend->audio_step_ms ? pend->audio_step_ms : 23u);
    if (zms_rtmp_aac_frame(au, len, tag_dts_ms, pend->pending_body[slot],
                           pend->pending_body_cap[slot], &tag_len) != ZTK_OK ||
        tag_len == 0) {
        return 0;
    }
    pend->pending_type[slot] = 8;
    pend->pending_tag_dts_ms[slot] = tag_dts_ms;
    pend->pending_len[slot] = tag_len;
    pend->pending_cnt++;
    ctx->idx++;
    ctx->queued = 1;
    return 0;
}

int zms_flv_video_cfg_body(const uint8_t *cfg, size_t clen, zms_codec_id fallback_vc,
                           uint8_t *scratch, size_t scratch_cap, const uint8_t **body,
                           size_t *body_len)
{
    zms_codec_id vc;
    size_t built = 0;

    if (!cfg || clen == 0 || !scratch || scratch_cap == 0 || !body || !body_len) {
        return -1;
    }
    vc = zms_flv_video_config_codec(cfg, clen);
    if (vc == ZMS_CODEC_INVALID && fallback_vc != ZMS_CODEC_INVALID) {
        vc = fallback_vc;
    }
    *body = cfg;
    *body_len = clen;
    if (vc == ZMS_CODEC_H265) {
        if (zms_h265_flv_sequence_header(cfg, clen, scratch, scratch_cap, &built) != ZTK_OK ||
            built == 0) {
            return 0;
        }
    } else if (vc == ZMS_CODEC_H266) {
        if (zms_h266_flv_sequence_header(cfg, clen, scratch, scratch_cap, &built) != ZTK_OK ||
            built == 0) {
            return 0;
        }
    } else if (vc == ZMS_CODEC_AV1) {
        if (zms_av1_flv_sequence_header(cfg, clen, scratch, scratch_cap, &built) != ZTK_OK ||
            built == 0) {
            return 0;
        }
    } else if (vc == ZMS_CODEC_VP8 || vc == ZMS_CODEC_VP9) {
        if (zms_vpx_flv_sequence_header(vc, cfg, clen, scratch, scratch_cap, &built) != ZTK_OK ||
            built == 0) {
            return 0;
        }
    } else if (vc != ZMS_CODEC_H264) {
        return 0;
    }
    if (built > 0) {
        *body = scratch;
        *body_len = built;
    }
    return 1;
}

int zms_flv_mux_write_video_cfg_tag(const uint8_t *cfg, size_t clen, uint8_t *out, size_t cap,
                                    size_t *out_len)
{
    uint8_t body_stack[4096];
    const uint8_t *body = NULL;
    size_t body_len = 0;
    int r;

    if (!out || !out_len) {
        return -1;
    }
    r = zms_flv_video_cfg_body(cfg, clen, ZMS_CODEC_INVALID, body_stack, sizeof(body_stack), &body,
                               &body_len);
    if (r <= 0) {
        return r;
    }
    if (zms_flv_write_tag(out, cap, out_len, 9, 0, body, body_len) != ZTK_OK) {
        return -1;
    }
    return 1;
}

int zms_flv_mux_try_split_aac_tag(zms_flv_mux_pending *pend, const uint8_t *tag, size_t tag_len,
                                  uint32_t pkt_tag_dts_ms)
{
    const uint8_t *es = NULL;
    size_t es_len = 0;
    zms_codec_id ac = ZMS_CODEC_INVALID;
    flv_aac_split_ctx ctx;

    if (!pend || tag_len <= 512 ||
        zms_flv_tag_audio_to_es(tag, tag_len, &es, &es_len, &ac) != ZTK_OK || ac != ZMS_CODEC_AAC ||
        es_len <= 400) {
        return 0;
    }
    ctx.pend = pend;
    ctx.base_tag_dts_ms = pkt_tag_dts_ms;
    ctx.idx = 0;
    ctx.queued = 0;
    pend->pending_cnt = 0;
    pend->pending_emit = 0;
    if (zms_aac_es_foreach_frame(es, es_len, flv_queue_aac_tag, &ctx) != ZTK_OK || !ctx.queued ||
        ctx.idx < 2) {
        return 0;
    }
    return 1;
}
