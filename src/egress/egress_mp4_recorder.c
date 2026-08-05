/**
 * @file egress_mp4_recorder.c
 * @brief 直播 MP4 滚动录像：gop_reader → annexb_to_mp4 → mov_writer。
 *
 * 路径规则见 egress_mp4_recorder.h（{root}/{stream}/{date}/{HHmmss}.mp4）。
 */
#include "zms/egress/egress_mp4_recorder.h"
#include "zms/ops/api/webhook/webhook_client.h"
#include "zms/engine/media_event.h"
#include "zms/media/codec/codec_id.h"
#include "zms/media/codec/h264/h264_config.h"
#include "zms/media/codec/h264/h264_es.h"
#include "zms/media/codec/h264/h264_over_rtmp.h"
#include "zms/media/codec/h265/h265_config.h"
#include "zms/media/codec/h265/h265_es.h"
#include "zms/media/codec/aac/aac_config.h"
#include "zms/media/codec/aac/aac_over_rtmp.h"
#include "zms/media/container/flv/flv_tag_probe.h"
#include "zms/engine/media_clock.h"
#include "zms/engine/gop/gop_queue.h"
#include "vod/io/mov_file_buffer.h"
#include "mov-writer.h"
#include "mov-format.h"
#include "ztk/util/log.h"
#include "ztk/util/timer.h"
#include "ztk/thread/sync.h"
#include "ztk/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#define zms_mp4_mkdir(p) _mkdir(p)
#define zms_mp4_access(p) _access((p), 0)
#else
#include <unistd.h>
#define zms_mp4_mkdir(p) mkdir((p), 0755)
#define zms_mp4_access(p) access((p), F_OK)
#endif

#define ZMS_MP4_REC_TICK_MS 20
#define ZMS_MP4_REC_TICK_FRAMES 32
#define ZMS_MP4_PATH_MAX 768

typedef struct zms_mp4_recorder {
    zms_media_source *src;
    zms_gop_reader *reader;
    ztk_mutex *mu;
    ztk_timer *timer;
    ztk_poller *timer_poller;
    int closing;

    mov_writer_t *mov;
    FILE *fp;
    char file_path[ZMS_MP4_PATH_MAX];
    char folder[ZMS_MP4_PATH_MAX];
    char file_name[256];
    time_t start_unix;
    uint32_t start_dts_ms;
    uint32_t last_dts_ms;
    int have_start_dts;
    int video_track;
    int audio_track;
    int video_armed;
    int sent_video_cfg;
    int sent_audio_cfg;

    zms_avc_config avc;
    zms_hevc_config hevc;
    zms_aac_config aac;
    int have_aac;
    zms_codec_id video_codec;

    uint8_t *mux_buf;
    size_t mux_buf_cap;
    zms_mux_av_timeline mux_av;
    int max_second;
} zms_mp4_recorder;

static char g_record_root[ZMS_CFG_PATH_MAX];
static int g_mp4_max_second = 180;

void zms_mp4_recorder_configure(const zms_record_config *rec)
{
    if (!rec) {
        return;
    }
    if (rec->root[0]) {
        strncpy(g_record_root, rec->root, sizeof(g_record_root) - 1);
        g_record_root[sizeof(g_record_root) - 1] = '\0';
    } else {
        strncpy(g_record_root, "./www/record", sizeof(g_record_root) - 1);
    }
    g_mp4_max_second = rec->mp4_max_second > 0 ? rec->mp4_max_second : 180;
}

const char *zms_mp4_recorder_root(void)
{
    return g_record_root[0] ? g_record_root : "./www/record";
}

static void path_norm_slashes(char *p)
{
    if (!p) {
        return;
    }
    for (; *p; ++p) {
        if (*p == '\\') {
            *p = '/';
        }
    }
}

static int path_has_dotdot(const char *p)
{
    const char *s;

    if (!p) {
        return 1;
    }
    for (s = p; *s; ++s) {
        if (s[0] == '.' && s[1] == '.' &&
            (s == p || s[-1] == '/' || s[-1] == '\\') &&
            (s[2] == '\0' || s[2] == '/' || s[2] == '\\')) {
            return 1;
        }
    }
    return 0;
}

