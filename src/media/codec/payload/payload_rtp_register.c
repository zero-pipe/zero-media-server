#include "zms/media/codec/payload/payload_registry.h"

extern const zms_payload_demux_ops zms_h264_over_rtp_demux_ops;
extern const zms_payload_demux_ops zms_h265_over_rtp_demux_ops;
extern const zms_payload_demux_ops zms_aac_over_rtp_demux_ops;
extern const zms_payload_demux_ops zms_g711a_over_rtp_demux_ops;
extern const zms_payload_demux_ops zms_g711u_over_rtp_demux_ops;
extern const zms_payload_demux_ops zms_av1_over_rtp_demux_ops;
extern const zms_payload_demux_ops zms_opus_over_rtp_demux_ops;
extern const zms_payload_demux_ops zms_vp8_over_rtp_demux_ops;
extern const zms_payload_demux_ops zms_vp9_over_rtp_demux_ops;
extern const zms_payload_demux_ops zms_h266_over_rtp_demux_ops;

static const zms_payload_demux_ops *const k_rtp_demux[] = {
    &zms_h264_over_rtp_demux_ops,
    &zms_h265_over_rtp_demux_ops,
    &zms_aac_over_rtp_demux_ops,
    &zms_g711a_over_rtp_demux_ops,
    &zms_g711u_over_rtp_demux_ops,
    &zms_av1_over_rtp_demux_ops,
    &zms_opus_over_rtp_demux_ops,
    &zms_vp8_over_rtp_demux_ops,
    &zms_vp9_over_rtp_demux_ops,
    &zms_h266_over_rtp_demux_ops,
    NULL,
};

void zms_rtp_payload_register_all(void)
{
    const zms_payload_demux_ops *const *p;

    for (p = k_rtp_demux; *p; ++p) {
        zms_payload_register_demux(*p);
    }
}
