/*
 * xemu Direct3D 12 presenter
 *
 * Copyright (c) 2026 danprice142 (https://github.com/danprice142/xemu-uwp)
 * Changed for xemu on Xbox Dev Mode by Skitzelles (https://github.com/skitzellez/xemu-uwp)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"

#if defined(CONFIG_D3D12) && defined(_WIN32)

#include "ui/d3d12-presenter.h"
#include "ui/xemu-settings.h"

#include <SDL3/SDL.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <fpng.h>
#include <inttypes.h>
#include <math.h>
#include <vector>

extern "C" {
#include "hw/xbox/nv2a/nv2a.h"
}
#include "hw/xbox/nv2a/nv2a_regs.h"
#include "ui/d3d12-fsr.h"
#include "ui/d3d12-presenter-compat.h"
#include "ui/xemu-widescreen.h"

#define XEMU_D3D12_BACKBUFFER_COUNT 3
/* One present waiting at most, as with Mesa (2 buffers, glFinish before each swap): with 2 the controller
 * answered a frame later. "max_latency=2" in d3d12_tune.txt brings the old queue back for comparison. */
#define XEMU_D3D12_MAX_FRAME_LATENCY 1
#define XEMU_D3D12_STREAM_CAPTURES 3
#define XEMU_D3D12_SRV_COUNT 1024
#define XEMU_D3D12_FONT_SRV_INDEX 0
#define XEMU_D3D12_FRAME_SRV_BASE 1
#define XEMU_D3D12_FRAME_SRV_STRIDE 3
#define XEMU_D3D12_SOURCE_SRV_INDEX(frame) \
    (XEMU_D3D12_FRAME_SRV_BASE + (frame) * XEMU_D3D12_FRAME_SRV_STRIDE)
#define XEMU_D3D12_PVIDEO_SRV_INDEX(frame) \
    (XEMU_D3D12_SOURCE_SRV_INDEX(frame) + 1)
#define XEMU_D3D12_PALETTE_SRV_INDEX(frame) \
    (XEMU_D3D12_SOURCE_SRV_INDEX(frame) + 2)
#define XEMU_D3D12_FSR_COMPOSE_SRV_INDEX \
    XEMU_D3D12_SOURCE_SRV_INDEX(XEMU_D3D12_BACKBUFFER_COUNT)
#define XEMU_D3D12_FSR_UPSCALE_SRV_INDEX (XEMU_D3D12_FSR_COMPOSE_SRV_INDEX + 1)
#define XEMU_D3D12_FSR_COMPOSE_RTV_INDEX XEMU_D3D12_BACKBUFFER_COUNT
#define XEMU_D3D12_FSR_UPSCALE_RTV_INDEX (XEMU_D3D12_BACKBUFFER_COUNT + 1)
#define XEMU_D3D12_RTV_COUNT (XEMU_D3D12_BACKBUFFER_COUNT + 2)
#define XEMU_D3D12_PALETTE_ENTRIES 256
/* 32-bit values in the display shader's DisplayConstants. */
#define XEMU_D3D12_DISPLAY_CONSTANTS 28

static_assert(XEMU_D3D12_FSR_UPSCALE_SRV_INDEX <
                  XEMU_D3D12_PRESENTER_SRV_COUNT,
              "presenter SRVs overlap the HUD range");
static_assert(XEMU_D3D12_FSR_CONSTANTS <= XEMU_D3D12_DISPLAY_CONSTANTS,
              "FSR constants exceed the root constant range");

/* Backbuffer readback slot for the stream encoder. The copy is recorded in
 * the frame command list and mapped on a later frame once its fence passes. */
struct XemuD3D12StreamCapture {
    ID3D12Resource *readback;
    void *mapped;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout;
    UINT width;
    UINT height;
    uint64_t fence_value;
    int64_t capture_us;
    bool in_flight;
    volatile LONG encoder_owned;
};

struct XemuD3D12PresenterFrame {
    ID3D12CommandAllocator *command_allocator;
    uint64_t fence_value;

    /* D3D12 command lists do not retain referenced resources. Keep the
     * renderer-owned scanout alive until this frame context's fence passes. */
    ID3D12Resource *source_resource;

    ID3D12Resource *cpu_surface_texture;
    ID3D12Resource *cpu_surface_upload;
    UINT64 cpu_surface_upload_capacity;
    D3D12_RESOURCE_STATES cpu_surface_texture_state;
    UINT cpu_surface_width;
    UINT cpu_surface_height;

    ID3D12Resource *pvideo_texture;
    ID3D12Resource *pvideo_upload;
    UINT64 pvideo_upload_capacity;
    D3D12_RESOURCE_STATES pvideo_texture_state;
    UINT pvideo_width;
    UINT pvideo_height;

    /* NV_PRAMDAC gamma ramp, packed like the GL blit shader palette. */
    ID3D12Resource *palette_upload;
    void *palette_mapped;
    uint8_t palette_cache[XEMU_D3D12_PALETTE_ENTRIES * 3];
    bool palette_valid;
};

struct XemuD3D12Presenter {
    SDL_Window *window;
#ifdef XEMU_UWP
    IUnknown *winrt_window;
#else
    HWND hwnd;
#endif

    ID3D12Device *device;
    ID3D12CommandQueue *queue;
    IDXGIFactory4 *factory;
    IDXGISwapChain3 *swapchain;
    /* Renderer fences for the scanout read by the frame being recorded. */
    ID3D12Fence *frame_ready_fence;
    uint64_t frame_ready_value;
    ID3D12Fence *frame_release_fence;
    uint64_t frame_release_value;
    /* Signalled when the swapchain can take another frame. */
    HANDLE frame_latency_waitable;

    ID3D12DescriptorHeap *rtv_heap;
    UINT rtv_descriptor_size;
    ID3D12Resource *backbuffers[XEMU_D3D12_BACKBUFFER_COUNT];
    D3D12_RESOURCE_STATES backbuffer_states[XEMU_D3D12_BACKBUFFER_COUNT];
    UINT frame_index;
    UINT width;
    UINT height;

    ID3D12DescriptorHeap *srv_heap;
    UINT srv_descriptor_size;

    XemuD3D12PresenterFrame frames[XEMU_D3D12_BACKBUFFER_COUNT];
    ID3D12GraphicsCommandList *command_list;
    ID3D12Fence *fence;
    HANDLE fence_event;
    uint64_t fence_value;

    ID3D12RootSignature *display_root_signature;
    ID3D12PipelineState *display_pso;

    /* FSR 1: the composed scanout at source size is upscaled (EASU) into
     * fsr_upscale, then sharpened (RCAS) into the backbuffer. */
    ID3D12PipelineState *fsr_easu_pso;
    ID3D12PipelineState *fsr_rcas_pso;
    ID3D12Resource *fsr_compose;
    ID3D12Resource *fsr_upscale;
    UINT fsr_input_width;
    UINT fsr_input_height;
    UINT fsr_output_width;
    UINT fsr_output_height;
    bool fsr_unavailable;

    char *pending_screenshot_path;
    char error[512];
    bool logged_active;
    uint64_t frame_counter;

    XemuD3D12StreamCapture stream_captures[XEMU_D3D12_STREAM_CAPTURES];
};

template <typename T>
static void d3d12_release(T **object)
{
    if (object != NULL && *object != NULL) {
        (*object)->Release();
        *object = NULL;
    }
}

static const char *hresult_name(HRESULT hr)
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

static bool set_error(XemuD3D12Presenter *p, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(p->error, sizeof(p->error), fmt, ap);
    va_end(ap);
    return false;
}

static bool set_hresult_error(XemuD3D12Presenter *p, HRESULT hr,
                              const char *what)
{
    char device_removed_detail[128] = "";

    if (p != NULL && p->device != NULL &&
        (hr == DXGI_ERROR_DEVICE_REMOVED ||
         hr == DXGI_ERROR_DEVICE_RESET ||
         hr == DXGI_ERROR_DEVICE_HUNG)) {
        HRESULT reason = p->device->GetDeviceRemovedReason();
        snprintf(device_removed_detail, sizeof(device_removed_detail),
                 ", device_removed_reason=%s (0x%08lx)",
                 hresult_name(reason), (unsigned long)reason);
    }

    return set_error(p, "%s failed: HRESULT %s (0x%08lx)%s", what,
                     hresult_name(hr), (unsigned long)hr,
                     device_removed_detail);
}

#if defined(XEMU_UWP) && defined(XEMU_D3D12_PRESENTER_TRACE)
static bool trace_presenter_frame(const XemuD3D12Presenter *p)
{
    return p != NULL && p->frame_counter < 8;
}

static void trace_presenter(XemuD3D12Presenter *p, const char *fmt, ...)
{
    va_list ap;

    if (!trace_presenter_frame(p)) {
        return;
    }

    fprintf(stderr, "Direct3D 12 UWP presenter trace frame=%" PRIu64 ": ",
            p->frame_counter);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}
#else
static void trace_presenter(XemuD3D12Presenter *p, const char *fmt, ...)
{
    (void)p;
    (void)fmt;
}
#endif

static D3D12_CPU_DESCRIPTOR_HANDLE cpu_descriptor(
    ID3D12DescriptorHeap *heap, UINT descriptor_size, UINT index)
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle =
        heap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += (SIZE_T)descriptor_size * index;
    return handle;
}

static D3D12_GPU_DESCRIPTOR_HANDLE gpu_descriptor(
    ID3D12DescriptorHeap *heap, UINT descriptor_size, UINT index)
{
    D3D12_GPU_DESCRIPTOR_HANDLE handle =
        heap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += (UINT64)descriptor_size * index;
    return handle;
}

static bool get_window_size(SDL_Window *window, UINT *width, UINT *height)
{
    int w = 0;
    int h = 0;

    SDL_GetWindowSizeInPixels(window, &w, &h);
    if (w <= 0 || h <= 0) {
        return false;
    }

    *width = (UINT)w;
    *height = (UINT)h;
    return true;
}

static bool create_descriptor_heaps(XemuD3D12Presenter *p)
{
    D3D12_DESCRIPTOR_HEAP_DESC desc = {};
    D3D12_SHADER_RESOURCE_VIEW_DESC null_srv = {};
    HRESULT hr;

    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    desc.NumDescriptors = XEMU_D3D12_RTV_COUNT;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    hr = p->device->CreateDescriptorHeap(
        &desc, IID_PPV_ARGS(&p->rtv_heap));
    if (FAILED(hr)) {
        return set_hresult_error(p, hr, "CreateDescriptorHeap(RTV)");
    }

    p->rtv_descriptor_size =
        p->device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    desc = {};
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.NumDescriptors = XEMU_D3D12_SRV_COUNT;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = p->device->CreateDescriptorHeap(
        &desc, IID_PPV_ARGS(&p->srv_heap));
    if (FAILED(hr)) {
        return set_hresult_error(p, hr, "CreateDescriptorHeap(CBV/SRV/UAV)");
    }

    p->srv_descriptor_size =
        p->device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    null_srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    null_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    null_srv.Shader4ComponentMapping =
        D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    null_srv.Texture2D.MipLevels = 1;
    for (UINT i = 0; i < XEMU_D3D12_BACKBUFFER_COUNT; i++) {
        p->device->CreateShaderResourceView(
            NULL, &null_srv,
            cpu_descriptor(p->srv_heap, p->srv_descriptor_size,
                           XEMU_D3D12_SOURCE_SRV_INDEX(i)));
        p->device->CreateShaderResourceView(
            NULL, &null_srv,
            cpu_descriptor(p->srv_heap, p->srv_descriptor_size,
                           XEMU_D3D12_PVIDEO_SRV_INDEX(i)));
    }

    return true;
}

static bool compile_shader(XemuD3D12Presenter *p, const char *source,
                           const char *entry, const char *target,
                           ID3DBlob **blob)
{
    ID3DBlob *errors = NULL;
    UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
    HRESULT hr = D3DCompile(source, strlen(source), NULL, NULL, NULL, entry,
                            target, flags, 0, blob, &errors);

    if (FAILED(hr)) {
        const char *message = errors != NULL ?
            (const char *)errors->GetBufferPointer() : "no compiler output";
        set_error(p, "D3DCompile(%s) failed: HRESULT 0x%08lx: %s", target,
                  (unsigned long)hr, message);
        d3d12_release(&errors);
        return false;
    }

    d3d12_release(&errors);
    return true;
}

/* Full-screen triangle into one B8G8R8A8 target, on the display root
 * signature. */
static bool create_fullscreen_pso(XemuD3D12Presenter *p, ID3DBlob *vs,
                                  ID3DBlob *ps, ID3D12PipelineState **pso,
                                  const char *what)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc = {};
    HRESULT hr;

    pso_desc.pRootSignature = p->display_root_signature;
    pso_desc.VS.pShaderBytecode = vs->GetBufferPointer();
    pso_desc.VS.BytecodeLength = vs->GetBufferSize();
    pso_desc.PS.pShaderBytecode = ps->GetBufferPointer();
    pso_desc.PS.BytecodeLength = ps->GetBufferSize();
    pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask =
        D3D12_COLOR_WRITE_ENABLE_ALL;
    pso_desc.SampleMask = UINT_MAX;
    pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso_desc.RasterizerState.FrontCounterClockwise = FALSE;
    pso_desc.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    pso_desc.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    pso_desc.RasterizerState.SlopeScaledDepthBias =
        D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    pso_desc.RasterizerState.DepthClipEnable = TRUE;
    pso_desc.DepthStencilState.DepthEnable = FALSE;
    pso_desc.DepthStencilState.StencilEnable = FALSE;
    pso_desc.InputLayout.pInputElementDescs = NULL;
    pso_desc.InputLayout.NumElements = 0;
    pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso_desc.NumRenderTargets = 1;
    pso_desc.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
    pso_desc.SampleDesc.Count = 1;

    hr = p->device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(pso));
    if (FAILED(hr)) {
        *pso = NULL;
        return set_hresult_error(p, hr, what);
    }
    return true;
}

