#include "zms/media/codec/h265/h265_es.h"
#include "zms/media/container/flv/flv_tag_probe.h"
#include "flv-header.h"
#include "flv-proto.h"
#include "mpeg4-hevc.h"
#include "mpeg4-avc.h"
#include <string.h>

static int hevc_nal_type(uint8_t b)
{
    return (b >> 1) & 0x3f;
}

static int hevc_is_sync_nal(int t)
{
    return t >= 16 && t <= 21;
}

static int hevc_is_idr_nal(int t)
{
    return t == 19 || t == 20;
}

typedef struct {
    int found;
} hevc_scan_ctx;

static void hevc_scan_sync_key(void *param, const uint8_t *nalu, size_t bytes)
{
    hevc_scan_ctx *c = (hevc_scan_ctx *)param;

    if (!c || c->found || !nalu || bytes == 0) {
        return;
    }
    if (hevc_is_sync_nal(hevc_nal_type(nalu[0]))) {
        c->found = 1;
    }
}

static void hevc_scan_idr(void *param, const uint8_t *nalu, size_t bytes)
{
    hevc_scan_ctx *c = (hevc_scan_ctx *)param;

    if (!c || c->found || !nalu || bytes == 0) {
        return;
    }
    if (hevc_is_idr_nal(hevc_nal_type(nalu[0]))) {
        c->found = 1;
    }
}

int zms_h265_annexb_is_sync_key(const uint8_t *annexb, size_t len)
{
    hevc_scan_ctx ctx;

    if (!annexb || len < 5) {
        return 0;
    }
    memset(&ctx, 0, sizeof(ctx));
    mpeg4_h264_annexb_nalu(annexb, len, hevc_scan_sync_key, &ctx);
    return ctx.found;
}

int zms_h265_annexb_is_idr(const uint8_t *annexb, size_t len)
{
    hevc_scan_ctx ctx;

    if (!annexb || len < 5) {
        return 0;
    }
    memset(&ctx, 0, sizeof(ctx));
    mpeg4_h264_annexb_nalu(annexb, len, hevc_scan_idr, &ctx);
    return ctx.found;
}

typedef struct {
    const uint8_t *vps;
    const uint8_t *sps;
    const uint8_t *pps;
    size_t vps_len;
    size_t sps_len;
    size_t pps_len;
} hevc_param_ctx;

static void hevc_scan_params(void *param, const uint8_t *nalu, size_t bytes)
{
    hevc_param_ctx *c = (hevc_param_ctx *)param;
    int t;

    if (!c || !nalu || bytes == 0) {
        return;
    }
    t = hevc_nal_type(nalu[0]);
    if (t == 32) {
        c->vps = nalu;
        c->vps_len = bytes;
    } else if (t == 33) {
        c->sps = nalu;
        c->sps_len = bytes;
    } else if (t == 34) {
        c->pps = nalu;
        c->pps_len = bytes;
    }
}

int zms_h265_annexb_extract_vps_sps_pps(const uint8_t *annexb, size_t len, const uint8_t **vps,
                                        size_t *vps_len, const uint8_t **sps, size_t *sps_len,
                                        const uint8_t **pps, size_t *pps_len)
{
    hevc_param_ctx ctx;

    if (vps) {
        *vps = NULL;
    }
    if (vps_len) {
        *vps_len = 0;
    }
    if (sps) {
        *sps = NULL;
    }
    if (sps_len) {
        *sps_len = 0;
    }
    if (pps) {
        *pps = NULL;
    }
    if (pps_len) {
        *pps_len = 0;
    }
    if (!annexb || len < 5) {
        return 0;
    }

    memset(&ctx, 0, sizeof(ctx));
    mpeg4_h264_annexb_nalu(annexb, len, hevc_scan_params, &ctx);
    if (vps) {
        *vps = ctx.vps;
    }
    if (vps_len) {
        *vps_len = ctx.vps_len;
    }
    if (sps) {
        *sps = ctx.sps;
    }
    if (sps_len) {
        *sps_len = ctx.sps_len;
    }
    if (pps) {
        *pps = ctx.pps;
    }
    if (pps_len) {
        *pps_len = ctx.pps_len;
    }
    return (ctx.sps && ctx.sps_len > 0 && ctx.pps && ctx.pps_len > 0) ? 1 : 0;
}

typedef struct {
    uint8_t *out;
    size_t cap;
    size_t pos;
    int err;
} hevc_vcl_ctx;

