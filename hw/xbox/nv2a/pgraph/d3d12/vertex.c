/*
 * Geforce NV2A PGRAPH Direct3D 12 Renderer
 *
 * Copyright (c) 2026 danprice142 (https://github.com/danprice142/xemu-uwp)
 * Changed for xemu on Xbox Dev Mode by Skitzelles (https://github.com/skitzellez/xemu-uwp)
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
#include "qapi/error.h"
#include "renderer.h"

static const char *vertex_attribute_format_name(unsigned int format)
{
    switch (format) {
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_D3D:
        return "UB_D3D";
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S1:
        return "S1";
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F:
        return "F";
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_OGL:
        return "UB_OGL";
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S32K:
        return "S32K";
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_CMP:
        return "CMP";
    default:
        return "unknown";
    }
}

static bool dxgi_format_for_attribute(const VertexAttribute *attr,
                                      DXGI_FORMAT *format,
                                      bool *needs_shader_conversion,
                                      bool *needs_cpu_expansion,
                                      bool *d3d_swizzle)
{
    static const DXGI_FORMAT float_to_count[] = {
        DXGI_FORMAT_R32_FLOAT,
        DXGI_FORMAT_R32G32_FLOAT,
        DXGI_FORMAT_R32G32B32_FLOAT,
        DXGI_FORMAT_R32G32B32A32_FLOAT,
    };
    static const DXGI_FORMAT ub_to_count[] = {
        DXGI_FORMAT_R8_UNORM,
        DXGI_FORMAT_R8G8_UNORM,
        DXGI_FORMAT_UNKNOWN,
        DXGI_FORMAT_R8G8B8A8_UNORM,
    };
    static const DXGI_FORMAT s1_to_count[] = {
        DXGI_FORMAT_R16_SNORM,
        DXGI_FORMAT_R16G16_SNORM,
        DXGI_FORMAT_UNKNOWN,
        DXGI_FORMAT_R16G16B16A16_SNORM,
    };
    *needs_shader_conversion = false;
    *needs_cpu_expansion = false;
    *d3d_swizzle = false;

    if (attr->count == 0 || attr->count > 4) {
        return false;
    }

    switch (attr->format) {
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_D3D:
        *d3d_swizzle = true;
        /* fall through */
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_OGL:
        if (attr->count == 3) {
            *format = DXGI_FORMAT_R8G8B8A8_UNORM;
            *needs_cpu_expansion = true;
        } else {
            *format = ub_to_count[attr->count - 1];
        }
        break;
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S1:
        if (attr->count == 3) {
            *format = DXGI_FORMAT_R16G16B16A16_SNORM;
            *needs_cpu_expansion = true;
        } else {
            *format = s1_to_count[attr->count - 1];
        }
        break;
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F:
        *format = float_to_count[attr->count - 1];
        break;
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S32K:
        *format = float_to_count[attr->count - 1];
        *needs_cpu_expansion = true;
        break;
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_CMP:
        if (attr->count != 1) {
            return false;
        }
        *needs_shader_conversion = true;
        *format = DXGI_FORMAT_R32_SINT;
        break;
    default:
        return false;
    }

    return *format != DXGI_FORMAT_UNKNOWN;
}

static const char *cpu_expansion_kind_name(const VertexAttribute *attr)
{
    switch (attr->format) {
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_D3D:
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_OGL:
        return "R8G8B8-to-R8G8B8A8";
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S1:
        return "R16G16B16_SNORM-to-R16G16B16A16_SNORM";
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S32K:
        return "S32K-to-R32_FLOAT";
    default:
        return "unknown";
    }
}

static void expand_vertex_attribute_3_to_4(const VertexAttribute *attr,
                                           const uint8_t *src, uint8_t *dst)
{
    switch (attr->format) {
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_D3D:
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_OGL:
        dst[0] = src[0];
        dst[1] = src[1];
        dst[2] = src[2];
        dst[3] = UINT8_MAX;
        break;
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S1: {
        const int16_t *src16 = (const int16_t *)src;
        int16_t *dst16 = (int16_t *)dst;

        dst16[0] = src16[0];
        dst16[1] = src16[1];
        dst16[2] = src16[2];
        dst16[3] = INT16_MAX;
        break;
    }
    default:
        g_assert_not_reached();
    }
}

