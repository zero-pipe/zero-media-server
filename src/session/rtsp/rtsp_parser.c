#include "zms/session/rtsp/rtsp_parser.h"
#include "util/strtok_r.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define strcasecmp _stricmp
#endif

static const char *method_names[] = {
    "OPTIONS",  "DESCRIBE",      "SETUP",         "PLAY",     "PAUSE",
    "TEARDOWN", "GET_PARAMETER", "SET_PARAMETER", "ANNOUNCE", "RECORD",
};

static zms_rtsp_method parse_method(const char *s)
{
    for (size_t i = 0; i < sizeof(method_names) / sizeof(method_names[0]); ++i) {
        if (strcasecmp(s, method_names[i]) == 0) {
            return (zms_rtsp_method)i;
        }
    }
    return ZMS_RTSP_UNKNOWN;
}

static void trim_crlf(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\r' || s[n - 1] == '\n')) {
        s[--n] = '\0';
    }
}

static ztk_err_t add_header(zms_rtsp_message *msg, const char *line)
{
    char key[ZMS_RTSP_KEY_MAX];
    char val[ZMS_RTSP_VAL_MAX];
    const char *p = strchr(line, ':');
    if (!p) {
        return ZTK_ERR_INVALID;
    }
    size_t klen = (size_t)(p - line);
    if (klen >= sizeof(key)) {
        return ZTK_ERR_INVALID;
    }
    memcpy(key, line, klen);
    key[klen] = '\0';
    trim_crlf(key);

    const char *vstart = p + 1;
    while (*vstart == ' ') {
        ++vstart;
    }
    strncpy(val, vstart, sizeof(val) - 1);
    val[sizeof(val) - 1] = '\0';
    trim_crlf(val);

    if (msg->header_count >= ZMS_RTSP_HDR_MAX) {
        return ZTK_ERR_BUFFER_TOO_SMALL;
    }

    strncpy(msg->headers[msg->header_count].key, key, sizeof(msg->headers[0].key) - 1);
    strncpy(msg->headers[msg->header_count].value, val, sizeof(msg->headers[0].value) - 1);
    msg->header_count++;
    return ZTK_OK;
}

static const char *find_header_end(const char *data, size_t len, size_t *header_len)
{
    for (size_t i = 0; i + 3 < len; ++i) {
        if (data[i] == '\r' && data[i + 1] == '\n' && data[i + 2] == '\r' && data[i + 3] == '\n') {
            if (header_len) {
                *header_len = i + 4;
            }
            return data + i;
        }
    }
    return NULL;
}

static ztk_err_t parse_header_block(char *block, zms_rtsp_message *msg)
{
    char *save = NULL;
    char *line = zms_strtok_r(block, "\r\n", &save);
    if (!line) {
        return ZTK_ERR_INVALID;
    }

    if (strncmp(line, "RTSP/", 5) == 0) {
        msg->is_response = 1;
        char ver[16];
        if (sscanf(line, "RTSP/%15s %d %127[^\r\n]", ver, &msg->status_code, msg->reason) < 2) {
            return ZTK_ERR_INVALID;
        }
        strncpy(msg->version, ver, sizeof(msg->version) - 1);
    } else {
        msg->is_response = 0;
        char method[64];
        char url[512];
        char ver[16];
        if (sscanf(line, "%63s %511s RTSP/%15s", method, url, ver) < 3) {
            return ZTK_ERR_INVALID;
        }
        msg->method = parse_method(method);
        strncpy(msg->url, url, sizeof(msg->url) - 1);
        strncpy(msg->version, ver, sizeof(msg->version) - 1);
    }

    while ((line = zms_strtok_r(NULL, "\r\n", &save)) != NULL) {
        if (line[0] == '\0') {
            break;
        }
        ztk_err_t err = add_header(msg, line);
        if (err != ZTK_OK) {
            return err;
        }
    }
    return ZTK_OK;
}

ztk_err_t zms_rtsp_message_parse_header(const char *data, size_t len, zms_rtsp_message *msg)
{
    if (!data || !msg || len == 0) {
        return ZTK_ERR_INVALID;
    }

    memset(msg, 0, sizeof(*msg));
    size_t hdr_len = 0;
    if (!find_header_end(data, len, &hdr_len)) {
        return ZTK_ERR_AGAIN;
    }

    char block[ZMS_RTSP_LINE_MAX];
    if (hdr_len >= sizeof(block)) {
        return ZTK_ERR_BUFFER_TOO_SMALL;
    }
    memcpy(block, data, hdr_len);
    block[hdr_len] = '\0';
    return parse_header_block(block, msg);
}

ztk_err_t zms_rtsp_message_parse(const char *data, size_t len, zms_rtsp_message *msg,
                                 size_t *consumed)
{
    if (!data || !msg || len == 0) {
        return ZTK_ERR_INVALID;
    }

    ztk_err_t err = zms_rtsp_message_parse_header(data, len, msg);
    if (err != ZTK_OK) {
        return err;
    }

    size_t hdr_len = 0;
    find_header_end(data, len, &hdr_len);

    int content_len = zms_rtsp_message_content_length(msg);
    size_t total = hdr_len + (content_len > 0 ? (size_t)content_len : 0);
    if (len < total) {
        return ZTK_ERR_AGAIN;
    }

    if (content_len > 0) {
        msg->body = data + hdr_len;
        msg->body_len = (size_t)content_len;
    }

    if (consumed) {
        *consumed = total;
    }
    return ZTK_OK;
}

