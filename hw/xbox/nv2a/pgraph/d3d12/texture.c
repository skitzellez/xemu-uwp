/*
 * Geforce NV2A PGRAPH Direct3D 12 Renderer
 *
 * Copyright (c) 2026
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

#include "qemu/osdep.h"
#include "qemu/fast-hash.h"
#include "qapi/error.h"
#include "hw/xbox/nv2a/pgraph/s3tc.h"
#include "hw/xbox/nv2a/pgraph/swizzle.h"
#include "renderer.h"

#define TEXTURE_SKIP_INVALID_FORMAT   (1u << 1)
#define TEXTURE_SKIP_CUBEMAP          (1u << 2)
#define TEXTURE_SKIP_DIMENSION        (1u << 3)
#define TEXTURE_SKIP_COMPRESSED       (1u << 4)
#define TEXTURE_SKIP_ZERO_SIZE        (1u << 7)
#define TEXTURE_SKIP_VRAM_RANGE       (1u << 8)
#define TEXTURE_SKIP_UPLOAD           (1u << 9)

typedef struct PGRAPHD3D12TextureFormatInfo {
    DXGI_FORMAT resource_format;
    DXGI_FORMAT dxgi_format;
    unsigned int upload_bytes_per_pixel;
    bool converted;
    bool compressed;
    bool depth;
} PGRAPHD3D12TextureFormatInfo;

typedef struct PGRAPHD3D12TextureUploadLevel {
    unsigned int face;
    unsigned int level;
    unsigned int width;
    unsigned int height;
    unsigned int depth;
    /* Source rows per slice: texel rows, or block rows for BC formats. */
    unsigned int rows;
    hwaddr vram_addr;
    uint32_t decoded_row_pitch;
    uint32_t decoded_slice_pitch;
    uint32_t upload_row_pitch;
    uint32_t upload_slice_pitch;
    uint64_t upload_offset;
    uint64_t upload_size;
} PGRAPHD3D12TextureUploadLevel;

#define D3D12_REPORTED_TEXTURE_UPLOADS 256

static const char *texture_color_format_name(unsigned int format)
{
    switch (format) {
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_Y8:
        return "SZ_Y8";
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_AY8:
        return "SZ_AY8";
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A1R5G5B5:
        return "SZ_A1R5G5B5";
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_X1R5G5B5:
        return "SZ_X1R5G5B5";
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A4R4G4B4:
        return "SZ_A4R4G4B4";
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R5G6B5:
        return "SZ_R5G6B5";
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8R8G8B8:
        return "SZ_A8R8G8B8";
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_X8R8G8B8:
        return "SZ_X8R8G8B8";
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_I8_A8R8G8B8:
        return "SZ_I8_A8R8G8B8";
    case NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5:
        return "L_DXT1_A1R5G5B5";
    case NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT23_A8R8G8B8:
        return "L_DXT23_A8R8G8B8";
    case NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT45_A8R8G8B8:
        return "L_DXT45_A8R8G8B8";
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A1R5G5B5:
        return "LU_IMAGE_A1R5G5B5";
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_R5G6B5:
        return "LU_IMAGE_R5G6B5";
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8R8G8B8:
        return "LU_IMAGE_A8R8G8B8";
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_Y8:
        return "LU_IMAGE_Y8";
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_G8B8:
        return "LU_IMAGE_G8B8";
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8:
        return "SZ_A8";
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8Y8:
        return "SZ_A8Y8";
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_AY8:
        return "LU_IMAGE_AY8";
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X1R5G5B5:
        return "LU_IMAGE_X1R5G5B5";
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A4R4G4B4:
        return "LU_IMAGE_A4R4G4B4";
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X8R8G8B8:
        return "LU_IMAGE_X8R8G8B8";
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8:
        return "LU_IMAGE_A8";
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8Y8:
        return "LU_IMAGE_A8Y8";
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R6G5B5:
        return "SZ_R6G5B5";
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_G8B8:
        return "SZ_G8B8";
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R8B8:
        return "SZ_R8B8";
    case NV097_SET_TEXTURE_FORMAT_COLOR_LC_IMAGE_CR8YB8CB8YA8:
        return "LC_IMAGE_CR8YB8CB8YA8";
    case NV097_SET_TEXTURE_FORMAT_COLOR_LC_IMAGE_YB8CR8YA8CB8:
        return "LC_IMAGE_YB8CR8YA8CB8";
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_DEPTH_Y16_FIXED:
        return "SZ_DEPTH_Y16_FIXED";
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_X8_Y24_FIXED:
        return "LU_IMAGE_DEPTH_X8_Y24_FIXED";
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_X8_Y24_FLOAT:
        return "LU_IMAGE_DEPTH_X8_Y24_FLOAT";
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_Y16_FIXED:
        return "LU_IMAGE_DEPTH_Y16_FIXED";
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_Y16_FLOAT:
        return "LU_IMAGE_DEPTH_Y16_FLOAT";
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_Y16:
        return "LU_IMAGE_Y16";
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8B8G8R8:
        return "SZ_A8B8G8R8";
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_B8G8R8A8:
        return "SZ_B8G8R8A8";
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R8G8B8A8:
        return "SZ_R8G8B8A8";
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8B8G8R8:
        return "LU_IMAGE_A8B8G8R8";
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_B8G8R8A8:
        return "LU_IMAGE_B8G8R8A8";
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_R8G8B8A8:
        return "LU_IMAGE_R8G8B8A8";
    default:
        return "unknown";
    }
}

static D3D12_CPU_DESCRIPTOR_HANDLE cpu_descriptor_handle(
    ID3D12DescriptorHeap *heap, UINT descriptor_size, uint32_t index)
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle;

    heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(heap, &handle);
    handle.ptr += (SIZE_T)index * descriptor_size;
    return handle;
}

static D3D12_GPU_DESCRIPTOR_HANDLE gpu_descriptor_handle(
    ID3D12DescriptorHeap *heap, UINT descriptor_size, uint32_t index)
{
    D3D12_GPU_DESCRIPTOR_HANDLE handle;

    heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(heap, &handle);
    handle.ptr += (UINT64)index * descriptor_size;
    return handle;
}

static void report_texture_skip(NV2AState *d, unsigned int stage,
                                uint32_t reason, const char *fmt, ...)
    G_GNUC_PRINTF(4, 5);

static void report_texture_skip(NV2AState *d, unsigned int stage,
                                uint32_t reason, const char *fmt, ...)
{
    PGRAPHD3D12State *r = d->pgraph.d3d12_renderer_state;
    va_list ap;

    if (r->reported_texture_skip_reasons[stage] & reason) {
        return;
    }
    r->reported_texture_skip_reasons[stage] |= reason;

    fprintf(stderr, "Direct3D 12 renderer: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static void report_texture_unsupported_format(NV2AState *d,
                                              unsigned int stage,
                                              unsigned int color_format,
                                              const char *detail)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    uint64_t bit = color_format < 64 ? 1ull << color_format : 0;
    uint32_t texfmt = pgraph_reg_r(pg, NV_PGRAPH_TEXFMT0 + stage * 4);
    uint32_t texctl0 = pgraph_reg_r(pg, NV_PGRAPH_TEXCTL0_0 + stage * 4);
    uint32_t texctl1 = pgraph_reg_r(pg, NV_PGRAPH_TEXCTL1_0 + stage * 4);
    uint32_t texfilter = pgraph_reg_r(pg, NV_PGRAPH_TEXFILTER0 + stage * 4);
    uint32_t texaddress = pgraph_reg_r(pg, NV_PGRAPH_TEXADDRESS0 + stage * 4);
    uint32_t shaderprog = pgraph_reg_r(pg, NV_PGRAPH_SHADERPROG);
    unsigned int attr_index = NV2A_VERTEX_ATTR_TEXTURE0 + stage;
    VertexAttribute *texcoord = &pg->vertex_attributes[attr_index];

    if (bit && (r->reported_unsupported_texture_color_formats[stage] & bit)) {
        return;
    }
    if (bit) {
        r->reported_unsupported_texture_color_formats[stage] |= bit;
    }

    fprintf(stderr,
            "Direct3D 12 renderer: stage %u texture disabled for this draw: "
            "unsupported color format %s (0x%x), %s. texfmt=0x%08x, "
            "texctl0=0x%08x, texctl1=0x%08x, texfilter=0x%08x, "
            "texaddress=0x%08x, shaderprog=0x%08x, texcoord_attr=%u "
            "count=%u type=%u stride=%u dma=%c offset=0x%" HWADDR_PRIx
            ". Draw will continue without sampling stage %u.\n",
            stage, texture_color_format_name(color_format), color_format, detail,
            texfmt, texctl0, texctl1, texfilter, texaddress, shaderprog,
            attr_index, texcoord->count, texcoord->format,
            texcoord->stride, texcoord->dma_select ? 'B' : 'A',
            (hwaddr)texcoord->offset, stage);
}

static void report_texture_shape_skip(NV2AState *d, unsigned int stage,
                                      uint32_t reason,
                                      const TextureShape *shape,
                                      hwaddr vram_addr, size_t length,
                                      const char *detail)
{
    PGRAPHState *pg = &d->pgraph;

    report_texture_skip(
        d, stage, reason,
        "stage %u texture disabled for this draw: %s. format=%s (0x%x), "
        "vram=0x%" HWADDR_PRIx ", length=%zu, cubemap=%d, "
        "dimensionality=%u, levels=%u, size=%ux%ux%u, pitch=%u, "
        "border=%d, min_lod=%u, max_lod=%u, texfmt=0x%08x, "
        "texctl0=0x%08x, texctl1=0x%08x, texfilter=0x%08x, "
        "texaddress=0x%08x, shaderprog=0x%08x. Draw will continue "
        "without sampling stage %u.",
        stage, detail, texture_color_format_name(shape->color_format),
        shape->color_format, vram_addr, length, shape->cubemap,
        shape->dimensionality, shape->levels, shape->width, shape->height,
        shape->depth, shape->pitch, shape->border,
        shape->min_mipmap_level, shape->max_mipmap_level,
        pgraph_reg_r(pg, NV_PGRAPH_TEXFMT0 + stage * 4),
        pgraph_reg_r(pg, NV_PGRAPH_TEXCTL0_0 + stage * 4),
        pgraph_reg_r(pg, NV_PGRAPH_TEXCTL1_0 + stage * 4),
        pgraph_reg_r(pg, NV_PGRAPH_TEXFILTER0 + stage * 4),
        pgraph_reg_r(pg, NV_PGRAPH_TEXADDRESS0 + stage * 4),
        pgraph_reg_r(pg, NV_PGRAPH_SHADERPROG), stage);
}

static bool texture_format_for_shape(const TextureShape *shape,
                                     PGRAPHD3D12TextureFormatInfo *info)
{
    BasicColorFormatInfo basic =
        kelvin_color_format_info_map[shape->color_format];

    memset(info, 0, sizeof(*info));
    info->depth = basic.depth;

    switch (shape->color_format) {
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_Y8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_Y8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_AY8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_AY8:
        info->dxgi_format = DXGI_FORMAT_R8_UNORM;
        info->upload_bytes_per_pixel = 1;
        return true;

    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_Y16:
        info->dxgi_format = DXGI_FORMAT_R16_UNORM;
        info->upload_bytes_per_pixel = 2;
        return true;

    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_DEPTH_Y16_FIXED:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_Y16_FIXED:
        info->dxgi_format = DXGI_FORMAT_R16_UNORM;
        info->upload_bytes_per_pixel = 2;
        return true;

    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_Y16_FLOAT:
        /* Guest texels are 16-bit floats, decoded to normalized depth; the
         * shadow shader rescales to float-depth units. A live Z16 surface
         * is copied as normalized 16-bit depth instead
         * (texture_format_use_surface_resource). */
        info->dxgi_format = DXGI_FORMAT_R32_FLOAT;
        info->upload_bytes_per_pixel = 4;
        info->converted = true;
        return true;

    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_X8_Y24_FIXED:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_X8_Y24_FLOAT:
        info->dxgi_format = DXGI_FORMAT_R32_FLOAT;
        info->upload_bytes_per_pixel = 4;
        info->converted = true;
        return true;

    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8Y8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8Y8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_G8B8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_G8B8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R8B8:
        info->dxgi_format = DXGI_FORMAT_R8G8_UNORM;
        info->upload_bytes_per_pixel = 2;
        return true;

    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R6G5B5:
        /* the renderer's own convert_r6g5b5_to_r8g8b8a8_snorm, not the shared conversion */
        info->dxgi_format = DXGI_FORMAT_R8G8B8A8_SNORM;
        info->upload_bytes_per_pixel = 4;
        info->converted = true;
        return true;

    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A1R5G5B5:
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_X1R5G5B5:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A1R5G5B5:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X1R5G5B5:
        info->dxgi_format = DXGI_FORMAT_B5G5R5A1_UNORM;
        info->upload_bytes_per_pixel = 2;
        return true;

    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R5G6B5:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_R5G6B5:
        info->dxgi_format = DXGI_FORMAT_B5G6R5_UNORM;
        info->upload_bytes_per_pixel = 2;
        return true;

    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A4R4G4B4:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A4R4G4B4:
        info->dxgi_format = DXGI_FORMAT_B4G4R4A4_UNORM;
        info->upload_bytes_per_pixel = 2;
        return true;

    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8R8G8B8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_X8R8G8B8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8R8G8B8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X8R8G8B8:
        info->dxgi_format = DXGI_FORMAT_B8G8R8A8_UNORM;
        info->upload_bytes_per_pixel = 4;
        return true;

    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8B8G8R8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_B8G8R8A8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R8G8B8A8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8B8G8R8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_B8G8R8A8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_R8G8B8A8:
        info->dxgi_format = DXGI_FORMAT_R8G8B8A8_UNORM;
        info->upload_bytes_per_pixel = 4;
        return true;

    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_I8_A8R8G8B8:
        /* our palette conversion writes R,G,B,A (texsigns_convert_i8_argb8888); BGRA here swapped red and blue */
        info->dxgi_format = DXGI_FORMAT_R8G8B8A8_UNORM;
        info->upload_bytes_per_pixel = 4;
        info->converted = true;
        return true;

    case NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5:
    case NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT23_A8R8G8B8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT45_A8R8G8B8:
        /* Guest DXT blocks are the DXGI BC layout, so they upload as-is.
         * Block formats need a block-aligned base level; smaller textures
         * take the CPU decode path, as do volumes, whose blocks NV2A
         * interleaves across groups of four slices. */
        if (shape->width >= 4 && shape->height >= 4 &&
            shape->dimensionality != 3) {
            info->dxgi_format =
                shape->color_format ==
                        NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5 ?
                    DXGI_FORMAT_BC1_UNORM :
                shape->color_format ==
                        NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT23_A8R8G8B8 ?
                    DXGI_FORMAT_BC2_UNORM :
                    DXGI_FORMAT_BC3_UNORM;
            info->upload_bytes_per_pixel = 0;
            info->converted = false;
            info->compressed = true;
            return true;
        }
        info->dxgi_format = DXGI_FORMAT_R8G8B8A8_UNORM;
        info->upload_bytes_per_pixel = 4;
        info->converted = true;
        info->compressed = true;
        return true;

    case NV097_SET_TEXTURE_FORMAT_COLOR_LC_IMAGE_CR8YB8CB8YA8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LC_IMAGE_YB8CR8YA8CB8:
        info->dxgi_format = DXGI_FORMAT_R8G8B8A8_UNORM;
        info->upload_bytes_per_pixel = 4;
        info->converted = true;
        return true;

    default:
        return false;
    }
}

static enum S3TC_DECOMPRESS_FORMAT texture_s3tc_format(
    unsigned int color_format)
{
    switch (color_format) {
    case NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5:
        return S3TC_DECOMPRESS_FORMAT_DXT1;
    case NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT23_A8R8G8B8:
        return S3TC_DECOMPRESS_FORMAT_DXT3;
    case NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT45_A8R8G8B8:
        return S3TC_DECOMPRESS_FORMAT_DXT5;
    default:
        g_assert_not_reached();
        return S3TC_DECOMPRESS_FORMAT_DXT1;
    }
}

static size_t texture_compressed_level_source_size(unsigned int color_format,
                                                  unsigned int width,
                                                  unsigned int height,
                                                  unsigned int depth)
{
    unsigned int block_size =
        color_format == NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5 ?
            8 : 16;
    unsigned int physical_width = (width + 3) & ~3;
    unsigned int physical_height = (height + 3) & ~3;

    return (size_t)(physical_width / 4) * (physical_height / 4) *
           MAX(depth, 1) * block_size;
}

static unsigned int texture_mip_levels_for_shape(const TextureShape *shape)
{
    BasicColorFormatInfo basic =
        kelvin_color_format_info_map[shape->color_format];

    return basic.linear ? 1 : MAX(shape->levels, 1);
}

/*
 * MIN_LOD_CLAMP is the base level: LOD is computed relative to it and the
 * finer levels are never sampled, also by the LOD0 min filters.
 */
static unsigned int texture_base_level(const TextureShape *shape)
{
    BasicColorFormatInfo basic =
        kelvin_color_format_info_map[shape->color_format];

    return basic.linear ? 0 :
        MIN(shape->min_mipmap_level, texture_mip_levels_for_shape(shape) - 1);
}

/* Levels from the base level down, as seen through the SRV. */
static unsigned int texture_view_levels(const TextureShape *shape)
{
    return texture_mip_levels_for_shape(shape) - texture_base_level(shape);
}

static size_t texture_face_source_size(PGRAPHState *pg,
                                       const TextureShape *shape)
{
    BasicColorFormatInfo basic =
        kelvin_color_format_info_map[shape->color_format];
    bool compressed =
        pgraph_is_texture_format_compressed(pg, shape->color_format);
    unsigned int mip_levels = texture_mip_levels_for_shape(shape);
    unsigned int width = MAX(shape->width, 1);
    unsigned int height = MAX(shape->height, 1);
    size_t length = 0;

    if (shape->border && !basic.linear) {
        width = MAX(16, width * 2);
        height = MAX(16, height * 2);
    }

    for (unsigned int level = 0; level < mip_levels; level++) {
        if (compressed) {
            length += texture_compressed_level_source_size(
                shape->color_format, width, height, 1);
        } else {
            length += (size_t)width * height * basic.bytes_per_pixel;
        }
        width = MAX(width / 2, 1);
        height = MAX(height / 2, 1);
    }

    if (shape->cubemap) {
        length = ROUND_UP(length, NV2A_CUBEMAP_FACE_ALIGNMENT);
    }
    return length;
}

static DXGI_FORMAT texture_resource_format(
    const PGRAPHD3D12TextureFormatInfo *info)
{
    return info->resource_format != DXGI_FORMAT_UNKNOWN ?
        info->resource_format : info->dxgi_format;
}

static void texture_level_dimensions(const TextureShape *shape,
                                     unsigned int level,
                                     unsigned int *width,
                                     unsigned int *height,
                                     unsigned int *depth)
{
    BasicColorFormatInfo basic =
        kelvin_color_format_info_map[shape->color_format];

    *width = MAX(shape->width, 1);
    *height = MAX(shape->height, 1);
    *depth = shape->dimensionality == 3 ? MAX(shape->depth, 1) : 1;
    if (shape->border && !basic.linear) {
        *width = MAX(16, *width * 2);
        *height = MAX(16, *height * 2);
        if (shape->dimensionality == 3) {
            *depth = MAX(16, *depth * 2);
        }
    }

    while (level-- > 0) {
        *width = MAX(*width / 2, 1);
        *height = MAX(*height / 2, 1);
        if (shape->dimensionality == 3) {
            *depth = MAX(*depth / 2, 1);
        }
    }
}

static size_t texture_source_length(PGRAPHState *pg, const TextureShape *shape)
{
    BasicColorFormatInfo basic =
        kelvin_color_format_info_map[shape->color_format];
    bool compressed =
        pgraph_is_texture_format_compressed(pg, shape->color_format);
    unsigned int mip_levels = texture_mip_levels_for_shape(shape);
    size_t length = 0;

    if (basic.linear) {
        return (size_t)shape->height * shape->pitch;
    }

    for (unsigned int level = 0; level < mip_levels; level++) {
        unsigned int width;
        unsigned int height;
        unsigned int depth;

        texture_level_dimensions(shape, level, &width, &height, &depth);
        if (compressed) {
            length += texture_compressed_level_source_size(
                shape->color_format, width, height, depth);
        } else {
            length += (size_t)width * height * depth *
                      basic.bytes_per_pixel;
        }
    }

    if (shape->cubemap) {
        length = ROUND_UP(length, NV2A_CUBEMAP_FACE_ALIGNMENT) * 6;
    }
    return length;
}

