#include "zms/live/play/dash/http_dash_playlist.h"
#include "dash-mpd.h"
#include "dash-proto.h"
#include "ztk/util/buf.h"
#include "ztk/thread/sync.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

struct zms_http_dash_playlist {
    dash_mpd_t *mpd;
    char prefix[64];
    unsigned seg_number;
    zms_http_dash_segment init_video;
    zms_http_dash_segment init_audio;
    zms_http_dash_segment *video_segments;
    zms_http_dash_segment *audio_segments;
    int video_seg_count;
    int video_seg_head;
    int audio_seg_count;
    int audio_seg_head;
    char *mpd_cache;
    size_t mpd_cache_cap;
    size_t mpd_cache_len;
    ztk_mutex *mtx;
};

static void dash_playlist_refresh_mpd_cache_locked(zms_http_dash_playlist *m)
{
    size_t n;

    if (!m || !m->mpd) {
        return;
    }
    if (!m->mpd_cache) {
        m->mpd_cache_cap = 16384;
        m->mpd_cache = (char *)malloc(m->mpd_cache_cap);
        if (!m->mpd_cache) {
            return;
        }
    }
    n = dash_mpd_playlist(m->mpd, m->mpd_cache, m->mpd_cache_cap);
    if (n > 0 && n < m->mpd_cache_cap) {
        m->mpd_cache_len = n;
    } else {
        m->mpd_cache_len = 0;
    }
}

static size_t dash_seg_len(const zms_http_dash_segment *s)
{
    return s && s->buf ? ztk_buf_len(s->buf) : 0;
}

static void dash_seg_free(zms_http_dash_segment *s)
{
    if (!s) {
        return;
    }
    if (s->buf) {
        ztk_buf_unref(s->buf);
    }
    s->buf = NULL;
    s->name[0] = '\0';
}

static int dash_is_init_segment(const char *name)
{
    return name && strstr(name, "-init.");
}

static int dash_is_audio_segment(const char *name)
{
    return name && strstr(name, ".m4a");
}

static zms_http_dash_segment *dash_init_slot(zms_http_dash_playlist *m, const char *name)
{
    if (!m || !name) {
        return NULL;
    }
    if (strstr(name, "-init.m4a")) {
        return &m->init_audio;
    }
    if (strstr(name, "-init.m4v")) {
        return &m->init_video;
    }
    return NULL;
}

/** MPD 用 $Time$ 模板 + SegmentTimeline，完整分片名不在 XML 里；按 t="TS" 判断是否在播表窗口。 */
static int dash_name_in_mpd_cache(const zms_http_dash_playlist *m, const char *name)
{
    const char *prefix;
    size_t plen;
    const char *rest;
    const char *dot;
    char *end;
    int64_t ts;
    char pat[48];

    if (!m || !name || !name[0] || !m->mpd_cache || m->mpd_cache_len == 0) {
        return 0;
    }
    if (dash_is_init_segment(name)) {
        return strstr(m->mpd_cache, name) != NULL;
    }

    prefix = m->prefix[0] ? m->prefix : "live";
    plen = strlen(prefix);
    if (strncmp(name, prefix, plen) != 0 || name[plen] != '-') {
        return 0;
    }
    rest = name + plen + 1;
    dot = strrchr(rest, '.');
    if (!dot || dot == rest) {
        return 0;
    }
    ts = strtoll(rest, &end, 10);
    if (!end || end == rest || end != dot) {
        return 0;
    }
    snprintf(pat, sizeof(pat), "<S t=\"%" PRId64 "\"", ts);
    return strstr(m->mpd_cache, pat) != NULL;
}

static void dash_slot_adopt(zms_http_dash_segment *slot, const char *name, ztk_buf *buf)
{
    if (!slot || !name || !buf || ztk_buf_len(buf) == 0) {
        return;
    }
    dash_seg_free(slot);
    snprintf(slot->name, sizeof(slot->name), "%s", name);
    slot->buf = buf;
}

static int dash_slot_matches_name(const zms_http_dash_segment *slot, const char *name)
{
    return slot && name && name[0] && slot->name[0] && strcmp(slot->name, name) == 0;
}

