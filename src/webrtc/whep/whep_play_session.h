#ifndef ZMS_WEBRTC_WHEP_PLAY_SESSION_H
#define ZMS_WEBRTC_WHEP_PLAY_SESSION_H

#include "ztk/ztk_errno.h"
#include <stddef.h>

struct zms_webrtc_session;

ztk_err_t zms_webrtc_play_on_stun_dtls(struct zms_webrtc_session *s, const void *data, size_t len);
void zms_webrtc_play_input(struct zms_webrtc_session *s, const void *data, size_t len);
void zms_webrtc_play_stop(struct zms_webrtc_session *s);

#endif /* ZMS_WEBRTC_WHEP_PLAY_SESSION_H */