static bool allocate_texture_descriptors(PGRAPHD3D12State *r,
                                         PGRAPHD3D12TextureBinding *binding,
                                         Error **errp)
{
    if (binding->descriptors_allocated) {
        return true;
    }

    if (!pgraph_d3d12_alloc_srv_descriptor(r, &binding->srv_descriptor_index,
                                           errp)) {
        return false;
    }

    binding->srv_cpu = cpu_descriptor_handle(
        r->cbv_srv_uav_heap, r->cbv_srv_uav_descriptor_size,
        binding->srv_descriptor_index);
    binding->srv_gpu = gpu_descriptor_handle(
        r->cbv_srv_uav_heap, r->cbv_srv_uav_descriptor_size,
        binding->srv_descriptor_index);
    binding->descriptors_allocated = true;
    return true;
}

/*
 * Texture cache adapter budget. The cache shares the 512 MiB console adapter
 * with render targets, the surface pool and the stash; least recently used
 * entries are released before a new resource would exceed it.
 */
#define TEXTURE_CACHE_BUDGET_BYTES (192ull * 1024 * 1024)

/* Hands a binding's resource and SRV slot to the deferred-release queue. */
static void release_texture_resource(PGRAPHState *pg,
                                     PGRAPHD3D12TextureBinding *binding)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;

    if (binding->resource != NULL || binding->descriptors_allocated) {
        pgraph_d3d12_defer_release(
            pg, binding->resource,
            binding->descriptors_allocated ?
                (int32_t)binding->srv_descriptor_index : -1,
            -1, -1, -1);
    }
    r->texture_cache_bytes -= MIN(r->texture_cache_bytes,
                                  binding->resource_bytes);
    binding->resource_bytes = 0;
    binding->resource = NULL;
    binding->descriptors_allocated = false;
    binding->valid = false;
}

static bool texture_cache_entry_pre_evict(Lru *lru, LruNode *node);

/*
 * Evicts least recently used entries, never a bound texture or `keep`,
 * until `needed` more bytes fit the budget or `count` entries are gone.
 * Returns the number of entries evicted.
 */
static unsigned int trim_texture_cache(PGRAPHD3D12State *r,
                                       PGRAPHD3D12TextureBinding *keep,
                                       uint64_t needed, unsigned int count)
{
    Lru *lru = &r->texture_cache;
    LruNode *node, *prev;
    unsigned int evicted = 0;

    QTAILQ_FOREACH_REVERSE_SAFE(node, &lru->global, next_global, prev) {
        /* needed may be UINT64_MAX (out of memory: evict up to count whatever the budget): no wrapping sum */
        if (evicted >= count ||
            (needed <= TEXTURE_CACHE_BUDGET_BYTES &&
             r->texture_cache_bytes <= TEXTURE_CACHE_BUDGET_BYTES - needed)) {
            break;
        }
        if (node == &keep->node || !lru_is_node_in_use(lru, node) ||
            !texture_cache_entry_pre_evict(lru, node)) {
            continue;
        }
        lru_evict_node(lru, node);
        evicted++;
    }
    return evicted;
}

static bool create_texture_resource(PGRAPHState *pg, unsigned int stage,
                                    PGRAPHD3D12TextureBinding *binding,
                                    const TextureShape *shape,
                                    hwaddr vram_addr, size_t length,
                                    const PGRAPHD3D12TextureFormatInfo *info,
                                    unsigned int resource_scale,
                                    bool render_target, Error **errp)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    D3D12_HEAP_PROPERTIES heap_props;
    D3D12_RESOURCE_DESC desc;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc;
    unsigned int mip_levels = texture_mip_levels_for_shape(shape);
    unsigned int base_level = texture_base_level(shape);
    unsigned int view_levels = texture_view_levels(shape);
    unsigned int resource_width;
    unsigned int resource_height;
    unsigned int resource_depth;
    HRESULT hr;
    char device_removed_detail[128];

    if (binding->resource != NULL) {
        /* Retire the old resource and SRV slot after in-flight commands. */
        release_texture_resource(pg, binding);
    }
    binding->valid = false;

    if (!allocate_texture_descriptors(r, binding, errp)) {
        return false;
    }

    memset(&heap_props, 0, sizeof(heap_props));
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap_props.CreationNodeMask = 1;
    heap_props.VisibleNodeMask = 1;

    texture_level_dimensions(shape, 0, &resource_width, &resource_height,
                             &resource_depth);
    if (resource_scale > 1) {
        resource_width *= resource_scale;
        resource_height *= resource_scale;
    }
    if (binding->replacement != NULL) {
        /* A custom texture keeps its own size, levels and format; the
         * shaders still sample in the guest texture's coordinates. */
        resource_width = binding->replacement->width;
        resource_height = binding->replacement->height;
        mip_levels = binding->replacement->mip_levels;
        base_level = MIN(base_level, mip_levels - 1);
        view_levels = mip_levels - base_level;
    }

    memset(&desc, 0, sizeof(desc));
    desc.Dimension = shape->dimensionality == 3 ?
        D3D12_RESOURCE_DIMENSION_TEXTURE3D :
        D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = resource_width;
    desc.Height = resource_height;
    desc.DepthOrArraySize =
        shape->dimensionality == 3 ? resource_depth :
        (shape->cubemap ? 6 : 1);
    desc.MipLevels = (UINT16)mip_levels;
    desc.Format = binding->replacement != NULL ?
        binding->replacement->dxgi_format : texture_resource_format(info);
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = render_target ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET :
                                 D3D12_RESOURCE_FLAG_NONE;

    {
        D3D12_RESOURCE_ALLOCATION_INFO alloc_info;

        r->device->lpVtbl->GetResourceAllocationInfo(r->device, &alloc_info,
                                                     0, 1, &desc);
        binding->resource_bytes = alloc_info.SizeInBytes;
    }
    trim_texture_cache(r, binding, binding->resource_bytes, UINT_MAX);

    binding->state = D3D12_RESOURCE_STATE_COMMON;
    hr = r->device->lpVtbl->CreateCommittedResource(
        r->device, &heap_props, D3D12_HEAP_FLAG_NONE, &desc, binding->state,
        NULL, &IID_ID3D12Resource, (void **)&binding->resource);
    if (hr == E_OUTOFMEMORY &&
        trim_texture_cache(r, binding, UINT64_MAX,
                           MAX(r->texture_cache.num_used / 4, 1u)) != 0) {
        /* Released memory returns once the queue is past its last use. */
        r->perf.finish_texture_oom++;
        pgraph_d3d12_finish(pg);
        hr = r->device->lpVtbl->CreateCommittedResource(
            r->device, &heap_props, D3D12_HEAP_FLAG_NONE, &desc,
            binding->state, NULL, &IID_ID3D12Resource,
            (void **)&binding->resource);
    }
    if (FAILED(hr)) {
        binding->resource = NULL;
        binding->resource_bytes = 0;
        error_setg(errp,
                   "Failed to create Direct3D 12 stage %u texture resource: "
                   "HRESULT %s (0x%08lx)%s, format=%s (0x%x), resource_dxgi=%u, "
                   "srv_dxgi=%u, "
                   "size=%ux%ux%u, dimensionality=%u, mips=%u, cubemap=%d, "
                   "border=%d, resource_scale=%u, "
                   "resource_size=%ux%ux%u, depth_or_array=%u, "
                   "vram=0x%" HWADDR_PRIx ", length=%zu",
                   stage, pgraph_d3d12_hresult_name(hr), (unsigned long)hr,
                   pgraph_d3d12_device_removed_detail(
                       r, hr, device_removed_detail,
                       sizeof(device_removed_detail)),
                   texture_color_format_name(shape->color_format),
                    shape->color_format, (unsigned int)desc.Format,
                    (unsigned int)info->dxgi_format,
                   shape->width, shape->height, shape->depth,
                   shape->dimensionality, mip_levels, shape->cubemap,
                   shape->border, resource_scale, resource_width, resource_height,
                   resource_depth, (unsigned int)desc.DepthOrArraySize,
                   vram_addr, length);
        return false;
    }

    /* The view starts at the base level, so sampler LOD 0 and the shader's
     * GetDimensions both refer to it. */
    memset(&srv_desc, 0, sizeof(srv_desc));
    srv_desc.Format = binding->replacement != NULL ?
        binding->replacement->dxgi_format : info->dxgi_format;
    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (shape->dimensionality == 3) {
        srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        srv_desc.Texture3D.MostDetailedMip = base_level;
        srv_desc.Texture3D.MipLevels = view_levels;
        srv_desc.Texture3D.ResourceMinLODClamp = 0.0f;
    } else if (shape->cubemap && shape->border) {
        srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        srv_desc.Texture2DArray.MostDetailedMip = base_level;
        srv_desc.Texture2DArray.MipLevels = view_levels;
        srv_desc.Texture2DArray.FirstArraySlice = 0;
        srv_desc.Texture2DArray.ArraySize = 6;
        srv_desc.Texture2DArray.PlaneSlice = 0;
        srv_desc.Texture2DArray.ResourceMinLODClamp = 0.0f;
    } else if (shape->cubemap) {
        srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        srv_desc.TextureCube.MostDetailedMip = base_level;
        srv_desc.TextureCube.MipLevels = view_levels;
        srv_desc.TextureCube.ResourceMinLODClamp = 0.0f;
    } else {
        srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Texture2D.MostDetailedMip = base_level;
        srv_desc.Texture2D.MipLevels = view_levels;
        srv_desc.Texture2D.PlaneSlice = 0;
        srv_desc.Texture2D.ResourceMinLODClamp = 0.0f;
    }
    r->device->lpVtbl->CreateShaderResourceView(
        r->device, binding->resource, &srv_desc, binding->srv_cpu);

    r->texture_cache_bytes += binding->resource_bytes;
    binding->shape = *shape;
    binding->vram_addr = vram_addr;
    binding->length = length;
    binding->resource_format = desc.Format;
    binding->dxgi_format = info->dxgi_format;
    binding->upload_bytes_per_pixel = info->upload_bytes_per_pixel;
    binding->converted = info->converted;
    binding->compressed = info->compressed;
    binding->scale = (float)MAX(resource_scale, 1);
    binding->valid = true;
    return true;
}

static void copy_tight_slices(uint8_t *dst, const uint8_t *src,
                              uint32_t src_pitch, uint32_t src_slice_pitch,
                              unsigned int width, unsigned int height,
                              unsigned int depth,
                              unsigned int bytes_per_pixel)
{
    uint32_t row_bytes = width * bytes_per_pixel;
    uint32_t dst_slice_pitch = row_bytes * height;

    if (src_slice_pitch == 0) {
        src_slice_pitch = src_pitch * height;
    }

    for (unsigned int z = 0; z < depth; z++) {
        for (unsigned int y = 0; y < height; y++) {
            memcpy(dst + (size_t)z * dst_slice_pitch + y * row_bytes,
                   src + (size_t)z * src_slice_pitch + y * src_pitch,
                   row_bytes);
        }
    }
}

static uint8_t *convert_r6g5b5_to_r8g8b8a8_snorm(
    const uint8_t *data, unsigned int width, unsigned int height,
    unsigned int depth, unsigned int row_pitch, unsigned int slice_pitch,
    size_t *converted_size)
{
    size_t size = (size_t)width * height * depth * 4;
    uint8_t *converted_data = g_malloc(size);

    for (unsigned int z = 0; z < depth; z++) {
        const uint8_t *slice = data + (size_t)z * slice_pitch;

        for (unsigned int y = 0; y < height; y++) {
            for (unsigned int x = 0; x < width; x++) {
                uint16_t rgb655;
                int8_t *pixel = (int8_t *)&converted_data[
                    (((size_t)z * height + y) * width + x) * 4];

                memcpy(&rgb655, slice + (size_t)y * row_pitch + x * 2,
                       sizeof(rgb655));
                rgb655 ^= (1 << 9) | (1 << 4);
                pixel[0] = ((rgb655 & 0xFC00) >> 10) * 0x7F / 0x3F;
                pixel[1] = ((rgb655 & 0x03E0) >> 5) * 0xFF / 0x1F - 0x80;
                pixel[2] = (rgb655 & 0x001F) * 0xFF / 0x1F - 0x80;
                pixel[3] = 0x7F;
            }
        }
    }

    if (converted_size != NULL) {
        *converted_size = size;
    }
    return converted_data;
}

static uint8_t *convert_x8y24_to_r32_float(
    const uint8_t *data, unsigned int width, unsigned int height,
    unsigned int depth, unsigned int row_pitch, unsigned int slice_pitch,
    size_t *converted_size)
{
    size_t size = (size_t)width * height * depth * sizeof(float);
    uint8_t *converted_data = g_malloc(size);

    for (unsigned int z = 0; z < depth; z++) {
        const uint8_t *slice = data + (size_t)z * slice_pitch;

        for (unsigned int y = 0; y < height; y++) {
            for (unsigned int x = 0; x < width; x++) {
                uint32_t raw;
                float depth_value;

                memcpy(&raw, slice + (size_t)y * row_pitch + x * sizeof(raw),
                       sizeof(raw));
                depth_value = (float)(raw >> 8) / 16777215.0f;
                memcpy(converted_data +
                           (((size_t)z * height + y) * width + x) *
                               sizeof(depth_value),
                       &depth_value, sizeof(depth_value));
            }
        }
    }

    if (converted_size != NULL) {
        *converted_size = size;
    }
    return converted_data;
}

/* IEEE 754 binary16 to binary32. */
static float half_to_float(uint16_t half)
{
    uint32_t sign = (uint32_t)(half & 0x8000) << 16;
    uint32_t exponent = (half >> 10) & 0x1f;
    uint32_t mantissa = half & 0x3ff;
    uint32_t bits;
    float value;

    if (exponent == 0) {
        /* Zero or subnormal: mantissa * 2^-24 is exact. */
        value = (float)mantissa * (1.0f / 16777216.0f);
        return sign ? -value : value;
    }

    bits = sign | (mantissa << 13) |
           (exponent == 0x1f ? 0xffu << 23 : (exponent + 112) << 23);
    memcpy(&value, &bits, sizeof(value));
    return value;
}

/* Y16 float depth texels: clamped to [0, 1] at 16-bit depth precision. */
static uint8_t *convert_y16_float_to_r32_float(
    const uint8_t *data, unsigned int width, unsigned int height,
    unsigned int depth, unsigned int row_pitch, unsigned int slice_pitch,
    size_t *converted_size)
{
    size_t size = (size_t)width * height * depth * sizeof(float);
    uint8_t *converted_data = g_malloc(size);

    for (unsigned int z = 0; z < depth; z++) {
        const uint8_t *slice = data + (size_t)z * slice_pitch;

        for (unsigned int y = 0; y < height; y++) {
            for (unsigned int x = 0; x < width; x++) {
                uint16_t raw;
                float depth_value;

                memcpy(&raw, slice + (size_t)y * row_pitch + x * sizeof(raw),
                       sizeof(raw));
                depth_value = half_to_float(raw);
                if (!(depth_value > 0.0f)) {
                    depth_value = 0.0f;
                } else if (depth_value > 1.0f) {
                    depth_value = 1.0f;
                }
                depth_value =
                    (float)(uint32_t)(depth_value * 65535.0f + 0.5f) /
                    65535.0f;
                memcpy(converted_data +
                           (((size_t)z * height + y) * width + x) *
                               sizeof(depth_value),
                       &depth_value, sizeof(depth_value));
            }
        }
    }

    if (converted_size != NULL) {
        *converted_size = size;
    }
    return converted_data;
}

static unsigned int dxgi_block_bytes(DXGI_FORMAT format)
{
    switch (format) {
    case DXGI_FORMAT_BC1_UNORM:
        return 8;
    case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC3_UNORM:
        return 16;
    default:
        return 0;
    }
}

static uint8_t *decode_texture_level(
    NV2AState *d, unsigned int stage, const TextureShape *shape,
    const PGRAPHD3D12TextureFormatInfo *info, hwaddr vram_addr,
    unsigned int level, unsigned int width, unsigned int height,
    unsigned int depth, uint32_t *row_pitch_out,
    uint32_t *slice_pitch_out, size_t *decoded_size_out, Error **errp)
{
    PGRAPHState *pg = &d->pgraph;
    BasicColorFormatInfo basic =
        kelvin_color_format_info_map[shape->color_format];
    const uint8_t *src = d->vram_ptr + vram_addr;
    uint32_t source_row_pitch = basic.linear ?
        shape->pitch : width * basic.bytes_per_pixel;
    uint32_t source_slice_pitch = source_row_pitch * height;
    uint32_t tight_source_pitch = width * basic.bytes_per_pixel;
    uint32_t tight_source_slice_pitch = tight_source_pitch * height;
    size_t tight_source_size = (size_t)tight_source_slice_pitch * depth;
    g_autofree uint8_t *unswizzled = NULL;
    const uint8_t *linear_src = src;
    uint8_t *decoded;

    if (info->compressed) {
        unsigned int block_bytes = dxgi_block_bytes(info->dxgi_format);

        if (block_bytes != 0) {
            size_t source_size = texture_compressed_level_source_size(
                shape->color_format, width, height, depth);

            *row_pitch_out = ((width + 3) / 4) * block_bytes;
            *slice_pitch_out = *row_pitch_out * ((height + 3) / 4);
            *decoded_size_out = source_size;
            return g_memdup2(src, source_size);
        }
        decoded = s3tc_decompress_3d(texture_s3tc_format(shape->color_format),
                                     src, width, height, depth);
        if (decoded == NULL) {
            error_setg(errp,
                       "Direct3D 12 stage %u texture mip %u S3TC decode "
                       "returned NULL for format=%s (0x%x), "
                       "mip_size=%ux%ux%u, "
                       "source_bytes=%zu, texture_vram=0x%" HWADDR_PRIx,
                       stage, level,
                       texture_color_format_name(shape->color_format),
                       shape->color_format, width, height, depth,
                       texture_compressed_level_source_size(
                           shape->color_format, width, height, depth),
                       vram_addr);
            return NULL;
        }
        *row_pitch_out = width * info->upload_bytes_per_pixel;
        *slice_pitch_out = *row_pitch_out * height;
        *decoded_size_out = (size_t)(*slice_pitch_out) * depth;
        return decoded;
    }

    if (!basic.linear) {
        unswizzled = g_malloc(tight_source_size);
        unswizzle_box(src, width, height, depth, unswizzled,
                      tight_source_pitch, tight_source_slice_pitch,
                      basic.bytes_per_pixel);
        linear_src = unswizzled;
        source_row_pitch = tight_source_pitch;
        source_slice_pitch = tight_source_slice_pitch;
    }

    if (info->converted) {
        const uint8_t *palette_data = NULL;
        size_t palette_length = 0;

        if (shape->color_format ==
                NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_X8_Y24_FIXED ||
            shape->color_format ==
                NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_X8_Y24_FLOAT) {
            *row_pitch_out = width * info->upload_bytes_per_pixel;
            *slice_pitch_out = *row_pitch_out * height;
            return convert_x8y24_to_r32_float(
                linear_src, width, height, depth, source_row_pitch,
                source_slice_pitch,
                decoded_size_out);
        }

        if (shape->color_format ==
            NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_Y16_FLOAT) {
            *row_pitch_out = width * info->upload_bytes_per_pixel;
            *slice_pitch_out = *row_pitch_out * height;
            return convert_y16_float_to_r32_float(
                linear_src, width, height, depth, source_row_pitch,
                source_slice_pitch, decoded_size_out);
        }

        if (shape->color_format == NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R6G5B5) {
            *row_pitch_out = width * info->upload_bytes_per_pixel;
            *slice_pitch_out = *row_pitch_out * height;
            return convert_r6g5b5_to_r8g8b8a8_snorm(
                linear_src, width, height, depth, source_row_pitch,
                source_slice_pitch,
                decoded_size_out);
        }

        if (shape->color_format ==
            NV097_SET_TEXTURE_FORMAT_COLOR_SZ_I8_A8R8G8B8) {
            hwaddr palette_addr = pgraph_get_texture_palette_phys_addr_length(
                pg, stage, &palette_length);
            uint64_t vram_size = memory_region_size(d->vram);

            if (palette_length > vram_size ||
                palette_addr > vram_size - palette_length) {
                error_setg(errp,
                           "Direct3D 12 stage %u texture mip %u indexed "
                           "palette is outside VRAM: palette=0x%"
                           HWADDR_PRIx ", length=%zu, vram_size=0x%"
                           PRIx64 ", format=%s (0x%x), "
                           "mip_size=%ux%ux%u, "
                           "texture_vram=0x%" HWADDR_PRIx,
                           stage, level, palette_addr, palette_length,
                           vram_size,
                           texture_color_format_name(shape->color_format),
                           shape->color_format, width, height, depth,
                           vram_addr);
                return NULL;
            }
            palette_data = d->vram_ptr + palette_addr;
        }

        decoded = pgraph_convert_texture_data(
            *shape, linear_src, palette_data, width, height, depth,
            source_row_pitch, source_slice_pitch, decoded_size_out);
        if (decoded != NULL &&
            (shape->color_format == NV097_SET_TEXTURE_FORMAT_COLOR_LC_IMAGE_CR8YB8CB8YA8 ||
             shape->color_format == NV097_SET_TEXTURE_FORMAT_COLOR_LC_IMAGE_YB8CR8YA8CB8)) {
            /* our conversion leaves video pixels as Y, Cb, Cr (the OpenGL renderer turns them into colours in
             * its shader, glsl/psh.c yuv_to_rgb); same formula here, once per upload */
            uint8_t *px = decoded;
            for (size_t n = (size_t)width * height * depth; n > 0; n--, px += 4) {
                int c = px[0] - 16, dcb = px[1] - 128, ecr = px[2] - 128;
                px[0] = cliptobyte((298 * c + 409 * ecr + 128) >> 8);
                px[1] = cliptobyte((298 * c - 100 * dcb - 208 * ecr + 128) >> 8);
                px[2] = cliptobyte((298 * c + 516 * dcb + 128) >> 8);
            }
        }
        if (decoded == NULL) {
            error_setg(errp,
                       "Direct3D 12 stage %u texture mip %u conversion "
                       "returned NULL for format=%s (0x%x), "
                       "mip_size=%ux%ux%u, source_pitch=%u, "
                       "source_slice_pitch=%u, texture_vram=0x%" HWADDR_PRIx,
                       stage, level,
                       texture_color_format_name(shape->color_format),
                       shape->color_format, width, height, depth,
                       source_row_pitch, source_slice_pitch,
                       vram_addr);
            return NULL;
        }
        *row_pitch_out = width * info->upload_bytes_per_pixel;
        *slice_pitch_out = *row_pitch_out * height;
        return decoded;
    }

    *row_pitch_out = width * info->upload_bytes_per_pixel;
    *slice_pitch_out = *row_pitch_out * height;
    *decoded_size_out = (size_t)(*slice_pitch_out) * depth;
    if (unswizzled != NULL && source_row_pitch == *row_pitch_out) {
        /* The unswizzled copy is already tightly packed. */
        return g_steal_pointer(&unswizzled);
    }
    decoded = g_malloc(*decoded_size_out);
    copy_tight_slices(decoded, linear_src, source_row_pitch,
                      source_slice_pitch, width, height, depth,
                      info->upload_bytes_per_pixel);
    return decoded;
}

