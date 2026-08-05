#include "session/http/http_session_internal.h"
#include "zms/vod/io/vod_source.h"
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <sys/stat.h>
#else
#include <strings.h>
#include <sys/stat.h>
#endif

static int parse_live_hls_ts_name(const char *name, char *stream, char *file, size_t file_cap);
static int parse_live_hls_ts_slash_name(const char *name, char *stream, char *file,
                                        size_t file_cap);

static int parse_query_uint64(const char *query, const char *key, uint64_t *out)
{
    char token[32];
    const char *p;

    snprintf(token, sizeof(token), "%s=", key);
    p = strstr(query, token);
    if (!p) {
        return 0;
    }
    *out = (uint64_t)strtoull(p + strlen(token), NULL, 10);
    return 1;
}

static int parse_query_double_ms(const char *query, const char *key, uint64_t *out)
{
    char token[32];
    const char *p;

    snprintf(token, sizeof(token), "%s=", key);
    p = strstr(query, token);
    if (!p) {
        return 0;
    }
    *out = (uint64_t)(strtod(p + strlen(token), NULL) * 1000.0 + 0.5);
    return 1;
}

static void parse_flv_query(const char *query, uint64_t *seek_ms)
{
    if (!query || !seek_ms) {
        return;
    }
    if (*query == '?') {
        query++;
    }
    if (parse_query_uint64(query, "start", seek_ms)) {
        return;
    }
    if (parse_query_double_ms(query, "seek", seek_ms)) {
        return;
    }
    if (parse_query_double_ms(query, "t", seek_ms)) {
        return;
    }
    if (parse_query_double_ms(query, "time", seek_ms)) {
        return;
    }
    if (parse_query_double_ms(query, "fs", seek_ms)) {
        return;
    }
    if (parse_query_double_ms(query, "ec_seek", seek_ms)) {
        return;
    }
    if (parse_query_uint64(query, "offset", seek_ms)) {
        return;
    }
    if (parse_query_uint64(query, "pos", seek_ms)) {
        return;
    }
    parse_query_double_ms(query, "pos", seek_ms);
}

static int path_ends_with_ci(const char *path, const char *suffix)
{
    size_t plen;
    size_t slen;

    if (!path || !suffix) {
        return 0;
    }
    plen = strlen(path);
    slen = strlen(suffix);
    if (plen < slen) {
        return 0;
    }
#ifdef _WIN32
    return _stricmp(path + plen - slen, suffix) == 0;
#else
    return strcasecmp(path + plen - slen, suffix) == 0;
#endif
}

