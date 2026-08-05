#include "zms/vod/vod_codec_ext.h"
#include <string.h>

#define ZMS_VOD_CODEC_OPS_MAX 8

static const zms_vod_codec_ops *g_codec_ops[ZMS_VOD_CODEC_OPS_MAX];
static int g_codec_ops_count;
static int g_vod_codec_ext_builtins_registered;

void zms_vod_codec_ext_register(const zms_vod_codec_ops *ops)
{
    int i;

    if (!ops || !ops->name || !ops->name[0]) {
        return;
    }
    for (i = 0; i < g_codec_ops_count; ++i) {
        if (g_codec_ops[i] && strcmp(g_codec_ops[i]->name, ops->name) == 0) {
            g_codec_ops[i] = ops;
            return;
        }
    }
    if (g_codec_ops_count >= ZMS_VOD_CODEC_OPS_MAX) {
        return;
    }
    g_codec_ops[g_codec_ops_count++] = ops;
}

static int hevc_probe_mp4_stub(const char *file_path)
{
    (void)file_path;
    return 0;
}

static const zms_vod_codec_ops k_hevc_ops = {
    .name = ZMS_VOD_CODEC_HEVC,
    .probe_mp4 = hevc_probe_mp4_stub,
    .lane_prepare = NULL,
    .lane_destroy = NULL,
};

void zms_vod_codec_ext_register_all(void)
{
    if (g_vod_codec_ext_builtins_registered) {
        return;
    }
    g_vod_codec_ext_builtins_registered = 1;
    zms_vod_codec_ext_register(&k_hevc_ops);
}
