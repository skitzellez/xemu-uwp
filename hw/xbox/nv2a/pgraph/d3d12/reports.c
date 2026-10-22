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
#include "qapi/error.h"
#include "renderer.h"

#define D3D12_ZPASS_QUERY_COUNT 16384

static uint32_t scale_zpass_result(PGRAPHState *pg, uint64_t result);

static void consume_reports_without_gpu_work(NV2AState *d)
{
    PGRAPHD3D12State *r = d->pgraph.d3d12_renderer_state;
    PGRAPHD3D12QueryReport *report;

    while ((report = QSIMPLEQ_FIRST(&r->report_queue)) != NULL &&
           report->fence_value == 0 &&
           report->query_count <= r->num_results_consumed) {
        if (report->clear) {
            r->zpass_pixel_count_result = 0;
        } else {
            pgraph_write_zpass_pixel_cnt_report(
                d, report->parameter,
                scale_zpass_result(&d->pgraph,
                                   r->zpass_pixel_count_result));
        }
        QSIMPLEQ_REMOVE_HEAD(&r->report_queue, entry);
        g_free(report);
    }
}

static void release_query_readback_buffer(PGRAPHD3D12Buffer *buffer)
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

static bool create_query_readback_buffer(PGRAPHD3D12State *r, uint64_t size,
                                         Error **errp)
{
    D3D12_HEAP_PROPERTIES heap_props;
    D3D12_RESOURCE_DESC desc;
    D3D12_RANGE read_range = { 0, 0 };
    HRESULT hr;

    memset(&heap_props, 0, sizeof(heap_props));
    heap_props.Type = D3D12_HEAP_TYPE_READBACK;
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
        r->device, &heap_props, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource,
        (void **)&r->query_readback_buffer.resource);
    if (FAILED(hr)) {
        error_setg(errp,
                   "Failed to create Direct3D 12 zpass query readback buffer "
                   "(%" PRIu64 " bytes): HRESULT 0x%08lx",
                   size, (unsigned long)hr);
        return false;
    }

    r->query_readback_buffer.size = size;
    r->query_readback_buffer.state = D3D12_RESOURCE_STATE_COPY_DEST;

    hr = r->query_readback_buffer.resource->lpVtbl->Map(
        r->query_readback_buffer.resource, 0, &read_range,
        &r->query_readback_buffer.mapped);
    if (FAILED(hr)) {
        error_setg(errp,
                   "Failed to map Direct3D 12 zpass query readback buffer: "
                   "HRESULT 0x%08lx",
                   (unsigned long)hr);
        release_query_readback_buffer(&r->query_readback_buffer);
        return false;
    }

    return true;
}

void pgraph_d3d12_init_reports(PGRAPHState *pg, Error **errp)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    D3D12_QUERY_HEAP_DESC query_heap_desc;
    uint64_t readback_size;
    HRESULT hr;

    QSIMPLEQ_INIT(&r->report_queue);
    r->max_queries_in_flight = D3D12_ZPASS_QUERY_COUNT;
    r->num_queries_in_flight = 0;
    r->num_queries_resolved = 0;
    r->num_results_consumed = 0;
    r->resolved_submitted_count = 0;
    r->query_in_flight = false;
    r->zpass_pixel_count_result = 0;

    memset(&query_heap_desc, 0, sizeof(query_heap_desc));
    query_heap_desc.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
    query_heap_desc.Count = r->max_queries_in_flight;
    query_heap_desc.NodeMask = 0;

    hr = r->device->lpVtbl->CreateQueryHeap(
        r->device, &query_heap_desc, &IID_ID3D12QueryHeap,
        (void **)&r->query_heap);
    if (FAILED(hr)) {
        error_setg(errp,
                   "Failed to create Direct3D 12 zpass occlusion query heap "
                   "(%u slots): HRESULT 0x%08lx",
                   r->max_queries_in_flight, (unsigned long)hr);
        return;
    }

    readback_size = r->max_queries_in_flight * sizeof(uint64_t);
    if (!create_query_readback_buffer(r, readback_size, errp)) {
        D3D12_RELEASE(r->query_heap);
        return;
    }
}