void zms_http_route_parse_flv_path(const char *path, char *app, char *stream, uint64_t *seek_ms)
{
    char path_only[512];
    const char *q;
    size_t plen;

    if (app) {
        app[0] = '\0';
    }
    if (stream) {
        stream[0] = '\0';
    }
    if (seek_ms) {
        *seek_ms = 0;
    }
    if (!path || path[0] != '/' || !app || !stream) {
        return;
    }
    q = strchr(path, '?');
    plen = q ? (size_t)(q - path) : strlen(path);
    if (plen >= sizeof(path_only)) {
        plen = sizeof(path_only) - 1;
    }
    memcpy(path_only, path, plen);
    path_only[plen] = '\0';
    if (q) {
        parse_flv_query(q, seek_ms);
    }

    const char *p = path_only + 1;
    const char *slash = strchr(p, '/');
    if (!slash) {
        strncpy(app, p, ZMS_APP_MAX - 1);
        return;
    }
    size_t alen = (size_t)(slash - p);
    if (alen >= ZMS_APP_MAX) {
        alen = ZMS_APP_MAX - 1;
    }
    memcpy(app, p, alen);
    app[alen] = '\0';
    const char *name = slash + 1;

    /* 点播 HTTP-FLV 封装：{app}/path/to/{file}.{mp4|mkv|mov|m4v}.flv */
    if (zms_vod_is_record_app(app) && zms_vod_stream_is_vod_flv_wrap_suffix(name)) {
        size_t slen = strlen(name) - 4;

        if (slen >= ZMS_STREAM_MAX) {
            slen = ZMS_STREAM_MAX - 1;
        }
        memcpy(stream, name, slen);
        stream[slen] = '\0';
        return;
    }

    /* 点播磁盘原生 FLV：{vod_app}/path/to/{file}.flv */
    if (zms_vod_is_record_app(app) && zms_vod_stream_is_native_flv_file(name)) {
        size_t nlen = strlen(name);

        if (nlen >= ZMS_STREAM_MAX) {
            nlen = ZMS_STREAM_MAX - 1;
        }
        memcpy(stream, name, nlen);
        stream[nlen] = '\0';
        return;
    }

    if (zms_vod_is_record_app(app)) {
        return;
    }

    /* 直播：{app}/{stream}.flv（stream 可含 /）；ZLM 兼容 {stream}.live.flv */
    if (path_ends_with_ci(name, ".live.flv")) {
        size_t nlen = strlen(name);
        size_t slen = nlen - 9;
        if (slen >= ZMS_STREAM_MAX) {
            slen = ZMS_STREAM_MAX - 1;
        }
        memcpy(stream, name, slen);
        stream[slen] = '\0';
        return;
    }
    if (path_ends_with_ci(name, ".flv")) {
        size_t nlen = strlen(name);
        size_t slen = nlen - 4;
        if (slen >= ZMS_STREAM_MAX) {
            slen = ZMS_STREAM_MAX - 1;
        }
        memcpy(stream, name, slen);
        stream[slen] = '\0';
        return;
    }
}

int zms_http_route_parse_live_ts_path(const char *path, char *app, char *stream)
{
    char path_only[512];
    const char *q;
    size_t plen;
    const char *p;
    const char *slash;
    const char *name;

    if (app) {
        app[0] = '\0';
    }
    if (stream) {
        stream[0] = '\0';
    }
    if (!path || path[0] != '/' || !app || !stream) {
        return 0;
    }

    q = strchr(path, '?');
    plen = q ? (size_t)(q - path) : strlen(path);
    if (plen >= sizeof(path_only)) {
        plen = sizeof(path_only) - 1;
    }
    memcpy(path_only, path, plen);
    path_only[plen] = '\0';

    p = path_only + 1;
    slash = strchr(p, '/');
    if (!slash) {
        return 0;
    }
    {
        size_t alen = (size_t)(slash - p);
        if (alen >= ZMS_APP_MAX) {
            alen = ZMS_APP_MAX - 1;
        }
        memcpy(app, p, alen);
        app[alen] = '\0';
    }
    if (zms_vod_is_record_app(app)) {
        return 0;
    }

    name = slash + 1;
    if (!name[0] || !path_ends_with_ci(name, ".ts")) {
        return 0;
    }
    {
        char hls_stream[ZMS_STREAM_MAX];
        char hls_file[128];

        if (parse_live_hls_ts_name(name, hls_stream, hls_file, sizeof(hls_file))) {
            return 0;
        }
        if (parse_live_hls_ts_slash_name(name, hls_stream, hls_file, sizeof(hls_file))) {
            return 0;
        }
    }
    {
        size_t slen = strlen(name) - 3;
        if (slen == 0 || slen >= ZMS_STREAM_MAX) {
            return 0;
        }
        memcpy(stream, name, slen);
        stream[slen] = '\0';
    }
    return 1;
}

