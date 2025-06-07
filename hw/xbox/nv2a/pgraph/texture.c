/*
 * QEMU Geforce NV2A implementation
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2015 Jannik Vogel
 * Copyright (c) 2018-2025 Matt Borgerson
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

#include "hw/xbox/nv2a/nv2a_int.h"
#include "texture.h"
#include "texsigns.h"
#include "util.h"

const BasicColorFormatInfo kelvin_color_format_info_map[66] = {
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_Y8] = { 1, false },
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_AY8] = { 1, false },
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A1R5G5B5] = { 2, false },
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_X1R5G5B5] = { 2, false },
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A4R4G4B4] = { 2, false },
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R5G6B5] = { 2, false },
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8R8G8B8] = { 4, false },
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_X8R8G8B8] = { 4, false },

    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_I8_A8R8G8B8] = { 1, false },

    [NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5] = { 4, false },
    [NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT23_A8R8G8B8] = { 4, false },
    [NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT45_A8R8G8B8] = { 4, false },
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A1R5G5B5] = { 2, true },
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_R5G6B5] = { 2, true },
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8R8G8B8] = { 4, true },
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_Y8] = { 1, true },
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_G8B8] = { 2, true },
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8] = { 1, false },
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8Y8] = { 2, false },
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_AY8] = { 1, true },
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X1R5G5B5] = { 2, true },
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A4R4G4B4] = { 2, true },
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X8R8G8B8] = { 4, true },
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8] = { 1, true },
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8Y8] = { 2, true },

    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R6G5B5] = { 2, false },
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_G8B8] = { 2, false },
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R8B8] = { 2, false },

    [NV097_SET_TEXTURE_FORMAT_COLOR_LC_IMAGE_CR8YB8CB8YA8] = { 2, true },
    [NV097_SET_TEXTURE_FORMAT_COLOR_LC_IMAGE_YB8CR8YA8CB8] = { 2, true },

    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_DEPTH_Y16_FIXED] = { 2, false, true },
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_X8_Y24_FIXED] = { 4, true,
                                                                     true },
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_X8_Y24_FLOAT] = { 4, true,
                                                                     true },
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_Y16_FIXED] = { 2, true,
                                                                  true },
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_Y16_FLOAT] = { 2, true,
                                                                  true },

    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_Y16] = { 2, false },
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R16B16] = { 4, false },
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_Y16] = { 2, true },
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_R16B16] = { 4, true },
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8B8G8R8] = { 4, false },
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_B8G8R8A8] = { 4, false },
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R8G8B8A8] = { 4, false },
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8B8G8R8] = { 4, true },
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_B8G8R8A8] = { 4, true },
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_R8G8B8A8] = { 4, true },
};

hwaddr pgraph_get_texture_phys_addr(PGRAPHState *pg, int texture_idx)
{
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    int i = texture_idx;

    uint32_t fmt = pgraph_reg_r(pg, NV_PGRAPH_TEXFMT0 + i*4);
    unsigned int dma_select =
        GET_MASK(fmt, NV_PGRAPH_TEXFMT0_CONTEXT_DMA);

    hwaddr offset = pgraph_reg_r(pg, NV_PGRAPH_TEXOFFSET0 + i*4);

    hwaddr dma_len;
    uint8_t *texture_data;
    if (dma_select) {
        texture_data = (uint8_t*)nv_dma_map(d, pg->dma_b, &dma_len);
    } else {
        texture_data = (uint8_t*)nv_dma_map(d, pg->dma_a, &dma_len);
    }
    assert(offset < dma_len);
    texture_data += offset;

    return texture_data - d->vram_ptr;
}

hwaddr pgraph_get_texture_palette_phys_addr_length(PGRAPHState *pg, int texture_idx, size_t *length)
{
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    int i = texture_idx;

    uint32_t palette = pgraph_reg_r(pg, NV_PGRAPH_TEXPALETTE0 + i*4);
    bool palette_dma_select =
        GET_MASK(palette, NV_PGRAPH_TEXPALETTE0_CONTEXT_DMA);
    unsigned int palette_length_index =
        GET_MASK(palette, NV_PGRAPH_TEXPALETTE0_LENGTH);
    unsigned int palette_offset =
        palette & NV_PGRAPH_TEXPALETTE0_OFFSET;

    unsigned int palette_length = 0;
    switch (palette_length_index) {
    case NV_PGRAPH_TEXPALETTE0_LENGTH_256: palette_length = 256; break;
    case NV_PGRAPH_TEXPALETTE0_LENGTH_128: palette_length = 128; break;
    case NV_PGRAPH_TEXPALETTE0_LENGTH_64: palette_length = 64; break;
    case NV_PGRAPH_TEXPALETTE0_LENGTH_32: palette_length = 32; break;
    default: assert(!"Invalid palette length"); break;
    }
    if (length) {
        *length = palette_length * 4;
    }

    hwaddr palette_dma_len;
    uint8_t *palette_data;
    if (palette_dma_select) {
        palette_data = (uint8_t*)nv_dma_map(d, pg->dma_b, &palette_dma_len);
    } else {
        palette_data = (uint8_t*)nv_dma_map(d, pg->dma_a, &palette_dma_len);
    }
    assert(palette_offset < palette_dma_len);
    palette_data += palette_offset;

    return palette_data - d->vram_ptr;
}

size_t pgraph_get_texture_length(PGRAPHState *pg, TextureShape *shape)
{
    BasicColorFormatInfo f = kelvin_color_format_info_map[shape->color_format];
    size_t length = 0;

    if (f.linear) {
        assert(shape->cubemap == false);
        assert(shape->dimensionality == 2);
        length = shape->height * shape->pitch;
    } else {
        if (shape->dimensionality >= 2) {
            unsigned int w = shape->width, h = shape->height;
            int level;
            if (!pgraph_is_texture_format_compressed(pg, shape->color_format)) {
                for (level = 0; level < shape->levels; level++) {
                    w = MAX(w, 1);
                    h = MAX(h, 1);
                    length += w * h * f.bytes_per_pixel;
                    w /= 2;
                    h /= 2;
                }
            } else {
                /* Compressed textures are a bit different */
                unsigned int block_size =
                    shape->color_format ==
                            NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5 ?
                        8 : 16;
                for (level = 0; level < shape->levels; level++) {
                    w = MAX(w, 1);
                    h = MAX(h, 1);
                    unsigned int phys_w = (w + 3) & ~3,
                                 phys_h = (h + 3) & ~3;
                    length += phys_w/4 * phys_h/4 * block_size;
                    w /= 2;
                    h /= 2;
                }
            }
            if (shape->cubemap) {
                assert(shape->dimensionality == 2);
                length = (length + NV2A_CUBEMAP_FACE_ALIGNMENT - 1) & ~(NV2A_CUBEMAP_FACE_ALIGNMENT - 1);
                length *= 6;
            }
            if (shape->dimensionality >= 3) {
                length *= shape->depth;
            }
        }
    }

    return length;
}

