/*
 * xemu Direct3D 12 presenter: frame generation (included by ui/d3d12-presenter.cc)
 *
 * Copyright (c) 2026 Skitzelles - part of Game Vault, https://github.com/skitzellez/xbox-game-vault
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * A game that shows 60 frames a second, on a TV at 120 Hz: between two of its frames one more is made from
 * both, so the picture moves at 120 while the game itself runs at its own speed (user 2026-11-02, JSRF:
 * "speed up the FPS and make it look faster but not fastforwarding"). The 60 FPS switch made such a game
 * run twice as fast instead.
 *
 * Each new game frame is composed at source size (as for FSR 1) into one of two history pictures, and its
 * brightness kept at a quarter of the size. When game frames come every 2nd refresh:
 *   1. motion: for each 4x4 block of the quarter-size picture (16x16 of the source, judged on 8x8 around it), the step v that best
 *      matches the older frame at -v/2 with the newer one at +v/2, so the picture halfway needs no holes
 *      filled (a wide search on every 2nd step, then a fine one around the best);
 *   2. the steps are smoothed (median of 3x3 blocks);
 *   3. the picture halfway: each pixel tries the steps of the blocks around it and keeps the one whose two
 *      samples agree best around it, then shows their average.
 * That picture is shown at once and the newer frame at the next refresh, so the newest frame is shown
 * 8 ms later than without. Anything else (60 Hz TV, 30 fps game, a picture the CPU made) shows the frames
 * as before. All work is on the GPU, made once per game frame.
 */

#define XEMU_D3D12_FG_SRV_BASE (XEMU_D3D12_FSR_UPSCALE_SRV_INDEX + 1)
#define XEMU_D3D12_FG_FRAME_SRV(i) (XEMU_D3D12_FG_SRV_BASE + (i))
#define XEMU_D3D12_FG_MID_SRV (XEMU_D3D12_FG_SRV_BASE + 2)
#define XEMU_D3D12_FG_MID_UAV (XEMU_D3D12_FG_SRV_BASE + 3)
#define XEMU_D3D12_FG_LUMA_SRV(i) (XEMU_D3D12_FG_SRV_BASE + 4 + (i))
#define XEMU_D3D12_FG_LUMA_UAV(i) (XEMU_D3D12_FG_SRV_BASE + 6 + (i))
#define XEMU_D3D12_FG_MV_SRV(i) (XEMU_D3D12_FG_SRV_BASE + 8 + (i))
#define XEMU_D3D12_FG_MV_UAV(i) (XEMU_D3D12_FG_SRV_BASE + 10 + (i))
#define XEMU_D3D12_FG_SRV_END (XEMU_D3D12_FG_SRV_BASE + 12)
#define XEMU_D3D12_FG_FRAME_RTV(i) (XEMU_D3D12_BACKBUFFER_COUNT + 2 + (i))

static_assert(XEMU_D3D12_FG_SRV_END <= XEMU_D3D12_PRESENTER_SRV_COUNT,
              "frame generation SRVs overlap the HUD range");

/* search reach in quarter-size pixels per frame (x4 in the source) */
#define XEMU_D3D12_FG_REACH 24

extern "C" bool xemu_uwp_test_file_exists(const char *name); /* ui/xemu.c */
extern "C" bool nv2a_uwp_fps60;                            /* hw/xbox/nv2a/nv2a.c */

