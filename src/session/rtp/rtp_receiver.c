#include "zms/session/rtp/rtp_receiver.h"
#include "zms/engine/media/media_limits.h"
#include "rtp-packet.h"
#include "rtp-queue.h"
#include <stdlib.h>
#include <string.h>

#define RTP_CLOCK_90KHZ 90000

struct zms_rtp_receiver {
    unsigned max_track;
    int jitter_ms;
    zms_rtp_sorted_cb on_sorted;
    void *user;
    rtp_queue_t **queues;
};

static struct rtp_packet_t *rtp_pkt_alloc(const uint8_t *data, size_t len)
{
    if (!data || len == 0 || len > 64 * 1024) {
        return NULL;
    }

    uint8_t *buf = (uint8_t *)malloc(sizeof(int) + sizeof(struct rtp_packet_t) + len);
    if (!buf) {
        return NULL;
    }

    *(int *)buf = (int)len;
    struct rtp_packet_t *pkt = (struct rtp_packet_t *)(buf + sizeof(int));
    memcpy(pkt + 1, data, len);
    if (rtp_packet_deserialize(pkt, pkt + 1, (int)len) != 0) {
        free(buf);
        return NULL;
    }
    return pkt;
}

static void rtp_pkt_free(void *param, struct rtp_packet_t *pkt)
{
    (void)param;
    if (!pkt) {
        return;
    }
    free((uint8_t *)pkt - sizeof(int));
}

static void drain_track(zms_rtp_receiver *r, int track_index)
{
    rtp_queue_t *q = r->queues[track_index];
    struct rtp_packet_t *pkt;
    while ((pkt = rtp_queue_read(q)) != NULL) {
        int bytes = *(int *)((uint8_t *)pkt - sizeof(int));
        const uint8_t *raw = (const uint8_t *)(pkt + 1);

        zms_rtp_packet out;
        if (zms_rtp_parse(raw, (size_t)bytes, &out) == ZTK_OK && r->on_sorted) {
            r->on_sorted(&out, track_index, r->user);
        }

        rtp_pkt_free(NULL, pkt);
    }
}

zms_rtp_receiver *zms_rtp_receiver_create(const zms_rtp_receiver_opts *opts)
{
    if (!opts || opts->max_track == 0) {
        return NULL;
    }

    zms_rtp_receiver *r = (zms_rtp_receiver *)calloc(1, sizeof(*r));
    if (!r) {
        return NULL;
    }

    int jitter_ms = opts->jitter_ms > 0 ? opts->jitter_ms : ZMS_RTP_JITTER_MS_UDP_DEFAULT;

    r->max_track = opts->max_track;
    r->jitter_ms = jitter_ms;
    r->on_sorted = opts->on_sorted;
    r->user = opts->user;

    r->queues = (rtp_queue_t **)calloc(opts->max_track, sizeof(rtp_queue_t *));
    if (!r->queues) {
        free(r);
        return NULL;
    }

    for (unsigned i = 0; i < opts->max_track; ++i) {
        r->queues[i] = rtp_queue_create(r->jitter_ms, RTP_CLOCK_90KHZ, rtp_pkt_free, NULL);
        if (!r->queues[i]) {
            zms_rtp_receiver_destroy(r);
            return NULL;
        }
    }
    return r;
}

void zms_rtp_receiver_destroy(zms_rtp_receiver *r)
{
    if (!r) {
        return;
    }
    if (r->queues) {
        for (unsigned i = 0; i < r->max_track; ++i) {
            if (r->queues[i]) {
                rtp_queue_destroy(r->queues[i]);
            }
        }
        free(r->queues);
    }
    free(r);
}

ztk_err_t zms_rtp_receiver_input(zms_rtp_receiver *r, int track_index, const uint8_t *data,
                                 size_t len)
{
    if (!r || track_index < 0 || (unsigned)track_index >= r->max_track || !data || len == 0) {
        return ZTK_ERR_INVALID;
    }

    struct rtp_packet_t *pkt = rtp_pkt_alloc(data, len);
    if (!pkt) {
        return ZTK_ERR_INVALID;
    }

    int wr = rtp_queue_write(r->queues[track_index], pkt);
    if (wr <= 0) {
        rtp_pkt_free(NULL, pkt);
        return wr < 0 ? ZTK_ERR_INVALID : ZTK_ERR_AGAIN;
    }

    drain_track(r, track_index);
    return ZTK_OK;
}

void zms_rtp_receiver_flush(zms_rtp_receiver *r)
{
    if (!r) {
        return;
    }
    for (unsigned i = 0; i < r->max_track; ++i) {
        if (r->queues[i]) {
            rtp_queue_destroy(r->queues[i]);
        }
        r->queues[i] = rtp_queue_create(r->jitter_ms, RTP_CLOCK_90KHZ, rtp_pkt_free, NULL);
    }
}

void zms_rtp_receiver_reset(zms_rtp_receiver *r)
{
    zms_rtp_receiver_flush(r);
}
