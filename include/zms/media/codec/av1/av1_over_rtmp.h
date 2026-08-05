#ifndef ZMS_CODEC_AV1_AV1_OVER_RTMP_H
#define ZMS_CODEC_AV1_AV1_OVER_RTMP_H

#include "zms/zms_export.h"
#include "ztk/ztk_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

ZMS_API int zms_av1_over_rtmp_config_extradata(const uint8_t *data, size_t len,
                                               const uint8_t **av1c, size_t *av1c_len);

ZMS_API ztk_err_t zms_av1_flv_sequence_header(const uint8_t *cfg, size_t cfg_len, uint8_t *out,
                                              size_t cap, size_t *out_len);

ZMS_API ztk_err_t zms_av1_over_rtmp_pack_es(const uint8_t *video_cfg, size_t cfg_len,
                                            const uint8_t *obu, size_t len, int key, uint8_t *out,
                                            size_t cap, size_t *out_len);

/** 从 AV1 OBU 访问单元生成 av1c extradata；成功返回写入字节数 */
ZMS_API int zms_av1_extradata_from_obu(const uint8_t *obu, size_t len, uint8_t *out, size_t cap,
                                       int *width, int *height);

/** OBU 链中是否含 Sequence Header（OBU type 1，AV1 RTP coded video sequence 起点） */
ZMS_API int zms_av1_obu_has_sequence_header(const uint8_t *obu, size_t len);

/** 是否含 Sequence Header 且可解析出宽高，可作为 DASH/HLS 切段 sync 点 */
ZMS_API int zms_av1_obu_is_sync_key(const uint8_t *obu, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_CODEC_AV1_AV1_OVER_RTMP_H */
