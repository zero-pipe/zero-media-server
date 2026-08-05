#include "zms/vod/vod_hls.h"
#include "zms/vod/io/vod_source.h"
#include "zms/vod/play/vod_play_lane.h"
#include "zms/vod/vod_flv_index.h"
#include "zms/egress/egress_sidecar_param_sets.h"
#include "zms/engine/gop/gop_queue.h"
#include "zms/media/codec/h264/h264_over_rtmp.h"
#include "zms/media/container/flv/flv_tag_probe.h"
#include "zms/media/codec/aac/aac_over_rtmp.h"
#include "zms/media/codec/h264/h264_es.h"
#include "zms/media/codec/h265/h265_es.h"
#include "zms/media/codec/codec_id.h"
#include "zms/media/container/container_dispatcher.h"
#include "zms/media/codec/aac/aac_config.h"
#include "mpeg4-hevc.h"
#include "ztk/util/log.h"
#include "ztk/thread/sync.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#include <sys/stat.h>
#define zms_vod_mkdir(path) _mkdir(path)
#define zms_vod_is_dir(st) (((st).st_mode & _S_IFDIR) != 0)
#else
#include <dirent.h>
#include <sys/stat.h>
#define zms_vod_mkdir(path) mkdir(path, 0755)
#define zms_vod_is_dir(st) S_ISDIR((st).st_mode)
#endif

/** m3u8/TS 格式代际：变更 mux 时间戳策略时递增，并清旧 .ts 缓存 */
#define ZMS_VOD_HLS_GEN 4
#define ZMS_VOD_HLS_GEN_LINE "# ZMS-HLS-GEN:4\r\n"

static ztk_mutex *g_vod_hls_mu;

static int file_mtime(const char *path, time_t *out)
{
    struct stat st;
    if (!path || !out || stat(path, &st) != 0) {
        return 0;
    }
    *out = st.st_mtime;
    return 1;
}

static int vod_hls_media_dir(const char *app, const char *stream, char *out, size_t out_cap)
{
    const zms_vod_config *cfg = zms_vod_config_get();
    char rel[ZMS_STREAM_MAX];
    char *slash;
    size_t n;

    if (!app || !stream || !out || out_cap == 0) {
        return 0;
    }
    strncpy(rel, stream, sizeof(rel) - 1);
    rel[sizeof(rel) - 1] = '\0';
    slash = strrchr(rel, '/');
    if (slash) {
        *slash = '\0';
    }
    if (rel[0]) {
        n = (size_t)snprintf(out, out_cap, "%s/%s/%s", cfg->record_root, app, rel);
    } else {
        n = (size_t)snprintf(out, out_cap, "%s/%s", cfg->record_root, app);
    }
    return n > 0 && n < out_cap;
}

static int playlist_file_valid(const char *path)
{
    struct stat st;
    return path && stat(path, &st) == 0 && st.st_size > 16;
}

static int segment_file_valid(const char *path)
{
    struct stat st;
    return path && stat(path, &st) == 0 && st.st_size >= 188;
}

static int rel_path_is_ts(const char *rel)
{
    size_t n;

    if (!rel || !zms_vod_rel_path_safe(rel)) {
        return 0;
    }
    n = strlen(rel);
    return n >= 4 && strcmp(rel + n - 3, ".ts") == 0;
}

int zms_vod_hls_resolve_playlist(const char *app, const char *stream, char *out, size_t out_cap)
{
    char m3u8_rel[ZMS_STREAM_MAX];

    if (!app || !stream || !out || out_cap == 0) {
        return 0;
    }
    if (!zms_vod_mp4_stream_to_m3u8_rel(stream, m3u8_rel, sizeof(m3u8_rel))) {
        return 0;
    }
    if (!zms_vod_resolve_rel_path(app, m3u8_rel, out, out_cap)) {
        return 0;
    }
    return playlist_file_valid(out);
}

int zms_vod_hls_has_static_pack(const char *app, const char *stream)
{
    char path[512];
    return zms_vod_hls_resolve_playlist(app, stream, path, sizeof(path));
}

