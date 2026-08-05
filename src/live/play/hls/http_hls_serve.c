#include "session/http/http_session_internal.h"
#include "zms/live/play/hls/http_hls_sidecar.h"
#include "zms/session/codec_filter.h"
#include "zms/live/play/hls/http_hls_playlist.h"
#include "zms/live/play/hls/http_hls_segmenter.h"
#include "zms/egress/egress_segment_recorder.h"
#include "zms/engine/media_event.h"
#include "ztk/util/buf.h"
#include "ztk/util/log.h"
#include <string.h>
#if !defined(_WIN32)
#include <strings.h>
#endif

static void hls_http_done(zms_http_hls_segmenter *rec)
{
    zms_http_hls_segmenter_http_leave(rec);
}

static void hls_http_done_request(zms_http_session *hs, zms_http_hls_segmenter *rec)
{
    (void)hs;
    if (rec) {
        hls_http_done(rec);
    }
}

void zms_http_live_hls_serve(zms_http_session *hs, zms_media_source *src, const char *app,
                             const char *stream, const char *file)
{
    zms_http_hls_segmenter *rec;
    size_t m3u8_len = 0;
    size_t ts_len = 0;
    int seg_count = 0;

    if (!hs || !src || !src->gop_queue) {
        zms_http_response_send_error(hs, 404, "Not Found");
        return;
    }

    if (zms_session_capability_check_source(ZMS_PROTO_CAP_HLS_PLAY, src) != ZTK_OK) {
        zms_session_capability_log_reject("hls", src, ZMS_PROTO_CAP_HLS_PLAY);
        zms_http_response_send_error(hs, 406, "Not Acceptable");
        return;
    }

    zms_http_hls_ensure_recorder(src, zms_http_hls_main_poller());
    rec = (zms_http_hls_segmenter *)zms_media_source_segment_rec_get(src, ZMS_SEGMENT_REC_HLS);
    if (!rec) {
        ztk_warn("HLS live 404: recorder unavailable app=%s stream=%s (see HLS ensure recorder "
                 "failed above)",
                 app, stream);
        zms_http_response_send_error(hs, 404, "Not Found");
        return;
    }

    zms_http_hls_segmenter_http_enter(rec);

    if (file && file[0] && strlen(file) >= 5 &&
#if defined(_WIN32)
        _stricmp(file + strlen(file) - 5, ".m3u8") == 0
#else
        strcasecmp(file + strlen(file) - 5, ".m3u8") == 0
#endif
    ) {
        static const char k_hls_empty[] =
            "#EXTM3U\r\n#EXT-X-VERSION:3\r\n#EXT-X-TARGETDURATION:2\r\n#EXT-X-MEDIA-SEQUENCE:0\r\n";

        if (zms_http_hls_segmenter_serve_m3u8(rec, (char *)hs->send_buf, hs->send_cap, &m3u8_len, 0,
                                              &seg_count) != ZTK_OK ||
            m3u8_len == 0 || seg_count <= 0) {
            ztk_info("HLS live m3u8 empty: app=%s stream=%s", app, stream);
            zms_http_response_send_bytes(hs, 200, "application/vnd.apple.mpegurl", k_hls_empty,
                                         sizeof(k_hls_empty) - 1);
            hls_http_done_request(hs, rec);
            return;
        }
        m3u8_len = zms_http_route_rewrite_live_hls_m3u8((char *)hs->send_buf, hs->send_cap,
                                                        src->app, src->stream);
        zms_media_event_play(src, "hls");
        {
            char latest[64];

            latest[0] = '\0';
            (void)zms_http_hls_playlist_latest_segment(zms_http_hls_segmenter_playlist(rec), latest,
                                                       sizeof(latest), NULL);
            ztk_info("HLS live m3u8: app=%s stream=%s len=%u segs=%d latest=%s", app, stream,
                     (unsigned)m3u8_len, seg_count, latest[0] ? latest : "-");
        }
        zms_http_response_send_bytes(hs, 200, "application/vnd.apple.mpegurl", hs->send_buf,
                                     m3u8_len);
        hls_http_done_request(hs, rec);
        return;
    }

    if (!file || !file[0]) {
        zms_http_response_send_error(hs, 404, "Not Found");
        hls_http_done_request(hs, rec);
        return;
    }

    {
        ztk_buf *seg = NULL;

        ztk_info("HLS live segment req: app=%s stream=%s file=%s", app, stream, file);
        if (zms_http_hls_segmenter_ref_segment(rec, file, &seg, &ts_len, 0) != ZTK_OK || !seg ||
            ts_len == 0) {
            ztk_warn("HLS live segment 404: app=%s stream=%s file=%s", app, stream, file);
            zms_http_response_send_error(hs, 404, "Not Found");
            hls_http_done_request(hs, rec);
            return;
        }
        zms_media_event_play(src, "hls");
        {
            const char *ctype = "video/mp2t";
            size_t flen = strlen(file);
#if defined(_WIN32)
            if (flen >= 4 &&
                (_stricmp(file + flen - 4, ".m4s") == 0 || _stricmp(file + flen - 4, ".mp4") == 0))
#else
            if (flen >= 4 && (strcasecmp(file + flen - 4, ".m4s") == 0 ||
                              strcasecmp(file + flen - 4, ".mp4") == 0))
#endif
                ctype = "video/mp4";
            ztk_info("HLS live segment: app=%s stream=%s file=%s len=%u", app, stream, file,
                     (unsigned)ts_len);
            zms_http_response_send_bytes_buf(hs, 200, ctype, seg);
        }
        ztk_buf_unref(seg);
        hls_http_done_request(hs, rec);
    }
}
