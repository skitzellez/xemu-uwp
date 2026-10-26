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

void pgraph_d3d12_init_command_objects(PGRAPHState *pg, Error **errp)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    HRESULT hr;

    QSIMPLEQ_INIT(&r->deferred_releases);

    D3D12_COMMAND_QUEUE_DESC queue_desc;
    memset(&queue_desc, 0, sizeof(queue_desc));
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;

    hr = r->device->lpVtbl->CreateCommandQueue(r->device, &queue_desc,
                                               &IID_ID3D12CommandQueue,
                                               (void **)&r->direct_queue);
    if (FAILED(hr)) {
        error_setg(errp,
                   "Failed to create Direct3D 12 direct queue: HRESULT 0x%08lx",
                   (unsigned long)hr);
        return;
    }

    for (uint32_t i = 0; i < PGRAPH_D3D12_COMMAND_ALLOCATORS; i++) {
        hr = r->device->lpVtbl->CreateCommandAllocator(
            r->device, D3D12_COMMAND_LIST_TYPE_DIRECT,
            &IID_ID3D12CommandAllocator,
            (void **)&r->command_allocators[i]);
        if (FAILED(hr)) {
            error_setg(
                errp,
                "Failed to create Direct3D 12 command allocator %u/%u: "
                "HRESULT 0x%08lx",
                i + 1, PGRAPH_D3D12_COMMAND_ALLOCATORS,
                (unsigned long)hr);
            return;
        }
    }
    r->command_allocator_index = 0;
    r->command_allocator = r->command_allocators[0];

    hr = r->device->lpVtbl->CreateCommandList(
        r->device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, r->command_allocator,
        NULL, &IID_ID3D12GraphicsCommandList, (void **)&r->command_list);
    if (FAILED(hr)) {
        error_setg(errp,
                   "Failed to create Direct3D 12 command list: HRESULT 0x%08lx",
                   (unsigned long)hr);
        return;
    }

    hr = r->command_list->lpVtbl->Close(r->command_list);
    if (FAILED(hr)) {
        error_setg(errp,
                   "Failed to close initial Direct3D 12 command list: HRESULT 0x%08lx",
                   (unsigned long)hr);
        return;
    }

    hr = r->device->lpVtbl->CreateFence(r->device, 0, D3D12_FENCE_FLAG_NONE,
                                        &IID_ID3D12Fence, (void **)&r->fence);
    if (FAILED(hr)) {
        error_setg(errp,
                   "Failed to create Direct3D 12 fence: HRESULT 0x%08lx",
                   (unsigned long)hr);
        return;
    }

#ifdef XEMU_UWP
    r->fence_event = CreateEventExW(NULL, NULL, 0,
                                    EVENT_MODIFY_STATE | SYNCHRONIZE);
#else
    r->fence_event = CreateEvent(NULL, FALSE, FALSE, NULL);
#endif
    if (r->fence_event == NULL) {
        error_setg_win32(errp, GetLastError(),
                         "Failed to create Direct3D 12 fence event");
        return;
    }

    /* Signalled by the presenter's queue when it no longer reads a display
     * snapshot; without it the presenter shares nothing to wait on. */
    if (FAILED(r->device->lpVtbl->CreateFence(
            r->device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence,
            (void **)&r->display_release_fence))) {
        r->display_release_fence = NULL;
    }

    /* GPU timers are diagnostics: failure only disables them. */
    r->gpu_timer_slot = -1;
    {
        D3D12_QUERY_HEAP_DESC heap_desc;
        D3D12_HEAP_PROPERTIES heap_props;
        D3D12_RESOURCE_DESC desc;
        D3D12_RANGE read_range = { 0, 0 };
        void *mapped = NULL;

        memset(&heap_desc, 0, sizeof(heap_desc));
        heap_desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        heap_desc.Count = PGRAPH_D3D12_GPU_TIMER_SLOTS * 2;
        memset(&heap_props, 0, sizeof(heap_props));
        heap_props.Type = D3D12_HEAP_TYPE_READBACK;
        memset(&desc, 0, sizeof(desc));
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = PGRAPH_D3D12_GPU_TIMER_SLOTS * 2 * sizeof(uint64_t);
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(r->direct_queue->lpVtbl->GetTimestampFrequency(
                r->direct_queue, &r->gpu_timer_frequency)) ||
            r->gpu_timer_frequency == 0 ||
            FAILED(r->device->lpVtbl->CreateQueryHeap(
                r->device, &heap_desc, &IID_ID3D12QueryHeap,
                (void **)&r->gpu_timer_heap)) ||
            FAILED(r->device->lpVtbl->CreateCommittedResource(
                r->device, &heap_props, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource,
                (void **)&r->gpu_timer_readback)) ||
            FAILED(r->gpu_timer_readback->lpVtbl->Map(
                r->gpu_timer_readback, 0, &read_range, &mapped))) {
            D3D12_RELEASE(r->gpu_timer_heap);
            D3D12_RELEASE(r->gpu_timer_readback);
            r->gpu_timer_frequency = 0;
        } else {
            r->gpu_timer_mapped = mapped;
        }
    }
}