const char *zms_rtsp_message_get(const zms_rtsp_message *msg, const char *key)
{
    if (!msg || !key) {
        return NULL;
    }
    for (unsigned i = 0; i < msg->header_count; ++i) {
        if (strcasecmp(msg->headers[i].key, key) == 0) {
            return msg->headers[i].value;
        }
    }
    return NULL;
}

int zms_rtsp_message_content_length(const zms_rtsp_message *msg)
{
    const char *v = zms_rtsp_message_get(msg, "Content-Length");
    if (!v || !v[0]) {
        return 0;
    }
    return atoi(v);
}

ztk_err_t zms_rtsp_message_build_request(char *out, size_t cap, zms_rtsp_method method,
                                         const char *url, unsigned cseq, const char *session,
                                         const char *extra_headers, const char *body,
                                         size_t body_len, size_t *out_len)
{
    if (!out || !url || method < 0 || method >= ZMS_RTSP_UNKNOWN) {
        return ZTK_ERR_INVALID;
    }

    const char *m = method_names[method];
    int n = snprintf(out, cap,
                     "%s %s RTSP/1.0\r\n"
                     "CSeq: %u\r\n"
                     "User-Agent: ZTK-RTSP/1.0\r\n",
                     m, url, cseq);
    if (n < 0 || (size_t)n >= cap) {
        return ZTK_ERR_BUFFER_TOO_SMALL;
    }

    if (session && session[0]) {
        n += snprintf(out + n, cap - (size_t)n, "Session: %s\r\n", session);
        if (n < 0 || (size_t)n >= cap) {
            return ZTK_ERR_BUFFER_TOO_SMALL;
        }
    }
    if (extra_headers && extra_headers[0]) {
        n += snprintf(out + n, cap - (size_t)n, "%s", extra_headers);
        if (n < 0 || (size_t)n >= cap) {
            return ZTK_ERR_BUFFER_TOO_SMALL;
        }
    }
    if (body && body_len > 0) {
        n += snprintf(out + n, cap - (size_t)n, "Content-Length: %zu\r\n", body_len);
        if (n < 0 || (size_t)n >= cap) {
            return ZTK_ERR_BUFFER_TOO_SMALL;
        }
    }
    n += snprintf(out + n, cap - (size_t)n, "\r\n");
    if (n < 0 || (size_t)n >= cap) {
        return ZTK_ERR_BUFFER_TOO_SMALL;
    }

    if (body && body_len > 0) {
        if ((size_t)n + body_len >= cap) {
            return ZTK_ERR_BUFFER_TOO_SMALL;
        }
        memcpy(out + n, body, body_len);
        n += (int)body_len;
    }

    if (out_len) {
        *out_len = (size_t)n;
    }
    return ZTK_OK;
}

ztk_err_t zms_rtsp_message_build_response(char *out, size_t cap, int status, const char *reason,
                                          unsigned cseq, const char *session,
                                          const char *extra_headers, const char *body,
                                          size_t body_len, size_t *out_len)
{
    if (!out || !reason) {
        return ZTK_ERR_INVALID;
    }

    int n = snprintf(out, cap,
                     "RTSP/1.0 %d %s\r\n"
                     "CSeq: %u\r\n"
                     "Server: ZMS-RTSP/1.0\r\n",
                     status, reason, cseq);
    if (n < 0 || (size_t)n >= cap) {
        return ZTK_ERR_BUFFER_TOO_SMALL;
    }

    if (session && session[0]) {
        n += snprintf(out + n, cap - (size_t)n, "Session: %s\r\n", session);
        if (n < 0 || (size_t)n >= cap) {
            return ZTK_ERR_BUFFER_TOO_SMALL;
        }
    }
    if (extra_headers && extra_headers[0]) {
        n += snprintf(out + n, cap - (size_t)n, "%s", extra_headers);
        if (n < 0 || (size_t)n >= cap) {
            return ZTK_ERR_BUFFER_TOO_SMALL;
        }
    }
    if (body && body_len > 0) {
        n += snprintf(out + n, cap - (size_t)n, "Content-Length: %zu\r\n", body_len);
        if (n < 0 || (size_t)n >= cap) {
            return ZTK_ERR_BUFFER_TOO_SMALL;
        }
    }
    n += snprintf(out + n, cap - (size_t)n, "\r\n");
    if (n < 0 || (size_t)n >= cap) {
        return ZTK_ERR_BUFFER_TOO_SMALL;
    }

    if (body && body_len > 0) {
        if ((size_t)n + body_len >= cap) {
            return ZTK_ERR_BUFFER_TOO_SMALL;
        }
        memcpy(out + n, body, body_len);
        n += (int)body_len;
    }

    if (out_len) {
        *out_len = (size_t)n;
    }
    return ZTK_OK;
}