TextureShape pgraph_get_texture_shape(PGRAPHState *pg, int texture_idx)
{
    int i = texture_idx;

    uint32_t ctl_0 = pgraph_reg_r(pg, NV_PGRAPH_TEXCTL0_0 + i*4);
    uint32_t ctl_1 = pgraph_reg_r(pg, NV_PGRAPH_TEXCTL1_0 + i*4);
    uint32_t fmt = pgraph_reg_r(pg, NV_PGRAPH_TEXFMT0 + i*4);
    uint32_t filter = pgraph_reg_r(pg, NV_PGRAPH_TEXFILTER0 + i*4);

#if DEBUG_NV2A
    uint32_t address = pgraph_reg_r(pg, NV_PGRAPH_TEXADDRESS0 + i*4);
#endif

    unsigned int min_mipmap_level =
        GET_MASK(ctl_0, NV_PGRAPH_TEXCTL0_0_MIN_LOD_CLAMP);
    unsigned int max_mipmap_level =
        GET_MASK(ctl_0, NV_PGRAPH_TEXCTL0_0_MAX_LOD_CLAMP);

    unsigned int pitch =
        GET_MASK(ctl_1, NV_PGRAPH_TEXCTL1_0_IMAGE_PITCH);

    bool cubemap =
        GET_MASK(fmt, NV_PGRAPH_TEXFMT0_CUBEMAPENABLE);
    unsigned int dimensionality =
        GET_MASK(fmt, NV_PGRAPH_TEXFMT0_DIMENSIONALITY);

    int tex_mode = (pgraph_reg_r(pg, NV_PGRAPH_SHADERPROG) >> (texture_idx * 5)) & 0x1F;
    if (tex_mode == 0x02) {
        assert(pgraph_is_texture_enabled(pg, texture_idx));
        // assert(state.dimensionality == 3);

        // OVERRIDE
        // dimensionality = 3;
    }

    unsigned int color_format = GET_MASK(fmt, NV_PGRAPH_TEXFMT0_COLOR);
    unsigned int levels = GET_MASK(fmt, NV_PGRAPH_TEXFMT0_MIPMAP_LEVELS);
    unsigned int log_width = GET_MASK(fmt, NV_PGRAPH_TEXFMT0_BASE_SIZE_U);
    unsigned int log_height = GET_MASK(fmt, NV_PGRAPH_TEXFMT0_BASE_SIZE_V);
    unsigned int log_depth = GET_MASK(fmt, NV_PGRAPH_TEXFMT0_BASE_SIZE_P);

    unsigned int rect_width =
        GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_TEXIMAGERECT0 + i*4),
                 NV_PGRAPH_TEXIMAGERECT0_WIDTH);
    unsigned int rect_height =
        GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_TEXIMAGERECT0 + i*4),
                 NV_PGRAPH_TEXIMAGERECT0_HEIGHT);
