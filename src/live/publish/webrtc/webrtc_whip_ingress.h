#ifndef ZMS_LIVE_PUBLISH_WEBRTC_WEBRTC_WHIP_INGRESS_H
#define ZMS_LIVE_PUBLISH_WEBRTC_WEBRTC_WHIP_INGRESS_H

#include "ztk/ztk_errno.h"
#include <stddef.h>

struct zms_webrtc_session;

void zms_webrtc_whip_ingress_on_udp(struct zms_webrtc_session *s, const void *data, size_t len);
void zms_webrtc_whip_ingress_stop(struct zms_webrtc_session *s);

#endif /* ZMS_LIVE_PUBLISH_WEBRTC_WEBRTC_WHIP_INGRESS_H */
