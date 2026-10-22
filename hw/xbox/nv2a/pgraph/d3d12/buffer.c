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
#include "qemu/bitmap.h"
#include "qapi/error.h"
#include "renderer.h"

#define D3D12_STAGING_BUFFER_SIZE (16 * 1024 * 1024)
#define D3D12_UNIFORM_BUFFER_SIZE (32 * 1024 * 1024)
/* 32-bit indices: 8 MiB holds about two million per frame before the ring
 * waits on a fence. */
#define D3D12_INDEX_BUFFER_SIZE (8 * 1024 * 1024)
#define D3D12_VERTEX_INLINE_BUFFER_SIZE (8 * 1024 * 1024)

static void release_buffer(PGRAPHD3D12Buffer *buffer)
{
    if (buffer->resource != NULL && buffer->mapped != NULL) {
        buffer->resource->lpVtbl->Unmap(buffer->resource, 0, NULL);
    }

    D3D12_RELEASE(buffer->resource);
    buffer->mapped = NULL;
    buffer->offset = 0;
    buffer->size = 0;
    buffer->state = D3D12_RESOURCE_STATE_COMMON;
}

static void create_buffer(PGRAPHD3D12State *r, PGRAPHD3D12Buffer *buffer,
                          uint64_t size, D3D12_HEAP_TYPE heap_type,
                          D3D12_RESOURCE_STATES initial_state,
                          bool map_cpu, Error **errp)
{
    D3D12_HEAP_PROPERTIES heap_props;
    D3D12_RESOURCE_DESC desc;
    HRESULT hr;

    memset(&heap_props, 0, sizeof(heap_props));
    heap_props.Type = heap_type;
    heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heap_props.CreationNodeMask = 1;
    heap_props.VisibleNodeMask = 1;

    memset(&desc, 0, sizeof(desc));
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    hr = r->device->lpVtbl->CreateCommittedResource(
        r->device, &heap_props, D3D12_HEAP_FLAG_NONE, &desc, initial_state,
        NULL, &IID_ID3D12Resource, (void **)&buffer->resource);
    if (FAILED(hr)) {
        error_setg(errp,
                   "Failed to create Direct3D 12 buffer (%" PRIu64
                   " bytes): HRESULT 0x%08lx",
                   size, (unsigned long)hr);
        return;
    }

    buffer->size = size;
    buffer->state = initial_state;
    buffer->offset = 0;

    if (map_cpu) {
        D3D12_RANGE read_range = { 0, 0 };
        hr = buffer->resource->lpVtbl->Map(buffer->resource, 0, &read_range,
                                           &buffer->mapped);
        if (FAILED(hr)) {
            error_setg(errp,
                       "Failed to map Direct3D 12 buffer: HRESULT 0x%08lx",
                       (unsigned long)hr);
            release_buffer(buffer);
            return;
        }
    }
}

static bool require_renderer(PGRAPHState *pg, PGRAPHD3D12State **rp,
                             Error **errp)
{
    PGRAPHD3D12State *r = pg != NULL ? pg->d3d12_renderer_state : NULL;

    if (r == NULL) {
        error_setg(errp, "Direct3D 12 renderer is not initialized");
        return false;
    }

    *rp = r;
    return true;
}

