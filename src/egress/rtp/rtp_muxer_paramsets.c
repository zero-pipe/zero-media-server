/**
 * @file rtp_muxer_paramsets.c
 * @brief RTP muxer 的 codec 参数集 / extradata 缓存（H.264/H.265/AV1/AAC）。
 *
 * 缓冲走 buf_pool 懒分配，避免每个 play 会话嵌 ~4.4KB 固定数组。
 */
#include "egress/rtp/rtp_muxer_internal.h"

#include "ztk/ztk_errno.h"

#include "zms/media/container/flv/flv_tag_probe.h"
#include "zms/media/codec/h264/h264_over_rtmp.h"
#include "zms/media/codec/h265/h265_es.h"
#include "zms/media/codec/av1/av1_over_rtmp.h"
#include "zms/util/buf_pool.h"

#include <string.h>

static int mux_slot_store(uint8_t **data, size_t *cap, size_t *len, const uint8_t *src, size_t n,
                          size_t max_n)
{
    if (!data || !cap || !len || !src || n == 0 || n > max_n) {
        return 0;
    }
    if (!zms_buf_pool_slot_resize(data, cap, n)) {
        return 0;
    }
    memcpy(*data, src, n);
    *len = n;
    return 1;
}

static void mux_slot_len_clear(size_t *len)
{
    if (len) {
        *len = 0;
    }
}

void mux_codec_cache_release(zms_rtp_muxer *m)
{
    if (!m) {
        return;
    }
    zms_buf_pool_slot_clear(&m->hevc_vps, &m->hevc_vps_cap);
    zms_buf_pool_slot_clear(&m->hevc_sps, &m->hevc_sps_cap);
    zms_buf_pool_slot_clear(&m->hevc_pps, &m->hevc_pps_cap);
    zms_buf_pool_slot_clear(&m->h264_sps, &m->h264_sps_cap);
    zms_buf_pool_slot_clear(&m->h264_pps, &m->h264_pps_cap);
    zms_buf_pool_slot_clear(&m->h264_avcc, &m->h264_avcc_cap);
    zms_buf_pool_slot_clear(&m->hevc_hvcc, &m->hevc_hvcc_cap);
    zms_buf_pool_slot_clear(&m->aac_asc, &m->aac_asc_cap);
    zms_buf_pool_slot_clear(&m->av1_av1c, &m->av1_av1c_cap);
    m->hevc_vps_len = m->hevc_sps_len = m->hevc_pps_len = 0;
    m->h264_sps_len = m->h264_pps_len = m->h264_avcc_len = 0;
    m->hevc_hvcc_len = m->aac_asc_len = m->av1_av1c_len = 0;
}

/** 从带内 OBU Sequence Header 推导 av1c（AV1 RTP / ISO 14496-15）。 */
int mux_av1_try_cache_av1c_from_obu(zms_rtp_muxer *m, const uint8_t *obu, size_t len)
{
    uint8_t stack[ZMS_RTSP_MUXER_AV1_AV1C_MAX];
    int n;

    if (!m || m->av1_av1c_len > 0 || !obu || len < 2) {
        return 0;
    }
    if (!zms_av1_obu_has_sequence_header(obu, len)) {
        return 0;
    }
    n = zms_av1_extradata_from_obu(obu, len, stack, sizeof(stack), NULL, NULL);
    if (n <= 0) {
        return -1;
    }
    if (!mux_slot_store(&m->av1_av1c, &m->av1_av1c_cap, &m->av1_av1c_len, stack, (size_t)n,
                        ZMS_RTSP_MUXER_AV1_AV1C_MAX)) {
        return -1;
    }
    return 1;
}

void mux_av1_store_av1c(zms_rtp_muxer *m, const uint8_t *av1c, size_t av1c_len)
{
    if (!m) {
        return;
    }
    (void)mux_slot_store(&m->av1_av1c, &m->av1_av1c_cap, &m->av1_av1c_len, av1c, av1c_len,
                         ZMS_RTSP_MUXER_AV1_AV1C_MAX);
}

void mux_hevc_cache_clear(zms_rtp_muxer *m)
{
    if (!m) {
        return;
    }
    /* 仅清逻辑长度，保留池槽供下一 IDR 复用（jump_live / seek 路径依赖） */
    mux_slot_len_clear(&m->hevc_vps_len);
    mux_slot_len_clear(&m->hevc_sps_len);
    mux_slot_len_clear(&m->hevc_pps_len);
}

void mux_hevc_cache_store(zms_rtp_muxer *m, const uint8_t *vps, size_t vps_len, const uint8_t *sps,
                          size_t sps_len, const uint8_t *pps, size_t pps_len)
{
    if (!m) {
        return;
    }
    mux_hevc_cache_clear(m);
    if (vps && vps_len > 0) {
        (void)mux_slot_store(&m->hevc_vps, &m->hevc_vps_cap, &m->hevc_vps_len, vps, vps_len,
                             ZMS_RTSP_MUXER_HEVC_PARAM_MAX);
    }
    if (sps && sps_len > 0) {
        (void)mux_slot_store(&m->hevc_sps, &m->hevc_sps_cap, &m->hevc_sps_len, sps, sps_len,
                             ZMS_RTSP_MUXER_HEVC_PARAM_MAX);
    }
    if (pps && pps_len > 0) {
        (void)mux_slot_store(&m->hevc_pps, &m->hevc_pps_cap, &m->hevc_pps_len, pps, pps_len,
                             ZMS_RTSP_MUXER_HEVC_PARAM_MAX);
    }
}

