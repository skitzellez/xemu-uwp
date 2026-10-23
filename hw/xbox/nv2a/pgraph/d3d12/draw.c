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
#include "qemu/xemu-uwp.h"
#include "renderer.h"

static void report_draw_error(Error *err)
{
    if (err == NULL) {
        return;
    }

    fprintf(stderr, "%s\n", error_get_pretty(err));
    error_free(err);
}

static const char *primitive_mode_name(unsigned int mode)
{
    switch (mode) {
    case PRIM_TYPE_INVALID:
        return "invalid";
    case PRIM_TYPE_POINTS:
        return "points";
    case PRIM_TYPE_LINES:
        return "lines";
    case PRIM_TYPE_LINE_LOOP:
        return "line-loop";
    case PRIM_TYPE_LINE_STRIP:
        return "line-strip";
    case PRIM_TYPE_TRIANGLES:
        return "triangles";
    case PRIM_TYPE_TRIANGLE_STRIP:
        return "triangle-strip";
    case PRIM_TYPE_TRIANGLE_FAN:
        return "triangle-fan";
    case PRIM_TYPE_QUADS:
        return "quads";
    case PRIM_TYPE_QUAD_STRIP:
        return "quad-strip";
    case PRIM_TYPE_POLYGON:
        return "polygon";
    default:
        return "unknown";
    }
}

static const char *polygon_mode_name(unsigned int mode)
{
    switch (mode) {
    case POLY_MODE_FILL:
        return "fill";
    case POLY_MODE_POINT:
        return "point";
    case POLY_MODE_LINE:
        return "line";
    default:
        return "unknown";
    }
}

static unsigned int polygon_front_mode(PGRAPHState *pg)
{
    return GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER),
                    NV_PGRAPH_SETUPRASTER_FRONTFACEMODE);
}

static unsigned int polygon_back_mode(PGRAPHState *pg)
{
    return GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER),
                    NV_PGRAPH_SETUPRASTER_BACKFACEMODE);
}

static bool triangle_family_primitive(unsigned int primitive_mode)
{
    switch (primitive_mode) {
    case PRIM_TYPE_TRIANGLES:
    case PRIM_TYPE_TRIANGLE_STRIP:
    case PRIM_TYPE_TRIANGLE_FAN:
    case PRIM_TYPE_QUADS:
    case PRIM_TYPE_QUAD_STRIP:
        return true;
    default:
        return false;
    }
}

static bool polygon_cullable_primitive(unsigned int primitive_mode)
{
    return triangle_family_primitive(primitive_mode) ||
           primitive_mode == PRIM_TYPE_POLYGON;
}

static bool smooth_shading(PGRAPHState *pg)
{
    return GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_3),
                    NV_PGRAPH_CONTROL_3_SHADEMODE) ==
           NV_PGRAPH_CONTROL_3_SHADEMODE_SMOOTH;
}

static bool first_vertex_provoking(PGRAPHState *pg)
{
    return GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_3),
                    NV_PGRAPH_CONTROL_3_PROVOKING_VERTEX) ==
           NV_PGRAPH_CONTROL_3_PROVOKING_VERTEX_FIRST;
}

/*
 * Quads reach the geometry shader whole, as four-vertex line-list-with-
 * adjacency primitives, when it needs vertex 3 (the flat colour source) or
 * the quad's own edges and vertices (line and point modes). Smooth filled
 * quads are split on the CPU.
 */
static bool quad_adjacency_primitive(PGRAPHState *pg)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;

    if (r->tune_flags & PGRAPH_D3D12_TUNE_QUADS_CPU) {
        return false;
    }
    return (pg->primitive_mode == PRIM_TYPE_QUADS ||
            pg->primitive_mode == PRIM_TYPE_QUAD_STRIP) &&
           (polygon_front_mode(pg) != POLY_MODE_FILL || !smooth_shading(pg));
}

/* CULLCTRL only removes filled polygons; line and point polygon modes draw
 * every edge or vertex whatever their facing. */
static bool draw_culled_front_and_back(PGRAPHState *pg)
{
    uint32_t setup = pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER);
    unsigned int cull_face =
        GET_MASK(setup, NV_PGRAPH_SETUPRASTER_CULLCTRL);

    return polygon_cullable_primitive(pg->primitive_mode) &&
           polygon_front_mode(pg) == POLY_MODE_FILL &&
           (setup & NV_PGRAPH_SETUPRASTER_CULLENABLE) &&
           cull_face == NV_PGRAPH_SETUPRASTER_CULLCTRL_FRONT_AND_BACK;
}

static void report_unsupported_topology(PGRAPHState *pg, const char *reason)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    uint64_t bit = pg->primitive_mode < 64 ? 1ull << pg->primitive_mode : 0;
    unsigned int front_mode = polygon_front_mode(pg);
    unsigned int back_mode = polygon_back_mode(pg);

    if (bit && (r->reported_unsupported_topology_mask & bit)) {
        return;
    }
    if (bit) {
        r->reported_unsupported_topology_mask |= bit;
    }

    fprintf(stderr,
            "Direct3D 12 renderer: draw skipped: unsupported NV2A "
            "primitive topology %s (%u); buffered payload: draw_arrays=%d, "
            "inline_elements=%d, inline_buffer=%d, inline_array_bytes=%d. "
            "polygon_front=%s (%u), polygon_back=%s (%u). %s\n",
            primitive_mode_name(pg->primitive_mode), pg->primitive_mode,
            pg->draw_arrays_length, pg->inline_elements_length,
            pg->inline_buffer_length, pg->inline_array_length,
            polygon_mode_name(front_mode), front_mode,
            polygon_mode_name(back_mode), back_mode, reason);
}

static bool get_native_primitive_topology(PGRAPHState *pg,
                                          D3D_PRIMITIVE_TOPOLOGY *topology)
{
    unsigned int front_mode = polygon_front_mode(pg);

    switch (pg->primitive_mode) {
    case PRIM_TYPE_POINTS:
        *topology = D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
        return true;
    case PRIM_TYPE_LINES:
        *topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST;
        return true;
    case PRIM_TYPE_LINE_STRIP:
        *topology = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
        return true;
    case PRIM_TYPE_TRIANGLES:
        if (front_mode != POLY_MODE_FILL) {
            return false;
        }
        *topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        return true;
    case PRIM_TYPE_TRIANGLE_STRIP:
        if (front_mode != POLY_MODE_FILL) {
            return false;
        }
        *topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
        return true;
    default:
        return false;
    }
}

static bool get_expanded_primitive_topology(PGRAPHState *pg,
                                            D3D_PRIMITIVE_TOPOLOGY *topology)
{
    unsigned int front_mode = polygon_front_mode(pg);

    switch (pg->primitive_mode) {
    case PRIM_TYPE_LINE_LOOP:
        *topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST;
        return true;
    case PRIM_TYPE_TRIANGLES:
    case PRIM_TYPE_TRIANGLE_STRIP:
    case PRIM_TYPE_TRIANGLE_FAN:
        /* Line and point polygon modes draw the edges or vertices of each
         * assembled triangle from the geometry shader. */
        *topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        return true;
    case PRIM_TYPE_QUADS:
    case PRIM_TYPE_QUAD_STRIP:
        *topology = quad_adjacency_primitive(pg) ?
                        D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ :
                        D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        return true;
    case PRIM_TYPE_POLYGON:
        if (front_mode == POLY_MODE_FILL) {
            *topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            return true;
        }
        /* Point mode pairs each vertex with vertex 0, its flat colour
         * source. */
        if (front_mode == POLY_MODE_LINE || front_mode == POLY_MODE_POINT) {
            *topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST;
            return true;
        }
        report_unsupported_topology(pg,
                                    "PRIM_TYPE_POLYGON has an unknown front "
                                    "polygon mode, so D3D12 cannot choose a "
                                    "CPU-expanded topology.");
        return false;
    default:
        report_unsupported_topology(pg,
                                    "No native D3D12 topology or CPU "
                                    "expansion path exists for this mode.");
        return false;
    }
}

