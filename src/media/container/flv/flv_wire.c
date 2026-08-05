#include "zms/media/container/flv/flv_wire.h"
#include "flv-header.h"
#include "flv-proto.h"
#include <string.h>

#define FLV_TAG_HEADER_SIZE 11

ztk_err_t zms_flv_write_header(uint8_t *out, size_t cap, size_t *out_len, int has_audio,
                               int has_video)
{
    int n;

    if (!out || cap < 13) {
        return ZTK_ERR_INVALID;
    }
    n = flv_header_write(has_audio, has_video, out, cap);
    if (n < 0) {
        return ZTK_ERR_INVALID;
    }
    if ((size_t)n + 4 > cap) {
        return ZTK_ERR_BUFFER_TOO_SMALL;
    }
    if (flv_tag_size_write(out + n, cap - (size_t)n, 0) < 0) {
        return ZTK_ERR_INVALID;
    }
    if (out_len) {
        *out_len = (size_t)n + 4;
    }
    return ZTK_OK;
}

static int flv_type_from_rtmp(uint8_t type_id)
{
    if (type_id == 9) {
        return FLV_TYPE_VIDEO;
    }
    if (type_id == 18) {
        return FLV_TYPE_SCRIPT;
    }
    return FLV_TYPE_AUDIO;
}

ztk_err_t zms_flv_write_tag(uint8_t *out, size_t cap, size_t *out_len, uint8_t type_id,
                            uint32_t tag_dts_ms, const void *data, size_t len)
{
    uint8_t scratch[FLV_TAG_HEADER_SIZE + 4];
    struct flv_tag_header_t tag;
    int hdr;
    uint32_t tag_total;

    if (!out || !data || len == 0) {
        return ZTK_ERR_INVALID;
    }

    memset(&tag, 0, sizeof(tag));
    tag.type = (uint8_t)flv_type_from_rtmp(type_id);
    tag.size = (uint32_t)len;
    tag.timestamp = tag_dts_ms;

    hdr = flv_tag_header_write(&tag, scratch, sizeof(scratch));
    if (hdr < 0) {
        return ZTK_ERR_BUFFER_TOO_SMALL;
    }
    tag_total = (uint32_t)hdr + (uint32_t)len;
    if (flv_tag_size_write(scratch + hdr, sizeof(scratch) - (size_t)hdr, tag_total) < 0) {
        return ZTK_ERR_INVALID;
    }
    if ((size_t)tag_total + 4 > cap) {
        return ZTK_ERR_BUFFER_TOO_SMALL;
    }

    memcpy(out, scratch, (size_t)hdr);
    memcpy(out + hdr, data, len);
    memcpy(out + tag_total, scratch + hdr, 4);
    if (out_len) {
        *out_len = (size_t)tag_total + 4;
    }
    return ZTK_OK;
}