static bool upload_converted_vertex_attribute(
    NV2AState *d, uint8_t attr_index, const VertexAttribute *attr,
    const uint8_t *source, uint64_t source_size, uint64_t source_base,
    uint32_t source_stride, unsigned int min_element,
    unsigned int max_element, bool inline_data, uint64_t *dst_offset,
    uint32_t *dst_stride, Error **errp)
{
    PGRAPHState *pg = &d->pgraph;
    uint32_t src_element_size = attr->size * attr->count;
    bool s32k_to_float =
        attr->format == NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S32K;
    uint32_t converted_stride =
        s32k_to_float ? attr->count * sizeof(float) : attr->size * 4;
    uint64_t alignment = s32k_to_float ? sizeof(float) : attr->size;
    /* Vertex pulling rebases the fetch, so only the draw's element range is
     * converted; the input assembler path still needs element zero onward. */
    unsigned int first_element = pgraph_d3d12_vertex_pulling() ?
        min_element : 0;
    uint64_t element_count = (uint64_t)max_element + 1 - first_element;
    uint64_t required_source_end;
    uint64_t converted_size;
    g_autofree uint8_t *converted_data = NULL;
    const void *chunks[1];
    uint64_t sizes[1];
    Error *local_err = NULL;

    assert(s32k_to_float || attr->count == 3);
    assert(source_stride != 0);

    if (max_element < min_element) {
        error_setg(errp,
                   "Direct3D 12 cannot CPU-convert vertex attribute %u: "
                   "invalid element range %u..%u for type=%s (%u), "
                   "stride=%u, inline_data=%d",
                   attr_index, min_element, max_element,
                   vertex_attribute_format_name(attr->format), attr->format,
                   source_stride, inline_data);
        return false;
    }

    if (umul64_overflow(max_element, source_stride, &required_source_end) ||
        uadd64_overflow(required_source_end, source_base,
                        &required_source_end) ||
        uadd64_overflow(required_source_end, src_element_size,
                        &required_source_end) ||
        required_source_end > source_size) {
        error_setg(errp,
                   "Direct3D 12 cannot CPU-convert vertex attribute %u: "
                   "source range is invalid for %s; type=%s (%u), count=%u, "
                   "src_element_bytes=%u, src_stride=%u, source_base=0x%"
                   PRIx64 ", source_size=0x%" PRIx64 ", element_range=%u..%u, "
                   "inline_data=%d, dma=%c, guest_offset=0x%" HWADDR_PRIx,
                   attr_index, cpu_expansion_kind_name(attr),
                   vertex_attribute_format_name(attr->format), attr->format,
                   attr->count, src_element_size, source_stride, source_base,
                   source_size, min_element, max_element, inline_data,
                   attr->dma_select ? 'B' : 'A', (hwaddr)attr->offset);
        return false;
    }

    if (umul64_overflow(element_count, converted_stride, &converted_size) ||
        converted_size > SIZE_MAX) {
        error_setg(errp,
                   "Direct3D 12 cannot CPU-convert vertex attribute %u: "
                   "converted size overflow for %s; elements=%" PRIu64
                   ", dst_stride=%u",
                   attr_index, cpu_expansion_kind_name(attr), element_count,
                   converted_stride);
        return false;
    }

    converted_data = g_malloc(converted_size);
    for (uint64_t i = 0; i < element_count; i++) {
        const uint8_t *src = source + source_base +
                             (first_element + i) * source_stride;
        uint8_t *dst = converted_data + i * converted_stride;

        if (s32k_to_float) {
            const int16_t *src16 = (const int16_t *)src;
            float *dst_float = (float *)dst;

            for (uint32_t c = 0; c < attr->count; c++) {
                dst_float[c] = (float)src16[c];
            }
        } else {
            expand_vertex_attribute_3_to_4(attr, src, dst);
        }
    }

    chunks[0] = converted_data;
    sizes[0] = converted_size;
    if (!pgraph_d3d12_upload_vertex_inline_buffer(
            pg, chunks, sizes, ARRAY_SIZE(chunks), alignment, dst_offset,
            &local_err)) {
        error_setg(errp,
                   "Direct3D 12 failed to upload CPU-converted vertex "
                   "attribute %u as %s: type=%s (%u), element_range=%u..%u, "
                   "src_stride=%u, dst_stride=%u, converted_size=%" PRIu64
                   ": %s",
                   attr_index, cpu_expansion_kind_name(attr),
                   vertex_attribute_format_name(attr->format), attr->format,
                   min_element, max_element, source_stride, converted_stride,
                   converted_size,
                   local_err ? error_get_pretty(local_err) : "unknown error");
        error_free(local_err);
        return false;
    }

    *dst_stride = converted_stride;

    return true;
}