static const char *xemu_d3d12_fg_cs_source =
    "Texture2D<float4> tex0 : register(t0);\n"
    "Texture2D<float4> tex1 : register(t1);\n"
    "Texture2D<float4> tex2 : register(t2);\n"
    "RWTexture2D<float4> out0 : register(u0);\n"
    "SamplerState lin : register(s0);\n"
    "cbuffer C : register(b0) { float4 c0; float4 c1; };\n"
    "float lum(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }\n"
    /* c0.xy: source size. Quarter-size brightness, each the mean of 4x4 source pixels. */
    "[numthreads(8, 8, 1)] void cs_luma(uint3 id : SV_DispatchThreadID) {\n"
    "    uint w, h; out0.GetDimensions(w, h);\n"
    "    if (id.x >= w || id.y >= h) return;\n"
    "    int2 top = int2(c0.xy) - 1; float s = 0;\n"
    "    [unroll] for (int y = 0; y < 4; y++) [unroll] for (int x = 0; x < 4; x++)\n"
    "        s += lum(tex0.Load(int3(min(int2(id.xy * 4) + int2(x, y), top), 0)).rgb);\n"
    "    out0[id.xy] = float4(s / 16.0, 0, 0, 0);\n"
    "}\n"
    /* c0.zw: 1 / quarter size, c1.x: reach. tex0 older, tex1 newer brightness. */
    "float block_err(float2 base, float2 v, int step) {\n"
    "    float e = 0;\n"
    "    for (int y = -2; y < 6; y += step) for (int x = -2; x < 6; x += step) {\n"
    "        float2 q = base + float2(x, y);\n"
    "        e += abs(tex0.SampleLevel(lin, (q - v * 0.5) * c0.zw, 0).r -\n"
    "                 tex1.SampleLevel(lin, (q + v * 0.5) * c0.zw, 0).r);\n"
    "    }\n"
    "    return e * (step * step) / 64.0 + 0.0015 * length(v);\n"
    "}\n"
    "[numthreads(8, 8, 1)] void cs_motion(uint3 id : SV_DispatchThreadID) {\n"
    "    uint w, h; out0.GetDimensions(w, h);\n"
    "    if (id.x >= w || id.y >= h) return;\n"
    "    float2 base = float2(id.xy) * 4.0 + 0.5;\n"
    "    int r = (int)c1.x; float best = block_err(base, 0, 2); float2 bv = 0;\n"
    "    for (int vy = -r; vy <= r; vy += 2) for (int vx = -r; vx <= r; vx += 2) {\n"
    "        float e = block_err(base, float2(vx, vy), 2);\n"
    "        if (e < best) { best = e; bv = float2(vx, vy); }\n"
    "    }\n"
    "    float2 c = bv; best = block_err(base, c, 1);\n"
    "    for (int dy = -3; dy <= 3; dy++) for (int dx = -3; dx <= 3; dx++) {\n"
    "        float2 v = c + float2(dx, dy) * 0.5;\n"
    "        float e = block_err(base, v, 1);\n"
    "        if (e < best) { best = e; bv = v; }\n"
    "    }\n"
    "    out0[id.xy] = float4(bv, best, 0);\n"
    "}\n"
    "float med3(float a, float b, float c) { return max(min(a, b), min(max(a, b), c)); }\n"
    "float med9(float v[9]) {\n"
    "    float a = med3(v[0], v[1], v[2]), b = med3(v[3], v[4], v[5]), c = med3(v[6], v[7], v[8]);\n"
    "    return med3(a, b, c);\n"
    "}\n"
    "[numthreads(8, 8, 1)] void cs_smooth(uint3 id : SV_DispatchThreadID) {\n"
    "    uint w, h; out0.GetDimensions(w, h);\n"
    "    if (id.x >= w || id.y >= h) return;\n"
    "    float vx[9], vy[9]; int k = 0;\n"
    "    [unroll] for (int y = -1; y <= 1; y++) [unroll] for (int x = -1; x <= 1; x++) {\n"
    "        float2 v = tex0.Load(int3(clamp(int2(id.xy) + int2(x, y), int2(0, 0), int2(w - 1, h - 1)), 0)).xy;\n"
    "        vx[k] = v.x; vy[k] = v.y; k++;\n"
    "    }\n"
    "    out0[id.xy] = float4(med9(vx), med9(vy), 0, 0);\n"
    "}\n"
    /* c0.xy: source size, c0.zw: 1 / source size. tex0 older, tex1 newer frame, tex2 block steps. */
    "[numthreads(8, 8, 1)] void cs_mid(uint3 id : SV_DispatchThreadID) {\n"
    "    if (id.x >= (uint)c0.x || id.y >= (uint)c0.y) return;\n"
    "    uint bw, bh; tex2.GetDimensions(bw, bh);\n"
    "    float2 p = float2(id.xy) + 0.5;\n"
    "    int2 b0 = int2(floor(p / 16.0));\n"
    "    float bestE = 1e9; float3 bestC = 0;\n"
    /* the steps of the 3x3 blocks around, and no step at all (still pictures, the game's own HUD) */
    "    for (int n = 0; n < 10; n++) {\n"
    "        float2 v = 0;\n"
    "        if (n < 9) {\n"
    "            int2 b = clamp(b0 + int2(n % 3 - 1, n / 3 - 1), int2(0, 0), int2(bw - 1, bh - 1));\n"
    "            v = tex2.Load(int3(b, 0)).xy * 4.0;\n"
    "        }\n"
    "        float e = 0; float3 a0 = 0, b1 = 0;\n"
    "        [unroll] for (int k = 0; k < 9; k++) {\n"
    "            float2 o = float2(k % 3 - 1, k / 3 - 1) * 2.0;\n"
    "            float3 a = tex0.SampleLevel(lin, (p + o - v * 0.5) * c0.zw, 0).rgb;\n"
    "            float3 bb = tex1.SampleLevel(lin, (p + o + v * 0.5) * c0.zw, 0).rgb;\n"
    "            e += dot(abs(a - bb), float3(1, 1, 1)) * (k == 4 ? 2.0 : 1.0);\n"
    "            if (k == 4) { a0 = a; b1 = bb; }\n"
    "        }\n"
    "        if (n == 9) e *= 0.9;\n"   /* a tie goes to standing still */
    "        if (e < bestE) { bestE = e; bestC = (a0 + b1) * 0.5; }\n"
    "    }\n"
    /* no step fits (something jumped further than the search reaches, a cut, a flash): the newer frame as it
     * is, a skipped step there instead of a broken shape */
    "    if (bestE > c1.y) bestC = tex1.SampleLevel(lin, p * c0.zw, 0).rgb;\n"
    "    out0[id.xy] = float4(bestC, 1);\n"
    "}\n";

