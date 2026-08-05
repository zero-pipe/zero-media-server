#include "zms/media/container/container_dispatcher.h"
#include "zms/media/codec/codec_id.h"
#include "zms/media/codec/aac/aac_over_rtp.h"
#include "zms/engine/media/media_limits.h"
#include "flv-reader.h"
#include "flv-demuxer.h"
#include "flv-proto.h"
#include "flv-header.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FLV_FILE_TAG_MIN ZMS_VOD_DEMUX_READ_MIN
#define FLV_FILE_VIDEO_TRACK 1u
#define FLV_FILE_AUDIO_TRACK 2u

typedef struct {
    zms_container_demux_opts cfg;
    char path[512];
    void *reader;
    flv_demuxer_t *demux;
    uint8_t *tag_buf;
    size_t tag_cap;
    uint8_t *pending;
    size_t pending_cap;
    size_t pending_len;
    int pending_type;
    uint32_t pending_tag_dts_ms;
    int eof;
    int samples_delivered;
    int seek_scan;
    int probe_mode;
    int64_t seek_target_ms;
    int seek_found;
    zms_codec_id video_codec;
    zms_codec_id audio_codec;
    uint32_t video_track;
    uint32_t audio_track;
    uint64_t duration_ms;
} flv_file_ctx;

static int flv_file_pump(void *ctx);

static zms_codec_id flv_codec_to_zms(int codec)
{
    switch (codec) {
    case FLV_VIDEO_H264:
    case FLV_VIDEO_AVCC:
        return ZMS_CODEC_H264;
    case FLV_VIDEO_H265:
    case FLV_VIDEO_HVCC:
        return ZMS_CODEC_H265;
    case FLV_AUDIO_AAC:
    case FLV_AUDIO_ASC:
        return ZMS_CODEC_AAC;
    default:
        return ZMS_CODEC_INVALID;
    }
}

static void flv_emit_packet(flv_file_ctx *c, uint32_t track, zms_codec_id codec, const void *data,
                            size_t len, uint32_t pts_ms, uint32_t dts_ms, int key, int config)
{
    zms_container_packet pkt;

    if (!c || !c->cfg.on_packet || !data || len == 0) {
        return;
    }
    memset(&pkt, 0, sizeof(pkt));
    pkt.kind = ZMS_CONTAINER_PKT_MP4_SAMPLE;
    pkt.track_id = track;
    pkt.codec = codec;
    pkt.pts_ms = pts_ms;
    pkt.dts_ms = dts_ms;
    pkt.key = key;
    pkt.config = config;
    pkt.data = (const uint8_t *)data;
    pkt.len = len;
    c->cfg.on_packet(&pkt, c->cfg.user);
    ++c->samples_delivered;
}

static int flv_file_on_es(void *param, int codec, const void *data, size_t bytes, uint32_t pts,
                          uint32_t dts, int flags)
{
    flv_file_ctx *c = (flv_file_ctx *)param;
    zms_codec_id zc;
    const uint8_t *raw;
    size_t raw_len;
    uint32_t track;
    int key;
    int config;

    if (!c || !data || bytes == 0) {
        return 0;
    }

    zc = flv_codec_to_zms(codec);
    if (zc == ZMS_CODEC_INVALID) {
        return 0;
    }

    config = (codec == FLV_VIDEO_AVCC || codec == FLV_VIDEO_HVCC || codec == FLV_AUDIO_ASC) ? 1 : 0;
    key = flags ? 1 : 0;

    if (zc == ZMS_CODEC_H264 || zc == ZMS_CODEC_H265) {
        c->video_codec = zc;
    } else if (zc == ZMS_CODEC_AAC) {
        c->audio_codec = zc;
    }

    if (c->probe_mode) {
        return 0;
    }

    if (c->seek_scan) {
        if (config) {
            return 0;
        }
        if (zc == ZMS_CODEC_H264 || zc == ZMS_CODEC_H265) {
            if ((int64_t)dts < c->seek_target_ms || !key) {
                return 0;
            }
        } else if ((int64_t)dts < c->seek_target_ms) {
            return 0;
        }
        c->seek_found = 1;
        c->seek_scan = 0;
    }

    if (zc == ZMS_CODEC_H264 || zc == ZMS_CODEC_H265) {
        track = c->video_track ? c->video_track : FLV_FILE_VIDEO_TRACK;
    } else if (zc == ZMS_CODEC_AAC) {
        track = c->audio_track ? c->audio_track : FLV_FILE_AUDIO_TRACK;
        if (!config) {
            raw = (const uint8_t *)data;
            raw_len = bytes;
            if (zms_aac_es_to_raw(raw, raw_len, &raw, &raw_len) != ZTK_OK || !raw || raw_len == 0) {
                return 0;
            }
            flv_emit_packet(c, track, zc, raw, raw_len, pts, dts, 0, 0);
            return 0;
        }
    } else {
        return 0;
    }

    flv_emit_packet(c, track, zc, data, bytes, pts, dts, key, config);
    return 0;
}

