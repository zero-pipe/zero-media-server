#include "zms/media/codec/payload/payload_registry.h"
#include <stddef.h>

#define ZMS_CODEC_MAX 32
#define ZMS_WIRE_FORMAT_MAX 16

static const zms_payload_demux_ops *g_demux[ZMS_CODEC_MAX][ZMS_WIRE_FORMAT_MAX];

static int slot_valid(zms_codec_id c, zms_wire_format_id w)
{
    return c > 0 && c < ZMS_CODEC_MAX && w > 0 && w < ZMS_WIRE_FORMAT_MAX;
}

void zms_payload_register_demux(const zms_payload_demux_ops *ops)
{
    if (!ops || !slot_valid(ops->codec, ops->wire)) {
        return;
    }
    g_demux[ops->codec][ops->wire] = ops;
}

const zms_payload_demux_ops *zms_payload_demux_find(zms_codec_id codec, zms_wire_format_id wire)
{
    if (!slot_valid(codec, wire)) {
        return NULL;
    }
    return g_demux[codec][wire];
}