static const char *xemu_d3d12_fg_blit_source =
    "Texture2D source_texture : register(t0);\n"
    "SamplerState linear_sampler : register(s0);\n"
    "SamplerState nearest_sampler : register(s1);\n"
    "cbuffer C : register(b0) { float4 c0; };\n"
    "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "VSOut vs_main(uint vertex_id : SV_VertexID) {\n"
    "    float2 pos[3] = { float2(-1.0, -1.0), float2(-1.0, 3.0), float2(3.0, -1.0) };\n"
    "    float2 uv[3] = { float2(0.0, 1.0), float2(0.0, -1.0), float2(2.0, 1.0) };\n"
    "    VSOut o; o.pos = float4(pos[vertex_id], 0.0, 1.0); o.uv = uv[vertex_id]; return o;\n"
    "}\n"
    "float4 ps_main(VSOut i) : SV_Target {\n"
    "    float4 c = c0.x > 0.5 ? source_texture.Sample(nearest_sampler, i.uv)\n"
    "                          : source_texture.Sample(linear_sampler, i.uv);\n"
    "    return float4(c.rgb, 1.0);\n"
    "}\n";

static void fg_release_targets(XemuD3D12Presenter *p)
{
    for (int i = 0; i < 2; i++) {
        d3d12_release(&p->fg_frame[i]);
        d3d12_release(&p->fg_luma[i]);
        d3d12_release(&p->fg_mv[i]);
    }
    d3d12_release(&p->fg_mid);
    p->fg_width = p->fg_height = 0;
    p->fg_have_prev = false;
    p->fg_pending = false;
}

static void fg_release(XemuD3D12Presenter *p)
{
    fg_release_targets(p);
    d3d12_release(&p->fg_cs_root);
    d3d12_release(&p->fg_luma_pso);
    d3d12_release(&p->fg_motion_pso);
    d3d12_release(&p->fg_smooth_pso);
    d3d12_release(&p->fg_mid_pso);
    d3d12_release(&p->fg_blit_pso);
}

static bool fg_create_pipelines(XemuD3D12Presenter *p)
{
    D3D12_DESCRIPTOR_RANGE ranges[4] = {};
    D3D12_ROOT_PARAMETER params[5] = {};
    D3D12_STATIC_SAMPLER_DESC sampler = {};
    D3D12_ROOT_SIGNATURE_DESC root_desc = {};
    ID3DBlob *signature = NULL, *signature_error = NULL;
    const char *entries[4] = { "cs_luma", "cs_motion", "cs_smooth", "cs_mid" };
    ID3D12PipelineState **psos[4] = { &p->fg_luma_pso, &p->fg_motion_pso, &p->fg_smooth_pso, &p->fg_mid_pso };
    ID3DBlob *vs = NULL, *ps = NULL;
    HRESULT hr;

    /* t0, t1, t2 and u0: one descriptor each, anywhere in the heap */
    for (int i = 0; i < 4; i++) {
        ranges[i].RangeType = i < 3 ? D3D12_DESCRIPTOR_RANGE_TYPE_SRV : D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[i].NumDescriptors = 1;
        ranges[i].BaseShaderRegister = i < 3 ? i : 0;
        params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[i].DescriptorTable.NumDescriptorRanges = 1;
        params[i].DescriptorTable.pDescriptorRanges = &ranges[i];
    }
    params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[4].Constants.Num32BitValues = 8;
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    root_desc.NumParameters = 5;
    root_desc.pParameters = params;
    root_desc.NumStaticSamplers = 1;
    root_desc.pStaticSamplers = &sampler;

    hr = D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &signature_error);
    if (FAILED(hr)) {
        d3d12_release(&signature_error);
        return set_hresult_error(p, hr, "D3D12SerializeRootSignature(frame generation)");
    }
    hr = p->device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
                                        IID_PPV_ARGS(&p->fg_cs_root));
    d3d12_release(&signature);
    d3d12_release(&signature_error);
    if (FAILED(hr)) {
        return set_hresult_error(p, hr, "CreateRootSignature(frame generation)");
    }

    for (int i = 0; i < 4; i++) {
        ID3DBlob *cs = NULL;
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
        if (!compile_shader(p, xemu_d3d12_fg_cs_source, entries[i], "cs_5_0", &cs)) {
            return false;
        }
        desc.pRootSignature = p->fg_cs_root;
        desc.CS.pShaderBytecode = cs->GetBufferPointer();
        desc.CS.BytecodeLength = cs->GetBufferSize();
        hr = p->device->CreateComputePipelineState(&desc, IID_PPV_ARGS(psos[i]));
        d3d12_release(&cs);
        if (FAILED(hr)) {
            return set_hresult_error(p, hr, "CreateComputePipelineState(frame generation)");
        }
    }

    bool ok = compile_shader(p, xemu_d3d12_fg_blit_source, "vs_main", "vs_5_0", &vs) &&
              compile_shader(p, xemu_d3d12_fg_blit_source, "ps_main", "ps_5_0", &ps) &&
              create_fullscreen_pso(p, vs, ps, &p->fg_blit_pso, "CreateGraphicsPipelineState(frame generation)");
    d3d12_release(&vs);
    d3d12_release(&ps);
    return ok;
}

