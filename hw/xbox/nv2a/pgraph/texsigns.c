/*
 * QEMU texture channel sign conversions for Geforce NV2A
 *
 * Copyright (c) 2024 coldhex
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "qemu/osdep.h"
#include "glsl/psh.h"
#include "texsigns.h"
#include "qemu/host-utils.h"
#include "hw/xbox/nv2a/nv2a_regs.h"
#include "qemu/xemu-uwp.h"

/*
 * Signed blend equations (upstream xemu #996: headlight glow drawn as a solid white area). The source
 * is read as two's complement, so a white source adds almost nothing. Approach of upstream xemu PR #1901
 * (polymetal0), checked there against nxdk_pgraph_tests hardware results.
 */
uint32_t texsigns_blend_equation(uint32_t equation, bool *signed_rgb)
{
    *signed_rgb = false;
    if (equation != 5 && equation != 6) {
        return equation;
    }
#ifdef XEMU_UWP
    static int off = -1;
    if (off < 0) {
        extern bool xemu_uwp_test_file_exists(const char *name); /* ui/xemu.c */
        off = xemu_uwp_test_file_exists("signed-blend-off.txt");
        if (off) {
            uwp_log("blend test: signed blend equations as plain add/subtract (signed-blend-off.txt)");
        }
    }
    if (off) {
        return equation;
    }
    static bool noted;
    if (!noted) {
        noted = true;
        uwp_log("signed blending: first draw with %s", equation == 6 ? "ADD_SIGNED" : "REVERSE_SUBTRACT_SIGNED");
    }
#endif
    *signed_rgb = true;
    return equation == 5 ? 6 : 5;
}

uint8_t texsigns_channel_signs(uint32_t filter)
{
#ifdef XEMU_UWP
    /* test file signed-tex-off.txt: signed channels are read as unsigned again (as before the port) */
    static int off = -1;
    if (off < 0) {
        extern bool xemu_uwp_test_file_exists(const char *name); /* ui/xemu.c */
        off = xemu_uwp_test_file_exists("signed-tex-off.txt");
        if (off) {
            uwp_log("texture test: signed texture channels read as unsigned (signed-tex-off.txt)");
        }
    }
    if (off) {
        return 0;
    }
#endif
    return GET_MASK(filter, NV_PGRAPH_TEXFILTER0_ASIGNED | NV_PGRAPH_TEXFILTER0_RSIGNED |
                            NV_PGRAPH_TEXFILTER0_GSIGNED | NV_PGRAPH_TEXFILTER0_BSIGNED);
}

struct conversion_info {
    bool asigned;
    bool rsigned;
    bool gsigned;
    bool bsigned;
    const uint8_t *palette_data;
};

#define CHANNEL_BIT_REPLICATE(value, num_in_bits, num_out_bits) \
    (((value) << ((num_out_bits) - (num_in_bits))) |            \
     ((value) >> (2*(num_in_bits) - (num_out_bits))))

static uint8_t channel_8bit_expansion(unsigned int value, int num_in_bits)
{
    return CHANNEL_BIT_REPLICATE(value, num_in_bits, 8);
}

static uint8_t channel_4to8_lookup[16] = { 0, };
static uint8_t channel_5to8_lookup[32] = { 0, };
static uint8_t channel_6to8_lookup[64] = { 0, };

void texsigns_init_conversion(void)
{
    unsigned int i;

    for (i = 0; i < sizeof(channel_4to8_lookup); i++) {
	channel_4to8_lookup[i] = channel_8bit_expansion(i, 4);
    }

    for (i = 0; i < sizeof(channel_5to8_lookup); i++) {
	channel_5to8_lookup[i] = channel_8bit_expansion(i, 5);
    }

    for (i = 0; i < sizeof(channel_6to8_lookup); i++) {
	channel_6to8_lookup[i] = channel_8bit_expansion(i, 6);
    }
}

static int convert_pixel_argb1555(const uint8_t *src, uint8_t *dest,
				  struct conversion_info *info)
{
    uint16_t argb1555 = src[0] + (src[1] << 8);
    unsigned int red = (argb1555 & 0x7C00) >> 10;
    unsigned int green = (argb1555 & 0x03E0) >> 5;
    unsigned int blue = argb1555 & 0x001F;
    unsigned int alpha = argb1555 >> 15;
    dest[0] = channel_5to8_lookup[red];
    dest[1] = channel_5to8_lookup[green];
    dest[2] = channel_5to8_lookup[blue];
    dest[3] = -(int)alpha;
    return 2;
}