/* Adds the GPU time of every timed list whose fence has passed. */
void pgraph_d3d12_collect_gpu_timers(PGRAPHD3D12State *r)
{
    uint64_t completed;

    if (r == NULL || r->gpu_timer_mapped == NULL || r->fence == NULL) {
        return;
    }
    completed = r->fence->lpVtbl->GetCompletedValue(r->fence);
    for (unsigned int i = 0; i < PGRAPH_D3D12_GPU_TIMER_SLOTS; i++) {
        uint64_t start, end;

        if (r->gpu_timer_fence[i] == 0 || r->gpu_timer_fence[i] > completed) {
            continue;
        }
        start = r->gpu_timer_mapped[i * 2];
        end = r->gpu_timer_mapped[i * 2 + 1];
        if (end > start) {
            r->perf.gpu_busy_ticks += end - start;
            r->perf.gpu_lists_timed++;
        }
        r->gpu_timer_fence[i] = 0;
    }
}

void pgraph_d3d12_finalize_command_objects(PGRAPHState *pg)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    PGRAPHD3D12DeferredRelease *release;

    if (r == NULL) {
        return;
    }

    /* GPU already drained and descriptor heaps gone; release resources only. */
    while ((release = QSIMPLEQ_FIRST(&r->deferred_releases)) != NULL) {
        QSIMPLEQ_REMOVE_HEAD(&r->deferred_releases, entry);
        if (release->resource != NULL) {
            release->resource->lpVtbl->Release(release->resource);
        }
        g_free(release);
    }

    if (r->fence_event != NULL) {
        CloseHandle(r->fence_event);
        r->fence_event = NULL;
    }
    D3D12_RELEASE(r->fence);
    D3D12_RELEASE(r->display_release_fence);
    if (r->gpu_timer_readback != NULL && r->gpu_timer_mapped != NULL) {
        r->gpu_timer_readback->lpVtbl->Unmap(r->gpu_timer_readback, 0, NULL);
        r->gpu_timer_mapped = NULL;
    }
    D3D12_RELEASE(r->gpu_timer_readback);
    D3D12_RELEASE(r->gpu_timer_heap);
    D3D12_RELEASE(r->command_list);
    r->command_allocator = NULL;
    for (uint32_t i = 0; i < PGRAPH_D3D12_COMMAND_ALLOCATORS; i++) {
        D3D12_RELEASE(r->command_allocators[i]);
        r->command_allocator_fence_values[i] = 0;
    }
    D3D12_RELEASE(r->direct_queue);
}