static void queue_vertex_ram_sync(NV2AState *d, hwaddr addr, hwaddr size)
{
    PGRAPHD3D12State *r = d->pgraph.d3d12_renderer_state;

    if (size == 0) {
        return;
    }

    assert(r->num_vertex_ram_buffer_syncs <
           ARRAY_SIZE(r->vertex_ram_buffer_syncs));
    r->vertex_ram_buffer_syncs[r->num_vertex_ram_buffer_syncs++] =
        (PGRAPHD3D12MemorySync){ .addr = addr, .size = size };
}

typedef struct VertexRamSpan {
    uint8_t *dma_data;
    hwaddr dma_len;
    /* Attribute base and the bytes the draw reads, as guest RAM offsets. */
    hwaddr addr;
    hwaddr read_start;
    hwaddr read_size;
} VertexRamSpan;

/*
 * Guest RAM span a RAM-sourced attribute reads for the element range.
 * Converted attributes are rebuilt from element zero on the input
 * assembler path; pulling only needs the draw range. The span must lie in
 * guest RAM: the mirror is read through an unbounded root SRV, and the
 * dirty-tracking and provoking-vertex reads index guest memory directly.
 */
static bool vertex_attribute_ram_span(NV2AState *d, int index,
                                      const VertexAttribute *attr,
                                      bool needs_cpu_expansion,
                                      unsigned int min_element,
                                      unsigned int max_element,
                                      VertexRamSpan *span, Error **errp)
{
    PGRAPHState *pg = &d->pgraph;
    static bool reported_ram_overflow;
    uint64_t vram_size = memory_region_size(d->vram);
    uint64_t stride = attr->stride;
    uint64_t first_element =
        needs_cpu_expansion && !pgraph_d3d12_vertex_pulling() ?
            0 : min_element;
    uint64_t last_element = stride ? max_element : first_element;
    uint64_t element_size = (uint64_t)attr->size * attr->count;
    uint64_t read_start64;
    uint64_t read_end64;

    span->dma_data = (uint8_t *)nv_dma_map(
        d, attr->dma_select ? pg->dma_vertex_b : pg->dma_vertex_a,
        &span->dma_len);
    /* The DMA limit is the offset of the object's last byte. */
    if (span->dma_data == NULL || attr->offset > span->dma_len) {
        error_setg(errp,
                   "Direct3D 12 vertex attribute %d DMA range is "
                   "invalid: dma=%c, offset=0x%" HWADDR_PRIx
                   ", dma_len=0x%" HWADDR_PRIx ", stride=%u, "
                   "element_range=%u..%u",
                   index, attr->dma_select ? 'B' : 'A',
                   (hwaddr)attr->offset, span->dma_len, attr->stride,
                   min_element, max_element);
        return false;
    }

    span->addr = span->dma_data + attr->offset - d->vram_ptr;

    if (umul64_overflow(first_element, stride, &read_start64) ||
        uadd64_overflow(read_start64, span->addr, &read_start64) ||
        umul64_overflow(last_element, stride, &read_end64) ||
        uadd64_overflow(read_end64, span->addr, &read_end64) ||
        uadd64_overflow(read_end64, element_size, &read_end64) ||
        read_end64 < read_start64) {
        error_setg(errp,
                   "Direct3D 12 vertex attribute %d source range "
                   "overflows: base=0x%" HWADDR_PRIx ", stride=%" PRIu64
                   ", elements=%" PRIu64 "..%" PRIu64 ", bytes=%" PRIu64,
                   index, span->addr, stride, first_element, last_element,
                   element_size);
        return false;
    }

    if (read_end64 > vram_size) {
        /* Malformed indices or offsets; the draw is dropped. Reported once
         * so a title doing this every frame does not flood the log. */
        if (errp != NULL && !reported_ram_overflow) {
            reported_ram_overflow = true;
            error_setg(errp,
                       "Direct3D 12 vertex attribute %d reads past guest "
                       "RAM, draw skipped (further occurrences are not "
                       "reported): base=0x%" HWADDR_PRIx ", stride=%" PRIu64
                       ", elements=%" PRIu64 "..%" PRIu64 ", end=0x%" PRIx64
                       ", ram=0x%" PRIx64,
                       index, span->addr, stride, first_element,
                       last_element, read_end64, vram_size);
        }
        return false;
    }

    span->read_start = read_start64;
    span->read_size = read_end64 - read_start64;
    return true;
}