int zms_mp4_recorder_path_under_root(const char *file_path)
{
    char root[ZMS_CFG_PATH_MAX];
    char www_root[ZMS_CFG_PATH_MAX];
    char path[ZMS_MP4_PATH_MAX];
    size_t root_len;

    if (!file_path || !file_path[0] || path_has_dotdot(file_path)) {
        return 0;
    }
    strncpy(path, file_path, sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';
    path_norm_slashes(path);

    strncpy(root, zms_mp4_recorder_root(), sizeof(root) - 1);
    root[sizeof(root) - 1] = '\0';
    path_norm_slashes(root);
    while (root[0] && (root[strlen(root) - 1] == '/' || root[strlen(root) - 1] == '\\')) {
        root[strlen(root) - 1] = '\0';
    }
    root_len = strlen(root);
    if (root_len > 0 && strncmp(path, root, root_len) == 0 &&
        (path[root_len] == '\0' || path[root_len] == '/')) {
        return 1;
    }

    /* 兼容历史点播：./www/vod/...；录像：./www/record/... */
    strncpy(www_root, "./www", sizeof(www_root) - 1);
    www_root[sizeof(www_root) - 1] = '\0';
    root_len = strlen(www_root);
    if (strncmp(path, www_root, root_len) == 0 &&
        (path[root_len] == '\0' || path[root_len] == '/')) {
        return 1;
    }
    return 0;
}

static int mkdir_p(const char *dir)
{
    char tmp[ZMS_MP4_PATH_MAX];
    size_t i;
    size_t n;

    if (!dir || !dir[0]) {
        return -1;
    }
    n = strlen(dir);
    if (n >= sizeof(tmp)) {
        return -1;
    }
    memcpy(tmp, dir, n + 1);
    path_norm_slashes(tmp);
    for (i = 1; i < n; ++i) {
        if (tmp[i] != '/') {
            continue;
        }
        tmp[i] = '\0';
        if (tmp[0] && zms_mp4_mkdir(tmp) != 0) {
#if defined(_WIN32)
            /* EEXIST ok */
#else
            /* ignore existing */
#endif
        }
        tmp[i] = '/';
    }
    if (zms_mp4_mkdir(tmp) != 0) {
        /* may already exist */
    }
    return 0;
}

static int aac_is_adts(const uint8_t *es, size_t es_len)
{
    return es && es_len >= 7 && es[0] == 0xff && (es[1] & 0xf0) == 0xf0;
}

static size_t aac_adts_hdr_len(const uint8_t *p, size_t left)
{
    if (!aac_is_adts(p, left)) {
        return 0;
    }
    return (p[1] & 0x01) ? 7u : 9u;
}

static int ensure_mux_buf(zms_mp4_recorder *rec, size_t need)
{
    uint8_t *p;

    if (!rec) {
        return 0;
    }
    if (rec->mux_buf_cap >= need) {
        return 1;
    }
    need = need < 65536u ? 65536u : need;
    p = (uint8_t *)realloc(rec->mux_buf, need);
    if (!p) {
        return 0;
    }
    rec->mux_buf = p;
    rec->mux_buf_cap = need;
    return 1;
}

static void close_file_fire_hook(zms_mp4_recorder *rec)
{
    struct stat st;
    int64_t file_size = 0;
    float time_len = 0.f;
    char app[ZMS_APP_MAX];
    char stream[ZMS_STREAM_MAX];

    if (!rec || !rec->mov) {
        return;
    }

    mov_writer_destroy(rec->mov);
    rec->mov = NULL;
    if (rec->fp) {
        fflush(rec->fp);
        fclose(rec->fp);
        rec->fp = NULL;
    }

    if (rec->file_path[0] && stat(rec->file_path, &st) == 0) {
        file_size = (int64_t)st.st_size;
    }
    if (rec->have_start_dts && rec->last_dts_ms >= rec->start_dts_ms) {
        time_len = (float)(rec->last_dts_ms - rec->start_dts_ms) / 1000.f;
    }
    if (rec->src) {
        strncpy(app, rec->src->app, sizeof(app) - 1);
        app[sizeof(app) - 1] = '\0';
        strncpy(stream, rec->src->stream, sizeof(stream) - 1);
        stream[sizeof(stream) - 1] = '\0';
    } else {
        app[0] = '\0';
        stream[0] = '\0';
    }

    if (file_size > 0 && rec->file_path[0]) {
        zms_webhook_on_record_mp4(app, stream, rec->file_name, rec->file_path, rec->folder,
                                  file_size, (int64_t)rec->start_unix, time_len);
        ztk_info("MP4 record done: %s size=%lld time_len=%.2f", rec->file_path,
                 (long long)file_size, (double)time_len);
    }

    rec->file_path[0] = '\0';
    rec->folder[0] = '\0';
    rec->file_name[0] = '\0';
    rec->video_track = -1;
    rec->audio_track = -1;
    rec->video_armed = 0;
    rec->sent_video_cfg = 0;
    rec->sent_audio_cfg = 0;
    rec->have_start_dts = 0;
    zms_mux_av_timeline_reset(&rec->mux_av);
}

static void path_sanitize_stream(char *stream, size_t cap)
{
    size_t i;
    if (!stream || cap == 0) {
        return;
    }
    for (i = 0; stream[i] && i + 1 < cap; ++i) {
        char c = stream[i];
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
            c == '>' || c == '|') {
            stream[i] = '_';
        }
    }
    if (strstr(stream, "..") != NULL) {
        strncpy(stream, "stream", cap - 1);
        stream[cap - 1] = '\0';
    }
}

