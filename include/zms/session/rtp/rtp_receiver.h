#ifndef ZMS_SESSION_RTP_RECEIVER_H
#define ZMS_SESSION_RTP_RECEIVER_H

#include "zms/zms_export.h"
#include "ztk/ztk_errno.h"
#include "zms/media/wire/rtp_packet.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*zms_rtp_sorted_cb)(const zms_rtp_packet *pkt, int track_index, void *user);

typedef struct zms_rtp_receiver zms_rtp_receiver;

typedef struct zms_rtp_receiver_opts {
    unsigned max_track;
    unsigned jitter_slots;
    zms_rtp_sorted_cb on_sorted;
    void *user;
    /** 跳过 seq 缺口前的重排保持（毫秒）；0 → 200（ZLM 风格 rtp sort）。 */
    int jitter_ms;
} zms_rtp_receiver_opts;

ZMS_API zms_rtp_receiver *zms_rtp_receiver_create(const zms_rtp_receiver_opts *opts);
ZMS_API void zms_rtp_receiver_destroy(zms_rtp_receiver *r);
ZMS_API ztk_err_t zms_rtp_receiver_input(zms_rtp_receiver *r, int track_index, const uint8_t *data,
                                         size_t len);
ZMS_API void zms_rtp_receiver_flush(zms_rtp_receiver *r);
ZMS_API void zms_rtp_receiver_reset(zms_rtp_receiver *r);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_SESSION_RTP_RECEIVER_H */