/*
 * Writes back stashed render-target images overlapping the draw's vertex
 * spans. It runs before any vertex byte is read and before the draw takes
 * ring space: a write-back submits and waits, and it clears the pages in
 * uploaded_bitmap so the following sync re-uploads them. Live render targets
 * aliased as vertex data are not downloaded (a per-attribute readback would
 * drain the queue); that needs a GPU-side copy.
 */
static void complete_vertex_stashes(NV2AState *d, unsigned int min_element,
                                    unsigned int max_element)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;

    if (QTAILQ_EMPTY(&r->stashed_surfaces)) {
        return;
    }

    for (int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
        const VertexAttribute *attr = &pg->vertex_attributes[i];
        DXGI_FORMAT format;
        bool needs_shader_conversion;
        bool needs_cpu_expansion;
        bool d3d_swizzle;
        VertexRamSpan span;

        /* Invalid attributes are reported by the bind that follows. */
        if (!attr->count ||
            !dxgi_format_for_attribute(attr, &format,
                                       &needs_shader_conversion,
                                       &needs_cpu_expansion, &d3d_swizzle) ||
            !vertex_attribute_ram_span(d, i, attr, needs_cpu_expansion,
                                       min_element, max_element, &span,
                                       NULL)) {
            continue;
        }

        pgraph_d3d12_complete_readbacks_in_range(
            d, span.read_start, span.read_size,
            PGRAPH_D3D12_STASH_TRIGGER_RANGE);
        if (QTAILQ_EMPTY(&r->stashed_surfaces)) {
            return;
        }
    }
}