void pgraph_d3d12_finalize_reports(PGRAPHState *pg)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    PGRAPHD3D12QueryReport *report;

    if (r == NULL) {
        return;
    }

    while ((report = QSIMPLEQ_FIRST(&r->report_queue)) != NULL) {
        QSIMPLEQ_REMOVE_HEAD(&r->report_queue, entry);
        g_free(report);
    }

    release_query_readback_buffer(&r->query_readback_buffer);
    D3D12_RELEASE(r->query_heap);
    r->max_queries_in_flight = 0;
    r->num_queries_in_flight = 0;
    r->num_queries_resolved = 0;
    r->num_results_consumed = 0;
    r->resolved_submitted_count = 0;
    r->query_in_flight = false;
    r->zpass_pixel_count_result = 0;
}

/* Stamp unstamped queued reports with a fence covering their resolves,
 * submitting at most once per call so bursts of report methods share one
 * submission. */
static void stamp_pending_reports(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    PGRAPHD3D12QueryReport *report;
    PGRAPHD3D12QueryReport *last_visible = NULL;
    Error *local_err = NULL;

    /* CLEAR_REPORT_VALUE writes nothing to guest memory and must not force a
     * submission; clears before the last unstamped GET_REPORT share its fence. */
    QSIMPLEQ_FOREACH(report, &r->report_queue, entry) {
        if (!report->clear && report->fence_value == 0) {
            last_visible = report;
        }
    }

    if (last_visible == NULL) {
        return;
    }

    /* Every submission resolves the pending queries before closing its
     * list, so no separate resolve is recorded per report. Allow one
     * report-driven submit per 2ms so titles that block on reports many
     * times per frame make progress. */
    if (r->command_list_open &&
        g_get_monotonic_time() - r->last_report_submit_us >= 2000) {
        r->perf.submit_src[6]++;
        if (!pgraph_d3d12_submit_commands_async(pg, &local_err)) {
            error_free(local_err);
            return;
        }
        r->last_report_submit_us = g_get_monotonic_time();
    }

    /* Stamp reports whose resolves are already behind a submitted fence. */
    QSIMPLEQ_FOREACH(report, &r->report_queue, entry) {
        if (report->fence_value == 0 &&
            report->query_count <= r->resolved_submitted_count) {
            report->fence_value = r->resolved_submitted_fence;
        }
        if (report == last_visible) {
            break;
        }
    }
}

static bool reports_need_completion(PGRAPHD3D12State *r)
{
    PGRAPHD3D12QueryReport *report;

    QSIMPLEQ_FOREACH(report, &r->report_queue, entry) {
        if (!report->clear) {
            return true;
        }
    }
    return false;
}

void pgraph_d3d12_clear_report_value(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    PGRAPHD3D12QueryReport *report;
    PGRAPHD3D12QueryReport *tail;

    if (r == NULL) {
        return;
    }

    pgraph_d3d12_end_zpass_query(d);
    tail = QSIMPLEQ_LAST(&r->report_queue, PGRAPHD3D12QueryReport, entry);
    if (tail != NULL && tail->clear && tail->fence_value == 0) {
        /* Consecutive unsubmitted clears have identical observable effect;
         * retain only the newest query boundary. */
        tail->query_count = r->num_queries_in_flight;
        consume_reports_without_gpu_work(d);
        return;
    }
    report = g_malloc0(sizeof(*report));
    report->clear = true;
    report->query_count = r->num_queries_in_flight;
    QSIMPLEQ_INSERT_TAIL(&r->report_queue, report, entry);
    consume_reports_without_gpu_work(d);
}