static bool create_display_pipeline(XemuD3D12Presenter *p)
{
    static const char *shader_source =
        "Texture2D source_texture : register(t0);\n"
        "Texture2D pvideo_texture : register(t1);\n"
        "ByteAddressBuffer dac_palette : register(t2);\n"
        "SamplerState linear_sampler : register(s0);\n"
        "SamplerState nearest_sampler : register(s1);\n"
        "cbuffer DisplayConstants : register(b0) {\n"
        "    float4 source_rect;\n"
        "    float4 display_info;\n"
        "    float4 pvideo_pos;\n"
        "    float4 pvideo_transform;\n"
        "    float4 pvideo_tex_size;\n"
        "    float4 pvideo_key;\n"
        "    float4 source_info;\n"
        "};\n"
        "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; float2 source_uv : TEXCOORD1; };\n"
        "VSOut vs_main(uint vertex_id : SV_VertexID) {\n"
        "    float2 pos[3] = { float2(-1.0, -1.0), float2(-1.0, 3.0), float2(3.0, -1.0) };\n"
        "    float2 uv[3] = { float2(0.0, 1.0), float2(0.0, -1.0), float2(2.0, 1.0) };\n"
        "    VSOut output;\n"
        "    output.pos = float4(pos[vertex_id], 0.0, 1.0);\n"
        "    output.uv = source_rect.xy + uv[vertex_id] * source_rect.zw;\n"
        "    output.source_uv = uv[vertex_id];\n"
        "    return output;\n"
        "}\n"
        "float4 ps_main(VSOut input) : SV_Target {\n"
        "    float4 color;\n"
        "    if (pvideo_tex_size.z > 0.5) {\n"
        "        color = source_texture.Sample(nearest_sampler, input.uv);\n"
        "    } else {\n"
        "        color = source_texture.Sample(linear_sampler, input.uv);\n"
        "    }\n"
        /* Raster lines below the scanned-out surface are black. */
        "    bool in_source = input.uv.y < source_info.x;\n"
        "    if (!in_source) {\n"
        "        color = float4(0.0, 0.0, 0.0, 1.0);\n"
        "    }\n"
        "    if (display_info.z > 0.5) {\n"
        "        float2 screen = input.source_uv * display_info.xy;\n"
        "        float2 pvideo_max = pvideo_pos.xy + pvideo_pos.zw;\n"
        "        bool4 clip = bool4(screen.x < pvideo_pos.x, screen.y < pvideo_pos.y,\n"
        "                          screen.x >= pvideo_max.x, screen.y >= pvideo_max.y);\n"
        "        float2 in_xy = pvideo_transform.xy +\n"
        "            (screen - pvideo_pos.xy) * pvideo_transform.zw;\n"
        "        float2 pvideo_uv = in_xy / pvideo_tex_size.xy;\n"
        "        float4 overlay = pvideo_texture.Sample(linear_sampler, pvideo_uv);\n"
        "        if (!any(clip)) {\n"
        /* The colour key matches the unfiltered 8-bit scan-out pixel. */
        "            bool key_match = true;\n"
        "            if (display_info.w > 0.5) {\n"
        "                float3 key_texel = float3(0.0, 0.0, 0.0);\n"
        "                if (in_source) {\n"
        "                    int2 texel = int2(floor(input.uv * source_info.zw));\n"
        "                    key_texel = source_texture.Load(int3(texel, 0)).rgb;\n"
        "                }\n"
        "                uint3 key_bits = (uint3)(saturate(key_texel) * 255.0 + 0.5);\n"
        "                key_match = all(key_bits == (uint3)pvideo_key.rgb);\n"
        "            }\n"
        "            if (key_match) {\n"
        "                color = overlay;\n"
        "            }\n"
        "        }\n"
        "    }\n"
        "    uint3 index = (uint3)(saturate(color.rgb) * 255.0);\n"
        "    color.r = (float)(dac_palette.Load(index.r * 4u) & 0xffu) / 255.0;\n"
        "    color.g = (float)((dac_palette.Load(index.g * 4u) >> 8u) & 0xffu) / 255.0;\n"
        "    color.b = (float)((dac_palette.Load(index.b * 4u) >> 16u) & 0xffu) / 255.0;\n"
        "    return color;\n"
        "}\n";

    D3D12_DESCRIPTOR_RANGE ranges[3] = {};
    D3D12_ROOT_PARAMETER params[2] = {};
    D3D12_STATIC_SAMPLER_DESC samplers[2] = {};
    D3D12_ROOT_SIGNATURE_DESC root_desc = {};
    ID3DBlob *signature = NULL;
    ID3DBlob *signature_error = NULL;
    ID3DBlob *vs = NULL;
    ID3DBlob *ps = NULL;
    HRESULT hr;
    bool ok;

    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 1;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].RegisterSpace = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;

    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[1].NumDescriptors = 1;
    ranges[1].BaseShaderRegister = 1;
    ranges[1].RegisterSpace = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = 1;

    ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[2].NumDescriptors = 1;
    ranges[2].BaseShaderRegister = 2;
    ranges[2].RegisterSpace = 0;
    ranges[2].OffsetInDescriptorsFromTableStart = 2;

    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[0].DescriptorTable.NumDescriptorRanges =
        (UINT)(sizeof(ranges) / sizeof(ranges[0]));
    params[0].DescriptorTable.pDescriptorRanges = ranges;

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].Constants.ShaderRegister = 0;
    params[1].Constants.RegisterSpace = 0;
    params[1].Constants.Num32BitValues = XEMU_D3D12_DISPLAY_CONSTANTS;

    samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].MipLODBias = 0.0f;
    samplers[0].MaxAnisotropy = 1;
    samplers[0].ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    samplers[0].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    samplers[0].MinLOD = 0.0f;
    samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[0].ShaderRegister = 0;
    samplers[0].RegisterSpace = 0;
    samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    samplers[1] = samplers[0];
    samplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    samplers[1].ShaderRegister = 1;

    root_desc.NumParameters = (UINT)(sizeof(params) / sizeof(params[0]));
    root_desc.pParameters = params;
    root_desc.NumStaticSamplers = (UINT)(sizeof(samplers) / sizeof(samplers[0]));
    root_desc.pStaticSamplers = samplers;
    root_desc.Flags =
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    hr = D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                     &signature, &signature_error);
    if (FAILED(hr)) {
        const char *message = signature_error != NULL ?
            (const char *)signature_error->GetBufferPointer() :
            "no serializer output";
        set_error(p,
                  "D3D12SerializeRootSignature failed: HRESULT 0x%08lx: %s",
                  (unsigned long)hr, message);
        d3d12_release(&signature_error);
        return false;
    }

    hr = p->device->CreateRootSignature(
        0, signature->GetBufferPointer(), signature->GetBufferSize(),
        IID_PPV_ARGS(&p->display_root_signature));
    d3d12_release(&signature);
    d3d12_release(&signature_error);
    if (FAILED(hr)) {
        return set_hresult_error(p, hr, "CreateRootSignature");
    }

    if (!compile_shader(p, shader_source, "vs_main", "vs_5_0", &vs) ||
        !compile_shader(p, shader_source, "ps_main", "ps_5_0", &ps)) {
        d3d12_release(&vs);
        d3d12_release(&ps);
        return false;
    }

    ok = create_fullscreen_pso(p, vs, ps, &p->display_pso,
                               "CreateGraphicsPipelineState");
    d3d12_release(&vs);
    d3d12_release(&ps);
    return ok;
}

static bool create_fsr_pipelines(XemuD3D12Presenter *p)
{
    ID3DBlob *vs = NULL;
    ID3DBlob *easu = NULL;
    ID3DBlob *rcas = NULL;
    bool ok;

    ok = compile_shader(p, xemu_d3d12_fsr_shader_source, "fsr_vs", "vs_5_0",
                        &vs) &&
         compile_shader(p, xemu_d3d12_fsr_shader_source, "easu_ps", "ps_5_0",
                        &easu) &&
         compile_shader(p, xemu_d3d12_fsr_shader_source, "rcas_ps", "ps_5_0",
                        &rcas) &&
         create_fullscreen_pso(p, vs, easu, &p->fsr_easu_pso,
                               "CreateGraphicsPipelineState(EASU)") &&
         create_fullscreen_pso(p, vs, rcas, &p->fsr_rcas_pso,
                               "CreateGraphicsPipelineState(RCAS)");
    d3d12_release(&vs);
    d3d12_release(&easu);
    d3d12_release(&rcas);
    if (!ok) {
        d3d12_release(&p->fsr_easu_pso);
        d3d12_release(&p->fsr_rcas_pso);
    }
    return ok;
}

static bool create_command_objects(XemuD3D12Presenter *p)
{
    HRESULT hr;

    for (UINT i = 0; i < XEMU_D3D12_BACKBUFFER_COUNT; i++) {
        hr = p->device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&p->frames[i].command_allocator));
        if (FAILED(hr)) {
            return set_hresult_error(p, hr, "CreateCommandAllocator");
        }
    }

    hr = p->device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        p->frames[0].command_allocator, NULL,
        IID_PPV_ARGS(&p->command_list));
    if (FAILED(hr)) {
        return set_hresult_error(p, hr, "CreateCommandList");
    }

    hr = p->command_list->Close();
    if (FAILED(hr)) {
        return set_hresult_error(p, hr, "Close initial command list");
    }

    hr = p->device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                IID_PPV_ARGS(&p->fence));
    if (FAILED(hr)) {
        return set_hresult_error(p, hr, "CreateFence");
    }

#ifdef XEMU_UWP
    p->fence_event = CreateEventExW(NULL, NULL, 0,
                                    EVENT_MODIFY_STATE | SYNCHRONIZE);
#else
    p->fence_event = CreateEvent(NULL, FALSE, FALSE, NULL);
#endif
    if (p->fence_event == NULL) {
        return set_error(p,
#ifdef XEMU_UWP
                         "CreateEventExW"
#else
                         "CreateEvent"
#endif
                         " failed: win32 error %lu",
                         (unsigned long)GetLastError());
    }

    return true;
}

static void release_backbuffers(XemuD3D12Presenter *p)
{
    for (UINT i = 0; i < XEMU_D3D12_BACKBUFFER_COUNT; i++) {
        d3d12_release(&p->backbuffers[i]);
        p->backbuffer_states[i] = D3D12_RESOURCE_STATE_PRESENT;
    }
}

static void transition_resource(ID3D12GraphicsCommandList *command_list,
                                ID3D12Resource *resource,
                                D3D12_RESOURCE_STATES *state,
                                D3D12_RESOURCE_STATES new_state);

static void release_stream_captures(XemuD3D12Presenter *p);
static void release_stream_capture(XemuD3D12StreamCapture *c);
static void drain_stream_captures(XemuD3D12Presenter *p);
static int record_stream_capture(XemuD3D12Presenter *p);

static void release_frame_transient_uploads(XemuD3D12PresenterFrame *frame)
{
    d3d12_release(&frame->source_resource);
}

static void release_frame_resources(XemuD3D12PresenterFrame *frame)
{
    release_frame_transient_uploads(frame);
    d3d12_release(&frame->cpu_surface_upload);
    frame->cpu_surface_upload_capacity = 0;
    d3d12_release(&frame->pvideo_upload);
    frame->pvideo_upload_capacity = 0;
    d3d12_release(&frame->palette_upload);
    frame->palette_mapped = NULL;
    frame->palette_valid = false;
    d3d12_release(&frame->cpu_surface_texture);
    d3d12_release(&frame->pvideo_texture);
    d3d12_release(&frame->command_allocator);
    frame->cpu_surface_texture_state = D3D12_RESOURCE_STATE_COMMON;
    frame->pvideo_texture_state = D3D12_RESOURCE_STATE_COMMON;
    frame->cpu_surface_width = 0;
    frame->cpu_surface_height = 0;
    frame->pvideo_width = 0;
    frame->pvideo_height = 0;
    frame->fence_value = 0;
}