static void dash_store_media_locked(zms_http_dash_playlist *m, const char *name, ztk_buf *buf)
{
    int is_audio = dash_is_audio_segment(name);
    zms_http_dash_segment *slots = is_audio ? m->audio_segments : m->video_segments;
    int *head = is_audio ? &m->audio_seg_head : &m->video_seg_head;
    int *count = is_audio ? &m->audio_seg_count : &m->video_seg_count;
    zms_http_dash_segment *slot;
    int i;
    int victim = -1;

    if (!slots || !buf) {
        return;
    }

    for (i = 0; i < (int)m->seg_number; ++i) {
        if (dash_slot_matches_name(&slots[i], name)) {
            dash_slot_adopt(&slots[i], name, buf);
            return;
        }
    }

    for (i = 0; i < (int)m->seg_number; ++i) {
        if (!slots[i].buf || slots[i].name[0] == '\0') {
            victim = i;
            break;
        }
        if (!dash_name_in_mpd_cache(m, slots[i].name)) {
            victim = i;
            break;
        }
    }
    if (victim < 0) {
        for (i = 0; i < (int)m->seg_number; ++i) {
            if (!dash_name_in_mpd_cache(m, slots[i].name)) {
                victim = i;
                break;
            }
        }
    }
    if (victim < 0) {
        victim = *head % (int)m->seg_number;
    }

    slot = &slots[victim];
    dash_slot_adopt(slot, name, buf);
    *head = (victim + 1) % (int)m->seg_number;
    if (*count < (int)m->seg_number) {
        ++*count;
    }
}

static void dash_store_segment_locked(zms_http_dash_playlist *m, const char *name, ztk_buf *buf)
{
    zms_http_dash_segment *slot;

    if (!m || !name || !buf || ztk_buf_len(buf) == 0) {
        return;
    }
    slot = dash_init_slot(m, name);
    if (slot) {
        dash_slot_adopt(slot, name, buf);
        return;
    }
    dash_store_media_locked(m, name, buf);
}

int zms_http_dash_playlist_on_segment(void *param, int track, const void *data, size_t bytes,
                                      int64_t pts, int64_t dts, int64_t duration, const char *name)
{
    zms_http_dash_playlist *m = (zms_http_dash_playlist *)param;
    ztk_buf *buf;

    (void)track;
    (void)pts;
    (void)dts;
    (void)duration;
    if (!m || !data || bytes == 0 || !name || !name[0]) {
        return -1;
    }

    buf = ztk_buf_alloc(bytes);
    if (!buf) {
        return -1;
    }
    memcpy((void *)ztk_buf_data(buf), data, bytes);
    ztk_buf_set_len(buf, bytes);

    ztk_mutex_lock(m->mtx);
    dash_store_segment_locked(m, name, buf);
    dash_playlist_refresh_mpd_cache_locked(m);
    ztk_mutex_unlock(m->mtx);
    return 0;
}

zms_http_dash_playlist *zms_http_dash_playlist_create(const zms_http_dash_playlist_opts *opts)
{
    zms_http_dash_playlist *m = (zms_http_dash_playlist *)calloc(1, sizeof(*m));

    if (!m) {
        return NULL;
    }
    m->seg_number = opts && opts->segment_count ? opts->segment_count : 12;
    if (m->seg_number < 6) {
        m->seg_number = 6;
    }
    /* 内存表容量 2×MPD 窗口，降低 MPD 仍引用时分片被环覆盖导致 404 */
    m->seg_number *= 2u;
    if (opts && opts->prefix && opts->prefix[0]) {
        snprintf(m->prefix, sizeof(m->prefix), "%s", opts->prefix);
    } else {
        strncpy(m->prefix, "live", sizeof(m->prefix) - 1);
    }

    m->mpd = dash_mpd_create(DASH_DYNAMIC, zms_http_dash_playlist_on_segment, m);
    m->mtx = ztk_mutex_create(0);
    m->video_segments =
        (zms_http_dash_segment *)calloc(m->seg_number, sizeof(zms_http_dash_segment));
    m->audio_segments =
        (zms_http_dash_segment *)calloc(m->seg_number, sizeof(zms_http_dash_segment));
    if (!m->mpd || !m->mtx || !m->video_segments || !m->audio_segments) {
        dash_mpd_destroy(m->mpd);
        ztk_mutex_destroy(m->mtx);
        free(m->video_segments);
        free(m->audio_segments);
        free(m);
        return NULL;
    }
    return m;
}

