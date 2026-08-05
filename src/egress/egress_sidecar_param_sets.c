#include "zms/egress/egress_sidecar_param_sets.h"
#include "zms/media/codec/h264/h264_over_rtmp.h"
#include "zms/media/codec/h265/h265_es.h"
#include <stdlib.h>
#include <string.h>

void zms_sidecar_param_sets_init(zms_sidecar_param_sets *ps)
{
    if (!ps) {
        return;
    }
    memset(ps, 0, sizeof(*ps));
}

void zms_sidecar_param_sets_clear(zms_sidecar_param_sets *ps)
{
    if (!ps) {
        return;
    }
    free(ps->vps);
    free(ps->sps);
    free(ps->pps);
    ps->vps = ps->sps = ps->pps = NULL;
    ps->vps_len = ps->sps_len = ps->pps_len = 0;
}

static int cache_copy(uint8_t **dst, size_t *dst_len, const uint8_t *src, size_t src_len)
{
    uint8_t *p;

    free(*dst);
    *dst = NULL;
    *dst_len = 0;
    if (!src || src_len == 0) {
        return 0;
    }
    p = (uint8_t *)malloc(src_len);
    if (!p) {
        return 0;
    }
    memcpy(p, src, src_len);
    *dst = p;
    *dst_len = src_len;
    return 1;
}

int zms_sidecar_cache_rtmp_video_cfg(zms_sidecar_param_sets *ps, const uint8_t *cfg, size_t clen)
{
    const uint8_t *sps = NULL;
    const uint8_t *pps = NULL;
    const uint8_t *vps = NULL;
    size_t sps_len = 0;
    size_t pps_len = 0;
    size_t vps_len = 0;

    if (!ps || !cfg || clen < 2) {
        return 0;
    }

    /* HVCC/VVCC 也以 configurationVersion=0x01 开头；先试 HEVC 再 AVCC。 */
    if (zms_h265_video_config_param_sets(cfg, clen, &vps, &vps_len, &sps, &sps_len, &pps,
                                         &pps_len)) {
        zms_sidecar_param_sets_clear(ps);
        if (vps && vps_len > 0 && !cache_copy(&ps->vps, &ps->vps_len, vps, vps_len)) {
            return 0;
        }
        return cache_copy(&ps->sps, &ps->sps_len, sps, sps_len) &&
               cache_copy(&ps->pps, &ps->pps_len, pps, pps_len);
    }

    if (zms_rtmp_avc_extract_sps_pps(cfg, clen, &sps, &sps_len, &pps, &pps_len)) {
        zms_sidecar_param_sets_clear(ps);
        return cache_copy(&ps->sps, &ps->sps_len, sps, sps_len) &&
               cache_copy(&ps->pps, &ps->pps_len, pps, pps_len);
    }

    return 0;
}