static bool fg_create_texture(XemuD3D12Presenter *p, UINT w, UINT h, DXGI_FORMAT format, bool rtv, int rtv_index,
                              int srv_index, int uav_index, ID3D12Resource **out)
{
    D3D12_HEAP_PROPERTIES heap = {};
    D3D12_RESOURCE_DESC desc = {};
    D3D12_CLEAR_VALUE clear = {};
    HRESULT hr;

    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = w;
    desc.Height = h;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = rtv ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    clear.Format = format;
    clear.Color[3] = 1.0f;
    /* kept in NON_PIXEL | PIXEL shader resource between uses */
    hr = p->device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                            D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, rtv ? &clear : NULL,
                                            IID_PPV_ARGS(out));
    if (FAILED(hr)) {
        *out = NULL;
        return set_hresult_error(p, hr, "CreateCommittedResource(frame generation)");
    }
    if (rtv) {
        p->device->CreateRenderTargetView(*out, NULL, cpu_descriptor(p->rtv_heap, p->rtv_descriptor_size, rtv_index));
    }
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Format = format;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        p->device->CreateShaderResourceView(*out, &srv, cpu_descriptor(p->srv_heap, p->srv_descriptor_size, srv_index));
    }
    if (uav_index >= 0) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
        uav.Format = format;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        p->device->CreateUnorderedAccessView(*out, NULL, &uav,
                                             cpu_descriptor(p->srv_heap, p->srv_descriptor_size, uav_index));
    }
    return true;
}