static bool wait_for_fence_value(XemuD3D12Presenter *p, uint64_t fence_value)
{
    HRESULT hr;
    DWORD wait_result;

    if (fence_value == 0 || p->fence == NULL) {
        return true;
    }

    if (p->fence->GetCompletedValue() >= fence_value) {
        return true;
    }

    hr = p->fence->SetEventOnCompletion(fence_value, p->fence_event);
    if (FAILED(hr)) {
        return set_hresult_error(p, hr, "ID3D12Fence::SetEventOnCompletion");
    }

#ifdef XEMU_UWP
    wait_result = WaitForSingleObjectEx(p->fence_event, INFINITE, FALSE);
#else
    wait_result = WaitForSingleObject(p->fence_event, INFINITE);
#endif
    if (wait_result != WAIT_OBJECT_0) {
        return set_error(p,
#ifdef XEMU_UWP
                         "WaitForSingleObjectEx"
#else
                         "WaitForSingleObject"
#endif
                         "(D3D12 fence %" PRIu64 ") "
                         "failed: result=%lu win32 error=%lu",
                         fence_value, (unsigned long)wait_result,
                         (unsigned long)GetLastError());
    }
    return true;
}

static bool wait_for_frame_context(XemuD3D12Presenter *p,
                                   XemuD3D12PresenterFrame *frame)
{
    if (!wait_for_fence_value(p, frame->fence_value)) {
        return false;
    }

    frame->fence_value = 0;
    release_frame_transient_uploads(frame);
    return true;
}

static bool signal_frame_context(XemuD3D12Presenter *p,
                                 XemuD3D12PresenterFrame *frame,
                                 uint64_t *signaled_value)
{
    uint64_t value;
    HRESULT hr;

    if (p->queue == NULL || p->fence == NULL) {
        if (signaled_value != NULL) {
            *signaled_value = 0;
        }
        return true;
    }

    value = ++p->fence_value;
    hr = p->queue->Signal(p->fence, value);
    if (FAILED(hr)) {
        return set_hresult_error(p, hr, "ID3D12CommandQueue::Signal");
    }

    frame->fence_value = value;
    if (signaled_value != NULL) {
        *signaled_value = value;
    }
    return true;
}

static bool wait_for_gpu(XemuD3D12Presenter *p)
{
    uint64_t fence_value;
    HRESULT hr;

    if (p->queue == NULL || p->fence == NULL) {
        return true;
    }

    fence_value = ++p->fence_value;
    hr = p->queue->Signal(p->fence, fence_value);
    if (FAILED(hr)) {
        return set_hresult_error(p, hr, "ID3D12CommandQueue::Signal");
    }

    return wait_for_fence_value(p, fence_value);
}

static bool save_bgra8_readback_png(
    XemuD3D12Presenter *p, ID3D12Resource *readback,
    const D3D12_PLACED_SUBRESOURCE_FOOTPRINT *layout, UINT width, UINT height,
    const char *path)
{
    D3D12_RANGE read_range = {};
    D3D12_RANGE written_range = {};
    uint8_t *mapped = NULL;
    std::vector<uint8_t> rgb;
    std::vector<uint8_t> png;
    HRESULT hr;

    if (readback == NULL || layout == NULL || width == 0 || height == 0 ||
        path == NULL) {
        return set_error(p, "invalid Direct3D 12 screenshot readback request");
    }

    read_range.Begin = (SIZE_T)layout->Offset;
    read_range.End = (SIZE_T)(layout->Offset +
                              (uint64_t)layout->Footprint.RowPitch * height);
    hr = readback->Map(0, &read_range, (void **)&mapped);
    if (FAILED(hr)) {
        return set_hresult_error(p, hr, "ID3D12Resource::Map(screenshot)");
    }

    rgb.resize((size_t)width * height * 3);
    for (UINT y = 0; y < height; y++) {
        const uint8_t *src = mapped + layout->Offset +
            (uint64_t)y * layout->Footprint.RowPitch;
        uint8_t *dst = rgb.data() + (size_t)y * width * 3;

        for (UINT x = 0; x < width; x++) {
            dst[x * 3 + 0] = src[x * 4 + 2];
            dst[x * 3 + 1] = src[x * 4 + 1];
            dst[x * 3 + 2] = src[x * 4 + 0];
        }
    }

    readback->Unmap(0, &written_range);

    if (!fpng::fpng_encode_image_to_memory(
            rgb.data(), (uint32_t)width, (uint32_t)height, 3, png)) {
        return set_error(p, "failed to encode Direct3D 12 screenshot PNG");
    }

    FILE *fp = qemu_fopen(path, "wb");
    if (fp == NULL) {
        return set_error(p, "failed to open Direct3D 12 screenshot path: %s",
                         path);
    }

    size_t written = fwrite(png.data(), png.size(), 1, fp);
    fclose(fp);
    if (written != 1) {
        return set_error(p, "failed to write Direct3D 12 screenshot path: %s",
                         path);
    }

    return true;
}

static bool record_screenshot_copy(
    XemuD3D12Presenter *p, ID3D12Resource **readback,
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT *layout, UINT *width, UINT *height)
{
    ID3D12Resource *backbuffer = p->backbuffers[p->frame_index];
    D3D12_RESOURCE_DESC desc = {};
    D3D12_HEAP_PROPERTIES heap_props = {};
    D3D12_RESOURCE_DESC buffer_desc = {};
    D3D12_TEXTURE_COPY_LOCATION src = {};
    D3D12_TEXTURE_COPY_LOCATION dst = {};
    UINT num_rows = 0;
    UINT64 row_size = 0;
    UINT64 total_size = 0;
    HRESULT hr;

    if (backbuffer == NULL) {
        return set_error(p, "cannot screenshot null Direct3D 12 backbuffer");
    }

    desc = backbuffer->GetDesc();
    if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
        return set_error(p,
                         "unsupported Direct3D 12 screenshot backbuffer "
                         "format: %u", (unsigned)desc.Format);
    }
    if (desc.SampleDesc.Count != 1) {
        return set_error(p,
                         "unsupported Direct3D 12 screenshot sample count: "
                         "%u", (unsigned)desc.SampleDesc.Count);
    }

    p->device->GetCopyableFootprints(
        &desc, 0, 1, 0, layout, &num_rows, &row_size, &total_size);
    if (total_size == 0 || num_rows == 0) {
        return set_error(p,
                         "Direct3D 12 screenshot footprint is empty: "
                         "rows=%u row_size=%" PRIu64 " total=%" PRIu64,
                         (unsigned)num_rows, row_size, total_size);
    }

    heap_props.Type = D3D12_HEAP_TYPE_READBACK;
    heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

    buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer_desc.Alignment = 0;
    buffer_desc.Width = total_size;
    buffer_desc.Height = 1;
    buffer_desc.DepthOrArraySize = 1;
    buffer_desc.MipLevels = 1;
    buffer_desc.Format = DXGI_FORMAT_UNKNOWN;
    buffer_desc.SampleDesc.Count = 1;
    buffer_desc.SampleDesc.Quality = 0;
    buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    buffer_desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    hr = p->device->CreateCommittedResource(
        &heap_props, D3D12_HEAP_FLAG_NONE, &buffer_desc,
        D3D12_RESOURCE_STATE_COPY_DEST, NULL, IID_PPV_ARGS(readback));
    if (FAILED(hr)) {
        return set_hresult_error(p, hr,
                                 "CreateCommittedResource(screenshot)");
    }

    transition_resource(p->command_list, backbuffer,
                        &p->backbuffer_states[p->frame_index],
                        D3D12_RESOURCE_STATE_COPY_SOURCE);

    src.pResource = backbuffer;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;

    dst.pResource = *readback;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = *layout;

    p->command_list->CopyTextureRegion(&dst, 0, 0, 0, &src, NULL);

    *width = (UINT)desc.Width;
    *height = desc.Height;
    return true;
}

static float get_display_aspect_ratio(UINT width, UINT height)
{
    switch (g_config.display.ui.aspect_ratio) {
    case CONFIG_DISPLAY_UI_ASPECT_RATIO_NATIVE:
        return height != 0 ? (float)width / (float)height : 4.0f / 3.0f;
    case CONFIG_DISPLAY_UI_ASPECT_RATIO_16X9:
        return 16.0f / 9.0f;
    case CONFIG_DISPLAY_UI_ASPECT_RATIO_4X3:
        return 4.0f / 3.0f;
    case CONFIG_DISPLAY_UI_ASPECT_RATIO_AUTO:
    default:
        /* 720p and 1080i modes are always 16:9, flag or not. */
        if (xemu_get_widescreen() ||
            (height != 0 && (uint64_t)width * 9 == (uint64_t)height * 16)) {
            return 16.0f / 9.0f;
        }
        return 4.0f / 3.0f;
    }
}

static void get_display_viewport(XemuD3D12Presenter *p, UINT source_width,
                                 UINT source_height,
                                 D3D12_VIEWPORT *viewport,
                                 D3D12_RECT *scissor)
{
    float target_aspect = get_display_aspect_ratio(source_width, source_height);
    float window_width = (float)p->width;
    float window_height = (float)p->height;
    float output_width = window_width;
    float output_height = window_height;
    float output_x = 0.0f;
    float output_y = 0.0f;

    if (g_config.display.ui.fit == CONFIG_DISPLAY_UI_FIT_CENTER) {
        output_width = (float)source_height * target_aspect;
        output_height = (float)source_height;

        if (output_width > window_width || output_height > window_height) {
            float width_scale = window_width / output_width;
            float height_scale = window_height / output_height;
            float scale = width_scale < height_scale ? width_scale : height_scale;
            output_width *= scale;
            output_height *= scale;
        }
    } else if (g_config.display.ui.fit != CONFIG_DISPLAY_UI_FIT_STRETCH) {
        float window_aspect = window_width / window_height;

        if (window_aspect >= target_aspect) {
            output_height = window_height;
            output_width = output_height * target_aspect;
        } else {
            output_width = window_width;
            output_height = output_width / target_aspect;
        }
    }

    output_x = (window_width - output_width) * 0.5f;
    output_y = (window_height - output_height) * 0.5f;

    viewport->TopLeftX = output_x;
    viewport->TopLeftY = output_y;
    viewport->Width = output_width;
    viewport->Height = output_height;
    viewport->MinDepth = 0.0f;
    viewport->MaxDepth = 1.0f;

    scissor->left = (LONG)floorf(output_x);
    scissor->top = (LONG)floorf(output_y);
    scissor->right = (LONG)ceilf(output_x + output_width);
    scissor->bottom = (LONG)ceilf(output_y + output_height);

    xemu_pointer_aim_set_display_rect(
        scissor->left, scissor->top, scissor->right - scissor->left,
        scissor->bottom - scissor->top);
}

static bool create_backbuffer_rtvs(XemuD3D12Presenter *p)
{
    for (UINT i = 0; i < XEMU_D3D12_BACKBUFFER_COUNT; i++) {
        HRESULT hr = p->swapchain->GetBuffer(
            i, IID_PPV_ARGS(&p->backbuffers[i]));
        if (FAILED(hr)) {
            return set_hresult_error(p, hr, "IDXGISwapChain::GetBuffer");
        }

        D3D12_CPU_DESCRIPTOR_HANDLE handle =
            cpu_descriptor(p->rtv_heap, p->rtv_descriptor_size, i);
        p->device->CreateRenderTargetView(p->backbuffers[i], NULL, handle);
        p->backbuffer_states[i] = D3D12_RESOURCE_STATE_PRESENT;
    }

    p->frame_index = p->swapchain->GetCurrentBackBufferIndex();
    return true;
}

