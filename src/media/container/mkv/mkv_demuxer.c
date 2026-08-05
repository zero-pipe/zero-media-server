#include "zms/media/container/container_dispatcher.h"
#include "zms/media/codec/codec_id.h"
#include "zms/engine/media/media_limits.h"
#include "mkv-reader.h"
#include "mkv-format.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const struct mkv_buffer_t *zms_mkv_file_buffer(void);

#define MKV_DEMUX_READ_BUF ZMS_VOD_DEMUX_READ_MIN

typedef struct {
    zms_container_demux_opts cfg;
    FILE *fp;
    mkv_reader_t *mkv;
    uint8_t *read_buf;
    size_t read_cap;
    int eof;
    int samples_delivered;
    zms_codec_id video_codec;
    zms_codec_id audio_codec;
    uint32_t video_track;
    uint32_t audio_track;
    uint64_t duration_ms;
} mkv_demux_ctx;

static zms_codec_id mkv_codec_to_ZMS(enum mkv_codec_t codec)
{
    switch (codec) {
    case MKV_CODEC_VIDEO_H264:
        return ZMS_CODEC_H264;
    case MKV_CODEC_VIDEO_H265:
        return ZMS_CODEC_H265;
    case MKV_CODEC_AUDIO_AAC:
        return ZMS_CODEC_AAC;
    default:
        return ZMS_CODEC_INVALID;
    }
}

static void mkv_emit_config(mkv_demux_ctx *c, uint32_t track, zms_codec_id codec, const void *extra,
                            size_t bytes, uint16_t width, uint16_t height, uint32_t sample_rate,
                            uint8_t channels)
{
    zms_container_packet pkt;

    if (!c || !c->cfg.on_packet || !extra || bytes == 0) {
        return;
    }
    memset(&pkt, 0, sizeof(pkt));
    pkt.kind = ZMS_CONTAINER_PKT_MP4_SAMPLE;
    pkt.track_id = track;
    pkt.codec = codec;
    pkt.config = 1;
    pkt.width = width;
    pkt.height = height;
    pkt.sample_rate = sample_rate;
    pkt.channels = channels;
    pkt.data = (const uint8_t *)extra;
    pkt.len = bytes;
    c->cfg.on_packet(&pkt, c->cfg.user);
}

static void mkv_on_video(void *param, uint32_t track, enum mkv_codec_t codec, int width, int height,
                         const void *extra, size_t bytes)
{
    mkv_demux_ctx *c = (mkv_demux_ctx *)param;

    if (!c) {
        return;
    }
    c->video_track = track;
    c->video_codec = mkv_codec_to_ZMS(codec);
    if (extra && bytes > 0) {
        mkv_emit_config(c, track, c->video_codec, extra, bytes, (uint16_t)(width > 0 ? width : 0),
                        (uint16_t)(height > 0 ? height : 0), 0, 0);
    }
}

static void mkv_on_audio(void *param, uint32_t track, enum mkv_codec_t codec, int channel_count,
                         int bit_per_sample, int sample_rate, const void *extra, size_t bytes)
{
    mkv_demux_ctx *c = (mkv_demux_ctx *)param;

    (void)bit_per_sample;
    if (!c) {
        return;
    }
    c->audio_track = track;
    c->audio_codec = mkv_codec_to_ZMS(codec);
    if (extra && bytes > 0) {
        mkv_emit_config(c, track, c->audio_codec, extra, bytes, 0, 0,
                        sample_rate > 0 ? (uint32_t)sample_rate : 0,
                        channel_count > 0 ? (uint8_t)channel_count : 0);
    }
}

static void mkv_onread(void *param, uint32_t track, const void *buffer, size_t bytes, int64_t pts,
                       int64_t dts, int flags)
{
    mkv_demux_ctx *c = (mkv_demux_ctx *)param;
    zms_container_packet pkt;
    zms_codec_id codec = ZMS_CODEC_INVALID;

    if (!c || !c->cfg.on_packet || !buffer || bytes == 0) {
        return;
    }
    if (track == c->video_track) {
        codec = c->video_codec;
    } else if (track == c->audio_track) {
        codec = c->audio_codec;
    } else {
        return;
    }

    memset(&pkt, 0, sizeof(pkt));
    pkt.kind = ZMS_CONTAINER_PKT_MP4_SAMPLE;
    pkt.track_id = track;
    pkt.codec = codec;
    pkt.pts_ms = (uint32_t)(pts > 0 ? pts : dts);
    pkt.dts_ms = (uint32_t)dts;
    pkt.key = (flags & MKV_FLAGS_KEYFRAME) ? 1 : 0;
    pkt.data = (const uint8_t *)buffer;
    pkt.len = bytes;
    c->cfg.on_packet(&pkt, c->cfg.user);
    ++c->samples_delivered;
}

typedef struct {
    uint32_t track;
    size_t bytes;
    int64_t pts;
    int64_t dts;
    int flags;
    mkv_demux_ctx *demux;
} mkv_read_sample;

static void *mkv_alloc_sample(void *param, uint32_t track, size_t bytes, int64_t pts, int64_t dts,
                              int flags)
{
    mkv_read_sample *sample = (mkv_read_sample *)param;
    mkv_demux_ctx *c;
    void *buf;

    if (!sample || !sample->demux || bytes == 0) {
        return NULL;
    }
    c = sample->demux;
    sample->track = track;
    sample->bytes = bytes;
    sample->pts = pts;
    sample->dts = dts;
    sample->flags = flags;
    if (c->read_cap < bytes) {
        size_t cap = bytes;
        if (cap < MKV_DEMUX_READ_BUF) {
            cap = MKV_DEMUX_READ_BUF;
        }
        buf = realloc(c->read_buf, cap);
        if (!buf) {
            return NULL;
        }
        c->read_buf = (uint8_t *)buf;
        c->read_cap = cap;
    }
    return c->read_buf;
}

