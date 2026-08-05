#include "zms/vod/play/vod_flv_muxer.h"
#include "zms/vod/vod_flv_metadata.h"
#include "zms/vod/vod_flv_index.h"
#include "zms/vod/io/vod_buffer.h"
#include "zms/vod/io/vod_source.h"
#include "media/container/flv/flv_file_muxer.h"
#include "zms/egress/egress_source.h"
#include "zms/engine/media_clock.h"
#include "zms/engine/media/media_limits.h"
#include "zms/engine/stream/stream_hub.h"
#include "zms/media/codec/codec_id.h"
#include "zms/media/container/flv/flv_wire.h"
#include "zms/media/container/flv/flv_tag_probe.h"
#include "zms/egress/egress_pipeline.h"
#include "zms/egress/egress_pacing.h"
#include "zms/session/session_dispatcher.h"
#include "zms/session/rtmp/rtmp.h"
#include "zms/egress/egress_clock.h"
#include "zms/util/buf_pool.h"
#include "ztk/poller/poller.h"
#include <stdlib.h>
#include <string.h>

struct zms_vod_flv_muxer {
    zms_media_source *source;
    zms_egress_source play;
    zms_vod_buffer_reader *vod_reader;
    int vod_reader_owned;
    int header_have_audio;
    int header_has_video;
    int sent_metadata;
    int sent_video_cfg;
    int sent_audio_cfg;
    int video_armed;
    zms_egress_clock play_clk;
    zms_egress_clock *play_clk_ext;
    const uint8_t *vod_video_cfg;
    size_t vod_video_cfg_len;
    const uint8_t *vod_audio_cfg;
    size_t vod_audio_cfg_len;
    uint64_t vod_duration_ms;
    zms_vod_flv_index_view *vod_flv_view;
    const zms_vod_flv_index *vod_flv_index_full;
    int vod_http_realtime;
    int vod_catchup;
    uint32_t vod_last_video_tag_dts_ms;
    uint32_t vod_last_audio_tag_dts_ms;
    zms_flv_mux_pending pending;
    zms_egress_pipeline *egress_pipe;
    ztk_poller *io_poller;
    uint8_t *tag_buf;
    size_t tag_cap;
    uint8_t *es_buf;
    size_t es_cap;
};

static ztk_err_t vod_attach_play(zms_vod_flv_muxer *m)
{
    zms_session_play_opts pcfg;

    if (!m || !m->source) {
        return ZTK_ERR_INVALID;
    }
    zms_session_dispatch_register_all();
    memset(&pcfg, 0, sizeof(pcfg));
    pcfg.player = ZMS_SESSION_HTTP_FLV;
    return zms_session_attach_play(ZMS_SESSION_HTTP_FLV, &m->play, m->source, &pcfg);
}

static void vod_detach_play(zms_vod_flv_muxer *m)
{
    if (!m) {
        return;
    }
    if (m->play.readers.vod) {
        zms_session_dispatch_register_all();
        zms_session_detach_play(ZMS_SESSION_HTTP_FLV, &m->play);
    }
    m->vod_reader = NULL;
}

static zms_egress_clock *vod_play_clk(zms_vod_flv_muxer *m)
{
    return (m && m->play_clk_ext) ? m->play_clk_ext : &m->play_clk;
}

static void vod_egress_create(zms_vod_flv_muxer *m)
{
    zms_egress_pipeline_opts ecfg;
    zms_egress_flv_bind fbind;

    if (!m || !m->vod_reader) {
        return;
    }
    zms_egress_pipeline_destroy(m->egress_pipe);
    m->egress_pipe = NULL;
    memset(&ecfg, 0, sizeof(ecfg));
    memset(&fbind, 0, sizeof(fbind));
    fbind.source = m->source;
    fbind.play_clk = vod_play_clk(m);
    fbind.video_armed = &m->video_armed;
    ecfg.wire = ZMS_WIRE_FORMAT_HTTP_FLV;
    ecfg.reader = &m->play;
    ecfg.flv = &fbind;
    m->egress_pipe = zms_egress_pipeline_create(&ecfg);
    if (m->egress_pipe) {
        zms_egress_pipeline_bind_poller(m->egress_pipe, m->io_poller);
    }
}

