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
#include <math.h>
#include "qapi/error.h"
#include "hw/xbox/nv2a/pgraph/swizzle.h"
#include "renderer.h"
#include "cpu-stats-compat.h"

#define D3D12_SURFACE_TRANSFER_SLAB_SIZE (16 * 1024 * 1024)
#define D3D12_ZETA_TRANSFER_MAX_SIZE (128 * 1024 * 1024)
#define D3D12_ZETA_TRANSFER_CEILING (512 * 1024 * 1024)

static const PGRAPHD3D12SurfaceFormatInfo d3d12_color_surface_format_map[] = {
    [NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_Z1R5G5B5] =
        { 2, DXGI_FORMAT_B5G5R5A1_UNORM, false, false },
    [NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5] =
        { 2, DXGI_FORMAT_B5G6R5_UNORM, false, false },
    [NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_Z8R8G8B8] =
        { 4, DXGI_FORMAT_B8G8R8A8_UNORM, false, false },
    [NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8] =
        { 4, DXGI_FORMAT_B8G8R8A8_UNORM, false, false },
    [NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_O1R5G5B5] =
        { 2, DXGI_FORMAT_B5G5R5A1_UNORM, false, false },
    [NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_O8R8G8B8] =
        { 4, DXGI_FORMAT_B8G8R8A8_UNORM, false, false },
    [NV097_SET_SURFACE_FORMAT_COLOR_LE_B8] =
        { 1, DXGI_FORMAT_R8_UNORM, false, false },
    [NV097_SET_SURFACE_FORMAT_COLOR_LE_G8B8] =
        { 2, DXGI_FORMAT_R8G8_UNORM, false, false },
};

static const PGRAPHD3D12SurfaceFormatInfo d3d12_zeta_surface_format_map[] = {
    [NV097_SET_SURFACE_FORMAT_ZETA_Z16] =
        { 2, DXGI_FORMAT_D16_UNORM, true, false },
    [NV097_SET_SURFACE_FORMAT_ZETA_Z24S8] =
        { 4, DXGI_FORMAT_D24_UNORM_S8_UINT, true, true },
};

typedef struct D3D12ZetaPlaneLayout {
    DXGI_FORMAT depth_format;
    DXGI_FORMAT stencil_format;
    uint32_t depth_row_pitch;
    uint32_t stencil_row_pitch;
    uint64_t depth_offset;
    uint64_t stencil_offset;
    uint64_t depth_size;
    uint64_t stencil_size;
    uint64_t total_size;
    bool has_stencil;
} D3D12ZetaPlaneLayout;

static const char *color_surface_format_name(unsigned int format)
{
    switch (format) {
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_Z1R5G5B5:
        return "LE_X1R5G5B5_Z1R5G5B5";
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5:
        return "LE_R5G6B5";
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_Z8R8G8B8:
        return "LE_X8R8G8B8_Z8R8G8B8";
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1A7R8G8B8_Z1A7R8G8B8:
        return "LE_X1A7R8G8B8_Z1A7R8G8B8";
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8:
        return "LE_A8R8G8B8";
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_B8:
        return "LE_B8";
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_G8B8:
        return "LE_G8B8";
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_O1R5G5B5:
        return "LE_X1R5G5B5_O1R5G5B5";
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_O8R8G8B8:
        return "LE_X8R8G8B8_O8R8G8B8";
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1A7R8G8B8_O1A7R8G8B8:
        return "LE_X1A7R8G8B8_O1A7R8G8B8";
    default:
        return "unknown";
    }
}

static const char *zeta_surface_format_name(unsigned int format)
{
    switch (format) {
    case NV097_SET_SURFACE_FORMAT_ZETA_Z16:
        return "Z16";
    case NV097_SET_SURFACE_FORMAT_ZETA_Z24S8:
        return "Z24S8";
    default:
        return "unknown";
    }
}

static const char *surface_type_name(unsigned int type)
{
    switch (type) {
    case NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE:
        return "swizzled";
    case NV097_SET_SURFACE_FORMAT_TYPE_PITCH:
        return "linear";
    default:
        return "unknown";
    }
}

static void report_surface_error(const char *operation,
                                 const PGRAPHD3D12SurfaceBinding *surface,
                                 Error *err)
{
    /* Persistent failures (e.g. scaled size over the adapter budget) recur
     * every frame; throttle the reports. */
    static unsigned int reports;

    if (reports >= 64) {
        return;
    }
    reports++;

    fprintf(stderr,
            "Direct3D 12 renderer: %s failed for %s surface addr=0x%"
            HWADDR_PRIx " %ux%u pitch=%u bytes_per_pixel=%u dxgi=%u "
            "layout=%s draw_dirty=%d upload_pending=%d download_pending=%d: "
            "%s\n",
            operation, surface->color ? "color" : "zeta",
            surface->vram_addr, surface->width, surface->height,
            surface->pitch, surface->fmt.bytes_per_pixel,
            (unsigned int)surface->fmt.dxgi_format,
            surface->swizzle ? "swizzled" : "linear", surface->draw_dirty,
            surface->upload_pending, surface->download_pending,
            err ? error_get_pretty(err) : "unknown error");

    if (reports == 64) {
        fprintf(stderr,
                "Direct3D 12 renderer: further surface transfer error "
                "reports suppressed for this session\n");
    }
}

void pgraph_d3d12_surface_mark_cpu_owned(
    PGRAPHD3D12SurfaceBinding *surface)
{
    if (surface == NULL) {
        return;
    }
    surface->draw_dirty = false;
}

static void invalidate_vertex_ram_bitmap_range(PGRAPHD3D12State *r,
                                               hwaddr addr, uint64_t size)
{
    uint64_t end;
    size_t first_page;
    size_t end_page;

    if (r->uploaded_bitmap == NULL || r->bitmap_size == 0 || size == 0) {
        return;
    }

    end = addr > UINT64_MAX - size ? UINT64_MAX : addr + size;
    first_page = addr / TARGET_PAGE_SIZE;
    end_page = MIN((uint64_t)r->bitmap_size,
                   DIV_ROUND_UP(end, TARGET_PAGE_SIZE));
    if (first_page < end_page) {
        bitmap_clear(r->uploaded_bitmap, first_page, end_page - first_page);
    }
}

static void invalidate_surface_vertex_ram_cache(
    PGRAPHD3D12State *r, const PGRAPHD3D12SurfaceBinding *surface)
{
    uint64_t row_bytes;

    if (surface->swizzle) {
        invalidate_vertex_ram_bitmap_range(r, surface->vram_addr,
                                           surface->size);
        return;
    }

    row_bytes = (uint64_t)surface->width * surface->fmt.bytes_per_pixel;
    for (unsigned int y = 0; y < surface->height; y++) {
        invalidate_vertex_ram_bitmap_range(
            r, surface->vram_addr + (uint64_t)y * surface->pitch, row_bytes);
    }
}

static void report_unsupported_surface_format_once(NV2AState *d, bool color,
                                                   unsigned int width,
                                                   unsigned int height)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    Surface *surface = color ? &pg->surface_color : &pg->surface_zeta;
    unsigned int format = color ? pg->surface_shape.color_format :
                                  pg->surface_shape.zeta_format;
    uint64_t bit = format < 64 ? 1ull << format : 0;

    if (color) {
        if (bit && (r->reported_unsupported_color_surface_formats & bit)) {
            return;
        }
        if (bit) {
            r->reported_unsupported_color_surface_formats |= bit;
        }
    } else {
        if (bit && (r->reported_unsupported_zeta_surface_formats & bit)) {
            return;
        }
        if (bit) {
            r->reported_unsupported_zeta_surface_formats |= bit;
        }
    }

    fprintf(stderr,
            "Direct3D 12 renderer: unsupported %s surface format %s (0x%x), "
            "requested %ux%u, pitch=%u, offset=0x%" HWADDR_PRIx
            ", surface_type=%s (%u), z_format=%u\n",
            color ? "color" : "zeta",
            color ? color_surface_format_name(format) :
                    zeta_surface_format_name(format),
            format, width, height, surface->pitch, (hwaddr)surface->offset,
            surface_type_name(pg->surface_type), pg->surface_type,
            pg->surface_shape.z_format);
}

static D3D12_CPU_DESCRIPTOR_HANDLE descriptor_handle(
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

static bool framebuffer_dirty(PGRAPHState const *pg)
{
    bool shape_changed = memcmp(&pg->surface_shape, &pg->last_surface_shape,
                                sizeof(SurfaceShape)) != 0;

    return shape_changed && (pg->surface_shape.color_format ||
                             pg->surface_shape.zeta_format);
}

static void get_surface_dimensions(PGRAPHState const *pg, unsigned int *width,
                                   unsigned int *height)
{
    bool swizzle = (pg->surface_type == NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE);

    if (swizzle) {
        *width = 1 << pg->surface_shape.log_width;
        *height = 1 << pg->surface_shape.log_height;
    } else {
        *width = pg->surface_shape.clip_width;
        *height = pg->surface_shape.clip_height;
    }
}

static bool surface_format_for_target(
    PGRAPHState const *pg, bool color, PGRAPHD3D12SurfaceFormatInfo *fmt)
{
    unsigned int index = color ? pg->surface_shape.color_format :
                                 pg->surface_shape.zeta_format;

    if (color) {
        if (index >= ARRAY_SIZE(d3d12_color_surface_format_map)) {
            return false;
        }
        *fmt = d3d12_color_surface_format_map[index];
    } else {
        if (index >= ARRAY_SIZE(d3d12_zeta_surface_format_map)) {
            return false;
        }
        *fmt = d3d12_zeta_surface_format_map[index];
    }

    return fmt->bytes_per_pixel != 0 &&
           fmt->dxgi_format != DXGI_FORMAT_UNKNOWN;
}

static void copy_scaled_rows(uint8_t *dst, uint32_t dst_pitch,
                             const uint8_t *src, uint32_t src_pitch,
                             unsigned int width, unsigned int height,
                             unsigned int bytes_per_pixel, unsigned int scale)
{
    if (scale <= 1) {
        uint32_t row_bytes = width * bytes_per_pixel;

        for (unsigned int y = 0; y < height; y++) {
            memcpy(dst + y * dst_pitch, src + y * src_pitch, row_bytes);
        }
        return;
    }

    /* dst is write-combined upload memory: expand each row in cached
     * memory and only write it there. */
    size_t row_size = (size_t)width * scale * bytes_per_pixel;
    g_autofree uint8_t *row = g_malloc(row_size);

    for (unsigned int y = 0; y < height; y++) {
        const uint8_t *src_row = src + y * src_pitch;
        uint8_t *dst_row = row;

        switch (bytes_per_pixel) {
        case 4: {
            const uint32_t *in = (const uint32_t *)src_row;
            uint32_t *out = (uint32_t *)dst_row;

            for (unsigned int x = 0; x < width; x++) {
                uint32_t v = in[x];
                for (unsigned int i = 0; i < scale; i++) {
                    *out++ = v;
                }
            }
            break;
        }
        case 2: {
            const uint16_t *in = (const uint16_t *)src_row;
            uint16_t *out = (uint16_t *)dst_row;

            for (unsigned int x = 0; x < width; x++) {
                uint16_t v = in[x];
                for (unsigned int i = 0; i < scale; i++) {
                    *out++ = v;
                }
            }
            break;
        }
        default:
            for (unsigned int x = 0; x < width; x++) {
                for (unsigned int i = 0; i < scale; i++) {
                    memcpy(dst_row + (x * scale + i) * bytes_per_pixel,
                           src_row + x * bytes_per_pixel, bytes_per_pixel);
                }
            }
            break;
        }
        for (unsigned int i = 0; i < scale; i++) {
            memcpy(dst + ((uint64_t)y * scale + i) * dst_pitch, row,
                   row_size);
        }
    }
}

static void copy_shrunk_rows(uint8_t *dst, uint32_t dst_pitch,
                             const uint8_t *src, uint32_t src_pitch,
                             unsigned int width, unsigned int height,
                             unsigned int bytes_per_pixel, unsigned int scale)
{
    if (scale <= 1) {
        uint32_t row_bytes = width * bytes_per_pixel;

        for (unsigned int y = 0; y < height; y++) {
            memcpy(dst + y * dst_pitch, src + y * src_pitch, row_bytes);
        }
        return;
    }

    for (unsigned int y = 0; y < height; y++) {
        const uint8_t *src_row = src + y * scale * src_pitch;
        uint8_t *dst_row = dst + y * dst_pitch;

        for (unsigned int x = 0; x < width; x++) {
            memcpy(dst_row + x * bytes_per_pixel,
                   src_row + x * scale * bytes_per_pixel,
                   bytes_per_pixel);
        }
    }
}

/*
 * Float-format Z16 zeta is kept as D16_UNORM; guest memory holds the
 * normalised depth as IEEE binary16.
 */
static bool surface_is_float_z16(const PGRAPHD3D12SurfaceBinding *surface)
{
    return !surface->color && surface->shape.z_format &&
           surface->fmt.dxgi_format == DXGI_FORMAT_D16_UNORM;
}

static uint16_t unorm16_to_half(uint16_t value)
{
    float f = value / 65535.0f;
    uint32_t bits;
    uint32_t half;
    uint32_t rest;

    if (f < 1.0f / 16384.0f) {
        /* Subnormal range: units of 2^-24. */
        return (uint16_t)lrintf(f * 16777216.0f);
    }
    memcpy(&bits, &f, sizeof(bits));
    half = ((((bits >> 23) & 0xff) - 127 + 15) << 10) |
           ((bits >> 13) & 0x3ff);
    rest = bits & 0x1fff;
    if (rest > 0x1000 || (rest == 0x1000 && (half & 1))) {
        half++;
    }
    return (uint16_t)half;
}

static uint16_t half_to_unorm16(uint16_t half)
{
    uint32_t exponent = (half >> 10) & 0x1f;
    uint32_t mantissa = half & 0x3ff;
    float f;

    if (half & 0x8000) {
        return 0;
    }
    if (exponent == 31) {
        return mantissa ? 0 : 0xffff;
    }
    f = exponent ? ldexpf(1.0f + mantissa / 1024.0f, (int)exponent - 15) :
                   mantissa / 16777216.0f;
    return (uint16_t)lrintf(MIN(f, 1.0f) * 65535.0f);
}

static void convert_float_z16_rows(uint8_t *data, uint32_t pitch,
                                   unsigned int width, unsigned int rows,
                                   bool to_guest)
{
    for (unsigned int y = 0; y < rows; y++) {
        uint8_t *row = data + (uint64_t)y * pitch;

        for (unsigned int x = 0; x < width; x++) {
            uint16_t value;

            memcpy(&value, row + x * 2, sizeof(value));
            value = to_guest ? unorm16_to_half(value) :
                               half_to_unorm16(value);
            memcpy(row + x * 2, &value, sizeof(value));
        }
    }
}

static uint64_t surface_transfer_slab_budget(uint64_t scaled_native_row_size)
{
    /* Never split a guest row across transfer commands: one guest row is
     * `scale` D3D12 rows, so a single native row is the minimum slab even
     * above the 16 MiB budget. */
    return MAX((uint64_t)D3D12_SURFACE_TRANSFER_SLAB_SIZE,
               scaled_native_row_size);
}

static unsigned int surface_transfer_slab_native_rows(
    uint32_t row_pitch, unsigned int scale, unsigned int rows_remaining)
{
    uint64_t scaled_native_row_size;
    uint64_t budget;
    uint64_t rows;

    assert(row_pitch != 0);
    assert(scale != 0);
    assert(rows_remaining != 0);

    scaled_native_row_size = (uint64_t)row_pitch * scale;
    budget = surface_transfer_slab_budget(scaled_native_row_size);
    rows = budget / scaled_native_row_size;
    assert(rows != 0);
    return (unsigned int)MIN((uint64_t)rows_remaining, rows);
}

static bool require_whole_zeta_transfer_fits(
    const PGRAPHD3D12State *r, const PGRAPHD3D12SurfaceBinding *surface,
    uint64_t transfer_size, uint64_t scaled_native_row_size,
    const char *direction, Error **errp)
{
    /* High internal scales need large whole-zeta copies; allow a share of
     * the adapter budget beyond the fixed floor. */
    uint64_t budget = MAX((uint64_t)D3D12_ZETA_TRANSFER_MAX_SIZE,
                          scaled_native_row_size);

    if (r != NULL) {
        budget = MAX(budget, MIN(r->video_memory_budget / 16,
                                 (uint64_t)D3D12_ZETA_TRANSFER_CEILING));
    }

    if (transfer_size <= budget) {
        return true;
    }

    /* CopyTextureRegion needs the whole subresource when a depth/stencil
     * resource participates, so zeta transfers cannot be row-slabbed.  Allow
     * a larger bounded whole-zeta transfer, then fail and leave the surface
     * dirty for retry. */
    error_setg(errp,
               "Direct3D 12 cannot %s %s zeta surface addr=0x%"
               HWADDR_PRIx " with a bounded staging copy: whole depth/stencil "
               "subresource requires %" PRIu64 " bytes, transfer budget is %"
               PRIu64 " bytes (size=%ux%u, pitch=%u)",
               direction, zeta_surface_format_name(surface->shape.zeta_format),
               surface->vram_addr, transfer_size, budget, surface->width,
               surface->height, surface->pitch);
    return false;
}

static DXGI_FORMAT surface_resource_format(
    const PGRAPHD3D12SurfaceBinding *surface)
{
    if (!surface->color &&
        surface->fmt.dxgi_format == DXGI_FORMAT_D24_UNORM_S8_UINT) {
        return DXGI_FORMAT_R24G8_TYPELESS;
    }

    return surface->fmt.dxgi_format;
}

/*
 * Z24S8 images are planar on the GPU and move as separate depth and stencil
 * planes. The resource format decides: a zeta surface's shape is copied from
 * the colour target and may carry an older zeta format.
 */
static bool surface_is_z24s8(const PGRAPHD3D12SurfaceBinding *surface)
{
    return !surface->color &&
           surface->fmt.dxgi_format == DXGI_FORMAT_D24_UNORM_S8_UINT;
}

static bool zeta_plane_layout(const PGRAPHD3D12SurfaceBinding *surface,
                              unsigned int width, unsigned int height,
                              D3D12ZetaPlaneLayout *layout, Error **errp)
{
    uint64_t depth_row_bytes;
    uint64_t stencil_row_bytes;

    memset(layout, 0, sizeof(*layout));

    switch (surface->fmt.dxgi_format) {
    case DXGI_FORMAT_D16_UNORM:
        depth_row_bytes = (uint64_t)width * 2;
        layout->depth_format = DXGI_FORMAT_D16_UNORM;
        break;
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
        depth_row_bytes = (uint64_t)width * 4;
        stencil_row_bytes = width;
        layout->depth_format = DXGI_FORMAT_R32_TYPELESS;
        layout->stencil_format = DXGI_FORMAT_R8_TYPELESS;
        layout->has_stencil = true;
        break;
    default:
        error_setg(errp,
                   "Direct3D 12 zeta copy has no plane layout for DXGI "
                   "format %u",
                   (unsigned int)surface->fmt.dxgi_format);
        return false;
    }

    layout->depth_row_pitch =
        ROUND_UP(depth_row_bytes, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    layout->depth_size = (uint64_t)layout->depth_row_pitch * height;
    layout->total_size = layout->depth_size;

    if (layout->has_stencil) {
        layout->stencil_offset =
            ROUND_UP(layout->depth_size, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
        layout->stencil_row_pitch =
            ROUND_UP(stencil_row_bytes, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
        layout->stencil_size = (uint64_t)layout->stencil_row_pitch * height;
        layout->total_size = layout->stencil_offset + layout->stencil_size;
    }

    return true;
}

static void set_buffer_texture_copy_location(D3D12_TEXTURE_COPY_LOCATION *loc,
                                             ID3D12Resource *buffer,
                                             uint64_t offset,
                                             DXGI_FORMAT format,
                                             unsigned int width,
                                             unsigned int height,
                                             uint32_t row_pitch)
{
    memset(loc, 0, sizeof(*loc));
    loc->pResource = buffer;
    loc->Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    loc->PlacedFootprint.Offset = offset;
    loc->PlacedFootprint.Footprint.Format = format;
    loc->PlacedFootprint.Footprint.Width = width;
    loc->PlacedFootprint.Footprint.Height = height;
    loc->PlacedFootprint.Footprint.Depth = 1;
    loc->PlacedFootprint.Footprint.RowPitch = row_pitch;
}

static void set_texture_subresource_copy_location(D3D12_TEXTURE_COPY_LOCATION *loc,
                                                  ID3D12Resource *texture,
                                                  unsigned int subresource)
{
    memset(loc, 0, sizeof(*loc));
    loc->pResource = texture;
    loc->Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    loc->SubresourceIndex = subresource;
}

/*
 * A 24-bit depth as a float depth-plane texel: the value the pixel shaders
 * write for it, floor(z) / 2^24 plus one ulp.
 */
static uint32_t zeta_depth_to_float_bits(uint32_t depth)
{
    float value = (float)depth / 16777216.0f;
    uint32_t bits;

    memcpy(&bits, &value, sizeof(bits));
    return bits + 1;
}

/* The 24-bit depth of a float depth-plane texel. */
static uint32_t zeta_depth_from_float_bits(uint32_t bits)
{
    float value;

    memcpy(&value, &bits, sizeof(value));
    if (!(value > 0.0f)) {
        return 0;
    }
    if (value >= 1.0f) {
        return 0x00ffffffu;
    }
    return (uint32_t)((double)value * 16777215.0 + 0.5);
}

static void pack_z24s8_to_d3d12_planes(uint8_t *dst,
                                       const D3D12ZetaPlaneLayout *layout,
                                       const uint8_t *src,
                                       uint32_t src_pitch,
                                       unsigned int width,
                                       unsigned int height,
                                       unsigned int scale,
                                       bool depth_float)
{
    uint8_t *depth_base = dst + layout->depth_offset;
    uint8_t *stencil_base = dst + layout->stencil_offset;
    size_t depth_row_bytes = (size_t)width * scale * 4;
    size_t stencil_row_bytes = (size_t)width * scale;
    /* dst is write-combined upload memory; rows are built in cached
     * memory and written there once per scaled row. */
    g_autofree uint8_t *depth_row = g_malloc(depth_row_bytes);
    g_autofree uint8_t *stencil_row = g_malloc(stencil_row_bytes);

    for (unsigned int y = 0; y < height; y++) {
        const uint8_t *src_row = src + (uint64_t)y * src_pitch;
        uint32_t *depth_out = (uint32_t *)depth_row;
        uint8_t *stencil_out = stencil_row;

        for (unsigned int x = 0; x < width; x++) {
            uint32_t raw = ldl_le_p(src_row + x * 4);
            uint32_t depth = (raw >> 8) & 0x00ffffffu;
            uint8_t stencil = raw & 0xffu;

            if (depth_float) {
                depth = zeta_depth_to_float_bits(depth);
            }
            for (unsigned int i = 0; i < scale; i++) {
                *depth_out++ = depth;
                *stencil_out++ = stencil;
            }
        }
        for (unsigned int i = 0; i < scale; i++) {
            uint64_t out_row = (uint64_t)y * scale + i;

            memcpy(depth_base + out_row * layout->depth_row_pitch, depth_row,
                   depth_row_bytes);
            memcpy(stencil_base + out_row * layout->stencil_row_pitch,
                   stencil_row, stencil_row_bytes);
        }
    }
}

static void pack_d3d12_planes_to_z24s8(uint8_t *dst, uint32_t dst_pitch,
                                       const uint8_t *src,
                                       const D3D12ZetaPlaneLayout *layout,
                                       unsigned int width,
                                       unsigned int height,
                                       unsigned int scale,
                                       bool depth_float)
{
    const uint8_t *depth_base = src + layout->depth_offset;
    const uint8_t *stencil_base = src + layout->stencil_offset;

    for (unsigned int y = 0; y < height; y++) {
        const uint8_t *depth_row =
            depth_base + (uint64_t)y * scale * layout->depth_row_pitch;
        const uint8_t *stencil_row =
            stencil_base + (uint64_t)y * scale * layout->stencil_row_pitch;
        uint8_t *dst_row = dst + y * dst_pitch;

        for (unsigned int x = 0; x < width; x++) {
            unsigned int src_x = x * scale;
            uint32_t depth = ldl_le_p(depth_row + src_x * 4);
            uint8_t stencil = stencil_row[src_x];

            depth = depth_float ? zeta_depth_from_float_bits(depth) :
                                  depth & 0x00ffffffu;

            stl_le_p(dst_row + x * 4, (depth << 8) | stencil);
        }
    }
}

static bool reserve_surface_upload_range(
    PGRAPHState *pg, const PGRAPHD3D12SurfaceBinding *surface,
    uint64_t upload_size, uint64_t *upload_offset, Error **errp)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    uint64_t offset;

    if (r == NULL || r->upload_buffer.mapped == NULL) {
        error_setg(errp, "Direct3D 12 surface upload buffer is not mapped");
        return false;
    }

    /* Color callers reserve bounded slabs; whole zeta copies are separately
     * capped before reaching this allocator. */
    if (!pgraph_d3d12_ring_alloc(pg, &r->upload_buffer, upload_size,
                                 D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT,
                                 &offset, errp)) {
        error_prepend(errp,
                      "Direct3D 12 %s surface upload could not reserve "
                      "%" PRIu64 " bytes: addr=0x%" HWADDR_PRIx
                      ", size=%ux%u, pitch=%u, bytes_per_pixel=%u, dxgi=%u: ",
                      surface->color ? "color" : "zeta", upload_size,
                      surface->vram_addr, surface->width, surface->height,
                      surface->pitch, surface->fmt.bytes_per_pixel,
                      (unsigned int)surface->fmt.dxgi_format);
        return false;
    }

    *upload_offset = offset;
    return true;
}

static bool account_surface_batch_command(PGRAPHState *pg, Error **errp)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;

    if (++r->draws_since_submit < PGRAPH_D3D12_DRAWS_PER_SUBMIT) {
        return true;
    }
    r->perf.submit_src[3]++;
    return pgraph_d3d12_submit_commands_async(pg, errp);
}

static bool allocate_surface_descriptor(PGRAPHD3D12State *r,
                                        PGRAPHD3D12SurfaceBinding *surface,
                                        Error **errp)
{
    if (surface->color) {
        if (r->num_free_rtv_descriptors > 0) {
            surface->descriptor_index =
                r->free_rtv_descriptors[--r->num_free_rtv_descriptors];
        } else if (r->next_rtv_descriptor < PGRAPH_D3D12_RTV_HEAP_SIZE) {
            surface->descriptor_index = r->next_rtv_descriptor++;
        } else {
            error_setg(errp, "Direct3D 12 RTV descriptor heap exhausted");
            return false;
        }
        surface->descriptor =
            descriptor_handle(r->rtv_heap, r->rtv_descriptor_size,
                              surface->descriptor_index);
    } else {
        if (r->num_free_dsv_descriptors > 0) {
            surface->descriptor_index =
                r->free_dsv_descriptors[--r->num_free_dsv_descriptors];
        } else if (r->next_dsv_descriptor < PGRAPH_D3D12_DSV_HEAP_SIZE) {
            surface->descriptor_index = r->next_dsv_descriptor++;
        } else {
            error_setg(errp, "Direct3D 12 DSV descriptor heap exhausted");
            return false;
        }
        surface->descriptor =
            descriptor_handle(r->dsv_heap, r->dsv_descriptor_size,
                              surface->descriptor_index);
    }

    surface->descriptor_allocated = true;
    return true;
}

static void release_surface_descriptor(PGRAPHD3D12State *r,
                                       PGRAPHD3D12SurfaceBinding *surface)
{
    if (r == NULL || surface == NULL || !surface->descriptor_allocated) {
        return;
    }

    if (surface->color) {
        if (surface->descriptor_index < PGRAPH_D3D12_RTV_HEAP_SIZE &&
            r->num_free_rtv_descriptors < PGRAPH_D3D12_RTV_HEAP_SIZE) {
            r->free_rtv_descriptors[r->num_free_rtv_descriptors++] =
                surface->descriptor_index;
        }
    } else {
        if (surface->descriptor_index < PGRAPH_D3D12_DSV_HEAP_SIZE &&
            r->num_free_dsv_descriptors < PGRAPH_D3D12_DSV_HEAP_SIZE) {
            r->free_dsv_descriptors[r->num_free_dsv_descriptors++] =
                surface->descriptor_index;
        }
    }

    surface->descriptor_allocated = false;
}

static bool create_surface_resource_impl(PGRAPHState *pg,
                                    PGRAPHD3D12SurfaceBinding *surface,
                                    Error **errp);

static void create_surface_view(PGRAPHD3D12State *r,
                                PGRAPHD3D12SurfaceBinding *surface)
{
    if (surface->color) {
        D3D12_RENDER_TARGET_VIEW_DESC view_desc;

        memset(&view_desc, 0, sizeof(view_desc));
        view_desc.Format = surface->fmt.dxgi_format;
        view_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        r->device->lpVtbl->CreateRenderTargetView(r->device,
                                                  surface->resource,
                                                  &view_desc,
                                                  surface->descriptor);
    } else {
        D3D12_DEPTH_STENCIL_VIEW_DESC view_desc;

        memset(&view_desc, 0, sizeof(view_desc));
        view_desc.Format = surface->fmt.dxgi_format;
        view_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        r->device->lpVtbl->CreateDepthStencilView(r->device,
                                                  surface->resource,
                                                  &view_desc,
                                                  surface->descriptor);
    }
}

/*
 * Surface image pool. Games rebuild the same render targets every frame
 * (aliased targets are recreated on each overlap), and committed resource
 * creation is slow on the Xbox driver. Released images are kept
 * for a few frames and handed to the next surface of the same shape; the
 * in-order queue makes the handover safe without a fence.
 */
#define SURFACE_POOL_MAX_BYTES (48u * 1024 * 1024)
#define SURFACE_POOL_MAX_AGE 8

static void surface_pool_scaled_size(PGRAPHState *pg,
                                     const PGRAPHD3D12SurfaceBinding *surface,
                                     unsigned int *width,
                                     unsigned int *height)
{
    *width = MAX(surface->width, 1);
    *height = MAX(surface->height, 1);
    pgraph_apply_scaling_factor(pg, width, height);
}

static void surface_pool_remove(PGRAPHD3D12State *r, unsigned int index)
{
    r->surface_pool_bytes -= r->surface_pool[index].bytes;
    memmove(&r->surface_pool[index], &r->surface_pool[index + 1],
            (r->surface_pool_count - index - 1) * sizeof(r->surface_pool[0]));
    r->surface_pool_count--;
}

static void surface_pool_retire(PGRAPHState *pg, unsigned int index)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;

    pgraph_d3d12_defer_release(pg, r->surface_pool[index].resource, -1, -1,
                               -1, -1);
    surface_pool_remove(r, index);
}

static void pool_surface_resource(PGRAPHState *pg,
                                  PGRAPHD3D12SurfaceBinding *surface)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    unsigned int width, height;
    uint64_t bytes;

    if (surface->resource == NULL) {
        return;
    }
    surface_pool_scaled_size(pg, surface, &width, &height);
    bytes = (uint64_t)width * height * MAX(surface->fmt.bytes_per_pixel, 1);
    if (bytes > SURFACE_POOL_MAX_BYTES / 4) {
        pgraph_d3d12_defer_release(pg, surface->resource, -1, -1, -1, -1);
        surface->resource = NULL;
        return;
    }
    while (r->surface_pool_count != 0 &&
           (r->surface_pool_count == ARRAY_SIZE(r->surface_pool) ||
            r->surface_pool_bytes + bytes > SURFACE_POOL_MAX_BYTES)) {
        surface_pool_retire(pg, 0);
    }
    r->surface_pool[r->surface_pool_count].resource = surface->resource;
    r->surface_pool[r->surface_pool_count].format =
        surface_resource_format(surface);
    r->surface_pool[r->surface_pool_count].width = width;
    r->surface_pool[r->surface_pool_count].height = height;
    r->surface_pool[r->surface_pool_count].bytes = bytes;
    r->surface_pool[r->surface_pool_count].color = surface->color;
    r->surface_pool[r->surface_pool_count].state = surface->state;
    r->surface_pool[r->surface_pool_count].frame_time = pg->frame_time;
    r->surface_pool_count++;
    r->surface_pool_bytes += bytes;
    surface->resource = NULL;
}