static int convert_pixel_xrgb1555(const uint8_t *src, uint8_t *dest,
				  struct conversion_info *info)
{
    uint16_t rgb555 = src[0] + (src[1] << 8);
    unsigned int red = (rgb555 & 0x7C00) >> 10;
    unsigned int green = (rgb555 & 0x03E0) >> 5;
    unsigned int blue = rgb555 & 0x001F;
    dest[0] = channel_5to8_lookup[red];
    dest[1] = channel_5to8_lookup[green];
    dest[2] = channel_5to8_lookup[blue];
    dest[3] = 255;
    return 2;
}

static int convert_pixel_argb4444(const uint8_t *src, uint8_t *dest,
				  struct conversion_info *info)
{
    unsigned int alpha = (src[1] & 0xF0) >> 4;
    unsigned int red = src[1] & 0x0F;
    unsigned int green = (src[0] & 0xF0) >> 4;
    unsigned int blue = src[0] & 0x0F;
    dest[0] = channel_4to8_lookup[red];
    dest[1] = channel_4to8_lookup[green];
    dest[2] = channel_4to8_lookup[blue];
    dest[3] = channel_4to8_lookup[alpha];
    return 2;
}

static int convert_pixel_rgb565(const uint8_t *src, uint8_t *dest,
				struct conversion_info *info)
{
    uint16_t rgb565 = src[0] + (src[1] << 8);
    unsigned int red = (rgb565 & 0xF800) >> 11;
    unsigned int green = (rgb565 & 0x07E0) >> 5;
    unsigned int blue = rgb565 & 0x001F;
    dest[0] = channel_5to8_lookup[red];
    dest[1] = channel_6to8_lookup[green];
    dest[2] = channel_5to8_lookup[blue];
    dest[3] = 255;
    return 2;
}

static int convert_pixel_rgb655(const uint8_t *src, uint8_t *dest,
				struct conversion_info *info)
{
    uint16_t rgb655 = src[0] + (src[1] << 8);
    unsigned int red = (rgb655 & 0xFC00) >> 10;
    unsigned int green = (rgb655 & 0x03E0) >> 5;
    unsigned int blue = rgb655 & 0x001F;
    dest[0] = channel_6to8_lookup[red];
    dest[1] = channel_5to8_lookup[green];
    dest[2] = channel_5to8_lookup[blue];
    dest[3] = 255;
    return 2;
}

static int convert_pixel_y8(const uint8_t *src, uint8_t *dest,
			    struct conversion_info *info)
{
    dest[0] = src[0];
    dest[1] = src[0];
    dest[2] = src[0];
    dest[3] = 255;
    return 1;
}

static int convert_pixel_y16(const uint16_t *src, uint16_t *dest)
{
    dest[0] = 65535;
    dest[1] = src[0];
    dest[2] = src[0];
    dest[3] = 65535;
    return 1;
}

static int convert_pixel_ay8(const uint8_t *src, uint8_t *dest,
                             struct conversion_info *info)
{
    dest[0] = src[0];
    dest[1] = src[0];
    dest[2] = src[0];
    dest[3] = src[0];
    return 1;
}

static int convert_pixel_a8(const uint8_t *src, uint8_t *dest,
			    struct conversion_info *info)
{
    dest[0] = 255;
    dest[1] = 255;
    dest[2] = 255;
    dest[3] = src[0];
    return 1;
}

static int convert_pixel_ay88(const uint8_t *src, uint8_t *dest,
                              struct conversion_info *info)
{
    dest[0] = src[0];
    dest[1] = src[0];
    dest[2] = src[0];
    dest[3] = src[1];
    return 2;
}


static int convert_pixel_rb88(const uint8_t *src, uint8_t *dest,
			      struct conversion_info *info)
{
    uint8_t blue = src[0];
    uint8_t red = src[1];
    uint8_t green = blue;
    uint8_t alpha = red;
    dest[0] = red;
    dest[1] = green;
    dest[2] = blue;
    dest[3] = alpha;
    return 2;
}

