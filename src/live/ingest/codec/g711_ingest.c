#include "zms/live/ingest/common/stream_ingest.h"
#include "zms/live/ingest/common/ingest_codec.h"
#include "live/ingest/common/ingest_internal.h"
#include "ztk/util/log.h"

void zms_live_ingest_g711_ensure(zms_live_ingest *ch, zms_codec_id codec)
{
    if (!ch || ch->have_audio_cfg) {
        return;
    }
    int rate = 8000;
    zms_media_timeline_set_audio(&ch->tl, codec, (uint32_t)rate);
    ch->have_audio_cfg = 1;
    ch->source->has_audio = 1;
    (void)zms_audio_track_from_g711(&ch->source->audio, codec, rate, 1);
    ztk_info("ingress: G711 audio ready codec=%s", zms_codec_name(codec));
}

ztk_err_t zms_live_ingest_input_g711_es(zms_live_ingest *ch, zms_codec_id codec,
                                        const uint8_t *g711, size_t len, uint32_t dts_ms)
{
    if (!ch || !g711 || len == 0 || (codec != ZMS_CODEC_G711A && codec != ZMS_CODEC_G711U)) {
        return ZTK_ERR_INVALID;
    }

    zms_live_ingest_g711_ensure(ch, codec);
    dts_ms = live_ingest_audio_pts(ch, dts_ms);

    if (!ch->source->gop_queue) {
        return ZTK_ERR_INVALID;
    }
    zms_frame frame;
    zms_frame_init(&frame);
    frame.data = (uint8_t *)g711;
    frame.size = len;
    frame.dts_ms = frame.pts_ms = dts_ms;
    frame.codec = codec;
    frame.track = ZMS_TRACK_AUDIO;
    live_ingest_write_frame(ch, &frame);
    ch->source->has_audio = 1;
    return ZTK_OK;
}