static void store_le16(uint8_t *dst, uint16_t value)
{
    dst[0] = value & 0xff;
    dst[1] = value >> 8;
}

static void store_le32(uint8_t *dst, uint32_t value)
{
    dst[0] = value & 0xff;
    dst[1] = (value >> 8) & 0xff;
    dst[2] = (value >> 16) & 0xff;
    dst[3] = value >> 24;
}

static void decoded_texture_rgb(DXGI_FORMAT format, const uint8_t *pixel,
                                uint8_t *r, uint8_t *g, uint8_t *b)
{
    switch (format) {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
        *b = pixel[0];
        *g = pixel[1];
        *r = pixel[2];
        break;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
        *r = pixel[0];
        *g = pixel[1];
        *b = pixel[2];
        break;
    case DXGI_FORMAT_R8G8_UNORM:
        *r = pixel[0];
        *g = pixel[1];
        *b = pixel[0];
        break;
    case DXGI_FORMAT_R8_UNORM:
        *r = pixel[0];
        *g = pixel[0];
        *b = pixel[0];
        break;
    default:
        *r = pixel[0];
        *g = pixel[0];
        *b = pixel[0];
        break;
    }
}

static bool texture_upload_signature_seen(uint64_t signature)
{
    static uint64_t signatures[D3D12_REPORTED_TEXTURE_UPLOADS];
    static unsigned int num_signatures;

    for (unsigned int i = 0; i < num_signatures; i++) {
        if (signatures[i] == signature) {
            return true;
        }
    }
    if (num_signatures < ARRAY_SIZE(signatures)) {
        signatures[num_signatures++] = signature;
    }
    return false;
}

static bool surface_texture_copy_signature_seen(uint64_t signature)
{
    static uint64_t signatures[D3D12_REPORTED_TEXTURE_UPLOADS];
    static unsigned int num_signatures;

    for (unsigned int i = 0; i < num_signatures; i++) {
        if (signatures[i] == signature) {
            return true;
        }
    }
    if (num_signatures < ARRAY_SIZE(signatures)) {
        signatures[num_signatures++] = signature;
    }
    return false;
}

static bool write_decoded_texture_bmp(
    const char *path, DXGI_FORMAT format, const uint8_t *decoded,
    unsigned int width, unsigned int height, unsigned int row_pitch,
    unsigned int bytes_per_pixel)
{
    uint32_t bmp_row_pitch = ROUND_UP(width * 3, 4);
    uint32_t image_size = bmp_row_pitch * height;
    uint8_t header[54];
    g_autofree uint8_t *row = g_malloc0(bmp_row_pitch);
    FILE *fp;

    memset(header, 0, sizeof(header));
    header[0] = 'B';
    header[1] = 'M';
    store_le32(&header[2], sizeof(header) + image_size);
    store_le32(&header[10], sizeof(header));
    store_le32(&header[14], 40);
    store_le32(&header[18], width);
    store_le32(&header[22], (uint32_t)(-(int32_t)height));
    store_le16(&header[26], 1);
    store_le16(&header[28], 24);
    store_le32(&header[34], image_size);

    fp = fopen(path, "wb");
    if (fp == NULL) {
        return false;
    }
    if (fwrite(header, sizeof(header), 1, fp) != 1) {
        fclose(fp);
        return false;
    }

    for (unsigned int y = 0; y < height; y++) {
        const uint8_t *src = decoded + (size_t)y * row_pitch;

        memset(row, 0, bmp_row_pitch);
        for (unsigned int x = 0; x < width; x++) {
            uint8_t r, g, b;

            decoded_texture_rgb(format, src + (size_t)x * bytes_per_pixel,
                                &r, &g, &b);
            row[x * 3 + 0] = b;
            row[x * 3 + 1] = g;
            row[x * 3 + 2] = r;
        }
        if (fwrite(row, bmp_row_pitch, 1, fp) != 1) {
            fclose(fp);
            return false;
        }
    }

    fclose(fp);
    return true;
}

static void report_texture_upload_sample(
    unsigned int stage, const PGRAPHD3D12TextureBinding *binding,
    const PGRAPHD3D12TextureUploadLevel *upload_level,
    const uint8_t *decoded, size_t decoded_size)
{
    BasicColorFormatInfo basic =
        kelvin_color_format_info_map[binding->shape.color_format];
    uint64_t signature;
    uint64_t total_nonzero_rgb = 0;
    uint64_t total_greenish_rgb = 0;
    uint64_t total_rgb_sum = 0;
    uint64_t total_alpha_sum = 0;
    uint64_t fnv = 1469598103934665603ull;
    uint32_t min16 = UINT32_MAX;
    uint32_t max16 = 0;
    uint64_t nonzero16 = 0;
    uint64_t sum16 = 0;
    uint8_t max_r = 0;
    uint8_t max_g = 0;
    uint8_t max_b = 0;
    uint8_t max_a = 0;
    g_autoptr(GString) rows = g_string_new(NULL);
    g_autofree char *dump_dir = NULL;
    g_autofree char *dump_name = NULL;
    g_autofree char *dump_path = NULL;
    const char *local_app_data;
    const char *prefix = "Direct3D 12 renderer";
    bool wrote_dump = false;
    bool boot_trace_report = false;

#if defined(XEMU_UWP) && defined(XEMU_D3D12_UWP_BOOT_TRACE)
    boot_trace_report =
        stage < NV2A_MAX_TEXTURES &&
        upload_level->level == 0 &&
        upload_level->depth == 1 &&
        upload_level->width <= 512 &&
        upload_level->height <= 512 &&
        binding->upload_bytes_per_pixel > 0 &&
        binding->upload_bytes_per_pixel <= 4;
    if (boot_trace_report) {
        prefix = "Direct3D 12 UWP boot";
    }
#endif

    if (!boot_trace_report &&
        (stage != 0 || upload_level->face != 0 ||
         upload_level->level != 0 || upload_level->depth != 1 ||
         !basic.linear || upload_level->width > 512 ||
         upload_level->height > 128 ||
         binding->upload_bytes_per_pixel > 4)) {
        return;
    }

    signature = ((uint64_t)stage << 60) ^
                ((uint64_t)upload_level->face << 56) ^
                ((uint64_t)upload_level->level << 52) ^
                ((uint64_t)binding->shape.color_format << 44) ^
                ((uint64_t)upload_level->width << 28) ^
                ((uint64_t)upload_level->height << 12) ^
                (uint64_t)binding->vram_addr;
    if (texture_upload_signature_seen(signature)) {
        return;
    }

    for (unsigned int y = 0; y < upload_level->height; y++) {
        const uint8_t *row =
            decoded + (size_t)y * upload_level->decoded_row_pitch;
        uint64_t row_nonzero_rgb = 0;
        uint64_t row_rgb_sum = 0;
        int first_nonzero = -1;
        int last_nonzero = -1;

        for (unsigned int x = 0; x < upload_level->width; x++) {
            const uint8_t *pixel =
                row + (size_t)x * binding->upload_bytes_per_pixel;
            uint8_t r, g, b;
            uint8_t a = binding->upload_bytes_per_pixel >= 4 ? pixel[3] : 0xff;

            decoded_texture_rgb(binding->dxgi_format, pixel, &r, &g, &b);
            fnv ^= r;
            fnv *= 1099511628211ull;
            fnv ^= g;
            fnv *= 1099511628211ull;
            fnv ^= b;
            fnv *= 1099511628211ull;
            fnv ^= a;
            fnv *= 1099511628211ull;
            row_rgb_sum += r + g + b;
            total_alpha_sum += a;
            max_r = MAX(max_r, r);
            max_g = MAX(max_g, g);
            max_b = MAX(max_b, b);
            max_a = MAX(max_a, a);
            if (r || g || b) {
                row_nonzero_rgb++;
                if (g > r + 8 && g > b + 8 && g > 16) {
                    total_greenish_rgb++;
                }
                if (first_nonzero < 0) {
                    first_nonzero = x;
                }
                last_nonzero = x;
            }
            if (binding->upload_bytes_per_pixel >= 2) {
                uint32_t value16 = pixel[0] | ((uint32_t)pixel[1] << 8);

                min16 = MIN(min16, value16);
                max16 = MAX(max16, value16);
                sum16 += value16;
                if (value16 != 0) {
                    nonzero16++;
                }
            }
        }

        total_nonzero_rgb += row_nonzero_rgb;
        total_rgb_sum += row_rgb_sum;
        if (y < 24) {
            g_string_append_printf(
                rows,
                "%s{y=%u nonzero_rgb=%" PRIu64
                " first=%d last=%d rgb_sum=%" PRIu64 "}",
                y ? "," : "", y, row_nonzero_rgb, first_nonzero,
                last_nonzero, row_rgb_sum);
        }
    }

    local_app_data = getenv("LOCALAPPDATA");
    if (local_app_data != NULL && local_app_data[0] != '\0') {
        dump_dir = g_build_filename(local_app_data, "xemu-d3d12-test",
                                    "captures", NULL);
        if (g_mkdir_with_parents(dump_dir, 0755) == 0) {
            dump_name = g_strdup_printf(
                "d3d12-tex-stage%u-%08" PRIx64 "-%ux%u.bmp",
                stage, (uint64_t)binding->vram_addr, upload_level->width,
                upload_level->height);
            dump_path = g_build_filename(dump_dir, dump_name, NULL);
            wrote_dump = write_decoded_texture_bmp(
                dump_path, binding->dxgi_format, decoded,
                upload_level->width, upload_level->height,
                upload_level->decoded_row_pitch,
                binding->upload_bytes_per_pixel);
        }
    }

    if (binding->upload_bytes_per_pixel < 2) {
        min16 = 0;
    }

    fprintf(stderr,
            "%s: decoded stage %u texture upload sample: face=%u level=%u "
            "format=%s (0x%x), dxgi=%u, vram=0x%" HWADDR_PRIx
            "..0x%" HWADDR_PRIx ", size=%ux%ux%u, source_pitch=%u, "
            "decoded_row_pitch=%u, decoded_slice_pitch=%u, "
            "decoded_size=%zu, linear=%d, cubemap=%d, texture3d=%d, "
            "bpp=%u, nonzero_rgb=%" PRIu64 ", greenish_rgb=%" PRIu64
            ", rgb_sum=%" PRIu64 ", alpha_sum=%" PRIu64
            ", max_rgba={%u,%u,%u,%u}, min16=%u, max16=%u, "
            "nonzero16=%" PRIu64 ", sum16=%" PRIu64
            ", rgb_fnv=0x%016" PRIx64
            ", rows=[%s], dump=%s%s%s.\n",
            prefix, stage, upload_level->face, upload_level->level,
            texture_color_format_name(binding->shape.color_format),
            binding->shape.color_format, (unsigned int)binding->dxgi_format,
            binding->vram_addr, binding->vram_addr + binding->length,
            upload_level->width, upload_level->height, upload_level->depth,
            binding->shape.pitch, upload_level->decoded_row_pitch,
            upload_level->decoded_slice_pitch, decoded_size, basic.linear,
            binding->shape.cubemap, binding->shape.dimensionality == 3,
            binding->upload_bytes_per_pixel, total_nonzero_rgb,
            total_greenish_rgb, total_rgb_sum, total_alpha_sum,
            max_r, max_g, max_b, max_a, min16, max16, nonzero16, sum16,
            fnv, rows->str,
            wrote_dump ? dump_path : "not-written",
            !wrote_dump && dump_path != NULL ? " path=" : "",
            !wrote_dump && dump_path != NULL ? dump_path : "");
}