static bool take_pooled_surface_resource(PGRAPHState *pg,
                                         PGRAPHD3D12SurfaceBinding *surface)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    DXGI_FORMAT format = surface_resource_format(surface);
    unsigned int width, height;

    surface_pool_scaled_size(pg, surface, &width, &height);
    for (unsigned int i = 0; i < r->surface_pool_count; i++) {
        if (r->surface_pool[i].format == format &&
            r->surface_pool[i].width == width &&
            r->surface_pool[i].height == height &&
            r->surface_pool[i].color == surface->color) {
            surface->resource = r->surface_pool[i].resource;
            surface->state = r->surface_pool[i].state;
            surface_pool_remove(r, i);
            return true;
        }
    }
    return false;
}

static void expire_pooled_surface_resources(PGRAPHState *pg)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;

    while (r->surface_pool_count != 0 &&
           pg->frame_time - r->surface_pool[0].frame_time >
               SURFACE_POOL_MAX_AGE) {
        surface_pool_retire(pg, 0);
    }
}

static void release_pooled_surface_resources(PGRAPHState *pg)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;

    while (r->surface_pool_count != 0) {
        surface_pool_retire(pg, 0);
    }
}

void pgraph_d3d12_purge_surface_pool(PGRAPHState *pg)
{
    if (pg->d3d12_renderer_state != NULL) {
        release_pooled_surface_resources(pg);
    }
}

static bool create_surface_resource(PGRAPHState *pg,
                                    PGRAPHD3D12SurfaceBinding *surface,
                                    Error **errp)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    int64_t start = g_get_monotonic_time();
    bool ok = create_surface_resource_impl(pg, surface, errp);

    r->perf.surface_create_us += g_get_monotonic_time() - start;
    r->perf.surface_creates++;
    return ok;
}

static bool create_surface_resource_impl(PGRAPHState *pg,
                                    PGRAPHD3D12SurfaceBinding *surface,
                                    Error **errp)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    unsigned int width = MAX(surface->width, 1);
    unsigned int height = MAX(surface->height, 1);
    D3D12_HEAP_PROPERTIES heap_props;
    D3D12_RESOURCE_DESC desc;
    D3D12_CLEAR_VALUE clear_value;
    HRESULT hr;

    pgraph_apply_scaling_factor(pg, &width, &height);

    memset(&heap_props, 0, sizeof(heap_props));
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap_props.CreationNodeMask = 1;
    heap_props.VisibleNodeMask = 1;

    memset(&desc, 0, sizeof(desc));
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = surface_resource_format(surface);
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = surface->color ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET :
                                  D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    memset(&clear_value, 0, sizeof(clear_value));
    clear_value.Format = surface->fmt.dxgi_format;
    if (surface->color) {
        clear_value.Color[3] = 1.0f;
    } else {
        clear_value.DepthStencil.Depth = 1.0f;
    }

    surface->state = D3D12_RESOURCE_STATE_COMMON;
    hr = take_pooled_surface_resource(pg, surface) ?
             S_OK :
             r->device->lpVtbl->CreateCommittedResource(
                 r->device, &heap_props, D3D12_HEAP_FLAG_NONE, &desc,
                 surface->state, &clear_value, &IID_ID3D12Resource,
                 (void **)&surface->resource);
    if (FAILED(hr)) {
        char detail[128];

        error_setg(errp,
                   "Failed to create Direct3D 12 %s surface %ux%u: "
                   "HRESULT %s (0x%08lx)%s, pitch=%u, format=%s (0x%x), "
                   "dxgi=%u, vram=0x%" HWADDR_PRIx,
                   surface->color ? "color" : "zeta", width, height,
                   pgraph_d3d12_hresult_name(hr), (unsigned long)hr,
                   pgraph_d3d12_device_removed_detail(r, hr, detail,
                                                       sizeof(detail)),
                   surface->pitch,
                   surface->color ?
                       color_surface_format_name(surface->shape.color_format) :
                       zeta_surface_format_name(surface->shape.zeta_format),
                   surface->color ? surface->shape.color_format :
                                    surface->shape.zeta_format,
                   (unsigned int)surface_resource_format(surface),
                   surface->vram_addr);
        return false;
    }

    if (!allocate_surface_descriptor(r, surface, errp)) {
        /* The image may come from the pool and still be referenced by
         * recorded work; the pool keeps it until the queue is past it. */
        pool_surface_resource(pg, surface);
        return false;
    }

    create_surface_view(r, surface);

    return true;
}

/*
 * Buffer copies of a D24_UNORM_S8_UINT depth plane carry 24-bit UNORM depth
 * in the low bits of each texel. The console driver keeps the plane as
 * 32-bit float and copies it unconverted: a cleared 1.0 reads back as 0.5
 * and an uploaded 0.5 lands as almost 0, failing every later depth test.
 * Before the first zeta transfer, a small target cleared to a known depth
 * shows which the adapter does. A failed probe keeps UNORM.
 */
static bool zeta_depth_plane_float(PGRAPHState *pg)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    const float probe_depth = 0.75f;
    PGRAPHD3D12SurfaceBinding probe;
    D3D12ZetaPlaneLayout layout;
    D3D12_HEAP_PROPERTIES heap_props;
    D3D12_RESOURCE_DESC desc;
    D3D12_CLEAR_VALUE clear_value;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    D3D12_TEXTURE_COPY_LOCATION dst_loc;
    D3D12_TEXTURE_COPY_LOCATION src_loc;
    Error *local_err = NULL;
    uint32_t texel;
    float value;
    HRESULT hr;

    if (r->zeta_depth_plane_probed) {
        return r->zeta_depth_plane_float;
    }
    r->zeta_depth_plane_probed = true;

    memset(&probe, 0, sizeof(probe));
    probe.fmt.dxgi_format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    probe.width = 8;
    probe.height = 8;
    if (!zeta_plane_layout(&probe, probe.width, probe.height, &layout,
                           &local_err)) {
        goto out;
    }

    memset(&heap_props, 0, sizeof(heap_props));
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap_props.CreationNodeMask = 1;
    heap_props.VisibleNodeMask = 1;
    memset(&desc, 0, sizeof(desc));
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = probe.width;
    desc.Height = probe.height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R24G8_TYPELESS;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    memset(&clear_value, 0, sizeof(clear_value));
    clear_value.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    clear_value.DepthStencil.Depth = probe_depth;

    hr = r->device->lpVtbl->CreateCommittedResource(
        r->device, &heap_props, D3D12_HEAP_FLAG_NONE, &desc, state,
        &clear_value, &IID_ID3D12Resource, (void **)&probe.resource);
    if (FAILED(hr)) {
        error_setg(&local_err, "probe target creation failed: 0x%08lx",
                   (unsigned long)hr);
        goto out;
    }
    if (!allocate_surface_descriptor(r, &probe, &local_err)) {
        goto out;
    }
    create_surface_view(r, &probe);
    if (!pgraph_d3d12_ensure_buffer_size(
            pg, &r->readback_buffer, layout.total_size,
            D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, true,
            "zeta depth plane probe", &local_err) ||
        !pgraph_d3d12_begin_commands(pg, &local_err)) {
        goto out;
    }

    r->command_list->lpVtbl->ClearDepthStencilView(
        r->command_list, probe.descriptor, D3D12_CLEAR_FLAG_DEPTH,
        probe_depth, 0, 0, NULL);
    pgraph_d3d12_transition_raw_resource(r, probe.resource, &state,
                                         D3D12_RESOURCE_STATE_COPY_SOURCE);
    set_buffer_texture_copy_location(&dst_loc, r->readback_buffer.resource,
                                     layout.depth_offset, layout.depth_format,
                                     probe.width, probe.height,
                                     layout.depth_row_pitch);
    set_texture_subresource_copy_location(&src_loc, probe.resource, 0);
    r->command_list->lpVtbl->CopyTextureRegion(r->command_list, &dst_loc, 0, 0,
                                               0, &src_loc, NULL);
    if (!pgraph_d3d12_submit_commands(pg, &local_err)) {
        goto out;
    }

    texel = ldl_le_p((const uint8_t *)r->readback_buffer.mapped +
                     layout.depth_offset);
    memcpy(&value, &texel, sizeof(value));
    r->zeta_depth_plane_float = fabsf(value - probe_depth) < 1e-6f;
    fprintf(stderr,
            "Direct3D 12 renderer: D24S8 depth plane copies hold %s depth "
            "(probe texel 0x%08x)\n",
            r->zeta_depth_plane_float ? "32-bit float" : "24-bit UNORM",
            texel);

out:
    if (local_err != NULL) {
        fprintf(stderr,
                "Direct3D 12 renderer: D24S8 depth plane probe failed, "
                "assuming 24-bit UNORM: %s\n",
                error_get_pretty(local_err));
        error_free(local_err);
    }
    if (probe.resource != NULL || probe.descriptor_allocated) {
        pgraph_d3d12_defer_release(
            pg, probe.resource, -1, -1,
            probe.descriptor_allocated ? (int32_t)probe.descriptor_index : -1,
            -1);
    }
    return r->zeta_depth_plane_float;
}

static bool upload_z24s8_surface_data(NV2AState *d,
                                      PGRAPHD3D12SurfaceBinding *surface,
                                      bool force,
                                      D3D12_RESOURCE_STATES final_state,
                                      Error **errp)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    unsigned int scale = MAX(pg->surface_scale_factor, 1);
    unsigned int scaled_width = surface->width;
    unsigned int scaled_height = surface->height;
    uint8_t *src = d->vram_ptr + surface->vram_addr;
    g_autofree uint8_t *unswizzled = NULL;
    D3D12ZetaPlaneLayout layout;
    D3D12ZetaPlaneLayout native_row_layout;
    D3D12_TEXTURE_COPY_LOCATION dst_loc;
    D3D12_TEXTURE_COPY_LOCATION src_loc;
    uint64_t upload_offset;
    bool depth_float;

    if (!(surface->upload_pending || force)) {
        return true;
    }
    surface->upload_pending = true;
    if (!surface->width || !surface->height) {
        surface->upload_pending = false;
        pgraph_d3d12_surface_mark_cpu_owned(surface);
        return true;
    }

    depth_float = zeta_depth_plane_float(pg);
    pgraph_apply_scaling_factor(pg, &scaled_width, &scaled_height);
    if (!zeta_plane_layout(surface, scaled_width, scaled_height, &layout,
                           errp)) {
        return false;
    }
    if (!zeta_plane_layout(surface, scaled_width, scale, &native_row_layout,
                           errp)) {
        return false;
    }
    if (!require_whole_zeta_transfer_fits(
            d->pgraph.d3d12_renderer_state, surface, layout.total_size,
            native_row_layout.total_size,
            "upload", errp)) {
        return false;
    }

    if (!reserve_surface_upload_range(pg, surface, layout.total_size,
                                      &upload_offset, errp)) {
        return false;
    }

    if (surface->swizzle) {
        unswizzled = g_malloc(surface->size);
        unswizzle_rect(src, surface->width, surface->height, unswizzled,
                       surface->pitch, surface->fmt.bytes_per_pixel);
        src = unswizzled;
        nv2a_profile_inc_counter(NV2A_PROF_SURF_SWIZZLE);
    }

    pack_z24s8_to_d3d12_planes(
        (uint8_t *)r->upload_buffer.mapped + upload_offset, &layout, src,
        surface->pitch, surface->width, surface->height, scale, depth_float);

    if (!pgraph_d3d12_begin_commands(pg, errp)) {
        surface->upload_pending = true;
        return false;
    }

    pgraph_d3d12_transition_raw_resource(r, surface->resource, &surface->state,
                                         D3D12_RESOURCE_STATE_COPY_DEST);

    set_texture_subresource_copy_location(&dst_loc, surface->resource, 0);
    set_buffer_texture_copy_location(&src_loc, r->upload_buffer.resource,
                                     upload_offset + layout.depth_offset,
                                     layout.depth_format, scaled_width,
                                     scaled_height, layout.depth_row_pitch);
    r->command_list->lpVtbl->CopyTextureRegion(r->command_list, &dst_loc, 0, 0,
                                               0, &src_loc, NULL);

    set_texture_subresource_copy_location(&dst_loc, surface->resource, 1);
    set_buffer_texture_copy_location(&src_loc, r->upload_buffer.resource,
                                     upload_offset + layout.stencil_offset,
                                     layout.stencil_format, scaled_width,
                                     scaled_height, layout.stencil_row_pitch);
    r->command_list->lpVtbl->CopyTextureRegion(r->command_list, &dst_loc, 0, 0,
                                               0, &src_loc, NULL);

    pgraph_d3d12_transition_raw_resource(r, surface->resource, &surface->state,
                                         final_state);

    /* Submit only at the batch threshold; ring exhaustion, reports,
     * downloads and flips submit earlier when required. */
    if (!account_surface_batch_command(pg, errp)) {
        surface->upload_pending = true;
        return false;
    }

    surface->upload_pending = false;
    pgraph_d3d12_surface_mark_cpu_owned(surface);
    surface->draw_time = ++pg->draw_time;
    surface->initialized = true;
    surface->cleared = false;
    nv2a_profile_inc_counter(NV2A_PROF_SURF_UPLOAD);
    return true;
}

static bool upload_surface_data_impl(NV2AState *d,
                                PGRAPHD3D12SurfaceBinding *surface,
                                bool force,
                                D3D12_RESOURCE_STATES final_state,
                                Error **errp);
static uint32_t evlog_ram_sum(NV2AState *d, hwaddr addr, hwaddr size,
                              uint32_t *nonzero);

static bool upload_surface_data(NV2AState *d,
                                PGRAPHD3D12SurfaceBinding *surface,
                                bool force,
                                D3D12_RESOURCE_STATES final_state,
                                Error **errp)
{
    PGRAPHD3D12State *r = d->pgraph.d3d12_renderer_state;
    int64_t start = g_get_monotonic_time();
    bool ok;

    if (surface->upload_pending || force) {
        if (r->evlog_frames_left > 0) {
            uint32_t nonzero;
            uint32_t sum = evlog_ram_sum(d, surface->vram_addr,
                                         surface->size, &nonzero);

            pgraph_d3d12_evlog(r, "upl %c 0x%" HWADDR_PRIx
                               " %ux%u force=%d sum=%08x nz=%u",
                               surface->color ? 'c' : 'z',
                               surface->vram_addr, surface->width,
                               surface->height, force, sum, nonzero);
        }
        pgraph_d3d12_complete_readbacks_in_range(
            d, surface->vram_addr, surface->size,
            PGRAPH_D3D12_STASH_TRIGGER_UPLOAD);
    }
    ok = upload_surface_data_impl(d, surface, force, final_state, errp);

    r->perf.surface_upload_us += g_get_monotonic_time() - start;
    return ok;
}

static bool upload_surface_data_impl(NV2AState *d,
                                PGRAPHD3D12SurfaceBinding *surface,
                                bool force,
                                D3D12_RESOURCE_STATES final_state,
                                Error **errp)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    unsigned int scale = MAX(pg->surface_scale_factor, 1);
    unsigned int width = surface->width;
    unsigned int height = surface->height;
    unsigned int scaled_width = width;
    unsigned int scaled_height = height;
    uint32_t row_bytes;
    uint32_t row_pitch;
    uint8_t *src = d->vram_ptr + surface->vram_addr;
    g_autofree uint8_t *unswizzled = NULL;
    unsigned int native_y;

    if (!(surface->upload_pending || force)) {
        return true;
    }

    if (surface_is_z24s8(surface)) {
        return upload_z24s8_surface_data(d, surface, force, final_state, errp);
    }

    surface->upload_pending = true;
    if (!width || !height) {
        surface->upload_pending = false;
        pgraph_d3d12_surface_mark_cpu_owned(surface);
        return true;
    }

    pgraph_apply_scaling_factor(pg, &scaled_width, &scaled_height);
    row_bytes = scaled_width * surface->fmt.bytes_per_pixel;
    row_pitch = ROUND_UP(row_bytes, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    if (!surface->color &&
        !require_whole_zeta_transfer_fits(
            d->pgraph.d3d12_renderer_state, surface,
            (uint64_t)row_pitch * scaled_height,
            (uint64_t)row_pitch * scale, "upload", errp)) {
        return false;
    }

    if (surface->swizzle) {
        unswizzled = g_malloc(surface->size);
        unswizzle_rect(src, width, height, unswizzled, surface->pitch,
                       surface->fmt.bytes_per_pixel);
        src = unswizzled;
        nv2a_profile_inc_counter(NV2A_PROF_SURF_SWIZZLE);
    }
    if (surface_is_float_z16(surface)) {
        if (unswizzled == NULL) {
            unswizzled = g_memdup2(src, (gsize)(height - 1) * surface->pitch +
                                            (gsize)width * 2);
            src = unswizzled;
        }
        convert_float_z16_rows(unswizzled, surface->pitch, width, height,
                               false);
    }

    for (native_y = 0; native_y < height;) {
        unsigned int slab_native_rows = surface->color ?
            surface_transfer_slab_native_rows(row_pitch, scale,
                                              height - native_y) :
            height;
        unsigned int slab_scaled_height = slab_native_rows * scale;
        uint64_t upload_size = (uint64_t)row_pitch * slab_scaled_height;
        uint64_t upload_offset;
        D3D12_TEXTURE_COPY_LOCATION dst_loc;
        D3D12_TEXTURE_COPY_LOCATION src_loc;

        if (!reserve_surface_upload_range(pg, surface, upload_size,
                                          &upload_offset, errp)) {
            return false;
        }

        copy_scaled_rows(
            (uint8_t *)r->upload_buffer.mapped + upload_offset, row_pitch,
            src + (uint64_t)native_y * surface->pitch, surface->pitch,
            width, slab_native_rows, surface->fmt.bytes_per_pixel, scale);

        if (!pgraph_d3d12_begin_commands(pg, errp)) {
            return false;
        }

        pgraph_d3d12_transition_raw_resource(
            r, surface->resource, &surface->state,
            D3D12_RESOURCE_STATE_COPY_DEST);
        set_texture_subresource_copy_location(&dst_loc, surface->resource, 0);
        set_buffer_texture_copy_location(
            &src_loc, r->upload_buffer.resource, upload_offset,
            surface->fmt.dxgi_format, scaled_width, slab_scaled_height,
            row_pitch);
        r->command_list->lpVtbl->CopyTextureRegion(
            r->command_list, &dst_loc, 0,
            surface->color ? native_y * scale : 0, 0, &src_loc, NULL);

        native_y += slab_native_rows;
    }

    pgraph_d3d12_transition_raw_resource(r, surface->resource,
                                         &surface->state, final_state);

    if (!account_surface_batch_command(pg, errp)) {
        return false;
    }

    surface->upload_pending = false;
    pgraph_d3d12_surface_mark_cpu_owned(surface);
    surface->draw_time = ++pg->draw_time;
    surface->initialized = true;
    surface->cleared = false;
    nv2a_profile_inc_counter(NV2A_PROF_SURF_UPLOAD);
    return true;
}

bool pgraph_d3d12_upload_surface_data(NV2AState *d,
                                      PGRAPHD3D12SurfaceBinding *surface,
                                      bool force, Error **errp)
{
    D3D12_RESOURCE_STATES final_state =
        surface != NULL && surface->color ? D3D12_RESOURCE_STATE_RENDER_TARGET :
                                            D3D12_RESOURCE_STATE_DEPTH_WRITE;
    return upload_surface_data(d, surface, force, final_state, errp);
}

bool pgraph_d3d12_upload_surface_data_with_state(
    NV2AState *d, PGRAPHD3D12SurfaceBinding *surface, bool force,
    D3D12_RESOURCE_STATES final_state, Error **errp)
{
    return upload_surface_data(d, surface, force, final_state, errp);
}

static bool allocate_framebuffer_copy_descriptor(PGRAPHD3D12State *r,
                                                 Error **errp)
{
    if (r->framebuffer_copy_descriptor_allocated) {
        return true;
    }

    if (!pgraph_d3d12_alloc_srv_descriptor(
            r, &r->framebuffer_copy_srv_descriptor_index, errp)) {
        error_prepend(errp,
                      "while allocating the framebuffer-fetch copy SRV: ");
        return false;
    }
    r->framebuffer_copy_srv_cpu =
        descriptor_handle(r->cbv_srv_uav_heap,
                          r->cbv_srv_uav_descriptor_size,
                          r->framebuffer_copy_srv_descriptor_index);
    r->framebuffer_copy_srv_gpu =
        gpu_descriptor_handle(r->cbv_srv_uav_heap,
                              r->cbv_srv_uav_descriptor_size,
                              r->framebuffer_copy_srv_descriptor_index);
    r->framebuffer_copy_descriptor_allocated = true;
    return true;
}

static bool ensure_framebuffer_copy_resource(PGRAPHState *pg,
                                             PGRAPHD3D12SurfaceBinding *surface,
                                             Error **errp)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    unsigned int width = MAX(surface->width, 1);
    unsigned int height = MAX(surface->height, 1);
    D3D12_HEAP_PROPERTIES heap_props;
    D3D12_RESOURCE_DESC desc;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc;
    HRESULT hr;

    pgraph_apply_scaling_factor(pg, &width, &height);

    if (r->framebuffer_copy_resource != NULL &&
        r->framebuffer_copy_format == surface->fmt.dxgi_format &&
        r->framebuffer_copy_width == width &&
        r->framebuffer_copy_height == height) {
        return true;
    }

    /* In-flight framebuffer-fetch draws may still reference the old copy;
     * retire it and its SRV slot behind the fence. */
    if (r->framebuffer_copy_resource != NULL) {
        pgraph_d3d12_defer_release(
            pg, r->framebuffer_copy_resource,
            r->framebuffer_copy_descriptor_allocated ?
                (int32_t)r->framebuffer_copy_srv_descriptor_index : -1,
            -1, -1, -1);
        r->framebuffer_copy_resource = NULL;
        r->framebuffer_copy_descriptor_allocated = false;
        r->record_framebuffer_srv_gpu = 0;
    }
    r->framebuffer_copy_valid = false;
    r->framebuffer_copy_state = D3D12_RESOURCE_STATE_COMMON;
    r->framebuffer_copy_format = DXGI_FORMAT_UNKNOWN;
    r->framebuffer_copy_width = 0;
    r->framebuffer_copy_height = 0;

    if (!allocate_framebuffer_copy_descriptor(r, errp)) {
        return false;
    }

    memset(&heap_props, 0, sizeof(heap_props));
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap_props.CreationNodeMask = 1;
    heap_props.VisibleNodeMask = 1;

    memset(&desc, 0, sizeof(desc));
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = surface->fmt.dxgi_format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    hr = r->device->lpVtbl->CreateCommittedResource(
        r->device, &heap_props, D3D12_HEAP_FLAG_NONE, &desc,
        r->framebuffer_copy_state, NULL, &IID_ID3D12Resource,
        (void **)&r->framebuffer_copy_resource);
    if (FAILED(hr)) {
        char detail[128];

        error_setg(errp,
                   "Failed to create Direct3D 12 framebuffer-fetch copy "
                   "resource: HRESULT %s (0x%08lx)%s, source_surface=0x%"
                   HWADDR_PRIx " %ux%u scaled=%ux%u pitch=%u "
                   "format=%s (0x%x) dxgi=%u",
                   pgraph_d3d12_hresult_name(hr), (unsigned long)hr,
                   pgraph_d3d12_device_removed_detail(r, hr, detail,
                                                       sizeof(detail)),
                   surface->vram_addr, surface->width,
                   surface->height, width, height, surface->pitch,
                   color_surface_format_name(surface->shape.color_format),
                   surface->shape.color_format,
                   (unsigned int)surface->fmt.dxgi_format);
        return false;
    }

    memset(&srv_desc, 0, sizeof(srv_desc));
    srv_desc.Format = surface->fmt.dxgi_format;
    srv_desc.Shader4ComponentMapping =
        D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MostDetailedMip = 0;
    srv_desc.Texture2D.MipLevels = 1;
    srv_desc.Texture2D.PlaneSlice = 0;
    srv_desc.Texture2D.ResourceMinLODClamp = 0.0f;
    r->device->lpVtbl->CreateShaderResourceView(
        r->device, r->framebuffer_copy_resource, &srv_desc,
        r->framebuffer_copy_srv_cpu);

    r->framebuffer_copy_format = surface->fmt.dxgi_format;
    r->framebuffer_copy_width = width;
    r->framebuffer_copy_height = height;
    return true;
}