static bool wait_for_fence_value(PGRAPHD3D12State *r, uint64_t fence_value,
                                 const char *operation)
{
    HRESULT hr;
    DWORD timeout_ms = 10000;
    const char *timeout_env;
    DWORD wait_result = WAIT_OBJECT_0;

    if (fence_value == 0 || r->fence == NULL) {
        return true;
    }

    timeout_env = getenv("XEMU_D3D12_FENCE_TIMEOUT_MS");
    if (timeout_env != NULL && timeout_env[0] != '\0') {
        char *end = NULL;
        unsigned long value = strtoul(timeout_env, &end, 10);

        if (end != timeout_env && *end == '\0') {
            timeout_ms = value == 0 ? INFINITE : (DWORD)value;
        }
    }

    /*
     * The shared auto-reset event can be signalled by a stale registration
     * from an earlier timed-out wait, so always re-check the fence value.
     */
    while (r->fence->lpVtbl->GetCompletedValue(r->fence) < fence_value) {
        hr = r->fence->lpVtbl->SetEventOnCompletion(r->fence, fence_value,
                                                    r->fence_event);
        if (FAILED(hr)) {
            fprintf(stderr,
                    "Direct3D 12 fence wait setup failed during %s: "
                    "fence=%" PRIu64 ", HRESULT 0x%08lx\n",
                    operation, fence_value, (unsigned long)hr);
            return false;
        }

#ifdef XEMU_UWP
        wait_result = WaitForSingleObjectEx(r->fence_event, timeout_ms,
                                            FALSE);
#else
        wait_result = WaitForSingleObject(r->fence_event, timeout_ms);
#endif
        if (wait_result == WAIT_OBJECT_0) {
            continue;
        }

        if (r->fence->lpVtbl->GetCompletedValue(r->fence) >= fence_value) {
            return true;
        }

        uint64_t completed = r->fence->lpVtbl->GetCompletedValue(r->fence);
        HRESULT removed = r->device != NULL ?
            r->device->lpVtbl->GetDeviceRemovedReason(r->device) : S_OK;
        char detail[128];

        fprintf(stderr,
                "Direct3D 12 fence wait failed during %s: fence=%" PRIu64
                ", completed=%" PRIu64 ", wait_result=0x%08lx, "
                "timeout_ms=%lu, device_removed=%s (0x%08lx)%s\n",
                operation, fence_value, completed,
                (unsigned long)wait_result, (unsigned long)timeout_ms,
                pgraph_d3d12_hresult_name(removed), (unsigned long)removed,
                pgraph_d3d12_device_removed_detail(r, removed, detail,
                                                    sizeof(detail)));
        pgraph_d3d12_report_info_queue(r, operation);
        return false;
    }
    return true;
}

bool pgraph_d3d12_wait_for_fence(PGRAPHState *pg, uint64_t fence_value,
                                 const char *operation)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;

    if (r == NULL) {
        return false;
    }

    if (!wait_for_fence_value(r, fence_value, operation)) {
        return false;
    }

    pgraph_d3d12_process_deferred_releases(pg);
    return true;
}

static bool close_and_execute_commands(PGRAPHD3D12State *r, HRESULT *hrp)
{
    ID3D12CommandList *command_lists[1];
    HRESULT hr;

    if (!r->command_list_open) {
        return true;
    }

    /* a held-back draw belongs to this list */
    pgraph_d3d12_merge_emit(r);
    r->submits++;

    if (r->gpu_timer_slot >= 0) {
        unsigned int slot = (unsigned int)r->gpu_timer_slot;

        r->command_list->lpVtbl->EndQuery(r->command_list, r->gpu_timer_heap,
                                          D3D12_QUERY_TYPE_TIMESTAMP,
                                          slot * 2 + 1);
        r->command_list->lpVtbl->ResolveQueryData(
            r->command_list, r->gpu_timer_heap, D3D12_QUERY_TYPE_TIMESTAMP,
            slot * 2, 2, r->gpu_timer_readback,
            slot * 2 * sizeof(uint64_t));
        r->gpu_timer_fence[slot] = r->fence_value + 1;
        r->gpu_timer_slot = -1;
    }

    hr = r->command_list->lpVtbl->Close(r->command_list);
    if (FAILED(hr)) {
        *hrp = hr;
        /* Batch lost; mark closed so the next begin resets the list. */
        r->command_list_open = false;
        r->vertex_ram_copies_in_list = false;
        fprintf(stderr,
                "Direct3D 12 renderer: dropped command batch after failed "
                "close: HRESULT 0x%08lx draws_since_submit=%u "
                "query_in_flight=%d queries_in_flight=%u\n",
                (unsigned long)hr, r->draws_since_submit,
                r->query_in_flight, r->num_queries_in_flight);
        return false;
    }

    command_lists[0] = (ID3D12CommandList *)r->command_list;
    r->direct_queue->lpVtbl->ExecuteCommandLists(r->direct_queue, 1,
                                                 command_lists);
    r->command_list_open = false;
    r->vertex_ram_copies_in_list = false;
    r->draw_constants_cached = false;
    r->vertex_fetch_cached = false;
    r->pixel_constants_cached = false;
    r->uber_constants_cached = false;
    r->perf.submits++;
    return true;
}