void pgraph_d3d12_get_report(NV2AState *d, uint32_t parameter)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    PGRAPHD3D12QueryReport *report;
    uint8_t type = GET_MASK(parameter, NV097_GET_REPORT_TYPE);

    if (r == NULL) {
        return;
    }

    if (type != NV097_GET_REPORT_TYPE_ZPASS_PIXEL_CNT) {
        pgraph_d3d12_warn_once(
            pg, PGRAPH_D3D12_DIAG_UNSUPPORTED_REPORT,
            "unsupported report type %u in NV097_GET_REPORT parameter "
            "0x%08" PRIx32 "; only ZPASS_PIXEL_CNT (%u) is implemented",
            type, parameter, NV097_GET_REPORT_TYPE_ZPASS_PIXEL_CNT);
        pgraph_write_zpass_pixel_cnt_report(d, parameter, 0);
        return;
    }

    /* Segment the persistent query exactly at the guest-visible report. */
    pgraph_d3d12_end_zpass_query(d);
    report = g_malloc0(sizeof(*report));
    report->clear = false;
    report->parameter = parameter;
    report->dma_report = pg->dma_report;
    report->issue_frame = g_nv2a_stats.frame_count;
    pgraph_d3d12_evlog(r, "report issue param=%08x", parameter);
    report->query_count = r->num_queries_in_flight;
    QSIMPLEQ_INSERT_TAIL(&r->report_queue, report, entry);

    consume_reports_without_gpu_work(d);
    /* Submit now without waiting so a later FIFO stall covers only work
     * recorded before this report. */
    stamp_pending_reports(d);
}

/* Writes a report through the DMA object that was current at GET_REPORT;
 * the guest may have changed it since. */
static void write_zpass_report(NV2AState *d,
                               const PGRAPHD3D12QueryReport *report,
                               uint32_t result)
{
    PGRAPHState *pg = &d->pgraph;
    uint32_t current = pg->dma_report;

    pg->dma_report = report->dma_report;
    pgraph_write_zpass_pixel_cnt_report(d, report->parameter, result);
    pg->dma_report = current;
    pgraph_d3d12_evlog(pg->d3d12_renderer_state,
                       "report write param=%08x issued=%u value=%u",
                       report->parameter, report->issue_frame, result);
}

static uint32_t scale_zpass_result(PGRAPHState *pg, uint64_t result)
{
    uint64_t divisor = pg->surface_scale_factor * pg->surface_scale_factor;

    if (divisor == 0) {
        divisor = 1;
    }
    result /= divisor;
    return result > UINT32_MAX ? UINT32_MAX : (uint32_t)result;
}

static void pgraph_d3d12_drain_zpass_queries(NV2AState *d,
                                             const char *reason)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    uint64_t *query_results;
    PGRAPHD3D12QueryReport *report;

    if (r == NULL || (r->num_queries_in_flight == 0 &&
                      QSIMPLEQ_EMPTY(&r->report_queue))) {
        return;
    }

    {
        int64_t perf_start = g_get_monotonic_time();

        r->perf.finish_report++;
        pgraph_d3d12_finish(pg);
        r->perf.report_flush_us += g_get_monotonic_time() - perf_start;
        r->perf.report_flushes++;
    }

    if (r->query_readback_buffer.mapped == NULL) {
        pgraph_d3d12_warn_once(
            pg, PGRAPH_D3D12_DIAG_ZPASS_REPORTS,
            "cannot process zpass reports: query_readback_buffer is not "
            "mapped; reason=%s, queued_reports_pending=%d, "
            "queries_in_flight=%u, max_queries=%u",
            reason, !QSIMPLEQ_EMPTY(&r->report_queue),
            r->num_queries_in_flight, r->max_queries_in_flight);
        return;
    }

    query_results = r->query_readback_buffer.mapped;

    while ((report = QSIMPLEQ_FIRST(&r->report_queue)) != NULL) {
        assert(report->query_count >= r->num_results_consumed);
        assert(report->query_count <= r->num_queries_in_flight);

        while (r->num_results_consumed < report->query_count) {
            r->zpass_pixel_count_result +=
                query_results[r->num_results_consumed++];
        }

        if (report->clear) {
            r->zpass_pixel_count_result = 0;
        } else {
            write_zpass_report(
                d, report,
                scale_zpass_result(pg, r->zpass_pixel_count_result));
        }

        QSIMPLEQ_REMOVE_HEAD(&r->report_queue, entry);
        g_free(report);
    }

    while (r->num_results_consumed < r->num_queries_in_flight) {
        r->zpass_pixel_count_result +=
            query_results[r->num_results_consumed++];
    }

    r->num_queries_in_flight = 0;
    r->num_queries_resolved = 0;
    r->num_results_consumed = 0;
    r->resolved_submitted_count = 0;
}

