/*
 * Geforce NV2A PGRAPH Direct3D 12 Renderer
 *
 * Copyright (c) 2026
 *
 * Based on the OpenGL and Vulkan CPU image blit paths.
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
#include "hw/display/vga_int.h"
#include "renderer.h"

static const char *blit_color_format_name(unsigned int format)
{
    switch (format) {
    case NV062_SET_COLOR_FORMAT_LE_Y8:
        return "LE_Y8";
    case NV062_SET_COLOR_FORMAT_LE_R5G6B5:
        return "LE_R5G6B5";
    case NV062_SET_COLOR_FORMAT_LE_A8R8G8B8:
        return "LE_A8R8G8B8";
    case NV062_SET_COLOR_FORMAT_LE_X8R8G8B8:
        return "LE_X8R8G8B8";
    case NV062_SET_COLOR_FORMAT_LE_X8R8G8B8_Z8R8G8B8:
        return "LE_X8R8G8B8_Z8R8G8B8";
    case NV062_SET_COLOR_FORMAT_LE_Y32:
        return "LE_Y32";
    default:
        return "unknown";
    }
}

static const char *blit_operation_name(unsigned int operation)
{
    switch (operation) {
    case NV09F_SET_OPERATION_SRCCOPY:
        return "SRCCOPY";
    case NV09F_SET_OPERATION_BLEND_AND:
        return "BLEND_AND";
    default:
        return "unknown";
    }
}

static void report_image_blit_state(NV2AState *d, const char *reason)
{
    PGRAPHState *pg = &d->pgraph;
    ContextSurfaces2DState *context_surfaces = &pg->context_surfaces_2d;
    ImageBlitState *image_blit = &pg->image_blit;

    fprintf(stderr,
            "Direct3D 12 renderer: image_blit skipped: %s. "
            "operation=%s (0x%x), color_format=%s (0x%x), "
            "source_dma=0x%" HWADDR_PRIx ", source_offset=0x%"
            HWADDR_PRIx ", source_pitch=%u, dest_dma=0x%" HWADDR_PRIx
            ", dest_offset=0x%" HWADDR_PRIx
            ", dest_pitch=%u, in=%u,%u, out=%u,%u, size=%ux%u, "
            "context_surfaces=0x%" HWADDR_PRIx
            ", image_blit_context=0x%" HWADDR_PRIx "\n",
            reason, blit_operation_name(image_blit->operation),
            image_blit->operation,
            blit_color_format_name(context_surfaces->color_format),
            context_surfaces->color_format,
            context_surfaces->dma_image_source,
            (hwaddr)context_surfaces->source_offset,
            context_surfaces->source_pitch,
            context_surfaces->dma_image_dest,
            (hwaddr)context_surfaces->dest_offset,
            context_surfaces->dest_pitch, image_blit->in_x,
            image_blit->in_y, image_blit->out_x, image_blit->out_y,
            image_blit->width, image_blit->height,
            context_surfaces->object_instance, image_blit->context_surfaces);
}

static bool should_report_masked(uint64_t *reported, unsigned int value)
{
    uint64_t bit;

    if (value >= 64) {
        return true;
    }

    bit = 1ull << value;
    if (*reported & bit) {
        return false;
    }

    *reported |= bit;
    return true;
}

static bool get_blit_bytes_per_pixel(NV2AState *d, unsigned int *bpp)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    unsigned int color_format = pg->context_surfaces_2d.color_format;

    switch (color_format) {
    case NV062_SET_COLOR_FORMAT_LE_Y8:
        *bpp = 1;
        return true;
    case NV062_SET_COLOR_FORMAT_LE_R5G6B5:
        *bpp = 2;
        return true;
    case NV062_SET_COLOR_FORMAT_LE_A8R8G8B8:
    case NV062_SET_COLOR_FORMAT_LE_X8R8G8B8:
    case NV062_SET_COLOR_FORMAT_LE_X8R8G8B8_Z8R8G8B8:
    case NV062_SET_COLOR_FORMAT_LE_Y32:
        *bpp = 4;
        return true;
    default:
        if (should_report_masked(&r->reported_unsupported_blit_color_formats,
                                 color_format)) {
            report_image_blit_state(d, "unsupported 2D surface color format");
        }
        return false;
    }
}

static bool blit_display_surface_format(unsigned int blit_format,
                                        uint32_t *surface_format)
{
    switch (blit_format) {
    case NV062_SET_COLOR_FORMAT_LE_Y8:
        *surface_format = NV097_SET_SURFACE_FORMAT_COLOR_LE_B8;
        return true;
    case NV062_SET_COLOR_FORMAT_LE_R5G6B5:
        *surface_format = NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5;
        return true;
    case NV062_SET_COLOR_FORMAT_LE_A8R8G8B8:
    case NV062_SET_COLOR_FORMAT_LE_X8R8G8B8:
        *surface_format = NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8;
        return true;
    case NV062_SET_COLOR_FORMAT_LE_X8R8G8B8_Z8R8G8B8:
        *surface_format =
            NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_Z8R8G8B8;
        return true;
    default:
        return false;
    }
}

/* dma_limit is the offset of the DMA object's last byte. */
static bool validate_dma_base(NV2AState *d, const char *name, hwaddr offset,
                              hwaddr dma_limit)
{
    g_autofree char *reason = NULL;

    if (offset >= dma_limit) {
        reason = g_strdup_printf(
            "%s base offset 0x%" HWADDR_PRIx
            " is outside DMA limit 0x%" HWADDR_PRIx,
            name, offset, dma_limit);
        report_image_blit_state(d, reason);
        return false;
    }

    return true;
}