static uint32_t vod_out_tag_dts_ms(zms_vod_flv_muxer *m, uint8_t type_id, uint32_t tag_dts_ms)
{
    uint32_t *last;
    uint32_t step;

    if (!m) {
        return tag_dts_ms;
    }
    last = (type_id == ZMS_RTMP_MSG_AUDIO) ? &m->vod_last_audio_tag_dts_ms
                                           : &m->vod_last_video_tag_dts_ms;
    step = (type_id == ZMS_RTMP_MSG_AUDIO && m->pending.audio_step_ms > 0)
               ? m->pending.audio_step_ms
               : 1u;
    if (*last > 0 && tag_dts_ms <= *last) {
        tag_dts_ms = *last + step;
    }
    *last = tag_dts_ms;
    return tag_dts_ms;
}

static int vod_pull_media(zms_vod_flv_muxer *m, uint8_t *body, size_t body_cap, size_t *body_len,
                          uint8_t *type_id, uint32_t *out_ts)
{
    zms_egress_clock *clk;

    if (!m || !m->vod_reader || !body || !body_len || !type_id || !out_ts) {
        return -1;
    }

    *body_len = 0;
    clk = vod_play_clk(m);

    if (m->pending.pending_emit < m->pending.pending_cnt) {
        int i = m->pending.pending_emit++;

        *type_id = m->pending.pending_type[i];
        *out_ts =
            vod_out_tag_dts_ms(m, m->pending.pending_type[i], m->pending.pending_tag_dts_ms[i]);
        if (body_cap < m->pending.pending_len[i] || !m->pending.pending_body[i]) {
            return -1;
        }
        memcpy(body, m->pending.pending_body[i], m->pending.pending_len[i]);
        *body_len = m->pending.pending_len[i];
        if (m->pending.pending_emit >= m->pending.pending_cnt) {
            m->pending.pending_cnt = m->pending.pending_emit = 0;
        }
        return 1;
    }

    if (m->egress_pipe) {
        zms_flv_vod_egress_bind vcfg;
        zms_egress_flv_tag tag;
        uint32_t pkt_ts;

        memset(&vcfg, 0, sizeof(vcfg));
        vcfg.vod_rd = m->vod_reader;
        vcfg.play_clk = clk;
        vcfg.catchup_left = &m->vod_catchup;
        vcfg.es_buf = &m->es_buf;
        vcfg.es_cap = &m->es_cap;
        vcfg.pace_when_locked = m->vod_http_realtime;
        if (zms_egress_pipeline_pull_flv_vod(m->egress_pipe, &vcfg, &tag, 0) != 1) {
            return 0;
        }
        if (body_cap < tag.len) {
            return -1;
        }
        pkt_ts = tag.tag_dts_ms;
        if (tag.msg_type == ZMS_RTMP_MSG_AUDIO &&
            zms_flv_mux_try_split_aac_tag(&m->pending, tag.body, tag.len, pkt_ts)) {
            m->pending.pending_emit = 0;
            *type_id = m->pending.pending_type[0];
            *out_ts =
                vod_out_tag_dts_ms(m, m->pending.pending_type[0], m->pending.pending_tag_dts_ms[0]);
            if (body_cap < m->pending.pending_len[0] || !m->pending.pending_body[0]) {
                return -1;
            }
            memcpy(body, m->pending.pending_body[0], m->pending.pending_len[0]);
            *body_len = m->pending.pending_len[0];
            m->pending.pending_emit = 1;
        } else {
            memcpy(body, tag.body, tag.len);
            *body_len = tag.len;
            *type_id = tag.msg_type;
            *out_ts = vod_out_tag_dts_ms(m, tag.msg_type, pkt_ts);
        }
        if (!zms_egress_clock_epoch_locked(clk)) {
            (void)zms_egress_clock_lock_epoch(clk, pkt_ts);
        }
        if (m->vod_catchup > 0 && !m->vod_http_realtime) {
            m->vod_catchup--;
        }
        return 1;
    }

    return 0;
}