bool pgraph_d3d12_ensure_buffer_size(PGRAPHState *pg,
                                     PGRAPHD3D12Buffer *buffer,
                                     uint64_t required_size,
                                     D3D12_HEAP_TYPE heap_type,
                                     D3D12_RESOURCE_STATES initial_state,
                                     bool map_cpu, const char *label,
                                     Error **errp)
{
    PGRAPHD3D12State *r;
    PGRAPHD3D12Buffer new_buffer = { 0 };
    Error *local_err = NULL;
    Error **resize_errp = errp ? errp : &local_err;
    uint64_t old_size;
    uint64_t new_size;

    if (!require_renderer(pg, &r, errp)) {
        return false;
    }

    if (buffer == NULL) {
        error_setg(errp,
                   "Direct3D 12 cannot grow %s buffer: buffer is NULL",
                   label ? label : "staging");
        return false;
    }

    if (required_size <= buffer->size) {
        return true;
    }

    old_size = buffer->size;
    new_size = ROUND_UP(required_size, D3D12_STAGING_BUFFER_SIZE);
    if (new_size < required_size) {
        new_size = required_size;
    }

    create_buffer(r, &new_buffer, new_size, heap_type, initial_state, map_cpu,
                  resize_errp);
    if (*resize_errp) {
        if (errp) {
            error_prepend(errp,
                          "Direct3D 12 failed to grow %s buffer from %" PRIu64
                          " to %" PRIu64 " bytes for required size %" PRIu64
                          ": ",
                          label ? label : "staging", old_size, new_size,
                          required_size);
        } else {
            error_free(local_err);
        }
        return false;
    }

    /* In-flight commands may still reference the old resource. */
    if (buffer->resource != NULL && buffer->mapped != NULL) {
        buffer->resource->lpVtbl->Unmap(buffer->resource, 0, NULL);
    }
    pgraph_d3d12_defer_release(pg, buffer->resource, -1, -1, -1, -1);
    buffer->resource = NULL;
    buffer->mapped = NULL;
    *buffer = new_buffer;

    fprintf(stderr,
            "Direct3D 12 renderer: grew %s buffer from %" PRIu64
            " to %" PRIu64 " bytes for required size %" PRIu64 "\n",
            label ? label : "staging", old_size, new_size, required_size);
    return true;
}

void pgraph_d3d12_init_buffers(NV2AState *d, Error **errp)
{
    PGRAPHD3D12State *r = d->pgraph.d3d12_renderer_state;
    uint64_t vram_size = memory_region_size(d->vram);

    create_buffer(r, &r->upload_buffer, D3D12_STAGING_BUFFER_SIZE,
                  D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ,
                  true, errp);
    if (*errp) {
        return;
    }

    create_buffer(r, &r->readback_buffer, D3D12_STAGING_BUFFER_SIZE,
                  D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST,
                  true, errp);
    if (*errp) {
        return;
    }

    create_buffer(r, &r->vertex_ram_buffer, vram_size, D3D12_HEAP_TYPE_DEFAULT,
                  D3D12_RESOURCE_STATE_COPY_DEST, false, errp);
    if (*errp) {
        return;
    }

    create_buffer(r, &r->index_upload_buffer, D3D12_INDEX_BUFFER_SIZE,
                  D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ,
                  true, errp);
    if (*errp) {
        return;
    }

    create_buffer(r, &r->vertex_inline_upload_buffer,
                  D3D12_VERTEX_INLINE_BUFFER_SIZE, D3D12_HEAP_TYPE_UPLOAD,
                  D3D12_RESOURCE_STATE_GENERIC_READ, true, errp);
    if (*errp) {
        return;
    }

    create_buffer(r, &r->uniform_upload_buffer, D3D12_UNIFORM_BUFFER_SIZE,
                  D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ,
                  true, errp);
    if (*errp) {
        return;
    }

    r->bitmap_size = DIV_ROUND_UP(vram_size, TARGET_PAGE_SIZE);
    r->uploaded_bitmap = bitmap_new(r->bitmap_size);
    bitmap_clear(r->uploaded_bitmap, 0, r->bitmap_size);
}

void pgraph_d3d12_finalize_buffers(PGRAPHState *pg)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;

    if (r == NULL) {
        return;
    }

    release_buffer(&r->uniform_upload_buffer);
    release_buffer(&r->vertex_inline_upload_buffer);
    release_buffer(&r->index_upload_buffer);
    release_buffer(&r->readback_buffer);
    release_buffer(&r->upload_buffer);
    release_buffer(&r->vertex_ram_buffer);
    g_free(r->expanded_index_scratch);
    r->expanded_index_scratch = NULL;
    r->expanded_index_scratch_capacity = 0;
    g_free(r->uploaded_bitmap);
    r->uploaded_bitmap = NULL;
    r->bitmap_size = 0;
}

bool pgraph_d3d12_buffer_has_space_for(PGRAPHD3D12Buffer *buffer,
                                       uint64_t size, uint64_t alignment)
{
    if (buffer == NULL) {
        return false;
    }

    if (alignment == 0) {
        alignment = 1;
    }

    uint64_t offset = ROUND_UP(buffer->offset, alignment);

    return buffer->mapped != NULL && offset <= buffer->size &&
           size <= buffer->size - offset;
}