static bool blit_addr_in_range(hwaddr addr, hwaddr start, uint64_t size)
{
    return size > 0 && addr >= start && (uint64_t)(addr - start) < size;
}

static bool blit_ranges_overlap(hwaddr a, uint64_t a_size, hwaddr b,
                                uint64_t b_size)
{
    uint64_t a_start = a;
    uint64_t b_start = b;
    uint64_t a_end;
    uint64_t b_end;

    if (a_size == 0 || b_size == 0) {
        return false;
    }

    a_end = UINT64_MAX - a_start < a_size ? UINT64_MAX : a_start + a_size;
    b_end = UINT64_MAX - b_start < b_size ? UINT64_MAX : b_start + b_size;

    return a_start < b_end && b_start < a_end;
}

static bool blit_surface_overlaps_range(
    const PGRAPHD3D12SurfaceBinding *surface, hwaddr range_start,
    uint64_t range_len)
{
    uint64_t range_end;
    uint64_t surface_end;
    uint64_t surface_span;
    uint64_t row_bytes;
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

/* Bytes from the first blit row to the last byte the blit touches. */
static uint64_t blit_rows_span(uint32_t pitch, uint64_t row_bytes,
                               size_t full_rows, uint64_t tail_bytes)
{
    if (tail_bytes > 0) {
        return (uint64_t)full_rows * pitch + tail_bytes;
    }
    return full_rows > 0 ? (uint64_t)(full_rows - 1) * pitch + row_bytes : 0;
}

static bool blit_surface_overlaps_rows(
    const PGRAPHD3D12SurfaceBinding *surface, hwaddr row_addr,
    uint32_t pitch, uint64_t row_bytes, size_t full_rows,
    uint64_t tail_bytes)
{
    uint64_t span = blit_rows_span(pitch, row_bytes, full_rows, tail_bytes);

    /* Most surfaces are nowhere near the blit; skip the per-row test. */
    if (span == 0 || surface->vram_addr >= row_addr + span ||
        row_addr >= surface->vram_addr + surface->size) {
        return false;
    }

    if (pitch != 0 && row_bytes == pitch) {
        uint64_t size = (uint64_t)full_rows * pitch + tail_bytes;

        return blit_surface_overlaps_range(surface, row_addr, size);
    }

    for (size_t y = 0; y < full_rows; y++) {
        if (blit_surface_overlaps_range(
                surface, row_addr + (uint64_t)y * pitch, row_bytes)) {
            return true;
        }
    }

    return tail_bytes > 0 &&
           blit_surface_overlaps_range(
               surface, row_addr + (uint64_t)full_rows * pitch,
               tail_bytes);
}

static bool download_dirty_surfaces_overlapping_blit_rows(
    NV2AState *d, hwaddr row_addr, uint32_t pitch, uint64_t row_bytes,
    size_t full_rows, uint64_t tail_bytes, const char *purpose)
{
    PGRAPHD3D12State *r = d->pgraph.d3d12_renderer_state;
    PGRAPHD3D12SurfaceBinding *surface;

    QTAILQ_FOREACH(surface, &r->surfaces, entry) {
        Error *local_err = NULL;

        if (!blit_surface_overlaps_rows(surface, row_addr, pitch, row_bytes,
                                        full_rows, tail_bytes)) {
            continue;
        }
        /* A failed write-back is reported and counted like any other; the
         * blit still runs on the guest bytes, as the other backends do. */
        if (!pgraph_d3d12_surface_download_if_dirty_checked_reason(
                d, surface, PGRAPH_D3D12_SURFACE_DOWNLOAD_BLIT_OVERLAP,
                &local_err)) {
            fprintf(stderr,
                    "Direct3D 12 renderer: could not preserve %s surface "
                    "addr=0x%" HWADDR_PRIx " for CPU image-blit %s: %s\n",
                    surface->color ? "color" : "zeta", surface->vram_addr,
                    purpose,
                    local_err ? error_get_pretty(local_err) :
                                "unknown error");
            error_free(local_err);
            pgraph_d3d12_surface_download_if_dirty(d, surface);
        }
    }

    return true;
}

static bool blit_fully_overwrites_surface(
    const PGRAPHD3D12SurfaceBinding *surface, hwaddr dest_row_addr,
    uint32_t dest_pitch, uint64_t row_bytes, size_t full_rows,
    uint64_t tail_bytes, uint32_t operation)
{
    uint64_t surface_row_bytes;

    if (operation != NV09F_SET_OPERATION_SRCCOPY || surface->swizzle ||
        dest_row_addr != surface->vram_addr ||
        dest_pitch != surface->pitch || surface->height == 0) {
        return false;
    }

    surface_row_bytes =
        (uint64_t)surface->width * surface->fmt.bytes_per_pixel;
    if (row_bytes < surface_row_bytes) {
        return false;
    }

    if (full_rows >= surface->height) {
        return true;
    }

    return full_rows + 1 == surface->height &&
           tail_bytes >= surface_row_bytes;
}

static bool prepare_destination_surfaces_for_blit(
    NV2AState *d, hwaddr dest_row_addr, uint32_t dest_pitch,
    uint64_t row_bytes, size_t full_rows, uint64_t tail_bytes,
    uint32_t operation)
{
    PGRAPHD3D12State *r = d->pgraph.d3d12_renderer_state;
    PGRAPHD3D12SurfaceBinding *surface;

    QTAILQ_FOREACH(surface, &r->surfaces, entry) {
        Error *local_err = NULL;
        bool full_overwrite;

        if (!blit_surface_overlaps_rows(surface, dest_row_addr, dest_pitch,
                                        row_bytes, full_rows, tail_bytes)) {
            continue;
        }

        full_overwrite = blit_fully_overwrites_surface(
            surface, dest_row_addr, dest_pitch, row_bytes, full_rows,
            tail_bytes, operation);
        if (!full_overwrite &&
            !pgraph_d3d12_surface_download_if_dirty_checked_reason(
                d, surface, PGRAPH_D3D12_SURFACE_DOWNLOAD_BLIT_OVERLAP,
                &local_err)) {
            fprintf(stderr,
                    "Direct3D 12 renderer: could not preserve %s surface "
                    "addr=0x%" HWADDR_PRIx
                    " before CPU image-blit destination write: %s\n",
                    surface->color ? "color" : "zeta", surface->vram_addr,
                    local_err ? error_get_pretty(local_err) :
                                "unknown error");
            error_free(local_err);
            pgraph_d3d12_surface_download_if_dirty(d, surface);
        }

        if (full_overwrite) {
            surface->download_pending = false;
            pgraph_d3d12_surface_mark_cpu_owned(surface);
        }
    }

    return true;
}

static void invalidate_blit_vertex_ram_range(PGRAPHD3D12State *r,
                                             hwaddr addr, uint64_t size)
{
    uint64_t end;
    size_t first_page;
    size_t end_page;

    if (r == NULL || r->uploaded_bitmap == NULL || r->bitmap_size == 0 ||
        size == 0) {
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

static void mark_blit_destination_range_dirty(NV2AState *d, hwaddr addr,
                                              uint64_t size)
{
    PGRAPHD3D12State *r = d->pgraph.d3d12_renderer_state;

    if (size == 0) {
        return;
    }

    memory_region_set_client_dirty(d->vram, addr, size, DIRTY_MEMORY_VGA);
    memory_region_set_client_dirty(d->vram, addr, size,
                                   DIRTY_MEMORY_NV2A_TEX);
    memory_region_set_client_dirty(d->vram, addr, size, DIRTY_MEMORY_NV2A);
    invalidate_blit_vertex_ram_range(r, addr, size);
    pgraph_d3d12_mark_textures_possibly_dirty(d, addr, size);
    if (r != NULL) {
        pgraph_d3d12_invalidate_blit_display_candidates(r, addr, size);
    }
}

static void mark_destination_surfaces_after_blit(
    NV2AState *d, hwaddr dest_row_addr, uint32_t dest_pitch,
    uint64_t row_bytes, size_t full_rows, uint64_t tail_bytes)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    PGRAPHD3D12SurfaceBinding *surface;
    bool touched = false;

    QTAILQ_FOREACH(surface, &r->surfaces, entry) {
        if (!blit_surface_overlaps_rows(surface, dest_row_addr, dest_pitch,
                                        row_bytes, full_rows, tail_bytes)) {
            continue;
        }

        surface->download_pending = false;
        surface->upload_pending = true;
        surface->frame_time = pg->frame_time;
        surface->write_frame_time = pg->frame_time;
        pgraph_d3d12_surface_mark_cpu_owned(surface);
        touched = true;
    }

    if (touched) {
        pg->draw_time++;
    }
}

void pgraph_d3d12_invalidate_blit_display_candidates(PGRAPHD3D12State *r,
                                                     hwaddr addr,
                                                     uint64_t size)
{
    if (r == NULL || size == 0) {
        return;
    }

    for (uint32_t i = 0; i < PGRAPH_D3D12_BLIT_DISPLAY_CANDIDATES; i++) {
        PGRAPHD3D12BlitDisplayCandidate *candidate =
            &r->blit_display_candidates[i];

        if (candidate->valid &&
            blit_ranges_overlap(addr, size, candidate->addr,
                                candidate->size)) {
            candidate->valid = false;
        }
    }

    if (r->blit_display_valid &&
        blit_ranges_overlap(addr, size, r->blit_display_addr,
                            r->blit_display_size)) {
        r->blit_display_valid = false;
        r->blit_display_size = 0;
    }
}

static bool should_report_successful_image_blit(
    PGRAPHD3D12State *r, hwaddr source_addr, hwaddr dest_addr,
    uint64_t dest_size, uint32_t operation, uint32_t color_format,
    bool important)
{
    uint32_t reported =
        MIN(r->num_reported_successful_image_blits,
            (uint32_t)PGRAPH_D3D12_REPORTED_IMAGE_BLITS);

    for (uint32_t i = 0; i < reported; i++) {
        PGRAPHD3D12ImageBlitReportKey *key =
            &r->reported_successful_image_blits[i];

        if (key->source_addr == source_addr &&
            key->dest_addr == dest_addr && key->dest_size == dest_size &&
            key->operation == operation &&
            key->color_format == color_format) {
            return false;
        }
    }

    if (reported >= PGRAPH_D3D12_REPORTED_IMAGE_BLITS && !important) {
        return false;
    }

    uint32_t slot = r->num_reported_successful_image_blits %
                    PGRAPH_D3D12_REPORTED_IMAGE_BLITS;
    r->reported_successful_image_blits[slot] =
        (PGRAPHD3D12ImageBlitReportKey) {
            .source_addr = source_addr,
            .dest_addr = dest_addr,
            .dest_size = dest_size,
            .operation = operation,
            .color_format = color_format,
        };
    r->num_reported_successful_image_blits++;
    return true;
}

static PGRAPHD3D12BlitDisplayCandidate *remember_blit_display_candidate(
    PGRAPHD3D12State *r, hwaddr dest_addr, uint64_t clipped_dest_size,
    size_t row_pixels, size_t adjusted_height, size_t leftover_bytes,
    uint32_t dest_pitch, unsigned int bytes_per_pixel, uint32_t color_format,
    uint32_t operation, int draw_time)
{
    uint32_t slot;
    PGRAPHD3D12BlitDisplayCandidate *candidate;

    slot = r->blit_display_next_candidate %
           PGRAPH_D3D12_BLIT_DISPLAY_CANDIDATES;
    r->blit_display_next_candidate++;
    candidate = &r->blit_display_candidates[slot];
    *candidate = (PGRAPHD3D12BlitDisplayCandidate) {
        .valid = true,
        .addr = dest_addr,
        .size = clipped_dest_size,
        .width = (uint32_t)row_pixels,
        .height = (uint32_t)(adjusted_height + (leftover_bytes > 0 ? 1 : 0)),
        .pitch = dest_pitch,
        .bytes_per_pixel = bytes_per_pixel,
        .color_format = color_format,
        .operation = operation,
        .draw_time = draw_time,
        .sequence = ++r->blit_display_sequence,
    };

    r->blit_display_valid = true;
    r->blit_display_addr = candidate->addr;
    r->blit_display_size = candidate->size;
    r->blit_display_width = candidate->width;
    r->blit_display_height = candidate->height;
    r->blit_display_pitch = candidate->pitch;
    r->blit_display_bytes_per_pixel = candidate->bytes_per_pixel;
    r->blit_display_color_format = candidate->color_format;
    r->blit_display_draw_time = candidate->draw_time;

    return candidate;
}

static void report_successful_image_blit(
    NV2AState *d, hwaddr source_addr, uint64_t source_size,
    hwaddr dest_addr, uint64_t dest_size, uint64_t clipped_dest_size,
    size_t row_pixels, size_t row_bytes, size_t adjusted_height,
    size_t leftover_bytes, unsigned int bytes_per_pixel,
    const PGRAPHD3D12BlitDisplayCandidate *candidate)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    ContextSurfaces2DState *context_surfaces = &pg->context_surfaces_2d;
    ImageBlitState *image_blit = &pg->image_blit;
    VGADisplayParams vga_display_params;
    hwaddr display_addr;
    bool covers_display;
    bool covers_0xa00;
    bool important;

    if (r == NULL) {
        return;
    }
    if (!pgraph_d3d12_verbose_reports(pg)) {
        return;
    }

    d->vga.get_params(&d->vga, &vga_display_params);
    display_addr = d->pcrtc.start + vga_display_params.line_offset;
    covers_display =
        blit_addr_in_range(display_addr, dest_addr, clipped_dest_size);
    covers_0xa00 = blit_addr_in_range(0xa00, dest_addr, clipped_dest_size);
    important = covers_display || covers_0xa00;

    if (!should_report_successful_image_blit(
            r, source_addr, dest_addr, clipped_dest_size,
            image_blit->operation, context_surfaces->color_format,
            important)) {
        return;
    }

    fprintf(stderr,
            "Direct3D 12 renderer: image_blit completed: "
            "operation=%s (0x%x), color_format=%s (0x%x), "
            "source=0x%" HWADDR_PRIx "..0x%" HWADDR_PRIx
            " source_size=0x%" PRIx64
            " source_dma=0x%" HWADDR_PRIx
            " source_base_offset=0x%" HWADDR_PRIx
            " source_row_offset=0x%" HWADDR_PRIx
            " source_pitch=%u, dest=0x%" HWADDR_PRIx "..0x%"
            HWADDR_PRIx " dest_requested_size=0x%" PRIx64
            " dest_clipped_size=0x%" PRIx64
            " dest_dma=0x%" HWADDR_PRIx
            " dest_base_offset=0x%" HWADDR_PRIx
            " dest_row_offset=0x%" HWADDR_PRIx
            " dest_pitch=%u, in=%u,%u, out=%u,%u, requested_size=%ux%u, "
            "copied_row_pixels=%zu, copied_row_bytes=%zu, copied_height=%zu, "
            "leftover_bytes=%zu, bytes_per_pixel=%u, beta=0x%x, "
            "pcrtc.start=0x%" HWADDR_PRIx ", vga.line_offset=%u, "
            "display_addr=0x%" HWADDR_PRIx ", covers_display=%d, "
            "covers_0xa00=%d, candidate_sequence=%u, draw_time=%d\n",
            blit_operation_name(image_blit->operation), image_blit->operation,
            blit_color_format_name(context_surfaces->color_format),
            context_surfaces->color_format, source_addr,
            source_addr + source_size, source_size,
            context_surfaces->dma_image_source,
            (hwaddr)context_surfaces->source_offset,
            (hwaddr)(image_blit->in_y * context_surfaces->source_pitch +
                     image_blit->in_x * bytes_per_pixel),
            context_surfaces->source_pitch, dest_addr,
            dest_addr + clipped_dest_size, dest_size, clipped_dest_size,
            context_surfaces->dma_image_dest,
            (hwaddr)context_surfaces->dest_offset,
            (hwaddr)(image_blit->out_y * context_surfaces->dest_pitch +
                     image_blit->out_x * bytes_per_pixel),
            context_surfaces->dest_pitch, image_blit->in_x,
            image_blit->in_y, image_blit->out_x, image_blit->out_y,
            image_blit->width, image_blit->height, row_pixels, row_bytes,
            adjusted_height, leftover_bytes, bytes_per_pixel,
            pg->beta.beta, d->pcrtc.start, vga_display_params.line_offset,
            display_addr, covers_display, covers_0xa00,
            candidate ? candidate->sequence : 0,
            candidate ? candidate->draw_time : pg->draw_time);
}

static uint32_t blend_component(uint32_t source, uint32_t dest,
                                uint32_t beta_mult,
                                uint32_t inv_beta_mult)
{
    return (source * beta_mult + dest * inv_beta_mult) / 0x7f80;
}

static bool perform_blit(NV2AState *d, uint8_t *source, uint8_t *dest,
                         size_t width, size_t height, size_t width_bytes,
                         size_t source_pitch, size_t dest_pitch,
                         unsigned int bytes_per_pixel)
{
    PGRAPHState *pg = &d->pgraph;
    ImageBlitState *image_blit = &pg->image_blit;
    BetaState *beta = &pg->beta;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    unsigned int operation = image_blit->operation;
    /* A destination starting inside the source after it would read rows
     * this blit already wrote; walk the rows bottom-up then. Within a row
     * memmove, and the blend's reverse pixel order, handle the overlap. */
    bool backwards = height > 0 && dest > source &&
                     dest < source + (height - 1) * source_pitch + width_bytes;
    ptrdiff_t source_step = backwards ? -(ptrdiff_t)source_pitch :
                                        (ptrdiff_t)source_pitch;
    ptrdiff_t dest_step = backwards ? -(ptrdiff_t)dest_pitch :
                                      (ptrdiff_t)dest_pitch;

    if (backwards) {
        source += (height - 1) * source_pitch;
        dest += (height - 1) * dest_pitch;
    }

    if (operation == NV09F_SET_OPERATION_SRCCOPY) {
        for (size_t y = 0; y < height; y++) {
            memmove(dest, source, width_bytes);
            source += source_step;
            dest += dest_step;
        }
        return true;
    }

    if (operation == NV09F_SET_OPERATION_BLEND_AND) {
        uint32_t max_beta_mult = 0x7f80;
        uint32_t beta_mult = beta->beta >> 16;
        uint32_t inv_beta_mult = max_beta_mult - beta_mult;

        if (bytes_per_pixel == 1) {
            for (size_t y = 0; y < height; y++) {
                uint8_t *s = source;
                uint8_t *dst = dest;

                for (size_t i = 0; i < width; i++) {
                    size_t x = backwards ? width - 1 - i : i;

                    dst[x] = blend_component(s[x], dst[x], beta_mult,
                                             inv_beta_mult);
                }

                source += source_step;
                dest += dest_step;
            }
            return true;
        }

        if (bytes_per_pixel == 2) {
            for (size_t y = 0; y < height; y++) {
                uint8_t *s = source;
                uint8_t *dst = dest;

                for (size_t i = 0; i < width; i++) {
                    size_t x = backwards ? width - 1 - i : i;
                    uint16_t src_pixel = lduw_le_p(s + x * 2);
                    uint16_t dst_pixel = lduw_le_p(dst + x * 2);
                    uint32_t src_r = (src_pixel >> 11) & 0x1f;
                    uint32_t src_g = (src_pixel >> 5) & 0x3f;
                    uint32_t src_b = src_pixel & 0x1f;
                    uint32_t dst_r = (dst_pixel >> 11) & 0x1f;
                    uint32_t dst_g = (dst_pixel >> 5) & 0x3f;
                    uint32_t dst_b = dst_pixel & 0x1f;
                    uint32_t out_r = blend_component(src_r, dst_r, beta_mult,
                                                     inv_beta_mult);
                    uint32_t out_g = blend_component(src_g, dst_g, beta_mult,
                                                     inv_beta_mult);
                    uint32_t out_b = blend_component(src_b, dst_b, beta_mult,
                                                     inv_beta_mult);

                    stw_le_p(dst + x * 2,
                             (out_r << 11) | (out_g << 5) | out_b);
                }

                source += source_step;
                dest += dest_step;
            }
            return true;
        }

        for (size_t y = 0; y < height; y++) {
            uint8_t *s = source;
            uint8_t *dst = dest;

            for (size_t i = 0; i < width; i++) {
                size_t x = backwards ? width - 1 - i : i;

                for (unsigned int ch = 0; ch < 3; ch++) {
                    uint32_t a = s[x * 4 + ch] * beta_mult;
                    uint32_t b = dst[x * 4 + ch] * inv_beta_mult;
                    dst[x * 4 + ch] = (a + b) / max_beta_mult;
                }
            }

            source += source_step;
            dest += dest_step;
        }
        return true;
    }

    if (should_report_masked(&r->reported_unsupported_blit_operations,
                             operation)) {
        report_image_blit_state(d, "unsupported 2D blit operation");
    }
    return false;
}

static void patch_alpha(uint8_t *dest, size_t width_pixels, size_t height,
                        size_t dest_pitch, uint8_t alpha_val)
{
    for (size_t y = 0; y < height; y++) {
        uint8_t *d = dest;

        for (size_t x = 0; x < width_pixels; x++) {
            d[x * 4 + 3] = alpha_val;
        }

        dest += dest_pitch;
    }
}

/*
 * Trims a blit to the rows, and whole pixels of a last partial row, that end
 * within `avail` bytes of its first row. Returns true if anything was cut.
 */
static bool clip_blit_rows(uint64_t avail, uint32_t pitch, size_t row_bytes,
                           unsigned int bytes_per_pixel, size_t *rows,
                           size_t *tail_bytes)
{
    size_t full_rows = *rows;
    uint64_t tail_start;
    uint64_t tail;

    if (blit_rows_span(pitch, row_bytes, *rows, *tail_bytes) <= avail) {
        return false;
    }

    if (avail < row_bytes) {
        full_rows = 0;
    } else {
        full_rows = MIN(full_rows, (size_t)((avail - row_bytes) / pitch + 1));
    }

    /* The new last row is either a cut full row or the cut partial row. */
    tail_start = (uint64_t)full_rows * pitch;
    tail = avail > tail_start ? avail - tail_start : 0;
    tail = MIN(tail, full_rows < *rows ? (uint64_t)row_bytes :
                                         (uint64_t)*tail_bytes);
    *rows = full_rows;
    *tail_bytes = (size_t)(tail - tail % bytes_per_pixel);
    return true;
}

void pgraph_d3d12_image_blit(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    ContextSurfaces2DState *context_surfaces = &pg->context_surfaces_2d;
    ImageBlitState *image_blit = &pg->image_blit;
    unsigned int bytes_per_pixel;
    hwaddr source_dma_len;
    hwaddr dest_dma_len;
    uint8_t *source;
    uint8_t *dest;
    hwaddr source_addr;
    hwaddr dest_addr;
    hwaddr source_offset;
    hwaddr dest_offset;
    size_t max_row_pixels;
    size_t row_pixels;
    hwaddr source_size;
    hwaddr dest_size;
    uint8_t *source_row;
    uint8_t *dest_row;
    size_t row_bytes;
    size_t adjusted_height;
    size_t leftover_bytes = 0;
    hwaddr clipped_dest_size;

    pgraph_d3d12_surface_update(d, false, true, true);

    if (context_surfaces->object_instance != image_blit->context_surfaces) {
        report_image_blit_state(d, "context surface object mismatch");
        return;
    }

    if (!get_blit_bytes_per_pixel(d, &bytes_per_pixel)) {
        return;
    }

    source = (uint8_t *)nv_dma_map(d, context_surfaces->dma_image_source,
                                   &source_dma_len);
    dest = (uint8_t *)nv_dma_map(d, context_surfaces->dma_image_dest,
                                 &dest_dma_len);

    source_offset = image_blit->in_y * context_surfaces->source_pitch +
                    image_blit->in_x * bytes_per_pixel;
    dest_offset = image_blit->out_y * context_surfaces->dest_pitch +
                  image_blit->out_x * bytes_per_pixel;

    max_row_pixels =
        MIN(context_surfaces->source_pitch, context_surfaces->dest_pitch) /
        bytes_per_pixel;
    row_pixels = MIN(max_row_pixels, image_blit->width);
    row_bytes = row_pixels * bytes_per_pixel;
    if (row_bytes == 0 || image_blit->height == 0) {
        return;
    }

    source_size =
        (uint64_t)(image_blit->height - 1) * context_surfaces->source_pitch +
        row_bytes;
    dest_size =
        (uint64_t)(image_blit->height - 1) * context_surfaces->dest_pitch +
        (uint64_t)image_blit->width * bytes_per_pixel;

    if (!validate_dma_base(d, "source", context_surfaces->source_offset,
                           source_dma_len) ||
        !validate_dma_base(d, "destination", context_surfaces->dest_offset,
                           dest_dma_len)) {
        return;
    }

    source += context_surfaces->source_offset;
    dest += context_surfaces->dest_offset;
    source_addr = source - d->vram_ptr;
    dest_addr = dest - d->vram_ptr;

    source_row = source + source_offset;
    dest_row = dest + dest_offset;
    adjusted_height = image_blit->height;

    clipped_dest_size =
        nv_clip_gpu_tile_blit(d, dest_addr + dest_offset, dest_size);

    if (clipped_dest_size < dest_size) {
        size_t consumed_bytes;

        adjusted_height = clipped_dest_size / context_surfaces->dest_pitch;
        consumed_bytes = adjusted_height * context_surfaces->dest_pitch;
        leftover_bytes = MIN(clipped_dest_size - consumed_bytes, row_bytes);
    }

    /* Nothing is written past the destination DMA object, whose limit is
     * its last byte, and nothing is accessed past guest memory. */
    {
        uint64_t vram_size = memory_region_size(d->vram);
        uint64_t dest_start = (uint64_t)dest_addr + dest_offset;
        uint64_t source_start = (uint64_t)source_addr + source_offset;
        uint64_t dest_dma_start =
            (uint64_t)context_surfaces->dest_offset + dest_offset;
        uint64_t dest_avail =
            dest_dma_start <= dest_dma_len ?
                (uint64_t)dest_dma_len + 1 - dest_dma_start : 0;
        uint64_t source_avail =
            source_start < vram_size ? vram_size - source_start : 0;
        bool clipped;

        if (dest_start >= vram_size) {
            dest_avail = 0;
        } else {
            dest_avail = MIN(dest_avail, vram_size - dest_start);
        }

        clipped = clip_blit_rows(dest_avail, context_surfaces->dest_pitch,
                                 row_bytes, bytes_per_pixel,
                                 &adjusted_height, &leftover_bytes);
        clipped |= clip_blit_rows(source_avail,
                                  context_surfaces->source_pitch, row_bytes,
                                  bytes_per_pixel, &adjusted_height,
                                  &leftover_bytes);
        if (clipped) {
            if (adjusted_height == 0 && leftover_bytes == 0) {
                report_image_blit_state(
                    d, "range is outside the DMA object or guest memory");
                return;
            }
            clipped_dest_size = blit_rows_span(
                context_surfaces->dest_pitch, row_bytes, adjusted_height,
                leftover_bytes);
        }
    }

    if (image_blit->operation != NV09F_SET_OPERATION_SRCCOPY &&
        image_blit->operation != NV09F_SET_OPERATION_BLEND_AND) {
        if (should_report_masked(&r->reported_unsupported_blit_operations,
                                 image_blit->operation)) {
            report_image_blit_state(d, "unsupported 2D blit operation");
        }
        return;
    }

    {
        uint64_t source_span = blit_rows_span(
            context_surfaces->source_pitch, row_bytes, adjusted_height,
            leftover_bytes);
        uint64_t dest_span = blit_rows_span(
            context_surfaces->dest_pitch, row_bytes, adjusted_height,
            leftover_bytes);

        /* Source bytes land first; a stash the copy then replaces
         * completely needs no write-back. */
        pgraph_d3d12_complete_readbacks_in_range(
            d, source_addr + source_offset, source_span,
            PGRAPH_D3D12_STASH_TRIGGER_BLIT);
        if (image_blit->operation == NV09F_SET_OPERATION_SRCCOPY &&
            context_surfaces->dest_pitch == row_bytes) {
            pgraph_d3d12_drop_readbacks_within(d, dest_addr + dest_offset,
                                               dest_span);
        }
        pgraph_d3d12_complete_readbacks_in_range(
            d, dest_addr + dest_offset, dest_span,
            PGRAPH_D3D12_STASH_TRIGGER_BLIT);
    }
    if (!download_dirty_surfaces_overlapping_blit_rows(
            d, source_addr + source_offset,
            context_surfaces->source_pitch, row_bytes, adjusted_height,
            leftover_bytes, "source read") ||
        !prepare_destination_surfaces_for_blit(
            d, dest_addr + dest_offset, context_surfaces->dest_pitch,
            row_bytes, adjusted_height, leftover_bytes,
            image_blit->operation)) {
        return;
    }

    NV2A_DPRINTF("  d3d12 blit 0x%" HWADDR_PRIx " -> 0x%" HWADDR_PRIx
                 " (Size: %" PRIu64
                 ", Clipped Height: %zu)\n",
                 source_addr, dest_addr, (uint64_t)dest_size,
                 adjusted_height);

    if (adjusted_height > 0 &&
        !perform_blit(d, source_row, dest_row, row_pixels, adjusted_height,
                      row_bytes, context_surfaces->source_pitch,
                      context_surfaces->dest_pitch, bytes_per_pixel)) {
        return;
    }

    if (leftover_bytes > 0) {
        uint8_t *src =
            source_row + adjusted_height * context_surfaces->source_pitch;
        uint8_t *dst =
            dest_row + adjusted_height * context_surfaces->dest_pitch;

        if (!perform_blit(d, src, dst, leftover_bytes / bytes_per_pixel, 1,
                          leftover_bytes, context_surfaces->source_pitch,
                          context_surfaces->dest_pitch, bytes_per_pixel)) {
            return;
        }
    }

    if (context_surfaces->color_format == NV062_SET_COLOR_FORMAT_LE_X8R8G8B8) {
        if (adjusted_height > 0) {
            patch_alpha(dest_row, row_pixels, adjusted_height,
                        context_surfaces->dest_pitch, 0xff);
        }
        if (leftover_bytes > 0) {
            uint8_t *dst =
                dest_row + adjusted_height * context_surfaces->dest_pitch;
            patch_alpha(dst, leftover_bytes / 4, 1, 0, 0xff);
        }
    } else if (context_surfaces->color_format ==
               NV062_SET_COLOR_FORMAT_LE_X8R8G8B8_Z8R8G8B8) {
        if (adjusted_height > 0) {
            patch_alpha(dest_row, row_pixels, adjusted_height,
                        context_surfaces->dest_pitch, 0);
        }
        if (leftover_bytes > 0) {
            uint8_t *dst =
                dest_row + adjusted_height * context_surfaces->dest_pitch;
            patch_alpha(dst, leftover_bytes / 4, 1, 0, 0);
        }
    }

    dest_addr += dest_offset;
    /*
     * Dirty tracking is page-granular.  Mark one conservative bounding span
     * instead of walking every texture-cache entry once per image row.
     */
    uint64_t dirty_span = leftover_bytes > 0 ?
        (uint64_t)adjusted_height * context_surfaces->dest_pitch +
            leftover_bytes :
        adjusted_height > 0 ?
            (uint64_t)(adjusted_height - 1) *
                context_surfaces->dest_pitch + row_bytes : 0;
    mark_blit_destination_range_dirty(d, dest_addr, dirty_span);
    mark_destination_surfaces_after_blit(
        d, dest_addr, context_surfaces->dest_pitch, row_bytes,
        adjusted_height, leftover_bytes);

    PGRAPHD3D12BlitDisplayCandidate *candidate = NULL;

    if (r != NULL && clipped_dest_size > 0 &&
        blit_display_surface_format(context_surfaces->color_format,
                                    &r->blit_display_color_format)) {
        candidate = remember_blit_display_candidate(
            r, dest_addr, clipped_dest_size, row_pixels, adjusted_height,
            leftover_bytes, context_surfaces->dest_pitch, bytes_per_pixel,
            r->blit_display_color_format, image_blit->operation,
            pg->draw_time);
    }

    report_successful_image_blit(
        d, source_addr + source_offset, source_size, dest_addr, dest_size,
        clipped_dest_size, row_pixels, row_bytes, adjusted_height,
        leftover_bytes, bytes_per_pixel, candidate);
}