int zms_http_route_parse_vod_hls_path(const char *path, char *app, char *stream, char *rel,
                                      size_t rel_cap)
{
    char path_only[512];
    const char *q;
    size_t plen;
    const char *p;
    const char *slash;
    const char *rel_part;
    size_t alen;
    size_t rlen;

    if (app) {
        app[0] = '\0';
    }
    if (stream) {
        stream[0] = '\0';
    }
    if (rel_cap && rel) {
        rel[0] = '\0';
    }
    if (!path || path[0] != '/' || !app || !stream || !rel || rel_cap == 0) {
        return 0;
    }

    q = strchr(path, '?');
    plen = q ? (size_t)(q - path) : strlen(path);
    if (plen >= sizeof(path_only)) {
        plen = sizeof(path_only) - 1;
    }
    memcpy(path_only, path, plen);
    path_only[plen] = '\0';

    p = path_only + 1;
    slash = strchr(p, '/');
    if (!slash) {
        return 0;
    }
    alen = (size_t)(slash - p);
    if (alen >= ZMS_APP_MAX) {
        alen = ZMS_APP_MAX - 1;
    }
    memcpy(app, p, alen);
    app[alen] = '\0';
    if (!zms_vod_is_record_app(app)) {
        return 0;
    }

    rel_part = slash + 1;
    if (!rel_part[0] || !zms_vod_rel_path_safe(rel_part)) {
        return 0;
    }
    rlen = strlen(rel_part);
    if (rlen >= rel_cap) {
        rlen = rel_cap - 1;
    }
    memcpy(rel, rel_part, rlen);
    rel[rlen] = '\0';

    if (path_ends_with_ci(rel, ".m3u8")) {
        if (!zms_vod_m3u8_rel_to_mp4_stream(rel, stream, ZMS_STREAM_MAX)) {
            return 0;
        }
        return 1;
    }
    if (path_ends_with_ci(rel, ".ts")) {
        return 1;
    }
    return 0;
}

/** m3u8 分片行为纯数字索引文件名 */
static int hls_seg_file_is_index_media(const char *fname, const char *ext, size_t ext_len)
{
    size_t nlen;
    size_t i;

    if (!fname || !ext || ext_len == 0 || !path_ends_with_ci(fname, ext)) {
        return 0;
    }
    nlen = strlen(fname);
    if (nlen < ext_len + 1) {
        return 0;
    }
    for (i = 0; i + ext_len < nlen; ++i) {
        if (fname[i] < '0' || fname[i] > '9') {
            return 0;
        }
    }
    return fname[i] == '.' &&
#if defined(_WIN32)
           _strnicmp(fname + i + 1, ext + 1, ext_len - 1) == 0;
#else
           strncasecmp(fname + i + 1, ext + 1, ext_len - 1) == 0;
#endif
}

static int hls_seg_file_is_index_ts(const char *fname)
{
    return hls_seg_file_is_index_media(fname, ".ts", 3);
}

static int hls_seg_file_is_index_m4s(const char *fname)
{
    return hls_seg_file_is_index_media(fname, ".m4s", 4);
}

/** 旧式目录 TS {app}/{stream}/{N}.ts；以 m3u8 相对路径解析成的 /{app}/{prefix}/{N}.ts */
static int parse_live_hls_ts_slash_name(const char *name, char *stream, char *file, size_t file_cap)
{
    const char *slash;
    const char *fname;
    size_t slen;

    if (!name || !stream || !file || file_cap == 0) {
        return 0;
    }
    stream[0] = '\0';
    file[0] = '\0';
    slash = strrchr(name, '/');
    if (!slash || slash == name) {
        return 0;
    }
    fname = slash + 1;
    if (!hls_seg_file_is_index_ts(fname)) {
        return 0;
    }
    slen = (size_t)(slash - name);
    if (slen == 0 || slen >= ZMS_STREAM_MAX) {
        return 0;
    }
    memcpy(stream, name, slen);
    stream[slen] = '\0';
    strncpy(file, fname, file_cap - 1);
    file[file_cap - 1] = '\0';
    return 1;
}