static void ring_reset(PGRAPHD3D12Buffer *buffer)
{
    buffer->offset = 0;
    buffer->ring_vhead = 0;
    buffer->ring_vtail = 0;
    buffer->ring_recorded_vhead = 0;
    buffer->ring_mark_head = 0;
    buffer->ring_mark_count = 0;
}

static bool reset_surface_transfer_buffer(PGRAPHD3D12State *r,
                                          PGRAPHD3D12Buffer *buffer,
                                          uint64_t target_size,
                                          D3D12_HEAP_TYPE heap_type,
                                          D3D12_RESOURCE_STATES initial_state,
                                          bool ring, const char *label,
                                          Error **errp)
{
    PGRAPHD3D12Buffer replacement = { 0 };
    uint64_t old_size = buffer->size;

    if (buffer->resource != NULL && buffer->mapped != NULL &&
        buffer->size <= target_size) {
        if (ring) {
            ring_reset(buffer);
        }
        return true;
    }

    /*
     * Called only at GPU idle. Create the replacement before releasing the
     * old one so a failed reset never leaves the renderer without a buffer.
     */
    create_buffer(r, &replacement, target_size, heap_type, initial_state,
                  true, errp);
    if (replacement.resource == NULL || replacement.mapped == NULL) {
        if (errp != NULL && *errp != NULL) {
            error_prepend(errp,
                          "Direct3D 12 could not reset %s buffer from %"
                          PRIu64 " to %" PRIu64 " bytes: ",
                          label, old_size, target_size);
        }
        return false;
    }

    release_buffer(buffer);
    *buffer = replacement;
    if (ring) {
        ring_reset(buffer);
    }
    fprintf(stderr,
            "Direct3D 12 renderer: reset %s buffer from %" PRIu64
            " to %" PRIu64 " bytes after surface-scale change\n",
            label, old_size, target_size);
    return true;
}

bool pgraph_d3d12_reset_surface_transfer_buffers(PGRAPHState *pg,
                                                  uint64_t target_size,
                                                  Error **errp)
{
    PGRAPHD3D12State *r;
    uint64_t completed;

    if (!require_renderer(pg, &r, errp)) {
        return false;
    }
    if (target_size == 0) {
        error_setg(errp,
                   "Direct3D 12 surface transfer buffer reset size is zero");
        return false;
    }

    /* Resetting mapped resources is safe only at the explicit GPU-idle point. */
    if (r->command_list_open) {
        error_setg(errp,
                   "Direct3D 12 cannot reset surface transfer buffers while "
                   "a command list is open");
        return false;
    }
    if (r->fence != NULL) {
        completed = r->fence->lpVtbl->GetCompletedValue(r->fence);
        if (completed < r->submitted_fence_value) {
            error_setg(errp,
                       "Direct3D 12 cannot reset surface transfer buffers "
                       "before GPU idle: completed fence=%" PRIu64
                       ", submitted fence=%" PRIu64,
                       completed, r->submitted_fence_value);
            return false;
        }
    }

    pgraph_d3d12_buffers_retire_completed(r);
    if (r->upload_buffer.ring_mark_count != 0) {
        error_setg(errp,
                   "Direct3D 12 cannot reset surface upload buffer with %u "
                   "unretired fence marks",
                   r->upload_buffer.ring_mark_count);
        return false;
    }

    if (!reset_surface_transfer_buffer(
            r, &r->upload_buffer, target_size, D3D12_HEAP_TYPE_UPLOAD,
            D3D12_RESOURCE_STATE_GENERIC_READ, true, "surface upload", errp)) {
        return false;
    }
    if (!reset_surface_transfer_buffer(
            r, &r->readback_buffer, target_size, D3D12_HEAP_TYPE_READBACK,
            D3D12_RESOURCE_STATE_COPY_DEST, false, "surface readback", errp)) {
        return false;
    }

    return true;
}

static void ring_retire_completed(PGRAPHD3D12State *r,
                                  PGRAPHD3D12Buffer *buffer)
{
    uint64_t completed;

    if (r->fence == NULL || buffer->ring_mark_count == 0) {
        return;
    }

    completed = r->fence->lpVtbl->GetCompletedValue(r->fence);
    while (buffer->ring_mark_count > 0) {
        PGRAPHD3D12BufferFenceMark *mark =
            &buffer->ring_marks[buffer->ring_mark_head];

        if (mark->fence_value > completed) {
            break;
        }
        buffer->ring_vtail = mark->vhead;
        buffer->ring_mark_head = (buffer->ring_mark_head + 1) %
                                 PGRAPH_D3D12_BUFFER_FENCE_MARKS;
        buffer->ring_mark_count--;
    }
}