void zms_http_dash_playlist_destroy(zms_http_dash_playlist *m)
{
    int i;

    if (!m) {
        return;
    }
    dash_mpd_destroy(m->mpd);
    ztk_mutex_destroy(m->mtx);
    dash_seg_free(&m->init_video);
    dash_seg_free(&m->init_audio);
    if (m->video_segments) {
        for (i = 0; i < (int)m->seg_number; ++i) {
            dash_seg_free(&m->video_segments[i]);
        }
        free(m->video_segments);
    }
    if (m->audio_segments) {
        for (i = 0; i < (int)m->seg_number; ++i) {
            dash_seg_free(&m->audio_segments[i]);
        }
        free(m->audio_segments);
    }
    free(m->mpd_cache);
    free(m);
}

struct dash_mpd_t *zms_http_dash_playlist_mpd(zms_http_dash_playlist *m)
{
    return m ? m->mpd : NULL;
}

static int dash_segment_slot_matches(const zms_http_dash_segment *s, const char *name)
{
    return s && name && name[0] && s->buf && dash_seg_len(s) > 0 && s->name[0] &&
           strcmp(s->name, name) == 0;
}

static int dash_ring_has_name(const zms_http_dash_segment *slots, int nslots, const char *name)
{
    int i;

    for (i = 0; i < nslots; ++i) {
        if (dash_segment_slot_matches(&slots[i], name)) {
            return 1;
        }
    }
    return 0;
}

static int dash_segment_in_cache_locked(const zms_http_dash_playlist *m, const char *name)
{
    if (!m || !name || !name[0]) {
        return 0;
    }
    if (dash_segment_slot_matches(&m->init_video, name) ||
        dash_segment_slot_matches(&m->init_audio, name)) {
        return 1;
    }
    if (dash_is_audio_segment(name)) {
        return dash_ring_has_name(m->audio_segments, (int)m->seg_number, name);
    }
    return dash_ring_has_name(m->video_segments, (int)m->seg_number, name);
}

static int dash_ext_matches(const char *name, const char *ext)
{
    size_t nlen;
    size_t elen;

    if (!name || !ext) {
        return 0;
    }
    nlen = strlen(name);
    elen = strlen(ext);
    if (nlen < elen) {
        return 0;
    }
    return strcmp(name + nlen - elen, ext) == 0;
}

static int dash_pick_from_ring(const zms_http_dash_playlist *m, const zms_http_dash_segment *slots,
                               const char *prefix, size_t plen, const char *rest, const char *dot,
                               char *out, size_t cap)
{
    int i;
    int64_t want_ts;
    char *end;
    char want[128];

    (void)m;
    if (!prefix || !rest || !dot || !out || cap == 0) {
        return 0;
    }

    want_ts = strtoll(rest, &end, 10);
    if (!end || end == rest || strcmp(end, dot) != 0) {
        return 0;
    }

    snprintf(want, sizeof(want), "%.*s-%.*s%s", (int)plen, prefix, (int)(end - rest), rest, dot);

    for (i = 0; i < (int)m->seg_number; ++i) {
        const zms_http_dash_segment *s = &slots[i];

        if (!s->buf || dash_seg_len(s) == 0 || !s->name[0] || dash_is_init_segment(s->name)) {
            continue;
        }
        if (strcmp(s->name, want) == 0) {
            snprintf(out, cap, "%s", s->name);
            return 1;
        }
        if (strncmp(s->name, prefix, plen) != 0 || s->name[plen] != '-') {
            continue;
        }
        if (strtoll(s->name + plen + 1, &end, 10) == want_ts && end && strcmp(end, dot) == 0) {
            snprintf(out, cap, "%s", s->name);
            return 1;
        }
    }
    return 0;
}