static int open_new_file(zms_mp4_recorder *rec)
{
    time_t now;
    struct tm tm_now;
    char date[32];
    char stamp[32];
    char root[ZMS_CFG_PATH_MAX];
    char stream[ZMS_STREAM_MAX];
    int n;
    int seq;

    if (!rec || !rec->src) {
        return -1;
    }
    strncpy(stream, rec->src->stream[0] ? rec->src->stream : "stream", sizeof(stream) - 1);
    stream[sizeof(stream) - 1] = '\0';
    path_sanitize_stream(stream, sizeof(stream));

    now = time(NULL);
    rec->start_unix = now;
#if defined(_WIN32)
    localtime_s(&tm_now, &now);
#else
    localtime_r(&now, &tm_now);
#endif
    strftime(date, sizeof(date), "%Y-%m-%d", &tm_now);
    strftime(stamp, sizeof(stamp), "%Y%m%d%H%M%S", &tm_now);

    strncpy(root, zms_mp4_recorder_root(), sizeof(root) - 1);
    root[sizeof(root) - 1] = '\0';
    path_norm_slashes(root);
    while (root[0] && root[strlen(root) - 1] == '/') {
        root[strlen(root) - 1] = '\0';
    }

    /* {root}/{stream}/{YYYY-MM-dd}/ */
    n = snprintf(rec->folder, sizeof(rec->folder), "%s/%s/%s", root, stream, date);
    if (n < 0 || (size_t)n >= sizeof(rec->folder)) {
        return -1;
    }
    if (mkdir_p(rec->folder) != 0) {
        ztk_warn("MP4 record mkdir failed: %s", rec->folder);
    }

    /* {YYYYMMDDHHMMSS}.mp4；同秒冲突则 _1 _2 … */
    for (seq = 0; seq < 100; ++seq) {
        if (seq == 0) {
            n = snprintf(rec->file_name, sizeof(rec->file_name), "%s.mp4", stamp);
        } else {
            n = snprintf(rec->file_name, sizeof(rec->file_name), "%s_%d.mp4", stamp, seq);
        }
        if (n < 0 || (size_t)n >= sizeof(rec->file_name)) {
            return -1;
        }
        n = snprintf(rec->file_path, sizeof(rec->file_path), "%s/%s", rec->folder, rec->file_name);
        if (n < 0 || (size_t)n >= sizeof(rec->file_path)) {
            return -1;
        }
        if (zms_mp4_access(rec->file_path) != 0) {
            break;
        }
    }
    if (seq >= 100) {
        ztk_warn("MP4 record name exhausted under %s", rec->folder);
        return -1;
    }

    rec->fp = fopen(rec->file_path, "wb+");
    if (!rec->fp) {
        ztk_warn("MP4 record open failed: %s", rec->file_path);
        rec->file_path[0] = '\0';
        return -1;
    }
    rec->mov = mov_writer_create(zms_mov_file_buffer(), rec->fp, MOV_FLAG_FASTSTART);
    if (!rec->mov) {
        fclose(rec->fp);
        rec->fp = NULL;
        rec->file_path[0] = '\0';
        return -1;
    }
    rec->video_track = -1;
    rec->audio_track = -1;
    rec->video_armed = 0;
    rec->sent_video_cfg = 0;
    rec->sent_audio_cfg = 0;
    rec->have_start_dts = 0;
    zms_mux_av_timeline_reset(&rec->mux_av);
    ztk_info("MP4 record start: %s", rec->file_path);
    return 0;
}