static uint32_t source_vertex(const uint32_t *indices, uint32_t start,
                              uint32_t i)
{
    return indices != NULL ? indices[i] : start + i;
}

static size_t expanded_index_count_for_span(PGRAPHState *pg, uint32_t count)
{
    unsigned int front_mode = polygon_front_mode(pg);
    size_t quad_indices = quad_adjacency_primitive(pg) ? 4 : 6;

    switch (pg->primitive_mode) {
    case PRIM_TYPE_LINE_LOOP:
        return count >= 2 ? count * 2 : 0;
    case PRIM_TYPE_TRIANGLES:
        return (count / 3) * 3;
    case PRIM_TYPE_TRIANGLE_STRIP:
    case PRIM_TYPE_TRIANGLE_FAN:
        return count >= 3 ? (count - 2) * 3 : 0;
    case PRIM_TYPE_QUADS:
        return (count / 4) * quad_indices;
    case PRIM_TYPE_QUAD_STRIP:
        return count >= 4 ? ((count - 2) / 2) * quad_indices : 0;
    case PRIM_TYPE_POLYGON:
        if (front_mode == POLY_MODE_FILL) {
            return count >= 3 ? (count - 2) * 3 : 0;
        }
        if (front_mode == POLY_MODE_LINE) {
            return count >= 2 ? count * 2 : 0;
        }
        if (front_mode == POLY_MODE_POINT) {
            return count >= 3 ? count * 2 : 0;
        }
        return 0;
    default:
        return 0;
    }
}

/*
 * Appends triangle (v[first], v[first + 1], v[first + 2]), indices modulo 3.
 * The rotation keeps the winding.
 */
static void append_triangle(uint32_t *dst, size_t *dst_count,
                            const uint32_t *indices, uint32_t start,
                            const uint32_t v[3], unsigned int first)
{
    for (unsigned int k = 0; k < 3; k++) {
        dst[(*dst_count)++] = source_vertex(indices, start, v[(first + k) % 3]);
    }
}

static void append_expanded_indices_for_span(PGRAPHState *pg, uint32_t *dst,
                                             size_t *dst_count,
                                             const uint32_t *indices,
                                             uint32_t start, uint32_t count)
{
    unsigned int front_mode = polygon_front_mode(pg);
    bool quad_adjacency = quad_adjacency_primitive(pg);
    /*
     * In line and point polygon modes each triangle starts at its provoking
     * vertex, the geometry shader's flat colour source: vertex 0 or 2 of a
     * list or strip triangle, outer vertex 1 or 2 of a fan triangle.
     * Filled fans keep the centre first.
     */
    unsigned int list_first = first_vertex_provoking(pg) ? 0 : 2;
    unsigned int fan_first = front_mode == POLY_MODE_FILL ? 0 :
                             first_vertex_provoking(pg) ? 1 : 2;

    switch (pg->primitive_mode) {
    case PRIM_TYPE_LINE_LOOP:
        for (uint32_t i = 0; i + 1 < count; i++) {
            dst[(*dst_count)++] = source_vertex(indices, start, i);
            dst[(*dst_count)++] = source_vertex(indices, start, i + 1);
        }
        if (count >= 2) {
            dst[(*dst_count)++] = source_vertex(indices, start, count - 1);
            dst[(*dst_count)++] = source_vertex(indices, start, 0);
        }
        break;

    case PRIM_TYPE_TRIANGLES:
        for (uint32_t i = 0; i + 2 < count; i += 3) {
            const uint32_t v[3] = { i, i + 1, i + 2 };

            append_triangle(dst, dst_count, indices, start, v, list_first);
        }
        break;

    case PRIM_TYPE_TRIANGLE_STRIP:
        for (uint32_t i = 0; i + 2 < count; i++) {
            const uint32_t v[3] = { i, i + 1, i + 2 };

            append_triangle(dst, dst_count, indices, start, v, list_first);
        }
        break;

    case PRIM_TYPE_TRIANGLE_FAN:
        for (uint32_t i = 1; i + 1 < count; i++) {
            const uint32_t v[3] = { 0, i, i + 1 };

            append_triangle(dst, dst_count, indices, start, v, fan_first);
        }
        break;

    case PRIM_TYPE_QUADS:
        for (uint32_t i = 0; i + 3 < count; i += 4) {
            if (quad_adjacency) {
                dst[(*dst_count)++] = source_vertex(indices, start, i);
                dst[(*dst_count)++] = source_vertex(indices, start, i + 1);
                dst[(*dst_count)++] = source_vertex(indices, start, i + 2);
                dst[(*dst_count)++] = source_vertex(indices, start, i + 3);
                continue;
            }
            dst[(*dst_count)++] = source_vertex(indices, start, i + 1);
            dst[(*dst_count)++] = source_vertex(indices, start, i + 2);
            dst[(*dst_count)++] = source_vertex(indices, start, i);
            dst[(*dst_count)++] = source_vertex(indices, start, i + 2);
            dst[(*dst_count)++] = source_vertex(indices, start, i + 3);
            dst[(*dst_count)++] = source_vertex(indices, start, i);
        }
        break;

    case PRIM_TYPE_QUAD_STRIP:
        for (uint32_t i = 0; i + 3 < count; i += 2) {
            if (quad_adjacency) {
                dst[(*dst_count)++] = source_vertex(indices, start, i);
                dst[(*dst_count)++] = source_vertex(indices, start, i + 1);
                dst[(*dst_count)++] = source_vertex(indices, start, i + 2);
                dst[(*dst_count)++] = source_vertex(indices, start, i + 3);
                continue;
            }
            dst[(*dst_count)++] = source_vertex(indices, start, i);
            dst[(*dst_count)++] = source_vertex(indices, start, i + 1);
            dst[(*dst_count)++] = source_vertex(indices, start, i + 2);
            dst[(*dst_count)++] = source_vertex(indices, start, i + 2);
            dst[(*dst_count)++] = source_vertex(indices, start, i + 1);
            dst[(*dst_count)++] = source_vertex(indices, start, i + 3);
        }
        break;

    case PRIM_TYPE_POLYGON:
        if (front_mode == POLY_MODE_FILL) {
            for (uint32_t i = 1; i + 1 < count; i++) {
                dst[(*dst_count)++] = source_vertex(indices, start, 0);
                dst[(*dst_count)++] = source_vertex(indices, start, i);
                dst[(*dst_count)++] = source_vertex(indices, start, i + 1);
            }
        } else if (front_mode == POLY_MODE_LINE) {
            for (uint32_t i = 0; i + 1 < count; i++) {
                dst[(*dst_count)++] = source_vertex(indices, start, i);
                dst[(*dst_count)++] = source_vertex(indices, start, i + 1);
            }
            if (count >= 2) {
                dst[(*dst_count)++] = source_vertex(indices, start, count - 1);
                dst[(*dst_count)++] = source_vertex(indices, start, 0);
            }
        } else if (front_mode == POLY_MODE_POINT && count >= 3) {
            for (uint32_t i = 0; i < count; i++) {
                dst[(*dst_count)++] = source_vertex(indices, start, i);
                dst[(*dst_count)++] = source_vertex(indices, start, 0);
            }
        }
        break;

    default:
        break;
    }
}