int zms_vod_hls_resolve_segment_file(const char *app, const char *stream, const char *rel,
                                     char *out, size_t out_cap)
{
    char dir[512];
    char alt[128];
    unsigned long long seg_no;
    const char *base;
    const char *slash;
    size_t n;

    (void)alt;
    if (!app || !rel || !out || out_cap == 0 || !rel_path_is_ts(rel)) {
        return 0;
    }
    if (zms_vod_resolve_rel_path(app, rel, out, out_cap) && segment_file_valid(out)) {
        return 1;
    }

    slash = strrchr(rel, '/');
    base = slash ? slash + 1 : rel;
    seg_no = strtoull(base, NULL, 10);
    if (seg_no == 0 && base[0] != '0') {
        return 0;
    }
    if (!stream || !stream[0] || !vod_hls_media_dir(app, stream, dir, sizeof(dir))) {
        return 0;
    }
    n = (size_t)snprintf(out, out_cap, "%s/%llu.ts", dir, seg_no);
    if (n > 0 && n < out_cap && segment_file_valid(out)) {
        return 1;
    }
    snprintf(alt, sizeof(alt), "%llu.ts", seg_no);
    if (slash) {
        char rel_alt[ZMS_STREAM_MAX];
        size_t plen = (size_t)(slash - rel);
        if (plen + strlen(alt) + 2 >= sizeof(rel_alt)) {
            return 0;
        }
        memcpy(rel_alt, rel, plen);
        rel_alt[plen] = '/';
        strcpy(rel_alt + plen + 1, alt);
        if (zms_vod_resolve_rel_path(app, rel_alt, out, out_cap) && segment_file_valid(out)) {
            return 1;
        }
    }
    return 0;
}

static int vod_hls_ensure_dir(const char *dir)
{
    struct stat st;
    if (!dir || !dir[0]) {
        return 0;
    }
    if (stat(dir, &st) == 0) {
        return zms_vod_is_dir(st) ? 1 : 0;
    }
    return zms_vod_mkdir(dir) == 0;
}

int zms_vod_hls_playlist_path(const char *app, const char *stream, char *out, size_t out_cap)
{
    char m3u8_rel[ZMS_STREAM_MAX];

    if (!zms_vod_mp4_stream_to_m3u8_rel(stream, m3u8_rel, sizeof(m3u8_rel))) {
        return 0;
    }
    return zms_vod_resolve_rel_path(app, m3u8_rel, out, out_cap);
}

int zms_vod_hls_segment_path(const char *app, const char *stream, uint64_t seg_no, char *out,
                             size_t out_cap)
{
    char dir[512];
    size_t n;

    if (!vod_hls_media_dir(app, stream, dir, sizeof(dir))) {
        return 0;
    }
    n = (size_t)snprintf(out, out_cap, "%s/%llu.ts", dir, (unsigned long long)seg_no);
    return n > 0 && n < out_cap;
}

typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
    int done;
} vod_hls_seg_capture;

typedef struct {
    const zms_container_muxer_ops *ts_mux_ops;
    void *ts_mux;
    uint8_t *annexb;
    size_t annexb_cap;
    uint8_t *mux_buf;
    size_t mux_buf_cap;
    uint8_t *adts;
    size_t adts_cap;
    zms_sidecar_param_sets params;
    uint8_t hevc_esinfo[32];
    size_t hevc_esinfo_len;
    zms_aac_config aac;
    zms_codec_id video_codec;
    int have_aac;
    int sent_video_cfg;
    int sent_audio_cfg;
    int video_armed;
    int enable_audio;
    int logged_sync;
    uint32_t last_mux_dts_ms;
    vod_hls_seg_capture *cap;
} vod_hls_mux;

static int vod_hls_segment_cb(void *param, const void *data, size_t bytes, int64_t pts, int64_t dts,
                              int64_t duration)
{
    vod_hls_seg_capture *cap = (vod_hls_seg_capture *)param;
    (void)pts;
    (void)dts;
    (void)duration;
    if (!cap || cap->done || !data || bytes == 0) {
        return 0;
    }
    if (bytes > cap->cap) {
        uint8_t *p = (uint8_t *)realloc(cap->data, bytes);
        if (!p) {
            return -1;
        }
        cap->data = p;
        cap->cap = bytes;
    }
    memcpy(cap->data, data, bytes);
    cap->len = bytes;
    cap->done = 1;
    return 0;
}

static void vod_ts_mux_destroy(vod_hls_mux *m)
{
    if (!m) {
        return;
    }
    if (m->ts_mux_ops && m->ts_mux) {
        m->ts_mux_ops->destroy(m->ts_mux);
    }
    m->ts_mux = NULL;
    m->ts_mux_ops = NULL;
}

static void vod_ts_set_extradata(vod_hls_mux *m, int stream_type, const void *extra, size_t len)
{
    if (m && m->ts_mux_ops && m->ts_mux && m->ts_mux_ops->set_extradata) {
        m->ts_mux_ops->set_extradata(m->ts_mux, stream_type, extra, len);
    }
}

