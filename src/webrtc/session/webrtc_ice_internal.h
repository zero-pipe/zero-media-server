#ifndef ZMS_SRC_WEBRTC_ICE_INTERNAL_H
#define ZMS_SRC_WEBRTC_ICE_INTERNAL_H
#include "webrtc/session/webrtc_session_internal.h"
#include <stddef.h>
struct zms_webrtc_ice;
void zms_webrtc_ice_port_init(ztk_poller *poller);
void zms_webrtc_ice_port_fini(void);
/** libice stun_timer_start：将 timer param 映射到 session poller（回退：service poller）。 */
ztk_poller *zms_webrtc_ice_timer_poller(void *param);
struct zms_webrtc_ice *zms_webrtc_ice_create(zms_webrtc_session *session);
void zms_webrtc_ice_destroy(struct zms_webrtc_ice *ice);
int zms_webrtc_ice_setup(struct zms_webrtc_ice *ice, const char *offer, size_t offer_len,
                         const char *local_ufrag, const char *local_pwd, const char *remote_ufrag,
                         const char *remote_pwd, const char *advertise_host, uint16_t local_port);
void zms_webrtc_ice_on_udp(struct zms_webrtc_ice *ice, const char *peer_ip, uint16_t peer_port,
                           const void *data, size_t len);
int zms_webrtc_ice_send(struct zms_webrtc_ice *ice, const void *data, size_t len);
int zms_webrtc_ice_connected(const struct zms_webrtc_ice *ice);
int zms_webrtc_session_send_udp(zms_webrtc_session *s, const void *data, size_t len);
void zms_webrtc_session_try_dtls_client(zms_webrtc_session *s);
#endif /* ZMS_SRC_WEBRTC_ICE_INTERNAL_H */