static bool signal_submitted_commands(PGRAPHD3D12State *r, uint64_t *value,
                                      HRESULT *hrp)
{
    HRESULT hr;

    *value = ++r->fence_value;
    hr = r->direct_queue->lpVtbl->Signal(r->direct_queue, r->fence, *value);
    if (FAILED(hr)) {
        *hrp = hr;
        return false;
    }

    r->submitted_fence_value = *value;
    r->draws_since_submit = 0;
    r->resolved_submitted_count = r->num_queries_resolved;
    r->resolved_submitted_fence = *value;
    pgraph_d3d12_buffers_on_submit(r, *value);
    return true;
}

void pgraph_d3d12_defer_release(PGRAPHState *pg, ID3D12Resource *resource,
                                int32_t srv_descriptor_index,
                                int32_t rtv_descriptor_index,
                                int32_t dsv_descriptor_index,
                                int32_t sampler_descriptor_index)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    PGRAPHD3D12DeferredRelease *release;

    if (r == NULL) {
        if (resource != NULL) {
            resource->lpVtbl->Release(resource);
        }
        return;
    }

    release = g_malloc0(sizeof(*release));
    release->resource = resource;
    release->srv_descriptor_index = srv_descriptor_index;
    release->rtv_descriptor_index = rtv_descriptor_index;
    release->dsv_descriptor_index = dsv_descriptor_index;
    release->sampler_descriptor_index = sampler_descriptor_index;
    /* An open list is covered by the next signal, else the last one. */
    release->fence_value = r->command_list_open ? r->fence_value + 1 :
                                                  r->submitted_fence_value;
    QSIMPLEQ_INSERT_TAIL(&r->deferred_releases, release, entry);
}

void pgraph_d3d12_process_deferred_releases(PGRAPHState *pg)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    PGRAPHD3D12DeferredRelease *release;
    uint64_t completed;

    if (r == NULL || r->fence == NULL ||
        QSIMPLEQ_EMPTY(&r->deferred_releases)) {
        return;
    }

    completed = r->fence->lpVtbl->GetCompletedValue(r->fence);
    while ((release = QSIMPLEQ_FIRST(&r->deferred_releases)) != NULL) {
        if (release->fence_value > completed) {
            break;
        }
        QSIMPLEQ_REMOVE_HEAD(&r->deferred_releases, entry);
        if (release->resource != NULL) {
            release->resource->lpVtbl->Release(release->resource);
        }
        if (release->srv_descriptor_index >= 0) {
            pgraph_d3d12_free_srv_descriptor(
                r, (uint32_t)release->srv_descriptor_index);
        }
        if (release->rtv_descriptor_index >= 0) {
            pgraph_d3d12_free_rtv_descriptor(
                r, (uint32_t)release->rtv_descriptor_index);
        }
        if (release->dsv_descriptor_index >= 0) {
            pgraph_d3d12_free_dsv_descriptor(
                r, (uint32_t)release->dsv_descriptor_index);
        }
        if (release->sampler_descriptor_index >= 0) {
            pgraph_d3d12_free_sampler_descriptor(
                r, (uint32_t)release->sampler_descriptor_index);
        }
        g_free(release);
    }
}