static int dash_resolve_segment_name(zms_http_dash_playlist *m, const char *req, char *out,
                                     size_t cap)
{
    const char *prefix;
    size_t plen;
    const char *rest;
    const char *dot;
    const zms_http_dash_segment *slots;
    int found = 0;

    if (!m || !req || !out || cap == 0) {
        return 0;
    }

    ztk_mutex_lock(m->mtx);
    if (dash_segment_in_cache_locked(m, req)) {
        snprintf(out, cap, "%s", req);
        ztk_mutex_unlock(m->mtx);
        return 1;
    }

    prefix = m->prefix[0] ? m->prefix : "live";
    plen = strlen(prefix);
    if (strncmp(req, prefix, plen) != 0 || req[plen] != '-') {
        ztk_mutex_unlock(m->mtx);
        return 0;
    }
    rest = req + plen + 1;
    dot = strrchr(rest, '.');
    if (!dot || dot == rest) {
        ztk_mutex_unlock(m->mtx);
        return 0;
    }

    slots = dash_is_audio_segment(req) ? m->audio_segments : m->video_segments;
    found = dash_pick_from_ring(m, slots, prefix, plen, rest, dot, out, cap);
    ztk_mutex_unlock(m->mtx);
    return found;
}

int zms_http_dash_playlist_has_media_segment(const zms_http_dash_playlist *m)
{
    int i;
    int found = 0;

    if (!m) {
        return 0;
    }
    ztk_mutex_lock(m->mtx);
    for (i = 0; i < (int)m->seg_number; ++i) {
        const zms_http_dash_segment *s = &m->video_segments[i];
        if (s->buf && dash_seg_len(s) > 0 && s->name[0] && !dash_is_init_segment(s->name)) {
            found = 1;
            break;
        }
    }
    ztk_mutex_unlock(m->mtx);
    return found;
}

ztk_err_t zms_http_dash_playlist_copy_mpd(const zms_http_dash_playlist *m, char *out, size_t cap,
                                          size_t *out_len)
{
    if (!m || !out || !out_len) {
        return ZTK_ERR_INVALID;
    }
    *out_len = 0;
    ztk_mutex_lock(m->mtx);
    if (m->mpd_cache_len == 0 || m->mpd_cache_len > cap) {
        ztk_mutex_unlock(m->mtx);
        return ZTK_ERR_INVALID;
    }
    memcpy(out, m->mpd_cache, m->mpd_cache_len);
    *out_len = m->mpd_cache_len;
    ztk_mutex_unlock(m->mtx);
    return ZTK_OK;
}

ztk_err_t zms_http_dash_playlist_build_mpd(zms_http_dash_playlist *m, char *out, size_t cap,
                                           size_t *out_len)
{
    if (!m || !m->mpd || !out || !out_len) {
        return ZTK_ERR_INVALID;
    }
    ztk_mutex_lock(m->mtx);
    dash_playlist_refresh_mpd_cache_locked(m);
    if (m->mpd_cache_len == 0 || m->mpd_cache_len > cap) {
        ztk_mutex_unlock(m->mtx);
        return ZTK_ERR_INVALID;
    }
    memcpy(out, m->mpd_cache, m->mpd_cache_len);
    *out_len = m->mpd_cache_len;
    ztk_mutex_unlock(m->mtx);
    return ZTK_OK;
}

static ztk_err_t dash_copy_from_ring(const zms_http_dash_segment *slots, int nslots,
                                     const char *resolved, uint8_t *buf, size_t cap,
                                     size_t *out_len)
{
    int i;

    for (i = 0; i < nslots; ++i) {
        const zms_http_dash_segment *s = &slots[i];
        if (!dash_segment_slot_matches(s, resolved)) {
            continue;
        }
        if (dash_seg_len(s) > cap) {
            return ZTK_ERR_INVALID;
        }
        memcpy(buf, ztk_buf_data(s->buf), dash_seg_len(s));
        *out_len = dash_seg_len(s);
        return ZTK_OK;
    }
    return ZTK_ERR_INVALID;
}

