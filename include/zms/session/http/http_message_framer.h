#ifndef ZMS_SESSION_HTTP_MESSAGE_FRAMER_H
#define ZMS_SESSION_HTTP_MESSAGE_FRAMER_H

#include "zms/zms_export.h"
#include "ztk/ztk_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zms_http_message_framer zms_http_message_framer;

/**
 * 收到完整 HTTP 头（含末尾 \r\n\r\n 之前部分）。
 * @return 后续 body 长度语义（HTTP 分块请求体解析）：
 *   >0  固定长度 body，收齐后一次调用 on_content
 *   0   无 body 或仍等待更多头数据
 *   <0  不定长 body，剩余数据分次 on_content
 */
typedef intptr_t (*zms_http_message_framer_on_header_cb)(const char *header, size_t header_len,
                                                         void *user);

typedef void (*zms_http_message_framer_on_content_cb)(const char *data, size_t len, void *user);

/**
 * 查找当前缓冲区内一个完整逻辑包末尾（返回尾后第一个字节指针）。
 * NULL 表示数据不足；默认实现查找 \r\n\r\n。
 */
typedef const char *(*zms_http_message_framer_search_tail_cb)(const char *data, size_t len,
                                                              void *user);

typedef struct zms_http_message_framer_opts {
    zms_http_message_framer_on_header_cb on_header;
    zms_http_message_framer_on_content_cb on_content;
    void *user;
    /** 0 表示默认 4MB */
    size_t max_cache_size;
    /** 可选，RTSP 等协议用于交织包 */
    zms_http_message_framer_search_tail_cb on_search_tail;
} zms_http_message_framer_opts;

/** 默认 HTTP/RTSP 文本包尾（\r\n\r\n 之后） */
ZMS_API const char *zms_http_message_search_tail(const char *data, size_t len);

ZMS_API zms_http_message_framer *
zms_http_message_framer_create(const zms_http_message_framer_opts *opts);
ZMS_API void zms_http_message_framer_destroy(zms_http_message_framer *s);
ZMS_API void zms_http_message_framer_reset(zms_http_message_framer *s);
ZMS_API ztk_err_t zms_http_message_framer_input(zms_http_message_framer *s, const void *data,
                                                size_t len);

ZMS_API size_t zms_http_message_framer_remain_size(const zms_http_message_framer *s);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_SESSION_HTTP_MESSAGE_FRAMER_H */