/** 直播 fMP4 init {app}/{stream}.init.mp4 */
static int parse_live_hls_init_name(const char *name, char *stream, char *file, size_t file_cap)
{
    const char *suffix = ".init.mp4";
    size_t nlen;
    size_t slen;

    if (!name || !stream || !file || file_cap == 0) {
        return 0;
    }
    stream[0] = '\0';
    file[0] = '\0';
    if (!path_ends_with_ci(name, suffix)) {
        return 0;
    }
    nlen = strlen(name);
    slen = nlen - strlen(suffix);
    if (slen == 0 || slen >= ZMS_STREAM_MAX) {
        return 0;
    }
    memcpy(stream, name, slen);
    stream[slen] = '\0';
    strncpy(file, "init.mp4", file_cap - 1);
    file[file_cap - 1] = '\0';
    return 1;
}

/** 直播 fMP4 {app}/{stream}.{N}.m4s */
static int parse_live_hls_m4s_name(const char *name, char *stream, char *file, size_t file_cap)
{
    size_t nlen;
    const char *dot;
    const char *p;

    if (!name || !stream || !file || file_cap == 0) {
        return 0;
    }
    stream[0] = '\0';
    file[0] = '\0';
    if (!path_ends_with_ci(name, ".m4s")) {
        return 0;
    }
    nlen = strlen(name);
    if (nlen < 6) {
        return 0;
    }
    dot = name + nlen - 4;
    if (dot[0] != '.' || dot[1] != 'm' || dot[2] != '4' || dot[3] != 's') {
        return 0;
    }
    if (dot == name || dot[-1] < '0' || dot[-1] > '9') {
        return 0;
    }
    p = dot - 1;
    while (p > name && p[-1] >= '0' && p[-1] <= '9') {
        --p;
    }
    if (p <= name || p[-1] != '.') {
        return 0;
    }
    {
        size_t slen = (size_t)(p - 1 - name);
        if (slen == 0 || slen >= ZMS_STREAM_MAX) {
            return 0;
        }
        memcpy(stream, name, slen);
        stream[slen] = '\0';
    }
    strncpy(file, p, file_cap - 1);
    file[file_cap - 1] = '\0';
    return 1;
}

/** 直播 TS {app}/{stream}.{N}.ts；stream + 内部文件 {N}.ts */
static int parse_live_hls_ts_name(const char *name, char *stream, char *file, size_t file_cap)
{
    size_t nlen;
    const char *ts_dot;
    const char *p;

    if (!name || !stream || !file || file_cap == 0) {
        return 0;
    }
    stream[0] = '\0';
    file[0] = '\0';
    if (!path_ends_with_ci(name, ".ts")) {
        return 0;
    }
    nlen = strlen(name);
    if (nlen < 5) {
        return 0;
    }
    ts_dot = name + nlen - 3;
    if (ts_dot[0] != '.' || ts_dot[1] != 't' || ts_dot[2] != 's') {
        return 0;
    }
    if (ts_dot == name || ts_dot[-1] < '0' || ts_dot[-1] > '9') {
        return 0;
    }
    p = ts_dot - 1;
    while (p > name && p[-1] >= '0' && p[-1] <= '9') {
        --p;
    }
    if (p <= name || p[-1] != '.') {
        return 0;
    }
    {
        size_t slen = (size_t)(p - 1 - name);
        if (slen == 0 || slen >= ZMS_STREAM_MAX) {
            return 0;
        }
        memcpy(stream, name, slen);
        stream[slen] = '\0';
    }
    strncpy(file, p, file_cap - 1);
    file[file_cap - 1] = '\0';
    return 1;
}

static int hls_line_is_plain_seg(const char *line, size_t len)
{
    char buf[64];
    size_t seg_len = len;

    if (!line || len < 4) {
        return 0;
    }
    while (seg_len > 0 && line[seg_len - 1] == '\r') {
        --seg_len;
    }
    if (seg_len >= sizeof(buf)) {
        return 0;
    }
    memcpy(buf, line, seg_len);
    buf[seg_len] = '\0';
#if defined(_WIN32)
    if (_stricmp(buf, "init.mp4") == 0)
#else
    if (strcasecmp(buf, "init.mp4") == 0)
#endif
        return 1;
    return hls_seg_file_is_index_ts(buf) || hls_seg_file_is_index_m4s(buf);
}