static bool prepare_texture_upload_level(
    unsigned int stage, const PGRAPHD3D12TextureBinding *binding,
    PGRAPHD3D12TextureUploadLevel *upload_level, uint64_t *upload_size,
    Error **errp)
{
    uint64_t decoded_row_pitch;
    uint64_t decoded_slice_pitch;
    uint64_t upload_row_pitch;
    uint64_t upload_slice_pitch;
    uint64_t aligned_offset;
    unsigned int block_bytes = dxgi_block_bytes(binding->dxgi_format);

    if (block_bytes != 0) {
        upload_level->rows = (upload_level->height + 3) / 4;
        decoded_row_pitch =
            (uint64_t)((upload_level->width + 3) / 4) * block_bytes;
    } else {
        upload_level->rows = upload_level->height;
        decoded_row_pitch =
            (uint64_t)upload_level->width * binding->upload_bytes_per_pixel;
    }
    if (decoded_row_pitch > UINT32_MAX ||
        decoded_row_pitch > UINT64_MAX / upload_level->rows) {
        error_setg(errp,
                   "Direct3D 12 stage %u texture face %u mip %u layout "
                   "overflow: format=%s (0x%x), dxgi=%u, size=%ux%ux%u, "
                   "bytes_per_pixel=%u, decoded_row_pitch=%" PRIu64,
                   stage, upload_level->face, upload_level->level,
                   texture_color_format_name(binding->shape.color_format),
                   binding->shape.color_format,
                   (unsigned int)binding->dxgi_format,
                   upload_level->width, upload_level->height,
                   upload_level->depth, binding->upload_bytes_per_pixel,
                   decoded_row_pitch);
        return false;
    }

    decoded_slice_pitch = decoded_row_pitch * upload_level->rows;
    if (decoded_slice_pitch > UINT32_MAX) {
        error_setg(errp,
                   "Direct3D 12 stage %u texture face %u mip %u decoded "
                   "slice pitch is too large: decoded_slice_pitch=%" PRIu64
                   ", format=%s (0x%x), dxgi=%u, size=%ux%ux%u",
                   stage, upload_level->face, upload_level->level,
                   decoded_slice_pitch,
                   texture_color_format_name(binding->shape.color_format),
                   binding->shape.color_format,
                   (unsigned int)binding->dxgi_format,
                   upload_level->width, upload_level->height,
                   upload_level->depth);
        return false;
    }

    upload_row_pitch = ROUND_UP(decoded_row_pitch,
                                D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    if (upload_row_pitch > UINT32_MAX ||
        upload_row_pitch > UINT64_MAX / upload_level->rows) {
        error_setg(errp,
                   "Direct3D 12 stage %u texture face %u mip %u upload row "
                   "pitch is too large: upload_row_pitch=%" PRIu64
                   ", format=%s (0x%x), dxgi=%u, size=%ux%ux%u",
                   stage, upload_level->face, upload_level->level,
                   upload_row_pitch,
                   texture_color_format_name(binding->shape.color_format),
                   binding->shape.color_format,
                   (unsigned int)binding->dxgi_format,
                   upload_level->width, upload_level->height,
                   upload_level->depth);
        return false;
    }

    upload_slice_pitch = upload_row_pitch * upload_level->rows;
    if (upload_slice_pitch > UINT32_MAX ||
        upload_slice_pitch > UINT64_MAX / upload_level->depth) {
        error_setg(errp,
                   "Direct3D 12 stage %u texture face %u mip %u upload "
                   "slice pitch is too large: upload_slice_pitch=%" PRIu64
                   ", format=%s (0x%x), dxgi=%u, size=%ux%ux%u",
                   stage, upload_level->face, upload_level->level,
                   upload_slice_pitch,
                   texture_color_format_name(binding->shape.color_format),
                   binding->shape.color_format,
                   (unsigned int)binding->dxgi_format,
                   upload_level->width, upload_level->height,
                   upload_level->depth);
        return false;
    }

    aligned_offset =
        ROUND_UP(*upload_size, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    if (aligned_offset < *upload_size ||
        upload_slice_pitch > UINT64_MAX / upload_level->depth ||
        aligned_offset >
            UINT64_MAX - upload_slice_pitch * upload_level->depth) {
        error_setg(errp,
                   "Direct3D 12 stage %u texture face %u mip %u upload "
                   "layout overflow: current_upload_size=%" PRIu64
                   ", upload_slice_pitch=%" PRIu64 ", depth=%u, "
                   "format=%s (0x%x), dxgi=%u",
                   stage, upload_level->face, upload_level->level,
                   *upload_size, upload_slice_pitch, upload_level->depth,
                   texture_color_format_name(binding->shape.color_format),
                   binding->shape.color_format,
                   (unsigned int)binding->dxgi_format);
        return false;
    }

    upload_level->decoded_row_pitch = (uint32_t)decoded_row_pitch;
    upload_level->decoded_slice_pitch = (uint32_t)decoded_slice_pitch;
    upload_level->upload_row_pitch = (uint32_t)upload_row_pitch;
    upload_level->upload_slice_pitch = (uint32_t)upload_slice_pitch;
    upload_level->upload_size = upload_slice_pitch * upload_level->depth;
    upload_level->upload_offset = aligned_offset;
    *upload_size = aligned_offset + upload_level->upload_size;
    return true;
}

static bool reserve_texture_upload_range(
    PGRAPHState *pg, unsigned int stage,
    const PGRAPHD3D12TextureBinding *binding, uint64_t upload_size,
    uint64_t *upload_base, Error **errp)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    uint64_t base;

    if (r == NULL || r->upload_buffer.mapped == NULL) {
        error_setg(errp, "Direct3D 12 texture upload buffer is not mapped");
        return false;
    }

    if (!pgraph_d3d12_ring_alloc(pg, &r->upload_buffer, upload_size,
                                 D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT,
                                 &base, errp)) {
        error_prepend(errp,
                      "Direct3D 12 stage %u texture upload could not reserve "
                      "%" PRIu64 " bytes: format=%s (0x%x), dxgi=%u, "
                      "texture_base=0x%" HWADDR_PRIx ", texture_length=%zu: ",
                      stage, upload_size,
                      texture_color_format_name(binding->shape.color_format),
                      binding->shape.color_format,
                      (unsigned int)binding->dxgi_format,
                      binding->vram_addr, binding->length);
        return false;
    }

    r->upload_buffer.offset = base + upload_size;
    *upload_base = base;
    return true;
}

/*
 * Uploads a custom texture into a resource created at its size, levels and
 * format; no guest decoding takes part.
 */
static bool upload_replacement_texture(NV2AState *d, unsigned int stage,
                                       PGRAPHD3D12TextureBinding *binding,
                                       Error **errp)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    const PGRAPHD3D12ReplacementTexture *rep = binding->replacement;
    unsigned int subresources = rep->faces * rep->mip_levels;
    bool block_format = dxgi_block_bytes(rep->dxgi_format) != 0;
    g_autofree uint64_t *offsets = g_new0(uint64_t, subresources);
    uint64_t total_size = 0;
    uint64_t upload_base = 0;

    for (unsigned int i = 0; i < subresources; i++) {
        const PGRAPHD3D12ReplacementSubresource *sub = &rep->subresources[i];

        total_size = ROUND_UP(total_size,
                              D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
        offsets[i] = total_size;
        total_size += (uint64_t)ROUND_UP(sub->row_pitch,
                                         D3D12_TEXTURE_DATA_PITCH_ALIGNMENT) *
                      sub->rows;
    }
    if (!reserve_texture_upload_range(pg, stage, binding, total_size,
                                      &upload_base, errp)) {
        return false;
    }

    for (unsigned int i = 0; i < subresources; i++) {
        const PGRAPHD3D12ReplacementSubresource *sub = &rep->subresources[i];
        uint32_t row_pitch = ROUND_UP(sub->row_pitch,
                                      D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
        uint8_t *dst = (uint8_t *)r->upload_buffer.mapped + upload_base +
                       offsets[i];

        for (uint32_t row = 0; row < sub->rows; row++) {
            memcpy(dst + (size_t)row * row_pitch,
                   rep->data + sub->offset + (size_t)row * sub->row_pitch,
                   sub->row_pitch);
        }
    }

    if (!pgraph_d3d12_begin_commands(pg, errp)) {
        return false;
    }
    pgraph_d3d12_transition_raw_resource(r, binding->resource,
                                         &binding->state,
                                         D3D12_RESOURCE_STATE_COPY_DEST);
    for (unsigned int i = 0; i < subresources; i++) {
        const PGRAPHD3D12ReplacementSubresource *sub = &rep->subresources[i];
        D3D12_TEXTURE_COPY_LOCATION dst;
        D3D12_TEXTURE_COPY_LOCATION src;

        memset(&dst, 0, sizeof(dst));
        dst.pResource = binding->resource;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = i;

        memset(&src, 0, sizeof(src));
        src.pResource = r->upload_buffer.resource;
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Offset = upload_base + offsets[i];
        src.PlacedFootprint.Footprint.Format = rep->dxgi_format;
        /* Block-compressed footprints cover whole 4x4 blocks. */
        src.PlacedFootprint.Footprint.Width =
            block_format ? ROUND_UP(sub->width, 4) : sub->width;
        src.PlacedFootprint.Footprint.Height =
            block_format ? ROUND_UP(sub->height, 4) : sub->height;
        src.PlacedFootprint.Footprint.Depth = 1;
        src.PlacedFootprint.Footprint.RowPitch =
            ROUND_UP(sub->row_pitch, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
        r->command_list->lpVtbl->CopyTextureRegion(r->command_list, &dst, 0,
                                                   0, 0, &src, NULL);
    }
    pgraph_d3d12_transition_raw_resource(
        r, binding->resource, &binding->state,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return true;
}

static bool upload_texture_resource(
    NV2AState *d, unsigned int stage, PGRAPHD3D12TextureBinding *binding,
    const PGRAPHD3D12TexturePackHash *pack_hash, Error **errp)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    PGRAPHD3D12TextureFormatInfo info = {
        .dxgi_format = binding->dxgi_format,
        .upload_bytes_per_pixel = binding->upload_bytes_per_pixel,
        .converted = binding->converted,
        .compressed = binding->compressed,
    };
    BasicColorFormatInfo basic =
        kelvin_color_format_info_map[binding->shape.color_format];
    unsigned int mip_levels = texture_mip_levels_for_shape(&binding->shape);
    unsigned int texture_faces = binding->shape.cubemap ? 6 : 1;
    unsigned int upload_subresources = texture_faces * mip_levels;
    size_t face_source_size = binding->shape.cubemap ?
        texture_face_source_size(pg, &binding->shape) : 0;
    g_autofree PGRAPHD3D12TextureUploadLevel *levels =
        g_new0(PGRAPHD3D12TextureUploadLevel, upload_subresources);
    uint64_t upload_size = 0;
    uint64_t upload_base = 0;
    /* Base level of each face, kept for a texture dump. */
    g_autoptr(GPtrArray) dump_faces = NULL;
    uint32_t dump_row_pitch = 0;

    if (pgraph_d3d12_texture_replace_eligible(&binding->shape,
                                              binding->dxgi_format) &&
        pgraph_d3d12_texture_dump_wanted(pg, &binding->shape,
                                         binding->vram_addr, pack_hash)) {
        dump_faces = g_ptr_array_new_with_free_func(g_free);
        g_ptr_array_set_size(dump_faces, texture_faces);
    }

    for (unsigned int face = 0; face < texture_faces; face++) {
        hwaddr level_vram_addr = binding->vram_addr +
                                 (hwaddr)face * face_source_size;

        for (unsigned int level = 0; level < mip_levels; level++) {
            unsigned int subresource = face * mip_levels + level;
            PGRAPHD3D12TextureUploadLevel *upload_level =
                &levels[subresource];

            upload_level->face = face;
            upload_level->level = level;
            upload_level->vram_addr = level_vram_addr;
            texture_level_dimensions(&binding->shape, level,
                                     &upload_level->width,
                                     &upload_level->height,
                                     &upload_level->depth);
            if (!prepare_texture_upload_level(stage, binding, upload_level,
                                              &upload_size, errp)) {
                return false;
            }

            if (binding->compressed) {
                level_vram_addr += texture_compressed_level_source_size(
                    binding->shape.color_format, upload_level->width,
                    upload_level->height, upload_level->depth);
            } else if (!basic.linear) {
                level_vram_addr +=
                    (hwaddr)upload_level->width * upload_level->height *
                    upload_level->depth * basic.bytes_per_pixel;
            }
        }
    }

    if (!reserve_texture_upload_range(pg, stage, binding, upload_size,
                                      &upload_base, errp)) {
        return false;
    }

    for (unsigned int subresource = 0; subresource < upload_subresources;
         subresource++) {
        levels[subresource].upload_offset += upload_base;
    }

    for (unsigned int subresource = 0; subresource < upload_subresources;
         subresource++) {
        PGRAPHD3D12TextureUploadLevel *upload_level = &levels[subresource];
        g_autofree uint8_t *decoded = NULL;
        size_t decoded_size;
        size_t expected_decoded_size;

        decoded = decode_texture_level(d, stage, &binding->shape, &info,
                                       upload_level->vram_addr,
                                       upload_level->level,
                                       upload_level->width,
                                       upload_level->height,
                                       upload_level->depth,
                                       &upload_level->decoded_row_pitch,
                                       &upload_level->decoded_slice_pitch,
                                       &decoded_size, errp);
        if (decoded == NULL) {
            if (r->upload_buffer.offset == upload_base + upload_size) {
                r->upload_buffer.offset = upload_base;
            }
            return false;
        }

        expected_decoded_size =
            (size_t)upload_level->decoded_slice_pitch * upload_level->depth;
        if (decoded_size < expected_decoded_size) {
            error_setg(
                errp,
                "Direct3D 12 stage %u texture face %u mip %u decoded too "
                "few bytes: decoded_size=%zu, expected_at_least=%zu, "
                "format=%s (0x%x), dxgi=%u, mip_size=%ux%ux%u, "
                "decoded_row_pitch=%u, decoded_slice_pitch=%u, "
                "texture_vram=0x%" HWADDR_PRIx,
                stage, upload_level->face, upload_level->level,
                decoded_size, expected_decoded_size,
                texture_color_format_name(binding->shape.color_format),
                binding->shape.color_format,
                (unsigned int)binding->dxgi_format,
                upload_level->width, upload_level->height,
                upload_level->depth, upload_level->decoded_row_pitch,
                upload_level->decoded_slice_pitch, upload_level->vram_addr);
            if (r->upload_buffer.offset == upload_base + upload_size) {
                r->upload_buffer.offset = upload_base;
            }
            return false;
        }
        if (pgraph_d3d12_resource_dumps(pg)
#if defined(XEMU_UWP) && defined(XEMU_D3D12_UWP_BOOT_TRACE)
            || true
#endif
            ) {
            report_texture_upload_sample(stage, binding, upload_level,
                                         decoded, decoded_size);
        }
        if (dump_faces != NULL && upload_level->level == 0) {
            g_ptr_array_index(dump_faces, upload_level->face) =
                g_memdup2(decoded, upload_level->decoded_slice_pitch);
            dump_row_pitch = upload_level->decoded_row_pitch;
        }

        /* Row and slice padding is never read by the copy engine, so the
         * staging region is not cleared first. */
        for (unsigned int z = 0; z < upload_level->depth; z++) {
            uint8_t *dst_slice = (uint8_t *)r->upload_buffer.mapped +
                                 upload_level->upload_offset +
                                 (uint64_t)z * upload_level->upload_slice_pitch;
            const uint8_t *src_slice =
                decoded + (size_t)z * upload_level->decoded_slice_pitch;

            if (upload_level->upload_row_pitch ==
                upload_level->decoded_row_pitch) {
                memcpy(dst_slice, src_slice,
                       (size_t)upload_level->decoded_row_pitch *
                           upload_level->rows);
                continue;
            }
            for (unsigned int y = 0; y < upload_level->rows; y++) {
                memcpy(dst_slice + (uint64_t)y * upload_level->upload_row_pitch,
                       src_slice + (size_t)y * upload_level->decoded_row_pitch,
                       upload_level->decoded_row_pitch);
            }
        }
    }

    if (!pgraph_d3d12_begin_commands(pg, errp)) {
        return false;
    }

    pgraph_d3d12_transition_raw_resource(r, binding->resource,
                                         &binding->state,
                                         D3D12_RESOURCE_STATE_COPY_DEST);

    for (unsigned int subresource = 0; subresource < upload_subresources;
         subresource++) {
        PGRAPHD3D12TextureUploadLevel *upload_level = &levels[subresource];
        D3D12_TEXTURE_COPY_LOCATION dst;
        D3D12_TEXTURE_COPY_LOCATION src;

        memset(&dst, 0, sizeof(dst));
        dst.pResource = binding->resource;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = subresource;

        memset(&src, 0, sizeof(src));
        src.pResource = r->upload_buffer.resource;
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Offset = upload_level->upload_offset;
        src.PlacedFootprint.Footprint.Format = binding->dxgi_format;
        src.PlacedFootprint.Footprint.Width = upload_level->width;
        src.PlacedFootprint.Footprint.Height = upload_level->height;
        if (dxgi_block_bytes(binding->dxgi_format) != 0) {
            /* Block-compressed footprints cover whole 4x4 blocks, also for
             * the 2x2 and 1x1 levels at the end of a mip chain. */
            src.PlacedFootprint.Footprint.Width =
                ROUND_UP(upload_level->width, 4);
            src.PlacedFootprint.Footprint.Height =
                ROUND_UP(upload_level->height, 4);
        }
        src.PlacedFootprint.Footprint.Depth = upload_level->depth;
        src.PlacedFootprint.Footprint.RowPitch =
            upload_level->upload_row_pitch;

        r->command_list->lpVtbl->CopyTextureRegion(r->command_list, &dst, 0,
                                                   0, 0, &src, NULL);
    }

    pgraph_d3d12_transition_raw_resource(
        r, binding->resource, &binding->state,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    if (dump_faces != NULL) {
        pgraph_d3d12_dump_texture(pg, &binding->shape, pack_hash,
                                  binding->dxgi_format,
                                  (const uint8_t *const *)dump_faces->pdata,
                                  dump_row_pitch);
    }
    return true;
}

static D3D12_TEXTURE_ADDRESS_MODE texture_address_mode(unsigned int mode)
{
    switch (mode) {
    case NV_PGRAPH_TEXADDRESS0_ADDRU_WRAP:
        return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    case NV_PGRAPH_TEXADDRESS0_ADDRU_MIRROR:
        return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    case NV_PGRAPH_TEXADDRESS0_ADDRU_CLAMP_TO_EDGE:
    case NV_PGRAPH_TEXADDRESS0_ADDRU_CLAMP_OGL:
        return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    case NV_PGRAPH_TEXADDRESS0_ADDRU_BORDER:
        return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    default:
        return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    }
}

static bool filter_is_linear(unsigned int filter, bool mag)
{
    if (mag) {
        return filter == 2 || filter == 4;
    }

    return filter == NV_PGRAPH_TEXFILTER0_MIN_TENT_LOD0 ||
           filter == NV_PGRAPH_TEXFILTER0_MIN_TENT_NEARESTLOD ||
           filter == NV_PGRAPH_TEXFILTER0_MIN_TENT_TENT_LOD ||
           filter == NV_PGRAPH_TEXFILTER0_MIN_CONVOLUTION_2D_LOD0;
}

static bool texture_mipmap_enabled(const TextureShape *shape, uint32_t filter)
{
    BasicColorFormatInfo basic =
        kelvin_color_format_info_map[shape->color_format];
    unsigned int min_filter = GET_MASK(filter, NV_PGRAPH_TEXFILTER0_MIN);

    return !basic.linear && texture_view_levels(shape) > 1 &&
           !(min_filter == NV_PGRAPH_TEXFILTER0_MIN_BOX_LOD0 ||
             min_filter == NV_PGRAPH_TEXFILTER0_MIN_TENT_LOD0 ||
             min_filter == NV_PGRAPH_TEXFILTER0_MIN_CONVOLUTION_2D_LOD0);
}

static bool texture_mipmap_nearest(const TextureShape *shape, uint32_t filter)
{
    BasicColorFormatInfo basic =
        kelvin_color_format_info_map[shape->color_format];
    unsigned int min_filter = GET_MASK(filter, NV_PGRAPH_TEXFILTER0_MIN);

    return basic.linear || texture_view_levels(shape) == 1 ||
           min_filter == NV_PGRAPH_TEXFILTER0_MIN_BOX_NEARESTLOD ||
           min_filter == NV_PGRAPH_TEXFILTER0_MIN_TENT_NEARESTLOD;
}

static D3D12_FILTER texture_filter(uint32_t filter, bool mipmap_en,
                                   bool mip_linear,
                                   uint32_t max_anisotropy)
{
    bool min_linear = filter_is_linear(
        GET_MASK(filter, NV_PGRAPH_TEXFILTER0_MIN), false);
    bool mag_linear = filter_is_linear(
        GET_MASK(filter, NV_PGRAPH_TEXFILTER0_MAG), true);

    /*
     * Anisotropy refines a linear min filter. The anisotropic filter always
     * magnifies linearly and blends mip levels, so a point mag filter or a
     * nearest-level min filter keeps its point sampling without it.
     */
    if (max_anisotropy > 1 && min_linear && mag_linear &&
        (mip_linear || !mipmap_en)) {
        return D3D12_FILTER_ANISOTROPIC;
    }

    if (min_linear && mag_linear && mip_linear) {
        return D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    }
    if (min_linear && mag_linear) {
        return D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    }
    if (min_linear && mip_linear) {
        return D3D12_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR;
    }
    if (min_linear) {
        return D3D12_FILTER_MIN_LINEAR_MAG_MIP_POINT;
    }
    if (mag_linear && mip_linear) {
        return D3D12_FILTER_MIN_POINT_MAG_MIP_LINEAR;
    }
    if (mag_linear) {
        return D3D12_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT;
    }
    if (mip_linear) {
        return D3D12_FILTER_MIN_MAG_POINT_MIP_LINEAR;
    }
    return D3D12_FILTER_MIN_MAG_MIP_POINT;
}

/*
 * The border colour replaces the texel before the format's channel swizzle
 * in the pixel shader, so it is stored with the inverse swizzle applied.
 * Formats without alpha read an opaque border.
 */
static void texture_border_color(unsigned int color_format,
                                 uint32_t border_color, float *rgba)
{
    float color[4];

    pgraph_argb_pack32_to_rgba_float(border_color, color);
    switch (color_format) {
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_B8G8R8A8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_B8G8R8A8:
        /* Sampled as (g, b, a, r). */
        rgba[0] = color[3];
        rgba[1] = color[0];
        rgba[2] = color[1];
        rgba[3] = color[2];
        return;
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R8G8B8A8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_R8G8B8A8:
        /* Sampled as (a, b, g, r). */
        rgba[0] = color[3];
        rgba[1] = color[2];
        rgba[2] = color[1];
        rgba[3] = color[0];
        return;
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R5G6B5:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_R5G6B5:
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R6G5B5:
        color[3] = 1.0f;
        break;
    default:
        break;
    }
    memcpy(rgba, color, sizeof(color));
}

static void fill_sampler_desc(const TextureShape *shape, uint32_t filter,
                              uint32_t address, uint32_t border_color,
                              uint32_t requested_max_anisotropy,
                              D3D12_SAMPLER_DESC *desc)
{
    bool mipmap_en = texture_mipmap_enabled(shape, filter);
    bool mipmap_nearest = texture_mipmap_nearest(shape, filter);
    bool mip_linear = mipmap_en && !mipmap_nearest;
    uint32_t max_anisotropy = requested_max_anisotropy ?
        MIN(requested_max_anisotropy, 16u) : 1u;

    memset(desc, 0, sizeof(*desc));
    desc->Filter = texture_filter(filter, mipmap_en, mip_linear,
                                  max_anisotropy);
    desc->AddressU = texture_address_mode(
        GET_MASK(address, NV_PGRAPH_TEXADDRESS0_ADDRU));
    desc->AddressV = texture_address_mode(
        GET_MASK(address, NV_PGRAPH_TEXADDRESS0_ADDRV));
    desc->AddressW = texture_address_mode(
        GET_MASK(address, NV_PGRAPH_TEXADDRESS0_ADDRP));
    desc->MipLODBias = pgraph_convert_lod_bias_to_float(
        GET_MASK(filter, NV_PGRAPH_TEXFILTER0_MIPMAP_LOD_BIAS));
    desc->MaxAnisotropy =
        desc->Filter == D3D12_FILTER_ANISOTROPIC ? max_anisotropy : 1;
    desc->ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    /* LOD is relative to the view, whose first level is the base level. */
    desc->MinLOD = 0.0f;
    if (mipmap_en) {
        desc->MaxLOD = D3D12_FLOAT32_MAX;
    } else if (desc->Filter == D3D12_FILTER_ANISOTROPIC) {
        desc->MaxLOD = 0.0f;
    } else {
        /* Point mip selection stays on the base level below LOD 0.5, and
         * a positive LOD still selects the min filter over the mag filter. */
        desc->MaxLOD = 0.25f;
    }
    texture_border_color(shape->color_format, border_color,
                         desc->BorderColor);
}

static guint sampler_desc_hash(gconstpointer data)
{
    return (guint)fast_hash(data, sizeof(D3D12_SAMPLER_DESC));
}

static gboolean sampler_desc_equal(gconstpointer a, gconstpointer b)
{
    return memcmp(a, b, sizeof(D3D12_SAMPLER_DESC)) == 0;
}

/* Samplers are immutable descriptors shared by every texture with the same
 * NV2A sampling state. Entries are never recycled individually; a full heap
 * is drained and rebuilt. */
static bool lookup_sampler(PGRAPHState *pg, const TextureShape *shape,
                           uint32_t filter, uint32_t address,
                           uint32_t border_color, uint32_t max_anisotropy,
                           D3D12_GPU_DESCRIPTOR_HANDLE *sampler_gpu,
                           Error **errp)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    D3D12_SAMPLER_DESC desc;
    gpointer value;
    uint32_t index;

    fill_sampler_desc(shape, filter, address, border_color, max_anisotropy,
                      &desc);

    value = g_hash_table_lookup(r->sampler_cache, &desc);
    if (value != NULL) {
        index = GPOINTER_TO_UINT(value) - 1;
        *sampler_gpu = gpu_descriptor_handle(
            r->sampler_heap, r->sampler_descriptor_size, index);
        return true;
    }

    if (r->next_sampler_descriptor >= PGRAPH_D3D12_SAMPLER_HEAP_SIZE) {
        /* In-flight commands may reference existing samplers; drain the
         * GPU before rewriting the heap. */
        r->perf.finish_sampler++;
        pgraph_d3d12_finish(pg);
        g_hash_table_remove_all(r->sampler_cache);
        r->next_sampler_descriptor = 0;
        r->texture_sampler_regs_valid = 0;
    }

    index = r->next_sampler_descriptor++;
    r->device->lpVtbl->CreateSampler(
        r->device, &desc,
        cpu_descriptor_handle(r->sampler_heap, r->sampler_descriptor_size,
                              index));
    g_hash_table_insert(r->sampler_cache,
                        g_memdup2(&desc, sizeof(desc)),
                        GUINT_TO_POINTER(index + 1));

    *sampler_gpu = gpu_descriptor_handle(
        r->sampler_heap, r->sampler_descriptor_size, index);
    return true;
}

static bool texture_dirty(NV2AState *d, hwaddr addr, size_t size)
{
    uint64_t vram_size = memory_region_size(d->vram);
    hwaddr start = addr & TARGET_PAGE_MASK;
    hwaddr end;

    if (size == 0 || addr >= vram_size) {
        return false;
    }

    if (size > vram_size || addr > vram_size - size) {
        end = vram_size;
    } else {
        end = MIN(TARGET_PAGE_ALIGN(addr + size), vram_size);
    }
    if (end <= start) {
        return false;
    }

    return memory_region_test_and_clear_dirty(d->vram, start, end - start,
                                              DIRTY_MEMORY_NV2A_TEX);
}

/* Test-and-clear the dirty pages of the texture and its palette. Because the
 * bits are cleared, the result is propagated to every overlapping entry. */
static bool check_texture_possibly_dirty(NV2AState *d, hwaddr vram_addr,
                                         size_t length,
                                         hwaddr palette_vram_addr,
                                         size_t palette_length,
                                         bool *texture_memory_dirty,
                                         bool *palette_memory_dirty)
{
    bool possibly_dirty = false;
    bool texture_dirty_now;
    bool palette_dirty_now = false;

    /* Deferred write-backs land before the dirty pages are tested. */
    pgraph_d3d12_complete_readbacks_in_range(
        d, vram_addr, length, PGRAPH_D3D12_STASH_TRIGGER_TEXTURE);
    if (palette_length != 0) {
        pgraph_d3d12_complete_readbacks_in_range(
            d, palette_vram_addr, palette_length,
            PGRAPH_D3D12_STASH_TRIGGER_TEXTURE);
    }
    texture_dirty_now = texture_dirty(d, vram_addr, length);
    if (texture_dirty_now) {
        possibly_dirty = true;
        pgraph_d3d12_mark_textures_possibly_dirty(d, vram_addr, length);
    }
    if (palette_length != 0 &&
        (palette_dirty_now =
             texture_dirty(d, palette_vram_addr, palette_length))) {
        possibly_dirty = true;
        pgraph_d3d12_mark_textures_possibly_dirty(d, palette_vram_addr,
                                                  palette_length);
    }
    if (texture_memory_dirty != NULL) {
        *texture_memory_dirty = texture_dirty_now;
    }
    if (palette_memory_dirty != NULL) {
        *palette_memory_dirty = palette_dirty_now;
    }
    return possibly_dirty;
}

static bool texture_range_overlaps_surface(
    hwaddr texture_addr, size_t texture_length,
    const PGRAPHD3D12SurfaceBinding *surface)
{
    uint64_t texture_end;
    uint64_t surface_end;
    uint64_t row_bytes;
    uint64_t visible_span;
    uint64_t clipped_start;
    uint64_t clipped_end;
    uint64_t first_row;
    uint64_t last_row;
    uint64_t row_start;

    if (texture_length == 0 || surface == NULL || surface->size == 0) {
        return false;
    }

    texture_end = texture_length > UINT64_MAX - texture_addr ?
                      UINT64_MAX :
                      texture_addr + texture_length;

    /* Swizzled surfaces occupy one contiguous guest-memory range. */
    if (surface->swizzle) {
        surface_end = surface->size > UINT64_MAX - surface->vram_addr ?
                          UINT64_MAX :
                          surface->vram_addr + surface->size;
        return texture_addr < surface_end &&
               surface->vram_addr < texture_end;
    }

    if (surface->width == 0 || surface->height == 0 ||
        surface->fmt.bytes_per_pixel == 0) {
        return false;
    }

    row_bytes = (uint64_t)surface->width * surface->fmt.bytes_per_pixel;
    visible_span = (uint64_t)(surface->height - 1) * surface->pitch +
                   row_bytes;
    surface_end = visible_span > UINT64_MAX - surface->vram_addr ?
                      UINT64_MAX :
                      surface->vram_addr + visible_span;
    if (texture_addr >= surface_end || surface->vram_addr >= texture_end) {
        return false;
    }

    /* Overlapping or adjacent rows have no pitch gaps to exclude. */
    if (surface->pitch <= row_bytes) {
        return true;
    }

    clipped_start = MAX((uint64_t)texture_addr,
                        (uint64_t)surface->vram_addr);
    clipped_end = MIN(texture_end, surface_end);
    first_row = (clipped_start - surface->vram_addr) / surface->pitch;
    last_row = (clipped_end - 1 - surface->vram_addr) / surface->pitch;
    if (last_row >= surface->height) {
        last_row = surface->height - 1;
    }

    /* Crossing into a later row necessarily reaches a visible row start. */
    if (first_row < last_row) {
        return true;
    }

    row_start = surface->vram_addr + first_row * surface->pitch;
    return clipped_start < row_start + row_bytes && row_start < clipped_end;
}

/* Whether a live or stashed surface overlaps the texture range. */
static bool texture_range_has_surface(PGRAPHD3D12State *r, hwaddr addr,
                                      size_t length)
{
    PGRAPHD3D12SurfaceBinding *surface;

    QTAILQ_FOREACH(surface, &r->surfaces, entry) {
        if (texture_range_overlaps_surface(addr, length, surface)) {
            return true;
        }
    }
    QTAILQ_FOREACH(surface, &r->stashed_surfaces, entry) {
        if (texture_range_overlaps_surface(addr, length, surface)) {
            return true;
        }
    }
    return false;
}

static bool surface_in_list(const PGRAPHD3D12SurfaceBinding *surface,
                            PGRAPHD3D12SurfaceBinding *const *list,
                            unsigned int count)
{
    for (unsigned int i = 0; i < count; i++) {
        if (list[i] == surface) {
            return true;
        }
    }
    return false;
}

/* Diagnostic: each distinct render-target readback a texture forces. */
static void log_texture_overlap_readback(
    const PGRAPHD3D12SurfaceBinding *surface, hwaddr texture_addr,
    const TextureShape *shape)
{
    static struct {
        hwaddr surface_addr;
        hwaddr texture_addr;
        unsigned int color_format;
    } seen[64];
    static unsigned int seen_count;
    unsigned int color_format = shape != NULL ? shape->color_format : ~0u;

    for (unsigned int i = 0; i < seen_count; i++) {
        if (seen[i].surface_addr == surface->vram_addr &&
            seen[i].texture_addr == texture_addr &&
            seen[i].color_format == color_format) {
            return;
        }
    }
    if (seen_count == ARRAY_SIZE(seen)) {
        return;
    }
    seen[seen_count].surface_addr = surface->vram_addr;
    seen[seen_count].texture_addr = texture_addr;
    seen[seen_count].color_format = color_format;
    seen_count++;
    fprintf(stderr,
            "Direct3D 12 renderer: texture readback: surface 0x%" HWADDR_PRIx
            " %ux%u %s dxgi=%u pitch=%u swz=%d -> texture 0x%" HWADDR_PRIx,
            surface->vram_addr, surface->width, surface->height,
            surface->color ? "color" : "zeta",
            (unsigned int)surface->fmt.dxgi_format, surface->pitch,
            surface->swizzle, texture_addr);
    if (shape != NULL) {
        fprintf(stderr, " cf=0x%x %ux%u pitch=%u levels=%u border=%d\n",
                shape->color_format, shape->width, shape->height,
                shape->pitch, shape->levels, shape->border);
    } else {
        fprintf(stderr, " palette\n");
    }
}

/* Write back dirty GPU surfaces overlapping the texture range before it is
 * hashed. Surfaces with a pending upload are skipped: guest memory is newer. */
static bool download_gpu_owned_overlapping_surfaces(
    NV2AState *d, hwaddr texture_addr, size_t texture_length,
    const TextureShape *shape,
    PGRAPHD3D12SurfaceBinding *const *skip_surfaces, unsigned int skip_count,
    bool *downloaded, Error **errp)
{
    PGRAPHD3D12State *r = d->pgraph.d3d12_renderer_state;
    PGRAPHD3D12SurfaceBinding *surface;
    PGRAPHD3D12SurfaceBinding *pending[16];
    unsigned int pending_count = 0;

    *downloaded = false;

    QTAILQ_FOREACH(surface, &r->surfaces, entry) {
        /* GPU-copy sources need no VRAM round trip. The active render
         * target is not exempt. */
        if (surface_in_list(surface, skip_surfaces, skip_count) ||
            !surface->draw_dirty || surface->upload_pending ||
            !texture_range_overlaps_surface(texture_addr, texture_length,
                                            surface)) {
            continue;
        }
        if (pending_count == ARRAY_SIZE(pending)) {
            if (!pgraph_d3d12_download_surfaces_checked_reason(
                    d, pending, pending_count,
                    PGRAPH_D3D12_SURFACE_DOWNLOAD_TEXTURE_OVERLAP, errp)) {
                error_prepend(errp,
                              "Direct3D 12 could not preserve GPU-owned "
                              "surfaces before sampling texture addr=0x%"
                              HWADDR_PRIx ": ",
                              texture_addr);
                return false;
            }
            pending_count = 0;
        }
        log_texture_overlap_readback(surface, texture_addr, shape);
        pending[pending_count++] = surface;
        *downloaded = true;
    }

    if (pending_count != 0 &&
        !pgraph_d3d12_download_surfaces_checked_reason(
            d, pending, pending_count,
            PGRAPH_D3D12_SURFACE_DOWNLOAD_TEXTURE_OVERLAP, errp)) {
        error_prepend(errp,
                      "Direct3D 12 could not preserve GPU-owned surfaces "
                      "before sampling texture addr=0x%" HWADDR_PRIx ": ",
                      texture_addr);
        return false;
    }

    return true;
}

static bool texture_format_is_depth_y16(unsigned int color_format)
{
    return color_format == NV097_SET_TEXTURE_FORMAT_COLOR_SZ_DEPTH_Y16_FIXED ||
           color_format ==
               NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_Y16_FIXED ||
           color_format ==
               NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_Y16_FLOAT;
}

static bool texture_format_is_depth_x8_y24(unsigned int color_format)
{
    return color_format ==
               NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_X8_Y24_FIXED ||
           color_format ==
               NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_X8_Y24_FLOAT;
}

static bool zeta_surface_texture_compatible(
    const PGRAPHD3D12SurfaceBinding *surface, const TextureShape *shape)
{
    switch (surface->fmt.dxgi_format) {
    case DXGI_FORMAT_D16_UNORM:
        return texture_format_is_depth_y16(shape->color_format);
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
        return texture_format_is_depth_x8_y24(shape->color_format);
    default:
        break;
    }

    switch (surface->shape.zeta_format) {
    case NV097_SET_SURFACE_FORMAT_ZETA_Z16:
        return texture_format_is_depth_y16(shape->color_format);
    case NV097_SET_SURFACE_FORMAT_ZETA_Z24S8:
        return texture_format_is_depth_x8_y24(shape->color_format);
    default:
        return false;
    }
}

static bool texture_surface_compatible(
    const PGRAPHD3D12SurfaceBinding *surface, const TextureShape *shape)
{
    if (surface == NULL ||
        (!surface->swizzle && surface->pitch != shape->pitch) ||
        surface->width != shape->width ||
        surface->height != shape->height ||
        shape->border ||
        shape->cubemap ||
        shape->dimensionality != 2 ||
        texture_mip_levels_for_shape(shape) != 1) {
        return false;
    }

    if (!surface->color) {
        return zeta_surface_texture_compatible(surface, shape);
    }

    switch (surface->shape.color_format) {
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_Z1R5G5B5:
        return shape->color_format ==
               NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X1R5G5B5;
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5:
        return shape->color_format ==
                   NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_R5G6B5 ||
               shape->color_format ==
                   NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R5G6B5;
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_Z8R8G8B8:
        return shape->color_format ==
                   NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X8R8G8B8 ||
               shape->color_format ==
                   NV097_SET_TEXTURE_FORMAT_COLOR_SZ_X8R8G8B8;
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8:
        return shape->color_format ==
                   NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8B8G8R8 ||
               shape->color_format ==
                   NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_R8G8B8A8 ||
               shape->color_format ==
                   NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8R8G8B8 ||
               shape->color_format ==
                   NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8R8G8B8;
    default:
        return false;
    }
}

/*
 * The source uploads and overlap write-backs before a surface copy can
 * retire a stash chosen as a source (writing back a live surface first
 * writes back the stashes under it). Pointers are only compared here,
 * never dereferenced; nothing creates surfaces in between.
 */
static bool texture_sources_alive(PGRAPHD3D12State *r,
                                  PGRAPHD3D12SurfaceBinding *const *sources,
                                  unsigned int count)
{
    for (unsigned int i = 0; i < count; i++) {
        PGRAPHD3D12SurfaceBinding *surface;
        bool found = false;

        QTAILQ_FOREACH(surface, &r->surfaces, entry) {
            found |= surface == sources[i];
        }
        QTAILQ_FOREACH(surface, &r->stashed_surfaces, entry) {
            found |= surface == sources[i];
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

static bool texture_sources_include_stash(
    PGRAPHD3D12State *r, PGRAPHD3D12SurfaceBinding *const *sources,
    unsigned int count)
{
    PGRAPHD3D12SurfaceBinding *surface;

    QTAILQ_FOREACH(surface, &r->stashed_surfaces, entry) {
        for (unsigned int i = 0; i < count; i++) {
            if (surface == sources[i]) {
                return true;
            }
        }
    }
    return false;
}

static PGRAPHD3D12SurfaceBinding *texture_source_surface(
    NV2AState *d, hwaddr texture_addr, const TextureShape *shape)
{
    PGRAPHD3D12State *r = d->pgraph.d3d12_renderer_state;
    PGRAPHD3D12SurfaceBinding *surface;

    /* Stashed images are as current as live ones for their bytes; the
     * newest stash of a shape wins. */
    QTAILQ_FOREACH(surface, &r->surfaces, entry) {
        if (surface->vram_addr == texture_addr &&
            surface->resource != NULL &&
            texture_surface_compatible(surface, shape)) {
            return surface;
        }
    }
    QTAILQ_FOREACH_REVERSE(surface, &r->stashed_surfaces, entry) {
        if (surface->vram_addr == texture_addr &&
            surface->resource != NULL &&
            texture_surface_compatible(surface, shape)) {
            return surface;
        }
    }

    return NULL;
}

/*
 * Finds a live swizzled colour surface holding a square `size` image at
 * `addr` (either exactly, or as a block-aligned rectangle of a larger
 * surface) and returns the rectangle origin in surface pixels.
 */
static PGRAPHD3D12SurfaceBinding *find_surface_block(
    NV2AState *d, hwaddr addr, unsigned int size, DXGI_FORMAT format,
    unsigned int *x, unsigned int *y)
{
    PGRAPHD3D12State *r = d->pgraph.d3d12_renderer_state;
    PGRAPHD3D12SurfaceBinding *surface;

    for (unsigned int pass = 0; pass < 2; pass++) {
        for (surface = pass ? QTAILQ_LAST(&r->stashed_surfaces) :
                              QTAILQ_FIRST(&r->surfaces);
             surface != NULL;
             surface = pass ? QTAILQ_PREV(surface, entry) :
                              QTAILQ_NEXT(surface, entry)) {
            unsigned int bpp = surface->fmt.bytes_per_pixel;

            if (!surface->color || !surface->swizzle ||
                surface->resource == NULL ||
                surface->fmt.dxgi_format != format ||
                addr < surface->vram_addr ||
                (uint64_t)surface->width * surface->height * bpp >
                    surface->size) {
                continue;
            }
            if (pgraph_d3d12_swizzled_block_origin(
                    surface->width, surface->height, bpp, size,
                    addr - surface->vram_addr, x, y)) {
                return surface;
            }
        }
    }
    return NULL;
}

/*
 * Render-target mip chains and cube maps: each level (of each face) of a
 * swizzled texture is drawn as its own render target, or lives inside a
 * larger one. When every subresource is GPU-resident it is copied on the
 * GPU instead of being written back to VRAM and re-uploaded. Entries are
 * in subresource order (face * levels + level). Returns the count, or 0.
 */
static unsigned int texture_level_surfaces(
    NV2AState *d, hwaddr texture_addr, const TextureShape *shape,
    const PGRAPHD3D12TextureFormatInfo *info,
    PGRAPHD3D12SurfaceBinding **levels, unsigned int *level_x,
    unsigned int *level_y, unsigned int max_levels)
{
    BasicColorFormatInfo basic =
        kelvin_color_format_info_map[shape->color_format];
    unsigned int mip_levels = texture_mip_levels_for_shape(shape);
    unsigned int faces = shape->cubemap ? 6 : 1;
    size_t face_stride;

    if (mip_levels < 1 || mip_levels * faces > max_levels || shape->border ||
        shape->dimensionality != 2 || basic.linear ||
        info->converted || info->compressed || info->depth ||
        shape->width != shape->height) {
        return 0;
    }

    face_stride = texture_face_source_size(&d->pgraph, shape);
    for (unsigned int face = 0; face < faces; face++) {
        hwaddr offset = (hwaddr)face * face_stride;

        for (unsigned int level = 0; level < mip_levels; level++) {
            unsigned int i = face * mip_levels + level;
            unsigned int width, height, depth;

            texture_level_dimensions(shape, level, &width, &height, &depth);
            levels[i] = find_surface_block(d, texture_addr + offset, width,
                                           info->dxgi_format, &level_x[i],
                                           &level_y[i]);
            if (levels[i] == NULL) {
                return 0;
            }
            offset += (hwaddr)width * height * basic.bytes_per_pixel;
        }
    }
    return mip_levels * faces;
}

static bool surface_texture_gpu_copy_supported(
    PGRAPHState *pg, const PGRAPHD3D12SurfaceBinding *surface,
    const TextureShape *shape, const PGRAPHD3D12TextureFormatInfo *info)
{
    (void)pg;

    if (surface == NULL || surface->resource == NULL || shape->border ||
        shape->cubemap || shape->dimensionality != 2 ||
        texture_mip_levels_for_shape(shape) != 1 ||
        info->converted || info->compressed ||
        surface->width != shape->width || surface->height != shape->height) {
        return false;
    }

    /* A linear surface sampled at a different pitch covers different guest
     * rows than the texture describes; a direct copy would shear every row. */
    if (!surface->swizzle && surface->pitch != shape->pitch) {
        return false;
    }

    if (surface->color) {
        return surface->fmt.dxgi_format == info->dxgi_format;
    }

    if (surface->fmt.dxgi_format == DXGI_FORMAT_D16_UNORM) {
        return texture_resource_format(info) == DXGI_FORMAT_R16_TYPELESS &&
               info->dxgi_format == DXGI_FORMAT_R16_UNORM &&
               texture_format_is_depth_y16(shape->color_format);
    }

    return surface->fmt.dxgi_format == DXGI_FORMAT_D24_UNORM_S8_UINT &&
           texture_resource_format(info) == DXGI_FORMAT_R24G8_TYPELESS &&
           info->dxgi_format == DXGI_FORMAT_R24_UNORM_X8_TYPELESS &&
           texture_format_is_depth_x8_y24(shape->color_format);
}

static void texture_format_use_surface_resource(
    PGRAPHD3D12TextureFormatInfo *info,
    const PGRAPHD3D12SurfaceBinding *source_surface,
    const TextureShape *shape)
{
    if (source_surface == NULL || source_surface->color) {
        return;
    }

    if (source_surface->fmt.dxgi_format == DXGI_FORMAT_D24_UNORM_S8_UINT &&
        texture_format_is_depth_x8_y24(shape->color_format)) {
        info->resource_format = DXGI_FORMAT_R24G8_TYPELESS;
        info->dxgi_format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        info->upload_bytes_per_pixel = 4;
        info->converted = false;
        info->compressed = false;
    } else if (source_surface->fmt.dxgi_format == DXGI_FORMAT_D16_UNORM &&
               texture_format_is_depth_y16(shape->color_format)) {
        info->resource_format = DXGI_FORMAT_R16_TYPELESS;
        info->dxgi_format = DXGI_FORMAT_R16_UNORM;
        info->upload_bytes_per_pixel = 2;
        info->converted = false;
        info->compressed = false;
    }
}

static void report_surface_texture_gpu_copy(
    unsigned int stage, const PGRAPHD3D12TextureBinding *binding,
    const PGRAPHD3D12SurfaceBinding *surface)
{
    uint64_t signature = ((uint64_t)stage << 56) ^
                         ((uint64_t)binding->shape.color_format << 48) ^
                         ((uint64_t)surface->draw_time << 32) ^
                         (uint64_t)binding->vram_addr;

    if (surface_texture_copy_signature_seen(signature)) {
        return;
    }

    fprintf(stderr,
            "Direct3D 12 renderer: stage %u texture refreshed directly from "
            "D3D12 %s surface: texture=0x%" HWADDR_PRIx "..0x%"
            HWADDR_PRIx " %ux%u format=%s (0x%x) dxgi=%u length=%zu, "
            "surface=0x%" HWADDR_PRIx "..0x%" HWADDR_PRIx
            " %ux%u pitch=%u format=0x%x dxgi=%u draw_dirty=%d "
            "download_pending=%d upload_pending=%d draw_time=%d, "
            "resource_state_after_copy=%u.\n",
            stage, surface->color ? "color" : "zeta",
            binding->vram_addr, binding->vram_addr + binding->length,
            binding->shape.width, binding->shape.height,
            texture_color_format_name(binding->shape.color_format),
            binding->shape.color_format, (unsigned int)binding->dxgi_format,
            binding->length, surface->vram_addr,
            surface->vram_addr + surface->size, surface->width,
            surface->height, surface->pitch,
            surface->color ? surface->shape.color_format :
                             surface->shape.zeta_format,
            (unsigned int)surface->fmt.dxgi_format, surface->draw_dirty,
            surface->download_pending, surface->upload_pending,
            surface->draw_time, (unsigned int)binding->state);
}

static bool copy_surface_to_texture_resource(
    NV2AState *d, unsigned int stage, PGRAPHD3D12TextureBinding *binding,
    PGRAPHD3D12SurfaceBinding *surface, unsigned int subresource,
    unsigned int src_x, unsigned int src_y, unsigned int src_size,
    Error **errp)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    D3D12_RESOURCE_STATES surface_state = surface->state;
    D3D12_TEXTURE_COPY_LOCATION dst;
    D3D12_TEXTURE_COPY_LOCATION src;

    if (binding->resource == NULL || surface->resource == NULL) {
        error_setg(errp,
                   "Direct3D 12 stage %u surface-to-texture copy missing "
                   "resource: texture_resource=%p, surface_resource=%p, "
                   "texture=0x%" HWADDR_PRIx "..0x%" HWADDR_PRIx
                   " %ux%u format=%s (0x%x) dxgi=%u, "
                   "surface=0x%" HWADDR_PRIx "..0x%" HWADDR_PRIx
                   " %ux%u pitch=%u format=0x%x dxgi=%u draw_dirty=%d "
                   "download_pending=%d upload_pending=%d draw_time=%d",
                   stage, binding->resource, surface->resource,
                   binding->vram_addr, binding->vram_addr + binding->length,
                   binding->shape.width, binding->shape.height,
                   texture_color_format_name(binding->shape.color_format),
                   binding->shape.color_format,
                   (unsigned int)binding->dxgi_format, surface->vram_addr,
                   surface->vram_addr + surface->size, surface->width,
                   surface->height, surface->pitch,
                   surface->shape.color_format,
                   (unsigned int)surface->fmt.dxgi_format,
                   surface->draw_dirty, surface->download_pending,
                   surface->upload_pending, surface->draw_time);
        return false;
    }

    /* Diagnostic: drain all earlier GPU work before the copy. */
    if (r->tune_flags & PGRAPH_D3D12_TUNE_S2T_SYNC) {
        pgraph_d3d12_finish(pg);
    }
    if (!pgraph_d3d12_begin_commands(pg, errp)) {
        error_prepend(errp,
                      "Direct3D 12 stage %u surface-to-texture copy could "
                      "not begin commands for texture=0x%" HWADDR_PRIx
                      " surface=0x%" HWADDR_PRIx ": ",
                      stage, binding->vram_addr, surface->vram_addr);
        return false;
    }

    pgraph_d3d12_transition_raw_resource(r, surface->resource,
                                         &surface->state,
                                         D3D12_RESOURCE_STATE_COPY_SOURCE);
    pgraph_d3d12_transition_raw_resource(r, binding->resource,
                                         &binding->state,
                                         D3D12_RESOURCE_STATE_COPY_DEST);

    memset(&dst, 0, sizeof(dst));
    dst.pResource = binding->resource;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = subresource;

    memset(&src, 0, sizeof(src));
    src.pResource = surface->resource;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;

    if (src_size != 0 &&
        (src_x != 0 || src_y != 0 || src_size != surface->width ||
         src_size != surface->height)) {
        unsigned int scale = MAX(pg->surface_scale_factor, 1);
        D3D12_BOX box = {
            .left = src_x * scale,
            .top = src_y * scale,
            .front = 0,
            .right = (src_x + src_size) * scale,
            .bottom = (src_y + src_size) * scale,
            .back = 1,
        };

        r->command_list->lpVtbl->CopyTextureRegion(r->command_list, &dst, 0,
                                                   0, 0, &src, &box);
    } else {
        r->command_list->lpVtbl->CopyTextureRegion(r->command_list, &dst, 0,
                                                   0, 0, &src, NULL);
    }

    pgraph_d3d12_transition_raw_resource(
        r, binding->resource, &binding->state,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    pgraph_d3d12_transition_raw_resource(r, surface->resource,
                                         &surface->state, surface_state);

    if (pgraph_d3d12_verbose_reports(pg)) {
        report_surface_texture_gpu_copy(stage, binding, surface);
    }
    return true;
}

static void report_texture_binding_once(
    NV2AState *d, unsigned int stage, const TextureShape *shape,
    hwaddr vram_addr, size_t length, const PGRAPHD3D12TextureFormatInfo *info,
    const PGRAPHD3D12TextureBinding *binding,
    const PGRAPHD3D12SurfaceBinding *source_surface, bool needs_upload,
    bool memory_dirty, bool source_surface_changed, bool gpu_copy_surface,
    bool texture_state_dirty, bool vram_dirty_checked, bool vram_dirty,
    bool source_upload_pending_before,
    bool was_surface_to_texture, hwaddr previous_source_surface_addr,
    int previous_source_surface_draw_time)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    BasicColorFormatInfo basic =
        kelvin_color_format_info_map[shape->color_format];

    if (!pgraph_d3d12_verbose_reports(pg)) {
        return;
    }
    if (r->reported_texture_bindings[stage]) {
        return;
    }
    r->reported_texture_bindings[stage] = true;

    fprintf(stderr,
            "Direct3D 12 renderer: stage %u texture binding: "
            "format=%s (0x%x), dxgi=%u, vram=0x%" HWADDR_PRIx
            "..0x%" HWADDR_PRIx ", length=%zu, size=%ux%ux%u, "
            "dimensionality=%u, pitch=%u, levels=%u, linear=%d, "
            "cubemap=%d, shape_border=%d, depth=%d, "
            "filter=0x%08x, address=0x%08x, address_u=%u, address_v=%u, "
            "address_p=%u, border=0x%08x, max_anisotropy=%u, scale=%f, "
            "surface_to_texture=%d, source_surface=0x%" HWADDR_PRIx
            ", source_draw_time=%d, needs_upload=%d, "
            "memory_dirty=%d, source_surface_changed=%d, "
            "gpu_copy_surface=%d, texture_state_dirty=%d, "
            "vram_dirty_checked=%d, vram_dirty=%d, "
            "source_upload_pending_before=%d, "
            "previous_surface_to_texture=%d, "
            "previous_source_surface=0x%" HWADDR_PRIx
            ", previous_source_draw_time=%d, "
            "texctl0=0x%08x, texctl1=0x%08x, texfmt=0x%08x, "
            "shaderprog=0x%08x.\n",
            stage, texture_color_format_name(shape->color_format),
            shape->color_format, (unsigned int)info->dxgi_format,
            vram_addr, vram_addr + length, length, shape->width,
            shape->height, shape->depth, shape->dimensionality, shape->pitch,
            shape->levels, basic.linear, shape->cubemap, shape->border,
            basic.depth,
            pgraph_reg_r(pg, NV_PGRAPH_TEXFILTER0 + stage * 4),
            pgraph_reg_r(pg, NV_PGRAPH_TEXADDRESS0 + stage * 4),
            GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_TEXADDRESS0 + stage * 4),
                     NV_PGRAPH_TEXADDRESS0_ADDRU),
            GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_TEXADDRESS0 + stage * 4),
                     NV_PGRAPH_TEXADDRESS0_ADDRV),
            GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_TEXADDRESS0 + stage * 4),
                     NV_PGRAPH_TEXADDRESS0_ADDRP),
            pgraph_reg_r(pg, NV_PGRAPH_BORDERCOLOR0 + stage * 4),
            1u << GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_TEXCTL0_0 + stage * 4),
                           NV_PGRAPH_TEXCTL0_0_MAX_ANISOTROPY),
            binding->scale,
            source_surface != NULL,
            source_surface != NULL ? source_surface->vram_addr : 0,
            source_surface != NULL ? source_surface->draw_time : -1,
            needs_upload, memory_dirty, source_surface_changed,
            gpu_copy_surface, texture_state_dirty, vram_dirty_checked,
            vram_dirty, source_upload_pending_before, was_surface_to_texture,
            previous_source_surface_addr, previous_source_surface_draw_time,
            pgraph_reg_r(pg, NV_PGRAPH_TEXCTL0_0 + stage * 4),
            pgraph_reg_r(pg, NV_PGRAPH_TEXCTL1_0 + stage * 4),
            pgraph_reg_r(pg, NV_PGRAPH_TEXFMT0 + stage * 4),
            pgraph_reg_r(pg, NV_PGRAPH_SHADERPROG));
}