static void ring_push_mark(PGRAPHD3D12Buffer *buffer, uint64_t fence_value,
                           uint64_t submitted_vhead)
{
    PGRAPHD3D12BufferFenceMark *last;
    uint32_t slot;

    if (buffer->size == 0 || buffer->mapped == NULL) {
        return;
    }

    assert(submitted_vhead >= buffer->ring_vtail);
    assert(submitted_vhead <= buffer->ring_vhead);

    if (buffer->ring_mark_count == 0 &&
        submitted_vhead == buffer->ring_vtail) {
        return;
    }

    if (buffer->ring_mark_count > 0) {
        last = &buffer->ring_marks[(buffer->ring_mark_head +
                                    buffer->ring_mark_count - 1) %
                                   PGRAPH_D3D12_BUFFER_FENCE_MARKS];
        assert(submitted_vhead >= last->vhead);
        if (last->vhead == submitted_vhead) {
            return;
        }
        if (buffer->ring_mark_count == PGRAPH_D3D12_BUFFER_FENCE_MARKS) {
            /*
             * Coalesce into the newest mark; retirement happens later but
             * frees at least as much space.
             */
            last->vhead = submitted_vhead;
            last->fence_value = fence_value;
            return;
        }
    }

    slot = (buffer->ring_mark_head + buffer->ring_mark_count) %
           PGRAPH_D3D12_BUFFER_FENCE_MARKS;
    buffer->ring_marks[slot].fence_value = fence_value;
    buffer->ring_marks[slot].vhead = submitted_vhead;
    buffer->ring_mark_count++;
}

static bool ring_try_place(PGRAPHD3D12Buffer *buffer, uint64_t size,
                           uint64_t alignment, uint64_t *vstart_out)
{
    uint64_t phys = buffer->ring_vhead % buffer->size;
    uint64_t aligned_phys = ROUND_UP(phys, alignment);
    uint64_t vstart;

    if (aligned_phys + size > buffer->size) {
        /*
         * Skip the fragment at the end; physical offset zero satisfies any
         * alignment.
         */
        vstart = buffer->ring_vhead + (buffer->size - phys);
    } else {
        vstart = buffer->ring_vhead + (aligned_phys - phys);
    }

    if (vstart + size > buffer->ring_vtail + buffer->size) {
        return false;
    }

    *vstart_out = vstart;
    return true;
}

static bool ring_grow(PGRAPHState *pg, PGRAPHD3D12Buffer *buffer,
                      uint64_t required_size, Error **errp)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    PGRAPHD3D12Buffer new_buffer = { 0 };
    uint64_t new_size = MAX(buffer->size * 2,
                            ROUND_UP(required_size,
                                     D3D12_STAGING_BUFFER_SIZE));

    if (buffer == &r->uniform_upload_buffer) {
        /* Cached constants offsets refer to the replaced resource. */
        r->draw_constants_cached = false;
        r->pixel_constants_cached = false;
        r->uber_constants_cached = false;
    }

    create_buffer(r, &new_buffer, new_size, D3D12_HEAP_TYPE_UPLOAD,
                  D3D12_RESOURCE_STATE_GENERIC_READ, true, errp);
    if (errp != NULL && *errp != NULL) {
        return false;
    }

    fprintf(stderr,
            "Direct3D 12 renderer: grew staging ring buffer from %" PRIu64
            " to %" PRIu64 " bytes for allocation of %" PRIu64 " bytes\n",
            buffer->size, new_size, required_size);

    /* In-flight copies may still read the old resource. */
    if (buffer->resource != NULL && buffer->mapped != NULL) {
        buffer->resource->lpVtbl->Unmap(buffer->resource, 0, NULL);
    }
    pgraph_d3d12_defer_release(pg, buffer->resource, -1, -1, -1, -1);
    buffer->resource = NULL;

    *buffer = new_buffer;
    ring_reset(buffer);
    return true;
}