static int hls_line_rewrite_map_uri(char *line, size_t cap, const char *prefix, size_t prefix_len)
{
    const char *uri = strstr(line, "URI=\"");
    char *end;
    char new_uri[ZMS_APP_MAX + 1 + ZMS_STREAM_MAX + 32];
    size_t uri_len;
    int n;

    if (!uri || !prefix || prefix_len == 0 || cap < 32) {
        return 0;
    }
    uri += 5;
    end = strchr(uri, '\"');
    if (!end) {
        return 0;
    }
    uri_len = (size_t)(end - uri);
    if (uri_len >= sizeof(new_uri) - prefix_len - 1) {
        return 0;
    }
    memcpy(new_uri, prefix, prefix_len);
    memcpy(new_uri + prefix_len, uri, uri_len);
    new_uri[prefix_len + uri_len] = '\0';
    n = snprintf(line, cap, "#EXT-X-MAP:URI=\"%s\",\r\n", new_uri);
    return n > 0 && (size_t)n < cap;
}

size_t zms_http_route_rewrite_live_hls_m3u8(char *buf, size_t cap, const char *app,
                                            const char *stream)
{
    char prefix[ZMS_APP_MAX + 1 + ZMS_STREAM_MAX + 2];
    char tmp[4096];
    const char *read;
    char *write;
    char *end;
    size_t prefix_len;
    size_t out_len = 0;
    int n;

    if (!buf || cap == 0 || !app || !app[0] || !stream || !stream[0]) {
        return buf ? strlen(buf) : 0;
    }

    n = snprintf(prefix, sizeof(prefix), "/%s/%s.", app, stream);
    if (n <= 0 || (size_t)n >= sizeof(prefix)) {
        return strlen(buf);
    }
    prefix_len = (size_t)n;

    read = buf;
    write = tmp;
    end = tmp + (cap < sizeof(tmp) ? cap : sizeof(tmp));
    while (*read && write < end) {
        char *nl = strchr(read, '\n');
        size_t line_len = nl ? (size_t)(nl - read) : strlen(read);
        size_t seg_len = line_len;

        while (seg_len > 0 && read[seg_len - 1] == '\r') {
            --seg_len;
        }

        if (hls_line_is_plain_seg(read, seg_len)) {
            if ((size_t)(write - tmp) + prefix_len + seg_len + (nl ? 1 : 0) + 1 >
                (size_t)(end - tmp)) {
                break;
            }
            memcpy(write, prefix, prefix_len);
            write += prefix_len;
            memcpy(write, read, seg_len);
            write += seg_len;
        } else if (seg_len > 12 && strncmp(read, "#EXT-X-MAP:", 11) == 0) {
            char map_line[256];
            if (seg_len >= sizeof(map_line)) {
                seg_len = sizeof(map_line) - 1;
            }
            memcpy(map_line, read, seg_len);
            map_line[seg_len] = '\0';
            if (hls_line_rewrite_map_uri(map_line, sizeof(map_line), prefix, prefix_len)) {
                size_t map_len = strlen(map_line);
                if ((size_t)(write - tmp) + map_len + (nl ? 1 : 0) + 1 > (size_t)(end - tmp)) {
                    break;
                }
                memcpy(write, map_line, map_len);
                write += map_len;
            } else {
                if ((size_t)(write - tmp) + line_len + (nl ? 1 : 0) + 1 > (size_t)(end - tmp)) {
                    break;
                }
                memcpy(write, read, line_len);
                write += line_len;
            }
        } else {
            if ((size_t)(write - tmp) + line_len + (nl ? 1 : 0) + 1 > (size_t)(end - tmp)) {
                break;
            }
            memcpy(write, read, line_len);
            write += line_len;
        }
        if (nl) {
            *write++ = '\n';
            read = nl + 1;
        } else {
            read += line_len;
        }
    }
    *write = '\0';
    out_len = (size_t)(write - tmp);
    if (out_len >= cap) {
        out_len = cap - 1;
    }
    memcpy(buf, tmp, out_len);
    buf[out_len] = '\0';
    return out_len;
}

