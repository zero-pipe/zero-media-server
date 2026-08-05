#if defined(ZMS_WEBRTC_USE_LIBICE) && ZMS_WEBRTC_USE_LIBICE

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include "webrtc/session/webrtc_ice_internal.h"
#include "ztk/poller/poller.h"
#include "ztk/util/timer.h"
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

static ztk_poller *g_ice_poller;
static unsigned g_ice_port_refs;

struct stun_timer_slot {
    ztk_timer *ztk;
    int active;
    void (*user_cb)(void *param);
    void *user_param;
};

void zms_webrtc_ice_port_init(ztk_poller *poller)
{
    if (!poller) {
        return;
    }
    if (g_ice_port_refs++ == 0) {
        g_ice_poller = poller;
    }
}

void zms_webrtc_ice_port_fini(void)
{
    if (g_ice_port_refs == 0) {
        return;
    }
    if (--g_ice_port_refs == 0) {
        g_ice_poller = NULL;
    }
}

int read_random(void *ptr, int bytes)
{
    uint8_t *p = (uint8_t *)ptr;
    int i;

    if (!p || bytes <= 0) {
        return -1;
    }
    for (i = 0; i < bytes; ++i) {
        p[i] = (uint8_t)(rand() & 0xff);
    }
    return 0;
}

static void stun_timer_bridge(void *user)
{
    struct stun_timer_slot *slot = (struct stun_timer_slot *)user;
    void (*cb)(void *) = slot->user_cb;
    void *param = slot->user_param;

    slot->active = 0;
    if (cb) {
        cb(param);
    }
}

void *stun_timer_start(int ms, void (*ontimer)(void *param), void *param)
{
    struct stun_timer_slot *slot;
    ztk_poller *pol;

    pol = zms_webrtc_ice_timer_poller(param);
    if (!pol) {
        pol = g_ice_poller;
    }
    if (!pol || ms <= 0 || !ontimer) {
        return NULL;
    }
    slot = (struct stun_timer_slot *)calloc(1, sizeof(*slot));
    if (!slot) {
        return NULL;
    }
    slot->active = 1;
    slot->user_cb = ontimer;
    slot->user_param = param;
    slot->ztk = ztk_timer_start(pol, (uint64_t)ms, 0, stun_timer_bridge, slot);
    if (!slot->ztk) {
        free(slot);
        return NULL;
    }
    return slot;
}

int stun_timer_stop(void *timer)
{
    struct stun_timer_slot *slot = (struct stun_timer_slot *)timer;

    if (!slot) {
        return -1;
    }
    if (!slot->active) {
        free(slot);
        return 1;
    }
    slot->active = 0;
    ztk_timer_stop(slot->ztk);
    free(slot);
    return 0;
}

#endif /* ZMS_WEBRTC_USE_LIBICE */