bool pgraph_d3d12_ring_alloc(PGRAPHState *pg, PGRAPHD3D12Buffer *buffer,
                             uint64_t size, uint64_t alignment,
                             uint64_t *offset, Error **errp)
{
    PGRAPHD3D12State *r;

    if (!require_renderer(pg, &r, errp)) {
        return false;
    }

    if (buffer == NULL || buffer->resource == NULL ||
        buffer->mapped == NULL || buffer->size == 0) {
        error_setg(errp,
                   "Direct3D 12 staging ring buffer is not initialized");
        return false;
    }

    if (alignment == 0) {
        alignment = 1;
    }

    for (;;) {
        uint64_t vstart;

        ring_retire_completed(r, buffer);
        if (buffer->ring_mark_count == 0 &&
            buffer->ring_vhead == buffer->ring_vtail) {
            /* Fully retired: rebase so a buffer-sized request fits at zero. */
            ring_reset(buffer);
        }
        if (ring_try_place(buffer, size, alignment, &vstart)) {
            buffer->ring_vhead = vstart + size;
            buffer->offset = vstart % buffer->size;
            if (offset != NULL) {
                *offset = buffer->offset;
            }
            return true;
        }

        if (buffer->ring_mark_count > 0) {
            uint64_t oldest =
                buffer->ring_marks[buffer->ring_mark_head].fence_value;
            int64_t wait_start = g_get_monotonic_time();
            bool wait_ok;

            wait_ok = pgraph_d3d12_wait_for_fence(pg, oldest,
                                                  "staging ring");
            r->perf.ring_waits++;
            r->perf.ring_wait_us += g_get_monotonic_time() - wait_start;
            if (!wait_ok) {
                error_setg(errp,
                           "Direct3D 12 staging ring wait failed: "
                           "fence=%" PRIu64 ", need=%" PRIu64
                           ", buffer=%" PRIu64,
                           oldest, size, buffer->size);
                return false;
            }
            continue;
        }

        /* No marks to wait on: submit the open list to create one, else grow. */
        if (r->command_list_open) {
            r->perf.submit_src[0]++;
            if (!pgraph_d3d12_submit_commands_async(pg, errp)) {
                return false;
            }
            continue;
        }

        /* Growing would invalidate offsets of allocations not yet consumed by
         * a recorded Draw; fail instead of corrupting geometry. */
        if (buffer != &r->upload_buffer &&
            buffer->ring_recorded_vhead != buffer->ring_vhead) {
            error_setg(errp,
                       "Direct3D 12 draw upload ring exhausted with "
                       "unrecorded allocations: recorded=%" PRIu64
                       ", head=%" PRIu64 ", need=%" PRIu64,
                       buffer->ring_recorded_vhead, buffer->ring_vhead, size);
            return false;
        }

        /* A new ring starts at offset zero, so no alignment slack is needed. */
        if (!ring_grow(pg, buffer, size, errp)) {
            return false;
        }
    }
}

void pgraph_d3d12_buffers_on_submit(PGRAPHD3D12State *r, uint64_t fence_value)
{
    /*
     * upload_buffer is consumed by Copy* commands as it is written, so its
     * whole head is fenced. The draw rings are read by Draw commands, so only
     * the head reached by a recorded draw is fenced.
     */
    ring_push_mark(&r->upload_buffer, fence_value,
                   r->upload_buffer.ring_vhead);
    ring_push_mark(&r->index_upload_buffer, fence_value,
                   r->index_upload_buffer.ring_recorded_vhead);
    ring_push_mark(&r->vertex_inline_upload_buffer, fence_value,
                   r->vertex_inline_upload_buffer.ring_recorded_vhead);
    ring_push_mark(&r->uniform_upload_buffer, fence_value,
                   r->uniform_upload_buffer.ring_recorded_vhead);
}

static void ring_discard_unrecorded(PGRAPHD3D12Buffer *buffer)
{
    assert(buffer->ring_recorded_vhead >= buffer->ring_vtail);
    assert(buffer->ring_recorded_vhead <= buffer->ring_vhead);
    buffer->ring_vhead = buffer->ring_recorded_vhead;
    buffer->offset = buffer->size != 0 ?
                         buffer->ring_vhead % buffer->size : 0;
}