static int convert_pixel_r16b16(const uint16_t *src, uint16_t *dest)
{
    uint16_t blue = src[0];
    uint16_t red = src[1];
    uint16_t green = blue;
    uint16_t alpha = red;
    dest[0] = red;
    dest[1] = green;
    dest[2] = blue;
    dest[3] = alpha;
    return 2;
}

static int convert_pixel_gb88(const uint8_t *src, uint8_t *dest,
			      struct conversion_info *info)
{
    uint8_t blue = src[0];
    uint8_t green = src[1];
    uint8_t red = blue;
    uint8_t alpha = green;
    dest[0] = red;
    dest[1] = green;
    dest[2] = blue;
    dest[3] = alpha;
    return 2;
}

static int convert_pixel_argb8888(const uint8_t *src, uint8_t *dest,
				  struct conversion_info *info)
{
    dest[0] = src[2];
    dest[1] = src[1];
    dest[2] = src[0];
    dest[3] = src[3];
    return 4;
}

static int convert_pixel_xrgb8888(const uint8_t *src, uint8_t *dest,
				  struct conversion_info *info)
{
    dest[0] = src[2];
    dest[1] = src[1];
    dest[2] = src[0];
    dest[3] = 255;
    return 4;
}

static int convert_pixel_abgr8888(const uint8_t *src, uint8_t *dest,
				  struct conversion_info *info)
{
    *(uint32_t *)dest = *(uint32_t *)src;
    return 4;
}

static int convert_pixel_bgra8888(const uint8_t *src, uint8_t *dest,
				  struct conversion_info *info)
{
    dest[0] = src[1];
    dest[1] = src[2];
    dest[2] = src[3];
    dest[3] = src[0];
    return 4;
}

static int convert_pixel_rgba8888(const uint8_t *src, uint8_t *dest,
				  struct conversion_info *info)
{
    dest[0] = src[3];
    dest[1] = src[2];
    dest[2] = src[1];
    dest[3] = src[0];
    return 4;
}

static int convert_pixel_i8_argb8888(const uint8_t *src, uint8_t *dest,
                                     struct conversion_info *info)
{
    uint32_t pixel = ((uint32_t *)info->palette_data)[src[0]];
    uint8_t red = (pixel >> 16) & 0xFF;
    uint8_t blue = pixel & 0xFF;
    *(uint32_t *)dest = (pixel & 0xFF00FF00) | (blue << 16) | red;
    return 1;
}

static inline uint8_t *convert(const uint8_t *data, const uint8_t *palette_data,
                               int width, int height, int depth, int row_pitch,
                               int slice_pitch, int channel_signs,
                               size_t *out_converted_size,
			       int (*convert_pixel)(const uint8_t *, uint8_t *, struct conversion_info *))
{
    size_t converted_size = width * height * depth * 4;
    uint8_t *converted_data = (uint8_t *)g_malloc(converted_size);
    uint8_t *pixel = converted_data;
    struct conversion_info info = {
        channel_signs & TEX_CHANNEL_ASIGNED,
        channel_signs & TEX_CHANNEL_RSIGNED,
        channel_signs & TEX_CHANNEL_GSIGNED,
        channel_signs & TEX_CHANNEL_BSIGNED,
        palette_data
    };
    uint32_t signs = (info.rsigned ? 0x80 : 0) |
        (info.gsigned ? 0x8000 : 0) |
        (info.bsigned ? 0x800000 : 0) |
        (info.asigned ? 0x80000000 : 0);
    int x, y, z;

    for (z = 0; z < depth; z++) {
	for (y = 0; y < height; y++) {
	    const uint8_t *row = data + y * row_pitch;
	    for (x = 0; x < width; x++) {
		row += convert_pixel(row, pixel, &info);
		*(uint32_t *)pixel ^= signs;
		pixel += 4;
	    }
	}

	data += slice_pitch;
    }

    if (out_converted_size) {
        *out_converted_size = converted_size;
    }
    return converted_data;
}