bool pgraph_d3d12_prepare_framebuffer_fetch(PGRAPHState *pg, Error **errp)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    PGRAPHD3D12SurfaceBinding *surface;
    D3D12_TEXTURE_COPY_LOCATION dst;
    D3D12_TEXTURE_COPY_LOCATION src;

    if (r == NULL) {
        return false;
    }

    surface = r->color_binding;
    if (surface == NULL || surface->resource == NULL) {
        error_setg(errp,
                   "Direct3D 12 framebuffer-fetch fallback requested without "
                   "a bound color surface: color_binding=%p, "
                   "color_surface_offset=0x%" HWADDR_PRIx ", pitch=%u, "
                   "format=%s (0x%x), primitive=%u",
                   surface, (hwaddr)pg->surface_color.offset,
                   pg->surface_color.pitch,
                   color_surface_format_name(pg->surface_shape.color_format),
                   pg->surface_shape.color_format, pg->primitive_mode);
        return false;
    }

    switch (surface->fmt.dxgi_format) {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B5G6R5_UNORM:
    case DXGI_FORMAT_B5G5R5A1_UNORM:
    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R8_UNORM:
        break;
    default:
        error_setg(errp,
                   "Direct3D 12 framebuffer-fetch fallback cannot exactly "
                   "emulate logic-op for color surface format=%s (0x%x) "
                   "dxgi=%u bytes_per_pixel=%u addr=0x%" HWADDR_PRIx
                   " %ux%u pitch=%u; only "
                   "B8G8R8A8_UNORM, B5G6R5_UNORM, B5G5R5A1_UNORM, "
                   "R8G8_UNORM, and R8_UNORM targets are enabled for the "
                   "shader logic-op path",
                   color_surface_format_name(surface->shape.color_format),
                   surface->shape.color_format,
                   (unsigned int)surface->fmt.dxgi_format,
                   surface->fmt.bytes_per_pixel, surface->vram_addr,
                   surface->width, surface->height, surface->pitch);
        return false;
    }

    if (!ensure_framebuffer_copy_resource(pg, surface, errp)) {
        return false;
    }

    pgraph_d3d12_transition_raw_resource(r, surface->resource,
                                         &surface->state,
                                         D3D12_RESOURCE_STATE_COPY_SOURCE);
    pgraph_d3d12_transition_raw_resource(r, r->framebuffer_copy_resource,
                                         &r->framebuffer_copy_state,
                                         D3D12_RESOURCE_STATE_COPY_DEST);

    memset(&dst, 0, sizeof(dst));
    dst.pResource = r->framebuffer_copy_resource;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;

    memset(&src, 0, sizeof(src));
    src.pResource = surface->resource;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;

    r->command_list->lpVtbl->CopyTextureRegion(r->command_list, &dst, 0, 0, 0,
                                               &src, NULL);

    pgraph_d3d12_transition_raw_resource(
        r, r->framebuffer_copy_resource, &r->framebuffer_copy_state,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    pgraph_d3d12_transition_raw_resource(r, surface->resource,
                                         &surface->state,
                                         D3D12_RESOURCE_STATE_RENDER_TARGET);
    r->framebuffer_copy_valid = true;
    return true;
}

static unsigned int surface_writeback_log_count;

/* Diagnostic: every surface image written back to (or stashed over) guest
 * memory, capped. */
static void log_surface_writeback(const PGRAPHD3D12SurfaceBinding *surface,
                                  PGRAPHD3D12SurfaceDownloadReason reason,
                                  const char *path)
{
    if (!xbox_diag_log || surface_writeback_log_count++ >= 6000) {
        return;
    }
    fprintf(stderr,
            "WB %s addr=0x%" HWADDR_PRIx " end=0x%" HWADDR_PRIx
            " %ux%u color=%d swz=%d reason=%u frame=%d\n",
            path, surface->vram_addr,
            surface->vram_addr + (hwaddr)surface->size, surface->width,
            surface->height, surface->color, surface->swizzle,
            (unsigned int)reason, surface->frame_time);
}

static void record_surface_download(
    PGRAPHD3D12State *r, PGRAPHD3D12SurfaceDownloadReason reason)
{
    if ((unsigned int)reason <
        PGRAPH_D3D12_SURFACE_DOWNLOAD_REASON_COUNT) {
        r->perf.surface_downloads[reason]++;
    }
}

static bool download_z24s8_surface_data(
    NV2AState *d, PGRAPHD3D12SurfaceBinding *surface,
    PGRAPHD3D12SurfaceDownloadReason reason, bool force, Error **errp)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    unsigned int scale = MAX(pg->surface_scale_factor, 1);
    unsigned int scaled_width = surface->width;
    unsigned int scaled_height = surface->height;
    hwaddr dirty_size = (hwaddr)surface->pitch * surface->height;
    uint8_t *dst = d->vram_ptr + surface->vram_addr;
    g_autofree uint8_t *linear = NULL;
    D3D12ZetaPlaneLayout layout;
    D3D12ZetaPlaneLayout native_row_layout;
    D3D12_TEXTURE_COPY_LOCATION dst_loc;
    D3D12_TEXTURE_COPY_LOCATION src_loc;
    bool depth_float;

    if (!(surface->download_pending || force) || !surface->draw_dirty) {
        return true;
    }
    surface->download_pending = true;

    if (!surface->width || !surface->height) {
        surface->download_pending = false;
        pgraph_d3d12_surface_mark_cpu_owned(surface);
        return true;
    }

    depth_float = zeta_depth_plane_float(pg);
    pgraph_apply_scaling_factor(pg, &scaled_width, &scaled_height);
    if (!zeta_plane_layout(surface, scaled_width, scaled_height, &layout,
                           errp)) {
        return false;
    }
    if (!zeta_plane_layout(surface, scaled_width, scale, &native_row_layout,
                           errp)) {
        return false;
    }
    if (!require_whole_zeta_transfer_fits(
            d->pgraph.d3d12_renderer_state, surface, layout.total_size,
            native_row_layout.total_size,
            "download", errp)) {
        return false;
    }

    if (!pgraph_d3d12_ensure_buffer_size(
            pg, &r->readback_buffer, layout.total_size,
            D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, true,
            "Z24S8 surface readback", errp)) {
        error_prepend(errp,
                      "Direct3D 12 cannot download Z24S8 zeta surface "
                      "addr=0x%" HWADDR_PRIx ", size=%ux%u, "
                      "scaled=%ux%u, pitch=%u: ",
                      surface->vram_addr, surface->width, surface->height,
                      scaled_width, scaled_height, surface->pitch);
        return false;
    }

    if (!pgraph_d3d12_begin_commands(pg, errp)) {
        return false;
    }

    pgraph_d3d12_transition_raw_resource(r, surface->resource, &surface->state,
                                         D3D12_RESOURCE_STATE_COPY_SOURCE);

    set_buffer_texture_copy_location(&dst_loc, r->readback_buffer.resource,
                                     layout.depth_offset,
                                     layout.depth_format, scaled_width,
                                     scaled_height, layout.depth_row_pitch);
    set_texture_subresource_copy_location(&src_loc, surface->resource, 0);
    r->command_list->lpVtbl->CopyTextureRegion(r->command_list, &dst_loc, 0, 0,
                                               0, &src_loc, NULL);

    set_buffer_texture_copy_location(&dst_loc, r->readback_buffer.resource,
                                     layout.stencil_offset,
                                     layout.stencil_format, scaled_width,
                                     scaled_height, layout.stencil_row_pitch);
    set_texture_subresource_copy_location(&src_loc, surface->resource, 1);
    r->command_list->lpVtbl->CopyTextureRegion(r->command_list, &dst_loc, 0, 0,
                                               0, &src_loc, NULL);

    pgraph_d3d12_transition_raw_resource(r, surface->resource, &surface->state,
                                         D3D12_RESOURCE_STATE_DEPTH_WRITE);

    if (!pgraph_d3d12_submit_commands(pg, errp)) {
        return false;
    }
    record_surface_download(r, reason);
    log_surface_writeback(surface, reason, "dl-z24s8");

    if (surface->swizzle) {
        linear = g_malloc(surface->size);
        dst = linear;
    }

    pack_d3d12_planes_to_z24s8(dst, surface->pitch,
                               r->readback_buffer.mapped, &layout,
                               surface->width, surface->height, scale,
                               depth_float);

    if (surface->swizzle) {
        swizzle_rect(linear, surface->width, surface->height,
                     d->vram_ptr + surface->vram_addr, surface->pitch,
                     surface->fmt.bytes_per_pixel);
    }

    memory_region_set_client_dirty(d->vram, surface->vram_addr, dirty_size,
                                   DIRTY_MEMORY_VGA);
    memory_region_set_client_dirty(d->vram, surface->vram_addr, dirty_size,
                                   DIRTY_MEMORY_NV2A_TEX);
    invalidate_surface_vertex_ram_cache(r, surface);
    pgraph_d3d12_mark_textures_possibly_dirty(d, surface->vram_addr,
                                              dirty_size);
    if (surface->color) {
        pgraph_d3d12_invalidate_blit_display_candidates(
            r, surface->vram_addr, dirty_size);
    }

    surface->download_pending = false;
    surface->download_failures = 0;
    pgraph_d3d12_surface_mark_cpu_owned(surface);
    nv2a_profile_inc_counter(NV2A_PROF_SURF_DOWNLOAD);
    return true;
}

static bool download_surface_data_impl(
    NV2AState *d, PGRAPHD3D12SurfaceBinding *surface,
    PGRAPHD3D12SurfaceDownloadReason reason, bool force, Error **errp);

static bool download_surface_data(
    NV2AState *d, PGRAPHD3D12SurfaceBinding *surface,
    PGRAPHD3D12SurfaceDownloadReason reason, bool force, Error **errp)
{
    PGRAPHD3D12State *r = d->pgraph.d3d12_renderer_state;
    int64_t start = g_get_monotonic_time();
    bool ok;

    if (surface->draw_dirty && (surface->download_pending || force)) {
        pgraph_d3d12_complete_readbacks_in_range(
            d, surface->vram_addr, surface->size,
            PGRAPH_D3D12_STASH_TRIGGER_DOWNLOAD);
    }
    ok = download_surface_data_impl(d, surface, reason, force, errp);

    r->perf.surface_download_us += g_get_monotonic_time() - start;
    return ok;
}

/* Guest memory holds the surface's bytes again: tell the consumers that
 * track those pages and hand the surface to the CPU. */
static void finish_surface_download(NV2AState *d,
                                    PGRAPHD3D12SurfaceBinding *surface)
{
    PGRAPHD3D12State *r = d->pgraph.d3d12_renderer_state;
    hwaddr dirty_size = (hwaddr)surface->pitch * surface->height;

    memory_region_set_client_dirty(d->vram, surface->vram_addr, dirty_size,
                                   DIRTY_MEMORY_VGA);
    memory_region_set_client_dirty(d->vram, surface->vram_addr, dirty_size,
                                   DIRTY_MEMORY_NV2A_TEX);
    invalidate_surface_vertex_ram_cache(r, surface);
    pgraph_d3d12_mark_textures_possibly_dirty(d, surface->vram_addr,
                                              dirty_size);
    if (surface->color) {
        pgraph_d3d12_invalidate_blit_display_candidates(
            r, surface->vram_addr, dirty_size);
    }

    surface->download_pending = false;
    surface->download_failures = 0;
    pgraph_d3d12_surface_mark_cpu_owned(surface);
    nv2a_profile_inc_counter(NV2A_PROF_SURF_DOWNLOAD);
}