static void vod_ts_write_frame(vod_hls_mux *m, int stream_type, const void *data, size_t len,
                               int64_t pts_ms, int64_t dts_ms, int flags)
{
    if (m && m->ts_mux_ops && m->ts_mux && m->ts_mux_ops->write_frame) {
        (void)m->ts_mux_ops->write_frame(m->ts_mux, stream_type, data, len, pts_ms, dts_ms, flags);
    }
}

static void vod_ts_flush(vod_hls_mux *m, int stream_type, int64_t pts_ms)
{
    if (m && m->ts_mux_ops && m->ts_mux && m->ts_mux_ops->flush) {
        m->ts_mux_ops->flush(m->ts_mux, stream_type, pts_ms);
    }
}

/** PMT ES_info：Registration 'HEVC' + HEVC video descriptor */
static int vod_hls_hevc_ts_esinfo(const uint8_t *hvcc, size_t hvcc_len, uint8_t *out, size_t cap)
{
    struct mpeg4_hevc_t hevc;
    uint64_t v;
    uint64_t copied44;
    uint8_t *p;

    if (!hvcc || hvcc_len < 7 || !out || cap < 6 + 2 + 13) {
        return -1;
    }
    memset(&hevc, 0, sizeof(hevc));
    if (mpeg4_hevc_decoder_configuration_record_load(hvcc, hvcc_len, &hevc) <= 0) {
        return -1;
    }

    p = out;
    *p++ = 0x05;
    *p++ = 4;
    memcpy(p, "HEVC", 4);
    p += 4;

    *p++ = 0x38;
    *p++ = 13;
    *p++ = (uint8_t)((hevc.general_profile_space << 6) | (hevc.general_tier_flag << 5) |
                     (hevc.general_profile_idc & 0x1f));
    p[0] = (uint8_t)((hevc.general_profile_compatibility_flags >> 24) & 0xff);
    p[1] = (uint8_t)((hevc.general_profile_compatibility_flags >> 16) & 0xff);
    p[2] = (uint8_t)((hevc.general_profile_compatibility_flags >> 8) & 0xff);
    p[3] = (uint8_t)(hevc.general_profile_compatibility_flags & 0xff);
    p += 4;

    copied44 = (hevc.general_constraint_indicator_flags >> 4) & 0xFFFFFFFFFFFULL;
    v = (1ULL << 63) | (1ULL << 60) | (copied44 << 16) | ((uint64_t)hevc.general_level_idc << 8);
    p[0] = (uint8_t)((v >> 56) & 0xff);
    p[1] = (uint8_t)((v >> 48) & 0xff);
    p[2] = (uint8_t)((v >> 40) & 0xff);
    p[3] = (uint8_t)((v >> 32) & 0xff);
    p[4] = (uint8_t)((v >> 24) & 0xff);
    p[5] = (uint8_t)((v >> 16) & 0xff);
    p[6] = (uint8_t)((v >> 8) & 0xff);
    p[7] = (uint8_t)(v & 0xff);
    return (int)(p + 8 - out);
}

static void feed_video_cfg(vod_hls_mux *m, const uint8_t *cfg, size_t clen)
{
    zms_codec_id vc;

    if (!m || !cfg || clen < 2) {
        return;
    }
    vc = zms_flv_tag_video_codec(cfg, clen);
    m->video_codec = vc;
    if (vc == ZMS_CODEC_H264) {
        const uint8_t *avcc = NULL;
        size_t avcc_len = 0;

        (void)zms_sidecar_cache_rtmp_video_cfg(&m->params, cfg, clen);
        if (m->ts_mux && zms_rtmp_avc_extradata(cfg, clen, &avcc, &avcc_len)) {
            vod_ts_set_extradata(m, zms_codec_mpeg_psi(ZMS_CODEC_H264), avcc, avcc_len);
        }
    } else if (vc == ZMS_CODEC_H265) {
        const uint8_t *hvcc = NULL;
        size_t hvcc_len = 0;

        (void)zms_sidecar_cache_rtmp_video_cfg(&m->params, cfg, clen);
        if (m->ts_mux && zms_h265_video_config_hvcc(cfg, clen, &hvcc, &hvcc_len)) {
            m->hevc_esinfo_len = 0;
            if (hvcc && hvcc_len > 0) {
                int es_len =
                    vod_hls_hevc_ts_esinfo(hvcc, hvcc_len, m->hevc_esinfo, sizeof(m->hevc_esinfo));
                if (es_len > 0) {
                    m->hevc_esinfo_len = (size_t)es_len;
                }
            }
            if (m->hevc_esinfo_len > 0) {
                vod_ts_set_extradata(m, zms_codec_mpeg_psi(ZMS_CODEC_H265), m->hevc_esinfo,
                                     m->hevc_esinfo_len);
            } else {
                vod_ts_set_extradata(m, zms_codec_mpeg_psi(ZMS_CODEC_H265), hvcc, hvcc_len);
            }
        }
    }
}

