#include "zms/live/ingest/common/protocol_opts.h"
#include <string.h>

void zms_protocol_opts_default(zms_protocol_opts *opts)
{
    if (!opts) {
        return;
    }
    memset(opts, 0, sizeof(*opts));
    opts->enable_rtmp = 1;
    opts->enable_rtsp = 1;
    opts->enable_srt = 1;
    opts->enable_hls = 1;
    opts->enable_audio = 1;
    opts->modify_stamp = 0;
}