static void texture_cache_entry_init(Lru *lru, LruNode *node, const void *key)
{
    PGRAPHD3D12TextureBinding *binding =
        container_of(node, PGRAPHD3D12TextureBinding, node);
    const PGRAPHD3D12TextureKey *tkey = key;

    memcpy(&binding->key, tkey, sizeof(binding->key));
    binding->shape = tkey->shape;
    binding->vram_addr = tkey->vram_addr;
    binding->length = tkey->length;
    binding->palette_vram_addr = tkey->palette_vram_addr;
    binding->palette_length = tkey->palette_length;
    binding->resource_format = tkey->resource_format;
    binding->dxgi_format = tkey->dxgi_format;
    binding->upload_bytes_per_pixel = tkey->upload_bytes_per_pixel;
    binding->converted = tkey->converted;
    binding->compressed = tkey->compressed;
    binding->scale = (float)MAX(tkey->scale, 1u);
    binding->surface_to_texture = tkey->surface_to_texture;
    binding->source_surface_addr = 0;
    binding->source_surface_draw_time = -1;
    binding->resource = NULL;
    binding->state = D3D12_RESOURCE_STATE_COMMON;
    binding->data_hash = 0;
    binding->possibly_dirty = false;
    binding->replacement = NULL;
    binding->draw_time = -1;
    binding->srv_descriptor_index = 0;
    binding->srv_cpu.ptr = 0;
    binding->srv_gpu.ptr = 0;
    binding->descriptors_allocated = false;
    binding->valid = false;
    binding->resource_bytes = 0;
}