/* Returns false when the queue could not be drained. */
bool pgraph_d3d12_finish(PGRAPHState *pg)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    HRESULT hr;
    uint64_t fence_value = 0;
    bool had_open_commands;
    int64_t perf_start;

    if (r == NULL || r->direct_queue == NULL || r->fence == NULL) {
        return false;
    }

    perf_start = g_get_monotonic_time();
    r->perf.downloads++;

    had_open_commands = r->command_list_open;
    if (r->query_in_flight) {
        pgraph_d3d12_end_zpass_query(container_of(pg, NV2AState, pgraph));
    }
    /* One batched resolve per command-list boundary. */
    pgraph_d3d12_flush_pending_query_resolves(r);

    if (!close_and_execute_commands(r, &hr)) {
        char detail[128];

        fprintf(stderr,
                "Direct3D 12 command list close failed: HRESULT %s "
                "(0x%08lx)%s\n",
                pgraph_d3d12_hresult_name(hr), (unsigned long)hr,
                pgraph_d3d12_device_removed_detail(r, hr, detail,
                                                    sizeof(detail)));
        pgraph_d3d12_report_info_queue(r, "command list close in finish");
        return false;
    }

    if (had_open_commands) {
        if (!signal_submitted_commands(r, &fence_value, &hr)) {
            char detail[128];

            fprintf(stderr,
                    "Direct3D 12 queue signal failed: HRESULT %s "
                    "(0x%08lx)%s\n",
                    pgraph_d3d12_hresult_name(hr), (unsigned long)hr,
                    pgraph_d3d12_device_removed_detail(r, hr, detail,
                                                        sizeof(detail)));
            return false;
        }
        /* as in submit: the allocator just submitted is reset only after its fence, also if the wait below
         * times out (a reset while the GPU still runs its list removes the device) */
        unsigned int allocator_index = r->command_allocator_index;
        r->command_allocator_fence_values[allocator_index] = fence_value;
        r->command_allocator_index =
            (allocator_index + 1) % PGRAPH_D3D12_COMMAND_ALLOCATORS;
        r->command_allocator = r->command_allocators[r->command_allocator_index];
    } else {
        fence_value = r->submitted_fence_value;
    }

    if (!wait_for_fence_value(r, fence_value, "finish")) {
        fprintf(stderr,
                "Direct3D 12 fence wait failed during finish: fence=%" PRIu64
                "\n",
                fence_value);
        return false;
    }

    pgraph_d3d12_buffers_retire_completed(r);
    pgraph_d3d12_process_deferred_releases(pg);
    r->perf.download_wait_us += g_get_monotonic_time() - perf_start;
    return true;
}

/*
 * Waits for a submitted fence value without touching other renderer state,
 * so the FIFO thread may release pg->lock around it.
 */
bool pgraph_d3d12_wait_for_fence_unlocked(PGRAPHState *pg, uint64_t value)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;

    return r == NULL || value == 0 ||
           wait_for_fence_value(r, value, "flip pipeline");
}

/*
 * Like pgraph_d3d12_wait_for_fence_unlocked, but in short slices: returns
 * false as soon as `interrupted` reports other work for the caller, or
 * after 10 s without progress.
 */
bool pgraph_d3d12_wait_for_fence_interruptible(PGRAPHState *pg,
                                               uint64_t value,
                                               bool (*interrupted)(void *),
                                               void *opaque)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    int64_t start = g_get_monotonic_time();

    if (r == NULL || r->fence == NULL || value == 0) {
        return true;
    }
    while (r->fence->lpVtbl->GetCompletedValue(r->fence) < value) {
        if (interrupted(opaque)) {
            return false;
        }
        if (g_get_monotonic_time() - start > 10 * 1000 * 1000) {
            fprintf(stderr,
                    "Direct3D 12 report fence wait timed out: fence=%" PRIu64
                    ", completed=%" PRIu64 "\n",
                    value, r->fence->lpVtbl->GetCompletedValue(r->fence));
            return false;
        }
        if (FAILED(r->fence->lpVtbl->SetEventOnCompletion(r->fence, value,
                                                          r->fence_event))) {
            return false;
        }
#ifdef XEMU_UWP
        WaitForSingleObjectEx(r->fence_event, 1, FALSE);
#else
        WaitForSingleObject(r->fence_event, 1);
#endif
    }
    return true;
}

