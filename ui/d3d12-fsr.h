/*
 * FSR 1 shaders for the Direct3D 12 presenter: edge-adaptive spatial
 * upsampling (EASU) and robust contrast-adaptive sharpening (RCAS), FP32.
 *
 * Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#ifndef UI_D3D12_FSR_H
#define UI_D3D12_FSR_H

/* 32-bit values in FsrConstants: EASU con0-con3, then RCAS
 * {strength 0-1, output origin x, output origin y, unused}. */
#define XEMU_D3D12_FSR_CONSTANTS 20

static const char xemu_d3d12_fsr_shader_source[] =
    "Texture2D fsr_input : register(t0);\n"
    "SamplerState linear_sampler : register(s0);\n"
    "cbuffer FsrConstants : register(b0) {\n"
    "    float4 easu_con0;\n"
    "    float4 easu_con1;\n"
    "    float4 easu_con2;\n"
    "    float4 easu_con3;\n"
    "    float4 rcas_con;\n"
    "};\n"
    "float4 fsr_vs(uint vertex_id : SV_VertexID) : SV_Position {\n"
    "    float2 pos[3] = { float2(-1.0, -1.0), float2(-1.0, 3.0),\n"
    "                      float2(3.0, -1.0) };\n"
    "    return float4(pos[vertex_id], 0.0, 1.0);\n"
    "}\n"
    "float rcp_lo(float a) { return asfloat(0x7ef07ebbu - asuint(a)); }\n"
    "float rsq_lo(float a) {\n"
    "    return asfloat(0x5f347d74u - (asuint(a) >> 1u));\n"
    "}\n"
    /* Gradient direction and edge length around one bilinear corner:
     * a above, b left, c centre, d right, e below. */
    "void easu_set(inout float2 dir, inout float len, float w, float la,\n"
    "              float lb, float lc, float ld, float le) {\n"
    "    float lx = saturate(abs(ld - lb) *\n"
    "                        rcp_lo(max(abs(ld - lc), abs(lc - lb))));\n"
    "    float ly = saturate(abs(le - la) *\n"
    "                        rcp_lo(max(abs(le - lc), abs(lc - la))));\n"
    "    dir += float2(ld - lb, le - la) * w;\n"
    "    len += (lx * lx + ly * ly) * w;\n"
    "}\n"
    /* Windowed approximation of Lanczos-2, stretched along the edge. */
    "void easu_tap(inout float3 acc, inout float acc_w, float2 off,\n"
    "              float2 dir, float2 len, float lob, float clp, float3 c) {\n"
    "    float2 v = float2(off.x * dir.x + off.y * dir.y,\n"
    "                      off.y * dir.x - off.x * dir.y) * len;\n"
    "    float d2 = min(dot(v, v), clp);\n"
    "    float wb = 0.4 * d2 - 1.0;\n"
    "    float wa = lob * d2 - 1.0;\n"
    "    wb *= wb;\n"
    "    wa *= wa;\n"
    "    float w = (1.5625 * wb - 0.5625) * wa;\n"
    "    acc += c * w;\n"
    "    acc_w += w;\n"
    "}\n"
    /* 12-tap kernel around f, gathered as bczz, ijfe, klhg and zzon:
     *     b c
     *   e f g h
     *   i j k l
     *     n o */
    "float4 easu_ps(float4 position : SV_Position) : SV_Target {\n"
    "    float2 pp = floor(position.xy) * easu_con0.xy + easu_con0.zw;\n"
    "    float2 fp = floor(pp);\n"
    "    pp -= fp;\n"
    "    float2 p0 = fp * easu_con1.xy + easu_con1.zw;\n"
    "    float2 p1 = p0 + easu_con2.xy;\n"
    "    float2 p2 = p0 + easu_con2.zw;\n"
    "    float2 p3 = p0 + easu_con3.xy;\n"
    "    float4 bczz_r = fsr_input.GatherRed(linear_sampler, p0);\n"
    "    float4 bczz_g = fsr_input.GatherGreen(linear_sampler, p0);\n"
    "    float4 bczz_b = fsr_input.GatherBlue(linear_sampler, p0);\n"
    "    float4 ijfe_r = fsr_input.GatherRed(linear_sampler, p1);\n"
    "    float4 ijfe_g = fsr_input.GatherGreen(linear_sampler, p1);\n"
    "    float4 ijfe_b = fsr_input.GatherBlue(linear_sampler, p1);\n"
    "    float4 klhg_r = fsr_input.GatherRed(linear_sampler, p2);\n"
    "    float4 klhg_g = fsr_input.GatherGreen(linear_sampler, p2);\n"
    "    float4 klhg_b = fsr_input.GatherBlue(linear_sampler, p2);\n"
    "    float4 zzon_r = fsr_input.GatherRed(linear_sampler, p3);\n"
    "    float4 zzon_g = fsr_input.GatherGreen(linear_sampler, p3);\n"
    "    float4 zzon_b = fsr_input.GatherBlue(linear_sampler, p3);\n"
    "    float4 bczz_l = bczz_b * 0.5 + (bczz_r * 0.5 + bczz_g);\n"
    "    float4 ijfe_l = ijfe_b * 0.5 + (ijfe_r * 0.5 + ijfe_g);\n"
    "    float4 klhg_l = klhg_b * 0.5 + (klhg_r * 0.5 + klhg_g);\n"
    "    float4 zzon_l = zzon_b * 0.5 + (zzon_r * 0.5 + zzon_g);\n"
    "    float2 dir = float2(0.0, 0.0);\n"
    "    float len = 0.0;\n"
    "    easu_set(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bczz_l.x,\n"
    "             ijfe_l.w, ijfe_l.z, klhg_l.w, ijfe_l.y);\n"
    "    easu_set(dir, len, pp.x * (1.0 - pp.y), bczz_l.y, ijfe_l.z,\n"
    "             klhg_l.w, klhg_l.z, klhg_l.x);\n"
    "    easu_set(dir, len, (1.0 - pp.x) * pp.y, ijfe_l.z, ijfe_l.x,\n"
    "             ijfe_l.y, klhg_l.x, zzon_l.w);\n"
    "    easu_set(dir, len, pp.x * pp.y, klhg_l.w, ijfe_l.y, klhg_l.x,\n"
    "             klhg_l.y, zzon_l.z);\n"
    "    float dir_r = dot(dir, dir);\n"
    "    bool no_dir = dir_r < 1.0 / 32768.0;\n"
    "    dir_r = no_dir ? 1.0 : rsq_lo(dir_r);\n"
    "    dir.x = no_dir ? 1.0 : dir.x;\n"
    "    dir *= dir_r;\n"
    "    len = len * 0.5;\n"
    "    len *= len;\n"
    "    float stretch = dot(dir, dir) * rcp_lo(max(abs(dir.x), abs(dir.y)));\n"
    "    float2 len2 = float2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);\n"
    "    float lob = 0.5 - 0.29 * len;\n"
    "    float clp = rcp_lo(lob);\n"
    "    float3 f = float3(ijfe_r.z, ijfe_g.z, ijfe_b.z);\n"
    "    float3 g = float3(klhg_r.w, klhg_g.w, klhg_b.w);\n"
    "    float3 j = float3(ijfe_r.y, ijfe_g.y, ijfe_b.y);\n"
    "    float3 k = float3(klhg_r.x, klhg_g.x, klhg_b.x);\n"
    "    float3 acc = float3(0.0, 0.0, 0.0);\n"
    "    float acc_w = 0.0;\n"
    "    easu_tap(acc, acc_w, float2(0.0, -1.0) - pp, dir, len2, lob, clp,\n"
    "             float3(bczz_r.x, bczz_g.x, bczz_b.x));\n"
    "    easu_tap(acc, acc_w, float2(1.0, -1.0) - pp, dir, len2, lob, clp,\n"
    "             float3(bczz_r.y, bczz_g.y, bczz_b.y));\n"
    "    easu_tap(acc, acc_w, float2(-1.0, 1.0) - pp, dir, len2, lob, clp,\n"
    "             float3(ijfe_r.x, ijfe_g.x, ijfe_b.x));\n"
    "    easu_tap(acc, acc_w, float2(0.0, 1.0) - pp, dir, len2, lob, clp, j);\n"
    "    easu_tap(acc, acc_w, -pp, dir, len2, lob, clp, f);\n"
    "    easu_tap(acc, acc_w, float2(-1.0, 0.0) - pp, dir, len2, lob, clp,\n"
    "             float3(ijfe_r.w, ijfe_g.w, ijfe_b.w));\n"
    "    easu_tap(acc, acc_w, float2(1.0, 1.0) - pp, dir, len2, lob, clp, k);\n"
    "    easu_tap(acc, acc_w, float2(2.0, 1.0) - pp, dir, len2, lob, clp,\n"
    "             float3(klhg_r.y, klhg_g.y, klhg_b.y));\n"
    "    easu_tap(acc, acc_w, float2(2.0, 0.0) - pp, dir, len2, lob, clp,\n"
    "             float3(klhg_r.z, klhg_g.z, klhg_b.z));\n"
    "    easu_tap(acc, acc_w, float2(1.0, 0.0) - pp, dir, len2, lob, clp, g);\n"
    "    easu_tap(acc, acc_w, float2(1.0, 2.0) - pp, dir, len2, lob, clp,\n"
    "             float3(zzon_r.z, zzon_g.z, zzon_b.z));\n"
    "    easu_tap(acc, acc_w, float2(0.0, 2.0) - pp, dir, len2, lob, clp,\n"
    "             float3(zzon_r.w, zzon_g.w, zzon_b.w));\n"
    /* Deringing: clamp to the four nearest texels. */
    "    float3 lo = min(min(f, g), min(j, k));\n"
    "    float3 hi = max(max(f, g), max(j, k));\n"
    "    return float4(min(hi, max(lo, acc * rcp(acc_w))), 1.0);\n"
    "}\n"
    /* Sharpening limited by the 3x3 cross so it cannot clip:
     *     b
     *   d e f
     *     h */
    "float4 rcas_ps(float4 position : SV_Position) : SV_Target {\n"
    "    int2 sp = int2(position.xy - rcas_con.yz);\n"
    "    uint width, height;\n"
    "    fsr_input.GetDimensions(width, height);\n"
    "    int2 lim = int2(width, height) - 1;\n"
    "    float3 b = fsr_input.Load(int3(clamp(sp + int2(0, -1), 0, lim), 0)).rgb;\n"
    "    float3 d = fsr_input.Load(int3(clamp(sp + int2(-1, 0), 0, lim), 0)).rgb;\n"
    "    float3 e = fsr_input.Load(int3(clamp(sp, 0, lim), 0)).rgb;\n"
    "    float3 f = fsr_input.Load(int3(clamp(sp + int2(1, 0), 0, lim), 0)).rgb;\n"
    "    float3 h = fsr_input.Load(int3(clamp(sp + int2(0, 1), 0, lim), 0)).rgb;\n"
    "    float3 mn4 = min(min(b, d), min(f, h));\n"
    "    float3 mx4 = max(max(b, d), max(f, h));\n"
    "    float3 hit_min = min(mn4, e) * rcp(4.0 * mx4);\n"
    "    float3 hit_max = (1.0 - max(mx4, e)) * rcp(4.0 * mn4 - 4.0);\n"
    "    float3 lobe3 = max(-hit_min, hit_max);\n"
    "    float lobe = max(-0.1875, min(max(lobe3.r, max(lobe3.g, lobe3.b)),\n"
    "                                  0.0)) * rcas_con.x;\n"
    /* Exact divide: zero strength passes the upscale through unchanged. */
    "    float3 c = (lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0);\n"
    "    return float4(c, 1.0);\n"
    "}\n";

#endif