static bool download_surface_data_impl(
    NV2AState *d, PGRAPHD3D12SurfaceBinding *surface,
    PGRAPHD3D12SurfaceDownloadReason reason, bool force, Error **errp)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    unsigned int scale = MAX(pg->surface_scale_factor, 1);
    unsigned int scaled_width = surface->width;
    unsigned int scaled_height = surface->height;
    hwaddr dirty_size = (hwaddr)surface->pitch * surface->height;
    uint32_t row_bytes;
    uint32_t row_pitch;
    uint8_t *dst = d->vram_ptr + surface->vram_addr;
    g_autofree uint8_t *linear = NULL;
    unsigned int native_y;

    pgraph_d3d12_evlog(r, "dl %c 0x%" HWADDR_PRIx " %ux%u dd=%d dp=%d force=%d reason=%d",
                       surface->color ? 'c' : 'z', surface->vram_addr,
                       surface->width, surface->height, surface->draw_dirty,
                       surface->download_pending, force, (int)reason);

    if (!(surface->download_pending || force) || !surface->draw_dirty) {
        return true;
    }

    if (surface_is_z24s8(surface)) {
        return download_z24s8_surface_data(d, surface, reason, force, errp);
    }

    surface->download_pending = true;

    if (!surface->width || !surface->height) {
        surface->download_pending = false;
        pgraph_d3d12_surface_mark_cpu_owned(surface);
        return true;
    }

    pgraph_apply_scaling_factor(pg, &scaled_width, &scaled_height);
    row_bytes = scaled_width * surface->fmt.bytes_per_pixel;
    row_pitch = ROUND_UP(row_bytes, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    if (!surface->color &&
        !require_whole_zeta_transfer_fits(
            d->pgraph.d3d12_renderer_state, surface,
            (uint64_t)row_pitch * scaled_height,
            (uint64_t)row_pitch * scale, "download", errp)) {
        return false;
    }

    if (surface->swizzle) {
        linear = g_malloc(surface->size);
        dst = linear;
    }

    for (native_y = 0; native_y < surface->height;) {
        unsigned int slab_native_rows = surface->color ?
            surface_transfer_slab_native_rows(
                row_pitch, scale, surface->height - native_y) :
            surface->height;
        unsigned int slab_scaled_height = slab_native_rows * scale;
        uint64_t readback_size = (uint64_t)row_pitch * slab_scaled_height;
        D3D12_TEXTURE_COPY_LOCATION dst_loc;
        D3D12_TEXTURE_COPY_LOCATION src_loc;
        D3D12_BOX src_box;
        const D3D12_BOX *src_box_ptr = NULL;
        bool final_slab = native_y + slab_native_rows == surface->height;

        if (!pgraph_d3d12_ensure_buffer_size(
                pg, &r->readback_buffer, readback_size,
                D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST,
                true, "surface readback slab", errp)) {
            error_prepend(errp,
                          "Direct3D 12 cannot download %s surface slab "
                          "addr=0x%" HWADDR_PRIx ", native rows=%u..%u, "
                          "scaled=%ux%u, pitch=%u: ",
                          surface->color ? "color" : "zeta",
                          surface->vram_addr, native_y,
                          native_y + slab_native_rows, scaled_width,
                          scaled_height, surface->pitch);
            return false;
        }

        if (!pgraph_d3d12_begin_commands(pg, errp)) {
            return false;
        }

        pgraph_d3d12_transition_raw_resource(
            r, surface->resource, &surface->state,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
        set_buffer_texture_copy_location(
            &dst_loc, r->readback_buffer.resource, 0,
            surface->fmt.dxgi_format, scaled_width, slab_scaled_height,
            row_pitch);
        set_texture_subresource_copy_location(&src_loc, surface->resource, 0);

        if (surface->color) {
            memset(&src_box, 0, sizeof(src_box));
            src_box.left = 0;
            src_box.top = native_y * scale;
            src_box.front = 0;
            src_box.right = scaled_width;
            src_box.bottom = src_box.top + slab_scaled_height;
            src_box.back = 1;
            src_box_ptr = &src_box;
        }

        r->command_list->lpVtbl->CopyTextureRegion(
            r->command_list, &dst_loc, 0, 0, 0, &src_loc, src_box_ptr);

        if (final_slab) {
            pgraph_d3d12_transition_raw_resource(
                r, surface->resource, &surface->state,
                surface->color ? D3D12_RESOURCE_STATE_RENDER_TARGET :
                                 D3D12_RESOURCE_STATE_DEPTH_WRITE);
        }

        if (!pgraph_d3d12_submit_commands(pg, errp)) {
            return false;
        }
        record_surface_download(r, reason);
        log_surface_writeback(surface, reason, "dl-sync");

        copy_shrunk_rows(
            dst + (uint64_t)native_y * surface->pitch, surface->pitch,
            r->readback_buffer.mapped, row_pitch, surface->width,
            slab_native_rows, surface->fmt.bytes_per_pixel, scale);
        if (surface_is_float_z16(surface)) {
            convert_float_z16_rows(dst + (uint64_t)native_y * surface->pitch,
                                   surface->pitch, surface->width,
                                   slab_native_rows, true);
        }
        native_y += slab_native_rows;
    }

    if (surface->swizzle) {
        swizzle_rect(linear, surface->width, surface->height,
                     d->vram_ptr + surface->vram_addr, surface->pitch,
                     surface->fmt.bytes_per_pixel);
    }

    memory_region_set_client_dirty(d->vram, surface->vram_addr, dirty_size,
                                   DIRTY_MEMORY_VGA);
    memory_region_set_client_dirty(d->vram, surface->vram_addr, dirty_size,
                                   DIRTY_MEMORY_NV2A_TEX);
    invalidate_surface_vertex_ram_cache(r, surface);
    pgraph_d3d12_mark_textures_possibly_dirty(d, surface->vram_addr,
                                              dirty_size);
    if (surface->color) {
        pgraph_d3d12_invalidate_blit_display_candidates(
            r, surface->vram_addr, dirty_size);
    }

    surface->download_pending = false;
    surface->download_failures = 0;
    pgraph_d3d12_surface_mark_cpu_owned(surface);
    nv2a_profile_inc_counter(NV2A_PROF_SURF_DOWNLOAD);
    return true;
}

/*
 * Displaced surface stash.
 *
 * A dirty surface displaced by an overlapping target keeps its image
 * instead of being written back to guest memory, until something needs
 * those bytes (texture fetch, blit, CPU access, upload, overlapping
 * write-back, CPU display or overlay, full sync). Write-backs land in stash
 * order with every older stash sharing bytes, so the last GPU writer lands
 * last; a newer stash covering an older one entirely retires it. A surface
 * of the same shape recreated at a stash's address takes the image back,
 * patched by GPU block copies from newer stashes, so per-frame target
 * rebuilds cost no round trip. The stash relies on the TCG watchpoints;
 * other accelerators write back at once.
 */
#define SURFACE_READBACK_BATCH_MAX 16
#define SURFACE_STASH_MAX 32
#define SURFACE_STASH_MAX_BYTES (64u * 1024 * 1024)

/*
 * Several colour surfaces written back with one submission: each whole
 * scaled image is copied to its own offset of the readback buffer, the
 * queue is drained once, and the images are unpacked in array order, so
 * overlapping surfaces land in the same order as one-by-one downloads.
 * A batch stays within one transfer slab, so the readback buffer never
 * grows for it. Zeta surfaces and images larger than a slab take the
 * single-surface path in their turn.
 */
static bool surface_batch_readback_layout(PGRAPHState *pg,
                                          const PGRAPHD3D12SurfaceBinding *s,
                                          uint32_t *row_pitch,
                                          uint64_t *bytes)
{
    unsigned int width = s->width;
    unsigned int height = s->height;

    if (!s->color || s->resource == NULL || width == 0 || height == 0) {
        return false;
    }
    pgraph_apply_scaling_factor(pg, &width, &height);
    *row_pitch = ROUND_UP(width * s->fmt.bytes_per_pixel,
                          D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    *bytes = (uint64_t)*row_pitch * height;
    return *bytes <= D3D12_SURFACE_TRANSFER_SLAB_SIZE;
}

static bool download_surfaces_batched(
    NV2AState *d, PGRAPHD3D12SurfaceBinding *const *surfaces,
    unsigned int count, PGRAPHD3D12SurfaceDownloadReason reason,
    Error **errp)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    unsigned int scale = MAX(pg->surface_scale_factor, 1);
    unsigned int i = 0;

    while (i < count) {
        PGRAPHD3D12SurfaceBinding *batch[SURFACE_STASH_MAX];
        uint64_t offsets[SURFACE_STASH_MAX];
        uint32_t pitches[SURFACE_STASH_MAX];
        unsigned int batch_count = 0;
        uint64_t total = 0;

        while (i < count && batch_count < ARRAY_SIZE(batch)) {
            PGRAPHD3D12SurfaceBinding *surface = surfaces[i];
            uint32_t row_pitch;
            uint64_t bytes;
            uint64_t offset;

            if (surface == NULL || !surface->draw_dirty) {
                i++;
                continue;
            }
            if (!surface_batch_readback_layout(pg, surface, &row_pitch,
                                               &bytes)) {
                break;
            }
            offset = ROUND_UP(total, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
            if (offset + bytes > D3D12_SURFACE_TRANSFER_SLAB_SIZE) {
                break;
            }
            batch[batch_count] = surface;
            offsets[batch_count] = offset;
            pitches[batch_count] = row_pitch;
            batch_count++;
            total = offset + bytes;
            i++;
        }

        if (batch_count == 0) {
            if (i == count) {
                break;
            }
            if (!download_surface_data_impl(d, surfaces[i], reason, true,
                                            errp)) {
                return false;
            }
            i++;
            continue;
        }

        if (!pgraph_d3d12_ensure_buffer_size(
                pg, &r->readback_buffer, total, D3D12_HEAP_TYPE_READBACK,
                D3D12_RESOURCE_STATE_COPY_DEST, true,
                "surface readback batch", errp) ||
            !pgraph_d3d12_begin_commands(pg, errp)) {
            return false;
        }
        for (unsigned int b = 0; b < batch_count; b++) {
            PGRAPHD3D12SurfaceBinding *surface = batch[b];
            unsigned int width = surface->width;
            unsigned int height = surface->height;
            D3D12_TEXTURE_COPY_LOCATION dst_loc;
            D3D12_TEXTURE_COPY_LOCATION src_loc;
            D3D12_BOX box;

            pgraph_apply_scaling_factor(pg, &width, &height);
            memset(&box, 0, sizeof(box));
            box.right = width;
            box.bottom = height;
            box.back = 1;
            pgraph_d3d12_transition_raw_resource(
                r, surface->resource, &surface->state,
                D3D12_RESOURCE_STATE_COPY_SOURCE);
            set_buffer_texture_copy_location(
                &dst_loc, r->readback_buffer.resource, offsets[b],
                surface->fmt.dxgi_format, width, height, pitches[b]);
            set_texture_subresource_copy_location(&src_loc, surface->resource,
                                                  0);
            r->command_list->lpVtbl->CopyTextureRegion(
                r->command_list, &dst_loc, 0, 0, 0, &src_loc, &box);
            pgraph_d3d12_transition_raw_resource(
                r, surface->resource, &surface->state,
                D3D12_RESOURCE_STATE_RENDER_TARGET);
        }
        if (!pgraph_d3d12_submit_commands(pg, errp)) {
            return false;
        }

        for (unsigned int b = 0; b < batch_count; b++) {
            PGRAPHD3D12SurfaceBinding *surface = batch[b];
            uint8_t *dst = d->vram_ptr + surface->vram_addr;
            g_autofree uint8_t *linear = NULL;

            if (surface->swizzle) {
                linear = g_malloc(surface->size);
                dst = linear;
            }
            copy_shrunk_rows(dst, surface->pitch,
                             (uint8_t *)r->readback_buffer.mapped + offsets[b],
                             pitches[b], surface->width, surface->height,
                             surface->fmt.bytes_per_pixel, scale);
            if (surface->swizzle) {
                swizzle_rect(linear, surface->width, surface->height,
                             d->vram_ptr + surface->vram_addr, surface->pitch,
                             surface->fmt.bytes_per_pixel);
            }
            record_surface_download(r, reason);
            log_surface_writeback(surface, reason, "dl-batch");
            finish_surface_download(d, surface);
        }
    }
    return true;
}

static bool surface_overlaps_range(PGRAPHD3D12SurfaceBinding const *surface,
                                   hwaddr range_start, hwaddr range_len);
static void release_detached_surface(NV2AState *d,
                                     PGRAPHD3D12SurfaceBinding *surface);
static void unregister_cpu_access_callback(
    NV2AState *d, PGRAPHD3D12SurfaceBinding *surface);
static void register_cpu_access_callback(
    NV2AState *d, PGRAPHD3D12SurfaceBinding *surface);
static void unbind_surface(PGRAPHState *pg, bool color);
static uint64_t swizzled_image_bytes(const PGRAPHD3D12SurfaceBinding *s);

/* Byte range a surface's CPU watchpoint covers, padding included. */
static bool surface_watch_overlaps(const PGRAPHD3D12SurfaceBinding *surface,
                                   hwaddr addr, hwaddr len)
{
    return len != 0 && surface->vram_addr < addr + len &&
           addr < surface->vram_addr + surface->size;
}

static uint64_t surface_image_bytes(PGRAPHState *pg,
                                    const PGRAPHD3D12SurfaceBinding *surface)
{
    unsigned int width = MAX(surface->width, 1);
    unsigned int height = MAX(surface->height, 1);

    pgraph_apply_scaling_factor(pg, &width, &height);
    return (uint64_t)width * height * MAX(surface->fmt.bytes_per_pixel, 1);
}

static void unstash_surface(NV2AState *d, PGRAPHD3D12SurfaceBinding *surface)
{
    PGRAPHD3D12State *r = d->pgraph.d3d12_renderer_state;

    QTAILQ_REMOVE(&r->stashed_surfaces, surface, entry);
    r->stashed_surface_count--;
    r->stashed_surface_bytes -= surface_image_bytes(&d->pgraph, surface);
}

/* Writes a stashed image back to guest memory and releases it. */
static void complete_stashed_surface(NV2AState *d,
                                     PGRAPHD3D12SurfaceBinding *surface,
                                     PGRAPHD3D12StashTrigger trigger)
{
    PGRAPHD3D12State *r = d->pgraph.d3d12_renderer_state;
    Error *local_err = NULL;
    int64_t start = g_get_monotonic_time();

    r->perf.stash_done_by[trigger]++;
    pgraph_d3d12_evlog(r, "stash-wb %c 0x%" HWADDR_PRIx " %ux%u dd=%d trig=%d",
                       surface->color ? 'c' : 'z', surface->vram_addr,
                       surface->width, surface->height, surface->draw_dirty,
                       (int)trigger);
    unstash_surface(d, surface);
    if (!download_surface_data_impl(
            d, surface, PGRAPH_D3D12_SURFACE_DOWNLOAD_OVERLAP_INVALIDATION,
            true, &local_err)) {
        report_surface_error("stashed surface write-back", surface,
                             local_err);
        error_free(local_err);
    }
    r->perf.readbacks_completed++;
    r->perf.readback_complete_us += g_get_monotonic_time() - start;
    release_detached_surface(d, surface);
}

/* Writes several stashed images back with one submission, in array
 * order, and releases them. */
static void complete_stashed_surfaces(NV2AState *d,
                                      PGRAPHD3D12SurfaceBinding **stashes,
                                      unsigned int count,
                                      PGRAPHD3D12StashTrigger trigger)
{
    PGRAPHD3D12State *r = d->pgraph.d3d12_renderer_state;
    Error *local_err = NULL;
    int64_t start = g_get_monotonic_time();

    if (count == 0) {
        return;
    }
    for (unsigned int i = 0; i < count; i++) {
        r->perf.stash_done_by[trigger]++;
        unstash_surface(d, stashes[i]);
    }
    if (!download_surfaces_batched(
            d, stashes, count,
            PGRAPH_D3D12_SURFACE_DOWNLOAD_OVERLAP_INVALIDATION, &local_err)) {
        error_free(local_err);
        /* Retry what the batch left behind one by one, in order. */
        for (unsigned int i = 0; i < count; i++) {
            local_err = NULL;
            if (stashes[i]->draw_dirty &&
                !download_surface_data_impl(
                    d, stashes[i],
                    PGRAPH_D3D12_SURFACE_DOWNLOAD_OVERLAP_INVALIDATION, true,
                    &local_err)) {
                report_surface_error("stashed surface write-back", stashes[i],
                                     local_err);
                error_free(local_err);
            }
        }
    }
    for (unsigned int i = 0; i < count; i++) {
        r->perf.readbacks_completed++;
        release_detached_surface(d, stashes[i]);
    }
    r->perf.readback_complete_us += g_get_monotonic_time() - start;
}

static void drop_stashed_surface(NV2AState *d,
                                 PGRAPHD3D12SurfaceBinding *surface)
{
    PGRAPHD3D12State *r = d->pgraph.d3d12_renderer_state;

    unstash_surface(d, surface);
    r->perf.readbacks_dropped++;
    release_detached_surface(d, surface);
}

/*
 * Writes back the stashes whose bytes overlap [addr, addr + size), older
 * overlapping stashes first. With watched set, a stash is selected when the
 * range touches its watched span, padding included, so an access that
 * trapped on padding retires the stash instead of trapping again.
 */
static bool complete_readbacks_in_range(NV2AState *d, hwaddr addr,
                                        uint64_t size,
                                        PGRAPHD3D12StashTrigger trigger,
                                        bool watched)
{
    PGRAPHD3D12State *r = d->pgraph.d3d12_renderer_state;
    PGRAPHD3D12SurfaceBinding *surface, *older;
    bool changed;
    bool any = false;

    if (r == NULL || size == 0 || QTAILQ_EMPTY(&r->stashed_surfaces)) {
        return false;
    }

    QTAILQ_FOREACH(surface, &r->stashed_surfaces, entry) {
        surface->stash_selected =
            watched ? surface_watch_overlaps(surface, addr, size) :
                      surface_overlaps_range(surface, addr, size);
    }
    /* Older stashes sharing bytes with a selected one must land first. */
    do {
        changed = false;
        QTAILQ_FOREACH(surface, &r->stashed_surfaces, entry) {
            if (!surface->stash_selected) {
                continue;
            }
            for (older = QTAILQ_FIRST(&r->stashed_surfaces); older != surface;
                 older = QTAILQ_NEXT(older, entry)) {
                if (!older->stash_selected &&
                    surface_overlaps_range(older, surface->vram_addr,
                                           surface->size)) {
                    older->stash_selected = true;
                    changed = true;
                }
            }
        }
    } while (changed);

    for (;;) {
        PGRAPHD3D12SurfaceBinding *selected[SURFACE_STASH_MAX];
        unsigned int count = 0;

        QTAILQ_FOREACH(surface, &r->stashed_surfaces, entry) {
            if (surface->stash_selected && count < ARRAY_SIZE(selected)) {
                selected[count++] = surface;
            }
        }
        if (count == 0) {
            return any;
        }
        complete_stashed_surfaces(d, selected, count, trigger);
        any = true;
    }
}

bool pgraph_d3d12_complete_readbacks_in_range(
    NV2AState *d, hwaddr addr, uint64_t size,
    PGRAPHD3D12StashTrigger trigger)
{
    return complete_readbacks_in_range(d, addr, size, trigger, false);
}

void pgraph_d3d12_drop_readbacks_within(NV2AState *d, hwaddr addr,
                                        uint64_t size)
{
    PGRAPHD3D12State *r = d->pgraph.d3d12_renderer_state;
    PGRAPHD3D12SurfaceBinding *surface, *next;

    if (r == NULL || size == 0) {
        return;
    }
    QTAILQ_FOREACH_SAFE(surface, &r->stashed_surfaces, entry, next) {
        if (surface->vram_addr >= addr &&
            surface->vram_addr + surface->size <= addr + size) {
            drop_stashed_surface(d, surface);
        }
    }
}

void pgraph_d3d12_drop_all_stashes(NV2AState *d)
{
    PGRAPHD3D12State *r = d->pgraph.d3d12_renderer_state;
    PGRAPHD3D12SurfaceBinding *surface;

    if (r == NULL) {
        return;
    }
    while ((surface = QTAILQ_FIRST(&r->stashed_surfaces)) != NULL) {
        drop_stashed_surface(d, surface);
    }
    r->cpu_access_range_count = 0;
    r->cpu_access_range_overflow = false;
}

void pgraph_d3d12_complete_all_readbacks(NV2AState *d,
                                         PGRAPHD3D12StashTrigger trigger)
{
    PGRAPHD3D12State *r = d->pgraph.d3d12_renderer_state;
    PGRAPHD3D12SurfaceBinding *surface;

    if (r == NULL) {
        return;
    }
    while (!QTAILQ_EMPTY(&r->stashed_surfaces)) {
        PGRAPHD3D12SurfaceBinding *all[SURFACE_STASH_MAX];
        unsigned int count = 0;

        QTAILQ_FOREACH(surface, &r->stashed_surfaces, entry) {
            if (count == ARRAY_SIZE(all)) {
                break;
            }
            all[count++] = surface;
        }
        complete_stashed_surfaces(d, all, count, trigger);
    }
}

static void stash_surface(NV2AState *d, PGRAPHD3D12SurfaceBinding *surface,
                          PGRAPHD3D12SurfaceDownloadReason reason)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    PGRAPHD3D12SurfaceBinding *older, *next;
    uint64_t bytes = surface_image_bytes(pg, surface);

    pgraph_d3d12_evlog(r, "stash %c 0x%" HWADDR_PRIx " %ux%u dd=%d reason=%d",
                       surface->color ? 'c' : 'z', surface->vram_addr,
                       surface->width, surface->height, surface->draw_dirty,
                       (int)reason);

    if (surface == r->color_binding) {
        unbind_surface(pg, true);
    }
    if (surface == r->zeta_binding) {
        unbind_surface(pg, false);
    }
    if (surface == r->display_surface_binding) {
        r->display_surface_binding = NULL;
        r->display_surface_valid = false;
    }
    QTAILQ_REMOVE(&r->surfaces, surface, entry);

    /* A newer image covering an older stash entirely retires it. Only the
     * bytes the write-back produces count: a swizzled image writes
     * width * height * bpp, not its padded size. */
    if (surface->swizzle ||
        surface->pitch == surface->width * surface->fmt.bytes_per_pixel) {
        uint64_t written = surface->swizzle ? swizzled_image_bytes(surface) :
                                              surface->size;

        QTAILQ_FOREACH_SAFE(older, &r->stashed_surfaces, entry, next) {
            if (older->vram_addr >= surface->vram_addr &&
                older->vram_addr + older->size <=
                    surface->vram_addr + written) {
                drop_stashed_surface(d, older);
            }
        }
    }
    while (r->stashed_surface_count != 0 &&
           (r->stashed_surface_count >= SURFACE_STASH_MAX ||
            r->stashed_surface_bytes + bytes > SURFACE_STASH_MAX_BYTES)) {
        complete_stashed_surface(d, QTAILQ_FIRST(&r->stashed_surfaces),
                                 PGRAPH_D3D12_STASH_TRIGGER_CAP);
    }

    /* The live watchpoint already covers the stash's bytes and matches
     * images by address, so it carries over; a dirty surface has it armed. */
    if (surface->access_cb == NULL) {
        register_cpu_access_callback(d, surface);
    }
    if (tcg_enabled()) {
        mem_access_callback_set_enabled(qemu_get_cpu(0), surface->access_cb,
                                        true);
    }

    QTAILQ_INSERT_TAIL(&r->stashed_surfaces, surface, entry);
    r->surface_layout_gen++;
    r->stashed_surface_count++;
    r->stashed_surface_bytes += bytes;
    record_surface_download(r, reason);
    log_surface_writeback(surface, reason, "stash");
    r->perf.readbacks_recorded++;
}

bool pgraph_d3d12_record_surface_readbacks(
    NV2AState *d, PGRAPHD3D12SurfaceBinding *const *surfaces,
    unsigned int count, PGRAPHD3D12SurfaceDownloadReason reason,
    Error **errp)
{
    for (unsigned int i = 0; i < count; i++) {
        PGRAPHD3D12SurfaceBinding *surface = surfaces[i];

        if (surface == NULL || !surface->draw_dirty) {
            continue;
        }
        if (!tcg_enabled() || surface->resource == NULL ||
            surface->upload_pending ||
            (d->pgraph.d3d12_renderer_state->tune_flags &
             PGRAPH_D3D12_TUNE_STASH_OFF)) {
            if (!download_surface_data(d, surface, reason, true, errp)) {
                return false;
            }
            continue;
        }
        stash_surface(d, surface, reason);
    }
    return true;
}

bool pgraph_d3d12_download_surfaces_checked_reason(
    NV2AState *d, PGRAPHD3D12SurfaceBinding *const *surfaces,
    unsigned int count, PGRAPHD3D12SurfaceDownloadReason reason,
    Error **errp)
{
    PGRAPHD3D12State *r = d->pgraph.d3d12_renderer_state;
    int64_t start = g_get_monotonic_time();
    bool ok = true;

    for (unsigned int base = 0; base < count && ok;
         base += SURFACE_READBACK_BATCH_MAX) {
        PGRAPHD3D12SurfaceBinding *dirty[SURFACE_READBACK_BATCH_MAX];
        unsigned int n = 0;

        for (unsigned int i = base;
             i < count && i < base + SURFACE_READBACK_BATCH_MAX; i++) {
            PGRAPHD3D12SurfaceBinding *surface = surfaces[i];

            if (surface == NULL || !surface->draw_dirty) {
                continue;
            }
            /* Older stashed bytes under the surface land first. */
            pgraph_d3d12_complete_readbacks_in_range(
                d, surface->vram_addr, surface->size,
                PGRAPH_D3D12_STASH_TRIGGER_DOWNLOAD);
            dirty[n++] = surface;
        }
        ok = download_surfaces_batched(d, dirty, n, reason, errp);
    }
    r->perf.surface_download_us += g_get_monotonic_time() - start;
    return ok;
}

static uint64_t swizzled_image_bytes(const PGRAPHD3D12SurfaceBinding *s)
{
    return (uint64_t)s->width * s->height * s->fmt.bytes_per_pixel;
}

static bool stash_images_overlap(const PGRAPHD3D12SurfaceBinding *a,
                                 const PGRAPHD3D12SurfaceBinding *b)
{
    uint64_t a_end = a->vram_addr +
                     (a->swizzle ? swizzled_image_bytes(a) : a->size);
    uint64_t b_end = b->vram_addr +
                     (b->swizzle ? swizzled_image_bytes(b) : b->size);

    return a->vram_addr < b_end && b->vram_addr < a_end;
}

typedef struct SwizzleBlock {
    unsigned int src_x;
    unsigned int src_y;
    unsigned int dst_x;
    unsigned int dst_y;
    unsigned int width;
    unsigned int height;
} SwizzleBlock;

static void swizzle_masks(unsigned int width, unsigned int height,
                          uint32_t *mask_x, uint32_t *mask_y)
{
    uint32_t bit = 1;
    uint32_t mask_bit = 1;
    bool done;

    *mask_x = 0;
    *mask_y = 0;
    do {
        done = true;
        if (bit < width) {
            *mask_x |= mask_bit;
            mask_bit <<= 1;
            done = false;
        }
        if (bit < height) {
            *mask_y |= mask_bit;
            mask_bit <<= 1;
            done = false;
        }
        bit <<= 1;
    } while (!done);
}

static bool is_power_of_two(unsigned int value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

/*
 * Splits the bytes two swizzled images of one format share into aligned
 * blocks. Inside the bits both images interleave, an aligned block of 2^m
 * pixels is a rectangle with identical pixel order in both images: x takes
 * the even bits, so an odd m doubles the width. Returns 0 when the overlap
 * cannot be expressed that way.
 */
static unsigned int swizzled_overlap_blocks(
    const PGRAPHD3D12SurfaceBinding *src,
    const PGRAPHD3D12SurfaceBinding *dst, SwizzleBlock *blocks,
    unsigned int max_blocks)
{
    unsigned int bpp = src->fmt.bytes_per_pixel;
    uint64_t lo = MAX(src->vram_addr, dst->vram_addr);
    uint64_t hi = MIN(src->vram_addr + swizzled_image_bytes(src),
                      dst->vram_addr + swizzled_image_bytes(dst));
    unsigned int side = MIN(MIN(src->width, src->height),
                            MIN(dst->width, dst->height));
    uint64_t limit = (uint64_t)side * side * bpp;
    uint32_t src_mask_x, src_mask_y, dst_mask_x, dst_mask_y;
    unsigned int count = 0;

    if (bpp == 0 || lo >= hi || !is_power_of_two(src->width) ||
        !is_power_of_two(src->height) || !is_power_of_two(dst->width) ||
        !is_power_of_two(dst->height)) {
        return 0;
    }
    swizzle_masks(src->width, src->height, &src_mask_x, &src_mask_y);
    swizzle_masks(dst->width, dst->height, &dst_mask_x, &dst_mask_y);

    for (uint64_t offset = lo; offset < hi;) {
        uint64_t src_offset = offset - src->vram_addr;
        uint64_t dst_offset = offset - dst->vram_addr;
        uint64_t bytes = bpp;
        unsigned int bits = 0;

        while (bytes * 2 <= hi - offset && bytes * 2 <= limit &&
               src_offset % (bytes * 2) == 0 &&
               dst_offset % (bytes * 2) == 0) {
            bytes *= 2;
            bits++;
        }
        if (src_offset % bytes != 0 || dst_offset % bytes != 0 ||
            count == max_blocks) {
            return 0;
        }
        blocks[count].width = 1u << ((bits + 1) / 2);
        blocks[count].height = 1u << (bits / 2);
        blocks[count].src_x =
            pgraph_d3d12_gather_bits((uint32_t)(src_offset / bpp), src_mask_x);
        blocks[count].src_y =
            pgraph_d3d12_gather_bits((uint32_t)(src_offset / bpp), src_mask_y);
        blocks[count].dst_x =
            pgraph_d3d12_gather_bits((uint32_t)(dst_offset / bpp), dst_mask_x);
        blocks[count].dst_y =
            pgraph_d3d12_gather_bits((uint32_t)(dst_offset / bpp), dst_mask_y);
        count++;
        offset += bytes;
    }
    return count;
}

static bool stash_patchable(const PGRAPHD3D12SurfaceBinding *stash,
                            const PGRAPHD3D12SurfaceBinding *target,
                            SwizzleBlock *blocks, unsigned int max_blocks)
{
    /* Block copies take a source box, which D3D12 allows only for colour
     * resources; depth/stencil restores are whole-resource adoptions. */
    return stash->color && target->color && stash->swizzle &&
           target->swizzle &&
           stash->fmt.dxgi_format == target->fmt.dxgi_format &&
           stash->fmt.bytes_per_pixel == target->fmt.bytes_per_pixel &&
           stash->shape.anti_aliasing == target->shape.anti_aliasing &&
           stash->resource != NULL && stash->draw_dirty &&
           !stash->upload_pending &&
           swizzled_overlap_blocks(stash, target, blocks, max_blocks) != 0;
}

static bool stash_same_shape(const PGRAPHD3D12SurfaceBinding *stash,
                             const PGRAPHD3D12SurfaceBinding *target)
{
    return stash->vram_addr == target->vram_addr &&
           stash->color == target->color &&
           stash->width == target->width &&
           stash->height == target->height && stash->size == target->size &&
           stash->fmt.dxgi_format == target->fmt.dxgi_format &&
           stash->swizzle == target->swizzle &&
           (stash->swizzle || stash->pitch == target->pitch) &&
           stash->shape.anti_aliasing == target->shape.anti_aliasing &&
           stash->resource != NULL && stash->draw_dirty &&
           !stash->upload_pending;
}

/* A stashed swizzled image that holds every byte of the target's image. */
static bool stash_covers(const PGRAPHD3D12SurfaceBinding *stash,
                         const PGRAPHD3D12SurfaceBinding *target,
                         SwizzleBlock *blocks, unsigned int max_blocks)
{
    return stash->vram_addr <= target->vram_addr &&
           stash->vram_addr + swizzled_image_bytes(stash) >=
               target->vram_addr + swizzled_image_bytes(target) &&
           stash_patchable(stash, target, blocks, max_blocks);
}

static void copy_stash_blocks(PGRAPHD3D12State *r, unsigned int scale,
                              PGRAPHD3D12SurfaceBinding *source,
                              PGRAPHD3D12SurfaceBinding *target,
                              const SwizzleBlock *blocks,
                              unsigned int block_count)
{
    pgraph_d3d12_transition_raw_resource(r, source->resource, &source->state,
                                         D3D12_RESOURCE_STATE_COPY_SOURCE);
    pgraph_d3d12_transition_raw_resource(r, target->resource, &target->state,
                                         D3D12_RESOURCE_STATE_COPY_DEST);
    for (unsigned int i = 0; i < block_count; i++) {
        D3D12_TEXTURE_COPY_LOCATION dst;
        D3D12_TEXTURE_COPY_LOCATION src;
        D3D12_BOX box = {
            .left = blocks[i].src_x * scale,
            .top = blocks[i].src_y * scale,
            .front = 0,
            .right = (blocks[i].src_x + blocks[i].width) * scale,
            .bottom = (blocks[i].src_y + blocks[i].height) * scale,
            .back = 1,
        };

        set_texture_subresource_copy_location(&dst, target->resource, 0);
        set_texture_subresource_copy_location(&src, source->resource, 0);
        r->command_list->lpVtbl->CopyTextureRegion(
            r->command_list, &dst, blocks[i].dst_x * scale,
            blocks[i].dst_y * scale, 0, &src, &box);
    }
}

/*
 * A recreated surface that needs its previous contents takes them from the
 * stash instead of guest memory: the newest stash of the same shape at its
 * address hands its image over, or the newest stash whose image covers the
 * target's bytes is block-copied into the fresh image. Bytes that stashes
 * newer than that base overwrote are then patched from their images. A
 * stash of the other kind (depth over colour or colour over depth) is not
 * a writer of the target's bytes: hardware would have interleaved both
 * writes, so titles never read such bytes back through the other kind.
 */
static void restore_surface_from_stash(NV2AState *d,
                                       PGRAPHD3D12SurfaceBinding *target)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    PGRAPHD3D12SurfaceBinding *base = NULL;
    PGRAPHD3D12SurfaceBinding *stash, *newer;
    SwizzleBlock blocks[64];
    unsigned int scale = MAX(pg->surface_scale_factor, 1);
    D3D12_RESOURCE_STATES target_state;
    Error *local_err = NULL;
    bool exact = false;

    if (!tcg_enabled() || target->resource == NULL) {
        return;
    }
    QTAILQ_FOREACH(stash, &r->stashed_surfaces, entry) {
        if (stash_same_shape(stash, target)) {
            base = stash;
            exact = true;
        } else if (target->swizzle &&
                   stash_covers(stash, target, blocks, ARRAY_SIZE(blocks))) {
            base = stash;
            exact = false;
        }
    }
    pgraph_d3d12_evlog(r, "restore %c 0x%" HWADDR_PRIx " %ux%u base=%d exact=%d",
                       target->color ? 'c' : 'z', target->vram_addr,
                       target->width, target->height, base != NULL, exact);
    if (base == NULL) {
        r->perf.restore_none++;
        return;
    }
    for (newer = QTAILQ_NEXT(base, entry); newer != NULL;
         newer = QTAILQ_NEXT(newer, entry)) {
        if (newer->color == target->color &&
            stash_images_overlap(newer, target) &&
            !stash_patchable(newer, target, blocks, ARRAY_SIZE(blocks))) {
            if (target->color) {
                r->perf.restore_abort_color++;
            } else {
                r->perf.restore_abort_zeta++;
            }
            return;
        }
    }
    if (!exact) {
        r->perf.restore_cover++;
    }
    if (!pgraph_d3d12_begin_commands(pg, &local_err)) {
        report_surface_error("stash restore", target, local_err);
        error_free(local_err);
        return;
    }

    if (exact) {
        pool_surface_resource(pg, target);
        target->resource = base->resource;
        target->state = base->state;
        base->resource = NULL;
        create_surface_view(r, target);
    }
    target_state = target->state;
    if (!exact) {
        copy_stash_blocks(r, scale, base, target, blocks,
                          swizzled_overlap_blocks(base, target, blocks,
                                                  ARRAY_SIZE(blocks)));
    }
    for (newer = QTAILQ_NEXT(base, entry); newer != NULL;
         newer = QTAILQ_NEXT(newer, entry)) {
        if (newer->color == target->color &&
            stash_images_overlap(newer, target)) {
            copy_stash_blocks(r, scale, newer, target, blocks,
                              swizzled_overlap_blocks(newer, target, blocks,
                                                      ARRAY_SIZE(blocks)));
        }
    }
    pgraph_d3d12_transition_raw_resource(r, target->resource, &target->state,
                                         target_state);

    target->draw_dirty = true;
    target->cleared = false;
    target->upload_pending = false;
    if (exact) {
        unstash_surface(d, base);
        release_detached_surface(d, base);
    }
    r->perf.readbacks_adopted++;
}

/*
 * Write back only the rows of a linear surface that overlap a guest byte
 * range.  The surface stays GPU-owned and draw_dirty; the band is remembered
 * per draw_time so repeated syncs in one frame are free.
 */
static bool download_surface_rows(
    NV2AState *d, PGRAPHD3D12SurfaceBinding *surface,
    PGRAPHD3D12SurfaceDownloadReason reason, uint32_t row_start,
    uint32_t row_end, Error **errp)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    unsigned int scale = MAX(pg->surface_scale_factor, 1);
    unsigned int scaled_width = surface->width;
    unsigned int scaled_height = surface->height;
    uint32_t band_rows = row_end - row_start;
    hwaddr band_addr = surface->vram_addr +
                       (hwaddr)row_start * surface->pitch;
    hwaddr band_size = (hwaddr)band_rows * surface->pitch;
    uint8_t *dst = d->vram_ptr + band_addr;
    D3D12_TEXTURE_COPY_LOCATION dst_loc;
    D3D12_TEXTURE_COPY_LOCATION src_loc;
    D3D12_BOX src_box;

    pgraph_apply_scaling_factor(pg, &scaled_width, &scaled_height);

    memset(&src_box, 0, sizeof(src_box));
    src_box.left = 0;
    src_box.top = row_start * scale;
    src_box.front = 0;
    src_box.right = scaled_width;
    src_box.bottom = row_end * scale;
    src_box.back = 1;

    if (surface_is_z24s8(surface)) {
        D3D12ZetaPlaneLayout layout;
        bool depth_float = zeta_depth_plane_float(pg);

        if (!zeta_plane_layout(surface, scaled_width, band_rows * scale,
                               &layout, errp)) {
            return false;
        }
        if (!pgraph_d3d12_ensure_buffer_size(
                pg, &r->readback_buffer, layout.total_size,
                D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST,
                true, "Z24S8 row band readback", errp)) {
            return false;
        }
        if (!pgraph_d3d12_begin_commands(pg, errp)) {
            return false;
        }

        pgraph_d3d12_transition_raw_resource(
            r, surface->resource, &surface->state,
            D3D12_RESOURCE_STATE_COPY_SOURCE);

        set_buffer_texture_copy_location(&dst_loc,
                                         r->readback_buffer.resource,
                                         layout.depth_offset,
                                         layout.depth_format, scaled_width,
                                         band_rows * scale,
                                         layout.depth_row_pitch);
        set_texture_subresource_copy_location(&src_loc, surface->resource,
                                              0);
        r->command_list->lpVtbl->CopyTextureRegion(r->command_list,
                                                   &dst_loc, 0, 0, 0,
                                                   &src_loc, &src_box);

        set_buffer_texture_copy_location(&dst_loc,
                                         r->readback_buffer.resource,
                                         layout.stencil_offset,
                                         layout.stencil_format,
                                         scaled_width, band_rows * scale,
                                         layout.stencil_row_pitch);
        set_texture_subresource_copy_location(&src_loc, surface->resource,
                                              1);
        r->command_list->lpVtbl->CopyTextureRegion(r->command_list,
                                                   &dst_loc, 0, 0, 0,
                                                   &src_loc, &src_box);

        pgraph_d3d12_transition_raw_resource(
            r, surface->resource, &surface->state,
            D3D12_RESOURCE_STATE_DEPTH_WRITE);

        if (!pgraph_d3d12_submit_commands(pg, errp)) {
            return false;
        }
        record_surface_download(r, reason);
        log_surface_writeback(surface, reason, "dl-band");

        pack_d3d12_planes_to_z24s8(dst, surface->pitch,
                                   r->readback_buffer.mapped, &layout,
                                   surface->width, band_rows, scale,
                                   depth_float);
    } else {
        uint32_t row_bytes = scaled_width * surface->fmt.bytes_per_pixel;
        uint32_t row_pitch = ROUND_UP(row_bytes,
                                      D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
        uint64_t readback_size = (uint64_t)row_pitch * band_rows * scale;

        if (!pgraph_d3d12_ensure_buffer_size(
                pg, &r->readback_buffer, readback_size,
                D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST,
                true, "surface row band readback", errp)) {
            return false;
        }
        if (!pgraph_d3d12_begin_commands(pg, errp)) {
            return false;
        }

        pgraph_d3d12_transition_raw_resource(
            r, surface->resource, &surface->state,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
        set_buffer_texture_copy_location(&dst_loc,
                                         r->readback_buffer.resource, 0,
                                         surface->fmt.dxgi_format,
                                         scaled_width, band_rows * scale,
                                         row_pitch);
        set_texture_subresource_copy_location(&src_loc, surface->resource,
                                              0);
        r->command_list->lpVtbl->CopyTextureRegion(r->command_list,
                                                   &dst_loc, 0, 0, 0,
                                                   &src_loc, &src_box);
        pgraph_d3d12_transition_raw_resource(
            r, surface->resource, &surface->state,
            surface->color ? D3D12_RESOURCE_STATE_RENDER_TARGET :
                             D3D12_RESOURCE_STATE_DEPTH_WRITE);

        if (!pgraph_d3d12_submit_commands(pg, errp)) {
            return false;
        }
        record_surface_download(r, reason);

        copy_shrunk_rows(dst, surface->pitch, r->readback_buffer.mapped,
                         row_pitch, surface->width, band_rows,
                         surface->fmt.bytes_per_pixel, scale);
        if (surface_is_float_z16(surface)) {
            convert_float_z16_rows(dst, surface->pitch, surface->width,
                                   band_rows, true);
        }
    }

    memory_region_set_client_dirty(d->vram, band_addr, band_size,
                                   DIRTY_MEMORY_VGA);
    memory_region_set_client_dirty(d->vram, band_addr, band_size,
                                   DIRTY_MEMORY_NV2A_TEX);
    invalidate_surface_vertex_ram_cache(r, surface);
    pgraph_d3d12_mark_textures_possibly_dirty(d, band_addr, band_size);

    surface->range_download_row_start = row_start;
    surface->range_download_row_end = row_end;
    surface->range_download_draw_time = surface->draw_time;
    surface->range_download_valid = true;
    nv2a_profile_inc_counter(NV2A_PROF_SURF_DOWNLOAD);
    return true;
}

/*
 * Structural download failures (e.g. scaled size over the adapter budget)
 * recur on every retry and would stall every CPU access or texture bind.
 * After repeated failures abandon the write-back and let guest memory go
 * stale for that surface.
 */
static void surface_note_download_failure(PGRAPHD3D12SurfaceBinding *surface)
{
    if (surface == NULL) {
        return;
    }
    if (surface->download_failures < UINT8_MAX) {
        surface->download_failures++;
    }
    if (surface->download_failures == 4) {
        fprintf(stderr,
                "Direct3D 12 renderer: abandoning write-back of %s surface "
                "addr=0x%" HWADDR_PRIx " (%ux%u pitch=%u) after repeated "
                "download failures; its GPU contents will not reach guest "
                "memory\n",
                surface->color ? "color" : "zeta", surface->vram_addr,
                surface->width, surface->height, surface->pitch);
        surface->download_pending = false;
        pgraph_d3d12_surface_mark_cpu_owned(surface);
    }
}

void pgraph_d3d12_surface_download_if_dirty(
    NV2AState *d, PGRAPHD3D12SurfaceBinding *surface)
{
    Error *local_err = NULL;

    if (!pgraph_d3d12_surface_download_if_dirty_checked(d, surface,
                                                        &local_err)) {
        report_surface_error("surface download", surface, local_err);
        error_free(local_err);
        surface_note_download_failure(surface);
    }
}

bool pgraph_d3d12_surface_download_if_dirty_checked(
    NV2AState *d, PGRAPHD3D12SurfaceBinding *surface, Error **errp)
{
    return pgraph_d3d12_surface_download_if_dirty_checked_reason(
        d, surface, PGRAPH_D3D12_SURFACE_DOWNLOAD_DISPLAY_CPU, errp);
}

bool pgraph_d3d12_surface_download_if_dirty_checked_reason(
    NV2AState *d, PGRAPHD3D12SurfaceBinding *surface,
    PGRAPHD3D12SurfaceDownloadReason reason, Error **errp)
{
    if (surface == NULL || !surface->draw_dirty) {
        return true;
    }

    return download_surface_data(d, surface, reason, true, errp);
}

static bool patch_color_clear_pixel(PGRAPHState *pg, uint8_t *pixel,
                                    uint32_t parameter)
{
    uint32_t clear_color = pgraph_reg_r(pg, NV_PGRAPH_COLORCLEARVALUE);

    switch (pg->surface_shape.color_format) {
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_O1R5G5B5:
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_Z1R5G5B5: {
        uint16_t value = pixel[0] | (pixel[1] << 8);

        if (parameter & NV097_CLEAR_SURFACE_R) {
            value = (value & ~0x7c00u) | (clear_color & 0x7c00u);
        }
        if (parameter & NV097_CLEAR_SURFACE_G) {
            value = (value & ~0x03e0u) | (clear_color & 0x03e0u);
        }
        if (parameter & NV097_CLEAR_SURFACE_B) {
            value = (value & ~0x001fu) | (clear_color & 0x001fu);
        }
        if (parameter & NV097_CLEAR_SURFACE_A) {
            value |= 0x8000u;
        }
        pixel[0] = value & 0xff;
        pixel[1] = value >> 8;
        return true;
    }
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5: {
        uint16_t value = pixel[0] | (pixel[1] << 8);

        if (parameter & NV097_CLEAR_SURFACE_R) {
            value = (value & ~0xf800u) | (clear_color & 0xf800u);
        }
        if (parameter & NV097_CLEAR_SURFACE_G) {
            value = (value & ~0x07e0u) | (clear_color & 0x07e0u);
        }
        if (parameter & NV097_CLEAR_SURFACE_B) {
            value = (value & ~0x001fu) | (clear_color & 0x001fu);
        }
        pixel[0] = value & 0xff;
        pixel[1] = value >> 8;
        return true;
    }
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_Z8R8G8B8:
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_O8R8G8B8:
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8:
        if (parameter & NV097_CLEAR_SURFACE_B) {
            pixel[0] = clear_color & 0xff;
        }
        if (parameter & NV097_CLEAR_SURFACE_G) {
            pixel[1] = (clear_color >> 8) & 0xff;
        }
        if (parameter & NV097_CLEAR_SURFACE_R) {
            pixel[2] = (clear_color >> 16) & 0xff;
        }
        if (parameter & NV097_CLEAR_SURFACE_A) {
            pixel[3] =
                pg->surface_shape.color_format ==
                    NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8 ?
                    (clear_color >> 24) & 0xff : 0xff;
        }
        return true;
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_B8:
        if (parameter & NV097_CLEAR_SURFACE_B) {
            pixel[0] = clear_color & 0xff;
        }
        return true;
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_G8B8:
        if (parameter & NV097_CLEAR_SURFACE_B) {
            pixel[0] = clear_color & 0xff;
        }
        if (parameter & NV097_CLEAR_SURFACE_G) {
            pixel[1] = (clear_color >> 8) & 0xff;
        }
        return true;
    default:
        return false;
    }
}

static bool color_clear_needs_cpu(PGRAPHState *pg)
{
    switch (pg->surface_shape.color_format) {
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_B8:
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_G8B8:
        /* pgraph_get_clear_color cannot represent these formats. */
        return true;
    default:
        return false;
    }
}

/*
 * Render-target channels a colour clear writes. The X bits of X1R5G5B5 and
 * X8R8G8B8 are stored in the alpha channel and follow the A flag; R5G6B5
 * has no alpha channel.
 */
static UINT8 color_clear_write_mask(PGRAPHState *pg, uint32_t parameter)
{
    UINT8 mask = 0;

    if (parameter & NV097_CLEAR_SURFACE_R) {
        mask |= D3D12_COLOR_WRITE_ENABLE_RED;
    }
    if (parameter & NV097_CLEAR_SURFACE_G) {
        mask |= D3D12_COLOR_WRITE_ENABLE_GREEN;
    }
    if (parameter & NV097_CLEAR_SURFACE_B) {
        mask |= D3D12_COLOR_WRITE_ENABLE_BLUE;
    }
    if ((parameter & NV097_CLEAR_SURFACE_A) &&
        pg->surface_shape.color_format !=
            NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5) {
        mask |= D3D12_COLOR_WRITE_ENABLE_ALPHA;
    }
    return mask;
}

/* Whether a colour clear writes every channel the surface stores. */
static bool color_clear_writes_all_channels(PGRAPHState *pg,
                                            uint32_t parameter)
{
    UINT8 all = pg->surface_shape.color_format ==
                        NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5 ?
                    (D3D12_COLOR_WRITE_ENABLE_RED |
                     D3D12_COLOR_WRITE_ENABLE_GREEN |
                     D3D12_COLOR_WRITE_ENABLE_BLUE) :
                    D3D12_COLOR_WRITE_ENABLE_ALL;

    return color_clear_write_mask(pg, parameter) == all;
}

/*
 * Clears the requested channels inside the scissor rectangle at render
 * resolution; the other channels and pixels keep their contents.
 */
static void record_masked_color_clear(PGRAPHState *pg,
                                      PGRAPHD3D12SurfaceBinding *surface,
                                      ID3D12PipelineState *pipeline_state,
                                      const float rgba[4],
                                      const D3D12_RECT *rect)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    ID3D12GraphicsCommandList *list = r->command_list;
    unsigned int width = MAX(surface->width, 1);
    unsigned int height = MAX(surface->height, 1);
    D3D12_VIEWPORT viewport;

    pgraph_apply_scaling_factor(pg, &width, &height);
    viewport = (D3D12_VIEWPORT){
        .TopLeftX = 0.0f,
        .TopLeftY = 0.0f,
        .Width = (FLOAT)width,
        .Height = (FLOAT)height,
        .MinDepth = 0.0f,
        .MaxDepth = 1.0f,
    };

    if (!r->record_root_signature_set) {
        list->lpVtbl->SetGraphicsRootSignature(list, r->draw_root_signature);
        r->record_root_signature_set = true;
    }
    list->lpVtbl->OMSetRenderTargets(list, 1, &surface->descriptor, FALSE,
                                     NULL);
    list->lpVtbl->RSSetViewports(list, 1, &viewport);
    list->lpVtbl->RSSetScissorRects(list, 1, rect);
    list->lpVtbl->SetPipelineState(list, pipeline_state);
    list->lpVtbl->OMSetBlendFactor(list, rgba);
    list->lpVtbl->IASetPrimitiveTopology(list,
                                         D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->lpVtbl->DrawInstanced(list, 3, 1, 0, 0);

    /* The next draw rebinds its own targets, viewport and pipeline. */
    r->record_render_targets_valid = false;
    r->record_viewport_valid = false;
    r->record_pipeline_state = pipeline_state;
    memcpy(r->record_blend_factor, rgba, sizeof(r->record_blend_factor));
    r->record_blend_factor_valid = true;
    r->record_topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    r->record_topology_valid = true;
}

static bool patch_color_clear_cpu(NV2AState *d,
                                  PGRAPHD3D12SurfaceBinding *surface,
                                  uint32_t parameter, unsigned int xmin,
                                  unsigned int ymin, unsigned int width,
                                  unsigned int height, Error **errp)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    uint8_t *pixels = d->vram_ptr + surface->vram_addr;
    hwaddr dirty_size = (hwaddr)surface->pitch * surface->height;
    g_autofree uint8_t *linear = NULL;

    if (!download_surface_data(
            d, surface, PGRAPH_D3D12_SURFACE_DOWNLOAD_MASKED_CLEAR, true,
            errp)) {
        return false;
    }

    if (surface->swizzle) {
        linear = g_malloc(surface->size);
        unswizzle_rect(pixels, surface->width, surface->height, linear,
                       surface->pitch, surface->fmt.bytes_per_pixel);
        pixels = linear;
    }

    for (unsigned int y = ymin; y < ymin + height; y++) {
        uint8_t *row = pixels + y * surface->pitch;

        for (unsigned int x = xmin; x < xmin + width; x++) {
            if (!patch_color_clear_pixel(
                    pg, row + x * surface->fmt.bytes_per_pixel, parameter)) {
                error_setg(errp,
                           "unsupported CPU color clear format %s (0x%x), "
                           "parameter=0x%08x, rect=%u,%u %ux%u, "
                           "bytes_per_pixel=%u",
                           color_surface_format_name(
                               pg->surface_shape.color_format),
                           pg->surface_shape.color_format, parameter,
                           xmin, ymin, width, height,
                           surface->fmt.bytes_per_pixel);
                return false;
            }
        }
    }

    if (surface->swizzle) {
        swizzle_rect(linear, surface->width, surface->height,
                     d->vram_ptr + surface->vram_addr, surface->pitch,
                     surface->fmt.bytes_per_pixel);
    }

    memory_region_set_client_dirty(d->vram, surface->vram_addr, dirty_size,
                                   DIRTY_MEMORY_VGA);
    memory_region_set_client_dirty(d->vram, surface->vram_addr, dirty_size,
                                   DIRTY_MEMORY_NV2A_TEX);
    invalidate_surface_vertex_ram_cache(r, surface);
    pgraph_d3d12_mark_textures_possibly_dirty(d, surface->vram_addr,
                                              dirty_size);
    pgraph_d3d12_invalidate_blit_display_candidates(
        r, surface->vram_addr, dirty_size);

    surface->upload_pending = true;
    pgraph_d3d12_surface_mark_cpu_owned(surface);
    return pgraph_d3d12_upload_surface_data(d, surface, true, errp);
}

static bool surface_overlaps_range(
    PGRAPHD3D12SurfaceBinding const *surface, hwaddr range_start,
    hwaddr range_len)
{
    uint64_t row_bytes;
    uint64_t surface_span;
    uint64_t surface_end;
    uint64_t range_end;
    uint64_t clipped_start;
    uint64_t offset;
    uint64_t row;
    uint64_t in_row;

    if (surface == NULL || range_len == 0 || surface->height == 0) {
        return false;
    }

    range_end = range_start > UINT64_MAX - range_len ?
                    UINT64_MAX : range_start + range_len;

    if (surface->swizzle) {
        surface_end = surface->vram_addr > UINT64_MAX - surface->size ?
                          UINT64_MAX : surface->vram_addr + surface->size;
        return surface->vram_addr < range_end && range_start < surface_end;
    }

    row_bytes = (uint64_t)surface->width * surface->fmt.bytes_per_pixel;
    if (row_bytes == 0 || surface->pitch == 0) {
        return false;
    }

    /* Bytes between row_bytes and pitch are padding; test the visible row
     * intervals so an unrelated access does not force a full download. */
    if (surface->height - 1 >
        (UINT64_MAX - row_bytes) / surface->pitch) {
        surface_span = UINT64_MAX;
    } else {
        surface_span = (uint64_t)(surface->height - 1) * surface->pitch +
                       row_bytes;
    }
    surface_end = surface->vram_addr > UINT64_MAX - surface_span ?
                      UINT64_MAX : surface->vram_addr + surface_span;
    if (surface->vram_addr >= range_end || range_start >= surface_end) {
        return false;
    }

    if (row_bytes >= surface->pitch) {
        return true;
    }

    clipped_start = MAX((uint64_t)range_start,
                        (uint64_t)surface->vram_addr);
    offset = clipped_start - surface->vram_addr;
    row = offset / surface->pitch;
    in_row = offset % surface->pitch;
    if (row < surface->height && in_row < row_bytes) {
        return true;
    }

    row++;
    return row < surface->height &&
           surface->vram_addr + row * surface->pitch < range_end;
}

static void record_cpu_access_range(PGRAPHD3D12State *r, hwaddr addr,
                                    hwaddr len, bool write)
{
    for (unsigned int i = 0; i < r->cpu_access_range_count; i++) {
        hwaddr start = r->cpu_access_ranges[i].addr;
        hwaddr end = start + r->cpu_access_ranges[i].len;

        if (addr >= start && addr + len <= end) {
            return;
        }
    }
    if (r->cpu_access_range_count == ARRAY_SIZE(r->cpu_access_ranges)) {
        r->cpu_access_range_overflow = true;
        return;
    }
    r->cpu_access_ranges[r->cpu_access_range_count].addr = addr;
    r->cpu_access_ranges[r->cpu_access_range_count].len = len;
    r->cpu_access_ranges[r->cpu_access_range_count].write = write;
    r->cpu_access_range_count++;
}

/*
 * vCPU access to bytes a live surface or a stash owns on the GPU. Every
 * watchpoint uses this callback with the device as its opaque pointer and
 * images are found by address under pg->lock, because PFIFO may free any
 * binding while a vCPU sits in here. PFIFO performs the write-backs; the
 * vCPU only records what it touched and waits.
 */
static void surface_access_callback(void *opaque, MemoryRegion *mr, hwaddr addr,
                                    hwaddr len, bool write)
{
    NV2AState *d = opaque;
    PGRAPHD3D12State *r;
    PGRAPHD3D12SurfaceBinding *surface;
    bool wait = false;

    (void)mr;
    qemu_mutex_lock(&d->pgraph.lock);
    r = d->pgraph.d3d12_renderer_state;
    if (r == NULL) {
        qemu_mutex_unlock(&d->pgraph.lock);
        return;
    }

    r->surface_callback_calls++;
    QTAILQ_FOREACH(surface, &r->surfaces, entry) {
        if (!surface_watch_overlaps(surface, addr, len)) {
            continue;
        }
        if (surface_overlaps_range(surface, addr, len)) {
            if (surface->draw_dirty) {
                surface->download_pending = true;
                wait = true;
            }
            if (write) {
                surface->upload_pending = true;
                surface->cleared = false;
                surface->range_download_valid = false;
                surface->write_frame_time = d->pgraph.frame_time;
            }
        }
        /* Clean surfaces return to the TLB fast path; PFIFO disarms the
         * ones it downloads for this access. */
        if (!surface->draw_dirty) {
            mem_access_callback_set_enabled(qemu_get_cpu(0),
                                            surface->access_cb, false);
        }
    }
    QTAILQ_FOREACH(surface, &r->stashed_surfaces, entry) {
        if (surface_watch_overlaps(surface, addr, len)) {
            wait = true;
            break;
        }
    }
    if (wait) {
        record_cpu_access_range(r, addr, len, write);
        r->surface_callback_waits++;
    }
    qemu_mutex_unlock(&d->pgraph.lock);

    if (wait) {
        qemu_mutex_lock(&d->pfifo.lock);
        qemu_event_reset(&r->downloads_complete);
        qatomic_set(&r->downloads_pending, true);
        pfifo_kick(d);
        qemu_mutex_unlock(&d->pfifo.lock);
        qemu_event_wait(&r->downloads_complete);
    }
}

static void register_cpu_access_callback(
    NV2AState *d, PGRAPHD3D12SurfaceBinding *surface)
{
    surface->access_cb = NULL;
    if (tcg_enabled() && surface->width && surface->height) {
        /* A surface that already holds GPU-only bytes (adopted from a
         * stash, or re-registered while dirty) is watched from the start. */
        surface->access_cb = surface->draw_dirty ?
            mem_access_callback_insert(qemu_get_cpu(0), d->vram,
                                       surface->vram_addr, surface->size,
                                       &surface_access_callback, d) :
            mem_access_callback_insert_disabled(
                qemu_get_cpu(0), d->vram, surface->vram_addr, surface->size,
                &surface_access_callback, d);
        d->pgraph.d3d12_renderer_state->surface_callback_registers++;
    }
}

static void unregister_cpu_access_callback(
    NV2AState *d, PGRAPHD3D12SurfaceBinding *surface)
{
    if (tcg_enabled() && surface->access_cb != NULL) {
        mem_access_callback_remove_by_ref(qemu_get_cpu(0), surface->access_cb);
        surface->access_cb = NULL;
        d->pgraph.d3d12_renderer_state->surface_callback_unregisters++;
    }
}

/*
 * Vertex-hull overlap downloads cost a GPU round trip each, and the hull of
 * an indexed draw can cover megabytes the indices never touch; skip them on
 * UWP.  TODO: an index-aware policy to serve render-target-as-vertex reads.
 */
#ifdef XEMU_UWP
#define PGRAPH_D3D12_SKIP_VERTEX_OVERLAP_DOWNLOADS 1
#endif

bool pgraph_d3d12_download_surfaces_in_range_if_dirty(
    PGRAPHState *pg, hwaddr start, hwaddr size, Error **errp)
{
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    PGRAPHD3D12SurfaceBinding *surface;
    static unsigned int diagnostic_reports;

    if (r == NULL || size == 0) {
        return true;
    }
    pgraph_d3d12_complete_readbacks_in_range(
        d, start, size, PGRAPH_D3D12_STASH_TRIGGER_RANGE);
#ifdef PGRAPH_D3D12_SKIP_VERTEX_OVERLAP_DOWNLOADS
    (void)surface;
    (void)diagnostic_reports;
    return true;
#endif

    /* Render targets can be reused as vertex data; refresh guest VRAM from
     * the GPU-owned surface first or stale bytes become positions. */
    QTAILQ_FOREACH(surface, &r->surfaces, entry) {
        if (!surface->draw_dirty ||
            !surface_overlaps_range(surface, start, size)) {
            continue;
        }

        /* Linear surfaces only need the overlapped rows written back. */
        if (!surface->swizzle && surface->pitch != 0 &&
            surface->width != 0 && surface->height != 0 &&
            surface->resource != NULL && !surface->upload_pending) {
            hwaddr clip_start = MAX(start, surface->vram_addr);
            hwaddr clip_end = MIN(start + size,
                                  surface->vram_addr + surface->size);
            uint32_t row_start =
                (clip_start - surface->vram_addr) / surface->pitch;
            uint32_t row_end =
                MIN((uint32_t)((clip_end - 1 - surface->vram_addr) /
                               surface->pitch) + 1,
                    surface->height);

            if (row_start >= row_end) {
                continue;
            }
            if (surface->range_download_valid &&
                surface->range_download_draw_time == surface->draw_time &&
                row_start >= surface->range_download_row_start &&
                row_end <= surface->range_download_row_end) {
                continue;
            }
            if (download_surface_rows(
                    d, surface, PGRAPH_D3D12_SURFACE_DOWNLOAD_DISPLAY_CPU,
                    row_start, row_end, errp)) {
                continue;
            }
            /* Fall through to the checked whole-surface path. */
            if (errp != NULL) {
                error_free(*errp);
                *errp = NULL;
            }
        }

        if (!pgraph_d3d12_surface_download_if_dirty_checked_reason(
                d, surface, PGRAPH_D3D12_SURFACE_DOWNLOAD_DISPLAY_CPU,
                errp)) {
            error_prepend(
                errp,
                "Direct3D 12 could not synchronize dirty %s surface "
                "addr=0x%" HWADDR_PRIx " with vertex range "
                "addr=0x%" HWADDR_PRIx " size=0x%" HWADDR_PRIx ": ",
                surface->color ? "color" : "zeta", surface->vram_addr,
                start, size);
            return false;
        }

        if (diagnostic_reports < 16) {
            fprintf(stderr,
                    "Direct3D 12 renderer: synchronized dirty %s surface "
                    "addr=0x%" HWADDR_PRIx " before vertex range "
                    "addr=0x%" HWADDR_PRIx " size=0x%" HWADDR_PRIx "\n",
                    surface->color ? "color" : "zeta", surface->vram_addr,
                    start, size);
            diagnostic_reports++;
        }
    }

    return true;
}

static bool surfaces_overlap(PGRAPHD3D12SurfaceBinding const *a,
                             PGRAPHD3D12SurfaceBinding const *b)
{
    const PGRAPHD3D12SurfaceBinding *rows = a;
    const PGRAPHD3D12SurfaceBinding *other = b;
    uint64_t row_bytes;

    if (a->swizzle) {
        return surface_overlaps_range(b, a->vram_addr, a->size);
    }
    if (b->swizzle) {
        return surface_overlaps_range(a, b->vram_addr, b->size);
    }
    if (b->height < a->height) {
        rows = b;
        other = a;
    }

    row_bytes = (uint64_t)rows->width * rows->fmt.bytes_per_pixel;
    for (unsigned int y = 0; y < rows->height; y++) {
        if (surface_overlaps_range(
                other, rows->vram_addr + (uint64_t)y * rows->pitch,
                row_bytes)) {
            return true;
        }
    }
    return false;
}

static void unbind_surface(PGRAPHState *pg, bool color)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;

    if (color) {
        r->color_binding = NULL;
    } else {
        r->zeta_binding = NULL;
    }
}