static int vod_hls_line_is_seg_uri(const char *line, size_t len)
{
    size_t i;

    if (!line || len < 4) {
        return 0;
    }
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == ' ' || line[len - 1] == '\t')) {
        --len;
    }
    if (len < 4 || line[0] == '#' || line[0] == '/') {
        return 0;
    }
    for (i = 0; i < len; ++i) {
        char c = line[i];
        if (c == '/') {
            continue;
        }
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '.' || c == '_' || c == '-') {
            continue;
        }
        return 0;
    }
    return (line[len - 3] == '.' && (line[len - 2] == 't' || line[len - 2] == 'T') &&
            (line[len - 1] == 's' || line[len - 1] == 'S'));
}

size_t zms_http_route_rewrite_vod_hls_m3u8(char *buf, size_t cap, const char *app,
                                           const char *m3u8_rel)
{
    char prefix[ZMS_CFG_PATH_MAX];
    char tmp[8192];
    const char *read;
    char *write;
    char *end;
    size_t prefix_len;
    size_t out_len = 0;
    const char *dir = "";
    char dirbuf[ZMS_STREAM_MAX];
    const char *slash;
    int n;

    if (!buf || cap == 0 || !app || !app[0]) {
        return buf ? strlen(buf) : 0;
    }

    if (m3u8_rel && m3u8_rel[0]) {
        slash = strrchr(m3u8_rel, '/');
        if (slash && slash > m3u8_rel) {
            size_t dlen = (size_t)(slash - m3u8_rel);
            if (dlen >= sizeof(dirbuf)) {
                dlen = sizeof(dirbuf) - 1;
            }
            memcpy(dirbuf, m3u8_rel, dlen);
            dirbuf[dlen] = '\0';
            dir = dirbuf;
        }
    }

    if (dir[0]) {
        n = snprintf(prefix, sizeof(prefix), "/%s/%s/", app, dir);
    } else {
        n = snprintf(prefix, sizeof(prefix), "/%s/", app);
    }
    if (n <= 0 || (size_t)n >= sizeof(prefix)) {
        return strlen(buf);
    }
    prefix_len = (size_t)n;

    read = buf;
    write = tmp;
    end = tmp + (cap < sizeof(tmp) ? cap : sizeof(tmp));
    while (*read && write < end) {
        char *nl = strchr(read, '\n');
        size_t line_len = nl ? (size_t)(nl - read) : strlen(read);
        size_t seg_len = line_len;

        while (seg_len > 0 && read[seg_len - 1] == '\r') {
            --seg_len;
        }

        if (vod_hls_line_is_seg_uri(read, seg_len)) {
            if ((size_t)(write - tmp) + prefix_len + seg_len + (nl ? 1 : 0) + 1 >
                (size_t)(end - tmp)) {
                break;
            }
            memcpy(write, prefix, prefix_len);
            write += prefix_len;
            memcpy(write, read, seg_len);
            write += seg_len;
        } else {
            if ((size_t)(write - tmp) + line_len + (nl ? 1 : 0) + 1 > (size_t)(end - tmp)) {
                break;
            }
            memcpy(write, read, line_len);
            write += line_len;
        }
        if (nl) {
            *write++ = '\n';
            read = nl + 1;
        } else {
            read += line_len;
        }
    }
    *write = '\0';
    out_len = (size_t)(write - tmp);
    if (out_len >= cap) {
        out_len = cap - 1;
    }
    memcpy(buf, tmp, out_len);
    buf[out_len] = '\0';
    return out_len;
}

