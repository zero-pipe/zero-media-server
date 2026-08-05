#ifndef ZMS_SESSION_RTMP_AMF_H
#define ZMS_SESSION_RTMP_AMF_H

#include "zms/zms_export.h"
#include "ztk/ztk_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum zms_amf_type {
    ZMS_AMF_NUMBER = 0,
    ZMS_AMF_BOOLEAN = 1,
    ZMS_AMF_STRING = 2,
    ZMS_AMF_OBJECT = 3,
    ZMS_AMF_NULL = 5,
    ZMS_AMF_ECMA_ARRAY = 8,
    ZMS_AMF_STRICT_ARRAY = 10,
} zms_amf_type;

typedef struct zms_amf_value zms_amf_value;

ZMS_API ztk_err_t zms_amf_decode(const uint8_t *data, size_t len, zms_amf_value **out,
                                 size_t *consumed);
ZMS_API void zms_amf_value_free(zms_amf_value *v);

ZMS_API zms_amf_type zms_amf_value_type(const zms_amf_value *v);
ZMS_API const char *zms_amf_value_str(const zms_amf_value *v);
ZMS_API double zms_amf_value_num(const zms_amf_value *v);
ZMS_API const zms_amf_value *zms_amf_object_get(const zms_amf_value *obj, const char *key);

ZMS_API size_t zms_amf_encode_string(uint8_t *out, size_t cap, const char *s);
ZMS_API size_t zms_amf_encode_number(uint8_t *out, size_t cap, double n);
/** AMF0 严格数字数组（类型字节 + U32 计数 + N × number） */
ZMS_API size_t zms_amf_encode_strict_array_numbers(uint8_t *out, size_t cap, const double *vals,
                                                   size_t count);
ZMS_API size_t zms_amf_encode_null(uint8_t *out, size_t cap);
ZMS_API size_t zms_amf_encode_object_end(uint8_t *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_SESSION_RTMP_AMF_H */