static bool texture_cache_entry_compare(Lru *lru, LruNode *node,
                                        const void *key)
{
    PGRAPHD3D12TextureBinding *binding =
        container_of(node, PGRAPHD3D12TextureBinding, node);

    return memcmp(&binding->key, key, sizeof(PGRAPHD3D12TextureKey));
}

static bool texture_cache_entry_pre_evict(Lru *lru, LruNode *node)
{
    PGRAPHD3D12State *r = container_of(lru, PGRAPHD3D12State, texture_cache);
    PGRAPHD3D12TextureBinding *binding =
        container_of(node, PGRAPHD3D12TextureBinding, node);

    for (unsigned int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        if (r->texture_binding[i] == binding) {
            return false;
        }
    }
    return true;
}

static void texture_cache_entry_post_evict(Lru *lru, LruNode *node)
{
    PGRAPHD3D12State *r = container_of(lru, PGRAPHD3D12State, texture_cache);
    PGRAPHD3D12TextureBinding *binding =
        container_of(node, PGRAPHD3D12TextureBinding, node);

    /* A failed creation keeps its SRV slot, so release it either way. */
    release_texture_resource(r->pg, binding);
    binding->possibly_dirty = false;
    binding->replacement = NULL;
}

void pgraph_d3d12_init_textures(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;

    lru_init(&r->texture_cache);
    r->texture_cache_entries =
        g_malloc0_n(PGRAPH_D3D12_TEXTURE_CACHE_SIZE,
                    sizeof(*r->texture_cache_entries));
    for (unsigned int i = 0; i < PGRAPH_D3D12_TEXTURE_CACHE_SIZE; i++) {
        lru_add_free(&r->texture_cache, &r->texture_cache_entries[i].node);
    }
    r->texture_cache.init_node = texture_cache_entry_init;
    r->texture_cache.compare_nodes = texture_cache_entry_compare;
    r->texture_cache.pre_node_evict = texture_cache_entry_pre_evict;
    r->texture_cache.post_node_evict = texture_cache_entry_post_evict;

    r->sampler_cache = g_hash_table_new_full(sampler_desc_hash,
                                             sampler_desc_equal, g_free,
                                             NULL);

    for (unsigned int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        r->texture_binding[i] = NULL;
        r->texture_sampler_gpu[i].ptr = 0;
        r->texture_scale[i] = 1.0f;
        pg->texture_dirty[i] = true;
    }
    r->texture_active_mask = 0;
    pgraph_d3d12_init_texture_replace(pg);
}

struct pgraph_d3d12_mark_dirty_ctx {
    hwaddr addr;
    hwaddr end;
};

static void mark_textures_possibly_dirty_visitor(Lru *lru, LruNode *node,
                                                 void *opaque)
{
    const struct pgraph_d3d12_mark_dirty_ctx *ctx = opaque;
    PGRAPHD3D12TextureBinding *binding =
        container_of(node, PGRAPHD3D12TextureBinding, node);
    bool overlapping;

    if (binding->resource == NULL || binding->possibly_dirty) {
        return;
    }

    overlapping = binding->length != 0 &&
                  ctx->addr < binding->vram_addr + binding->length &&
                  binding->vram_addr < ctx->end;
    if (binding->palette_length != 0) {
        overlapping |=
            ctx->addr < binding->palette_vram_addr +
                            binding->palette_length &&
            binding->palette_vram_addr < ctx->end;
    }

    binding->possibly_dirty |= overlapping;
}

void pgraph_d3d12_mark_textures_possibly_dirty(NV2AState *d, hwaddr addr,
                                               hwaddr size)
{
    PGRAPHState *pg;
    PGRAPHD3D12State *r;
    hwaddr vram_size;
    struct pgraph_d3d12_mark_dirty_ctx ctx;

    if (d == NULL || size == 0) {
        return;
    }

    pg = &d->pgraph;
    r = pg->d3d12_renderer_state;
    if (r == NULL) {
        return;
    }

    vram_size = memory_region_size(d->vram);
    if (addr >= vram_size) {
        return;
    }

    size = MIN(size, vram_size - addr);
    ctx.addr = addr & TARGET_PAGE_MASK;
    ctx.end = MIN(TARGET_PAGE_ALIGN(addr + size), vram_size);

    lru_visit_active(&r->texture_cache, mark_textures_possibly_dirty_visitor,
                     &ctx);

    /* Force currently-bound stages that overlap to revalidate. */
    for (unsigned int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        PGRAPHD3D12TextureBinding *binding = r->texture_binding[i];

        if (binding != NULL && binding->possibly_dirty) {
            pg->texture_dirty[i] = true;
        }
    }
}

static bool texture_palette_range(NV2AState *d, unsigned int stage,
                                  const TextureShape *shape,
                                  hwaddr *palette_vram_addr,
                                  size_t *palette_length,
                                  Error **errp)
{
    PGRAPHState *pg = &d->pgraph;
    uint64_t vram_size = memory_region_size(d->vram);

    *palette_vram_addr = 0;
    *palette_length = 0;

    if (shape->color_format !=
        NV097_SET_TEXTURE_FORMAT_COLOR_SZ_I8_A8R8G8B8) {
        return true;
    }

    *palette_vram_addr =
        pgraph_get_texture_palette_phys_addr_length(pg, stage,
                                                    palette_length);
    if (*palette_length > vram_size ||
        *palette_vram_addr > vram_size - *palette_length) {
        error_setg(errp,
                   "Direct3D 12 stage %u indexed texture palette is outside "
                   "guest VRAM: palette=0x%" HWADDR_PRIx ", length=%zu, "
                   "vram_size=0x%" PRIx64,
                   stage, *palette_vram_addr, *palette_length, vram_size);
        return false;
    }

    return true;
}

void pgraph_d3d12_finalize_textures(PGRAPHState *pg)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;

    if (r == NULL) {
        return;
    }

    pgraph_d3d12_finish(pg);
    for (unsigned int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        r->texture_binding[i] = NULL;
        r->texture_sampler_gpu[i].ptr = 0;
        r->texture_scale[i] = 1.0f;
    }
    if (r->texture_cache_entries != NULL) {
        lru_flush(&r->texture_cache);
        g_free(r->texture_cache_entries);
        r->texture_cache_entries = NULL;
    }
    if (r->sampler_cache != NULL) {
        g_hash_table_destroy(r->sampler_cache);
        r->sampler_cache = NULL;
    }
    /* After the flush above: bindings point into the custom textures. */
    pgraph_d3d12_finalize_texture_replace(pg);
    /* All fences completed above, so deferred releases drain immediately. */
    pgraph_d3d12_process_deferred_releases(pg);
    r->texture_active_mask = 0;
}

void pgraph_d3d12_flush_scale_dependent_textures(PGRAPHState *pg)
{
    /* Surface-backed resources inherit the internal resolution and cannot
     * survive a scale change. The GPU is idle, so releases drain
     * immediately. */
    pgraph_d3d12_flush_textures(pg);
}