static int flv_tag_is_video_key(const uint8_t *body, size_t len)
{
    struct flv_video_tag_header_t vhdr;

    memset(&vhdr, 0, sizeof(vhdr));
    if (flv_video_tag_header_read(&vhdr, body, len) < 1) {
        return 0;
    }
    return vhdr.keyframe == FLV_VIDEO_KEY_FRAME;
}

static int flv_ensure_tag_buf(flv_file_ctx *c, size_t need)
{
    uint8_t *p;

    if (!c) {
        return -1;
    }
    if (c->tag_cap >= need) {
        return 0;
    }
    p = (uint8_t *)realloc(c->tag_buf, need);
    if (!p) {
        return -1;
    }
    c->tag_buf = p;
    c->tag_cap = need;
    return 0;
}

static int flv_ensure_pending(flv_file_ctx *c, size_t need)
{
    uint8_t *p;

    if (!c) {
        return -1;
    }
    if (c->pending_cap >= need) {
        return 0;
    }
    p = (uint8_t *)realloc(c->pending, need);
    if (!p) {
        return -1;
    }
    c->pending = p;
    c->pending_cap = need;
    return 0;
}

static void flv_clear_pending(flv_file_ctx *c)
{
    if (!c) {
        return;
    }
    c->pending_len = 0;
    c->pending_type = 0;
    c->pending_tag_dts_ms = 0;
}

static void flv_close_reader(flv_file_ctx *c)
{
    if (!c) {
        return;
    }
    if (c->demux) {
        flv_demuxer_destroy(c->demux);
        c->demux = NULL;
    }
    if (c->reader) {
        flv_reader_destroy(c->reader);
        c->reader = NULL;
    }
    c->eof = 0;
    c->samples_delivered = 0;
    flv_clear_pending(c);
}

static int flv_open_reader(flv_file_ctx *c)
{
    if (!c || !c->path[0]) {
        return -1;
    }
    flv_close_reader(c);
    c->reader = flv_reader_create(c->path);
    if (!c->reader) {
        return -1;
    }
    c->demux = flv_demuxer_create(flv_file_on_es, c);
    if (!c->demux) {
        flv_reader_destroy(c->reader);
        c->reader = NULL;
        return -1;
    }
    c->eof = 0;
    c->samples_delivered = 0;
    return 0;
}

static int flv_process_tag(flv_file_ctx *c, int type, uint32_t ts, const uint8_t *body, size_t len)
{
    if (!c || !c->demux || !body || len == 0) {
        return 0;
    }
    if (type != FLV_TYPE_AUDIO && type != FLV_TYPE_VIDEO) {
        return 0;
    }
    return flv_demuxer_input(c->demux, type, body, len, ts);
}