static void feed_audio_cfg(vod_hls_mux *m, const uint8_t *cfg, size_t clen)
{
    const uint8_t *asc = NULL;
    size_t asc_len = 0;

    if (!m || !cfg || clen < 4) {
        return;
    }
    if (zms_flv_tag_audio_codec(cfg, clen) != ZMS_CODEC_AAC) {
        return;
    }
    if (cfg[1] == 0) {
        int sr = 0;
        int ch = 0;
        (void)zms_aac_parse_asc(cfg + 2, clen - 2, &sr, &ch);
        if (!zms_rtmp_aac_extradata(cfg, clen, &asc, &asc_len)) {
            return;
        }
        zms_aac_config_set_defaults(&m->aac, 44100, 2);
        if (zms_aac_config_load_asc(&m->aac, asc, asc_len)) {
            m->have_aac = 1;
        }
        if (m->ts_mux) {
            vod_ts_set_extradata(m, zms_codec_mpeg_psi(ZMS_CODEC_AAC), asc, asc_len);
        }
    }
}

static int aac_es_is_adts(const uint8_t *es, size_t es_len)
{
    return es && es_len >= 7 && es[0] == 0xff && (es[1] & 0xf0) == 0xf0;
}

static void feed_h264_video_es(vod_hls_mux *m, const uint8_t *annexb, size_t len, uint32_t ts,
                               int keyframe)
{
    const uint8_t *mux_ptr = annexb;
    size_t mux_len = len;

    if (!m || !m->ts_mux || !annexb || len < 4) {
        return;
    }
    if (!m->video_armed) {
        if (!keyframe) {
            return;
        }
        m->video_armed = 1;
        if (!m->logged_sync) {
            m->logged_sync = 1;
            ztk_info("vod hls: first H264 IDR at ts=%u", (unsigned)ts);
        }
    }
    if (keyframe && m->params.sps && m->params.pps) {
        size_t full_len = 0;
        if (zms_h264_annexb_prepend_sps_pps(m->params.sps, m->params.sps_len, m->params.pps,
                                            m->params.pps_len, annexb, len, m->mux_buf,
                                            m->mux_buf_cap, &full_len) == ZTK_OK &&
            full_len) {
            mux_ptr = m->mux_buf;
            mux_len = full_len;
        }
    }
    m->last_mux_dts_ms = ts;
    vod_ts_write_frame(m, zms_codec_mpeg_psi(ZMS_CODEC_H264), mux_ptr, mux_len, (int64_t)ts,
                       (int64_t)ts, keyframe ? ZMS_CONTAINER_MUX_FLAG_KEYFRAME : 0);
}

static void feed_h265_video_es(vod_hls_mux *m, const uint8_t *annexb, size_t len, uint32_t ts,
                               int keyframe)
{
    const uint8_t *mux_ptr = annexb;
    size_t mux_len = len;
    int sync;

    if (!m || !m->ts_mux || !annexb || len < 4) {
        return;
    }

    sync = keyframe || zms_h265_annexb_is_sync_key(annexb, len);
    if (!m->video_armed) {
        if (!sync) {
            return;
        }
        m->video_armed = 1;
        if (!m->logged_sync) {
            m->logged_sync = 1;
            ztk_info("vod hls: first H265 sync at ts=%u", (unsigned)ts);
        }
    }

    if (sync && m->params.sps_len > 0 && m->params.pps_len > 0) {
        size_t full_len = 0;
        if (zms_h265_annexb_build_rtp_au(m->params.vps_len ? m->params.vps : NULL,
                                         m->params.vps_len, m->params.sps, m->params.sps_len,
                                         m->params.pps, m->params.pps_len, annexb, len, 1,
                                         m->mux_buf, m->mux_buf_cap, &full_len) == ZTK_OK &&
            full_len > 0) {
            mux_ptr = m->mux_buf;
            mux_len = full_len;
        }
    } else {
        size_t vcl_len = 0;
        if (zms_h265_annexb_copy_vcl(annexb, len, m->mux_buf, m->mux_buf_cap, &vcl_len) == ZTK_OK &&
            vcl_len > 0) {
            mux_ptr = m->mux_buf;
            mux_len = vcl_len;
        }
    }

    m->last_mux_dts_ms = ts;
    vod_ts_write_frame(m, zms_codec_mpeg_psi(ZMS_CODEC_H265), mux_ptr, mux_len, (int64_t)ts,
                       (int64_t)ts, sync ? ZMS_CONTAINER_MUX_FLAG_KEYFRAME : 0);
}

