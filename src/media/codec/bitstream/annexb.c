#include "zms/media/codec/bitstream/annexb.h"
#include "mpeg4-avc.h"
#include <string.h>

typedef struct {
    const uint8_t *found;
    size_t found_len;
    int got;
} zms_annexb_find_ctx;

static void zms_annexb_on_nal(void *param, const uint8_t *nalu, size_t bytes)
{
    zms_annexb_find_ctx *c = (zms_annexb_find_ctx *)param;

    if (!c || c->got || !nalu || bytes == 0) {
        return;
    }
    c->found = nalu;
    c->found_len = bytes;
    c->got = 1;
}

const uint8_t *zms_annexb_find_nal(const uint8_t *p, const uint8_t *end, size_t *nal_len)
{
    zms_annexb_find_ctx ctx;

    if (!p || !end || !nal_len || p >= end) {
        return NULL;
    }
    memset(&ctx, 0, sizeof(ctx));
    mpeg4_h264_annexb_nalu(p, (size_t)(end - p), zms_annexb_on_nal, &ctx);
    if (!ctx.got) {
        *nal_len = 0;
        return NULL;
    }
    *nal_len = ctx.found_len;
    return ctx.found;
}
