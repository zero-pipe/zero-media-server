#include "zms/media/codec/h264/h264_sps.h"
#include <string.h>

typedef struct {
    const uint8_t *buf;
    size_t size;
    size_t bit_pos;
} br;

static int br_eof(const br *b)
{
    return b->bit_pos >= b->size * 8;
}

static uint32_t br_read_bit(br *b)
{
    if (br_eof(b)) {
        return 0;
    }
    size_t byte = b->bit_pos / 8;
    size_t bit = 7 - (b->bit_pos % 8);
    b->bit_pos++;
    return (b->buf[byte] >> bit) & 1;
}

static uint32_t br_read_bits(br *b, int n)
{
    uint32_t v = 0;
    while (n-- > 0) {
        v = (v << 1) | br_read_bit(b);
    }
    return v;
}

static uint32_t br_read_ue(br *b)
{
    int zeros = 0;
    while (!br_eof(b) && br_read_bit(b) == 0) {
        zeros++;
    }
    if (zeros == 0) {
        return 0;
    }
    return (1u << zeros) - 1 + br_read_bits(b, zeros);
}

static int32_t br_read_se(br *b)
{
    uint32_t ue = br_read_ue(b);
    int32_t v = (int32_t)((ue + 1) / 2);
    return (ue & 1) ? v : -v;
}

static size_t rbsp_unescape(const uint8_t *in, size_t in_len, uint8_t *out, size_t cap)
{
    size_t j = 0;
    for (size_t i = 0; i < in_len && j < cap; ++i) {
        if (i + 2 < in_len && in[i] == 0 && in[i + 1] == 0 && in[i + 2] == 3) {
            out[j++] = 0;
            out[j++] = 0;
            i += 2;
        } else {
            out[j++] = in[i];
        }
    }
    return j;
}

static void skip_scaling_list(br *b, int size)
{
    int last = 8, next = 8;
    for (int i = 0; i < size; ++i) {
        if (next != 0) {
            int delta = br_read_se(b);
            next = (last + delta + 256) % 256;
        }
        if (next) {
            last = next;
        }
    }
}

int zms_h264_sps_parse(const uint8_t *sps, size_t len, zms_h264_sps_info *info)
{
    int chroma = 1; /* 缺省 4:2:0 */

    if (!sps || len < 4 || !info || (sps[0] & 0x1f) != 7) {
        return 0;
    }

    uint8_t rbsp[512];
    size_t rbsp_len = rbsp_unescape(sps + 1, len - 1, rbsp, sizeof(rbsp));
    if (rbsp_len < 3) {
        return 0;
    }

    br b = {rbsp, rbsp_len, 0};
    int profile = (int)br_read_bits(&b, 8);
    br_read_bits(&b, 8); /* constraint_set_flags + reserved_zero_2bits */
    br_read_bits(&b, 8); /* level_idc */
    br_read_ue(&b);      /* seq_parameter_set_id */

    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 ||
        profile == 83 || profile == 86 || profile == 118 || profile == 128 || profile == 138 ||
        profile == 139 || profile == 134 || profile == 135) {
        chroma = (int)br_read_ue(&b);
        if (chroma == 3) {
            br_read_bit(&b);
        }
        br_read_ue(&b);
        br_read_ue(&b);
        br_read_bit(&b);
        if (br_read_bit(&b)) {
            int n = chroma != 3 ? 8 : 12;
            for (int i = 0; i < n; ++i) {
                if (br_read_bit(&b)) {
                    skip_scaling_list(&b, i < 6 ? 16 : 64);
                }
            }
        }
    }

    br_read_ue(&b);
    int poc = (int)br_read_ue(&b);
    if (poc == 0) {
        br_read_ue(&b);
    } else if (poc == 1) {
        br_read_bit(&b);
        br_read_se(&b);
        br_read_se(&b);
        int cnt = (int)br_read_ue(&b);
        for (int i = 0; i < cnt; ++i) {
            br_read_se(&b);
        }
    }
    br_read_ue(&b);
    br_read_bit(&b);

    int pic_width_in_mbs = (int)br_read_ue(&b) + 1;
    int pic_height_in_map = (int)br_read_ue(&b) + 1;
    int frame_mbs_only = (int)br_read_bit(&b);
    if (!frame_mbs_only) {
        br_read_bit(&b);
    }
    br_read_bit(&b);

    int crop_left = 0, crop_right = 0, crop_top = 0, crop_bottom = 0;
    if (br_read_bit(&b)) {
        crop_left = (int)br_read_ue(&b);
        crop_right = (int)br_read_ue(&b);
        crop_top = (int)br_read_ue(&b);
        crop_bottom = (int)br_read_ue(&b);
    }

    /* ITU-T H.264: CropUnitX / CropUnitY 依赖 chroma_format_idc */
    int crop_unit_x = 1;
    int crop_unit_y = 2 - frame_mbs_only;
    if (chroma == 1 || chroma == 2) {
        crop_unit_x = 2;
    }
    if (chroma == 1) {
        crop_unit_y = 2 * (2 - frame_mbs_only);
    }

    int w = pic_width_in_mbs * 16;
    int h = (2 - frame_mbs_only) * pic_height_in_map * 16;
    w -= (crop_left + crop_right) * crop_unit_x;
    h -= (crop_top + crop_bottom) * crop_unit_y;
    if (w <= 0 || h <= 0) {
        return 0;
    }

    memset(info, 0, sizeof(*info));
    info->width = w;
    info->height = h;
    info->fps = 0.0f;

    if (br_read_bit(&b)) {
        if (br_read_bit(&b)) {
            br_read_bits(&b, 8);
            br_read_bits(&b, 8);
            br_read_bits(&b, 8);
        }
        if (br_read_bit(&b)) {
            br_read_bits(&b, 32);
            br_read_bits(&b, 32);
            br_read_bits(&b, 32);
        }
        if (br_read_bit(&b)) {
            br_read_bit(&b);
        }
        if (br_read_bit(&b)) {
            br_read_bits(&b, 16);
            br_read_bits(&b, 16);
            br_read_bits(&b, 16);
            br_read_bits(&b, 16);
            br_read_bits(&b, 16);
            br_read_bits(&b, 16);
            br_read_bits(&b, 24);
            br_read_bits(&b, 24);
            if (br_read_bit(&b)) {
                br_read_ue(&b);
            }
        }
        if (br_read_bit(&b)) {
            br_read_ue(&b);
            br_read_ue(&b);
            br_read_ue(&b);
            br_read_ue(&b);
            if (br_read_bit(&b)) {
                uint32_t num_units = br_read_bits(&b, 32);
                uint32_t time_scale = br_read_bits(&b, 32);
                int fixed = (int)br_read_bit(&b);
                if (num_units > 0 && time_scale > 0) {
                    (void)fixed;
                    /* H.264: frame rate ≈ time_scale / (2 * num_units_in_tick) */
                    info->fps = (float)time_scale / (2.0f * (float)num_units);
                }
            }
        }
    }
    return 1;
}