static void feed_audio_es(vod_hls_mux *m, const uint8_t *es, size_t es_len, uint32_t ts)
{
    const uint8_t *mux_ptr = es;
    size_t mux_len = es_len;

    if (!m || !m->ts_mux || !m->enable_audio || !es || es_len == 0 || !m->video_armed) {
        return;
    }
    if (!aac_es_is_adts(es, es_len)) {
        int hdr;
        if (!m->have_aac || es_len > 0x1fff) {
            return;
        }
        hdr = zms_aac_config_adts_header(&m->aac, (uint16_t)es_len, m->adts, m->adts_cap);
        if (hdr < 7 || (size_t)hdr + es_len > m->adts_cap) {
            return;
        }
        memcpy(m->adts + hdr, es, es_len);
        mux_ptr = m->adts;
        mux_len = (size_t)hdr + es_len;
    }
    m->last_mux_dts_ms = ts;
    vod_ts_write_frame(m, zms_codec_mpeg_psi(ZMS_CODEC_AAC), mux_ptr, mux_len, (int64_t)ts,
                       (int64_t)ts, 0);
}

static void vod_hls_mux_free(vod_hls_mux *m)
{
    int psi_video;

    if (!m) {
        return;
    }
    psi_video = zms_codec_mpeg_psi(m->video_codec);
    if (!psi_video) {
        psi_video = zms_codec_mpeg_psi(ZMS_CODEC_H264);
    }
    if (m->ts_mux && m->video_armed) {
        vod_ts_flush(m, psi_video, (int64_t)m->last_mux_dts_ms);
    }
    vod_ts_mux_destroy(m);
    free(m->annexb);
    free(m->mux_buf);
    free(m->adts);
    zms_sidecar_param_sets_clear(&m->params);
    memset(m, 0, sizeof(*m));
}

static ztk_err_t write_segment_file(const char *path, const void *data, size_t len)
{
    char tmp[560];
    FILE *fp;
    size_t n;

    n = (size_t)snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (n == 0 || n >= sizeof(tmp)) {
        return ZTK_ERR_INVALID;
    }
    fp = fopen(tmp, "wb");
    if (!fp) {
        return ZTK_ERR_IO;
    }
    if (fwrite(data, 1, len, fp) != len) {
        fclose(fp);
        remove(tmp);
        return ZTK_ERR_IO;
    }
    fclose(fp);
    remove(path);
    if (rename(tmp, path) != 0) {
        remove(tmp);
        return ZTK_ERR_IO;
    }
    return ZTK_OK;
}