void pgraph_d3d12_discard_unrecorded_draw_uploads(PGRAPHD3D12State *r)
{
    /* Reclaim allocations of a draw that was never recorded. The constant
     * caches are dropped only when uniform space was reclaimed, since only
     * then can they point into it. */
    bool uniform_reclaimed = r->uniform_upload_buffer.ring_vhead !=
                             r->uniform_upload_buffer.ring_recorded_vhead;

    ring_discard_unrecorded(&r->index_upload_buffer);
    ring_discard_unrecorded(&r->vertex_inline_upload_buffer);
    ring_discard_unrecorded(&r->uniform_upload_buffer);
    if (uniform_reclaimed) {
        r->draw_constants_cached = false;
        r->pixel_constants_cached = false;
        r->uber_constants_cached = false;
    }
}

void pgraph_d3d12_draw_uploads_recorded(PGRAPHD3D12State *r)
{
    /* Call only after the Draw/DrawIndexed command has been recorded. */
    r->index_upload_buffer.ring_recorded_vhead =
        r->index_upload_buffer.ring_vhead;
    r->vertex_inline_upload_buffer.ring_recorded_vhead =
        r->vertex_inline_upload_buffer.ring_vhead;
    r->uniform_upload_buffer.ring_recorded_vhead =
        r->uniform_upload_buffer.ring_vhead;
}

void pgraph_d3d12_buffers_retire_completed(PGRAPHD3D12State *r)
{
    ring_retire_completed(r, &r->upload_buffer);
    ring_retire_completed(r, &r->index_upload_buffer);
    ring_retire_completed(r, &r->vertex_inline_upload_buffer);
    ring_retire_completed(r, &r->uniform_upload_buffer);
}

uint64_t pgraph_d3d12_append_to_buffer(PGRAPHD3D12Buffer *buffer,
                                       const void **data,
                                       const uint64_t *sizes,
                                       size_t count, uint64_t alignment)
{
    uint64_t total_size = 0;
    uint64_t starting_offset;

    if (alignment == 0) {
        alignment = 1;
    }

    for (size_t i = 0; i < count; i++) {
        total_size += sizes[i];
    }
    assert(pgraph_d3d12_buffer_has_space_for(buffer, total_size, alignment));

    buffer->offset = ROUND_UP(buffer->offset, alignment);
    starting_offset = buffer->offset;

    for (size_t i = 0; i < count; i++) {
        buffer->offset = ROUND_UP(buffer->offset, alignment);
        if (sizes[i] != 0) {
            assert(data[i] != NULL);
            memcpy((uint8_t *)buffer->mapped + buffer->offset, data[i],
                   sizes[i]);
        }
        buffer->offset += sizes[i];
    }

    return starting_offset;
}

static bool copy_buffer_internal(PGRAPHState *pg, PGRAPHD3D12Buffer *dst,
                                 uint64_t dst_offset,
                                 PGRAPHD3D12Buffer *src,
                                 uint64_t src_offset, uint64_t size,
                                 D3D12_RESOURCE_STATES final_state,
                                 bool submit, Error **errp)
{
    PGRAPHD3D12State *r;

    if (!require_renderer(pg, &r, errp)) {
        return false;
    }

    if (size == 0) {
        return true;
    }

    if (dst == NULL || src == NULL || dst->resource == NULL ||
        src->resource == NULL) {
        error_setg(errp, "Direct3D 12 copy requested for uninitialized buffer");
        return false;
    }

    if (dst_offset > dst->size || size > dst->size - dst_offset ||
        src_offset > src->size || size > src->size - src_offset) {
        error_setg(errp,
                   "Direct3D 12 buffer copy out of range: dst=%" PRIu64
                   "/%" PRIu64 " src=%" PRIu64 "/%" PRIu64
                   " size=%" PRIu64,
                   dst_offset, dst->size, src_offset, src->size, size);
        return false;
    }

    if (!pgraph_d3d12_begin_commands(pg, errp)) {
        return false;
    }

    pgraph_d3d12_transition_resource(r, dst, D3D12_RESOURCE_STATE_COPY_DEST);
    r->command_list->lpVtbl->CopyBufferRegion(r->command_list, dst->resource,
                                              dst_offset, src->resource,
                                              src_offset, size);
    pgraph_d3d12_transition_resource(r, dst, final_state);

    if (submit) {
        return pgraph_d3d12_submit_commands(pg, errp);
    }

    return true;
}