bool pgraph_d3d12_bind_vertex_attributes(NV2AState *d,
                                         unsigned int min_element,
                                         unsigned int max_element,
                                         bool inline_data,
                                         unsigned int inline_stride,
                                         unsigned int provoking_element,
                                         Error **errp)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;

    pg->compressed_attrs = 0;
    pg->uniform_attrs = 0;
    pg->swizzle_attrs = 0;

    r->num_active_vertex_attribute_descriptions = 0;
    r->num_active_vertex_buffer_views = 0;
    r->converted_vertex_attribute_mask = 0;
    r->vertex_fetch_inline_mask = 0;
    r->vertex_fetch_ring_base = 0;

    if (!inline_data) {
        complete_vertex_stashes(d, min_element, max_element);
    }

    for (int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
        VertexAttribute *attr = &pg->vertex_attributes[i];
        DXGI_FORMAT format;
        bool needs_shader_conversion;
        bool needs_cpu_expansion;
        bool d3d_swizzle;
        hwaddr attrib_data_addr;
        hwaddr start = 0;
        size_t stride;
        const uint8_t *source_data = NULL;
        uint64_t source_size = 0;
        uint64_t source_base = 0;
        uint32_t provoking_element_index;
        const uint8_t *last_entry;

        r->vertex_attribute_to_description_location[i] = -1;
        r->vertex_fetch_raw_bits[i] = 0;

        if (!attr->count) {
            pg->uniform_attrs |= 1 << i;
            continue;
        }

        if (!dxgi_format_for_attribute(attr, &format, &needs_shader_conversion,
                                       &needs_cpu_expansion, &d3d_swizzle)) {
            error_setg(errp,
                       "Direct3D 12 unsupported vertex attribute %d: "
                       "type=%s (%u), count=%u, stride=%u, inline_data=%d, "
                       "element_range=%u..%u, dma=%c, offset=0x%" HWADDR_PRIx,
                       i, vertex_attribute_format_name(attr->format),
                       attr->format, attr->count, attr->stride, inline_data,
                       min_element, max_element,
                       attr->dma_select ? 'B' : 'A', (hwaddr)attr->offset);
            return false;
        }

        /* The run-time decode reads these formats from RAM itself: no
         * per-draw conversion and upload (and the draw can merge). */
        if (needs_cpu_expansion && !inline_data && attr->stride &&
            pgraph_d3d12_vertex_pulling() && !r->raw_fetch_off &&
            !(r->tune_flags & PGRAPH_D3D12_TUNE_VTXFMT_STATIC)) {
            r->vertex_fetch_raw_bits[i] =
                pgraph_d3d12_vertex_fetch_raw_bits(attr, d3d_swizzle);
            needs_cpu_expansion = r->vertex_fetch_raw_bits[i] == 0;
        }

        if (inline_data) {
            attrib_data_addr = attr->inline_array_offset;
            stride = inline_stride;
            source_data = (const uint8_t *)pg->inline_array;
            source_size = pg->inline_array_length * sizeof(pg->inline_array[0]);
            source_base = attr->inline_array_offset;
        } else {
            VertexRamSpan span;

            /* Also bounds the provoking-vertex and CPU-conversion reads,
             * which stay inside the span. */
            if (!vertex_attribute_ram_span(d, i, attr, needs_cpu_expansion,
                                           min_element, max_element, &span,
                                           errp)) {
                return false;
            }

            attrib_data_addr = span.addr;
            stride = attr->stride;
            /* Converted reads are bounded by guest RAM, as the span is;
             * the DMA limit only bounds the attribute base. */
            source_data = d->vram_ptr;
            source_size = memory_region_size(d->vram);
            source_base = span.addr;

            start = attrib_data_addr + (uint64_t)min_element * stride;
            if (!needs_cpu_expansion) {
                queue_vertex_ram_sync(d, span.read_start, span.read_size);
            }
        }

        provoking_element_index = provoking_element - min_element;
        if (inline_data) {
            last_entry =
                (const uint8_t *)pg->inline_array + attr->inline_array_offset;
        } else {
            last_entry = d->vram_ptr + start;
        }

        if (!stride) {
            pg->uniform_attrs |= 1 << i;
            pgraph_update_inline_value(attr, last_entry);
            continue;
        }

        last_entry += stride * provoking_element_index;
        pgraph_update_inline_value(attr, last_entry);

        if (needs_cpu_expansion) {
            uint64_t converted_offset;
            uint32_t converted_stride;

            if (!upload_converted_vertex_attribute(
                    d, i, attr, source_data, source_size, source_base, stride,
                    min_element, max_element, inline_data, &converted_offset,
                    &converted_stride, errp)) {
                return false;
            }

            r->converted_vertex_attribute_mask |= 1u << i;
            attrib_data_addr = converted_offset;
            if (pgraph_d3d12_vertex_pulling()) {
                /* The shader indexes from element zero; wrap the base so
                 * element min_element lands at converted_offset. */
                attrib_data_addr -= (uint64_t)min_element * converted_stride;
            }
            stride = converted_stride;
        }

        r->vertex_attribute_to_description_location[i] =
            r->num_active_vertex_attribute_descriptions;
        r->vertex_attribute_descriptions
            [r->num_active_vertex_attribute_descriptions++] =
            (D3D12_INPUT_ELEMENT_DESC){
                .SemanticName = "TEXCOORD",
                .SemanticIndex = i,
                .Format = format,
                .InputSlot = r->vertex_attribute_to_description_location[i],
                .AlignedByteOffset = 0,
                .InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
                .InstanceDataStepRate = 0,
            };

        r->vertex_attribute_offsets[i] = attrib_data_addr;
        r->vertex_attribute_strides[i] = stride;
        /* Views are clamped to this draw's fetch span: the Xbox UWP driver
         * mis-fetches vertices through very large buffer views. */
        r->vertex_attribute_spans[i] =
            (uint64_t)max_element * stride +
            (needs_cpu_expansion ? stride :
                                   (uint64_t)attr->size * attr->count);

        if (needs_shader_conversion) {
            pg->compressed_attrs |= 1 << i;
        }
        if (d3d_swizzle) {
            pg->swizzle_attrs |= 1 << i;
        }
    }

    return true;
}