static ztk_err_t generate_segment(zms_media_source *src, uint64_t seg_no, ztk_poller *poller,
                                  const char *out_path)
{
    const zms_vod_flv_index *idx;
    zms_vod_play_lane *lane = NULL;
    zms_vod_buffer_reader *rd;
    zms_vod_reader *reader;
    vod_hls_mux mux;
    vod_hls_seg_capture cap = {0};
    uint64_t start_ms;
    uint64_t dur_ms;
    size_t seg_count;
    int is_last;
    int idle;
    int loops;

    idx = zms_vod_source_flv_index(src);
    if (!idx || idx->count == 0) {
        return ZTK_ERR_INVALID;
    }
    seg_count = idx->count;
    if (seg_no >= seg_count) {
        return ZTK_ERR_INVALID;
    }

    dur_ms = zms_vod_source_duration_ms(src);
    start_ms = (uint64_t)(idx->times[seg_no] * 1000.0);
    is_last = (seg_no + 1 >= seg_count);

    lane = zms_vod_play_lane_open(src, poller);
    if (!lane) {
        return ZTK_ERR_NOMEM;
    }
    (void)zms_vod_play_lane_prepare(lane, start_ms);
    zms_vod_play_lane_seek_ms(lane, start_ms);
    zms_vod_play_lane_prefill(lane);
    zms_vod_play_lane_align_reader(lane);
    rd = zms_vod_play_lane_buffer_reader(lane);
    reader = zms_vod_play_lane_reader(lane);
    if (!rd || !reader) {
        zms_vod_play_lane_close(lane);
        return ZTK_ERR_INVALID;
    }

    memset(&mux, 0, sizeof(mux));
    mux.enable_audio = src->has_audio;
    mux.cap = &cap;
    mux.annexb_cap = 256 * 1024;
    mux.mux_buf_cap = mux.annexb_cap;
    mux.adts_cap = 8 * 1024;
    mux.annexb = (uint8_t *)malloc(mux.annexb_cap);
    mux.mux_buf = (uint8_t *)malloc(mux.mux_buf_cap);
    mux.adts = (uint8_t *)malloc(mux.adts_cap);
    if (!mux.annexb || !mux.mux_buf || !mux.adts) {
        vod_hls_mux_free(&mux);
        free(cap.data);
        zms_vod_play_lane_close(lane);
        return ZTK_ERR_NOMEM;
    }
    /* duration=0：每个关键帧切一片；seek 到第 N 个关键帧后只保留第一个 */
    {
        zms_container_mux_opts mcfg;

        memset(&mcfg, 0, sizeof(mcfg));
        mcfg.id = ZMS_CONTAINER_MPEGTS;
        mcfg.segment_duration_ms = 0;
        mcfg.on_segment = vod_hls_segment_cb;
        mcfg.user = &cap;
        mux.ts_mux_ops = zms_container_muxer_find(ZMS_CONTAINER_MPEGTS);
        mux.ts_mux =
            mux.ts_mux_ops && mux.ts_mux_ops->create ? mux.ts_mux_ops->create(&mcfg) : NULL;
    }
    if (!mux.ts_mux) {
        vod_hls_mux_free(&mux);
        free(cap.data);
        zms_vod_play_lane_close(lane);
        return ZTK_ERR_NOMEM;
    }

    idle = 0;
    loops = 0;
    while (loops < 500000 && idle < 128) {
        zms_gop_slot slot;
        int n;
        int got_slot = 0;

        if (!is_last && cap.done) {
            break;
        }

        for (n = 0; n < 32; ++n) {
            if (zms_vod_reader_pump(reader) <= 0) {
                break;
            }
        }
        if (!mux.sent_video_cfg) {
            size_t clen = 0;
            const uint8_t *cfg = zms_vod_play_lane_video_config(lane, &clen);
            if (cfg && clen) {
                feed_video_cfg(&mux, cfg, clen);
                mux.sent_video_cfg = 1;
            }
        }
        if (!mux.sent_audio_cfg && mux.enable_audio) {
            size_t clen = 0;
            const uint8_t *cfg = zms_vod_play_lane_audio_config(lane, &clen);
            if (cfg && clen) {
                feed_audio_cfg(&mux, cfg, clen);
                mux.sent_audio_cfg = 1;
            } else if (clen == 0) {
                mux.sent_audio_cfg = 1;
            }
        }

        n = 0;
        while (n < 64 && zms_vod_buffer_reader_peek_muxed(rd, &slot) > 0) {
            if (!is_last && cap.done) {
                break;
            }
            if (!slot.data || slot.len == 0) {
                zms_vod_buffer_reader_advance(rd);
                continue;
            }
            if (slot.track == ZMS_TRACK_VIDEO) {
                int sync = slot.keyframe;

                if (slot.codec == ZMS_CODEC_H265) {
                    sync = sync || zms_h265_annexb_is_sync_key(slot.data, slot.len);
                }
                if (!mux.video_armed && !sync) {
                    zms_vod_buffer_reader_advance(rd);
                    continue;
                }
                if (slot.codec == ZMS_CODEC_H265) {
                    feed_h265_video_es(&mux, slot.data, slot.len, slot.dts_ms, slot.keyframe);
                } else {
                    feed_h264_video_es(&mux, slot.data, slot.len, slot.dts_ms, slot.keyframe);
                }
            } else if (slot.track == ZMS_TRACK_AUDIO) {
                if (!mux.video_armed) {
                    zms_vod_buffer_reader_advance(rd);
                    continue;
                }
                feed_audio_es(&mux, slot.data, slot.len, slot.dts_ms);
            }
            zms_vod_buffer_reader_advance(rd);
            idle = 0;
            got_slot = 1;
            n++;
            if (!is_last && cap.done) {
                break;
            }
        }
        if (!got_slot) {
            idle++;
        }
        loops++;
    }

    if (is_last && !cap.done && mux.video_armed) {
        int psi_video = zms_codec_mpeg_psi(mux.video_codec);
        if (!psi_video) {
            psi_video = zms_codec_mpeg_psi(ZMS_CODEC_H264);
        }
        vod_ts_flush(&mux, psi_video, (int64_t)mux.last_mux_dts_ms);
    }

    vod_hls_mux_free(&mux);
    zms_vod_play_lane_close(lane);

    if (!cap.done || cap.len < 188) {
        free(cap.data);
        ztk_warn("vod hls: segment generate failed %s/%s seg=%llu start=%llu ms", src->app,
                 src->stream, (unsigned long long)seg_no, (unsigned long long)start_ms);
        return ZTK_ERR_INVALID;
    }
    if (write_segment_file(out_path, cap.data, cap.len) != ZTK_OK) {
        free(cap.data);
        return ZTK_ERR_IO;
    }
    free(cap.data);
    ztk_info("vod hls: generated %s (%u bytes, start=%llu ms, last=%d)", out_path,
             (unsigned)cap.len, (unsigned long long)start_ms, is_last);
    (void)dur_ms;
    return ZTK_OK;
}