#if DEBUG_NV2A
    unsigned int lod_bias =
        GET_MASK(filter, NV_PGRAPH_TEXFILTER0_MIPMAP_LOD_BIAS);
#endif
    unsigned int border_source = GET_MASK(fmt,
                                          NV_PGRAPH_TEXFMT0_BORDER_SOURCE);

    NV2A_DPRINTF(" texture %d is format 0x%x, "
                    "off 0x%" HWADDR_PRIx " (r %d, %d or %d, %d, %d; %d%s),"
                    " filter %x %x, levels %d-%d %d bias %d\n",
                 i, color_format, address,
                 rect_width, rect_height,
                 1 << log_width, 1 << log_height, 1 << log_depth,
                 pitch,
                 cubemap ? "; cubemap" : "",
                 GET_MASK(filter, NV_PGRAPH_TEXFILTER0_MIN),
                 GET_MASK(filter, NV_PGRAPH_TEXFILTER0_MAG),
                 min_mipmap_level, max_mipmap_level, levels,
                 lod_bias);

    assert(color_format < ARRAY_SIZE(kelvin_color_format_info_map));
    BasicColorFormatInfo f = kelvin_color_format_info_map[color_format];
    if (f.bytes_per_pixel == 0) {
        fprintf(stderr, "nv2a: unimplemented texture color format 0x%x\n",
                color_format);
        abort();
    }

    unsigned int width, height, depth;
    if (f.linear) {
        assert(dimensionality == 2);
        width = rect_width;
        height = rect_height;
        depth = 1;
    } else {
        width = 1 << log_width;
        height = 1 << log_height;
        depth = 1 << log_depth;
        pitch = 0;

        levels = MIN(levels, max_mipmap_level + 1);

        /* Discard mipmap levels that would be smaller than 1x1.
         * FIXME: Is this actually needed?
         *
         * >> Level 0: 32 x 4
         *    Level 1: 16 x 2
         *    Level 2: 8 x 1
         *    Level 3: 4 x 1
         *    Level 4: 2 x 1
         *    Level 5: 1 x 1
         */
        levels = MIN(levels, MAX(log_width, log_height) + 1);
        assert(levels > 0);

        if (dimensionality == 3) {
            /* FIXME: What about 3D mipmaps? */
            if (log_width < 2 || log_height < 2) {
                /* Base level is smaller than 4x4... */
                levels = 1;
            } else {
                levels = MIN(levels, MIN(log_width, log_height) - 1);
            }
        }
        min_mipmap_level = MIN(levels-1, min_mipmap_level);
        max_mipmap_level = MIN(levels-1, max_mipmap_level);
    }

    TextureShape shape;

    // We will hash it, so make sure any padding is zero
    memset(&shape, 0, sizeof(shape));

    shape.cubemap = cubemap;
    shape.dimensionality = dimensionality;
    shape.color_format = color_format;
    shape.levels = levels;
    shape.width = width;
    shape.height = height;
    shape.depth = depth;
    shape.min_mipmap_level = min_mipmap_level;
    shape.max_mipmap_level = max_mipmap_level;
    shape.pitch = pitch;
    shape.border = border_source != NV_PGRAPH_TEXFMT0_BORDER_SOURCE_COLOR;
    shape.channel_signs = texsigns_channel_signs(filter);
    return shape;
}