static bool update_expanded_index_buffer(PGRAPHState *pg, const uint32_t *data,
                                         size_t count, uint64_t *index_offset,
                                         Error **errp)
{
    uint64_t index_data_size = count * sizeof(*data);

    /* The upload reserves its own ring space. */
    return pgraph_d3d12_update_index_buffer(pg, data, index_data_size,
                                            index_offset, errp);
}

static void record_index_buffer(PGRAPHState *pg, uint64_t index_offset,
                                uint64_t index_data_size)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    D3D12_INDEX_BUFFER_VIEW index_view = {
        .BufferLocation =
            r->index_upload_buffer.resource->lpVtbl->GetGPUVirtualAddress(
                r->index_upload_buffer.resource) + index_offset,
        .SizeInBytes = (UINT)index_data_size,
        .Format = DXGI_FORMAT_R32_UINT,
    };

    r->command_list->lpVtbl->IASetIndexBuffer(r->command_list, &index_view);
}

static int compare_memory_sync_requirement_by_addr(const void *p1,
                                                   const void *p2)
{
    const PGRAPHD3D12MemorySync *l = p1;
    const PGRAPHD3D12MemorySync *r = p2;

    if (l->addr < r->addr) {
        return -1;
    }
    if (l->addr > r->addr) {
        return 1;
    }
    return 0;
}

static bool upload_vertex_ram_range(PGRAPHState *pg, hwaddr addr, hwaddr size,
                                    Error **errp)
{
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    hwaddr remaining = size;
    hwaddr cursor = addr;

    if (r->upload_buffer.size == 0) {
        error_setg(errp, "Direct3D 12 upload buffer is not initialized");
        return false;
    }

    while (remaining > 0) {
        uint64_t chunk = MIN(remaining, r->upload_buffer.size);

        if (!pgraph_d3d12_update_vertex_ram_buffer_async(pg, cursor,
                                                         d->vram_ptr + cursor,
                                                         chunk, errp)) {
            return false;
        }

        cursor += chunk;
        remaining -= chunk;
    }

    return true;
}

static bool sync_vertex_ram_buffer(PGRAPHState *pg, Error **errp)
{
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    PGRAPHD3D12MemorySync merged[NV2A_VERTEXSHADER_ATTRIBUTES];
    int num_syncs = 0;

    if (r->num_vertex_ram_buffer_syncs == 0) {
        return true;
    }

    for (size_t i = 0; i < r->num_vertex_ram_buffer_syncs; i++) {
        hwaddr start_addr = r->vertex_ram_buffer_syncs[i].addr &
                            TARGET_PAGE_MASK;

        if (r->evlog_draws && r->evlog_frames_left > 0) {
            r->evlog_vtx_hash =
                r->evlog_vtx_hash * 1099511628211ull ^
                fast_hash(d->vram_ptr + r->vertex_ram_buffer_syncs[i].addr,
                          r->vertex_ram_buffer_syncs[i].size);
        }
        hwaddr end_addr = r->vertex_ram_buffer_syncs[i].addr +
                          r->vertex_ram_buffer_syncs[i].size;

        end_addr = ROUND_UP(end_addr, TARGET_PAGE_SIZE);
        r->vertex_ram_buffer_syncs[i].addr = start_addr;
        r->vertex_ram_buffer_syncs[i].size = end_addr - start_addr;
    }

    qsort(r->vertex_ram_buffer_syncs, r->num_vertex_ram_buffer_syncs,
          sizeof(r->vertex_ram_buffer_syncs[0]),
          compare_memory_sync_requirement_by_addr);

    for (size_t i = 0; i < r->num_vertex_ram_buffer_syncs; i++) {
        PGRAPHD3D12MemorySync *current = &r->vertex_ram_buffer_syncs[i];

        if (current->size == 0) {
            continue;
        }

        if (num_syncs > 0) {
            PGRAPHD3D12MemorySync *previous = &merged[num_syncs - 1];
            hwaddr previous_end = previous->addr + previous->size;

            if (current->addr <= previous_end) {
                hwaddr current_end = current->addr + current->size;
                previous->size = MAX(previous_end, current_end) -
                                 previous->addr;
                continue;
            }
        }

        merged[num_syncs++] = *current;
    }

    for (int i = 0; i < num_syncs; i++) {
        hwaddr addr = merged[i].addr;
        hwaddr size = merged[i].size;
        bool already_uploaded;
        bool dirty;

        /*
         * No surface-coherency test on the page-aligned range: the padding
         * can overlap an unrelated render target and force a synchronous
         * readback. bind_vertex_attributes() already tested the exact span,
         * and a readback there invalidates uploaded_bitmap.
         */
        already_uploaded =
            pgraph_d3d12_vertex_ram_range_uploaded(pg, addr, size);
        dirty = memory_region_test_and_clear_dirty(d->vram, addr, size,
                                                   DIRTY_MEMORY_NV2A);

        r->evlog_vtx_uploads += dirty || !already_uploaded;
        if ((dirty || !already_uploaded ||
             (r->tune_flags & PGRAPH_D3D12_TUNE_VTX_ALWAYS)) &&
            !upload_vertex_ram_range(pg, addr, size, errp)) {
            r->num_vertex_ram_buffer_syncs = 0;
            return false;
        }
    }

    r->num_vertex_ram_buffer_syncs = 0;
    return true;
}

/* Grow-only scratch for CPU-expanded index lists; freed at renderer teardown. */
static uint32_t *expanded_index_scratch(PGRAPHD3D12State *r, size_t count)
{
    if (count > r->expanded_index_scratch_capacity) {
        r->expanded_index_scratch_capacity =
            MAX(count, r->expanded_index_scratch_capacity * 2);
        r->expanded_index_scratch =
            g_realloc(r->expanded_index_scratch,
                      r->expanded_index_scratch_capacity *
                          sizeof(*r->expanded_index_scratch));
    }
    return r->expanded_index_scratch;
}

/* Vertex pulling adds this to SV_VertexID; zero unless a draw-arrays span
 * sets it. */
static void record_vertex_start(PGRAPHD3D12State *r, uint32_t start)
{
    if (!pgraph_d3d12_vertex_pulling() ||
        (r->record_vertex_start_valid && r->record_vertex_start == start)) {
        return;
    }
    r->command_list->lpVtbl->SetGraphicsRoot32BitConstant(
        r->command_list, PGRAPH_D3D12_ROOT_VERTEX_START, start, 0);
    r->record_vertex_start = start;
    r->record_vertex_start_valid = true;
}

#define CPK_SLOTS 4096
#define CPK_SLOT_SIZE 16384
#define CPK_W 64
#define CPK_H 32

static unsigned int cpk_bytes_per_pixel(const PGRAPHD3D12SurfaceBinding *s)
{
    switch (s->fmt.dxgi_format) {
    case DXGI_FORMAT_B5G6R5_UNORM:
    case DXGI_FORMAT_B5G5R5A1_UNORM:
    case DXGI_FORMAT_B4G4R4A4_UNORM:
        return 2;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
        return 8;
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        return 16;
    default:
        return MAX(s->fmt.bytes_per_pixel, 1u);
    }
}