static void invalidate_surface(NV2AState *d,
                               PGRAPHD3D12SurfaceBinding *surface)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;

    if (surface == r->color_binding) {
        unbind_surface(pg, true);
    }
    if (surface == r->zeta_binding) {
        unbind_surface(pg, false);
    }
    if (surface == r->display_surface_binding) {
        r->display_surface_binding = NULL;
        r->display_surface_valid = false;
    }

    unregister_cpu_access_callback(d, surface);

    /* The image goes to the pool; in-flight GPU work may still reference
     * the RTV/DSV slot, so that is released behind the fence. */
    pool_surface_resource(pg, surface);
    pgraph_d3d12_defer_release(
        pg, NULL, -1,
        surface->descriptor_allocated && surface->color ?
            (int32_t)surface->descriptor_index : -1,
        surface->descriptor_allocated && !surface->color ?
            (int32_t)surface->descriptor_index : -1,
        -1);
    surface->descriptor_allocated = false;
    QTAILQ_REMOVE(&r->surfaces, surface, entry);
    if (r->display_snapshot_source == (const void *)surface) {
        r->display_snapshot_source = NULL;
        r->display_snapshot_valid = false;
    }
    g_free(surface);
}

PGRAPHD3D12SurfaceBinding *pgraph_d3d12_surface_get(NV2AState *d,
                                                     hwaddr addr)
{
    PGRAPHD3D12State *r = d->pgraph.d3d12_renderer_state;
    PGRAPHD3D12SurfaceBinding *surface;

    QTAILQ_FOREACH(surface, &r->surfaces, entry) {
        if (surface->vram_addr == addr) {
            return surface;
        }
    }

    return NULL;
}

static PGRAPHD3D12SurfaceBinding *surface_get_color_within(NV2AState *d,
                                                           hwaddr addr)
{
    PGRAPHD3D12State *r = d->pgraph.d3d12_renderer_state;
    PGRAPHD3D12SurfaceBinding *surface;

    QTAILQ_FOREACH(surface, &r->surfaces, entry) {
        if (surface->color && surface_overlaps_range(surface, addr, 1)) {
            return surface;
        }
    }

    return NULL;
}

static bool display_addr_in_range(hwaddr addr, hwaddr start, uint64_t size)
{
    return size > 0 && addr >= start && (uint64_t)(addr - start) < size;
}

/* Newest blit whose destination starts at the scan-out start and spans at
 * least min_height lines. */
static const PGRAPHD3D12BlitDisplayCandidate *find_blit_display_surface(
    PGRAPHD3D12State *r, hwaddr pcrtc_start, uint32_t min_height)
{
    const PGRAPHD3D12BlitDisplayCandidate *best = NULL;

    for (uint32_t i = 0; i < PGRAPH_D3D12_BLIT_DISPLAY_CANDIDATES; i++) {
        const PGRAPHD3D12BlitDisplayCandidate *candidate =
            &r->blit_display_candidates[i];

        if (!candidate->valid || candidate->addr != pcrtc_start ||
            candidate->height < min_height) {
            continue;
        }

        if (best == NULL || candidate->sequence > best->sequence) {
            best = candidate;
        }
    }

    return best;
}

static void append_recent_blit_candidates(FILE *stream, PGRAPHD3D12State *r,
                                          hwaddr display_addr)
{
    bool any = false;

    for (uint32_t i = 0; i < PGRAPH_D3D12_BLIT_DISPLAY_CANDIDATES; i++) {
        const PGRAPHD3D12BlitDisplayCandidate *candidate =
            &r->blit_display_candidates[i];

        if (!candidate->valid) {
            continue;
        }

        any = true;
        fprintf(stream,
                " [seq=%u addr=0x%" HWADDR_PRIx "..0x%" HWADDR_PRIx
                " size=0x%" PRIx64 " %ux%u pitch=%u bpp=%u format=%s "
                "(0x%x) op=0x%x draw_time=%d covers_display=%d]",
                candidate->sequence, candidate->addr,
                candidate->addr + candidate->size, candidate->size,
                candidate->width, candidate->height, candidate->pitch,
                candidate->bytes_per_pixel,
                color_surface_format_name(candidate->color_format),
                candidate->color_format, candidate->operation,
                candidate->draw_time,
                display_addr_in_range(display_addr, candidate->addr,
                                      candidate->size));
    }

    if (!any) {
        fprintf(stream, " none");
    }
}

static void report_missing_display_surface(NV2AState *d, hwaddr display_addr,
                                           hwaddr pcrtc_start,
                                           uint32_t line_offset)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    PGRAPHD3D12SurfaceBinding *surface;

    if (!pgraph_d3d12_verbose_reports(pg)) {
        return;
    }
    if (r->reported_missing_display_surface &&
        r->reported_missing_display_surface_addr == display_addr) {
        return;
    }

    r->reported_missing_display_surface = true;
    r->reported_missing_display_surface_addr = display_addr;

    fprintf(stderr,
            "Direct3D 12 renderer: display fallback found no color surface "
            "covering PCRTC address 0x%" HWADDR_PRIx " (pcrtc.start=0x%"
            HWADDR_PRIx ", vga.line_offset=%u). Active color binding: ",
            display_addr, pcrtc_start, line_offset);

    if (r->color_binding != NULL) {
        hwaddr color_end = r->color_binding->vram_addr +
                           r->color_binding->size;
        fprintf(stderr,
                "addr=0x%" HWADDR_PRIx "..0x%" HWADDR_PRIx
                " %ux%u pitch=%u bytes_per_pixel=%u draw_dirty=%d "
                "download_pending=%d",
                r->color_binding->vram_addr, color_end,
                r->color_binding->width, r->color_binding->height,
                r->color_binding->pitch,
                r->color_binding->fmt.bytes_per_pixel,
                r->color_binding->draw_dirty,
                r->color_binding->download_pending);
    } else {
        fprintf(stderr, "none");
    }

    fprintf(stderr, ". Cached color surfaces:");
    QTAILQ_FOREACH(surface, &r->surfaces, entry) {
        if (!surface->color) {
            continue;
        }

        fprintf(stderr,
                " [addr=0x%" HWADDR_PRIx "..0x%" HWADDR_PRIx
                " %ux%u pitch=%u format=0x%x draw_dirty=%d]",
                surface->vram_addr, surface->vram_addr + surface->size,
                surface->width, surface->height, surface->pitch,
                surface->shape.color_format, surface->draw_dirty);
    }
    fprintf(stderr, ". Recent CPU blit display candidates:");
    append_recent_blit_candidates(stderr, r, display_addr);
    fputc('\n', stderr);
}

static void set_display_tracked_surface_state(
    PGRAPHD3D12State *r, PGRAPHD3D12SurfaceBinding *surface, hwaddr addr,
    uint32_t width, uint32_t height, uint32_t pitch)
{
    uint32_t x_offset = 0;
    uint32_t y_offset = 0;

    if (pitch == 0) {
        pitch = surface->pitch;
    }
    if (addr >= surface->vram_addr && surface->fmt.bytes_per_pixel != 0 &&
        surface->pitch != 0) {
        hwaddr byte_offset = addr - surface->vram_addr;
        y_offset = byte_offset / surface->pitch;
        x_offset = (byte_offset % surface->pitch) /
                   surface->fmt.bytes_per_pixel;
    }

    r->display_surface_valid = true;
    r->display_surface_binding = surface;
    r->display_surface_addr = addr;
    r->display_surface_width = width;
    r->display_surface_height = height;
    r->display_surface_pitch = pitch;
    r->display_surface_x_offset = x_offset;
    r->display_surface_y_offset = y_offset;
    r->display_surface_bytes_per_pixel = surface->fmt.bytes_per_pixel;
    r->display_surface_color_format = surface->shape.color_format;
}

static void set_display_surface_state(PGRAPHD3D12State *r,
                                      PGRAPHD3D12SurfaceBinding *surface,
                                      hwaddr addr, uint32_t pitch)
{
    set_display_tracked_surface_state(r, surface, addr, surface->width,
                                      surface->height, pitch);
}

static void set_display_raw_surface_state(PGRAPHD3D12State *r, hwaddr addr,
                                          uint32_t width, uint32_t height,
                                          uint32_t pitch,
                                          uint32_t bytes_per_pixel,
                                          uint32_t color_format)
{
    r->display_surface_valid = true;
    r->display_surface_binding = NULL;
    r->display_surface_addr = addr;
    r->display_surface_width = width;
    r->display_surface_height = height;
    r->display_surface_pitch = pitch;
    r->display_surface_x_offset = 0;
    r->display_surface_y_offset = 0;
    r->display_surface_bytes_per_pixel = bytes_per_pixel;
    r->display_surface_color_format = color_format;
}

static void report_blit_display_surface(
    NV2AState *d, hwaddr display_addr, hwaddr pcrtc_start,
    uint32_t line_offset,
    const PGRAPHD3D12BlitDisplayCandidate *candidate)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;

    if (!pgraph_d3d12_verbose_reports(pg)) {
        return;
    }
    if (r->reported_blit_display_surface &&
        r->reported_blit_display_surface_addr == candidate->addr) {
        return;
    }

    r->reported_blit_display_surface = true;
    r->reported_blit_display_surface_addr = candidate->addr;

    fprintf(stderr,
            "Direct3D 12 renderer: display using CPU blit destination "
            "format because the PCRTC mode has no scan-out format: "
            "display address 0x%" HWADDR_PRIx " (pcrtc.start=0x%"
            HWADDR_PRIx ", vga.line_offset=%u), blit range "
            "0x%" HWADDR_PRIx "..0x%" HWADDR_PRIx
            "; blit_surface=%ux%u pitch=%u bpp=%u format=%s (0x%x) "
            "operation=0x%x sequence=%u draw_time=%d.\n",
            display_addr, pcrtc_start, line_offset, candidate->addr,
            candidate->addr + candidate->size, candidate->width,
            candidate->height, candidate->pitch,
            candidate->bytes_per_pixel,
            color_surface_format_name(candidate->color_format),
            candidate->color_format, candidate->operation,
            candidate->sequence, candidate->draw_time);
}

static bool raw_pcrtc_display_format(NV2AState *d,
                                     uint32_t *bytes_per_pixel,
                                     uint32_t *color_format, int *vga_bpp)
{
    *vga_bpp = d->vga.get_bpp(&d->vga);

    switch (*vga_bpp) {
    case 15:
        *bytes_per_pixel = 2;
        *color_format =
            NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_Z1R5G5B5;
        return true;
    case 16:
        *bytes_per_pixel = 2;
        *color_format = NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5;
        return true;
    case 32:
        *bytes_per_pixel = 4;
        *color_format = NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8;
        return true;
    default:
        return false;
    }
}

static void report_raw_pcrtc_display_surface(
    NV2AState *d, hwaddr display_addr, hwaddr pcrtc_start,
    uint32_t line_offset, uint32_t width, uint32_t height,
    uint32_t bytes_per_pixel, uint32_t color_format, int vga_bpp)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    uint64_t source_size = (uint64_t)line_offset * height;

    if (!pgraph_d3d12_verbose_reports(pg)) {
        return;
    }
    if (r->reported_raw_pcrtc_display_surface &&
        r->reported_raw_pcrtc_display_surface_addr == pcrtc_start &&
        r->reported_raw_pcrtc_display_surface_format == color_format) {
        return;
    }

    r->reported_raw_pcrtc_display_surface = true;
    r->reported_raw_pcrtc_display_surface_addr = pcrtc_start;
    r->reported_raw_pcrtc_display_surface_format = color_format;

    fprintf(stderr,
            "Direct3D 12 renderer: display using authoritative raw PCRTC "
            "VRAM at display address 0x%" HWADDR_PRIx
            " (pcrtc.start=0x%" HWADDR_PRIx ", vga.line_offset=%u). "
            "raw_source=0x%" HWADDR_PRIx "..0x%" HWADDR_PRIx
            " %ux%u pitch=%u bpp=%u vga_bpp=%d format=%s (0x%x). "
            "Recent CPU blit display candidates:",
            display_addr, pcrtc_start, line_offset, pcrtc_start,
            pcrtc_start + source_size, width, height, line_offset,
            bytes_per_pixel, vga_bpp, color_surface_format_name(color_format),
            color_format);
    append_recent_blit_candidates(stderr, r, display_addr);
    fputc('\n', stderr);
}

