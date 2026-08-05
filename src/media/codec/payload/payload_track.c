#include "zms/media/codec/payload/payload_track.h"
#include <string.h>

void zms_payload_track_bank_clear(zms_payload_track_bank *bank)
{
    unsigned i;
    if (!bank) {
        return;
    }
    for (i = 0; i < ZMS_TRACK_SLOT_MAX; ++i) {
        zms_payload_track_slot *s = &bank->slots[i];
        if (!s->ctx) {
            continue;
        }
        const zms_payload_demux_ops *ops = zms_payload_demux_find(s->codec, ZMS_WIRE_FORMAT_RTP);
        if (ops && ops->destroy) {
            ops->destroy(s->ctx);
        }
        s->ctx = NULL;
        s->codec = ZMS_CODEC_INVALID;
    }
}

ztk_err_t zms_payload_track_bank_input_rtp(zms_payload_track_bank *bank, int track_index,
                                           zms_codec_id codec, zms_wire_format_id wire,
                                           uint32_t rtp_clock_hz, zms_payload_frame_cb on_frame,
                                           void *user, const zms_rtp_packet *pkt)
{
    zms_payload_track_slot *slot;
    const zms_payload_demux_ops *ops;
    zms_payload_demux_opts cfg;

    if (!bank || !pkt || track_index < 0 || (unsigned)track_index >= ZMS_TRACK_SLOT_MAX) {
        return ZTK_ERR_INVALID;
    }
    if (codec == ZMS_CODEC_INVALID || !on_frame) {
        return ZTK_ERR_INVALID;
    }

    ops = zms_payload_demux_find(codec, wire);
    if (!ops || !ops->input_rtp) {
        return ZTK_ERR_NOT_IMPL;
    }

    slot = &bank->slots[track_index];
    if (!slot->ctx || slot->codec != codec) {
        if (slot->ctx) {
            const zms_payload_demux_ops *old = zms_payload_demux_find(slot->codec, wire);
            if (old && old->destroy) {
                old->destroy(slot->ctx);
            }
            slot->ctx = NULL;
        }
        memset(&cfg, 0, sizeof(cfg));
        cfg.codec = codec;
        cfg.wire = wire;
        cfg.rtp_clock_hz = rtp_clock_hz;
        cfg.on_frame = on_frame;
        cfg.user = user;
        slot->ctx = ops->create ? ops->create(&cfg) : NULL;
        slot->codec = slot->ctx ? codec : ZMS_CODEC_INVALID;
        if (!slot->ctx) {
            return ZTK_ERR_INVALID;
        }
    }

    return ops->input_rtp(slot->ctx, pkt, NULL);
}