/* Diagnostic: copies the colour target's bottom-left corner after a draw. */
static void record_color_checkpoint(PGRAPHState *pg)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    PGRAPHD3D12SurfaceBinding *s = r->color_binding;
    D3D12_TEXTURE_COPY_LOCATION dst;
    D3D12_TEXTURE_COPY_LOCATION src;
    D3D12_RESOURCE_STATES before;
    D3D12_BOX box;
    unsigned int scale = MAX(pg->surface_scale_factor, 1);
    unsigned int bpp;
    uint32_t slot;
    uint32_t row_pitch;

    r->cpk_draw_index++;
    if (!r->cpk_enabled || r->evlog_frames_left == 0 || s == NULL ||
        s->resource == NULL || !r->command_list_open ||
        s->width < CPK_W || s->height < CPK_H ||
        r->cpk_next - r->cpk_done >= CPK_SLOTS) {
        return;
    }
    bpp = cpk_bytes_per_pixel(s);
    row_pitch = ROUND_UP(CPK_W * bpp, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    if (row_pitch * CPK_H > CPK_SLOT_SIZE) {
        return;
    }
    if (r->cpk_buffer == NULL) {
        D3D12_HEAP_PROPERTIES heap = { .Type = D3D12_HEAP_TYPE_READBACK };
        D3D12_RESOURCE_DESC desc = {
            .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
            .Width = (UINT64)CPK_SLOTS * CPK_SLOT_SIZE,
            .Height = 1,
            .DepthOrArraySize = 1,
            .MipLevels = 1,
            .SampleDesc.Count = 1,
            .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
        };

        if (FAILED(r->device->lpVtbl->CreateCommittedResource(
                r->device, &heap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource,
                (void **)&r->cpk_buffer)) ||
            FAILED(r->cpk_buffer->lpVtbl->Map(r->cpk_buffer, 0, NULL,
                                              (void **)&r->cpk_mapped))) {
            D3D12_RELEASE(r->cpk_buffer);
            r->cpk_enabled = false;
            return;
        }
    }

    slot = r->cpk_next % CPK_SLOTS;
    memset(&dst, 0, sizeof(dst));
    dst.pResource = r->cpk_buffer;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = (UINT64)slot * CPK_SLOT_SIZE;
    dst.PlacedFootprint.Footprint.Format = s->fmt.dxgi_format;
    dst.PlacedFootprint.Footprint.Width = CPK_W;
    dst.PlacedFootprint.Footprint.Height = CPK_H;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = row_pitch;
    memset(&src, 0, sizeof(src));
    src.pResource = s->resource;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    {
        unsigned int x = MIN((unsigned int)MAX(r->cpk_x, 0),
                             s->width - CPK_W / scale) * scale;
        unsigned int y = r->cpk_y < 0 ?
            s->height * scale - CPK_H :
            MIN((unsigned int)r->cpk_y, s->height - CPK_H / scale) * scale;

        box = (D3D12_BOX){
            .left = x,
            .top = y,
            .front = 0,
            .right = x + CPK_W,
            .bottom = y + CPK_H,
            .back = 1,
        };
    }

    before = s->state;
    pgraph_d3d12_transition_raw_resource(r, s->resource, &s->state,
                                         D3D12_RESOURCE_STATE_COPY_SOURCE);
    r->command_list->lpVtbl->CopyTextureRegion(r->command_list, &dst, 0, 0,
                                               0, &src, &box);
    pgraph_d3d12_transition_raw_resource(r, s->resource, &s->state, before);

    r->cpk_meta[slot].fence = r->fence_value + 1;
    r->cpk_meta[slot].frame = g_nv2a_stats.frame_count;
    r->cpk_meta[slot].draw = r->cpk_draw_index;
    r->cpk_meta[slot].row_pitch = row_pitch;
    r->cpk_meta[slot].row_bytes = CPK_W * bpp;
    r->cpk_meta[slot].rows = CPK_H;
    r->cpk_next++;
}

/* Logs each checkpoint whose corner differs from the previous one. */
void pgraph_d3d12_process_color_checkpoints(PGRAPHD3D12State *r)
{
    uint64_t completed;

    r->cpk_draw_index = 0;
    if (r->cpk_buffer == NULL || r->cpk_mapped == NULL) {
        return;
    }
    completed = r->fence->lpVtbl->GetCompletedValue(r->fence);
    while (r->cpk_done != r->cpk_next) {
        uint32_t slot = r->cpk_done % CPK_SLOTS;
        const uint8_t *data = r->cpk_mapped + (size_t)slot * CPK_SLOT_SIZE;
        uint32_t h = 0;
        uint32_t black = 0;
        uint32_t sum = 0;

        if (r->cpk_meta[slot].fence > completed) {
            break;
        }
        for (uint32_t row = 0; row < r->cpk_meta[slot].rows; row++) {
            const uint8_t *p = data + row * r->cpk_meta[slot].row_pitch;
            uint32_t bpp = r->cpk_meta[slot].row_bytes / CPK_W;

            h = h * 1000003u ^
                (uint32_t)fast_hash(p, r->cpk_meta[slot].row_bytes);
            for (uint32_t x = 0; x < CPK_W; x++) {
                uint32_t rr, gg, bb;

                if (bpp == 2) {
                    uint16_t v = p[x * 2] | (p[x * 2 + 1] << 8);

                    rr = ((v >> 11) & 31) << 3;
                    gg = ((v >> 5) & 63) << 2;
                    bb = (v & 31) << 3;
                } else {
                    bb = p[x * bpp];
                    gg = p[x * bpp + 1];
                    rr = p[x * bpp + 2];
                }
                black += rr < 12 && gg < 12 && bb < 12;
                sum += rr + gg + bb;
            }
        }
        if (r->cpk_meta[slot].frame != r->cpk_last_frame) {
            r->cpk_last_frame = r->cpk_meta[slot].frame;
            r->cpk_last_hash = 0;
        }
        if (h != r->cpk_last_hash) {
            pgraph_d3d12_evlog(r, "cpk rec=%u i=%u h=%08x blk=%u sum=%u",
                               r->cpk_meta[slot].frame,
                               r->cpk_meta[slot].draw, h, black, sum);
            r->cpk_last_hash = h;
        }
        r->cpk_done++;
    }
}

