#include "zms/session/srt/srt_streamid.h"
#include "zms/engine/stream/stream_hub.h"
#include <string.h>

static const char *streamid_key_value(const char *streamid, const char *key, char *out,
                                      size_t out_cap)
{
    const char *p;
    size_t klen;
    size_t i;

    if (!streamid || !key || !out || out_cap == 0) {
        return NULL;
    }
    klen = strlen(key);
    for (p = streamid; *p; ++p) {
        if (strncmp(p, key, klen) == 0) {
            p += klen;
            for (i = 0; p[i] && p[i] != ',' && i + 1 < out_cap; ++i) {
                out[i] = p[i];
            }
            out[i] = '\0';
            return out;
        }
    }
    return NULL;
}

int zms_srt_streamid_parse(const char *streamid, char *app, char *stream, zms_srt_stream_mode *mode)
{
    char resource[ZMS_STREAM_MAX + ZMS_APP_MAX];
    char mode_buf[32];
    const char *m;

    if (!streamid || !app || !stream) {
        return -1;
    }
    app[0] = streamid[0] ? '\0' : '\0';
    stream[0] = '\0';
    if (mode) {
        *mode = ZMS_SRT_MODE_PUBLISH;
    }

    if (!streamid[0]) {
        return -1;
    }

    if (!streamid_key_value(streamid, "r=", resource, sizeof(resource)) &&
        !streamid_key_value(streamid, "h=", resource, sizeof(resource))) {
        if (strchr(streamid, '/')) {
            zms_media_split_path(streamid, app, stream);
            return (app[0] && stream[0]) ? 0 : -1;
        }
        return -1;
    }

    zms_media_split_path(resource, app, stream);
    if (!app[0] || !stream[0]) {
        return -1;
    }

    m = streamid_key_value(streamid, "m=", mode_buf, sizeof(mode_buf));
    if (m && mode) {
        if (strncmp(mode_buf, "publish", 7) == 0) {
            *mode = ZMS_SRT_MODE_PUBLISH;
        } else if (strncmp(mode_buf, "request", 7) == 0 || strncmp(mode_buf, "play", 4) == 0) {
            *mode = ZMS_SRT_MODE_PLAY;
        } else {
            *mode = ZMS_SRT_MODE_INVALID;
        }
    }
    if (mode && *mode == ZMS_SRT_MODE_INVALID) {
        return -1;
    }
    return 0;
}