static inline uint8_t *convert_16bit(const uint8_t *data, int width, int height,
                                     int depth, int row_pitch, int slice_pitch,
                                     size_t *out_converted_size,
                                     int (*convert_pixel)(const uint16_t *, uint16_t *))
{
    size_t converted_size = width * height * depth * 8;
    uint8_t *converted_data = (uint8_t *)g_malloc(converted_size);
    uint16_t *pixel = (uint16_t *)converted_data;
    int x, y, z;

    for (z = 0; z < depth; z++) {
	for (y = 0; y < height; y++) {
	    const uint16_t *row = (uint16_t *)(data + y * row_pitch);
	    for (x = 0; x < width; x++) {
		row += convert_pixel(row, pixel);
		*(uint64_t *)pixel ^= 0x8000000080000000;
		pixel += 4;
	    }
	}

	data += slice_pitch;
    }

    if (out_converted_size) {
        *out_converted_size = converted_size;
    }
    return converted_data;
}

void texsigns_inplace_to_unsigned_rgba(uint8_t *data, int len, int channel_signs)
{
    if (!channel_signs) {
        return;
    }

    bool asigned = channel_signs & TEX_CHANNEL_ASIGNED;
    bool rsigned = channel_signs & TEX_CHANNEL_RSIGNED;
    bool gsigned = channel_signs & TEX_CHANNEL_GSIGNED;
    bool bsigned = channel_signs & TEX_CHANNEL_BSIGNED;
    uint32_t signs = (rsigned ? 0x80 : 0) |
        (gsigned ? 0x8000 : 0) |
        (bsigned ? 0x800000 : 0) |
        (asigned ? 0x80000000 : 0);
    uint32_t *dest = (uint32_t *)data;
    int i;

    for (i = 0; i < len; i += 4) {
        *dest++ ^= signs;
    }
}

uint8_t *texsigns_convert_argb1555(const uint8_t *data, int width, int height,
                                   int depth, int row_pitch, int slice_pitch,
                                   int channel_signs,
                                   size_t *out_converted_size)
{
    return convert(data, NULL, width, height, depth, row_pitch, slice_pitch,
                   channel_signs, out_converted_size, convert_pixel_argb1555);
}

uint8_t *texsigns_convert_xrgb1555(const uint8_t *data, int width, int height,
                                   int depth, int row_pitch, int slice_pitch,
                                   int channel_signs,
                                   size_t *out_converted_size)
{
    return convert(data, NULL, width, height, depth, row_pitch, slice_pitch,
                   channel_signs, out_converted_size, convert_pixel_xrgb1555);
}

uint8_t *texsigns_convert_argb4444(const uint8_t *data, int width, int height,
                                   int depth, int row_pitch, int slice_pitch,
                                   int channel_signs,
                                   size_t *out_converted_size)
{
    return convert(data, NULL, width, height, depth, row_pitch, slice_pitch,
                   channel_signs, out_converted_size, convert_pixel_argb4444);
}

uint8_t *texsigns_convert_rgb565(const uint8_t *data, int width, int height,
                                 int depth, int row_pitch, int slice_pitch,
                                 int channel_signs, size_t *out_converted_size)
{
    return convert(data, NULL, width, height, depth, row_pitch, slice_pitch,
                   channel_signs, out_converted_size, convert_pixel_rgb565);
}

uint8_t *texsigns_convert_rgb655(const uint8_t *data, int width, int height,
                                 int depth, int row_pitch, int slice_pitch,
                                 int channel_signs, size_t *out_converted_size)
{
    return convert(data, NULL, width, height, depth, row_pitch, slice_pitch,
                   channel_signs, out_converted_size, convert_pixel_rgb655);
}

uint8_t *texsigns_convert_y8(const uint8_t *data, int width, int height,
                             int depth, int row_pitch, int slice_pitch,
                             int channel_signs, size_t *out_converted_size)
{
    return convert(data, NULL, width, height, depth, row_pitch, slice_pitch,
                   channel_signs, out_converted_size, convert_pixel_y8);
}

uint8_t *texsigns_convert_y16(const uint8_t *data, int width, int height,
                              int depth, int row_pitch, int slice_pitch,
                              size_t *out_converted_size)
{
    return convert_16bit(data, width, height, depth, row_pitch, slice_pitch,
                         out_converted_size, convert_pixel_y16);
}