static bool fg_prepare_targets(XemuD3D12Presenter *p, UINT w, UINT h)
{
    UINT qw = (w + 3) / 4, qh = (h + 3) / 4, bw = (qw + 3) / 4, bh = (qh + 3) / 4;

    if (p->fg_frame[0] && p->fg_width == w && p->fg_height == h) {
        return true;
    }
    if (p->fg_frame[0] && !wait_for_gpu(p)) {
        return false;
    }
    fg_release_targets(p);
    for (int i = 0; i < 2; i++) {
        if (!fg_create_texture(p, w, h, DXGI_FORMAT_B8G8R8A8_UNORM, true, XEMU_D3D12_FG_FRAME_RTV(i),
                               XEMU_D3D12_FG_FRAME_SRV(i), -1, &p->fg_frame[i]) ||
            !fg_create_texture(p, qw, qh, DXGI_FORMAT_R16_FLOAT, false, -1, XEMU_D3D12_FG_LUMA_SRV(i),
                               XEMU_D3D12_FG_LUMA_UAV(i), &p->fg_luma[i]) ||
            !fg_create_texture(p, bw, bh, DXGI_FORMAT_R16G16B16A16_FLOAT, false, -1, XEMU_D3D12_FG_MV_SRV(i),
                               XEMU_D3D12_FG_MV_UAV(i), &p->fg_mv[i])) {
            fg_release_targets(p);
            return false;
        }
    }
    if (!fg_create_texture(p, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, false, -1, XEMU_D3D12_FG_MID_SRV,
                           XEMU_D3D12_FG_MID_UAV, &p->fg_mid)) {
        fg_release_targets(p);
        return false;
    }
    p->fg_width = w;
    p->fg_height = h;
    uwp_log("frame generation: %ux%u, motion in %ux%u blocks", w, h, bw, bh);
    return true;
}

/* Frame generation is wanted: the game's 60 FPS switch (or framegen.txt for testing), and the TV at 120 Hz.
 * The TV is asked once; its rate is seen from how often frames are shown. */
static bool fg_wanted(XemuD3D12Presenter *p)
{
    static int forced = -1;
    if (forced < 0) {
        forced = xemu_uwp_test_file_exists("framegen.txt");
        if (xemu_uwp_test_file_exists("framegen-off.txt")) {
            forced = 2;
        }
    }
    if (forced == 2 || p->fg_unavailable) {
        return false;
    }
    bool want = forced || nv2a_uwp_fps60;
    if (want && !p->fg_asked_tv) {
        p->fg_asked_tv = true;
        xemu_uwp_hdmi_request_hz(120.0);
    }
    return want;
}

static void fg_uav_barrier(ID3D12GraphicsCommandList *cl, ID3D12Resource *r)
{
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = r;
    cl->ResourceBarrier(1, &b);
}

static void fg_state(ID3D12GraphicsCommandList *cl, ID3D12Resource *r, D3D12_RESOURCE_STATES from,
                     D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_STATES s = from;
    transition_resource(cl, r, &s, to);
}