/* Consume reports whose covering fence has completed, without waiting. */
static void pgraph_d3d12_consume_ready_reports(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    PGRAPHD3D12QueryReport *report;
    uint64_t *query_results;
    uint64_t completed;

    if (r == NULL || r->query_readback_buffer.mapped == NULL ||
        r->fence == NULL) {
        return;
    }
    if (QSIMPLEQ_EMPTY(&r->report_queue) && r->num_queries_in_flight == 0) {
        return;
    }

    completed = r->fence->lpVtbl->GetCompletedValue(r->fence);
    query_results = r->query_readback_buffer.mapped;

    while ((report = QSIMPLEQ_FIRST(&r->report_queue)) != NULL) {
        if (report->fence_value == 0 ||
            report->fence_value > completed ||
            report->query_count > r->num_queries_resolved) {
            break;
        }

        while (r->num_results_consumed < report->query_count) {
            r->zpass_pixel_count_result +=
                query_results[r->num_results_consumed++];
        }

        if (report->clear) {
            r->zpass_pixel_count_result = 0;
        } else {
            write_zpass_report(
                d, report,
                scale_zpass_result(pg, r->zpass_pixel_count_result));
        }

        QSIMPLEQ_REMOVE_HEAD(&r->report_queue, entry);
        g_free(report);
    }

    /* Restart the query heap once every recorded query has been read back,
     * so the rollover drain before a draw stays rare. */
    if (QSIMPLEQ_EMPTY(&r->report_queue) && !r->query_in_flight &&
        r->num_queries_in_flight != 0 &&
        r->resolved_submitted_count == r->num_queries_in_flight &&
        r->resolved_submitted_fence <= completed) {
        while (r->num_results_consumed < r->num_queries_in_flight) {
            r->zpass_pixel_count_result +=
                query_results[r->num_results_consumed++];
        }
        r->num_queries_in_flight = 0;
        r->num_queries_resolved = 0;
        r->num_results_consumed = 0;
        r->resolved_submitted_count = 0;
    }
}

void pgraph_d3d12_flush_reports(NV2AState *d)
{
    pgraph_d3d12_drain_zpass_queries(d, "explicit report flush");
}

/*
 * Called from a PGRAPH method (SET_CONTEXT_DMA_REPORT) with pg->lock held.
 * Reports carry the DMA object they were issued with, so nothing has to be
 * forced out here; ready ones are written without waiting.
 */
void pgraph_d3d12_process_pending_reports(NV2AState *d)
{
    if (d->pgraph.d3d12_renderer_state == NULL) {
        return;
    }
    stamp_pending_reports(d);
    pgraph_d3d12_consume_ready_reports(d);
}

/* New guest commands or a sync/download request end a report wait early:
 * the FIFO serves them and comes back to the report on its next pass. */
static bool report_wait_interrupted(void *opaque)
{
    NV2AState *d = opaque;
    PGRAPHD3D12State *r = d->pgraph.d3d12_renderer_state;

    return qatomic_read(&d->pfifo.fifo_kick) ||
           qatomic_read(&d->pgraph.sync_pending) ||
           qatomic_read(&d->pgraph.flush_pending) ||
           qatomic_read(&r->downloads_pending) ||
           qatomic_read(&r->download_dirty_surfaces_pending);
}

