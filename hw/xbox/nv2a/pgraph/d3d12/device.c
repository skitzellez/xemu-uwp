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

#include <d3d12sdklayers.h>

static const char *shader_model_name(D3D_SHADER_MODEL model)
{
    switch ((unsigned int)model) {
    case (unsigned int)D3D_SHADER_MODEL_6_4:
        return "6.4";
    case (unsigned int)D3D_SHADER_MODEL_6_3:
        return "6.3";
    case (unsigned int)D3D_SHADER_MODEL_6_2:
        return "6.2";
    case (unsigned int)D3D_SHADER_MODEL_6_1:
        return "6.1";
    case (unsigned int)D3D_SHADER_MODEL_6_0:
        return "6.0";
    case (unsigned int)D3D_SHADER_MODEL_5_1:
        return "5.1";
    default:
        return "unknown";
    }
}

static bool d3d12_device_env_flag(const char *name)
{
    const char *value = getenv(name);

    return value != NULL && value[0] != '\0' &&
           g_ascii_strcasecmp(value, "0") != 0 &&
           g_ascii_strcasecmp(value, "false") != 0 &&
           g_ascii_strcasecmp(value, "off") != 0;
}

const char *pgraph_d3d12_hresult_name(HRESULT hr)
{
    switch (hr) {
    case S_OK:
        return "S_OK";
    case DXGI_ERROR_DEVICE_HUNG:
        return "DXGI_ERROR_DEVICE_HUNG";
    case DXGI_ERROR_DEVICE_REMOVED:
        return "DXGI_ERROR_DEVICE_REMOVED";
    case DXGI_ERROR_DEVICE_RESET:
        return "DXGI_ERROR_DEVICE_RESET";
    case DXGI_ERROR_DRIVER_INTERNAL_ERROR:
        return "DXGI_ERROR_DRIVER_INTERNAL_ERROR";
    case DXGI_ERROR_INVALID_CALL:
        return "DXGI_ERROR_INVALID_CALL";
    case E_OUTOFMEMORY:
        return "E_OUTOFMEMORY";
    case E_INVALIDARG:
        return "E_INVALIDARG";
    default:
        return "unknown";
    }
}

const char *pgraph_d3d12_device_removed_detail(PGRAPHD3D12State *r,
                                               HRESULT hr, char *buf,
                                               size_t buflen)
{
    HRESULT reason;

    if (buflen == 0) {
        return "";
    }

    buf[0] = '\0';
    if (r == NULL || r->device == NULL ||
        (hr != DXGI_ERROR_DEVICE_REMOVED &&
         hr != DXGI_ERROR_DEVICE_RESET &&
         hr != DXGI_ERROR_DEVICE_HUNG)) {
        return buf;
    }

    reason = r->device->lpVtbl->GetDeviceRemovedReason(r->device);
    snprintf(buf, buflen, ", device_removed_reason=%s (0x%08lx)",
             pgraph_d3d12_hresult_name(reason), (unsigned long)reason);
    return buf;
}

static void copy_adapter_description(PGRAPHD3D12State *r,
                                     const DXGI_ADAPTER_DESC1 *desc,
                                     UINT adapter_index)
{
    int len = WideCharToMultiByte(CP_UTF8, 0, desc->Description, -1,
                                  r->adapter_description,
                                  sizeof(r->adapter_description),
                                  NULL, NULL);
    if (len <= 0) {
        snprintf(r->adapter_description, sizeof(r->adapter_description),
                 "adapter %u", adapter_index);
    }
}

static bool adapter_supports_target(IDXGIAdapter1 *adapter,
                                    ID3D12Device **device,
                                    D3D_SHADER_MODEL *shader_model)
{
    ID3D12Device *candidate = NULL;
    HRESULT hr;

    hr = D3D12CreateDevice((IUnknown *)adapter, D3D_FEATURE_LEVEL_11_0,
                           &IID_ID3D12Device, (void **)&candidate);
    if (FAILED(hr)) {
        return false;
    }

    D3D12_FEATURE_DATA_SHADER_MODEL sm = {
        .HighestShaderModel = D3D_SHADER_MODEL_6_4,
    };
    hr = candidate->lpVtbl->CheckFeatureSupport(candidate,
                                                D3D12_FEATURE_SHADER_MODEL,
                                                &sm, sizeof(sm));
    if (FAILED(hr) || sm.HighestShaderModel < D3D_SHADER_MODEL_6_4) {
        D3D12_RELEASE(candidate);
        return false;
    }

    *device = candidate;
    *shader_model = sm.HighestShaderModel;
    return true;
}