static bool set_raw_pcrtc_display_surface(NV2AState *d, hwaddr display_addr,
                                          hwaddr pcrtc_start,
                                          uint32_t line_offset)
{
    PGRAPHD3D12State *r = d->pgraph.d3d12_renderer_state;
    uint32_t bytes_per_pixel;
    uint32_t color_format;
    int vga_bpp;
    int display_width = 0;
    int display_height = 0;

    if (!line_offset || pcrtc_start >= memory_region_size(d->vram) ||
        !raw_pcrtc_display_format(d, &bytes_per_pixel, &color_format,
                                  &vga_bpp)) {
        return false;
    }

    d->vga.get_resolution(&d->vga, &display_width, &display_height);
    if (display_width <= 0 || display_height <= 0) {
        return false;
    }

    uint32_t visible_width = line_offset / bytes_per_pixel;
    if (visible_width == 0) {
        return false;
    }
    if ((uint32_t)display_width > visible_width) {
        display_width = visible_width;
    }

    uint64_t source_size = (uint64_t)line_offset * (uint32_t)display_height;
    if (source_size > memory_region_size(d->vram) - pcrtc_start) {
        return false;
    }

    report_raw_pcrtc_display_surface(
        d, display_addr, pcrtc_start, line_offset, (uint32_t)display_width,
        (uint32_t)display_height, bytes_per_pixel, color_format, vga_bpp);
    set_display_raw_surface_state(r, pcrtc_start, (uint32_t)display_width,
                                  (uint32_t)display_height, line_offset,
                                  bytes_per_pixel, color_format);
    return true;
}

/*
 * For a CRTC mode without a scan-out pixel format, a 2D blit that wrote the
 * whole raster from the scan-out start supplies format and pitch. Only the
 * raster is shown, one guest line per display line.
 */
static bool set_blit_display_surface(NV2AState *d, hwaddr display_addr,
                                     hwaddr pcrtc_start, uint32_t line_offset)
{
    PGRAPHD3D12State *r = d->pgraph.d3d12_renderer_state;
    const PGRAPHD3D12BlitDisplayCandidate *candidate;
    int display_width = 0;
    int display_height = 0;

    d->vga.get_resolution(&d->vga, &display_width, &display_height);
    if (display_width <= 0 || display_height <= 0) {
        return false;
    }

    candidate = find_blit_display_surface(r, pcrtc_start,
                                          (uint32_t)display_height);
    if (candidate == NULL) {
        return false;
    }

    report_blit_display_surface(d, display_addr, pcrtc_start, line_offset,
                                candidate);
    set_display_raw_surface_state(
        r, candidate->addr, MIN(candidate->width, (uint32_t)display_width),
        (uint32_t)display_height, candidate->pitch,
        candidate->bytes_per_pixel, candidate->color_format);
    return true;
}

static bool set_pcrtc_scanout_display_surface(
    NV2AState *d, hwaddr display_addr, hwaddr pcrtc_start,
    uint32_t line_offset, PGRAPHD3D12SurfaceBinding *surface)
{
    PGRAPHD3D12State *r = d->pgraph.d3d12_renderer_state;
    uint32_t bytes_per_pixel;
    uint32_t visible_width;
    uint32_t byte_offset;
    uint32_t row_offset;
    uint32_t x_offset;
    uint64_t source_size;
    int display_width = 0;
    int display_height = 0;

    if (surface == NULL || !line_offset || !surface->fmt.bytes_per_pixel ||
        pcrtc_start < surface->vram_addr ||
        pcrtc_start >= surface->vram_addr + surface->size) {
        return false;
    }

    bytes_per_pixel = surface->fmt.bytes_per_pixel;
    if (line_offset % bytes_per_pixel != 0) {
        return false;
    }

    d->vga.get_resolution(&d->vga, &display_width, &display_height);
    if (display_width <= 0 || display_height <= 0) {
        return false;
    }

    visible_width = line_offset / bytes_per_pixel;
    if (visible_width == 0) {
        return false;
    }
    if ((uint32_t)display_width > visible_width) {
        display_width = visible_width;
    }

    source_size = (uint64_t)line_offset * (uint32_t)display_height;
    if (pcrtc_start >= memory_region_size(d->vram) ||
        source_size > memory_region_size(d->vram) - pcrtc_start) {
        return false;
    }

    byte_offset = pcrtc_start - surface->vram_addr;
    row_offset = surface->pitch ? byte_offset / surface->pitch : 0;
    x_offset = surface->pitch && bytes_per_pixel ?
               (byte_offset % surface->pitch) / bytes_per_pixel : 0;

    if (pgraph_d3d12_verbose_reports(&d->pgraph) &&
        (!r->reported_pcrtc_scanout_surface ||
         r->reported_pcrtc_scanout_surface_addr != surface->vram_addr ||
         r->reported_pcrtc_scanout_addr != pcrtc_start ||
         r->reported_pcrtc_scanout_line_offset != line_offset)) {
        r->reported_pcrtc_scanout_surface = true;
        r->reported_pcrtc_scanout_surface_addr = surface->vram_addr;
        r->reported_pcrtc_scanout_addr = pcrtc_start;
        r->reported_pcrtc_scanout_line_offset = line_offset;

        fprintf(stderr,
                "Direct3D 12 renderer: display using PCRTC scanout window "
                "inside tracked color surface: display_addr=0x%" HWADDR_PRIx
                ", pcrtc.start=0x%" HWADDR_PRIx
                ", surface=0x%" HWADDR_PRIx "..0x%" HWADDR_PRIx
                " %ux%u surface_pitch=%u, scanout=0x%" HWADDR_PRIx
                "..0x%" HWADDR_PRIx " %ux%u line_offset=%u, "
                "bytes_per_pixel=%u, byte_offset=0x%x, row_offset=%u, "
                "x_offset=%u, format=%s (0x%x), draw_dirty=%d, "
                "download_pending=%d, initialized=%d.\n",
                display_addr, pcrtc_start, surface->vram_addr,
                surface->vram_addr + surface->size, surface->width,
                surface->height, surface->pitch, pcrtc_start,
                pcrtc_start + source_size, (uint32_t)display_width,
                (uint32_t)display_height, line_offset, bytes_per_pixel,
                byte_offset, row_offset, x_offset,
                color_surface_format_name(surface->shape.color_format),
                surface->shape.color_format, surface->draw_dirty,
                surface->download_pending, surface->initialized);
    }

    set_display_tracked_surface_state(r, surface, pcrtc_start,
                                      (uint32_t)display_width,
                                      (uint32_t)display_height, line_offset);
    return true;
}

static bool surface_guest_writes_cover(
    const PGRAPHD3D12SurfaceBinding *writer,
    const PGRAPHD3D12SurfaceBinding *old_surface);

/*
 * A square swizzled colour target whose bytes sit block-aligned inside a
 * larger swizzled target of the same format occupies one rectangle of the
 * larger image with identical pixel order, so its pixels can be carried
 * over by a GPU copy instead of a write-back and re-upload. Returns the
 * rectangle origin in target pixels. The target's resource may not exist
 * yet when the decision is made on the same-address rebind path.
 */
static bool alias_copy_rect(PGRAPHD3D12SurfaceBinding const *old_surface,
                            PGRAPHD3D12SurfaceBinding const *target,
                            unsigned int *x, unsigned int *y)
{
    unsigned int bpp = old_surface->fmt.bytes_per_pixel;

    if (old_surface->nv2a != NULL &&
        (old_surface->nv2a->pgraph.d3d12_renderer_state->tune_flags &
         PGRAPH_D3D12_TUNE_ALIAS_OFF)) {
        return false;
    }
    if (!old_surface->color || !target->color || !old_surface->swizzle ||
        !target->swizzle ||
        old_surface->fmt.dxgi_format != target->fmt.dxgi_format ||
        bpp == 0 || bpp != target->fmt.bytes_per_pixel ||
        old_surface->shape.anti_aliasing != target->shape.anti_aliasing ||
        old_surface->resource == NULL ||
        !old_surface->draw_dirty || old_surface->upload_pending ||
        old_surface->width != old_surface->height ||
        old_surface->vram_addr < target->vram_addr ||
        (uint64_t)target->width * target->height * bpp > target->size) {
        return false;
    }
    return pgraph_d3d12_swizzled_block_origin(
        target->width, target->height, bpp, old_surface->width,
        old_surface->vram_addr - target->vram_addr, x, y);
}

/* Frees a surface that was already removed from the surface list. */
static void release_detached_surface(NV2AState *d,
                                     PGRAPHD3D12SurfaceBinding *surface)
{
    PGRAPHD3D12State *r = d->pgraph.d3d12_renderer_state;

    unregister_cpu_access_callback(d, surface);
    pool_surface_resource(&d->pgraph, surface);
    pgraph_d3d12_defer_release(
        &d->pgraph, NULL, -1,
        surface->descriptor_allocated && surface->color ?
            (int32_t)surface->descriptor_index : -1,
        surface->descriptor_allocated && !surface->color ?
            (int32_t)surface->descriptor_index : -1,
        -1);
    surface->descriptor_allocated = false;
    if (r->display_snapshot_source == (const void *)surface) {
        r->display_snapshot_source = NULL;
        r->display_snapshot_valid = false;
    }
    g_free(surface);
}

static void apply_alias_copies(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;

    for (unsigned int i = 0; i < r->alias_copy_count; i++) {
        PGRAPHD3D12SurfaceBinding *source = r->alias_copies[i].source;
        PGRAPHD3D12SurfaceBinding *target = r->alias_copies[i].target;
        Error *local_err = NULL;
        D3D12_RESOURCE_STATES target_state;
        D3D12_TEXTURE_COPY_LOCATION dst;
        D3D12_TEXTURE_COPY_LOCATION src;
        unsigned int x = r->alias_copies[i].x;
        unsigned int y = r->alias_copies[i].y;

        /* The target's guest bytes must land first; the copy then supplies
         * the region whose bytes never reached guest memory. */
        if (target->upload_pending &&
            !pgraph_d3d12_upload_surface_data(d, target, false,
                                              &local_err)) {
            report_surface_error("alias copy target upload", target,
                                 local_err);
            local_err = NULL;
        }
        if (target->resource == NULL || source->resource == NULL ||
            !pgraph_d3d12_begin_commands(pg, &local_err)) {
            report_surface_error("alias copy", source, local_err);
            release_detached_surface(d, source);
            continue;
        }

        pgraph_apply_scaling_factor(pg, &x, &y);
        target_state = target->state;
        pgraph_d3d12_transition_raw_resource(r, source->resource,
                                             &source->state,
                                             D3D12_RESOURCE_STATE_COPY_SOURCE);
        pgraph_d3d12_transition_raw_resource(r, target->resource,
                                             &target->state,
                                             D3D12_RESOURCE_STATE_COPY_DEST);
        memset(&dst, 0, sizeof(dst));
        dst.pResource = target->resource;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        memset(&src, 0, sizeof(src));
        src.pResource = source->resource;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        r->command_list->lpVtbl->CopyTextureRegion(r->command_list, &dst,
                                                   x, y, 0, &src, NULL);
        pgraph_d3d12_transition_raw_resource(r, target->resource,
                                             &target->state, target_state);

        target->draw_dirty = true;
        target->cleared = false;
        if (tcg_enabled() && target->access_cb != NULL) {
            mem_access_callback_set_enabled(qemu_get_cpu(0),
                                            target->access_cb, true);
        }
        r->perf.surface_alias_copies++;
        release_detached_surface(d, source);
    }
    r->alias_copy_count = 0;
}

static bool invalidate_overlapping_surfaces(
    NV2AState *d, PGRAPHD3D12SurfaceBinding *target,
    const PGRAPHD3D12SurfaceBinding *discard_surface,
    bool discard_covered_overlaps, Error **errp)
{
    PGRAPHD3D12State *r = d->pgraph.d3d12_renderer_state;
    PGRAPHD3D12SurfaceBinding *surface, *next;
    unsigned int alias_slots;
    unsigned int alias_planned = 0;
    PGRAPHD3D12SurfaceBinding *pending[SURFACE_READBACK_BATCH_MAX];
    unsigned int pending_count = 0;

    /* A queued alias copy whose target this surface replaces would copy
     * into a freed image. Its source goes back to the live list and is
     * preserved like any other overlap. */
    for (unsigned int i = 0; i < r->alias_copy_count;) {
        if (surfaces_overlap(r->alias_copies[i].target, target)) {
            QTAILQ_INSERT_TAIL(&r->surfaces, r->alias_copies[i].source,
                               entry);
            r->surface_layout_gen++;
            r->alias_copies[i] = r->alias_copies[--r->alias_copy_count];
        } else {
            i++;
        }
    }
    alias_slots = ARRAY_SIZE(r->alias_copies) - r->alias_copy_count;

    /* Preserve every overlap except the fully overwritten discard candidate
     * before invalidating any, so the old set stays intact if a readback
     * fails. */
    QTAILQ_FOREACH(surface, &r->surfaces, entry) {
        bool overwritten = discard_covered_overlaps &&
            surface_guest_writes_cover(target, surface);
        unsigned int alias_x, alias_y;

        if (surfaces_overlap(surface, target)) {
            pgraph_d3d12_evlog(
                r, "ovl %c 0x%" HWADDR_PRIx " %ux%u swz=%d dd=%d up=%d clr=%d "
                "disc=%d over=%d -> %c %ux%u swz=%d",
                surface->color ? 'c' : 'z', surface->vram_addr,
                surface->width, surface->height, surface->swizzle,
                surface->draw_dirty, surface->upload_pending,
                surface->cleared, surface == discard_surface, overwritten,
                target->color ? 'c' : 'z', target->width, target->height,
                target->swizzle);
        }
        if (surface != discard_surface && !overwritten &&
            surfaces_overlap(surface, target)) {
            if (alias_planned < alias_slots &&
                alias_copy_rect(surface, target, &alias_x, &alias_y)) {
                alias_planned++;
                continue;
            }
            if (!surface->draw_dirty) {
                continue;
            }
            /* Guests sample these bytes, so skipping the write-back here
             * only moves it to the texture-overlap path. */
            if (pending_count < ARRAY_SIZE(pending)) {
                pending[pending_count++] = surface;
                continue;
            }
            if (!download_surface_data(
                    d, surface,
                    PGRAPH_D3D12_SURFACE_DOWNLOAD_OVERLAP_INVALIDATION, true,
                    errp)) {
                error_prepend(
                    errp,
                    "Direct3D 12 could not preserve overlapping %s "
                    "surface addr=0x%" HWADDR_PRIx
                    " before creating %s surface addr=0x%" HWADDR_PRIx ": ",
                    surface->color ? "color" : "zeta", surface->vram_addr,
                    target->color ? "color" : "zeta", target->vram_addr);
                return false;
            }
        }
    }

    if (pending_count != 0 &&
        !pgraph_d3d12_record_surface_readbacks(
            d, pending, pending_count,
            PGRAPH_D3D12_SURFACE_DOWNLOAD_OVERLAP_INVALIDATION, errp)) {
        error_prepend(errp,
                      "Direct3D 12 could not preserve the surfaces "
                      "overlapping new %s surface addr=0x%" HWADDR_PRIx ": ",
                      target->color ? "color" : "zeta", target->vram_addr);
        return false;
    }

    QTAILQ_FOREACH_SAFE(surface, &r->surfaces, entry, next) {
        unsigned int alias_x, alias_y;

        if (!surfaces_overlap(surface, target)) {
            continue;
        }
        if (surface != discard_surface &&
            !(discard_covered_overlaps &&
              surface_guest_writes_cover(target, surface)) &&
            r->alias_copy_count < ARRAY_SIZE(r->alias_copies) &&
            alias_copy_rect(surface, target, &alias_x, &alias_y)) {
            /* Detach now; the pixels move once the target has uploaded. */
            if (surface == r->display_surface_binding) {
                r->display_surface_binding = NULL;
                r->display_surface_valid = false;
            }
            QTAILQ_REMOVE(&r->surfaces, surface, entry);
            r->alias_copies[r->alias_copy_count].source = surface;
            r->alias_copies[r->alias_copy_count].target = target;
            r->alias_copies[r->alias_copy_count].x = alias_x;
            r->alias_copies[r->alias_copy_count].y = alias_y;
            r->alias_copy_count++;
            continue;
        }
        invalidate_surface(d, surface);
    }

    return true;
}

static bool check_surface_compatibility(
    PGRAPHD3D12SurfaceBinding const *a,
    PGRAPHD3D12SurfaceBinding const *b, bool strict)
{
    bool format_compatible = a->color == b->color &&
                             a->fmt.dxgi_format == b->fmt.dxgi_format &&
                             (a->swizzle || a->pitch == b->pitch) &&
                             a->swizzle == b->swizzle;

    if (!format_compatible) {
        return false;
    }

    if (strict) {
        return a->width == b->width && a->height == b->height;
    }

    return a->width >= b->width && a->height >= b->height;
}

static bool check_surface_compatibility_except_swizzle(
    PGRAPHD3D12SurfaceBinding const *a,
    PGRAPHD3D12SurfaceBinding const *b, bool strict)
{
    bool format_compatible = a->color == b->color &&
                             a->fmt.dxgi_format == b->fmt.dxgi_format &&
                             a->pitch == b->pitch;

    if (!format_compatible) {
        return false;
    }

    if (strict) {
        return a->width == b->width && a->height == b->height;
    }

    return a->width >= b->width && a->height >= b->height;
}

static bool surface_guest_range_is_covered(
    const PGRAPHD3D12SurfaceBinding *surface, hwaddr start, uint64_t size)
{
    uint64_t logical_row_bytes;
    uint64_t logical_size;
    uint64_t offset;
    uint64_t row;
    uint64_t in_row;
    uint64_t range_end;
    uint64_t surface_end;

    if (size == 0) {
        return true;
    }
    if (start < surface->vram_addr || surface->width == 0 ||
        surface->height == 0) {
        return false;
    }

    if (umul64_overflow(surface->width, surface->fmt.bytes_per_pixel,
                        &logical_row_bytes) || logical_row_bytes == 0) {
        return false;
    }

    if (uadd64_overflow(start, size, &range_end)) {
        return false;
    }
    if (surface->swizzle) {
        if (umul64_overflow(logical_row_bytes, surface->height,
                            &logical_size) ||
            uadd64_overflow(surface->vram_addr, logical_size,
                            &surface_end)) {
            return false;
        }
        return range_end <= surface_end;
    }

    if (surface->pitch == 0) {
        return false;
    }

    offset = start - surface->vram_addr;
    row = offset / surface->pitch;
    in_row = offset % surface->pitch;
    if (row >= surface->height || in_row >= logical_row_bytes) {
        return false;
    }

    /* A range inside one visible row is covered despite pitch padding.  A
     * range crossing rows is covered only if the visible row intervals
     * touch; a clear never defines padding bytes. */
    if (size <= logical_row_bytes - in_row) {
        return true;
    }
    if (logical_row_bytes < surface->pitch) {
        return false;
    }

    if (umul64_overflow(surface->height - 1, surface->pitch,
                        &logical_size) ||
        uadd64_overflow(logical_size, logical_row_bytes, &logical_size) ||
        uadd64_overflow(surface->vram_addr, logical_size, &surface_end)) {
        return false;
    }
    return range_end <= surface_end;
}

static bool surface_guest_writes_cover(
    const PGRAPHD3D12SurfaceBinding *writer,
    const PGRAPHD3D12SurfaceBinding *old_surface)
{
    uint64_t old_row_bytes;
    uint64_t old_size;

    if (old_surface->width == 0 || old_surface->height == 0) {
        return false;
    }

    if (umul64_overflow(old_surface->width,
                        old_surface->fmt.bytes_per_pixel,
                        &old_row_bytes) || old_row_bytes == 0) {
        return false;
    }
    if (old_surface->swizzle) {
        if (umul64_overflow(old_row_bytes, old_surface->height, &old_size)) {
            return false;
        }
        return surface_guest_range_is_covered(
            writer, old_surface->vram_addr, old_size);
    }

    if (old_surface->pitch == 0) {
        return false;
    }
    for (unsigned int y = 0; y < old_surface->height; y++) {
        uint64_t row_offset;
        uint64_t row_start;

        if (umul64_overflow(y, old_surface->pitch, &row_offset) ||
            uadd64_overflow(old_surface->vram_addr, row_offset,
                            &row_start)) {
            return false;
        }
        if (!surface_guest_range_is_covered(
                writer, row_start, old_row_bytes)) {
            return false;
        }
    }
    return true;
}

static bool clear_rect_fully_covers_surface(
    PGRAPHState *pg, const PGRAPHD3D12SurfaceBinding *surface)
{
    uint32_t clearrectx = pgraph_reg_r(pg, NV_PGRAPH_CLEARRECTX);
    uint32_t clearrecty = pgraph_reg_r(pg, NV_PGRAPH_CLEARRECTY);
    unsigned int xmin = GET_MASK(clearrectx, NV_PGRAPH_CLEARRECTX_XMIN);
    unsigned int xmax = GET_MASK(clearrectx, NV_PGRAPH_CLEARRECTX_XMAX);
    unsigned int ymin = GET_MASK(clearrecty, NV_PGRAPH_CLEARRECTY_YMIN);
    unsigned int ymax = GET_MASK(clearrecty, NV_PGRAPH_CLEARRECTY_YMAX);
    unsigned int width;
    unsigned int height;

    if (!surface->width || !surface->height || xmin || ymin) {
        return false;
    }

    /* Match clear_surface's inclusive rectangle semantics.  A rectangle
     * larger than the target is clipped by D3D12 and still overwrites every
     * texel, while a reversed endpoint is only a one-pixel clear. */
    width = xmax >= xmin ? xmax - xmin + 1 : 1;
    height = ymax >= ymin ? ymax - ymin + 1 : 1;
    pgraph_apply_anti_aliasing_factor(pg, &width, &height);
    return width >= surface->width && height >= surface->height;
}

static bool clear_fully_overwrites_surface(
    PGRAPHState *pg, const PGRAPHD3D12SurfaceBinding *surface,
    uint32_t parameter)
{
    if (!clear_rect_fully_covers_surface(pg, surface)) {
        return false;
    }

    if (surface->color) {
        /* Deliberately false for B8/G8B8: they take the CPU
         * read/modify/write clear path, so discarding the old GPU layout
         * would make that preservation download reinterpret the surface.
         * A channel-masked clear keeps the other channels. */
        return !color_clear_needs_cpu(pg) &&
               color_clear_writes_all_channels(pg, parameter);
    }

    if (!(parameter & NV097_CLEAR_SURFACE_Z)) {
        return false;
    }

    /* Z16 is completely defined by depth.  Every physical component of a
     * Z24S8 resource is overwritten only when both depth and stencil are
     * selected. */
    return !surface->fmt.stencil ||
           (parameter & NV097_CLEAR_SURFACE_STENCIL);
}

static bool populate_surface_target_sized(NV2AState *d, bool color,
                                          unsigned int width,
                                          unsigned int height,
                                          PGRAPHD3D12SurfaceBinding *target)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    Surface *surface = color ? &pg->surface_color : &pg->surface_zeta;
    hwaddr dma_address = color ? pg->dma_color : pg->dma_zeta;
    DMAObject dma;

    memset(target, 0, sizeof(*target));
    if (!surface_format_for_target(pg, color, &target->fmt)) {
        report_unsupported_surface_format_once(d, color, width, height);
        return false;
    }

    dma = nv_dma_load(d, dma_address);
    assert(dma.dma_class == NV_DMA_IN_MEMORY_CLASS);
    assert(surface->offset <= dma.limit);
    assert(surface->offset + surface->pitch * height <= dma.limit + 1);
    assert(surface->pitch % target->fmt.bytes_per_pixel == 0);
    assert((dma.address & ~0x07FFFFFF) == 0);

    target->shape = (color || !r->color_binding) ? pg->surface_shape :
                                                 r->color_binding->shape;
    if (!color) {
        /* Clip and anti-aliasing follow the colour target; the depth
         * format is the one currently selected. */
        target->shape.zeta_format = pg->surface_shape.zeta_format;
        target->shape.z_format = pg->surface_shape.z_format;
    }
    target->color = color;
    target->swizzle =
        pg->surface_type == NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE;
    target->vram_addr = dma.address + surface->offset;
    target->width = width;
    target->height = height;
    target->pitch = surface->pitch;
    if (!target->swizzle && height != 0) {
        /* Exclude the unused tail after the final visible linear row. */
        target->size = (uint64_t)(height - 1) * surface->pitch +
                       (uint64_t)width * target->fmt.bytes_per_pixel;
    } else {
        target->size = (uint64_t)height *
                       MAX(surface->pitch,
                           width * target->fmt.bytes_per_pixel);
    }
    target->upload_pending = true;
    target->dma_addr = dma.address;
    target->dma_len = dma.limit;
    target->frame_time = pg->frame_time;
    target->write_frame_time = pg->frame_time;
    target->draw_time = pg->draw_time;
    target->state = D3D12_RESOURCE_STATE_COMMON;
    return true;
}

static bool populate_surface_target(NV2AState *d, bool color,
                                    PGRAPHD3D12SurfaceBinding *target)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    unsigned int width, height;

    if (color || !r->color_binding) {
        get_surface_dimensions(pg, &width, &height);
        pgraph_apply_anti_aliasing_factor(pg, &width, &height);

        if (pg->surface_type != NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE) {
            width += pg->surface_shape.clip_x;
            height += pg->surface_shape.clip_y;
        }
    } else {
        width = r->color_binding->width;
        height = r->color_binding->height;
    }

    return populate_surface_target_sized(d, color, width, height, target);
}

static bool surface_put(
    NV2AState *d, PGRAPHD3D12SurfaceBinding const *target,
    const PGRAPHD3D12SurfaceBinding *discard_surface,
    bool discard_covered_overlaps, bool guest_memory_changed,
    PGRAPHD3D12SurfaceBinding **surface_out, Error **errp)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    PGRAPHD3D12SurfaceBinding *surface = g_memdup2(target, sizeof(*target));

    surface->nv2a = d;

    if (!create_surface_resource(pg, surface, errp)) {
        if (r->display_snapshot_source == (const void *)surface) {
            r->display_snapshot_source = NULL;
            r->display_snapshot_valid = false;
        }
        g_free(surface);
        return false;
    }
    /* Allocate the replacement before retiring any existing image; every
     * overlap, including the discard candidate, stays alive until all
     * required downloads succeed. */
    if (!invalidate_overlapping_surfaces(d, surface, discard_surface,
                                         discard_covered_overlaps, errp)) {
        release_surface_descriptor(r, surface);
        pool_surface_resource(pg, surface);
        if (r->display_snapshot_source == (const void *)surface) {
            r->display_snapshot_source = NULL;
            r->display_snapshot_valid = false;
        }
        g_free(surface);
        return false;
    }
    /* A CPU write to a stashed range completes the stash through its
     * watchpoint before the write lands, so a stash is never older than
     * guest memory written by the CPU. The memory-dirty bits cannot tell
     * device writes apart from CPU writes made before the stash existed. */
    (void)guest_memory_changed;
    if (surface->upload_pending) {
        restore_surface_from_stash(d, surface);
    }
    QTAILQ_INSERT_TAIL(&r->surfaces, surface, entry);
    r->surface_layout_gen++;
    register_cpu_access_callback(d, surface);
    *surface_out = surface;
    return true;
}

/* Diagnostic: surface creations per perf interval, by target shape and the
 * shape of the surface each replaced at the same address. */
typedef struct SurfaceChurn {
    hwaddr addr;
    unsigned int width, height, pitch, fmt;
    unsigned int old_width, old_height, old_pitch, old_fmt;
    uint32_t control_0, control_1;
    bool color, swizzle, replaced, old_color, old_swizzle;
    unsigned int count;
} SurfaceChurn;

static SurfaceChurn surface_churn[32];
static unsigned int surface_churn_entries;
static unsigned int surface_churn_untracked;

