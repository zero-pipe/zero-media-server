#ifndef ZMS_WEBRTC_WHEP_H
#define ZMS_WEBRTC_WHEP_H

#include "zms/zms_export.h"

#ifdef __cplusplus
extern "C" {
#endif

struct zms_http_session;
struct zms_http_request;

/** WHEP / ZLM webrtc 播放：POST offer SDP，回 201 + answer SDP。DELETE Location 资源以拆除。 */
ZMS_API void zms_webrtc_whep_handle(struct zms_http_session *hs,
                                    const struct zms_http_request *req);

/** WHIP 推流：POST offer SDP，回 201 + answer SDP。DELETE Location 资源以拆除。 */
ZMS_API void zms_webrtc_whip_handle(struct zms_http_session *hs,
                                    const struct zms_http_request *req);

void zms_webrtc_whep_routes_register(void);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_WEBRTC_WHEP_H */