ztk_err_t zms_http_dash_playlist_copy_segment(zms_http_dash_playlist *m, const char *name,
                                              uint8_t *buf, size_t cap, size_t *out_len)
{
    char resolved[128];
    ztk_err_t err;

    if (!m || !name || !buf || !out_len) {
        return ZTK_ERR_INVALID;
    }
    *out_len = 0;
    if (!dash_resolve_segment_name(m, name, resolved, sizeof(resolved))) {
        return ZTK_ERR_INVALID;
    }

    ztk_mutex_lock(m->mtx);
    if (dash_segment_slot_matches(&m->init_video, resolved) ||
        dash_segment_slot_matches(&m->init_audio, resolved)) {
        const zms_http_dash_segment *init_slots[2] = {&m->init_video, &m->init_audio};
        int j;

        for (j = 0; j < 2; ++j) {
            const zms_http_dash_segment *s = init_slots[j];
            if (!dash_segment_slot_matches(s, resolved)) {
                continue;
            }
            if (dash_seg_len(s) > cap) {
                ztk_mutex_unlock(m->mtx);
                return ZTK_ERR_INVALID;
            }
            memcpy(buf, ztk_buf_data(s->buf), dash_seg_len(s));
            *out_len = dash_seg_len(s);
            ztk_mutex_unlock(m->mtx);
            return ZTK_OK;
        }
    }

    if (dash_is_audio_segment(resolved)) {
        err =
            dash_copy_from_ring(m->audio_segments, (int)m->seg_number, resolved, buf, cap, out_len);
    } else {
        err =
            dash_copy_from_ring(m->video_segments, (int)m->seg_number, resolved, buf, cap, out_len);
    }
    ztk_mutex_unlock(m->mtx);
    return err;
}

static ztk_err_t dash_ref_from_ring(const zms_http_dash_segment *slots, int nslots,
                                    const char *resolved, ztk_buf **out_buf, size_t *out_len)
{
    int i;

    for (i = 0; i < nslots; ++i) {
        const zms_http_dash_segment *s = &slots[i];
        if (!dash_segment_slot_matches(s, resolved)) {
            continue;
        }
        *out_buf = ztk_buf_ref((ztk_buf *)s->buf);
        *out_len = dash_seg_len(s);
        return ZTK_OK;
    }
    return ZTK_ERR_INVALID;
}

ztk_err_t zms_http_dash_playlist_ref_segment(zms_http_dash_playlist *m, const char *name,
                                             ztk_buf **out_buf, size_t *out_len)
{
    char resolved[128];
    ztk_err_t err;

    if (!m || !name || !out_buf || !out_len) {
        return ZTK_ERR_INVALID;
    }
    *out_buf = NULL;
    *out_len = 0;
    if (!dash_resolve_segment_name(m, name, resolved, sizeof(resolved))) {
        return ZTK_ERR_INVALID;
    }

    ztk_mutex_lock(m->mtx);
    if (dash_segment_slot_matches(&m->init_video, resolved) ||
        dash_segment_slot_matches(&m->init_audio, resolved)) {
        const zms_http_dash_segment *init_slots[2] = {&m->init_video, &m->init_audio};
        int j;

        for (j = 0; j < 2; ++j) {
            const zms_http_dash_segment *s = init_slots[j];
            if (!dash_segment_slot_matches(s, resolved)) {
                continue;
            }
            *out_buf = ztk_buf_ref((ztk_buf *)s->buf);
            *out_len = dash_seg_len(s);
            ztk_mutex_unlock(m->mtx);
            return ZTK_OK;
        }
    }

    if (dash_is_audio_segment(resolved)) {
        err = dash_ref_from_ring(m->audio_segments, (int)m->seg_number, resolved, out_buf, out_len);
    } else {
        err = dash_ref_from_ring(m->video_segments, (int)m->seg_number, resolved, out_buf, out_len);
    }
    ztk_mutex_unlock(m->mtx);
    return err;
}

int zms_http_dash_playlist_segment_count(const zms_http_dash_playlist *m)
{
    return m ? m->video_seg_count + m->audio_seg_count : 0;
}

const char *zms_http_dash_playlist_prefix(const zms_http_dash_playlist *m)
{
    return m && m->prefix[0] ? m->prefix : "live";
}