static bool present_black_backbuffer(XemuD3D12Presenter *p)
{
    const float clear_color[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    ID3D12CommandList *command_lists[1];
    XemuD3D12PresenterFrame *frame_ctx = &p->frames[p->frame_index];
    ID3D12Resource *backbuffer = p->backbuffers[p->frame_index];
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    HRESULT hr;

    if (!wait_for_frame_context(p, frame_ctx)) {
        return false;
    }

    hr = frame_ctx->command_allocator->Reset();
    if (FAILED(hr)) {
        return set_hresult_error(p, hr, "ID3D12CommandAllocator::Reset");
    }

    hr = p->command_list->Reset(frame_ctx->command_allocator, NULL);
    if (FAILED(hr)) {
        return set_hresult_error(p, hr, "ID3D12GraphicsCommandList::Reset");
    }

    transition_resource(p->command_list, backbuffer,
                        &p->backbuffer_states[p->frame_index],
                        D3D12_RESOURCE_STATE_RENDER_TARGET);

    rtv = cpu_descriptor(p->rtv_heap, p->rtv_descriptor_size, p->frame_index);
    p->command_list->OMSetRenderTargets(1, &rtv, FALSE, NULL);
    p->command_list->ClearRenderTargetView(rtv, clear_color, 0, NULL);

    transition_resource(p->command_list, backbuffer,
                        &p->backbuffer_states[p->frame_index],
                        D3D12_RESOURCE_STATE_PRESENT);

    hr = p->command_list->Close();
    if (FAILED(hr)) {
        return set_hresult_error(p, hr, "ID3D12GraphicsCommandList::Close");
    }

    command_lists[0] = p->command_list;
    p->queue->ExecuteCommandLists(1, command_lists);

    if (!signal_frame_context(p, frame_ctx, NULL)) {
        return false;
    }

    hr = p->swapchain->Present(0, 0);
    if (FAILED(hr)) {
        return set_hresult_error(p, hr, "IDXGISwapChain::Present");
    }

    p->frame_index = p->swapchain->GetCurrentBackBufferIndex();
    return true;
}

static bool prime_swapchain_black(XemuD3D12Presenter *p)
{
    for (UINT i = 0; i < XEMU_D3D12_BACKBUFFER_COUNT; i++) {
        if (!present_black_backbuffer(p)) {
            return false;
        }
    }

    return wait_for_gpu(p);
}

static UINT presenter_max_frame_latency(void)
{
    g_autofree char *path = g_strdup_printf("%sd3d12_tune.txt",
                                            xemu_settings_get_base_path());
    g_autofree gchar *text = NULL;
    const char *v;
    UINT latency = XEMU_D3D12_MAX_FRAME_LATENCY;

    if (g_file_get_contents(path, &text, NULL, NULL) &&
        (v = strstr(text, "max_latency=")) != NULL) {
        int n = atoi(v + strlen("max_latency="));
        if (n >= 1 && n <= 3) {
            latency = (UINT)n;
        }
    }
    return latency;
}

static bool create_or_resize_swapchain(XemuD3D12Presenter *p)
{
    UINT width;
    UINT height;
    HRESULT hr;

    if (!get_window_size(p->window, &width, &height)) {
        return set_error(p, "window has no drawable D3D12 size");
    }

    if (p->swapchain != NULL && p->width == width && p->height == height) {
        return true;
    }

    if (!wait_for_gpu(p)) {
        return false;
    }

    release_backbuffers(p);

    if (p->swapchain == NULL) {
        DXGI_SWAP_CHAIN_DESC1 desc = {};
        IDXGISwapChain1 *swapchain1 = NULL;

        desc.Width = width;
        desc.Height = height;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.Stereo = FALSE;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = XEMU_D3D12_BACKBUFFER_COUNT;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

#ifdef XEMU_UWP
        hr = p->factory->CreateSwapChainForCoreWindow(
            p->queue, p->winrt_window, &desc, NULL, &swapchain1);
        if (FAILED(hr)) {
            return set_hresult_error(p, hr, "CreateSwapChainForCoreWindow");
        }
#else
        hr = p->factory->CreateSwapChainForHwnd(
            p->queue, p->hwnd, &desc, NULL, NULL, &swapchain1);
        if (FAILED(hr)) {
            return set_hresult_error(p, hr, "CreateSwapChainForHwnd");
        }
#endif

        hr = swapchain1->QueryInterface(IID_PPV_ARGS(&p->swapchain));
        d3d12_release(&swapchain1);
        if (FAILED(hr)) {
            return set_hresult_error(p, hr, "QueryInterface(IDXGISwapChain3)");
        }

        /* The renderer and the swapchain share one queue: with one present
         * waiting there, renderer work never queues behind one that waits
         * for a free back buffer, and the newest game frame is shown. */
        UINT latency = presenter_max_frame_latency();
        if (SUCCEEDED(p->swapchain->SetMaximumFrameLatency(latency))) {
            p->frame_latency_waitable =
                p->swapchain->GetFrameLatencyWaitableObject();
        }
        fprintf(stderr, "Direct3D 12 presenter: %u frame(s) queued at most\n",
                latency);

#ifndef XEMU_UWP
        p->factory->MakeWindowAssociation(p->hwnd, DXGI_MWA_NO_ALT_ENTER);
#endif
    } else {
        hr = p->swapchain->ResizeBuffers(
            XEMU_D3D12_BACKBUFFER_COUNT, width, height,
            DXGI_FORMAT_B8G8R8A8_UNORM,
            DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
        if (FAILED(hr)) {
            return set_hresult_error(p, hr, "IDXGISwapChain::ResizeBuffers");
        }
    }

    p->width = width;
    p->height = height;
    return create_backbuffer_rtvs(p) && prime_swapchain_black(p);
}

static bool initialize_from_frame(XemuD3D12Presenter *p,
                                  const NV2ADisplayFrame *frame)
{
    SDL_PropertiesID props;
    HRESULT hr;

    if (p->device != NULL) {
        return true;
    }

    if (frame == NULL ||
        (frame->type != NV2A_DISPLAY_FRAME_NONE &&
         frame->type != NV2A_DISPLAY_FRAME_D3D12_RESOURCE &&
         frame->type != NV2A_DISPLAY_FRAME_CPU_SURFACE) ||
        frame->d3d12.device == NULL || frame->d3d12.command_queue == NULL) {
        return set_error(
            p,
            "D3D12 display frame is not backed by a device and queue: "
            "type=%d device=%p queue=%p",
            frame != NULL ? frame->type : -1,
            frame != NULL ? frame->d3d12.device : NULL,
            frame != NULL ? frame->d3d12.command_queue : NULL);
    }

    props = SDL_GetWindowProperties(p->window);
#ifdef XEMU_UWP
    p->winrt_window = (IUnknown *)SDL_GetPointerProperty(
        props, SDL_PROP_WINDOW_WINRT_WINDOW_POINTER, NULL);
    if (p->winrt_window == NULL) {
        return set_error(
            p,
            "SDL window does not expose a WinRT CoreWindow "
            "(%s property is NULL)",
            SDL_PROP_WINDOW_WINRT_WINDOW_POINTER);
    }
#else
    p->hwnd = (HWND)SDL_GetPointerProperty(
        props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
    if (p->hwnd == NULL) {
        return set_error(
            p,
            "SDL window does not expose a Win32 HWND "
            "(%s property is NULL)",
            SDL_PROP_WINDOW_WIN32_HWND_POINTER);
    }
#endif

    p->device = (ID3D12Device *)frame->d3d12.device;
    p->device->AddRef();
    /* Own queue: presents wait for vblank there, so emulator work queued
     * on the renderer's queue never sits behind them. */
    {
        D3D12_COMMAND_QUEUE_DESC queue_desc = {};

        queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        hr = p->device->CreateCommandQueue(&queue_desc,
                                           IID_PPV_ARGS(&p->queue));
        if (FAILED(hr)) {
            return set_hresult_error(p, hr, "CreateCommandQueue(presenter)");
        }
    }

    hr = CreateDXGIFactory1(IID_PPV_ARGS(&p->factory));
    if (FAILED(hr)) {
        return set_hresult_error(p, hr, "CreateDXGIFactory1");
    }

    if (!create_descriptor_heaps(p) ||
        !create_display_pipeline(p) ||
        !create_command_objects(p) ||
        !create_or_resize_swapchain(p)) {
        return false;
    }

    if (!p->logged_active) {
#ifdef XEMU_UWP
        fprintf(stderr,
                "Direct3D 12 UWP presenter active: CoreWindow=%p, "
                "backbuffers=%u, format=DXGI_FORMAT_B8G8R8A8_UNORM\n",
                p->winrt_window, (unsigned)XEMU_D3D12_BACKBUFFER_COUNT);
#else
        fprintf(stderr,
                "Direct3D 12 presenter active: hwnd=%p, backbuffers=%u, "
                "format=DXGI_FORMAT_B8G8R8A8_UNORM\n",
                p->hwnd, (unsigned)XEMU_D3D12_BACKBUFFER_COUNT);
#endif
        p->logged_active = true;
    }

    return true;
}

static void transition_resource(ID3D12GraphicsCommandList *command_list,
                                ID3D12Resource *resource,
                                D3D12_RESOURCE_STATES *state,
                                D3D12_RESOURCE_STATES new_state)
{
    D3D12_RESOURCE_BARRIER barrier = {};

    if (resource == NULL || state == NULL || *state == new_state) {
        return;
    }

    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = *state;
    barrier.Transition.StateAfter = new_state;
    command_list->ResourceBarrier(1, &barrier);
    *state = new_state;
}

static bool update_pvideo_texture(XemuD3D12Presenter *p,
                                  XemuD3D12PresenterFrame *frame_ctx,
                                  const NV2ADisplayFrame *frame)
{
    const NV2ADisplayFramePvideo *pvideo = &frame->pvideo;
    D3D12_RESOURCE_DESC texture_desc = {};
    D3D12_HEAP_PROPERTIES heap_props = {};
    D3D12_RESOURCE_DESC upload_desc = {};
    D3D12_TEXTURE_COPY_LOCATION src = {};
    D3D12_TEXTURE_COPY_LOCATION dst = {};
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    void *mapped = NULL;
    UINT64 row_bytes;
    UINT64 required_size;
    UINT64 upload_pitch64;
    UINT64 upload_size;
    UINT upload_pitch;
    HRESULT hr;

    if (!pvideo->enabled) {
        return true;
    }
    if (pvideo->rgba == NULL || pvideo->width == 0 || pvideo->height == 0) {
        return set_error(p,
                         "PVIDEO overlay has invalid RGBA payload: data=%p, "
                         "size=%u, dimensions=%ux%u",
                         pvideo->rgba, pvideo->rgba_size, pvideo->width,
                         pvideo->height);
    }

    row_bytes = (UINT64)pvideo->width * 4u;
    if (row_bytes > UINT32_MAX ||
        row_bytes > UINT64_MAX / pvideo->height) {
        return set_error(
            p,
            "PVIDEO overlay dimensions overflow upload layout: "
            "dimensions=%ux%u row_bytes=%" PRIu64 " payload_size=%u",
            pvideo->width, pvideo->height, row_bytes, pvideo->rgba_size);
    }

    required_size = row_bytes * pvideo->height;
    if (required_size > pvideo->rgba_size) {
        return set_error(
            p,
            "PVIDEO overlay RGBA payload is too small: have=%u need=%" PRIu64
            " dimensions=%ux%u row_bytes=%" PRIu64,
            pvideo->rgba_size, required_size, pvideo->width, pvideo->height,
            row_bytes);
    }

    if (frame_ctx->pvideo_texture == NULL ||
        frame_ctx->pvideo_width != pvideo->width ||
        frame_ctx->pvideo_height != pvideo->height) {
        d3d12_release(&frame_ctx->pvideo_texture);

        texture_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texture_desc.Width = pvideo->width;
        texture_desc.Height = pvideo->height;
        texture_desc.DepthOrArraySize = 1;
        texture_desc.MipLevels = 1;
        texture_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        texture_desc.SampleDesc.Count = 1;
        texture_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

        heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
        hr = p->device->CreateCommittedResource(
            &heap_props, D3D12_HEAP_FLAG_NONE, &texture_desc,
            D3D12_RESOURCE_STATE_COPY_DEST, NULL,
            IID_PPV_ARGS(&frame_ctx->pvideo_texture));
        if (FAILED(hr)) {
            return set_hresult_error(p, hr, "CreateCommittedResource(PVIDEO)");
        }

        frame_ctx->pvideo_width = pvideo->width;
        frame_ctx->pvideo_height = pvideo->height;
        frame_ctx->pvideo_texture_state = D3D12_RESOURCE_STATE_COPY_DEST;

        srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        p->device->CreateShaderResourceView(
            frame_ctx->pvideo_texture, &srv,
            cpu_descriptor(p->srv_heap, p->srv_descriptor_size,
                           XEMU_D3D12_PVIDEO_SRV_INDEX(p->frame_index)));
    } else {
        transition_resource(p->command_list, frame_ctx->pvideo_texture,
                            &frame_ctx->pvideo_texture_state,
                            D3D12_RESOURCE_STATE_COPY_DEST);
    }

    upload_pitch64 = (row_bytes + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u) &
                     ~(UINT64)(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u);
    if (upload_pitch64 > UINT32_MAX ||
        upload_pitch64 > UINT64_MAX / pvideo->height) {
        return set_error(
            p,
            "PVIDEO overlay upload allocation is too large: "
            "dimensions=%ux%u row_bytes=%" PRIu64 " upload_pitch=%" PRIu64,
            pvideo->width, pvideo->height, row_bytes, upload_pitch64);
    }

    upload_pitch = (UINT)upload_pitch64;
    upload_size = upload_pitch64 * pvideo->height;

    if (frame_ctx->pvideo_upload == NULL ||
        frame_ctx->pvideo_upload_capacity < upload_size) {
        d3d12_release(&frame_ctx->pvideo_upload);
        frame_ctx->pvideo_upload_capacity = 0;

        upload_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        upload_desc.Width = upload_size;
        upload_desc.Height = 1;
        upload_desc.DepthOrArraySize = 1;
        upload_desc.MipLevels = 1;
        upload_desc.Format = DXGI_FORMAT_UNKNOWN;
        upload_desc.SampleDesc.Count = 1;
        upload_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        heap_props = {};
        heap_props.Type = D3D12_HEAP_TYPE_UPLOAD;
        hr = p->device->CreateCommittedResource(
            &heap_props, D3D12_HEAP_FLAG_NONE, &upload_desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
            IID_PPV_ARGS(&frame_ctx->pvideo_upload));
        if (FAILED(hr)) {
            return set_hresult_error(
                p, hr, "CreateCommittedResource(PVIDEO upload)");
        }
        frame_ctx->pvideo_upload_capacity = upload_size;
    }

    {
        D3D12_RANGE read_range = { 0, 0 };
        hr = frame_ctx->pvideo_upload->Map(0, &read_range, &mapped);
    }
    if (FAILED(hr)) {
        return set_hresult_error(p, hr, "ID3D12Resource::Map(PVIDEO upload)");
    }

    for (uint32_t y = 0; y < pvideo->height; y++) {
        memcpy((uint8_t *)mapped + (uint64_t)y * upload_pitch,
               (const uint8_t *)pvideo->rgba + (uint64_t)y * row_bytes,
               (size_t)row_bytes);
    }
    frame_ctx->pvideo_upload->Unmap(0, NULL);

    src.pResource = frame_ctx->pvideo_upload;
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    src.PlacedFootprint.Footprint.Width = pvideo->width;
    src.PlacedFootprint.Footprint.Height = pvideo->height;
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = upload_pitch;

    dst.pResource = frame_ctx->pvideo_texture;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    p->command_list->CopyTextureRegion(&dst, 0, 0, 0, &src, NULL);

    transition_resource(p->command_list, frame_ctx->pvideo_texture,
                        &frame_ctx->pvideo_texture_state,
                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return true;
}

static uint8_t expand_5_to_8(uint16_t v)
{
    return (uint8_t)((v << 3) | (v >> 2));
}

static uint8_t expand_6_to_8(uint16_t v)
{
    return (uint8_t)((v << 2) | (v >> 4));
}

static bool write_cpu_surface_bgra8(XemuD3D12Presenter *p,
                                    const NV2ADisplayFrameCpuSurface *surface,
                                    uint8_t *dst, UINT dst_pitch)
{
    const uint8_t *src = (const uint8_t *)surface->data;

    for (uint32_t y = 0; y < surface->height; y++) {
        const uint8_t *src_row = src + (uint64_t)y * surface->pitch;
        uint8_t *dst_row = dst + (uint64_t)y * dst_pitch;

        switch (surface->color_format) {
        case NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_Z8R8G8B8:
        case NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_O8R8G8B8:
        case NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8:
            for (uint32_t x = 0; x < surface->width; x++) {
                const uint8_t *s = src_row + x * 4u;
                uint8_t *d = dst_row + x * 4u;

                d[0] = s[0];
                d[1] = s[1];
                d[2] = s[2];
                d[3] =
                    surface->color_format ==
                            NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8 ?
                        s[3] :
                        0xff;
            }
            break;

        case NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5:
            for (uint32_t x = 0; x < surface->width; x++) {
                uint16_t pixel;
                uint8_t *d = dst_row + x * 4u;

                memcpy(&pixel, src_row + x * 2u, sizeof(pixel));
                d[0] = expand_5_to_8(pixel & 0x1f);
                d[1] = expand_6_to_8((pixel >> 5) & 0x3f);
                d[2] = expand_5_to_8((pixel >> 11) & 0x1f);
                d[3] = 0xff;
            }
            break;

        case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_Z1R5G5B5:
        case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_O1R5G5B5:
            for (uint32_t x = 0; x < surface->width; x++) {
                uint16_t pixel;
                uint8_t *d = dst_row + x * 4u;

                memcpy(&pixel, src_row + x * 2u, sizeof(pixel));
                d[0] = expand_5_to_8(pixel & 0x1f);
                d[1] = expand_5_to_8((pixel >> 5) & 0x1f);
                d[2] = expand_5_to_8((pixel >> 10) & 0x1f);
                d[3] = 0xff;
            }
            break;

        case NV097_SET_SURFACE_FORMAT_COLOR_LE_B8:
            for (uint32_t x = 0; x < surface->width; x++) {
                uint8_t v = src_row[x];
                uint8_t *d = dst_row + x * 4u;

                d[0] = v;
                d[1] = v;
                d[2] = v;
                d[3] = 0xff;
            }
            break;

        case NV097_SET_SURFACE_FORMAT_COLOR_LE_G8B8:
            for (uint32_t x = 0; x < surface->width; x++) {
                const uint8_t *s = src_row + x * 2u;
                uint8_t *d = dst_row + x * 4u;

                d[0] = s[0];
                d[1] = s[1];
                d[2] = s[0];
                d[3] = 0xff;
            }
            break;

        default:
            return set_error(
                p,
                "unsupported Direct3D 12 CPU scanout color format 0x%x "
                "(%ux%u pitch=%u bpp=%u vram=0x%" PRIx64 ")",
                surface->color_format, surface->width, surface->height,
                surface->pitch, surface->bytes_per_pixel,
                (uint64_t)surface->vram_addr);
        }
    }

    return true;
}

static bool update_cpu_surface_texture(XemuD3D12Presenter *p,
                                       XemuD3D12PresenterFrame *frame_ctx,
                                       const NV2ADisplayFrame *frame)
{
    const NV2ADisplayFrameCpuSurface *surface = &frame->cpu;
    D3D12_RESOURCE_DESC texture_desc = {};
    D3D12_HEAP_PROPERTIES heap_props = {};
    D3D12_RESOURCE_DESC upload_desc = {};
    D3D12_TEXTURE_COPY_LOCATION src = {};
    D3D12_TEXTURE_COPY_LOCATION dst = {};
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    void *mapped = NULL;
    UINT64 row_bytes;
    UINT64 upload_pitch64;
    UINT upload_pitch;
    UINT64 upload_size;
    UINT64 source_size;
    HRESULT hr;

    if (surface->data == NULL || surface->width == 0 ||
        surface->height == 0 || surface->bytes_per_pixel == 0 ||
        (uint64_t)surface->pitch <
            (uint64_t)surface->width * surface->bytes_per_pixel) {
        return set_error(
            p,
            "invalid Direct3D 12 CPU scanout payload: data=%p "
            "size=%u dimensions=%ux%u pitch=%u bpp=%u format=0x%x",
            surface->data, surface->data_size, surface->width,
            surface->height, surface->pitch, surface->bytes_per_pixel,
            surface->color_format);
    }

    source_size = (UINT64)(surface->height - 1) * surface->pitch +
                  (UINT64)surface->width * surface->bytes_per_pixel;
    if (source_size > surface->data_size) {
        return set_error(
            p,
            "Direct3D 12 CPU scanout payload is too small: have=%u "
            "need=%" PRIu64 " dimensions=%ux%u pitch=%u bpp=%u "
            "format=0x%x vram=0x%" PRIx64,
            surface->data_size, source_size, surface->width,
            surface->height, surface->pitch, surface->bytes_per_pixel,
            surface->color_format, (uint64_t)surface->vram_addr);
    }

    row_bytes = (UINT64)surface->width * 4u;
    if (row_bytes > UINT32_MAX) {
        return set_error(
            p,
            "Direct3D 12 CPU scanout upload row is too large: "
            "dimensions=%ux%u row_bytes=%" PRIu64
            " pitch=%u bpp=%u format=0x%x vram=0x%" PRIx64,
            surface->width, surface->height, row_bytes, surface->pitch,
            surface->bytes_per_pixel, surface->color_format,
            (uint64_t)surface->vram_addr);
    }

    if (frame_ctx->cpu_surface_texture == NULL ||
        frame_ctx->cpu_surface_width != surface->width ||
        frame_ctx->cpu_surface_height != surface->height) {
        d3d12_release(&frame_ctx->cpu_surface_texture);

        texture_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texture_desc.Width = surface->width;
        texture_desc.Height = surface->height;
        texture_desc.DepthOrArraySize = 1;
        texture_desc.MipLevels = 1;
        texture_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        texture_desc.SampleDesc.Count = 1;
        texture_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

        heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
        hr = p->device->CreateCommittedResource(
            &heap_props, D3D12_HEAP_FLAG_NONE, &texture_desc,
            D3D12_RESOURCE_STATE_COPY_DEST, NULL,
            IID_PPV_ARGS(&frame_ctx->cpu_surface_texture));
        if (FAILED(hr)) {
            return set_hresult_error(
                p, hr, "CreateCommittedResource(CPU scanout)");
        }

        frame_ctx->cpu_surface_width = surface->width;
        frame_ctx->cpu_surface_height = surface->height;
        frame_ctx->cpu_surface_texture_state = D3D12_RESOURCE_STATE_COPY_DEST;
    } else {
        transition_resource(p->command_list, frame_ctx->cpu_surface_texture,
                            &frame_ctx->cpu_surface_texture_state,
                            D3D12_RESOURCE_STATE_COPY_DEST);
    }

    upload_pitch64 = (row_bytes + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u) &
                     ~(UINT64)(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u);
    if (upload_pitch64 > UINT32_MAX ||
        upload_pitch64 > UINT64_MAX / surface->height) {
        return set_error(
            p,
            "Direct3D 12 CPU scanout upload allocation is too large: "
            "dimensions=%ux%u row_bytes=%" PRIu64 " upload_pitch=%" PRIu64
            " pitch=%u bpp=%u format=0x%x vram=0x%" PRIx64,
            surface->width, surface->height, row_bytes, upload_pitch64,
            surface->pitch, surface->bytes_per_pixel, surface->color_format,
            (uint64_t)surface->vram_addr);
    }

    upload_pitch = (UINT)upload_pitch64;
    upload_size = upload_pitch64 * surface->height;

    upload_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    upload_desc.Width = upload_size;
    upload_desc.Height = 1;
    upload_desc.DepthOrArraySize = 1;
    upload_desc.MipLevels = 1;
    upload_desc.Format = DXGI_FORMAT_UNKNOWN;
    upload_desc.SampleDesc.Count = 1;
    upload_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    /* Reused while large enough; the frame context's fence wait already
     * covers its previous use. */
    if (frame_ctx->cpu_surface_upload == NULL ||
        frame_ctx->cpu_surface_upload_capacity < upload_size) {
        d3d12_release(&frame_ctx->cpu_surface_upload);
        frame_ctx->cpu_surface_upload_capacity = 0;
        heap_props = {};
        heap_props.Type = D3D12_HEAP_TYPE_UPLOAD;
        hr = p->device->CreateCommittedResource(
            &heap_props, D3D12_HEAP_FLAG_NONE, &upload_desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
            IID_PPV_ARGS(&frame_ctx->cpu_surface_upload));
        if (FAILED(hr)) {
            return set_hresult_error(
                p, hr, "CreateCommittedResource(CPU scanout upload)");
        }
        frame_ctx->cpu_surface_upload_capacity = upload_size;
    }

    {
        D3D12_RANGE read_range = { 0, 0 };
        hr = frame_ctx->cpu_surface_upload->Map(0, &read_range, &mapped);
    }
    if (FAILED(hr)) {
        return set_hresult_error(
            p, hr, "ID3D12Resource::Map(CPU scanout upload)");
    }

    if (!write_cpu_surface_bgra8(p, surface, (uint8_t *)mapped,
                                 upload_pitch)) {
        frame_ctx->cpu_surface_upload->Unmap(0, NULL);
        return false;
    }
    frame_ctx->cpu_surface_upload->Unmap(0, NULL);

    src.pResource = frame_ctx->cpu_surface_upload;
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    src.PlacedFootprint.Footprint.Width = surface->width;
    src.PlacedFootprint.Footprint.Height = surface->height;
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = upload_pitch;

    dst.pResource = frame_ctx->cpu_surface_texture;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    p->command_list->CopyTextureRegion(&dst, 0, 0, 0, &src, NULL);

    transition_resource(p->command_list, frame_ctx->cpu_surface_texture,
                        &frame_ctx->cpu_surface_texture_state,
                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    srv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    p->device->CreateShaderResourceView(
        frame_ctx->cpu_surface_texture, &srv,
        cpu_descriptor(p->srv_heap, p->srv_descriptor_size,
                       XEMU_D3D12_SOURCE_SRV_INDEX(p->frame_index)));
    return true;
}

/*
 * Keeps this frame context's copy of the DAC palette current and binds it
 * as a raw buffer SRV. The upload heap buffer is read directly by the
 * shader; the frame fence guarantees the previous use has completed.
 */
static bool update_dac_palette(XemuD3D12Presenter *p,
                               XemuD3D12PresenterFrame *frame_ctx)
{
    const uint8_t *palette = nv2a_get_dac_palette();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};

    if (frame_ctx->palette_upload == NULL) {
        D3D12_HEAP_PROPERTIES heap_props = {};
        D3D12_RESOURCE_DESC desc = {};
        D3D12_RANGE read_range = { 0, 0 };
        HRESULT hr;

        heap_props.Type = D3D12_HEAP_TYPE_UPLOAD;
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = XEMU_D3D12_PALETTE_ENTRIES * sizeof(uint32_t);
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        hr = p->device->CreateCommittedResource(
            &heap_props, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
            IID_PPV_ARGS(&frame_ctx->palette_upload));
        if (FAILED(hr)) {
            return set_hresult_error(p, hr, "CreateCommittedResource(palette)");
        }
        hr = frame_ctx->palette_upload->Map(0, &read_range,
                                            &frame_ctx->palette_mapped);
        if (FAILED(hr)) {
            d3d12_release(&frame_ctx->palette_upload);
            return set_hresult_error(p, hr, "Map(palette)");
        }
        frame_ctx->palette_valid = false;
    }

    if (!frame_ctx->palette_valid ||
        memcmp(frame_ctx->palette_cache, palette,
               sizeof(frame_ctx->palette_cache)) != 0) {
        uint32_t *packed = (uint32_t *)frame_ctx->palette_mapped;

        for (unsigned int i = 0; i < XEMU_D3D12_PALETTE_ENTRIES; i++) {
            packed[i] = ((uint32_t)palette[i * 3 + 2] << 16) |
                        ((uint32_t)palette[i * 3 + 1] << 8) |
                        palette[i * 3];
        }
        memcpy(frame_ctx->palette_cache, palette,
               sizeof(frame_ctx->palette_cache));
        frame_ctx->palette_valid = true;
        /* the game's gamma ramp (a colour tint over the whole picture), the first changes into the log */
        static int logged;
        if (logged < 12) {
            logged++;
            fprintf(stderr, "New graphics: gamma ramp now r/g/b at 0,64,128,192,255: "
                    "%u/%u/%u %u/%u/%u %u/%u/%u %u/%u/%u %u/%u/%u\n",
                    palette[0], palette[1], palette[2], palette[192], palette[193], palette[194],
                    palette[384], palette[385], palette[386], palette[576], palette[577], palette[578],
                    palette[765], palette[766], palette[767]);
        }
    }

    srv.Format = DXGI_FORMAT_R32_TYPELESS;
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.FirstElement = 0;
    srv.Buffer.NumElements = XEMU_D3D12_PALETTE_ENTRIES;
    srv.Buffer.StructureByteStride = 0;
    srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    p->device->CreateShaderResourceView(
        frame_ctx->palette_upload, &srv,
        cpu_descriptor(p->srv_heap, p->srv_descriptor_size,
                       XEMU_D3D12_PALETTE_SRV_INDEX(p->frame_index)));
    return true;
}

static void release_fsr_targets(XemuD3D12Presenter *p)
{
    d3d12_release(&p->fsr_compose);
    d3d12_release(&p->fsr_upscale);
    p->fsr_input_width = 0;
    p->fsr_input_height = 0;
    p->fsr_output_width = 0;
    p->fsr_output_height = 0;
}

static bool create_fsr_target(XemuD3D12Presenter *p, UINT width, UINT height,
                              UINT rtv_index, UINT srv_index,
                              ID3D12Resource **texture)
{
    D3D12_HEAP_PROPERTIES heap_props = {};
    D3D12_RESOURCE_DESC desc = {};
    D3D12_CLEAR_VALUE clear = {};
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    HRESULT hr;

    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    clear.Format = desc.Format;
    clear.Color[3] = 1.0f;

    /* Kept in PIXEL_SHADER_RESOURCE between frames. */
    hr = p->device->CreateCommittedResource(
        &heap_props, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear,
        IID_PPV_ARGS(texture));
    if (FAILED(hr)) {
        *texture = NULL;
        return set_hresult_error(p, hr, "CreateCommittedResource(FSR)");
    }

    p->device->CreateRenderTargetView(
        *texture, NULL,
        cpu_descriptor(p->rtv_heap, p->rtv_descriptor_size, rtv_index));

    srv.Format = desc.Format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    p->device->CreateShaderResourceView(
        *texture, &srv,
        cpu_descriptor(p->srv_heap, p->srv_descriptor_size, srv_index));
    return true;
}

static bool prepare_fsr_targets(XemuD3D12Presenter *p, UINT input_width,
                                UINT input_height, UINT output_width,
                                UINT output_height)
{
    if (p->fsr_compose != NULL && p->fsr_input_width == input_width &&
        p->fsr_input_height == input_height &&
        p->fsr_output_width == output_width &&
        p->fsr_output_height == output_height) {
        return true;
    }

    /* Earlier frames may still sample the old targets. */
    if (p->fsr_compose != NULL && !wait_for_gpu(p)) {
        return false;
    }
    release_fsr_targets(p);

    if (!create_fsr_target(p, input_width, input_height,
                           XEMU_D3D12_FSR_COMPOSE_RTV_INDEX,
                           XEMU_D3D12_FSR_COMPOSE_SRV_INDEX,
                           &p->fsr_compose) ||
        !create_fsr_target(p, output_width, output_height,
                           XEMU_D3D12_FSR_UPSCALE_RTV_INDEX,
                           XEMU_D3D12_FSR_UPSCALE_SRV_INDEX,
                           &p->fsr_upscale)) {
        release_fsr_targets(p);
        return false;
    }

    p->fsr_input_width = input_width;
    p->fsr_input_height = input_height;
    p->fsr_output_width = output_width;
    p->fsr_output_height = output_height;
    fprintf(stderr, "Direct3D 12 presenter: FSR 1 upscaling %ux%u to %ux%u\n",
            input_width, input_height, output_width, output_height);
    return true;
}

/* FSR 1 applies when the setting is on and the scanout is enlarged; a
 * reduced or same-size image keeps the filter method. */
static bool use_fsr(XemuD3D12Presenter *p, UINT input_width,
                    UINT input_height, UINT output_width, UINT output_height)
{
    if (!g_config.display.fsr) {
        if (p->fsr_compose != NULL && wait_for_gpu(p)) {
            release_fsr_targets(p);
        }
        p->fsr_unavailable = false;
        return false;
    }

    if (p->fsr_unavailable || output_width < input_width ||
        output_height < input_height ||
        (output_width == input_width && output_height == input_height)) {
        return false;
    }

    if ((p->fsr_easu_pso == NULL && !create_fsr_pipelines(p)) ||
        !prepare_fsr_targets(p, input_width, input_height, output_width,
                             output_height)) {
        fprintf(stderr, "Direct3D 12 presenter: FSR 1 unavailable: %s\n",
                p->error);
        p->error[0] = '\0';
        p->fsr_unavailable = true;
        return false;
    }
    return true;
}

/* Expects the display pipeline, source table and display constants bound;
 * leaves the backbuffer bound for the HUD. */
static void draw_fsr(XemuD3D12Presenter *p, const D3D12_RECT *output)
{
    ID3D12GraphicsCommandList *cl = p->command_list;
    D3D12_RESOURCE_STATES compose_state =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    D3D12_RESOURCE_STATES upscale_state =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    float in_w = (float)p->fsr_input_width;
    float in_h = (float)p->fsr_input_height;
    float out_w = (float)p->fsr_output_width;
    float out_h = (float)p->fsr_output_height;
    float constants[XEMU_D3D12_FSR_CONSTANTS] = {};
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    D3D12_VIEWPORT viewport = {};
    D3D12_RECT scissor = {};

    /* Scanout, overlay and gamma ramp at source size. */
    transition_resource(cl, p->fsr_compose, &compose_state,
                        D3D12_RESOURCE_STATE_RENDER_TARGET);
    rtv = cpu_descriptor(p->rtv_heap, p->rtv_descriptor_size,
                         XEMU_D3D12_FSR_COMPOSE_RTV_INDEX);
    cl->OMSetRenderTargets(1, &rtv, FALSE, NULL);
    viewport.Width = in_w;
    viewport.Height = in_h;
    viewport.MaxDepth = 1.0f;
    scissor.right = (LONG)p->fsr_input_width;
    scissor.bottom = (LONG)p->fsr_input_height;
    cl->RSSetViewports(1, &viewport);
    cl->RSSetScissorRects(1, &scissor);
    cl->DrawInstanced(3, 1, 0, 0);
    transition_resource(cl, p->fsr_compose, &compose_state,
                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    /* EASU: output pixel to source position, then the gather origins
     * relative to the top-left of the nearest texel. */
    constants[0] = in_w / out_w;
    constants[1] = in_h / out_h;
    constants[2] = 0.5f * in_w / out_w - 0.5f;
    constants[3] = 0.5f * in_h / out_h - 0.5f;
    constants[4] = 1.0f / in_w;
    constants[5] = 1.0f / in_h;
    constants[6] = 1.0f / in_w;
    constants[7] = -1.0f / in_h;
    constants[8] = -1.0f / in_w;
    constants[9] = 2.0f / in_h;
    constants[10] = 1.0f / in_w;
    constants[11] = 2.0f / in_h;
    constants[13] = 4.0f / in_h;
    constants[16] = fminf(fmaxf(g_config.display.fsr_sharpness, 0.0f), 1.0f);
    constants[17] = (float)output->left;
    constants[18] = (float)output->top;

    transition_resource(cl, p->fsr_upscale, &upscale_state,
                        D3D12_RESOURCE_STATE_RENDER_TARGET);
    rtv = cpu_descriptor(p->rtv_heap, p->rtv_descriptor_size,
                         XEMU_D3D12_FSR_UPSCALE_RTV_INDEX);
    cl->OMSetRenderTargets(1, &rtv, FALSE, NULL);
    viewport.Width = out_w;
    viewport.Height = out_h;
    scissor.right = (LONG)p->fsr_output_width;
    scissor.bottom = (LONG)p->fsr_output_height;
    cl->RSSetViewports(1, &viewport);
    cl->RSSetScissorRects(1, &scissor);
    cl->SetPipelineState(p->fsr_easu_pso);
    cl->SetGraphicsRootDescriptorTable(
        0, gpu_descriptor(p->srv_heap, p->srv_descriptor_size,
                          XEMU_D3D12_FSR_COMPOSE_SRV_INDEX));
    cl->SetGraphicsRoot32BitConstants(1, XEMU_D3D12_FSR_CONSTANTS,
                                      constants, 0);
    cl->DrawInstanced(3, 1, 0, 0);
    transition_resource(cl, p->fsr_upscale, &upscale_state,
                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    /* RCAS into the display rectangle of the backbuffer. */
    rtv = cpu_descriptor(p->rtv_heap, p->rtv_descriptor_size, p->frame_index);
    cl->OMSetRenderTargets(1, &rtv, FALSE, NULL);
    viewport.TopLeftX = (float)output->left;
    viewport.TopLeftY = (float)output->top;
    cl->RSSetViewports(1, &viewport);
    cl->RSSetScissorRects(1, output);
    cl->SetPipelineState(p->fsr_rcas_pso);
    cl->SetGraphicsRootDescriptorTable(
        0, gpu_descriptor(p->srv_heap, p->srv_descriptor_size,
                          XEMU_D3D12_FSR_UPSCALE_SRV_INDEX));
    cl->DrawInstanced(3, 1, 0, 0);
}

static bool render_display_frame(XemuD3D12Presenter *p,
                                 XemuD3D12PresenterFrame *frame_ctx,
                                 const NV2ADisplayFrame *frame)
{
    ID3D12Resource *resource = NULL;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    ID3D12DescriptorHeap *heaps[] = { p->srv_heap };
    D3D12_VIEWPORT viewport = {};
    D3D12_RECT scissor = {};
    DXGI_FORMAT source_format = DXGI_FORMAT_UNKNOWN;
    D3D12_RESOURCE_STATES source_original_state =
        D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES source_state = D3D12_RESOURCE_STATE_COMMON;
    uint32_t resource_width = 0;
    uint32_t resource_height = 0;
    uint32_t src_x = 0;
    uint32_t src_y = 0;
    uint32_t src_w = 0;
    uint32_t src_h = 0;
    uint32_t resolved_display_width = 0;
    uint32_t resolved_display_height = 0;
    float constants[XEMU_D3D12_DISPLAY_CONSTANTS] = {};
    UINT compose_width;
    UINT compose_height;
    bool pvideo_ready;
    bool fsr;

    /* Sequencer screen-off: the backbuffer already holds opaque black. */
    if (nv2a_get_screen_off()) {
        return true;
    }

    if (frame->type == NV2A_DISPLAY_FRAME_D3D12_RESOURCE) {
        uint32_t scale = frame->d3d12.resource_scale ?
            frame->d3d12.resource_scale : 1;

        resource = (ID3D12Resource *)frame->d3d12.resource;
        if (resource != NULL) {
            resource->AddRef();
            frame_ctx->source_resource = resource;
        }
        source_format = (DXGI_FORMAT)frame->d3d12.dxgi_format;
        source_original_state =
            (D3D12_RESOURCE_STATES)frame->d3d12.resource_state;
        source_state = source_original_state;
        resource_width = frame->d3d12.resource_width;
        resource_height = frame->d3d12.resource_height;
        src_x = frame->d3d12.source_x * scale;
        src_y = frame->d3d12.source_y * scale;
        src_w = frame->d3d12.source_width * scale;
        src_h = frame->d3d12.source_height * scale;
    } else if (frame->type == NV2A_DISPLAY_FRAME_CPU_SURFACE) {
        if (!update_cpu_surface_texture(p, frame_ctx, frame)) {
            return false;
        }
        resource = frame_ctx->cpu_surface_texture;
        source_format = DXGI_FORMAT_B8G8R8A8_UNORM;
        resource_width = frame->cpu.width;
        resource_height = frame->cpu.height;
        src_w = frame->cpu.width;
        src_h = frame->cpu.height;
    } else {
        return true;
    }

    if (resource == NULL || resource_width == 0 || resource_height == 0 ||
        src_w == 0 || src_h == 0) {
        trace_presenter(p,
                        "skipping display draw because scanout is empty: "
                        "type=%d resource=%p resource=%ux%u source=%ux%u",
                        frame->type, resource, resource_width,
                        resource_height, src_w, src_h);
        return true;
    }

    if (src_x >= resource_width || src_y >= resource_height) {
        fprintf(stderr,
                "Direct3D 12 presenter: scanout source is outside resource: "
                "source=(%u,%u %ux%u), resource=%ux%u\n",
                src_x, src_y, src_w, src_h, resource_width,
                resource_height);
        return true;
    }

    src_w = src_w < resource_width - src_x ?
        src_w : resource_width - src_x;
    src_h = src_h < resource_height - src_y ?
        src_h : resource_height - src_y;

    {
        uint32_t display_scale = frame->display_scale ?
            frame->display_scale : 1;
        uint64_t scaled_width =
            (uint64_t)frame->display_width * display_scale;
        uint64_t scaled_height =
            (uint64_t)frame->display_height * display_scale;

        resolved_display_width = scaled_width > UINT32_MAX ?
            UINT32_MAX : (uint32_t)scaled_width;
        resolved_display_height = scaled_height > UINT32_MAX ?
            UINT32_MAX : (uint32_t)scaled_height;
        if (resolved_display_width == 0 || resolved_display_height == 0) {
            resolved_display_width = src_w;
            resolved_display_height = src_h;
        }
    }

    if (frame->type == NV2A_DISPLAY_FRAME_D3D12_RESOURCE) {
        if (source_format == DXGI_FORMAT_UNKNOWN) {
            return set_error(
                p,
                "D3D12 scanout resource has unknown SRV format: "
                "resource=%p state=0x%x resource=%ux%u source=(%u,%u %ux%u) "
                "vram=0x%" PRIx64 " color_format=0x%x",
                resource, (unsigned)source_original_state, resource_width,
                resource_height, src_x, src_y, src_w, src_h,
                (uint64_t)frame->d3d12.vram_addr,
                frame->d3d12.color_format);
        }

        trace_presenter(
            p,
            "binding D3D12 scanout SRV: resource=%p state=0x%x "
            "format=%u resource=%ux%u source=(%u,%u %ux%u) scale=%u "
            "pitch=%u bpp=%u vram=0x%" PRIx64 " color_format=0x%x",
            resource, (unsigned)source_original_state,
            (unsigned)source_format, resource_width, resource_height, src_x,
            src_y, src_w, src_h, frame->d3d12.resource_scale,
            frame->d3d12.source_pitch, frame->d3d12.bytes_per_pixel,
            (uint64_t)frame->d3d12.vram_addr, frame->d3d12.color_format);

        transition_resource(p->command_list, resource, &source_state,
                            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        srv.Format = source_format;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping =
            D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MostDetailedMip = 0;
        srv.Texture2D.MipLevels = 1;
        srv.Texture2D.PlaneSlice = 0;
        srv.Texture2D.ResourceMinLODClamp = 0.0f;
        p->device->CreateShaderResourceView(
            resource, &srv,
            cpu_descriptor(p->srv_heap, p->srv_descriptor_size,
                           XEMU_D3D12_SOURCE_SRV_INDEX(p->frame_index)));
    } else {
        trace_presenter(
            p,
            "binding CPU scanout texture: resource=%p format=%u "
            "resource=%ux%u source=%ux%u pitch=%u bpp=%u vram=0x%" PRIx64
            " color_format=0x%x",
            resource, (unsigned)source_format, resource_width,
            resource_height, src_w, src_h, frame->cpu.pitch,
            frame->cpu.bytes_per_pixel, (uint64_t)frame->cpu.vram_addr,
            frame->cpu.color_format);
    }

    pvideo_ready = update_pvideo_texture(p, frame_ctx, frame);
    if (!pvideo_ready) {
        if (frame->type == NV2A_DISPLAY_FRAME_D3D12_RESOURCE &&
            source_state != source_original_state) {
            transition_resource(p->command_list, resource, &source_state,
                                source_original_state);
        }
        return false;
    }

    constants[0] = (float)src_x / (float)resource_width;
    constants[1] = (float)src_y / (float)resource_height;
    constants[2] = (float)src_w / (float)resource_width;
    if (frame->type == NV2A_DISPLAY_FRAME_D3D12_RESOURCE) {
        float line_ratio = 1.0f;

        if (frame->d3d12.resource_pitch &&
            frame->d3d12.source_pitch) {
            line_ratio = (float)frame->d3d12.resource_pitch /
                         (float)frame->d3d12.source_pitch;
            if (line_ratio <= 0.0f) {
                line_ratio = 1.0f;
            }
        }
        /* One surface line per raster line; a surface shorter than the
         * raster is not stretched. */
        constants[3] =
            (float)resolved_display_height / (float)resource_height /
            line_ratio;
    } else {
        constants[3] = (float)src_h / (float)resource_height;
    }
    /* PVIDEO placement and scaling are expressed in guest display pixels. */
    constants[4] = (float)(frame->display_width ? frame->display_width : src_w);
    constants[5] =
        (float)(frame->display_height ? frame->display_height : src_h);
    constants[6] = frame->pvideo.enabled ? 1.0f : 0.0f;
    constants[7] = frame->pvideo.color_key_enabled ? 1.0f : 0.0f;
    constants[8] = (float)frame->pvideo.out_x;
    constants[9] = (float)frame->pvideo.out_y;
    constants[10] = (float)frame->pvideo.out_width;
    constants[11] = (float)frame->pvideo.out_height;
    constants[12] = frame->pvideo.in_x;
    constants[13] = frame->pvideo.in_y;
    constants[14] = frame->pvideo.scale_x;
    constants[15] = frame->pvideo.scale_y;
    constants[16] = (float)frame->pvideo.width;
    constants[17] = (float)frame->pvideo.height;
    constants[18] =
        g_config.display.filtering == CONFIG_DISPLAY_FILTERING_NEAREST ?
            1.0f : 0.0f;
    /* 8-bit key components, compared exactly. */
    constants[20] = (float)((frame->pvideo.color_key >> 16) & 0xff);
    constants[21] = (float)((frame->pvideo.color_key >> 8) & 0xff);
    constants[22] = (float)(frame->pvideo.color_key & 0xff);
    /* Bottom of the scanned-out rows, and the source size in texels. */
    constants[24] = (float)(src_y + src_h) / (float)resource_height;
    constants[26] = (float)resource_width;
    constants[27] = (float)resource_height;

    get_display_viewport(p, resolved_display_width,
                         resolved_display_height, &viewport, &scissor);

    trace_presenter(
        p,
        "drawing display triangle: backbuffer_index=%u viewport={%.1f,%.1f "
        "%.1fx%.1f} scissor={%ld,%ld,%ld,%ld} pvideo={enabled:%d "
        "out:%u,%u %ux%u key:%d color:0x%08x}",
        p->frame_index, viewport.TopLeftX, viewport.TopLeftY, viewport.Width,
        viewport.Height, scissor.left, scissor.top, scissor.right,
        scissor.bottom, frame->pvideo.enabled, frame->pvideo.out_x,
        frame->pvideo.out_y, frame->pvideo.out_width,
        frame->pvideo.out_height, frame->pvideo.color_key_enabled,
        frame->pvideo.color_key);

    if (!update_dac_palette(p, frame_ctx)) {
        if (frame->type == NV2A_DISPLAY_FRAME_D3D12_RESOURCE &&
            source_state != source_original_state) {
            transition_resource(p->command_list, resource, &source_state,
                                source_original_state);
        }
        return false;
    }

    /* The source rows that span the raster, one texel per pixel. */
    compose_width = src_w;
    compose_height = (UINT)lroundf(constants[3] * (float)resource_height);
    compose_height = compose_height < 1 ? 1 : compose_height;
    fsr = compose_width <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION &&
          compose_height <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION &&
          use_fsr(p, compose_width, compose_height,
                  (UINT)(scissor.right - scissor.left),
                  (UINT)(scissor.bottom - scissor.top));

    p->command_list->SetDescriptorHeaps(1, heaps);
    p->command_list->SetGraphicsRootSignature(p->display_root_signature);
    p->command_list->SetPipelineState(p->display_pso);
    p->command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    p->command_list->SetGraphicsRootDescriptorTable(
        0, gpu_descriptor(p->srv_heap, p->srv_descriptor_size,
                          XEMU_D3D12_SOURCE_SRV_INDEX(p->frame_index)));
    p->command_list->SetGraphicsRoot32BitConstants(
        1, XEMU_D3D12_DISPLAY_CONSTANTS, constants, 0);
    if (fsr) {
        draw_fsr(p, &scissor);
    } else {
        p->command_list->RSSetViewports(1, &viewport);
        p->command_list->RSSetScissorRects(1, &scissor);
        p->command_list->DrawInstanced(3, 1, 0, 0);
    }

    if (frame->type == NV2A_DISPLAY_FRAME_D3D12_RESOURCE &&
        source_state != source_original_state) {
        transition_resource(p->command_list, resource, &source_state,
                            source_original_state);
    }

    return true;
}

XemuD3D12Presenter *xemu_d3d12_presenter_create(SDL_Window *window)
{
    XemuD3D12Presenter *p = g_new0(XemuD3D12Presenter, 1);

    p->window = window;
    for (UINT i = 0; i < XEMU_D3D12_BACKBUFFER_COUNT; i++) {
        p->backbuffer_states[i] = D3D12_RESOURCE_STATE_PRESENT;
    }

    return p;
}

void xemu_d3d12_presenter_destroy(XemuD3D12Presenter *p)
{
    if (p == NULL) {
        return;
    }

    wait_for_gpu(p);
    release_stream_captures(p);
    release_backbuffers(p);
    release_fsr_targets(p);
    if (p->fence_event != NULL) {
        CloseHandle(p->fence_event);
    }
    d3d12_release(&p->fsr_easu_pso);
    d3d12_release(&p->fsr_rcas_pso);
    d3d12_release(&p->display_pso);
    d3d12_release(&p->display_root_signature);
    d3d12_release(&p->fence);
    d3d12_release(&p->command_list);
    for (UINT i = 0; i < XEMU_D3D12_BACKBUFFER_COUNT; i++) {
        release_frame_resources(&p->frames[i]);
    }
    d3d12_release(&p->srv_heap);
    d3d12_release(&p->rtv_heap);
    if (p->frame_latency_waitable != NULL) {
        CloseHandle(p->frame_latency_waitable);
        p->frame_latency_waitable = NULL;
    }
    d3d12_release(&p->swapchain);
    d3d12_release(&p->factory);
    d3d12_release(&p->queue);
    d3d12_release(&p->device);
    g_free(p->pending_screenshot_path);
    g_free(p);
}

bool xemu_d3d12_presenter_begin_frame(XemuD3D12Presenter *p,
                                      const NV2ADisplayFrame *frame)
{
    ID3D12Resource *backbuffer;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    XemuD3D12PresenterFrame *frame_ctx;
    const float clear_color[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    HRESULT hr;

    if (p == NULL) {
        return false;
    }

    p->error[0] = '\0';
    trace_presenter(
        p,
        "begin frame requested: frame_type=%d display=%ux%u "
        "d3d12_resource=%p d3d12_state=0x%x cpu_data=%p",
        frame != NULL ? frame->type : -1,
        frame != NULL ? frame->display_width : 0,
        frame != NULL ? frame->display_height : 0,
        frame != NULL ? frame->d3d12.resource : NULL,
        frame != NULL ? frame->d3d12.resource_state : 0,
        frame != NULL ? frame->cpu.data : NULL);

    if (!initialize_from_frame(p, frame) || !create_or_resize_swapchain(p)) {
        return false;
    }

    frame_ctx = &p->frames[p->frame_index];
    if (!wait_for_frame_context(p, frame_ctx)) {
        return false;
    }

    p->frame_ready_fence = NULL;
    p->frame_release_fence = NULL;
    if (frame != NULL && frame->type == NV2A_DISPLAY_FRAME_D3D12_RESOURCE) {
        p->frame_ready_fence = (ID3D12Fence *)frame->d3d12.ready_fence;
        p->frame_ready_value = frame->d3d12.ready_value;
        p->frame_release_fence = (ID3D12Fence *)frame->d3d12.release_fence;
        p->frame_release_value = frame->d3d12.release_value;
    }

    hr = frame_ctx->command_allocator->Reset();
    if (FAILED(hr)) {
        return set_hresult_error(p, hr, "ID3D12CommandAllocator::Reset");
    }

    hr = p->command_list->Reset(frame_ctx->command_allocator, NULL);
    if (FAILED(hr)) {
        return set_hresult_error(p, hr, "ID3D12GraphicsCommandList::Reset");
    }

    backbuffer = p->backbuffers[p->frame_index];
    transition_resource(p->command_list, backbuffer,
                        &p->backbuffer_states[p->frame_index],
                        D3D12_RESOURCE_STATE_RENDER_TARGET);

    rtv = cpu_descriptor(p->rtv_heap, p->rtv_descriptor_size, p->frame_index);
    p->command_list->OMSetRenderTargets(1, &rtv, FALSE, NULL);
    p->command_list->ClearRenderTargetView(rtv, clear_color, 0, NULL);
    trace_presenter(p,
                    "backbuffer ready: index=%u resource=%p rtv=0x%" PRIx64
                    " size=%ux%u",
                    p->frame_index, backbuffer, (uint64_t)rtv.ptr,
                    p->width, p->height);

    /* A composition failure (an allocation on the shared adapter, an
     * unsupported format) skips the scanout for this frame only; the HUD
     * and Present still run, so the list is closed and the next frame
     * starts from a clean allocator. */
    if (frame != NULL && !render_display_frame(p, frame_ctx, frame)) {
        fprintf(stderr, "Direct3D 12 presenter: display frame skipped: %s\n",
                p->error);
    }

    return true;
}

static void release_stream_captures(XemuD3D12Presenter *p)
{
    for (UINT i = 0; i < XEMU_D3D12_STREAM_CAPTURES; i++) {
        release_stream_capture(&p->stream_captures[i]);
    }
}

static void release_stream_capture(XemuD3D12StreamCapture *c)
{
    if (InterlockedCompareExchange(&c->encoder_owned, 0, 0) != 0) {
        return;
    }
    if (c->readback != NULL && c->mapped != NULL) {
        D3D12_RANGE written_range = { 0, 0 };
        c->readback->Unmap(0, &written_range);
    }
    c->mapped = NULL;
    d3d12_release(&c->readback);
    c->layout = {};
    c->in_flight = false;
    c->width = 0;
    c->height = 0;
    c->fence_value = 0;
}

static void stream_capture_encoder_release(void *opaque)
{
    XemuD3D12StreamCapture *c =
        static_cast<XemuD3D12StreamCapture *>(opaque);
    InterlockedExchange(&c->encoder_owned, 0);
}

/* Hand completed captures to the stream encoder; never waits on the fence. */
static void drain_stream_captures(XemuD3D12Presenter *p)
{
    uint64_t completed;
    bool has_readback = false;

    if (p->fence == NULL) {
        return;
    }

    for (UINT i = 0; i < XEMU_D3D12_STREAM_CAPTURES; i++) {
        if (p->stream_captures[i].readback != NULL) {
            has_readback = true;
            break;
        }
    }
    if (!has_readback) {
        return;
    }

    completed = p->fence->GetCompletedValue();
    for (UINT i = 0; i < XEMU_D3D12_STREAM_CAPTURES; i++) {
        XemuD3D12StreamCapture *c = &p->stream_captures[i];

        if (!c->in_flight || c->readback == NULL || c->mapped == NULL) {
            continue;
        }
        if (completed < c->fence_value) {
            continue;
        }

        const uint8_t *base =
            (const uint8_t *)c->mapped + c->layout.Offset;
        /* Slot stays mapped; the encoder worker releases ownership. */
        InterlockedExchange(&c->encoder_owned, 1);
        c->in_flight = false;
        xemu_stream_submit_bgra_borrowed(
            base, (int)c->width, (int)c->height,
            (int)c->layout.Footprint.RowPitch, c->capture_us,
            stream_capture_encoder_release, c);
    }

    if (!xemu_stream_is_active()) {
        for (UINT i = 0; i < XEMU_D3D12_STREAM_CAPTURES; i++) {
            if (!p->stream_captures[i].in_flight &&
                InterlockedCompareExchange(
                    &p->stream_captures[i].encoder_owned, 0, 0) == 0) {
                release_stream_capture(&p->stream_captures[i]);
            }
        }
    }
}

static bool stream_capture_slot_free(XemuD3D12Presenter *p)
{
    for (UINT i = 0; i < XEMU_D3D12_STREAM_CAPTURES; i++) {
        XemuD3D12StreamCapture *c = &p->stream_captures[i];
        if (!c->in_flight &&
            InterlockedCompareExchange(&c->encoder_owned, 0, 0) == 0) {
            return true;
        }
    }
    return false;
}

/* Copy the current backbuffer into a free capture slot. Returns the slot
 * index (caller tags it with the submit fence value) or -1. */
static int record_stream_capture(XemuD3D12Presenter *p)
{
    ID3D12Resource *backbuffer = p->backbuffers[p->frame_index];
    D3D12_RESOURCE_DESC desc = {};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout = {};
    D3D12_TEXTURE_COPY_LOCATION src = {};
    D3D12_TEXTURE_COPY_LOCATION dst = {};
    UINT num_rows = 0;
    UINT64 row_size = 0;
    UINT64 total_size = 0;
    int slot = -1;
    XemuD3D12StreamCapture *c;

    if (backbuffer == NULL || p->device == NULL) {
        return -1;
    }

    for (UINT i = 0; i < XEMU_D3D12_STREAM_CAPTURES; i++) {
        if (!p->stream_captures[i].in_flight &&
            InterlockedCompareExchange(
                &p->stream_captures[i].encoder_owned, 0, 0) == 0) {
            slot = (int)i;
            break;
        }
    }
    if (slot < 0) {
        return -1;
    }
    c = &p->stream_captures[slot];

    desc = backbuffer->GetDesc();
    if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM ||
        desc.SampleDesc.Count != 1) {
        return -1;
    }

    p->device->GetCopyableFootprints(&desc, 0, 1, 0, &layout, &num_rows,
                                     &row_size, &total_size);
    if (total_size == 0 || num_rows == 0) {
        return -1;
    }

    if (c->readback == NULL || c->width != (UINT)desc.Width ||
        c->height != desc.Height) {
        D3D12_HEAP_PROPERTIES heap_props = {};
        D3D12_RESOURCE_DESC buffer_desc = {};
        D3D12_RANGE read_range;
        HRESULT hr;

        release_stream_capture(c);

        heap_props.Type = D3D12_HEAP_TYPE_READBACK;
        heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

        buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer_desc.Width = total_size;
        buffer_desc.Height = 1;
        buffer_desc.DepthOrArraySize = 1;
        buffer_desc.MipLevels = 1;
        buffer_desc.Format = DXGI_FORMAT_UNKNOWN;
        buffer_desc.SampleDesc.Count = 1;
        buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        hr = p->device->CreateCommittedResource(
            &heap_props, D3D12_HEAP_FLAG_NONE, &buffer_desc,
            D3D12_RESOURCE_STATE_COPY_DEST, NULL,
            IID_PPV_ARGS(&c->readback));
        if (FAILED(hr)) {
            c->readback = NULL;
            return -1;
        }
        read_range.Begin = 0;
        read_range.End = (SIZE_T)total_size;
        hr = c->readback->Map(0, &read_range, &c->mapped);
        if (FAILED(hr)) {
            release_stream_capture(c);
            return -1;
        }
        c->width = (UINT)desc.Width;
        c->height = desc.Height;
    }

    c->layout = layout;

    transition_resource(p->command_list, backbuffer,
                        &p->backbuffer_states[p->frame_index],
                        D3D12_RESOURCE_STATE_COPY_SOURCE);

    src.pResource = backbuffer;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;

    dst.pResource = c->readback;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = layout;

    p->command_list->CopyTextureRegion(&dst, 0, 0, 0, &src, NULL);
    return slot;
}

bool xemu_d3d12_presenter_end_frame(XemuD3D12Presenter *p, bool vsync)
{
    ID3D12CommandList *command_lists[1];
    ID3D12Resource *screenshot_readback = NULL;
    XemuD3D12PresenterFrame *frame_ctx;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT screenshot_layout = {};
    UINT screenshot_width = 0;
    UINT screenshot_height = 0;
    uint64_t submitted_fence_value = 0;
    char *screenshot_path;
    bool screenshot_failed = false;
    int stream_slot = -1;
    HRESULT hr;

    if (p == NULL || p->swapchain == NULL || p->command_list == NULL) {
        return false;
    }

    trace_presenter(p, "ending frame: index=%u vsync=%d", p->frame_index,
                    vsync);

    drain_stream_captures(p);

    screenshot_path = p->pending_screenshot_path;
    p->pending_screenshot_path = NULL;
    frame_ctx = &p->frames[p->frame_index];
    if (screenshot_path != NULL &&
        !record_screenshot_copy(p, &screenshot_readback, &screenshot_layout,
                                &screenshot_width, &screenshot_height)) {
        screenshot_failed = true;
    }

    /* Claim a capture only when a slot can take it, so the stream's frame
     * cadence is not spent on a copy that would be dropped. */
    if (stream_capture_slot_free(p) && xemu_stream_wants_frame()) {
        stream_slot = record_stream_capture(p);
        if (stream_slot >= 0) {
            p->stream_captures[stream_slot].capture_us = g_get_monotonic_time();
        }
    }

    transition_resource(p->command_list, p->backbuffers[p->frame_index],
                        &p->backbuffer_states[p->frame_index],
                        D3D12_RESOURCE_STATE_PRESENT);

    hr = p->command_list->Close();
    if (FAILED(hr)) {
        return set_hresult_error(p, hr, "ID3D12GraphicsCommandList::Close");
    }
    trace_presenter(p, "command list closed for present");

    /* The scanout copy was queued on the renderer's queue. */
    if (p->frame_ready_fence != NULL && p->frame_ready_value != 0) {
        p->queue->Wait(p->frame_ready_fence, p->frame_ready_value);
    }
    command_lists[0] = p->command_list;
    p->queue->ExecuteCommandLists(1, command_lists);
    trace_presenter(p, "command list executed on queue=%p", p->queue);
    /* Queue order: the renderer may reuse the scanout once this passes. */
    if (p->frame_release_fence != NULL) {
        p->queue->Signal(p->frame_release_fence, p->frame_release_value);
    }
    p->frame_ready_fence = NULL;
    p->frame_release_fence = NULL;

    if (!signal_frame_context(p, frame_ctx, &submitted_fence_value)) {
        d3d12_release(&screenshot_readback);
        g_free(screenshot_path);
        return false;
    }

    if (stream_slot >= 0) {
        p->stream_captures[stream_slot].fence_value = submitted_fence_value;
        p->stream_captures[stream_slot].in_flight = true;
    }

    hr = p->swapchain->Present(vsync ? 1 : 0, 0);
    if (FAILED(hr)) {
        d3d12_release(&screenshot_readback);
        g_free(screenshot_path);
        return set_hresult_error(p, hr, "IDXGISwapChain::Present");
    }
    trace_presenter(p, "present completed");

    if (screenshot_readback != NULL &&
        !wait_for_fence_value(p, submitted_fence_value)) {
        d3d12_release(&screenshot_readback);
        g_free(screenshot_path);
        return false;
    }

    if (screenshot_readback != NULL &&
        !save_bgra8_readback_png(p, screenshot_readback, &screenshot_layout,
                                 screenshot_width, screenshot_height,
                                 screenshot_path)) {
        screenshot_failed = true;
    }

    d3d12_release(&screenshot_readback);
    g_free(screenshot_path);

    p->frame_index = p->swapchain->GetCurrentBackBufferIndex();
    p->frame_counter++;
    return !screenshot_failed;
}

bool xemu_d3d12_presenter_request_screenshot(XemuD3D12Presenter *p,
                                             const char *path)
{
    if (p == NULL || path == NULL || path[0] == '\0') {
        return false;
    }

    g_free(p->pending_screenshot_path);
    p->pending_screenshot_path = g_strdup(path);
    if (p->pending_screenshot_path == NULL) {
        return set_error(p, "failed to copy Direct3D 12 screenshot path");
    }

    return true;
}

/* Diagnostic switch: "latency_wait=0" in d3d12_tune.txt skips the wait. */
static bool presenter_latency_wait_enabled(void)
{
    static unsigned int calls;
    static bool enabled = true;

    if (calls++ % 300 == 0) {
        g_autofree char *path = g_strdup_printf(
            "%sd3d12_tune.txt", xemu_settings_get_base_path());
        g_autofree gchar *text = NULL;
        bool now = true;

        if (g_file_get_contents(path, &text, NULL, NULL)) {
            now = strstr(text, "latency_wait=0") == NULL;
        }
        if (now != enabled) {
            fprintf(stderr, "Direct3D 12 tune: latency_wait=%d\n", now);
        }
        enabled = now;
    }
    return enabled;
}

void xemu_d3d12_presenter_wait_frame_latency(XemuD3D12Presenter *p)
{
    if (p != NULL && p->frame_latency_waitable != NULL &&
        presenter_latency_wait_enabled()) {
        /* Bounded so a lost signal can never stall the UI thread. */
        WaitForSingleObjectEx(p->frame_latency_waitable, 100, TRUE);
    }
}

const char *xemu_d3d12_presenter_get_error(XemuD3D12Presenter *p)
{
    if (p == NULL || p->error[0] == '\0') {
        return "unknown Direct3D 12 presenter error";
    }
    return p->error;
}

void *xemu_d3d12_presenter_get_device(XemuD3D12Presenter *p)
{
    return p != NULL ? p->device : NULL;
}

void *xemu_d3d12_presenter_get_command_queue(XemuD3D12Presenter *p)
{
    return p != NULL ? p->queue : NULL;
}

void *xemu_d3d12_presenter_get_command_list(XemuD3D12Presenter *p)
{
    return p != NULL ? p->command_list : NULL;
}

void *xemu_d3d12_presenter_get_srv_heap(XemuD3D12Presenter *p)
{
    return p != NULL ? p->srv_heap : NULL;
}

uint64_t xemu_d3d12_presenter_get_font_srv_cpu(XemuD3D12Presenter *p)
{
    if (p == NULL || p->srv_heap == NULL) {
        return 0;
    }
    return (uint64_t)cpu_descriptor(
        p->srv_heap, p->srv_descriptor_size, XEMU_D3D12_FONT_SRV_INDEX).ptr;
}

uint64_t xemu_d3d12_presenter_get_font_srv_gpu(XemuD3D12Presenter *p)
{
    if (p == NULL || p->srv_heap == NULL) {
        return 0;
    }
    return (uint64_t)gpu_descriptor(
        p->srv_heap, p->srv_descriptor_size, XEMU_D3D12_FONT_SRV_INDEX).ptr;
}

#endif