zms_vod_flv_muxer *zms_vod_flv_muxer_create(zms_media_source *src)
{
    zms_vod_flv_muxer *m;

    if (!src || !zms_media_source_is_vod(src)) {
        return NULL;
    }
    m = (zms_vod_flv_muxer *)calloc(1, sizeof(*m));
    if (!m) {
        return NULL;
    }
    m->source = src;
    m->vod_reader_owned = 1;
    if (vod_attach_play(m) != ZTK_OK) {
        free(m);
        return NULL;
    }
    m->vod_reader = m->play.readers.vod;
    zms_egress_clock_init(&m->play_clk);
    vod_egress_create(m);
    return m;
}

zms_vod_flv_muxer *zms_vod_flv_muxer_create_reader(zms_media_source *src, zms_vod_buffer_reader *rd)
{
    zms_vod_flv_muxer *m;

    if (!src || !zms_media_source_is_vod(src) || !rd) {
        return NULL;
    }
    m = (zms_vod_flv_muxer *)calloc(1, sizeof(*m));
    if (!m) {
        return NULL;
    }
    m->source = src;
    m->vod_reader = rd;
    m->vod_reader_owned = 0;
    zms_egress_clock_init(&m->play_clk);
    vod_egress_create(m);
    return m;
}

void zms_vod_flv_muxer_bind_source(zms_vod_flv_muxer *m, zms_media_source *src)
{
    if (m) {
        m->source = src;
    }
}

void zms_vod_flv_muxer_bind_poller(zms_vod_flv_muxer *m, ztk_poller *pol)
{
    if (!m) {
        return;
    }
    m->io_poller = pol;
    if (m->egress_pipe) {
        zms_egress_pipeline_bind_poller(m->egress_pipe, pol);
    }
}

void zms_vod_flv_muxer_destroy(zms_vod_flv_muxer *m)
{
    if (!m) {
        return;
    }
    zms_vod_flv_index_view_free(m->vod_flv_view);
    m->vod_flv_view = NULL;
    if (m->vod_reader_owned) {
        vod_detach_play(m);
    }
    zms_flv_mux_pending_clear(&m->pending);
    if (m->io_poller) {
        zms_buf_pool_slot_clear_poller(&m->tag_buf, &m->tag_cap, m->io_poller);
        zms_buf_pool_slot_clear_poller(&m->es_buf, &m->es_cap, m->io_poller);
    } else {
        zms_buf_pool_slot_clear(&m->tag_buf, &m->tag_cap);
        zms_buf_pool_slot_clear(&m->es_buf, &m->es_cap);
    }
    zms_egress_pipeline_destroy(m->egress_pipe);
    m->egress_pipe = NULL;
    free(m);
}

ztk_err_t zms_vod_flv_muxer_start(zms_vod_flv_muxer *m, int has_audio, int has_video, uint8_t *out,
                                  size_t cap, size_t *out_len)
{
    size_t vlen = 0, alen = 0;

    if (!m || !out) {
        return ZTK_ERR_INVALID;
    }
    if (m->source) {
        (void)zms_media_source_video_config(m->source, &vlen);
        (void)zms_media_source_audio_config(m->source, &alen);
    }
    m->header_have_audio = has_audio || alen > 0;
    m->header_has_video = has_video || vlen > 0;
    if (m->source && m->source->audio.sample_rate > 0) {
        zms_codec_id ac = ZMS_CODEC_AAC;
        size_t clen = 0;
        const uint8_t *cfg = zms_media_source_audio_config(m->source, &clen);
        if (cfg && clen) {
            ac = zms_flv_tag_audio_codec(cfg, clen);
        }
        m->pending.audio_step_ms =
            zms_codec_frame_duration_ms(ac, (uint32_t)m->source->audio.sample_rate);
    }
    if (m->pending.audio_step_ms == 0) {
        m->pending.audio_step_ms = ZMS_MEDIA_TS_MIN_STEP_MS;
    }
    zms_egress_clock_arm(vod_play_clk(m));
    return zms_flv_write_header(out, cap, out_len, m->header_have_audio, m->header_has_video);
}