void pgraph_d3d12_flush_textures(PGRAPHState *pg)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;

    if (r == NULL || r->texture_cache_entries == NULL) {
        return;
    }

    /* Releases wait behind the fence of any recorded draw using them. */
    for (unsigned int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        r->texture_binding[i] = NULL;
        r->texture_sampler_gpu[i].ptr = 0;
        r->texture_scale[i] = 1.0f;
        r->record_texture_srv_gpu[i] = 0;
        r->record_texture_sampler_gpu[i] = 0;
        pg->texture_dirty[i] = true;
    }
    r->texture_active_mask = 0;
    lru_flush(&r->texture_cache);
    pgraph_d3d12_process_deferred_releases(pg);
}

/*
 * Titles that sample their Z24S8 target's raw bytes through a 32-bit colour
 * format: a live or stashed Z24S8 surface laid out exactly like the
 * texture. The guest words are rebuilt on the GPU instead of read back.
 */
static PGRAPHD3D12SurfaceBinding *texture_zeta_view_source(
    NV2AState *d, hwaddr texture_addr, const TextureShape *shape,
    const PGRAPHD3D12TextureFormatInfo *info)
{
    PGRAPHD3D12State *r = d->pgraph.d3d12_renderer_state;
    BasicColorFormatInfo basic =
        kelvin_color_format_info_map[shape->color_format];
    PGRAPHD3D12SurfaceBinding *surface;

    if (shape->border || shape->cubemap || shape->dimensionality != 2 ||
        texture_mip_levels_for_shape(shape) != 1 || info->converted ||
        info->compressed || info->upload_bytes_per_pixel != 4 ||
        texture_resource_format(info) != info->dxgi_format ||
        (info->dxgi_format != DXGI_FORMAT_R8G8B8A8_UNORM &&
         info->dxgi_format != DXGI_FORMAT_B8G8R8A8_UNORM)) {
        return NULL;
    }

    for (unsigned int pass = 0; pass < 2; pass++) {
        for (surface = pass ? QTAILQ_LAST(&r->stashed_surfaces) :
                              QTAILQ_FIRST(&r->surfaces);
             surface != NULL;
             surface = pass ? QTAILQ_PREV(surface, entry) :
                              QTAILQ_NEXT(surface, entry)) {
            if (!surface->color && surface->resource != NULL &&
                surface->fmt.dxgi_format == DXGI_FORMAT_D24_UNORM_S8_UINT &&
                surface->vram_addr == texture_addr &&
                surface->width == shape->width &&
                surface->height == shape->height &&
                surface->swizzle == !basic.linear &&
                (surface->swizzle || surface->pitch == shape->pitch)) {
                return surface;
            }
        }
    }
    return NULL;
}

/* Draws the zeta surface's guest words into the texture resource. */
static bool render_zeta_view(NV2AState *d,
                             PGRAPHD3D12TextureBinding *binding,
                             PGRAPHD3D12SurfaceBinding *surface,
                             Error **errp)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    D3D12_RESOURCE_STATES surface_state = surface->state;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc;
    D3D12_RENDER_TARGET_VIEW_DESC rtv_desc;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    ID3D12GraphicsCommandList *list;
    ID3D12PipelineState *pipeline_state;
    uint32_t srv_index[2];
    uint32_t rtv_index;
    unsigned int width = MAX(surface->width, 1);
    unsigned int height = MAX(surface->height, 1);
    D3D12_VIEWPORT viewport;
    D3D12_RECT scissor;

    pipeline_state =
        pgraph_d3d12_get_zeta_view_pipeline(pg, binding->dxgi_format, errp);
    if (pipeline_state == NULL || binding->resource == NULL ||
        !pgraph_d3d12_begin_commands(pg, errp)) {
        if (pipeline_state != NULL && binding->resource == NULL) {
            error_setg(errp, "Direct3D 12 zeta view has no texture "
                       "resource");
        }
        return false;
    }
    if (!pgraph_d3d12_alloc_srv_descriptor(r, &srv_index[0], errp)) {
        return false;
    }
    if (!pgraph_d3d12_alloc_srv_descriptor(r, &srv_index[1], errp)) {
        pgraph_d3d12_free_srv_descriptor(r, srv_index[0]);
        return false;
    }
    if (r->num_free_rtv_descriptors > 0) {
        rtv_index = r->free_rtv_descriptors[--r->num_free_rtv_descriptors];
    } else if (r->next_rtv_descriptor < PGRAPH_D3D12_RTV_HEAP_SIZE) {
        rtv_index = r->next_rtv_descriptor++;
    } else {
        pgraph_d3d12_free_srv_descriptor(r, srv_index[1]);
        pgraph_d3d12_free_srv_descriptor(r, srv_index[0]);
        error_setg(errp, "Direct3D 12 RTV descriptor heap exhausted");
        return false;
    }

    memset(&srv_desc, 0, sizeof(srv_desc));
    srv_desc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Shader4ComponentMapping =
        D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.Texture2D.MipLevels = 1;
    r->device->lpVtbl->CreateShaderResourceView(
        r->device, surface->resource, &srv_desc,
        cpu_descriptor_handle(r->cbv_srv_uav_heap,
                              r->cbv_srv_uav_descriptor_size, srv_index[0]));
    srv_desc.Format = DXGI_FORMAT_X24_TYPELESS_G8_UINT;
    srv_desc.Texture2D.PlaneSlice = 1;
    r->device->lpVtbl->CreateShaderResourceView(
        r->device, surface->resource, &srv_desc,
        cpu_descriptor_handle(r->cbv_srv_uav_heap,
                              r->cbv_srv_uav_descriptor_size, srv_index[1]));

    memset(&rtv_desc, 0, sizeof(rtv_desc));
    rtv_desc.Format = binding->dxgi_format;
    rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    rtv = cpu_descriptor_handle(r->rtv_heap, r->rtv_descriptor_size,
                                rtv_index);
    r->device->lpVtbl->CreateRenderTargetView(r->device, binding->resource,
                                              &rtv_desc, rtv);

    /* The texture resource carries the surface scale. */
    pgraph_apply_scaling_factor(pg, &width, &height);
    viewport = (D3D12_VIEWPORT){
        .Width = (FLOAT)width,
        .Height = (FLOAT)height,
        .MaxDepth = 1.0f,
    };
    scissor = (D3D12_RECT){ 0, 0, (LONG)width, (LONG)height };

    pgraph_d3d12_transition_raw_resource(
        r, surface->resource, &surface->state,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    pgraph_d3d12_transition_raw_resource(r, binding->resource,
                                         &binding->state,
                                         D3D12_RESOURCE_STATE_RENDER_TARGET);

    list = r->command_list;
    if (!r->record_root_signature_set) {
        list->lpVtbl->SetGraphicsRootSignature(list, r->draw_root_signature);
        r->record_root_signature_set = true;
    }
    list->lpVtbl->SetGraphicsRootDescriptorTable(
        list, PGRAPH_D3D12_ROOT_TEXTURE_SRV(0),
        gpu_descriptor_handle(r->cbv_srv_uav_heap,
                              r->cbv_srv_uav_descriptor_size, srv_index[0]));
    list->lpVtbl->SetGraphicsRootDescriptorTable(
        list, PGRAPH_D3D12_ROOT_TEXTURE_SRV(1),
        gpu_descriptor_handle(r->cbv_srv_uav_heap,
                              r->cbv_srv_uav_descriptor_size, srv_index[1]));
    list->lpVtbl->OMSetRenderTargets(list, 1, &rtv, FALSE, NULL);
    list->lpVtbl->RSSetViewports(list, 1, &viewport);
    list->lpVtbl->RSSetScissorRects(list, 1, &scissor);
    list->lpVtbl->SetPipelineState(list, pipeline_state);
    list->lpVtbl->IASetPrimitiveTopology(list,
                                         D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->lpVtbl->DrawInstanced(list, 3, 1, 0, 0);

    pgraph_d3d12_transition_raw_resource(
        r, binding->resource, &binding->state,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    pgraph_d3d12_transition_raw_resource(r, surface->resource,
                                         &surface->state, surface_state);

    /* The next draw rebinds its own tables, targets and pipeline. */
    r->record_texture_srv_gpu[0] = 0;
    r->record_texture_srv_gpu[1] = 0;
    r->record_render_targets_valid = false;
    r->record_viewport_valid = false;
    r->record_pipeline_state = pipeline_state;
    r->record_topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    r->record_topology_valid = true;

    pgraph_d3d12_defer_release(pg, NULL, (int32_t)srv_index[0],
                               (int32_t)rtv_index, -1, -1);
    pgraph_d3d12_defer_release(pg, NULL, (int32_t)srv_index[1], -1, -1,
                               -1);
    r->perf.tex_zeta_view++;
    return true;
}

/*
 * Diagnostic (event log window only): statistics of a 16-bit depth texture
 * just copied from a zeta surface. Reads it back and waits for the GPU.
 */
static void evlog_depth_texture_stats(PGRAPHState *pg,
                                      PGRAPHD3D12TextureBinding *binding)
{
    /* Deferred: each copy is read back once its fence completes, so the
     * diagnostic never stalls the GPU. */
    static struct {
        ID3D12Resource *buffer;
        uint8_t *mapped;
        uint64_t capacity;
        uint64_t fence;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
        unsigned int frame;
        hwaddr vram_addr;
        bool pending;
    } slots[4];
    static unsigned int next_slot;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    D3D12_RESOURCE_DESC desc;
    D3D12_TEXTURE_COPY_LOCATION dst, src;
    UINT64 total = 0;
    unsigned int scale = MAX((unsigned int)binding->scale, 1);
    uint64_t completed = r->fence->lpVtbl->GetCompletedValue(r->fence);

    for (unsigned int i = 0; i < ARRAY_SIZE(slots); i++) {
        uint64_t far_count = 0, sum = 0, count = 0;
        unsigned int minv = 0xffff, maxv = 0;

        if (!slots[i].pending || slots[i].fence == 0 ||
            slots[i].fence > completed) {
            continue;
        }
        for (unsigned int y = 0; y < slots[i].fp.Footprint.Height; y++) {
            const uint16_t *row = (const uint16_t *)(
                slots[i].mapped + slots[i].fp.Offset +
                (uint64_t)y * slots[i].fp.Footprint.RowPitch);

            for (unsigned int x = 0; x < slots[i].fp.Footprint.Width; x++) {
                unsigned int v = row[x];

                if (v == 0xffff) {
                    far_count++;
                    continue;
                }
                minv = MIN(minv, v);
                maxv = MAX(maxv, v);
                sum += v;
                count++;
            }
        }
        fprintf(stderr,
                "EV zstat rec=%u tex=0x%" HWADDR_PRIx
                " far=%.2f%% min=%u max=%u mean=%.1f\n",
                slots[i].frame, slots[i].vram_addr,
                100.0 * far_count / MAX(far_count + count, 1), minv, maxv,
                count ? (double)sum / count : 0.0);
        slots[i].pending = false;
    }

    if (binding == NULL || binding->resource == NULL ||
        binding->resource_format != DXGI_FORMAT_R16_TYPELESS) {
        return;
    }
    unsigned int s = next_slot;
    if (slots[s].pending) {
        return;
    }
    memset(&desc, 0, sizeof(desc));
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = binding->shape.width * scale;
    desc.Height = binding->shape.height * scale;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R16_TYPELESS;
    desc.SampleDesc.Count = 1;
    r->device->lpVtbl->GetCopyableFootprints(r->device, &desc, 0, 1, 0,
                                             &fp, NULL, NULL, &total);
    if (slots[s].buffer == NULL || slots[s].capacity < total) {
        D3D12_HEAP_PROPERTIES heap = { .Type = D3D12_HEAP_TYPE_READBACK,
                                       .CreationNodeMask = 1,
                                       .VisibleNodeMask = 1 };
        D3D12_RESOURCE_DESC bdesc = {
            .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
            .Width = total, .Height = 1, .DepthOrArraySize = 1,
            .MipLevels = 1, .SampleDesc.Count = 1,
            .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
        };

        if (slots[s].buffer != NULL) {
            return;
        }
        if (FAILED(r->device->lpVtbl->CreateCommittedResource(
                r->device, &heap, D3D12_HEAP_FLAG_NONE, &bdesc,
                D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource,
                (void **)&slots[s].buffer)) ||
            FAILED(slots[s].buffer->lpVtbl->Map(slots[s].buffer, 0, NULL,
                                                (void **)&slots[s].mapped))) {
            slots[s].buffer = NULL;
            return;
        }
        slots[s].capacity = total;
    }
    if (!pgraph_d3d12_begin_commands(pg, NULL)) {
        return;
    }
    pgraph_d3d12_transition_raw_resource(r, binding->resource,
                                         &binding->state,
                                         D3D12_RESOURCE_STATE_COPY_SOURCE);
    memset(&dst, 0, sizeof(dst));
    dst.pResource = slots[s].buffer;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = fp;
    memset(&src, 0, sizeof(src));
    src.pResource = binding->resource;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    r->command_list->lpVtbl->CopyTextureRegion(r->command_list, &dst, 0, 0,
                                               0, &src, NULL);
    pgraph_d3d12_transition_raw_resource(
        r, binding->resource, &binding->state,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    /* The open list is signalled with the next fence value. */
    slots[s].fence = r->fence_value + 1;
    slots[s].fp = fp;
    slots[s].frame = g_nv2a_stats.frame_count;
    slots[s].vram_addr = binding->vram_addr;
    slots[s].pending = true;
    next_slot = (s + 1) % ARRAY_SIZE(slots);
}

/*
 * Samplers depend on the shape and four registers; address modes are not
 * covered by the texture dirty bit, so the registers are compared here.
 * `shape_changed` drops the stage's cached sampler.
 */
static bool bind_stage_sampler(PGRAPHState *pg, unsigned int stage,
                               const TextureShape *shape, bool shape_changed,
                               Error **errp)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    uint32_t regs[4] = {
        pgraph_reg_r(pg, NV_PGRAPH_TEXFILTER0 + stage * 4),
        pgraph_reg_r(pg, NV_PGRAPH_TEXADDRESS0 + stage * 4),
        pgraph_reg_r(pg, NV_PGRAPH_BORDERCOLOR0 + stage * 4),
        GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_TEXCTL0_0 + stage * 4),
                 NV_PGRAPH_TEXCTL0_0_MAX_ANISOTROPY),
    };

    if (!shape_changed && (r->texture_sampler_regs_valid & (1u << stage)) &&
        !memcmp(regs, r->texture_sampler_regs[stage], sizeof(regs))) {
        return true;
    }
    r->texture_sampler_regs_valid &= ~(1u << stage);
    if (!lookup_sampler(pg, shape, regs[0], regs[1], regs[2],
                        regs[3] >= 4 ? 16u : 1u << regs[3],
                        &r->texture_sampler_gpu[stage], errp)) {
        return false;
    }
    memcpy(r->texture_sampler_regs[stage], regs, sizeof(regs));
    r->texture_sampler_regs_valid |= 1u << stage;
    return true;
}

/* Everything the texture's address, shape and palette are derived from. */
static void read_texture_stage_regs(PGRAPHState *pg, unsigned int stage,
                                    uint32_t regs[8])
{
    regs[0] = pgraph_reg_r(pg, NV_PGRAPH_TEXOFFSET0 + stage * 4);
    regs[1] = pgraph_reg_r(pg, NV_PGRAPH_TEXFMT0 + stage * 4);
    regs[2] = pgraph_reg_r(pg, NV_PGRAPH_TEXCTL0_0 + stage * 4);
    regs[3] = pgraph_reg_r(pg, NV_PGRAPH_TEXCTL1_0 + stage * 4);
    regs[4] = pgraph_reg_r(pg, NV_PGRAPH_TEXIMAGERECT0 + stage * 4);
    regs[5] = pgraph_reg_r(pg, NV_PGRAPH_TEXPALETTE0 + stage * 4);
    regs[6] = pg->dma_a;
    regs[7] = pg->dma_b;
}

/*
 * Unchanged registers on a texture validated with no surface over its bytes:
 * while the surface layout is unchanged only guest writes can stale it, and
 * those leave dirty pages. The registers are compared rather than trusting
 * the dirty bit, which every texture method sets even when rewriting the
 * same value. Every other source of that bit drops the binding or marks it
 * possibly dirty.
 */
static bool texture_stage_unchanged(NV2AState *d, unsigned int stage)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    PGRAPHD3D12TextureBinding *binding = r->texture_binding[stage];
    uint32_t regs[8];

    if (binding == NULL) {
        return false;
    }
    read_texture_stage_regs(pg, stage, regs);
    return !memcmp(regs, r->texture_stage_regs[stage], sizeof(regs)) &&
           binding->valid &&
           binding->resource != NULL && binding->surface_free &&
           binding->surface_free_gen == r->surface_layout_gen &&
           !binding->possibly_dirty &&
           !check_texture_possibly_dirty(d, binding->vram_addr,
                                         binding->length,
                                         binding->palette_vram_addr,
                                         binding->palette_length, NULL,
                                         NULL);
}

/*
 * vCPU writes to a surface's guest bytes since the last look, from the
 * DIRTY_MEMORY_NV2A bitmap (also read when the surface is bound as a
 * target). Only whole pages inside the surface count: the first and last
 * page may hold other data that is written while the surface stays put.
 */
static bool surface_guest_bytes_written(NV2AState *d,
                                        PGRAPHD3D12SurfaceBinding *surface)
{
    hwaddr start = TARGET_PAGE_ALIGN(surface->vram_addr);
    hwaddr end = (surface->vram_addr + surface->size) & TARGET_PAGE_MASK;

    if (!tcg_enabled() || surface->size == 0) {
        return false;
    }
    if (end <= start) {
        start = surface->vram_addr;
        end = surface->vram_addr + surface->size;
    }
    return memory_region_test_and_clear_dirty(d->vram, start, end - start,
                                              DIRTY_MEMORY_NV2A);
}