int zms_http_route_parse_hls_path(const char *path, char *app, char *stream, char *file,
                                  size_t file_cap)
{
    char path_only[512];
    const char *q;
    size_t plen;
    const char *p;
    const char *slash;
    const char *name;

    if (app) {
        app[0] = '\0';
    }
    if (stream) {
        stream[0] = '\0';
    }
    if (file_cap && file) {
        file[0] = '\0';
    }
    if (!path || path[0] != '/' || !app || !stream) {
        return 0;
    }

    q = strchr(path, '?');
    plen = q ? (size_t)(q - path) : strlen(path);
    if (plen >= sizeof(path_only)) {
        plen = sizeof(path_only) - 1;
    }
    memcpy(path_only, path, plen);
    path_only[plen] = '\0';

    /* 点播 HLS 路由：{app}/*.m3u8|*.ts */
    {
        const char *pp = path_only + 1;
        const char *ps = strchr(pp, '/');
        if (ps) {
            char app_probe[ZMS_APP_MAX];
            size_t alen = (size_t)(ps - pp);
            if (alen >= ZMS_APP_MAX) {
                alen = ZMS_APP_MAX - 1;
            }
            memcpy(app_probe, pp, alen);
            app_probe[alen] = '\0';
            if (zms_vod_is_record_app(app_probe)) {
                const char *tail = ps + 1;
                if (path_ends_with_ci(tail, ".m3u8") || path_ends_with_ci(tail, ".ts")) {
                    return 0;
                }
            }
        }
    }

    p = path_only + 1;
    slash = strchr(p, '/');
    if (!slash) {
        return 0;
    }
    {
        size_t alen = (size_t)(slash - p);
        if (alen >= ZMS_APP_MAX) {
            alen = ZMS_APP_MAX - 1;
        }
        memcpy(app, p, alen);
        app[alen] = '\0';
    }
    if (zms_vod_is_record_app(app)) {
        return 0;
    }

    name = slash + 1;
    if (!name[0]) {
        return 0;
    }

    /* ZLM 兼容：{app}/{stream}/hls.m3u8 */
    if (path_ends_with_ci(name, "/hls.m3u8")) {
        size_t nlen = strlen(name);
        size_t slen = nlen - 9;
        if (slen == 0 || slen >= ZMS_STREAM_MAX) {
            return 0;
        }
        memcpy(stream, name, slen);
        stream[slen] = '\0';
        if (file_cap && file) {
            strncpy(file, "hls.m3u8", file_cap - 1);
        }
        return 1;
    }

    /* 直播：{app}/{stream}.m3u8（stream 可含 /）*/
    if (path_ends_with_ci(name, ".m3u8")) {
        size_t slen = strlen(name) - 5;
        if (slen == 0 || slen >= ZMS_STREAM_MAX) {
            return 0;
        }
        memcpy(stream, name, slen);
        stream[slen] = '\0';
        if (file_cap && file) {
            strncpy(file, name, file_cap - 1);
        }
        return 1;
    }

    /* 直播：{app}/{stream}.{N}.ts / {N}.m4s / init.mp4 */
    if (parse_live_hls_ts_name(name, stream, file, file_cap)) {
        return 1;
    }
    if (parse_live_hls_m4s_name(name, stream, file, file_cap)) {
        return 1;
    }
    if (parse_live_hls_init_name(name, stream, file, file_cap)) {
        return 1;
    }
    if (parse_live_hls_ts_slash_name(name, stream, file, file_cap)) {
        return 1;
    }

    return 0;
}