static int flv_read_one_tag(flv_file_ctx *c, int *type, uint32_t *ts, const uint8_t **body,
                            size_t *len)
{
    int tagtype = 0;
    uint32_t timestamp = 0;
    size_t taglen = 0;
    int n;

    if (!c || !c->reader || !type || !ts || !body || !len) {
        return 0;
    }

    if (flv_ensure_tag_buf(c, FLV_FILE_TAG_MIN) != 0) {
        return -1;
    }

    n = flv_reader_read(c->reader, &tagtype, &timestamp, &taglen, c->tag_buf, c->tag_cap);
    if (n == FLV_READER_NEED_BUF) {
        if (flv_ensure_tag_buf(c, taglen) != 0) {
            return -1;
        }
        n = flv_reader_read(c->reader, &tagtype, &timestamp, &taglen, c->tag_buf, c->tag_cap);
    }
    if (n < 0) {
        return -1;
    }
    if (n == 0) {
        c->eof = 1;
        return 0;
    }
    if (taglen == 0) {
        return 1;
    }
    *type = tagtype;
    *ts = timestamp;
    *body = c->tag_buf;
    *len = taglen;
    return 1;
}

static uint64_t flv_scan_duration_ms(const char *path)
{
    void *reader;
    int tagtype;
    uint32_t ts;
    size_t taglen;
    uint8_t buf[64 * 1024];
    uint64_t max_ts = 0;
    int n;

    if (!path || !path[0]) {
        return 0;
    }
    reader = flv_reader_create(path);
    if (!reader) {
        return 0;
    }
    for (;;) {
        tagtype = 0;
        ts = 0;
        taglen = 0;
        n = flv_reader_read(reader, &tagtype, &ts, &taglen, buf, sizeof(buf));
        if (n < 0) {
            break;
        }
        if (n == 0) {
            break;
        }
        if (taglen > sizeof(buf)) {
            break;
        }
        if ((tagtype == FLV_TYPE_AUDIO || tagtype == FLV_TYPE_VIDEO) && (uint64_t)ts > max_ts) {
            max_ts = ts;
        }
    }
    flv_reader_destroy(reader);
    return max_ts;
}

uint64_t zms_container_flv_file_duration_ms(const char *path)
{
    return flv_scan_duration_ms(path);
}

static void *flv_file_create(const zms_container_demux_opts *opts)
{
    flv_file_ctx *c;

    if (!opts || !opts->on_packet) {
        return NULL;
    }
    c = (flv_file_ctx *)calloc(1, sizeof(*c));
    if (!c) {
        return NULL;
    }
    c->cfg = *opts;
    c->video_track = FLV_FILE_VIDEO_TRACK;
    c->audio_track = FLV_FILE_AUDIO_TRACK;
    return c;
}

static void flv_file_destroy(void *ctx)
{
    flv_file_ctx *c = (flv_file_ctx *)ctx;

    if (!c) {
        return;
    }
    flv_close_reader(c);
    free(c->tag_buf);
    free(c->pending);
    free(c);
}

static ztk_err_t flv_file_open(void *ctx, const char *path)
{
    flv_file_ctx *c = (flv_file_ctx *)ctx;

    if (!c || !path || !path[0]) {
        return ZTK_ERR_INVALID;
    }
    snprintf(c->path, sizeof(c->path), "%s", path);
    c->video_codec = ZMS_CODEC_INVALID;
    c->audio_codec = ZMS_CODEC_INVALID;
    c->duration_ms = flv_scan_duration_ms(path);
    if (flv_open_reader(c) != 0) {
        return ZTK_ERR_IO;
    }

    c->probe_mode = 1;
    while (c->video_codec == ZMS_CODEC_INVALID && !c->eof) {
        if (flv_file_pump(c) < 0) {
            c->probe_mode = 0;
            return ZTK_ERR_INVALID;
        }
    }
    c->probe_mode = 0;
    if (flv_open_reader(c) != 0) {
        return ZTK_ERR_IO;
    }
    return ZTK_OK;
}