static void vod_hls_purge_segments(const char *dir)
{
#if defined(_WIN32)
    char pattern[560];
    struct _finddata_t fd;
    intptr_t h;

    if (!dir || !dir[0]) {
        return;
    }
    snprintf(pattern, sizeof(pattern), "%s/*.ts", dir);
    h = _findfirst(pattern, &fd);
    if (h == -1) {
        return;
    }
    do {
        char path[560];
        if (fd.name[0] == '.') {
            continue;
        }
        snprintf(path, sizeof(path), "%s/%s", dir, fd.name);
        remove(path);
    } while (_findnext(h, &fd) == 0);
    _findclose(h);
#else
    DIR *d;
    struct dirent *ent;

    if (!dir || !dir[0]) {
        return;
    }
    d = opendir(dir);
    if (!d) {
        return;
    }
    while ((ent = readdir(d)) != NULL) {
        char path[560];
        size_t nlen;
        if (ent->d_name[0] == '.') {
            continue;
        }
        nlen = strlen(ent->d_name);
        if (nlen < 4 || strcmp(ent->d_name + nlen - 3, ".ts") != 0) {
            continue;
        }
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        remove(path);
    }
    closedir(d);
#endif
}

static int playlist_cache_valid(const char *m3u8, time_t mp4_t)
{
    char buf[4096];
    FILE *fp;
    time_t m3u8_t;
    size_t n;

    if (!segment_file_valid(m3u8) || !file_mtime(m3u8, &m3u8_t) || m3u8_t < mp4_t) {
        return 0;
    }
    fp = fopen(m3u8, "rb");
    if (!fp) {
        return 0;
    }
    n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    if (n == 0) {
        return 0;
    }
    buf[n] = '\0';
    return strstr(buf, ZMS_VOD_HLS_GEN_LINE) != NULL;
}

static unsigned zms_hls_target_duration_sec(double sec)
{
    unsigned u;

    if (sec < 1.0) {
        return 1u;
    }
    u = (unsigned)sec;
    return (sec > (double)u) ? u + 1u : u;
}

static ztk_err_t build_playlist_file(zms_media_source *src, const char *path)
{
    const zms_vod_flv_index *idx;
    char tmp[560];
    FILE *fp;
    double dur_sec;
    double max_inf = 0.0;
    size_t i;

    idx = zms_vod_source_flv_index(src);
    if (!idx || idx->count == 0) {
        return ZTK_ERR_INVALID;
    }
    dur_sec = zms_vod_source_duration_ms(src) / 1000.0;
    if (dur_sec <= 0.0) {
        return ZTK_ERR_INVALID;
    }

    for (i = 0; i < idx->count; ++i) {
        double t0 = idx->times[i];
        double t1 = (i + 1 < idx->count) ? idx->times[i + 1] : dur_sec;
        double inf = t1 - t0;
        if (inf > max_inf) {
            max_inf = inf;
        }
    }
    if (max_inf < 1.0) {
        max_inf = 1.0;
    }

    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    fp = fopen(tmp, "wb");
    if (!fp) {
        return ZTK_ERR_IO;
    }
    fprintf(fp,
            "#EXTM3U\r\n" ZMS_VOD_HLS_GEN_LINE "#EXT-X-VERSION:3\r\n#EXT-X-ALLOW-CACHE:YES\r\n");
    fprintf(fp, "#EXT-X-TARGETDURATION:%u\r\n", zms_hls_target_duration_sec(max_inf));
    fprintf(fp, "#EXT-X-PLAYLIST-TYPE:VOD\r\n#EXT-X-MEDIA-SEQUENCE:0\r\n");
    for (i = 0; i < idx->count; ++i) {
        double t0 = idx->times[i];
        double t1 = (i + 1 < idx->count) ? idx->times[i + 1] : dur_sec;
        double inf = t1 - t0;
        if (inf < 0.001) {
            inf = 0.001;
        }
        fprintf(fp, "#EXTINF:%.3f,\r\n%zu.ts\r\n", inf, i);
    }
    fprintf(fp, "#EXT-X-ENDLIST\r\n");
    fclose(fp);
    remove(path);
    if (rename(tmp, path) != 0) {
        remove(tmp);
        return ZTK_ERR_IO;
    }
    ztk_info("vod hls: playlist %s segs=%zu target=%.1fs dur=%.1fs", path, idx->count, max_inf,
             dur_sec);
    return ZTK_OK;
}