unsigned int pgraph_get_converted_bit_depth(const TextureShape *s)
{
    switch (s->color_format) {
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_I8_A8R8G8B8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R6G5B5:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LC_IMAGE_CR8YB8CB8YA8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LC_IMAGE_YB8CR8YA8CB8:
        // Will be converted to RGBA8
        return 8;
    }

    if (!s->channel_signs) {
        // Will not be converted
        return 0;
    }

    switch (s->color_format) {
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A1R5G5B5:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A1R5G5B5:
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_X1R5G5B5:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X1R5G5B5:
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A4R4G4B4:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A4R4G4B4:
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R5G6B5:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_R5G6B5:
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_Y8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_Y8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_AY8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_AY8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8Y8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8Y8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R8B8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_G8B8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_G8B8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8R8G8B8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8R8G8B8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_X8R8G8B8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X8R8G8B8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8B8G8R8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8B8G8R8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_B8G8R8A8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_B8G8R8A8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R8G8B8A8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_R8G8B8A8:
        // Will be converted to RGBA8
        return 8;

    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_Y16:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_Y16:
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R16B16:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_R16B16:
        // Will be converted to RGBA16
        return 16;

    default:
        // Will not be converted
        return 0;
    }
}

uint8_t *pgraph_convert_texture_data(const TextureShape s, const uint8_t *data,
                                     const uint8_t *palette_data,
                                     unsigned int width, unsigned int height,
                                     unsigned int depth, unsigned int row_pitch,
                                     unsigned int slice_pitch,
                                     size_t *converted_size)
{
    switch (s.color_format) {
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_I8_A8R8G8B8:
        assert(pgraph_get_converted_bit_depth(&s) == 8);
        return texsigns_convert_i8_argb8888(data, palette_data, width, height,
                                            depth, row_pitch, slice_pitch,
                                            s.channel_signs, converted_size);

    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R6G5B5:
        assert(pgraph_get_converted_bit_depth(&s) == 8);
        return texsigns_convert_rgb655(data, width, height, depth, row_pitch,
                                       slice_pitch, s.channel_signs,
                                       converted_size);

    case NV097_SET_TEXTURE_FORMAT_COLOR_LC_IMAGE_CR8YB8CB8YA8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LC_IMAGE_YB8CR8YA8CB8: {
        assert(pgraph_get_converted_bit_depth(&s) == 8);
        // TODO: Investigate whether a non-1 depth is possible.
        // Generally the hardware asserts when attempting to use volumetric
        // textures in linear formats.
        assert(depth == 1); /* FIXME */
        // FIXME: only valid if control0 register allows for colorspace conversion
        size_t sz = width * height * 4;
        uint8_t *converted_data = g_malloc(sz);
        uint8_t *pixel = converted_data;
        int x, y;
        if (s.color_format
            == NV097_SET_TEXTURE_FORMAT_COLOR_LC_IMAGE_CR8YB8CB8YA8) {
            for (y = 0; y < height; y++) {
                const uint8_t *line = &data[y * row_pitch];
                for (x = 0; x < width; x++, pixel += 4) {
                    pixel[0] = line[2*x];
                    pixel[1] = line[(2*x & ~3) + 1];
                    pixel[2] = line[(2*x & ~3) + 3];
                    pixel[3] = 255;
                }
            }
        } else {
            for (y = 0; y < height; y++) {
                const uint8_t *line = &data[y * row_pitch];
                for (x = 0; x < width; x++, pixel += 4) {
                    pixel[0] = line[2*x + 1];
                    pixel[1] = line[2*x & ~3];
                    pixel[2] = line[(2*x & ~3) + 2];
                    pixel[3] = 255;
                }
            }
        }
        texsigns_inplace_to_unsigned_rgba(converted_data, width * height * 4,
                                          s.channel_signs);
        if (converted_size) {
            *converted_size = sz;
        }
        return converted_data;
    }
    default:
        if (!s.channel_signs) {
            assert(pgraph_get_converted_bit_depth(&s) == 0);
            return NULL;
        }
        break;
    }

    uint8_t *converted_data = NULL;

    switch (s.color_format) {
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A1R5G5B5:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A1R5G5B5:
        converted_data = texsigns_convert_argb1555(data, width, height, depth,
                                                   row_pitch, slice_pitch,
                                                   s.channel_signs,
                                                   converted_size);
        break;
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_X1R5G5B5:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X1R5G5B5:
        converted_data = texsigns_convert_xrgb1555(data, width, height, depth,
                                                   row_pitch, slice_pitch,
                                                   s.channel_signs,
                                                   converted_size);
        break;
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A4R4G4B4:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A4R4G4B4:
        converted_data = texsigns_convert_argb4444(data, width, height, depth,
                                                   row_pitch, slice_pitch,
                                                   s.channel_signs,
                                                   converted_size);
        break;
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R5G6B5:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_R5G6B5:
        converted_data = texsigns_convert_rgb565(data, width, height, depth,
                                                 row_pitch, slice_pitch,
                                                 s.channel_signs,
                                                 converted_size);
        break;
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_Y8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_Y8:
        converted_data = texsigns_convert_y8(data, width, height, depth,
                                             row_pitch, slice_pitch,
                                             s.channel_signs, converted_size);
        break;
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_Y16:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_Y16:
        assert(pgraph_get_converted_bit_depth(&s) == 16);
        return texsigns_convert_y16(data, width, height, depth, row_pitch,
                                    slice_pitch, converted_size);
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_AY8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_AY8:
        converted_data = texsigns_convert_ay8(data, width, height, depth,
                                              row_pitch, slice_pitch,
                                              s.channel_signs, converted_size);
        break;
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8:
        converted_data = texsigns_convert_a8(data, width, height, depth,
                                             row_pitch, slice_pitch,
                                             s.channel_signs, converted_size);
        break;
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8Y8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8Y8:
        converted_data = texsigns_convert_ay88(data, width, height, depth,
                                               row_pitch, slice_pitch,
                                               s.channel_signs, converted_size);
        break;
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R8B8:
        converted_data = texsigns_convert_rb88(data, width, height, depth,
                                               row_pitch, slice_pitch,
                                               s.channel_signs, converted_size);
        break;
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_G8B8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_G8B8:
        converted_data = texsigns_convert_gb88(data, width, height, depth,
                                               row_pitch, slice_pitch,
                                               s.channel_signs, converted_size);
        break;
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R16B16:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_R16B16:
        assert(pgraph_get_converted_bit_depth(&s) == 16);
        return texsigns_convert_r16b16(data, width, height, depth, row_pitch,
                                       slice_pitch, converted_size);
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8R8G8B8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8R8G8B8:
        converted_data = texsigns_convert_argb8888(data, width, height, depth,
                                                   row_pitch, slice_pitch,
                                                   s.channel_signs,
                                                   converted_size);
        break;
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_X8R8G8B8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X8R8G8B8:
        converted_data = texsigns_convert_xrgb8888(data, width, height, depth,
                                                   row_pitch, slice_pitch,
                                                   s.channel_signs,
                                                   converted_size);
        break;
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8B8G8R8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8B8G8R8:
        converted_data = texsigns_convert_abgr8888(data, width, height, depth,
                                                   row_pitch, slice_pitch,
                                                   s.channel_signs,
                                                   converted_size);
        break;
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_B8G8R8A8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_B8G8R8A8:
        converted_data = texsigns_convert_bgra8888(data, width, height, depth,
                                                   row_pitch, slice_pitch,
                                                   s.channel_signs,
                                                   converted_size);
        break;
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R8G8B8A8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_R8G8B8A8:
        converted_data = texsigns_convert_rgba8888(data, width, height, depth,
                                                   row_pitch, slice_pitch,
                                                   s.channel_signs,
                                                   converted_size);
        break;
    default:
        NV2A_UNIMPLEMENTED("Signed channel data for color_format 0x%x",
                           s.color_format);
        assert(pgraph_get_converted_bit_depth(&s) == 0);
        return NULL;
    }

    assert(pgraph_get_converted_bit_depth(&s) == 8);
    return converted_data;
}