int zms_http_route_parse_mp4_path(const char *path, char *app, char *stream)
{
    char path_only[512];
    const char *q;
    size_t plen;
    const char *p;
    const char *slash;
    const char *name;
    size_t slen;

    if (app) {
        app[0] = '\0';
    }
    if (stream) {
        stream[0] = '\0';
    }
    if (!path || path[0] != '/' || !app || !stream) {
        return 0;
    }

    q = strchr(path, '?');
    plen = q ? (size_t)(q - path) : strlen(path);
    if (plen >= sizeof(path_only)) {
        plen = sizeof(path_only) - 1;
    }
    memcpy(path_only, path, plen);
    path_only[plen] = '\0';
    if (!path_ends_with_ci(path_only, ".mp4")) {
        return 0;
    }

    p = path_only + 1;
    slash = strchr(p, '/');
    if (!slash) {
        return 0;
    }

    {
        size_t alen = (size_t)(slash - p);
        if (alen >= ZMS_APP_MAX) {
            alen = ZMS_APP_MAX - 1;
        }
        memcpy(app, p, alen);
        app[alen] = '\0';
    }

    name = slash + 1;
    if (!name[0] || !path_ends_with_ci(name, ".mp4")) {
        return 0;
    }
    slen = strlen(name);
    if (slen >= ZMS_STREAM_MAX) {
        slen = ZMS_STREAM_MAX - 1;
    }
    memcpy(stream, name, slen);
    stream[slen] = '\0';
    if (!zms_vod_rel_path_safe(stream)) {
        return 0;
    }
    return 1;
}

static int dash_flat_stream_from_fname(const char *fname, char *stream, size_t stream_cap)
{
    const char *dot;
    const char *p;
    size_t slen;

    if (!fname || !fname[0] || !stream || stream_cap == 0) {
        return 0;
    }

    if (path_ends_with_ci(fname, ".mpd")) {
        slen = strlen(fname) - 4;
        if (slen == 0 || slen >= stream_cap) {
            return 0;
        }
        memcpy(stream, fname, slen);
        stream[slen] = '\0';
        return 1;
    }

    dot = strrchr(fname, '.');
    if (!dot || dot == fname) {
        return 0;
    }

    if ((size_t)(dot - fname) >= 5 && strncmp(dot - 5, "-init", 5) == 0) {
        slen = (size_t)(dot - 5 - fname);
    } else {
        p = dot - 1;
        while (p > fname && *p >= '0' && *p <= '9') {
            --p;
        }
        if (p == fname || *p != '-') {
            return 0;
        }
        slen = (size_t)(p - fname);
    }
    if (slen == 0 || slen >= stream_cap) {
        return 0;
    }
    memcpy(stream, fname, slen);
    stream[slen] = '\0';
    return 1;
}

int zms_http_route_parse_dash_path(const char *path, char *app, char *stream, char *file,
                                   size_t file_cap)
{
    char path_only[512];
    const char *q;
    size_t plen;
    const char *p;
    const char *slash;
    const char *name;

    if (app) {
        app[0] = '\0';
    }
    if (stream) {
        stream[0] = '\0';
    }
    if (file_cap && file) {
        file[0] = '\0';
    }
    if (!path || path[0] != '/' || !app || !stream) {
        return 0;
    }

    q = strchr(path, '?');
    plen = q ? (size_t)(q - path) : strlen(path);
    if (plen >= sizeof(path_only)) {
        plen = sizeof(path_only) - 1;
    }
    memcpy(path_only, path, plen);
    path_only[plen] = '\0';

    p = path_only + 1;
    slash = strchr(p, '/');
    if (!slash) {
        return 0;
    }
    {
        size_t alen = (size_t)(slash - p);
        if (alen >= ZMS_APP_MAX) {
            alen = ZMS_APP_MAX - 1;
        }
        memcpy(app, p, alen);
        app[alen] = '\0';
    }
    if (zms_vod_is_record_app(app)) {
        return 0;
    }

    name = slash + 1;
    if (!name[0] || strchr(name, '/')) {
        return 0;
    }

    if (!path_ends_with_ci(name, ".mpd") && !path_ends_with_ci(name, ".m4s") &&
        !path_ends_with_ci(name, ".m4v") && !path_ends_with_ci(name, ".m4a")) {
        return 0;
    }
    if (!dash_flat_stream_from_fname(name, stream, ZMS_STREAM_MAX)) {
        return 0;
    }
    if (file_cap && file) {
        strncpy(file, name, file_cap - 1);
    }
    return 1;
}