void pgraph_d3d12_bind_vertex_attributes_inline(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;

    pg->compressed_attrs = 0;
    pg->uniform_attrs = 0;
    pg->swizzle_attrs = 0;

    r->num_active_vertex_attribute_descriptions = 0;
    r->num_active_vertex_buffer_views = 0;
    r->converted_vertex_attribute_mask = 0;
    r->vertex_fetch_inline_mask = 0xffff;
    r->vertex_fetch_ring_base = 0;

    for (int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
        VertexAttribute *attr = &pg->vertex_attributes[i];

        if (attr->inline_buffer_populated) {
            r->vertex_attribute_to_description_location[i] =
                r->num_active_vertex_attribute_descriptions;
            r->vertex_attribute_descriptions
                [r->num_active_vertex_attribute_descriptions++] =
                (D3D12_INPUT_ELEMENT_DESC){
                    .SemanticName = "TEXCOORD",
                    .SemanticIndex = i,
                    .Format = DXGI_FORMAT_R32G32B32A32_FLOAT,
                    .InputSlot =
                        r->vertex_attribute_to_description_location[i],
                    .AlignedByteOffset = 0,
                    .InputSlotClass =
                        D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
                    .InstanceDataStepRate = 0,
                };
            r->vertex_attribute_offsets[i] = 0;
            r->vertex_attribute_strides[i] = 4 * sizeof(float);
            r->vertex_attribute_spans[i] =
                (uint64_t)pg->inline_buffer_length * 4 * sizeof(float);
            memcpy(attr->inline_value,
                   attr->inline_buffer + (pg->inline_buffer_length - 1) * 4,
                   sizeof(attr->inline_value));
        } else {
            r->vertex_attribute_to_description_location[i] = -1;
            pg->uniform_attrs |= 1 << i;
        }
    }
}

/*
 * A draw that rasterizes nothing still leaves each array attribute's current
 * value at its provoking element, as a recorded draw does. Nothing is bound
 * or uploaded.
 */
