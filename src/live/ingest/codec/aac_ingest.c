#include "zms/live/ingest/common/stream_ingest.h"
#include "zms/media/codec/aac/aac_over_rtmp.h"
#include "zms/media/codec/aac/aac_over_rtp.h"
#include "zms/engine/media_clock.h"
#include "live/ingest/common/ingest_internal.h"
#include "ztk/util/log.h"
#include <string.h>
#include "zms/util/hex_decode.h"

typedef struct {
    zms_live_ingest *ch;
    uint32_t pes_raw_dts;
    unsigned idx;
    uint32_t frame_dur_ms;
    ztk_err_t err;
} aac_split_ctx;

static ztk_err_t aac_write_one_au(zms_live_ingest *ch, const uint8_t *au, size_t len,
                                  uint32_t raw_dts_ms)
{
    zms_frame frame;

    if (!ch || !au || len == 0) {
        return ZTK_ERR_INVALID;
    }
    if (!ch->source->gop_queue) {
        return ZTK_ERR_INVALID;
    }

    zms_frame_init(&frame);
    frame.data = (uint8_t *)au;
    frame.size = len;
    frame.dts_ms = frame.pts_ms = live_ingest_audio_pts(ch, raw_dts_ms);
    frame.codec = ZMS_CODEC_AAC;
    frame.track = ZMS_TRACK_AUDIO;
    live_ingest_write_frame(ch, &frame);
    ch->source->has_audio = 1;
    return ZTK_OK;
}

static int aac_split_handler(const uint8_t *au, size_t len, void *user)
{
    aac_split_ctx *ctx = (aac_split_ctx *)user;
    uint32_t raw_dts;
    ztk_err_t err;

    if (!ctx || !ctx->ch || ctx->err != ZTK_OK) {
        return -1;
    }
    raw_dts = ctx->pes_raw_dts + ctx->idx * ctx->frame_dur_ms;
    err = aac_write_one_au(ctx->ch, au, len, raw_dts);
    if (err != ZTK_OK) {
        ctx->err = err;
        return -1;
    }
    ctx->idx++;
    return 0;
}

ztk_err_t zms_live_ingest_set_aac_config_hex(zms_live_ingest *ch, const char *config_hex)
{
    if (!ch || !ch->source || !config_hex) {
        return ZTK_ERR_INVALID;
    }
    uint8_t asc[16];
    size_t asc_len = 0;
    if (zms_hex_decode(config_hex, asc, sizeof(asc), &asc_len) != ZTK_OK || asc_len == 0) {
        return ZTK_ERR_INVALID;
    }

    uint8_t *buf = live_ingest_work_buf(ch);
    size_t tag_len = 0;
    ztk_err_t err = zms_rtmp_aac_seq_header(asc, asc_len, buf, ZMS_LIVE_INGEST_WORK_BUF, &tag_len);
    if (err == ZTK_OK) {
        live_ingest_set_audio_config(ch, buf, tag_len);
        ch->have_audio_cfg = 1;
        ch->source->has_audio = 1;
        (void)zms_audio_track_from_asc(&ch->source->audio, asc, asc_len);
    }
    return err;
}

ztk_err_t zms_live_ingest_ensure_aac_config(zms_live_ingest *ch, int sample_rate, int channels)
{
    if (!ch || ch->have_audio_cfg) {
        return ZTK_OK;
    }
    char hex[16];
    if (!zms_aac_build_config_hex(sample_rate, channels, hex, sizeof(hex))) {
        return ZTK_ERR_INVALID;
    }
    ztk_info("ingress: ensure AAC config=%s rate=%d ch=%d", hex, sample_rate, channels);
    if (!ch->tl.audio_codec) {
        zms_media_timeline_set_audio(&ch->tl, ZMS_CODEC_AAC, (uint32_t)sample_rate);
    }
    return zms_live_ingest_set_aac_config_hex(ch, hex);
}

ztk_err_t zms_live_ingest_input_aac_es(zms_live_ingest *ch, const uint8_t *aac, size_t len,
                                       uint32_t dts_ms)
{
    int sample_rate;
    uint32_t frame_dur;
    aac_split_ctx split;

    if (!ch || !aac || len == 0) {
        return ZTK_ERR_INVALID;
    }

    /* defer_gop_vcfg 下队列从 IDR 起；AV 原点未设前跳过 AAC。 */
    if (ch->defer_gop_vcfg && ch->tl.linear_ms && !ch->av_origin_set) {
        return ZTK_OK;
    }
    if (ch->defer_gop_vcfg && !ch->tl.linear_ms && !ch->gop_vcfg_applied) {
        return ZTK_OK;
    }

    if (!ch->have_audio_cfg) {
        int rate = 0;
        int chans = 0;

        if (len >= 7 && aac[0] == 0xff && (aac[1] & 0xf0) == 0xf0 &&
            zms_aac_adts_parse(aac, len, &rate, &chans)) {
            (void)zms_live_ingest_ensure_aac_config(ch, rate, chans);
        } else if (ch->source && ch->source->audio.ready) {
            (void)zms_live_ingest_ensure_aac_config(ch, (int)ch->source->audio.sample_rate,
                                                    (int)ch->source->audio.channels);
        }
    }

    sample_rate =
        ch->source && ch->source->audio.ready ? (int)ch->source->audio.sample_rate : 44100;
    frame_dur = zms_codec_frame_duration_ms(ZMS_CODEC_AAC, (uint32_t)sample_rate);

    memset(&split, 0, sizeof(split));
    split.ch = ch;
    split.pes_raw_dts = dts_ms;
    split.frame_dur_ms = frame_dur > 0 ? frame_dur : 23u;
    split.err = ZTK_OK;

    if (len >= 7 && aac[0] == 0xff && (aac[1] & 0xf0) == 0xf0) {
        if (zms_aac_es_foreach_frame(aac, len, aac_split_handler, &split) != ZTK_OK &&
            split.err != ZTK_OK) {
            return split.err;
        }
        if (split.idx == 0) {
            return ZTK_ERR_INVALID;
        }
    } else {
        if (aac_write_one_au(ch, aac, len, dts_ms) != ZTK_OK) {
            return ZTK_ERR_INVALID;
        }
        split.idx = 1;
    }

    return ZTK_OK;
}