/*
 * Called by the FIFO loop with pfifo.lock held and pg->lock not held.
 * Report state is renderer state, so it is handled under pg->lock;
 * pfifo.lock is released so the vCPU can reach the FIFO registers, and the
 * GPU wait holds no lock at all.
 */
void pgraph_d3d12_process_pending_reports_fifo(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    bool fifo_idle;
    uint64_t wait_fence = 0;

    if (r == NULL) {
        return;
    }
    fifo_idle = d->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET] ==
                d->pfifo.regs[NV_PFIFO_CACHE1_DMA_PUT];

    qemu_mutex_unlock(&d->pfifo.lock);
    qemu_mutex_lock(&d->pgraph.lock);

    stamp_pending_reports(d);
    pgraph_d3d12_consume_ready_reports(d);

    /* Diagnostic: write every pending report before the FIFO goes on,
     * like a renderer that reads query results synchronously. */
    if ((r->tune_flags & PGRAPH_D3D12_TUNE_REPORT_SYNC) &&
        reports_need_completion(r)) {
        pgraph_d3d12_drain_zpass_queries(d, "synchronous reports");
        qemu_mutex_unlock(&d->pgraph.lock);
        qemu_mutex_lock(&d->pfifo.lock);
        return;
    }

    /* The FIFO sleeps until the guest's reports are written, so the queued
     * reports are submitted now, past the submit throttle. The wait covers
     * the first one: the guest usually polls that one first, and each pass
     * consumes whatever has completed. Fences are stamped in queue order. */
    if (fifo_idle && reports_need_completion(r)) {
        PGRAPHD3D12QueryReport *report;
        PGRAPHD3D12QueryReport *last = NULL;

        QSIMPLEQ_FOREACH(report, &r->report_queue, entry) {
            if (!report->clear) {
                last = report;
                if (r->tune_report_wait_head) {
                    break;
                }
            }
        }
        if (last->fence_value == 0 && r->command_list_open) {
            Error *submit_err = NULL;

            r->perf.submit_src[6]++;
            if (pgraph_d3d12_submit_commands_async(pg, &submit_err)) {
                r->last_report_submit_us = g_get_monotonic_time();
                QSIMPLEQ_FOREACH(report, &r->report_queue, entry) {
                    if (report->fence_value == 0 &&
                        report->query_count <=
                            r->resolved_submitted_count) {
                        report->fence_value = r->resolved_submitted_fence;
                    }
                }
            } else {
                error_free(submit_err);
            }
        }

        if (last->fence_value != 0 &&
            last->query_count <= r->num_queries_resolved) {
            wait_fence = last->fence_value;
        } else {
            pgraph_d3d12_drain_zpass_queries(d, "FIFO stalled on report");
        }
    }
    qemu_mutex_unlock(&d->pgraph.lock);

    if (wait_fence != 0) {
        int64_t wait_start = g_get_monotonic_time();
        uint64_t completed = r->fence->lpVtbl->GetCompletedValue(r->fence);
        uint64_t wait_us;
        bool reached;

        reached = pgraph_d3d12_wait_for_fence_interruptible(
            pg, wait_fence, report_wait_interrupted, d);
        wait_us = g_get_monotonic_time() - wait_start;
        qemu_mutex_lock(&d->pgraph.lock);
        r->perf.report_waits++;
        r->perf.report_wait_breaks += !reached;
        r->perf.report_wait_max_us = MAX(r->perf.report_wait_max_us, wait_us);
        r->perf.report_wait_lag += wait_fence > completed ?
                                       wait_fence - completed : 0;
        pgraph_d3d12_buffers_retire_completed(r);
        pgraph_d3d12_process_deferred_releases(pg);
        r->perf.report_flush_us += g_get_monotonic_time() - wait_start;
        r->perf.report_flushes++;
        pgraph_d3d12_consume_ready_reports(d);
        qemu_mutex_unlock(&d->pgraph.lock);
    }

    qemu_mutex_lock(&d->pfifo.lock);
}