static void flv_file_close_file(void *ctx)
{
    flv_close_reader((flv_file_ctx *)ctx);
}

static int flv_file_pump(void *ctx)
{
    flv_file_ctx *c = (flv_file_ctx *)ctx;
    int type;
    uint32_t ts;
    const uint8_t *body;
    size_t len;
    int n;

    if (!c || !c->reader || c->eof) {
        return 0;
    }

    c->samples_delivered = 0;

    if (c->pending_len > 0) {
        (void)flv_process_tag(c, c->pending_type, c->pending_tag_dts_ms, c->pending,
                              c->pending_len);
        flv_clear_pending(c);
        return c->samples_delivered > 0 ? c->samples_delivered : 1;
    }

    n = flv_read_one_tag(c, &type, &ts, &body, &len);
    if (n < 0) {
        return -1;
    }
    if (n == 0) {
        return 0;
    }
    if (len == 0) {
        return 1;
    }

    if (c->seek_scan) {
        if (type == FLV_TYPE_VIDEO || type == FLV_TYPE_AUDIO) {
            int ready = 0;
            if (type == FLV_TYPE_VIDEO) {
                if ((int64_t)ts >= c->seek_target_ms && flv_tag_is_video_key(body, len)) {
                    ready = 1;
                }
            } else if (c->video_codec == ZMS_CODEC_INVALID && (int64_t)ts >= c->seek_target_ms) {
                ready = 1;
            }
            if (ready) {
                if (flv_ensure_pending(c, len) != 0) {
                    return -1;
                }
                memcpy(c->pending, body, len);
                c->pending_len = len;
                c->pending_type = type;
                c->pending_tag_dts_ms = ts;
                c->seek_scan = 0;
                c->seek_found = 1;
                return 1;
            }
        }
        (void)flv_process_tag(c, type, ts, body, len);
        return c->samples_delivered > 0 ? c->samples_delivered : 1;
    }

    (void)flv_process_tag(c, type, ts, body, len);
    return c->samples_delivered > 0 ? c->samples_delivered : 1;
}

static ztk_err_t flv_file_seek(void *ctx, int64_t *seek_ms)
{
    flv_file_ctx *c = (flv_file_ctx *)ctx;

    if (!c || !seek_ms) {
        return ZTK_ERR_INVALID;
    }
    if (*seek_ms < 0) {
        *seek_ms = 0;
    }
    if (c->duration_ms > 0 && (uint64_t)*seek_ms > c->duration_ms) {
        *seek_ms = (int64_t)c->duration_ms;
    }

    c->seek_target_ms = *seek_ms;
    c->seek_scan = 1;
    c->seek_found = 0;
    if (flv_open_reader(c) != 0) {
        return ZTK_ERR_IO;
    }

    while (!c->eof && c->seek_scan) {
        if (flv_file_pump(c) < 0) {
            return ZTK_ERR_INVALID;
        }
    }
    if (!c->seek_found && c->pending_len == 0) {
        *seek_ms = 0;
    }
    return ZTK_OK;
}

static uint64_t flv_file_duration_ms(void *ctx)
{
    flv_file_ctx *c = (flv_file_ctx *)ctx;

    return c ? c->duration_ms : 0;
}

void zms_container_demux_flv_file_codecs(void *ctx, zms_codec_id *video, zms_codec_id *audio)
{
    flv_file_ctx *c = (flv_file_ctx *)ctx;

    if (video) {
        *video = c ? c->video_codec : ZMS_CODEC_INVALID;
    }
    if (audio) {
        *audio = c ? c->audio_codec : ZMS_CODEC_INVALID;
    }
}

const zms_container_demuxer_ops zms_container_flv_file_ops = {
    ZMS_CONTAINER_FLV_FILE,
    "flv-file",
    flv_file_create,
    flv_file_destroy,
    NULL,
    NULL,
    flv_file_open,
    flv_file_close_file,
    flv_file_pump,
    flv_file_seek,
    flv_file_duration_ms,
};