bool pgraph_d3d12_wait_for_submitted_fence(PGRAPHState *pg, uint64_t value)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;

    if (r == NULL || value == 0) {
        return true;
    }
    if (!wait_for_fence_value(r, value, "flip pipeline")) {
        return false;
    }
    pgraph_d3d12_buffers_retire_completed(r);
    pgraph_d3d12_process_deferred_releases(pg);
    return true;
}

bool pgraph_d3d12_begin_commands(PGRAPHState *pg, Error **errp)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    HRESULT hr;
    uint32_t allocator_index;
    uint64_t allocator_fence;

    if (r == NULL || r->command_allocator == NULL || r->command_list == NULL) {
        error_setg(errp, "Direct3D 12 command objects are not initialized");
        return false;
    }

    if (r->command_list_open) {
        return true;
    }

    allocator_index = r->command_allocator_index;
    allocator_fence = r->command_allocator_fence_values[allocator_index];
    if (allocator_fence != 0 &&
        r->fence->lpVtbl->GetCompletedValue(r->fence) < allocator_fence) {
        int64_t wait_start = g_get_monotonic_time();
        bool wait_ok = wait_for_fence_value(r, allocator_fence,
                                            "command allocator reuse");

        r->perf.allocator_waits++;
        r->perf.allocator_wait_us += g_get_monotonic_time() - wait_start;
        if (!wait_ok) {
            error_setg(errp,
                       "Timed out waiting for Direct3D 12 command allocator "
                       "%u reuse fence=%" PRIu64,
                       allocator_index, allocator_fence);
            return false;
        }
    } else if (!wait_for_fence_value(r, allocator_fence,
                                     "command allocator reuse")) {
        error_setg(errp,
                   "Timed out waiting for Direct3D 12 command allocator %u "
                   "reuse fence=%" PRIu64,
                   allocator_index, allocator_fence);
        return false;
    }
    r->command_allocator_fence_values[allocator_index] = 0;
    r->command_allocator = r->command_allocators[allocator_index];

    hr = r->command_allocator->lpVtbl->Reset(r->command_allocator);
    if (FAILED(hr)) {
        char detail[128];

        error_setg(errp,
                   "Failed to reset Direct3D 12 command allocator: HRESULT "
                   "%s (0x%08lx)%s",
                   pgraph_d3d12_hresult_name(hr), (unsigned long)hr,
                   pgraph_d3d12_device_removed_detail(r, hr, detail,
                                                       sizeof(detail)));
        return false;
    }

    hr = r->command_list->lpVtbl->Reset(r->command_list, r->command_allocator,
                                        NULL);
    if (FAILED(hr)) {
        char detail[128];

        error_setg(errp,
                   "Failed to reset Direct3D 12 command list: HRESULT %s "
                   "(0x%08lx)%s",
                   pgraph_d3d12_hresult_name(hr), (unsigned long)hr,
                   pgraph_d3d12_device_removed_detail(r, hr, detail,
                                                       sizeof(detail)));
        return false;
    }

    /* Time this list when a timer slot is free. */
    r->gpu_timer_slot = -1;
    if (r->gpu_timer_mapped != NULL) {
        unsigned int slot = r->gpu_timer_next;

        pgraph_d3d12_collect_gpu_timers(r);
        if (r->gpu_timer_fence[slot] == 0) {
            r->command_list->lpVtbl->EndQuery(
                r->command_list, r->gpu_timer_heap,
                D3D12_QUERY_TYPE_TIMESTAMP, slot * 2);
            r->gpu_timer_slot = (int)slot;
            r->gpu_timer_next = (slot + 1) % PGRAPH_D3D12_GPU_TIMER_SLOTS;
        }
    }

    /* Heaps never change; bind once per list (per-draw rebinds can flush). */
    if (r->cbv_srv_uav_heap != NULL && r->sampler_heap != NULL) {
        ID3D12DescriptorHeap *heaps[2] = { r->cbv_srv_uav_heap,
                                           r->sampler_heap };

        r->command_list->lpVtbl->SetDescriptorHeaps(r->command_list,
                                                    ARRAY_SIZE(heaps), heaps);
    }

    /* A fresh list has no pipeline state; invalidate the record shadows. */
    r->record_root_signature_set = false;
    r->record_pipeline_state = NULL;
    r->record_vertex_ram_srv = 0;
    r->record_vertex_scratch_srv = 0;
    r->record_vertex_start_valid = false;
    memset(r->record_texture_srv_gpu, 0, sizeof(r->record_texture_srv_gpu));
    memset(r->record_texture_sampler_gpu, 0,
           sizeof(r->record_texture_sampler_gpu));
    r->record_framebuffer_srv_gpu = 0;
    r->record_render_targets_valid = false;
    r->record_viewport_valid = false;
    r->record_blend_factor_valid = false;
    r->record_stencil_ref_valid = false;
    r->record_topology_valid = false;

    r->command_list_open = true;
    return true;
}