void zms_vod_flv_muxer_configure(zms_vod_flv_muxer *m, const uint8_t *video_cfg, size_t video_len,
                                 const uint8_t *audio_cfg, size_t audio_len, uint64_t duration_ms)
{
    if (!m) {
        return;
    }
    m->vod_video_cfg = video_cfg;
    m->vod_video_cfg_len = video_len;
    m->vod_audio_cfg = audio_cfg;
    m->vod_audio_cfg_len = audio_len;
    m->vod_duration_ms = duration_ms;
    if (m->source && m->source->audio.sample_rate > 0) {
        zms_codec_id ac = ZMS_CODEC_AAC;
        if (audio_cfg && audio_len) {
            ac = zms_flv_tag_audio_codec(audio_cfg, audio_len);
        }
        m->pending.audio_step_ms =
            zms_codec_frame_duration_ms(ac, (uint32_t)m->source->audio.sample_rate);
    }
    if (m->pending.audio_step_ms == 0) {
        m->pending.audio_step_ms = ZMS_MEDIA_TS_MIN_STEP_MS;
    }
}

void zms_vod_flv_muxer_set_index_view(zms_vod_flv_muxer *m, zms_vod_flv_index_view *view)
{
    if (!m) {
        return;
    }
    zms_vod_flv_index_view_free(m->vod_flv_view);
    m->vod_flv_view = view;
}

void zms_vod_flv_muxer_set_index_full(zms_vod_flv_muxer *m, const zms_vod_flv_index *idx)
{
    if (m) {
        m->vod_flv_index_full = idx;
    }
}

void zms_vod_flv_muxer_set_http_realtime_pace(zms_vod_flv_muxer *m, int on)
{
    if (m) {
        m->vod_http_realtime = on ? 1 : 0;
    }
}

void zms_vod_flv_muxer_seek(zms_vod_flv_muxer *m, uint32_t ms)
{
    zms_egress_clock *clk;

    if (!m || !m->vod_reader) {
        return;
    }
    clk = vod_play_clk(m);
    zms_egress_clock_arm(clk);
    (void)zms_egress_clock_lock_epoch(clk, ms);
    m->video_armed = 0;
    m->vod_last_video_tag_dts_ms = 0;
    m->vod_last_audio_tag_dts_ms = 0;
    m->pending.pending_cnt = m->pending.pending_emit = 0;
    /* seek 后先 burst 关键帧，HTTP realtime 节奏由后续 tick 恢复 */
    m->vod_catchup = (int)ZMS_EGRESS_FRAME_BUDGET_CATCHUP;
}

void zms_vod_flv_muxer_skip_bootstrap(zms_vod_flv_muxer *m)
{
    if (!m) {
        return;
    }
    m->sent_metadata = 1;
    m->sent_video_cfg = 1;
    m->sent_audio_cfg = 1;
}

void zms_vod_flv_muxer_bind_play_clock(zms_vod_flv_muxer *m, zms_egress_clock *clk)
{
    if (m) {
        m->play_clk_ext = clk;
    }
}

void zms_vod_flv_muxer_set_catchup(zms_vod_flv_muxer *m, int catchup)
{
    if (m) {
        m->vod_catchup = catchup > 0 ? catchup : 0;
    }
}

int zms_vod_flv_muxer_video_armed(const zms_vod_flv_muxer *m)
{
    return m && m->video_armed ? 1 : 0;
}

int zms_vod_flv_muxer_catchup(const zms_vod_flv_muxer *m)
{
    return m ? m->vod_catchup : 0;
}

int zms_vod_flv_muxer_next_rtmp_media(zms_vod_flv_muxer *m, zms_vod_flv_rtmp_media *meta,
                                      uint8_t *body, size_t body_cap, size_t *body_len)
{
    uint8_t type_id = 0;
    uint32_t ts = 0;
    int r;

    if (!m || !meta || !body || !body_len) {
        return -1;
    }
    r = vod_pull_media(m, body, body_cap, body_len, &type_id, &ts);
    if (r <= 0) {
        return r;
    }
    meta->msg_type = type_id;
    meta->tag_dts_ms = ts;
    return 1;
}