void pgraph_d3d12_update_provoking_attribute_values(
    NV2AState *d, unsigned int provoking_element, bool inline_data,
    unsigned int inline_stride)
{
    PGRAPHState *pg = &d->pgraph;
    uint64_t vram_size = memory_region_size(d->vram);
    uint64_t inline_size =
        (uint64_t)pg->inline_array_length * sizeof(pg->inline_array[0]);

    for (int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
        VertexAttribute *attr = &pg->vertex_attributes[i];
        uint64_t element_size = (uint64_t)attr->size * attr->count;
        DXGI_FORMAT format;
        bool needs_shader_conversion;
        bool needs_cpu_expansion;
        bool d3d_swizzle;
        const uint8_t *entry;

        if (!attr->count ||
            !dxgi_format_for_attribute(attr, &format, &needs_shader_conversion,
                                       &needs_cpu_expansion, &d3d_swizzle)) {
            continue;
        }

        if (inline_data) {
            uint64_t offset = attr->inline_array_offset +
                              (uint64_t)provoking_element * inline_stride;

            if (offset + element_size > inline_size) {
                continue;
            }
            entry = (const uint8_t *)pg->inline_array + offset;
        } else {
            hwaddr dma_len;
            uint8_t *dma_data = (uint8_t *)nv_dma_map(
                d, attr->dma_select ? pg->dma_vertex_b : pg->dma_vertex_a,
                &dma_len);
            uint64_t addr;

            if (dma_data == NULL || attr->offset > dma_len) {
                continue;
            }
            /* A zero stride repeats the first element. */
            addr = dma_data + attr->offset - d->vram_ptr +
                   (uint64_t)provoking_element * attr->stride;
            if (addr + element_size > vram_size) {
                continue;
            }
            entry = d->vram_ptr + addr;
        }

        pgraph_update_inline_value(attr, entry);
    }
}

static void fill_vertex_buffer_views(PGRAPHState *pg, uint16_t inline_map,
                                     uint64_t offset)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;

    r->num_active_vertex_buffer_views = 0;

    for (int i = 0; i < r->num_active_vertex_attribute_descriptions; i++) {
        int attr_idx = r->vertex_attribute_descriptions[i].SemanticIndex;
        bool converted = r->converted_vertex_attribute_mask & (1u << attr_idx);
        PGRAPHD3D12Buffer *buffer =
            ((inline_map & (1 << attr_idx)) || converted) ?
                &r->vertex_inline_upload_buffer :
                &r->vertex_ram_buffer;
        uint64_t buffer_offset = converted ?
            r->vertex_attribute_offsets[attr_idx] :
            offset + r->vertex_attribute_offsets[attr_idx];

        if (buffer->resource == NULL || buffer_offset >= buffer->size) {
            continue;
        }

        uint64_t view_size = buffer->size - buffer_offset;
        uint64_t span = r->vertex_attribute_spans[attr_idx];

        if (span != 0 && span < view_size) {
            view_size = span;
        }

        r->vertex_buffer_views[r->num_active_vertex_buffer_views++] =
            (D3D12_VERTEX_BUFFER_VIEW){
                .BufferLocation =
                    buffer->resource->lpVtbl->GetGPUVirtualAddress(
                        buffer->resource) + buffer_offset,
                .SizeInBytes = (UINT)MIN(view_size, (uint64_t)UINT32_MAX),
                .StrideInBytes = r->vertex_attribute_strides[attr_idx],
            };
    }
}

void pgraph_d3d12_record_vertex_buffers(PGRAPHState *pg, uint16_t inline_map,
                                        uint64_t offset)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;

    if (pgraph_d3d12_vertex_pulling()) {
        /* Draws read vertices through buffer SRVs; nothing to bind. */
        return;
    }

    fill_vertex_buffer_views(pg, inline_map, offset);
    if (r->command_list == NULL || !r->command_list_open ||
        r->num_active_vertex_buffer_views == 0) {
        return;
    }

    /* Copies leave the buffer in COPY_DEST; transition it once per
     * copy/draw alternation rather than per copy. */
    pgraph_d3d12_transition_resource(
        r, &r->vertex_ram_buffer,
        D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);

    r->command_list->lpVtbl->IASetVertexBuffers(
        r->command_list, 0, r->num_active_vertex_buffer_views,
        r->vertex_buffer_views);
}

void pgraph_d3d12_record_inline_vertex_buffers(PGRAPHState *pg,
                                               uint64_t offset)
{
    pgraph_d3d12_record_vertex_buffers(pg, 0xffff, offset);
}