ztk_err_t zms_vod_hls_ensure_playlist(zms_media_source *src)
{
    char mp4[512];
    char m3u8[512];
    char dir[512];
    time_t mp4_t = 0;
    ztk_err_t err;

    if (!src) {
        return ZTK_ERR_INVALID;
    }

    if (zms_vod_hls_has_static_pack(src->app, src->stream)) {
        char static_m3u8[512];
        if (zms_vod_hls_resolve_playlist(src->app, src->stream, static_m3u8, sizeof(static_m3u8))) {
            ztk_info("vod hls: static playlist app=%s stream=%s path=%s", src->app, src->stream,
                     static_m3u8);
        }
        return ZTK_OK;
    }

    if (!zms_vod_source_file_path(src, mp4, sizeof(mp4)) &&
        !zms_vod_resolve_file_path(src->app, src->stream, mp4, sizeof(mp4))) {
        return ZTK_ERR_INVALID;
    }
    if (!zms_vod_hls_playlist_path(src->app, src->stream, m3u8, sizeof(m3u8))) {
        return ZTK_ERR_INVALID;
    }
    if (!vod_hls_media_dir(src->app, src->stream, dir, sizeof(dir))) {
        return ZTK_ERR_INVALID;
    }
    if (!vod_hls_ensure_dir(dir)) {
        return ZTK_ERR_IO;
    }

    if (file_mtime(mp4, &mp4_t) && playlist_cache_valid(m3u8, mp4_t)) {
        return ZTK_OK;
    }

    if (!g_vod_hls_mu) {
        g_vod_hls_mu = ztk_mutex_create(0);
    }
    if (!g_vod_hls_mu) {
        return ZTK_ERR_NOMEM;
    }
    ztk_mutex_lock(g_vod_hls_mu);
    if (file_mtime(mp4, &mp4_t) && playlist_cache_valid(m3u8, mp4_t)) {
        ztk_mutex_unlock(g_vod_hls_mu);
        return ZTK_OK;
    }
    vod_hls_purge_segments(dir);
    ztk_info("vod hls: rebuild playlist (gen=%d), purged cached .ts under %s", ZMS_VOD_HLS_GEN,
             dir);
    err = build_playlist_file(src, m3u8);
    ztk_mutex_unlock(g_vod_hls_mu);
    return err;
}

ztk_err_t zms_vod_hls_ensure_segment(zms_media_source *src, uint64_t seg_no, ztk_poller *poller)
{
    char mp4[512];
    char ts_path[512];
    char dir[512];
    time_t mp4_t = 0;
    time_t ts_t = 0;
    ztk_err_t err;

    if (!src) {
        return ZTK_ERR_INVALID;
    }

    if (zms_vod_hls_has_static_pack(src->app, src->stream)) {
        if (zms_vod_hls_segment_path(src->app, src->stream, seg_no, ts_path, sizeof(ts_path)) &&
            segment_file_valid(ts_path)) {
            return ZTK_OK;
        }
        return ZTK_ERR_INVALID;
    }

    if (!zms_vod_source_file_path(src, mp4, sizeof(mp4)) &&
        !zms_vod_resolve_file_path(src->app, src->stream, mp4, sizeof(mp4))) {
        return ZTK_ERR_INVALID;
    }
    if (!zms_vod_hls_segment_path(src->app, src->stream, seg_no, ts_path, sizeof(ts_path))) {
        return ZTK_ERR_INVALID;
    }
    if (!vod_hls_media_dir(src->app, src->stream, dir, sizeof(dir))) {
        return ZTK_ERR_INVALID;
    }
    if (!vod_hls_ensure_dir(dir)) {
        return ZTK_ERR_IO;
    }
    if (zms_vod_hls_ensure_playlist(src) != ZTK_OK) {
        return ZTK_ERR_INVALID;
    }

    if (segment_file_valid(ts_path) && file_mtime(mp4, &mp4_t) && file_mtime(ts_path, &ts_t) &&
        ts_t >= mp4_t) {
        return ZTK_OK;
    }

    if (!g_vod_hls_mu) {
        g_vod_hls_mu = ztk_mutex_create(0);
    }
    if (!g_vod_hls_mu) {
        return ZTK_ERR_NOMEM;
    }
    ztk_mutex_lock(g_vod_hls_mu);
    if (segment_file_valid(ts_path) && file_mtime(mp4, &mp4_t) && file_mtime(ts_path, &ts_t) &&
        ts_t >= mp4_t) {
        ztk_mutex_unlock(g_vod_hls_mu);
        return ZTK_OK;
    }
    err = generate_segment(src, seg_no, poller, ts_path);
    ztk_mutex_unlock(g_vod_hls_mu);
    return err;
}