int mux_hevc_refresh_params_from_au(zms_rtp_muxer *m, const uint8_t *annexb, size_t len)
{
    const uint8_t *vps = NULL, *sps = NULL, *pps = NULL;
    size_t vps_len = 0, sps_len = 0, pps_len = 0;

    if (!m || !annexb || len < 5) {
        return 0;
    }
    if (!zms_h265_annexb_extract_vps_sps_pps(annexb, len, &vps, &vps_len, &sps, &sps_len, &pps,
                                             &pps_len)) {
        return 0;
    }
    if (!sps || sps_len == 0 || !pps || pps_len == 0) {
        return 0;
    }
    mux_hevc_cache_store(m, vps, vps_len, sps, sps_len, pps, pps_len);
    return 1;
}

void mux_h264_cache_store(zms_rtp_muxer *m, const uint8_t *sps, size_t sps_len, const uint8_t *pps,
                          size_t pps_len)
{
    if (!m) {
        return;
    }
    mux_slot_len_clear(&m->h264_sps_len);
    mux_slot_len_clear(&m->h264_pps_len);
    if (sps && sps_len > 0) {
        (void)mux_slot_store(&m->h264_sps, &m->h264_sps_cap, &m->h264_sps_len, sps, sps_len,
                             ZMS_RTSP_MUXER_H264_PARAM_MAX);
    }
    if (pps && pps_len > 0) {
        (void)mux_slot_store(&m->h264_pps, &m->h264_pps_cap, &m->h264_pps_len, pps, pps_len,
                             ZMS_RTSP_MUXER_H264_PARAM_MAX);
    }
}

void mux_h264_store_avcc(zms_rtp_muxer *m, const uint8_t *vcfg, size_t vcfg_len)
{
    const uint8_t *avcc = NULL;
    size_t avcc_len = 0;

    if (!m) {
        return;
    }
    mux_slot_len_clear(&m->h264_avcc_len);
    if (!vcfg || vcfg_len == 0) {
        return;
    }
    if (!zms_rtmp_avc_extradata(vcfg, vcfg_len, &avcc, &avcc_len) || !avcc || avcc_len == 0) {
        return;
    }
    (void)mux_slot_store(&m->h264_avcc, &m->h264_avcc_cap, &m->h264_avcc_len, avcc, avcc_len,
                         ZMS_RTSP_MUXER_H264_AVCC_MAX);
}

int mux_h264_build_avcc_fallback(zms_rtp_muxer *m)
{
    uint8_t flv_buf[ZMS_RTSP_MUXER_H264_AVCC_MAX];
    size_t out_len = 0;

    if (!m || m->h264_avcc_len > 0) {
        return m && m->h264_avcc_len > 0;
    }
    if (m->h264_sps_len == 0 || m->h264_pps_len == 0 || !m->h264_sps || !m->h264_pps) {
        return 0;
    }
    if (zms_rtmp_avc_seq_header(m->h264_sps, m->h264_sps_len, m->h264_pps, m->h264_pps_len, flv_buf,
                                sizeof(flv_buf), &out_len) != ZTK_OK) {
        return 0;
    }
    mux_h264_store_avcc(m, flv_buf, out_len);
    return m->h264_avcc_len > 0;
}

void mux_hevc_store_hvcc(zms_rtp_muxer *m, const uint8_t *vcfg, size_t vcfg_len)
{
    const uint8_t *hvcc = NULL;
    size_t hvcc_len = 0;

    if (!m) {
        return;
    }
    mux_slot_len_clear(&m->hevc_hvcc_len);
    if (!vcfg || vcfg_len == 0) {
        return;
    }
    if (!zms_h265_video_config_hvcc(vcfg, vcfg_len, &hvcc, &hvcc_len) || !hvcc || hvcc_len == 0) {
        return;
    }
    (void)mux_slot_store(&m->hevc_hvcc, &m->hevc_hvcc_cap, &m->hevc_hvcc_len, hvcc, hvcc_len,
                         ZMS_RTSP_MUXER_H265_HVCC_MAX);
}

int mux_hevc_build_hvcc_fallback(zms_rtp_muxer *m)
{
    uint8_t stack[ZMS_RTSP_MUXER_H265_HVCC_MAX];
    size_t out_len = 0;

    if (!m || m->hevc_hvcc_len > 0) {
        return m && m->hevc_hvcc_len > 0;
    }
    if (m->hevc_sps_len == 0 || m->hevc_pps_len == 0 || !m->hevc_sps || !m->hevc_pps) {
        return 0;
    }
    if (zms_h265_hvcc_from_param_sets(m->hevc_vps_len ? m->hevc_vps : NULL, m->hevc_vps_len,
                                      m->hevc_sps, m->hevc_sps_len, m->hevc_pps, m->hevc_pps_len,
                                      stack, sizeof(stack), &out_len) != ZTK_OK) {
        return 0;
    }
    if (!mux_slot_store(&m->hevc_hvcc, &m->hevc_hvcc_cap, &m->hevc_hvcc_len, stack, out_len,
                        ZMS_RTSP_MUXER_H265_HVCC_MAX)) {
        return 0;
    }
    return 1;
}

int mux_aac_store_asc(zms_rtp_muxer *m, const uint8_t *asc, size_t asc_len)
{
    if (!m) {
        return 0;
    }
    return mux_slot_store(&m->aac_asc, &m->aac_asc_cap, &m->aac_asc_len, asc, asc_len,
                          ZMS_RTSP_MUXER_AAC_ASC_MAX);
}