static int ensure_tracks(zms_mp4_recorder *rec)
{
    const uint8_t *cfg = NULL;
    size_t clen = 0;
    const uint8_t *extra = NULL;
    size_t extra_len = 0;
    int w = 0;
    int h = 0;
    zms_codec_id vcodec;

    if (!rec || !rec->mov || !rec->src || !rec->src->gop_queue) {
        return -1;
    }

    if (!rec->sent_video_cfg) {
        cfg = zms_gop_queue_video_config(rec->src->gop_queue, &clen);
        if (!cfg || clen < 2) {
            return 0; /* wait */
        }
        vcodec = zms_flv_video_config_codec(cfg, clen);
        if (vcodec == ZMS_CODEC_INVALID) {
            vcodec = rec->src->video.codec;
        }
        rec->video_codec = vcodec;
        w = (int)rec->src->video.width;
        h = (int)rec->src->video.height;
        if (w <= 0) {
            w = 1920;
        }
        if (h <= 0) {
            h = 1080;
        }

        if (vcodec == ZMS_CODEC_H264 && zms_rtmp_avc_extradata(cfg, clen, &extra, &extra_len) &&
            extra && extra_len > 0) {
            (void)zms_avc_config_load_record(&rec->avc, extra, extra_len);
            rec->video_track =
                mov_writer_add_video(rec->mov, MOV_OBJECT_H264, w, h, extra, extra_len);
        } else if (vcodec == ZMS_CODEC_H265 &&
                   zms_h265_video_config_hvcc(cfg, clen, &extra, &extra_len) && extra &&
                   extra_len > 0) {
            (void)zms_hevc_config_load_record(&rec->hevc, extra, extra_len);
            rec->video_track =
                mov_writer_add_video(rec->mov, MOV_OBJECT_H265, w, h, extra, extra_len);
        } else {
            ztk_warn("MP4 record: unsupported video cfg codec=%d", (int)vcodec);
            return -1;
        }
        if (rec->video_track < 0) {
            return -1;
        }
        rec->sent_video_cfg = 1;
    }

    if (!rec->sent_audio_cfg && rec->src->has_audio) {
        const uint8_t *acfg = NULL;
        size_t alen = 0;
        const uint8_t *asc = NULL;
        size_t asc_len = 0;
        int sr = 0;
        int ch = 0;

        acfg = zms_gop_queue_audio_config(rec->src->gop_queue, &alen);
        if (acfg && alen > 0 && zms_rtmp_aac_extradata(acfg, alen, &asc, &asc_len) && asc &&
            asc_len > 0) {
            zms_aac_config_set_defaults(&rec->aac, 44100, 2);
            if (zms_aac_config_load_asc(&rec->aac, asc, asc_len)) {
                sr = zms_aac_config_sample_rate(&rec->aac);
                ch = zms_aac_config_channels(&rec->aac);
                if (sr <= 0) {
                    sr = (int)rec->src->audio.sample_rate;
                }
                if (ch <= 0) {
                    ch = (int)rec->src->audio.channels;
                }
                if (sr <= 0) {
                    sr = 48000;
                }
                if (ch <= 0) {
                    ch = 2;
                }
                rec->audio_track =
                    mov_writer_add_audio(rec->mov, MOV_OBJECT_AAC, ch, 16, sr, asc, asc_len);
                if (rec->audio_track >= 0) {
                    rec->have_aac = 1;
                    rec->sent_audio_cfg = 1;
                }
            }
        }
    }
    return rec->sent_video_cfg ? 1 : 0;
}

static void feed_h264(zms_mp4_recorder *rec, const uint8_t *annexb, size_t len, uint32_t dts_ms,
                      int keyframe)
{
    int vcl = 0;
    int update = 0;
    int n;
    int64_t rel_dts;
    int64_t rel_pts;
    int flags;
    int sync;
    int idr;
    uint32_t pts_ms = dts_ms;

    if (!rec || rec->video_track < 0 || !annexb || len < 4) {
        return;
    }
    sync = keyframe || zms_h264_annexb_is_sync_key(annexb, len);
    idr = keyframe || zms_h264_annexb_is_idr(annexb, len);
    if (!rec->video_armed) {
        if (!sync) {
            return;
        }
        rec->video_armed = 1;
        zms_mux_av_timeline_lock_origin(&rec->mux_av, dts_ms);
        rec->start_dts_ms = dts_ms;
        rec->have_start_dts = 1;
    }
    if (!ensure_mux_buf(rec, len + 65536u)) {
        return;
    }
    n = zms_avc_config_annexb_to_mp4(&rec->avc, annexb, len, rec->mux_buf, rec->mux_buf_cap, &vcl,
                                     &update);
    if (n <= 0) {
        return;
    }
    rel_dts = (int64_t)zms_mux_av_timeline_pts(&rec->mux_av, ZMS_TRACK_VIDEO, dts_ms);
    rel_pts = (int64_t)zms_mux_av_timeline_pts(&rec->mux_av, ZMS_TRACK_VIDEO, pts_ms);
    flags = idr ? MOV_AV_FLAG_KEYFREAME : 0;
    (void)mov_writer_write(rec->mov, rec->video_track, rec->mux_buf, (size_t)n, rel_pts, rel_dts,
                           flags);
    rec->last_dts_ms = dts_ms;
}