void pgraph_d3d12_init_device(PGRAPHState *pg, Error **errp)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    HRESULT hr;
    ID3D12Debug *debug = NULL;

    r->feature_level = D3D_FEATURE_LEVEL_11_0;
    r->shader_model = D3D_SHADER_MODEL_6_4;
    r->resource_binding_tier = D3D12_RESOURCE_BINDING_TIER_1;
    r->output_merger_logic_op = false;

    if (d3d12_device_env_flag("XEMU_D3D12_DEBUG_LAYER")) {
        hr = D3D12GetDebugInterface(&IID_ID3D12Debug, (void **)&debug);
        if (SUCCEEDED(hr) && debug != NULL) {
            debug->lpVtbl->EnableDebugLayer(debug);
            r->debug_layer_enabled = true;
            D3D12_RELEASE(debug);
        }
    }

    hr = CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&r->factory);
    if (FAILED(hr)) {
        error_setg(errp, "Failed to create DXGI factory: HRESULT 0x%08lx",
                   (unsigned long)hr);
        return;
    }

    /* XEMU_D3D12_WARP=1 selects the strict WARP software rasterizer. */
    bool want_warp = d3d12_device_env_flag("XEMU_D3D12_WARP");

    for (UINT i = 0; ; i++) {
        IDXGIAdapter1 *adapter = NULL;
        DXGI_ADAPTER_DESC1 desc;
        ID3D12Device *device = NULL;
        D3D_SHADER_MODEL shader_model = D3D_SHADER_MODEL_5_1;

        if (want_warp) {
            IDXGIFactory4 *factory4 = NULL;

            hr = DXGI_ERROR_NOT_FOUND;
            if (i == 0 &&
                SUCCEEDED(r->factory->lpVtbl->QueryInterface(
                    r->factory, &IID_IDXGIFactory4, (void **)&factory4))) {
                hr = factory4->lpVtbl->EnumWarpAdapter(
                    factory4, &IID_IDXGIAdapter1, (void **)&adapter);
                factory4->lpVtbl->Release(factory4);
            }
        } else {
            hr = r->factory->lpVtbl->EnumAdapters1(r->factory, i, &adapter);
        }
        if (hr == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (FAILED(hr)) {
            continue;
        }

        memset(&desc, 0, sizeof(desc));
        hr = adapter->lpVtbl->GetDesc1(adapter, &desc);
        if (FAILED(hr) ||
            (!want_warp && (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))) {
            D3D12_RELEASE(adapter);
            continue;
        }

        if (!adapter_supports_target(adapter, &device, &shader_model)) {
            D3D12_RELEASE(adapter);
            continue;
        }

        r->adapter = adapter;
        r->device = device;
        r->shader_model = shader_model;
        r->adapter_luid = desc.AdapterLuid;
        r->dedicated_video_memory = desc.DedicatedVideoMemory;
        r->video_memory_budget = desc.DedicatedVideoMemory;
        {
            IDXGIAdapter3 *adapter3 = NULL;
            DXGI_QUERY_VIDEO_MEMORY_INFO info;

            if (SUCCEEDED(adapter->lpVtbl->QueryInterface(
                    adapter, &IID_IDXGIAdapter3, (void **)&adapter3))) {
                if (SUCCEEDED(adapter3->lpVtbl->QueryVideoMemoryInfo(
                        adapter3, 0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL,
                        &info)) &&
                    info.Budget > r->video_memory_budget) {
                    r->video_memory_budget = info.Budget;
                }
                D3D12_RELEASE(adapter3);
            }
        }
        copy_adapter_description(r, &desc, i);

        D3D12_FEATURE_DATA_D3D12_OPTIONS options;
        memset(&options, 0, sizeof(options));
        hr = r->device->lpVtbl->CheckFeatureSupport(r->device,
                                                    D3D12_FEATURE_D3D12_OPTIONS,
                                                    &options, sizeof(options));
        if (SUCCEEDED(hr)) {
            r->resource_binding_tier = options.ResourceBindingTier;
            r->output_merger_logic_op = options.OutputMergerLogicOp;
        }

        {
            /* Existing heaps would let the GPU write straight into guest RAM. */
            D3D12_FEATURE_DATA_EXISTING_HEAPS existing_heaps;
            HRESULT ehr;

            memset(&existing_heaps, 0, sizeof(existing_heaps));
            ehr = r->device->lpVtbl->CheckFeatureSupport(
                r->device, D3D12_FEATURE_EXISTING_HEAPS, &existing_heaps,
                sizeof(existing_heaps));
            fprintf(stderr,
                    "Direct3D 12 renderer: existing_heaps=%s\n",
                    SUCCEEDED(ehr) && existing_heaps.Supported ? "yes" :
                                                                 "no");
        }

        fprintf(stderr,
                "Direct3D 12 renderer: selected adapter \"%s\" "
                "(vendor=0x%04x device=0x%04x luid=%08lx:%08lx), "
                "feature_level=11_0 shader_model=%s "
                "dedicated_video_memory=%llu MiB budget=%llu MiB "
                "resource_binding_tier=%u output_merger_logic_op=%s "
                "debug_layer=%s\n",
                r->adapter_description,
                (unsigned int)desc.VendorId,
                (unsigned int)desc.DeviceId,
                (unsigned long)desc.AdapterLuid.HighPart,
                (unsigned long)desc.AdapterLuid.LowPart,
                shader_model_name(r->shader_model),
                (unsigned long long)(r->dedicated_video_memory /
                                     (1024ull * 1024ull)),
                (unsigned long long)(r->video_memory_budget >> 20),
                (unsigned int)r->resource_binding_tier,
                r->output_merger_logic_op ? "yes" : "no",
                r->debug_layer_enabled ? "yes" : "no");

        return;
    }

    error_setg(errp,
               "Failed to find a Direct3D 12 adapter supporting feature level "
               "11_0 and shader model %s",
               shader_model_name(D3D_SHADER_MODEL_6_4));
}