static void mkv_free_read_buf(mkv_demux_ctx *c)
{
    if (!c) {
        return;
    }
    free(c->read_buf);
    c->read_buf = NULL;
    c->read_cap = 0;
}

static void mkv_close_file(mkv_demux_ctx *c)
{
    if (!c) {
        return;
    }
    if (c->mkv) {
        mkv_reader_destroy(c->mkv);
        c->mkv = NULL;
    }
    if (c->fp) {
        fclose(c->fp);
        c->fp = NULL;
    }
    mkv_free_read_buf(c);
    c->eof = 0;
    c->samples_delivered = 0;
    c->video_codec = ZMS_CODEC_INVALID;
    c->audio_codec = ZMS_CODEC_INVALID;
    c->video_track = 0;
    c->audio_track = 0;
    c->duration_ms = 0;
}

static void *mkv_create(const zms_container_demux_opts *opts)
{
    mkv_demux_ctx *c;

    if (!opts || !opts->on_packet) {
        return NULL;
    }
    c = (mkv_demux_ctx *)calloc(1, sizeof(*c));
    if (!c) {
        return NULL;
    }
    c->cfg = *opts;
    return c;
}

static void mkv_destroy(void *ctx)
{
    mkv_demux_ctx *c = (mkv_demux_ctx *)ctx;

    if (!c) {
        return;
    }
    mkv_close_file(c);
    free(c);
}

static ztk_err_t mkv_feed(void *ctx, const uint8_t *buf, size_t len)
{
    (void)ctx;
    (void)buf;
    (void)len;
    return ZTK_ERR_INVALID;
}

static ztk_err_t mkv_input_tag(void *ctx, uint8_t type_id, const uint8_t *body, size_t len,
                               uint32_t tag_dts_ms)
{
    (void)ctx;
    (void)type_id;
    (void)body;
    (void)len;
    (void)tag_dts_ms;
    return ZTK_ERR_INVALID;
}

static ztk_err_t mkv_open_file(void *ctx, const char *path)
{
    mkv_demux_ctx *c = (mkv_demux_ctx *)ctx;
    struct mkv_reader_trackinfo_t info;

    if (!c || !path || !path[0]) {
        return ZTK_ERR_INVALID;
    }

    mkv_close_file(c);
    c->fp = fopen(path, "rb");
    if (!c->fp) {
        return ZTK_ERR_IO;
    }

    c->mkv = mkv_reader_create(zms_mkv_file_buffer(), c->fp);
    if (!c->mkv) {
        fclose(c->fp);
        c->fp = NULL;
        return ZTK_ERR_IO;
    }

    memset(&info, 0, sizeof(info));
    info.onvideo = mkv_on_video;
    info.onaudio = mkv_on_audio;
    if (mkv_reader_getinfo(c->mkv, &info, c) != 0) {
        mkv_close_file(c);
        return ZTK_ERR_INVALID;
    }
    if (c->video_codec != ZMS_CODEC_H264 && c->video_codec != ZMS_CODEC_H265) {
        mkv_close_file(c);
        return ZTK_ERR_NOT_IMPL;
    }

    c->duration_ms = mkv_reader_getduration(c->mkv);
    c->eof = 0;
    return ZTK_OK;
}

static void mkv_close_file_op(void *ctx)
{
    mkv_close_file((mkv_demux_ctx *)ctx);
}

static int mkv_pump(void *ctx)
{
    mkv_demux_ctx *c = (mkv_demux_ctx *)ctx;
    mkv_read_sample sample;
    int n;

    if (!c || !c->mkv || c->eof) {
        return 0;
    }

    c->samples_delivered = 0;
    memset(&sample, 0, sizeof(sample));
    sample.demux = c;
    n = mkv_reader_read2(c->mkv, mkv_alloc_sample, &sample);
    if (n > 0) {
        if (c->read_buf && sample.bytes > 0) {
            mkv_onread(c, sample.track, c->read_buf, sample.bytes, sample.pts, sample.dts,
                       sample.flags);
        }
        return c->samples_delivered > 0 ? c->samples_delivered : 1;
    }
    if (n == 0) {
        c->eof = 1;
        return 0;
    }
    return -1;
}

static ztk_err_t mkv_seek(void *ctx, int64_t *seek_ms)
{
    mkv_demux_ctx *c = (mkv_demux_ctx *)ctx;

    if (!c || !c->mkv || !seek_ms) {
        return ZTK_ERR_INVALID;
    }
    if (mkv_reader_seek(c->mkv, seek_ms) != 0) {
        return ZTK_ERR_INVALID;
    }
    c->eof = 0;
    return ZTK_OK;
}

static uint64_t mkv_duration_ms(void *ctx)
{
    mkv_demux_ctx *c = (mkv_demux_ctx *)ctx;

    return c ? c->duration_ms : 0;
}

void zms_container_demux_mkv_codecs(void *ctx, zms_codec_id *video, zms_codec_id *audio)
{
    mkv_demux_ctx *c = (mkv_demux_ctx *)ctx;

    if (video) {
        *video = c ? c->video_codec : ZMS_CODEC_INVALID;
    }
    if (audio) {
        *audio = c ? c->audio_codec : ZMS_CODEC_INVALID;
    }
}

const zms_container_demuxer_ops zms_container_mkv_ops = {
    ZMS_CONTAINER_MKV, "mkv",         mkv_create,      mkv_destroy,
    mkv_feed,          mkv_input_tag, mkv_open_file,   mkv_close_file_op,
    mkv_pump,          mkv_seek,      mkv_duration_ms,
};