static void fg_dispatch(XemuD3D12Presenter *p, ID3D12PipelineState *pso, int t0, int t1, int t2, int u0,
                        ID3D12Resource *target, const float c[8], UINT w, UINT h)
{
    ID3D12GraphicsCommandList *cl = p->command_list;
    fg_state(cl, target, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cl->SetPipelineState(pso);
    cl->SetComputeRootDescriptorTable(0, gpu_descriptor(p->srv_heap, p->srv_descriptor_size, t0));
    cl->SetComputeRootDescriptorTable(1, gpu_descriptor(p->srv_heap, p->srv_descriptor_size, t1 >= 0 ? t1 : t0));
    cl->SetComputeRootDescriptorTable(2, gpu_descriptor(p->srv_heap, p->srv_descriptor_size, t2 >= 0 ? t2 : t0));
    cl->SetComputeRootDescriptorTable(3, gpu_descriptor(p->srv_heap, p->srv_descriptor_size, u0));
    cl->SetComputeRoot32BitConstants(4, 8, c, 0);
    cl->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
    fg_uav_barrier(cl, target);
    fg_state(cl, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
}

/* The chosen picture (an SRV slot) into the display rectangle of the backbuffer. */
static void fg_output(XemuD3D12Presenter *p, int srv, const D3D12_VIEWPORT *viewport, const D3D12_RECT *scissor)
{
    ID3D12GraphicsCommandList *cl = p->command_list;
    ID3D12DescriptorHeap *heaps[] = { p->srv_heap };
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = cpu_descriptor(p->rtv_heap, p->rtv_descriptor_size, p->frame_index);
    float constants[XEMU_D3D12_DISPLAY_CONSTANTS] = {};

    constants[0] = g_config.display.filtering == CONFIG_DISPLAY_FILTERING_NEAREST ? 1.0f : 0.0f;
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetGraphicsRootSignature(p->display_root_signature);
    cl->SetPipelineState(p->fg_blit_pso);
    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cl->OMSetRenderTargets(1, &rtv, FALSE, NULL);
    cl->SetGraphicsRootDescriptorTable(0, gpu_descriptor(p->srv_heap, p->srv_descriptor_size, srv));
    cl->SetGraphicsRoot32BitConstants(1, XEMU_D3D12_DISPLAY_CONSTANTS, constants, 0);
    cl->RSSetViewports(1, viewport);
    cl->RSSetScissorRects(1, scissor);
    cl->DrawInstanced(3, 1, 0, 0);
}

/* Instead of the plain draw: expects the display pipeline, source table and display constants bound, as the
 * plain draw does. False when frame generation does not take this frame (then the caller draws as before). */
static bool fg_draw(XemuD3D12Presenter *p, const NV2ADisplayFrame *frame, UINT compose_width, UINT compose_height,
                    const D3D12_VIEWPORT *viewport, const D3D12_RECT *scissor)
{
    ID3D12GraphicsCommandList *cl = p->command_list;
    bool fresh;
    uint64_t gap;

    p->fg_presents++;
    if (frame->type != NV2A_DISPLAY_FRAME_D3D12_RESOURCE || !fg_wanted(p) ||
        compose_width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION || compose_height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION) {
        p->fg_have_prev = false;
        return false;
    }
    if ((!p->fg_cs_root && !fg_create_pipelines(p)) || !fg_prepare_targets(p, compose_width, compose_height)) {
        uwp_log("frame generation: not available: %s", p->error);
        p->error[0] = '\0';
        p->fg_unavailable = true;
        fg_release(p);
        return false;
    }

    fresh = frame->d3d12.ready_value != p->fg_last_ready;
    if (!fresh) {   /* the same game frame again: the newest one (the one held back, or a repeat) */
        p->fg_pending = false;
        fg_output(p, XEMU_D3D12_FG_FRAME_SRV(p->fg_cur), viewport, scissor);
        return true;
    }
    gap = p->fg_presents - p->fg_last_fresh_present;
    p->fg_last_fresh_present = p->fg_presents;
    p->fg_last_ready = frame->d3d12.ready_value;
    p->fg_cur ^= 1;

    /* the scanout, overlay and gamma ramp at source size, into this frame's history picture */
    {
        D3D12_CPU_DESCRIPTOR_HANDLE rtv =
            cpu_descriptor(p->rtv_heap, p->rtv_descriptor_size, XEMU_D3D12_FG_FRAME_RTV(p->fg_cur));
        D3D12_VIEWPORT vp = {};
        D3D12_RECT sc = {};
        fg_state(cl, p->fg_frame[p->fg_cur], D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
                 D3D12_RESOURCE_STATE_RENDER_TARGET);
        cl->OMSetRenderTargets(1, &rtv, FALSE, NULL);
        vp.Width = (float)compose_width;
        vp.Height = (float)compose_height;
        vp.MaxDepth = 1.0f;
        sc.right = (LONG)compose_width;
        sc.bottom = (LONG)compose_height;
        cl->RSSetViewports(1, &vp);
        cl->RSSetScissorRects(1, &sc);
        cl->DrawInstanced(3, 1, 0, 0);
        fg_state(cl, p->fg_frame[p->fg_cur], D3D12_RESOURCE_STATE_RENDER_TARGET,
                 D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    }

    UINT qw = (compose_width + 3) / 4, qh = (compose_height + 3) / 4, bw = (qw + 3) / 4, bh = (qh + 3) / 4;
    float c[8] = { (float)compose_width, (float)compose_height, 1.0f / qw, 1.0f / qh,
                   (float)XEMU_D3D12_FG_REACH, 0, 0, 0 };
    ID3D12DescriptorHeap *heaps[] = { p->srv_heap };
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetComputeRootSignature(p->fg_cs_root);
    fg_dispatch(p, p->fg_luma_pso, XEMU_D3D12_FG_FRAME_SRV(p->fg_cur), -1, -1, XEMU_D3D12_FG_LUMA_UAV(p->fg_cur),
                p->fg_luma[p->fg_cur], c, qw, qh);

    /* a picture between the two only when game frames come every 2nd refresh (60 fps on a 120 Hz TV).
     * framegen-mid.txt (testing): at 60 Hz too, and only the made pictures are shown, to judge them on
     * screenshots (the picture is then half a frame late). */
    static int mid_test = -1;
    if (mid_test < 0) {
        mid_test = xemu_uwp_test_file_exists("framegen-mid.txt");
    }
    if (p->fg_have_prev && (gap == 2 || mid_test)) {
        int prev = p->fg_cur ^ 1, cur = p->fg_cur;
        fg_dispatch(p, p->fg_motion_pso, XEMU_D3D12_FG_LUMA_SRV(prev), XEMU_D3D12_FG_LUMA_SRV(cur), -1,
                    XEMU_D3D12_FG_MV_UAV(0), p->fg_mv[0], c, bw, bh);
        fg_dispatch(p, p->fg_smooth_pso, XEMU_D3D12_FG_MV_SRV(0), -1, -1, XEMU_D3D12_FG_MV_UAV(1), p->fg_mv[1], c,
                    bw, bh);
        c[2] = 1.0f / compose_width;
        c[5] = 1.5f;   /* above this mismatch (9 samples, centre twice, RGB summed) the newer frame is shown */
        c[3] = 1.0f / compose_height;
        fg_dispatch(p, p->fg_mid_pso, XEMU_D3D12_FG_FRAME_SRV(prev), XEMU_D3D12_FG_FRAME_SRV(cur),
                    XEMU_D3D12_FG_MV_SRV(1), XEMU_D3D12_FG_MID_UAV, p->fg_mid, c, compose_width, compose_height);
        fg_output(p, XEMU_D3D12_FG_MID_SRV, viewport, scissor);
        p->fg_pending = !mid_test;
        p->fg_made++;
    } else {
        fg_output(p, XEMU_D3D12_FG_FRAME_SRV(p->fg_cur), viewport, scissor);
    }
    p->fg_have_prev = true;

    /* every 10 s: how many pictures were made, and how often game frames came */
    if (p->fg_presents - p->fg_logged_at >= 1200) {
        uwp_log("frame generation: %llu made in the last %llu refreshes (last gap %llu)",
                (unsigned long long)p->fg_made, (unsigned long long)(p->fg_presents - p->fg_logged_at),
                (unsigned long long)gap);
        p->fg_logged_at = p->fg_presents;
        p->fg_made = 0;
    }
    return true;
}