static bool begin_ready_draw(PGRAPHState *pg, D3D_PRIMITIVE_TOPOLOGY topology,
                             bool *pending, Error **errp)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;

    /* Diagnostic: drop draws that sample a depth texture. */
    if (r->tune_flags & PGRAPH_D3D12_TUNE_SKIP_SHADOW) {
        for (unsigned int i = 0; i < NV2A_MAX_TEXTURES; i++) {
            unsigned int cf = GET_MASK(
                pgraph_reg_r(pg, NV_PGRAPH_TEXFMT0 + i * 4),
                NV_PGRAPH_TEXFMT0_COLOR);

            if (pgraph_is_texture_stage_active(pg, i) &&
                pgraph_is_texture_enabled(pg, i) &&
                (cf == NV097_SET_TEXTURE_FORMAT_COLOR_SZ_DEPTH_Y16_FIXED ||
                (cf >= NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_X8_Y24_FIXED &&
                 cf <= NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_Y16_FLOAT))) {
                *pending = true;
                return false;
            }
        }
    }
    if (!pgraph_d3d12_begin_draw_commands(pg, pending, errp)) {
        return false;
    }

    if (!r->record_topology_valid || r->record_topology != topology) {
        r->command_list->lpVtbl->IASetPrimitiveTopology(r->command_list,
                                                        topology);
        r->record_topology = topology;
        r->record_topology_valid = true;
    }
    record_vertex_start(r, 0);
    pgraph_d3d12_capture_draw(pg, (unsigned int)topology);
    if (r->evlog_draws && r->evlog_frames_left > 0) {
        hwaddr tex[NV2A_MAX_TEXTURES] = { 0 };
        unsigned int res[NV2A_MAX_TEXTURES] = { 0 };
        unsigned int content[NV2A_MAX_TEXTURES] = { 0 };
        unsigned int draw_hash = 0;
        unsigned int pixel_hash = 0;
        unsigned int index_hash = 0;

        for (unsigned int i = 0; i < NV2A_MAX_TEXTURES; i++) {
            const PGRAPHD3D12TextureBinding *b = r->texture_binding[i];

            if ((r->texture_active_mask & (1u << i)) && b != NULL) {
                tex[i] = b->vram_addr;
                res[i] = (unsigned int)(((uintptr_t)b->resource >> 6) &
                                        0xffff);
                content[i] = b->surface_to_texture ?
                    (unsigned int)b->draw_time & 0xffff :
                    (unsigned int)b->data_hash & 0xffff;
            }
        }
        if (r->draw_constants_cached && r->draw_constants_cache) {
            draw_hash = (unsigned int)fast_hash(
                (const uint8_t *)r->draw_constants_cache,
                r->draw_constants_cache_size);
        }
        if (r->pixel_constants_cached && r->pixel_constants_cache) {
            pixel_hash = (unsigned int)fast_hash(
                (const uint8_t *)r->pixel_constants_cache,
                r->pixel_constants_cache_size);
        }
        if (pg->inline_elements_length) {
            index_hash = (unsigned int)fast_hash(
                (const uint8_t *)pg->inline_elements,
                pg->inline_elements_length * sizeof(pg->inline_elements[0]));
        } else if (pg->draw_arrays_length) {
            index_hash = (unsigned int)fast_hash(
                (const uint8_t *)pg->draw_arrays_start,
                pg->draw_arrays_length * sizeof(pg->draw_arrays_start[0])) ^
                (unsigned int)fast_hash(
                (const uint8_t *)pg->draw_arrays_count,
                pg->draw_arrays_length * sizeof(pg->draw_arrays_count[0]));
        } else if (pg->inline_array_length) {
            index_hash = (unsigned int)fast_hash(
                (const uint8_t *)pg->inline_array,
                pg->inline_array_length * sizeof(pg->inline_array[0]));
        }
        pgraph_d3d12_evlog(
            r, "draw c=0x%" HWADDR_PRIx " z=0x%" HWADDR_PRIx
            " prim=%u n=%u/%u/%u/%u key=%08x t=%" HWADDR_PRIx ",%"
            HWADDR_PRIx ",%" HWADDR_PRIx ",%" HWADDR_PRIx
            " vh=%08x vu=%u ch=%08x ph=%08x ih=%08x"
            " r=%04x:%04x,%04x:%04x,%04x:%04x,%04x:%04x"
            " pso=%04x vp=%08x rt=%04x:%04x ff=%d sub=%" PRIu64
            " c6w=%08x pc=%08x",
            r->color_binding ? r->color_binding->vram_addr : 0,
            r->zeta_binding ? r->zeta_binding->vram_addr : 0,
            pg->primitive_mode, pg->inline_elements_length,
            pg->draw_arrays_length, pg->inline_array_length,
            pg->inline_buffer_length,
            (unsigned int)fast_hash((const uint8_t *)&r->draw_pipeline_key,
                                    sizeof(r->draw_pipeline_key)),
            tex[0], tex[1], tex[2], tex[3],
            (unsigned int)r->evlog_vtx_hash, r->evlog_vtx_uploads,
            draw_hash, pixel_hash, index_hash,
            res[0], content[0], res[1], content[1], res[2], content[2],
            res[3], content[3],
            (unsigned int)(((uintptr_t)r->draw_pipeline_state >> 6) &
                           0xffff),
            (unsigned int)(fast_hash((const uint8_t *)&r->record_viewport,
                                     sizeof(r->record_viewport)) ^
                           fast_hash((const uint8_t *)&r->record_scissor,
                                     sizeof(r->record_scissor))),
            (unsigned int)(r->record_rtv_ptr & 0xffff),
            (unsigned int)(r->record_dsv_ptr & 0xffff),
            r->draw_pipeline_key_valid &&
                r->draw_pipeline_key.shader_framebuffer_fetch,
            (uint64_t)r->perf.submits,
            /* c[6].w and the position constants c[4..8], c[21..22]. */
            r->draw_constants_cache ?
                ((const uint32_t *)r->draw_constants_cache)[64 + 6 * 4 + 3] :
                0,
            r->draw_constants_cache ?
                (unsigned int)(fast_hash((const uint8_t *)
                                   r->draw_constants_cache + (64 + 16) * 4,
                                   20 * 4) ^
                               fast_hash((const uint8_t *)
                                   r->draw_constants_cache + (64 + 84) * 4,
                                   8 * 4)) :
                0);
        {
            uint32_t key32 = (uint32_t)fast_hash(
                (const uint8_t *)&r->draw_pipeline_key,
                sizeof(r->draw_pipeline_key));

            /* Pipeline state of each key once per window. */
            {
                unsigned int slot = key32 & 1023;

                while (r->evlog_seen_key[slot] != 0 &&
                       r->evlog_seen_key[slot] != key32) {
                    slot = (slot + 1) & 1023;
                }
                if (r->evlog_seen_key[slot] == 0 && key32 != 0) {
                    const PGRAPHD3D12PipelineKey *pk =
                        &r->draw_pipeline_key;

                    r->evlog_seen_key[slot] = key32;
                    pgraph_d3d12_evlog(
                        r, "pkey key=%08x depth=%d/%d/%d stencil=%d "
                        "func=%d rm=%02x wm=%02x ops=%d,%d,%d "
                        "blend=%d %d/%d/%d cwm=%x cull=%d ccw=%d "
                        "atest=%d zpersp=%d zclip=%d tune=%x",
                        key32, pk->depth_enable, pk->depth_write,
                        pk->depth_func, pk->stencil_enable,
                        pk->stencil_func, pk->stencil_read_mask,
                        pk->stencil_write_mask, pk->stencil_fail_op,
                        pk->stencil_depth_fail_op, pk->stencil_pass_op,
                        pk->blend_enable, pk->src_blend, pk->dest_blend,
                        pk->blend_op, pk->color_write_mask, pk->cull_mode,
                        pk->front_counter_clockwise,
                        pk->alpha_test_enable, pk->z_perspective,
                        pk->depth_clipping, pk->tune_flags);
                }
            }
            pgraph_d3d12_record_depth_checkpoint(pg, key32,
                                                 r->cpk_draw_index + 1);
            /* Position constants of every draw of the first dump key. */
            if (r->evlog_dump_key[0] != 0 && key32 == r->evlog_dump_key[0] &&
                r->draw_constants_cache != NULL) {
                const float *c = (const float *)r->draw_constants_cache + 64;

                pgraph_d3d12_evlog(
                    r, "pcd i=%u c4=%g,%g,%g,%g c5=%g,%g,%g,%g "
                    "c6=%g,%g,%g,%g c7=%g,%g,%g,%g c8=%g,%g,%g,%g "
                    "c21=%g,%g,%g,%g c22=%g,%g,%g,%g",
                    r->cpk_draw_index + 1,
                    c[16], c[17], c[18], c[19], c[20], c[21], c[22], c[23],
                    c[24], c[25], c[26], c[27], c[28], c[29], c[30], c[31],
                    c[32], c[33], c[34], c[35], c[84], c[85], c[86], c[87],
                    c[88], c[89], c[90], c[91]);
            }
            /* Guest positions of the first four vertices of that key. */
            if (r->evlog_dump_key[0] != 0 && key32 == r->evlog_dump_key[0] &&
                pg->draw_arrays_length > 0 &&
                !(pg->uniform_attrs & (1u << NV2A_VERTEX_ATTR_POSITION))) {
                NV2AState *d = container_of(pg, NV2AState, pgraph);
                VertexAttribute *attr =
                    &pg->vertex_attributes[NV2A_VERTEX_ATTR_POSITION];
                hwaddr dma_len;
                uint8_t *dma_data = (uint8_t *)nv_dma_map(
                    d, attr->dma_select ? pg->dma_vertex_b :
                                          pg->dma_vertex_a,
                    &dma_len);

                if (dma_data != NULL && attr->offset <= dma_len &&
                    attr->format == NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F) {
                    float v[4][4] = { { 0 } };
                    uint64_t base = (uint64_t)(dma_data - d->vram_ptr) +
                        attr->offset +
                        (uint64_t)pg->draw_arrays_start[0] * attr->stride;

                    for (unsigned int i = 0; i < 4; i++) {
                        uint64_t a = base + (uint64_t)i * attr->stride;

                        if (a + attr->count * 4 <=
                            memory_region_size(d->vram)) {
                            memcpy(v[i], d->vram_ptr + a,
                                   MIN(attr->count, 4u) * 4);
                        }
                    }
                    pgraph_d3d12_evlog(
                        r, "vpos n=%u stride=%u v0=%g,%g,%g,%g "
                        "v1=%g,%g,%g,%g v2=%g,%g,%g,%g v3=%g,%g,%g,%g",
                        attr->count, attr->stride, v[0][0], v[0][1], v[0][2],
                        v[0][3], v[1][0], v[1][1], v[1][2], v[1][3],
                        v[2][0], v[2][1], v[2][2], v[2][3], v[3][0],
                        v[3][1], v[3][2], v[3][3]);
                }
            }
            for (unsigned int k = 0; k < 2; k++) {
                if (r->evlog_dump_key[k] == 0 ||
                    r->evlog_dump_key[k] != key32 ||
                    r->evlog_dump_frame[k] == g_nv2a_stats.frame_count ||
                    !r->draw_constants_cached ||
                    r->draw_constants_cache == NULL) {
                    continue;
                }
                const uint32_t *w = r->draw_constants_cache;
                GString *s = g_string_new(NULL);

                for (size_t i = 0;
                     i < r->draw_constants_cache_size / 4; i++) {
                    g_string_append_printf(s, "%s%08x", i ? "," : "",
                                           w[i]);
                }
                pgraph_d3d12_evlog(r, "dconst key=%08x w=%s", key32,
                                   s->str);
                g_string_free(s, TRUE);
                r->evlog_dump_frame[k] = g_nv2a_stats.frame_count;
            }
        }
        /* Dump each distinct pixel-constant block once per window. */
        if (pixel_hash != 0) {
            unsigned int slot = pixel_hash & 4095;

            while (r->evlog_seen_ph[slot] != 0 &&
                   r->evlog_seen_ph[slot] != pixel_hash) {
                slot = (slot + 1) & 4095;
            }
            if (r->evlog_seen_ph[slot] == 0) {
                const uint32_t *w = r->pixel_constants_cache;
                GString *s = g_string_new(NULL);

                r->evlog_seen_ph[slot] = pixel_hash;
                for (size_t i = 0;
                     i < r->pixel_constants_cache_size / 4; i++) {
                    g_string_append_printf(s, "%s%08x", i ? "," : "",
                                           w[i]);
                }
                pgraph_d3d12_evlog(r, "pconst ph=%08x w=%s", pixel_hash,
                                   s->str);
                g_string_free(s, TRUE);
            }
        }
    }
    return true;
}