static void record_surface_churn(PGRAPHState *pg,
                                 const PGRAPHD3D12SurfaceBinding *target,
                                 const PGRAPHD3D12SurfaceBinding *old)
{
    SurfaceChurn key;

    memset(&key, 0, sizeof(key));
    key.addr = target->vram_addr;
    key.width = target->width;
    key.height = target->height;
    key.pitch = target->pitch;
    key.fmt = target->fmt.dxgi_format;
    key.color = target->color;
    key.swizzle = target->swizzle;
    key.control_0 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0) &
                    (NV_PGRAPH_CONTROL_0_ZENABLE | NV_PGRAPH_CONTROL_0_ZFUNC |
                     NV_PGRAPH_CONTROL_0_ZWRITEENABLE |
                     NV_PGRAPH_CONTROL_0_STENCIL_WRITE_ENABLE);
    key.control_1 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1) &
                    (NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE |
                     NV_PGRAPH_CONTROL_1_STENCIL_FUNC);
    if (old != NULL) {
        key.replaced = true;
        key.old_width = old->width;
        key.old_height = old->height;
        key.old_pitch = old->pitch;
        key.old_fmt = old->fmt.dxgi_format;
        key.old_color = old->color;
        key.old_swizzle = old->swizzle;
    }
    for (unsigned int i = 0; i < surface_churn_entries; i++) {
        if (!memcmp(&surface_churn[i], &key, offsetof(SurfaceChurn, count))) {
            surface_churn[i].count++;
            return;
        }
    }
    if (surface_churn_entries == ARRAY_SIZE(surface_churn)) {
        surface_churn_untracked++;
        return;
    }
    key.count = 1;
    surface_churn[surface_churn_entries++] = key;
}

/* Event lines are buffered: writing each line to the log perturbs the
 * timing being observed. */
static GString *evlog_buffer;

#define ZCPK_SLOTS 8
#define ZCPK_SLOT_SIZE (2560u * 480u * 4u)

/* Diagnostic: reads the zeta depth plane back before the first draw of a
 * watched key in each frame. */
void pgraph_d3d12_record_depth_checkpoint(PGRAPHState *pg, uint32_t key,
                                          uint32_t draw)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    PGRAPHD3D12SurfaceBinding *s = r->zeta_binding;
    D3D12ZetaPlaneLayout layout;
    D3D12_TEXTURE_COPY_LOCATION dst;
    D3D12_TEXTURE_COPY_LOCATION src;
    D3D12_RESOURCE_STATES before;
    unsigned int scale = MAX(pg->surface_scale_factor, 1);
    unsigned int width;
    unsigned int height;
    uint32_t slot;
    int k;

    if (r->evlog_frames_left == 0 || s == NULL || s->resource == NULL ||
        !r->command_list_open ||
        s->fmt.dxgi_format != DXGI_FORMAT_D24_UNORM_S8_UINT) {
        return;
    }
    for (k = 0; k < 2; k++) {
        if (r->zcpk_key[k] != 0 && r->zcpk_key[k] == key &&
            r->zcpk_frame[k] != g_nv2a_stats.frame_count) {
            break;
        }
    }
    if (k == 2 || r->zcpk_next - r->zcpk_done >= ZCPK_SLOTS) {
        return;
    }
    width = s->width * scale;
    height = s->height * scale;
    if (!zeta_plane_layout(s, width, height, &layout, NULL) ||
        layout.depth_size > ZCPK_SLOT_SIZE) {
        return;
    }
    if (r->zcpk_buffer == NULL) {
        D3D12_HEAP_PROPERTIES heap = { .Type = D3D12_HEAP_TYPE_READBACK };
        D3D12_RESOURCE_DESC desc = {
            .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
            .Width = (UINT64)ZCPK_SLOTS * ZCPK_SLOT_SIZE,
            .Height = 1,
            .DepthOrArraySize = 1,
            .MipLevels = 1,
            .SampleDesc.Count = 1,
            .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
        };

        if (FAILED(r->device->lpVtbl->CreateCommittedResource(
                r->device, &heap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource,
                (void **)&r->zcpk_buffer)) ||
            FAILED(r->zcpk_buffer->lpVtbl->Map(r->zcpk_buffer, 0, NULL,
                                               (void **)&r->zcpk_mapped))) {
            D3D12_RELEASE(r->zcpk_buffer);
            r->zcpk_key[0] = r->zcpk_key[1] = 0;
            return;
        }
    }

    slot = r->zcpk_next % ZCPK_SLOTS;
    set_buffer_texture_copy_location(&dst, r->zcpk_buffer,
                                     (uint64_t)slot * ZCPK_SLOT_SIZE,
                                     layout.depth_format, width, height,
                                     layout.depth_row_pitch);
    set_texture_subresource_copy_location(&src, s->resource, 0);
    before = s->state;
    pgraph_d3d12_transition_raw_resource(r, s->resource, &s->state,
                                         D3D12_RESOURCE_STATE_COPY_SOURCE);
    r->command_list->lpVtbl->CopyTextureRegion(r->command_list, &dst, 0, 0,
                                               0, &src, NULL);
    pgraph_d3d12_transition_raw_resource(r, s->resource, &s->state, before);

    r->zcpk_frame[k] = g_nv2a_stats.frame_count;
    r->zcpk_meta[slot].fence = r->fence_value + 1;
    r->zcpk_meta[slot].frame = g_nv2a_stats.frame_count;
    r->zcpk_meta[slot].key = key;
    r->zcpk_meta[slot].draw = draw;
    r->zcpk_meta[slot].row_pitch = layout.depth_row_pitch;
    r->zcpk_meta[slot].width = width;
    r->zcpk_meta[slot].height = height;
    r->zcpk_next++;
}

void pgraph_d3d12_process_depth_checkpoints(PGRAPHD3D12State *r)
{
    uint64_t completed;

    if (r->zcpk_buffer == NULL || r->zcpk_mapped == NULL) {
        return;
    }
    completed = r->fence->lpVtbl->GetCompletedValue(r->fence);
    while (r->zcpk_done != r->zcpk_next) {
        uint32_t slot = r->zcpk_done % ZCPK_SLOTS;
        const uint8_t *data = r->zcpk_mapped + (size_t)slot * ZCPK_SLOT_SIZE;
        uint32_t h = 0;
        uint32_t zmin = 0xffffff;
        uint32_t zmax = 0;
        uint32_t far_count = 0;
        uint64_t zsum = 0;
        uint32_t y0;

        if (r->zcpk_meta[slot].fence > completed) {
            break;
        }
        y0 = r->zcpk_meta[slot].height - 32;
        for (uint32_t y = y0; y < r->zcpk_meta[slot].height; y++) {
            const uint32_t *row = (const uint32_t *)
                (data + (size_t)y * r->zcpk_meta[slot].row_pitch);

            for (uint32_t x = 0; x < 64; x++) {
                uint32_t z = r->zeta_depth_plane_float ?
                                 zeta_depth_from_float_bits(row[x]) :
                                 row[x] & 0xffffff;

                h = h * 1000003u ^ z;
                zmin = MIN(zmin, z);
                zmax = MAX(zmax, z);
                zsum += z;
                far_count += z == 0xffffff;
            }
        }
        pgraph_d3d12_evlog(r, "zcpk rec=%u key=%08x i=%u h=%08x min=%06x "
                           "max=%06x mean=%06x far=%u",
                           r->zcpk_meta[slot].frame, r->zcpk_meta[slot].key,
                           r->zcpk_meta[slot].draw, h, zmin, zmax,
                           (uint32_t)(zsum / 2048), far_count);
        r->zcpk_done++;
    }
}

void pgraph_d3d12_evlog_flush(void)
{
    if (evlog_buffer != NULL && evlog_buffer->len != 0) {
        fwrite(evlog_buffer->str, 1, evlog_buffer->len, stderr);
        fflush(stderr);
        g_string_truncate(evlog_buffer, 0);
    }
}

void pgraph_d3d12_evlog(PGRAPHD3D12State *r, const char *fmt, ...)
{
    va_list ap;

    if (r == NULL || r->evlog_frames_left == 0) {
        return;
    }
    if (evlog_buffer == NULL) {
        evlog_buffer = g_string_sized_new(16 << 20);
    }
    g_string_append_printf(evlog_buffer, "EV f=%u ",
                           g_nv2a_stats.frame_count);
    va_start(ap, fmt);
    g_string_append_vprintf(evlog_buffer, fmt, ap);
    va_end(ap);
    g_string_append_c(evlog_buffer, '\n');
    if (evlog_buffer->len > (48u << 20)) {
        pgraph_d3d12_evlog_flush();
    }
}

void pgraph_d3d12_report_surface_churn(void)
{
    unsigned int total = surface_churn_untracked;

    for (unsigned int i = 0; i < surface_churn_entries; i++) {
        total += surface_churn[i].count;
    }
    if (total >= 20 && xbox_diag_log) {
        fprintf(stderr, "Direct3D 12 surface churn: %u", total);
        for (unsigned int n = 0; n < 6; n++) {
            SurfaceChurn *best = NULL;

            for (unsigned int i = 0; i < surface_churn_entries; i++) {
                if (surface_churn[i].count != 0 &&
                    (best == NULL || surface_churn[i].count > best->count)) {
                    best = &surface_churn[i];
                }
            }
            if (best == NULL) {
                break;
            }
            fprintf(stderr,
                    " [%u 0x%" HWADDR_PRIx " %ux%u p%u %c%s dxgi=%u",
                    best->count, best->addr, best->width, best->height,
                    best->pitch, best->color ? 'c' : 'z',
                    best->swizzle ? "s" : "", best->fmt);
            fprintf(stderr, " c0=%x c1=%x", best->control_0, best->control_1);
            if (best->replaced) {
                fprintf(stderr, " <- %ux%u p%u %c%s dxgi=%u]",
                        best->old_width, best->old_height, best->old_pitch,
                        best->old_color ? 'c' : 'z',
                        best->old_swizzle ? "s" : "", best->old_fmt);
            } else {
                fprintf(stderr, " <- none]");
            }
            best->count = 0;
        }
        if (surface_churn_untracked != 0) {
            fprintf(stderr, " untracked=%u", surface_churn_untracked);
        }
        fprintf(stderr, "\n");
    }
    surface_churn_entries = 0;
    surface_churn_untracked = 0;
}

/* Diagnostic: dword sum and non-zero dword count of a guest range. */
static uint32_t evlog_ram_sum(NV2AState *d, hwaddr addr, hwaddr size,
                              uint32_t *nonzero)
{
    const uint32_t *p = (const uint32_t *)(d->vram_ptr + (addr & ~3));
    uint32_t sum = 0;

    *nonzero = 0;
    for (hwaddr i = 0; i < size / 4; i++) {
        sum += p[i];
        *nonzero += p[i] != 0;
    }
    return sum;
}

static bool update_surface_part(NV2AState *d, bool upload, bool color,
                                const uint32_t *clear_parameter,
                                Error **errp)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    PGRAPHD3D12SurfaceBinding target;
    PGRAPHD3D12SurfaceBinding *surface;
    Surface *pg_surface = color ? &pg->surface_color : &pg->surface_zeta;

    if (!populate_surface_target(d, color, &target)) {
        error_setg(errp,
                   "Direct3D 12 cannot bind %s surface: format=%s (0x%x), "
                   "requested_write=%d, pitch=%u, offset=0x%" HWADDR_PRIx
                   ", surface_type=%s (%u), clip=%u,%u %ux%u, log=%ux%u",
                   color ? "color" : "zeta",
                   color ? color_surface_format_name(
                               pg->surface_shape.color_format) :
                           zeta_surface_format_name(
                               pg->surface_shape.zeta_format),
                   color ? pg->surface_shape.color_format :
                           pg->surface_shape.zeta_format,
                   upload, pg_surface->pitch, (hwaddr)pg_surface->offset,
                   surface_type_name(pg->surface_type), pg->surface_type,
                   pg->surface_shape.clip_x, pg->surface_shape.clip_y,
                   pg->surface_shape.clip_width,
                   pg->surface_shape.clip_height,
                   pg->surface_shape.log_width, pg->surface_shape.log_height);
        return false;
    }

    bool mem_dirty = false;

    if (upload && r->evlog_frames_left > 0) {
        /* Diagnostic: locate the dirty pages. */
        hwaddr first = 0, last = 0;
        unsigned int pages = 0;

        for (hwaddr a = target.vram_addr & ~(hwaddr)0xfff;
             a < target.vram_addr + target.size; a += 0x1000) {
            if (memory_region_test_and_clear_dirty(d->vram, a, 0x1000,
                                                   DIRTY_MEMORY_NV2A)) {
                if (pages++ == 0) {
                    first = a;
                }
                last = a;
            }
        }
        mem_dirty = pages != 0;
        if (mem_dirty) {
            uint32_t nonzero;
            uint32_t sum = evlog_ram_sum(d, target.vram_addr, target.size,
                                         &nonzero);

            pgraph_d3d12_evlog(
                r, "mdirty %c 0x%" HWADDR_PRIx " pages=%u first=0x%"
                HWADDR_PRIx " last=0x%" HWADDR_PRIx " sum=%08x nz=%u",
                color ? 'c' : 'z', target.vram_addr, pages, first, last,
                sum, nonzero);
        }
    } else {
        mem_dirty = upload &&
            memory_region_test_and_clear_dirty(d->vram, target.vram_addr,
                                               target.size,
                                               DIRTY_MEMORY_NV2A);
    }
    bool clear_discards_upload = clear_parameter != NULL &&
        clear_fully_overwrites_surface(pg, &target, *clear_parameter);
    PGRAPHD3D12SurfaceBinding *current =
        color ? r->color_binding : r->zeta_binding;

    if (mem_dirty && tcg_enabled()) {
        /* A draw-dirty surface is watched byte-exactly: a vCPU write to its
         * bytes writes the image back and marks it for upload before the
         * write lands. Its guest bytes are stale, and the page-granular
         * dirty bitmap then only reports writes to other data sharing its
         * first or last page; uploading on those would replace the GPU
         * image with stale memory. */
        PGRAPHD3D12SurfaceBinding *existing =
            current != NULL && current->vram_addr == target.vram_addr ?
                current : pgraph_d3d12_surface_get(d, target.vram_addr);

        if (existing != NULL && existing->draw_dirty &&
            existing->access_cb != NULL &&
            existing->size >= target.size) {
            pgraph_d3d12_evlog(r, "mdirty ignored %c 0x%" HWADDR_PRIx,
                               color ? 'c' : 'z', target.vram_addr);
            mem_dirty = false;
        }
    }

    if (clear_discards_upload) {
        /* The following full clear defines every texel; uploading stale
         * guest VRAM first is dead work and can overwrite a newer aliased
         * surface. */
        target.upload_pending = false;
    }

    if (!current || (upload && (pg_surface->buffer_dirty || mem_dirty))) {
        const bool allow_clear_layout_migration = true;

        pgraph_d3d12_evlog(r, "rebind %c 0x%" HWADDR_PRIx
                           " cur=%d bd=%d md=%d up=%d",
                           color ? 'c' : 'z', target.vram_addr,
                           current != NULL, pg_surface->buffer_dirty,
                           mem_dirty, upload);
        const bool allow_clear_surface_discard = true;
        bool should_create = true;
        PGRAPHD3D12SurfaceBinding *discard_surface = NULL;

        unbind_surface(pg, color);
        surface = pgraph_d3d12_surface_get(d, target.vram_addr);

        if (surface != NULL) {
            bool compatible = check_surface_compatibility(surface, &target,
                                                          false);
            bool migrate_swizzle = false;
            bool discard_for_clear = false;

            /* A uniform (fully cleared) image is invariant under the guest
             * linear/swizzled permutation, so reuse the resource across a
             * type change when identity is otherwise exact and either side
             * is a complete clear.  The cleared bit is kept only while every
             * clear since the last complete one covered the whole image, so
             * partial clears cannot enter this path. */
            if (allow_clear_layout_migration && !compatible &&
                surface->swizzle != target.swizzle &&
                check_surface_compatibility_except_swizzle(surface, &target,
                                                            true) &&
                ((clear_parameter != NULL &&
                  clear_fully_overwrites_surface(pg, &target,
                                                 *clear_parameter)) ||
                 (surface->cleared && !mem_dirty))) {
                compatible = true;
                migrate_swizzle = true;
            }

            /* A full incoming clear can also make preservation unnecessary.
             * Discard the old image only when the new target's guest writes
             * cover every byte it represents; clipped/pitched tails and
             * partial component clears are excluded. */
            if (allow_clear_surface_discard && !compatible &&
                clear_parameter != NULL &&
                clear_discards_upload &&
                surface_guest_writes_cover(&target, surface)) {
                discard_for_clear = true;
            }

            if (compatible && color && !migrate_swizzle &&
                !check_surface_compatibility(surface, &target, true)) {
                PGRAPHD3D12SurfaceBinding zeta_target;
                hwaddr color_end = surface->vram_addr + surface->size;

                if (populate_surface_target_sized(d, false, surface->width,
                                                  surface->height,
                                                  &zeta_target)) {
                    hwaddr zeta_end = zeta_target.vram_addr +
                                      zeta_target.size;
                    compatible &= surface->vram_addr >= zeta_end ||
                                  zeta_target.vram_addr >= color_end;
                }
            }

            if (compatible && !color && r->color_binding) {
                compatible &= surface->width == r->color_binding->width &&
                              surface->height == r->color_binding->height;
            }

            if (compatible) {
                if (migrate_swizzle) {
                    bool callback_range_changed = surface->size != target.size;

                    if (callback_range_changed) {
                        unregister_cpu_access_callback(d, surface);
                    }
                    surface->shape = target.shape;
                    surface->fmt = target.fmt;
                    surface->swizzle = target.swizzle;
                    surface->size = target.size;
                    r->surface_layout_gen++;
                    surface->dma_addr = target.dma_addr;
                    surface->dma_len = target.dma_len;
                    surface->upload_pending = false;
                    surface->download_pending = false;
                    surface->download_failures = 0;
                    surface->range_download_valid = false;
                    if (callback_range_changed) {
                        register_cpu_access_callback(d, surface);
                    }
                }
                pg->surface_binding_dim.width = surface->width;
                pg->surface_binding_dim.clip_x = surface->shape.clip_x;
                pg->surface_binding_dim.clip_width = surface->shape.clip_width;
                pg->surface_binding_dim.height = surface->height;
                pg->surface_binding_dim.clip_y = surface->shape.clip_y;
                pg->surface_binding_dim.clip_height =
                    surface->shape.clip_height;
                if (!migrate_swizzle) {
                    surface->upload_pending |= mem_dirty;
                }
                pg->surface_zeta.buffer_dirty |= color;
                should_create = false;
            } else if (discard_for_clear) {
                target.upload_pending = false;
                discard_surface = surface;
            } else {
                unsigned int alias_x, alias_y;
                bool alias_eligible =
                    r->alias_copy_count < ARRAY_SIZE(r->alias_copies) &&
                    alias_copy_rect(surface, &target, &alias_x, &alias_y);

                /* Under TCG the overlap pass in surface_put stashes the
                 * old image instead of draining the queue here. */
                if (!alias_eligible && !tcg_enabled() &&
                    !pgraph_d3d12_surface_download_if_dirty_checked_reason(
                        d, surface,
                        PGRAPH_D3D12_SURFACE_DOWNLOAD_INCOMPATIBLE, errp)) {
                    error_prepend(
                        errp,
                        "Direct3D 12 could not preserve incompatible %s "
                        "surface addr=0x%" HWADDR_PRIx ": ",
                        surface->color ? "color" : "zeta",
                        surface->vram_addr);
                    /* The old target was unbound before compatibility was
                     * known; keep it active so a transient failure does not
                     * turn every following draw into a missing-target retry. */
                    if (color) {
                        r->color_binding = current;
                    } else {
                        r->zeta_binding = current;
                    }
                    return false;
                }
            }
        }

        if (should_create) {
            record_surface_churn(pg, &target, surface);
            pgraph_d3d12_evlog(
                r, "create %c 0x%" HWADDR_PRIx " %ux%u p%u swz=%d dxgi=%u "
                "up=%d old=%s", color ? 'c' : 'z', target.vram_addr,
                target.width, target.height, target.pitch, target.swizzle,
                (unsigned int)target.fmt.dxgi_format, target.upload_pending,
                surface != NULL ? "replaced" : "none");
            if (!surface_put(d, &target, discard_surface,
                             clear_discards_upload, mem_dirty, &surface,
                             errp)) {
                if (color) {
                    r->color_binding = current;
                } else {
                    r->zeta_binding = current;
                }
                return false;
            }

            pg->surface_binding_dim.width = target.width;
            pg->surface_binding_dim.clip_x = target.shape.clip_x;
            pg->surface_binding_dim.clip_width = target.shape.clip_width;
            pg->surface_binding_dim.height = target.height;
            pg->surface_binding_dim.clip_y = target.shape.clip_y;
            pg->surface_binding_dim.clip_height = target.shape.clip_height;

            if (color && r->zeta_binding &&
                (r->zeta_binding->width != target.width ||
                 r->zeta_binding->height != target.height)) {
                pg->surface_zeta.buffer_dirty = true;
            }
        }

        if (color) {
            r->color_binding = surface;
        } else {
            r->zeta_binding = surface;
        }
        pg_surface->buffer_dirty = false;
    }

    surface = color ? r->color_binding : r->zeta_binding;
    if (!upload && pg_surface->draw_dirty) {
        if (!tcg_enabled()) {
            if (!download_surface_data(
                    d, surface, PGRAPH_D3D12_SURFACE_DOWNLOAD_DISPLAY_CPU,
                    true, errp)) {
                return false;
            }
        }
        pg_surface->write_enabled_cache = false;
        pg_surface->draw_dirty = false;
    }

    return true;
}

void pgraph_d3d12_init_surfaces(PGRAPHState *pg)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;

    QTAILQ_INIT(&r->surfaces);
    r->framebuffer_copy_state = D3D12_RESOURCE_STATE_COMMON;
    r->framebuffer_copy_format = DXGI_FORMAT_UNKNOWN;
    r->display_snapshot_state = D3D12_RESOURCE_STATE_COPY_DEST;
    r->display_snapshot_format = DXGI_FORMAT_UNKNOWN;
    r->downloads_pending = false;
    qemu_event_init(&r->downloads_complete, false);
    QTAILQ_INIT(&r->stashed_surfaces);
    r->download_dirty_surfaces_pending = false;
    qemu_event_init(&r->dirty_surfaces_download_complete, false);
}

void pgraph_d3d12_process_pending_downloads(NV2AState *d)
{
    PGRAPHD3D12State *r = d->pgraph.d3d12_renderer_state;
    PGRAPHD3D12SurfaceBinding *surface;
    PGRAPHD3D12SurfaceBinding *pending[SURFACE_READBACK_BATCH_MAX];
    unsigned int pending_count = 0;
    unsigned int range_count = r->cpu_access_range_count;

    /* Only the stashes the vCPU touched are written back. Live dirty
     * surfaces over those bytes are downloaded too: one may have adopted a
     * touched stash's image, or received it through an alias copy, after
     * the vCPU recorded the access. */
    if (r->cpu_access_range_overflow) {
        pgraph_d3d12_complete_all_readbacks(d, PGRAPH_D3D12_STASH_TRIGGER_CPU);
        QTAILQ_FOREACH(surface, &r->surfaces, entry) {
            if (surface->draw_dirty) {
                surface->download_pending = true;
            }
        }
    } else {
        for (unsigned int i = 0; i < range_count; i++) {
            hwaddr addr = r->cpu_access_ranges[i].addr;
            hwaddr len = r->cpu_access_ranges[i].len;

            pgraph_d3d12_evlog(r, "cpudl range=0x%" HWADDR_PRIx
                               "+0x%" HWADDR_PRIx " w=%d", addr, len,
                               r->cpu_access_ranges[i].write);
            complete_readbacks_in_range(d, addr, len,
                                        PGRAPH_D3D12_STASH_TRIGGER_CPU, true);
            QTAILQ_FOREACH(surface, &r->surfaces, entry) {
                if (surface->draw_dirty &&
                    surface_overlaps_range(surface, addr, len)) {
                    surface->download_pending = true;
                }
            }
        }
    }
    r->cpu_access_range_count = 0;
    r->cpu_access_range_overflow = false;

    QTAILQ_FOREACH(surface, &r->surfaces, entry) {
        Error *local_err = NULL;

        if (!surface->download_pending || !surface->draw_dirty) {
            continue;
        }
        pgraph_d3d12_evlog(r, "cpudl surf=0x%" HWADDR_PRIx " %ux%u",
                           surface->vram_addr, surface->width,
                           surface->height);
        if (pending_count == ARRAY_SIZE(pending)) {
            if (!pgraph_d3d12_download_surfaces_checked_reason(
                    d, pending, pending_count,
                    PGRAPH_D3D12_SURFACE_DOWNLOAD_DISPLAY_CPU, &local_err)) {
                /* The per-surface pass below retries and reports. */
                error_free(local_err);
            }
            pending_count = 0;
        }
        pending[pending_count++] = surface;
    }
    if (pending_count != 0) {
        Error *local_err = NULL;

        if (!pgraph_d3d12_download_surfaces_checked_reason(
                d, pending, pending_count,
                PGRAPH_D3D12_SURFACE_DOWNLOAD_DISPLAY_CPU, &local_err)) {
            error_free(local_err);
        }
    }

    QTAILQ_FOREACH(surface, &r->surfaces, entry) {
        Error *local_err = NULL;
        bool logged = surface->download_pending && surface->draw_dirty;

        if (!download_surface_data(
                d, surface, PGRAPH_D3D12_SURFACE_DOWNLOAD_DISPLAY_CPU,
                false, &local_err)) {
            report_surface_error("CPU-access surface download", surface,
                                 local_err);
            error_free(local_err);
            surface_note_download_failure(surface);
        }
        if (logged && r->evlog_frames_left > 0) {
            uint32_t nonzero;
            uint32_t sum = evlog_ram_sum(d, surface->vram_addr,
                                         surface->size, &nonzero);

            pgraph_d3d12_evlog(r, "cpudl done 0x%" HWADDR_PRIx
                               " sum=%08x nz=%u", surface->vram_addr,
                               sum, nonzero);
        }
        /* Downloaded surfaces no longer need their watchpoint. */
        if (!surface->draw_dirty && tcg_enabled()) {
            mem_access_callback_set_enabled(qemu_get_cpu(0),
                                            surface->access_cb, false);
        }
    }

    qatomic_set(&r->downloads_pending, false);
    qemu_event_set(&r->downloads_complete);
}

void pgraph_d3d12_download_dirty_surfaces(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    PGRAPHD3D12SurfaceBinding *surface;

    pgraph_d3d12_complete_all_readbacks(d, PGRAPH_D3D12_STASH_TRIGGER_SYNC);
    QTAILQ_FOREACH(surface, &r->surfaces, entry) {
        pgraph_d3d12_surface_download_if_dirty(d, surface);
    }

    qatomic_set(&r->download_dirty_surfaces_pending, false);
    qemu_event_set(&r->dirty_surfaces_download_complete);
}

void pgraph_d3d12_finalize_surfaces(PGRAPHState *pg)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    PGRAPHD3D12SurfaceBinding *surface, *next;

    if (r == NULL) {
        return;
    }

    pgraph_d3d12_complete_all_readbacks(container_of(pg, NV2AState, pgraph),
                                        PGRAPH_D3D12_STASH_TRIGGER_FINAL);
    pgraph_d3d12_finish(pg);
    release_pooled_surface_resources(pg);
    QTAILQ_FOREACH_SAFE(surface, &r->surfaces, entry, next) {
        unregister_cpu_access_callback(
            container_of(pg, NV2AState, pgraph), surface);
        release_surface_descriptor(r, surface);
        D3D12_RELEASE(surface->resource);
        QTAILQ_REMOVE(&r->surfaces, surface, entry);
                if (r->display_snapshot_source == (const void *)surface) {
            r->display_snapshot_source = NULL;
            r->display_snapshot_valid = false;
        }
        g_free(surface);
    }

    D3D12_RELEASE(r->framebuffer_copy_resource);
    r->framebuffer_copy_state = D3D12_RESOURCE_STATE_COMMON;
    r->framebuffer_copy_format = DXGI_FORMAT_UNKNOWN;
    r->framebuffer_copy_width = 0;
    r->framebuffer_copy_height = 0;
    r->framebuffer_copy_valid = false;

    D3D12_RELEASE(r->display_snapshot_resource);
    for (unsigned int i = 0; i < ARRAY_SIZE(r->display_snapshot_spares); i++) {
        D3D12_RELEASE(r->display_snapshot_spares[i].resource);
    }
    r->display_snapshot_release_value = 0;
    r->display_snapshot_state = D3D12_RESOURCE_STATE_COPY_DEST;
    r->display_snapshot_format = DXGI_FORMAT_UNKNOWN;
    r->display_snapshot_width = 0;
    r->display_snapshot_height = 0;
    r->display_snapshot_valid = false;

    r->color_binding = NULL;
    r->zeta_binding = NULL;
    r->next_rtv_descriptor = 0;
    r->next_dsv_descriptor = 0;
    r->num_free_rtv_descriptors = 0;
    r->num_free_dsv_descriptors = 0;
    qemu_event_destroy(&r->downloads_complete);
    qemu_event_destroy(&r->dirty_surfaces_download_complete);
}