static void feed_h265(zms_mp4_recorder *rec, const uint8_t *annexb, size_t len, uint32_t dts_ms,
                      int keyframe)
{
    int vcl = 0;
    int update = 0;
    int n;
    int64_t rel_dts;
    int flags;
    int sync;
    int idr;

    if (!rec || rec->video_track < 0 || !annexb || len < 4) {
        return;
    }
    sync = keyframe || zms_h265_annexb_is_sync_key(annexb, len);
    idr = keyframe || zms_h265_annexb_is_idr(annexb, len);
    if (!rec->video_armed) {
        if (!sync) {
            return;
        }
        rec->video_armed = 1;
        zms_mux_av_timeline_lock_origin(&rec->mux_av, dts_ms);
        rec->start_dts_ms = dts_ms;
        rec->have_start_dts = 1;
    }
    if (!ensure_mux_buf(rec, len + 65536u)) {
        return;
    }
    n = zms_hevc_config_annexb_to_mp4(&rec->hevc, annexb, len, rec->mux_buf, rec->mux_buf_cap, &vcl,
                                      &update);
    if (n <= 0) {
        return;
    }
    rel_dts = (int64_t)zms_mux_av_timeline_pts(&rec->mux_av, ZMS_TRACK_VIDEO, dts_ms);
    flags = idr ? MOV_AV_FLAG_KEYFREAME : 0;
    (void)mov_writer_write(rec->mov, rec->video_track, rec->mux_buf, (size_t)n, rel_dts, rel_dts,
                           flags);
    rec->last_dts_ms = dts_ms;
}

static void feed_aac(zms_mp4_recorder *rec, const uint8_t *es, size_t len, uint32_t dts_ms)
{
    const uint8_t *raw = es;
    size_t raw_len = len;
    size_t hdr;
    int64_t rel_dts;

    if (!rec || rec->audio_track < 0 || !es || len == 0 || !rec->video_armed) {
        return;
    }
    if (aac_is_adts(es, len)) {
        hdr = aac_adts_hdr_len(es, len);
        if (hdr == 0 || len <= hdr) {
            return;
        }
        raw = es + hdr;
        raw_len = len - hdr;
    }
    rel_dts = (int64_t)zms_mux_av_timeline_pts(&rec->mux_av, ZMS_TRACK_AUDIO, dts_ms);
    (void)mov_writer_write(rec->mov, rec->audio_track, raw, raw_len, rel_dts, rel_dts, 0);
    if (dts_ms > rec->last_dts_ms) {
        rec->last_dts_ms = dts_ms;
    }
}

static int should_rotate(zms_mp4_recorder *rec, int keyframe)
{
    uint32_t dur_ms;

    if (!rec || !rec->have_start_dts || !keyframe || !rec->video_armed) {
        return 0;
    }
    if (rec->last_dts_ms < rec->start_dts_ms) {
        return 0;
    }
    dur_ms = rec->last_dts_ms - rec->start_dts_ms;
    return (int)dur_ms >= rec->max_second * 1000;
}

static void tick_nolock(zms_mp4_recorder *rec)
{
    zms_gop_slot slot;
    int n;

    if (!rec || rec->closing || !rec->reader) {
        return;
    }
    if (!rec->mov) {
        if (open_new_file(rec) != 0) {
            return;
        }
    }
    if (ensure_tracks(rec) <= 0) {
        return;
    }

    for (n = 0; n < ZMS_MP4_REC_TICK_FRAMES && zms_gop_reader_read_muxed(rec->reader, &slot, 0) > 0;
         ++n) {
        if (slot.track == ZMS_TRACK_VIDEO) {
            int key = slot.keyframe;
            if (should_rotate(rec, key)) {
                close_file_fire_hook(rec);
                if (open_new_file(rec) != 0 || ensure_tracks(rec) <= 0) {
                    return;
                }
                /* 切文件后从本关键帧重新武装 */
            }
            if (rec->video_codec == ZMS_CODEC_H265 || slot.codec == ZMS_CODEC_H265) {
                feed_h265(rec, slot.data, slot.len, slot.dts_ms, key);
            } else {
                feed_h264(rec, slot.data, slot.len, slot.dts_ms, key);
            }
        } else if (slot.track == ZMS_TRACK_AUDIO) {
            if (slot.codec == ZMS_CODEC_AAC || rec->have_aac) {
                feed_aac(rec, slot.data, slot.len, slot.dts_ms);
            }
        }
    }
}