/* Packs the enabled attributes of an inline-array vertex; returns its size
 * in bytes. */
static unsigned int layout_inline_array_vertex(PGRAPHState *pg)
{
    unsigned int offset = 0;

    for (int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
        VertexAttribute *attr = &pg->vertex_attributes[i];

        if (attr->count == 0) {
            continue;
        }

        offset = ROUND_UP(offset, attr->size);
        attr->inline_array_offset = offset;
        offset += attr->size * attr->count;
        offset = ROUND_UP(offset, attr->size);
    }

    return offset;
}

/* A culled draw still leaves the current attribute values at its provoking
 * (array sources) or last (inline buffer) vertex. */
static void update_culled_draw_attributes(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;

    if (pg->draw_arrays_length) {
        pgraph_d3d12_update_provoking_attribute_values(
            d, pg->draw_arrays_max_count - 1, false, 0);
    } else if (pg->inline_elements_length) {
        pgraph_d3d12_update_provoking_attribute_values(
            d, pg->inline_elements[pg->inline_elements_length - 1], false, 0);
    } else if (pg->inline_buffer_length) {
        for (int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
            VertexAttribute *attr = &pg->vertex_attributes[i];

            if (!attr->inline_buffer_populated) {
                continue;
            }
            attr->inline_buffer_populated = false;
            memcpy(attr->inline_value,
                   attr->inline_buffer + (pg->inline_buffer_length - 1) * 4,
                   sizeof(attr->inline_value));
        }
    } else if (pg->inline_array_length) {
        unsigned int vertex_size = layout_inline_array_vertex(pg);
        unsigned int index_count =
            vertex_size ? pg->inline_array_length * 4 / vertex_size : 0;

        if (index_count != 0) {
            pgraph_d3d12_update_provoking_attribute_values(
                d, index_count - 1, true, vertex_size);
        }
    }
}

void pgraph_d3d12_draw_begin(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    uint32_t control_0 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0);
    bool color_write =
        control_0 & (NV_PGRAPH_CONTROL_0_ALPHA_WRITE_ENABLE |
                     NV_PGRAPH_CONTROL_0_RED_WRITE_ENABLE |
                     NV_PGRAPH_CONTROL_0_GREEN_WRITE_ENABLE |
                     NV_PGRAPH_CONTROL_0_BLUE_WRITE_ENABLE);
    bool depth_test = control_0 & NV_PGRAPH_CONTROL_0_ZENABLE;
    bool stencil_test =
        pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1) &
        NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE;
    bool fragment_output = color_write || depth_test || stencil_test;

    if (r == NULL) {
        return;
    }

    r->num_vertex_ram_buffer_syncs = 0;

    if (fragment_output) {
        int64_t perf_start = uwp_prof_now(); /* once per draw: the cheap clock */

        /* Depth and stencil writes only happen with their tests enabled. */
        pgraph_d3d12_surface_update(d, true, color_write,
                                    depth_test || stencil_test);
        r->perf.surface_update_ticks += uwp_prof_now() - perf_start;
    }
}