int zms_vod_flv_muxer_next(zms_vod_flv_muxer *m, uint8_t *out, size_t cap, size_t *out_len)
{
    if (!m || !out) {
        return -1;
    }

    if (!m->sent_metadata && m->source) {
        uint8_t stack[16384];
        uint8_t *amf = stack;
        uint8_t *heap = NULL;
        size_t amf_cap = sizeof(stack);
        double dur_override = m->vod_duration_ms > 0 ? m->vod_duration_ms / 1000.0 : 0.0;
        size_t amf_len;
        const zms_vod_flv_index_view *kv = m->vod_flv_view;
        const zms_vod_flv_index *kf = kv ? NULL : m->vod_flv_index_full;

        if ((kv && kv->count > 400) || (kf && kf->count > 400)) {
            amf_cap = 8192 + (kv ? kv->count : kf->count) * 24;
        }
        amf_len = zms_vod_flv_metadata_encode(m->source, amf, amf_cap, dur_override, kf, kv);
        if (amf_len == 0 || amf_len > amf_cap) {
            amf_cap = amf_len > amf_cap ? amf_len + 256 : sizeof(stack);
            heap = (uint8_t *)malloc(amf_cap);
            if (!heap) {
                return -1;
            }
            amf = heap;
            amf_len = zms_vod_flv_metadata_encode(m->source, amf, amf_cap, dur_override, kf, kv);
        }
        if (amf_len > 0) {
            if (zms_flv_write_tag(out, cap, out_len, 18, 0, amf, amf_len) != ZTK_OK) {
                free(heap);
                return -1;
            }
            m->sent_metadata = 1;
            free(heap);
            return 1;
        }
        free(heap);
        m->sent_metadata = 1;
    }

    if (!m->sent_video_cfg) {
        size_t clen = 0;
        const uint8_t *cfg = NULL;

        if (m->vod_video_cfg && m->vod_video_cfg_len) {
            cfg = m->vod_video_cfg;
            clen = m->vod_video_cfg_len;
        } else if (m->source) {
            cfg = zms_media_source_video_config(m->source, &clen);
        }
        if (cfg && clen) {
            int w = zms_flv_mux_write_video_cfg_tag(cfg, clen, out, cap, out_len);
            if (w == 1) {
                m->sent_video_cfg = 1;
                return 1;
            }
            if (w < 0) {
                return -1;
            }
        }
        m->sent_video_cfg = 1;
    }

    if (!m->sent_audio_cfg) {
        size_t clen = 0;
        const uint8_t *cfg = NULL;

        if (m->vod_audio_cfg && m->vod_audio_cfg_len) {
            cfg = m->vod_audio_cfg;
            clen = m->vod_audio_cfg_len;
        } else if (m->source) {
            cfg = zms_media_source_audio_config(m->source, &clen);
        }
        if (cfg && clen) {
            if (zms_flv_write_tag(out, cap, out_len, 8, 0, cfg, clen) != ZTK_OK) {
                return -1;
            }
            m->sent_audio_cfg = 1;
            return 1;
        }
        m->sent_audio_cfg = 1;
    }

    {
        uint8_t tag_stack[4096];
        uint8_t *tag_buf;
        size_t tag_cap = cap > sizeof(tag_stack) ? cap : sizeof(tag_stack);
        size_t blen = 0;
        uint8_t type_id = 0;
        uint32_t ts = 0;
        int r;

        tag_buf = zms_flv_mux_tag_buf_poller(&m->tag_buf, &m->tag_cap, tag_cap, tag_stack,
                                             sizeof(tag_stack), m->io_poller);
        if (!tag_buf) {
            return 0;
        }
        r = vod_pull_media(m, tag_buf, tag_cap, &blen, &type_id, &ts);
        if (r <= 0) {
            return r;
        }
        if (zms_flv_write_tag(out, cap, out_len, type_id, ts, tag_buf, blen) != ZTK_OK) {
            return -1;
        }
        return 1;
    }
}
