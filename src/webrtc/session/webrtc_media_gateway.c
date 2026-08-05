#include "webrtc/session/webrtc_media_gateway.h"
#include "webrtc/session/webrtc_media_internal.h"
#include <string.h>

zms_webrtc_gateway_pkt_kind zms_webrtc_gateway_classify(const uint8_t *data, size_t len)
{
    if (!data || len == 0) {
        return ZMS_WEBRTC_GATEWAY_UNKNOWN;
    }
    if (zms_webrtc_stun_is_binding_req(data, len)) {
        return ZMS_WEBRTC_GATEWAY_STUN;
    }
    if (zms_webrtc_packet_is_dtls(data, len)) {
        return ZMS_WEBRTC_GATEWAY_DTLS;
    }
    if (zms_rtp_is_rtcp(data, len)) {
        return ZMS_WEBRTC_GATEWAY_SRTCP;
    }
    if (zms_webrtc_packet_is_rtp(data, len)) {
        return ZMS_WEBRTC_GATEWAY_SRTP;
    }
    return ZMS_WEBRTC_GATEWAY_UNKNOWN;
}

void zms_webrtc_gateway_ingest_cfg_defaults(zms_webrtc_gateway_ingest_cfg *cfg)
{
    if (!cfg) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->canon_video_pt = ZMS_WEBRTC_CANON_VIDEO_PT;
    cfg->canon_audio_pt = ZMS_WEBRTC_CANON_AUDIO_PT;
    cfg->answer_has_video = 1;
}

int zms_webrtc_gateway_match_media_track(const zms_webrtc_gateway_ingest_cfg *cfg, uint8_t pt)
{
    uint8_t vpt;
    uint8_t apt;

    if (!cfg) {
        return -1;
    }
    vpt = cfg->wire_video_pt ? cfg->wire_video_pt : cfg->canon_video_pt;
    apt = cfg->wire_audio_pt ? cfg->wire_audio_pt : cfg->canon_audio_pt;
    if (cfg->answer_has_video && pt == vpt) {
        return 1;
    }
    if (cfg->answer_has_audio && pt == apt) {
        return 0;
    }
    if (cfg->answer_has_video && !cfg->answer_has_audio && pt == cfg->canon_video_pt) {
        return 1;
    }
    return -1;
}

void zms_webrtc_gateway_remap_pt(uint8_t *rtp, size_t len, const zms_webrtc_gateway_ingest_cfg *cfg,
                                 int is_video)
{
    uint8_t wire_pt;
    uint8_t canon_pt;
    uint8_t cur;

    if (!rtp || len < 2 || !cfg) {
        return;
    }
    if (is_video) {
        wire_pt = cfg->wire_video_pt ? cfg->wire_video_pt : cfg->canon_video_pt;
        canon_pt = cfg->canon_video_pt ? cfg->canon_video_pt : ZMS_WEBRTC_CANON_VIDEO_PT;
    } else {
        wire_pt = cfg->wire_audio_pt ? cfg->wire_audio_pt : cfg->canon_audio_pt;
        canon_pt = cfg->canon_audio_pt ? cfg->canon_audio_pt : ZMS_WEBRTC_CANON_AUDIO_PT;
    }
    if (wire_pt == 0 || canon_pt == 0 || wire_pt == canon_pt) {
        return;
    }
    cur = zms_rtp_payload_type(rtp, len);
    if (cur == wire_pt) {
        zms_rtp_set_payload_type(rtp, len, canon_pt);
    }
}

void zms_webrtc_gateway_remap_pt_egress(uint8_t *rtp, size_t len,
                                        const zms_webrtc_gateway_ingest_cfg *cfg, int is_video)
{
    uint8_t wire_pt;
    uint8_t canon_pt;
    uint8_t cur;

    if (!rtp || len < 2 || !cfg) {
        return;
    }
    if (is_video) {
        wire_pt = cfg->wire_video_pt ? cfg->wire_video_pt : cfg->canon_video_pt;
        canon_pt = cfg->canon_video_pt ? cfg->canon_video_pt : ZMS_WEBRTC_CANON_VIDEO_PT;
    } else {
        wire_pt = cfg->wire_audio_pt ? cfg->wire_audio_pt : cfg->canon_audio_pt;
        canon_pt = cfg->canon_audio_pt ? cfg->canon_audio_pt : ZMS_WEBRTC_CANON_AUDIO_PT;
    }
    if (wire_pt == 0 || canon_pt == 0 || wire_pt == canon_pt) {
        return;
    }
    cur = zms_rtp_payload_type(rtp, len);
    if (cur == canon_pt) {
        zms_rtp_set_payload_type(rtp, len, wire_pt);
    }
}