static bool surface_flush(NV2AState *d, bool lossy, Error **errp)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    PGRAPHD3D12SurfaceBinding *surface, *next;

    if (r == NULL) {
        return true;
    }

    pgraph_d3d12_finish(pg);

    /* Preserve every GPU surface before invalidating any; if a readback
     * fails (e.g. scale outside the memory budget) the old-scale set stays
     * bound so the caller can reject the change. A lossy flush drops the
     * surfaces it cannot preserve instead. */
    QTAILQ_FOREACH(surface, &r->surfaces, entry) {
        Error *local_err = NULL;

        if (!pgraph_d3d12_surface_download_if_dirty_checked(d, surface,
                                                            &local_err)) {
            if (lossy) {
                report_surface_error("surface flush", surface, local_err);
                error_free(local_err);
                continue;
            }
            error_propagate_prepend(
                errp, local_err,
                "Direct3D 12 surface flush could not preserve %s "
                "surface addr=0x%" HWADDR_PRIx ": ",
                surface->color ? "color" : "zeta", surface->vram_addr);
            return false;
        }
    }

    r->display_surface_valid = false;
    r->display_surface_binding = NULL;
    pg->surface_color.draw_dirty = false;
    pg->surface_zeta.draw_dirty = false;
    memset(&pg->last_surface_shape, 0, sizeof(pg->last_surface_shape));

    QTAILQ_FOREACH_SAFE(surface, &r->surfaces, entry, next) {
        invalidate_surface(d, surface);
    }

    /* Do not reset the RTV/DSV slot allocators: invalidated slots return to
     * the free lists behind the fence, and a reset would alias two live
     * surfaces onto one slot. */
    return true;
}

bool pgraph_d3d12_surface_flush_checked(NV2AState *d, Error **errp)
{
    return surface_flush(d, false, errp);
}

void pgraph_d3d12_surface_flush_lossy(NV2AState *d)
{
    surface_flush(d, true, NULL);
}

void pgraph_d3d12_surface_flush(NV2AState *d)
{
    Error *local_err = NULL;

    if (!pgraph_d3d12_surface_flush_checked(d, &local_err)) {
        fprintf(stderr, "Direct3D 12 renderer: surface flush failed: %s\n",
                local_err != NULL ? error_get_pretty(local_err) :
                                    "unknown error");
        error_free(local_err);
    }
}

/*
 * Guest reset and state load replace guest memory, which is then
 * authoritative: every surface, stashed image and blit display candidate
 * is dropped without being written back.
 */
void pgraph_d3d12_surface_discard_all(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    PGRAPHD3D12SurfaceBinding *surface, *next;

    if (r == NULL) {
        return;
    }

    pgraph_d3d12_finish(pg);
    pgraph_d3d12_drop_all_stashes(d);

    r->display_surface_valid = false;
    r->display_surface_binding = NULL;
    pg->surface_color.draw_dirty = false;
    pg->surface_zeta.draw_dirty = false;
    memset(&pg->last_surface_shape, 0, sizeof(pg->last_surface_shape));

    QTAILQ_FOREACH_SAFE(surface, &r->surfaces, entry, next) {
        surface->draw_dirty = false;
        surface->download_pending = false;
        invalidate_surface(d, surface);
    }
    pgraph_d3d12_invalidate_blit_display_candidates(
        r, 0, memory_region_size(d->vram));
}

/*
 * Release surfaces that no recent frame has used, as the GL and Vulkan
 * renderers do after five frames, so the adapter budget stays bounded. A
 * surface whose write-back fails is kept and retried later.
 */
static void expire_old_surfaces(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    PGRAPHD3D12SurfaceBinding *surface, *next;
    const int age_limit = 5;

    if (r->surface_expire_frame_time == pg->frame_time) {
        return;
    }
    r->surface_expire_frame_time = pg->frame_time;
    expire_pooled_surface_resources(pg);

    QTAILQ_FOREACH_SAFE(surface, &r->surfaces, entry, next) {
        Error *local_err = NULL;

        if (surface == r->color_binding || surface == r->zeta_binding ||
            surface == r->display_surface_binding ||
            pg->frame_time - surface->frame_time < age_limit) {
            continue;
        }
        if (!pgraph_d3d12_surface_download_if_dirty_checked(d, surface,
                                                            &local_err)) {
            error_free(local_err);
            continue;
        }
        invalidate_surface(d, surface);
    }

    /* Stashes nobody adopted within the age limit are written back, oldest
     * first, so they stop pinning adapter memory and a later device write
     * into their bytes is not overwritten by a late write-back. */
    while ((surface = QTAILQ_FIRST(&r->stashed_surfaces)) != NULL &&
           pg->frame_time - surface->frame_time >= age_limit) {
        complete_stashed_surface(d, surface, PGRAPH_D3D12_STASH_TRIGGER_CAP);
    }
}

/*
 * A depth or stencil test reads the zeta buffer unless its function ignores
 * the stored value. Without a zeta the draw runs with both tests disabled,
 * which matches ALWAYS but not NEVER.
 */
static bool zeta_test_reads_buffer(PGRAPHState *pg)
{
    uint32_t control_0 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0);
    uint32_t control_1 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1);

    return ((control_0 & NV_PGRAPH_CONTROL_0_ZENABLE) &&
            GET_MASK(control_0, NV_PGRAPH_CONTROL_0_ZFUNC) !=
                NV_PGRAPH_CONTROL_0_ZFUNC_ALWAYS) ||
           ((control_1 & NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE) &&
            GET_MASK(control_1, NV_PGRAPH_CONTROL_1_STENCIL_FUNC) !=
                NV_PGRAPH_CONTROL_1_STENCIL_FUNC_ALWAYS);
}

/*
 * Whether a draw writes the zeta buffer, as the pipeline state does it
 * (fill_depth_stencil_key): depth only with the depth test on, stencil only
 * with the stencil test on. The write-enable bits alone are not enough: Hot
 * Wheels draws its 256x256 and 128x128 glow passes (swizzled) with the main
 * 640x480 depth buffer still set, ZFUNC ALWAYS, depth writes off and the
 * stencil test off but its write bit on. Counted as a write, each pass made a
 * swizzled depth surface over the 640x480 one and read the main depth back
 * and up again several times a frame; the title logo was then tested against
 * scrambled depth and cut along the mountains.
 */
static bool zeta_draw_writes_buffer(PGRAPHState *pg)
{
    uint32_t control_0 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0);
    uint32_t control_1 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1);

    return ((control_0 & NV_PGRAPH_CONTROL_0_ZENABLE) &&
            (control_0 & NV_PGRAPH_CONTROL_0_ZWRITEENABLE)) ||
           ((control_1 & NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE) &&
            (control_0 & NV_PGRAPH_CONTROL_0_STENCIL_WRITE_ENABLE));
}

static void pgraph_d3d12_surface_update_internal(
    NV2AState *d, bool upload, bool color_write, bool zeta_write,
    const uint32_t *clear_parameter)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    bool zeta_access = zeta_write;
    Error *local_err = NULL;

    if (r == NULL) {
        return;
    }

    pg->surface_shape.z_format =
        GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER),
                 NV_PGRAPH_SETUPRASTER_Z_FORMAT);

    color_write = color_write &&
                  (pg->clearing || pgraph_color_write_enabled(pg));
    zeta_write = zeta_write &&
                 (pg->clearing || zeta_draw_writes_buffer(pg));
    zeta_access = zeta_access && (pg->clearing || zeta_write ||
                                  zeta_test_reads_buffer(pg));

    if (upload) {
        if (framebuffer_dirty(pg)) {
            const SurfaceShape *o = &pg->last_surface_shape;
            const SurfaceShape *n = &pg->surface_shape;

            pgraph_d3d12_evlog(
                r, "fbd zf=%u>%u cf=%u>%u zt=%u>%u lw=%u>%u lh=%u>%u "
                "clip=%u,%u,%u,%u>%u,%u,%u,%u aa=%u>%u",
                o->z_format, n->z_format, o->color_format,
                n->color_format, o->zeta_format, n->zeta_format,
                o->log_width, n->log_width, o->log_height, n->log_height,
                o->clip_x, o->clip_y, o->clip_width, o->clip_height,
                n->clip_x, n->clip_y, n->clip_width, n->clip_height,
                o->anti_aliasing, n->anti_aliasing);
            memcpy(&pg->last_surface_shape, &pg->surface_shape,
                   sizeof(SurfaceShape));
            pg->surface_color.buffer_dirty = true;
            pg->surface_zeta.buffer_dirty = true;
        }

        if (pg->surface_color.buffer_dirty) {
            unbind_surface(pg, true);
        }
        if (color_write &&
            !update_surface_part(d, true, true, clear_parameter,
                                 &local_err)) {
            goto fail;
        }

        if (pg->surface_zeta.buffer_dirty) {
            unbind_surface(pg, false);
        }
        if (zeta_access &&
            !update_surface_part(d, true, false, clear_parameter,
                                 &local_err)) {
            goto fail;
        }

    } else {
        if ((color_write || pg->surface_color.write_enabled_cache) &&
            pg->surface_color.draw_dirty &&
            !update_surface_part(d, false, true, NULL, &local_err)) {
            goto fail;
        }
        if ((zeta_write || pg->surface_zeta.write_enabled_cache) &&
            pg->surface_zeta.draw_dirty &&
            !update_surface_part(d, false, false, NULL, &local_err)) {
            goto fail;
        }
    }

    bool swizzle = pg->surface_type == NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE;

    if (r->color_binding) {
        r->color_binding->frame_time = pg->frame_time;
        if (upload) {
            if (clear_parameter != NULL &&
                clear_fully_overwrites_surface(
                    pg, r->color_binding, *clear_parameter)) {
                r->color_binding->upload_pending = false;
            }
            if (!upload_surface_data(d, r->color_binding, false,
                                     D3D12_RESOURCE_STATE_RENDER_TARGET,
                                     &local_err)) {
                goto fail;
            }
            if (r->color_binding->swizzle != swizzle) {
                r->color_binding->swizzle = swizzle;
                r->surface_layout_gen++;
            }
        }
    }

    if (r->zeta_binding) {
        r->zeta_binding->frame_time = pg->frame_time;
        if (upload) {
            if (clear_parameter != NULL &&
                clear_fully_overwrites_surface(
                    pg, r->zeta_binding, *clear_parameter)) {
                r->zeta_binding->upload_pending = false;
            }
            if (!upload_surface_data(d, r->zeta_binding, false,
                                     D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                     &local_err)) {
                goto fail;
            }
            if (r->zeta_binding->swizzle != swizzle) {
                r->zeta_binding->swizzle = swizzle;
                r->surface_layout_gen++;
            }
        }
    }

    if (r->color_binding && r->zeta_binding) {
        assert(r->color_binding->width == r->zeta_binding->width);
        assert(r->color_binding->height == r->zeta_binding->height);
    }

    /* The new targets have their guest bytes; carry over aliased pixels
     * and retire idle surfaces. */
    apply_alias_copies(d);
    expire_old_surfaces(d);
    return;

fail:
    apply_alias_copies(d);
    fprintf(stderr,
            "Direct3D 12 renderer: surface_update failed: upload=%d, "
            "color_write=%d, zeta_access=%d, zeta_write=%d, "
            "color_format=%s (0x%x), "
            "zeta_format=%s (0x%x), surface_type=%s (%u): %s\n",
            upload, color_write, zeta_access, zeta_write,
            color_surface_format_name(pg->surface_shape.color_format),
            pg->surface_shape.color_format,
            zeta_surface_format_name(pg->surface_shape.zeta_format),
            pg->surface_shape.zeta_format,
            surface_type_name(pg->surface_type), pg->surface_type,
            local_err ? error_get_pretty(local_err) : "unknown error");
    if (local_err) {
        error_free(local_err);
    }
}

bool pgraph_d3d12_select_display_surface(NV2AState *d, hwaddr display_addr,
                                         hwaddr pcrtc_start,
                                         uint32_t line_offset,
                                         bool download)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    PGRAPHD3D12SurfaceBinding *surface;
    Error *local_err = NULL;

    if (r == NULL) {
        return false;
    }

    r->display_surface_valid = false;
    r->display_surface_binding = NULL;

    surface = surface_get_color_within(d, display_addr);
    if (surface != NULL && surface->upload_pending &&
        !surface->draw_dirty &&
        set_raw_pcrtc_display_surface(d, display_addr, pcrtc_start,
                                      line_offset)) {
        /* CPU writes marked this tracked surface for upload and a
         * CPU-scanout guest may never flip or draw again; guest VRAM is
         * authoritative, so keep raw scanout until a GPU bind consumes the
         * upload. */
        return true;
    }
    if (surface == NULL) {
        /* Scan-out memory no tracked surface covers is shown from guest
         * memory in the CRTC mode. A mode without a scan-out pixel format
         * takes it from a blit that wrote the whole raster; otherwise
         * nothing is shown. */
        if (set_raw_pcrtc_display_surface(d, display_addr, pcrtc_start,
                                          line_offset) ||
            set_blit_display_surface(d, display_addr, pcrtc_start,
                                     line_offset)) {
            return true;
        }

        report_missing_display_surface(d, display_addr, pcrtc_start,
                                       line_offset);
        return false;
    }

    if (!surface->width || !surface->height) {
        pgraph_d3d12_warn_once(
            pg, PGRAPH_D3D12_DIAG_DISPLAY_DOWNLOAD,
            "display fallback skipped zero-sized color surface: "
            "display_addr=0x%" HWADDR_PRIx ", surface_addr=0x%"
            HWADDR_PRIx ", size=%ux%u, pitch=%u, bytes_per_pixel=%u, "
            "format=0x%x, draw_dirty=%d, download_pending=%d",
            display_addr, surface->vram_addr, surface->width,
            surface->height, surface->pitch, surface->fmt.bytes_per_pixel,
            surface->shape.color_format, surface->draw_dirty,
            surface->download_pending);
        return false;
    }

    surface->frame_time = pg->frame_time;
    if (download &&
        !download_surface_data(
            d, surface, PGRAPH_D3D12_SURFACE_DOWNLOAD_DISPLAY_CPU, true,
            &local_err)) {
        report_surface_error("display surface download", surface, local_err);
        error_free(local_err);
        return false;
    }

    if ((pcrtc_start != surface->vram_addr || line_offset != surface->pitch) &&
        set_pcrtc_scanout_display_surface(d, display_addr, pcrtc_start,
                                          line_offset, surface)) {
        return true;
    }

    set_display_surface_state(r, surface, surface->vram_addr,
                              surface->pitch);

    return true;
}

bool pgraph_d3d12_download_display_surface(NV2AState *d, hwaddr display_addr,
                                           hwaddr pcrtc_start,
                                           uint32_t line_offset)
{
    return pgraph_d3d12_select_display_surface(d, display_addr, pcrtc_start,
                                               line_offset, true);
}

void pgraph_d3d12_set_surface_dirty(PGRAPHState *pg, bool color, bool zeta)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;

    color = color && (pg->clearing || pgraph_color_write_enabled(pg));
    zeta = zeta && (pg->clearing || zeta_draw_writes_buffer(pg));

    pg->surface_color.draw_dirty |= color;
    pg->surface_zeta.draw_dirty |= zeta;

    if (r->color_binding) {
        if (color && !r->color_binding->draw_dirty) {
            r->color_binding->draw_dirty = true;
            if (tcg_enabled()) {
                mem_access_callback_set_enabled(
                    qemu_get_cpu(0), r->color_binding->access_cb, true);
            }
        }
        r->color_binding->frame_time = pg->frame_time;
        if (color) {
            r->color_binding->cleared = false;
            r->color_binding->write_frame_time = pg->frame_time;
        }
    }

    if (r->zeta_binding) {
        if (zeta && !r->zeta_binding->draw_dirty) {
            r->zeta_binding->draw_dirty = true;
            if (tcg_enabled()) {
                mem_access_callback_set_enabled(
                    qemu_get_cpu(0), r->zeta_binding->access_cb, true);
            }
        }
        r->zeta_binding->frame_time = pg->frame_time;
        if (zeta) {
            r->zeta_binding->cleared = false;
            r->zeta_binding->write_frame_time = pg->frame_time;
        }
    }
}

void pgraph_d3d12_surface_update(NV2AState *d, bool upload,
                                 bool color_write, bool zeta_write)
{
    pgraph_d3d12_surface_update_internal(d, upload, color_write, zeta_write,
                                         NULL);
}

void pgraph_d3d12_bind_render_targets(PGRAPHState *pg)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    D3D12_CPU_DESCRIPTOR_HANDLE *rtv_ptr = NULL;
    D3D12_CPU_DESCRIPTOR_HANDLE *dsv_ptr = NULL;

    if (r == NULL || r->command_list == NULL) {
        return;
    }

    if (r->color_binding) {
        pgraph_d3d12_transition_raw_resource(
            r, r->color_binding->resource, &r->color_binding->state,
            D3D12_RESOURCE_STATE_RENDER_TARGET);
        rtv = r->color_binding->descriptor;
        rtv_ptr = &rtv;
    }

    if (r->zeta_binding) {
        pgraph_d3d12_transition_raw_resource(
            r, r->zeta_binding->resource, &r->zeta_binding->state,
            D3D12_RESOURCE_STATE_DEPTH_WRITE);
        dsv_ptr = &r->zeta_binding->descriptor;
    }

    if (!rtv_ptr && !dsv_ptr) {
        pgraph_d3d12_warn_once(pg, PGRAPH_D3D12_DIAG_MISSING_SURFACE,
                               "draw has no bound color or zeta surface: "
                               "primitive_mode=%u, color_format=%s (0x%x), "
                               "zeta_format=%s (0x%x), surface_type=%s "
                               "(%u), color_offset=0x%" HWADDR_PRIx
                               ", color_pitch=%u, zeta_offset=0x%"
                               HWADDR_PRIx ", zeta_pitch=%u, "
                               "color_buffer_dirty=%d, zeta_buffer_dirty=%d, "
                               "color_draw_dirty=%d, zeta_draw_dirty=%d",
                               pg->primitive_mode,
                               color_surface_format_name(
                                   pg->surface_shape.color_format),
                               pg->surface_shape.color_format,
                               zeta_surface_format_name(
                                   pg->surface_shape.zeta_format),
                               pg->surface_shape.zeta_format,
                               surface_type_name(pg->surface_type),
                               pg->surface_type,
                               (hwaddr)pg->surface_color.offset,
                               pg->surface_color.pitch,
                               (hwaddr)pg->surface_zeta.offset,
                               pg->surface_zeta.pitch,
                               pg->surface_color.buffer_dirty,
                               pg->surface_zeta.buffer_dirty,
                               pg->surface_color.draw_dirty,
                               pg->surface_zeta.draw_dirty);
        return;
    }

    {
        /* Skip the redundant re-bind when both attachments are unchanged;
         * the transitions above still run every draw. */
        uint64_t rtv_id = rtv_ptr != NULL ? rtv_ptr->ptr : 0;
        uint64_t dsv_id = dsv_ptr != NULL ? dsv_ptr->ptr : 0;

        if (r->record_render_targets_valid && r->record_rtv_ptr == rtv_id &&
            r->record_dsv_ptr == dsv_id) {
            return;
        }
        r->record_rtv_ptr = rtv_id;
        r->record_dsv_ptr = dsv_id;
        r->record_render_targets_valid = true;
    }

    r->command_list->lpVtbl->OMSetRenderTargets(r->command_list,
                                                rtv_ptr ? 1 : 0, rtv_ptr,
                                                FALSE, dsv_ptr);
}

void pgraph_d3d12_clear_surface(NV2AState *d, uint32_t parameter)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    bool write_color = parameter & NV097_CLEAR_SURFACE_COLOR;
    bool write_zeta = parameter &
        (NV097_CLEAR_SURFACE_Z | NV097_CLEAR_SURFACE_STENCIL);
    bool cleared_color = false;
    bool cleared_zeta = false;
    bool command_clear = false;
    Error *local_err = NULL;

    if (r == NULL) {
        return;
    }

    nv2a_profile_inc_counter(NV2A_PROF_CLEAR);
    pg->clearing = true;
    pgraph_d3d12_end_zpass_query(d);
    pgraph_d3d12_surface_update_internal(d, true, write_color, write_zeta,
                                         &parameter);

    PGRAPHD3D12SurfaceBinding *binding = r->color_binding ?: r->zeta_binding;
    if (binding == NULL || (!binding->width && !binding->height)) {
        pg->clearing = false;
        return;
    }

    /* Clearing the whole rectangle keeps a uniform image uniform even when
     * only some components are written, such as depth without stencil. */
    bool color_was_uniform = r->color_binding && r->color_binding->cleared;
    bool zeta_was_uniform = r->zeta_binding && r->zeta_binding->cleared;

    uint32_t clearrectx = pgraph_reg_r(pg, NV_PGRAPH_CLEARRECTX);
    uint32_t clearrecty = pgraph_reg_r(pg, NV_PGRAPH_CLEARRECTY);
    unsigned int xmin = GET_MASK(clearrectx, NV_PGRAPH_CLEARRECTX_XMIN);
    unsigned int xmax = GET_MASK(clearrectx, NV_PGRAPH_CLEARRECTX_XMAX);
    unsigned int ymin = GET_MASK(clearrecty, NV_PGRAPH_CLEARRECTY_YMIN);
    unsigned int ymax = GET_MASK(clearrecty, NV_PGRAPH_CLEARRECTY_YMAX);
    /* Inclusive bounds in pre-AA pixels; a reversed pair clears one. */
    unsigned int xend = MAX(xmax, xmin) + 1;
    unsigned int yend = MAX(ymax, ymin) + 1;

    pgraph_apply_anti_aliasing_factor(pg, &xmin, &ymin);
    pgraph_apply_anti_aliasing_factor(pg, &xend, &yend);

    /* A rectangle starting outside the surface clears nothing; one that
     * extends past its edge is clipped. */
    if (xmin >= binding->width || ymin >= binding->height) {
        pg->clearing = false;
        return;
    }
    xend = MIN(xend, binding->width);
    yend = MIN(yend, binding->height);

    unsigned int scissor_width = xend - xmin;
    unsigned int scissor_height = yend - ymin;

    unsigned int cpu_xmin = xmin;
    unsigned int cpu_ymin = ymin;
    unsigned int cpu_width = scissor_width;
    unsigned int cpu_height = scissor_height;

    bool full_clear = !xmin && !ymin &&
        scissor_width >= pg->surface_binding_dim.width &&
        scissor_height >= pg->surface_binding_dim.height;

    pgraph_apply_scaling_factor(pg, &xmin, &ymin);
    pgraph_apply_scaling_factor(pg, &scissor_width, &scissor_height);

    D3D12_RECT rect = {
        .left = (LONG)xmin,
        .top = (LONG)ymin,
        .right = (LONG)(xmin + scissor_width),
        .bottom = (LONG)(ymin + scissor_height),
    };

    UINT8 color_write_mask = color_clear_write_mask(pg, parameter);
    ID3D12PipelineState *masked_pipeline = NULL;
    bool gpu_color_clear = false;

    if (write_color && r->color_binding && color_write_mask != 0) {
        bool cpu_clear = color_clear_needs_cpu(pg) ||
            (r->tune_flags & PGRAPH_D3D12_TUNE_MASKED_CLEAR_CPU);

        if (!cpu_clear && !color_clear_writes_all_channels(pg, parameter)) {
            masked_pipeline = pgraph_d3d12_get_masked_clear_pipeline(
                pg, r->color_binding->fmt.dxgi_format, color_write_mask,
                &local_err);
            if (masked_pipeline == NULL) {
                pgraph_d3d12_warn_once(
                    pg, PGRAPH_D3D12_DIAG_PARTIAL_COLOR_CLEAR,
                    "channel-masked color clears fall back to guest memory: "
                    "%s",
                    local_err != NULL ? error_get_pretty(local_err) :
                                        "unknown error");
                error_free(local_err);
                local_err = NULL;
                cpu_clear = true;
            }
        }
        pgraph_d3d12_capture_clear(pg, parameter,
                                   cpu_clear ? "cpu" :
                                   masked_pipeline ? "masked" : "full");
        if (cpu_clear) {
            if (!patch_color_clear_cpu(d, r->color_binding, parameter,
                                       cpu_xmin, cpu_ymin, cpu_width,
                                       cpu_height, &local_err)) {
                goto fail;
            }
            cleared_color = true;
        } else {
            gpu_color_clear = true;
            command_clear = true;
        }
    }

    if (write_zeta && r->zeta_binding) {
        command_clear = true;
    }

    if (command_clear && !pgraph_d3d12_begin_commands(pg, &local_err)) {
        goto fail;
    }

    if (gpu_color_clear) {
        float rgba[4];

        pgraph_get_clear_color(pg, rgba);
        pgraph_d3d12_evlog(r, "cclear c=0x%" HWADDR_PRIx
                           " rgba=%g,%g,%g,%g mask=%x masked=%d",
                           r->color_binding->vram_addr, rgba[0], rgba[1],
                           rgba[2], rgba[3], color_write_mask,
                           masked_pipeline != NULL);
        pgraph_d3d12_transition_raw_resource(
            r, r->color_binding->resource, &r->color_binding->state,
            D3D12_RESOURCE_STATE_RENDER_TARGET);
        pgraph_d3d12_set_surface_dirty(pg, true, false);
        if (masked_pipeline != NULL) {
            record_masked_color_clear(pg, r->color_binding, masked_pipeline,
                                      rgba, &rect);
        } else {
            r->command_list->lpVtbl->ClearRenderTargetView(
                r->command_list, r->color_binding->descriptor, rgba, 1,
                &rect);
        }
        cleared_color = true;
    }

    if (write_zeta && r->zeta_binding) {
        int stencil_value = 0;
        float depth_value = 1.0f;
        D3D12_CLEAR_FLAGS flags = 0;

        pgraph_get_clear_depth_stencil_value(pg, &depth_value,
                                             &stencil_value);
        if (parameter & NV097_CLEAR_SURFACE_Z) {
            flags |= D3D12_CLEAR_FLAG_DEPTH;
        }
        if ((parameter & NV097_CLEAR_SURFACE_STENCIL) &&
            r->zeta_binding->fmt.stencil) {
            flags |= D3D12_CLEAR_FLAG_STENCIL;
        }
        if (flags) {
            pgraph_d3d12_evlog(
                r, "zclear z=0x%" HWADDR_PRIx " %ux%u c=0x%" HWADDR_PRIx
                " flags=%u depth=%g rect=%ld,%ld-%ld,%ld full=%d",
                r->zeta_binding->vram_addr, r->zeta_binding->width,
                r->zeta_binding->height,
                r->color_binding ? r->color_binding->vram_addr : 0,
                (unsigned int)flags, depth_value, (long)rect.left,
                (long)rect.top, (long)rect.right, (long)rect.bottom,
                full_clear);
            pgraph_d3d12_transition_raw_resource(
                r, r->zeta_binding->resource, &r->zeta_binding->state,
                D3D12_RESOURCE_STATE_DEPTH_WRITE);
            pgraph_d3d12_set_surface_dirty(pg, false, true);
            r->command_list->lpVtbl->ClearDepthStencilView(
                r->command_list, r->zeta_binding->descriptor, flags,
                depth_value, (UINT8)stencil_value, 1, &rect);
            cleared_zeta = true;
        }
    }

    if (command_clear &&
        !account_surface_batch_command(pg, &local_err)) {
        goto fail;
    }

    if (cleared_color || cleared_zeta) {
        pg->draw_time++;
        if (r->color_binding && cleared_color) {
            r->color_binding->draw_time = pg->draw_time;
        }
        if (r->zeta_binding && cleared_zeta) {
            r->zeta_binding->draw_time = pg->draw_time;
        }
    }

    /* CPU colour clears patch guest memory, which the layout migration this
     * bit allows would discard. */
    if (r->color_binding && cleared_color) {
        r->color_binding->cleared =
            full_clear &&
            (clear_fully_overwrites_surface(pg, r->color_binding,
                                            parameter) ||
             (color_was_uniform && gpu_color_clear &&
              clear_rect_fully_covers_surface(pg, r->color_binding)));
        r->color_binding->initialized = true;
    }
    if (r->zeta_binding && cleared_zeta) {
        r->zeta_binding->cleared =
            full_clear &&
            (clear_fully_overwrites_surface(pg, r->zeta_binding,
                                            parameter) ||
             (zeta_was_uniform &&
              clear_rect_fully_covers_surface(pg, r->zeta_binding)));
        r->zeta_binding->initialized = true;
    }
    pg->clearing = false;
    return;

fail:
    fprintf(stderr,
            "Direct3D 12 renderer: clear_surface failed: parameter=0x%08x, "
            "write_color=%d, write_zeta=%d, color_bound=%d, zeta_bound=%d, "
            "color_format=%s (0x%x), zeta_format=%s (0x%x): %s\n",
            parameter, write_color, write_zeta, r->color_binding != NULL,
            r->zeta_binding != NULL,
            color_surface_format_name(pg->surface_shape.color_format),
            pg->surface_shape.color_format,
            zeta_surface_format_name(pg->surface_shape.zeta_format),
            pg->surface_shape.zeta_format,
            local_err ? error_get_pretty(local_err) : "unknown error");
    if (local_err) {
        error_free(local_err);
    }
    pg->clearing = false;
}