void pgraph_d3d12_finalize_device(PGRAPHState *pg)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;

    if (r == NULL) {
        return;
    }

    D3D12_RELEASE(r->device);
    D3D12_RELEASE(r->adapter);
    D3D12_RELEASE(r->factory);
}

static const char *message_severity_name(D3D12_MESSAGE_SEVERITY severity)
{
    switch (severity) {
    case D3D12_MESSAGE_SEVERITY_CORRUPTION:
        return "corruption";
    case D3D12_MESSAGE_SEVERITY_ERROR:
        return "error";
    case D3D12_MESSAGE_SEVERITY_WARNING:
        return "warning";
    case D3D12_MESSAGE_SEVERITY_INFO:
        return "info";
    case D3D12_MESSAGE_SEVERITY_MESSAGE:
        return "message";
    default:
        return "unknown";
    }
}

void pgraph_d3d12_report_info_queue(PGRAPHD3D12State *r,
                                    const char *operation)
{
    ID3D12InfoQueue *queue = NULL;
    UINT64 num_messages;
    UINT64 first_message;
    HRESULT hr;

    if (r == NULL || r->device == NULL) {
        return;
    }

    hr = r->device->lpVtbl->QueryInterface(
        r->device, &IID_ID3D12InfoQueue, (void **)&queue);
    if (FAILED(hr) || queue == NULL) {
        if (r->debug_layer_enabled) {
            fprintf(stderr,
                    "Direct3D 12 debug queue unavailable while reporting %s: "
                    "QueryInterface(ID3D12InfoQueue) HRESULT 0x%08lx\n",
                    operation, (unsigned long)hr);
        }
        return;
    }

    num_messages =
        queue->lpVtbl->GetNumStoredMessagesAllowedByRetrievalFilter(queue);
    if (num_messages == 0) {
        queue->lpVtbl->ClearStoredMessages(queue);
        D3D12_RELEASE(queue);
        return;
    }

    first_message = num_messages > 16 ? num_messages - 16 : 0;
    fprintf(stderr,
            "Direct3D 12 debug messages for %s: showing %llu of %llu\n",
            operation,
            (unsigned long long)(num_messages - first_message),
            (unsigned long long)num_messages);

    for (UINT64 i = first_message; i < num_messages; i++) {
        SIZE_T length = 0;
        D3D12_MESSAGE *message;

        hr = queue->lpVtbl->GetMessage(queue, i, NULL, &length);
        if (FAILED(hr) || length == 0) {
            fprintf(stderr,
                    "Direct3D 12 debug message %llu could not be sized: "
                    "HRESULT 0x%08lx, length=%zu\n",
                    (unsigned long long)i, (unsigned long)hr, length);
            continue;
        }

        message = g_malloc(length);
        hr = queue->lpVtbl->GetMessage(queue, i, message, &length);
        if (FAILED(hr)) {
            fprintf(stderr,
                    "Direct3D 12 debug message %llu could not be read: "
                    "HRESULT 0x%08lx\n",
                    (unsigned long long)i, (unsigned long)hr);
            g_free(message);
            continue;
        }

        fprintf(stderr,
                "Direct3D 12 debug: severity=%s(%u), category=%u, id=%u: "
                "%.*s\n",
                message_severity_name(message->Severity),
                (unsigned int)message->Severity,
                (unsigned int)message->Category,
                (unsigned int)message->ID,
                (int)MIN(message->DescriptionByteLength, 4096),
                message->pDescription != NULL ? message->pDescription : "");
        g_free(message);
    }

    queue->lpVtbl->ClearStoredMessages(queue);
    D3D12_RELEASE(queue);
}