static void tick_cb(void *user)
{
    zms_mp4_recorder *rec = (zms_mp4_recorder *)user;

    if (!rec || !rec->mu) {
        return;
    }
    ztk_mutex_lock(rec->mu);
    if (!rec->closing) {
        tick_nolock(rec);
    }
    ztk_mutex_unlock(rec->mu);
}

static void destroy_recorder(zms_mp4_recorder *rec)
{
    zms_media_source *src;

    if (!rec) {
        return;
    }
    if (rec->timer) {
        ztk_timer_stop(rec->timer);
        rec->timer = NULL;
    }
    src = rec->src;
    if (rec->mu) {
        ztk_mutex_lock(rec->mu);
        rec->closing = 1;
        close_file_fire_hook(rec);
        if (rec->reader) {
            zms_gop_reader_detach(rec->reader);
            rec->reader = NULL;
        }
        if (src && src->mp4_recorder == rec) {
            src->mp4_recorder = NULL;
        }
        ztk_mutex_unlock(rec->mu);
        ztk_mutex_destroy(rec->mu);
        rec->mu = NULL;
    } else {
        close_file_fire_hook(rec);
        if (rec->reader) {
            zms_gop_reader_detach(rec->reader);
        }
        if (src && src->mp4_recorder == rec) {
            src->mp4_recorder = NULL;
        }
    }
    free(rec->mux_buf);
    free(rec);
}

ztk_err_t zms_mp4_recorder_start(zms_media_source *src, ztk_poller *poller)
{
    zms_mp4_recorder *rec;
    ztk_poller *pol;

    if (!src || !src->gop_queue) {
        return ZTK_ERR_INVALID;
    }
    if (!src->enable_mp4) {
        return ZTK_OK;
    }
    if (src->mp4_recorder) {
        return ZTK_OK;
    }
    if (!g_record_root[0]) {
        strncpy(g_record_root, "./www/record", sizeof(g_record_root) - 1);
    }

    rec = (zms_mp4_recorder *)calloc(1, sizeof(*rec));
    if (!rec) {
        return ZTK_ERR_NOMEM;
    }
    rec->src = src;
    rec->video_track = -1;
    rec->audio_track = -1;
    rec->max_second = g_mp4_max_second > 0 ? g_mp4_max_second : 180;
    rec->mu = ztk_mutex_create(0);
    if (!rec->mu) {
        free(rec);
        return ZTK_ERR_NOMEM;
    }
    rec->mux_buf_cap = 256 * 1024;
    rec->mux_buf = (uint8_t *)malloc(rec->mux_buf_cap);
    if (!rec->mux_buf) {
        ztk_mutex_destroy(rec->mu);
        free(rec);
        return ZTK_ERR_NOMEM;
    }
    zms_mux_av_timeline_reset(&rec->mux_av);

    rec->reader = zms_gop_reader_attach(src->gop_queue);
    if (!rec->reader) {
        free(rec->mux_buf);
        ztk_mutex_destroy(rec->mu);
        free(rec);
        return ZTK_ERR_NOMEM;
    }
    zms_gop_reader_seek_live_key(rec->reader);

    src->mp4_recorder = rec;
    pol = poller ? poller : zms_media_events_poller();
    if (pol) {
        rec->timer_poller = pol;
        rec->timer = ztk_timer_start(pol, ZMS_MP4_REC_TICK_MS, 1, tick_cb, rec);
    }
    ztk_info("MP4 recorder attached: %s/%s max_sec=%d", src->app, src->stream, rec->max_second);
    return ZTK_OK;
}

void zms_mp4_recorder_stop(zms_media_source *src)
{
    zms_mp4_recorder *rec;

    if (!src || !src->mp4_recorder) {
        return;
    }
    rec = (zms_mp4_recorder *)src->mp4_recorder;
    destroy_recorder(rec);
}