static bool bind_texture_stage(NV2AState *d, unsigned int stage, Error **errp)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    PGRAPHD3D12TextureBinding *binding;
    PGRAPHD3D12TextureKey key;
    LruNode *node;
    uint32_t texfmt = pgraph_reg_r(pg, NV_PGRAPH_TEXFMT0 + stage * 4);
    unsigned int color_format = GET_MASK(texfmt, NV_PGRAPH_TEXFMT0_COLOR);
    TextureShape shape;
    PGRAPHD3D12TextureFormatInfo info;
    hwaddr vram_addr;
    size_t length;
    uint64_t vram_size = memory_region_size(d->vram);
    BasicColorFormatInfo basic;
    unsigned int dimensionality;
    bool needs_upload;
    bool memory_dirty;
    bool texture_state_dirty;
    bool content_needs_revalidation;
    bool vram_dirty_checked;
    bool vram_dirty;
    bool guest_memory_dirty;
    bool texture_memory_dirty = false;
    bool palette_memory_dirty = false;
    bool overlap_surface_downloaded;
    bool palette_surface_downloaded = false;
    bool source_surface_changed;
    bool gpu_copy_surface;
    PGRAPHD3D12SurfaceBinding *level_surfaces[6 * 16];
    unsigned int level_x[6 * 16];
    unsigned int level_y[6 * 16];
    unsigned int level_surface_count = 0;
    int source_draw_time = -1;
    bool source_upload_pending_before;
    bool sources_include_stash;
    bool was_surface_to_texture;
    hwaddr previous_source_surface_addr;
    int previous_source_surface_draw_time;
    uint64_t data_hash;
    /* Texels and palette hashed apart, as custom texture files name them. */
    PGRAPHD3D12TexturePackHash pack_hash;
    hwaddr palette_vram_addr = 0;
    size_t palette_length = 0;
    bool resource_created = false;
    PGRAPHD3D12SurfaceBinding *source_surface = NULL;
    unsigned int target_resource_scale = 1;
    bool zeta_view = false;

    if (!pgraph_is_texture_stage_active(pg, stage) ||
        !pgraph_is_texture_enabled(pg, stage)) {
        return true;
    }

    if (texture_stage_unchanged(d, stage)) {
        binding = r->texture_binding[stage];
        if (!bind_stage_sampler(pg, stage, &binding->shape, false, errp)) {
            return false;
        }
        r->perf.tex_fast++;
        r->texture_scale[stage] = binding->scale;
        pg->texture_dirty[stage] = false;
        r->texture_active_mask |= 1u << stage;
        return true;
    }

    if (color_format >= ARRAY_SIZE(kelvin_color_format_info_map) ||
        kelvin_color_format_info_map[color_format].bytes_per_pixel == 0) {
        report_texture_unsupported_format(
            d, stage, color_format,
            "no shared NV2A color format metadata exists for this value");
        return true;
    }

    basic = kelvin_color_format_info_map[color_format];
    dimensionality = GET_MASK(texfmt, NV_PGRAPH_TEXFMT0_DIMENSIONALITY);
    if (basic.linear && dimensionality != 2) {
        TextureShape partial;

        memset(&partial, 0, sizeof(partial));
        partial.color_format = color_format;
        partial.dimensionality = dimensionality;
        partial.cubemap = GET_MASK(texfmt, NV_PGRAPH_TEXFMT0_CUBEMAPENABLE);
        partial.levels = GET_MASK(texfmt, NV_PGRAPH_TEXFMT0_MIPMAP_LEVELS);
        partial.width = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_TEXIMAGERECT0 +
                                              stage * 4),
                                 NV_PGRAPH_TEXIMAGERECT0_WIDTH);
        partial.height = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_TEXIMAGERECT0 +
                                               stage * 4),
                                  NV_PGRAPH_TEXIMAGERECT0_HEIGHT);
        partial.depth = 1;
        partial.pitch = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_TEXCTL1_0 +
                                              stage * 4),
                                 NV_PGRAPH_TEXCTL1_0_IMAGE_PITCH);
        report_texture_shape_skip(
            d, stage, TEXTURE_SKIP_DIMENSION, &partial, 0, 0,
            "linear texture dimensionality is not 2D, which the shared "
            "texture shape helper would reject");
        return true;
    }

    shape = pgraph_get_texture_shape(pg, stage);
    vram_addr = pgraph_get_texture_phys_addr(pg, stage);
    length = texture_source_length(pg, &shape);
    if (!texture_palette_range(d, stage, &shape, &palette_vram_addr,
                               &palette_length, errp)) {
        report_texture_shape_skip(
            d, stage, TEXTURE_SKIP_VRAM_RANGE, &shape, vram_addr, length,
            errp != NULL && *errp != NULL ?
                error_get_pretty(*errp) :
                "indexed texture palette memory range is invalid");
        return false;
    }

    if (shape.dimensionality != 2 && shape.dimensionality != 3) {
        report_texture_shape_skip(d, stage, TEXTURE_SKIP_DIMENSION, &shape,
                                  vram_addr, length,
                                  "texture dimensionality is neither 2D nor "
                                  "3D, so D3D12 cannot create a matching SRV");
        return true;
    }

    if (shape.dimensionality == 3 && shape.cubemap) {
        report_texture_shape_skip(d, stage, TEXTURE_SKIP_CUBEMAP, &shape,
                                  vram_addr, length,
                                  "3D cubemap texture is not a valid D3D12 "
                                  "resource shape");
        return true;
    }

    if (shape.width == 0 || shape.height == 0 ||
        (shape.dimensionality == 3 && shape.depth == 0)) {
        report_texture_shape_skip(d, stage, TEXTURE_SKIP_ZERO_SIZE, &shape,
                                  vram_addr, length, "zero-sized texture");
        return true;
    }

    if (!texture_format_for_shape(&shape, &info)) {
        report_texture_unsupported_format(
            d, stage, shape.color_format,
            "there is no D3D12 DXGI upload/SRV mapping for this format yet");
        return true;
    }

    if (length > vram_size || vram_addr > vram_size - length) {
        report_texture_shape_skip(d, stage, TEXTURE_SKIP_VRAM_RANGE, &shape,
                                  vram_addr, length,
                                  "texture memory range is outside guest "
                                  "VRAM");
        return true;
    }

    source_surface = texture_source_surface(d, vram_addr, &shape);
    if (r->tune_flags & PGRAPH_D3D12_TUNE_S2T_OFF) {
        source_surface = NULL;
    }
    texture_format_use_surface_resource(&info, source_surface, &shape);
    gpu_copy_surface =
        surface_texture_gpu_copy_supported(pg, source_surface, &shape, &info);
    if (r->tune_flags & PGRAPH_D3D12_TUNE_S2T_OFF) {
        gpu_copy_surface = false;
    }
    if (gpu_copy_surface) {
        level_surfaces[0] = source_surface;
        level_x[0] = 0;
        level_y[0] = 0;
        level_surface_count = 1;
    } else if (source_surface == NULL &&
               !(r->tune_flags & (PGRAPH_D3D12_TUNE_S2T_OFF |
                                  PGRAPH_D3D12_TUNE_S2T_LEVELS_OFF))) {
        level_surface_count = texture_level_surfaces(
            d, vram_addr, &shape, &info, level_surfaces, level_x, level_y,
            ARRAY_SIZE(level_surfaces));
        if (level_surface_count != 0) {
            source_surface = level_surfaces[0];
            gpu_copy_surface = true;
        } else if (!(r->tune_flags & PGRAPH_D3D12_TUNE_ZVIEW_READBACK) &&
                   (source_surface = texture_zeta_view_source(
                        d, vram_addr, &shape, &info)) != NULL) {
            level_surfaces[0] = source_surface;
            level_x[0] = 0;
            level_y[0] = 0;
            level_surface_count = 1;
            gpu_copy_surface = true;
            zeta_view = true;
        }
    }
    target_resource_scale =
        gpu_copy_surface ? MAX(pg->surface_scale_factor, 1) : 1;
    sources_include_stash =
        gpu_copy_surface &&
        texture_sources_include_stash(r, level_surfaces, level_surface_count);

    source_upload_pending_before = false;
    for (unsigned int level = 0; level < level_surface_count; level++) {
        PGRAPHD3D12SurfaceBinding *level_surface = level_surfaces[level];

        /* A clean surface has its watchpoint off, so vCPU writes to it (a
         * movie frame written into a surface-backed texture) only show in
         * the dirty bitmap. Its guest bytes are current, so take them. */
        if (!level_surface->upload_pending && !level_surface->draw_dirty &&
            surface_guest_bytes_written(d, level_surface)) {
            level_surface->upload_pending = true;
        }
        if (!level_surface->upload_pending) {
            continue;
        }
        source_upload_pending_before = true;
        if (!pgraph_d3d12_upload_surface_data(d, level_surface, false,
                                              errp)) {
            report_texture_shape_skip(
                d, stage, TEXTURE_SKIP_UPLOAD, &shape, vram_addr, length,
                errp != NULL && *errp != NULL ?
                    error_get_pretty(*errp) :
                    "surface upload before surface-to-texture copy failed");
            return false;
        }
    }

    /* After the uploads above: an upload advances the source's draw time. */
    for (unsigned int level = 0; level < level_surface_count; level++) {
        source_draw_time = MAX(source_draw_time,
                               level_surfaces[level]->draw_time);
    }

    texture_state_dirty = pg->texture_dirty[stage];
    guest_memory_dirty = false;
    if (!gpu_copy_surface) {
        /* Texture dirty bits only force a re-hash; surface ownership changes
         * come from the access callback and the render-target bind check,
         * because the renderer's own downloads set these bits too. */
        guest_memory_dirty = check_texture_possibly_dirty(
            d, vram_addr, length, palette_vram_addr, palette_length,
            &texture_memory_dirty, &palette_memory_dirty);
    }

    if (!download_gpu_owned_overlapping_surfaces(
            d, vram_addr, length, &shape, level_surfaces,
            level_surface_count, &overlap_surface_downloaded, errp)) {
        return false;
    }
    if (palette_length != 0 &&
        !download_gpu_owned_overlapping_surfaces(
            d, palette_vram_addr, palette_length, NULL, NULL, 0,
            &palette_surface_downloaded, errp)) {
        return false;
    }
    overlap_surface_downloaded |= palette_surface_downloaded;

    if (sources_include_stash) {
        if (!texture_sources_alive(r, level_surfaces, level_surface_count)) {
            /* The stash's bytes are in guest memory now; bind again. */
            r->perf.tex_stash_retry++;
            return bind_texture_stage(d, stage, errp);
        }
        r->perf.tex_from_stash++;
    }

    memset(&key, 0, sizeof(key));
    key.shape = shape;
    key.vram_addr = vram_addr;
    key.length = length;
    key.palette_vram_addr = palette_vram_addr;
    key.palette_length = palette_length;
    key.resource_format = texture_resource_format(&info);
    key.dxgi_format = info.dxgi_format;
    key.upload_bytes_per_pixel = info.upload_bytes_per_pixel;
    key.scale = target_resource_scale;
    key.converted = info.converted;
    key.compressed = info.compressed;
    /* Keep guest-backed and GPU surface-copy resources in separate cache
     * entries; sharing one would force a full hash/upload or GPU copy each
     * time a title alternates between the two views. */
    key.surface_to_texture = gpu_copy_surface;
    key.zeta_view = zeta_view;

    node = lru_lookup(&r->texture_cache,
                      fast_hash((const uint8_t *)&key, sizeof(key)), &key);
    binding = container_of(node, PGRAPHD3D12TextureBinding, node);

    was_surface_to_texture = binding->surface_to_texture;
    previous_source_surface_addr = binding->source_surface_addr;
    previous_source_surface_draw_time = binding->source_surface_draw_time;

    content_needs_revalidation =
        guest_memory_dirty || overlap_surface_downloaded ||
        binding->resource == NULL ||
        binding->possibly_dirty ||
        was_surface_to_texture != gpu_copy_surface;

    /* Titles overlap VRAM allocations (e.g. a palette in the tail pages of a
     * display surface); a guest write wins over the cached surface. */
    if (!gpu_copy_surface && overlap_surface_downloaded) {
        /* Consume the dirty notification produced by an exact readback. */
        guest_memory_dirty |= check_texture_possibly_dirty(
            d, vram_addr, length, palette_vram_addr, palette_length,
            NULL, NULL);
    }

    /* A matching hash means the cached copy is still valid. */
    data_hash = binding->data_hash;
    memset(&pack_hash, 0, sizeof(pack_hash));
    if (!gpu_copy_surface && content_needs_revalidation) {
        pack_hash.texels = fast_hash(d->vram_ptr + vram_addr, length);
        if (palette_length != 0) {
            pack_hash.palette = fast_hash(d->vram_ptr + palette_vram_addr,
                                          palette_length);
            pack_hash.has_palette = true;
        }
        data_hash = pack_hash.texels ^ pack_hash.palette;
    }

    /* The texels decide the custom texture; switching to or from one
     * changes the resource's size and format. */
    if (!gpu_copy_surface && content_needs_revalidation) {
        const PGRAPHD3D12ReplacementTexture *replacement =
            pgraph_d3d12_texture_replace_eligible(&shape, info.dxgi_format) ?
                pgraph_d3d12_texture_replacement_lookup(pg, &shape,
                                                        &pack_hash) :
                NULL;

        if (replacement != binding->replacement) {
            release_texture_resource(pg, binding);
            binding->replacement = replacement;
        }
    }

    if (binding->resource == NULL) {
        if (!create_texture_resource(pg, stage, binding, &shape, vram_addr,
                                     length, &info, target_resource_scale,
                                     zeta_view, errp)) {
            return false;
        }
        resource_created = true;
    }

    memory_dirty = !gpu_copy_surface && content_needs_revalidation &&
                   data_hash != binding->data_hash;
    vram_dirty_checked = !gpu_copy_surface;
    vram_dirty = memory_dirty;

    source_surface_changed =
        gpu_copy_surface &&
        (resource_created ||
         was_surface_to_texture != gpu_copy_surface ||
         binding->draw_time < source_draw_time ||
         previous_source_surface_addr != source_surface->vram_addr);
    /* A surface copy overwrites the cached resource without changing the
     * saved guest-data hash. Force the first transition back to guest VRAM
     * even when those guest bytes match the hash from before the GPU copy. */
    needs_upload = gpu_copy_surface ?
        (resource_created || source_surface_changed) :
        (resource_created || memory_dirty || was_surface_to_texture);
    if (gpu_copy_surface) {
        pgraph_d3d12_evlog(
            r, "s2t st=%u tex=0x%" HWADDR_PRIx " %ux%u cf=0x%x src=0x%"
            HWADDR_PRIx " %ux%u obj=%04x dt=%d bdt=%d copy=%d clr=%d "
            "dirty=%d up=%d n=%u stash=%d", stage, vram_addr, shape.width,
            shape.height, shape.color_format, source_surface->vram_addr,
            source_surface->width, source_surface->height,
            (unsigned int)(((uintptr_t)source_surface >> 4) & 0xffff),
            source_draw_time, binding->draw_time, needs_upload,
            source_surface->cleared, source_surface->draw_dirty,
            source_upload_pending_before, level_surface_count,
            sources_include_stash);
    }
    report_texture_binding_once(d, stage, &shape, vram_addr, length, &info,
                                binding, source_surface, needs_upload,
                                memory_dirty, source_surface_changed,
                                gpu_copy_surface, texture_state_dirty,
                                vram_dirty_checked, vram_dirty,
                                source_upload_pending_before,
                                was_surface_to_texture,
                                previous_source_surface_addr,
                                previous_source_surface_draw_time);
    if (needs_upload) {
        if (zeta_view) {
            if (!render_zeta_view(d, binding, level_surfaces[0], errp)) {
                report_texture_shape_skip(
                    d, stage, TEXTURE_SKIP_UPLOAD, &shape, vram_addr, length,
                    errp != NULL && *errp != NULL ?
                        error_get_pretty(*errp) :
                        "zeta view render failed");
                /* Recreate and refill on the next bind. */
                release_texture_resource(pg, binding);
                return false;
            }
            binding->draw_time = source_draw_time;
        } else if (gpu_copy_surface) {
            unsigned int mip_levels = texture_mip_levels_for_shape(&shape);

            for (unsigned int i = 0; i < level_surface_count; i++) {
                unsigned int level_w, level_h, level_d;

                texture_level_dimensions(&shape, i % mip_levels, &level_w,
                                         &level_h, &level_d);
                if (!copy_surface_to_texture_resource(
                        d, stage, binding, level_surfaces[i], i,
                        level_x[i], level_y[i],
                        level_surface_count > 1 ||
                                level_surfaces[i]->width != level_w ?
                            level_w : 0,
                        errp)) {
                    report_texture_shape_skip(
                        d, stage, TEXTURE_SKIP_UPLOAD, &shape, vram_addr,
                        length,
                        errp != NULL && *errp != NULL ?
                            error_get_pretty(*errp) :
                            "surface-to-texture copy failed");
                    /* Recreate and refill on the next bind. */
                    release_texture_resource(pg, binding);
                    return false;
                }
            }
            binding->draw_time = source_draw_time;
            if (r->evlog_frames_left > 0 && !level_surfaces[0]->color) {
                evlog_depth_texture_stats(pg, binding);
            }
        } else if (binding->replacement != NULL ?
                       !upload_replacement_texture(d, stage, binding, errp) :
                       !upload_texture_resource(d, stage, binding,
                                                &pack_hash, errp)) {
            report_texture_shape_skip(
                d, stage, TEXTURE_SKIP_UPLOAD, &shape, vram_addr, length,
                errp != NULL && *errp != NULL ? error_get_pretty(*errp) :
                                                "texture upload failed");
            /* Recreate and refill on the next bind. */
            release_texture_resource(pg, binding);
            return false;
        }
    }

    if (!gpu_copy_surface) {
        binding->data_hash = data_hash;
    }
    binding->possibly_dirty = false;
    binding->valid = true;
    binding->surface_to_texture = gpu_copy_surface;
    binding->source_surface_addr = source_surface != NULL ?
        source_surface->vram_addr : 0;
    binding->source_surface_draw_time = source_surface != NULL ?
        source_draw_time : -1;
    binding->surface_free =
        !gpu_copy_surface &&
        !texture_range_has_surface(r, vram_addr, length) &&
        (palette_length == 0 ||
         !texture_range_has_surface(r, palette_vram_addr, palette_length));
    binding->surface_free_gen = r->surface_layout_gen;

    if (!bind_stage_sampler(pg, stage, &shape, true, errp)) {
        return false;
    }

    r->texture_binding[stage] = binding;
    read_texture_stage_regs(pg, stage, r->texture_stage_regs[stage]);
    r->texture_scale[stage] = binding->scale;
    pg->texture_dirty[stage] = false;
    r->texture_active_mask |= 1u << stage;
    return true;
}

bool pgraph_d3d12_bind_textures(NV2AState *d, Error **errp)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;

    r->texture_active_mask = 0;

    /* Wrap the sampler heap before any stage takes a handle, so a later
     * stage's new sampler never overwrites an earlier stage's slot. */
    if (r->next_sampler_descriptor + NV2A_MAX_TEXTURES >
        PGRAPH_D3D12_SAMPLER_HEAP_SIZE) {
        r->perf.finish_sampler++;
        pgraph_d3d12_finish(pg);
        g_hash_table_remove_all(r->sampler_cache);
        r->next_sampler_descriptor = 0;
        r->texture_sampler_regs_valid = 0;
    }

    for (unsigned int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        if (!bind_texture_stage(d, i, errp)) {
            return false;
        }
    }

    return true;
}

void pgraph_d3d12_record_texture_bindings(PGRAPHState *pg)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    bool framebuffer_fetch;
    bool uber_ps;

    if (r == NULL || r->command_list == NULL || !r->command_list_open) {
        return;
    }

    framebuffer_fetch =
        r->draw_pipeline_key_valid &&
        r->draw_pipeline_key.shader_framebuffer_fetch &&
        r->framebuffer_copy_valid;
    /* The interpreter pixel shader declares every stage, so every stage's
     * tables point into the heaps; unbound stages get the null view. */
    uber_ps = pgraph_d3d12_interpreter_pipeline(r) &&
              r->uber_null_srv_gpu.ptr != 0;
    if (r->texture_active_mask == 0 && !framebuffer_fetch && !uber_ps) {
        return;
    }

    /* The descriptor heaps are bound once when the command list opens. */
    for (unsigned int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        if (!(r->texture_active_mask & (1u << i)) ||
            r->texture_binding[i] == NULL) {
            D3D12_GPU_DESCRIPTOR_HANDLE sampler;

            if (!uber_ps ||
                r->record_texture_srv_gpu[i] == r->uber_null_srv_gpu.ptr) {
                continue;
            }
            /* Never sampled; any sampler of the heap serves. */
            sampler = gpu_descriptor_handle(r->sampler_heap,
                                            r->sampler_descriptor_size, 0);
            r->command_list->lpVtbl->SetGraphicsRootDescriptorTable(
                r->command_list, PGRAPH_D3D12_ROOT_TEXTURE_SRV(i),
                r->uber_null_srv_gpu);
            r->command_list->lpVtbl->SetGraphicsRootDescriptorTable(
                r->command_list, PGRAPH_D3D12_ROOT_TEXTURE_SAMPLER(i),
                sampler);
            r->record_texture_srv_gpu[i] = r->uber_null_srv_gpu.ptr;
            r->record_texture_sampler_gpu[i] = sampler.ptr;
            continue;
        }
        if (r->record_texture_srv_gpu[i] ==
                r->texture_binding[i]->srv_gpu.ptr &&
            r->record_texture_sampler_gpu[i] ==
                r->texture_sampler_gpu[i].ptr) {
            continue;
        }
        r->command_list->lpVtbl->SetGraphicsRootDescriptorTable(
            r->command_list, PGRAPH_D3D12_ROOT_TEXTURE_SRV(i),
            r->texture_binding[i]->srv_gpu);
        r->command_list->lpVtbl->SetGraphicsRootDescriptorTable(
            r->command_list, PGRAPH_D3D12_ROOT_TEXTURE_SAMPLER(i),
            r->texture_sampler_gpu[i]);
        r->record_texture_srv_gpu[i] = r->texture_binding[i]->srv_gpu.ptr;
        r->record_texture_sampler_gpu[i] = r->texture_sampler_gpu[i].ptr;
    }
    if (framebuffer_fetch &&
        r->record_framebuffer_srv_gpu != r->framebuffer_copy_srv_gpu.ptr) {
        r->command_list->lpVtbl->SetGraphicsRootDescriptorTable(
            r->command_list, PGRAPH_D3D12_ROOT_FRAMEBUFFER_SRV,
            r->framebuffer_copy_srv_gpu);
        r->record_framebuffer_srv_gpu = r->framebuffer_copy_srv_gpu.ptr;
    }
}