bool pgraph_d3d12_copy_buffer(PGRAPHState *pg, PGRAPHD3D12Buffer *dst,
                              uint64_t dst_offset, PGRAPHD3D12Buffer *src,
                              uint64_t src_offset, uint64_t size,
                              D3D12_RESOURCE_STATES final_state,
                              Error **errp)
{
    return copy_buffer_internal(pg, dst, dst_offset, src, src_offset, size,
                                final_state, true, errp);
}

bool pgraph_d3d12_upload_staged_buffer(PGRAPHState *pg,
                                       PGRAPHD3D12Buffer *dst,
                                       PGRAPHD3D12Buffer *src, uint64_t size,
                                       D3D12_RESOURCE_STATES final_state,
                                       Error **errp)
{
    return pgraph_d3d12_copy_buffer(pg, dst, 0, src, 0, size, final_state,
                                    errp);
}

/*
 * Forgets which guest RAM pages the vertex mirror holds. Draws upload the
 * pages they read on demand, so a reset or state load costs only the pages
 * that are used afterwards instead of a copy of all guest RAM.
 */
void pgraph_d3d12_invalidate_vertex_ram_buffer(NV2AState *d)
{
    PGRAPHD3D12State *r = d->pgraph.d3d12_renderer_state;

    if (r != NULL && r->uploaded_bitmap != NULL && r->bitmap_size != 0) {
        bitmap_clear(r->uploaded_bitmap, 0, r->bitmap_size);
    }
}

bool pgraph_d3d12_vertex_ram_range_uploaded(PGRAPHState *pg, hwaddr offset,
                                            uint64_t size)
{
    PGRAPHD3D12State *r;
    uint64_t end_addr;
    size_t start_bit;
    size_t end_bit;
    unsigned long next_missing_bit;

    if (!require_renderer(pg, &r, NULL)) {
        return false;
    }

    if (size == 0) {
        return true;
    }

    if (r->uploaded_bitmap == NULL || r->bitmap_size == 0 ||
        offset > UINT64_MAX - size) {
        return false;
    }

    end_addr = offset + size;
    start_bit = offset / TARGET_PAGE_SIZE;
    end_bit = DIV_ROUND_UP(end_addr, TARGET_PAGE_SIZE);

    if (start_bit >= r->bitmap_size) {
        return false;
    }

    end_bit = MIN(end_bit, r->bitmap_size);
    if (start_bit >= end_bit) {
        return true;
    }

    next_missing_bit = find_next_zero_bit(r->uploaded_bitmap, end_bit,
                                          start_bit);
    return next_missing_bit >= end_bit;
}

static bool append_to_upload_buffer(PGRAPHState *pg,
                                    PGRAPHD3D12Buffer *buffer,
                                    const void **data,
                                    const uint64_t *sizes, size_t count,
                                    uint64_t alignment, uint64_t *offset,
                                    Error **errp)
{
    uint64_t total_size = 0;
    uint64_t worst_case;
    uint64_t buffer_offset;

    if (data == NULL || sizes == NULL || buffer == NULL ||
        buffer->mapped == NULL) {
        error_setg(errp, "Direct3D 12 upload requested with invalid data");
        return false;
    }

    if (alignment == 0) {
        alignment = 1;
    }

    for (size_t i = 0; i < count; i++) {
        if (sizes[i] != 0 && data[i] == NULL) {
            error_setg(errp,
                       "Direct3D 12 upload chunk %zu has no source data", i);
            return false;
        }
        total_size += sizes[i];
    }

    if (total_size == 0) {
        if (offset != NULL) {
            *offset = ROUND_UP(buffer->offset, alignment);
        }
        return true;
    }

    /* Account for the per-chunk alignment padding worst case. */
    worst_case = total_size + (uint64_t)count * (alignment - 1);
    if (!pgraph_d3d12_ring_alloc(pg, buffer, worst_case, alignment, NULL,
                                 errp)) {
        return false;
    }

    buffer_offset = pgraph_d3d12_append_to_buffer(buffer, data, sizes, count,
                                                  alignment);
    if (offset != NULL) {
        *offset = buffer_offset;
    }

    return true;
}

