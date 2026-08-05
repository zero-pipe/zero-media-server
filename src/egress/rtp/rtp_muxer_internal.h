#ifndef ZMS_SRC_SESSION_RTSP_MUXER_INTERNAL_H
#define ZMS_SRC_SESSION_RTSP_MUXER_INTERNAL_H

/**
 * @file rtp_muxer_internal.h
 * @brief zms_rtp_muxer 私有布局与 codec 参数集缓存 helpers。
 */
#include "zms/egress/rtp/rtp_muxer.h"
#include "zms/engine/media_clock.h"
#include "zms/media/codec/codec_id.h"

#include <stddef.h>
#include <stdint.h>

struct rtsp_muxer_t; /* librtsp 不透明 muxer */

#define ZMS_RTSP_MUXER_HEVC_PARAM_MAX 512u
#define ZMS_RTSP_MUXER_H264_PARAM_MAX 512u
#define ZMS_RTSP_MUXER_H264_AVCC_MAX 512u
#define ZMS_RTSP_MUXER_H265_HVCC_MAX 1024u
#define ZMS_RTSP_MUXER_AAC_ASC_MAX 64u
#define ZMS_RTSP_MUXER_AV1_AV1C_MAX 256u

struct zms_rtp_muxer {
    zms_rtp_muxer_opts opts;
    zms_rtp_muxer_stats stats;
    zms_rtp_mux_on_rtp on_rtp;
    void *user;
    uint16_t v_seq;
    uint16_t a_seq;
    zms_egress_clock clk;
    int video_key_out;
    int catchup;
    int catchup_left;
    int abs_rtp_ts;
    uint32_t vod_seek_ms;
    struct rtsp_muxer_t *zms;
    int v_pid; /**< librtsp 视频 track id */
    int a_pid; /**< librtsp 音频 track id */
    int v_mid; /**< RTP payload type（视频） */
    int a_mid; /**< RTP payload type（音频） */
    zms_codec_id video_codec;
    /** codec 参数集：buf_pool 懒分配（按实际 codec 写入，避免每会话嵌 ~4.4KB） */
    uint8_t *hevc_vps;
    size_t hevc_vps_cap;
    size_t hevc_vps_len;
    uint8_t *hevc_sps;
    size_t hevc_sps_cap;
    size_t hevc_sps_len;
    uint8_t *hevc_pps;
    size_t hevc_pps_cap;
    size_t hevc_pps_len;
    uint8_t *h264_sps;
    size_t h264_sps_cap;
    size_t h264_sps_len;
    uint8_t *h264_pps;
    size_t h264_pps_cap;
    size_t h264_pps_len;
    uint8_t *h264_avcc;
    size_t h264_avcc_cap;
    size_t h264_avcc_len;
    uint8_t *hevc_hvcc;
    size_t hevc_hvcc_cap;
    size_t hevc_hvcc_len;
    uint8_t *aac_asc;
    size_t aac_asc_cap;
    size_t aac_asc_len;
    uint8_t *av1_av1c;
    size_t av1_av1c_cap;
    size_t av1_av1c_len;
};

/* --- codec 参数集 / extradata 缓存（rtp_muxer_paramsets.c）--- */
void mux_codec_cache_release(zms_rtp_muxer *m);
int mux_av1_try_cache_av1c_from_obu(zms_rtp_muxer *m, const uint8_t *obu, size_t len);
void mux_av1_store_av1c(zms_rtp_muxer *m, const uint8_t *av1c, size_t av1c_len);
void mux_hevc_cache_clear(zms_rtp_muxer *m);
void mux_hevc_cache_store(zms_rtp_muxer *m, const uint8_t *vps, size_t vps_len, const uint8_t *sps,
                          size_t sps_len, const uint8_t *pps, size_t pps_len);
int mux_hevc_refresh_params_from_au(zms_rtp_muxer *m, const uint8_t *annexb, size_t len);
void mux_hevc_store_hvcc(zms_rtp_muxer *m, const uint8_t *vcfg, size_t vcfg_len);
int mux_hevc_build_hvcc_fallback(zms_rtp_muxer *m);
void mux_h264_cache_store(zms_rtp_muxer *m, const uint8_t *sps, size_t sps_len, const uint8_t *pps,
                          size_t pps_len);
void mux_h264_store_avcc(zms_rtp_muxer *m, const uint8_t *vcfg, size_t vcfg_len);
int mux_h264_build_avcc_fallback(zms_rtp_muxer *m);
int mux_aac_store_asc(zms_rtp_muxer *m, const uint8_t *asc, size_t asc_len);

#endif /* ZMS_SRC_SESSION_RTSP_MUXER_INTERNAL_H */
