#include "zms/live/play/dash/http_dash_sidecar.h"
#include "zms/live/play/dash/http_dash_segmenter.h"
#include "zms/live/play/hls/http_hls_sidecar.h"
#include "zms/egress/egress_segment_recorder.h"
#include "zms/engine/stream/stream_hub.h"
#include "zms/vod/io/vod_source.h"
#include "ztk/util/log.h"

void zms_http_dash_ensure_recorder(zms_media_source *src, ztk_poller *poller)
{
    zms_http_dash_segmenter_opts ropts;
    ztk_err_t err;
    ztk_poller *bind_pol;

    (void)poller;
    if (!src || zms_media_source_is_vod(src)) {
        return;
    }

    zms_http_dash_segmenter_default_opts(&ropts);
    bind_pol = zms_http_hls_main_poller();
    err = zms_segment_recorder_ensure_live(src, ZMS_SEGMENT_REC_DASH, bind_pol, &ropts);
    if (err != ZTK_OK) {
        ztk_warn("DASH ensure recorder failed: %s/%s err=%d", src->app, src->stream, (int)err);
    }
}
