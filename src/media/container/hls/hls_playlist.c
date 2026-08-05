/**
 * @file hls_playlist.c
 * @brief M3U8 播放列表门面实现（封装 zmk libhls hls_m3u8）。
 *
 * Copyright (c) zero-media-server
 */
#include "zms/media/container/hls/hls_playlist.h"
#include "hls-m3u8.h"

zms_hls_m3u8 *zms_hls_m3u8_create(int live, int version)
{
    return (zms_hls_m3u8 *)hls_m3u8_create(live, version);
}

void zms_hls_m3u8_destroy(zms_hls_m3u8 *m)
{
    hls_m3u8_destroy((hls_m3u8_t *)m);
}

int zms_hls_m3u8_set_x_map(zms_hls_m3u8 *m, const char *name)
{
    return hls_m3u8_set_x_map((hls_m3u8_t *)m, name);
}

int zms_hls_m3u8_add(zms_hls_m3u8 *m, const char *name, int64_t pts_ms, int64_t duration_ms,
                     int discontinuity)
{
    return hls_m3u8_add((hls_m3u8_t *)m, name, pts_ms, duration_ms, discontinuity);
}

int zms_hls_m3u8_playlist(zms_hls_m3u8 *m, int eof, char *out, size_t cap)
{
    return hls_m3u8_playlist((hls_m3u8_t *)m, eof, out, cap);
}