static void hevc_append_vcl(void *param, const uint8_t *nalu, size_t bytes)
{
    hevc_vcl_ctx *c = (hevc_vcl_ctx *)param;
    int t;

    if (!c || c->err || !nalu || bytes == 0) {
        return;
    }
    t = hevc_nal_type(nalu[0]);
    if (t < 0 || t >= 32) {
        return;
    }
    if (c->pos + 4 + bytes > c->cap) {
        c->err = 1;
        return;
    }
    c->out[c->pos++] = 0;
    c->out[c->pos++] = 0;
    c->out[c->pos++] = 0;
    c->out[c->pos++] = 1;
    memcpy(c->out + c->pos, nalu, bytes);
    c->pos += bytes;
}

static size_t h265_annexb_write_nal(uint8_t *out, size_t cap, size_t pos, const uint8_t *nal,
                                    size_t len)
{
    if (!nal || len == 0 || pos + 4 + len > cap) {
        return 0;
    }
    out[pos++] = 0;
    out[pos++] = 0;
    out[pos++] = 0;
    out[pos++] = 1;
    memcpy(out + pos, nal, len);
    return pos + len;
}

ztk_err_t zms_h265_annexb_copy_vcl(const uint8_t *annexb, size_t len, uint8_t *out, size_t cap,
                                   size_t *out_len)
{
    hevc_vcl_ctx ctx;

    if (!out || !out_len) {
        return ZTK_ERR_INVALID;
    }
    *out_len = 0;
    if (!annexb || len < 5) {
        return ZTK_ERR_INVALID;
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.out = out;
    ctx.cap = cap;
    mpeg4_h264_annexb_nalu(annexb, len, hevc_append_vcl, &ctx);
    if (ctx.err || ctx.pos == 0) {
        return ctx.err ? ZTK_ERR_BUFFER_TOO_SMALL : ZTK_ERR_INVALID;
    }
    *out_len = ctx.pos;
    return ZTK_OK;
}

ztk_err_t zms_h265_annexb_build_rtp_au(const uint8_t *vps, size_t vps_len, const uint8_t *sps,
                                       size_t sps_len, const uint8_t *pps, size_t pps_len,
                                       const uint8_t *annexb, size_t len, int prepend_params,
                                       uint8_t *out, size_t cap, size_t *out_len)
{
    size_t pos = 0;
    size_t vcl_len = 0;
    ztk_err_t err;

    if (!out || !out_len || !annexb || len < 5) {
        return ZTK_ERR_INVALID;
    }
    *out_len = 0;

    if (prepend_params && sps && sps_len > 0 && pps && pps_len > 0) {
        if (vps && vps_len > 0) {
            pos = h265_annexb_write_nal(out, cap, pos, vps, vps_len);
        }
        pos = h265_annexb_write_nal(out, cap, pos, sps, sps_len);
        pos = h265_annexb_write_nal(out, cap, pos, pps, pps_len);
        if (pos == 0) {
            return ZTK_ERR_BUFFER_TOO_SMALL;
        }
    }

    err = zms_h265_annexb_copy_vcl(annexb, len, out + pos, cap - pos, &vcl_len);
    if (err != ZTK_OK) {
        return err;
    }
    *out_len = pos + vcl_len;
    return ZTK_OK;
}

static int hevc_pick_param_sets(struct mpeg4_hevc_t *hevc, const uint8_t **vps, size_t *vps_len,
                                const uint8_t **sps, size_t *sps_len, const uint8_t **pps,
                                size_t *pps_len)
{
    int i;

    if (vps) {
        *vps = NULL;
    }
    if (vps_len) {
        *vps_len = 0;
    }
    if (sps) {
        *sps = NULL;
    }
    if (sps_len) {
        *sps_len = 0;
    }
    if (pps) {
        *pps = NULL;
    }
    if (pps_len) {
        *pps_len = 0;
    }
    if (!hevc) {
        return 0;
    }

    for (i = 0; i < hevc->numOfArrays; ++i) {
        const uint8_t *d = hevc->nalu[i].data;
        size_t n = (size_t)hevc->nalu[i].bytes;
        int t;

        if (!d || n == 0) {
            continue;
        }
        t = (d[0] >> 1) & 0x3f;
        if (t == 32 && vps) {
            *vps = d;
            if (vps_len) {
                *vps_len = n;
            }
        } else if (t == 33 && sps) {
            *sps = d;
            if (sps_len) {
                *sps_len = n;
            }
        } else if (t == 34 && pps) {
            *pps = d;
            if (pps_len) {
                *pps_len = n;
            }
        }
    }
    return (sps && *sps && sps_len && *sps_len > 0 && pps && *pps && pps_len && *pps_len > 0) ? 1
                                                                                              : 0;
}

int zms_h265_hvcc_param_sets(const uint8_t *hvcc, size_t hvcc_len, const uint8_t **vps,
                             size_t *vps_len, const uint8_t **sps, size_t *sps_len,
                             const uint8_t **pps, size_t *pps_len)
{
    struct mpeg4_hevc_t hevc;

    if (!hvcc || hvcc_len < 7) {
        return 0;
    }
    memset(&hevc, 0, sizeof(hevc));
    if (mpeg4_hevc_decoder_configuration_record_load(hvcc, hvcc_len, &hevc) <= 0) {
        return 0;
    }
    return hevc_pick_param_sets(&hevc, vps, vps_len, sps, sps_len, pps, pps_len);
}

int zms_h265_video_config_hvcc(const uint8_t *cfg, size_t cfg_len, const uint8_t **hvcc,
                               size_t *hvcc_len)
{
    struct flv_video_tag_header_t vh;
    int hdr;

    if (!hvcc || !hvcc_len || !cfg || cfg_len < 7) {
        return 0;
    }
    if (zms_flv_tag_video_codec(cfg, cfg_len) == ZMS_CODEC_H265) {
        memset(&vh, 0, sizeof(vh));
        hdr = flv_video_tag_header_read(&vh, cfg, cfg_len);
        if (hdr < 1 || vh.avpacket != FLV_SEQUENCE_HEADER) {
            return 0;
        }
        if (cfg_len <= (size_t)hdr) {
            return 0;
        }
        *hvcc = cfg + hdr;
        *hvcc_len = cfg_len - (size_t)hdr;
        return *hvcc_len >= 7;
    }
    *hvcc = cfg;
    *hvcc_len = cfg_len;
    return cfg_len >= 7 && cfg[0] == 1;
}

int zms_h265_video_config_param_sets(const uint8_t *cfg, size_t cfg_len, const uint8_t **vps,
                                     size_t *vps_len, const uint8_t **sps, size_t *sps_len,
                                     const uint8_t **pps, size_t *pps_len)
{
    const uint8_t *hvcc = NULL;
    size_t hvcc_len = 0;

    if (!cfg || !zms_h265_video_config_hvcc(cfg, cfg_len, &hvcc, &hvcc_len)) {
        return 0;
    }
    return zms_h265_hvcc_param_sets(hvcc, hvcc_len, vps, vps_len, sps, sps_len, pps, pps_len);
}

ztk_err_t zms_h265_hvcc_from_annexb(const uint8_t *annexb, size_t len, uint8_t *out, size_t cap,
                                    size_t *out_len)
{
    struct mpeg4_hevc_t hevc;
    int n;

    if (!annexb || !out || !out_len) {
        return ZTK_ERR_INVALID;
    }
    memset(&hevc, 0, sizeof(hevc));
    if (mpeg4_hevc_from_nalu(annexb, len, &hevc) <= 0) {
        return ZTK_ERR_INVALID;
    }
    n = mpeg4_hevc_decoder_configuration_record_save(&hevc, out, cap);
    if (n <= 0) {
        return ZTK_ERR_INVALID;
    }
    *out_len = (size_t)n;
    return ZTK_OK;
}

ztk_err_t zms_h265_hvcc_from_param_sets(const uint8_t *vps, size_t vps_len, const uint8_t *sps,
                                        size_t sps_len, const uint8_t *pps, size_t pps_len,
                                        uint8_t *out, size_t cap, size_t *out_len)
{
    uint8_t annexb[4096];
    size_t annexb_len = 0;
    ztk_err_t err;

    err = zms_h265_param_sets_to_annexb(vps, vps_len, sps, sps_len, pps, pps_len, annexb,
                                        sizeof(annexb), &annexb_len);
    if (err != ZTK_OK) {
        return err;
    }
    return zms_h265_hvcc_from_annexb(annexb, annexb_len, out, cap, out_len);
}

ztk_err_t zms_h265_param_sets_to_annexb(const uint8_t *vps, size_t vps_len, const uint8_t *sps,
                                        size_t sps_len, const uint8_t *pps, size_t pps_len,
                                        uint8_t *out, size_t cap, size_t *out_len)
{
    size_t pos = 0;

    if (!sps || !pps || sps_len == 0 || pps_len == 0 || !out || !out_len) {
        return ZTK_ERR_INVALID;
    }

    *out_len = 0;
    if (vps && vps_len > 0) {
        pos = h265_annexb_write_nal(out, cap, pos, vps, vps_len);
        if (pos == 0) {
            return ZTK_ERR_BUFFER_TOO_SMALL;
        }
    }
    pos = h265_annexb_write_nal(out, cap, pos, sps, sps_len);
    if (pos == 0) {
        return ZTK_ERR_BUFFER_TOO_SMALL;
    }
    pos = h265_annexb_write_nal(out, cap, pos, pps, pps_len);
    if (pos == 0) {
        return ZTK_ERR_BUFFER_TOO_SMALL;
    }
    *out_len = pos;
    return ZTK_OK;
}