void pgraph_d3d12_prepare_zpass_queries(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;

    if (r == NULL) {
        return;
    }

    if (!pg->zpass_pixel_count_enable || pg->clearing) {
        pgraph_d3d12_end_zpass_query(d);
        return;
    }

    if (r->num_queries_in_flight < r->max_queries_in_flight) {
        return;
    }

    pgraph_d3d12_end_zpass_query(d);
    pgraph_d3d12_drain_zpass_queries(d, "query heap rollover before draw");
}

bool pgraph_d3d12_begin_zpass_query(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    uint32_t query_index;

    if (r == NULL || !pg->zpass_pixel_count_enable || pg->clearing) {
        return false;
    }

    if (r->query_in_flight) {
        return true;
    }

    if (r->command_list == NULL || !r->command_list_open ||
        r->query_heap == NULL || r->query_readback_buffer.resource == NULL) {
        pgraph_d3d12_warn_once(
            pg, PGRAPH_D3D12_DIAG_ZPASS_QUERY,
            "zpass query skipped: command_list=%p command_list_open=%d "
            "query_heap=%p query_readback_resource=%p zpass_enable=%d "
            "clearing=%d",
            r->command_list, r->command_list_open, r->query_heap,
            r->query_readback_buffer.resource, pg->zpass_pixel_count_enable,
            pg->clearing);
        return false;
    }

    if (r->num_queries_in_flight >= r->max_queries_in_flight) {
        pgraph_d3d12_warn_once(
            pg, PGRAPH_D3D12_DIAG_ZPASS_QUERY,
            "zpass query skipped: query heap is full; "
            "queries_in_flight=%u, max_queries=%u, "
            "queued_reports_pending=%d, zpass_result_accumulator=%" PRIu64,
            r->num_queries_in_flight, r->max_queries_in_flight,
            !QSIMPLEQ_EMPTY(&r->report_queue),
            r->zpass_pixel_count_result);
        return false;
    }

    query_index = r->num_queries_in_flight;
    r->command_list->lpVtbl->BeginQuery(r->command_list, r->query_heap,
                                        D3D12_QUERY_TYPE_OCCLUSION,
                                        query_index);
    r->query_in_flight = true;
    r->num_queries_in_flight++;
    return true;
}

void pgraph_d3d12_end_zpass_query(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    uint32_t query_index;

    if (r == NULL || !r->query_in_flight) {
        return;
    }

    assert(r->num_queries_in_flight > 0);
    query_index = r->num_queries_in_flight - 1;

    r->command_list->lpVtbl->EndQuery(r->command_list, r->query_heap,
                                      D3D12_QUERY_TYPE_OCCLUSION,
                                      query_index);
    /* Resolves are batched into one ResolveQueryData at submission: the Xbox
     * UWP AMD driver mishandles mid-render-pass resolves after every draw. */
    r->query_in_flight = false;
}

void pgraph_d3d12_flush_pending_query_resolves(PGRAPHD3D12State *r)
{
    uint32_t ended_queries;
    uint32_t pending;

    if (r == NULL || r->command_list == NULL || !r->command_list_open ||
        r->query_heap == NULL ||
        r->query_readback_buffer.resource == NULL) {
        return;
    }

    /* Resolve only segments that have reached EndQuery; a newer one may still
     * be recording. */
    ended_queries = r->num_queries_in_flight - (r->query_in_flight ? 1 : 0);
    if (ended_queries <= r->num_queries_resolved) {
        return;
    }

    pending = ended_queries - r->num_queries_resolved;
    r->command_list->lpVtbl->ResolveQueryData(
        r->command_list, r->query_heap, D3D12_QUERY_TYPE_OCCLUSION,
        r->num_queries_resolved, pending,
        r->query_readback_buffer.resource,
        (uint64_t)r->num_queries_resolved * sizeof(uint64_t));
    r->num_queries_resolved = ended_queries;
}