bool pgraph_d3d12_update_index_buffer(PGRAPHState *pg, const void *data,
                                      uint64_t size, uint64_t *offset,
                                      Error **errp)
{
    PGRAPHD3D12State *r;
    const void *chunks[] = { data };
    const uint64_t sizes[] = { size };

    if (!require_renderer(pg, &r, errp)) {
        return false;
    }

    /* Read directly from the ring by the draw; 4-byte alignment covers both
     * 16- and 32-bit index buffer views. */
    return append_to_upload_buffer(pg, &r->index_upload_buffer, chunks,
                                   sizes, ARRAY_SIZE(chunks), 4, offset,
                                   errp);
}

bool pgraph_d3d12_update_vertex_inline_buffer(PGRAPHState *pg,
                                              const void **data,
                                              const uint64_t *sizes,
                                              size_t count, uint64_t *offset,
                                              Error **errp)
{
    return pgraph_d3d12_upload_vertex_inline_buffer(
        pg, data, sizes, count, 1, offset, errp);
}

bool pgraph_d3d12_upload_vertex_inline_buffer(
    PGRAPHState *pg, const void **data, const uint64_t *sizes, size_t count,
    uint64_t alignment, uint64_t *offset, Error **errp)
{
    PGRAPHD3D12State *r;

    if (!require_renderer(pg, &r, errp)) {
        return false;
    }

    /* Read directly from the ring by the draw. */
    return append_to_upload_buffer(pg, &r->vertex_inline_upload_buffer,
                                   data, sizes, count, alignment, offset,
                                   errp);
}

bool pgraph_d3d12_update_uniform_buffer(PGRAPHState *pg, const void **data,
                                        const uint64_t *sizes, size_t count,
                                        uint64_t alignment, uint64_t *offset,
                                        Error **errp)
{
    PGRAPHD3D12State *r;
    if (!require_renderer(pg, &r, errp)) {
        return false;
    }

    for (size_t i = 0; i < count; i++) {
        r->perf.constants_bytes += sizes[i];
    }

    /* Root CBVs read directly from the ring. */
    return append_to_upload_buffer(pg, &r->uniform_upload_buffer, data,
                                   sizes, count, alignment, offset, errp);
}

bool pgraph_d3d12_update_vertex_ram_buffer(PGRAPHState *pg, hwaddr offset,
                                            const void *data, uint64_t size,
                                            Error **errp)
{
    return pgraph_d3d12_update_vertex_ram_buffer_async(pg, offset, data,
                                                       size, errp);
}

bool pgraph_d3d12_update_vertex_ram_buffer_async(PGRAPHState *pg,
                                                 hwaddr offset,
                                                 const void *data,
                                                 uint64_t size,
                                                 Error **errp)
{
    PGRAPHD3D12State *r;
    const void *chunks[] = { data };
    const uint64_t sizes[] = { size };
    uint64_t upload_offset = 0;
    size_t start_bit;
    size_t end_bit;

    if (!require_renderer(pg, &r, errp)) {
        return false;
    }

    if (r->upload_buffer.mapped == NULL) {
        error_setg(errp, "Direct3D 12 upload buffer is not initialized");
        return false;
    }

    if (size == 0) {
        return true;
    }

    if (data == NULL) {
        error_setg(errp, "Direct3D 12 vertex RAM upload has no source data");
        return false;
    }

    if (!append_to_upload_buffer(pg, &r->upload_buffer, chunks, sizes,
                                 ARRAY_SIZE(chunks), 1, &upload_offset,
                                 errp)) {
        return false;
    }

    /*
     * Stay in COPY_DEST so consecutive copies share one barrier pair; the
     * draw path transitions to the read state once when it next fetches.
     */
    if (!copy_buffer_internal(pg, &r->vertex_ram_buffer, offset,
                              &r->upload_buffer, upload_offset, size,
                              D3D12_RESOURCE_STATE_COPY_DEST,
                              false, errp)) {
        return false;
    }
    r->perf.vertex_upload_ranges++;
    r->perf.vertex_upload_bytes += size;
    r->vertex_ram_copies_in_list = true;

    if (r->uploaded_bitmap != NULL) {
        start_bit = offset / TARGET_PAGE_SIZE;
        end_bit = TARGET_PAGE_ALIGN(offset + size) / TARGET_PAGE_SIZE;
        end_bit = MIN(end_bit, r->bitmap_size);

        if (start_bit < end_bit) {
            bitmap_set(r->uploaded_bitmap, start_bit, end_bit - start_bit);
        }
    }

    return true;
}