uint8_t *texsigns_convert_ay8(const uint8_t *data, int width, int height,
                              int depth, int row_pitch, int slice_pitch,
                              int channel_signs, size_t *out_converted_size)
{
    return convert(data, NULL, width, height, depth, row_pitch, slice_pitch,
                   channel_signs, out_converted_size, convert_pixel_ay8);
}

uint8_t *texsigns_convert_a8(const uint8_t *data, int width, int height,
                             int depth, int row_pitch, int slice_pitch,
                             int channel_signs, size_t *out_converted_size)
{
    return convert(data, NULL, width, height, depth, row_pitch, slice_pitch,
                   channel_signs, out_converted_size, convert_pixel_a8);
}

uint8_t *texsigns_convert_ay88(const uint8_t *data, int width, int height,
                               int depth, int row_pitch, int slice_pitch,
                               int channel_signs, size_t *out_converted_size)
{
    return convert(data, NULL, width, height, depth, row_pitch, slice_pitch,
                   channel_signs, out_converted_size, convert_pixel_ay88);
}

uint8_t *texsigns_convert_rb88(const uint8_t *data, int width, int height,
                               int depth, int row_pitch, int slice_pitch,
                               int channel_signs, size_t *out_converted_size)
{
    return convert(data, NULL, width, height, depth, row_pitch, slice_pitch,
                   channel_signs, out_converted_size, convert_pixel_rb88);
}

uint8_t *texsigns_convert_gb88(const uint8_t *data, int width, int height,
                               int depth, int row_pitch, int slice_pitch,
                               int channel_signs, size_t *out_converted_size)
{
    return convert(data, NULL, width, height, depth, row_pitch, slice_pitch,
                   channel_signs, out_converted_size, convert_pixel_gb88);
}

uint8_t *texsigns_convert_r16b16(const uint8_t *data, int width, int height,
                                 int depth, int row_pitch, int slice_pitch,
                                 size_t *out_converted_size)
{
    return convert_16bit(data, width, height, depth, row_pitch, slice_pitch,
                         out_converted_size, convert_pixel_r16b16);
}

uint8_t *texsigns_convert_argb8888(const uint8_t *data, int width, int height,
                                   int depth, int row_pitch, int slice_pitch,
                                   int channel_signs,
                                   size_t *out_converted_size)
{
    return convert(data, NULL, width, height, depth, row_pitch, slice_pitch,
                   channel_signs, out_converted_size, convert_pixel_argb8888);
}

uint8_t *texsigns_convert_xrgb8888(const uint8_t *data, int width, int height,
                                   int depth, int row_pitch, int slice_pitch,
                                   int channel_signs,
                                   size_t *out_converted_size)
{
    return convert(data, NULL, width, height, depth, row_pitch, slice_pitch,
                   channel_signs, out_converted_size, convert_pixel_xrgb8888);
}

uint8_t *texsigns_convert_abgr8888(const uint8_t *data, int width, int height,
                                   int depth, int row_pitch, int slice_pitch,
                                   int channel_signs,
                                   size_t *out_converted_size)
{
    return convert(data, NULL, width, height, depth, row_pitch, slice_pitch,
                   channel_signs, out_converted_size, convert_pixel_abgr8888);
}

uint8_t *texsigns_convert_bgra8888(const uint8_t *data, int width, int height,
                                   int depth, int row_pitch, int slice_pitch,
                                   int channel_signs,
                                   size_t *out_converted_size)
{
    return convert(data, NULL, width, height, depth, row_pitch, slice_pitch,
                   channel_signs, out_converted_size, convert_pixel_bgra8888);
}

uint8_t *texsigns_convert_rgba8888(const uint8_t *data, int width, int height,
                                   int depth, int row_pitch, int slice_pitch,
                                   int channel_signs,
                                   size_t *out_converted_size)
{
    return convert(data, NULL, width, height, depth, row_pitch, slice_pitch,
                   channel_signs, out_converted_size, convert_pixel_rgba8888);
}

uint8_t *texsigns_convert_i8_argb8888(const uint8_t *data,
                                      const uint8_t *palette_data, int width,
                                      int height, int depth, int row_pitch,
                                      int slice_pitch, int channel_signs,
                                      size_t *out_converted_size)
{
    return convert(data, palette_data, width, height, depth, row_pitch,
                   slice_pitch, channel_signs, out_converted_size,
                   convert_pixel_i8_argb8888);
}
