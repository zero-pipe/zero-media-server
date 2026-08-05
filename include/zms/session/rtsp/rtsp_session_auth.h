#ifndef ZMS_SESSION_RTSP_AUTH_H
#define ZMS_SESSION_RTSP_AUTH_H
#include "zms/session/rtsp/rtsp_parser.h"
#include "zms/zms_export.h"
#ifdef __cplusplus
extern "C" {
#endif
struct zms_rtsp_session;
/** 配置 RTSP Digest 鉴权；user/pass 均为空则关闭 */
ZMS_API void zms_rtsp_auth_configure(const char *user, const char *pass);
/** 测试用：PLAY 拉流 SETUP 拒绝 TCP interleaved 返回 461，用于验证客户端 AUTO 回退 UDP */
ZMS_API void zms_rtsp_set_test_reject_tcp_setup(int enable);
ZMS_API int zms_rtsp_auth_enabled(void);
/** @return 1 可继续处理请求；0 已回 401 */
ZMS_API int zms_rtsp_auth_check(struct zms_rtsp_session *rs, const zms_rtsp_message *msg);
ZMS_API int zms_rtsp_test_reject_tcp_setup(void);
#ifdef __cplusplus
}
#endif
#endif /* ZMS_SESSION_RTSP_AUTH_H */