bool pgraph_d3d12_submit_commands_async(PGRAPHState *pg, Error **errp)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    HRESULT hr;
    uint64_t fence_value;
    uint32_t allocator_index;

    if (r == NULL || r->command_list == NULL || r->direct_queue == NULL) {
        error_setg(errp, "Direct3D 12 command objects are not initialized");
        return false;
    }

    if (!r->command_list_open) {
        return true;
    }

    /* EndQuery is required before Close; resolve in the same submission. */
    if (r->query_in_flight) {
        pgraph_d3d12_end_zpass_query(container_of(pg, NV2AState, pgraph));
    }
    pgraph_d3d12_flush_pending_query_resolves(r);

    if (!close_and_execute_commands(r, &hr)) {
        char detail[128];

        pgraph_d3d12_report_info_queue(r, "command list close in submit");
        error_setg(errp,
                   "Failed to close Direct3D 12 command list: HRESULT %s "
                   "(0x%08lx)%s",
                   pgraph_d3d12_hresult_name(hr), (unsigned long)hr,
                   pgraph_d3d12_device_removed_detail(r, hr, detail,
                                                       sizeof(detail)));
        return false;
    }

    if (!signal_submitted_commands(r, &fence_value, &hr)) {
        char detail[128];

        error_setg(errp,
                   "Failed to signal Direct3D 12 command queue: HRESULT %s "
                   "(0x%08lx)%s",
                   pgraph_d3d12_hresult_name(hr), (unsigned long)hr,
                   pgraph_d3d12_device_removed_detail(r, hr, detail,
                                                       sizeof(detail)));
        return false;
    }

    allocator_index = r->command_allocator_index;
    r->command_allocator_fence_values[allocator_index] = fence_value;
    r->command_allocator_index =
        (allocator_index + 1) % PGRAPH_D3D12_COMMAND_ALLOCATORS;
    r->command_allocator = r->command_allocators[r->command_allocator_index];

    /* Diagnostic: no CPU recording overlaps GPU execution. */
    if ((r->tune_flags & PGRAPH_D3D12_TUNE_SUBMIT_SYNC) &&
        wait_for_fence_value(r, fence_value, "submit sync")) {
        pgraph_d3d12_buffers_retire_completed(r);
    }
    pgraph_d3d12_process_deferred_releases(pg);
    return true;
}

bool pgraph_d3d12_submit_commands(PGRAPHState *pg, Error **errp)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;

    if (r != NULL) {
        r->perf.submit_src[1]++;
    }
    if (!pgraph_d3d12_submit_commands_async(pg, errp)) {
        return false;
    }

    /* Callers read results (readbacks) right after; a failed wait means the
     * copy may still be running. */
    if (!pgraph_d3d12_finish(pg)) {
        error_setg(errp, "Direct3D 12 GPU wait after submit failed");
        return false;
    }
    return true;
}