void pgraph_d3d12_draw_end(NV2AState *d)
{
    pgraph_d3d12_flush_draw(d);
}

static void pgraph_d3d12_flush_draw_body(NV2AState *d);

void pgraph_d3d12_flush_draw(NV2AState *d)
{
    pgraph_d3d12_flush_draw_body(d);
}

static void pgraph_d3d12_flush_draw_body(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    D3D_PRIMITIVE_TOPOLOGY topology;
    bool expanded_topology = false;
    bool draw_pending = false;
    uint32_t control_0 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0);
    uint32_t control_1 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1);
    bool color_write =
        control_0 & (NV_PGRAPH_CONTROL_0_ALPHA_WRITE_ENABLE |
                     NV_PGRAPH_CONTROL_0_RED_WRITE_ENABLE |
                     NV_PGRAPH_CONTROL_0_GREEN_WRITE_ENABLE |
                     NV_PGRAPH_CONTROL_0_BLUE_WRITE_ENABLE);
    bool zeta_write =
        control_0 & (NV_PGRAPH_CONTROL_0_ZWRITEENABLE |
                     NV_PGRAPH_CONTROL_0_STENCIL_WRITE_ENABLE);
    bool depth_test = control_0 & NV_PGRAPH_CONTROL_0_ZENABLE;
    bool stencil_test =
        control_1 & NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE;
    bool fragment_output = color_write || depth_test || stencil_test;
    Error *local_err = NULL;

    if (r == NULL) {
        return;
    }

    pgraph_d3d12_discard_unrecorded_draw_uploads(r);
    r->evlog_vtx_hash = 0;
    r->evlog_vtx_uploads = 0;

    if (!fragment_output) {
        return;
    }

    if (draw_culled_front_and_back(pg)) {
        update_culled_draw_attributes(d);
        return;
    }

    if (!get_native_primitive_topology(pg, &topology)) {
        expanded_topology = true;
        if (!get_expanded_primitive_topology(pg, &topology)) {
            report_draw_error(local_err);
            return;
        }
    }

    if (pg->draw_arrays_length) {
        uint32_t *expanded_indices = NULL;
        size_t expanded_index_count = 0;
        uint64_t index_offset = 0;

        nv2a_profile_inc_counter(NV2A_PROF_DRAW_ARRAYS);

        int64_t vertex_start = uwp_prof_now();
        bool vertices_ok = pgraph_d3d12_bind_vertex_attributes(
                d, pg->draw_arrays_min_start, pg->draw_arrays_max_count - 1,
                false, 0, pg->draw_arrays_max_count - 1, &local_err) &&
            sync_vertex_ram_buffer(pg, &local_err);
        r->perf.vertex_setup_ticks += uwp_prof_now() - vertex_start;
        if (!vertices_ok) {
            report_draw_error(local_err);
            return;
        }

        if (expanded_topology) {
            for (int i = 0; i < pg->draw_arrays_length; i++) {
                expanded_index_count += expanded_index_count_for_span(
                    pg, pg->draw_arrays_count[i]);
            }

            if (expanded_index_count == 0) {
                return;
            }

            expanded_indices = expanded_index_scratch(r, expanded_index_count);
            expanded_index_count = 0;
            for (int i = 0; i < pg->draw_arrays_length; i++) {
                append_expanded_indices_for_span(
                    pg, expanded_indices, &expanded_index_count, NULL,
                    pg->draw_arrays_start[i], pg->draw_arrays_count[i]);
            }

            if (!update_expanded_index_buffer(pg, expanded_indices,
                                              expanded_index_count,
                                              &index_offset, &local_err)) {
                report_draw_error(local_err);
                return;
            }
        }

        pgraph_d3d12_prepare_zpass_queries(d);
        if (!begin_ready_draw(pg, topology, &draw_pending, &local_err)) {
            if (!draw_pending) {
                report_draw_error(local_err);
            }
            return;
        }

        pgraph_d3d12_record_vertex_buffers(pg, 0, 0);
        pgraph_d3d12_set_surface_dirty(pg, color_write,
                                       depth_test || stencil_test);
        pgraph_d3d12_begin_zpass_query(d);
        if (expanded_topology) {
            record_index_buffer(pg, index_offset,
                                expanded_index_count *
                                    sizeof(*expanded_indices));
            r->command_list->lpVtbl->DrawIndexedInstanced(
                r->command_list, expanded_index_count, 1, 0, 0, 0);
        } else {
            for (int i = 0; i < pg->draw_arrays_length; i++) {
                if (pgraph_d3d12_vertex_pulling()) {
                    record_vertex_start(r, pg->draw_arrays_start[i]);
                    r->command_list->lpVtbl->DrawInstanced(
                        r->command_list, pg->draw_arrays_count[i], 1, 0, 0);
                } else {
                    r->command_list->lpVtbl->DrawInstanced(
                        r->command_list, pg->draw_arrays_count[i], 1,
                        pg->draw_arrays_start[i], 0);
                }
            }
        }
    } else if (pg->inline_elements_length) {
        uint32_t *expanded_indices = NULL;
        size_t output_index_count = pg->inline_elements_length;
        uint64_t index_data_size;
        uint64_t index_offset = 0;
        uint32_t min_element = (uint32_t)-1;
        uint32_t max_element = 0;

        nv2a_profile_inc_counter(NV2A_PROF_INLINE_ELEMENTS);

        for (int i = 0; i < pg->inline_elements_length; i++) {
            max_element = MAX(pg->inline_elements[i], max_element);
            min_element = MIN(pg->inline_elements[i], min_element);
        }

        if (expanded_topology) {
            output_index_count = expanded_index_count_for_span(
                pg, pg->inline_elements_length);
            if (output_index_count == 0) {
                pgraph_d3d12_update_provoking_attribute_values(
                    d, pg->inline_elements[pg->inline_elements_length - 1],
                    false, 0);
                return;
            }

            expanded_indices = expanded_index_scratch(r, output_index_count);
            output_index_count = 0;
            append_expanded_indices_for_span(
                pg, expanded_indices, &output_index_count,
                pg->inline_elements, 0, pg->inline_elements_length);
        }

        index_data_size = output_index_count * sizeof(pg->inline_elements[0]);
        int64_t vertex_start = uwp_prof_now();
        bool vertices_ok = pgraph_d3d12_bind_vertex_attributes(
                d, min_element, max_element, false, 0,
                pg->inline_elements[pg->inline_elements_length - 1],
                &local_err) &&
            sync_vertex_ram_buffer(pg, &local_err) &&
            pgraph_d3d12_update_index_buffer(
                pg, expanded_topology ? expanded_indices : pg->inline_elements,
                index_data_size, &index_offset, &local_err);
        r->perf.vertex_setup_ticks += uwp_prof_now() - vertex_start;
        if (!vertices_ok) {
            report_draw_error(local_err);
            return;
        }

        pgraph_d3d12_prepare_zpass_queries(d);
        if (!begin_ready_draw(pg, topology, &draw_pending, &local_err)) {
            if (!draw_pending) {
                report_draw_error(local_err);
            }
            return;
        }

        pgraph_d3d12_record_vertex_buffers(pg, 0, 0);
        record_index_buffer(pg, index_offset, index_data_size);
        pgraph_d3d12_set_surface_dirty(pg, color_write,
                                       depth_test || stencil_test);
        pgraph_d3d12_begin_zpass_query(d);
        r->command_list->lpVtbl->DrawIndexedInstanced(
            r->command_list, output_index_count, 1, 0, 0, 0);
    } else if (pg->inline_buffer_length) {
        const void *data[NV2A_VERTEXSHADER_ATTRIBUTES];
        uint64_t sizes[NV2A_VERTEXSHADER_ATTRIBUTES];
        uint64_t vertex_data_size = pg->inline_buffer_length *
                                    sizeof(float) * 4;
        uint64_t total_size = 0;
        uint64_t buffer_offset = 0;
        uint32_t *expanded_indices = NULL;
        size_t expanded_index_count = 0;
        uint64_t index_offset = 0;

        nv2a_profile_inc_counter(NV2A_PROF_INLINE_BUFFERS);

        pgraph_d3d12_bind_vertex_attributes_inline(d);
        for (int i = 0; i < r->num_active_vertex_attribute_descriptions; i++) {
            int attr_index = r->vertex_attribute_descriptions[i].SemanticIndex;
            VertexAttribute *attr = &pg->vertex_attributes[attr_index];

            r->vertex_attribute_offsets[attr_index] = total_size;
            data[i] = attr->inline_buffer;
            sizes[i] = vertex_data_size;
            attr->inline_buffer_populated = false;
            total_size += vertex_data_size;
        }

        int64_t vertex_start = uwp_prof_now();
        if (!pgraph_d3d12_update_vertex_inline_buffer(
                pg, data, sizes, r->num_active_vertex_attribute_descriptions,
                &buffer_offset, &local_err)) {
            r->perf.vertex_setup_ticks +=
                uwp_prof_now() - vertex_start;
            report_draw_error(local_err);
            return;
        }
        r->perf.vertex_setup_ticks += uwp_prof_now() - vertex_start;
        r->vertex_fetch_ring_base = buffer_offset;

        if (expanded_topology) {
            expanded_index_count = expanded_index_count_for_span(
                pg, pg->inline_buffer_length);
            if (expanded_index_count == 0) {
                return;
            }

            expanded_indices = expanded_index_scratch(r, expanded_index_count);
            expanded_index_count = 0;
            append_expanded_indices_for_span(pg, expanded_indices,
                                             &expanded_index_count, NULL, 0,
                                             pg->inline_buffer_length);

            if (!update_expanded_index_buffer(pg, expanded_indices,
                                              expanded_index_count,
                                              &index_offset, &local_err)) {
                report_draw_error(local_err);
                return;
            }
        }

        pgraph_d3d12_prepare_zpass_queries(d);
        if (!begin_ready_draw(pg, topology, &draw_pending, &local_err)) {
            if (!draw_pending) {
                report_draw_error(local_err);
            }
            return;
        }

        pgraph_d3d12_record_inline_vertex_buffers(pg, buffer_offset);
        pgraph_d3d12_set_surface_dirty(pg, color_write,
                                       depth_test || stencil_test);
        pgraph_d3d12_begin_zpass_query(d);
        if (expanded_topology) {
            record_index_buffer(pg, index_offset,
                                expanded_index_count *
                                    sizeof(*expanded_indices));
            r->command_list->lpVtbl->DrawIndexedInstanced(
                r->command_list, expanded_index_count, 1, 0, 0, 0);
        } else {
            r->command_list->lpVtbl->DrawInstanced(
                r->command_list, pg->inline_buffer_length, 1, 0, 0);
        }
    } else if (pg->inline_array_length) {
        const void *inline_array_data = pg->inline_array;
        uint64_t inline_array_data_size = pg->inline_array_length * 4;
        uint64_t buffer_offset = 0;
        uint32_t *expanded_indices = NULL;
        size_t expanded_index_count = 0;
        uint64_t index_offset = 0;
        unsigned int vertex_size;
        unsigned int index_count;

        nv2a_profile_inc_counter(NV2A_PROF_INLINE_ARRAYS);

        vertex_size = layout_inline_array_vertex(pg);
        if (vertex_size == 0) {
            return;
        }
        index_count = pg->inline_array_length * 4 / vertex_size;

        int64_t vertex_start = uwp_prof_now();
        bool vertices_ok = pgraph_d3d12_bind_vertex_attributes(
                d, 0, index_count - 1, true, vertex_size, index_count - 1,
                &local_err) &&
            pgraph_d3d12_update_vertex_inline_buffer(
                pg, &inline_array_data, &inline_array_data_size, 1,
                &buffer_offset, &local_err);
        r->perf.vertex_setup_ticks += uwp_prof_now() - vertex_start;
        if (!vertices_ok) {
            report_draw_error(local_err);
            return;
        }
        /* Non-converted attributes source the uploaded inline array. */
        r->vertex_fetch_inline_mask = 0xffff;
        r->vertex_fetch_ring_base = buffer_offset;

        if (expanded_topology) {
            expanded_index_count = expanded_index_count_for_span(
                pg, index_count);
            if (expanded_index_count == 0) {
                return;
            }

            expanded_indices = expanded_index_scratch(r, expanded_index_count);
            expanded_index_count = 0;
            append_expanded_indices_for_span(pg, expanded_indices,
                                             &expanded_index_count, NULL, 0,
                                             index_count);

            if (!update_expanded_index_buffer(pg, expanded_indices,
                                              expanded_index_count,
                                              &index_offset, &local_err)) {
                report_draw_error(local_err);
                return;
            }
        }

        pgraph_d3d12_prepare_zpass_queries(d);
        if (!begin_ready_draw(pg, topology, &draw_pending, &local_err)) {
            if (!draw_pending) {
                report_draw_error(local_err);
            }
            return;
        }

        pgraph_d3d12_record_inline_vertex_buffers(pg, buffer_offset);
        pgraph_d3d12_set_surface_dirty(pg, color_write,
                                       depth_test || stencil_test);
        pgraph_d3d12_begin_zpass_query(d);
        if (expanded_topology) {
            record_index_buffer(pg, index_offset,
                                expanded_index_count *
                                    sizeof(*expanded_indices));
            r->command_list->lpVtbl->DrawIndexedInstanced(
                r->command_list, expanded_index_count, 1, 0, 0, 0);
        } else {
            r->command_list->lpVtbl->DrawInstanced(
                r->command_list, index_count, 1, 0, 0);
        }
    } else {
        return;
    }

    record_color_checkpoint(pg);

    /* Transient uploads made while preparing this draw now belong to it,
     * not to a batch retired by an earlier ring-pressure submit. */
    pgraph_d3d12_draw_uploads_recorded(r);

    /* Batch draws per command list; submit periodically to bound list growth
     * and let the staging rings retire. */
    if (++r->draws_since_submit >= PGRAPH_D3D12_DRAWS_PER_SUBMIT) {
        r->perf.submit_src[2]++;
        if (!pgraph_d3d12_submit_commands_async(pg, &local_err)) {
            report_draw_error(local_err);
            return;
        }
    }

    pg->draw_time++;
    if (r->color_binding && color_write) {
        r->color_binding->draw_time = pg->draw_time;
    }
    if (r->zeta_binding && zeta_write) {
        r->zeta_binding->draw_time = pg->draw_time;
    }

}
