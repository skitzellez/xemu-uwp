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
#include "qemu/fast-hash.h"
#include <glib/gstdio.h>
#include <io.h>
#include "hw/xbox/nv2a/pgraph/psh_regs.h"
#include "hw/xbox/nv2a/pgraph/util.h"
#include "hw/xbox/nv2a/pgraph/texsigns.h"
#include "hw/xbox/nv2a/pgraph/glsl/psh.h"
#include "qemu/xemu-uwp.h"
#include "ui/xemu-settings.h"
#include "renderer.h"

#define DXC_CP_UTF8 65001

/* Bump to invalidate on-disk DXIL when compile flags or codegen change. */
#define D3D12_SHADER_CACHE_VERSION 39

typedef struct IDxcBlob IDxcBlob;
typedef struct IDxcResult IDxcResult;
typedef struct IDxcCompiler3 IDxcCompiler3;

typedef enum DXC_OUT_KIND {
    DXC_OUT_OBJECT = 1,
    DXC_OUT_ERRORS = 2,
} DXC_OUT_KIND;

typedef struct DxcBuffer {
    LPCVOID Ptr;
    SIZE_T Size;
    UINT Encoding;
} DxcBuffer;

typedef HRESULT(__stdcall *DxcCreateInstanceProc)(REFCLSID rclsid,
                                                  REFIID riid, LPVOID *ppv);

typedef struct IDxcBlobVtbl {
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(IDxcBlob *This, REFIID riid,
                                               void **ppvObject);
    ULONG(STDMETHODCALLTYPE *AddRef)(IDxcBlob *This);
    ULONG(STDMETHODCALLTYPE *Release)(IDxcBlob *This);
    LPVOID(STDMETHODCALLTYPE *GetBufferPointer)(IDxcBlob *This);
    SIZE_T(STDMETHODCALLTYPE *GetBufferSize)(IDxcBlob *This);
} IDxcBlobVtbl;

struct IDxcBlob {
    const IDxcBlobVtbl *lpVtbl;
};

typedef struct IDxcResultVtbl {
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(IDxcResult *This, REFIID riid,
                                               void **ppvObject);
    ULONG(STDMETHODCALLTYPE *AddRef)(IDxcResult *This);
    ULONG(STDMETHODCALLTYPE *Release)(IDxcResult *This);
    HRESULT(STDMETHODCALLTYPE *GetStatus)(IDxcResult *This, HRESULT *pStatus);
    HRESULT(STDMETHODCALLTYPE *GetResult)(IDxcResult *This,
                                          IDxcBlob **ppResult);
    HRESULT(STDMETHODCALLTYPE *GetErrorBuffer)(IDxcResult *This,
                                               void **ppErrors);
    BOOL(STDMETHODCALLTYPE *HasOutput)(IDxcResult *This,
                                       DXC_OUT_KIND dxcOutKind);
    HRESULT(STDMETHODCALLTYPE *GetOutput)(IDxcResult *This,
                                          DXC_OUT_KIND dxcOutKind,
                                          REFIID iid, void **ppvObject,
                                          void **ppOutputName);
    UINT32(STDMETHODCALLTYPE *GetNumOutputs)(IDxcResult *This);
    DXC_OUT_KIND(STDMETHODCALLTYPE *GetOutputByIndex)(IDxcResult *This,
                                                      UINT32 index);
    DXC_OUT_KIND(STDMETHODCALLTYPE *PrimaryOutput)(IDxcResult *This);
} IDxcResultVtbl;

struct IDxcResult {
    const IDxcResultVtbl *lpVtbl;
};

typedef struct IDxcCompiler3Vtbl {
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(IDxcCompiler3 *This,
                                               REFIID riid,
                                               void **ppvObject);
    ULONG(STDMETHODCALLTYPE *AddRef)(IDxcCompiler3 *This);
    ULONG(STDMETHODCALLTYPE *Release)(IDxcCompiler3 *This);
    HRESULT(STDMETHODCALLTYPE *Compile)(IDxcCompiler3 *This,
                                        const DxcBuffer *pSource,
                                        LPCWSTR *pArguments, UINT32 argCount,
                                        void *pIncludeHandler, REFIID riid,
                                        LPVOID *ppResult);
    HRESULT(STDMETHODCALLTYPE *Disassemble)(IDxcCompiler3 *This,
                                            const DxcBuffer *pObject,
                                            REFIID riid, LPVOID *ppResult);
} IDxcCompiler3Vtbl;

struct IDxcCompiler3 {
    const IDxcCompiler3Vtbl *lpVtbl;
};

static const GUID CLSID_DxcCompiler = {
    0x73e22d93,
    0xe6ce,
    0x47f3,
    { 0xb5, 0xbf, 0xf0, 0x66, 0x4f, 0x39, 0xc1, 0xb0 },
};

static const GUID IID_IDxcBlob = {
    0x8ba5fb08,
    0x5195,
    0x40e2,
    { 0xac, 0x58, 0x0d, 0x98, 0x9c, 0x3a, 0x01, 0x02 },
};

static const GUID IID_IDxcBlobUtf8 = {
    0x3da636c9,
    0xba71,
    0x4024,
    { 0xa3, 0x01, 0x30, 0xcb, 0xf1, 0x25, 0x30, 0x5b },
};

static const GUID IID_IDxcCompiler3 = {
    0x228b4687,
    0x5a6a,
    0x4730,
    { 0x90, 0x0c, 0x97, 0x02, 0xb2, 0x20, 0x3f, 0x54 },
};

static const GUID IID_IDxcResult = {
    0x58346cda,
    0xdde7,
    0x4497,
    { 0x94, 0x61, 0x6f, 0x87, 0xaf, 0x5e, 0x06, 0x59 },
};

static HMODULE cached_dxcompiler_module;
static HMODULE cached_dxil_module;
static DxcCreateInstanceProc cached_dxc_create_instance;

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

static const char *texture_convolution_filter_name(unsigned int filter);

static const char *primitive_topology_type_name(
    D3D12_PRIMITIVE_TOPOLOGY_TYPE type)
{
    switch (type) {
    case D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT:
        return "point";
    case D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE:
        return "line";
    case D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE:
        return "triangle";
    case D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH:
        return "patch";
    default:
        return "undefined";
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

static const char *depth_format_name(PGRAPHD3D12DepthFormat format)
{
    switch (format) {
    case PGRAPH_D3D12_DEPTH_FORMAT_D24:
        return "D24";
    case PGRAPH_D3D12_DEPTH_FORMAT_D16:
        return "D16";
    case PGRAPH_D3D12_DEPTH_FORMAT_F24:
        return "F24";
    case PGRAPH_D3D12_DEPTH_FORMAT_F16:
        return "F16";
    default:
        return "unknown";
    }
}

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

static const D3D12_COMPARISON_FUNC d3d12_compare_func_map[] = {
    D3D12_COMPARISON_FUNC_NEVER,
    D3D12_COMPARISON_FUNC_LESS,
    D3D12_COMPARISON_FUNC_EQUAL,
    D3D12_COMPARISON_FUNC_LESS_EQUAL,
    D3D12_COMPARISON_FUNC_GREATER,
    D3D12_COMPARISON_FUNC_NOT_EQUAL,
    D3D12_COMPARISON_FUNC_GREATER_EQUAL,
    D3D12_COMPARISON_FUNC_ALWAYS,
};

static const D3D12_STENCIL_OP d3d12_stencil_op_map[] = {
    0,
    D3D12_STENCIL_OP_KEEP,
    D3D12_STENCIL_OP_ZERO,
    D3D12_STENCIL_OP_REPLACE,
    D3D12_STENCIL_OP_INCR_SAT,
    D3D12_STENCIL_OP_DECR_SAT,
    D3D12_STENCIL_OP_INVERT,
    D3D12_STENCIL_OP_INCR,
    D3D12_STENCIL_OP_DECR,
};

static const D3D12_BLEND d3d12_blend_factor_map[] = {
    D3D12_BLEND_ZERO,
    D3D12_BLEND_ONE,
    D3D12_BLEND_SRC_COLOR,
    D3D12_BLEND_INV_SRC_COLOR,
    D3D12_BLEND_SRC_ALPHA,
    D3D12_BLEND_INV_SRC_ALPHA,
    D3D12_BLEND_DEST_ALPHA,
    D3D12_BLEND_INV_DEST_ALPHA,
    D3D12_BLEND_DEST_COLOR,
    D3D12_BLEND_INV_DEST_COLOR,
    D3D12_BLEND_SRC_ALPHA_SAT,
    0,
    D3D12_BLEND_BLEND_FACTOR,
    D3D12_BLEND_INV_BLEND_FACTOR,
    D3D12_BLEND_BLEND_FACTOR,
    D3D12_BLEND_INV_BLEND_FACTOR,
};

static const D3D12_BLEND d3d12_alpha_blend_factor_map[] = {
    D3D12_BLEND_ZERO,
    D3D12_BLEND_ONE,
    D3D12_BLEND_SRC_ALPHA,
    D3D12_BLEND_INV_SRC_ALPHA,
    D3D12_BLEND_SRC_ALPHA,
    D3D12_BLEND_INV_SRC_ALPHA,
    D3D12_BLEND_DEST_ALPHA,
    D3D12_BLEND_INV_DEST_ALPHA,
    D3D12_BLEND_DEST_ALPHA,
    D3D12_BLEND_INV_DEST_ALPHA,
    D3D12_BLEND_ONE,
    0,
    D3D12_BLEND_BLEND_FACTOR,
    D3D12_BLEND_INV_BLEND_FACTOR,
    D3D12_BLEND_BLEND_FACTOR,
    D3D12_BLEND_INV_BLEND_FACTOR,
};

static const D3D12_BLEND_OP d3d12_blend_equation_map[] = {
    D3D12_BLEND_OP_SUBTRACT,
    D3D12_BLEND_OP_REV_SUBTRACT,
    D3D12_BLEND_OP_ADD,
    D3D12_BLEND_OP_MIN,
    D3D12_BLEND_OP_MAX,
    D3D12_BLEND_OP_REV_SUBTRACT,
    D3D12_BLEND_OP_ADD,
};

static const char *blend_factor_name(unsigned int factor)
{
    switch (factor) {
    case NV_PGRAPH_BLEND_SFACTOR_ZERO:
        return "zero";
    case NV_PGRAPH_BLEND_SFACTOR_ONE:
        return "one";
    case NV_PGRAPH_BLEND_SFACTOR_SRC_COLOR:
        return "src-color";
    case NV_PGRAPH_BLEND_SFACTOR_ONE_MINUS_SRC_COLOR:
        return "one-minus-src-color";
    case NV_PGRAPH_BLEND_SFACTOR_SRC_ALPHA:
        return "src-alpha";
    case NV_PGRAPH_BLEND_SFACTOR_ONE_MINUS_SRC_ALPHA:
        return "one-minus-src-alpha";
    case NV_PGRAPH_BLEND_SFACTOR_DST_ALPHA:
        return "dst-alpha";
    case NV_PGRAPH_BLEND_SFACTOR_ONE_MINUS_DST_ALPHA:
        return "one-minus-dst-alpha";
    case NV_PGRAPH_BLEND_SFACTOR_DST_COLOR:
        return "dst-color";
    case NV_PGRAPH_BLEND_SFACTOR_ONE_MINUS_DST_COLOR:
        return "one-minus-dst-color";
    case NV_PGRAPH_BLEND_SFACTOR_SRC_ALPHA_SATURATE:
        return "src-alpha-saturate";
    case NV_PGRAPH_BLEND_SFACTOR_CONSTANT_COLOR:
        return "constant-color";
    case NV_PGRAPH_BLEND_SFACTOR_ONE_MINUS_CONSTANT_COLOR:
        return "one-minus-constant-color";
    case NV_PGRAPH_BLEND_SFACTOR_CONSTANT_ALPHA:
        return "constant-alpha";
    case NV_PGRAPH_BLEND_SFACTOR_ONE_MINUS_CONSTANT_ALPHA:
        return "one-minus-constant-alpha";
    default:
        return "unknown";
    }
}

static const char *logic_op_name(unsigned int logic_op)
{
    switch (logic_op) {
    case 0:
        return "clear";
    case 1:
        return "and";
    case 2:
        return "and-reverse";
    case 3:
        return "copy";
    case 4:
        return "and-inverted";
    case 5:
        return "noop";
    case 6:
        return "xor";
    case 7:
        return "or";
    case 8:
        return "nor";
    case 9:
        return "equiv";
    case 10:
        return "invert";
    case 11:
        return "or-reverse";
    case 12:
        return "copy-inverted";
    case 13:
        return "or-inverted";
    case 14:
        return "nand";
    case 15:
        return "set";
    default:
        return "unknown";
    }
}

static const char *blend_equation_name(unsigned int equation)
{
    switch (equation) {
    case 0:
        return "func-subtract";
    case 1:
        return "func-reverse-subtract";
    case 2:
        return "func-add";
    case 3:
        return "min";
    case 4:
        return "max";
    case 5:
        return "func-reverse-subtract-signed";
    case 6:
        return "func-add-signed";
    default:
        return "unknown";
    }
}

static const char *alpha_func_name(unsigned int func)
{
    switch (func) {
    case 0:
        return "never";
    case 1:
        return "less";
    case 2:
        return "equal";
    case 3:
        return "less-or-equal";
    case 4:
        return "greater";
    case 5:
        return "not-equal";
    case 6:
        return "greater-or-equal";
    case 7:
        return "always";
    default:
        return "unknown";
    }
}

static const char *cull_face_name(unsigned int face)
{
    switch (face) {
    case 0:
        return "none";
    case NV_PGRAPH_SETUPRASTER_CULLCTRL_FRONT:
        return "front";
    case NV_PGRAPH_SETUPRASTER_CULLCTRL_BACK:
        return "back";
    case NV_PGRAPH_SETUPRASTER_CULLCTRL_FRONT_AND_BACK:
        return "front-and-back";
    default:
        return "unknown";
    }
}

static const char *texture_shader_mode_name(unsigned int mode)
{
    switch (mode) {
    case PS_TEXTUREMODES_NONE:
        return "none";
    case PS_TEXTUREMODES_PROJECT2D:
        return "project2d";
    case PS_TEXTUREMODES_PROJECT3D:
        return "project3d";
    case PS_TEXTUREMODES_CUBEMAP:
        return "cubemap";
    case PS_TEXTUREMODES_PASSTHRU:
        return "passthru";
    case PS_TEXTUREMODES_CLIPPLANE:
        return "clipplane";
    case PS_TEXTUREMODES_BUMPENVMAP:
        return "bumpenvmap";
    case PS_TEXTUREMODES_BUMPENVMAP_LUM:
        return "bumpenvmap_lum";
    case PS_TEXTUREMODES_BRDF:
        return "brdf";
    case PS_TEXTUREMODES_DOT_ST:
        return "dot_st";
    case PS_TEXTUREMODES_DOT_ZW:
        return "dot_zw";
    case PS_TEXTUREMODES_DOT_RFLCT_DIFF:
        return "dot_rflct_diff";
    case PS_TEXTUREMODES_DOT_RFLCT_SPEC:
        return "dot_rflct_spec";
    case PS_TEXTUREMODES_DOT_STR_3D:
        return "dot_str_3d";
    case PS_TEXTUREMODES_DOT_STR_CUBE:
        return "dot_str_cube";
    case PS_TEXTUREMODES_DPNDNT_AR:
        return "dpndnt_ar";
    case PS_TEXTUREMODES_DPNDNT_GB:
        return "dpndnt_gb";
    case PS_TEXTUREMODES_DOTPRODUCT:
        return "dotproduct";
    case PS_TEXTUREMODES_DOT_RFLCT_SPEC_CONST:
        return "dot_rflct_spec_const";
    default:
        return "reserved";
    }
}

static bool state_key_already_reported(uint32_t *keys, uint32_t *num_keys,
                                       uint32_t key)
{
    for (uint32_t i = 0; i < *num_keys; i++) {
        if (keys[i] == key) {
            return true;
        }
    }

    if (*num_keys < PGRAPH_D3D12_REPORTED_STATE_KEYS) {
        keys[(*num_keys)++] = key;
        return false;
    }

    return true;
}

static uint8_t get_color_write_mask(PGRAPHState *pg);

static void report_unsupported_blend_state(PGRAPHState *pg, uint32_t key,
                                           const char *reason)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    uint32_t blend = pgraph_reg_r(pg, NV_PGRAPH_BLEND);
    uint32_t sfactor = GET_MASK(blend, NV_PGRAPH_BLEND_SFACTOR);
    uint32_t dfactor = GET_MASK(blend, NV_PGRAPH_BLEND_DFACTOR);
    uint32_t equation = GET_MASK(blend, NV_PGRAPH_BLEND_EQN);
    uint32_t logic_op = GET_MASK(blend, NV_PGRAPH_BLEND_LOGICOP);

    if (state_key_already_reported(
            r->reported_unsupported_blend_state_keys,
            &r->num_reported_unsupported_blend_state_keys, key)) {
        return;
    }

    fprintf(stderr,
            "Direct3D 12 renderer: draw skipped: unsupported NV2A blend "
            "state; blend=0x%08x, blend_enable=%d, logic_op_enable=%d, "
            "logic_op=%s (%u), src_factor=%s (%u), dst_factor=%s (%u), "
            "equation=%s (%u), blend_color=0x%08x, "
            "output_merger_logic_op=%d, rtv_bound=%d, rtv_dxgi=%u, "
            "color_format=0x%x, color_addr=0x%" HWADDR_PRIx
            ", color_pitch=%u, color_size=%ux%u, color_bpp=%u. %s\n",
            blend, !!(blend & NV_PGRAPH_BLEND_EN),
            !!(blend & NV_PGRAPH_BLEND_LOGICOP_ENABLE),
            logic_op_name(logic_op), logic_op, blend_factor_name(sfactor),
            sfactor, blend_factor_name(dfactor), dfactor,
            blend_equation_name(equation), equation,
            pgraph_reg_r(pg, NV_PGRAPH_BLENDCOLOR),
            r->output_merger_logic_op, r->color_binding != NULL,
            r->color_binding != NULL ?
                (unsigned int)r->color_binding->fmt.dxgi_format : 0,
            pg->surface_shape.color_format, (hwaddr)pg->surface_color.offset,
            pg->surface_color.pitch,
            r->color_binding != NULL ? r->color_binding->width : 0,
            r->color_binding != NULL ? r->color_binding->height : 0,
            r->color_binding != NULL ?
                r->color_binding->fmt.bytes_per_pixel : 0,
            reason);
}

static void report_unsupported_raster_state(PGRAPHState *pg, uint32_t key,
                                            const char *reason)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    uint32_t setup = pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER);
    uint32_t front_mode =
        GET_MASK(setup, NV_PGRAPH_SETUPRASTER_FRONTFACEMODE);
    uint32_t back_mode =
        GET_MASK(setup, NV_PGRAPH_SETUPRASTER_BACKFACEMODE);
    uint32_t cull_face = GET_MASK(setup, NV_PGRAPH_SETUPRASTER_CULLCTRL);

    if (state_key_already_reported(
            r->reported_unsupported_raster_state_keys,
            &r->num_reported_unsupported_raster_state_keys, key)) {
        return;
    }

    fprintf(stderr,
            "Direct3D 12 renderer: draw skipped: unsupported NV2A raster "
            "state; primitive=%s (%u), setup_raster=0x%08x, "
            "front_polygon=%s (%u), back_polygon=%s (%u), "
            "cull_enable=%d, cull_face=%s (%u), front_face_ccw=%d. %s\n",
            primitive_mode_name(pg->primitive_mode), pg->primitive_mode,
            setup, polygon_mode_name(front_mode), front_mode,
            polygon_mode_name(back_mode), back_mode,
            !!(setup & NV_PGRAPH_SETUPRASTER_CULLENABLE),
            cull_face_name(cull_face), cull_face,
            !!(setup & NV_PGRAPH_SETUPRASTER_FRONTFACE), reason);
}

static void report_unsupported_depth_stencil_state(PGRAPHState *pg,
                                                  uint32_t key,
                                                  const char *reason)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    uint32_t control_0 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0);
    uint32_t control_1 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1);
    uint32_t control_2 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_2);
    uint32_t depth_func = GET_MASK(control_0, NV_PGRAPH_CONTROL_0_ZFUNC);
    uint32_t stencil_func =
        GET_MASK(control_1, NV_PGRAPH_CONTROL_1_STENCIL_FUNC);
    uint32_t op_fail =
        GET_MASK(control_2, NV_PGRAPH_CONTROL_2_STENCIL_OP_FAIL);
    uint32_t op_zfail =
        GET_MASK(control_2, NV_PGRAPH_CONTROL_2_STENCIL_OP_ZFAIL);
    uint32_t op_zpass =
        GET_MASK(control_2, NV_PGRAPH_CONTROL_2_STENCIL_OP_ZPASS);
    bool stencil_func_mapped =
        stencil_func < ARRAY_SIZE(d3d12_compare_func_map);
    bool op_fail_mapped =
        op_fail < ARRAY_SIZE(d3d12_stencil_op_map) &&
        d3d12_stencil_op_map[op_fail] != 0;
    bool op_zfail_mapped =
        op_zfail < ARRAY_SIZE(d3d12_stencil_op_map) &&
        d3d12_stencil_op_map[op_zfail] != 0;
    bool op_zpass_mapped =
        op_zpass < ARRAY_SIZE(d3d12_stencil_op_map) &&
        d3d12_stencil_op_map[op_zpass] != 0;

    if (state_key_already_reported(
            r->reported_unsupported_depth_stencil_state_keys,
            &r->num_reported_unsupported_depth_stencil_state_keys, key)) {
        return;
    }

    fprintf(stderr,
            "Direct3D 12 renderer: draw skipped: unsupported NV2A "
            "depth/stencil state; control_0=0x%08x, control_1=0x%08x, "
            "control_2=0x%08x, depth_enable=%d, depth_write=%d, "
            "depth_func=%s (%u), depth_func_mapped=%d, "
            "stencil_enable=%d, stencil_func=%s (%u), "
            "stencil_func_mapped=%d, stencil_ref=%u, "
            "stencil_read_mask=0x%02x, stencil_write_mask=0x%02x, "
            "ops={fail:%u mapped:%d,zfail:%u mapped:%d,"
            "zpass:%u mapped:%d}, zeta_bound=%d, zeta_surface_format=%u, "
            "zeta_dxgi=%u, zeta_offset=0x%" HWADDR_PRIx ", "
            "zeta_pitch=%u. %s\n",
            control_0, control_1, control_2,
            !!(control_0 & NV_PGRAPH_CONTROL_0_ZENABLE),
            !!(control_0 & NV_PGRAPH_CONTROL_0_ZWRITEENABLE),
            alpha_func_name(depth_func), depth_func,
            depth_func < ARRAY_SIZE(d3d12_compare_func_map),
            !!(control_1 & NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE),
            alpha_func_name(stencil_func), stencil_func, stencil_func_mapped,
            GET_MASK(control_1, NV_PGRAPH_CONTROL_1_STENCIL_REF),
            GET_MASK(control_1, NV_PGRAPH_CONTROL_1_STENCIL_MASK_READ),
            GET_MASK(control_1, NV_PGRAPH_CONTROL_1_STENCIL_MASK_WRITE),
            op_fail, op_fail_mapped, op_zfail, op_zfail_mapped, op_zpass,
            op_zpass_mapped, r->zeta_binding != NULL,
            pg->surface_shape.zeta_format,
            r->zeta_binding != NULL ?
                (unsigned int)r->zeta_binding->fmt.dxgi_format : 0,
            (hwaddr)pg->surface_zeta.offset, pg->surface_zeta.pitch, reason);
}

static void report_unsupported_alpha_state(PGRAPHState *pg, uint32_t key,
                                           const char *reason)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    uint32_t control_0 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0);
    uint32_t alpha_func = GET_MASK(control_0, NV_PGRAPH_CONTROL_0_ALPHAFUNC);
    uint32_t alpha_ref = GET_MASK(control_0, NV_PGRAPH_CONTROL_0_ALPHAREF);

    if (state_key_already_reported(
            r->reported_unsupported_alpha_state_keys,
            &r->num_reported_unsupported_alpha_state_keys, key)) {
        return;
    }

    fprintf(stderr,
            "Direct3D 12 renderer: draw skipped: unsupported NV2A alpha "
            "test state; control_0=0x%08x, alpha_test_enable=%d, "
            "alpha_func=%s (%u), alpha_ref=%u, color_write_mask=0x%x, "
            "primitive=%s (%u), rtv_bound=%d. %s\n",
            control_0,
            !!(control_0 & NV_PGRAPH_CONTROL_0_ALPHATESTENABLE),
            alpha_func_name(alpha_func), alpha_func, alpha_ref,
            get_color_write_mask(pg), primitive_mode_name(pg->primitive_mode),
            pg->primitive_mode, r->color_binding != NULL, reason);
}

static bool blend_factor_uses_constant_color(uint32_t factor)
{
    return factor == NV_PGRAPH_BLEND_SFACTOR_CONSTANT_COLOR ||
           factor == NV_PGRAPH_BLEND_SFACTOR_ONE_MINUS_CONSTANT_COLOR;
}

static bool blend_factor_uses_constant_alpha(uint32_t factor)
{
    return factor == NV_PGRAPH_BLEND_SFACTOR_CONSTANT_ALPHA ||
           factor == NV_PGRAPH_BLEND_SFACTOR_ONE_MINUS_CONSTANT_ALPHA;
}

static PGRAPHD3D12BlendConstantMode blend_constant_mode_for_factor(
    uint32_t factor)
{
    if (blend_factor_uses_constant_color(factor)) {
        return PGRAPH_D3D12_BLEND_CONSTANT_RGBA;
    }
    if (blend_factor_uses_constant_alpha(factor)) {
        return PGRAPH_D3D12_BLEND_CONSTANT_ALPHA;
    }
    return PGRAPH_D3D12_BLEND_CONSTANT_NONE;
}

static bool blend_factor_is_constant(uint32_t factor)
{
    return blend_constant_mode_for_factor(factor) !=
           PGRAPH_D3D12_BLEND_CONSTANT_NONE;
}

static bool alpha_blend_factor_supported(uint32_t factor)
{
    return factor < ARRAY_SIZE(d3d12_alpha_blend_factor_map) &&
           d3d12_alpha_blend_factor_map[factor] != 0;
}

static bool polygon_primitive(unsigned int primitive_mode)
{
    return primitive_mode >= PRIM_TYPE_TRIANGLES &&
           primitive_mode <= PRIM_TYPE_POLYGON;
}

/* Polygon mode of the draw's primitives; points and lines have none. */
static uint8_t get_polygon_mode(PGRAPHState *pg)
{
    if (!polygon_primitive(pg->primitive_mode)) {
        return POLY_MODE_FILL;
    }
    return GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER),
                    NV_PGRAPH_SETUPRASTER_FRONTFACEMODE);
}

static bool get_primitive_topology_type(
    PGRAPHState *pg, D3D12_PRIMITIVE_TOPOLOGY_TYPE *type)
{
    unsigned int polygon_front_mode;

    switch (pg->primitive_mode) {
    case PRIM_TYPE_POINTS:
        *type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
        return true;
    case PRIM_TYPE_LINES:
    case PRIM_TYPE_LINE_LOOP:
    case PRIM_TYPE_LINE_STRIP:
        *type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        return true;
    case PRIM_TYPE_TRIANGLES:
    case PRIM_TYPE_TRIANGLE_STRIP:
    case PRIM_TYPE_TRIANGLE_FAN:
        /* Line and point polygon modes are drawn by the geometry shader
         * from the assembled triangles. */
        *type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        return true;
    case PRIM_TYPE_QUADS:
    case PRIM_TYPE_QUAD_STRIP:
        /* Four-vertex quads (line list with adjacency) when the geometry
         * shader needs vertex 3 or the quad outline; see
         * quad_adjacency_primitive() in draw.c. */
        polygon_front_mode = GET_MASK(
            pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER),
            NV_PGRAPH_SETUPRASTER_FRONTFACEMODE);
        if (polygon_front_mode == POLY_MODE_FILL &&
            GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_3),
                     NV_PGRAPH_CONTROL_3_SHADEMODE) ==
                NV_PGRAPH_CONTROL_3_SHADEMODE_SMOOTH) {
            *type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            return true;
        }
        *type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        return true;
    case PRIM_TYPE_POLYGON:
        polygon_front_mode = GET_MASK(
            pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER),
            NV_PGRAPH_SETUPRASTER_FRONTFACEMODE);
        if (polygon_front_mode == POLY_MODE_FILL) {
            *type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            return true;
        }
        /* Point mode draws each vertex paired with vertex 0. */
        if (polygon_front_mode == POLY_MODE_LINE ||
            polygon_front_mode == POLY_MODE_POINT) {
            *type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
            return true;
        }
        *type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED;
        return false;
    default:
        *type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED;
        return false;
    }
}

static uint8_t get_color_write_mask(PGRAPHState *pg)
{
    uint32_t control_0 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0);
    uint8_t mask = 0;

    if (control_0 & NV_PGRAPH_CONTROL_0_RED_WRITE_ENABLE) {
        mask |= D3D12_COLOR_WRITE_ENABLE_RED;
    }
    if (control_0 & NV_PGRAPH_CONTROL_0_GREEN_WRITE_ENABLE) {
        mask |= D3D12_COLOR_WRITE_ENABLE_GREEN;
    }
    if (control_0 & NV_PGRAPH_CONTROL_0_BLUE_WRITE_ENABLE) {
        mask |= D3D12_COLOR_WRITE_ENABLE_BLUE;
    }
    if (control_0 & NV_PGRAPH_CONTROL_0_ALPHA_WRITE_ENABLE) {
        mask |= D3D12_COLOR_WRITE_ENABLE_ALPHA;
    }

    return mask;
}

static bool fill_depth_stencil_key(PGRAPHState *pg,
                                   PGRAPHD3D12PipelineKey *key)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    uint32_t control_0 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0);
    uint32_t control_1 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1);
    uint32_t control_2 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_2);
    bool depth_requested = control_0 & NV_PGRAPH_CONTROL_0_ZENABLE;
    bool depth_write = control_0 & NV_PGRAPH_CONTROL_0_ZWRITEENABLE;
    bool stencil_requested =
        control_1 & NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE;
    uint32_t depth_func = GET_MASK(control_0, NV_PGRAPH_CONTROL_0_ZFUNC);
    uint32_t stencil_func =
        GET_MASK(control_1, NV_PGRAPH_CONTROL_1_STENCIL_FUNC);
    uint32_t op_fail =
        GET_MASK(control_2, NV_PGRAPH_CONTROL_2_STENCIL_OP_FAIL);
    uint32_t op_zfail =
        GET_MASK(control_2, NV_PGRAPH_CONTROL_2_STENCIL_OP_ZFAIL);
    uint32_t op_zpass =
        GET_MASK(control_2, NV_PGRAPH_CONTROL_2_STENCIL_OP_ZPASS);
    unsigned int z_format =
        GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER),
                 NV_PGRAPH_SETUPRASTER_Z_FORMAT);

    key->dsv_format = r->zeta_binding != NULL ?
        r->zeta_binding->fmt.dxgi_format : DXGI_FORMAT_UNKNOWN;
    key->z_perspective =
        !!(control_0 & NV_PGRAPH_CONTROL_0_Z_PERSPECTIVE_ENABLE);
    key->depth_clipping =
        GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_ZCOMPRESSOCCLUDE),
                 NV_PGRAPH_ZCOMPRESSOCCLUDE_ZCLAMP_EN) ==
        NV_PGRAPH_ZCOMPRESSOCCLUDE_ZCLAMP_EN_CULL &&
        !(r->tune_flags & PGRAPH_D3D12_TUNE_ZCLIP_CLAMP);
    switch (pg->surface_shape.zeta_format) {
    case NV097_SET_SURFACE_FORMAT_ZETA_Z16:
        key->depth_format = z_format ?
            PGRAPH_D3D12_DEPTH_FORMAT_F16 :
            PGRAPH_D3D12_DEPTH_FORMAT_D16;
        break;
    case NV097_SET_SURFACE_FORMAT_ZETA_Z24S8:
    default:
        key->depth_format = z_format ?
            PGRAPH_D3D12_DEPTH_FORMAT_F24 :
            PGRAPH_D3D12_DEPTH_FORMAT_D24;
        break;
    }

    if (r->zeta_binding == NULL) {
        if (depth_requested || stencil_requested) {
            pgraph_d3d12_warn_once(
                pg, PGRAPH_D3D12_DIAG_MISSING_ZETA_FOR_DEPTH,
                "depth/stencil requested without a bound zeta surface; "
                "depth_enable=%d, depth_write=%d, depth_func=%u, "
                "stencil_enable=%d, stencil_func=%u, stencil_ref=%u, "
                "stencil_read_mask=0x%02x, stencil_write_mask=0x%02x, "
                "ops={fail:%u,zfail:%u,zpass:%u}, zeta_format=%u, "
                "z_format=%u, depth_format=%s, z_perspective=%d, "
                "depth_clipping=%d, zeta_offset=0x%" HWADDR_PRIx
                ", zeta_pitch=%u. "
                "The D3D12 PSO will draw with depth/stencil disabled.",
                depth_requested, depth_write, depth_func, stencil_requested,
                stencil_func,
                GET_MASK(control_1, NV_PGRAPH_CONTROL_1_STENCIL_REF),
                GET_MASK(control_1, NV_PGRAPH_CONTROL_1_STENCIL_MASK_READ),
                GET_MASK(control_1, NV_PGRAPH_CONTROL_1_STENCIL_MASK_WRITE),
                op_fail, op_zfail, op_zpass, pg->surface_shape.zeta_format,
                z_format, depth_format_name(key->depth_format),
                key->z_perspective, key->depth_clipping,
                (hwaddr)pg->surface_zeta.offset, pg->surface_zeta.pitch);
        }
        return true;
    }

    /* With the depth test disabled the NV2A does not write depth either. */
    key->depth_enable = depth_requested;
    key->depth_write = depth_requested && depth_write;
    key->depth_func = D3D12_COMPARISON_FUNC_ALWAYS;
    if (depth_requested) {
        if (depth_func >= ARRAY_SIZE(d3d12_compare_func_map)) {
            report_unsupported_depth_stencil_state(
                pg, 0x10000u | depth_func,
                "The NV2A depth compare function is outside the current "
                "D3D12 compare-function map.");
            return false;
        }
        key->depth_func = d3d12_compare_func_map[depth_func];
    }

    if (stencil_requested && !r->zeta_binding->fmt.stencil) {
        pgraph_d3d12_warn_once(
            pg, PGRAPH_D3D12_DIAG_STENCIL_WITHOUT_STENCIL_SURFACE,
            "stencil test requested on a zeta surface without stencil bits; "
            "control_1=0x%08x, control_2=0x%08x, zeta_surface_format=%u, "
            "zeta_dxgi=%u. The D3D12 PSO will use depth only.",
            control_1, control_2, pg->surface_shape.zeta_format,
            (unsigned int)key->dsv_format);
        stencil_requested = false;
    }

    key->stencil_enable = stencil_requested;
    if (!stencil_requested) {
        return true;
    }

    if (stencil_func >= ARRAY_SIZE(d3d12_compare_func_map) ||
        op_fail >= ARRAY_SIZE(d3d12_stencil_op_map) ||
        op_zfail >= ARRAY_SIZE(d3d12_stencil_op_map) ||
        op_zpass >= ARRAY_SIZE(d3d12_stencil_op_map) ||
        d3d12_stencil_op_map[op_fail] == 0 ||
        d3d12_stencil_op_map[op_zfail] == 0 ||
        d3d12_stencil_op_map[op_zpass] == 0) {
        report_unsupported_depth_stencil_state(
            pg, 0x20000u | (stencil_func << 12) | (op_fail << 8) |
                    (op_zfail << 4) | op_zpass,
            "The stencil compare function or one of the stencil operations "
            "is outside the current D3D12 stencil-state maps.");
        return false;
    }

    key->stencil_func = d3d12_compare_func_map[stencil_func];
    key->stencil_read_mask =
        GET_MASK(control_1, NV_PGRAPH_CONTROL_1_STENCIL_MASK_READ);
    key->stencil_write_mask =
        GET_MASK(control_1, NV_PGRAPH_CONTROL_1_STENCIL_MASK_WRITE);
    key->stencil_fail_op = d3d12_stencil_op_map[op_fail];
    key->stencil_depth_fail_op = d3d12_stencil_op_map[op_zfail];
    key->stencil_pass_op = d3d12_stencil_op_map[op_zpass];
    return true;
}

static bool fill_blend_key(PGRAPHState *pg, PGRAPHD3D12PipelineKey *key)
{
    uint32_t blend = pgraph_reg_r(pg, NV_PGRAPH_BLEND);
    uint32_t sfactor = GET_MASK(blend, NV_PGRAPH_BLEND_SFACTOR);
    uint32_t dfactor = GET_MASK(blend, NV_PGRAPH_BLEND_DFACTOR);
    uint32_t equation = GET_MASK(blend, NV_PGRAPH_BLEND_EQN);
    bool constant_color;
    bool constant_alpha;
    bool signed_rgb = false;

    key->blend_enable = false;
    key->logic_op_enable = false;
    key->logic_op = D3D12_LOGIC_OP_NOOP;
    key->shader_framebuffer_fetch = false;
    key->shader_blend_enable = false;
    key->shader_logic_op_enable = false;
    key->shader_blend_src_factor = NV_PGRAPH_BLEND_SFACTOR_ONE;
    key->shader_blend_dest_factor = NV_PGRAPH_BLEND_DFACTOR_ZERO;
    key->shader_blend_equation = 2;
    key->shader_logic_op = 3;
    key->src_blend = D3D12_BLEND_ONE;
    key->dest_blend = D3D12_BLEND_ZERO;
    key->blend_op = D3D12_BLEND_OP_ADD;
    key->src_blend_alpha = D3D12_BLEND_ONE;
    key->dest_blend_alpha = D3D12_BLEND_ZERO;
    key->blend_op_alpha = D3D12_BLEND_OP_ADD;
    key->blend_constant_mode = PGRAPH_D3D12_BLEND_CONSTANT_NONE;
    key->shader_src_blend_factor_enable = false;
    key->shader_src_blend_factor = NV_PGRAPH_BLEND_SFACTOR_ONE;

    /*
     * NV_PGRAPH_BLEND_LOGICOP is ignored like the GL and Vulkan renderers do.
     * Titles leave it enabled with LOGICOP_COPY; honouring it would force a
     * dest-read path with per-draw render-target copies.
     */

    if (!(blend & NV_PGRAPH_BLEND_EN)) {
        return true;
    }

    if (equation >= ARRAY_SIZE(d3d12_blend_equation_map)) {
        report_unsupported_blend_state(
            pg, 0x40000u | equation,
            "The blend equation is outside the NV2A equation values handled "
            "by the GL/Vulkan renderers.");
        return false;
    }
    /* ADD_SIGNED / REVERSE_SUBTRACT_SIGNED as the GL and Vulkan renderers
     * (texsigns.c): swapped, RGB factors ONE_MINUS_SRC_COLOR / ONE. */
    equation = texsigns_blend_equation(equation, &signed_rgb);
    key->blend_op = d3d12_blend_equation_map[equation];
    key->blend_op_alpha = key->blend_op;
    if (key->blend_op == D3D12_BLEND_OP_MIN ||
        key->blend_op == D3D12_BLEND_OP_MAX) {
        /* MIN and MAX ignore both blend factors. */
        key->blend_enable = true;
        return true;
    }

    if (sfactor >= ARRAY_SIZE(d3d12_blend_factor_map) ||
        d3d12_blend_factor_map[sfactor] == 0) {
        report_unsupported_blend_state(
            pg, 0x20000u | sfactor,
            "The source blend factor is not representable by the current "
            "D3D12 blend-factor map.");
        return false;
    }
    if (dfactor >= ARRAY_SIZE(d3d12_blend_factor_map) ||
        d3d12_blend_factor_map[dfactor] == 0) {
        report_unsupported_blend_state(
            pg, 0x30000u | dfactor,
            "The destination blend factor is not representable by the "
            "current D3D12 blend-factor map.");
        return false;
    }
    if (!alpha_blend_factor_supported(sfactor)) {
        report_unsupported_blend_state(
            pg, 0x31000u | sfactor,
            "The source blend factor is not representable by the current "
            "D3D12 alpha blend-factor map.");
        return false;
    }
    if (!alpha_blend_factor_supported(dfactor)) {
        report_unsupported_blend_state(
            pg, 0x32000u | dfactor,
            "The destination blend factor is not representable by the current "
            "D3D12 alpha blend-factor map.");
        return false;
    }

    constant_color = blend_factor_uses_constant_color(sfactor) ||
                     blend_factor_uses_constant_color(dfactor);
    constant_alpha = blend_factor_uses_constant_alpha(sfactor) ||
                     blend_factor_uses_constant_alpha(dfactor);
    if (constant_color && constant_alpha) {
        if (!blend_factor_is_constant(sfactor) ||
            !blend_factor_is_constant(dfactor)) {
            report_unsupported_blend_state(
                pg, 0x50000u | (sfactor << 4) | dfactor,
                "This draw mixes constant-color and constant-alpha blend "
                "factors, but the source side is not a constant factor that "
                "can be baked into the SM 6.4 pixel shader.");
            return false;
        }

        key->shader_src_blend_factor_enable = true;
        key->shader_src_blend_factor = sfactor;
        key->blend_constant_mode = blend_constant_mode_for_factor(dfactor);
    } else if (constant_color) {
        key->blend_constant_mode = PGRAPH_D3D12_BLEND_CONSTANT_RGBA;
    } else if (constant_alpha) {
        key->blend_constant_mode = PGRAPH_D3D12_BLEND_CONSTANT_ALPHA;
    }

    key->blend_enable = true;
    key->src_blend = key->shader_src_blend_factor_enable ?
        D3D12_BLEND_ONE : d3d12_blend_factor_map[sfactor];
    key->dest_blend = d3d12_blend_factor_map[dfactor];
    key->src_blend_alpha = key->shader_src_blend_factor_enable ?
        D3D12_BLEND_ONE : d3d12_alpha_blend_factor_map[sfactor];
    key->dest_blend_alpha = d3d12_alpha_blend_factor_map[dfactor];
    if (signed_rgb && !key->shader_src_blend_factor_enable) {
        key->src_blend = D3D12_BLEND_INV_SRC_COLOR;
        key->dest_blend = D3D12_BLEND_ONE;
    }

    return true;
}

/* The alpha test function, ALWAYS (7) while the test is off. Returns false
 * for a function outside the compare map. */
static bool get_alpha_test_func(PGRAPHState *pg, uint32_t *func)
{
    uint32_t control_0 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0);

    *func = 7;
    if (!(control_0 & NV_PGRAPH_CONTROL_0_ALPHATESTENABLE)) {
        return true;
    }
    *func = GET_MASK(control_0, NV_PGRAPH_CONTROL_0_ALPHAFUNC);
    return *func < ARRAY_SIZE(d3d12_compare_func_map);
}

/* Stages with alpha kill enabled among the bound textures. */
static uint8_t get_alphakill_mask(PGRAPHState *pg)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    uint8_t mask = 0;

    for (unsigned int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        if ((r->texture_active_mask & (1u << i)) &&
            (pgraph_reg_r(pg, NV_PGRAPH_TEXCTL0_0 + i * 4) &
             NV_PGRAPH_TEXCTL0_0_ALPHAKILLEN)) {
            mask |= 1u << i;
        }
    }

    return mask;
}

static bool fill_alpha_test_key(PGRAPHState *pg,
                                PGRAPHD3D12PipelineKey *key)
{
    uint32_t alpha_func;

    key->alpha_test_enable = false;
    key->alpha_func = 7;
    key->alpha_ref = 0;

    if (!get_alpha_test_func(pg, &alpha_func)) {
        report_unsupported_alpha_state(
            pg, 0x10000u | alpha_func,
            "The NV2A alpha test function is outside the current D3D12 "
            "compare-function map.");
        return false;
    }

    key->alpha_func = alpha_func;
    /* The function and reference come from pixel constants. */
    key->alpha_test_enable = alpha_func != 7;
    return true;
}

static bool polygon_offset_enabled_for_mode(PGRAPHState *pg, uint32_t setup,
                                            uint32_t front_mode)
{
    if (pg->primitive_mode < PRIM_TYPE_TRIANGLES) {
        return false;
    }

    return (front_mode == POLY_MODE_FILL &&
            (setup & NV_PGRAPH_SETUPRASTER_POFFSETFILLENABLE)) ||
           (front_mode == POLY_MODE_LINE &&
            (setup & NV_PGRAPH_SETUPRASTER_POFFSETLINEENABLE)) ||
           (front_mode == POLY_MODE_POINT &&
            (setup & NV_PGRAPH_SETUPRASTER_POFFSETPOINTENABLE));
}

static bool fill_rasterizer_key(PGRAPHState *pg,
                                PGRAPHD3D12PipelineKey *key)
{
    uint32_t setup = pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER);
    uint32_t front_mode =
        GET_MASK(setup, NV_PGRAPH_SETUPRASTER_FRONTFACEMODE);
    uint32_t cull_face = GET_MASK(setup, NV_PGRAPH_SETUPRASTER_CULLCTRL);

    /* Line and point polygon modes are drawn by the geometry shader, so the
     * rasterizer always fills. Polygon offset is applied per fragment with
     * the rest of the depth computation; see fill_depth_info(). */
    key->cull_mode = D3D12_CULL_MODE_NONE;
    key->front_counter_clockwise =
        !!(setup & NV_PGRAPH_SETUPRASTER_FRONTFACE);

    if (front_mode != POLY_MODE_FILL &&
        front_mode != POLY_MODE_POINT &&
        front_mode != POLY_MODE_LINE) {
        report_unsupported_raster_state(
            pg, 0x10000u | front_mode,
            "The front polygon mode is outside the NV2A fill/point/line "
            "values, so D3D12 cannot select a rasterizer fill mode.");
        return false;
    }

    /* Culling removes filled polygons only; the edges and vertices of line
     * and point polygon modes are drawn whatever their facing. */
    if (!(setup & NV_PGRAPH_SETUPRASTER_CULLENABLE) ||
        !polygon_primitive(pg->primitive_mode) ||
        key->polygon_mode != POLY_MODE_FILL) {
        return true;
    }

    switch (cull_face) {
    case NV_PGRAPH_SETUPRASTER_CULLCTRL_FRONT:
        key->cull_mode = D3D12_CULL_MODE_FRONT;
        return true;
    case NV_PGRAPH_SETUPRASTER_CULLCTRL_BACK:
        key->cull_mode = D3D12_CULL_MODE_BACK;
        return true;
    case NV_PGRAPH_SETUPRASTER_CULLCTRL_FRONT_AND_BACK:
        report_unsupported_raster_state(
            pg, 0x30000u | pg->primitive_mode,
            "NV2A cull front-and-back culls all polygon primitives. The draw "
            "path should drop this draw before PSO creation, so this indicates "
            "the rasterizer key was built for a draw that should already have "
            "returned.");
        return false;
    default:
        report_unsupported_raster_state(
            pg, 0x40000u | cull_face,
            "Cull is enabled but NV_PGRAPH_SETUPRASTER_CULLCTRL is not "
            "front, back, or front-and-back.");
        return false;
    }
}

static char *pipeline_key_attribute_summary(
    const PGRAPHD3D12PipelineKey *key)
{
    GString *summary = g_string_new("");

    for (uint8_t i = 0; i < key->num_attributes; i++) {
        if (i != 0) {
            g_string_append(summary, ", ");
        }
        g_string_append_printf(summary, "attr%u:dxgi=%u",
                               key->attribute_indices[i],
                               (unsigned int)key->attribute_formats[i]);
    }

    if (summary->len == 0) {
        g_string_append(summary, "none");
    }

    return g_string_free(summary, FALSE);
}

static guint pipeline_key_hash(gconstpointer data)
{
    uint64_t hash = fast_hash(data, sizeof(PGRAPHD3D12PipelineKey));

    return (guint)(hash ^ (hash >> 32));
}

static bool shader_dump_signature_seen(uint32_t signature)
{
    /* Pipelines are built on several threads. */
    static GMutex lock;
    static uint32_t signatures[PGRAPH_D3D12_REPORTED_STATE_KEYS];
    static uint32_t num_signatures;
    bool seen = true;

    g_mutex_lock(&lock);
    for (uint32_t i = 0; i < num_signatures; i++) {
        if (signatures[i] == signature) {
            g_mutex_unlock(&lock);
            return true;
        }
    }
    if (num_signatures < ARRAY_SIZE(signatures)) {
        signatures[num_signatures++] = signature;
        seen = false;
    }
    g_mutex_unlock(&lock);
    return seen;
}

static void maybe_dump_shader_sources(PGRAPHState *pg,
                                      const PGRAPHD3D12PipelineKey *key,
                                      const char *vs_source,
                                      const char *gs_source,
                                      const char *ps_source)
{
    const bool text_state =
        (key->texture_mask & 1) &&
        key->texture_width[0] == 100 &&
        key->texture_height[0] == 17;
    const bool surface_copy_state =
        (key->texture_surface_to_texture_mask & 1) &&
        key->texture_width[0] >= 640 &&
        key->texture_height[0] >= 480;
    const bool point_state =
        key->primitive_topology_type == D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    const char *tag = text_state ? "text-100x17" :
        surface_copy_state ? "surface-copy" :
        point_state ? "point" : NULL;
    const char *local_app_data;
    uint32_t state_key;
    uint32_t signature;
    g_autofree char *dump_dir = NULL;
    g_autofree char *vs_name = NULL;
    g_autofree char *gs_name = NULL;
    g_autofree char *ps_name = NULL;
    g_autofree char *vs_path = NULL;
    g_autofree char *gs_path = NULL;
    g_autofree char *ps_path = NULL;
    GError *gerr = NULL;
    bool wrote_vs = false;
    bool wrote_gs = false;
    bool wrote_ps = false;

    if (!pgraph_d3d12_resource_dumps(pg) || tag == NULL) {
        return;
    }

    state_key = pipeline_key_hash(key);
    signature = state_key ^
        (text_state ? 0xa73d1201u :
         surface_copy_state ? 0x05fac0f1u : 0x901d1200u);
    if (shader_dump_signature_seen(signature)) {
        return;
    }

    local_app_data = getenv("LOCALAPPDATA");
    if (local_app_data == NULL || local_app_data[0] == '\0') {
        return;
    }

    dump_dir = g_build_filename(local_app_data, "xemu-d3d12-test",
                                "captures", NULL);
    if (g_mkdir_with_parents(dump_dir, 0755) != 0) {
        fprintf(stderr,
                "Direct3D 12 renderer: shader source dump skipped for "
                "%s state_key=0x%08x because dump directory could not be "
                "created: %s\n",
                tag, state_key, dump_dir);
        return;
    }

    vs_name = g_strdup_printf("d3d12-shader-%s-%08x-vs.hlsl",
                              tag, state_key);
    gs_name = g_strdup_printf("d3d12-shader-%s-%08x-gs.hlsl",
                              tag, state_key);
    ps_name = g_strdup_printf("d3d12-shader-%s-%08x-ps.hlsl",
                              tag, state_key);
    vs_path = g_build_filename(dump_dir, vs_name, NULL);
    gs_path = g_build_filename(dump_dir, gs_name, NULL);
    ps_path = g_build_filename(dump_dir, ps_name, NULL);

    wrote_vs = g_file_set_contents(vs_path, vs_source, -1, &gerr);
    if (gerr != NULL) {
        fprintf(stderr,
                "Direct3D 12 renderer: vertex shader source dump failed "
                "for %s state_key=0x%08x path=%s: %s\n",
                tag, state_key, vs_path, gerr->message);
        g_clear_error(&gerr);
    }
    if (gs_source != NULL) {
        wrote_gs = g_file_set_contents(gs_path, gs_source, -1, &gerr);
        if (gerr != NULL) {
            fprintf(stderr,
                    "Direct3D 12 renderer: geometry shader source dump "
                    "failed for %s state_key=0x%08x path=%s: %s\n",
                    tag, state_key, gs_path, gerr->message);
            g_clear_error(&gerr);
        }
    }
    wrote_ps = g_file_set_contents(ps_path, ps_source, -1, &gerr);
    if (gerr != NULL) {
        fprintf(stderr,
                "Direct3D 12 renderer: pixel shader source dump failed "
                "for %s state_key=0x%08x path=%s: %s\n",
                tag, state_key, ps_path, gerr->message);
        g_clear_error(&gerr);
    }

    fprintf(stderr,
            "Direct3D 12 renderer: dumped SM 6.4 shader sources for %s "
            "state: state_key=0x%08x, primitive=%s, rtv_dxgi=%u, "
            "dsv_dxgi=%u, color_write_mask=0x%x, blend_enable=%d, "
            "logic_op={enable:%d,op:%u}, "
            "blend={const_mode:%u,shader_src_factor_enable:%d,"
            "shader_src_factor:%s(%u)}, "
            "shader_framebuffer_fetch=%d, "
            "shader_blend={enable:%d,src:%s(%u),dst:%s(%u),eq:%s(%u)}, "
            "shader_logic_op={enable:%d,op:%s(%u)}, "
            "depth={enable:%d,write:%d,func:%u,format:%s(%u),"
            "z_perspective:%d,clip:%d}, stencil_enable=%d, texture_mask=0x%x, "
            "texture_surface_to_texture_mask=0x%x, texture_3d_mask=0x%x, "
            "texture_cubemap_mask=0x%x, texture_border_adjust_mask=0x%x, "
            "tex0={format=0x%x, "
            "dxgi=%u, size=%ux%ux%u, pitch=%u, bpp=%u, linear=%d, "
            "texture3d=%d, border_adjust=%d, convolution=%s(%u), "
            "coord_norm=%ux%u}, "
            "shaderprog=0x%08x, "
            "shaderctl=0x%08x, window_clip_exclusive=%d, "
            "combinectl=0x%08x, final0=0x%08x, final1=0x%08x, "
            "fog={enable:%d,mode:%u,foggen:%u}, "
            "point_params={enable:%d,point_sprite:%d,raw_size:%f,scaled_size:%f,"
            "params=[%f,%f,%f,%f,%f,%f,%f,%f]}, "
            "fixed_function={lighting:%d,local_eye:%d,"
            "specular_enable:%d,separate_specular:%d,"
            "ignore_specular_alpha:%d,material_src={emission:%u,"
            "ambient:%u,diffuse:%u,specular:%u},"
            "lights=[%u,%u,%u,%u,%u,%u,%u,%u]}, "
            "stage0={rgb_in=0x%08x, rgb_out=0x%08x, alpha_in=0x%08x, "
            "alpha_out=0x%08x}, vs_dump=%s%s, gs_dump=%s%s, "
            "ps_dump=%s%s\n",
            tag, state_key,
            primitive_topology_type_name(key->primitive_topology_type),
            (unsigned int)key->rtv_format, (unsigned int)key->dsv_format,
            key->color_write_mask, key->blend_enable, key->logic_op_enable,
            key->logic_op, key->blend_constant_mode,
            key->shader_src_blend_factor_enable,
            blend_factor_name(key->shader_src_blend_factor),
            key->shader_src_blend_factor, key->shader_framebuffer_fetch,
            key->shader_blend_enable,
            blend_factor_name(key->shader_blend_src_factor),
            key->shader_blend_src_factor,
            blend_factor_name(key->shader_blend_dest_factor),
            key->shader_blend_dest_factor,
            blend_equation_name(key->shader_blend_equation),
            key->shader_blend_equation, key->shader_logic_op_enable,
            logic_op_name(key->shader_logic_op), key->shader_logic_op,
            key->depth_enable, key->depth_write, key->depth_func,
            depth_format_name(key->depth_format), key->depth_format,
            key->z_perspective, key->depth_clipping, key->stencil_enable,
            key->texture_mask,
            key->texture_surface_to_texture_mask, key->texture_3d_mask,
            key->texture_cubemap_mask, key->texture_border_adjust_mask,
            key->texture_color_format[0],
            (unsigned int)key->texture_dxgi_format[0],
            key->texture_width[0], key->texture_height[0],
            key->texture_depth[0],
            key->texture_pitch[0], key->texture_bytes_per_pixel[0],
            !!(key->texture_linear_mask & 1),
            !!(key->texture_3d_mask & 1),
            !!(key->texture_border_adjust_mask & 1),
            texture_convolution_filter_name(
                key->texture_convolution_filter[0]),
            key->texture_convolution_filter[0],
            key->texture_width[0], key->texture_height[0],
            key->shader_stage_program, key->other_stage_input,
            key->window_clip_exclusive,
            key->combiner_control, key->final_inputs_0,
            key->final_inputs_1, key->fog_enable, key->fog_mode,
            key->fixed_function_foggen, key->point_params_enable,
            key->point_sprite_enable,
            pgraph_reg_r(pg, NV_PGRAPH_POINTSIZE) / 8.0f,
            MAX(pgraph_reg_r(pg, NV_PGRAPH_POINTSIZE) / 8.0f, 1.0f) *
                MAX(pg->surface_scale_factor, 1),
            pg->point_params[0], pg->point_params[1], pg->point_params[2],
            pg->point_params[3], pg->point_params[4], pg->point_params[5],
            pg->point_params[6], pg->point_params[7],
            key->fixed_function_lighting, key->fixed_function_local_eye,
            key->specular_enable, key->separate_specular,
            key->ignore_specular_alpha, key->fixed_function_emission_src,
            key->fixed_function_ambient_src, key->fixed_function_diffuse_src,
            key->fixed_function_specular_src,
            key->fixed_function_light[0], key->fixed_function_light[1],
            key->fixed_function_light[2], key->fixed_function_light[3],
            key->fixed_function_light[4], key->fixed_function_light[5],
            key->fixed_function_light[6], key->fixed_function_light[7],
            key->rgb_inputs[0], key->rgb_outputs[0],
            key->alpha_inputs[0], key->alpha_outputs[0],
            wrote_vs ? "" : "not-written:", vs_path,
            gs_source == NULL ? "none:" :
                (wrote_gs ? "" : "not-written:"),
            gs_source == NULL ? "" : gs_path,
            wrote_ps ? "" : "not-written:", ps_path);
}

static gboolean pipeline_key_equal(gconstpointer a, gconstpointer b)
{
    return memcmp(a, b, sizeof(PGRAPHD3D12PipelineKey)) == 0;
}

/* A draw pipeline, and the pipeline list that last recorded its key. */
typedef struct PipelineCacheEntry {
    ID3D12PipelineState *pipeline_state;
    uint32_t recorded_list;
    /* Set once it is current; until then no command list has used it. */
    bool drawn;
} PipelineCacheEntry;

static void destroy_pipeline_entry(gpointer value)
{
    PipelineCacheEntry *entry = value;

    D3D12_RELEASE(entry->pipeline_state);
    g_free(entry);
}

static bool pipeline_key_has_vertex_value(
    const PGRAPHD3D12PipelineKey *key, uint8_t attr);
static void append_vertex_value_expr(GString *source,
                                     const PGRAPHD3D12PipelineKey *key,
                                     uint8_t attr);
static uint8_t fixed_function_texgen_mode(uint32_t csv1_a, uint32_t csv1_b,
                                          unsigned int stage,
                                          unsigned int component);
static void fill_fixed_function_key(PGRAPHState *pg,
                                    PGRAPHD3D12PipelineKey *key);
static void report_programmable_vsh_fallback(
    PGRAPHState *pg, const PGRAPHD3D12PipelineKey *key);

static DXGI_FORMAT pipeline_key_attribute_format(
    const PGRAPHD3D12PipelineKey *key, uint8_t attr)
{
    for (uint8_t i = 0; i < key->num_attributes; i++) {
        if (key->attribute_indices[i] == attr) {
            return key->attribute_formats[i];
        }
    }

    return DXGI_FORMAT_UNKNOWN;
}

static bool dxgi_format_is_signed_integer(DXGI_FORMAT format)
{
    switch (format) {
    case DXGI_FORMAT_R16_SINT:
    case DXGI_FORMAT_R16G16_SINT:
    case DXGI_FORMAT_R16G16B16A16_SINT:
    case DXGI_FORMAT_R32_SINT:
        return true;
    default:
        return false;
    }
}

typedef struct PGRAPHD3D12VshFieldMapping {
    VshFieldName field_name;
    uint8_t subtoken;
    uint8_t start_bit;
    uint8_t bit_length;
} PGRAPHD3D12VshFieldMapping;

static const PGRAPHD3D12VshFieldMapping d3d12_vsh_field_mapping[] = {
    { FLD_ILU, 1, 25, 3 },
    { FLD_MAC, 1, 21, 4 },
    { FLD_CONST, 1, 13, 8 },
    { FLD_V, 1, 9, 4 },
    { FLD_A_NEG, 1, 8, 1 },
    { FLD_A_SWZ_X, 1, 6, 2 },
    { FLD_A_SWZ_Y, 1, 4, 2 },
    { FLD_A_SWZ_Z, 1, 2, 2 },
    { FLD_A_SWZ_W, 1, 0, 2 },
    { FLD_A_R, 2, 28, 4 },
    { FLD_A_MUX, 2, 26, 2 },
    { FLD_B_NEG, 2, 25, 1 },
    { FLD_B_SWZ_X, 2, 23, 2 },
    { FLD_B_SWZ_Y, 2, 21, 2 },
    { FLD_B_SWZ_Z, 2, 19, 2 },
    { FLD_B_SWZ_W, 2, 17, 2 },
    { FLD_B_R, 2, 13, 4 },
    { FLD_B_MUX, 2, 11, 2 },
    { FLD_C_NEG, 2, 10, 1 },
    { FLD_C_SWZ_X, 2, 8, 2 },
    { FLD_C_SWZ_Y, 2, 6, 2 },
    { FLD_C_SWZ_Z, 2, 4, 2 },
    { FLD_C_SWZ_W, 2, 2, 2 },
    { FLD_C_R_HIGH, 2, 0, 2 },
    { FLD_C_R_LOW, 3, 30, 2 },
    { FLD_C_MUX, 3, 28, 2 },
    { FLD_OUT_MAC_MASK, 3, 24, 4 },
    { FLD_OUT_R, 3, 20, 4 },
    { FLD_OUT_ILU_MASK, 3, 16, 4 },
    { FLD_OUT_O_MASK, 3, 12, 4 },
    { FLD_OUT_ORB, 3, 11, 1 },
    { FLD_OUT_ADDRESS, 3, 3, 8 },
    { FLD_OUT_MUX, 3, 2, 1 },
    { FLD_A0X, 3, 1, 1 },
    { FLD_FINAL, 3, 0, 1 },
};

typedef struct PGRAPHD3D12VshOpcodeParams {
    bool a;
    bool b;
    bool c;
} PGRAPHD3D12VshOpcodeParams;

static const PGRAPHD3D12VshOpcodeParams d3d12_vsh_mac_params[] = {
    [MAC_NOP] = { false, false, false },
    [MAC_MOV] = { true, false, false },
    [MAC_MUL] = { true, true, false },
    [MAC_ADD] = { true, false, true },
    [MAC_MAD] = { true, true, true },
    [MAC_DP3] = { true, true, false },
    [MAC_DPH] = { true, true, false },
    [MAC_DP4] = { true, true, false },
    [MAC_DST] = { true, true, false },
    [MAC_MIN] = { true, true, false },
    [MAC_MAX] = { true, true, false },
    [MAC_SLT] = { true, true, false },
    [MAC_SGE] = { true, true, false },
    [MAC_ARL] = { true, false, false },
};

static const bool d3d12_vsh_ilu_force_scalar[] = {
    [ILU_NOP] = false,
    [ILU_MOV] = false,
    [ILU_RCP] = true,
    [ILU_RCC] = true,
    [ILU_RSQ] = true,
    [ILU_EXP] = true,
    [ILU_LOG] = true,
    [ILU_LIT] = false,
};

static const char *d3d12_vsh_mac_opcode[] = {
    [MAC_NOP] = "NOP",
    [MAC_MOV] = "MOV",
    [MAC_MUL] = "MUL",
    [MAC_ADD] = "ADD",
    [MAC_MAD] = "MAD",
    [MAC_DP3] = "DP3",
    [MAC_DPH] = "DPH",
    [MAC_DP4] = "DP4",
    [MAC_DST] = "DST",
    [MAC_MIN] = "MIN",
    [MAC_MAX] = "MAX",
    [MAC_SLT] = "SLT",
    [MAC_SGE] = "SGE",
    [MAC_ARL] = "ARL",
};

static const char *d3d12_vsh_ilu_opcode[] = {
    [ILU_NOP] = "NOP",
    [ILU_MOV] = "MOV",
    [ILU_RCP] = "RCP",
    [ILU_RCC] = "RCC",
    [ILU_RSQ] = "RSQ",
    [ILU_EXP] = "EXP",
    [ILU_LOG] = "LOG",
    [ILU_LIT] = "LIT",
};

static const char *d3d12_vsh_mask[] = {
    ",", ",w", ",z", ",zw", ",y", ",yw", ",yz", ",yzw",
    ",x", ",xw", ",xz", ",xzw", ",xy", ",xyw", ",xyz", ",xyzw",
};

static const char *d3d12_vsh_fog_mask[] = {
    ",", ",x", ",x", ",xy", ",x", ",xy", ",xy", ",xyz",
    ",x", ",xy", ",xy", ",xyz", ",xy", ",xyz", ",xyz", ",xyzw",
};

static const char *d3d12_vsh_output_reg[] = {
    "oPos", "unsupported_o1", "unsupported_o2", "oD0",
    "oD1", "oFog", "oPts", "oB0",
    "oB1", "oT0", "oT1", "oT2",
    "oT3", "unsupported_o13", "unsupported_o14", "unsupported_o15",
};

static uint8_t d3d12_vsh_get_field(const uint32_t *token,
                                   VshFieldName field_name)
{
    const PGRAPHD3D12VshFieldMapping *field =
        &d3d12_vsh_field_mapping[field_name];
    uint32_t mask = field->bit_length == 32 ?
        UINT32_MAX : ((1u << field->bit_length) - 1u);

    return (token[field->subtoken] >> field->start_bit) & mask;
}

static int16_t d3d12_vsh_convert_c_register(int16_t c_reg)
{
    int16_t reg = ((((c_reg >> 5) & 7) - 3) * 32) + (c_reg & 31);

    return reg + VSH_D3DSCM_CORRECTION;
}

static char *d3d12_vsh_decode_swizzle(const uint32_t *token,
                                      VshFieldName swizzle_field)
{
    const char swizzle[] = "xyzw";
    VshSwizzle x, y, z, w;

    if (swizzle_field == FLD_C_SWZ_X &&
        d3d12_vsh_ilu_force_scalar[d3d12_vsh_get_field(token, FLD_ILU)]) {
        x = y = z = w = d3d12_vsh_get_field(token, swizzle_field);
    } else {
        x = d3d12_vsh_get_field(token, swizzle_field++);
        y = d3d12_vsh_get_field(token, swizzle_field++);
        z = d3d12_vsh_get_field(token, swizzle_field++);
        w = d3d12_vsh_get_field(token, swizzle_field);
    }

    if (x == SWIZZLE_X && y == SWIZZLE_Y &&
        z == SWIZZLE_Z && w == SWIZZLE_W) {
        return g_strdup("");
    }
    if (x == y && y == z && z == w) {
        return g_strdup_printf(".%c", swizzle[x]);
    }
    if (y == z && z == w) {
        return g_strdup_printf(".%c%c", swizzle[x], swizzle[y]);
    }
    if (z == w) {
        return g_strdup_printf(".%c%c%c", swizzle[x], swizzle[y],
                               swizzle[z]);
    }
    return g_strdup_printf(".%c%c%c%c", swizzle[x], swizzle[y],
                           swizzle[z], swizzle[w]);
}

static char *d3d12_vsh_decode_input(const uint32_t *token,
                                    VshParameterType param,
                                    VshFieldName neg_field, int reg_num)
{
    g_autofree char *swizzle = d3d12_vsh_decode_swizzle(token, neg_field + 1);
    const char *neg = d3d12_vsh_get_field(token, neg_field) ? "-" : "";

    switch (param) {
    case PARAM_R:
        return g_strdup_printf("%sR%d%s", neg, reg_num, swizzle);
    case PARAM_V:
        reg_num = d3d12_vsh_get_field(token, FLD_V);
        return g_strdup_printf("%sv%d%s", neg, reg_num, swizzle);
    case PARAM_C:
        reg_num = d3d12_vsh_convert_c_register(
            d3d12_vsh_get_field(token, FLD_CONST));
        if (d3d12_vsh_get_field(token, FLD_A0X)) {
            return g_strdup_printf("%sc[A0+%d]%s", neg, reg_num, swizzle);
        }
        return g_strdup_printf("%sc[%d]%s", neg, reg_num, swizzle);
    default:
        return g_strdup("(float4)0");
    }
}

static void d3d12_vsh_append_opcode(GString *source, const uint32_t *token,
                                    VshOutputMux out_mux, uint32_t mask,
                                    const char *opcode, const char *inputs,
                                    GString **suffix)
{
    int reg_num = d3d12_vsh_get_field(token, FLD_OUT_R);
    bool use_temp_var = false;
    bool use_temp_opos = false;
    const char *output_mask = NULL;

    if (out_mux == OMUX_MAC &&
        d3d12_vsh_get_field(token, FLD_ILU) != ILU_NOP) {
        use_temp_var = true;
        if (reg_num == 1) {
            mask = 0;
        }
    } else if (out_mux == OMUX_ILU &&
               d3d12_vsh_get_field(token, FLD_MAC) != MAC_NOP) {
        reg_num = 1;
    }

    if (d3d12_vsh_get_field(token, FLD_OUT_MUX) == out_mux &&
        d3d12_vsh_get_field(token, FLD_OUT_O_MASK) != 0) {
        if (d3d12_vsh_get_field(token, FLD_OUT_ORB) == OUTPUT_C) {
            g_string_append_printf(
                source,
                "    // D3D12 XVS skipped writable constant output: "
                "address=%u, opcode=%s, token={0x%08x,0x%08x,0x%08x,0x%08x}\n",
                d3d12_vsh_get_field(token, FLD_OUT_ADDRESS), opcode,
                token[0], token[1], token[2], token[3]);
        } else {
            int out_reg = d3d12_vsh_get_field(token, FLD_OUT_ADDRESS) & 0xf;
            int write_mask = d3d12_vsh_get_field(token, FLD_OUT_O_MASK);
            bool fog = out_reg == 5;

            output_mask = fog ? d3d12_vsh_fog_mask[write_mask] :
                                d3d12_vsh_mask[write_mask];
            /*
             * R12 mirrors oPos and paired MAC+ILU tokens read all sources
             * before writing, so defer the oPos write to the token suffix.
             */
            use_temp_opos = out_reg == 0 && (use_temp_var || mask != 0);
            g_string_append_printf(
                source, "    %s(%s%s%s);\n", opcode,
                use_temp_opos ? "_temp_opos" :
                                d3d12_vsh_output_reg[out_reg],
                output_mask, inputs);
        }
    }

    if (use_temp_var) {
        assert(suffix != NULL);
        *suffix = g_string_new(NULL);
        if (strcmp(opcode, "ARL") == 0) {
            g_string_append_printf(source, "    ARL(_temp_addr%s);\n",
                                   inputs);
            g_string_append(*suffix, "    A0 = _temp_addr;\n");
        } else if (mask > 0) {
            const char *mask_components = &d3d12_vsh_mask[mask][1];

            g_string_append_printf(source, "    %s(_temp_vec%s%s);\n",
                                   opcode, d3d12_vsh_mask[mask], inputs);
            if (mask_components[0]) {
                g_string_append_printf(*suffix,
                                       "    R%d.%s = _temp_vec.%s;\n",
                                       reg_num, mask_components,
                                       mask_components);
            } else {
                g_string_append_printf(*suffix, "    R%d = _temp_vec;\n",
                                       reg_num);
            }
        }
    } else if (strcmp(opcode, "ARL") == 0) {
        g_string_append_printf(source, "    ARL(A0%s);\n", inputs);
    } else if (mask > 0) {
        g_string_append_printf(source, "    %s(R%d%s%s);\n", opcode,
                               reg_num, d3d12_vsh_mask[mask], inputs);
    }

    if (use_temp_opos) {
        const char *mask_components;

        assert(suffix != NULL);
        if (*suffix == NULL) {
            *suffix = g_string_new(NULL);
        }
        assert(output_mask != NULL && output_mask[0] == ',');
        mask_components = output_mask + 1;
        assert(mask_components[0] != '\0');
        g_string_append_printf(*suffix,
                               "    oPos.%s = _temp_opos.%s;\n",
                               mask_components, mask_components);
    }
}

static void d3d12_vsh_append_token(GString *source, const uint32_t *token,
                                   unsigned int slot)
{
    VshMAC mac = d3d12_vsh_get_field(token, FLD_MAC);
    VshILU ilu = d3d12_vsh_get_field(token, FLD_ILU);
    GString *suffix = NULL;
    g_autofree char *input_c = NULL;

    /* The 4-bit field can encode values past the last defined opcode. */
    if (mac > MAC_ARL) {
        mac = MAC_NOP;
    }

    if (mac == MAC_NOP && ilu == ILU_NOP) {
        return;
    }

    input_c = d3d12_vsh_decode_input(
        token, d3d12_vsh_get_field(token, FLD_C_MUX), FLD_C_NEG,
        (d3d12_vsh_get_field(token, FLD_C_R_HIGH) << 2) |
            d3d12_vsh_get_field(token, FLD_C_R_LOW));

    g_string_append_printf(
        source,
        "    // Slot %u: 0x%08x 0x%08x 0x%08x 0x%08x\n",
        slot, token[0], token[1], token[2], token[3]);

    if (mac != MAC_NOP) {
        g_autoptr(GString) inputs = g_string_new(NULL);

        if (d3d12_vsh_mac_params[mac].a) {
            g_autofree char *input = d3d12_vsh_decode_input(
                token, d3d12_vsh_get_field(token, FLD_A_MUX), FLD_A_NEG,
                d3d12_vsh_get_field(token, FLD_A_R));
            g_string_append_printf(inputs, ", %s", input);
        }
        if (d3d12_vsh_mac_params[mac].b) {
            g_autofree char *input = d3d12_vsh_decode_input(
                token, d3d12_vsh_get_field(token, FLD_B_MUX), FLD_B_NEG,
                d3d12_vsh_get_field(token, FLD_B_R));
            g_string_append_printf(inputs, ", %s", input);
        }
        if (d3d12_vsh_mac_params[mac].c) {
            g_string_append_printf(inputs, ", %s", input_c);
        }

        d3d12_vsh_append_opcode(
            source, token, OMUX_MAC,
            d3d12_vsh_get_field(token, FLD_OUT_MAC_MASK),
            d3d12_vsh_mac_opcode[mac], inputs->str, &suffix);
    }

    if (ilu != ILU_NOP) {
        g_autofree char *inputs = g_strdup_printf(", %s", input_c);

        d3d12_vsh_append_opcode(
            source, token, OMUX_ILU,
            d3d12_vsh_get_field(token, FLD_OUT_ILU_MASK),
            d3d12_vsh_ilu_opcode[ilu], inputs, &suffix);
    }

    if (suffix != NULL) {
        g_string_append(source, suffix->str);
        g_string_free(suffix, TRUE);
    }
}

/*
 * NaN/Inf tests on the bit pattern. FXC without IEEE strictness may fold
 * isnan()/isinf() to false, which would drop the guards.
 */
static void append_float_class_helpers(GString *source)
{
    g_string_append(source,
                    "bool float_is_nan(float v)\n"
                    "{\n"
                    "    return (asuint(v) & 0x7fffffffu) > 0x7f800000u;\n"
                    "}\n"
                    "bool float_is_inf(float v)\n"
                    "{\n"
                    "    return (asuint(v) & 0x7fffffffu) == 0x7f800000u;\n"
                    "}\n");
}

static void append_programmable_vsh_helpers(GString *source)
{
    g_string_append(source,
        "float4 vsh_in(float v) { return float4(v, v, v, v); }\n"
        "float4 vsh_in(float2 v) { return float4(v.x, v.y, v.y, v.y); }\n"
        "float4 vsh_in(float3 v) { return float4(v.x, v.y, v.z, v.z); }\n"
        "float4 vsh_in(float4 v) { return v; }\n"
        "float4 vsh_nan_to_one(float4 v)\n"
        "{\n"
        "    if (float_is_nan(v.x)) { v.x = 1.0; }\n"
        "    if (float_is_nan(v.y)) { v.y = 1.0; }\n"
        "    if (float_is_nan(v.z)) { v.z = 1.0; }\n"
        "    if (float_is_nan(v.w)) { v.w = 1.0; }\n"
        "    return v;\n"
        "}\n"
        "float4 vsh_mov(float4 src) { return src; }\n"
        "float4 vsh_mul(float4 a, float4 b)\n"
        "{\n"
        "    float4 zero = sign(vsh_nan_to_one(a)) *\n"
        "                  sign(vsh_nan_to_one(b));\n"
        "    float4 ret = a * b;\n"
        "    if (zero.x == 0.0) { ret.x = 0.0; }\n"
        "    if (zero.y == 0.0) { ret.y = 0.0; }\n"
        "    if (zero.z == 0.0) { ret.z = 0.0; }\n"
        "    if (zero.w == 0.0) { ret.w = 0.0; }\n"
        "    return ret;\n"
        "}\n"
        "float4 vsh_add(float4 a, float4 b) { return a + b; }\n"
        "float4 vsh_mad(float4 a, float4 b, float4 c)\n"
        "{\n"
        "    return vsh_mul(a, b) + c;\n"
        "}\n"
        "float4 vsh_dp3(float4 a, float4 b)\n"
        "{\n"
        "    return float4(dot(a.xyz, b.xyz), dot(a.xyz, b.xyz),\n"
        "                  dot(a.xyz, b.xyz), dot(a.xyz, b.xyz));\n"
        "}\n"
        "float4 vsh_dph(float4 a, float4 b)\n"
        "{\n"
        "    float v = dot(float4(a.xyz, 1.0), b);\n"
        "    return float4(v, v, v, v);\n"
        "}\n"
        "float4 vsh_dp4(float4 a, float4 b)\n"
        "{\n"
        "    float v = dot(a, b);\n"
        "    return float4(v, v, v, v);\n"
        "}\n"
        "float4 vsh_dst(float4 a, float4 b)\n"
        "{\n"
        "    return float4(1.0, a.y * b.y, a.z, b.w);\n"
        "}\n"
        "float4 vsh_min(float4 a, float4 b) { return min(a, b); }\n"
        "float4 vsh_max(float4 a, float4 b) { return max(a, b); }\n"
        "float4 vsh_slt(float4 a, float4 b)\n"
        "{\n"
        "    return float4(a.x < b.x ? 1.0 : 0.0,\n"
        "                  a.y < b.y ? 1.0 : 0.0,\n"
        "                  a.z < b.z ? 1.0 : 0.0,\n"
        "                  a.w < b.w ? 1.0 : 0.0);\n"
        "}\n"
        "float4 vsh_sge(float4 a, float4 b)\n"
        "{\n"
        "    return float4(a.x >= b.x ? 1.0 : 0.0,\n"
        "                  a.y >= b.y ? 1.0 : 0.0,\n"
        "                  a.z >= b.z ? 1.0 : 0.0,\n"
        "                  a.w >= b.w ? 1.0 : 0.0);\n"
        "}\n"
        "int vsh_arl(float src) { return (int)floor(src + 0.001); }\n"
        "float4 vsh_rcp(float src)\n"
        "{\n"
        "    float v = 1.0 / src;\n"
        "    return float4(v, v, v, v);\n"
        "}\n"
        "float4 vsh_rcc(float src)\n"
        "{\n"
        "    float v = clamp_away_zero_inf(1.0 / src);\n"
        "    return float4(v, v, v, v);\n"
        "}\n"
        "float4 vsh_rsq(float src)\n"
        "{\n"
        "    if (src == 0.0) { return float4(1.0 / 0.0, 1.0 / 0.0,\n"
        "                                     1.0 / 0.0, 1.0 / 0.0); }\n"
        "    if (float_is_inf(src)) { return float4(0.0, 0.0, 0.0, 0.0); }\n"
        "    float v = rsqrt(abs(src));\n"
        "    return float4(v, v, v, v);\n"
        "}\n"
        "float4 vsh_exp(float src)\n"
        "{\n"
        "    return float4(exp2(floor(src)), src - floor(src),\n"
        "                  exp2(src), 1.0);\n"
        "}\n"
        "float4 vsh_log(float src)\n"
        "{\n"
        "    float tmp = abs(src);\n"
        "    if (tmp == 0.0) { return float4(-1.0 / 0.0, 1.0,\n"
        "                                      -1.0 / 0.0, 1.0); }\n"
        "    return float4(floor(log2(tmp)), tmp / exp2(floor(log2(tmp))),\n"
        "                  log2(tmp), 1.0);\n"
        "}\n"
        "float4 vsh_lit(float4 src)\n"
        "{\n"
        "    float4 s = src;\n"
        "    float epsilon = 1.0 / 256.0;\n"
        "    s.w = clamp(s.w, -(128.0 - epsilon), 128.0 - epsilon);\n"
        "    s.x = max(s.x, 0.0);\n"
        "    s.y = max(s.y, 0.0);\n"
        "    float4 t = float4(1.0, 0.0, 0.0, 1.0);\n"
        "    t.y = s.x;\n"
        "    t.z = (s.x > 0.0) ? exp2(s.w * log2(s.y)) : 0.0;\n"
        "    return t;\n"
        "}\n"
        "#define MOV(dest, mask, src) dest.mask = vsh_mov(vsh_in(src)).mask\n"
        "#define MUL(dest, mask, src0, src1) dest.mask = vsh_mul(vsh_in(src0), vsh_in(src1)).mask\n"
        "#define ADD(dest, mask, src0, src1) dest.mask = vsh_add(vsh_in(src0), vsh_in(src1)).mask\n"
        "#define MAD(dest, mask, src0, src1, src2) dest.mask = vsh_mad(vsh_in(src0), vsh_in(src1), vsh_in(src2)).mask\n"
        "#define DP3(dest, mask, src0, src1) dest.mask = vsh_dp3(vsh_in(src0), vsh_in(src1)).mask\n"
        "#define DPH(dest, mask, src0, src1) dest.mask = vsh_dph(vsh_in(src0), vsh_in(src1)).mask\n"
        "#define DP4(dest, mask, src0, src1) dest.mask = vsh_dp4(vsh_in(src0), vsh_in(src1)).mask\n"
        "#define DST(dest, mask, src0, src1) dest.mask = vsh_dst(vsh_in(src0), vsh_in(src1)).mask\n"
        "#define MIN(dest, mask, src0, src1) dest.mask = vsh_min(vsh_in(src0), vsh_in(src1)).mask\n"
        "#define MAX(dest, mask, src0, src1) dest.mask = vsh_max(vsh_in(src0), vsh_in(src1)).mask\n"
        "#define SLT(dest, mask, src0, src1) dest.mask = vsh_slt(vsh_in(src0), vsh_in(src1)).mask\n"
        "#define SGE(dest, mask, src0, src1) dest.mask = vsh_sge(vsh_in(src0), vsh_in(src1)).mask\n"
        "#define ARL(dest, src) dest = vsh_arl(vsh_in(src).x)\n"
        "#define RCP(dest, mask, src) dest.mask = vsh_rcp(vsh_in(src).x).mask\n"
        "#define RCC(dest, mask, src) dest.mask = vsh_rcc(vsh_in(src).x).mask\n"
        "#define RSQ(dest, mask, src) dest.mask = vsh_rsq(vsh_in(src).x).mask\n"
        "#define EXP(dest, mask, src) dest.mask = vsh_exp(vsh_in(src).x).mask\n"
        "#define LOG(dest, mask, src) dest.mask = vsh_log(vsh_in(src).x).mask\n"
        "#define LIT(dest, mask, src) dest.mask = vsh_lit(vsh_in(src)).mask\n");
}

static void append_fog_output_from_distance(
    GString *source, const PGRAPHD3D12PipelineKey *key,
    const char *fog_distance_expr);

static void append_programmable_vsh_body(
    GString *source, const PGRAPHD3D12PipelineKey *key)
{
    for (unsigned int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
        g_string_append_printf(source, "    float4 v%u = ", i);
        if (pipeline_key_has_vertex_value(key, i)) {
            append_vertex_value_expr(source, key, i);
        } else {
            g_string_append(source, "float4(0.0, 0.0, 0.0, 1.0)");
        }
        g_string_append(source, ";\n");
    }

    g_string_append(source,
                    "    int A0 = 0;\n"
                    "    int _temp_addr = 0;\n"
                    "    float4 _temp_vec = float4(0.0, 0.0, 0.0, 0.0);\n"
                    "    float4 _temp_opos = float4(0.0, 0.0, 0.0, 0.0);\n"
                    "    float4 R0 = float4(0.0, 0.0, 0.0, 0.0);\n"
                    "    float4 R1 = float4(0.0, 0.0, 0.0, 0.0);\n"
                    "    float4 R2 = float4(0.0, 0.0, 0.0, 0.0);\n"
                    "    float4 R3 = float4(0.0, 0.0, 0.0, 0.0);\n"
                    "    float4 R4 = float4(0.0, 0.0, 0.0, 0.0);\n"
                    "    float4 R5 = float4(0.0, 0.0, 0.0, 0.0);\n"
                    "    float4 R6 = float4(0.0, 0.0, 0.0, 0.0);\n"
                    "    float4 R7 = float4(0.0, 0.0, 0.0, 0.0);\n"
                    "    float4 R8 = float4(0.0, 0.0, 0.0, 0.0);\n"
                    "    float4 R9 = float4(0.0, 0.0, 0.0, 0.0);\n"
                    "    float4 R10 = float4(0.0, 0.0, 0.0, 0.0);\n"
                    "    float4 R11 = float4(0.0, 0.0, 0.0, 0.0);\n"
                    "    float4 oPos = float4(0.0, 0.0, 0.0, 1.0);\n"
                    "    float4 oD0 = float4(0.0, 0.0, 0.0, 1.0);\n"
                    "    float4 oD1 = float4(0.0, 0.0, 0.0, 1.0);\n"
                    "    float4 oB0 = float4(0.0, 0.0, 0.0, 1.0);\n"
                    "    float4 oB1 = float4(0.0, 0.0, 0.0, 1.0);\n"
                    "    float4 oFog = float4(0.0, 0.0, 0.0, 1.0);\n"
                    "    float4 oPts = float4(0.0, 0.0, 0.0, 1.0);\n"
                    "    float4 oT0 = float4(0.0, 0.0, 0.0, 1.0);\n"
                    "    float4 oT1 = float4(0.0, 0.0, 0.0, 1.0);\n"
                    "    float4 oT2 = float4(0.0, 0.0, 0.0, 1.0);\n"
                    "    float4 oT3 = float4(0.0, 0.0, 0.0, 1.0);\n"
                    "    float4 unsupported_o1 = float4(0.0, 0.0, 0.0, 0.0);\n"
                    "    float4 unsupported_o2 = float4(0.0, 0.0, 0.0, 0.0);\n"
                    "    float4 unsupported_o13 = float4(0.0, 0.0, 0.0, 0.0);\n"
                    "    float4 unsupported_o14 = float4(0.0, 0.0, 0.0, 0.0);\n"
                    "    float4 unsupported_o15 = float4(0.0, 0.0, 0.0, 0.0);\n"
                    "#define R12 oPos\n");

    for (unsigned int i = 0; i < key->transform_program_length; i++) {
        d3d12_vsh_append_token(source, key->transform_program_data[i], i);
        if (key->transform_program_data[i][3] & 1) {
            break;
        }
    }

    /* precise keeps the position bit-identical across shader variants,
     * which multipass depth-equal rendering relies on. */
    g_string_append(source,
                    "#undef R12\n"
                    "    precise float4 depthPos = "
                    "programmable_depth_position(oPos);\n"
                    "    precise float4 screenPos = "
                    "programmable_screen_position(oPos);\n"
                    "    precise float4 rasterDepthPos = VS_DEPTH_POS;\n"
                    "    output.depthPos = rasterDepthPos;\n"
                    "    output.position = screenPos;\n"
                    "    output.diffuse = saturate(nan_to_one(oD0));\n"
                    "    output.specular = saturate(nan_to_one(oD1));\n");
    if (!key->specular_enable) {
        g_string_append(
            source,
            "    output.specular = float4(0.0, 0.0, 0.0, 1.0);\n");
    } else if (key->ignore_specular_alpha) {
        g_string_append(source, "    output.specular.a = 1.0;\n");
    }
    if (key->point_params_enable) {
        g_string_append(source, "    output.pointSize = oPts.x;\n");
    } else {
        g_string_append(source,
                        "    output.pointSize = "
                        "programmable_point_size();\n");
    }
    append_fog_output_from_distance(source, key, "oFog.x");
    for (unsigned int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        if (key->texture_coord_mask & (1u << i)) {
            g_string_append_printf(source, "    output.tex%u = oT%u;\n",
                                   i, i);
        }
    }
}

static uint8_t fixed_function_texgen_mode(uint32_t csv1_a, uint32_t csv1_b,
                                          unsigned int stage,
                                          unsigned int component)
{
    static const uint32_t masks[2][4] = {
        {
            NV_PGRAPH_CSV1_A_T0_S,
            NV_PGRAPH_CSV1_A_T0_T,
            NV_PGRAPH_CSV1_A_T0_R,
            NV_PGRAPH_CSV1_A_T0_Q,
        },
        {
            NV_PGRAPH_CSV1_A_T1_S,
            NV_PGRAPH_CSV1_A_T1_T,
            NV_PGRAPH_CSV1_A_T1_R,
            NV_PGRAPH_CSV1_A_T1_Q,
        },
    };
    uint32_t reg = stage < 2 ? csv1_a : csv1_b;

    assert(stage < NV2A_MAX_TEXTURES);
    assert(component < 4);

    return GET_MASK(reg, masks[stage & 1][component]);
}

static unsigned int fixed_function_texgen_plane_base(unsigned int stage)
{
    static const unsigned int bases[NV2A_MAX_TEXTURES] = {
        NV_IGRAPH_XF_XFCTX_TG0MAT,
        NV_IGRAPH_XF_XFCTX_TG1MAT,
        NV_IGRAPH_XF_XFCTX_TG2MAT,
        NV_IGRAPH_XF_XFCTX_TG3MAT,
    };

    assert(stage < NV2A_MAX_TEXTURES);
    return bases[stage];
}

static unsigned int fixed_function_texture_matrix_base(unsigned int stage)
{
    static const unsigned int bases[NV2A_MAX_TEXTURES] = {
        NV_IGRAPH_XF_XFCTX_T0MAT,
        NV_IGRAPH_XF_XFCTX_T1MAT,
        NV_IGRAPH_XF_XFCTX_T2MAT,
        NV_IGRAPH_XF_XFCTX_T3MAT,
    };

    assert(stage < NV2A_MAX_TEXTURES);
    return bases[stage];
}

static void append_fixed_function_texgen_component(
    GString *source, const PGRAPHD3D12PipelineKey *key, unsigned int stage,
    unsigned int component)
{
    static const char coords[] = "xyzw";
    char coord = coords[component];
    uint8_t mode = key->fixed_function_texgen[stage][component];
    unsigned int plane = fixed_function_texgen_plane_base(stage) + component;

    switch (mode) {
    case TEXGEN_DISABLE:
        g_string_append_printf(source,
                               "    oT%u.%c = texture%u.%c;\n",
                               stage, coord, stage, coord);
        break;
    case TEXGEN_EYE_LINEAR:
        g_string_append_printf(source,
                               "    oT%u.%c = dot(c[%u], tPosition);\n",
                               stage, coord, plane);
        break;
    case TEXGEN_OBJECT_LINEAR:
        g_string_append_printf(source,
                               "    oT%u.%c = dot(c[%u], position);\n",
                               stage, coord, plane);
        break;
    case TEXGEN_SPHERE_MAP:
        if (component >= 2) {
            g_string_append_printf(source,
                                   "    oT%u.%c = texture%u.%c;\n",
                                   stage, coord, stage, coord);
            break;
        }
        g_string_append_printf(
            source,
            "    {\n"
            "        float3 u = normalize(tPosition.xyz);\n"
            "        float3 r = reflect(u, tNormal);\n"
            "        float invM = 1.0 / "
            "(2.0 * length(r + float3(0.0, 0.0, 1.0)));\n"
            "        oT%u.%c = r.%c * invM + 0.5;\n"
            "    }\n",
            stage, coord, coord);
        break;
    case TEXGEN_REFLECTION_MAP:
        if (component >= 3) {
            g_string_append_printf(source,
                                   "    oT%u.%c = texture%u.%c;\n",
                                   stage, coord, stage, coord);
            break;
        }
        g_string_append_printf(
            source,
            "    {\n"
            "        float3 u = normalize(tPosition.xyz);\n"
            "        float3 r = reflect(u, tNormal);\n"
            "        oT%u.%c = r.%c;\n"
            "    }\n",
            stage, coord, coord);
        break;
    case TEXGEN_NORMAL_MAP:
        if (component >= 3) {
            g_string_append_printf(source,
                                   "    oT%u.%c = texture%u.%c;\n",
                                   stage, coord, stage, coord);
            break;
        }
        g_string_append_printf(source,
                               "    oT%u.%c = tNormal.%c;\n",
                               stage, coord, coord);
        break;
    default:
        g_string_append_printf(source,
                               "    oT%u.%c = texture%u.%c;\n",
                               stage, coord, stage, coord);
        break;
    }
}

static void append_fixed_function_texture_output(
    GString *source, const PGRAPHD3D12PipelineKey *key, unsigned int stage)
{
    g_string_append_printf(source, "    float4 texture%u = ", stage);
    if (pipeline_key_has_vertex_value(key, NV2A_VERTEX_ATTR_TEXTURE0 + stage)) {
        append_vertex_value_expr(source, key,
                                 NV2A_VERTEX_ATTR_TEXTURE0 + stage);
    } else {
        g_string_append(source, "float4(0.0, 0.0, 0.0, 1.0)");
    }
    g_string_append_printf(source,
                           ";\n"
                           "    float4 oT%u = texture%u;\n",
                           stage, stage);
    for (unsigned int component = 0; component < 4; component++) {
        append_fixed_function_texgen_component(source, key, stage, component);
    }
    if (key->fixed_function_texture_matrix_enable_mask & (1u << stage)) {
        g_string_append_printf(source,
                               "    oT%u = mul_vec_mat4(oT%u, %u);\n",
                               stage, stage,
                               fixed_function_texture_matrix_base(stage));
    }
    g_string_append_printf(source, "    output.tex%u = oT%u;\n", stage,
                           stage);
}

static const char *fixed_function_alpha_source(uint8_t material_source)
{
    switch (material_source) {
    case MATERIAL_COLOR_SRC_MATERIAL:
        return "material_alpha";
    case MATERIAL_COLOR_SRC_SPECULAR:
        return "specular.a";
    case MATERIAL_COLOR_SRC_DIFFUSE:
    default:
        return "diffuse.a";
    }
}

/* Colour-material source 3 selects no colour; NULL means the term is
 * absent. */
static const char *fixed_function_rgb_source(uint8_t material_source,
                                             const char *material_expr,
                                             const char *diffuse_expr,
                                             const char *specular_expr)
{
    switch (material_source) {
    case MATERIAL_COLOR_SRC_MATERIAL:
        return material_expr;
    case MATERIAL_COLOR_SRC_DIFFUSE:
        return diffuse_expr;
    case MATERIAL_COLOR_SRC_SPECULAR:
        return specular_expr;
    default:
        return NULL;
    }
}

static void append_fixed_function_light_contribution(
    GString *source, const PGRAPHD3D12PipelineKey *key, unsigned int light)
{
    const char *diffuse_term = fixed_function_rgb_source(
        key->fixed_function_diffuse_src, "lightDiffuse",
        "diffuse.xyz * lightDiffuse", "specular.xyz * lightDiffuse");
    const char *specular_term = fixed_function_rgb_source(
        key->fixed_function_specular_src, "lightSpecular",
        "diffuse.xyz * lightSpecular", "specular.xyz * lightSpecular");

    g_string_append_printf(
        source,
        "        float pf = (nDotVP == 0.0 || nDotHV == 0.0) ? "
        "0.0 : pow(nDotHV, specularPower);\n"
        "        float3 lightAmbient = light_ambient_color(%u) * "
        "attenuation;\n"
        "        float3 lightDiffuse = light_diffuse_color(%u) * "
        "attenuation * nDotVP;\n"
        "        float3 lightSpecular = light_specular_color(%u) * "
        "attenuation * pf;\n"
        "        output.diffuse.xyz += lightAmbient;\n",
        light, light, light);
    if (diffuse_term != NULL) {
        g_string_append_printf(source, "        output.diffuse.xyz += %s;\n",
                               diffuse_term);
    }
    if (specular_term != NULL) {
        g_string_append_printf(source, "        output.specular.xyz += %s;\n",
                               specular_term);
    }
}

static void append_fixed_function_lighting_output(
    GString *source, const PGRAPHD3D12PipelineKey *key)
{
    const char *alpha_source =
        fixed_function_alpha_source(key->fixed_function_diffuse_src);

    g_string_append(source,
                    "    float material_alpha = fixedFunctionInfo.x;\n"
                    "    float specularPower = fixedFunctionInfo.y;\n");

    if (!key->fixed_function_lighting) {
        g_string_append(source,
                        "    output.diffuse = saturate(nan_to_one(diffuse));\n"
                        "    output.specular = saturate(nan_to_one(specular));\n");
    } else {
        const char *ambient_term = fixed_function_rgb_source(
            key->fixed_function_ambient_src,
            "ltctxa[D3D12_NV2A_LTCTXA_FR_AMB].xyz", "diffuse.rgb",
            "specular.rgb");
        const char *emission_term = fixed_function_rgb_source(
            key->fixed_function_emission_src,
            "ltctxa[D3D12_NV2A_LTCTXA_FR_AMB].rgb", "diffuse.rgb",
            "specular.rgb");

        /* Without an ambient source D0 keeps its reset value. */
        if (ambient_term != NULL) {
            g_string_append_printf(source,
                                   "    output.diffuse = float4(%s, %s);\n",
                                   ambient_term, alpha_source);
        } else {
            g_string_append(source,
                            "    output.diffuse = "
                            "float4(0.0, 0.0, 0.0, 1.0);\n");
        }
        g_string_append(source,
                        "    output.diffuse.rgb *= "
                        "ltctxa[D3D12_NV2A_LTCTXA_CM_COL].rgb;\n");
        if (emission_term != NULL) {
            g_string_append_printf(source,
                                   "    output.diffuse.rgb += %s;\n",
                                   emission_term);
        }
        g_string_append(source,
                        "    output.specular = float4(0.0, 0.0, 0.0, "
                        "specular.a);\n");

        if (key->fixed_function_local_eye) {
            g_string_append(
                source,
                "    float3 VPeye = normalize("
                "c[D3D12_NV2A_EYEP].xyz / c[D3D12_NV2A_EYEP].w - "
                "tPosition.xyz / tPosition.w);\n");
        }

        for (unsigned int i = 0; i < NV2A_MAX_LIGHTS; i++) {
            switch (key->fixed_function_light[i]) {
            case LIGHT_OFF:
                break;
            case LIGHT_INFINITE:
                g_string_append_printf(
                    source,
                    "    /* Light %u: infinite */\n"
                    "    {\n"
                    "        float attenuation = 1.0;\n"
                    "        float3 lightDirection = "
                    "normalize(lightInfiniteDirection[%u].xyz);\n"
                    "        float nDotVP = max(0.0, dot(tNormal, "
                    "lightDirection));\n",
                    i, i);
                if (key->fixed_function_local_eye) {
                    g_string_append(
                        source,
                        "        float nDotHV = max(0.0, dot(tNormal, "
                        "normalize(lightDirection + VPeye)));\n");
                } else {
                    g_string_append_printf(
                        source,
                        "        float nDotHV = max(0.0, dot(tNormal, "
                        "lightInfiniteHalfVector[%u].xyz));\n",
                        i);
                }
                append_fixed_function_light_contribution(source, key, i);
                g_string_append(source, "    }\n");
                break;
            case LIGHT_LOCAL:
            case LIGHT_SPOT:
                g_string_append_printf(
                    source,
                    "    /* Light %u: %s */\n"
                    "    {\n"
                    "        float3 tPos = tPosition.xyz / tPosition.w;\n"
                    "        float3 VP = lightLocalPosition[%u].xyz - "
                    "tPos;\n"
                    "        float d = length(VP);\n"
                    "        if (d <= light_local_range(%u)) {\n"
                    "            VP = normalize(VP);\n"
                    "            float attenuation = 1.0 / "
                    "(lightLocalAttenuation[%u].x + "
                    "lightLocalAttenuation[%u].y * d + "
                    "lightLocalAttenuation[%u].z * d * d);\n"
                    "            float3 halfVector = normalize(VP + %s);\n"
                    "            float nDotVP = max(0.0, dot(tNormal, VP));\n"
                    "            float nDotHV = max(0.0, dot(tNormal, "
                    "halfVector));\n",
                    i, key->fixed_function_light[i] == LIGHT_SPOT ?
                        "spot" : "local",
                    i, i, i, i, i,
                    key->fixed_function_local_eye ?
                        "VPeye" : "float3(0.0, 0.0, 0.0)");
                if (key->fixed_function_light[i] == LIGHT_SPOT) {
                    g_string_append_printf(
                        source,
                        "            float4 spotDir = "
                        "light_spot_direction(%u);\n"
                        "            float invScale = 1.0 / "
                        "length(spotDir.xyz);\n"
                        "            float cosHalfPhi = "
                        "-invScale * spotDir.w;\n"
                        "            float cosHalfTheta = "
                        "invScale + cosHalfPhi;\n"
                        "            float spotDirDotVP = "
                        "dot(spotDir.xyz, VP);\n"
                        "            float rho = invScale * "
                        "spotDirDotVP;\n"
                        "            if (rho <= cosHalfPhi) {\n"
                        "                attenuation = 0.0;\n"
                        "            } else if (rho <= cosHalfTheta) {\n"
                        "                attenuation *= "
                        "spotDirDotVP + spotDir.w;\n"
                        "            }\n",
                        i);
                }
                append_fixed_function_light_contribution(source, key, i);
                g_string_append(source,
                                "        }\n"
                                "    }\n");
                break;
            default:
                g_string_append_printf(
                    source,
                    "    /* Unsupported fixed-function light %u mode %u; "
                    "ignored for D3D12 shader generation. */\n",
                    i, key->fixed_function_light[i]);
                break;
            }
        }

    }

    if (!key->specular_enable) {
        g_string_append(source,
                        "    output.specular = float4(0.0, 0.0, 0.0, 1.0);\n");
    } else {
        if (!key->separate_specular) {
            if (key->fixed_function_lighting) {
                g_string_append(
                    source,
                    "    output.diffuse.xyz += output.specular.xyz;\n");
            }
            g_string_append(source, "    output.specular = specular;\n");
        }
        if (key->ignore_specular_alpha) {
            g_string_append(source, "    output.specular.a = 1.0;\n");
        }
    }

    /* Clamp once the diffuse/specular combination is final, before
     * interpolation. */
    g_string_append(source,
                    "    output.diffuse = saturate(nan_to_one(output.diffuse));\n"
                    "    output.specular = saturate(nan_to_one(output.specular));\n");
}

/*
 * Blended skinning transform, mirroring append_skinning_code() in
 * glsl/vsh-ff.c. Same-type palette matrices sit eight vectors apart.
 */
static void append_skinning_blend(GString *source, bool mix,
                                  unsigned int count, const char *type,
                                  const char *output, const char *input,
                                  const char *base_define,
                                  const char *swizzle)
{
    if (count == 0) {
        g_string_append_printf(source,
                               "    %s %s = mul_vec_mat4(%s,\n"
                               "                         %s).%s;\n",
                               type, output, input, base_define, swizzle);
        return;
    }

    g_string_append_printf(source, "    %s %s = (%s)0.0;\n",
                           type, output, type);
    if (mix) {
        /* Generated final weight (like GL_WEIGHT_SUM_UNITY_ARB) */
        g_string_append(source,
                        "    {\n"
                        "        float weight_i;\n"
                        "        float weight_n = 1.0;\n");
        for (unsigned int i = 0; i < count; i++) {
            if (i < count - 1) {
                g_string_append_printf(source,
                                       "        weight_i = weight.%c;\n"
                                       "        weight_n -= weight_i;\n",
                                       "xyzw"[i]);
            } else {
                g_string_append(source,
                                "        weight_i = weight_n;\n");
            }
            g_string_append_printf(
                source,
                "        %s += mul_vec_mat4(%s, %s + %uu).%s * weight_i;\n",
                output, input, base_define, i * 8, swizzle);
        }
        g_string_append(source, "    }\n");
    } else {
        /* Individual weights */
        for (unsigned int i = 0; i < count; i++) {
            g_string_append_printf(
                source,
                "    %s += mul_vec_mat4(%s, %s + %uu).%s * weight.%c;\n",
                output, input, base_define, i * 8, swizzle, "xyzw"[i]);
        }
    }
}

static void append_fixed_function_skinning(GString *source,
                                           const PGRAPHD3D12PipelineKey *key)
{
    bool mix = false;
    unsigned int count = 0;

    switch (key->fixed_function_skinning) {
    case SKINNING_OFF:
    default:
        mix = false; count = 0; break;
    case SKINNING_1WEIGHTS:
        mix = true; count = 2; break;
    case SKINNING_2WEIGHTS2MATRICES:
        mix = false; count = 2; break;
    case SKINNING_2WEIGHTS:
        mix = true; count = 3; break;
    case SKINNING_3WEIGHTS3MATRICES:
        mix = false; count = 3; break;
    case SKINNING_3WEIGHTS:
        mix = true; count = 4; break;
    case SKINNING_4WEIGHTS4MATRICES:
        mix = false; count = 4; break;
    }

    if (count > 0) {
        g_string_append(source, "    float4 weight = ");
        if (pipeline_key_has_vertex_value(key, NV2A_VERTEX_ATTR_WEIGHT)) {
            append_vertex_value_expr(source, key, NV2A_VERTEX_ATTR_WEIGHT);
        } else {
            g_string_append(source, "float4(0.0, 0.0, 0.0, 0.0)");
        }
        g_string_append(source, ";\n");
    }

    append_skinning_blend(source, mix, count, "float4", "tPosition",
                          "position", "D3D12_NV2A_MMAT0", "xyzw");
    append_skinning_blend(source, mix, count, "float3", "tNormal",
                          "float4(normal, 0.0)", "D3D12_NV2A_IMMAT0", "xyz");
}

/*
 * Vertex pulling: emit ByteAddressBuffer SRVs and a fetch_vertex() that
 * fills VSInput from the vertexFetch[] table (x=buffer select, y=base byte
 * offset, z=stride, w=VERTEX_FETCH_* bits). Loads go through an unaligned
 * dword helper because NV2A strides and offsets are only element-aligned.
 *
 * .w bit 0 is set when base and stride are dword aligned, so a single wide
 * raw load replaces the per-dword unaligned assembly. The remaining bits
 * describe the attribute format, which the shader decodes at run time
 * unless the static tune switch bakes it in.
 */
#define VERTEX_FETCH_ALIGNED        (1u << 0)
#define VERTEX_FETCH_DWORDS_SHIFT   1       /* dwords loaded - 1 */
#define VERTEX_FETCH_KIND_SHIFT     4       /* VertexFetchKind */
#define VERTEX_FETCH_KIND_MASK      7u
#define VERTEX_FETCH_COUNT_SHIFT    8       /* components - 1 */
#define VERTEX_FETCH_SWIZZLE        (1u << 12)  /* BGRA */

typedef enum VertexFetchKind {
    VERTEX_FETCH_KIND_NONE,     /* float4(0, 0, 0, 1), nothing loaded */
    VERTEX_FETCH_KIND_FLOAT,
    VERTEX_FETCH_KIND_UNORM8,
    VERTEX_FETCH_KIND_SNORM16,
    VERTEX_FETCH_KIND_CMP,      /* 11:11:10 */
    VERTEX_FETCH_KIND_SINT,
} VertexFetchKind;

/* The key-only check lets PSO workers decide from the key alone. */
static bool pipeline_key_dynamic_vertex_formats(
    const PGRAPHD3D12PipelineKey *key)
{
    return pgraph_d3d12_vertex_pulling() &&
           !(key->tune_flags & PGRAPH_D3D12_TUNE_VTXFMT_STATIC);
}

/*
 * vertexFetch[].w format bits for a pulled attribute. Precedence and load
 * widths match the static decode in append_vertex_fetch_function() and
 * append_vertex_value_expr(); never load more, the vertex SRVs are
 * unbounded root descriptors.
 */
static uint32_t vertex_fetch_format_bits(DXGI_FORMAT format, bool compressed,
                                         bool swizzle)
{
    VertexFetchKind kind;
    unsigned int dwords = 1;
    unsigned int count = 4;

    if (compressed) {
        kind = VERTEX_FETCH_KIND_CMP;
        swizzle = false;
    } else if (dxgi_format_is_signed_integer(format)) {
        kind = VERTEX_FETCH_KIND_SINT;
        swizzle = false;
    } else {
        switch (format) {
        case DXGI_FORMAT_R32_FLOAT:
            kind = VERTEX_FETCH_KIND_FLOAT;
            dwords = count = 1;
            break;
        case DXGI_FORMAT_R32G32_FLOAT:
            kind = VERTEX_FETCH_KIND_FLOAT;
            dwords = count = 2;
            break;
        case DXGI_FORMAT_R32G32B32_FLOAT:
            kind = VERTEX_FETCH_KIND_FLOAT;
            dwords = count = 3;
            break;
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
            kind = VERTEX_FETCH_KIND_FLOAT;
            dwords = count = 4;
            break;
        case DXGI_FORMAT_R8_UNORM:
            kind = VERTEX_FETCH_KIND_UNORM8;
            count = 1;
            break;
        case DXGI_FORMAT_R8G8_UNORM:
            kind = VERTEX_FETCH_KIND_UNORM8;
            count = 2;
            break;
        case DXGI_FORMAT_R8G8B8A8_UNORM:
            kind = VERTEX_FETCH_KIND_UNORM8;
            break;
        case DXGI_FORMAT_R16_SNORM:
            kind = VERTEX_FETCH_KIND_SNORM16;
            count = 1;
            break;
        case DXGI_FORMAT_R16G16_SNORM:
            kind = VERTEX_FETCH_KIND_SNORM16;
            count = 2;
            break;
        case DXGI_FORMAT_R16G16B16A16_SNORM:
            kind = VERTEX_FETCH_KIND_SNORM16;
            dwords = 2;
            break;
        default:
            /* float4(0, 0, 0, 1) is its own BGRA swizzle. */
            return 0;
        }
    }

    return ((dwords - 1) << VERTEX_FETCH_DWORDS_SHIFT) |
           ((uint32_t)kind << VERTEX_FETCH_KIND_SHIFT) |
           ((count - 1) << VERTEX_FETCH_COUNT_SHIFT) |
           (swizzle ? VERTEX_FETCH_SWIZZLE : 0);
}

/*
 * Run-time format decode for pulled attributes; the same expressions as the
 * static per-format code, so the fetched values are identical.
 */
static void append_vertex_fetch_dynamic_helpers(GString *source)
{
    static const char *const buffers[] = { "vertexScratch", "vertexRam" };

    g_string_append(source,
                    "uint4 load_vertex_words(uint sel, uint addr, "
                    "uint dwords)\n"
                    "{\n"
                    "    uint4 w = uint4(0u, 0u, 0u, 0u);\n");
    for (unsigned int i = 0; i < ARRAY_SIZE(buffers); i++) {
        g_string_append(source, i == 0 ? "    [branch] if (sel != 0u) {\n" :
                                         "    } else {\n");
        g_string_append_printf(source,
                               "        [forcecase] switch (dwords) {\n"
                               "        case 1:\n"
                               "            w.x = %s.Load(addr);\n"
                               "            break;\n"
                               "        case 2:\n"
                               "            w.xy = %s.Load2(addr);\n"
                               "            break;\n"
                               "        case 3:\n"
                               "            w.xyz = %s.Load3(addr);\n"
                               "            break;\n"
                               "        default:\n"
                               "            w = %s.Load4(addr);\n"
                               "            break;\n"
                               "        }\n",
                               buffers[i], buffers[i], buffers[i], buffers[i]);
    }
    g_string_append(source,
                    "    }\n"
                    "    return w;\n"
                    "}\n");

    g_string_append_printf(
        source,
        "float4 fetch_attribute(uint4 f, uint vid)\n"
        "{\n"
        "    uint kind = (f.w >> %uu) & %uu;\n"
        "    float4 r = float4(0.0, 0.0, 0.0, 1.0);\n"
        "    [branch] if (kind != 0u) {\n"
        "        uint dwords = ((f.w >> %uu) & 3u) + 1u;\n"
        "        uint count = ((f.w >> %uu) & 3u) + 1u;\n"
        "        uint sel = f.x;\n"
        "        uint addr = f.y + vid * f.z;\n"
        "        uint4 w;\n"
        "        [branch] if ((f.w & %uu) != 0u) {\n"
        "            w = load_vertex_words(sel, addr, dwords);\n"
        "        } else {\n"
        "            w = uint4(fetch_u32(sel, addr), 0u, 0u, 0u);\n"
        "            [branch] if (dwords >= 2u) {\n"
        "                w.y = fetch_u32(sel, addr + 4u);\n"
        "            }\n"
        "            [branch] if (dwords >= 3u) {\n"
        "                w.z = fetch_u32(sel, addr + 8u);\n"
        "            }\n"
        "            [branch] if (dwords >= 4u) {\n"
        "                w.w = fetch_u32(sel, addr + 12u);\n"
        "            }\n"
        "        }\n"
        "        [forcecase] switch (kind) {\n"
        "        case %u:\n"
        "            r = float4(asfloat(w.x), asfloat(w.y), asfloat(w.z), "
        "asfloat(w.w));\n"
        "            break;\n"
        "        case %u:\n"
        "            r = float4((float)(w.x & 0xffu) / 255.0,\n"
        "                       (float)((w.x >> 8u) & 0xffu) / 255.0,\n"
        "                       (float)((w.x >> 16u) & 0xffu) / 255.0,\n"
        "                       (float)((w.x >> 24u) & 0xffu) / 255.0);\n"
        "            break;\n"
        "        case %u:\n"
        "            r = float4(snorm16(w.x), snorm16(w.x >> 16u), "
        "snorm16(w.y),\n"
        "                       snorm16(w.y >> 16u));\n"
        "            break;\n"
        "        case %u:\n"
        "            r = decompress_11_11_10(asint(w.x));\n"
        "            break;\n"
        "        case %u:\n"
        "            r = (float4)int4(asint(w.x), 0, 0, 1);\n"
        "            break;\n"
        "        default:\n"
        "            break;\n"
        "        }\n"
        "        [flatten] if (count < 2u) {\n"
        "            r.y = 0.0;\n"
        "        }\n"
        "        [flatten] if (count < 3u) {\n"
        "            r.z = 0.0;\n"
        "        }\n"
        "        [flatten] if (count < 4u) {\n"
        "            r.w = 1.0;\n"
        "        }\n"
        "        [flatten] if ((f.w & %uu) != 0u) {\n"
        "            r = r.bgra;\n"
        "        }\n"
        "    }\n"
        "    return r;\n"
        "}\n",
        VERTEX_FETCH_KIND_SHIFT, VERTEX_FETCH_KIND_MASK,
        VERTEX_FETCH_DWORDS_SHIFT, VERTEX_FETCH_COUNT_SHIFT,
        VERTEX_FETCH_ALIGNED,
        VERTEX_FETCH_KIND_FLOAT, VERTEX_FETCH_KIND_UNORM8,
        VERTEX_FETCH_KIND_SNORM16, VERTEX_FETCH_KIND_CMP,
        VERTEX_FETCH_KIND_SINT, VERTEX_FETCH_SWIZZLE);
}

static void append_vertex_fetch_component_loads(GString *source,
                                                unsigned int dwords)
{
    g_string_append(source,
                    "        uint w0, w1, w2, w3;\n"
                    "        w1 = w2 = w3 = 0u;\n"
                    "        [branch] if (aligned != 0u) {\n");
    switch (dwords) {
    case 1:
        g_string_append(source,
                        "            [branch] if (sel != 0u) {\n"
                        "                w0 = vertexScratch.Load(addr);\n"
                        "            } else {\n"
                        "                w0 = vertexRam.Load(addr);\n"
                        "            }\n");
        break;
    case 2:
        g_string_append(source,
                        "            uint2 wv;\n"
                        "            [branch] if (sel != 0u) {\n"
                        "                wv = vertexScratch.Load2(addr);\n"
                        "            } else {\n"
                        "                wv = vertexRam.Load2(addr);\n"
                        "            }\n"
                        "            w0 = wv.x; w1 = wv.y;\n");
        break;
    case 3:
        g_string_append(source,
                        "            uint3 wv;\n"
                        "            [branch] if (sel != 0u) {\n"
                        "                wv = vertexScratch.Load3(addr);\n"
                        "            } else {\n"
                        "                wv = vertexRam.Load3(addr);\n"
                        "            }\n"
                        "            w0 = wv.x; w1 = wv.y; w2 = wv.z;\n");
        break;
    default:
        g_string_append(source,
                        "            uint4 wv;\n"
                        "            [branch] if (sel != 0u) {\n"
                        "                wv = vertexScratch.Load4(addr);\n"
                        "            } else {\n"
                        "                wv = vertexRam.Load4(addr);\n"
                        "            }\n"
                        "            w0 = wv.x; w1 = wv.y; w2 = wv.z; "
                        "w3 = wv.w;\n");
        break;
    }
    g_string_append(source,
                    "        } else {\n"
                    "            w0 = fetch_u32(sel, addr);\n");
    if (dwords >= 2) {
        g_string_append(source,
                        "            w1 = fetch_u32(sel, addr + 4u);\n");
    }
    if (dwords >= 3) {
        g_string_append(source,
                        "            w2 = fetch_u32(sel, addr + 8u);\n");
    }
    if (dwords >= 4) {
        g_string_append(source,
                        "            w3 = fetch_u32(sel, addr + 12u);\n");
    }
    g_string_append(source, "        }\n");
}

/* The vertex SRVs and the unaligned load helpers of vertex pulling. */
static void append_vertex_fetch_buffers(GString *source)
{
    g_string_append_printf(
        source,
        "ByteAddressBuffer vertexRam : register(t%u);\n"
        "ByteAddressBuffer vertexScratch : register(t%u);\n"
        "uint load_vertex_u32(uint sel, uint addr)\n"
        "{\n"
        "    uint v;\n"
        "    [branch] if (sel != 0u) {\n"
        "        v = vertexScratch.Load(addr);\n"
        "    } else {\n"
        "        v = vertexRam.Load(addr);\n"
        "    }\n"
        "    return v;\n"
        "}\n"
        "uint fetch_u32(uint sel, uint addr)\n"
        "{\n"
        "    uint aligned = addr & ~3u;\n"
        "    uint shift = (addr & 3u) * 8u;\n"
        "    uint lo = load_vertex_u32(sel, aligned);\n"
        "    if (shift == 0u) {\n"
        "        return lo;\n"
        "    }\n"
        "    uint hi = load_vertex_u32(sel, aligned + 4u);\n"
        "    return (lo >> shift) | (hi << (32u - shift));\n"
        "}\n"
        "float snorm16(uint bits)\n"
        "{\n"
        "    int v = asint(bits << 16u) >> 16u;\n"
        "    return max((float)v / 32767.0, -1.0);\n"
        "}\n",
        PGRAPH_D3D12_VERTEX_RAM_SRV_REGISTER,
        PGRAPH_D3D12_VERTEX_SCRATCH_SRV_REGISTER);
}

static void append_vertex_fetch_function(GString *source,
                                         const PGRAPHD3D12PipelineKey *key)
{
    append_vertex_fetch_buffers(source);

    if (pipeline_key_dynamic_vertex_formats(key)) {
        /* One copy of the decode, looped: a copy per attribute multiplies
         * the compile time and the bytecode each PSO hands the driver. The
         * switch stores keep VSInput out of an indexable array. Without
         * [fastopt], FXC's loop simulation rejects short loops over the
         * branched buffer loads with a spurious X4014. */
        append_vertex_fetch_dynamic_helpers(source);
        g_string_append(source,
                        "VSInput fetch_vertex(uint vid)\n"
                        "{\n"
                        "    VSInput v = (VSInput)0;\n");
        if (key->num_attributes != 0) {
            g_string_append_printf(source,
                                   "    static const uint attrs[%u] = {",
                                   key->num_attributes);
            for (uint8_t i = 0; i < key->num_attributes; i++) {
                g_string_append_printf(source, "%s %uu", i ? "," : "",
                                       key->attribute_indices[i]);
            }
            g_string_append_printf(
                source,
                " };\n"
                "    [fastopt] [loop] for (uint i = 0u; i < %uu; i++) {\n"
                "        float4 value = "
                "fetch_attribute(vertexFetch[attrs[i]], vid);\n"
                "        [forcecase] switch (i) {\n",
                key->num_attributes);
            for (uint8_t i = 0; i < key->num_attributes; i++) {
                g_string_append_printf(source,
                                       "        case %u:\n"
                                       "            v.a%u = value;\n"
                                       "            break;\n",
                                       i, key->attribute_indices[i]);
            }
            g_string_append(source,
                            "        default:\n"
                            "            break;\n"
                            "        }\n"
                            "    }\n");
        }
        g_string_append(source,
                        "    return v;\n"
                        "}\n");
        return;
    }

    g_string_append(source,
                    "VSInput fetch_vertex(uint vid)\n"
                    "{\n"
                    "    VSInput v = (VSInput)0;\n");
    for (uint8_t i = 0; i < key->num_attributes; i++) {
        uint8_t attr = key->attribute_indices[i];
        DXGI_FORMAT format = pipeline_key_attribute_format(key, attr);

        g_string_append_printf(
            source,
            "    {\n"
            "        uint sel = vertexFetch[%u].x;\n"
            "        uint addr = vertexFetch[%u].y + vid * vertexFetch[%u].z;\n"
            "        uint aligned = vertexFetch[%u].w & 1u;\n",
            attr, attr, attr, attr);

        if (key->compressed_attrs & (1u << attr)) {
            append_vertex_fetch_component_loads(source, 1);
            g_string_append_printf(source,
                                   "        v.a%u_cmp = asint(w0);\n", attr);
        } else if (dxgi_format_is_signed_integer(format)) {
            append_vertex_fetch_component_loads(source, 1);
            g_string_append_printf(
                source, "        v.a%u_i = int4(asint(w0), 0, 0, 1);\n",
                attr);
        } else {
            switch (format) {
            case DXGI_FORMAT_R32_FLOAT:
                append_vertex_fetch_component_loads(source, 1);
                g_string_append_printf(
                    source,
                    "        v.a%u = float4(asfloat(w0), 0.0, 0.0, 1.0);\n",
                    attr);
                break;
            case DXGI_FORMAT_R32G32_FLOAT:
                append_vertex_fetch_component_loads(source, 2);
                g_string_append_printf(
                    source,
                    "        v.a%u = float4(asfloat(w0), asfloat(w1), "
                    "0.0, 1.0);\n",
                    attr);
                break;
            case DXGI_FORMAT_R32G32B32_FLOAT:
                append_vertex_fetch_component_loads(source, 3);
                g_string_append_printf(
                    source,
                    "        v.a%u = float4(asfloat(w0), asfloat(w1), "
                    "asfloat(w2), 1.0);\n",
                    attr);
                break;
            case DXGI_FORMAT_R32G32B32A32_FLOAT:
                append_vertex_fetch_component_loads(source, 4);
                g_string_append_printf(
                    source,
                    "        v.a%u = float4(asfloat(w0), asfloat(w1), "
                    "asfloat(w2), asfloat(w3));\n",
                    attr);
                break;
            case DXGI_FORMAT_R8_UNORM:
                append_vertex_fetch_component_loads(source, 1);
                g_string_append_printf(
                    source,
                    "        v.a%u = float4((float)(w0 & 0xffu) / 255.0, "
                    "0.0, 0.0, 1.0);\n",
                    attr);
                break;
            case DXGI_FORMAT_R8G8_UNORM:
                append_vertex_fetch_component_loads(source, 1);
                g_string_append_printf(
                    source,
                    "        v.a%u = float4((float)(w0 & 0xffu) / 255.0, "
                    "(float)((w0 >> 8u) & 0xffu) / 255.0, 0.0, 1.0);\n",
                    attr);
                break;
            case DXGI_FORMAT_R8G8B8A8_UNORM:
                append_vertex_fetch_component_loads(source, 1);
                g_string_append_printf(
                    source,
                    "        v.a%u = float4((float)(w0 & 0xffu) / 255.0, "
                    "(float)((w0 >> 8u) & 0xffu) / 255.0, "
                    "(float)((w0 >> 16u) & 0xffu) / 255.0, "
                    "(float)((w0 >> 24u) & 0xffu) / 255.0);\n",
                    attr);
                break;
            case DXGI_FORMAT_R16_SNORM:
                append_vertex_fetch_component_loads(source, 1);
                g_string_append_printf(
                    source,
                    "        v.a%u = float4(snorm16(w0), 0.0, 0.0, 1.0);\n",
                    attr);
                break;
            case DXGI_FORMAT_R16G16_SNORM:
                append_vertex_fetch_component_loads(source, 1);
                g_string_append_printf(
                    source,
                    "        v.a%u = float4(snorm16(w0), "
                    "snorm16(w0 >> 16u), 0.0, 1.0);\n",
                    attr);
                break;
            case DXGI_FORMAT_R16G16B16A16_SNORM:
                append_vertex_fetch_component_loads(source, 2);
                g_string_append_printf(
                    source,
                    "        v.a%u = float4(snorm16(w0), "
                    "snorm16(w0 >> 16u), snorm16(w1), "
                    "snorm16(w1 >> 16u));\n",
                    attr);
                break;
            default:
                g_string_append_printf(
                    source,
                    "        v.a%u = float4(0.0, 0.0, 0.0, 1.0);\n", attr);
                break;
            }
        }
        g_string_append(source, "    }\n");
    }

    g_string_append(source,
                    "    return v;\n"
                    "}\n");
}

static bool draw_geometry_shader_needed(const PGRAPHD3D12PipelineKey *key);
static const char *rasterized_depth_pos_expr(
    const PGRAPHD3D12PipelineKey *key, bool last_stage, const char *value);

/* The fixed-function point size, with or without point parameters. */
static void append_fixed_function_point_size(GString *source, bool params,
                                             const char *name)
{
    if (params) {
        g_string_append_printf(
            source,
            "float %s(float4 position)\n"
            "{\n"
            "    float d_e = length(mul_vec_mat4(position,\n"
            "                                     D3D12_NV2A_MMAT0));\n"
            "    float ptMinSize = min(point_param(7), 63.875);\n"
            "    float ptMaxSize = min(point_param(3) + ptMinSize,\n"
            "                          63.875);\n"
            "    float pointSize = rsqrt(point_param(0) +\n"
            "                            point_param(1) * d_e +\n"
            "                            point_param(2) * d_e * d_e) +\n"
            "                      point_param(6);\n"
            "    return clamp(pointSize * point_param(3) + point_param(7),\n"
            "                 ptMinSize, ptMaxSize);\n"
            "}\n", name);
    } else {
        g_string_append_printf(source,
                        "float %s(float4 position)\n"
                        "{\n"
                        "    return max(surfaceInfo.w, 1.0);\n"
                        "}\n", name);
    }
}

/* Helpers every vertex shader shares: NaN handling, matrices, lights and
 * the position transforms. */
static void append_vertex_common_helpers(GString *source)
{
    append_float_class_helpers(source);
    g_string_append(source,
                    "float clamp_away_zero_inf(float t)\n"
                    "{\n"
                    "    if (t > 0.0 || asuint(t) == 0u) {\n"
                    "        return clamp(t, asfloat(0x1f800000),\n"
                    "                     asfloat(0x5f800000));\n"
                    "    }\n"
                    "    return clamp(t, asfloat(0xdf800000),\n"
                    "                 asfloat(0x9f800000));\n"
                    "}\n"
                    "float2 round_screen_coords(float2 pos)\n"
                    "{\n"
                    "    return trunc(pos * 16.0) / 16.0;\n"
                    "}\n"
                    "float nan_scalar_to_value(float v, float fallback)\n"
                    "{\n"
                    "    return float_is_nan(v) ? fallback : v;\n"
                    "}\n"
                    "float nan_scalar_to_one(float v)\n"
                    "{\n"
                    "    return nan_scalar_to_value(v, 1.0);\n"
                    "}\n"
                    "float4 nan_to_one(float4 v)\n"
                    "{\n"
                    "    return float4(nan_scalar_to_one(v.x),\n"
                    "                  nan_scalar_to_one(v.y),\n"
                    "                  nan_scalar_to_one(v.z),\n"
                    "                  nan_scalar_to_one(v.w));\n"
                    "}\n"
                    "float4 mul_vec_mat4(float4 v, uint base)\n"
                    "{\n"
                    "    return float4(dot(v, c[base + 0]),\n"
                    "                  dot(v, c[base + 1]),\n"
                    "                  dot(v, c[base + 2]),\n"
                    "                  dot(v, c[base + 3]));\n"
                    "}\n"
                    "float point_param(uint i)\n"
                    "{\n"
                    "    return pointParams[i >> 2][i & 3];\n"
                    "}\n"
                    "float3 light_ambient_color(uint i)\n"
                    "{\n"
                    "    return ltctxb[D3D12_NV2A_LTCTXB_L0_AMB + i * 6].xyz;\n"
                    "}\n"
                    "float3 light_diffuse_color(uint i)\n"
                    "{\n"
                    "    return ltctxb[D3D12_NV2A_LTCTXB_L0_DIF + i * 6].xyz;\n"
                    "}\n"
                    "float3 light_specular_color(uint i)\n"
                    "{\n"
                    "    return ltctxb[D3D12_NV2A_LTCTXB_L0_SPC + i * 6].xyz;\n"
                    "}\n"
                    "float4 light_spot_direction(uint i)\n"
                    "{\n"
                    "    return ltctxa[D3D12_NV2A_LTCTXA_L0_SPT + i * 2];\n"
                    "}\n"
                    "float light_local_range(uint i)\n"
                    "{\n"
                    "    return ltc1[D3D12_NV2A_LTC1_R0 + i].x;\n"
                    "}\n"
                    "float clamp_fog_interpolant(float v)\n"
                    "{\n"
                    "    return clamp(v, -asfloat(0x7f7fffff),\n"
                    "                 asfloat(0x7f7fffff));\n"
                    "}\n"
                    "float4 fixed_function_position(float4 position)\n"
                    "{\n"
                    "    float4 oPos = mul_vec_mat4(position,\n"
                    "                                D3D12_NV2A_CMAT0);\n"
                    "    oPos.w = clamp_away_zero_inf(oPos.w);\n"
                    "    oPos.xy /= oPos.w;\n"
                    "    oPos.xy += c[D3D12_NV2A_VPOFF].xy;\n"
                    "    oPos.xy = round_screen_coords(oPos.xy);\n"
                    "    float2 surfaceSize = max(surfaceInfo.xy,\n"
                    "                             float2(1.0, 1.0));\n"
                    "    oPos.z = oPos.z / max(clipRange.y, 1.0);\n"
                    "    oPos.x = (2.0 * oPos.x - surfaceSize.x) /\n"
                    "             surfaceSize.x;\n"
                    "    oPos.y = (surfaceSize.y - 2.0 * oPos.y) /\n"
                    "             surfaceSize.y;\n"
                    "    oPos.xy *= oPos.w;\n"
                    "    return oPos;\n"
                    "}\n"
                    "float4 fixed_function_depth_position(float4 position)\n"
                    "{\n"
                    "    float4 oPos = mul_vec_mat4(position,\n"
                    "                                D3D12_NV2A_CMAT0);\n"
                    "    oPos.w = clamp_away_zero_inf(oPos.w);\n"
                    "    oPos.xy /= oPos.w;\n"
                    "    oPos.xy += c[D3D12_NV2A_VPOFF].xy;\n"
                    "    oPos.xy = round_screen_coords(oPos.xy);\n"
                    "    return float4(oPos.xy, oPos.z / oPos.w, oPos.w);\n"
                    "}\n"
                    "float4 programmable_screen_position(float4 position)\n"
                    "{\n"
                    "    float4 oPos = position;\n"
                    "    oPos.w = clamp_away_zero_inf(oPos.w);\n"
                    "    oPos.xy = round_screen_coords(oPos.xy);\n"
                    "    float2 surfaceSize = max(surfaceInfo.xy,\n"
                    "                             float2(1.0, 1.0));\n"
                    "    oPos.x = (2.0 * oPos.x - surfaceSize.x) /\n"
                    "             surfaceSize.x;\n"
                    "    oPos.y = (surfaceSize.y - 2.0 * oPos.y) /\n"
                    "             surfaceSize.y;\n"
                    "    oPos.z = oPos.z / max(clipRange.y, 1.0);\n"
                    "    oPos.xyz *= oPos.w;\n"
                    "    return oPos;\n"
                    "}\n"
                    "float4 programmable_depth_position(float4 position)\n"
                    "{\n"
                    "    float4 oPos = position;\n"
                    "    oPos.w = clamp_away_zero_inf(oPos.w);\n"
                    "    oPos.xy = round_screen_coords(oPos.xy);\n"
                    "    return oPos;\n"
                    "}\n"
                    "float4 programmable_position_fallback(float4 position)\n"
                    "{\n"
                    "    return programmable_screen_position(position);\n"
                    "}\n"
                    "float4 programmable_depth_position_fallback(float4 position)\n"
                    "{\n"
                    "    return programmable_depth_position(position);\n"
                    "}\n"
                    "float4 vertex_output_position(float4 position)\n"
                    "{\n"
                    "#if D3D12_NV2A_VSH_MODE == 0\n"
                    "    return fixed_function_position(position);\n"
                    "#else\n"
                    "    return programmable_position_fallback(position);\n"
                    "#endif\n"
                    "}\n"
                    "float4 vertex_output_depth_position(float4 position)\n"
                    "{\n"
                    "#if D3D12_NV2A_VSH_MODE == 0\n"
                    "    return fixed_function_depth_position(position);\n"
                    "#else\n"
                    "    return programmable_depth_position_fallback(position);\n"
                    "#endif\n"
                    "}\n");
}

/* The 11:11:10 normal decode. */
static void append_vertex_decompress_helpers(GString *source)
{
    g_string_append(source,
                    "int sign_extend_bits(int value, int bits)\n"
                    "{\n"
                    "    int shift = 32 - bits;\n"
                    "    return (value << shift) >> shift;\n"
                    "}\n"
                    "float4 decompress_11_11_10(int cmp)\n"
                    "{\n"
                    "    int x = sign_extend_bits(cmp & 0x7ff, 11);\n"
                    "    int y = sign_extend_bits((cmp >> 11) & 0x7ff, 11);\n"
                    "    int z = sign_extend_bits((cmp >> 22) & 0x3ff, 10);\n"
                    "    return float4((float)x / 1023.0, (float)y / 1023.0,\n"
                    "                  (float)z / 511.0, 1.0);\n"
                    "}\n");
}

/* The transform context offsets the vertex shaders use. */
static void append_vertex_defines(GString *source, unsigned int vsh_mode)
{
    g_string_append_printf(source,
                           "#define D3D12_NV2A_VSH_MODE %u\n"
                           "#define D3D12_NV2A_MMAT0 %u\n"
                           "#define D3D12_NV2A_CMAT0 %u\n"
                           "#define D3D12_NV2A_IMMAT0 %u\n"
                           "#define D3D12_NV2A_EYEP %u\n"
                           "#define D3D12_NV2A_FOGPLANE %u\n"
                           "#define D3D12_NV2A_VPOFF %u\n"
                           "#define D3D12_NV2A_LTCTXA_FR_AMB %u\n"
                           "#define D3D12_NV2A_LTCTXA_CM_COL %u\n"
                           "#define D3D12_NV2A_LTCTXA_L0_K %u\n"
                           "#define D3D12_NV2A_LTCTXA_L0_SPT %u\n"
                           "#define D3D12_NV2A_LTCTXB_L0_AMB %u\n"
                           "#define D3D12_NV2A_LTCTXB_L0_DIF %u\n"
                           "#define D3D12_NV2A_LTCTXB_L0_SPC %u\n"
                           "#define D3D12_NV2A_LTC1_R0 %u\n",
                           vsh_mode,
                           NV_IGRAPH_XF_XFCTX_MMAT0,
                           NV_IGRAPH_XF_XFCTX_CMAT0,
                           NV_IGRAPH_XF_XFCTX_IMMAT0,
                           NV_IGRAPH_XF_XFCTX_EYEP,
                           NV_IGRAPH_XF_XFCTX_FOG,
                           NV_IGRAPH_XF_XFCTX_VPOFF,
                           NV_IGRAPH_XF_LTCTXA_FR_AMB,
                           NV_IGRAPH_XF_LTCTXA_CM_COL,
                           NV_IGRAPH_XF_LTCTXA_L0_K,
                           NV_IGRAPH_XF_LTCTXA_L0_SPT,
                           NV_IGRAPH_XF_LTCTXB_L0_AMB,
                           NV_IGRAPH_XF_LTCTXB_L0_DIF,
                           NV_IGRAPH_XF_LTCTXB_L0_SPC,
                           NV_IGRAPH_XF_LTC1_r0);
}

/* b0 as the vertex shaders declare it, PGRAPHD3D12DrawConstants. */
static const char vertex_draw_constants_hlsl[] =
        "cbuffer DrawConstants : register(b0) {\n"
        "    float4 inlineValue[16];\n"
        "    float4 c[192];\n"
        "    float4 clipRange;\n"
        "    float4 surfaceInfo;\n"
        "    float4 pointParams[2];\n"
        "    float4 fogParam;\n"
        "    float4 ltctxa[26];\n"
        "    float4 ltctxb[52];\n"
        "    float4 ltc1[20];\n"
        "    float4 lightInfiniteDirection[8];\n"
        "    float4 lightInfiniteHalfVector[8];\n"
        "    float4 lightLocalPosition[8];\n"
        "    float4 lightLocalAttenuation[8];\n"
        "    float4 fixedFunctionInfo;\n"
        "    uint4 vertexFetch[16];\n"
        "};\n"
        "\n";

static char *build_minimal_vertex_shader(
    const PGRAPHD3D12PipelineKey *key)
{
    GString *source = g_string_new(vertex_draw_constants_hlsl);
    g_string_append_printf(
        source, "#define VS_DEPTH_POS %s\n",
        rasterized_depth_pos_expr(key, !draw_geometry_shader_needed(key),
                                  "depthPos"));

    append_vertex_defines(source, key->vertex_shader_mode);
    g_string_append(source, "struct VSInput {\n");

    for (uint8_t i = 0; i < key->num_attributes; i++) {
        uint8_t attr = key->attribute_indices[i];

        if (pipeline_key_dynamic_vertex_formats(key)) {
            /* fetch_attribute() returns the decoded value. */
            g_string_append_printf(source, "    float4 a%u;\n", attr);
        } else if (key->compressed_attrs & (1u << attr)) {
            g_string_append_printf(source,
                                   pgraph_d3d12_vertex_pulling() ?
                                       "    int a%u_cmp;\n" :
                                       "    int a%u_cmp : TEXCOORD%u;\n",
                                   attr, attr);
        } else if (dxgi_format_is_signed_integer(
                       pipeline_key_attribute_format(key, attr))) {
            g_string_append_printf(source,
                                   pgraph_d3d12_vertex_pulling() ?
                                       "    int4 a%u_i;\n" :
                                       "    int4 a%u_i : TEXCOORD%u;\n",
                                   attr, attr);
        } else {
            g_string_append_printf(source,
                                   pgraph_d3d12_vertex_pulling() ?
                                       "    float4 a%u;\n" :
                                       "    float4 a%u : TEXCOORD%u;\n",
                                   attr, attr);
        }
    }
    g_string_append(source, "};\n");
    /* Ahead of fetch_vertex(), which decodes compressed attributes when
     * the format is dynamic. */
    append_vertex_decompress_helpers(source);

    if (pgraph_d3d12_vertex_pulling()) {
        append_vertex_fetch_function(source, key);
    }

    g_string_append(source,
                    "struct VSOutput {\n"
                    "    float4 position : SV_Position;\n"
                    "    float4 diffuse : TEXCOORD0;\n"
                    "    float4 specular : TEXCOORD1;\n"
                    "    float fog : TEXCOORD2;\n"
                    "    float pointSize : TEXCOORD7;\n");
    for (unsigned int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        if (key->texture_coord_mask & (1u << i)) {
            g_string_append_printf(source,
                                   "    float4 tex%u : TEXCOORD%u;\n",
                                   i, i + 3);
        }
    }
    g_string_append(source,
                    "    float4 depthPos : TEXCOORD8;\n"
                    "};\n");
    append_vertex_common_helpers(source);
    append_fixed_function_point_size(source, key->point_params_enable,
                                     "fixed_function_point_size");
    /* A vertex program without point params takes the register size as
     * is; only a zero size becomes one pixel. */
    g_string_append(source,
                    "float programmable_point_size()\n"
                    "{\n"
                    "    return surfaceInfo.w > 0.0 ? surfaceInfo.w : 1.0;\n"
                    "}\n");
    if (key->vertex_shader_mode != 0) {
        append_programmable_vsh_helpers(source);
    }

    if (pgraph_d3d12_vertex_pulling()) {
        g_string_append(source,
                        "cbuffer DrawVertexStart : register(b2) {\n"
                        "    uint vertexStart;\n"
                        "};\n"
                        "VSOutput vs_main(uint vertexID : SV_VertexID)\n"
                        "{\n"
                        "    VSInput input = fetch_vertex(vertexID + "
                        "vertexStart);\n"
                        "    VSOutput output;\n");
    } else {
        g_string_append(source,
                        "VSOutput vs_main(VSInput input)\n"
                        "{\n"
                        "    VSOutput output;\n");
    }
    if (key->vertex_shader_mode != 0 && key->transform_program_length > 0) {
        append_programmable_vsh_body(source, key);
    } else {
        g_string_append(source, "    float4 position = ");
        append_vertex_value_expr(source, key, NV2A_VERTEX_ATTR_POSITION);
        g_string_append(source, ";\n");
        if (key->vertex_shader_mode == 0) {
            g_string_append(source, "    float3 normal = (");
            if (pipeline_key_has_vertex_value(key, NV2A_VERTEX_ATTR_NORMAL)) {
                append_vertex_value_expr(source, key, NV2A_VERTEX_ATTR_NORMAL);
            } else {
                g_string_append(source, "float4(0.0, 0.0, 1.0, 0.0)");
            }
            g_string_append(source, ").xyz;\n");
            append_fixed_function_skinning(source, key);
            if (key->fixed_function_normalization) {
                g_string_append(source,
                                "    tNormal = normalize(tNormal);\n");
            }
            /* With skinning the composite matrix is projection only;
             * otherwise it includes the modelview. */
            g_string_append_printf(
                source, "    float4 clipInput = %s;\n",
                key->fixed_function_skinning != 0 ? "tPosition" : "position");
            g_string_append(source,
                            "    precise float4 depthPos = "
                            "vertex_output_depth_position(clipInput);\n"
                            "    precise float4 screenPos = "
                            "vertex_output_position(clipInput);\n"
                            "    precise float4 rasterDepthPos = VS_DEPTH_POS;\n"
                    "    output.depthPos = rasterDepthPos;\n"
                            "    output.position = screenPos;\n"
                            "    output.pointSize = "
                            "fixed_function_point_size(position);\n");
        } else {
            g_string_append(source,
                            "    precise float4 depthPos = "
                            "vertex_output_depth_position(position);\n"
                            "    precise float4 screenPos = "
                            "vertex_output_position(position);\n"
                            "    precise float4 rasterDepthPos = VS_DEPTH_POS;\n"
                    "    output.depthPos = rasterDepthPos;\n"
                            "    output.position = screenPos;\n"
                            "    output.pointSize = "
                            "programmable_point_size();\n");
        }
        g_string_append(source, "    float4 diffuse = ");
        if (pipeline_key_has_vertex_value(key, NV2A_VERTEX_ATTR_DIFFUSE)) {
            append_vertex_value_expr(source, key, NV2A_VERTEX_ATTR_DIFFUSE);
        } else if (key->texture_mask) {
            g_string_append(source, "float4(1.0, 1.0, 1.0, 1.0)");
        } else {
            g_string_append(source, "float4(0.0, 0.0, 0.0, 1.0)");
        }
        g_string_append(source, ";\n");
        g_string_append(source, "    float4 specular = ");
        if (pipeline_key_has_vertex_value(key, NV2A_VERTEX_ATTR_SPECULAR)) {
            append_vertex_value_expr(source, key, NV2A_VERTEX_ATTR_SPECULAR);
        } else {
            g_string_append(source, "float4(0.0, 0.0, 0.0, 1.0)");
        }
        g_string_append(source, ";\n");
        if (key->vertex_shader_mode == 0) {
            append_fixed_function_lighting_output(source, key);
        } else {
            g_string_append(source,
                            "    output.diffuse = saturate(nan_to_one(diffuse));\n"
                            "    output.specular = saturate(nan_to_one(specular));\n");
            if (!key->specular_enable) {
                g_string_append(
                    source,
                    "    output.specular = float4(0.0, 0.0, 0.0, 1.0);\n");
            } else if (key->ignore_specular_alpha) {
                g_string_append(source, "    output.specular.a = 1.0;\n");
            }
        }
        if (key->vertex_shader_mode == 0) {
            switch (key->fixed_function_foggen) {
            case FOGGEN_SPEC_ALPHA:
                append_fog_output_from_distance(
                    source, key, "clamp(specular.a, 0.0, 1.0)");
                break;
            case FOGGEN_RADIAL:
                append_fog_output_from_distance(source, key,
                                                "length(tPosition.xyz)");
                break;
            case FOGGEN_PLANAR:
                append_fog_output_from_distance(
                    source, key,
                    "dot(c[D3D12_NV2A_FOGPLANE].xyz, tPosition.xyz) + "
                    "c[D3D12_NV2A_FOGPLANE].w");
                break;
            case FOGGEN_ABS_PLANAR:
                append_fog_output_from_distance(
                    source, key,
                    "abs(dot(c[D3D12_NV2A_FOGPLANE].xyz, tPosition.xyz) + "
                    "c[D3D12_NV2A_FOGPLANE].w)");
                break;
            case FOGGEN_FOG_X:
                if (pipeline_key_has_vertex_value(key, NV2A_VERTEX_ATTR_FOG)) {
                    g_string_append(source,
                                    "    float d3d12FogDistance = (");
                    append_vertex_value_expr(source, key,
                                             NV2A_VERTEX_ATTR_FOG);
                    g_string_append(source, ").x;\n");
                    append_fog_output_from_distance(source, key,
                                                    "d3d12FogDistance");
                } else {
                    append_fog_output_from_distance(source, key, "1.0");
                }
                break;
            default:
                append_fog_output_from_distance(source, key, "1.0");
                break;
            }
        } else if (pipeline_key_has_vertex_value(key, NV2A_VERTEX_ATTR_FOG)) {
            g_string_append(source, "    float d3d12FogDistance = (");
            append_vertex_value_expr(source, key, NV2A_VERTEX_ATTR_FOG);
            g_string_append(source, ").x;\n");
            append_fog_output_from_distance(source, key,
                                            "d3d12FogDistance");
        } else {
            append_fog_output_from_distance(source, key,
                                            "output.specular.a");
        }
        if (key->vertex_shader_mode == 0) {
            for (unsigned int i = 0; i < NV2A_MAX_TEXTURES; i++) {
                if (key->texture_coord_mask & (1u << i)) {
                    append_fixed_function_texture_output(source, key, i);
                }
            }
        } else {
            for (unsigned int i = 0; i < NV2A_MAX_TEXTURES; i++) {
                if (key->texture_coord_mask & (1u << i)) {
                    g_string_append_printf(source, "    output.tex%u = ", i);
                    if (pipeline_key_has_vertex_value(
                            key, NV2A_VERTEX_ATTR_TEXTURE0 + i)) {
                        append_vertex_value_expr(
                            source, key, NV2A_VERTEX_ATTR_TEXTURE0 + i);
                    } else {
                        g_string_append(source,
                                        "float4(0.0, 0.0, 0.0, 1.0)");
                    }
                    g_string_append(source, ";\n");
                }
            }
        }
    }
    g_string_append(source,
                    "    return output;\n"
                    "}\n");

    return g_string_free(source, FALSE);
}

static const char *texture_swizzle_expression(unsigned int color_format)
{
    switch (color_format) {
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_Y8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_Y8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_Y16:
        return "float4(t.r, t.r, t.r, 1.0)";
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_AY8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_AY8:
        return "float4(t.r, t.r, t.r, t.r)";
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8:
        return "float4(1.0, 1.0, 1.0, t.r)";
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8Y8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8Y8:
        return "float4(t.r, t.r, t.r, t.g)";
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_G8B8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_G8B8:
        return "float4(t.r, t.g, t.r, t.g)";
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R8B8:
        return "float4(t.g, t.r, t.r, t.g)";
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_DEPTH_Y16_FIXED:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_Y16_FIXED:
        return "float4(t.r, 0.0, 0.0, 0.0)";
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_X8_Y24_FIXED:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_X8_Y24_FLOAT:
        return "float4(t.r, 1.0, 0.0, 0.0)";
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_Y16_FLOAT:
        return "float4(t.r, 0.0, 1.0, 0.0)";
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_X1R5G5B5:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X1R5G5B5:
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_X8R8G8B8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X8R8G8B8:
        return "float4(t.rgb, 1.0)";
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_B8G8R8A8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_B8G8R8A8:
        return "float4(t.g, t.b, t.a, t.r)";
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R8G8B8A8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_R8G8B8A8:
        return "float4(t.a, t.b, t.g, t.r)";
    default:
        return "t";
    }
}

static uint32_t texture_color_key_mask(unsigned int color_format)
{
    switch (color_format) {
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_X1R5G5B5:
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_X8R8G8B8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X1R5G5B5:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X8R8G8B8:
        return 0x00ffffff;
    default:
        return 0xffffffff;
    }
}

static bool dxgi_format_is_snorm(DXGI_FORMAT format)
{
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_SNORM:
    case DXGI_FORMAT_R8G8_SNORM:
    case DXGI_FORMAT_R8_SNORM:
        return true;
    default:
        return false;
    }
}

static float border_inv_real_size(unsigned int logical_size)
{
    return logical_size < 8 ? 0.0625f : 1.0f / ((float)logical_size * 2.0f);
}

typedef struct PGRAPHD3D12PSInputInfo {
    int reg;
    int mod;
    int chan;
} PGRAPHD3D12PSInputInfo;

typedef struct PGRAPHD3D12PSOutputInfo {
    int ab;
    int cd;
    int muxsum;
    int flags;
    int ab_op;
    int cd_op;
    int muxsum_op;
    int mapping;
} PGRAPHD3D12PSOutputInfo;

typedef struct PGRAPHD3D12PSHLSLContext {
    const PGRAPHD3D12PipelineKey *key;
    unsigned int stage;
    const char *ef_prod_expr;
    bool final_clamp_sum;
    bool final_inv_v1;
    bool final_inv_r0;
} PGRAPHD3D12PSHLSLContext;

static void parse_ps_input(PGRAPHD3D12PSInputInfo *input, int value)
{
    input->reg = value & 0xf;
    input->chan = value & 0x10;
    input->mod = value & 0xe0;
}

static void parse_ps_inputs(uint32_t value,
                            PGRAPHD3D12PSInputInfo *a,
                            PGRAPHD3D12PSInputInfo *b,
                            PGRAPHD3D12PSInputInfo *c,
                            PGRAPHD3D12PSInputInfo *d)
{
    parse_ps_input(d, value & 0xff);
    parse_ps_input(c, (value >> 8) & 0xff);
    parse_ps_input(b, (value >> 16) & 0xff);
    parse_ps_input(a, (value >> 24) & 0xff);
}

static void parse_ps_output(PGRAPHD3D12PSOutputInfo *output, uint32_t value)
{
    output->cd = value & 0xf;
    output->ab = (value >> 4) & 0xf;
    output->muxsum = (value >> 8) & 0xf;
    output->flags = value >> 12;
    output->cd_op = output->flags & PS_COMBINEROUTPUT_CD_DOT_PRODUCT;
    output->ab_op = output->flags & PS_COMBINEROUTPUT_AB_DOT_PRODUCT;
    output->muxsum_op = output->flags & PS_COMBINEROUTPUT_AB_CD_MUX;
    output->mapping = output->flags & 0x38;
}

static char *hlsl_combiner_constant_expr(unsigned int flags,
                                         unsigned int stage,
                                         unsigned int which)
{
    unsigned int index;

    if (stage == 8 ||
        (which == 0 && (flags & PS_COMBINERCOUNT_UNIQUE_C0)) ||
        (which == 1 && (flags & PS_COMBINERCOUNT_UNIQUE_C1))) {
        index = stage * 2 + which;
    } else {
        index = which;
    }

    return g_strdup_printf("combinerConst[%u]", index);
}

static char *hlsl_register_expr(const PGRAPHD3D12PSHLSLContext *ctx,
                                int reg, bool is_dest)
{
    unsigned int flags = ctx->key->combiner_control >> 8;

    switch (reg) {
    case PS_REGISTER_DISCARD:
        return g_strdup(is_dest ? "" : "float4(0.0, 0.0, 0.0, 0.0)");
    case PS_REGISTER_C0:
        return is_dest ? g_strdup("") :
            hlsl_combiner_constant_expr(flags, ctx->stage, 0);
    case PS_REGISTER_C1:
        return is_dest ? g_strdup("") :
            hlsl_combiner_constant_expr(flags, ctx->stage, 1);
    case PS_REGISTER_FOG:
        return is_dest ? g_strdup("") : g_strdup("pFog");
    case PS_REGISTER_V0:
        return g_strdup("v0");
    case PS_REGISTER_V1:
        return g_strdup("v1");
    case PS_REGISTER_T0:
        return g_strdup("t0");
    case PS_REGISTER_T1:
        return g_strdup("t1");
    case PS_REGISTER_T2:
        return g_strdup("t2");
    case PS_REGISTER_T3:
        return g_strdup("t3");
    case PS_REGISTER_R0:
        return g_strdup("r0");
    case PS_REGISTER_R1:
        return g_strdup("r1");
    case PS_REGISTER_V1R0_SUM:
        if (is_dest) {
            return g_strdup("");
        }
        return g_strdup_printf(
            "%sfloat4((%s).rgb + (%s).rgb, 0.0)%s",
            ctx->final_clamp_sum ? "saturate(" : "",
            ctx->final_inv_v1 ? "(1.0 - v1)" : "v1",
            ctx->final_inv_r0 ? "(1.0 - r0)" : "r0",
            ctx->final_clamp_sum ? ")" : "");
    case PS_REGISTER_EF_PROD:
        if (is_dest) {
            return g_strdup("");
        }
        return g_strdup(ctx->ef_prod_expr != NULL ? ctx->ef_prod_expr :
                                                 "float4(0.0, 0.0, 0.0, 0.0)");
    default:
        return g_strdup(is_dest ? "" : "float4(0.0, 0.0, 0.0, 0.0)");
    }
}

/* A combiner input mapping applied to component; NULL for none. */
static char *hlsl_input_mapping_expr(const char *component, int mod)
{
    switch (mod) {
    case PS_INPUTMAPPING_UNSIGNED_IDENTITY:
        return g_strdup_printf("max(%s, 0.0)", component);
    case PS_INPUTMAPPING_UNSIGNED_INVERT:
        return g_strdup_printf("(1.0 - saturate(%s))", component);
    case PS_INPUTMAPPING_EXPAND_NORMAL:
        return g_strdup_printf("(2.0 * max(%s, 0.0) - 1.0)", component);
    case PS_INPUTMAPPING_EXPAND_NEGATE:
        return g_strdup_printf("(-2.0 * max(%s, 0.0) + 1.0)", component);
    case PS_INPUTMAPPING_HALFBIAS_NORMAL:
        return g_strdup_printf("(max(%s, 0.0) - 0.5)", component);
    case PS_INPUTMAPPING_HALFBIAS_NEGATE:
        return g_strdup_printf("(-max(%s, 0.0) + 0.5)", component);
    case PS_INPUTMAPPING_SIGNED_IDENTITY:
        return g_strdup(component);
    case PS_INPUTMAPPING_SIGNED_NEGATE:
        return g_strdup_printf("-(%s)", component);
    default:
        return NULL;
    }
}

static char *hlsl_input_expr(const PGRAPHD3D12PSHLSLContext *ctx,
                             PGRAPHD3D12PSInputInfo input, bool is_alpha)
{
    g_autofree char *reg = hlsl_register_expr(ctx, input.reg, false);
    g_autofree char *component = NULL;
    char *mapped;

    if (is_alpha) {
        component = g_strdup_printf("(%s).%c", reg,
                                    input.chan == PS_CHANNEL_ALPHA ? 'a' : 'b');
    } else {
        component = g_strdup_printf("(%s).%s", reg,
                                    input.chan == PS_CHANNEL_ALPHA ?
                                        "aaa" : "rgb");
    }

    mapped = hlsl_input_mapping_expr(component, input.mod);
    if (mapped != NULL) {
        return mapped;
    }
    return g_strdup(is_alpha ? "0.0" : "float3(0.0, 0.0, 0.0)");
}

static char *hlsl_output_map_expr(const char *expr, int mapping)
{
    switch (mapping) {
    case PS_COMBINEROUTPUT_IDENTITY:
        return g_strdup(expr);
    case PS_COMBINEROUTPUT_BIAS:
        return g_strdup_printf("(%s - 0.5)", expr);
    case PS_COMBINEROUTPUT_SHIFTLEFT_1:
        return g_strdup_printf("(%s * 2.0)", expr);
    case PS_COMBINEROUTPUT_SHIFTLEFT_1_BIAS:
        return g_strdup_printf("((%s - 0.5) * 2.0)", expr);
    case PS_COMBINEROUTPUT_SHIFTLEFT_2:
        return g_strdup_printf("(%s * 4.0)", expr);
    case PS_COMBINEROUTPUT_SHIFTRIGHT_1:
        return g_strdup_printf("(%s / 2.0)", expr);
    default:
        return g_strdup(expr);
    }
}

static void append_combiner_component(GString *source,
                                      GString *assignments,
                                      const PGRAPHD3D12PSHLSLContext *ctx,
                                      uint32_t input_word,
                                      uint32_t output_word,
                                      bool is_alpha)
{
    PGRAPHD3D12PSInputInfo input_a, input_b, input_c, input_d;
    PGRAPHD3D12PSOutputInfo output;
    g_autofree char *a = NULL;
    g_autofree char *b = NULL;
    g_autofree char *c = NULL;
    g_autofree char *d = NULL;
    g_autofree char *ab = NULL;
    g_autofree char *cd = NULL;
    g_autofree char *ab_value = NULL;
    g_autofree char *cd_value = NULL;
    g_autofree char *ab_mapped = NULL;
    g_autofree char *cd_mapped = NULL;
    g_autofree char *mux = NULL;
    g_autofree char *mux_mapped = NULL;
    g_autofree char *ab_dest = NULL;
    g_autofree char *cd_dest = NULL;
    g_autofree char *mux_dest = NULL;
    const char *mask = is_alpha ? "a" : "rgb";
    const char *mux_cond =
        (ctx->key->combiner_control >> 8) & PS_COMBINERCOUNT_MUX_MSB ?
            "(r0.a >= 0.5)" :
            "(((uint)(r0.a * 255.0) & 1u) == 1u)";

    parse_ps_inputs(input_word, &input_a, &input_b, &input_c, &input_d);
    parse_ps_output(&output, output_word);

    a = hlsl_input_expr(ctx, input_a, is_alpha);
    b = hlsl_input_expr(ctx, input_b, is_alpha);
    c = hlsl_input_expr(ctx, input_c, is_alpha);
    d = hlsl_input_expr(ctx, input_d, is_alpha);

    if (!is_alpha && output.ab_op == PS_COMBINEROUTPUT_AB_DOT_PRODUCT) {
        g_autofree char *dot =
            g_strdup_printf("dot(float3(%s), float3(%s))", a, b);
        ab = g_strdup_printf("float3(%s, %s, %s)", dot, dot, dot);
    } else {
        ab = g_strdup_printf("(%s * %s)", a, b);
    }
    if (!is_alpha && output.cd_op == PS_COMBINEROUTPUT_CD_DOT_PRODUCT) {
        g_autofree char *dot =
            g_strdup_printf("dot(float3(%s), float3(%s))", c, d);
        cd = g_strdup_printf("float3(%s, %s, %s)", dot, dot, dot);
    } else {
        cd = g_strdup_printf("(%s * %s)", c, d);
    }

    ab_dest = hlsl_register_expr(ctx, output.ab, true);
    cd_dest = hlsl_register_expr(ctx, output.cd, true);
    mux_dest = hlsl_register_expr(ctx, output.muxsum, true);

    /* A combiner stage reads every input before writing any destination;
     * temporaries keep aliased destinations from feeding back. */
    g_string_append_printf(source, "    ab.%s = %s;\n", mask, ab);
    g_string_append_printf(source, "    cd.%s = %s;\n", mask, cd);

    if (output.muxsum_op == PS_COMBINEROUTPUT_AB_CD_MUX) {
        mux = g_strdup_printf("(%s ? cd.%s : ab.%s)",
                              mux_cond, mask, mask);
    } else {
        mux = g_strdup_printf("(ab.%s + cd.%s)", mask, mask);
    }
    mux_mapped = hlsl_output_map_expr(mux, output.mapping);
    if (mux_dest[0] != '\0') {
        g_string_append_printf(
            source, "    mux_sum.%s = clamp(%s, -1.0, 1.0);\n",
            mask, mux_mapped);
    }

    ab_value = g_strdup_printf("ab.%s", mask);
    cd_value = g_strdup_printf("cd.%s", mask);
    ab_mapped = hlsl_output_map_expr(ab_value, output.mapping);
    cd_mapped = hlsl_output_map_expr(cd_value, output.mapping);

    if (ab_dest[0] != '\0') {
        g_string_append_printf(
            source, "    ab.%s = clamp(%s, -1.0, 1.0);\n",
            mask, ab_mapped);
        g_string_append_printf(assignments, "    %s.%s = ab.%s;\n",
                               ab_dest, mask, mask);
        if (!is_alpha &&
            (output.flags & PS_COMBINEROUTPUT_AB_BLUE_TO_ALPHA)) {
            g_string_append_printf(assignments, "    %s.a = ab.b;\n",
                                   ab_dest);
        }
    }
    if (cd_dest[0] != '\0') {
        g_string_append_printf(
            source, "    cd.%s = clamp(%s, -1.0, 1.0);\n",
            mask, cd_mapped);
        g_string_append_printf(assignments, "    %s.%s = cd.%s;\n",
                               cd_dest, mask, mask);
        if (!is_alpha &&
            (output.flags & PS_COMBINEROUTPUT_CD_BLUE_TO_ALPHA)) {
            g_string_append_printf(assignments, "    %s.a = cd.b;\n",
                                   cd_dest);
        }
    }
    if (mux_dest[0] != '\0') {
        g_string_append_printf(assignments,
                               "    %s.%s = mux_sum.%s;\n",
                               mux_dest, mask, mask);
    }
}

static void append_combiner_stage(GString *source,
                                  const PGRAPHD3D12PipelineKey *key,
                                  unsigned int stage)
{
    g_autoptr(GString) rgb_assignments = g_string_new(NULL);
    g_autoptr(GString) alpha_assignments = g_string_new(NULL);
    PGRAPHD3D12PSHLSLContext ctx = {
        .key = key,
        .stage = stage,
    };

    g_string_append_printf(source, "    // Combiner stage %u\n", stage);
    append_combiner_component(source, rgb_assignments, &ctx,
                              key->rgb_inputs[stage],
                              key->rgb_outputs[stage], false);
    append_combiner_component(source, alpha_assignments, &ctx,
                              key->alpha_inputs[stage],
                              key->alpha_outputs[stage], true);
    g_string_append(source, rgb_assignments->str);
    g_string_append(source, alpha_assignments->str);
}

static void append_final_combiner(GString *source,
                                  const PGRAPHD3D12PipelineKey *key)
{
    PGRAPHD3D12PSInputInfo a_in, b_in, c_in, d_in;
    PGRAPHD3D12PSInputInfo e_in, f_in, g_in, unused;
    PGRAPHD3D12PSHLSLContext ctx = {
        .key = key,
        .stage = 8,
        .final_clamp_sum =
            key->final_inputs_1 & PS_FINALCOMBINERSETTING_CLAMP_SUM,
        .final_inv_v1 =
            key->final_inputs_1 & PS_FINALCOMBINERSETTING_COMPLEMENT_V1,
        .final_inv_r0 =
            key->final_inputs_1 & PS_FINALCOMBINERSETTING_COMPLEMENT_R0,
    };
    g_autofree char *e = NULL;
    g_autofree char *f = NULL;
    g_autofree char *ef = NULL;
    g_autofree char *a = NULL;
    g_autofree char *b = NULL;
    g_autofree char *c = NULL;
    g_autofree char *d = NULL;
    g_autofree char *g = NULL;

    parse_ps_inputs(key->final_inputs_0, &a_in, &b_in, &c_in, &d_in);
    parse_ps_inputs(key->final_inputs_1, &e_in, &f_in, &g_in, &unused);

    e = hlsl_input_expr(&ctx, e_in, false);
    f = hlsl_input_expr(&ctx, f_in, false);
    ef = g_strdup_printf("float4(float3(%s) * float3(%s), 0.0)", e, f);
    ctx.ef_prod_expr = ef;

    a = hlsl_input_expr(&ctx, a_in, false);
    b = hlsl_input_expr(&ctx, b_in, false);
    c = hlsl_input_expr(&ctx, c_in, false);
    d = hlsl_input_expr(&ctx, d_in, false);
    g = hlsl_input_expr(&ctx, g_in, true);

    g_string_append(source, "    // Final combiner\n");
    g_string_append_printf(
        source,
        "    color.rgb = float3(%s) + lerp(float3(%s), float3(%s), "
        "float3(%s));\n"
        "    color.a = %s;\n",
        d, c, b, a, g);
}

static bool key_has_color_key(const PGRAPHD3D12PipelineKey *key)
{
    for (unsigned int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        if (key->texture_colorkey_mode[i] != COLOR_KEY_NONE) {
            return true;
        }
    }

    return false;
}

static unsigned int texture_stage_mode(const PGRAPHD3D12PipelineKey *key,
                                       unsigned int stage)
{
    return (key->shader_stage_program >> (stage * 5)) & 0x1f;
}

static int texture_stage_input(const PGRAPHD3D12PipelineKey *key,
                               unsigned int stage)
{
    switch (stage) {
    case 1:
        return 0;
    case 2:
        return (key->other_stage_input >> 16) & 0xf;
    case 3:
        return (key->other_stage_input >> 20) & 0xf;
    default:
        return -1;
    }
}

static unsigned int texture_stage_dot_map(const PGRAPHD3D12PipelineKey *key,
                                          unsigned int stage)
{
    if (stage == 0) {
        return 0;
    }

    return (key->other_stage_input >> ((stage - 1) * 4)) & 0xf;
}

static uint8_t texture_convolution_filter_from_register(uint32_t filter)
{
    unsigned int min_filter = GET_MASK(filter, NV_PGRAPH_TEXFILTER0_MIN);
    unsigned int kernel;

    if (min_filter != NV_PGRAPH_TEXFILTER0_MIN_CONVOLUTION_2D_LOD0) {
        return CONVOLUTION_FILTER_DISABLED;
    }

    kernel = GET_MASK(filter, NV_PGRAPH_TEXFILTER0_CONVOLUTION_KERNEL);
    switch (kernel) {
    case NV_PGRAPH_TEXFILTER0_CONVOLUTION_KERNEL_QUINCUNX:
    case NV_PGRAPH_TEXFILTER0_CONVOLUTION_KERNEL_GAUSSIAN_3:
        return kernel;
    default:
        return CONVOLUTION_FILTER_DISABLED;
    }
}

static const char *texture_convolution_filter_name(unsigned int filter)
{
    switch (filter) {
    case CONVOLUTION_FILTER_DISABLED:
        return "disabled";
    case CONVOLUTION_FILTER_QUINCUNX:
        return "quincunx";
    case CONVOLUTION_FILTER_GAUSSIAN:
        return "gaussian";
    default:
        return "unknown";
    }
}

static uint8_t texture_mode_coord_mask(unsigned int stage, unsigned int mode)
{
    uint8_t mask = 1u << stage;

    switch (mode) {
    case PS_TEXTUREMODES_DOT_RFLCT_DIFF:
        if (stage == 2) {
            mask |= 1u << 3;
        }
        break;
    case PS_TEXTUREMODES_DOT_RFLCT_SPEC:
        if (stage == 3) {
            mask |= (1u << 1) | (1u << 2);
        }
        break;
    default:
        break;
    }

    return mask;
}

static bool texture_mode_samples(unsigned int mode)
{
    switch (mode) {
    case PS_TEXTUREMODES_PROJECT2D:
    case PS_TEXTUREMODES_PROJECT3D:
    case PS_TEXTUREMODES_CUBEMAP:
    case PS_TEXTUREMODES_BUMPENVMAP:
    case PS_TEXTUREMODES_BUMPENVMAP_LUM:
    case PS_TEXTUREMODES_DOT_ST:
    case PS_TEXTUREMODES_DOT_RFLCT_DIFF:
    case PS_TEXTUREMODES_DOT_RFLCT_SPEC:
    case PS_TEXTUREMODES_DOT_STR_3D:
    case PS_TEXTUREMODES_DOT_STR_CUBE:
    case PS_TEXTUREMODES_DPNDNT_AR:
    case PS_TEXTUREMODES_DPNDNT_GB:
        return true;
    default:
        return false;
    }
}

/* Whether a stage samples its texture; a sampling mode on a stage or input
 * it cannot use emits a constant instead. */
static bool texture_stage_samples(const PGRAPHD3D12PipelineKey *key,
                                  unsigned int stage)
{
    int input = texture_stage_input(key, stage);
    bool input_valid = input >= 0 && input < NV2A_MAX_TEXTURES;

    switch (texture_stage_mode(key, stage)) {
    case PS_TEXTUREMODES_PROJECT2D:
    case PS_TEXTUREMODES_PROJECT3D:
    case PS_TEXTUREMODES_CUBEMAP:
        return true;
    case PS_TEXTUREMODES_BUMPENVMAP:
    case PS_TEXTUREMODES_BUMPENVMAP_LUM:
    case PS_TEXTUREMODES_DOT_ST:
    case PS_TEXTUREMODES_DPNDNT_AR:
    case PS_TEXTUREMODES_DPNDNT_GB:
        return input_valid;
    case PS_TEXTUREMODES_DOT_RFLCT_DIFF:
        return stage == 2 && input_valid;
    case PS_TEXTUREMODES_DOT_RFLCT_SPEC:
    case PS_TEXTUREMODES_DOT_STR_3D:
    case PS_TEXTUREMODES_DOT_STR_CUBE:
        return stage == 3 && input_valid;
    default:
        return false;
    }
}

/*
 * PASSTHRU, CLIPPLANE, DOTPRODUCT and DOT_ZW read no SRV and must run even
 * with TEXCTL disabled. Sampling modes run only when a texture is bound so
 * the HLSL never references an unbound descriptor.
 */
static uint8_t texture_stage_execution_mask(
    const PGRAPHD3D12PipelineKey *key)
{
    uint8_t mask = 0;

    for (unsigned int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        unsigned int mode = texture_stage_mode(key, i);

        if (mode == PS_TEXTUREMODES_NONE) {
            continue;
        }
        if (!texture_mode_samples(mode) ||
            (key->texture_mask & (1u << i))) {
            mask |= 1u << i;
        }
    }

    return mask;
}

/* Stage helpers of every pixel shader that runs a texture stage. */
static void append_texture_common_helpers(GString *source)
{
    g_string_append(
        source,
        "float sign1(float x)\n"
        "{\n"
        "    x *= 255.0;\n"
        "    return (x - 128.0) / 127.0;\n"
        "}\n"
        "float sign2(float x)\n"
        "{\n"
        "    x *= 255.0;\n"
        "    return x >= 128.0 ? (x - 255.5) / 127.5 : "
        "(x + 0.5) / 127.5;\n"
        "}\n"
        "float sign3(float x)\n"
        "{\n"
        "    x *= 255.0;\n"
        "    return x >= 128.0 ? (x - 256.0) / 127.0 : x / 127.0;\n"
        "}\n"
        "float project_w(float w)\n"
        "{\n"
        "    return w < 0.0 ? min(w, -1e-20) : max(w, 1e-20);\n"
        "}\n"
        "float3 dotmap(uint mode, float4 col)\n"
        "{\n"
        "    switch (mode) {\n"
        "    case 1u:\n"
        "        return float3(sign1(col.r), sign1(col.g), sign1(col.b));\n"
        "    case 2u:\n"
        "        return float3(sign2(col.r), sign2(col.g), sign2(col.b));\n"
        "    case 3u:\n"
        "        return float3(sign3(col.r), sign3(col.g), sign3(col.b));\n"
        "    case 4u:\n"
        "    {\n"
        "        uint4 c = (uint4)max(col * 255.0, 0.0);\n"
        "        uint hi = (c.a << 8) | c.r;\n"
        "        uint lo = (c.g << 8) | c.b;\n"
        "        return float3((float)hi / 65535.0, "
        "(float)lo / 65535.0, 1.0);\n"
        "    }\n"
        "    case 5u:\n"
        "    case 6u:\n"
        "    case 7u:\n"
        "        return col.rgb;\n"
        "    default:\n"
        "        return col.rgb;\n"
        "    }\n"
        "}\n"
        "float2 remapCubeTo2D(float3 texCoord)\n"
        "{\n"
        "    float2 uv;\n"
        "    float3 absTexCoord = abs(texCoord);\n"
        "    if (absTexCoord.x > absTexCoord.y && "
        "absTexCoord.x > absTexCoord.z) {\n"
        "        uv = texCoord.x > 0.0 ? "
        "float2(-texCoord.z, texCoord.y) : "
        "float2(texCoord.z, texCoord.y);\n"
        "        uv /= max(absTexCoord.x, 1e-20);\n"
        "    } else if (absTexCoord.y > absTexCoord.x && "
        "absTexCoord.y > absTexCoord.z) {\n"
        "        uv = texCoord.y > 0.0 ? "
        "float2(texCoord.x, -texCoord.z) : "
        "float2(texCoord.x, texCoord.z);\n"
        "        uv /= max(absTexCoord.y, 1e-20);\n"
        "    } else {\n"
        "        uv = texCoord.z > 0.0 ? "
        "float2(texCoord.x, texCoord.y) : "
        "float2(-texCoord.x, texCoord.y);\n"
        "        uv /= max(absTexCoord.z, 1e-20);\n"
        "    }\n"
        "    return uv;\n"
        "}\n"
        "float2 bumpMatrixMul(uint stage, float2 value)\n"
        "{\n"
        "    float4 m = bumpMat[stage];\n"
        "    return float2(dot(m.xz, value), dot(m.yw, value));\n"
        "}\n"
        /* Signed channels (m = 1) are stored offset by 128, as the GL
         * renderer's signed_channel(); signed_raw gives the guest byte back
         * for the stages that read a texel as raw bytes (dot mapping, bump
         * and dependent reads, it%d in the GLSL). */
        "float4 signed_decode(float4 u, float4 m)\n"
        "{\n"
        "    return lerp(u, clamp((u * 255.0 - 128.0) / 127.0, -1.0, 1.0), "
        "m);\n"
        "}\n"
        "float4 signed_raw(float4 s, float4 m)\n"
        "{\n"
        "    float4 x = round(s * 127.0);\n"
        "    x += 256.0 * (float4)(x < 0.0);\n"
        "    return lerp(s, x / 255.0, m);\n"
        "}\n");
}

/* float4 mask of a TEX_CHANNEL_* sign set, in r, g, b, a order. */
static char *texture_signs_mask_expr(uint8_t signs)
{
    return g_strdup_printf("float4(%d.0, %d.0, %d.0, %d.0)",
                           !!(signs & TEX_CHANNEL_RSIGNED),
                           !!(signs & TEX_CHANNEL_GSIGNED),
                           !!(signs & TEX_CHANNEL_BSIGNED),
                           !!(signs & TEX_CHANNEL_ASIGNED));
}

/* Name of the value a dot-mapping, bump or dependent stage reads from
 * stage input: the raw bytes when that texture has signed channels. */
static const char *texture_stage_raw_name(const PGRAPHD3D12PipelineKey *key,
                                          int input, char *buf, size_t size)
{
    if (input >= 0 && input < NV2A_MAX_TEXTURES && key->texture_signs[input]) {
        snprintf(buf, size, "traw%d", input);
    } else {
        snprintf(buf, size, "t%d", input);
    }
    return buf;
}

/*
 * Cube face coordinates in a bordered cube map, stored as a six-layer
 * array: signature names the function and its texCoord parameter, scale
 * and inv are the float2 logical size and inverse real size.
 */
static void append_cube_border_coord_function(GString *source,
                                              const char *signature,
                                              const char *scale,
                                              const char *inv)
{
    g_string_append_printf(
        source,
        "%s\n"
        "{\n"
        "    float2 uv;\n"
        "    float face;\n"
        "    float3 absTexCoord = abs(texCoord);\n"
        "    if (absTexCoord.x > absTexCoord.y && "
        "absTexCoord.x > absTexCoord.z) {\n"
        "        if (texCoord.x > 0.0) {\n"
        "            uv = float2(-texCoord.z, -texCoord.y) / "
        "max(absTexCoord.x, 1e-20);\n"
        "            face = 0.0;\n"
        "        } else {\n"
        "            uv = float2(texCoord.z, -texCoord.y) / "
        "max(absTexCoord.x, 1e-20);\n"
        "            face = 1.0;\n"
        "        }\n"
        "    } else if (absTexCoord.y > absTexCoord.x && "
        "absTexCoord.y > absTexCoord.z) {\n"
        "        if (texCoord.y > 0.0) {\n"
        "            uv = float2(texCoord.x, texCoord.z) / "
        "max(absTexCoord.y, 1e-20);\n"
        "            face = 2.0;\n"
        "        } else {\n"
        "            uv = float2(texCoord.x, -texCoord.z) / "
        "max(absTexCoord.y, 1e-20);\n"
        "            face = 3.0;\n"
        "        }\n"
        "    } else {\n"
        "        if (texCoord.z > 0.0) {\n"
        "            uv = float2(texCoord.x, -texCoord.y) / "
        "max(absTexCoord.z, 1e-20);\n"
        "            face = 4.0;\n"
        "        } else {\n"
        "            uv = float2(-texCoord.x, -texCoord.y) / "
        "max(absTexCoord.z, 1e-20);\n"
        "            face = 5.0;\n"
        "        }\n"
        "    }\n"
        "    uv = uv * 0.5 + 0.5;\n"
        "    uv = (uv * %s + float2(4.0, 4.0)) * %s;\n"
        "    return float3(uv, face);\n"
        "}\n",
        signature, scale, inv);
}

static void append_texture_sampling_helpers(
    GString *source, const PGRAPHD3D12PipelineKey *key)
{
    if (texture_stage_execution_mask(key) == 0) {
        return;
    }

    append_texture_common_helpers(source);
    for (unsigned int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        if (!(key->texture_mask & (1u << i))) {
            continue;
        }
        bool cubemap = key->texture_cubemap_mask & (1u << i);
        bool texture3d = key->texture_3d_mask & (1u << i);
        g_autofree char *signed_swizzle = NULL;
        const char *swizzle =
            texture_swizzle_expression(key->texture_color_format[i]);

        if (key->texture_signs[i]) {
            /* stored as R, G, B, A by the shared conversion, so no format
             * swizzle */
            g_autofree char *mask =
                texture_signs_mask_expr(key->texture_signs[i]);
            signed_swizzle = g_strdup_printf("signed_decode(t, %s)", mask);
            swizzle = signed_swizzle;
        }

        g_string_append_printf(source, "float4 sample_tex%u(float2 coord)\n",
                               i);
        g_string_append(source, "{\n");
        if (!cubemap && !texture3d && key->texture_linear_mask & (1u << i)) {
            /* Linear coordinates are in guest pixels; normalize by the
             * guest dimensions, not the scaled resource size. The
             * dimensions are constants so each new size needs no PSO. */
            g_string_append_printf(
                source,
                "    coord /= max(texSize[%u].xy, float2(1.0, 1.0));\n",
                i);
        }
        if (texture3d) {
            g_string_append_printf(
                source,
                "    float3 coord3 = float3(coord, 0.0);\n");
            if (key->texture_border_adjust_mask & (1u << i)) {
                g_string_append_printf(
                    source,
                    "    coord3.xy = (coord3.xy * float2(%f, %f) + "
                    "float2(4.0, 4.0)) * float2(%f, %f);\n",
                    key->texture_border_logical_size[i][0],
                    key->texture_border_logical_size[i][1],
                    key->texture_border_inv_real_size[i][0],
                    key->texture_border_inv_real_size[i][1]);
            }
            g_string_append_printf(
                source,
                "    float4 t = tex%u.Sample(samp%u, "
                "coord3);\n"
                "    return %s;\n"
                "}\n",
                i, i, swizzle);
            g_string_append_printf(
                source,
                "float4 sample_tex3d%u(float3 coord)\n"
                "{\n",
                i);
            if (key->texture_border_adjust_mask & (1u << i)) {
                g_string_append_printf(
                    source,
                    "    coord = (coord * float3(%f, %f, %f) + "
                    "float3(4.0, 4.0, 4.0)) * float3(%f, %f, %f);\n",
                    key->texture_border_logical_size[i][0],
                    key->texture_border_logical_size[i][1],
                    key->texture_border_logical_size[i][2],
                    key->texture_border_inv_real_size[i][0],
                    key->texture_border_inv_real_size[i][1],
                    key->texture_border_inv_real_size[i][2]);
            }
            g_string_append_printf(
                source,
                "    float4 t = tex%u.Sample(samp%u, coord);\n"
                "    return %s;\n"
                "}\n",
                i, i, swizzle);
            g_string_append_printf(
                source,
                "float4 sample_tex_cube%u(float3 coord)\n"
                "{\n"
                "    return sample_tex3d%u(coord);\n"
                "}\n",
                i, i);
        } else if (cubemap) {
            if (key->texture_border_adjust_mask & (1u << i)) {
                g_autofree char *signature = g_strdup_printf(
                    "float3 cube_border_coord%u(float3 texCoord)", i);
                g_autofree char *scale = g_strdup_printf(
                    "float2(%f, %f)", key->texture_border_logical_size[i][0],
                    key->texture_border_logical_size[i][1]);
                g_autofree char *inv = g_strdup_printf(
                    "float2(%f, %f)",
                    key->texture_border_inv_real_size[i][0],
                    key->texture_border_inv_real_size[i][1]);

                append_cube_border_coord_function(source, signature, scale,
                                                  inv);
                g_string_append_printf(
                    source,
                    "    float4 t = tex%u.Sample(samp%u, "
                    "cube_border_coord%u(float3(coord, 1.0)));\n"
                    "    return %s;\n"
                    "}\n",
                    i, i, i, swizzle);
                g_string_append_printf(
                    source,
                    "float4 sample_tex_cube%u(float3 coord)\n"
                    "{\n"
                    "    float4 t = tex%u.Sample(samp%u, "
                    "cube_border_coord%u(coord));\n"
                    "    return %s;\n"
                    "}\n",
                    i, i, i, i, swizzle);
            } else {
                g_string_append_printf(
                    source,
                    "    float4 t = tex%u.Sample(samp%u, "
                    "float3(coord, 1.0));\n"
                    "    return %s;\n"
                    "}\n",
                    i, i, swizzle);
                g_string_append_printf(
                    source,
                    "float4 sample_tex_cube%u(float3 coord)\n"
                    "{\n"
                    "    float4 t = tex%u.Sample(samp%u, coord);\n"
                    "    return %s;\n"
                    "}\n",
                    i, i, i, swizzle);
            }
            g_string_append_printf(
                source,
                "float4 sample_tex3d%u(float3 coord)\n"
                "{\n"
                "    return sample_tex_cube%u(coord);\n"
                "}\n",
                i, i);
        } else {
            if (key->texture_border_adjust_mask & (1u << i)) {
                g_string_append_printf(
                    source,
                    "    coord = (coord * float2(%f, %f) + "
                    "float2(4.0, 4.0)) * float2(%f, %f);\n",
                    key->texture_border_logical_size[i][0],
                    key->texture_border_logical_size[i][1],
                    key->texture_border_inv_real_size[i][0],
                    key->texture_border_inv_real_size[i][1]);
            }
            g_string_append_printf(
                source,
                "    float4 t = tex%u.Sample(samp%u, coord);\n"
                "    return %s;\n"
                "}\n"
                "float4 sample_tex_offset%u(float2 coord, float2 offset)\n"
                "{\n",
                i, i, swizzle, i);
            if (key->texture_linear_mask & (1u << i)) {
                g_string_append_printf(
                    source,
                    "    coord /= max(texSize[%u].xy, float2(1.0, 1.0));\n",
                    i);
            }
            if (key->texture_border_adjust_mask & (1u << i)) {
                g_string_append_printf(
                    source,
                    "    coord = (coord * float2(%f, %f) + "
                    "float2(4.0, 4.0)) * float2(%f, %f);\n",
                    key->texture_border_logical_size[i][0],
                    key->texture_border_logical_size[i][1],
                    key->texture_border_inv_real_size[i][0],
                    key->texture_border_inv_real_size[i][1]);
            }
            g_string_append_printf(
                source,
                "    coord += offset;\n"
                "    float4 t = tex%u.Sample(samp%u, coord);\n"
                "    return %s;\n"
                "}\n"
                "float4 sample_tex_cube%u(float3 coord)\n"
                "{\n"
                "    return sample_tex%u(remapCubeTo2D(coord));\n"
                "}\n",
                i, i, swizzle, i, i);
            g_string_append_printf(
                source,
                "float4 sample_tex3d%u(float3 coord)\n"
                "{\n"
                "    return sample_tex%u(coord.xy);\n"
                "}\n",
                i, i);
        }
    }
}

static void append_texture_sample_epilogue(
    GString *source, const PGRAPHD3D12PipelineKey *key, unsigned int stage,
    bool sampled)
{
    if (!sampled) {
        return;
    }

    if (key->texture_signs[stage]) {
        g_autofree char *mask = texture_signs_mask_expr(key->texture_signs[stage]);

        g_string_append_printf(source, "    traw%u = signed_raw(t%u, %s);\n",
                               stage, stage, mask);
    }

    /* Alpha kill stages come from pixel constants. */
    if (key->alpha_test_enable) {
        g_string_append_printf(
            source,
            "    if ((alphaTest.z & %uu) != 0u && t%u.a == 0.0) discard;\n",
            1u << stage, stage);
    }
    if (key->texture_colorkey_mode[stage] != COLOR_KEY_NONE) {
        g_string_append_printf(
            source,
            "    if (check_color_key(t%u, colorKey[%u], "
            "colorKeyMask[%u])) {\n",
            stage, stage, stage);
        switch (key->texture_colorkey_mode[stage]) {
        case COLOR_KEY_KILL_ALPHA:
            g_string_append_printf(source, "        t%u.a = 0.0;\n",
                                   stage);
            break;
        case COLOR_KEY_KILL_COLOR_AND_ALPHA:
            g_string_append_printf(
                source,
                "        t%u = float4(0.0, 0.0, 0.0, 0.0);\n",
                stage);
            break;
        case COLOR_KEY_DISCARD:
            g_string_append(source, "        discard;\n");
            break;
        default:
            break;
        }
        g_string_append(source, "    }\n");
    }
}

static void append_shadow_texture_sample(
    GString *source, const PGRAPHD3D12PipelineKey *key, unsigned int stage,
    bool compare_z)
{
    /* Diagnostic: every shadow lookup passes. */
    if (key->tune_flags & PGRAPH_D3D12_TUNE_SHADOW_LIT) {
        g_string_append_printf(source,
                               "    t%u = float4(1.0, 1.0, 1.0, 1.0);\n",
                               stage);
        return;
    }

    /* The compare function comes from pixel constants. */
    if (key->texture_3d_mask & (1u << stage)) {
        g_string_append(
            source,
            "    // NV2A shadow lookup samples xy/w; Texture3D uses z=0 "
            "here to match the GL renderer.\n");
    }
    g_string_append_printf(
        source,
        "    float4 t%u_depth = sample_tex%u(input.tex%u.xy / "
        "project_w(input.tex%u.w));\n"
        "    float t%u_max_depth = t%u_depth.g > 0.0 ? 16777215.0 : "
        "(t%u_depth.b > 0.0 ? 511.9375 : 65535.0);\n"
        "    float t%u_sample = t%u_depth.r * t%u_max_depth;\n",
        stage, stage, stage, stage, stage, stage, stage, stage, stage,
        stage);

    if (compare_z) {
        g_string_append_printf(
            source,
            "    float t%u_ref = clamp(input.tex%u.z / "
            "project_w(input.tex%u.w), 0.0, t%u_max_depth);\n"
            "    float t%u_shadow = shadow_compare(alphaTest.w, t%u_sample, "
            "t%u_ref) ? 1.0 : 0.0;\n"
            "    t%u = float4(t%u_shadow, t%u_shadow, t%u_shadow, "
            "t%u_shadow);\n",
            stage, stage, stage, stage, stage, stage, stage,
            stage, stage, stage, stage, stage);
    } else {
        g_string_append_printf(
            source,
            "    float t%u_shadow = shadow_compare(alphaTest.w, t%u_sample, "
            "0.0) ? 1.0 : 0.0;\n"
            "    t%u = float4(t%u_shadow, t%u_shadow, t%u_shadow, "
            "t%u_shadow);\n",
            stage, stage, stage, stage, stage, stage, stage);
    }
}

static bool texture_stage_uses_convolution(
    const PGRAPHD3D12PipelineKey *key, unsigned int stage, unsigned int mode)
{
    if (mode != PS_TEXTUREMODES_PROJECT2D) {
        return false;
    }
    if (key->texture_convolution_filter[stage] == CONVOLUTION_FILTER_DISABLED) {
        return false;
    }
    if (key->texture_depth_mask & (1u << stage)) {
        return false;
    }
    if (key->texture_cubemap_mask & (1u << stage)) {
        return false;
    }
    if (key->texture_3d_mask & (1u << stage)) {
        return false;
    }

    return true;
}

/* The 3x3 tent of both convolution kernels: offset and weight. */
static const struct {
    float x;
    float y;
    const char *weight;
} convolution_kernel[] = {
    { -1.0f, -1.0f, "0.0625" },
    {  0.0f, -1.0f, "0.125"  },
    {  1.0f, -1.0f, "0.0625" },
    { -1.0f,  0.0f, "0.125"  },
    {  0.0f,  0.0f, "0.25"   },
    {  1.0f,  0.0f, "0.125"  },
    { -1.0f,  1.0f, "0.0625" },
    {  0.0f,  1.0f, "0.125"  },
    {  1.0f,  1.0f, "0.0625" },
};

static void append_convolution_texture_sample(
    GString *source, const PGRAPHD3D12PipelineKey *key, unsigned int stage)
{
    g_string_append_printf(
        source,
        "    // %s convolution filter, matching the GL renderer\n"
        "    t%u = float4(0.0, 0.0, 0.0, 0.0);\n"
        "    {\n"
        "        uint t%u_width = 1u;\n"
        "        uint t%u_height = 1u;\n"
        "        tex%u.GetDimensions(t%u_width, t%u_height);\n"
        "        float2 t%u_texel = 1.0 / max(float2((float)t%u_width, "
        "(float)t%u_height), float2(1.0, 1.0));\n"
        "        float2 t%u_base = input.tex%u.xy / project_w(input.tex%u.w);\n",
        texture_convolution_filter_name(key->texture_convolution_filter[stage]),
        stage, stage, stage, stage, stage, stage, stage, stage, stage, stage,
        stage, stage);

    for (unsigned int i = 0; i < ARRAY_SIZE(convolution_kernel); i++) {
        g_string_append_printf(
            source,
            "        t%u += sample_tex_offset%u(t%u_base, "
            "t%u_texel * float2(%f, %f)) * %s;\n",
            stage, stage, stage, stage, convolution_kernel[i].x,
            convolution_kernel[i].y, convolution_kernel[i].weight);
    }

    g_string_append(source, "    }\n");
}

static void append_texture_shader_stage(
    GString *source, const PGRAPHD3D12PipelineKey *key, unsigned int stage)
{
    unsigned int mode = texture_stage_mode(key, stage);
    int input = texture_stage_input(key, stage);
    unsigned int dot_map = texture_stage_dot_map(key, stage);
    /* Also decides whether the key may discard; see
     * canonicalize_pipeline_key(). */
    bool sampled = texture_stage_samples(key, stage);
    char in_buf[16];
    const char *in = texture_stage_raw_name(key, input, in_buf, sizeof(in_buf));

    if (!(texture_stage_execution_mask(key) & (1u << stage))) {
        return;
    }

    g_string_append_printf(source, "    // Texture shader stage %u mode 0x%x\n",
                           stage, mode);
    switch (mode) {
    case PS_TEXTUREMODES_NONE:
        g_string_append_printf(
            source,
            "    t%u = float4(0.0, 0.0, 0.0, 1.0);\n",
            stage);
        break;
    case PS_TEXTUREMODES_PROJECT2D:
    case PS_TEXTUREMODES_PROJECT3D:
        if (key->texture_depth_mask & (1u << stage)) {
            append_shadow_texture_sample(
                source, key, stage, mode == PS_TEXTUREMODES_PROJECT3D);
        } else if (texture_stage_uses_convolution(key, stage, mode)) {
            append_convolution_texture_sample(source, key, stage);
        } else if (mode == PS_TEXTUREMODES_PROJECT3D &&
                   (key->texture_3d_mask & (1u << stage))) {
            g_string_append_printf(
                source,
                "    t%u = sample_tex3d%u(input.tex%u.xyz / "
                "project_w(input.tex%u.w));\n",
                stage, stage, stage, stage);
        } else if (mode == PS_TEXTUREMODES_PROJECT2D &&
                   (key->texture_cubemap_mask & (1u << stage))) {
            /* Match remap2DToCube(pT.xyw) in the GLSL renderer. */
            g_string_append_printf(
                source,
                "    float2 project2d%u = input.tex%u.xy / "
                "project_w(input.tex%u.w);\n"
                "    t%u = sample_tex_cube%u(normalize(float3(1.0, "
                "project2d%u.y, -project2d%u.x)));\n",
                stage, stage, stage, stage, stage, stage, stage);
        } else {
            g_string_append_printf(
                source,
                "    t%u = sample_tex%u(input.tex%u.xy / "
                "project_w(input.tex%u.w));\n",
                stage, stage, stage, stage);
        }
        break;
    case PS_TEXTUREMODES_CUBEMAP:
        g_string_append_printf(
            source,
            "    t%u = sample_tex_cube%u(input.tex%u.xyz);\n",
            stage, stage, stage);
        break;
    case PS_TEXTUREMODES_PASSTHRU:
        g_string_append_printf(source, "    t%u = input.tex%u;\n",
                               stage, stage);
        break;
    case PS_TEXTUREMODES_CLIPPLANE:
        g_string_append_printf(
            source,
            "    t%u = float4(0.0, 0.0, 0.0, 0.0);\n",
            stage);
        for (unsigned int component = 0; component < 4; component++) {
            bool greater_equal =
                (key->shader_clip_mode >> (4 * stage + component)) & 1;
            g_string_append_printf(
                source,
                "    if (input.tex%u.%c %s 0.0) discard;\n",
                stage, "xyzw"[component],
                greater_equal ? ">=" : "<");
        }
        break;
    case PS_TEXTUREMODES_BUMPENVMAP:
    case PS_TEXTUREMODES_BUMPENVMAP_LUM:
        if (!sampled) {
            g_string_append_printf(
                source,
                "    t%u = float4(0.0, 0.0, 0.0, 0.0); "
                "// invalid bump input texture %d\n",
                stage, input);
            break;
        }
        if (mode == PS_TEXTUREMODES_BUMPENVMAP) {
            if (key->texture_snorm_mask & (1u << input)) {
                g_string_append_printf(source,
                                       "    float2 dsdt%u = t%d.bg;\n",
                                       stage, input);
            } else {
                g_string_append_printf(
                    source,
                    "    float2 dsdt%u = float2(sign3(%s.b), "
                    "sign3(%s.g));\n",
                    stage, in, in);
            }
            g_string_append_printf(
                source,
                "    dsdt%u = bumpMatrixMul(%u, dsdt%u);\n",
                stage, stage, stage);
            if (key->texture_3d_mask & (1u << stage)) {
                g_string_append_printf(
                    source,
                    "    t%u = sample_tex3d%u(float3(input.tex%u.xy + "
                    "dsdt%u, input.tex%u.z));\n",
                    stage, stage, stage, stage, stage);
            } else {
                g_string_append_printf(
                    source,
                    "    t%u = sample_tex%u(input.tex%u.xy + dsdt%u);\n",
                    stage, stage, stage, stage);
            }
        } else {
            if (key->texture_snorm_mask & (1u << input)) {
                /* du/dv are signed; the L6 luminance is stored as 0..1. */
                g_string_append_printf(
                    source,
                    "    float3 dsdtl%u = float3(t%d.b, t%d.g, t%d.r);\n",
                    stage, input, input, input);
            } else {
                g_string_append_printf(
                    source,
                    "    float3 dsdtl%u = float3(sign3(%s.b), "
                    "sign3(%s.g), %s.r);\n",
                    stage, in, in, in);
            }
            g_string_append_printf(
                source,
                "    dsdtl%u.xy = bumpMatrixMul(%u, dsdtl%u.xy);\n",
                stage, stage, stage);
            if (key->texture_3d_mask & (1u << stage)) {
                g_string_append_printf(
                    source,
                    "    t%u = sample_tex3d%u(float3(input.tex%u.xy + "
                    "dsdtl%u.xy, input.tex%u.z));\n",
                    stage, stage, stage, stage, stage);
            } else {
                g_string_append_printf(
                    source,
                    "    t%u = sample_tex%u(input.tex%u.xy + dsdtl%u.xy);\n",
                    stage, stage, stage, stage);
            }
            g_string_append_printf(
                source,
                "    t%u *= (bumpScale[%u] * dsdtl%u.z + "
                "bumpOffset[%u]);\n",
                stage, stage, stage, stage);
        }
        break;
    case PS_TEXTUREMODES_BRDF:
        g_string_append_printf(
            source,
            "    t%u = float4(0.0, 0.0, 0.0, 0.0); "
            "// PS_TEXTUREMODES_BRDF is not implemented\n",
            stage);
        break;
    case PS_TEXTUREMODES_DOT_ST:
        if (!sampled) {
            g_string_append_printf(
                source,
                "    t%u = float4(0.0, 0.0, 0.0, 0.0); "
                "// invalid DOT_ST input texture %d\n",
                stage, input);
            break;
        }
        g_string_append_printf(
            source,
            "    dot%u = dot(input.tex%u.xyz, dotmap(%uu, %s));\n"
            "    t%u = sample_tex%u(float2(dot%u, dot%u));\n",
            stage, stage, dot_map, in, stage, stage,
            stage > 0 ? stage - 1 : stage, stage);
        break;
    case PS_TEXTUREMODES_DOT_ZW:
        if (input >= 0 && input < NV2A_MAX_TEXTURES) {
            g_string_append_printf(
                source,
                "    dot%u = dot(input.tex%u.xyz, dotmap(%uu, %s));\n",
                stage, stage, dot_map, in);
        }
        g_string_append_printf(
            source,
            "    t%u = float4(0.0, 0.0, 0.0, 0.0);\n",
            stage);
        break;
    case PS_TEXTUREMODES_DOT_RFLCT_DIFF:
        if (!sampled) {
            g_string_append_printf(
                source,
                "    t%u = float4(0.0, 0.0, 0.0, 0.0); "
                "// invalid DOT_RFLCT_DIFF stage/input %u/%d\n",
                stage, stage, input);
            break;
        }
        {
            int next_input = texture_stage_input(key, 3);
            unsigned int next_dot_map = texture_stage_dot_map(key, 3);
            char next_buf[16];

            if (next_input < 0 || next_input >= NV2A_MAX_TEXTURES) {
                next_input = input;
            }
            g_string_append_printf(
                source,
                "    dot2 = dot(input.tex2.xyz, dotmap(%uu, %s));\n"
                "    float dot2_n = dot(input.tex3.xyz, "
                "dotmap(%uu, %s));\n"
                "    float3 n_2 = float3(dot1, dot2, dot2_n);\n"
                "    t2 = sample_tex_cube2(n_2);\n",
                dot_map, in, next_dot_map,
                texture_stage_raw_name(key, next_input, next_buf,
                                       sizeof(next_buf)));
        }
        break;
    case PS_TEXTUREMODES_DOT_RFLCT_SPEC:
        if (!sampled) {
            g_string_append_printf(
                source,
                "    t%u = float4(0.0, 0.0, 0.0, 0.0); "
                "// invalid DOT_RFLCT_SPEC stage/input %u/%d\n",
                stage, stage, input);
            break;
        }
        g_string_append_printf(
            source,
            "    dot3 = dot(input.tex3.xyz, dotmap(%uu, %s));\n"
            "    float3 n_3 = float3(dot1, dot2, dot3);\n"
            "    float3 e_3 = float3(input.tex1.w, input.tex2.w, "
            "input.tex3.w);\n"
            "    float n_3_len2 = max(abs(dot(n_3, n_3)), 1e-20);\n"
            "    float3 rv_3 = 2.0 * n_3 * dot(n_3, e_3) / "
            "n_3_len2 - e_3;\n"
            "    t3 = sample_tex_cube3(rv_3);\n",
            dot_map, in);
        break;
    case PS_TEXTUREMODES_DOT_STR_3D:
        if (!sampled) {
            g_string_append_printf(
                source,
                "    t%u = float4(0.0, 0.0, 0.0, 0.0); "
                "// invalid DOT_STR_3D stage/input %u/%d\n",
                stage, stage, input);
            break;
        }
        if (key->texture_3d_mask & (1u << stage)) {
            g_string_append_printf(
                source,
                "    dot3 = dot(input.tex3.xyz, dotmap(%uu, %s));\n"
                "    t3 = sample_tex3d3(float3(dot1, dot2, dot3));\n",
                dot_map, in);
        } else {
            g_string_append_printf(
                source,
                "    dot3 = dot(input.tex3.xyz, dotmap(%uu, %s));\n"
                "    t3 = sample_tex3(float2(dot1, dot2));\n",
                dot_map, in);
        }
        break;
    case PS_TEXTUREMODES_DOT_STR_CUBE:
        if (!sampled) {
            g_string_append_printf(
                source,
                "    t%u = float4(0.0, 0.0, 0.0, 0.0); "
                "// invalid DOT_STR_CUBE stage/input %u/%d\n",
                stage, stage, input);
            break;
        }
        g_string_append_printf(
            source,
            "    dot3 = dot(input.tex3.xyz, dotmap(%uu, %s));\n"
            "    float3 dotSTR3Cube = float3(dot1, dot2, dot3);\n"
            "    t3 = sample_tex_cube3(dotSTR3Cube);\n",
            dot_map, in);
        break;
    case PS_TEXTUREMODES_DPNDNT_AR:
        if (!sampled) {
            g_string_append_printf(
                source,
                "    t%u = float4(0.0, 0.0, 0.0, 0.0); "
                "// invalid DPNDNT_AR input texture %d\n",
                stage, input);
            break;
        }
        g_string_append_printf(
            source,
            "    t%u = sample_tex%u(float2(%s.a, %s.r));\n",
            stage, stage, in, in);
        break;
    case PS_TEXTUREMODES_DPNDNT_GB:
        if (!sampled) {
            g_string_append_printf(
                source,
                "    t%u = float4(0.0, 0.0, 0.0, 0.0); "
                "// invalid DPNDNT_GB input texture %d\n",
                stage, input);
            break;
        }
        g_string_append_printf(
            source,
            "    t%u = sample_tex%u(%s.gb);\n",
            stage, stage, in);
        break;
    case PS_TEXTUREMODES_DOTPRODUCT:
        if (input >= 0 && input < NV2A_MAX_TEXTURES) {
            g_string_append_printf(
                source,
                "    dot%u = dot(input.tex%u.xyz, dotmap(%uu, %s));\n",
                stage, stage, dot_map, in);
        }
        g_string_append_printf(
            source,
            "    t%u = float4(0.0, 0.0, 0.0, 0.0);\n",
            stage);
        break;
    case PS_TEXTUREMODES_DOT_RFLCT_SPEC_CONST:
        g_string_append_printf(
            source,
            "    t%u = float4(0.0, 0.0, 0.0, 0.0); "
            "// PS_TEXTUREMODES_DOT_RFLCT_SPEC_CONST not implemented\n",
            stage);
        break;
    default:
        g_string_append_printf(
            source,
            "    t%u = float4(0.0, 0.0, 0.0, 0.0); "
            "// reserved texture shader mode 0x%x\n",
            stage, mode);
        break;
    }

    append_texture_sample_epilogue(source, key, stage, sampled);
}

/* NV2A compare function selected at run time; 7 and unknown values pass.
 * Explicit compares keep NaN results identical to the static operators. */
static void append_compare_func_helper(GString *source, const char *name,
                                       const char *type)
{
    g_string_append_printf(
        source,
        "bool %s(uint func, %s value, %s ref)\n"
        "{\n"
        "    bool passed = true;\n"
        "    switch (func) {\n"
        "    case 0u: passed = false; break;\n"
        "    case 1u: passed = value < ref; break;\n"
        "    case 2u: passed = value == ref; break;\n"
        "    case 3u: passed = value <= ref; break;\n"
        "    case 4u: passed = value > ref; break;\n"
        "    case 5u: passed = value != ref; break;\n"
        "    case 6u: passed = value >= ref; break;\n"
        "    default: break;\n"
        "    }\n"
        "    return passed;\n"
        "}\n",
        name, type, type);
}

static void append_alpha_test_shader(GString *source,
                                     const PGRAPHD3D12PipelineKey *key)
{
    if (!key->alpha_test_enable) {
        return;
    }

    /* Function and reference come from pixel constants; 7 passes. */
    g_string_append(
        source,
        "    int frag_alpha = (int)round(color.a * 255.0);\n"
        "    if (!alpha_compare(alphaTest.y, frag_alpha, "
        "(int)alphaTest.x)) {\n"
        "        discard;\n"
        "    }\n");
}

static void append_shader_src_blend_factor(GString *source,
                                           const PGRAPHD3D12PipelineKey *key)
{
    if (!key->shader_src_blend_factor_enable) {
        return;
    }

    switch (key->shader_src_blend_factor) {
    case NV_PGRAPH_BLEND_SFACTOR_CONSTANT_COLOR:
        g_string_append(source, "    color *= blendColor;\n");
        break;
    case NV_PGRAPH_BLEND_SFACTOR_ONE_MINUS_CONSTANT_COLOR:
        g_string_append(source, "    color *= (1.0 - blendColor);\n");
        break;
    case NV_PGRAPH_BLEND_SFACTOR_CONSTANT_ALPHA:
        g_string_append(source, "    color *= blendColor.a;\n");
        break;
    case NV_PGRAPH_BLEND_SFACTOR_ONE_MINUS_CONSTANT_ALPHA:
        g_string_append(source, "    color *= (1.0 - blendColor.a);\n");
        break;
    default:
        g_string_append_printf(
            source,
            "    /* Unsupported baked NV2A source blend factor %u; "
            "leave color unmodified. */\n",
            key->shader_src_blend_factor);
        break;
    }
}

static void framebuffer_fetch_logic_mask(
    const PGRAPHD3D12PipelineKey *key, uint32_t mask[4])
{
    switch (key->rtv_format) {
    case DXGI_FORMAT_B5G6R5_UNORM:
        mask[0] = 31;
        mask[1] = 63;
        mask[2] = 31;
        mask[3] = 1;
        break;
    case DXGI_FORMAT_B5G5R5A1_UNORM:
        mask[0] = 31;
        mask[1] = 31;
        mask[2] = 31;
        mask[3] = 1;
        break;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R8_UNORM:
    default:
        mask[0] = 255;
        mask[1] = 255;
        mask[2] = 255;
        mask[3] = 255;
        break;
    }
}

static void append_framebuffer_fetch_shader_helpers(
    GString *source, const PGRAPHD3D12PipelineKey *key)
{
    uint32_t logic_mask[4];

    if (!key->shader_framebuffer_fetch) {
        return;
    }

    framebuffer_fetch_logic_mask(key, logic_mask);
    g_string_append(
        source,
        "float4 shaderBlendFactor(uint factor, float4 src, float4 dst)\n"
        "{\n"
        "    switch (factor) {\n"
        "    case 0u: return float4(0.0, 0.0, 0.0, 0.0);\n"
        "    case 1u: return float4(1.0, 1.0, 1.0, 1.0);\n"
        "    case 2u: return src;\n"
        "    case 3u: return 1.0 - src;\n"
        "    case 4u: return src.aaaa;\n"
        "    case 5u: return 1.0 - src.aaaa;\n"
        "    case 6u: return dst.aaaa;\n"
        "    case 7u: return 1.0 - dst.aaaa;\n"
        "    case 8u: return dst;\n"
        "    case 9u: return 1.0 - dst;\n"
        "    case 10u:\n"
        "        return float4(min(src.a, 1.0 - dst.a),\n"
        "                      min(src.a, 1.0 - dst.a),\n"
        "                      min(src.a, 1.0 - dst.a), 1.0);\n"
        "    case 12u: return blendColor;\n"
        "    case 13u: return 1.0 - blendColor;\n"
        "    case 14u: return blendColor.aaaa;\n"
        "    case 15u: return 1.0 - blendColor.aaaa;\n"
        "    default: return float4(1.0, 1.0, 1.0, 1.0);\n"
        "    }\n"
        "}\n"
        "float4 shaderBlendEquation(uint equation, float4 srcTerm,\n"
        "                           float4 dstTerm)\n"
        "{\n"
        "    switch (equation) {\n"
        "    case 0u: return srcTerm - dstTerm;\n"
        "    case 1u: return dstTerm - srcTerm;\n"
        "    case 2u: return srcTerm + dstTerm;\n"
        "    case 3u: return min(srcTerm, dstTerm);\n"
        "    case 4u: return max(srcTerm, dstTerm);\n"
        "    case 5u: return dstTerm - srcTerm;\n"
        "    case 6u: return srcTerm + dstTerm;\n"
        "    default: return srcTerm;\n"
        "    }\n"
        "}\n"
        "uint4 framebufferLogicMask()\n"
        "{\n"
    );
    g_string_append_printf(
        source,
        "    return uint4(%uu, %uu, %uu, %uu);\n",
        logic_mask[0], logic_mask[1], logic_mask[2], logic_mask[3]);
    g_string_append(
        source,
        "}\n"
        "uint4 colorToLogic(float4 value, uint4 full)\n"
        "{\n"
        "    return (uint4)round(saturate(value) * (float4)full);\n"
        "}\n"
        "float4 logicToColor(uint4 value, uint4 full)\n"
        "{\n"
        "    return (float4)(value & full) / (float4)full;\n"
        "}\n"
        "uint4 shaderLogicOp(uint op, uint4 src, uint4 dst)\n"
        "{\n"
        "    uint4 full = framebufferLogicMask();\n"
        "    src &= full;\n"
        "    dst &= full;\n"
        "    switch (op) {\n"
        "    case 0u: return uint4(0u, 0u, 0u, 0u);\n"
        "    case 1u: return src & dst;\n"
        "    case 2u: return src & ((~dst) & full);\n"
        "    case 3u: return src;\n"
        "    case 4u: return ((~src) & full) & dst;\n"
        "    case 5u: return dst;\n"
        "    case 6u: return src ^ dst;\n"
        "    case 7u: return src | dst;\n"
        "    case 8u: return (~(src | dst)) & full;\n"
        "    case 9u: return (~(src ^ dst)) & full;\n"
        "    case 10u: return (~dst) & full;\n"
        "    case 11u: return src | ((~dst) & full);\n"
        "    case 12u: return (~src) & full;\n"
        "    case 13u: return ((~src) & full) | dst;\n"
        "    case 14u: return (~(src & dst)) & full;\n"
        "    case 15u: return full;\n"
        "    default: return src;\n"
        "    }\n"
        "}\n");
}

static void append_framebuffer_fetch_shader(
    GString *source, const PGRAPHD3D12PipelineKey *key)
{
    if (!key->shader_framebuffer_fetch) {
        return;
    }

    g_string_append_printf(
        source,
        "    int2 framebufferFetchCoord = int2(floor(input.position.x), "
        "floor(input.position.y));\n"
        "    float4 framebufferDest = framebufferCopy.Load(\n"
        "        int3(framebufferFetchCoord, 0));\n");
    if (key->shader_blend_enable) {
        g_string_append_printf(
            source,
            "    float4 shaderSrcFactor = shaderBlendFactor(%uu, color, "
            "framebufferDest);\n"
            "    float4 shaderDstFactor = shaderBlendFactor(%uu, color, "
            "framebufferDest);\n"
            "    color = saturate(shaderBlendEquation(%uu, "
            "color * shaderSrcFactor, framebufferDest * "
            "shaderDstFactor));\n",
            key->shader_blend_src_factor, key->shader_blend_dest_factor,
            key->shader_blend_equation);
    }
    if (key->shader_logic_op_enable) {
        g_string_append_printf(
            source,
            "    uint4 logicMask = framebufferLogicMask();\n"
            "    color = logicToColor(shaderLogicOp(%uu, "
            "colorToLogic(color, logicMask), "
            "colorToLogic(framebufferDest, logicMask)), logicMask);\n",
            key->shader_logic_op);
    }
}

static void append_window_clip_shader(GString *source,
                                      const PGRAPHD3D12PipelineKey *key)
{
    g_string_append_printf(
        source,
        "    // Window clip (%slusive)\n"
        "    float2 windowClipCoord = input.position.xy - "
        "float2(0.5, 0.5);\n",
        key->window_clip_exclusive ? "exc" : "inc");

    if (!key->window_clip_exclusive) {
        g_string_append(source, "    bool windowClipContained = false;\n");
    }

    g_string_append(
        source,
        "    for (uint windowClipIndex = 0u; windowClipIndex < 8u; "
        "windowClipIndex++) {\n"
        "        uint4 r = clipRegion[windowClipIndex];\n"
        "        bool2 below = windowClipCoord < "
        "float2((float)r.x, (float)r.y);\n"
        "        bool2 above = windowClipCoord >= "
        "float2((float)r.z, (float)r.w);\n"
        "        bool outside = any(below) || any(above);\n"
        "        if (!outside) {\n");
    if (key->window_clip_exclusive) {
        g_string_append(source, "            discard;\n");
    } else {
        g_string_append(source,
                        "            windowClipContained = true;\n"
                        "            break;\n");
    }
    g_string_append(source,
                    "        }\n"
                    "    }\n");
    if (!key->window_clip_exclusive) {
        g_string_append(source,
                        "    if (!windowClipContained) {\n"
                        "        discard;\n"
                        "    }\n");
    }
}

/*
 * The Xbox UWP AMD driver corrupts the geometry-shader depth payload, so
 * UWP derives fragment depth from the interpolated SV_Position.w and
 * screen-space z in the pixel shader instead.
 * XEMU_D3D12_NO_GS_DEPTH=1 selects that variant elsewhere.
 */
static bool pgraph_d3d12_gs_depth_payload_enabled(void)
{
#ifdef XEMU_UWP
    return false;
#else
    static int cached = -1;

    if (cached < 0) {
        const char *env = getenv("XEMU_D3D12_NO_GS_DEPTH");

        cached = env == NULL || env[0] != '1';
    }
    return cached;
#endif
}

/*
 * NV2A computes fragment depth after rasterization: the polygon offset, the
 * ZCLIPMIN/ZCLIPMAX cull or clamp and the integer-depth floor all apply per
 * fragment, so the pixel shader writes depth for every depth-tested draw.
 */
static bool pipeline_key_shader_depth_output(const PGRAPHD3D12PipelineKey *key)
{
    if ((key->tune_flags & PGRAPH_D3D12_TUNE_ZDEPTH_NATIVE) &&
        !key->z_perspective) {
        return false;
    }
    return key->dsv_format != DXGI_FORMAT_UNKNOWN && key->depth_enable;
}

/* ZCLAMP_EN=CULL discards fragments outside the clip range even when no
 * depth is tested or stored. */
static bool pipeline_key_fragment_zvalue(const PGRAPHD3D12PipelineKey *key)
{
    return pipeline_key_shader_depth_output(key) || key->depth_clipping;
}

static bool pipeline_key_gs_depth_payload(const PGRAPHD3D12PipelineKey *key)
{
    return pipeline_key_shader_depth_output(key) &&
           pgraph_d3d12_gs_depth_payload_enabled();
}

/*
 * Screen-space z reaches the pixel shader as (z * w, w) with perspective
 * interpolation and is recovered as their ratio. For an ordinary triangle
 * that is the same screen-linear z; for one with a vertex behind the
 * camera the clip-space interpolation stays valid, where a noperspective
 * value is interpolated from the mirrored projection of that vertex.
 */
static bool pipeline_key_perspective_depth(const PGRAPHD3D12PipelineKey *key)
{
    return pipeline_key_fragment_zvalue(key) && !key->z_perspective &&
           !pipeline_key_gs_depth_payload(key) &&
           !(key->tune_flags & PGRAPH_D3D12_TUNE_ZINTERP_SCREEN);
}

/* depthPos as the rasterizer interpolates it, from the last stage before
 * rasterization. */
static const char *rasterized_depth_pos_expr(
    const PGRAPHD3D12PipelineKey *key, bool last_stage, const char *value)
{
    /* one per thread: the PSO workers and the FIFO thread write shaders at the same time; each caller
     * formats the result before its next call */
    static __thread char expr[128];

    if (!last_stage || !pipeline_key_perspective_depth(key)) {
        snprintf(expr, sizeof(expr), "%s", value);
    } else {
        snprintf(expr, sizeof(expr),
                 "float4(%s.xy, %s.z * %s.w, %s.w)", value, value, value,
                 value);
    }
    return expr;
}

/*
 * Line and point polygon modes: the rasterized edges and points have no
 * depth slope of their own, so the geometry shader passes the source
 * polygon's slope in TEXCOORD7 for the polygon offset.
 */
static bool pipeline_key_primitive_slope(const PGRAPHD3D12PipelineKey *key)
{
    return key->polygon_mode != POLY_MODE_FILL &&
           pipeline_key_fragment_zvalue(key) &&
           !pipeline_key_gs_depth_payload(key);
}

static const char *depth_format_output_expr(PGRAPHD3D12DepthFormat format)
{
    /* Integer depth buffers floor. For D24 the +1 ulp on floor(z) / 2^24
     * gives floor(z) / (2^24 - 1) after the UNORM conversion. */
    switch (format) {
    case PGRAPH_D3D12_DEPTH_FORMAT_D16:
        return "floor(fragmentZ) / 65535.0";
    case PGRAPH_D3D12_DEPTH_FORMAT_D24:
        return "asfloat(asuint(floor(fragmentZ) / 16777216.0) + 1u)";
    default:
        return "fragmentZ / clipRange.y";
    }
}

static const char *fragment_depth_output_expr(
    const PGRAPHD3D12PipelineKey *key)
{
    return depth_format_output_expr(key->depth_format);
}

/* b1 of the draw pixel shaders, PGRAPHD3D12PixelConstants. */
static const char pixel_constants_hlsl[] =
    "cbuffer PixelConstants : register(b1) {\n"
    "    float4 combinerConst[18];\n"
    "    uint4 colorKey;\n"
    "    uint4 colorKeyMask;\n"
    "    float4 fogColor;\n"
    "    float4 bumpMat[4];\n"
    "    float4 bumpScale;\n"
    "    float4 bumpOffset;\n"
    "    float4 texScale;\n"
    "    float4 blendColor;\n"
    "    float4 clipRange;\n"
    "    float4 depthInfo;\n"
    "    uint4 clipRegion[8];\n"
    "    uint4 alphaTest;\n"
    "    float4 texSize[4];\n"
    "};\n";

static void append_color_key_helper(GString *source)
{
    g_string_append(source,
                    "bool check_color_key(float4 texel, uint key, "
                    "uint mask)\n"
                    "{\n"
                    "    uint4 c = (uint4)max(texel * 255.0 + 0.5, "
                    "0.0);\n"
                    "    uint color = (c.a << 24) | (c.r << 16) | "
                    "(c.g << 8) | c.b;\n"
                    "    return (color & mask) == (key & mask);\n"
                    "}\n");
}

/* The pixel shader input of a key, in the member order of the stage that
 * feeds it. */
static void append_ps_input_struct(GString *source,
                                   const PGRAPHD3D12PipelineKey *key)
{
    g_string_append(source,
                    "struct PSInput {\n"
                    "    float4 position : SV_Position;\n"
                    "    float4 diffuse : TEXCOORD0;\n"
                    "    float4 specular : TEXCOORD1;\n"
                    "    float fog : TEXCOORD2;\n");
    if (pipeline_key_primitive_slope(key)) {
        g_string_append(source, "    float primitiveSlope : TEXCOORD7;\n");
    }
    for (unsigned int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        if (key->texture_coord_mask & (1u << i)) {
            g_string_append_printf(source,
                                   "    float4 tex%u : TEXCOORD%u;\n",
                                   i, i + 3);
        }
    }
    /* Stage-boundary member order: depthPos follows the texture
     * coordinates, then the optional geometry-shader depth payload. */
    if (pipeline_key_gs_depth_payload(key)) {
        g_string_append(source,
                        "    float4 depthPos : TEXCOORD8;\n"
                        "    nointerpolation float4 depthPos0 : TEXCOORD9;\n"
                        "    nointerpolation float4 depthPos1 : TEXCOORD10;\n"
                        "    nointerpolation float4 depthPos2 : TEXCOORD11;\n"
                        "    nointerpolation float depthSlope : TEXCOORD12;\n");
    } else if (pipeline_key_perspective_depth(key)) {
        g_string_append(source,
                        "    float4 depthPos : TEXCOORD8;\n");
    } else if (pipeline_key_fragment_zvalue(key) && !key->z_perspective) {
        g_string_append(source,
                        "    noperspective float4 depthPos : TEXCOORD8;\n");
    }
    g_string_append(source, "};\n");
}

/*
 * The fragment depth function: z_perspective takes the w-buffer value,
 * otherwise screen-space z from depthPos, as (z * w, w) with
 * perspective_depth. The ZCLIPMIN/ZCLIPMAX cull of depth_clipping becomes
 * a run-time choice on clip_condition when that is set.
 */
static void append_fragment_zvalue_function(GString *source,
                                            const char *name,
                                            bool z_perspective,
                                            bool perspective_depth,
                                            bool primitive_slope,
                                            bool depth_clipping,
                                            const char *clip_condition)
{
    g_string_append_printf(source,
                           "float %s(PSInput input)\n"
                           "{\n"
                           "    float2 guestScale = max(depthInfo.xy, "
                           "float2(1.0, 1.0));\n",
                           name);
    if (z_perspective) {
        /* In the pixel shader SV_Position.w is the perspective-correct
         * clip w, the value NV2A stores in a w-buffer. Derivatives are
         * per render-target pixel; the polygon offset slope is defined
         * per guest pixel. */
        g_string_append(source,
                        "    precise float zvalue = input.position.w;\n");
        /* The source polygon's slope is that of 1/w; the w slope
         * scales it by w^2. */
        g_string_append(
            source,
            primitive_slope ?
            "    float slope = input.primitiveSlope * zvalue * zvalue;\n" :
            "    float slope = max(abs(ddx(zvalue)) * guestScale.x,\n"
            "                      abs(ddy(zvalue)) * guestScale.y);\n");
        /* Helper lanes can extrapolate to a non-finite slope; with the
         * offset factor at zero that would still turn depth into NaN. */
        g_string_append(
            source,
            "    if (zvalue > 0.0) {\n"
            "        zvalue += depthInfo.z;\n"
            "        if (depthInfo.w != 0.0 && !float_is_nan(slope) &&\n"
            "            !float_is_inf(slope)) {\n"
            "            zvalue += depthInfo.w * slope;\n"
            "        }\n"
            "    } else {\n"
            "        zvalue = asfloat(0x7f7fffff);\n"
            "    }\n"
            "    if (float_is_nan(zvalue) || float_is_inf(zvalue)) {\n"
            "        zvalue = asfloat(0x7f7fffff);\n"
            "    }\n"
            /* The w-buffer value saturates to the depth format's
             * range before the clip test; only w <= 0, mapped to the
             * largest float above, lies beyond it. */
            "    if (zvalue < asfloat(0x7f7fffff)) {\n"
            "        zvalue = min(zvalue, clipRange.y);\n"
            "    }\n");
    } else {
        /* Screen-space z is linear in screen space. The slope term is
         * the primitive's largest depth gradient per guest pixel. */
        g_string_append(
            source,
            perspective_depth ?
            "    precise float zvalue = input.depthPos.z / "
            "input.depthPos.w;\n" :
            "    precise float zvalue = input.depthPos.z;\n");
        g_string_append(
            source,
            primitive_slope ?
            "    float slope = input.primitiveSlope;\n" :
            "    float slope = max(abs(ddx(zvalue)) * guestScale.x,\n"
            "                      abs(ddy(zvalue)) * guestScale.y);\n");
        /* As above: 0 * Inf from a helper-lane slope is NaN. */
        g_string_append(
            source,
            "    zvalue += depthInfo.z;\n"
            "    if (depthInfo.w != 0.0 && !float_is_nan(slope) &&\n"
            "        !float_is_inf(slope)) {\n"
            "        zvalue += depthInfo.w * slope;\n"
            "    }\n");
    }
    if (clip_condition != NULL) {
        g_string_append_printf(
            source,
            "    if (%s) {\n"
            "    if (zvalue < clipRange.z || clipRange.w < zvalue) {\n"
            "        discard;\n"
            "    }\n"
            "    } else {\n"
            "    zvalue = clamp(zvalue, clipRange.z, clipRange.w);\n"
            "    }\n",
            clip_condition);
    } else if (depth_clipping) {
        g_string_append(
            source,
            "    if (zvalue < clipRange.z || clipRange.w < zvalue) {\n"
            "        discard;\n"
            "    }\n");
    } else {
        g_string_append(
            source,
            "    zvalue = clamp(zvalue, clipRange.z, clipRange.w);\n");
    }
    g_string_append(source,
                    "    return zvalue;\n"
                    "}\n");
}

static char *build_minimal_pixel_shader(const PGRAPHD3D12PipelineKey *key)
{
    GString *source = g_string_new("");
    bool has_color_target = key->rtv_format != DXGI_FORMAT_UNKNOWN;
    bool depth_output = pipeline_key_shader_depth_output(key);
    bool fragment_zvalue = pipeline_key_fragment_zvalue(key);
    bool gs_depth_payload = pipeline_key_gs_depth_payload(key);
    bool primitive_slope = pipeline_key_primitive_slope(key);
    unsigned int num_stages = MIN(key->combiner_control & 0xff, 8);
    bool has_color_key = key_has_color_key(key);
    uint8_t texture_execution_mask = texture_stage_execution_mask(key);

    if (key->shader_framebuffer_fetch) {
        g_string_append_printf(
            source,
            "Texture2D framebufferCopy : register(t%u);\n",
            PGRAPH_D3D12_FRAMEBUFFER_SRV_REGISTER);
    }
    for (unsigned int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        if (key->texture_mask & (1u << i)) {
            if (key->texture_3d_mask & (1u << i)) {
                g_string_append_printf(
                    source,
                    "Texture3D tex%u : register(t%u);\n"
                    "SamplerState samp%u : register(s%u);\n",
                    i, i, i, i);
            } else if ((key->texture_cubemap_mask & (1u << i)) &&
                       (key->texture_border_adjust_mask & (1u << i))) {
                g_string_append_printf(
                    source,
                    "Texture2DArray tex%u : register(t%u);\n"
                    "SamplerState samp%u : register(s%u);\n",
                    i, i, i, i);
            } else if (key->texture_cubemap_mask & (1u << i)) {
                g_string_append_printf(
                    source,
                    "TextureCube tex%u : register(t%u);\n"
                    "SamplerState samp%u : register(s%u);\n",
                    i, i, i, i);
            } else {
                g_string_append_printf(
                    source,
                    "Texture2D tex%u : register(t%u);\n"
                    "SamplerState samp%u : register(s%u);\n",
                    i, i, i, i);
            }
        }
    }
    g_string_append(source, pixel_constants_hlsl);
    /* alphaTest: x=alpha ref, y=alpha func, z=alpha kill stage mask,
     * w=shadow compare func; NV2A compare encodings, 7 passes. */
    if (key->alpha_test_enable) {
        append_compare_func_helper(source, "alpha_compare", "int");
    }
    if (key->texture_depth_mask &&
        !(key->tune_flags & PGRAPH_D3D12_TUNE_SHADOW_LIT)) {
        append_compare_func_helper(source, "shadow_compare", "float");
    }
    append_texture_sampling_helpers(source, key);
    if (has_color_key) {
        append_color_key_helper(source);
    }
    append_framebuffer_fetch_shader_helpers(source, key);

    append_ps_input_struct(source, key);
    append_float_class_helpers(source);
    if (fragment_zvalue && !gs_depth_payload) {
        append_fragment_zvalue_function(
            source, "fragment_zvalue", key->z_perspective,
            pipeline_key_perspective_depth(key), primitive_slope,
            key->depth_clipping, NULL);
    }
    if (gs_depth_payload) {
        g_string_append_printf(
            source,
            "float kahan_det(float2 a, float2 b)\n"
            "{\n"
            "    precise float cd = a.y * b.x;\n"
            "    precise float err = mad(-a.y, b.x, cd);\n"
            "    precise float result = mad(a.x, b.y, -cd) + err;\n"
            "    return result;\n"
            "}\n"
            "float area2(float2 a, float2 b, float2 c)\n"
            "{\n"
            "    return kahan_det(b - a, c - a);\n"
            "}\n"
            "float fragment_zvalue(PSInput input)\n"
            "{\n"
            "    float2 unscaledXY = input.position.xy /\n"
            "        max(depthInfo.xy, float2(1.0, 1.0));\n"
            "    float posMax = max(abs(input.depthPos0.x),\n"
            "                       abs(input.depthPos0.y));\n"
            "    posMax = max(posMax, max(abs(input.depthPos1.x),\n"
            "                             abs(input.depthPos1.y)));\n"
            "    posMax = max(posMax, max(abs(input.depthPos2.x),\n"
            "                             abs(input.depthPos2.y)));\n"
            "    float posExp;\n"
            "    frexp(posMax, posExp);\n"
            "    posExp = min(60.0 - posExp, 0.0);\n"
            "    float posScale = ldexp(1.0, posExp);\n"
            /* Scale the sample point too; scaling only the vertices
             * changes the barycentric weights. */
            "    precise float2 sampleXY = unscaledXY * posScale;\n"
            "    precise float2 p0 = input.depthPos0.xy * posScale;\n"
            "    precise float2 p1 = input.depthPos1.xy * posScale;\n"
            "    precise float2 p2 = input.depthPos2.xy * posScale;\n"
            "    precise float bc0 = area2(sampleXY, p1, p2);\n"
            "    precise float bc1 = area2(sampleXY, p2, p0);\n"
            "    precise float bc2 = area2(sampleXY, p0, p1);\n"
            "    precise float zvalue;\n"
            "    if (%s) {\n"
            "        bc0 /= input.depthPos0.w;\n"
            "        bc1 /= input.depthPos1.w;\n"
            "        bc2 /= input.depthPos2.w;\n"
            "        float invBcsum = 1.0 / (bc0 + bc1 + bc2);\n"
            "        if (float_is_inf(invBcsum)) {\n"
            "            invBcsum = 0.0;\n"
            "        }\n"
            "        bc1 *= invBcsum;\n"
            "        bc2 *= invBcsum;\n"
            "        zvalue = input.depthPos0.w +\n"
            "            (bc1 * (input.depthPos1.w - input.depthPos0.w) +\n"
            "             bc2 * (input.depthPos2.w - input.depthPos0.w));\n"
            "        if (zvalue > 0.0) {\n"
            "            float slopeOffset = depthInfo.w * input.depthSlope *\n"
            "                                zvalue * zvalue;\n"
            "            zvalue += depthInfo.z;\n"
            "            zvalue += slopeOffset;\n"
            "        } else {\n"
            "            zvalue = asfloat(0x7f7fffff);\n"
            "        }\n"
            "        if (float_is_nan(zvalue)) {\n"
            "            zvalue = asfloat(0x7f7fffff);\n"
            "        }\n"
            "        if (zvalue < asfloat(0x7f7fffff)) {\n"
            "            zvalue = min(zvalue, clipRange.y);\n"
            "        }\n"
            "    } else {\n"
            "        float invBcsum = 1.0 / (bc0 + bc1 + bc2);\n"
            "        if (float_is_inf(invBcsum)) {\n"
            "            invBcsum = 0.0;\n"
            "        }\n"
            "        bc1 *= invBcsum;\n"
            "        bc2 *= invBcsum;\n"
            "        zvalue = input.depthPos0.z +\n"
            "            (bc1 * (input.depthPos1.z - input.depthPos0.z) +\n"
            "             bc2 * (input.depthPos2.z - input.depthPos0.z));\n"
            "        zvalue += depthInfo.z;\n"
            "        zvalue += depthInfo.w * input.depthSlope;\n"
            "    }\n"
            "    if (float_is_nan(zvalue) || float_is_inf(zvalue)) {\n"
            "        zvalue = asfloat(0x7f7fffff);\n"
            "    }\n"
            "    if (%s) {\n"
            "        if (zvalue < clipRange.z || clipRange.w < zvalue) {\n"
            "            discard;\n"
            "        }\n"
            "    } else {\n"
            "        zvalue = clamp(zvalue, clipRange.z, clipRange.w);\n"
            "    }\n"
            "    return zvalue;\n"
            "}\n",
            key->z_perspective ? "true" : "false",
            key->depth_clipping ? "true" : "false");
    }
    if (has_color_target || depth_output) {
        g_string_append(source, "struct PSOutput {\n");
        if (has_color_target) {
            g_string_append(source, "    float4 color : SV_Target0;\n");
        }
        if (depth_output) {
            g_string_append(source, "    float depth : SV_Depth;\n");
        }
        g_string_append(source,
                        "};\n"
                        "PSOutput ps_main(PSInput input)\n");
    } else {
        g_string_append(source, "void ps_main(PSInput input)\n");
    }
    g_string_append(source, "{\n");
    if (has_color_target || depth_output) {
        g_string_append(source, "    PSOutput output;\n");
    }
    if (depth_output) {
        g_string_append_printf(
            source,
            "    precise float fragmentZ = fragment_zvalue(input);\n"
            "    output.depth = %s;\n",
            fragment_depth_output_expr(key));
    } else if (fragment_zvalue) {
        g_string_append(source, "    fragment_zvalue(input);\n");
    }
    g_string_append(source,
                    "    float4 color = saturate(input.diffuse);\n"
                    "    float4 v0 = saturate(input.diffuse);\n"
                    "    float4 v1 = saturate(input.specular);\n"
                    "    float4 pFog = float4(fogColor.rgb, saturate(input.fog));\n"
                    "    float4 r0 = float4(0.0, 0.0, 0.0, 0.0);\n"
                    "    float4 r1 = float4(0.0, 0.0, 0.0, 0.0);\n"
                    "    float4 ab = float4(0.0, 0.0, 0.0, 0.0);\n"
                    "    float4 cd = float4(0.0, 0.0, 0.0, 0.0);\n"
                    "    float4 mux_sum = float4(0.0, 0.0, 0.0, 0.0);\n"
                    "    float4 t0 = float4(0.0, 0.0, 0.0, 1.0);\n"
                    "    float4 t1 = float4(0.0, 0.0, 0.0, 1.0);\n"
                    "    float4 t2 = float4(0.0, 0.0, 0.0, 1.0);\n"
                    "    float4 t3 = float4(0.0, 0.0, 0.0, 1.0);\n");
    if (texture_execution_mask) {
        g_string_append(source,
                        "    float dot0 = 0.0;\n"
                        "    float dot1 = 0.0;\n"
                        "    float dot2 = 0.0;\n"
                        "    float dot3 = 0.0;\n");
        for (unsigned int i = 0; i < NV2A_MAX_TEXTURES; i++) {
            if (key->texture_signs[i]) {
                g_string_append_printf(
                    source, "    float4 traw%u = float4(0.0, 0.0, 0.0, 1.0);\n",
                    i);
            }
        }
    }
    append_window_clip_shader(source, key);
    if (texture_execution_mask) {
        for (unsigned int i = 0; i < NV2A_MAX_TEXTURES; i++) {
            append_texture_shader_stage(source, key, i);
        }
    }
    if (texture_execution_mask & 1) {
        g_string_append(source, "    r0.a = t0.a;\n");
    } else {
        g_string_append(source, "    r0.a = 1.0;\n");
    }
    for (unsigned int i = 0; i < num_stages; i++) {
        append_combiner_stage(source, key, i);
    }
    if ((key->tune_flags & PGRAPH_D3D12_TUNE_FC_R0) &&
        !key->final_inputs_0 && !key->final_inputs_1) {
        if (num_stages > 0) {
            g_string_append(source, "    color = r0;\n");
        } else {
            for (unsigned int i = 0; i < NV2A_MAX_TEXTURES; i++) {
                if (key->texture_mask & (1u << i)) {
                    g_string_append_printf(source, "    color *= t%u;\n", i);
                }
            }
        }
    } else {
        /* The final combiner is always active; zero control words decode
         * to all-ZERO inputs and output (0, 0, 0, 0). */
        append_final_combiner(source, key);
    }
    append_alpha_test_shader(source, key);
    append_shader_src_blend_factor(source, key);
    append_framebuffer_fetch_shader(source, key);
    if (has_color_target) {
        g_string_append(source, "    output.color = color;\n");
    }
    if (has_color_target || depth_output) {
        g_string_append(source, "    return output;\n");
    }
    g_string_append(source, "}\n");

    return g_string_free(source, FALSE);
}

/*
 * Interpreter pixel shader. One shader per interface (fill_uber_ps_
 * interface()) and class runs the texture stages, register combiners, fog,
 * alpha test, window clip, source blend factor and fragment depth of any
 * key it supports from the constants fill_uber_ps_constants() puts in b3,
 * with the arithmetic of build_minimal_pixel_shader(). A stage binds its
 * one view in spaces 0-3 and samples only the declaration of the view's
 * dimension. Samples sit outside loops; all control flow is uniform.
 */
static uint8_t fallback_ps_class(const PGRAPHD3D12PipelineKey *key);

/* Texture stage operations (uberStage[].x), as append_texture_shader_stage()
 * resolves a stage mode. */
enum {
    UBER_TEX_SKIP,              /* not run: t keeps (0, 0, 0, 1) */
    UBER_TEX_PROJECT,
    UBER_TEX_PROJECT_3D,
    UBER_TEX_PROJECT_CUBE,
    UBER_TEX_SHADOW,
    UBER_TEX_SHADOW_LIT,
    UBER_TEX_CUBEMAP,
    UBER_TEX_PASSTHRU,
    UBER_TEX_CLIPPLANE,
    /* The rest only in the full class. */
    UBER_TEX_ZERO,
    UBER_TEX_CONVOLUTION,
    UBER_TEX_BUMP,
    UBER_TEX_BUMP_LUM,
    UBER_TEX_DOT_ST,
    UBER_TEX_DOT_ZERO,          /* DOT_ZW and DOTPRODUCT */
    UBER_TEX_DOT_RFLCT_DIFF,
    UBER_TEX_DOT_RFLCT_SPEC,
    UBER_TEX_DOT_STR_3D,
    UBER_TEX_DOT_STR_CUBE,
    UBER_TEX_DPNDNT_AR,
    UBER_TEX_DPNDNT_GB,
};
#define UBER_TEX_FIRST_FULL         UBER_TEX_ZERO

/* uberStage[].y */
#define UBER_STAGE_SAMPLED          (1u << 0)   /* alpha kill, colour key */
#define UBER_STAGE_SNORM_INPUT      (1u << 1)
#define UBER_STAGE_3D               (1u << 2)
#define UBER_STAGE_LINEAR           (1u << 3)
#define UBER_STAGE_BORDER           (1u << 4)
#define UBER_STAGE_DOT_INPUT        (1u << 5)
#define UBER_STAGE_COMPARE_Z        (1u << 6)
#define UBER_STAGE_COLOR_KEY_SHIFT  8
/* uberStage[].z: input stage, dot mapping, and for DOT_RFLCT_DIFF the
 * next stage's input and dot mapping, four bits each. uberStage[].w: the
 * clip-plane tests in bits 0-3, then the swizzle and the dimension. */
#define UBER_STAGE_SWIZZLE_SHIFT    8
#define UBER_STAGE_DIM_SHIFT        16
enum {
    UBER_DIM_2D,
    UBER_DIM_CUBE,
    UBER_DIM_CUBE_ARRAY,        /* cube map with border */
    UBER_DIM_3D,
};
/* uberControl.x; .y is the source blend factor (0 none, 1-4 as in
 * append_shader_src_blend_factor()) and .z the depth format. */
#define UBER_CONTROL_ALPHA_TEST     (1u << 0)
#define UBER_CONTROL_CLIP_EXCLUSIVE (1u << 1)
#define UBER_CONTROL_DEPTH_CLIP     (1u << 2)
#define UBER_CONTROL_Z_PERSPECTIVE  (1u << 3)
/* uberControl.w: culling without a geometry shader, by SV_IsFrontFace of a
 * pipeline whose rasterizer culls nothing. */
#define UBER_CULL_FRONT_FACING      (1u << 0)
#define UBER_CULL_BACK_FACING       (1u << 1)

/* b3 of the interpreter pixel shader, uber_ps_constants_hlsl. */
typedef struct PGRAPHD3D12UberPixelConstants {
    uint32_t stage[NV2A_MAX_TEXTURES][4];
    float border_size[NV2A_MAX_TEXTURES][4];
    float border_inv[NV2A_MAX_TEXTURES][4];
    uint32_t combiner[8][4];        /* rgb in, rgb out, alpha in, out */
    uint32_t final[4];              /* final inputs 0 and 1, control */
    uint32_t control[4];
} PGRAPHD3D12UberPixelConstants;

QEMU_BUILD_BUG_ON(offsetof(PGRAPHD3D12UberPixelConstants, border_size) !=
                  4 * 16);
QEMU_BUILD_BUG_ON(offsetof(PGRAPHD3D12UberPixelConstants, border_inv) !=
                  8 * 16);
QEMU_BUILD_BUG_ON(offsetof(PGRAPHD3D12UberPixelConstants, combiner) !=
                  12 * 16);
QEMU_BUILD_BUG_ON(offsetof(PGRAPHD3D12UberPixelConstants, final) != 20 * 16);
QEMU_BUILD_BUG_ON(offsetof(PGRAPHD3D12UberPixelConstants, control) !=
                  21 * 16);
QEMU_BUILD_BUG_ON(sizeof(PGRAPHD3D12UberPixelConstants) != 22 * 16);

static const char uber_ps_constants_hlsl[] =
    "cbuffer UberPixelConstants : register(b3) {\n"
    "    uint4 uberStage[4];\n"
    "    float4 uberBorderSize[4];\n"
    "    float4 uberBorderInv[4];\n"
    "    uint4 uberCombiner[8];\n"
    "    uint4 uberFinal;\n"
    "    uint4 uberControl;\n"
    "};\n";

/* texture_swizzle_expression() results, by uberStage[].w index. */
static const char *const uber_swizzles[] = {
    "t",
    "float4(t.r, t.r, t.r, 1.0)",
    "float4(t.r, t.r, t.r, t.r)",
    "float4(1.0, 1.0, 1.0, t.r)",
    "float4(t.r, t.r, t.r, t.g)",
    "float4(t.r, t.g, t.r, t.g)",
    "float4(t.g, t.r, t.r, t.g)",
    "float4(t.r, 0.0, 0.0, 0.0)",
    "float4(t.r, 1.0, 0.0, 0.0)",
    "float4(t.r, 0.0, 1.0, 0.0)",
    "float4(t.rgb, 1.0)",
    "float4(t.g, t.b, t.a, t.r)",
    "float4(t.a, t.b, t.g, t.r)",
};

static int uber_swizzle_index(unsigned int color_format)
{
    const char *expr = texture_swizzle_expression(color_format);

    for (unsigned int i = 0; i < ARRAY_SIZE(uber_swizzles); i++) {
        if (strcmp(expr, uber_swizzles[i]) == 0) {
            return i;
        }
    }
    return -1;
}

/* Why the interpreter pixel shader cannot replace a key's, or NULL. */
static const char *uber_ps_unsupported(const PGRAPHD3D12PipelineKey *key)
{
    uint8_t sampled = texture_stage_execution_mask(key) & key->texture_mask;

    if (key->shader_framebuffer_fetch) {
        return "framebuffer fetch";
    }
    if (pipeline_key_gs_depth_payload(key)) {
        return "geometry shader depth payload";
    }
    if ((key->tune_flags & PGRAPH_D3D12_TUNE_FC_R0) &&
        !key->final_inputs_0 && !key->final_inputs_1) {
        return "fc=r0";
    }
    for (unsigned int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        if ((sampled & (1u << i)) &&
            uber_swizzle_index(key->texture_color_format[i]) < 0) {
            return "texture swizzle";
        }
        if ((sampled & (1u << i)) && key->texture_signs[i]) {
            return "signed texture channels";
        }
    }
    return NULL;
}

/* Marks a key the interpreter pixel shader can draw (fallback=ps). */
static bool mark_uber_ps_key(PGRAPHD3D12PipelineKey *key)
{
    if (uber_ps_unsupported(key) != NULL) {
        return false;
    }
    key->tune_flags |= PGRAPH_D3D12_TUNE_UBER_PS;
    return true;
}

/*
 * The interface an interpreter pixel shader is built for, as a key whose
 * other fields are zero: the outputs, and the inputs of the stage before
 * with all four texture coordinates. The w-buffer choice, the depth clip
 * and the depth format are constants.
 */
static void fill_uber_ps_interface(const PGRAPHD3D12PipelineKey *key,
                                   PGRAPHD3D12PipelineKey *io)
{
    bool depth_output = pipeline_key_shader_depth_output(key);

    memset(io, 0, sizeof(*io));
    if (key->rtv_format != DXGI_FORMAT_UNKNOWN) {
        io->rtv_format = DXGI_FORMAT_B8G8R8A8_UNORM;
    }
    if (depth_output) {
        io->dsv_format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        io->depth_enable = true;
    } else if (pipeline_key_fragment_zvalue(key)) {
        io->depth_clipping = true;
    }
    io->polygon_mode = pipeline_key_primitive_slope(key) ? POLY_MODE_LINE :
                                                           POLY_MODE_FILL;
    io->tune_flags = key->tune_flags & PGRAPH_D3D12_TUNE_ZINTERP_SCREEN;
    io->smooth_shading = true;
    io->texture_coord_mask = (1u << NV2A_MAX_TEXTURES) - 1;
}

/* The specialised vertex shader key of a fallback=ps pipeline: it writes
 * every texture coordinate the interpreter reads. */
static void uber_ps_vertex_key(const PGRAPHD3D12PipelineKey *key,
                               PGRAPHD3D12PipelineKey *vs_key)
{
    *vs_key = *key;
    vs_key->tune_flags &= ~PGRAPH_D3D12_TUNE_UBER_PS;
    vs_key->texture_coord_mask = (1u << NV2A_MAX_TEXTURES) - 1;
}

/* A float as the specialised shader bakes it, through "%f". */
static float uber_baked_float(float value)
{
    char text[64];

    snprintf(text, sizeof(text), "%f", value);
    return (float)g_ascii_strtod(text, NULL);
}

/* A stage's operation and its uberStage[].y flags. */
static uint32_t uber_stage_op(const PGRAPHD3D12PipelineKey *key,
                              unsigned int stage, uint32_t *flags)
{
    unsigned int mode = texture_stage_mode(key, stage);
    int input = texture_stage_input(key, stage);
    bool input_valid = input >= 0 && input < NV2A_MAX_TEXTURES;
    bool sampled = texture_stage_samples(key, stage);
    uint8_t bit = 1u << stage;

    *flags = 0;
    if (!(texture_stage_execution_mask(key) & bit)) {
        return UBER_TEX_SKIP;
    }
    if (sampled) {
        *flags |= UBER_STAGE_SAMPLED;
    }
    if (input_valid && (key->texture_snorm_mask & (1u << input))) {
        *flags |= UBER_STAGE_SNORM_INPUT;
    }
    if (key->texture_3d_mask & bit) {
        *flags |= UBER_STAGE_3D;
    }
    if (key->texture_linear_mask & bit) {
        *flags |= UBER_STAGE_LINEAR;
    }
    if (key->texture_border_adjust_mask & bit) {
        *flags |= UBER_STAGE_BORDER;
    }
    *flags |= (uint32_t)(key->texture_colorkey_mode[stage] & 3)
              << UBER_STAGE_COLOR_KEY_SHIFT;

    switch (mode) {
    case PS_TEXTUREMODES_NONE:
        return UBER_TEX_SKIP;
    case PS_TEXTUREMODES_PROJECT2D:
    case PS_TEXTUREMODES_PROJECT3D:
        if (key->texture_depth_mask & bit) {
            if (mode == PS_TEXTUREMODES_PROJECT3D) {
                *flags |= UBER_STAGE_COMPARE_Z;
            }
            return (key->tune_flags & PGRAPH_D3D12_TUNE_SHADOW_LIT) ?
                       UBER_TEX_SHADOW_LIT : UBER_TEX_SHADOW;
        }
        if (texture_stage_uses_convolution(key, stage, mode)) {
            return UBER_TEX_CONVOLUTION;
        }
        if (mode == PS_TEXTUREMODES_PROJECT3D &&
            (key->texture_3d_mask & bit)) {
            return UBER_TEX_PROJECT_3D;
        }
        if (mode == PS_TEXTUREMODES_PROJECT2D &&
            (key->texture_cubemap_mask & bit)) {
            return UBER_TEX_PROJECT_CUBE;
        }
        return UBER_TEX_PROJECT;
    case PS_TEXTUREMODES_CUBEMAP:
        return UBER_TEX_CUBEMAP;
    case PS_TEXTUREMODES_PASSTHRU:
        return UBER_TEX_PASSTHRU;
    case PS_TEXTUREMODES_CLIPPLANE:
        return UBER_TEX_CLIPPLANE;
    case PS_TEXTUREMODES_BUMPENVMAP:
        return sampled ? UBER_TEX_BUMP : UBER_TEX_ZERO;
    case PS_TEXTUREMODES_BUMPENVMAP_LUM:
        return sampled ? UBER_TEX_BUMP_LUM : UBER_TEX_ZERO;
    case PS_TEXTUREMODES_DOT_ST:
        return sampled ? UBER_TEX_DOT_ST : UBER_TEX_ZERO;
    case PS_TEXTUREMODES_DOT_ZW:
    case PS_TEXTUREMODES_DOTPRODUCT:
        if (input_valid) {
            *flags |= UBER_STAGE_DOT_INPUT;
        }
        return UBER_TEX_DOT_ZERO;
    case PS_TEXTUREMODES_DOT_RFLCT_DIFF:
        return sampled ? UBER_TEX_DOT_RFLCT_DIFF : UBER_TEX_ZERO;
    case PS_TEXTUREMODES_DOT_RFLCT_SPEC:
        return sampled ? UBER_TEX_DOT_RFLCT_SPEC : UBER_TEX_ZERO;
    case PS_TEXTUREMODES_DOT_STR_3D:
        return sampled ? UBER_TEX_DOT_STR_3D : UBER_TEX_ZERO;
    case PS_TEXTUREMODES_DOT_STR_CUBE:
        return sampled ? UBER_TEX_DOT_STR_CUBE : UBER_TEX_ZERO;
    case PS_TEXTUREMODES_DPNDNT_AR:
        return sampled ? UBER_TEX_DPNDNT_AR : UBER_TEX_ZERO;
    case PS_TEXTUREMODES_DPNDNT_GB:
        return sampled ? UBER_TEX_DPNDNT_GB : UBER_TEX_ZERO;
    default:
        /* BRDF, DOT_RFLCT_SPEC_CONST and the reserved modes. */
        return UBER_TEX_ZERO;
    }
}

/* The b3 constants of a key the interpreter pixel shader draws; a pure
 * function of the canonical key. */
static void fill_uber_ps_constants(const PGRAPHD3D12PipelineKey *key,
                                   PGRAPHD3D12UberPixelConstants *c)
{
    unsigned int num_stages = MIN(key->combiner_control & 0xff, 8);
    int next_input = texture_stage_input(key, 3);

    memset(c, 0, sizeof(*c));
    for (unsigned int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        int input = texture_stage_input(key, i);
        bool input_valid = input >= 0 && input < NV2A_MAX_TEXTURES;
        bool next_valid = next_input >= 0 && next_input < NV2A_MAX_TEXTURES;
        uint32_t flags;
        uint32_t dim = UBER_DIM_2D;
        int swizzle = 0;

        c->stage[i][0] = uber_stage_op(key, i, &flags);
        c->stage[i][1] = flags;
        /* Inputs past the four stages never sample, see uber_stage_op(). */
        c->stage[i][2] = (input_valid ? (uint32_t)input : 0) |
                         texture_stage_dot_map(key, i) << 4 |
                         (uint32_t)(next_valid ? next_input :
                                    input_valid ? input : 0) << 8 |
                         texture_stage_dot_map(key, 3) << 12;
        if (key->texture_mask & (1u << i)) {
            if (key->texture_3d_mask & (1u << i)) {
                dim = UBER_DIM_3D;
            } else if (key->texture_cubemap_mask & (1u << i)) {
                dim = (key->texture_border_adjust_mask & (1u << i)) ?
                          UBER_DIM_CUBE_ARRAY : UBER_DIM_CUBE;
            }
            swizzle = MAX(uber_swizzle_index(key->texture_color_format[i]),
                          0);
        }
        c->stage[i][3] = ((key->shader_clip_mode >> (4 * i)) & 0xf) |
                         (uint32_t)swizzle << UBER_STAGE_SWIZZLE_SHIFT |
                         dim << UBER_STAGE_DIM_SHIFT;
        if (key->texture_border_adjust_mask & (1u << i)) {
            for (unsigned int j = 0; j < 3; j++) {
                c->border_size[i][j] =
                    uber_baked_float(key->texture_border_logical_size[i][j]);
                c->border_inv[i][j] =
                    uber_baked_float(key->texture_border_inv_real_size[i][j]);
            }
        }
    }
    for (unsigned int i = 0; i < num_stages; i++) {
        c->combiner[i][0] = key->rgb_inputs[i];
        c->combiner[i][1] = key->rgb_outputs[i];
        c->combiner[i][2] = key->alpha_inputs[i];
        c->combiner[i][3] = key->alpha_outputs[i];
    }
    c->final[0] = key->final_inputs_0;
    c->final[1] = key->final_inputs_1;
    c->final[2] = key->combiner_control;
    c->control[0] = (key->alpha_test_enable ? UBER_CONTROL_ALPHA_TEST : 0) |
                    (key->window_clip_exclusive ?
                         UBER_CONTROL_CLIP_EXCLUSIVE : 0) |
                    (key->depth_clipping ? UBER_CONTROL_DEPTH_CLIP : 0) |
                    (key->z_perspective ? UBER_CONTROL_Z_PERSPECTIVE : 0);
    if (key->shader_src_blend_factor_enable) {
        switch (key->shader_src_blend_factor) {
        case NV_PGRAPH_BLEND_SFACTOR_CONSTANT_COLOR:
            c->control[1] = 1;
            break;
        case NV_PGRAPH_BLEND_SFACTOR_ONE_MINUS_CONSTANT_COLOR:
            c->control[1] = 2;
            break;
        case NV_PGRAPH_BLEND_SFACTOR_CONSTANT_ALPHA:
            c->control[1] = 3;
            break;
        case NV_PGRAPH_BLEND_SFACTOR_ONE_MINUS_CONSTANT_ALPHA:
            c->control[1] = 4;
            break;
        default:
            break;
        }
    }
    c->control[2] = key->depth_format;
}

/* uberControl.w of a fallback pipeline, which culls in the shader when it
 * has no geometry shader (fill_fallback_pipeline_key()). */
static uint32_t uber_cull_bits(const PGRAPHD3D12PipelineKey *key)
{
    bool cull_front = key->cull_mode == D3D12_CULL_MODE_FRONT;

    if (key->primitive_topology_type != D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE ||
        draw_geometry_shader_needed(key) ||
        key->cull_mode == D3D12_CULL_MODE_NONE) {
        return 0;
    }
    return cull_front != key->front_counter_clockwise ?
               UBER_CULL_FRONT_FACING : UBER_CULL_BACK_FACING;
}

/*
 * Interpreter vertex shader. The vertex program is decoded on the CPU into
 * one uint4 per token (uberVsStep[]) with the token semantics of
 * d3d12_vsh_append_token(); fixed-function state goes into uberVsControl
 * and uberVsTexgen. Both follow the pixel block in b3.
 */
/* uberVsStep[].x */
#define UBER_VS_MAC_DST_SHIFT       8
#define UBER_VS_MAC_MASK_SHIFT      12
#define UBER_VS_ILU_DST_SHIFT       16
#define UBER_VS_ILU_MASK_SHIFT      20
#define UBER_VS_OUT_REG_SHIFT       24
#define UBER_VS_OUT_MASK_SHIFT      28
#define UBER_VS_OUT_NONE            0xfu
/* uberVsStep[].y bits 24 and up */
#define UBER_VS_STEP_OUT_ILU        (1u << 24)
#define UBER_VS_STEP_DEFER_OUT      (1u << 25)
#define UBER_VS_STEP_DEFER_MAC      (1u << 26)
/* Inputs in uberVsStep[].yzw, bits 0-19: mux, negate, A0-relative,
 * swizzle, register. */
#define UBER_VS_IN_NEG              (1u << 2)
#define UBER_VS_IN_RELATIVE         (1u << 3)
#define UBER_VS_IN_SWIZZLE_SHIFT    4
#define UBER_VS_IN_INDEX_SHIFT      12
#define UBER_VS_IN_BITS             0xfffffu
/* uberVsControl.x */
#define UBER_VS_NORMALIZE           (1u << 0)
#define UBER_VS_LIGHTING            (1u << 1)
#define UBER_VS_LOCAL_EYE           (1u << 2)
#define UBER_VS_FOG                 (1u << 3)
#define UBER_VS_POINT_PARAMS        (1u << 4)
#define UBER_VS_SPECULAR            (1u << 5)
#define UBER_VS_SEPARATE_SPECULAR   (1u << 6)
#define UBER_VS_IGNORE_SPEC_ALPHA   (1u << 7)
#define UBER_VS_TEXTURED            (1u << 8)
#define UBER_VS_SKIN_MIX            (1u << 9)
#define UBER_VS_SKIN_COUNT_SHIFT    12
#define UBER_VS_FOG_MODE_SHIFT      16
#define UBER_VS_FOGGEN_SHIFT        20
#define UBER_VS_MATERIAL_SHIFT      24  /* emission, ambient, diffuse, spec */
/* uberVsControl.y: light modes, then the texture matrix mask at bit 16;
 * .z: inline attributes, fetched attributes at bit 16; .w: steps. */

typedef struct PGRAPHD3D12UberVertexConstants {
    uint32_t control[4];
    uint32_t texgen[4];
    uint32_t step[NV2A_MAX_TRANSFORM_PROGRAM_LENGTH][4];
} PGRAPHD3D12UberVertexConstants;

/* All of b3 for an interpreter pipeline. */
typedef struct PGRAPHD3D12UberConstants {
    PGRAPHD3D12UberPixelConstants ps;
    PGRAPHD3D12UberVertexConstants vs;
} PGRAPHD3D12UberConstants;

QEMU_BUILD_BUG_ON(offsetof(PGRAPHD3D12UberConstants, vs) != 22 * 16);
QEMU_BUILD_BUG_ON(sizeof(PGRAPHD3D12UberVertexConstants) !=
                  (2 + NV2A_MAX_TRANSFORM_PROGRAM_LENGTH) * 16);

/* A token input as uberVsStep[] holds it, or false if the specialised
 * shader could not compile it. */
static bool uber_vs_encode_input(const uint32_t *token,
                                 VshParameterType param,
                                 VshFieldName neg_field, int reg_num,
                                 uint32_t *out)
{
    VshFieldName swz = neg_field + 1;
    uint32_t sw[4];
    uint32_t index;

    if (swz == FLD_C_SWZ_X &&
        d3d12_vsh_ilu_force_scalar[d3d12_vsh_get_field(token, FLD_ILU)]) {
        sw[0] = sw[1] = sw[2] = sw[3] = d3d12_vsh_get_field(token, swz);
    } else {
        for (unsigned int i = 0; i < 4; i++) {
            sw[i] = d3d12_vsh_get_field(token, swz + i);
        }
    }
    switch (param) {
    case PARAM_R:
        if (reg_num > 12) {
            return false;
        }
        index = reg_num;
        break;
    case PARAM_V:
        index = d3d12_vsh_get_field(token, FLD_V);
        break;
    case PARAM_C:
        index = d3d12_vsh_convert_c_register(
            d3d12_vsh_get_field(token, FLD_CONST));
        if (!d3d12_vsh_get_field(token, FLD_A0X) &&
            index >= NV2A_VERTEXSHADER_CONSTANTS) {
            return false;
        }
        break;
    default:
        /* (float4)0, without negation or swizzle. */
        *out = 0;
        return true;
    }
    *out = (uint32_t)param |
           (d3d12_vsh_get_field(token, neg_field) ? UBER_VS_IN_NEG : 0) |
           (param == PARAM_C && d3d12_vsh_get_field(token, FLD_A0X) ?
                UBER_VS_IN_RELATIVE : 0) |
           (sw[0] | sw[1] << 2 | sw[2] << 4 | sw[3] << 6)
               << UBER_VS_IN_SWIZZLE_SHIFT |
           (index & 0xffu) << UBER_VS_IN_INDEX_SHIFT;
    return true;
}

/* One token as d3d12_vsh_append_token() emits it; false if the
 * specialised shader could not compile it. */
static bool uber_vs_encode_token(const uint32_t *token, uint32_t step[4])
{
    VshMAC raw_mac = d3d12_vsh_get_field(token, FLD_MAC);
    VshMAC mac = raw_mac > MAC_ARL ? MAC_NOP : raw_mac;
    VshILU ilu = d3d12_vsh_get_field(token, FLD_ILU);
    unsigned int out_r = d3d12_vsh_get_field(token, FLD_OUT_R);
    unsigned int mac_mask = d3d12_vsh_get_field(token, FLD_OUT_MAC_MASK);
    unsigned int ilu_mask = d3d12_vsh_get_field(token, FLD_OUT_ILU_MASK);
    unsigned int ilu_dst = raw_mac != MAC_NOP ? 1 : out_r;
    unsigned int out_reg = UBER_VS_OUT_NONE;
    unsigned int out_mask = 0;
    uint32_t flags = 0;
    uint32_t in_a = 0, in_b = 0, in_c = 0;

    memset(step, 0, 4 * sizeof(step[0]));
    if (mac == MAC_NOP && ilu == ILU_NOP) {
        return true;
    }
    if (mac != MAC_NOP && ilu != ILU_NOP) {
        flags |= UBER_VS_STEP_DEFER_MAC;
        if (out_r == 1) {
            mac_mask = 0;
        }
    }
    if (mac == MAC_NOP || mac == MAC_ARL) {
        mac_mask = 0;
    }
    if (ilu == ILU_NOP) {
        ilu_mask = 0;
    }
    if ((mac_mask && out_r > 12) || (ilu_mask && ilu_dst > 12)) {
        return false;
    }
    if (d3d12_vsh_get_field(token, FLD_OUT_O_MASK) != 0 &&
        d3d12_vsh_get_field(token, FLD_OUT_ORB) != OUTPUT_C) {
        bool by_ilu = d3d12_vsh_get_field(token, FLD_OUT_MUX) == OMUX_ILU;
        unsigned int reg = d3d12_vsh_get_field(token, FLD_OUT_ADDRESS) & 0xf;
        unsigned int write_mask = d3d12_vsh_get_field(token, FLD_OUT_O_MASK);

        if ((by_ilu && ilu != ILU_NOP) || (!by_ilu && mac != MAC_NOP)) {
            if (!by_ilu && mac == MAC_ARL) {
                return false;
            }
            if (reg == 0 || (reg >= 3 && reg <= 12)) {
                out_reg = reg;
                if (reg == 5) {
                    unsigned int n = ctpop32(write_mask);

                    out_mask = (0xf0u >> n) & 0xfu;
                } else {
                    out_mask = write_mask;
                }
                if (by_ilu) {
                    flags |= UBER_VS_STEP_OUT_ILU;
                }
                if (reg == 0 &&
                    (by_ilu ? ilu_mask != 0 :
                              (ilu != ILU_NOP || mac_mask != 0))) {
                    flags |= UBER_VS_STEP_DEFER_OUT;
                }
            }
        }
    }
    if (mac != MAC_NOP) {
        if (d3d12_vsh_mac_params[mac].a &&
            !uber_vs_encode_input(token, d3d12_vsh_get_field(token, FLD_A_MUX),
                                  FLD_A_NEG,
                                  d3d12_vsh_get_field(token, FLD_A_R),
                                  &in_a)) {
            return false;
        }
        if (d3d12_vsh_mac_params[mac].b &&
            !uber_vs_encode_input(token, d3d12_vsh_get_field(token, FLD_B_MUX),
                                  FLD_B_NEG,
                                  d3d12_vsh_get_field(token, FLD_B_R),
                                  &in_b)) {
            return false;
        }
    }
    if ((mac != MAC_NOP && d3d12_vsh_mac_params[mac].c) || ilu != ILU_NOP) {
        if (!uber_vs_encode_input(token, d3d12_vsh_get_field(token, FLD_C_MUX),
                                  FLD_C_NEG,
                                  (d3d12_vsh_get_field(token, FLD_C_R_HIGH)
                                   << 2) |
                                      d3d12_vsh_get_field(token, FLD_C_R_LOW),
                                  &in_c)) {
            return false;
        }
    }
    step[0] = mac | ilu << 4 | (out_r & 0xf) << UBER_VS_MAC_DST_SHIFT |
              mac_mask << UBER_VS_MAC_MASK_SHIFT |
              (ilu_dst & 0xf) << UBER_VS_ILU_DST_SHIFT |
              ilu_mask << UBER_VS_ILU_MASK_SHIFT |
              out_reg << UBER_VS_OUT_REG_SHIFT |
              out_mask << UBER_VS_OUT_MASK_SHIFT;
    step[1] = in_a | flags;
    step[2] = in_b;
    step[3] = in_c;
    return true;
}

/* Why the interpreter vertex shader cannot replace a key's, or NULL. */
static const char *uber_vs_unsupported(const PGRAPHD3D12PipelineKey *key)
{
    if (!pipeline_key_dynamic_vertex_formats(key)) {
        return "static vertex formats";
    }
    if (key->vertex_shader_mode == 0) {
        return key->fixed_function_skinning > SKINNING_4WEIGHTS4MATRICES ?
                   "skinning mode" : NULL;
    }
    if (key->transform_program_length == 0) {
        return "empty vertex program";
    }
    for (unsigned int i = 0; i < key->transform_program_length; i++) {
        uint32_t step[4];

        if (!uber_vs_encode_token(key->transform_program_data[i], step)) {
            return "vertex program token";
        }
    }
    return NULL;
}

static uint32_t uber_vs_attribute_mask(const PGRAPHD3D12PipelineKey *key)
{
    uint32_t mask = 0;

    for (uint8_t i = 0; i < key->num_attributes; i++) {
        mask |= 1u << key->attribute_indices[i];
    }
    return mask;
}

static void fill_uber_vs_constants(const PGRAPHD3D12PipelineKey *key,
                                   PGRAPHD3D12UberVertexConstants *c)
{
    static const uint8_t skin_count[] = { 0, 2, 2, 3, 3, 4, 4 };
    static const bool skin_mix[] = { false, true, false, true, false, true,
                                     false };
    unsigned int skinning = MIN(key->fixed_function_skinning, 6);

    memset(c, 0, sizeof(*c));
    c->control[0] =
        (key->fixed_function_normalization ? UBER_VS_NORMALIZE : 0) |
        (key->fixed_function_lighting ? UBER_VS_LIGHTING : 0) |
        (key->fixed_function_local_eye ? UBER_VS_LOCAL_EYE : 0) |
        (key->fog_enable ? UBER_VS_FOG : 0) |
        (key->point_params_enable ? UBER_VS_POINT_PARAMS : 0) |
        (key->specular_enable ? UBER_VS_SPECULAR : 0) |
        (key->separate_specular ? UBER_VS_SEPARATE_SPECULAR : 0) |
        (key->ignore_specular_alpha ? UBER_VS_IGNORE_SPEC_ALPHA : 0) |
        (key->texture_mask ? UBER_VS_TEXTURED : 0) |
        (skin_mix[skinning] ? UBER_VS_SKIN_MIX : 0) |
        (uint32_t)skin_count[skinning] << UBER_VS_SKIN_COUNT_SHIFT |
        (uint32_t)(key->fog_mode & 7) << UBER_VS_FOG_MODE_SHIFT |
        (uint32_t)(key->fixed_function_foggen & 7) << UBER_VS_FOGGEN_SHIFT |
        (uint32_t)((key->fixed_function_emission_src & 3) |
                   (key->fixed_function_ambient_src & 3) << 2 |
                   (key->fixed_function_diffuse_src & 3) << 4 |
                   (key->fixed_function_specular_src & 3) << 6)
            << UBER_VS_MATERIAL_SHIFT;
    if (key->fixed_function_lighting) {
        for (unsigned int i = 0; i < NV2A_MAX_LIGHTS; i++) {
            c->control[1] |= (uint32_t)(key->fixed_function_light[i] & 3)
                             << (i * 2);
        }
    }
    c->control[1] |=
        (uint32_t)(key->fixed_function_texture_matrix_enable_mask & 0xf)
        << 16;
    c->control[2] = (key->uniform_attrs & 0xffffu) |
                    uber_vs_attribute_mask(key) << 16;
    for (unsigned int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        for (unsigned int j = 0; j < 4; j++) {
            c->texgen[i] |= (uint32_t)(key->fixed_function_texgen[i][j] & 0xf)
                            << (j * 4);
        }
    }
    if (key->vertex_shader_mode != 0) {
        unsigned int n = MIN(key->transform_program_length,
                             NV2A_MAX_TRANSFORM_PROGRAM_LENGTH);

        for (unsigned int i = 0; i < n; i++) {
            if (!uber_vs_encode_token(key->transform_program_data[i],
                                      c->step[i])) {
                memset(c->step[i], 0, sizeof(c->step[i]));
            }
        }
        c->control[3] = n;
    }
}

static void append_uber_vs_defines(GString *source)
{
    static const struct {
        const char *name;
        unsigned int value;
    } defines[] = {
#define UBER_DEFINE(name) { #name, name }
        UBER_DEFINE(UBER_VS_MAC_DST_SHIFT),
        UBER_DEFINE(UBER_VS_MAC_MASK_SHIFT),
        UBER_DEFINE(UBER_VS_ILU_DST_SHIFT),
        UBER_DEFINE(UBER_VS_ILU_MASK_SHIFT),
        UBER_DEFINE(UBER_VS_OUT_REG_SHIFT),
        UBER_DEFINE(UBER_VS_OUT_MASK_SHIFT),
        UBER_DEFINE(UBER_VS_STEP_OUT_ILU),
        UBER_DEFINE(UBER_VS_STEP_DEFER_OUT),
        UBER_DEFINE(UBER_VS_STEP_DEFER_MAC),
        UBER_DEFINE(UBER_VS_IN_NEG),
        UBER_DEFINE(UBER_VS_IN_RELATIVE),
        UBER_DEFINE(UBER_VS_IN_SWIZZLE_SHIFT),
        UBER_DEFINE(UBER_VS_IN_INDEX_SHIFT),
        UBER_DEFINE(UBER_VS_NORMALIZE),
        UBER_DEFINE(UBER_VS_LIGHTING),
        UBER_DEFINE(UBER_VS_LOCAL_EYE),
        UBER_DEFINE(UBER_VS_FOG),
        UBER_DEFINE(UBER_VS_POINT_PARAMS),
        UBER_DEFINE(UBER_VS_SPECULAR),
        UBER_DEFINE(UBER_VS_SEPARATE_SPECULAR),
        UBER_DEFINE(UBER_VS_IGNORE_SPEC_ALPHA),
        UBER_DEFINE(UBER_VS_TEXTURED),
        UBER_DEFINE(UBER_VS_SKIN_MIX),
        UBER_DEFINE(UBER_VS_SKIN_COUNT_SHIFT),
        UBER_DEFINE(UBER_VS_FOG_MODE_SHIFT),
        UBER_DEFINE(UBER_VS_FOGGEN_SHIFT),
        UBER_DEFINE(UBER_VS_MATERIAL_SHIFT),
        UBER_DEFINE(MAC_MOV), UBER_DEFINE(MAC_MUL), UBER_DEFINE(MAC_ADD),
        UBER_DEFINE(MAC_MAD), UBER_DEFINE(MAC_DP3), UBER_DEFINE(MAC_DPH),
        UBER_DEFINE(MAC_DP4), UBER_DEFINE(MAC_DST), UBER_DEFINE(MAC_MIN),
        UBER_DEFINE(MAC_MAX), UBER_DEFINE(MAC_SLT), UBER_DEFINE(MAC_SGE),
        UBER_DEFINE(MAC_ARL),
        UBER_DEFINE(ILU_MOV), UBER_DEFINE(ILU_RCP), UBER_DEFINE(ILU_RCC),
        UBER_DEFINE(ILU_RSQ), UBER_DEFINE(ILU_EXP), UBER_DEFINE(ILU_LOG),
        UBER_DEFINE(ILU_LIT),
        UBER_DEFINE(PARAM_R), UBER_DEFINE(PARAM_V), UBER_DEFINE(PARAM_C),
        UBER_DEFINE(FOG_MODE_LINEAR), UBER_DEFINE(FOG_MODE_EXP),
        UBER_DEFINE(FOG_MODE_EXP2), UBER_DEFINE(FOG_MODE_LINEAR_ABS),
        UBER_DEFINE(FOG_MODE_EXP_ABS), UBER_DEFINE(FOG_MODE_EXP2_ABS),
        UBER_DEFINE(FOGGEN_SPEC_ALPHA), UBER_DEFINE(FOGGEN_RADIAL),
        UBER_DEFINE(FOGGEN_PLANAR), UBER_DEFINE(FOGGEN_ABS_PLANAR),
        UBER_DEFINE(FOGGEN_FOG_X),
        UBER_DEFINE(MATERIAL_COLOR_SRC_MATERIAL),
        UBER_DEFINE(MATERIAL_COLOR_SRC_DIFFUSE),
        UBER_DEFINE(MATERIAL_COLOR_SRC_SPECULAR),
        UBER_DEFINE(LIGHT_OFF), UBER_DEFINE(LIGHT_INFINITE),
        UBER_DEFINE(LIGHT_SPOT),
        UBER_DEFINE(TEXGEN_EYE_LINEAR), UBER_DEFINE(TEXGEN_OBJECT_LINEAR),
        UBER_DEFINE(TEXGEN_SPHERE_MAP), UBER_DEFINE(TEXGEN_NORMAL_MAP),
        UBER_DEFINE(TEXGEN_REFLECTION_MAP),
        UBER_DEFINE(NV2A_VERTEX_ATTR_POSITION),
        UBER_DEFINE(NV2A_VERTEX_ATTR_WEIGHT),
        UBER_DEFINE(NV2A_VERTEX_ATTR_NORMAL),
        UBER_DEFINE(NV2A_VERTEX_ATTR_DIFFUSE),
        UBER_DEFINE(NV2A_VERTEX_ATTR_SPECULAR),
        UBER_DEFINE(NV2A_VERTEX_ATTR_FOG),
        UBER_DEFINE(NV2A_VERTEX_ATTR_TEXTURE0),
#undef UBER_DEFINE
    };

    for (unsigned int i = 0; i < ARRAY_SIZE(defines); i++) {
        g_string_append_printf(source, "#define %s %uu\n", defines[i].name,
                               defines[i].value);
    }
}

/* Register access by switch; no indexable temporaries. */
static void append_uber_vs_program(GString *source)
{
    GString *regs = g_string_new(NULL);

    g_string_append(source,
                    "static float4 v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, "
                    "v10, v11, v12, v13, v14, v15;\n"
                    "static float4 R0, R1, R2, R3, R4, R5, R6, R7, R8, R9, "
                    "R10, R11;\n"
                    "static float4 oPos, oD0, oD1, oFog, oPts, oT0, oT1, "
                    "oT2, oT3;\n"
                    "float uber_component(float4 v, uint i)\n"
                    "{\n"
                    "    return i == 0u ? v.x : i == 1u ? v.y : "
                    "i == 2u ? v.z : v.w;\n"
                    "}\n"
                    "float4 uber_masked(float4 old, float4 v, uint mask)\n"
                    "{\n"
                    "    return float4((mask & 8u) != 0u ? v.x : old.x,\n"
                    "                  (mask & 4u) != 0u ? v.y : old.y,\n"
                    "                  (mask & 2u) != 0u ? v.z : old.z,\n"
                    "                  (mask & 1u) != 0u ? v.w : old.w);\n"
                    "}\n"
                    "float4 uber_vs_reg(uint i)\n"
                    "{\n"
                    "    [forcecase] switch (i) {\n");
    for (unsigned int i = 0; i < 12; i++) {
        g_string_append_printf(source, "    case %uu: return R%u;\n", i, i);
    }
    g_string_append(source,
                    "    case 12u: return oPos;\n"
                    "    default: return float4(0.0, 0.0, 0.0, 0.0);\n"
                    "    }\n"
                    "}\n"
                    "float4 uber_vs_attr(uint i)\n"
                    "{\n"
                    "    [forcecase] switch (i) {\n");
    for (unsigned int i = 0; i < 16; i++) {
        g_string_append_printf(source, "    case %uu: return v%u;\n", i, i);
    }
    g_string_append(source,
                    "    default: return float4(0.0, 0.0, 0.0, 1.0);\n"
                    "    }\n"
                    "}\n"
                    "void uber_vs_write_reg(uint i, uint mask, float4 v)\n"
                    "{\n"
                    "    [forcecase] switch (i) {\n");
    for (unsigned int i = 0; i < 12; i++) {
        g_string_append_printf(source,
                               "    case %uu: R%u = uber_masked(R%u, v, mask); "
                               "break;\n",
                               i, i, i);
    }
    g_string_append(source,
                    "    case 12u: oPos = uber_masked(oPos, v, mask); break;\n"
                    "    default: break;\n"
                    "    }\n"
                    "}\n"
                    "void uber_vs_write_out(uint i, uint mask, float4 v)\n"
                    "{\n"
                    "    [forcecase] switch (i) {\n"
                    "    case 0u: oPos = uber_masked(oPos, v, mask); break;\n"
                    "    case 3u: oD0 = uber_masked(oD0, v, mask); break;\n"
                    "    case 4u: oD1 = uber_masked(oD1, v, mask); break;\n"
                    "    case 5u: oFog = uber_masked(oFog, v, mask); break;\n"
                    "    case 6u: oPts = uber_masked(oPts, v, mask); break;\n"
                    "    case 9u: oT0 = uber_masked(oT0, v, mask); break;\n"
                    "    case 10u: oT1 = uber_masked(oT1, v, mask); break;\n"
                    "    case 11u: oT2 = uber_masked(oT2, v, mask); break;\n"
                    "    case 12u: oT3 = uber_masked(oT3, v, mask); break;\n"
                    "    default: break;\n"
                    "    }\n"
                    "}\n"
                    "float4 uber_vs_input(uint e, int a0)\n"
                    "{\n"
                    "    uint mux = e & 3u;\n"
                    "    uint index = (e >> UBER_VS_IN_INDEX_SHIFT) & 0xffu;\n"
                    "    uint swz = (e >> UBER_VS_IN_SWIZZLE_SHIFT) & 0xffu;\n"
                    "    float4 v;\n"
                    "    [branch] if (mux == 0u) {\n"
                    "        return float4(0.0, 0.0, 0.0, 0.0);\n"
                    "    } else if (mux == PARAM_R) {\n"
                    "        v = uber_vs_reg(index);\n"
                    "    } else if (mux == PARAM_V) {\n"
                    "        v = uber_vs_attr(index);\n"
                    "    } else {\n"
                    "        int ci = (int)index;\n"
                    "        if ((e & UBER_VS_IN_RELATIVE) != 0u) {\n"
                    "            ci += a0;\n"
                    "        }\n"
                    "        v = c[ci];\n"
                    "    }\n"
                    "    v = float4(uber_component(v, swz & 3u),\n"
                    "               uber_component(v, (swz >> 2) & 3u),\n"
                    "               uber_component(v, (swz >> 4) & 3u),\n"
                    "               uber_component(v, (swz >> 6) & 3u));\n"
                    "    if ((e & UBER_VS_IN_NEG) != 0u) {\n"
                    "        v = -v;\n"
                    "    }\n"
                    "    return v;\n"
                    "}\n"
                    "float4 uber_vs_mac(uint op, float4 a, float4 b, "
                    "float4 c)\n"
                    "{\n"
                    "    [forcecase] switch (op) {\n"
                    "    case MAC_MOV: return vsh_mov(a);\n"
                    "    case MAC_MUL: return vsh_mul(a, b);\n"
                    "    case MAC_ADD: return vsh_add(a, c);\n"
                    "    case MAC_MAD: return vsh_mad(a, b, c);\n"
                    "    case MAC_DP3: return vsh_dp3(a, b);\n"
                    "    case MAC_DPH: return vsh_dph(a, b);\n"
                    "    case MAC_DP4: return vsh_dp4(a, b);\n"
                    "    case MAC_DST: return vsh_dst(a, b);\n"
                    "    case MAC_MIN: return vsh_min(a, b);\n"
                    "    case MAC_MAX: return vsh_max(a, b);\n"
                    "    case MAC_SLT: return vsh_slt(a, b);\n"
                    "    case MAC_SGE: return vsh_sge(a, b);\n"
                    "    default: return float4(0.0, 0.0, 0.0, 0.0);\n"
                    "    }\n"
                    "}\n"
                    "float4 uber_vs_ilu(uint op, float4 c)\n"
                    "{\n"
                    "    [forcecase] switch (op) {\n"
                    "    case ILU_MOV: return vsh_mov(c);\n"
                    "    case ILU_RCP: return vsh_rcp(c.x);\n"
                    "    case ILU_RCC: return vsh_rcc(c.x);\n"
                    "    case ILU_RSQ: return vsh_rsq(c.x);\n"
                    "    case ILU_EXP: return vsh_exp(c.x);\n"
                    "    case ILU_LOG: return vsh_log(c.x);\n"
                    "    case ILU_LIT: return vsh_lit(c);\n"
                    "    default: return float4(0.0, 0.0, 0.0, 0.0);\n"
                    "    }\n"
                    "}\n"
                    /* Token order of d3d12_vsh_append_token(): inputs are
                     * read first; with both units the MAC register and A0
                     * writes and any oPos write land after the ILU. */
                    "void uber_vs_run_program()\n"
                    "{\n"
                    "    int A0 = 0;\n"
                    "    float4 tempOpos = float4(0.0, 0.0, 0.0, 0.0);\n"
                    "    uint steps = min(uberVsControl.w, 136u);\n"
                    "    [loop] for (uint i = 0u; i < steps; i++) {\n"
                    "        uint4 s = uberVsStep[i];\n"
                    "        uint mac = s.x & 0xfu;\n"
                    "        uint ilu = (s.x >> 4) & 7u;\n"
                    "        [branch] if (mac == 0u && ilu == 0u) {\n"
                    "            continue;\n"
                    "        }\n"
                    "        uint flags = s.y;\n"
                    "        float4 a = uber_vs_input(s.y & 0xfffffu, A0);\n"
                    "        float4 b = uber_vs_input(s.z, A0);\n"
                    "        float4 cc = uber_vs_input(s.w, A0);\n"
                    "        uint macDst = (s.x >> UBER_VS_MAC_DST_SHIFT) & 0xfu;\n"
                    "        uint macMask = (s.x >> UBER_VS_MAC_MASK_SHIFT) & 0xfu;\n"
                    "        uint iluDst = (s.x >> UBER_VS_ILU_DST_SHIFT) & 0xfu;\n"
                    "        uint iluMask = (s.x >> UBER_VS_ILU_MASK_SHIFT) & 0xfu;\n"
                    "        uint outReg = (s.x >> UBER_VS_OUT_REG_SHIFT) & 0xfu;\n"
                    "        uint outMask = s.x >> UBER_VS_OUT_MASK_SHIFT;\n"
                    "        bool deferMac = (flags & UBER_VS_STEP_DEFER_MAC) != 0u;\n"
                    "        bool deferOut = (flags & UBER_VS_STEP_DEFER_OUT) != 0u;\n"
                    "        bool outIlu = (flags & UBER_VS_STEP_OUT_ILU) != 0u;\n"
                    "        float4 macResult = float4(0.0, 0.0, 0.0, 0.0);\n"
                    "        int newA0 = A0;\n"
                    "        [branch] if (mac == MAC_ARL) {\n"
                    "            newA0 = vsh_arl(a.x);\n"
                    "            if (!deferMac) {\n"
                    "                A0 = newA0;\n"
                    "            }\n"
                    "        } else if (mac != 0u) {\n"
                    "            macResult = uber_vs_mac(mac, a, b, cc);\n"
                    "            if (outMask != 0u && !outIlu) {\n"
                    "                if (deferOut) {\n"
                    "                    tempOpos = macResult;\n"
                    "                } else {\n"
                    "                    uber_vs_write_out(outReg, outMask, "
                    "macResult);\n"
                    "                }\n"
                    "            }\n"
                    "            if (macMask != 0u && !deferMac) {\n"
                    "                uber_vs_write_reg(macDst, macMask, "
                    "macResult);\n"
                    "            }\n"
                    "        }\n"
                    "        [branch] if (ilu != 0u) {\n"
                    "            float4 iluResult = uber_vs_ilu(ilu, cc);\n"
                    "            if (outMask != 0u && outIlu) {\n"
                    "                if (deferOut) {\n"
                    "                    tempOpos = iluResult;\n"
                    "                } else {\n"
                    "                    uber_vs_write_out(outReg, outMask, "
                    "iluResult);\n"
                    "                }\n"
                    "            }\n"
                    "            if (iluMask != 0u) {\n"
                    "                uber_vs_write_reg(iluDst, iluMask, "
                    "iluResult);\n"
                    "            }\n"
                    "        }\n"
                    "        if (deferMac) {\n"
                    "            if (mac == MAC_ARL) {\n"
                    "                A0 = newA0;\n"
                    "            } else if (macMask != 0u) {\n"
                    "                uber_vs_write_reg(macDst, macMask, "
                    "macResult);\n"
                    "            }\n"
                    "        }\n"
                    "        if (deferOut) {\n"
                    "            oPos = uber_masked(oPos, tempOpos, outMask);\n"
                    "        }\n"
                    "    }\n"
                    "}\n");
    g_string_free(regs, TRUE);
}

/* append_fog_output_from_distance() with the mode from b3. */
static void append_uber_vs_fog(GString *source)
{
    g_string_append(source,
                    "float uber_fog(float fogDistance)\n"
                    "{\n"
                    "    uint flags = uberVsControl.x;\n"
                    "    [branch] if ((flags & UBER_VS_FOG) == 0u) {\n"
                    "        return 1.0;\n"
                    "    }\n"
                    "    uint mode = (flags >> UBER_VS_FOG_MODE_SHIFT) & 7u;\n"
                    "    float fogFactor = 1.0;\n"
                    "    float infResult = 0.0;\n"
                    "    float nanResult = 0.0;\n"
                    "    [forcecase] switch (mode) {\n"
                    "    case FOG_MODE_LINEAR:\n"
                    "    case FOG_MODE_LINEAR_ABS:\n"
                    "        infResult = 1.0;\n"
                    "        nanResult = 1.0;\n"
                    "        fogFactor = fogParam.x + fogDistance * fogParam.y;\n"
                    "        fogFactor -= 1.0;\n"
                    "        break;\n"
                    "    case FOG_MODE_EXP:\n"
                    "        infResult = 1.0;\n"
                    "        nanResult = 1.0;\n"
                    "        fogFactor = fogParam.x + "
                    "exp2(fogDistance * fogParam.y * 16.0);\n"
                    "        fogFactor -= 1.5;\n"
                    "        break;\n"
                    "    case FOG_MODE_EXP_ABS:\n"
                    "        fogFactor = fogParam.x + "
                    "exp2(fogDistance * fogParam.y * 16.0);\n"
                    "        fogFactor -= 1.5;\n"
                    "        break;\n"
                    "    case FOG_MODE_EXP2:\n"
                    "    case FOG_MODE_EXP2_ABS:\n"
                    "        fogFactor = fogParam.x + exp2(-fogDistance * "
                    "fogDistance * fogParam.y * fogParam.y * 32.0);\n"
                    "        fogFactor -= 1.5;\n"
                    "        break;\n"
                    "    default:\n"
                    "        break;\n"
                    "    }\n"
                    "    if (mode == FOG_MODE_LINEAR_ABS || "
                    "mode == FOG_MODE_EXP_ABS ||\n"
                    "        mode == FOG_MODE_EXP2_ABS) {\n"
                    "        fogFactor = abs(fogFactor);\n"
                    "    }\n"
                    "    if (float_is_inf(fogDistance)) {\n"
                    "        return infResult;\n"
                    "    }\n"
                    "    return clamp_fog_interpolant(\n"
                    "        nan_scalar_to_value(fogFactor, nanResult));\n"
                    "}\n");
}

/* The fixed-function path of build_minimal_vertex_shader() with its
 * state from b3. */
static void append_uber_vs_fixed_function(GString *source)
{
    g_string_append(source,
                    "float3 uber_rgb_source(uint src, float3 m, float3 d, "
                    "float3 s)\n"
                    "{\n"
                    "    return src == MATERIAL_COLOR_SRC_MATERIAL ? m :\n"
                    "           src == MATERIAL_COLOR_SRC_DIFFUSE ? d : s;\n"
                    "}\n"
                    "float3 uber_light_term(uint src, float3 light, "
                    "float3 d, float3 s)\n"
                    "{\n"
                    "    return src == MATERIAL_COLOR_SRC_MATERIAL ? light :\n"
                    "           src == MATERIAL_COLOR_SRC_DIFFUSE ? d * light :\n"
                    "                                               s * light;\n"
                    "}\n"
                    "float uber_texgen(uint mode, uint comp, uint plane, "
                    "float value,\n"
                    "                  float4 position, float4 tPosition, "
                    "float3 tNormal)\n"
                    "{\n"
                    "    [forcecase] switch (mode) {\n"
                    "    case TEXGEN_EYE_LINEAR:\n"
                    "        return dot(c[plane], tPosition);\n"
                    "    case TEXGEN_OBJECT_LINEAR:\n"
                    "        return dot(c[plane], position);\n"
                    "    case TEXGEN_SPHERE_MAP:\n"
                    "        [branch] if (comp >= 2u) {\n"
                    "            return value;\n"
                    "        } else {\n"
                    "            float3 u = normalize(tPosition.xyz);\n"
                    "            float3 r = reflect(u, tNormal);\n"
                    "            float invM = 1.0 / "
                    "(2.0 * length(r + float3(0.0, 0.0, 1.0)));\n"
                    "            return uber_component(float4(r, 0.0), comp) * "
                    "invM + 0.5;\n"
                    "        }\n"
                    "    case TEXGEN_REFLECTION_MAP:\n"
                    "        [branch] if (comp >= 3u) {\n"
                    "            return value;\n"
                    "        } else {\n"
                    "            float3 u = normalize(tPosition.xyz);\n"
                    "            float3 r = reflect(u, tNormal);\n"
                    "            return uber_component(float4(r, 0.0), comp);\n"
                    "        }\n"
                    "    case TEXGEN_NORMAL_MAP:\n"
                    "        if (comp >= 3u) {\n"
                    "            return value;\n"
                    "        }\n"
                    "        return uber_component(float4(tNormal, 0.0), comp);\n"
                    "    default:\n"
                    "        return value;\n"
                    "    }\n"
                    "}\n");
}

static void append_uber_vs_ff_body(GString *source)
{
    g_string_append(source,
                    "    uint flags = uberVsControl.x;\n"
                    "    float4 position = uber_vertex_value("
                    "NV2A_VERTEX_ATTR_POSITION, vid,\n"
                    "        float4(0.0, 0.0, 0.0, 1.0));\n"
                    "    float3 normal = uber_vertex_value("
                    "NV2A_VERTEX_ATTR_NORMAL, vid,\n"
                    "        float4(0.0, 0.0, 1.0, 0.0)).xyz;\n"
                    "    uint skinCount = (flags >> UBER_VS_SKIN_COUNT_SHIFT) & "
                    "7u;\n"
                    "    float4 tPosition;\n"
                    "    float3 tNormal;\n"
                    "    [branch] if (skinCount == 0u) {\n"
                    "        tPosition = mul_vec_mat4(position,\n"
                    "                                 D3D12_NV2A_MMAT0).xyzw;\n"
                    "        tNormal = mul_vec_mat4(float4(normal, 0.0),\n"
                    "                               D3D12_NV2A_IMMAT0).xyz;\n"
                    "    } else {\n"
                    "        float4 weight = uber_vertex_value("
                    "NV2A_VERTEX_ATTR_WEIGHT, vid,\n"
                    "            float4(0.0, 0.0, 0.0, 0.0));\n"
                    "        bool mix = (flags & UBER_VS_SKIN_MIX) != 0u;\n"
                    "        float weight_n = 1.0;\n"
                    "        tPosition = (float4)0.0;\n"
                    "        tNormal = (float3)0.0;\n"
                    "        [loop] for (uint i = 0u; i < skinCount; i++) {\n"
                    "            float weight_i = uber_component(weight, i);\n"
                    "            if (mix) {\n"
                    "                if (i < skinCount - 1u) {\n"
                    "                    weight_n -= weight_i;\n"
                    "                } else {\n"
                    "                    weight_i = weight_n;\n"
                    "                }\n"
                    "            }\n"
                    "            tPosition += mul_vec_mat4(position,\n"
                    "                D3D12_NV2A_MMAT0 + i * 8u).xyzw * weight_i;\n"
                    "            tNormal += mul_vec_mat4(float4(normal, 0.0),\n"
                    "                D3D12_NV2A_IMMAT0 + i * 8u).xyz * weight_i;\n"
                    "        }\n"
                    "    }\n"
                    "    if ((flags & UBER_VS_NORMALIZE) != 0u) {\n"
                    "        tNormal = normalize(tNormal);\n"
                    "    }\n"
                    "    float4 clipInput = skinCount != 0u ? tPosition : "
                    "position;\n"
                    "    precise float4 depthPos = "
                    "fixed_function_depth_position(clipInput);\n"
                    "    precise float4 screenPos = "
                    "fixed_function_position(clipInput);\n"
                    "    precise float4 rasterDepthPos = VS_DEPTH_POS;\n"
                    "    output.depthPos = rasterDepthPos;\n"
                    "    output.position = screenPos;\n"
                    "    output.pointSize = (flags & UBER_VS_POINT_PARAMS) != 0u ?\n"
                    "        fixed_function_point_size_params(position) :\n"
                    "        fixed_function_point_size(position);\n"
                    "    float4 diffuse = uber_vertex_value("
                    "NV2A_VERTEX_ATTR_DIFFUSE, vid,\n"
                    "        (flags & UBER_VS_TEXTURED) != 0u ?\n"
                    "            float4(1.0, 1.0, 1.0, 1.0) : "
                    "float4(0.0, 0.0, 0.0, 1.0));\n"
                    "    float4 specular = uber_vertex_value("
                    "NV2A_VERTEX_ATTR_SPECULAR, vid,\n"
                    "        float4(0.0, 0.0, 0.0, 1.0));\n"
                    "    uint material = flags >> UBER_VS_MATERIAL_SHIFT;\n"
                    "    uint emissionSrc = material & 3u;\n"
                    "    uint ambientSrc = (material >> 2) & 3u;\n"
                    "    uint diffuseSrc = (material >> 4) & 3u;\n"
                    "    uint specularSrc = (material >> 6) & 3u;\n"
                    "    float material_alpha = fixedFunctionInfo.x;\n"
                    "    float specularPower = fixedFunctionInfo.y;\n"
                    "    [branch] if ((flags & UBER_VS_LIGHTING) == 0u) {\n"
                    "        output.diffuse = saturate(nan_to_one(diffuse));\n"
                    "        output.specular = saturate(nan_to_one(specular));\n"
                    "    } else {\n"
                    "        float alphaSource =\n"
                    "            diffuseSrc == MATERIAL_COLOR_SRC_MATERIAL ?\n"
                    "                material_alpha :\n"
                    "            diffuseSrc == MATERIAL_COLOR_SRC_SPECULAR ?\n"
                    "                specular.a : diffuse.a;\n"
                    "        if (ambientSrc != 3u) {\n"
                    "            output.diffuse = float4(uber_rgb_source(\n"
                    "                ambientSrc, "
                    "ltctxa[D3D12_NV2A_LTCTXA_FR_AMB].xyz,\n"
                    "                diffuse.rgb, specular.rgb), alphaSource);\n"
                    "        } else {\n"
                    "            output.diffuse = float4(0.0, 0.0, 0.0, 1.0);\n"
                    "        }\n"
                    "        output.diffuse.rgb *= "
                    "ltctxa[D3D12_NV2A_LTCTXA_CM_COL].rgb;\n"
                    "        if (emissionSrc != 3u) {\n"
                    "            output.diffuse.rgb += uber_rgb_source(\n"
                    "                emissionSrc, "
                    "ltctxa[D3D12_NV2A_LTCTXA_FR_AMB].rgb,\n"
                    "                diffuse.rgb, specular.rgb);\n"
                    "        }\n"
                    "        output.specular = float4(0.0, 0.0, 0.0, "
                    "specular.a);\n"
                    "        bool localEye = (flags & UBER_VS_LOCAL_EYE) != 0u;\n"
                    "        float3 VPeye = float3(0.0, 0.0, 0.0);\n"
                    "        if (localEye) {\n"
                    "            VPeye = normalize("
                    "c[D3D12_NV2A_EYEP].xyz / c[D3D12_NV2A_EYEP].w - "
                    "tPosition.xyz / tPosition.w);\n"
                    "        }\n"
                    "        uint lights = uberVsControl.y;\n"
                    "        [loop] for (uint i = 0u; i < 8u; i++) {\n"
                    "            uint mode = (lights >> (i * 2u)) & 3u;\n"
                    "            [branch] if (mode == LIGHT_OFF) {\n"
                    "                continue;\n"
                    "            }\n"
                    "            float attenuation;\n"
                    "            float nDotVP;\n"
                    "            float nDotHV;\n"
                    "            bool lit = true;\n"
                    "            [branch] if (mode == LIGHT_INFINITE) {\n"
                    "                attenuation = 1.0;\n"
                    "                float3 lightDirection = "
                    "normalize(lightInfiniteDirection[i].xyz);\n"
                    "                nDotVP = max(0.0, dot(tNormal, "
                    "lightDirection));\n"
                    "                if (localEye) {\n"
                    "                    nDotHV = max(0.0, dot(tNormal, "
                    "normalize(lightDirection + VPeye)));\n"
                    "                } else {\n"
                    "                    nDotHV = max(0.0, dot(tNormal, "
                    "lightInfiniteHalfVector[i].xyz));\n"
                    "                }\n"
                    "            } else {\n"
                    "                float3 tPos = tPosition.xyz / tPosition.w;\n"
                    "                float3 VP = lightLocalPosition[i].xyz - "
                    "tPos;\n"
                    "                float d = length(VP);\n"
                    "                lit = d <= light_local_range(i);\n"
                    "                VP = normalize(VP);\n"
                    "                attenuation = 1.0 / "
                    "(lightLocalAttenuation[i].x + "
                    "lightLocalAttenuation[i].y * d + "
                    "lightLocalAttenuation[i].z * d * d);\n"
                    "                float3 halfVector = normalize(VP + VPeye);\n"
                    "                nDotVP = max(0.0, dot(tNormal, VP));\n"
                    "                nDotHV = max(0.0, dot(tNormal, "
                    "halfVector));\n"
                    "                if (mode == LIGHT_SPOT) {\n"
                    "                    float4 spotDir = "
                    "light_spot_direction(i);\n"
                    "                    float invScale = 1.0 / "
                    "length(spotDir.xyz);\n"
                    "                    float cosHalfPhi = "
                    "-invScale * spotDir.w;\n"
                    "                    float cosHalfTheta = "
                    "invScale + cosHalfPhi;\n"
                    "                    float spotDirDotVP = "
                    "dot(spotDir.xyz, VP);\n"
                    "                    float rho = invScale * spotDirDotVP;\n"
                    "                    if (rho <= cosHalfPhi) {\n"
                    "                        attenuation = 0.0;\n"
                    "                    } else if (rho <= cosHalfTheta) {\n"
                    "                        attenuation *= "
                    "spotDirDotVP + spotDir.w;\n"
                    "                    }\n"
                    "                }\n"
                    "            }\n"
                    "            [branch] if (lit) {\n"
                    "                float pf = (nDotVP == 0.0 || "
                    "nDotHV == 0.0) ? 0.0 : pow(nDotHV, specularPower);\n"
                    "                float3 lightAmbient = "
                    "light_ambient_color(i) * attenuation;\n"
                    "                float3 lightDiffuse = "
                    "light_diffuse_color(i) * attenuation * nDotVP;\n"
                    "                float3 lightSpecular = "
                    "light_specular_color(i) * attenuation * pf;\n"
                    "                output.diffuse.xyz += lightAmbient;\n"
                    "                if (diffuseSrc != 3u) {\n"
                    "                    output.diffuse.xyz += uber_light_term(\n"
                    "                        diffuseSrc, lightDiffuse, "
                    "diffuse.xyz, specular.xyz);\n"
                    "                }\n"
                    "                if (specularSrc != 3u) {\n"
                    "                    output.specular.xyz += "
                    "uber_light_term(\n"
                    "                        specularSrc, lightSpecular, "
                    "diffuse.xyz, specular.xyz);\n"
                    "                }\n"
                    "            }\n"
                    "        }\n"
                    "    }\n"
                    "    [branch] if ((flags & UBER_VS_SPECULAR) == 0u) {\n"
                    "        output.specular = float4(0.0, 0.0, 0.0, 1.0);\n"
                    "    } else {\n"
                    "        if ((flags & UBER_VS_SEPARATE_SPECULAR) == 0u) {\n"
                    "            if ((flags & UBER_VS_LIGHTING) != 0u) {\n"
                    "                output.diffuse.xyz += "
                    "output.specular.xyz;\n"
                    "            }\n"
                    "            output.specular = specular;\n"
                    "        }\n"
                    "        if ((flags & UBER_VS_IGNORE_SPEC_ALPHA) != 0u) {\n"
                    "            output.specular.a = 1.0;\n"
                    "        }\n"
                    "    }\n"
                    "    output.diffuse = saturate(nan_to_one(output.diffuse));\n"
                    "    output.specular = "
                    "saturate(nan_to_one(output.specular));\n"
                    "    float fogDistance = 1.0;\n"
                    "    [forcecase] switch ((flags >> UBER_VS_FOGGEN_SHIFT) & "
                    "7u) {\n"
                    "    case FOGGEN_SPEC_ALPHA:\n"
                    "        fogDistance = clamp(specular.a, 0.0, 1.0);\n"
                    "        break;\n"
                    "    case FOGGEN_RADIAL:\n"
                    "        fogDistance = length(tPosition.xyz);\n"
                    "        break;\n"
                    "    case FOGGEN_PLANAR:\n"
                    "        fogDistance = dot(c[D3D12_NV2A_FOGPLANE].xyz, "
                    "tPosition.xyz) + c[D3D12_NV2A_FOGPLANE].w;\n"
                    "        break;\n"
                    "    case FOGGEN_ABS_PLANAR:\n"
                    "        fogDistance = abs(dot(c[D3D12_NV2A_FOGPLANE].xyz, "
                    "tPosition.xyz) + c[D3D12_NV2A_FOGPLANE].w);\n"
                    "        break;\n"
                    "    case FOGGEN_FOG_X:\n"
                    "        fogDistance = uber_vertex_value("
                    "NV2A_VERTEX_ATTR_FOG, vid,\n"
                    "            float4(1.0, 1.0, 1.0, 1.0)).x;\n"
                    "        break;\n"
                    "    default:\n"
                    "        break;\n"
                    "    }\n"
                    "    output.fog = uber_fog(fogDistance);\n");
    for (unsigned int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        g_string_append_printf(
            source,
            "    {\n"
            "        float4 texture%u = uber_vertex_value("
            "NV2A_VERTEX_ATTR_TEXTURE0 + %uu, vid,\n"
            "            float4(0.0, 0.0, 0.0, 1.0));\n"
            "        float4 oT = texture%u;\n"
            "        uint tg = uberVsTexgen[%u];\n"
            "        oT.x = uber_texgen(tg & 0xfu, 0u, %uu, texture%u.x, "
            "position, tPosition, tNormal);\n"
            "        oT.y = uber_texgen((tg >> 4) & 0xfu, 1u, %uu, texture%u.y, "
            "position, tPosition, tNormal);\n"
            "        oT.z = uber_texgen((tg >> 8) & 0xfu, 2u, %uu, texture%u.z, "
            "position, tPosition, tNormal);\n"
            "        oT.w = uber_texgen((tg >> 12) & 0xfu, 3u, %uu, "
            "texture%u.w, position, tPosition, tNormal);\n"
            "        if ((uberVsControl.y & %uu) != 0u) {\n"
            "            oT = mul_vec_mat4(oT, %uu);\n"
            "        }\n"
            "        output.tex%u = oT;\n"
            "    }\n",
            i, i, i, i,
            fixed_function_texgen_plane_base(i), i,
            fixed_function_texgen_plane_base(i) + 1, i,
            fixed_function_texgen_plane_base(i) + 2, i,
            fixed_function_texgen_plane_base(i) + 3, i,
            1u << (16 + i), fixed_function_texture_matrix_base(i), i);
    }
}

/*
 * The interpreter vertex shader of a fallback pipeline key: fixed-function
 * or vertex-program class, writing the specialised VSOutput with all four
 * texture coordinates and depthPos as the rest of the pipeline takes it.
 */
static char *build_uber_vertex_shader(const PGRAPHD3D12PipelineKey *key,
                                      bool program)
{
    GString *source = g_string_new(vertex_draw_constants_hlsl);

    g_string_append_printf(source, "// NV2A vertex interpreter, %s class\n",
                           program ? "program" : "fixed-function");
    g_string_append_printf(
        source, "#define VS_DEPTH_POS %s\n",
        rasterized_depth_pos_expr(key, !draw_geometry_shader_needed(key),
                                  "depthPos"));
    append_vertex_defines(source, program ? 1 : 0);
    append_uber_vs_defines(source);
    g_string_append(source,
                    "cbuffer UberConstants : register(b3) {\n"
                    "    uint4 uberPixelBlock[22];\n"
                    "    uint4 uberVsControl;\n"
                    "    uint4 uberVsTexgen;\n");
    g_string_append_printf(source, "    uint4 uberVsStep[%u];\n"
                           "};\n",
                           NV2A_MAX_TRANSFORM_PROGRAM_LENGTH);
    append_vertex_decompress_helpers(source);
    append_vertex_fetch_buffers(source);
    append_vertex_fetch_dynamic_helpers(source);
    g_string_append(source,
                    "struct VSOutput {\n"
                    "    float4 position : SV_Position;\n"
                    "    float4 diffuse : TEXCOORD0;\n"
                    "    float4 specular : TEXCOORD1;\n"
                    "    float fog : TEXCOORD2;\n"
                    "    float pointSize : TEXCOORD7;\n");
    for (unsigned int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        g_string_append_printf(source, "    float4 tex%u : TEXCOORD%u;\n",
                               i, i + 3);
    }
    g_string_append(source,
                    "    float4 depthPos : TEXCOORD8;\n"
                    "};\n");
    append_vertex_common_helpers(source);
    append_fixed_function_point_size(source, true,
                                     "fixed_function_point_size_params");
    append_fixed_function_point_size(source, false,
                                     "fixed_function_point_size");
    g_string_append(source,
                    "float programmable_point_size()\n"
                    "{\n"
                    "    return surfaceInfo.w > 0.0 ? surfaceInfo.w : 1.0;\n"
                    "}\n"
                    "float4 uber_vertex_value(uint i, uint vid, float4 absent)\n"
                    "{\n"
                    "    uint bit = 1u << i;\n"
                    "    [branch] if ((uberVsControl.z & bit) != 0u) {\n"
                    "        return inlineValue[i];\n"
                    "    }\n"
                    "    [branch] if (((uberVsControl.z >> 16) & bit) != 0u) {\n"
                    "        return fetch_attribute(vertexFetch[i], vid);\n"
                    "    }\n"
                    "    return absent;\n"
                    "}\n");
    append_uber_vs_fog(source);
    if (program) {
        append_programmable_vsh_helpers(source);
        append_uber_vs_program(source);
    } else {
        g_string_append(source,
                        "float uber_component(float4 v, uint i)\n"
                        "{\n"
                        "    return i == 0u ? v.x : i == 1u ? v.y : "
                        "i == 2u ? v.z : v.w;\n"
                        "}\n");
        append_uber_vs_fixed_function(source);
    }
    g_string_append(source,
                    "cbuffer DrawVertexStart : register(b2) {\n"
                    "    uint vertexStart;\n"
                    "};\n"
                    "VSOutput vs_main(uint vertexID : SV_VertexID)\n"
                    "{\n"
                    "    uint vid = vertexID + vertexStart;\n"
                    "    VSOutput output;\n");
    if (program) {
        g_string_append(source,
                        "    [fastopt] [loop] for (uint i = 0u; i < 16u; "
                        "i++) {\n"
                        "        float4 value = uber_vertex_value(i, vid,\n"
                        "            float4(0.0, 0.0, 0.0, 1.0));\n"
                        "        [forcecase] switch (i) {\n");
        for (unsigned int i = 0; i < 16; i++) {
            g_string_append_printf(source,
                                   "        case %uu: v%u = value; break;\n",
                                   i, i);
        }
        g_string_append(source,
                        "        default: break;\n"
                        "        }\n"
                        "    }\n"
                        "    R0 = R1 = R2 = R3 = R4 = R5 = R6 = R7 = R8 = R9 = "
                        "R10 = R11 =\n"
                        "        float4(0.0, 0.0, 0.0, 0.0);\n"
                        "    oPos = oD0 = oD1 = oFog = oPts = oT0 = oT1 = "
                        "oT2 = oT3 =\n"
                        "        float4(0.0, 0.0, 0.0, 1.0);\n"
                        "    uber_vs_run_program();\n"
                        "    precise float4 depthPos = "
                        "programmable_depth_position(oPos);\n"
                        "    precise float4 screenPos = "
                        "programmable_screen_position(oPos);\n"
                        "    precise float4 rasterDepthPos = VS_DEPTH_POS;\n"
                        "    output.depthPos = rasterDepthPos;\n"
                        "    output.position = screenPos;\n"
                        "    output.diffuse = saturate(nan_to_one(oD0));\n"
                        "    output.specular = saturate(nan_to_one(oD1));\n"
                        "    uint flags = uberVsControl.x;\n"
                        "    if ((flags & UBER_VS_SPECULAR) == 0u) {\n"
                        "        output.specular = float4(0.0, 0.0, 0.0, 1.0);\n"
                        "    } else if ((flags & UBER_VS_IGNORE_SPEC_ALPHA) != "
                        "0u) {\n"
                        "        output.specular.a = 1.0;\n"
                        "    }\n"
                        "    output.pointSize = (flags & UBER_VS_POINT_PARAMS) "
                        "!= 0u ?\n"
                        "        oPts.x : programmable_point_size();\n"
                        "    output.fog = uber_fog(oFog.x);\n"
                        "    output.tex0 = oT0;\n"
                        "    output.tex1 = oT1;\n"
                        "    output.tex2 = oT2;\n"
                        "    output.tex3 = oT3;\n");
    } else {
        append_uber_vs_ff_body(source);
    }
    g_string_append(source,
                    "    return output;\n"
                    "}\n");
    return g_string_free(source, FALSE);
}

/* Appends text with '$' as the stage number and '@' as the one before. */
static void append_uber_stage_text(GString *source, const char *text,
                                   unsigned int stage)
{
    for (const char *p = text; *p != '\0'; p++) {
        if (*p == '$') {
            g_string_append_printf(source, "%u", stage);
        } else if (*p == '@') {
            g_string_append_printf(source, "%u", stage - 1);
        } else {
            g_string_append_c(source, *p);
        }
    }
}

static void append_uber_ps_defines(GString *source)
{
    static const struct {
        const char *name;
        unsigned int value;
    } defines[] = {
#define UBER_DEFINE(name) { #name, name }
        UBER_DEFINE(UBER_TEX_SKIP),
        UBER_DEFINE(UBER_TEX_PROJECT),
        UBER_DEFINE(UBER_TEX_PROJECT_3D),
        UBER_DEFINE(UBER_TEX_PROJECT_CUBE),
        UBER_DEFINE(UBER_TEX_SHADOW),
        UBER_DEFINE(UBER_TEX_SHADOW_LIT),
        UBER_DEFINE(UBER_TEX_CUBEMAP),
        UBER_DEFINE(UBER_TEX_PASSTHRU),
        UBER_DEFINE(UBER_TEX_CLIPPLANE),
        UBER_DEFINE(UBER_TEX_ZERO),
        UBER_DEFINE(UBER_TEX_CONVOLUTION),
        UBER_DEFINE(UBER_TEX_BUMP),
        UBER_DEFINE(UBER_TEX_BUMP_LUM),
        UBER_DEFINE(UBER_TEX_DOT_ST),
        UBER_DEFINE(UBER_TEX_DOT_ZERO),
        UBER_DEFINE(UBER_TEX_DOT_RFLCT_DIFF),
        UBER_DEFINE(UBER_TEX_DOT_RFLCT_SPEC),
        UBER_DEFINE(UBER_TEX_DOT_STR_3D),
        UBER_DEFINE(UBER_TEX_DOT_STR_CUBE),
        UBER_DEFINE(UBER_TEX_DPNDNT_AR),
        UBER_DEFINE(UBER_TEX_DPNDNT_GB),
        UBER_DEFINE(UBER_STAGE_SAMPLED),
        UBER_DEFINE(UBER_STAGE_SNORM_INPUT),
        UBER_DEFINE(UBER_STAGE_3D),
        UBER_DEFINE(UBER_STAGE_LINEAR),
        UBER_DEFINE(UBER_STAGE_BORDER),
        UBER_DEFINE(UBER_STAGE_DOT_INPUT),
        UBER_DEFINE(UBER_STAGE_COMPARE_Z),
        UBER_DEFINE(UBER_STAGE_COLOR_KEY_SHIFT),
        UBER_DEFINE(UBER_STAGE_SWIZZLE_SHIFT),
        UBER_DEFINE(UBER_STAGE_DIM_SHIFT),
        UBER_DEFINE(UBER_DIM_CUBE),
        UBER_DEFINE(UBER_DIM_CUBE_ARRAY),
        UBER_DEFINE(UBER_DIM_3D),
        UBER_DEFINE(UBER_CONTROL_ALPHA_TEST),
        UBER_DEFINE(UBER_CONTROL_CLIP_EXCLUSIVE),
        UBER_DEFINE(UBER_CONTROL_DEPTH_CLIP),
        UBER_DEFINE(UBER_CONTROL_Z_PERSPECTIVE),
        UBER_DEFINE(UBER_CULL_FRONT_FACING),
        UBER_DEFINE(UBER_CULL_BACK_FACING),
        UBER_DEFINE(COLOR_KEY_KILL_ALPHA),
        UBER_DEFINE(COLOR_KEY_KILL_COLOR_AND_ALPHA),
        UBER_DEFINE(PS_COMBINERCOUNT_MUX_MSB),
        UBER_DEFINE(PS_COMBINERCOUNT_UNIQUE_C0),
        UBER_DEFINE(PS_COMBINERCOUNT_UNIQUE_C1),
        UBER_DEFINE(PS_COMBINEROUTPUT_AB_BLUE_TO_ALPHA),
        UBER_DEFINE(PS_COMBINEROUTPUT_CD_BLUE_TO_ALPHA),
        UBER_DEFINE(PS_COMBINEROUTPUT_AB_DOT_PRODUCT),
        UBER_DEFINE(PS_COMBINEROUTPUT_CD_DOT_PRODUCT),
        UBER_DEFINE(PS_COMBINEROUTPUT_AB_CD_MUX),
        UBER_DEFINE(PS_FINALCOMBINERSETTING_CLAMP_SUM),
        UBER_DEFINE(PS_FINALCOMBINERSETTING_COMPLEMENT_V1),
        UBER_DEFINE(PS_FINALCOMBINERSETTING_COMPLEMENT_R0),
        UBER_DEFINE(PS_CHANNEL_ALPHA),
#undef UBER_DEFINE
    };

    for (unsigned int i = 0; i < ARRAY_SIZE(defines); i++) {
        g_string_append_printf(source, "#define %s %uu\n", defines[i].name,
                               defines[i].value);
    }
}

/* The swizzle, the per-stage samplers and the combiner register helpers. */
static void append_uber_ps_helpers(GString *source, bool full)
{
    static const char sample_text[] =
        "float4 uber_sample$(uint kind, float3 c)\n"
        "{\n"
        "    uint4 s = uberStage[$];\n"
        "    uint dim = (s.w >> UBER_STAGE_DIM_SHIFT) & 0xffu;\n"
        "    bool border = (s.y & UBER_STAGE_BORDER) != 0u;\n"
        "    float4 t;\n"
        "    [branch] if (dim == UBER_DIM_3D) {\n"
        "        float3 coord3 = c;\n"
        "        if (kind == 0u) {\n"
        "            coord3 = float3(c.xy, 0.0);\n"
        "            if (border) {\n"
        "                coord3.xy = (coord3.xy * uberBorderSize[$].xy + "
        "float2(4.0, 4.0)) * uberBorderInv[$].xy;\n"
        "            }\n"
        "        } else if (border) {\n"
        "            coord3 = (coord3 * uberBorderSize[$].xyz + "
        "float3(4.0, 4.0, 4.0)) * uberBorderInv[$].xyz;\n"
        "        }\n"
        "        t = tex3d$.Sample(samp$, coord3);\n"
        "    } else if (dim == UBER_DIM_CUBE_ARRAY) {\n"
        "        t = texArray$.Sample(samp$, uber_cube_border_coord(\n"
        "            kind == 0u ? float3(c.xy, 1.0) : c, "
        "uberBorderSize[$].xy,\n"
        "            uberBorderInv[$].xy));\n"
        "    } else if (dim == UBER_DIM_CUBE) {\n"
        "        t = texCube$.Sample(samp$, kind == 0u ? float3(c.xy, 1.0) : "
        "c);\n"
        "    } else {\n"
        "        float2 coord = kind == 1u ? remapCubeTo2D(c) : c.xy;\n"
        "        if ((s.y & UBER_STAGE_LINEAR) != 0u) {\n"
        "            coord /= max(texSize[$].xy, float2(1.0, 1.0));\n"
        "        }\n"
        "        if (border) {\n"
        "            coord = (coord * uberBorderSize[$].xy + "
        "float2(4.0, 4.0)) * uberBorderInv[$].xy;\n"
        "        }\n"
        "        t = tex2d$.Sample(samp$, coord);\n"
        "    }\n"
        "    return uber_swizzle((s.w >> UBER_STAGE_SWIZZLE_SHIFT) & 0xffu, "
        "t);\n"
        "}\n";
    static const char sample_offset_text[] =
        "float4 uber_sample_offset$(float2 coord, float2 offset)\n"
        "{\n"
        "    uint4 s = uberStage[$];\n"
        "    if ((s.y & UBER_STAGE_LINEAR) != 0u) {\n"
        "        coord /= max(texSize[$].xy, float2(1.0, 1.0));\n"
        "    }\n"
        "    if ((s.y & UBER_STAGE_BORDER) != 0u) {\n"
        "        coord = (coord * uberBorderSize[$].xy + float2(4.0, 4.0)) * "
        "uberBorderInv[$].xy;\n"
        "    }\n"
        "    coord += offset;\n"
        "    float4 t = tex2d$.Sample(samp$, coord);\n"
        "    return uber_swizzle((s.w >> UBER_STAGE_SWIZZLE_SHIFT) & 0xffu, "
        "t);\n"
        "}\n";
    static const struct {
        int reg;
        const char *name;
    } registers[] = {
        { PS_REGISTER_C0, "c0" }, { PS_REGISTER_C1, "c1" },
        { PS_REGISTER_FOG, "fog" }, { PS_REGISTER_V0, "v0" },
        { PS_REGISTER_V1, "v1" }, { PS_REGISTER_T0, "t0" },
        { PS_REGISTER_T1, "t1" }, { PS_REGISTER_T2, "t2" },
        { PS_REGISTER_T3, "t3" }, { PS_REGISTER_R0, "r0" },
        { PS_REGISTER_R1, "r1" }, { PS_REGISTER_V1R0_SUM, "sum" },
        { PS_REGISTER_EF_PROD, "ef" },
    };
    static const int output_mappings[] = {
        PS_COMBINEROUTPUT_IDENTITY, PS_COMBINEROUTPUT_BIAS,
        PS_COMBINEROUTPUT_SHIFTLEFT_1, PS_COMBINEROUTPUT_SHIFTLEFT_1_BIAS,
        PS_COMBINEROUTPUT_SHIFTLEFT_2, PS_COMBINEROUTPUT_SHIFTRIGHT_1,
    };
    static const char *const write_parameters =
        "inout float4 v0, inout float4 v1,\n"
        "    inout float4 t0, inout float4 t1, inout float4 t2, "
        "inout float4 t3,\n"
        "    inout float4 r0, inout float4 r1)\n";

    g_string_append(source, "float4 uber_swizzle(uint mode, float4 t)\n"
                            "{\n"
                            "    switch (mode) {\n");
    for (unsigned int i = 1; i < ARRAY_SIZE(uber_swizzles); i++) {
        g_string_append_printf(source, "    case %uu: return %s;\n", i,
                               uber_swizzles[i]);
    }
    g_string_append(source, "    default: return t;\n"
                            "    }\n"
                            "}\n");
    append_cube_border_coord_function(
        source,
        "float3 uber_cube_border_coord(float3 texCoord, float2 borderSize,\n"
        "                              float2 borderInv)",
        "borderSize", "borderInv");
    for (unsigned int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        append_uber_stage_text(source, sample_text, i);
        if (full) {
            append_uber_stage_text(source, sample_offset_text, i);
        }
    }
    if (full) {
        g_string_append(
            source,
            "float4 uber_stage_input(uint i, float4 t0, float4 t1, float4 t2,\n"
            "                        float4 t3)\n"
            "{\n"
            "    return i == 0u ? t0 : (i == 1u ? t1 : (i == 2u ? t2 : t3));\n"
            "}\n");
    }

    /* Combiner registers: reads, input and output mappings, writes. */
    g_string_append(source,
                    "float4 uber_reg(uint reg, float4 c0, float4 c1, "
                    "float4 fog,\n"
                    "                float4 v0, float4 v1, float4 t0, "
                    "float4 t1,\n"
                    "                float4 t2, float4 t3, float4 r0, "
                    "float4 r1,\n"
                    "                float4 sum, float4 ef)\n"
                    "{\n"
                    "    switch (reg) {\n");
    for (unsigned int i = 0; i < ARRAY_SIZE(registers); i++) {
        g_string_append_printf(source, "    case %uu: return %s;\n",
                               registers[i].reg, registers[i].name);
    }
    g_string_append(source,
                    "    default: return float4(0.0, 0.0, 0.0, 0.0);\n"
                    "    }\n"
                    "}\n"
                    "#define UBER_REG(reg) uber_reg((reg) & 0xfu, c0, c1, "
                    "pFog, v0, v1, t0, t1, t2, t3, r0, r1, sum, ef)\n");
    for (unsigned int alpha = 0; alpha < 2; alpha++) {
        const char *type = alpha ? "float" : "float3";

        g_string_append_printf(source,
                               "%s uber_input_map%s(uint mod, %s x)\n"
                               "{\n"
                               "    switch (mod) {\n",
                               type, alpha ? "1" : "3", type);
        for (unsigned int mod = 0; mod < 0x100; mod += 0x20) {
            g_autofree char *expr = hlsl_input_mapping_expr("x", mod);

            g_string_append_printf(source, "    case 0x%02xu: return %s;\n",
                                   mod, expr);
        }
        g_string_append_printf(source,
                               "    default: return %s;\n"
                               "    }\n"
                               "}\n"
                               "%s uber_output_map%s(uint mapping, %s x)\n"
                               "{\n"
                               "    switch (mapping) {\n",
                               alpha ? "0.0" : "float3(0.0, 0.0, 0.0)", type,
                               alpha ? "1" : "3", type);
        for (unsigned int i = 0; i < ARRAY_SIZE(output_mappings); i++) {
            g_autofree char *expr =
                hlsl_output_map_expr("x", output_mappings[i]);

            g_string_append_printf(source, "    case 0x%02xu: return %s;\n",
                                   output_mappings[i], expr);
        }
        g_string_append(source,
                        "    default: return x;\n"
                        "    }\n"
                        "}\n");
    }
    g_string_append(source,
                    "float3 uber_rgb_input(uint v, float4 value)\n"
                    "{\n"
                    "    return uber_input_map3(v & 0xe0u,\n"
                    "        (v & PS_CHANNEL_ALPHA) != 0u ? value.aaa : "
                    "value.rgb);\n"
                    "}\n"
                    "float uber_alpha_input(uint v, float4 value)\n"
                    "{\n"
                    "    return uber_input_map1(v & 0xe0u,\n"
                    "        (v & PS_CHANNEL_ALPHA) != 0u ? value.a : "
                    "value.b);\n"
                    "}\n");
    for (unsigned int alpha = 0; alpha < 2; alpha++) {
        g_string_append_printf(source,
                               "void uber_write_%s(uint reg, %s value, %s"
                               "{\n"
                               "    switch (reg) {\n",
                               alpha ? "a" : "rgb", alpha ? "float" : "float3",
                               write_parameters);
        /* The writable registers; hlsl_register_expr() leaves the rest
         * unwritten. */
        for (unsigned int i = 3; i < 11; i++) {
            g_string_append_printf(source, "    case %uu: %s.%s = value; "
                                   "break;\n",
                                   registers[i].reg, registers[i].name,
                                   alpha ? "a" : "rgb");
        }
        g_string_append(source,
                        "    default: break;\n"
                        "    }\n"
                        "}\n");
    }
    g_string_append(source,
                    "#define UBER_WRITE_RGB(reg, value) uber_write_rgb("
                    "(reg) & 0xfu, value, v0, v1, t0, t1, t2, t3, r0, r1)\n"
                    "#define UBER_WRITE_A(reg, value) uber_write_a("
                    "(reg) & 0xfu, value, v0, v1, t0, t1, t2, t3, r0, r1)\n");
}

/* One texture stage of the interpreter's main function. */
static void append_uber_texture_stage(GString *source, unsigned int stage,
                                      bool full)
{
    static const char head_text[] =
        "    // Texture stage $\n"
        "    [branch] if (uberStage[$].x != UBER_TEX_SKIP) {\n"
        "        uint4 stage$ = uberStage[$];\n"
        "        float3 coord$ = float3(0.0, 0.0, 0.0);\n"
        "        uint kind$ = 0xffu;\n";
    static const char full_head_text[] =
        "        float4 tin$ = uber_stage_input(stage$.z & 0xfu, t0, t1, t2, "
        "t3);\n"
        "        float lum$ = 1.0;\n";
    static const char common_text[] =
        "        switch (stage$.x) {\n"
        "        case UBER_TEX_PROJECT:\n"
        "            coord$.xy = input.tex$.xy / project_w(input.tex$.w);\n"
        "            kind$ = 0u;\n"
        "            break;\n"
        "        case UBER_TEX_PROJECT_3D:\n"
        "            coord$ = input.tex$.xyz / project_w(input.tex$.w);\n"
        "            kind$ = 2u;\n"
        "            break;\n"
        "        case UBER_TEX_PROJECT_CUBE:\n"
        "        {\n"
        "            float2 project2d$ = input.tex$.xy / "
        "project_w(input.tex$.w);\n"
        "            coord$ = normalize(float3(1.0, project2d$.y, "
        "-project2d$.x));\n"
        "            kind$ = 1u;\n"
        "            break;\n"
        "        }\n"
        "        case UBER_TEX_SHADOW:\n"
        "            coord$.xy = input.tex$.xy / project_w(input.tex$.w);\n"
        "            kind$ = 0u;\n"
        "            break;\n"
        "        case UBER_TEX_SHADOW_LIT:\n"
        "            t$ = float4(1.0, 1.0, 1.0, 1.0);\n"
        "            break;\n"
        "        case UBER_TEX_CUBEMAP:\n"
        "            coord$ = input.tex$.xyz;\n"
        "            kind$ = 1u;\n"
        "            break;\n"
        "        case UBER_TEX_PASSTHRU:\n"
        "            t$ = input.tex$;\n"
        "            break;\n"
        "        case UBER_TEX_CLIPPLANE:\n"
        "            t$ = float4(0.0, 0.0, 0.0, 0.0);\n"
        "            if ((stage$.w & 1u) != 0u ? input.tex$.x >= 0.0 : "
        "input.tex$.x < 0.0) discard;\n"
        "            if ((stage$.w & 2u) != 0u ? input.tex$.y >= 0.0 : "
        "input.tex$.y < 0.0) discard;\n"
        "            if ((stage$.w & 4u) != 0u ? input.tex$.z >= 0.0 : "
        "input.tex$.z < 0.0) discard;\n"
        "            if ((stage$.w & 8u) != 0u ? input.tex$.w >= 0.0 : "
        "input.tex$.w < 0.0) discard;\n"
        "            break;\n";
    static const char zero_text[] =
        "        case UBER_TEX_ZERO:\n"
        "            t$ = float4(0.0, 0.0, 0.0, 0.0);\n"
        "            break;\n"
        "        case UBER_TEX_DOT_ZERO:\n"
        "            if ((stage$.y & UBER_STAGE_DOT_INPUT) != 0u) {\n"
        "                dot$ = dot(input.tex$.xyz, "
        "dotmap((stage$.z >> 4) & 0xfu, tin$));\n"
        "            }\n"
        "            t$ = float4(0.0, 0.0, 0.0, 0.0);\n"
        "            break;\n"
        "        case UBER_TEX_CONVOLUTION:\n"
        "        {\n"
        "            t$ = float4(0.0, 0.0, 0.0, 0.0);\n"
        "            uint t$_width = 1u;\n"
        "            uint t$_height = 1u;\n"
        "            tex2d$.GetDimensions(t$_width, t$_height);\n"
        "            float2 t$_texel = 1.0 / max(float2((float)t$_width, "
        "(float)t$_height), float2(1.0, 1.0));\n"
        "            float2 t$_base = input.tex$.xy / project_w(input.tex$.w);\n";
    static const char input_text[] =
        "        case UBER_TEX_BUMP:\n"
        "        {\n"
        "            float2 dsdt$ = (stage$.y & UBER_STAGE_SNORM_INPUT) != 0u ?\n"
        "                tin$.bg : float2(sign3(tin$.b), sign3(tin$.g));\n"
        "            dsdt$ = bumpMatrixMul($, dsdt$);\n"
        "            if ((stage$.y & UBER_STAGE_3D) != 0u) {\n"
        "                coord$ = float3(input.tex$.xy + dsdt$, input.tex$.z);\n"
        "                kind$ = 2u;\n"
        "            } else {\n"
        "                coord$.xy = input.tex$.xy + dsdt$;\n"
        "                kind$ = 0u;\n"
        "            }\n"
        "            break;\n"
        "        }\n"
        "        case UBER_TEX_BUMP_LUM:\n"
        "        {\n"
        "            float3 dsdtl$ = (stage$.y & UBER_STAGE_SNORM_INPUT) != 0u ?\n"
        "                float3(tin$.b, tin$.g, tin$.r) :\n"
        "                float3(sign3(tin$.b), sign3(tin$.g), tin$.r);\n"
        "            dsdtl$.xy = bumpMatrixMul($, dsdtl$.xy);\n"
        "            if ((stage$.y & UBER_STAGE_3D) != 0u) {\n"
        "                coord$ = float3(input.tex$.xy + dsdtl$.xy, "
        "input.tex$.z);\n"
        "                kind$ = 2u;\n"
        "            } else {\n"
        "                coord$.xy = input.tex$.xy + dsdtl$.xy;\n"
        "                kind$ = 0u;\n"
        "            }\n"
        "            lum$ = (bumpScale[$] * dsdtl$.z + bumpOffset[$]);\n"
        "            break;\n"
        "        }\n"
        "        case UBER_TEX_DOT_ST:\n"
        "            dot$ = dot(input.tex$.xyz, dotmap((stage$.z >> 4) & 0xfu, "
        "tin$));\n"
        "            coord$.xy = float2(dot@, dot$);\n"
        "            kind$ = 0u;\n"
        "            break;\n"
        "        case UBER_TEX_DPNDNT_AR:\n"
        "            coord$.xy = float2(tin$.a, tin$.r);\n"
        "            kind$ = 0u;\n"
        "            break;\n"
        "        case UBER_TEX_DPNDNT_GB:\n"
        "            coord$.xy = tin$.gb;\n"
        "            kind$ = 0u;\n"
        "            break;\n";
    static const char stage2_text[] =
        "        case UBER_TEX_DOT_RFLCT_DIFF:\n"
        "        {\n"
        "            dot2 = dot(input.tex2.xyz, dotmap((stage2.z >> 4) & 0xfu, "
        "tin2));\n"
        "            float dot2_n = dot(input.tex3.xyz, "
        "dotmap((stage2.z >> 12) & 0xfu,\n"
        "                uber_stage_input((stage2.z >> 8) & 0xfu, t0, t1, t2, "
        "t3)));\n"
        "            float3 n_2 = float3(dot1, dot2, dot2_n);\n"
        "            coord2 = n_2;\n"
        "            kind2 = 1u;\n"
        "            break;\n"
        "        }\n";
    static const char stage3_text[] =
        "        case UBER_TEX_DOT_RFLCT_SPEC:\n"
        "        {\n"
        "            dot3 = dot(input.tex3.xyz, dotmap((stage3.z >> 4) & 0xfu, "
        "tin3));\n"
        "            float3 n_3 = float3(dot1, dot2, dot3);\n"
        "            float3 e_3 = float3(input.tex1.w, input.tex2.w, "
        "input.tex3.w);\n"
        "            float n_3_len2 = max(abs(dot(n_3, n_3)), 1e-20);\n"
        "            float3 rv_3 = 2.0 * n_3 * dot(n_3, e_3) / n_3_len2 - e_3;\n"
        "            coord3 = rv_3;\n"
        "            kind3 = 1u;\n"
        "            break;\n"
        "        }\n"
        "        case UBER_TEX_DOT_STR_3D:\n"
        "            dot3 = dot(input.tex3.xyz, dotmap((stage3.z >> 4) & 0xfu, "
        "tin3));\n"
        "            if ((stage3.y & UBER_STAGE_3D) != 0u) {\n"
        "                coord3 = float3(dot1, dot2, dot3);\n"
        "                kind3 = 2u;\n"
        "            } else {\n"
        "                coord3.xy = float2(dot1, dot2);\n"
        "                kind3 = 0u;\n"
        "            }\n"
        "            break;\n"
        "        case UBER_TEX_DOT_STR_CUBE:\n"
        "        {\n"
        "            dot3 = dot(input.tex3.xyz, dotmap((stage3.z >> 4) & 0xfu, "
        "tin3));\n"
        "            float3 dotSTR3Cube = float3(dot1, dot2, dot3);\n"
        "            coord3 = dotSTR3Cube;\n"
        "            kind3 = 1u;\n"
        "            break;\n"
        "        }\n";
    static const char tail_text[] =
        "        default:\n"
        "            break;\n"
        "        }\n"
        "        if (kind$ != 0xffu) {\n"
        "            t$ = uber_sample$(kind$, coord$);\n"
        "        }\n"
        "        if (stage$.x == UBER_TEX_SHADOW) {\n"
        "            float4 t$_depth = t$;\n"
        "            float t$_max_depth = t$_depth.g > 0.0 ? 16777215.0 : "
        "(t$_depth.b > 0.0 ? 511.9375 : 65535.0);\n"
        "            float t$_sample = t$_depth.r * t$_max_depth;\n"
        "            float t$_ref = (stage$.y & UBER_STAGE_COMPARE_Z) != 0u ?\n"
        "                clamp(input.tex$.z / project_w(input.tex$.w), 0.0, "
        "t$_max_depth) : 0.0;\n"
        "            float t$_shadow = shadow_compare(alphaTest.w, t$_sample, "
        "t$_ref) ? 1.0 : 0.0;\n"
        "            t$ = float4(t$_shadow, t$_shadow, t$_shadow, "
        "t$_shadow);\n"
        "        }\n";
    static const char full_tail_text[] =
        "        if (stage$.x == UBER_TEX_BUMP_LUM) {\n"
        "            t$ *= lum$;\n"
        "        }\n";
    static const char epilogue_text[] =
        "        if ((stage$.y & UBER_STAGE_SAMPLED) != 0u) {\n"
        "            if ((uberControl.x & UBER_CONTROL_ALPHA_TEST) != 0u &&\n"
        "                (alphaTest.z & (1u << $)) != 0u && t$.a == 0.0) "
        "discard;\n"
        "            uint colorKeyMode$ = "
        "(stage$.y >> UBER_STAGE_COLOR_KEY_SHIFT) & 3u;\n"
        "            if (colorKeyMode$ != 0u &&\n"
        "                check_color_key(t$, colorKey[$], colorKeyMask[$])) {\n"
        "                if (colorKeyMode$ == COLOR_KEY_KILL_ALPHA) {\n"
        "                    t$.a = 0.0;\n"
        "                } else if (colorKeyMode$ == "
        "COLOR_KEY_KILL_COLOR_AND_ALPHA) {\n"
        "                    t$ = float4(0.0, 0.0, 0.0, 0.0);\n"
        "                } else {\n"
        "                    discard;\n"
        "                }\n"
        "            }\n"
        "        }\n"
        "    }\n";

    append_uber_stage_text(source, head_text, stage);
    if (full) {
        append_uber_stage_text(source, full_head_text, stage);
    }
    append_uber_stage_text(source, common_text, stage);
    if (full) {
        append_uber_stage_text(source, zero_text, stage);
        for (unsigned int i = 0; i < ARRAY_SIZE(convolution_kernel); i++) {
            g_string_append_printf(
                source,
                "            t%u += uber_sample_offset%u(t%u_base, "
                "t%u_texel * float2(%f, %f)) * %s;\n",
                stage, stage, stage, stage, convolution_kernel[i].x,
                convolution_kernel[i].y, convolution_kernel[i].weight);
        }
        g_string_append(source,
                        "            break;\n"
                        "        }\n");
        /* Modes with an input stage; see texture_stage_input(). */
        if (stage > 0) {
            append_uber_stage_text(source, input_text, stage);
        }
        if (stage == 2) {
            g_string_append(source, stage2_text);
        } else if (stage == 3) {
            g_string_append(source, stage3_text);
        }
    }
    append_uber_stage_text(source, tail_text, stage);
    if (full) {
        append_uber_stage_text(source, full_tail_text, stage);
    }
    append_uber_stage_text(source, epilogue_text, stage);
}

static char *build_uber_pixel_shader_class(const PGRAPHD3D12PipelineKey *key,
                                           bool full)
{
    static const char window_clip_text[] =
        "    // Window clip\n"
        "    float2 windowClipCoord = input.position.xy - float2(0.5, 0.5);\n"
        "    bool windowClipExclusive =\n"
        "        (uberControl.x & UBER_CONTROL_CLIP_EXCLUSIVE) != 0u;\n"
        "    bool windowClipContained = false;\n"
        "    for (uint windowClipIndex = 0u; windowClipIndex < 8u; "
        "windowClipIndex++) {\n"
        "        uint4 r = clipRegion[windowClipIndex];\n"
        "        bool2 below = windowClipCoord < "
        "float2((float)r.x, (float)r.y);\n"
        "        bool2 above = windowClipCoord >= "
        "float2((float)r.z, (float)r.w);\n"
        "        bool outside = any(below) || any(above);\n"
        "        if (!outside) {\n"
        "            if (windowClipExclusive) {\n"
        "                discard;\n"
        "            }\n"
        "            windowClipContained = true;\n"
        "            break;\n"
        "        }\n"
        "    }\n"
        "    if (!windowClipExclusive && !windowClipContained) {\n"
        "        discard;\n"
        "    }\n";
    static const char combiner_text[] =
        "    // Register combiners\n"
        "    {\n"
        "        uint combinerFlags = uberFinal.z >> 8;\n"
        "        uint numStages = min(uberFinal.z & 0xffu, 8u);\n"
        "        [loop] for (uint stage = 0u; stage < numStages; stage++) {\n"
        "            uint4 words = uberCombiner[stage];\n"
        "            float4 c0 = combinerConst[\n"
        "                (combinerFlags & PS_COMBINERCOUNT_UNIQUE_C0) != 0u ?\n"
        "                stage * 2u : 0u];\n"
        "            float4 c1 = combinerConst[\n"
        "                (combinerFlags & PS_COMBINERCOUNT_UNIQUE_C1) != 0u ?\n"
        "                stage * 2u + 1u : 1u];\n"
        "            float4 sum = float4((v1).rgb + (r0).rgb, 0.0);\n"
        "            float4 ef = float4(0.0, 0.0, 0.0, 0.0);\n"
        "            float3 a = uber_rgb_input(words.x >> 24, "
        "UBER_REG(words.x >> 24));\n"
        "            float3 b = uber_rgb_input(words.x >> 16, "
        "UBER_REG(words.x >> 16));\n"
        "            float3 c = uber_rgb_input(words.x >> 8, "
        "UBER_REG(words.x >> 8));\n"
        "            float3 d = uber_rgb_input(words.x, UBER_REG(words.x));\n"
        "            float aa = uber_alpha_input(words.z >> 24, "
        "UBER_REG(words.z >> 24));\n"
        "            float ba = uber_alpha_input(words.z >> 16, "
        "UBER_REG(words.z >> 16));\n"
        "            float ca = uber_alpha_input(words.z >> 8, "
        "UBER_REG(words.z >> 8));\n"
        "            float da = uber_alpha_input(words.z, "
        "UBER_REG(words.z));\n"
        "            uint rgbFlags = words.y >> 12;\n"
        "            uint alphaFlags = words.w >> 12;\n"
        "            bool muxCond = (combinerFlags & PS_COMBINERCOUNT_MUX_MSB) "
        "!= 0u ?\n"
        "                (r0.a >= 0.5) : "
        "(((uint)(r0.a * 255.0) & 1u) == 1u);\n"
        "            float4 ab;\n"
        "            float4 cd;\n"
        "            float4 mux_sum;\n"
        "            if ((rgbFlags & PS_COMBINEROUTPUT_AB_DOT_PRODUCT) != 0u) {\n"
        "                float abDot = dot(float3(a), float3(b));\n"
        "                ab.rgb = float3(abDot, abDot, abDot);\n"
        "            } else {\n"
        "                ab.rgb = (a * b);\n"
        "            }\n"
        "            if ((rgbFlags & PS_COMBINEROUTPUT_CD_DOT_PRODUCT) != 0u) {\n"
        "                float cdDot = dot(float3(c), float3(d));\n"
        "                cd.rgb = float3(cdDot, cdDot, cdDot);\n"
        "            } else {\n"
        "                cd.rgb = (c * d);\n"
        "            }\n"
        "            ab.a = (aa * ba);\n"
        "            cd.a = (ca * da);\n"
        "            mux_sum.rgb = clamp(uber_output_map3(rgbFlags & 0x38u,\n"
        "                (rgbFlags & PS_COMBINEROUTPUT_AB_CD_MUX) != 0u ?\n"
        "                (muxCond ? cd.rgb : ab.rgb) : (ab.rgb + cd.rgb)), "
        "-1.0, 1.0);\n"
        "            ab.rgb = clamp(uber_output_map3(rgbFlags & 0x38u, "
        "ab.rgb), -1.0, 1.0);\n"
        "            cd.rgb = clamp(uber_output_map3(rgbFlags & 0x38u, "
        "cd.rgb), -1.0, 1.0);\n"
        "            mux_sum.a = clamp(uber_output_map1(alphaFlags & 0x38u,\n"
        "                (alphaFlags & PS_COMBINEROUTPUT_AB_CD_MUX) != 0u ?\n"
        "                (muxCond ? cd.a : ab.a) : (ab.a + cd.a)), -1.0, "
        "1.0);\n"
        "            ab.a = clamp(uber_output_map1(alphaFlags & 0x38u, ab.a), "
        "-1.0, 1.0);\n"
        "            cd.a = clamp(uber_output_map1(alphaFlags & 0x38u, cd.a), "
        "-1.0, 1.0);\n"
        /* Every input is read before any output is written, in the order
         * of append_combiner_component(). */
        "            UBER_WRITE_RGB(words.y >> 4, ab.rgb);\n"
        "            if ((rgbFlags & PS_COMBINEROUTPUT_AB_BLUE_TO_ALPHA) != "
        "0u) {\n"
        "                UBER_WRITE_A(words.y >> 4, ab.b);\n"
        "            }\n"
        "            UBER_WRITE_RGB(words.y, cd.rgb);\n"
        "            if ((rgbFlags & PS_COMBINEROUTPUT_CD_BLUE_TO_ALPHA) != "
        "0u) {\n"
        "                UBER_WRITE_A(words.y, cd.b);\n"
        "            }\n"
        "            UBER_WRITE_RGB(words.y >> 8, mux_sum.rgb);\n"
        "            UBER_WRITE_A(words.w >> 4, ab.a);\n"
        "            UBER_WRITE_A(words.w, cd.a);\n"
        "            UBER_WRITE_A(words.w >> 8, mux_sum.a);\n"
        "        }\n"
        "    }\n"
        "    // Final combiner\n"
        "    {\n"
        "        uint final0 = uberFinal.x;\n"
        "        uint final1 = uberFinal.y;\n"
        "        float4 c0 = combinerConst[16];\n"
        "        float4 c1 = combinerConst[17];\n"
        "        float4 sum = float4(\n"
        "            ((final1 & PS_FINALCOMBINERSETTING_COMPLEMENT_V1) != 0u ?\n"
        "                (1.0 - v1) : v1).rgb +\n"
        "            ((final1 & PS_FINALCOMBINERSETTING_COMPLEMENT_R0) != 0u ?\n"
        "                (1.0 - r0) : r0).rgb, 0.0);\n"
        "        if ((final1 & PS_FINALCOMBINERSETTING_CLAMP_SUM) != 0u) {\n"
        "            sum = saturate(sum);\n"
        "        }\n"
        "        float4 ef = float4(0.0, 0.0, 0.0, 0.0);\n"
        "        float3 e = uber_rgb_input(final1 >> 24, "
        "UBER_REG(final1 >> 24));\n"
        "        float3 f = uber_rgb_input(final1 >> 16, "
        "UBER_REG(final1 >> 16));\n"
        "        ef = float4(float3(e) * float3(f), 0.0);\n"
        "        float3 a = uber_rgb_input(final0 >> 24, "
        "UBER_REG(final0 >> 24));\n"
        "        float3 b = uber_rgb_input(final0 >> 16, "
        "UBER_REG(final0 >> 16));\n"
        "        float3 c = uber_rgb_input(final0 >> 8, "
        "UBER_REG(final0 >> 8));\n"
        "        float3 d = uber_rgb_input(final0, UBER_REG(final0));\n"
        "        float g = uber_alpha_input(final1 >> 8, "
        "UBER_REG(final1 >> 8));\n"
        "        color.rgb = float3(d) + lerp(float3(c), float3(b), "
        "float3(a));\n"
        "        color.a = g;\n"
        "    }\n";
    static const char output_text[] =
        "    if ((uberControl.x & UBER_CONTROL_ALPHA_TEST) != 0u) {\n"
        "        int frag_alpha = (int)round(color.a * 255.0);\n"
        "        if (!alpha_compare(alphaTest.y, frag_alpha, "
        "(int)alphaTest.x)) {\n"
        "            discard;\n"
        "        }\n"
        "    }\n"
        "    switch (uberControl.y) {\n"
        "    case 1u: color *= blendColor; break;\n"
        "    case 2u: color *= (1.0 - blendColor); break;\n"
        "    case 3u: color *= blendColor.a; break;\n"
        "    case 4u: color *= (1.0 - blendColor.a); break;\n"
        "    default: break;\n"
        "    }\n";
    PGRAPHD3D12PipelineKey io;
    GString *source = g_string_new("");
    bool has_color_target;
    bool depth_output;
    bool fragment_zvalue;

    fill_uber_ps_interface(key, &io);
    has_color_target = io.rtv_format != DXGI_FORMAT_UNKNOWN;
    depth_output = pipeline_key_shader_depth_output(&io);
    fragment_zvalue = pipeline_key_fragment_zvalue(&io);

    g_string_append_printf(source, "// NV2A pixel interpreter, %s class\n",
                           full ? "full" : "common");
    append_uber_ps_defines(source);
    for (unsigned int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        g_string_append_printf(source,
                               "Texture2D tex2d%u : register(t%u, space0);\n"
                               "TextureCube texCube%u : "
                               "register(t%u, space1);\n"
                               "Texture2DArray texArray%u : "
                               "register(t%u, space2);\n"
                               "Texture3D tex3d%u : register(t%u, space3);\n"
                               "SamplerState samp%u : register(s%u);\n",
                               i, i, i, i, i, i, i, i, i, i);
    }
    g_string_append(source, pixel_constants_hlsl);
    g_string_append(source, uber_ps_constants_hlsl);
    append_compare_func_helper(source, "alpha_compare", "int");
    append_compare_func_helper(source, "shadow_compare", "float");
    append_texture_common_helpers(source);
    append_color_key_helper(source);
    append_ps_input_struct(source, &io);
    append_float_class_helpers(source);
    if (fragment_zvalue) {
        g_autofree char *clip = g_strdup_printf(
            "(uberControl.x & %uu) != 0u", UBER_CONTROL_DEPTH_CLIP);

        append_fragment_zvalue_function(
            source, "fragment_zvalue_w", true, false,
            pipeline_key_primitive_slope(&io), false, clip);
        append_fragment_zvalue_function(
            source, "fragment_zvalue_screen", false,
            pipeline_key_perspective_depth(&io),
            pipeline_key_primitive_slope(&io), false, clip);
        g_string_append(source,
                        "float fragment_zvalue(PSInput input)\n"
                        "{\n"
                        "    [branch] if ((uberControl.x & "
                        "UBER_CONTROL_Z_PERSPECTIVE) != 0u) {\n"
                        "        return fragment_zvalue_w(input);\n"
                        "    }\n"
                        "    return fragment_zvalue_screen(input);\n"
                        "}\n");
    }
    append_uber_ps_helpers(source, full);

    if (has_color_target || depth_output) {
        g_string_append(source, "struct PSOutput {\n");
        if (has_color_target) {
            g_string_append(source, "    float4 color : SV_Target0;\n");
        }
        if (depth_output) {
            g_string_append(source, "    float depth : SV_Depth;\n");
        }
        g_string_append(source,
                        "};\n"
                        "PSOutput ps_main(PSInput input, "
                        "bool isFrontFace : SV_IsFrontFace)\n"
                        "{\n"
                        "    PSOutput output;\n");
    } else {
        g_string_append(source,
                        "void ps_main(PSInput input, "
                        "bool isFrontFace : SV_IsFrontFace)\n"
                        "{\n");
    }
    g_string_append(source,
                    "    if ((uberControl.w & (isFrontFace ? "
                    "UBER_CULL_FRONT_FACING : UBER_CULL_BACK_FACING)) != "
                    "0u) {\n"
                    "        discard;\n"
                    "    }\n");
    if (depth_output) {
        g_string_append_printf(
            source,
            "    precise float fragmentZ = fragment_zvalue(input);\n"
            "    switch (uberControl.z) {\n"
            "    case %uu: output.depth = %s; break;\n"
            "    case %uu: output.depth = %s; break;\n"
            "    default: output.depth = %s; break;\n"
            "    }\n",
            PGRAPH_D3D12_DEPTH_FORMAT_D16,
            depth_format_output_expr(PGRAPH_D3D12_DEPTH_FORMAT_D16),
            PGRAPH_D3D12_DEPTH_FORMAT_D24,
            depth_format_output_expr(PGRAPH_D3D12_DEPTH_FORMAT_D24),
            depth_format_output_expr(PGRAPH_D3D12_DEPTH_FORMAT_F24));
    } else if (fragment_zvalue) {
        g_string_append(source, "    fragment_zvalue(input);\n");
    }
    g_string_append(source,
                    "    float4 color = saturate(input.diffuse);\n"
                    "    float4 v0 = saturate(input.diffuse);\n"
                    "    float4 v1 = saturate(input.specular);\n"
                    "    float4 pFog = float4(fogColor.rgb, "
                    "saturate(input.fog));\n"
                    "    float4 r0 = float4(0.0, 0.0, 0.0, 0.0);\n"
                    "    float4 r1 = float4(0.0, 0.0, 0.0, 0.0);\n"
                    "    float4 t0 = float4(0.0, 0.0, 0.0, 1.0);\n"
                    "    float4 t1 = float4(0.0, 0.0, 0.0, 1.0);\n"
                    "    float4 t2 = float4(0.0, 0.0, 0.0, 1.0);\n"
                    "    float4 t3 = float4(0.0, 0.0, 0.0, 1.0);\n");
    if (full) {
        g_string_append(source,
                        "    float dot0 = 0.0;\n"
                        "    float dot1 = 0.0;\n"
                        "    float dot2 = 0.0;\n"
                        "    float dot3 = 0.0;\n");
    }
    g_string_append(source, window_clip_text);
    for (unsigned int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        append_uber_texture_stage(source, i, full);
    }
    /* t0 keeps alpha 1 where stage 0 does not run. */
    g_string_append(source, "    r0.a = t0.a;\n");
    g_string_append(source, combiner_text);
    g_string_append(source, output_text);
    if (has_color_target) {
        g_string_append(source, "    output.color = color;\n");
    }
    if (has_color_target || depth_output) {
        g_string_append(source, "    return output;\n");
    }
    g_string_append(source, "}\n");

    return g_string_free(source, FALSE);
}

static char *build_uber_pixel_shader(const PGRAPHD3D12PipelineKey *key)
{
    return build_uber_pixel_shader_class(
        key, fallback_ps_class(key) == PGRAPH_D3D12_FALLBACK_PS_FULL);
}

/* The geometry shader carries depth payloads, point expansion,
 * provoking-vertex fixup, widened lines and the line and point polygon
 * modes; other draws have none. */
static bool draw_geometry_shader_needed(const PGRAPHD3D12PipelineKey *key)
{
    return key->primitive_topology_type ==
               D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT ||
           pipeline_key_gs_depth_payload(key) || !key->smooth_shading ||
           key->line_expand_width > 1 || key->polygon_mode != POLY_MODE_FILL;
}

static void append_gs_point_helpers(GString *source,
                                    const PGRAPHD3D12PipelineKey *key)
{
    g_string_append(source,
                    "GSOutput make_point_corner(GSInput input, GSInput shade, "
                    "float slope, float2 corner)\n"
                    "{\n"
                    "    float4 pos = input.position;\n"
                    "    float2 surfaceSize = max(surfaceInfo.xy, "
                    "float2(1.0, 1.0));\n"
                    /* Sizes are in guest pixels; the smallest point
                     * covers one pixel at the render scale. */
                    "    float pointSize = max(input.pointSize, "
                    "1.0 / max(surfaceInfo.z, 1.0));\n"
                    "    float2 pixelToClip = float2(2.0 / surfaceSize.x, "
                    "-2.0 / surfaceSize.y) * pos.w;\n"
                    "    GSOutput output = attach_depth_payload(\n"
                    "        copy_gs_base(input, shade), input.depthPos, "
                    "input.depthPos, input.depthPos, slope);\n"
                    "    output.position = pos;\n"
                    "    output.position.xy += corner * pointSize * 0.5 * "
                    "pixelToClip;\n");
    if (key->texture_coord_mask & (1u << 3) &&
        key->point_sprite_enable) {
        g_string_append(
            source,
            "    output.tex3 = float4((corner.x + 1.0) * 0.5, "
            "(corner.y + 1.0) * 0.5, 1.0, 1.0);\n");
    }
    g_string_append(source,
                    "    return output;\n"
                    "}\n"
                    "void emit_point(GSInput input, GSInput shade, "
                    "float slope,\n"
                    "                inout TriangleStream<GSOutput> stream)\n"
                    "{\n"
                    "    stream.Append(make_point_corner(input, shade, slope, "
                    "float2(-1.0, -1.0)));\n"
                    "    stream.Append(make_point_corner(input, shade, slope, "
                    "float2(-1.0, 1.0)));\n"
                    "    stream.Append(make_point_corner(input, shade, slope, "
                    "float2(1.0, -1.0)));\n"
                    "    stream.Append(make_point_corner(input, shade, slope, "
                    "float2(1.0, 1.0)));\n"
                    "    stream.RestartStrip();\n"
                    "}\n");
}

static void append_gs_edge_helpers(GString *source,
                                   const PGRAPHD3D12PipelineKey *key)
{
    /* The third payload vertex is the edge rotated 90 degrees about its
     * first endpoint, so the triangle depth path interpolates along it. */
    static const char edge_vertices[] =
        "    float4 p0 = a.depthPos;\n"
        "    float4 p1 = b.depthPos;\n"
        "    float2 delta = p1.xy - p0.xy;\n"
        "    float4 p2 = float4(float2(-delta.y, delta.x) + p0.xy, "
        "p0.z, p0.w);\n"
        "    GSOutput v0 = attach_depth_payload(copy_gs_base(a, shade), "
        "p0, p1, p2, slope);\n"
        "    GSOutput v1 = attach_depth_payload(copy_gs_base(b, shade), "
        "p0, p1, p2, slope);\n";

    if (key->line_expand_width <= 1) {
        g_string_append(source,
                        "void emit_edge(GSInput a, GSInput b, GSInput shade, "
                        "float slope,\n"
                        "               inout LineStream<GSOutput> stream)\n"
                        "{\n");
        g_string_append(source, edge_vertices);
        g_string_append(source,
                        "    stream.Append(v0);\n"
                        "    stream.Append(v1);\n"
                        "    stream.RestartStrip();\n"
                        "}\n");
        return;
    }

    /*
     * D3D12 has no wide lines; expand each segment into a quad in
     * guest-pixel space. The scaled viewport applies the resolution
     * scale, so it must not be embedded here.
     */
    g_string_append(source,
                    "GSOutput make_line_corner(GSOutput base, "
                    "float4 pos, float2 offsetNdc, float side)\n"
                    "{\n"
                    "    base.position = pos;\n"
                    "    base.position.xy += offsetNdc * side * pos.w;\n"
                    "    return base;\n"
                    "}\n"
                    "void emit_edge(GSInput a, GSInput b, GSInput shade, "
                    "float slope,\n"
                    "               inout TriangleStream<GSOutput> stream)\n"
                    "{\n");
    g_string_append(source, edge_vertices);
    g_string_append_printf(
        source,
                    "    float2 surfaceSize = max(surfaceInfo.xy, "
                    "float2(1.0, 1.0));\n"
                    "    float4 pos0 = a.position;\n"
                    "    float4 pos1 = b.position;\n"
                    "    float w0 = abs(pos0.w) > 1e-6 ? pos0.w : 1e-6;\n"
                    "    float w1 = abs(pos1.w) > 1e-6 ? pos1.w : 1e-6;\n"
                    "    float2 px0 = pos0.xy / w0 * "
                    "float2(0.5, -0.5) * surfaceSize;\n"
                    "    float2 px1 = pos1.xy / w1 * "
                    "float2(0.5, -0.5) * surfaceSize;\n"
                    "    float2 dirPx = px1 - px0;\n"
                    "    float lenPx = length(dirPx);\n"
                    "    dirPx = lenPx > 1e-6 ? dirPx / lenPx : "
                    "float2(1.0, 0.0);\n"
                    "    float2 normalPx = float2(-dirPx.y, dirPx.x) * "
                    "%f;\n"
                    "    float2 offsetNdc = normalPx * "
                    "float2(2.0, -2.0) / surfaceSize;\n"
                    "    stream.Append(make_line_corner(v0, pos0, "
                    "offsetNdc, 1.0));\n"
                    "    stream.Append(make_line_corner(v0, pos0, "
                    "offsetNdc, -1.0));\n"
                    "    stream.Append(make_line_corner(v1, pos1, "
                    "offsetNdc, 1.0));\n"
                    "    stream.Append(make_line_corner(v1, pos1, "
                    "offsetNdc, -1.0));\n"
                    "    stream.RestartStrip();\n"
                    "}\n",
        0.5);
}

/*
 * Quads arrive whole (line list with adjacency) and are drawn as the
 * assembled polygon: filled as triangles (1, 2, 0), (2, 3, 0) for quads and
 * (0, 1, 2), (2, 1, 3) for strip quads, or as their four edges or
 * vertices. Vertex 3 is the flat colour source; the offset slope is that of
 * the triangle containing the edge or vertex.
 */
static void append_gs_quad_main(GString *source,
                                const PGRAPHD3D12PipelineKey *key)
{
    bool strip = key->primitive_mode == PRIM_TYPE_QUAD_STRIP;
    bool wide = key->line_expand_width > 1;

    if (key->polygon_mode == POLY_MODE_FILL) {
        g_string_append(source,
                        "void emit_triangle(GSInput a, GSInput b, GSInput c, "
                        "GSInput shade,\n"
                        "                   inout TriangleStream<GSOutput> "
                        "stream)\n"
                        "{\n"
                        "    float4 p0 = a.depthPos;\n"
                        "    float4 p1 = b.depthPos;\n"
                        "    float4 p2 = c.depthPos;\n"
                        "    float slope = calc_depth_slope(p0, p1, p2);\n"
                        "    stream.Append(attach_depth_payload("
                        "copy_gs_base(a, shade), p0, p1, p2, slope));\n"
                        "    stream.Append(attach_depth_payload("
                        "copy_gs_base(b, shade), p0, p1, p2, slope));\n"
                        "    stream.Append(attach_depth_payload("
                        "copy_gs_base(c, shade), p0, p1, p2, slope));\n"
                        "    stream.RestartStrip();\n"
                        "}\n"
                        "[maxvertexcount(6)]\n"
                        "void gs_main(lineadj GSInput input[4], "
                        "inout TriangleStream<GSOutput> stream)\n"
                        "{\n");
        g_string_append(source,
                        strip ?
                        "    emit_triangle(input[0], input[1], input[2], "
                        "input[3], stream);\n"
                        "    emit_triangle(input[2], input[1], input[3], "
                        "input[3], stream);\n" :
                        "    emit_triangle(input[1], input[2], input[0], "
                        "input[3], stream);\n"
                        "    emit_triangle(input[2], input[3], input[0], "
                        "input[3], stream);\n");
        g_string_append(source, "}\n");
        return;
    }

    if (key->polygon_mode == POLY_MODE_LINE) {
        append_gs_edge_helpers(source, key);
        g_string_append_printf(source,
                               "[maxvertexcount(%u)]\n"
                               "void gs_main(lineadj GSInput input[4], "
                               "inout %s<GSOutput> stream)\n"
                               "{\n",
                               wide ? 16 : 8,
                               wide ? "TriangleStream" : "LineStream");
    } else {
        append_gs_point_helpers(source, key);
        g_string_append(source,
                        "[maxvertexcount(16)]\n"
                        "void gs_main(lineadj GSInput input[4], "
                        "inout TriangleStream<GSOutput> stream)\n"
                        "{\n");
    }
    g_string_append_printf(source,
                           "    float slope0 = calc_depth_slope("
                           "input[0].depthPos, input[1].depthPos,\n"
                           "                                    "
                           "input[2].depthPos);\n"
                           "    float slope1 = calc_depth_slope("
                           "input[%u].depthPos, input[2].depthPos,\n"
                           "                                    "
                           "input[3].depthPos);\n",
                           strip ? 1 : 0);
    if (key->polygon_mode == POLY_MODE_LINE) {
        g_string_append(source,
                        strip ?
                        "    emit_edge(input[0], input[1], input[3], slope0, "
                        "stream);\n"
                        "    emit_edge(input[1], input[3], input[3], slope1, "
                        "stream);\n"
                        "    emit_edge(input[3], input[2], input[3], slope1, "
                        "stream);\n"
                        "    emit_edge(input[2], input[0], input[3], slope0, "
                        "stream);\n" :
                        "    emit_edge(input[0], input[1], input[3], slope0, "
                        "stream);\n"
                        "    emit_edge(input[1], input[2], input[3], slope0, "
                        "stream);\n"
                        "    emit_edge(input[2], input[3], input[3], slope1, "
                        "stream);\n"
                        "    emit_edge(input[3], input[0], input[3], slope1, "
                        "stream);\n");
    } else {
        g_string_append(source,
                        "    emit_point(input[0], input[3], slope0, stream);\n"
                        "    emit_point(input[1], input[3], slope0, stream);\n"
                        "    emit_point(input[2], input[3], slope0, stream);\n"
                        "    emit_point(input[3], input[3], slope1, stream);\n");
    }
    g_string_append(source, "}\n");
}

/*
 * Triangles in line and point polygon modes: each triangle's edges or
 * vertices, all with the triangle's offset slope. The CPU expansion starts
 * every triangle at its provoking vertex, the flat colour source.
 */
static void append_gs_triangle_outline_main(GString *source,
                                            const PGRAPHD3D12PipelineKey *key)
{
    bool wide = key->line_expand_width > 1;

    if (key->polygon_mode == POLY_MODE_LINE) {
        append_gs_edge_helpers(source, key);
        g_string_append_printf(source,
                               "[maxvertexcount(%u)]\n"
                               "void gs_main(triangle GSInput input[3], "
                               "inout %s<GSOutput> stream)\n"
                               "{\n",
                               wide ? 12 : 6,
                               wide ? "TriangleStream" : "LineStream");
    } else {
        append_gs_point_helpers(source, key);
        g_string_append(source,
                        "[maxvertexcount(12)]\n"
                        "void gs_main(triangle GSInput input[3], "
                        "inout TriangleStream<GSOutput> stream)\n"
                        "{\n");
    }
    g_string_append(source,
                    "    float slope = calc_depth_slope(input[0].depthPos, "
                    "input[1].depthPos,\n"
                    "                                   "
                    "input[2].depthPos);\n");
    if (key->polygon_mode == POLY_MODE_LINE) {
        g_string_append(source,
                        "    emit_edge(input[0], input[1], input[0], slope, "
                        "stream);\n"
                        "    emit_edge(input[1], input[2], input[0], slope, "
                        "stream);\n"
                        "    emit_edge(input[2], input[0], input[0], slope, "
                        "stream);\n");
    } else {
        g_string_append(source,
                        "    emit_point(input[0], input[0], slope, stream);\n"
                        "    emit_point(input[1], input[0], slope, stream);\n"
                        "    emit_point(input[2], input[0], slope, stream);\n");
    }
    g_string_append(source, "}\n");
}

static char *build_draw_geometry_shader(const PGRAPHD3D12PipelineKey *key)
{
    GString *source;
    bool gs_depth_payload = pipeline_key_gs_depth_payload(key);
    bool quad_adjacency =
        (key->primitive_mode == PRIM_TYPE_QUADS ||
         key->primitive_mode == PRIM_TYPE_QUAD_STRIP) &&
        key->primitive_topology_type == D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    const char *line_shade_index =
        key->first_vertex_provoking ? "0u" : "1u";
    const char *triangle_shade_index = "2u";

    if (key->primitive_mode == PRIM_TYPE_POLYGON) {
        /* Polygons shade from their first vertex: the fan centre in slot 0
         * when filled, and the first endpoint of each edge in line mode. */
        line_shade_index = "0u";
        triangle_shade_index = "0u";
    } else if (key->primitive_mode == PRIM_TYPE_TRIANGLE_FAN) {
        /* Fans are expanded as (centre, i, i + 1); the provoking vertex of
         * fan triangle i is outer vertex i (first) or i + 1 (last). */
        triangle_shade_index = key->first_vertex_provoking ? "1u" : "2u";
    } else if (key->first_vertex_provoking) {
        triangle_shade_index = "0u";
    } else if (key->primitive_mode == PRIM_TYPE_TRIANGLE_STRIP) {
        triangle_shade_index = "2u - (primitiveID & 1u)";
    }

    if (!draw_geometry_shader_needed(key)) {
        return NULL;
    }

    source = g_string_new(
        "cbuffer DrawConstants : register(b0) {\n"
        "    float4 inlineValue[16];\n"
        "    float4 c[192];\n"
        "    float4 clipRange;\n"
        "    float4 surfaceInfo;\n"
        "    float4 pointParams[2];\n"
        "};\n"
        "struct GSInput {\n"
        "    float4 position : SV_Position;\n"
        "    float4 diffuse : TEXCOORD0;\n"
        "    float4 specular : TEXCOORD1;\n"
        "    float fog : TEXCOORD2;\n"
        "    float pointSize : TEXCOORD7;\n");
    for (unsigned int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        if (key->texture_coord_mask & (1u << i)) {
            g_string_append_printf(source,
                                   "    float4 tex%u : TEXCOORD%u;\n",
                                   i, i + 3);
        }
    }
    g_string_append(source,
                    "    float4 depthPos : TEXCOORD8;\n"
                    "};\n"
                    "struct GSOutput {\n"
                    "    float4 position : SV_Position;\n"
                    "    float4 diffuse : TEXCOORD0;\n"
                    "    float4 specular : TEXCOORD1;\n"
                    "    float fog : TEXCOORD2;\n"
                    /* Takes the point size's slot; see
                     * pipeline_key_primitive_slope(). */
                    "    float primitiveSlope : TEXCOORD7;\n");
    for (unsigned int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        if (key->texture_coord_mask & (1u << i)) {
            g_string_append_printf(source,
                                   "    float4 tex%u : TEXCOORD%u;\n",
                                   i, i + 3);
        }
    }
    g_string_append(source, "    float4 depthPos : TEXCOORD8;\n");
    if (gs_depth_payload) {
        g_string_append(source,
                        "    nointerpolation float4 depthPos0 : TEXCOORD9;\n"
                        "    nointerpolation float4 depthPos1 : TEXCOORD10;\n"
                        "    nointerpolation float4 depthPos2 : TEXCOORD11;\n"
                        "    nointerpolation float depthSlope : TEXCOORD12;\n");
    }
    g_string_append(source,
                    "};\n"
                    "GSOutput copy_gs_base(GSInput input, GSInput shade)\n"
                    "{\n"
                    "    GSOutput output;\n"
                    "    output.position = input.position;\n");
    if (key->smooth_shading) {
        /* Smooth D0/D1 stay per-vertex; only flat shading uses the
         * provoking vertex. */
        g_string_append(source,
                        "    output.diffuse = input.diffuse;\n"
                        "    output.specular = input.specular;\n");
    } else {
        g_string_append(source,
                        "    output.diffuse = shade.diffuse;\n"
                        "    output.specular = shade.specular;\n");
    }
    g_string_append(source,
                    "    output.fog = input.fog;\n"
                    "    output.primitiveSlope = 0.0;\n");
    for (unsigned int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        if (key->texture_coord_mask & (1u << i)) {
            g_string_append_printf(source,
                                   "    output.tex%u = input.tex%u;\n",
                                   i, i);
        }
    }
    g_string_append_printf(source, "    output.depthPos = %s;\n",
                           rasterized_depth_pos_expr(key, true,
                                                     "input.depthPos"));
    if (gs_depth_payload) {
        g_string_append(source,
                        "    output.depthPos0 = input.depthPos;\n"
                        "    output.depthPos1 = input.depthPos;\n"
                        "    output.depthPos2 = input.depthPos;\n"
                        "    output.depthSlope = 0.0;\n");
    }
    g_string_append(source,
                    "    return output;\n"
                    "}\n");

    append_float_class_helpers(source);
    g_string_append_printf(
        source,
            "float kahan_det(float2 a, float2 b)\n"
            "{\n"
            "    precise float cd = a.y * b.x;\n"
            "    precise float err = mad(-a.y, b.x, cd);\n"
            "    precise float result = mad(a.x, b.y, -cd) + err;\n"
            "    return result;\n"
            "}\n"
            "float calc_depth_slope(float4 p0, float4 p1, float4 p2)\n"
            "{\n"
            "    float2 m0 = p1.xy - p0.xy;\n"
            "    float2 m1 = p2.xy - p0.xy;\n"
            "    float2 b;\n"
            "    if (%s) {\n"
            "        b = float2(p0.w - p1.w, p0.w - p2.w) /\n"
            "            (float2(p1.w, p2.w) * p0.w);\n"
            "    } else {\n"
            "        b = float2(p1.z - p0.z, p2.z - p0.z);\n"
            "    }\n"
            "    float det = kahan_det(m0, m1);\n"
            "    float dzx = kahan_det(float2(b.x, b.y),\n"
            "                           float2(m0.y, m1.y)) / det;\n"
            "    float dzy = kahan_det(float2(b.y, b.x),\n"
            "                           float2(m1.x, m0.x)) / det;\n"
            "    float dz = max(abs(dzx), abs(dzy));\n"
            "    return (float_is_nan(dz) || float_is_inf(dz)) ? 0.0 : dz;\n"
            "}\n"
            "GSOutput attach_depth_payload(GSOutput output, float4 p0, "
            "float4 p1, float4 p2, float slope)\n"
            "{\n"
            "%s"
            "    output.primitiveSlope = slope;\n"
            "    return output;\n"
            "}\n",
        key->z_perspective ? "true" : "false",
        gs_depth_payload ? "    output.depthPos0 = p0;\n"
                           "    output.depthPos1 = p1;\n"
                           "    output.depthPos2 = p2;\n"
                           "    output.depthSlope = slope;\n" : "");

    switch (key->primitive_topology_type) {
    case D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT:
        append_gs_point_helpers(source, key);
        g_string_append(source,
                        "[maxvertexcount(4)]\n"
                        "void gs_main(point GSInput input[1], "
                        "inout TriangleStream<GSOutput> stream)\n"
                        "{\n"
                        "    emit_point(input[0], input[0], 0.0, stream);\n"
                        "}\n");
        break;
    case D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE:
        if (quad_adjacency) {
            append_gs_quad_main(source, key);
            break;
        }
        if (key->polygon_mode == POLY_MODE_POINT) {
            /* Polygon vertices arrive paired with vertex 0, the flat
             * colour source. */
            append_gs_point_helpers(source, key);
            g_string_append(source,
                            "[maxvertexcount(4)]\n"
                            "void gs_main(line GSInput input[2], "
                            "inout TriangleStream<GSOutput> stream)\n"
                            "{\n"
                            "    emit_point(input[0], input[1], 0.0, "
                            "stream);\n"
                            "}\n");
            break;
        }
        append_gs_edge_helpers(source, key);
        g_string_append_printf(source,
                               "[maxvertexcount(%u)]\n"
                               "void gs_main(line GSInput input[2], "
                               "inout %s<GSOutput> stream)\n"
                               "{\n"
                               "    emit_edge(input[0], input[1], input[%s], "
                               "0.0, stream);\n"
                               "}\n",
                               key->line_expand_width > 1 ? 4 : 2,
                               key->line_expand_width > 1 ? "TriangleStream" :
                                                            "LineStream",
                               line_shade_index);
        break;
    case D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE:
        if (key->polygon_mode != POLY_MODE_FILL) {
            append_gs_triangle_outline_main(source, key);
            break;
        }
        g_string_append_printf(
            source,
                        "[maxvertexcount(3)]\n"
                        "void gs_main(triangle GSInput input[3], "
                        "uint primitiveID : SV_PrimitiveID, "
                        "inout TriangleStream<GSOutput> stream)\n"
                        "{\n"
                        "    uint shadeIndex = %s;\n"
                        "    GSInput shade = input[shadeIndex];\n"
                        "    float4 p0 = input[0].depthPos;\n"
                        "    float4 p1 = input[1].depthPos;\n"
                        "    float4 p2 = input[2].depthPos;\n"
                        "    float slope = calc_depth_slope(p0, p1, p2);\n"
                        "    GSOutput v0 = attach_depth_payload(\n"
                        "        copy_gs_base(input[0], shade), p0, p1, p2, "
                        "slope);\n"
                        "    GSOutput v1 = attach_depth_payload(\n"
                        "        copy_gs_base(input[1], shade), p0, p1, p2, "
                        "slope);\n"
                        "    GSOutput v2 = attach_depth_payload(\n"
                        "        copy_gs_base(input[2], shade), p0, p1, p2, "
                        "slope);\n"
                        "    stream.Append(v0);\n"
                        "    stream.Append(v1);\n"
                        "    stream.Append(v2);\n"
                        "    stream.RestartStrip();\n"
                        "}\n",
            triangle_shade_index);
        break;
    default:
        g_string_free(source, TRUE);
        return NULL;
    }

    return g_string_free(source, FALSE);
}

static bool compile_sm64_shader(PGRAPHState *pg, IDxcCompiler3 *compiler,
                                const char *source, LPCWSTR entry,
                                LPCWSTR target, IDxcBlob **blob,
                                Error **errp)
{
    IDxcResult *result = NULL;
    IDxcBlob *errors = NULL;
    HRESULT status = S_OK;
    DxcBuffer source_buffer = {
        .Ptr = source,
        .Size = strlen(source),
        .Encoding = DXC_CP_UTF8,
    };
    LPCWSTR arguments[] = {
        L"-E", entry,
        L"-T", target,
        L"-HV", L"2021",
        L"-O1",
        L"-Qstrip_debug",
        L"-Qstrip_reflect",
    };
    HRESULT hr;

    assert(compiler != NULL);

    hr = compiler->lpVtbl->Compile(
        compiler, &source_buffer, arguments, ARRAY_SIZE(arguments), NULL,
        &IID_IDxcResult, (LPVOID *)&result);
    if (FAILED(hr)) {
        error_setg(errp,
                   "Direct3D 12 DXC failed to compile %ls/%ls: "
                   "HRESULT 0x%08lx",
                   entry, target, (unsigned long)hr);
        return false;
    }

    result->lpVtbl->GetOutput(result, DXC_OUT_ERRORS, &IID_IDxcBlobUtf8,
                              (void **)&errors, NULL);
    hr = result->lpVtbl->GetStatus(result, &status);
    if (FAILED(hr) || FAILED(status)) {
        const char *message = "";
        int message_len = 0;

        if (errors != NULL && errors->lpVtbl->GetBufferSize(errors) > 1) {
            message = errors->lpVtbl->GetBufferPointer(errors);
            message_len = MIN((int)errors->lpVtbl->GetBufferSize(errors),
                              4096);
        }
        error_setg(errp,
                   "Direct3D 12 DXC failed compiling %ls/%ls: status "
                   "0x%08lx, get-status HRESULT 0x%08lx: %.*s",
                   entry, target, (unsigned long)status, (unsigned long)hr,
                   message_len, message);
        D3D12_RELEASE(errors);
        D3D12_RELEASE(result);
        return false;
    }

    hr = result->lpVtbl->GetOutput(result, DXC_OUT_OBJECT, &IID_IDxcBlob,
                                   (void **)blob, NULL);
    if (FAILED(hr) || *blob == NULL) {
        error_setg(errp,
                   "Direct3D 12 DXC produced no object for %ls/%ls: "
                   "HRESULT 0x%08lx",
                   entry, target, (unsigned long)hr);
        D3D12_RELEASE(errors);
        D3D12_RELEASE(result);
        return false;
    }

    D3D12_RELEASE(errors);
    D3D12_RELEASE(result);
    (void)pg;
    return true;
}

static uint64_t fnv1a64_hash(const uint8_t *data, size_t len, uint64_t hash)
{
    for (size_t i = 0; i < len; i++) {
        hash ^= data[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

static uint64_t shader_bytecode_hash(const char *source, LPCWSTR target)
{
    uint64_t hash = fnv1a64_hash((const uint8_t *)source, strlen(source),
                                 1469598103934665603ull);

    hash = fnv1a64_hash((const uint8_t *)target,
                        wcslen(target) * sizeof(*target), hash);
    hash ^= (uint64_t)D3D12_SHADER_CACHE_VERSION << 56;
    return hash;
}

/* Set on the PSO workers; perf counters belong to the FIFO thread. */
static __thread bool pso_worker_thread;
static __thread PGRAPHD3D12PsoWorker *current_pso_worker;

/*
 * Raises a background worker to normal priority while a draw waits on its
 * work, so the emulation threads cannot starve it, along with the workers
 * whose shader compiles it waits for; each drops back when its job ends.
 * pso_mutex held.
 */
static void boost_pso_worker(PGRAPHD3D12PsoWorker *w)
{
    for (unsigned int i = 0; w != NULL && i < PGRAPH_D3D12_MAX_PSO_WORKERS;
         i++) {
        if (!w->boosted && w->priority < THREAD_PRIORITY_NORMAL &&
            w->handle != NULL &&
            SetThreadPriority(w->handle, THREAD_PRIORITY_NORMAL)) {
            w->boosted = true;
            w->priority = THREAD_PRIORITY_NORMAL;
        }
        w = qatomic_read(&w->waiting_on);
    }
}

/* Bytecode kept in memory; past this the disk cache serves repeats. */
#define SHADER_BYTECODE_MEMORY_LIMIT (32 * 1024 * 1024)

/* A compile in progress; other threads needing the shader wait for it. */
typedef struct ShaderCompileSlot {
    uint64_t hash;
    GBytes *bytes;              /* result, NULL if the compile failed */
    PGRAPHD3D12PsoWorker *owner;    /* compiling worker, if any */
    unsigned int waiters;
    bool finished;
} ShaderCompileSlot;

/* perf.cache_shaders gates every shader cache file read and write. */
static bool shader_disk_cache_enabled(PGRAPHD3D12State *r)
{
    return r->shader_cache_dir != NULL && g_config.perf.cache_shaders;
}

static void write_shader_cache_file(PGRAPHD3D12State *r, uint64_t hash,
                                    GBytes *bytes)
{
    g_autofree char *path = NULL;
    g_autofree char *tmp_path = NULL;
    gconstpointer data;
    gsize size;

    if (!shader_disk_cache_enabled(r)) {
        return;
    }

    path = g_strdup_printf("%s/%016" PRIx64 ".dxbc", r->shader_cache_dir,
                           hash);
    tmp_path = g_strdup_printf("%s.%lu.tmp", path,
                               (unsigned long)GetCurrentThreadId());
    data = g_bytes_get_data(bytes, &size);
    if (g_file_set_contents(tmp_path, data, size, NULL)) {
        if (g_rename(tmp_path, path) != 0) {
            g_unlink(tmp_path);
        }
    }
}

static bool load_dxc(PGRAPHState *pg, HMODULE *dxcompiler, HMODULE *dxil,
                     DxcCreateInstanceProc *create_instance);

/*
 * FXC (DXBC / SM 5.1) path: always available on UWP, XEMU_D3D12_FXC=1
 * elsewhere. DXIL is preferred; DXBC is the retry when DXIL fails.
 */
#define XEMU_D3DCOMPILE_OPTIMIZATION_LEVEL3 (1u << 15)

typedef HRESULT(WINAPI *PFN_XemuD3DCompile)(
    LPCVOID src_data, SIZE_T src_size, LPCSTR source_name,
    const void *defines, void *include, LPCSTR entry_point, LPCSTR target,
    UINT flags1, UINT flags2, IDxcBlob **code, IDxcBlob **errors);

static HMODULE cached_fxc_module;
static PFN_XemuD3DCompile cached_d3dcompile;

static bool pgraph_d3d12_use_fxc(void)
{
#ifdef XEMU_UWP
    return true;
#else
    static int cached = -1;

    if (cached < 0) {
        const char *env = getenv("XEMU_D3D12_FXC");

        cached = env != NULL && env[0] == '1';
    }
    return cached;
#endif
}

static const struct {
    const wchar_t *dxc_target;
    const wchar_t *fxc_hash_target;
    const char *fxc_target;
    const char *entry;
} fxc_target_map[] = {
    { L"vs_6_4", L"vs_5_1", "vs_5_1", "vs_main" },
    { L"gs_6_4", L"gs_5_1", "gs_5_1", "gs_main" },
    { L"ps_6_4", L"ps_5_1", "ps_5_1", "ps_main" },
};

static bool load_fxc(PGRAPHState *pg)
{
    if (cached_d3dcompile != NULL) {
        return true;
    }

#ifdef XEMU_UWP
    cached_fxc_module = LoadPackagedLibrary(L"d3dcompiler_47.dll", 0);
#else
    cached_fxc_module = LoadLibraryA("d3dcompiler_47.dll");
#endif
    if (cached_fxc_module == NULL) {
        pgraph_d3d12_warn_once(
            pg, PGRAPH_D3D12_DIAG_DXC_UNAVAILABLE,
            "draw skipped: d3dcompiler_47.dll could not be loaded for "
            "Shader Model 5.1 compilation; GetLastError=%lu.",
            (unsigned long)GetLastError());
        return false;
    }
    cached_d3dcompile = (PFN_XemuD3DCompile)GetProcAddress(cached_fxc_module,
                                                           "D3DCompile");
    if (cached_d3dcompile == NULL) {
        pgraph_d3d12_warn_once(
            pg, PGRAPH_D3D12_DIAG_DXC_UNAVAILABLE,
            "draw skipped: d3dcompiler_47.dll is missing D3DCompile.");
        FreeLibrary(cached_fxc_module);
        cached_fxc_module = NULL;
        return false;
    }
    return true;
}

static bool compile_fxc_shader(PGRAPHState *pg, const char *source,
                               const char *entry, const char *target,
                               IDxcBlob **blob_out, Error **errp)
{
    IDxcBlob *code = NULL;
    IDxcBlob *errors = NULL;
    HRESULT hr;

    if (!load_fxc(pg)) {
        error_setg(errp,
                   "Direct3D 12 FXC compiler is unavailable and no cached "
                   "bytecode exists for this shader");
        return false;
    }

    hr = cached_d3dcompile(source, strlen(source), NULL, NULL, NULL, entry,
                           target, XEMU_D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                           &code, &errors);
    if (FAILED(hr)) {
        error_setg(errp,
                   "Direct3D 12 FXC %s compilation failed: HRESULT 0x%08lx: "
                   "%.*s",
                   target, (unsigned long)hr,
                   errors != NULL ?
                       (int)MIN(errors->lpVtbl->GetBufferSize(errors),
                                (SIZE_T)512) : 0,
                   errors != NULL ?
                       (const char *)errors->lpVtbl->GetBufferPointer(
                           errors) : "");
        D3D12_RELEASE(errors);
        D3D12_RELEASE(code);
        return false;
    }
    D3D12_RELEASE(errors);
    *blob_out = code;
    return true;
}

/* Maps a DXC target to the FXC one when FXC compiles, which dxil overrides;
 * *target becomes the target that names the shader in the caches. */
static void resolve_shader_target(bool dxil, LPCWSTR *target,
                                  const char **fxc_target,
                                  const char **fxc_entry)
{
    *fxc_target = NULL;
    *fxc_entry = NULL;
    if (dxil || !pgraph_d3d12_use_fxc()) {
        return;
    }
    for (size_t i = 0; i < ARRAY_SIZE(fxc_target_map); i++) {
        if (wcscmp(*target, fxc_target_map[i].dxc_target) == 0) {
            *fxc_target = fxc_target_map[i].fxc_target;
            *fxc_entry = fxc_target_map[i].entry;
            *target = fxc_target_map[i].fxc_hash_target;
            return;
        }
    }
}

/* A new reference to the bytecode in memory, or NULL. */
static GBytes *lookup_memory_bytecode(PGRAPHD3D12State *r, const char *source,
                                      LPCWSTR target, bool dxil)
{
    const char *fxc_target;
    const char *fxc_entry;
    uint64_t hash;
    GBytes *bytes;

    resolve_shader_target(dxil, &target, &fxc_target, &fxc_entry);
    hash = shader_bytecode_hash(source, target);
    qemu_mutex_lock(&r->shader_bytecode_mutex);
    bytes = g_hash_table_lookup(r->shader_bytecode_cache, &hash);
    if (bytes != NULL) {
        g_bytes_ref(bytes);
    }
    qemu_mutex_unlock(&r->shader_bytecode_mutex);
    return bytes;
}

static GBytes *read_shader_cache_file(PGRAPHD3D12State *r, uint64_t hash)
{
    g_autofree char *path = NULL;
    char *data = NULL;
    gsize length = 0;

    if (!shader_disk_cache_enabled(r)) {
        return NULL;
    }
    path = g_strdup_printf("%s/%016" PRIx64 ".dxbc", r->shader_cache_dir,
                           hash);
    if (!g_file_get_contents(path, &data, &length, NULL)) {
        return NULL;
    }
    /* DXBC and DXIL both use the DXBC container. */
    if (length < 4 || memcmp(data, "DXBC", 4) != 0) {
        g_free(data);
        return NULL;
    }
    return g_bytes_new_take(data, length);
}

static void release_compile_slot(ShaderCompileSlot *slot)
{
    if (slot->bytes != NULL) {
        g_bytes_unref(slot->bytes);
    }
    g_free(slot);
}

/*
 * Compiles source with FXC when fxc_target is set, else with DXC; *compiler
 * is created lazily and released by the caller. Returns new bytes or NULL.
 */
static GBytes *compile_shader_source(PGRAPHState *pg,
                                     IDxcCompiler3 **compiler,
                                     const char *source, LPCWSTR entry,
                                     LPCWSTR target, const char *fxc_target,
                                     const char *fxc_entry, Error **errp)
{
    IDxcBlob *blob = NULL;
    GBytes *bytes;

    if (fxc_target != NULL) {
        if (!compile_fxc_shader(pg, source, fxc_entry, fxc_target, &blob,
                                errp)) {
            return NULL;
        }
    } else {
        if (*compiler == NULL) {
            HMODULE dxcompiler = NULL;
            HMODULE dxil = NULL;
            DxcCreateInstanceProc create_instance = NULL;
            HRESULT hr;

            if (!load_dxc(pg, &dxcompiler, &dxil, &create_instance)) {
                error_setg(errp,
                           "Direct3D 12 DXC compiler is unavailable and no "
                           "cached bytecode exists for this shader");
                return NULL;
            }
            hr = create_instance(&CLSID_DxcCompiler, &IID_IDxcCompiler3,
                                 (LPVOID *)compiler);
            if (FAILED(hr)) {
                error_setg(errp,
                           "Direct3D 12 DXC failed to create compiler: "
                           "HRESULT 0x%08lx",
                           (unsigned long)hr);
                return NULL;
            }
        }
        if (!compile_sm64_shader(pg, *compiler, source, entry, target, &blob,
                                 errp)) {
            return NULL;
        }
    }
    bytes = g_bytes_new(blob->lpVtbl->GetBufferPointer(blob),
                        blob->lpVtbl->GetBufferSize(blob));
    D3D12_RELEASE(blob);
    return bytes;
}

/*
 * Compiled bytecode via the in-memory cache, then the on-disk cache, then
 * the compiler. Returns a new reference; *compiler is created lazily and
 * released by the caller. *compile_us is the compile time when this call
 * ran the compiler, else 0. dxil selects DXC where FXC is the default.
 */
static bool get_shader_bytecode(PGRAPHState *pg, IDxcCompiler3 **compiler,
                                const char *source, LPCWSTR entry,
                                LPCWSTR target, bool dxil, GBytes **bytes_out,
                                int64_t *compile_us, Error **errp)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    const char *fxc_target;
    const char *fxc_entry;
    ShaderCompileSlot *slot = NULL;
    uint64_t hash;
    GBytes *bytes = NULL;
    bool ok = true;

    if (compile_us != NULL) {
        *compile_us = 0;
    }
    resolve_shader_target(dxil, &target, &fxc_target, &fxc_entry);
    hash = shader_bytecode_hash(source, target);

    /* One thread produces each shader; others wait for its result. */
    qemu_mutex_lock(&r->shader_bytecode_mutex);
    for (;;) {
        bytes = g_hash_table_lookup(r->shader_bytecode_cache, &hash);
        if (bytes != NULL) {
            g_bytes_ref(bytes);
            break;
        }
        slot = g_hash_table_lookup(r->shader_inflight, &hash);
        if (slot == NULL) {
            slot = g_new0(ShaderCompileSlot, 1);
            slot->hash = hash;
            slot->owner = current_pso_worker;
            g_hash_table_insert(r->shader_inflight, &slot->hash, slot);
            break;
        }
        slot->waiters++;
        if (slot->owner != NULL) {
            /* A draw waiting here, directly or through a boosted worker,
             * raises the compiling worker. */
            qemu_mutex_lock(&r->pso_mutex);
            if (current_pso_worker != NULL) {
                qatomic_set(&current_pso_worker->waiting_on, slot->owner);
            }
            if (current_pso_worker == NULL || current_pso_worker->boosted) {
                boost_pso_worker(slot->owner);
            }
            qemu_mutex_unlock(&r->pso_mutex);
        }
        while (!slot->finished) {
            qemu_cond_wait(&r->shader_inflight_cond,
                           &r->shader_bytecode_mutex);
        }
        if (current_pso_worker != NULL) {
            qatomic_set(&current_pso_worker->waiting_on, NULL);
        }
        if (slot->bytes != NULL) {
            bytes = g_bytes_ref(slot->bytes);
        }
        if (--slot->waiters == 0) {
            release_compile_slot(slot);
        }
        slot = NULL;
        if (bytes != NULL) {
            break;
        }
        /* It failed; compile here too, for this caller's error. */
    }
    qemu_mutex_unlock(&r->shader_bytecode_mutex);
    if (slot == NULL) {
        *bytes_out = bytes;
        return true;
    }

    bytes = read_shader_cache_file(r, hash);
    if (bytes == NULL) {
        int64_t perf_start = g_get_monotonic_time();

        bytes = compile_shader_source(pg, compiler, source, entry, target,
                                      fxc_target, fxc_entry, errp);
        ok = bytes != NULL;
        if (ok) {
            write_shader_cache_file(r, hash, bytes);
            if (compile_us != NULL) {
                *compile_us = MAX(g_get_monotonic_time() - perf_start, 1);
            }
        }
    }

    qemu_mutex_lock(&r->shader_bytecode_mutex);
    if (ok) {
        gsize size = g_bytes_get_size(bytes);

        if (r->shader_bytecode_bytes + size <=
                SHADER_BYTECODE_MEMORY_LIMIT &&
            !g_hash_table_contains(r->shader_bytecode_cache, &hash)) {
            g_hash_table_insert(r->shader_bytecode_cache,
                                g_memdup2(&hash, sizeof(hash)),
                                g_bytes_ref(bytes));
            r->shader_bytecode_bytes += size;
        }
        slot->bytes = g_bytes_ref(bytes);
    }
    slot->finished = true;
    g_hash_table_remove(r->shader_inflight, &hash);
    if (slot->waiters == 0) {
        release_compile_slot(slot);
    } else {
        qemu_cond_broadcast(&r->shader_inflight_cond);
    }
    qemu_mutex_unlock(&r->shader_bytecode_mutex);

    if (!ok) {
        return false;
    }
    *bytes_out = bytes;
    return true;
}

/* A failed load is not retried until the modules are released. */
static bool dxc_load_failed;

/* shader_bytecode_mutex held. */
static bool load_dxc_locked(PGRAPHState *pg, HMODULE *dxcompiler,
                            HMODULE *dxil,
                            DxcCreateInstanceProc *create_instance)
{
    if (cached_dxcompiler_module != NULL && cached_dxil_module != NULL &&
        cached_dxc_create_instance != NULL) {
        *dxcompiler = cached_dxcompiler_module;
        *dxil = cached_dxil_module;
        *create_instance = cached_dxc_create_instance;
        return true;
    }
    if (dxc_load_failed) {
        return false;
    }
    dxc_load_failed = true;

#ifdef XEMU_UWP
    *dxcompiler = LoadPackagedLibrary(L"dxcompiler.dll", 0);
#else
    *dxcompiler = LoadLibraryA("dxcompiler.dll");
#endif
    *dxil = NULL;
    if (*dxcompiler == NULL) {
        pgraph_d3d12_warn_once(
            pg, PGRAPH_D3D12_DIAG_DXC_UNAVAILABLE,
#ifdef XEMU_UWP
            "draw skipped: packaged dxcompiler.dll could not be loaded for "
            "Shader Model 6.4 compilation; GetLastError=%lu. Include "
            "dxcompiler.dll in the UWP package payload.",
#else
            "draw skipped: dxcompiler.dll could not be loaded for Shader "
            "Model 6.4 compilation; GetLastError=%lu. Install the DXC "
            "runtime next to xemu.exe or make it available on PATH.",
#endif
            (unsigned long)GetLastError());
        return false;
    }

    *create_instance = (DxcCreateInstanceProc)GetProcAddress(
        *dxcompiler, "DxcCreateInstance");
    if (*create_instance == NULL) {
        pgraph_d3d12_warn_once(
            pg, PGRAPH_D3D12_DIAG_DXC_UNAVAILABLE,
            "draw skipped: dxcompiler.dll is missing DxcCreateInstance; "
            "GetLastError=%lu.",
            (unsigned long)GetLastError());
        FreeLibrary(*dxcompiler);
        *dxcompiler = NULL;
        return false;
    }

#ifdef XEMU_UWP
    *dxil = LoadPackagedLibrary(L"dxil.dll", 0);
#else
    *dxil = LoadLibraryA("dxil.dll");
#endif
    if (*dxil == NULL) {
        pgraph_d3d12_warn_once(
            pg, PGRAPH_D3D12_DIAG_DXC_UNAVAILABLE,
#ifdef XEMU_UWP
            "draw skipped: packaged dxil.dll could not be loaded for Shader "
            "Model 6.4 DXIL validation/signing; GetLastError=%lu. Include "
            "dxil.dll in the UWP package payload with dxcompiler.dll.",
#else
            "draw skipped: dxil.dll could not be loaded for Shader Model "
            "6.4 DXIL validation/signing; GetLastError=%lu. Copy dxil.dll "
            "next to xemu.exe with dxcompiler.dll, or make both available on "
            "PATH.",
#endif
            (unsigned long)GetLastError());
        FreeLibrary(*dxcompiler);
        *dxcompiler = NULL;
        return false;
    }

    /* dxil.dll stays loaded: DXC signs its output through it by name. */
    cached_dxcompiler_module = *dxcompiler;
    cached_dxil_module = *dxil;
    cached_dxc_create_instance = *create_instance;
    dxc_load_failed = false;
    return true;
}

static bool load_dxc(PGRAPHState *pg, HMODULE *dxcompiler, HMODULE *dxil,
                     DxcCreateInstanceProc *create_instance)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    bool ok;

    /* Stage helpers and pipeline workers may need it first together. */
    qemu_mutex_lock(&r->shader_bytecode_mutex);
    ok = load_dxc_locked(pg, dxcompiler, dxil, create_instance);
    qemu_mutex_unlock(&r->shader_bytecode_mutex);
    return ok;
}

/* Whether DXC can compile, loading it on first use. */
static bool dxc_ready(PGRAPHState *pg)
{
    HMODULE dxcompiler;
    HMODULE dxil;
    DxcCreateInstanceProc create_instance;

    return load_dxc(pg, &dxcompiler, &dxil, &create_instance);
}

static void release_dxc_modules(void)
{
    if (cached_dxil_module != NULL) {
        FreeLibrary(cached_dxil_module);
        cached_dxil_module = NULL;
    }
    if (cached_dxcompiler_module != NULL) {
        FreeLibrary(cached_dxcompiler_module);
        cached_dxcompiler_module = NULL;
    }
    cached_dxc_create_instance = NULL;
    dxc_load_failed = false;
}

static bool pipeline_key_has_attribute(
    const PGRAPHD3D12PipelineKey *key, uint8_t attr)
{
    for (uint8_t i = 0; i < key->num_attributes; i++) {
        if (key->attribute_indices[i] == attr) {
            return true;
        }
    }

    return false;
}

static bool pipeline_key_has_vertex_value(
    const PGRAPHD3D12PipelineKey *key, uint8_t attr)
{
    return (key->uniform_attrs & (1u << attr)) ||
           pipeline_key_has_attribute(key, attr);
}

static void append_vertex_value_expr(GString *source,
                                     const PGRAPHD3D12PipelineKey *key,
                                     uint8_t attr)
{
    if (key->uniform_attrs & (1u << attr)) {
        g_string_append_printf(source, "inlineValue[%u]", attr);
    } else if (pipeline_key_dynamic_vertex_formats(key)) {
        /* Already decoded and swizzled by fetch_attribute(). */
        g_string_append_printf(source, "input.a%u", attr);
    } else if (key->compressed_attrs & (1u << attr)) {
        g_string_append_printf(source, "decompress_11_11_10(input.a%u_cmp)",
                               attr);
    } else if (dxgi_format_is_signed_integer(
                   pipeline_key_attribute_format(key, attr))) {
        g_string_append_printf(source, "(float4)input.a%u_i", attr);
    } else if (key->swizzle_attrs & (1u << attr)) {
        g_string_append_printf(source, "input.a%u.bgra", attr);
    } else {
        g_string_append_printf(source, "input.a%u", attr);
    }
}

static bool d3d12_fog_mode_is_abs(uint8_t fog_mode)
{
    switch (fog_mode) {
    case FOG_MODE_LINEAR_ABS:
    case FOG_MODE_EXP_ABS:
    case FOG_MODE_EXP2_ABS:
        return true;
    default:
        return false;
    }
}

static void append_fog_output_from_distance(
    GString *source, const PGRAPHD3D12PipelineKey *key,
    const char *fog_distance_expr)
{
    float infinite_fogdistance_result = 0.0f;
    float nan_fogfactor_result = 0.0f;

    if (!key->fog_enable) {
        g_string_append(source, "    output.fog = 1.0;\n");
        return;
    }

    g_string_append_printf(
        source,
        "    {\n"
        "        float fogDistance = %s;\n"
        "        float fogFactor = 1.0;\n",
        fog_distance_expr);

    switch (key->fog_mode) {
    case FOG_MODE_LINEAR:
    case FOG_MODE_LINEAR_ABS:
        infinite_fogdistance_result = 1.0f;
        nan_fogfactor_result = 1.0f;
        g_string_append(
            source,
            "        fogFactor = fogParam.x + fogDistance * fogParam.y;\n"
            "        fogFactor -= 1.0;\n");
        break;
    case FOG_MODE_EXP:
        infinite_fogdistance_result = 1.0f;
        nan_fogfactor_result = 1.0f;
        /* fall through */
    case FOG_MODE_EXP_ABS:
        g_string_append(
            source,
            "        fogFactor = fogParam.x + "
            "exp2(fogDistance * fogParam.y * 16.0);\n"
            "        fogFactor -= 1.5;\n");
        break;
    case FOG_MODE_EXP2:
    case FOG_MODE_EXP2_ABS:
        g_string_append(
            source,
            "        fogFactor = fogParam.x + exp2(-fogDistance * "
            "fogDistance * fogParam.y * fogParam.y * 32.0);\n"
            "        fogFactor -= 1.5;\n");
        break;
    default:
        g_string_append_printf(
            source,
            "        /* Unsupported NV2A fog mode %u; preserve visible "
            "geometry with fog factor 1.0. */\n",
            key->fog_mode);
        break;
    }

    if (d3d12_fog_mode_is_abs(key->fog_mode)) {
        g_string_append(source, "        fogFactor = abs(fogFactor);\n");
    }

    g_string_append_printf(
        source,
        "        if (float_is_inf(fogDistance)) {\n"
        "            output.fog = %f;\n"
        "        } else {\n"
        "            output.fog = clamp_fog_interpolant(\n"
        "                nan_scalar_to_value(fogFactor, %f));\n"
        "        }\n"
        "    }\n",
        infinite_fogdistance_result, nan_fogfactor_result);
}

static void fill_fixed_function_key(PGRAPHState *pg,
                                    PGRAPHD3D12PipelineKey *key)
{
    uint32_t csv0_c = pgraph_reg_r(pg, NV_PGRAPH_CSV0_C);
    uint32_t csv0_d = pgraph_reg_r(pg, NV_PGRAPH_CSV0_D);
    uint32_t csv1_a = pgraph_reg_r(pg, NV_PGRAPH_CSV1_A);
    uint32_t csv1_b = pgraph_reg_r(pg, NV_PGRAPH_CSV1_B);

    key->fixed_function_skinning =
        GET_MASK(csv0_d, NV_PGRAPH_CSV0_D_SKIN);
    key->fixed_function_normalization =
        !!(csv0_c & NV_PGRAPH_CSV0_C_NORMALIZATION_ENABLE);
    key->fixed_function_lighting =
        !!(csv0_c & NV_PGRAPH_CSV0_C_LIGHTING);
    key->fixed_function_local_eye =
        !!(csv0_c & NV_PGRAPH_CSV0_C_LOCALEYE);
    key->fixed_function_emission_src =
        GET_MASK(csv0_c, NV_PGRAPH_CSV0_C_EMISSION);
    key->fixed_function_ambient_src =
        GET_MASK(csv0_c, NV_PGRAPH_CSV0_C_AMBIENT);
    key->fixed_function_diffuse_src =
        GET_MASK(csv0_c, NV_PGRAPH_CSV0_C_DIFFUSE);
    key->fixed_function_specular_src =
        GET_MASK(csv0_c, NV_PGRAPH_CSV0_C_SPECULAR);
    if (key->fixed_function_lighting) {
        for (unsigned int i = 0; i < NV2A_MAX_LIGHTS; i++) {
            key->fixed_function_light[i] =
                GET_MASK(csv0_d, NV_PGRAPH_CSV0_D_LIGHT0 << (i * 2));
        }
    }

    for (unsigned int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        if (pg->texture_matrix_enable[i]) {
            key->fixed_function_texture_matrix_enable_mask |= 1u << i;
        }
        for (unsigned int j = 0; j < 4; j++) {
            key->fixed_function_texgen[i][j] =
                fixed_function_texgen_mode(csv1_a, csv1_b, i, j);
        }
    }
}

static bool fill_pipeline_key(PGRAPHState *pg,
                              PGRAPHD3D12PipelineKey *key)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    uint8_t color_write_mask;

    memset(key, 0, sizeof(*key));

    color_write_mask = get_color_write_mask(pg);
    if (r->color_binding == NULL && color_write_mask != 0) {
        pgraph_d3d12_warn_once(
            pg, PGRAPH_D3D12_DIAG_MISSING_SURFACE,
            "draw skipped: color writes are enabled but no color render "
            "target is bound for PSO creation; "
            "primitive=%s (%u), active_input_attributes=%d, "
            "color_write_mask=0x%x, zeta_bound=%d.",
            primitive_mode_name(pg->primitive_mode), pg->primitive_mode,
            r->num_active_vertex_attribute_descriptions,
            color_write_mask, r->zeta_binding != NULL);
        return false;
    }

    if (r->color_binding == NULL && r->zeta_binding == NULL) {
        pgraph_d3d12_warn_once(
            pg, PGRAPH_D3D12_DIAG_MISSING_SURFACE,
            "draw skipped: no color or zeta render target is bound for PSO "
            "creation; primitive=%s (%u), active_input_attributes=%d, "
            "color_write_mask=0x%x, depth_write=%d, depth_test=%d, "
            "stencil_test=%d.",
            primitive_mode_name(pg->primitive_mode), pg->primitive_mode,
            r->num_active_vertex_attribute_descriptions, color_write_mask,
            !!(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0) &
               NV_PGRAPH_CONTROL_0_ZWRITEENABLE),
            !!(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0) &
               NV_PGRAPH_CONTROL_0_ZENABLE),
            !!(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1) &
               NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE));
        return false;
    }

    if (!get_primitive_topology_type(pg, &key->primitive_topology_type)) {
        return false;
    }

    key->polygon_mode = get_polygon_mode(pg);
    key->tune_flags = r->tune_flags & PGRAPH_D3D12_TUNE_SHADER_FLAGS;
    /* Lines and line-mode polygon edges are one guest pixel wide. */
    key->line_expand_width = 1;
    if (pg->surface_scale_factor > 1 &&
        (key->polygon_mode == POLY_MODE_LINE ||
         (!polygon_primitive(pg->primitive_mode) &&
          key->primitive_topology_type ==
              D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE))) {
        key->line_expand_width = 2;
    }

    key->primitive_mode = pg->primitive_mode;
    key->uniform_attrs = pg->uniform_attrs;
    key->swizzle_attrs = pg->swizzle_attrs;
    key->compressed_attrs = pg->compressed_attrs;
    key->smooth_shading =
        GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_3),
                 NV_PGRAPH_CONTROL_3_SHADEMODE) ==
        NV_PGRAPH_CONTROL_3_SHADEMODE_SMOOTH;
    key->first_vertex_provoking =
        GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_3),
                 NV_PGRAPH_CONTROL_3_PROVOKING_VERTEX) ==
        NV_PGRAPH_CONTROL_3_PROVOKING_VERTEX_FIRST;

    if (!(key->uniform_attrs & (1u << NV2A_VERTEX_ATTR_POSITION)) &&
        r->vertex_attribute_to_description_location
            [NV2A_VERTEX_ATTR_POSITION] < 0) {
        VertexAttribute *attr =
            &pg->vertex_attributes[NV2A_VERTEX_ATTR_POSITION];

        pgraph_d3d12_warn_once(
            pg, PGRAPH_D3D12_DIAG_UNSUPPORTED_DRAW_STATE,
            "draw skipped: minimal SM 6.4 PSO requires streamed POSITION "
            "or uniform POSITION attribute %u; current position type=%s (%u), "
            "count=%u, stride=%u, uniform_attrs=0x%04x, "
            "inline_value=(%f,%f,%f,%f).",
            NV2A_VERTEX_ATTR_POSITION,
            vertex_attribute_format_name(attr->format), attr->format,
            attr->count, attr->stride, pg->uniform_attrs,
            attr->inline_value[0], attr->inline_value[1],
            attr->inline_value[2], attr->inline_value[3]);
        return false;
    }

    key->rtv_format = r->color_binding != NULL ?
        r->color_binding->fmt.dxgi_format : DXGI_FORMAT_UNKNOWN;
    key->color_write_mask = r->color_binding != NULL ? color_write_mask : 0;
    if (!fill_blend_key(pg, key) || !fill_alpha_test_key(pg, key) ||
        !fill_rasterizer_key(pg, key)) {
        return false;
    }
    if (!fill_depth_stencil_key(pg, key)) {
        return false;
    }
    key->texture_mask = r->texture_active_mask;
    key->combiner_control = pgraph_reg_r(pg, NV_PGRAPH_COMBINECTL);
    key->shader_stage_program = pgraph_reg_r(pg, NV_PGRAPH_SHADERPROG);
    key->other_stage_input = pgraph_reg_r(pg, NV_PGRAPH_SHADERCTL);
    key->shader_clip_mode = pgraph_reg_r(pg, NV_PGRAPH_SHADERCLIPMODE);
    key->window_clip_exclusive =
        !!(pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER) &
           NV_PGRAPH_SETUPRASTER_WINDOWCLIPTYPE);
    key->fog_enable =
        !!(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_3) &
           NV_PGRAPH_CONTROL_3_FOGENABLE);
    /* Fog mode and generation only shape the shader while fog is enabled;
     * keeping them zero otherwise avoids redundant pipeline variants. */
    key->fog_mode = 0;
    key->fixed_function_foggen = 0;
    if (key->fog_enable) {
        key->fog_mode =
            GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_3),
                     NV_PGRAPH_CONTROL_3_FOG_MODE);
        key->fixed_function_foggen =
            GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CSV0_D),
                     NV_PGRAPH_CSV0_D_FOGGENMODE);
    }
    key->point_params_enable =
        GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CSV0_D),
                 NV_PGRAPH_CSV0_D_POINTPARAMSENABLE);
    key->point_sprite_enable =
        !!(pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER) &
           NV_PGRAPH_SETUPRASTER_POINTSMOOTHENABLE);
    key->specular_enable =
        !!(pgraph_reg_r(pg, NV_PGRAPH_CSV0_C) &
           NV_PGRAPH_CSV0_C_SPECULAR_ENABLE);
    key->separate_specular =
        !!(pgraph_reg_r(pg, NV_PGRAPH_CSV0_C) &
           NV_PGRAPH_CSV0_C_SEPARATE_SPECULAR);
    key->ignore_specular_alpha =
        !(pgraph_reg_r(pg, NV_PGRAPH_CSV0_C) &
          NV_PGRAPH_CSV0_C_ALPHA_FROM_MATERIAL_SPECULAR);
    key->vertex_shader_mode =
        GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CSV0_D), NV_PGRAPH_CSV0_D_MODE);
    if (key->vertex_shader_mode == 0) {
        fill_fixed_function_key(pg, key);
    }
    key->transform_program_start =
        GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CSV0_C),
                 NV_PGRAPH_CSV0_C_CHEOPS_PROGRAM_START);
    if (key->vertex_shader_mode != 0 &&
        key->transform_program_start < NV2A_MAX_TRANSFORM_PROGRAM_LENGTH) {
        for (unsigned int i = key->transform_program_start;
             i < NV2A_MAX_TRANSFORM_PROGRAM_LENGTH; i++) {
            unsigned int dst = key->transform_program_length++;

            memcpy(key->transform_program_data[dst], pg->program_data[i],
                   sizeof(key->transform_program_data[dst]));
            if (d3d12_vsh_get_field(pg->program_data[i], FLD_OUT_O_MASK) &&
                d3d12_vsh_get_field(pg->program_data[i], FLD_OUT_ORB) ==
                    OUTPUT_C) {
                pgraph_d3d12_warn_once(
                    pg, PGRAPH_D3D12_DIAG_UNSUPPORTED_DRAW_STATE,
                    "vertex program slot %u writes the constant file; "
                    "constant writes are not emulated", i);
            }
            if (pg->program_data[i][3] & 1) {
                break;
            }
        }
    }
    key->shadow_depth_func =
        GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_SHADOWCTL),
                 NV_PGRAPH_SHADOWCTL_SHADOW_ZFUNC);
    key->texture_alphakill_mask = get_alphakill_mask(pg);
    key->final_inputs_0 = pgraph_reg_r(pg, NV_PGRAPH_COMBINESPECFOG0);
    key->final_inputs_1 = pgraph_reg_r(pg, NV_PGRAPH_COMBINESPECFOG1);
    for (unsigned int i = 0; i < 8; i++) {
        key->rgb_inputs[i] =
            pgraph_reg_r(pg, NV_PGRAPH_COMBINECOLORI0 + i * 4);
        key->rgb_outputs[i] =
            pgraph_reg_r(pg, NV_PGRAPH_COMBINECOLORO0 + i * 4);
        key->alpha_inputs[i] =
            pgraph_reg_r(pg, NV_PGRAPH_COMBINEALPHAI0 + i * 4);
        key->alpha_outputs[i] =
            pgraph_reg_r(pg, NV_PGRAPH_COMBINEALPHAO0 + i * 4);
    }
    for (unsigned int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        if (key->texture_mask & (1u << i)) {
            uint32_t texctl0 =
                pgraph_reg_r(pg, NV_PGRAPH_TEXCTL0_0 + i * 4);
            uint32_t texfilter =
                pgraph_reg_r(pg, NV_PGRAPH_TEXFILTER0 + i * 4);
            const PGRAPHD3D12TextureBinding *texture = r->texture_binding[i];
            BasicColorFormatInfo basic =
                kelvin_color_format_info_map[texture->shape.color_format];

            /* A custom texture holds the colours the shader would see, so
             * it is sampled without the guest format's swizzle. */
            key->texture_color_format[i] =
                texture->replacement != NULL ?
                    NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8R8G8B8 :
                    texture->shape.color_format;
            key->texture_dxgi_format[i] =
                texture->replacement != NULL ?
                    texture->replacement->dxgi_format : texture->dxgi_format;
            key->texture_width[i] = texture->shape.width;
            key->texture_height[i] = texture->shape.height;
            key->texture_depth[i] = texture->shape.depth;
            key->texture_pitch[i] = texture->shape.pitch;
            key->texture_bytes_per_pixel[i] =
                MAX(texture->upload_bytes_per_pixel, 1);
            /* texture.c keeps channel_signs only where it stored the
             * channels offset by 128 (texture_effective_signs) */
            key->texture_signs[i] = texture->replacement != NULL ? 0 :
                texture->shape.channel_signs & TEX_CHANNEL_SIGNED_MASK;
            if (basic.linear) {
                key->texture_linear_mask |= 1u << i;
            }
            if (texture->surface_to_texture) {
                key->texture_surface_to_texture_mask |= 1u << i;
            }
            if (texture->shape.cubemap) {
                key->texture_cubemap_mask |= 1u << i;
            }
            if (texture->shape.dimensionality == 3) {
                key->texture_3d_mask |= 1u << i;
            }
            if (texture->shape.border && !basic.linear) {
                key->texture_border_adjust_mask |= 1u << i;
                key->texture_border_logical_size[i][0] =
                    MAX(texture->shape.width, 1);
                key->texture_border_logical_size[i][1] =
                    MAX(texture->shape.height, 1);
                key->texture_border_logical_size[i][2] =
                    texture->shape.dimensionality == 3 ?
                        MAX(texture->shape.depth, 1) : 1;
                key->texture_border_inv_real_size[i][0] =
                    border_inv_real_size(key->texture_border_logical_size[i][0]);
                key->texture_border_inv_real_size[i][1] =
                    border_inv_real_size(key->texture_border_logical_size[i][1]);
                key->texture_border_inv_real_size[i][2] =
                    border_inv_real_size(key->texture_border_logical_size[i][2]);
            }
            if (basic.depth) {
                key->texture_depth_mask |= 1u << i;
            }
            if (dxgi_format_is_snorm(texture->dxgi_format)) {
                key->texture_snorm_mask |= 1u << i;
            }
            key->texture_colorkey_mode[i] =
                GET_MASK(texctl0, NV_PGRAPH_TEXCTL0_0_COLORKEYMODE);
            key->texture_convolution_filter[i] =
                texture_convolution_filter_from_register(texfilter);
        }
    }
    {
        uint8_t execution_mask = texture_stage_execution_mask(key);

        for (unsigned int i = 0; i < NV2A_MAX_TEXTURES; i++) {
            if (execution_mask & (1u << i)) {
                key->texture_coord_mask |=
                    texture_mode_coord_mask(i, texture_stage_mode(key, i));
            }
        }
    }
    if (r->num_active_vertex_attribute_descriptions < 0 ||
        r->num_active_vertex_attribute_descriptions >
            NV2A_VERTEXSHADER_ATTRIBUTES) {
        pgraph_d3d12_warn_once(
            pg, PGRAPH_D3D12_DIAG_UNSUPPORTED_DRAW_STATE,
            "draw skipped: D3D12 vertex input layout has invalid attribute "
            "description count %d; valid range is 0..%u, primitive=%s (%u), "
            "uniform_attrs=0x%04x, swizzle_attrs=0x%04x, "
            "compressed_attrs=0x%04x.",
            r->num_active_vertex_attribute_descriptions,
            (unsigned)NV2A_VERTEXSHADER_ATTRIBUTES,
            primitive_mode_name(pg->primitive_mode), pg->primitive_mode,
            pg->uniform_attrs, pg->swizzle_attrs, pg->compressed_attrs);
        return false;
    }

    key->num_attributes = r->num_active_vertex_attribute_descriptions;
    for (uint8_t i = 0; i < key->num_attributes; i++) {
        const D3D12_INPUT_ELEMENT_DESC *attr =
            &r->vertex_attribute_descriptions[i];

        key->attribute_indices[i] = attr->SemanticIndex;
        key->attribute_formats[i] = attr->Format;
    }

    if (pgraph_d3d12_verbose_reports(pg)) {
        report_programmable_vsh_fallback(pg, key);
    }

    return true;
}

/*
 * Clears key fields that do not change the generated shaders or the PSO, so
 * equivalent draws share one pipeline.
 */
static void canonicalize_pipeline_key(PGRAPHD3D12PipelineKey *key)
{
    unsigned int num_stages = MIN(key->combiner_control & 0xff, 8);

    key->texture_surface_to_texture_mask = 0;
    key->transform_program_start = 0;

    /* Depth mode and format only select fragment depth code. */
    if (!pipeline_key_fragment_zvalue(key)) {
        key->z_perspective = false;
    }
    if (!pipeline_key_shader_depth_output(key)) {
        key->depth_format = PGRAPH_D3D12_DEPTH_FORMAT_D24;
    }

    for (unsigned int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        key->texture_dxgi_format[i] = DXGI_FORMAT_UNKNOWN;
        key->texture_pitch[i] = 0;
        key->texture_bytes_per_pixel[i] = 0;
        key->texture_depth[i] = 0;
        /* Linear sizes are pixel constants. */
        key->texture_width[i] = 0;
        key->texture_height[i] = 0;
        /* The shader only applies the format's swizzle; every format
         * sampled as-is shares one. */
        if (strcmp(texture_swizzle_expression(key->texture_color_format[i]),
                   "t") == 0) {
            key->texture_color_format[i] =
                NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8R8G8B8;
        }
    }

    /* Alpha test, alpha kill and the shadow compare are pixel constants;
     * the key keeps only whether the shader may discard. */
    {
        uint8_t executed = texture_stage_execution_mask(key);
        uint8_t kill = 0;

        /* Kill applies only where the shader emits the sample. */
        for (unsigned int i = 0; i < NV2A_MAX_TEXTURES; i++) {
            if ((executed & (1u << i)) && texture_stage_samples(key, i)) {
                kill |= key->texture_alphakill_mask & (1u << i);
            }
        }
        key->alpha_test_enable = key->alpha_test_enable || kill != 0;
    }
    key->alpha_func = 0;
    key->texture_alphakill_mask = 0;
    key->shadow_depth_func = 0;
    key->tune_flags &= PGRAPH_D3D12_TUNE_SHADER_FLAGS;

    /* Combiner stages past the active count are never emitted. */
    for (unsigned int i = num_stages; i < 8; i++) {
        key->rgb_inputs[i] = 0;
        key->rgb_outputs[i] = 0;
        key->alpha_inputs[i] = 0;
        key->alpha_outputs[i] = 0;
    }

    /* Primitive mode and provoking vertex only select geometry-shader code;
     * the PSO takes the topology type. Triangle edges and points need
     * neither: the CPU expansion starts each triangle at its provoking
     * vertex. */
    if (!draw_geometry_shader_needed(key) ||
        (key->primitive_topology_type ==
             D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE &&
         key->polygon_mode != POLY_MODE_FILL)) {
        key->primitive_mode = 0;
        key->first_vertex_provoking = false;
    }
    /* The provoking vertex only picks flat colours, and quads (vertex 3)
     * and polygons (vertex 0) ignore it. */
    if (key->smooth_shading ||
        key->primitive_mode == PRIM_TYPE_QUADS ||
        key->primitive_mode == PRIM_TYPE_QUAD_STRIP ||
        key->primitive_mode == PRIM_TYPE_POLYGON) {
        key->first_vertex_provoking = false;
    }
    /* Winding only matters for culling. */
    if (key->cull_mode == D3D12_CULL_MODE_NONE) {
        key->front_counter_clockwise = false;
    }
    /* Pulled attributes decode from vertexFetch[].w at run time; the input
     * assembler path bakes the formats into its layout. */
    if (pipeline_key_dynamic_vertex_formats(key)) {
        memset(key->attribute_formats, 0, sizeof(key->attribute_formats));
        key->compressed_attrs = 0;
        key->swizzle_attrs = 0;
    }
}

static void run_shader_stage(PGRAPHState *pg, PGRAPHD3D12ShaderStage *stage)
{
    IDxcCompiler3 *compiler = NULL;

    stage->ok = get_shader_bytecode(pg, &compiler, stage->source,
                                    stage->entry_point, stage->target,
                                    stage->dxil, &stage->bytes,
                                    &stage->compile_us, &stage->err);
    D3D12_RELEASE(compiler);
}

static void *shader_stage_thread(void *opaque)
{
    PGRAPHState *pg = opaque;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;

    qemu_mutex_lock(&r->stage_mutex);
    while (!r->stage_stop) {
        PGRAPHD3D12ShaderStage *stage = QSIMPLEQ_FIRST(&r->stage_jobs);

        if (stage == NULL) {
            qemu_cond_wait(&r->stage_cond, &r->stage_mutex);
            continue;
        }
        QSIMPLEQ_REMOVE_HEAD(&r->stage_jobs, entry);
        qemu_mutex_unlock(&r->stage_mutex);
        run_shader_stage(pg, stage);
        qemu_mutex_lock(&r->stage_mutex);
        stage->done = true;
        qemu_cond_broadcast(&r->stage_done_cond);
    }
    qemu_mutex_unlock(&r->stage_mutex);
    return NULL;
}

/*
 * Bytecode for a pipeline's stages; stages without source are skipped.
 * With parallel set, the stages missing from memory compile on the stage
 * helpers alongside the last one, which compiles here, so a miss costs the
 * slowest compile instead of the sum.
 */
static void compile_shader_stages(PGRAPHState *pg,
                                  PGRAPHD3D12ShaderStage *stages,
                                  unsigned int count, bool parallel)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    PGRAPHD3D12ShaderStage *missing[3];
    unsigned int num_missing = 0;
    unsigned int queued = 0;

    assert(count <= ARRAY_SIZE(missing));
    for (unsigned int i = 0; i < count; i++) {
        PGRAPHD3D12ShaderStage *stage = &stages[i];

        if (stage->source == NULL) {
            continue;
        }
        stage->bytes = lookup_memory_bytecode(r, stage->source,
                                              stage->target, stage->dxil);
        if (stage->bytes != NULL) {
            stage->ok = true;
            stage->memory_hit = true;
        } else {
            missing[num_missing++] = stage;
        }
    }

    if (parallel && num_missing > 1) {
        qemu_mutex_lock(&r->stage_mutex);
        while (queued + 1 < num_missing && queued < r->num_stage_threads) {
            missing[queued]->done = false;
            QSIMPLEQ_INSERT_TAIL(&r->stage_jobs, missing[queued], entry);
            queued++;
        }
        qemu_cond_broadcast(&r->stage_cond);
        qemu_mutex_unlock(&r->stage_mutex);
    }
    for (unsigned int i = queued; i < num_missing; i++) {
        run_shader_stage(pg, missing[i]);
    }
    if (queued != 0) {
        qemu_mutex_lock(&r->stage_mutex);
        for (unsigned int i = 0; i < queued; i++) {
            while (!missing[i]->done) {
                qemu_cond_wait(&r->stage_done_cond, &r->stage_mutex);
            }
        }
        qemu_mutex_unlock(&r->stage_mutex);
    }
}

static void release_shader_stages(PGRAPHD3D12ShaderStage *stages,
                                  unsigned int count)
{
    for (unsigned int i = 0; i < count; i++) {
        if (stages[i].bytes != NULL) {
            g_bytes_unref(stages[i].bytes);
            stages[i].bytes = NULL;
        }
        error_free(stages[i].err);
        stages[i].err = NULL;
    }
}

/*
 * A damaged cache file can pass the container check and fail only at
 * pipeline creation. Compiles the stages that came from a cache again and
 * replaces cached bytecode that differs, in memory and on disk. Returns
 * true if any stage changed.
 */
static bool refresh_cached_stages(PGRAPHState *pg,
                                  PGRAPHD3D12ShaderStage *stages,
                                  unsigned int count)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    IDxcCompiler3 *compiler = NULL;
    bool changed = false;

    if (!shader_disk_cache_enabled(r)) {
        return false;
    }
    for (unsigned int i = 0; i < count; i++) {
        PGRAPHD3D12ShaderStage *stage = &stages[i];
        LPCWSTR target = stage->target;
        const char *fxc_target;
        const char *fxc_entry;
        GBytes *bytes;
        GBytes *cached;
        uint64_t hash;

        /* Stages compiled by this build are already fresh. DXIL from memory
         * is not compiled again: a DXIL failure rebuilds as DXBC. */
        if (stage->bytes == NULL || stage->compile_us != 0 ||
            (stage->dxil && stage->memory_hit)) {
            continue;
        }
        resolve_shader_target(stage->dxil, &target, &fxc_target, &fxc_entry);
        bytes = compile_shader_source(pg, &compiler, stage->source,
                                      stage->entry_point, target, fxc_target,
                                      fxc_entry, NULL);
        if (bytes == NULL) {
            continue;
        }
        if (g_bytes_equal(bytes, stage->bytes)) {
            g_bytes_unref(bytes);
            continue;
        }
        hash = shader_bytecode_hash(stage->source, target);
        write_shader_cache_file(r, hash, bytes);
        qemu_mutex_lock(&r->shader_bytecode_mutex);
        cached = g_hash_table_lookup(r->shader_bytecode_cache, &hash);
        if (cached != NULL) {
            r->shader_bytecode_bytes = r->shader_bytecode_bytes -
                                       g_bytes_get_size(cached) +
                                       g_bytes_get_size(bytes);
            g_hash_table_insert(r->shader_bytecode_cache,
                                g_memdup2(&hash, sizeof(hash)),
                                g_bytes_ref(bytes));
        }
        qemu_mutex_unlock(&r->shader_bytecode_mutex);
        g_bytes_unref(stage->bytes);
        stage->bytes = bytes;
        changed = true;
    }
    D3D12_RELEASE(compiler);
    return changed;
}

/* Points the pipeline's shader stages at the stages' bytecode. */
static void set_pipeline_bytecode(D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc,
                                  PGRAPHD3D12ShaderStage *stages)
{
    D3D12_SHADER_BYTECODE *bytecode[3] = { &desc->VS, &desc->GS, &desc->PS };

    for (unsigned int i = 0; i < ARRAY_SIZE(bytecode); i++) {
        gsize size = 0;

        bytecode[i]->pShaderBytecode = stages[i].bytes != NULL ?
            g_bytes_get_data(stages[i].bytes, &size) : NULL;
        bytecode[i]->BytecodeLength = size;
    }
}

/* Sets up a draw pipeline's stages; gs may be NULL. */
static void init_draw_stages(PGRAPHD3D12ShaderStage stages[3],
                             const char *vs, const char *gs, const char *ps,
                             bool dxil)
{
    memset(stages, 0, 3 * sizeof(stages[0]));
    stages[0].source = vs;
    stages[0].entry_point = L"vs_main";
    stages[0].target = L"vs_6_4";
    stages[1].source = gs;
    stages[1].entry_point = L"gs_main";
    stages[1].target = L"gs_6_4";
    stages[2].source = ps;
    stages[2].entry_point = L"ps_main";
    stages[2].target = L"ps_6_4";
    for (unsigned int i = 0; i < 3; i++) {
        stages[i].dxil = dxil;
    }
}

/* Compiles a draw pipeline's stages; the FIFO thread counts compiles. */
static void compile_draw_stages(PGRAPHState *pg,
                                PGRAPHD3D12ShaderStage stages[3])
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;

    /* A draw waits on this build or on a worker's raised demand build, so
     * its stages compile concurrently; precompile workers build pipelines
     * side by side instead. */
    compile_shader_stages(pg, stages, 3,
                          !pso_worker_thread ||
                              (current_pso_worker != NULL &&
                               qatomic_read(&current_pso_worker->boosted)));
    if (!pso_worker_thread) {
        for (unsigned int i = 0; i < 3; i++) {
            if (stages[i].compile_us != 0) {
                r->perf.shader_compiles++;
                r->perf.shader_compile_us += stages[i].compile_us;
            }
        }
    }
}

/* The first stage with source that failed to compile, or NULL. */
static PGRAPHD3D12ShaderStage *failed_draw_stage(
    PGRAPHD3D12ShaderStage stages[3])
{
    for (unsigned int i = 0; i < 3; i++) {
        if (stages[i].source != NULL && !stages[i].ok) {
            return &stages[i];
        }
    }
    return NULL;
}

/* DXC leaves the container digest zero when dxil.dll did not sign its
 * output, and drivers reject unsigned DXIL. */
static void check_dxil_signed(PGRAPHState *pg, GBytes *bytes)
{
    static const uint8_t zero_digest[16];
    gsize size = 0;
    const uint8_t *data = bytes != NULL ? g_bytes_get_data(bytes, &size) :
                                          NULL;

    if (size >= 4 + sizeof(zero_digest) &&
        memcmp(data + 4, zero_digest, sizeof(zero_digest)) == 0) {
        pgraph_d3d12_warn_once(pg, PGRAPH_D3D12_DIAG_DXIL_UNSIGNED,
                               "DXIL shaders are unsigned; their pipelines "
                               "fall back to DXBC if the driver rejects "
                               "them");
    }
}

/* Turns DXIL off after a failure so later pipelines skip straight to DXBC
 * instead of repeating it; reported once each time it turns off. */
static void G_GNUC_PRINTF(2, 3) disable_dxil(PGRAPHD3D12State *r,
                                             const char *fmt, ...)
{
    va_list ap;

    if (qatomic_xchg(&r->dxil_disabled, true)) {
        return;
    }
    fprintf(stderr, "Direct3D 12 renderer: DXIL off until \"dxbc\" is "
                    "added to and removed from the tune file: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* CreateGraphicsPipelineState and its duration. */
static HRESULT create_timed_pso(PGRAPHD3D12State *r,
                                const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc,
                                ID3D12PipelineState **pso_out,
                                int64_t *driver_us)
{
    int64_t start = g_get_monotonic_time();
    HRESULT hr = r->device->lpVtbl->CreateGraphicsPipelineState(
        r->device, desc, &IID_ID3D12PipelineState, (void **)pso_out);

    *driver_us = g_get_monotonic_time() - start;
    return hr;
}

/* How create_minimal_draw_pso() built a pipeline. */
typedef struct DrawPsoBuildInfo {
    bool dxil;              /* the stages are DXIL */
    bool dxil_fallback;     /* DXIL was asked for, DXBC built it */
    int64_t driver_us;      /* the create call that built it */
    int64_t twin_us;        /* measurement twins, not part of the build */
} DrawPsoBuildInfo;

/*
 * Measurement path (tune "twintest"): creates one twin of a draw pipeline
 * and releases it at once. Twins are never bound, so no command list
 * references them.
 */
static void create_twin_pso(PGRAPHD3D12State *r,
                            const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc,
                            const char *kind, uint64_t *count,
                            uint64_t *total_us)
{
    static unsigned int failures_logged;
    ID3D12PipelineState *pso = NULL;
    int64_t us;
    HRESULT hr = create_timed_pso(r, desc, &pso, &us);

    if (FAILED(hr)) {
        qatomic_inc(&r->twin_pso.failed);
        if (qatomic_fetch_inc(&failures_logged) < 8) {
            fprintf(stderr,
                    "Direct3D 12 twintest: %s twin failed: HRESULT 0x%08lx "
                    "removed=0x%08lx\n",
                    kind, (unsigned long)hr,
                    (unsigned long)r->device->lpVtbl->GetDeviceRemovedReason(
                        r->device));
        }
        return;
    }
    D3D12_RELEASE(pso);
    qatomic_inc(count);
    qatomic_add(total_us, (uint64_t)us);
}

/*
 * Rebuilds a draw pipeline with only the depth function, the blend state or
 * the render-target format changed, to see whether the driver reuses the
 * compiled stages. Kinds that cannot apply (no depth or colour target, a
 * logic op) are skipped. Returns the time spent.
 */
static int64_t create_twin_psos(PGRAPHD3D12State *r,
                                const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc,
                                int64_t base_us)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC twin;
    D3D12_RENDER_TARGET_BLEND_DESC *rt = &twin.BlendState.RenderTarget[0];
    int64_t start = g_get_monotonic_time();

    qatomic_inc(&r->twin_pso.sets);
    qatomic_add(&r->twin_pso.base_us, (uint64_t)base_us);

    if (desc->DSVFormat != DXGI_FORMAT_UNKNOWN) {
        twin = *desc;
        if (twin.DepthStencilState.DepthEnable) {
            twin.DepthStencilState.DepthFunc =
                twin.DepthStencilState.DepthFunc ==
                        D3D12_COMPARISON_FUNC_LESS_EQUAL ?
                    D3D12_COMPARISON_FUNC_LESS :
                    D3D12_COMPARISON_FUNC_LESS_EQUAL;
        } else {
            twin.DepthStencilState.DepthEnable = TRUE;
            twin.DepthStencilState.DepthWriteMask =
                D3D12_DEPTH_WRITE_MASK_ZERO;
            twin.DepthStencilState.DepthFunc =
                D3D12_COMPARISON_FUNC_LESS_EQUAL;
        }
        create_twin_pso(r, &twin, "depth", &r->twin_pso.depth,
                        &r->twin_pso.depth_us);
    }
    if (desc->NumRenderTargets != 0 &&
        !desc->BlendState.RenderTarget[0].LogicOpEnable) {
        twin = *desc;
        if (rt->BlendEnable) {
            rt->BlendEnable = FALSE;
        } else {
            rt->BlendEnable = TRUE;
            rt->SrcBlend = D3D12_BLEND_SRC_ALPHA;
            rt->DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
            rt->BlendOp = D3D12_BLEND_OP_ADD;
            rt->SrcBlendAlpha = D3D12_BLEND_ONE;
            rt->DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
            rt->BlendOpAlpha = D3D12_BLEND_OP_ADD;
        }
        create_twin_pso(r, &twin, "blend", &r->twin_pso.blend,
                        &r->twin_pso.blend_us);
    }
    if (desc->NumRenderTargets != 0) {
        /* The other common guest colour format; both take blending. */
        twin = *desc;
        twin.RTVFormats[0] =
            desc->RTVFormats[0] == DXGI_FORMAT_B8G8R8A8_UNORM ?
                DXGI_FORMAT_B5G5R5A1_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
        create_twin_pso(r, &twin, "rtv", &r->twin_pso.rtv,
                        &r->twin_pso.rtv_us);
    }
    return g_get_monotonic_time() - start;
}

/*
 * Compiles a draw pipeline's stages and creates it with the key's fixed
 * state; takes the sources. An interpreter pipeline is DXIL only.
 */
static bool create_draw_pso_from_sources(
    PGRAPHState *pg, const PGRAPHD3D12PipelineKey *key,
    const D3D12_INPUT_ELEMENT_DESC *input_descs, char *vs_source,
    char *gs_source, char *ps_source, bool uber_ps,
    ID3D12PipelineState **pso_out, DrawPsoBuildInfo *info, Error **errp)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    PGRAPHD3D12ShaderStage stages[3];
    PGRAPHD3D12ShaderStage *failed;
    ID3D12PipelineState *pipeline_state = NULL;
    char *attrs = NULL;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc;
    D3D12_RENDER_TARGET_BLEND_DESC rt_blend;
    D3D12_DEPTH_STENCILOP_DESC stencil_op;
    int64_t driver_us = 0;
    int64_t twin_us = 0;
    bool dxil_requested;
    bool dxil;
    HRESULT hr;

    if (info != NULL) {
        info->dxil = false;
        info->dxil_fallback = false;
        info->driver_us = 0;
        info->twin_us = 0;
    }
    maybe_dump_shader_sources(pg, key, vs_source, gs_source, ps_source);

    /* Where FXC is available, the tune bit (set by default) selects DXIL. A
     * pipeline cannot mix the two, so any DXIL failure rebuilds every
     * stage. The interpreter is DXIL only, and so is the rest of its
     * pipeline; it has no DXBC retry. */
    dxil_requested = uber_ps ||
                     ((key->tune_flags & PGRAPH_D3D12_TUNE_SHADER_DXIL) &&
                      pgraph_d3d12_use_fxc());
    dxil = uber_ps || (dxil_requested && !qatomic_read(&r->dxil_disabled));
    if (dxil && !dxc_ready(pg)) {
        if (uber_ps) {
            error_setg(errp, "Direct3D 12 interpreter pixel shader needs "
                             "DXC, which is unavailable");
            g_free(vs_source);
            g_free(gs_source);
            g_free(ps_source);
            return false;
        }
        pgraph_d3d12_warn_once(pg, PGRAPH_D3D12_DIAG_DXIL_FALLBACK,
                               "DXIL shaders unavailable; pipelines use "
                               "DXBC");
        dxil = false;
    }
    init_draw_stages(stages, vs_source, gs_source, ps_source, dxil);
    compile_draw_stages(pg, stages);
    failed = dxil && !uber_ps ? failed_draw_stage(stages) : NULL;
    if (failed != NULL) {
        g_autofree char *dxil_error = g_strdup(
            failed->err != NULL ? error_get_pretty(failed->err) :
                                  "unknown error");

        release_shader_stages(stages, ARRAY_SIZE(stages));
        dxil = false;
        init_draw_stages(stages, vs_source, gs_source, ps_source, false);
        compile_draw_stages(pg, stages);
        /* Only a failure DXBC does not share turns DXIL off. */
        if (failed_draw_stage(stages) == NULL) {
            disable_dxil(r, "compile failed: %s", dxil_error);
        }
    } else if (dxil) {
        check_dxil_signed(pg, stages[0].bytes);
    }

    if (!stages[0].ok) {
        error_propagate(errp, stages[0].err);
        stages[0].err = NULL;
        error_prepend(errp,
                      "Direct3D 12 SM 6.4 vertex shader generation failed "
                      "for primitive=%s, uniform_attrs=0x%04x, "
                      "swizzle_attrs=0x%04x, compressed_attrs=0x%04x, "
                      "texture_mask=0x%x, shaderprog=0x%08x, "
                      "vsh_mode=%u, transform_program_start=%u, "
                      "fog_enable=%d, fog_mode=%u, foggen=%u, "
                      "point_params={enable:%d,point_sprite:%d,"
                      "raw_size:%f,scaled_size:%f,"
                      "params=[%f,%f,%f,%f,%f,%f,%f,%f]}: ",
                      primitive_topology_type_name(
                          key->primitive_topology_type),
                      key->uniform_attrs, key->swizzle_attrs,
                      key->compressed_attrs, key->texture_mask,
                      key->shader_stage_program, key->vertex_shader_mode,
                      key->transform_program_start, key->fog_enable,
                      key->fog_mode, key->fixed_function_foggen,
                      key->point_params_enable, key->point_sprite_enable,
                      pgraph_reg_r(pg, NV_PGRAPH_POINTSIZE) / 8.0f,
                      MAX(pgraph_reg_r(pg, NV_PGRAPH_POINTSIZE) / 8.0f,
                          1.0f) *
                          MAX(pg->surface_scale_factor, 1),
                      pg->point_params[0], pg->point_params[1],
                      pg->point_params[2], pg->point_params[3],
                      pg->point_params[4], pg->point_params[5],
                      pg->point_params[6], pg->point_params[7]);
        g_free(vs_source);
        g_free(gs_source);
        g_free(ps_source);
        release_shader_stages(stages, ARRAY_SIZE(stages));
        return false;
    }
    if (gs_source != NULL && !stages[1].ok) {
        error_propagate(errp, stages[1].err);
        stages[1].err = NULL;
        error_prepend(errp,
                      "Direct3D 12 SM 6.4 geometry shader generation "
                      "failed for primitive=%s, texture_coord_mask=0x%x, "
                      "depth={target:%d,format:%s(%u),z_perspective:%d,"
                      "clip:%d}, "
                      "point_params={enable:%d,point_sprite:%d,"
                      "raw_size:%f,scaled_size:%f,"
                      "params=[%f,%f,%f,%f,%f,%f,%f,%f]}, "
                      "surface=%ux%u: ",
                      primitive_topology_type_name(
                          key->primitive_topology_type),
                      key->texture_coord_mask,
                      key->dsv_format != DXGI_FORMAT_UNKNOWN,
                      depth_format_name(key->depth_format),
                      key->depth_format, key->z_perspective,
                      key->depth_clipping,
                      key->point_params_enable, key->point_sprite_enable,
                      pgraph_reg_r(pg, NV_PGRAPH_POINTSIZE) / 8.0f,
                      MAX(pgraph_reg_r(pg, NV_PGRAPH_POINTSIZE) / 8.0f,
                          1.0f) *
                          MAX(pg->surface_scale_factor, 1),
                      pg->point_params[0], pg->point_params[1],
                      pg->point_params[2], pg->point_params[3],
                      pg->point_params[4], pg->point_params[5],
                      pg->point_params[6], pg->point_params[7],
                      pg->surface_binding_dim.width,
                      pg->surface_binding_dim.height);
        g_free(vs_source);
        g_free(gs_source);
        g_free(ps_source);
        release_shader_stages(stages, ARRAY_SIZE(stages));
        return false;
    }
    if (!stages[2].ok) {
        error_propagate(errp, stages[2].err);
        stages[2].err = NULL;
        error_prepend(errp,
                      "Direct3D 12 SM 6.4 pixel shader generation failed "
                      "for combinectl=0x%08x, shaderprog=0x%08x, "
                      "shaderctl=0x%08x, shaderclip=0x%08x, "
                      "window_clip_exclusive=%d, "
                      "shader_framebuffer_fetch=%d, "
                      "shader_blend={enable:%d,src:%s(%u),dst:%s(%u),"
                      "eq:%s(%u)}, shader_logic_op={enable:%d,op:%s(%u)}, "
                      "depth={target:%d,format:%s(%u),z_perspective:%d,"
                      "clip:%d}, "
                      "texture_mask=0x%x, texture_modes=[0x%x,0x%x,0x%x,"
                      "0x%x], texture_linear_mask=0x%x, "
                      "texture_cubemap_mask=0x%x, texture_3d_mask=0x%x, "
                      "texture_border_adjust_mask=0x%x, "
                      "texture_depth_mask=0x%x, texture_snorm_mask=0x%x, "
                      "texture_coord_mask=0x%x, texture_depths=[%u,%u,%u,%u], "
                      "texture_convolution=[%s(%u),%s(%u),%s(%u),%s(%u)], "
                      "final0=0x%08x, final1=0x%08x: ",
                      key->combiner_control, key->shader_stage_program,
                      key->other_stage_input, key->shader_clip_mode,
                      key->window_clip_exclusive,
                      key->shader_framebuffer_fetch,
                      key->shader_blend_enable,
                      blend_factor_name(key->shader_blend_src_factor),
                      key->shader_blend_src_factor,
                      blend_factor_name(key->shader_blend_dest_factor),
                      key->shader_blend_dest_factor,
                      blend_equation_name(key->shader_blend_equation),
                      key->shader_blend_equation,
                      key->shader_logic_op_enable,
                      logic_op_name(key->shader_logic_op),
                      key->shader_logic_op,
                      key->dsv_format != DXGI_FORMAT_UNKNOWN,
                      depth_format_name(key->depth_format),
                      key->depth_format, key->z_perspective,
                      key->depth_clipping,
                      key->texture_mask, texture_stage_mode(key, 0),
                      texture_stage_mode(key, 1), texture_stage_mode(key, 2),
                      texture_stage_mode(key, 3), key->texture_linear_mask,
                      key->texture_cubemap_mask, key->texture_3d_mask,
                      key->texture_border_adjust_mask,
                      key->texture_depth_mask, key->texture_snorm_mask,
                      key->texture_coord_mask, key->texture_depth[0],
                      key->texture_depth[1], key->texture_depth[2],
                      key->texture_depth[3],
                      texture_convolution_filter_name(
                          key->texture_convolution_filter[0]),
                      key->texture_convolution_filter[0],
                      texture_convolution_filter_name(
                          key->texture_convolution_filter[1]),
                      key->texture_convolution_filter[1],
                      texture_convolution_filter_name(
                          key->texture_convolution_filter[2]),
                      key->texture_convolution_filter[2],
                      texture_convolution_filter_name(
                          key->texture_convolution_filter[3]),
                      key->texture_convolution_filter[3],
                      key->final_inputs_0, key->final_inputs_1);
        g_free(vs_source);
        g_free(gs_source);
        g_free(ps_source);
        release_shader_stages(stages, ARRAY_SIZE(stages));
        return false;
    }

    memset(&desc, 0, sizeof(desc));
    desc.pRootSignature = r->draw_root_signature;
    set_pipeline_bytecode(&desc, stages);

    memset(&rt_blend, 0, sizeof(rt_blend));
    rt_blend.BlendEnable = key->logic_op_enable ? FALSE : key->blend_enable;
    rt_blend.LogicOpEnable = key->logic_op_enable;
    rt_blend.SrcBlend = key->src_blend;
    rt_blend.DestBlend = key->dest_blend;
    rt_blend.BlendOp = key->blend_op;
    rt_blend.SrcBlendAlpha = key->src_blend_alpha;
    rt_blend.DestBlendAlpha = key->dest_blend_alpha;
    rt_blend.BlendOpAlpha = key->blend_op_alpha;
    rt_blend.LogicOp = key->logic_op;
    rt_blend.RenderTargetWriteMask = key->color_write_mask;
    desc.BlendState.RenderTarget[0] = rt_blend;

    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = key->cull_mode;
    desc.RasterizerState.FrontCounterClockwise =
        key->front_counter_clockwise;
    /* The pixel shader applies the polygon offset and the ZCLIPMIN/ZCLIPMAX
     * cull or clamp; the rasterizer never clips on depth. */
    desc.RasterizerState.DepthBias = 0;
    desc.RasterizerState.DepthBiasClamp = 0.0f;
    desc.RasterizerState.SlopeScaledDepthBias = 0.0f;
    desc.RasterizerState.DepthClipEnable = FALSE;
    desc.RasterizerState.MultisampleEnable = FALSE;
    desc.RasterizerState.AntialiasedLineEnable = FALSE;
    desc.RasterizerState.ForcedSampleCount = 0;
    desc.RasterizerState.ConservativeRaster =
        D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    memset(&stencil_op, 0, sizeof(stencil_op));
    stencil_op.StencilFailOp = key->stencil_enable ?
        key->stencil_fail_op : D3D12_STENCIL_OP_KEEP;
    stencil_op.StencilDepthFailOp = key->stencil_enable ?
        key->stencil_depth_fail_op : D3D12_STENCIL_OP_KEEP;
    stencil_op.StencilPassOp = key->stencil_enable ?
        key->stencil_pass_op : D3D12_STENCIL_OP_KEEP;
    stencil_op.StencilFunc = key->stencil_enable ?
        key->stencil_func : D3D12_COMPARISON_FUNC_ALWAYS;
    desc.DepthStencilState.DepthEnable = key->depth_enable;
    desc.DepthStencilState.DepthWriteMask = key->depth_write ?
        D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    desc.DepthStencilState.DepthFunc = key->depth_enable ?
        key->depth_func : D3D12_COMPARISON_FUNC_ALWAYS;
    desc.DepthStencilState.StencilEnable = key->stencil_enable;
    desc.DepthStencilState.StencilReadMask = key->stencil_enable ?
        key->stencil_read_mask : 0xff;
    desc.DepthStencilState.StencilWriteMask = key->stencil_enable ?
        key->stencil_write_mask : 0xff;
    desc.DepthStencilState.FrontFace = stencil_op;
    desc.DepthStencilState.BackFace = stencil_op;

    if (pgraph_d3d12_vertex_pulling()) {
        desc.InputLayout.pInputElementDescs = NULL;
        desc.InputLayout.NumElements = 0;
    } else {
        desc.InputLayout.pInputElementDescs = input_descs;
        desc.InputLayout.NumElements = key->num_attributes;
    }
    desc.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
    desc.PrimitiveTopologyType = key->primitive_topology_type;
    if (key->rtv_format != DXGI_FORMAT_UNKNOWN) {
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = key->rtv_format;
    } else {
        desc.NumRenderTargets = 0;
    }
    desc.DSVFormat = key->dsv_format;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

    /* driver_us ends as the time of the call that built the pipeline. */
    hr = create_timed_pso(r, &desc, &pipeline_state, &driver_us);
    if (FAILED(hr) &&
        refresh_cached_stages(pg, stages, ARRAY_SIZE(stages))) {
        set_pipeline_bytecode(&desc, stages);
        hr = create_timed_pso(r, &desc, &pipeline_state, &driver_us);
    }
    if (FAILED(hr) && dxil && !uber_ps) {
        HRESULT dxil_hr = hr;

        /* Rebuilt as DXBC; a failure here would skip the state's draws
         * for good. */
        release_shader_stages(stages, ARRAY_SIZE(stages));
        dxil = false;
        init_draw_stages(stages, vs_source, gs_source, ps_source, false);
        compile_draw_stages(pg, stages);
        if (failed_draw_stage(stages) == NULL) {
            set_pipeline_bytecode(&desc, stages);
            hr = create_timed_pso(r, &desc, &pipeline_state, &driver_us);
        }
        /* Only a failure DXBC does not share turns DXIL off. */
        if (SUCCEEDED(hr)) {
            disable_dxil(r, "pipeline creation failed: HRESULT 0x%08lx",
                         (unsigned long)dxil_hr);
        }
    }
    /* Measurement only; the twins use the stage bytecode still held. */
    if (SUCCEEDED(hr) &&
        (qatomic_read(&r->tune_flags) & PGRAPH_D3D12_TUNE_TWIN_PSOS)) {
        twin_us = create_twin_psos(r, &desc, driver_us);
    }
    /* The device keeps its own copy of the bytecode. The retries above
     * recompile from the sources, so they live until here. */
    release_shader_stages(stages, ARRAY_SIZE(stages));
    g_free(vs_source);
    g_free(gs_source);
    g_free(ps_source);
    if (FAILED(hr)) {
        pgraph_d3d12_report_info_queue(r, "graphics pipeline state creation");
        attrs = pipeline_key_attribute_summary(key);
        error_setg(errp,
                   "Failed to create Direct3D 12 minimal SM 6.4 PSO: "
                   "HRESULT 0x%08lx, primitive_topology_type=%s, "
                   "rtv_dxgi=%u, dsv_dxgi=%u, color_write_mask=0x%x, "
                   "blend_enable=%d, logic_op={enable:%d,op:%u}, "
                   "blend={src:%u,dst:%u,op:%u,"
                   "src_alpha:%u,dst_alpha:%u,op_alpha:%u,const_mode:%u,"
                   "shader_src_factor_enable:%d,"
                   "shader_src_factor:%s(%u)}, "
                   "shader_framebuffer_fetch=%d, "
                   "shader_blend={enable:%d,src:%s(%u),dst:%s(%u),"
                   "eq:%s(%u)}, shader_logic_op={enable:%d,op:%s(%u)}, "
                   "alpha_test={may_discard:%d}, "
                   "raster={cull:%u,front_ccw:%d}, "
                   "depth={enable:%d,write:%d,func:%u,format:%s(%u),"
                   "z_perspective:%d,clip:%d}, "
                   "stencil_enable=%d, stencil_func=%u, "
                   "stencil_read_mask=0x%02x, stencil_write_mask=0x%02x, "
                   "stencil_ops={fail:%u,zfail:%u,zpass:%u}, "
                   "shader={combinectl:0x%08x, shaderprog:0x%08x, "
                   "shaderctl:0x%08x, shaderclip:0x%08x, "
                   "window_clip_exclusive:%d, "
                   "vsh_mode:%u, transform_start:%u, "
                   "final0:0x%08x, final1:0x%08x, "
                   "fog_enable:%d, fog_mode:%u, foggen:%u, "
                   "point_params:{enable:%d,point_sprite:%d,raw_size:%f,"
                   "scaled_size:%f,"
                   "params:[%f,%f,%f,%f,%f,%f,%f,%f]}}, "
                   "fixed_function={lighting:%d,local_eye:%d,"
                   "specular_enable:%d,separate_specular:%d,"
                   "ignore_specular_alpha:%d,material_src={emission:%u,"
                   "ambient:%u,diffuse:%u,specular:%u},"
                   "lights=[%u,%u,%u,%u,%u,%u,%u,%u]}, "
                   "texture_mask=0x%x, texture_modes=[0x%x,0x%x,0x%x,0x%x], "
                   "texture_formats=[0x%x,0x%x,0x%x,0x%x], "
                   "texture_dxgi=[%u,%u,%u,%u], "
                   "texture_size=[%ux%ux%u,%ux%ux%u,%ux%ux%u,%ux%ux%u], "
                   "texture_pitch=[%u,%u,%u,%u], "
                   "texture_bpp=[%u,%u,%u,%u], "
                   "texture_linear_mask=0x%x, texture_cubemap_mask=0x%x, "
                   "texture_3d_mask=0x%x, texture_border_adjust_mask=0x%x, "
                   "texture_depth_mask=0x%x, texture_snorm_mask=0x%x, "
                   "texture_surface_to_texture_mask=0x%x, "
                   "texture_coord_mask=0x%x, "
                   "texture_colorkey=[%u,%u,%u,%u], "
                   "texture_convolution=[%s(%u),%s(%u),%s(%u),%s(%u)], "
                   "rgb_inputs=[0x%08x,0x%08x,0x%08x,0x%08x,"
                   "0x%08x,0x%08x,0x%08x,0x%08x], "
                   "rgb_outputs=[0x%08x,0x%08x,0x%08x,0x%08x,"
                   "0x%08x,0x%08x,0x%08x,0x%08x], "
                   "alpha_inputs=[0x%08x,0x%08x,0x%08x,0x%08x,"
                   "0x%08x,0x%08x,0x%08x,0x%08x], "
                   "alpha_outputs=[0x%08x,0x%08x,0x%08x,0x%08x,"
                   "0x%08x,0x%08x,0x%08x,0x%08x], "
                   "uniform_attrs=0x%04x, "
                   "swizzle_attrs=0x%04x, compressed_attrs=0x%04x, "
                   "attributes=[%s]",
                   (unsigned long)hr,
                   primitive_topology_type_name(
                       key->primitive_topology_type),
                   (unsigned int)key->rtv_format,
                   (unsigned int)key->dsv_format, key->color_write_mask,
                   key->blend_enable, key->logic_op_enable, key->logic_op,
                   key->src_blend, key->dest_blend, key->blend_op,
                   key->src_blend_alpha,
                   key->dest_blend_alpha, key->blend_op_alpha,
                   key->blend_constant_mode,
                   key->shader_src_blend_factor_enable,
                   blend_factor_name(key->shader_src_blend_factor),
                   key->shader_src_blend_factor,
                   key->shader_framebuffer_fetch,
                   key->shader_blend_enable,
                   blend_factor_name(key->shader_blend_src_factor),
                   key->shader_blend_src_factor,
                   blend_factor_name(key->shader_blend_dest_factor),
                   key->shader_blend_dest_factor,
                   blend_equation_name(key->shader_blend_equation),
                   key->shader_blend_equation, key->shader_logic_op_enable,
                   logic_op_name(key->shader_logic_op),
                   key->shader_logic_op,
                   key->alpha_test_enable,
                   key->cull_mode, key->front_counter_clockwise,
                   key->depth_enable, key->depth_write, key->depth_func,
                   depth_format_name(key->depth_format), key->depth_format,
                   key->z_perspective, key->depth_clipping,
                   key->stencil_enable, key->stencil_func,
                   key->stencil_read_mask, key->stencil_write_mask,
                   key->stencil_fail_op, key->stencil_depth_fail_op,
                   key->stencil_pass_op,
                   key->combiner_control, key->shader_stage_program,
                   key->other_stage_input, key->shader_clip_mode,
                   key->window_clip_exclusive, key->vertex_shader_mode,
                   key->transform_program_start,
                   key->final_inputs_0, key->final_inputs_1,
                   key->fog_enable, key->fog_mode,
                   key->fixed_function_foggen, key->point_params_enable,
                   key->point_sprite_enable,
                   pgraph_reg_r(pg, NV_PGRAPH_POINTSIZE) / 8.0f,
                   MAX(pgraph_reg_r(pg, NV_PGRAPH_POINTSIZE) / 8.0f,
                       1.0f) * MAX(pg->surface_scale_factor, 1),
                   pg->point_params[0], pg->point_params[1],
                   pg->point_params[2], pg->point_params[3],
                   pg->point_params[4], pg->point_params[5],
                   pg->point_params[6], pg->point_params[7],
                   key->fixed_function_lighting,
                   key->fixed_function_local_eye, key->specular_enable,
                   key->separate_specular, key->ignore_specular_alpha,
                   key->fixed_function_emission_src,
                   key->fixed_function_ambient_src,
                   key->fixed_function_diffuse_src,
                   key->fixed_function_specular_src,
                   key->fixed_function_light[0],
                   key->fixed_function_light[1],
                   key->fixed_function_light[2],
                   key->fixed_function_light[3],
                   key->fixed_function_light[4],
                   key->fixed_function_light[5],
                   key->fixed_function_light[6],
                   key->fixed_function_light[7],
                   key->texture_mask,
                   texture_stage_mode(key, 0), texture_stage_mode(key, 1),
                   texture_stage_mode(key, 2), texture_stage_mode(key, 3),
                   key->texture_color_format[0],
                   key->texture_color_format[1],
                   key->texture_color_format[2],
                   key->texture_color_format[3],
                   (unsigned int)key->texture_dxgi_format[0],
                   (unsigned int)key->texture_dxgi_format[1],
                   (unsigned int)key->texture_dxgi_format[2],
                   (unsigned int)key->texture_dxgi_format[3],
                   key->texture_width[0], key->texture_height[0],
                   key->texture_depth[0],
                   key->texture_width[1], key->texture_height[1],
                   key->texture_depth[1],
                   key->texture_width[2], key->texture_height[2],
                   key->texture_depth[2],
                   key->texture_width[3], key->texture_height[3],
                   key->texture_depth[3],
                   key->texture_pitch[0], key->texture_pitch[1],
                   key->texture_pitch[2], key->texture_pitch[3],
                   key->texture_bytes_per_pixel[0],
                   key->texture_bytes_per_pixel[1],
                   key->texture_bytes_per_pixel[2],
                   key->texture_bytes_per_pixel[3],
                   key->texture_linear_mask, key->texture_cubemap_mask,
                   key->texture_3d_mask, key->texture_border_adjust_mask,
                   key->texture_depth_mask,
                   key->texture_snorm_mask,
                   key->texture_surface_to_texture_mask,
                   key->texture_coord_mask,
                   key->texture_colorkey_mode[0],
                   key->texture_colorkey_mode[1],
                   key->texture_colorkey_mode[2],
                   key->texture_colorkey_mode[3],
                   texture_convolution_filter_name(
                       key->texture_convolution_filter[0]),
                   key->texture_convolution_filter[0],
                   texture_convolution_filter_name(
                       key->texture_convolution_filter[1]),
                   key->texture_convolution_filter[1],
                   texture_convolution_filter_name(
                       key->texture_convolution_filter[2]),
                   key->texture_convolution_filter[2],
                   texture_convolution_filter_name(
                       key->texture_convolution_filter[3]),
                   key->texture_convolution_filter[3],
                   key->rgb_inputs[0], key->rgb_inputs[1],
                   key->rgb_inputs[2], key->rgb_inputs[3],
                   key->rgb_inputs[4], key->rgb_inputs[5],
                   key->rgb_inputs[6], key->rgb_inputs[7],
                   key->rgb_outputs[0], key->rgb_outputs[1],
                   key->rgb_outputs[2], key->rgb_outputs[3],
                   key->rgb_outputs[4], key->rgb_outputs[5],
                   key->rgb_outputs[6], key->rgb_outputs[7],
                   key->alpha_inputs[0], key->alpha_inputs[1],
                   key->alpha_inputs[2], key->alpha_inputs[3],
                   key->alpha_inputs[4], key->alpha_inputs[5],
                   key->alpha_inputs[6], key->alpha_inputs[7],
                   key->alpha_outputs[0], key->alpha_outputs[1],
                   key->alpha_outputs[2], key->alpha_outputs[3],
                   key->alpha_outputs[4], key->alpha_outputs[5],
                   key->alpha_outputs[6], key->alpha_outputs[7],
                   key->uniform_attrs, key->swizzle_attrs,
                   key->compressed_attrs, attrs);
        g_free(attrs);
        return false;
    }

    if (info != NULL) {
        info->dxil = dxil || !pgraph_d3d12_use_fxc();
        info->dxil_fallback = dxil_requested && !dxil;
        info->driver_us = driver_us;
        info->twin_us = twin_us;
    }
    *pso_out = pipeline_state;
    return true;
}

static bool __attribute__((noinline)) create_minimal_draw_pso(
    PGRAPHState *pg, const PGRAPHD3D12PipelineKey *key,
    const D3D12_INPUT_ELEMENT_DESC *input_descs,
    ID3D12PipelineState **pso_out, DrawPsoBuildInfo *info, Error **errp)
{
    PGRAPHD3D12PipelineKey stable_key;
    char *vs_source = NULL;
    char *gs_source = NULL;
    char *ps_source = NULL;
    bool uber_ps;

    if (info != NULL) {
        info->dxil = false;
        info->dxil_fallback = false;
        info->driver_us = 0;
        info->twin_us = 0;
    }
    if (key == NULL) {
        error_setg(errp, "Direct3D 12 PSO creation received NULL key");
        return false;
    }
    stable_key = *key;
    key = &stable_key;
    uber_ps = key->tune_flags & PGRAPH_D3D12_TUNE_UBER_PS;

    if (uber_ps) {
        /* fallback=ps: the specialised vertex and geometry shaders with
         * every texture coordinate, and the interpreter pixel shader. */
        PGRAPHD3D12PipelineKey vs_key;

        uber_ps_vertex_key(key, &vs_key);
        vs_source = build_minimal_vertex_shader(&vs_key);
        gs_source = build_draw_geometry_shader(&vs_key);
        ps_source = build_uber_pixel_shader(key);
    } else {
        vs_source = build_minimal_vertex_shader(key);
        gs_source = build_draw_geometry_shader(key);
        ps_source = build_minimal_pixel_shader(key);
    }
    if (vs_source == NULL || ps_source == NULL) {
        error_setg(errp,
                   "Direct3D 12 SM 6.4 shader source generation failed: "
                   "vs_source=%p ps_source=%p",
                   vs_source, ps_source);
        g_free(vs_source);
        g_free(gs_source);
        g_free(ps_source);
        return false;
    }
    return create_draw_pso_from_sources(pg, key, input_descs, vs_source,
                                        gs_source, ps_source, uber_ps,
                                        pso_out, info, errp);
}

/*
 * Pipeline lists: every state a title needed a PSO for, precompiled when
 * the title starts again. Each title (0 when unknown) has its own
 * <cache>/<title id>/pipelines.bin of keys; PSO blobs are never stored.
 * The key version must change whenever the key's fields change. Records
 * are canonicalized on load, so a canonical form that only clears or merges
 * fields keeps old lists.
 */
static guint fallback_key_hash(gconstpointer data);
static gboolean fallback_key_equal(gconstpointer a, gconstpointer b);
static bool fill_fallback_key(const PGRAPHD3D12PipelineKey *key,
                              PGRAPHD3D12FallbackKey *fb);
static void queue_urgent_pso_job(PGRAPHD3D12State *r, PGRAPHD3D12PsoJob *job);
static bool fallback_mode_draws(PGRAPHD3D12FallbackMode mode);

#define PIPELINE_LIST_MAGIC 0x4b535058u         /* "XPSK" */
#define PIPELINE_LIST_KEY_VERSION 5
#define PIPELINE_LIST_FLAG_VERTEX_PULLING (1u << 0)
/* Records kept per title, and pipelines precompiled per title. */
#define PIPELINE_LIST_MAX 8192
#define PIPELINE_PRECOMPILE_MAX 4096
/* Recorded keys reach the file at most this often, and at title switch. */
#define PIPELINE_LIST_FLUSH_US (3 * G_USEC_PER_SEC)

typedef struct PipelineListHeader {
    uint32_t magic;
    uint32_t key_version;
    uint32_t key_size;
    uint32_t title_id;
    uint32_t flags;
    uint32_t reserved[3];
} PipelineListHeader;

#define PIPELINE_LIST_RECORD_SIZE \
    (sizeof(PGRAPHD3D12PipelineKey) + sizeof(uint64_t))

/*
 * A title's list. A worker opens and reads the file, then hands it to the
 * FIFO thread under pso_mutex (loaded). If the FIFO thread has moved to
 * another title by then (abandoned), the worker writes the records
 * recorded meanwhile and frees the list itself.
 */
struct PGRAPHD3D12PipelineList {
    uint32_t title_id;
    uint32_t run;
    bool precompile;
    unsigned int scale;
    uint32_t tune_flags;
    /* fallback=ps: keys the interpreter pixel shader can draw precompile
     * with it. */
    bool uber_ps;
    /* fallback=all/debug: the interpreter pipelines of the recorded keys
     * build first. */
    bool fallback;
    bool loaded;                /* pso_mutex */
    bool abandoned;             /* pso_mutex */
    /* The loader's until loaded, then the FIFO thread's. */
    FILE *file;
    GHashTable *file_keys;      /* hashes of the canonical keys in file */
    unsigned int records;
    /* The FIFO thread's until abandoned, then the loader's. */
    GByteArray *pending;
    bool adopted;
};

static uint64_t pipeline_key_hash64(const PGRAPHD3D12PipelineKey *key)
{
    return fast_hash((const uint8_t *)key, sizeof(*key));
}

static GHashTable *new_hash_set(void)
{
    return g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
}

/* Returns true if hash was not in set yet. */
static bool hash_set_add(GHashTable *set, uint64_t hash)
{
    if (g_hash_table_contains(set, &hash)) {
        return false;
    }
    g_hash_table_add(set, g_memdup2(&hash, sizeof(hash)));
    return true;
}

static uint64_t pipeline_list_check(const PGRAPHD3D12PipelineKey *key)
{
    return fnv1a64_hash((const uint8_t *)key, sizeof(*key),
                        1469598103934665603ull);
}

/*
 * Adapts a recorded key to the current surface scale and shader tune bits;
 * records never carry the key-only bits, which the run's bits supply.
 * Returns false when the key cannot match a draw now.
 */
static bool adapt_recorded_pipeline_key(PGRAPHD3D12PipelineKey *key,
                                        unsigned int scale,
                                        uint32_t tune_flags)
{
    uint8_t line_expand_width = 1;

    if ((key->tune_flags ^ tune_flags) & ~PGRAPH_D3D12_TUNE_KEY_ONLY_FLAGS) {
        return false;
    }
    key->tune_flags = tune_flags;
    /* As fill_pipeline_key(); a topology of lines with a fill polygon mode
     * comes from line primitives. */
    if (scale > 1 &&
        (key->polygon_mode == POLY_MODE_LINE ||
         (!polygon_primitive(key->primitive_mode) &&
          key->primitive_topology_type ==
              D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE))) {
        line_expand_width = 2;
    }
    if (key->line_expand_width == line_expand_width) {
        return true;
    }
    /* Narrow smooth lines are recorded without their primitive mode, which
     * the widened draw's geometry shader key carries. */
    if (line_expand_width > 1 && key->primitive_mode == 0 &&
        !(key->primitive_topology_type ==
              D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE &&
          key->polygon_mode != POLY_MODE_FILL)) {
        return false;
    }
    key->line_expand_width = line_expand_width;
    canonicalize_pipeline_key(key);
    return true;
}

static void free_pipeline_list(PGRAPHD3D12PipelineList *list)
{
    if (list->file != NULL) {
        fclose(list->file);
    }
    if (list->file_keys != NULL) {
        g_hash_table_destroy(list->file_keys);
    }
    g_byte_array_free(list->pending, TRUE);
    g_free(list);
}

/* Drops pending records already in the file and those past the cap. */
static void filter_pending_records(PGRAPHD3D12PipelineList *list)
{
    size_t out = 0;

    for (size_t in = 0; in + PIPELINE_LIST_RECORD_SIZE <= list->pending->len;
         in += PIPELINE_LIST_RECORD_SIZE) {
        PGRAPHD3D12PipelineKey key;

        memcpy(&key, list->pending->data + in, sizeof(key));
        if (list->records >= PIPELINE_LIST_MAX ||
            !hash_set_add(list->file_keys, pipeline_key_hash64(&key))) {
            continue;
        }
        memmove(list->pending->data + out, list->pending->data + in,
                PIPELINE_LIST_RECORD_SIZE);
        out += PIPELINE_LIST_RECORD_SIZE;
        list->records++;
    }
    g_byte_array_set_size(list->pending, out);
}

static void write_pending_records(PGRAPHD3D12PipelineList *list)
{
    if (list->pending->len == 0) {
        return;
    }
    if (list->file != NULL && g_config.perf.cache_shaders &&
        (fwrite(list->pending->data, list->pending->len, 1,
                list->file) != 1 ||
         fflush(list->file) != 0)) {
        fclose(list->file);
        list->file = NULL;
    }
    g_byte_array_set_size(list->pending, 0);
}

static void publish_precompile_status(PGRAPHD3D12State *r,
                                      NV2AShaderPrecompileState state)
{
    PGRAPHState *pg = r->pg;

    qatomic_set(&pg->shader_cache.state, state);
    qatomic_set(&pg->shader_cache.total, r->pso_run_total);
    qatomic_set(&pg->shader_cache.done, r->pso_run_done);
    qatomic_set(&pg->shader_cache.failed, r->pso_run_failed);
    qatomic_set(&pg->shader_cache.recorded, r->pso_run_recorded);
    qatomic_store_release(&pg->shader_cache.status_gen, r->pso_run_request);
}

/* Ends the current precompile run, finished or not; pso_mutex held. */
static void end_precompile_run(PGRAPHD3D12State *r)
{
    if (!r->pso_run_active) {
        return;
    }
    r->pso_run_active = false;
    fprintf(stderr,
            "Direct3D 12 precompile: title=%08x keys=%u built=%u failed=%u "
            "workers=%u time=%ums drv=%ums dxil=%u dxilfb=%u\n",
            r->pso_run_title, r->pso_run_total, r->pso_run_done,
            r->pso_run_failed, r->pso_run_workers,
            (unsigned int)((g_get_monotonic_time() - r->pso_run_start_us) /
                           1000),
            (unsigned int)(r->pso_run_driver_us / 1000), r->pso_run_dxil,
            r->pso_run_dxil_fallbacks);
}

/* Counts a finished job of precompile run; builds draws queued belong to
 * none. pso_mutex held. */
static void precompile_job_finished(PGRAPHD3D12State *r, uint32_t run,
                                    bool built)
{
    if (run == PGRAPH_D3D12_PSO_RUN_NONE || !r->pso_run_active ||
        run != r->pso_run) {
        return;
    }
    if (built) {
        r->pso_run_done++;
    } else {
        r->pso_run_failed++;
    }
    if (r->pso_run_done + r->pso_run_failed >= r->pso_run_total) {
        end_precompile_run(r);
        publish_precompile_status(r, NV2A_SHADER_PRECOMPILE_DONE);
    } else {
        publish_precompile_status(r, NV2A_SHADER_PRECOMPILE_COMPILING);
    }
}

/*
 * Key version 4 lists lack texture_signs[]; their keys are the version 5
 * keys with those bytes left out, so they are rewritten with no signed
 * channels instead of being dropped (players keep their recorded pipelines).
 */
static void upgrade_pipeline_list_v4(const char *path,
                                     const PipelineListHeader *expected)
{
    const size_t gap = offsetof(PGRAPHD3D12PipelineKey, texture_signs);
    const size_t gap_size = sizeof(((PGRAPHD3D12PipelineKey *)0)->texture_signs);
    const size_t old_size = sizeof(PGRAPHD3D12PipelineKey) - gap_size;
    PipelineListHeader header;
    g_autofree uint8_t *old_key = g_malloc(old_size);
    g_autofree char *tmp_path = g_strdup_printf("%s.v5", path);
    FILE *in = g_fopen(path, "rb");
    FILE *out = NULL;
    unsigned int records = 0;

    if (in == NULL) {
        return;
    }
    if (fread(&header, sizeof(header), 1, in) != 1 ||
        header.magic != expected->magic || header.key_version != 4 ||
        header.key_size != old_size || header.title_id != expected->title_id ||
        header.flags != expected->flags) {
        fclose(in);
        return;
    }
    out = g_fopen(tmp_path, "wb");
    if (out == NULL || fwrite(expected, sizeof(*expected), 1, out) != 1) {
        goto fail;
    }
    while (records < PIPELINE_LIST_MAX) {
        PGRAPHD3D12PipelineKey key;
        uint64_t check;

        if (fread(old_key, old_size, 1, in) != 1 ||
            fread(&check, sizeof(check), 1, in) != 1 ||
            check != fnv1a64_hash(old_key, old_size, 1469598103934665603ull)) {
            break;
        }
        memset(&key, 0, sizeof(key));
        memcpy(&key, old_key, gap);
        memcpy((uint8_t *)&key + gap + gap_size, old_key + gap,
               old_size - gap);
        check = pipeline_list_check(&key);
        if (fwrite(&key, sizeof(key), 1, out) != 1 ||
            fwrite(&check, sizeof(check), 1, out) != 1) {
            goto fail;
        }
        records++;
    }
    fclose(in);
    in = NULL;
    if (fclose(out) != 0) {
        out = NULL;
        goto fail;
    }
    out = NULL;
    if (g_remove(path) != 0 || g_rename(tmp_path, path) != 0) {
        goto fail;
    }
    uwp_log("New graphics: pipeline list %08x kept from the older format "
            "(%u pipelines)", expected->title_id, records);
    return;

fail:
    if (in != NULL) {
        fclose(in);
    }
    if (out != NULL) {
        fclose(out);
    }
    g_remove(tmp_path);
}

/*
 * Reads a title's list on a worker: queues its keys for precompile and
 * leaves the file open for appending, recreating it when it is stale and
 * cutting off a damaged tail.
 */
static void load_pipeline_list(PGRAPHD3D12State *r,
                               PGRAPHD3D12PipelineList *list)
{
    const PipelineListHeader expected = {
        .magic = PIPELINE_LIST_MAGIC,
        .key_version = PIPELINE_LIST_KEY_VERSION,
        .key_size = sizeof(PGRAPHD3D12PipelineKey),
        .title_id = list->title_id,
        .flags = PIPELINE_LIST_FLAG_VERTEX_PULLING,
    };
    g_autofree char *dir = g_strdup_printf("%s/%08x", r->shader_cache_dir,
                                           list->title_id);
    g_autofree char *path = g_strdup_printf("%s/pipelines.bin", dir);
    QSIMPLEQ_HEAD(, PGRAPHD3D12PsoJob) jobs;
    QSIMPLEQ_HEAD(, PGRAPHD3D12PsoJob) fallback_jobs;
    GHashTable *keys = new_hash_set();
    GHashTable *unique;
    GHashTable *fallback_unique;
    unsigned int fallback_queued = 0;
    PipelineListHeader header;
    PGRAPHD3D12PsoJob *job;
    unsigned int records = 0;
    unsigned int candidates = 0;
    unsigned int queued = 0;
    bool current = false;
    FILE *file;

    QSIMPLEQ_INIT(&jobs);
    QSIMPLEQ_INIT(&fallback_jobs);
    unique = g_hash_table_new(pipeline_key_hash, pipeline_key_equal);
    fallback_unique = g_hash_table_new(fallback_key_hash, fallback_key_equal);
    g_mkdir_with_parents(dir, 0755);
    upgrade_pipeline_list_v4(path, &expected);
    file = g_fopen(path, "r+b");
    if (file != NULL &&
        (fread(&header, sizeof(header), 1, file) != 1 ||
         memcmp(&header, &expected, sizeof(header)) != 0)) {
        fclose(file);
        file = NULL;
    }
    if (file != NULL) {
        PGRAPHD3D12PipelineKey key;
        uint64_t check;
        long valid_end;

        while (records < PIPELINE_LIST_MAX &&
               fread(&key, sizeof(key), 1, file) == 1 &&
               fread(&check, sizeof(check), 1, file) == 1 &&
               check == pipeline_list_check(&key)) {
            records++;
            /* Keys recorded under an older canonical form collapse. */
            canonicalize_pipeline_key(&key);
            hash_set_add(keys, pipeline_key_hash64(&key));
            if (!list->precompile || candidates >= PIPELINE_PRECOMPILE_MAX ||
                !adapt_recorded_pipeline_key(&key, list->scale,
                                             list->tune_flags)) {
                continue;
            }
            if (list->uber_ps) {
                mark_uber_ps_key(&key);
            }
            if (g_hash_table_contains(unique, &key)) {
                continue;
            }
            if (list->fallback) {
                PGRAPHD3D12FallbackKey fb;

                if (fill_fallback_key(&key, &fb) &&
                    !key.shader_framebuffer_fetch &&
                    !g_hash_table_contains(fallback_unique, &fb)) {
                    job = g_new0(PGRAPHD3D12PsoJob, 1);
                    job->kind = PGRAPH_D3D12_PSO_JOB_FALLBACK;
                    job->run = PGRAPH_D3D12_PSO_RUN_NONE;
                    job->fallback_key = fb;
                    g_hash_table_add(fallback_unique, &job->fallback_key);
                    QSIMPLEQ_INSERT_TAIL(&fallback_jobs, job, entry);
                }
            }
            job = g_new0(PGRAPHD3D12PsoJob, 1);
            job->kind = PGRAPH_D3D12_PSO_JOB_BUILD;
            job->run = list->run;
            job->key = key;
            g_hash_table_add(unique, &job->key);
            QSIMPLEQ_INSERT_TAIL(&jobs, job, entry);
            candidates++;
        }
        valid_end = sizeof(header) + records * PIPELINE_LIST_RECORD_SIZE;
        if (fseek(file, 0, SEEK_END) != 0 ||
            (ftell(file) != valid_end &&
             _chsize_s(_fileno(file), valid_end) != 0) ||
            fseek(file, valid_end, SEEK_SET) != 0) {
            fclose(file);
            file = NULL;
        }
    }
    g_hash_table_destroy(unique);
    g_hash_table_destroy(fallback_unique);
    if (file == NULL) {
        g_hash_table_remove_all(keys);
        records = 0;
        file = g_fopen(path, "w+b");
        if (file != NULL &&
            (fwrite(&expected, sizeof(expected), 1, file) != 1 ||
             fflush(file) != 0)) {
            fclose(file);
            file = NULL;
        }
    }

    qemu_mutex_lock(&r->pso_mutex);
    list->file = file;
    list->file_keys = keys;
    list->records = records;
    if (list->abandoned) {
        qemu_mutex_unlock(&r->pso_mutex);
        filter_pending_records(list);
        write_pending_records(list);
        free_pipeline_list(list);
    } else {
        list->loaded = true;
        current = list->run == r->pso_run;
        if (current) {
            r->pso_run_recorded = records;
        }
        if (list->precompile && current) {
            uint64_t building[PGRAPH_D3D12_MAX_PSO_WORKERS];
            unsigned int num_building = 0;

            /* Keys workers are building are neither queued nor built. */
            for (unsigned int i = 0; i < r->pso_num_workers; i++) {
                PGRAPHD3D12PsoJob *inflight = r->pso_workers[i].inflight;

                if (inflight != NULL &&
                    inflight->kind == PGRAPH_D3D12_PSO_JOB_BUILD) {
                    building[num_building++] =
                        pipeline_key_hash64(&inflight->key);
                }
            }
            /* Interpreter pipelines first, ahead of the precompile. */
            while ((job = QSIMPLEQ_FIRST(&fallback_jobs)) != NULL) {
                bool skip = g_hash_table_contains(r->fallback_queued,
                                                  &job->fallback_key) ||
                            g_hash_table_contains(r->fallback_done,
                                                  &job->fallback_key);

                for (unsigned int i = 0; i < r->pso_num_workers && !skip;
                     i++) {
                    PGRAPHD3D12PsoJob *inflight = r->pso_workers[i].inflight;

                    skip = inflight != NULL &&
                           inflight->kind == PGRAPH_D3D12_PSO_JOB_FALLBACK &&
                           fallback_key_equal(&inflight->fallback_key,
                                              &job->fallback_key);
                }
                QSIMPLEQ_REMOVE_HEAD(&fallback_jobs, entry);
                if (skip) {
                    g_free(job);
                    continue;
                }
                g_hash_table_insert(r->fallback_queued, &job->fallback_key,
                                    job);
                queue_urgent_pso_job(r, job);
                fallback_queued++;
            }
            while ((job = QSIMPLEQ_FIRST(&jobs)) != NULL) {
                uint64_t hash = pipeline_key_hash64(&job->key);
                bool skip = g_hash_table_contains(r->pso_built, &hash) ||
                            g_hash_table_contains(r->pso_queued, &job->key);

                for (unsigned int i = 0; i < num_building && !skip; i++) {
                    skip = building[i] == hash;
                }
                QSIMPLEQ_REMOVE_HEAD(&jobs, entry);
                if (skip) {
                    g_free(job);
                    continue;
                }
                QSIMPLEQ_INSERT_TAIL(&r->pso_jobs, job, entry);
                g_hash_table_insert(r->pso_queued, &job->key, job);
                queued++;
            }
            r->pso_run_total = queued;
            r->pso_run_active = queued != 0;
            publish_precompile_status(r, queued != 0 ?
                                             NV2A_SHADER_PRECOMPILE_COMPILING :
                                             NV2A_SHADER_PRECOMPILE_DONE);
            qemu_cond_broadcast(&r->pso_cond);
        }
        qemu_mutex_unlock(&r->pso_mutex);
    }
    while ((job = QSIMPLEQ_FIRST(&jobs)) != NULL) {
        QSIMPLEQ_REMOVE_HEAD(&jobs, entry);
        g_free(job);
    }
    while ((job = QSIMPLEQ_FIRST(&fallback_jobs)) != NULL) {
        QSIMPLEQ_REMOVE_HEAD(&fallback_jobs, entry);
        g_free(job);
    }
    if (current) {
        fprintf(stderr,
                "Direct3D 12 renderer: precompiling %u of %u recorded "
                "pipelines for title %08x, %u interpreter pipelines\n",
                queued, records, expected.title_id, fallback_queued);
    }
}

/* A worker applies the mode's priority to itself; pso_mutex held. */
static void update_worker_priority(PGRAPHD3D12PsoWorker *w)
{
    int priority = w->r->pso_worker_priority;

    if (w->boosted) {
        priority = MAX(priority, THREAD_PRIORITY_NORMAL);
    }
    if (w->priority != priority) {
        SetThreadPriority(GetCurrentThread(), priority);
        w->priority = priority;
    }
}

/*
 * The draw root signature, 20 DWORDs: the per-stage SRV and sampler tables,
 * the framebuffer-fetch table, the b0 and b1 CBVs, the vertex RAM and
 * scratch SRVs, the b2 start vertex and the b3 interpreter CBV. Each SRV
 * table has PGRAPH_D3D12_TEXTURE_SRV_SPACES ranges, all at offset 0 so the
 * interpreter reads the stage's one descriptor at its dimension; the
 * specialised shaders declare space 0 only. Disjoint ranges (offsets 0-3)
 * are for the uber test. desc points into the struct's own arrays.
 */
typedef struct DrawRootSignatureDesc {
    D3D12_DESCRIPTOR_RANGE srv_ranges[NV2A_MAX_TEXTURES]
                                     [PGRAPH_D3D12_TEXTURE_SRV_SPACES];
    D3D12_DESCRIPTOR_RANGE sampler_ranges[NV2A_MAX_TEXTURES];
    D3D12_DESCRIPTOR_RANGE framebuffer_range;
    D3D12_ROOT_PARAMETER parameters[PGRAPH_D3D12_ROOT_PARAMETER_COUNT];
    D3D12_ROOT_SIGNATURE_DESC desc;
} DrawRootSignatureDesc;

static void fill_draw_root_signature_desc(DrawRootSignatureDesc *rs,
                                          bool overlap)
{
    D3D12_ROOT_PARAMETER *p;

    memset(rs, 0, sizeof(*rs));
    for (unsigned int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        for (unsigned int space = 0; space < PGRAPH_D3D12_TEXTURE_SRV_SPACES;
             space++) {
            D3D12_DESCRIPTOR_RANGE *range = &rs->srv_ranges[i][space];

            range->RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            range->NumDescriptors = 1;
            range->BaseShaderRegister = i;
            range->RegisterSpace = space;
            range->OffsetInDescriptorsFromTableStart = overlap ? 0 : space;
        }
        p = &rs->parameters[PGRAPH_D3D12_ROOT_TEXTURE_SRV(i)];
        p->ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p->DescriptorTable.NumDescriptorRanges =
            PGRAPH_D3D12_TEXTURE_SRV_SPACES;
        p->DescriptorTable.pDescriptorRanges = rs->srv_ranges[i];
        p->ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        rs->sampler_ranges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
        rs->sampler_ranges[i].NumDescriptors = 1;
        rs->sampler_ranges[i].BaseShaderRegister = i;
        rs->sampler_ranges[i].OffsetInDescriptorsFromTableStart =
            D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        p = &rs->parameters[PGRAPH_D3D12_ROOT_TEXTURE_SAMPLER(i)];
        p->ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p->DescriptorTable.NumDescriptorRanges = 1;
        p->DescriptorTable.pDescriptorRanges = &rs->sampler_ranges[i];
        p->ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    rs->framebuffer_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    rs->framebuffer_range.NumDescriptors = 1;
    rs->framebuffer_range.BaseShaderRegister =
        PGRAPH_D3D12_FRAMEBUFFER_SRV_REGISTER;
    rs->framebuffer_range.OffsetInDescriptorsFromTableStart =
        D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    p = &rs->parameters[PGRAPH_D3D12_ROOT_FRAMEBUFFER_SRV];
    p->ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p->DescriptorTable.NumDescriptorRanges = 1;
    p->DescriptorTable.pDescriptorRanges = &rs->framebuffer_range;
    p->ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    p = &rs->parameters[PGRAPH_D3D12_ROOT_DRAW_CONSTANTS];
    p->ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    p->Descriptor.ShaderRegister = 0;
    p->ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    p = &rs->parameters[PGRAPH_D3D12_ROOT_PIXEL_CONSTANTS];
    p->ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    p->Descriptor.ShaderRegister = 1;
    p->ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    p = &rs->parameters[PGRAPH_D3D12_ROOT_VERTEX_RAM_SRV];
    p->ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    p->Descriptor.ShaderRegister = PGRAPH_D3D12_VERTEX_RAM_SRV_REGISTER;
    p->ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    p = &rs->parameters[PGRAPH_D3D12_ROOT_VERTEX_SCRATCH_SRV];
    p->ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    p->Descriptor.ShaderRegister = PGRAPH_D3D12_VERTEX_SCRATCH_SRV_REGISTER;
    p->ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    p = &rs->parameters[PGRAPH_D3D12_ROOT_VERTEX_START];
    p->ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p->Constants.ShaderRegister = 2;
    p->Constants.Num32BitValues = 1;
    p->ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    p = &rs->parameters[PGRAPH_D3D12_ROOT_UBER_CONSTANTS];
    p->ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    p->Descriptor.ShaderRegister = 3;
    p->ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    QEMU_BUILD_BUG_ON(PGRAPH_D3D12_ROOT_UBER_CONSTANTS + 1 !=
                      PGRAPH_D3D12_ROOT_PARAMETER_COUNT);

    rs->desc.NumParameters = ARRAY_SIZE(rs->parameters);
    rs->desc.pParameters = rs->parameters;
    rs->desc.Flags =
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
}

/*
 * Measurement path (tune "ubertest=<id>"): times DXC and FXC on the
 * interpreter shaders of ubertest.c, then pipeline creation from them: a
 * first pipeline, variants of the same bytecode with the depth function,
 * blend, render-target format or cull mode changed, an identical repeat,
 * and last one with a root signature whose per-stage SRV tables put the
 * Texture2D, TextureCube, Texture2DArray and Texture3D ranges at offset 0
 * in spaces 0-3. Runs once per id on a PSO worker. Every object is
 * released after creation and none is ever bound.
 */
#define UBER_TEST_LOG "Direct3D 12 ubertest id=%s "

static double uber_test_ms(int64_t start)
{
    return (g_get_monotonic_time() - start) / 1000.0;
}

static unsigned long uber_test_removed(PGRAPHD3D12State *r)
{
    return (unsigned long)r->device->lpVtbl->GetDeviceRemovedReason(
        r->device);
}

/* The draw root signature with its SRV ranges all at offset 0 (overlap,
 * as drawn with) or at offsets 0-3. */
static ID3D12RootSignature *create_uber_test_root_signature(
    PGRAPHD3D12State *r, const char *id, bool overlap)
{
    DrawRootSignatureDesc rs;
    ID3D12RootSignature *root_signature = NULL;
    ID3DBlob *signature = NULL;
    ID3DBlob *error_blob = NULL;
    int64_t start = g_get_monotonic_time();
    HRESULT serialize_hr;
    HRESULT hr = E_FAIL;

    fill_draw_root_signature_desc(&rs, overlap);
    serialize_hr = D3D12SerializeRootSignature(
        &rs.desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error_blob);
    if (SUCCEEDED(serialize_hr)) {
        hr = r->device->lpVtbl->CreateRootSignature(
            r->device, 0, signature->lpVtbl->GetBufferPointer(signature),
            signature->lpVtbl->GetBufferSize(signature),
            &IID_ID3D12RootSignature, (void **)&root_signature);
    }
    fprintf(stderr,
            UBER_TEST_LOG "step=rootsig_%s serialize=0x%08lx hr=0x%08lx "
            "ms=%.1f removed=0x%08lx%s%.*s\n",
            id, overlap ? "overlap" : "disjoint",
            (unsigned long)serialize_hr, (unsigned long)hr,
            uber_test_ms(start), uber_test_removed(r),
            error_blob != NULL ? " error=" : "",
            error_blob != NULL ?
                (int)MIN(error_blob->lpVtbl->GetBufferSize(error_blob),
                         (SIZE_T)300) : 0,
            error_blob != NULL ?
                (const char *)error_blob->lpVtbl->GetBufferPointer(
                    error_blob) : "");
    D3D12_RELEASE(signature);
    D3D12_RELEASE(error_blob);
    if (FAILED(hr)) {
        D3D12_RELEASE(root_signature);
    }
    return root_signature;
}

/* One interpreter stage from DXC (dxil) or FXC; logs the step. */
static IDxcBlob *compile_uber_test_stage(PGRAPHState *pg,
                                         IDxcCompiler3 *compiler,
                                         const char *id, const char *source,
                                         bool dxil, bool vertex)
{
    IDxcBlob *blob = NULL;
    Error *err = NULL;
    int64_t start = g_get_monotonic_time();
    bool ok;

    if (dxil) {
        ok = compiler != NULL &&
             compile_sm64_shader(pg, compiler, source,
                                 vertex ? L"vs_main" : L"ps_main",
                                 vertex ? L"vs_6_4" : L"ps_6_4", &blob,
                                 &err);
    } else {
        ok = compile_fxc_shader(pg, source, vertex ? "vs_main" : "ps_main",
                                vertex ? "vs_5_1" : "ps_5_1", &blob, &err);
    }
    fprintf(stderr,
            UBER_TEST_LOG "step=%s_%s ok=%d ms=%.1f bytes=%u source=%u"
            "%s%.300s\n",
            id, dxil ? "dxc" : "fxc", vertex ? "vs" : "ps", ok,
            uber_test_ms(start),
            ok ? (unsigned int)blob->lpVtbl->GetBufferSize(blob) : 0,
            (unsigned int)strlen(source), ok ? "" : " error=",
            ok ? "" : err != NULL ? error_get_pretty(err) :
                                    "DXC compiler unavailable");
    if (err != NULL) {
        error_free(err);
    }
    if (!ok) {
        D3D12_RELEASE(blob);
    }
    return blob;
}

static void fill_uber_test_desc(D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc,
                                ID3D12RootSignature *root_signature,
                                IDxcBlob *vs, IDxcBlob *ps)
{
    D3D12_RENDER_TARGET_BLEND_DESC *rt = &desc->BlendState.RenderTarget[0];
    D3D12_DEPTH_STENCILOP_DESC stencil_op = {
        .StencilFailOp = D3D12_STENCIL_OP_KEEP,
        .StencilDepthFailOp = D3D12_STENCIL_OP_KEEP,
        .StencilPassOp = D3D12_STENCIL_OP_KEEP,
        .StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS,
    };

    memset(desc, 0, sizeof(*desc));
    desc->pRootSignature = root_signature;
    desc->VS.pShaderBytecode = vs->lpVtbl->GetBufferPointer(vs);
    desc->VS.BytecodeLength = vs->lpVtbl->GetBufferSize(vs);
    desc->PS.pShaderBytecode = ps->lpVtbl->GetBufferPointer(ps);
    desc->PS.BytecodeLength = ps->lpVtbl->GetBufferSize(ps);
    rt->BlendEnable = FALSE;
    rt->LogicOpEnable = FALSE;
    rt->SrcBlend = D3D12_BLEND_ONE;
    rt->DestBlend = D3D12_BLEND_ZERO;
    rt->BlendOp = D3D12_BLEND_OP_ADD;
    rt->SrcBlendAlpha = D3D12_BLEND_ONE;
    rt->DestBlendAlpha = D3D12_BLEND_ZERO;
    rt->BlendOpAlpha = D3D12_BLEND_OP_ADD;
    rt->LogicOp = D3D12_LOGIC_OP_NOOP;
    rt->RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc->SampleMask = UINT_MAX;
    desc->RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc->RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc->RasterizerState.DepthClipEnable = FALSE;
    desc->DepthStencilState.DepthEnable = TRUE;
    desc->DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    desc->DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    desc->DepthStencilState.StencilEnable = FALSE;
    desc->DepthStencilState.StencilReadMask = 0xff;
    desc->DepthStencilState.StencilWriteMask = 0xff;
    desc->DepthStencilState.FrontFace = stencil_op;
    desc->DepthStencilState.BackFace = stencil_op;
    desc->IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
    desc->PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc->NumRenderTargets = 1;
    desc->RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc->DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    desc->SampleDesc.Count = 1;
}

/* Creates, times and releases one pipeline; false once the device is
 * removed. */
static bool create_uber_test_pso(PGRAPHD3D12State *r, const char *id,
                                 const char *format, const char *step,
                                 const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc)
{
    ID3D12PipelineState *pso = NULL;
    unsigned long removed;
    int64_t us = 0;
    HRESULT hr;

    if (desc->pRootSignature == NULL) {
        fprintf(stderr, UBER_TEST_LOG "fmt=%s step=%s skipped: no root "
                "signature\n", id, format, step);
        return true;
    }
    hr = create_timed_pso(r, desc, &pso, &us);
    D3D12_RELEASE(pso);
    removed = uber_test_removed(r);
    fprintf(stderr,
            UBER_TEST_LOG "fmt=%s step=%s hr=0x%08lx ms=%.1f "
            "removed=0x%08lx\n",
            id, format, step, (unsigned long)hr, us / 1000.0, removed);
    return removed == (unsigned long)S_OK;
}

/* The first pipeline, its fixed-state variants and an identical repeat. */
static bool create_uber_test_variants(PGRAPHD3D12State *r, const char *id,
                                      const char *format,
                                      const D3D12_GRAPHICS_PIPELINE_STATE_DESC
                                          *base)
{
    static const char *const steps[] = {
        "pso_base", "pso_depth", "pso_blend", "pso_rtv", "pso_cull",
        "pso_same",
    };

    for (unsigned int i = 0; i < ARRAY_SIZE(steps); i++) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = *base;
        D3D12_RENDER_TARGET_BLEND_DESC *rt = &desc.BlendState.RenderTarget[0];

        switch (i) {
        case 1:
            desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
            break;
        case 2:
            rt->BlendEnable = TRUE;
            rt->SrcBlend = D3D12_BLEND_SRC_ALPHA;
            rt->DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
            rt->SrcBlendAlpha = D3D12_BLEND_ONE;
            rt->DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
            break;
        case 3:
            desc.RTVFormats[0] = DXGI_FORMAT_B5G5R5A1_UNORM;
            break;
        case 4:
            desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
            break;
        default:
            break;
        }
        if (!create_uber_test_pso(r, id, format, steps[i], &desc)) {
            return false;
        }
    }
    return true;
}

static void run_uber_test(PGRAPHD3D12State *r, const char *id,
                          unsigned int busy_workers)
{
    PGRAPHState *pg = r->pg;
    g_autofree char *vs_source = pgraph_d3d12_uber_test_vs_source();
    g_autofree char *ps_source = pgraph_d3d12_uber_test_ps_source();
    IDxcCompiler3 *compiler = NULL;
    IDxcBlob *vs[2] = { NULL, NULL };   /* DXIL, DXBC */
    IDxcBlob *ps[2] = { NULL, NULL };
    static const char *const formats[2] = { "dxil", "dxbc" };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc;
    ID3D12RootSignature *disjoint = NULL;
    ID3D12RootSignature *overlap = NULL;
    HMODULE dxcompiler;
    HMODULE dxil;
    DxcCreateInstanceProc create_instance;
    int64_t start = g_get_monotonic_time();
    bool alive = true;

    fprintf(stderr, UBER_TEST_LOG "begin busy_workers=%u removed=0x%08lx\n",
            id, busy_workers, uber_test_removed(r));
    if (load_dxc(pg, &dxcompiler, &dxil, &create_instance) &&
        FAILED(create_instance(&CLSID_DxcCompiler, &IID_IDxcCompiler3,
                               (LPVOID *)&compiler))) {
        compiler = NULL;
    }
    vs[0] = compile_uber_test_stage(pg, compiler, id, vs_source, true, true);
    ps[0] = compile_uber_test_stage(pg, compiler, id, ps_source, true, false);
    D3D12_RELEASE(compiler);
    vs[1] = compile_uber_test_stage(pg, NULL, id, vs_source, false, true);
    ps[1] = compile_uber_test_stage(pg, NULL, id, ps_source, false, false);

    disjoint = create_uber_test_root_signature(r, id, false);
    for (unsigned int i = 0; i < 2 && alive; i++) {
        if (vs[i] != NULL && ps[i] != NULL) {
            fill_uber_test_desc(&desc, disjoint, vs[i], ps[i]);
            alive = create_uber_test_variants(r, id, formats[i], &desc);
        }
    }
    /* Last, as the step the driver is most likely to fail. */
    if (alive) {
        overlap = create_uber_test_root_signature(r, id, true);
    }
    for (unsigned int i = 0; i < 2 && alive; i++) {
        if (vs[i] != NULL && ps[i] != NULL) {
            fill_uber_test_desc(&desc, overlap, vs[i], ps[i]);
            alive = create_uber_test_pso(r, id, formats[i], "pso_overlap",
                                         &desc);
        }
    }

    for (unsigned int i = 0; i < 2; i++) {
        D3D12_RELEASE(vs[i]);
        D3D12_RELEASE(ps[i]);
    }
    D3D12_RELEASE(disjoint);
    D3D12_RELEASE(overlap);
    fprintf(stderr, UBER_TEST_LOG "done ms=%.1f removed=0x%08lx\n", id,
            uber_test_ms(start), uber_test_removed(r));
}

void pgraph_d3d12_queue_uber_test(PGRAPHD3D12State *r, const char *id)
{
    PGRAPHD3D12PsoJob *job;
    PGRAPHD3D12PsoJob *after = NULL;
    PGRAPHD3D12PsoJob *it;

    if (!r->pso_workers_running) {
        fprintf(stderr, UBER_TEST_LOG "skipped: no pipeline workers\n", id);
        return;
    }
    job = g_new0(PGRAPHD3D12PsoJob, 1);
    job->kind = PGRAPH_D3D12_PSO_JOB_UBER_TEST;
    g_strlcpy(job->test_id, id, sizeof(job->test_id));
    qemu_mutex_lock(&r->pso_mutex);
    /* Behind list loads, ahead of builds; a title switch drops it with
     * the builds. */
    QSIMPLEQ_FOREACH(it, &r->pso_jobs, entry) {
        if (it->kind != PGRAPH_D3D12_PSO_JOB_LOAD_LIST) {
            break;
        }
        after = it;
    }
    if (after != NULL) {
        QSIMPLEQ_INSERT_AFTER(&r->pso_jobs, after, job, entry);
    } else {
        QSIMPLEQ_INSERT_HEAD(&r->pso_jobs, job, entry);
    }
    qemu_cond_broadcast(&r->pso_cond);
    qemu_mutex_unlock(&r->pso_mutex);
    fprintf(stderr, UBER_TEST_LOG "queued\n", id);
}

/*
 * Interpreter fallback. A fallback key holds the state an interpreter
 * pipeline cannot take from constants (see PGRAPHD3D12FallbackKey); its
 * pipeline serves every draw key with that state.
 */
static guint fallback_key_hash(gconstpointer data)
{
    uint64_t hash = fast_hash(data, sizeof(PGRAPHD3D12FallbackKey));

    return (guint)(hash ^ (hash >> 32));
}

static gboolean fallback_key_equal(gconstpointer a, gconstpointer b)
{
    return memcmp(a, b, sizeof(PGRAPHD3D12FallbackKey)) == 0;
}

/* An interpreter pipeline; none of them is released before finalize. */
typedef struct FallbackPipelineEntry {
    ID3D12PipelineState *pipeline_state;
    bool queued;        /* a worker builds it */
    bool failed;
} FallbackPipelineEntry;

static void destroy_fallback_entry(gpointer value)
{
    FallbackPipelineEntry *entry = value;

    D3D12_RELEASE(entry->pipeline_state);
    g_free(entry);
}

/* Stages past projective, cube, pass-through and clip-plane sampling need
 * the full pixel interpreter. */
static uint8_t fallback_ps_class(const PGRAPHD3D12PipelineKey *key)
{
    uint8_t executed = texture_stage_execution_mask(key);

    for (unsigned int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        unsigned int mode = texture_stage_mode(key, i);

        if (!(executed & (1u << i))) {
            continue;
        }
        switch (mode) {
        case PS_TEXTUREMODES_PROJECT2D:
            if (texture_stage_uses_convolution(key, i, mode)) {
                return PGRAPH_D3D12_FALLBACK_PS_FULL;
            }
            break;
        case PS_TEXTUREMODES_PROJECT3D:
        case PS_TEXTUREMODES_CUBEMAP:
        case PS_TEXTUREMODES_PASSTHRU:
        case PS_TEXTUREMODES_CLIPPLANE:
            break;
        default:
            return PGRAPH_D3D12_FALLBACK_PS_FULL;
        }
    }
    return PGRAPH_D3D12_FALLBACK_PS_COMMON;
}

/*
 * The fallback key of a canonical pipeline key. The interpreter takes the
 * rest from constants: vertex program or fixed-function state, combiners,
 * texture modes and formats, fog, alpha test, depth mode and, without a
 * geometry shader, culling. False if the interpreter pixel shader cannot
 * draw the key, which then has no fallback.
 */
static bool fill_fallback_key(const PGRAPHD3D12PipelineKey *key,
                              PGRAPHD3D12FallbackKey *fb)
{
    memset(fb, 0, sizeof(*fb));
    fb->primitive_topology_type = key->primitive_topology_type;
    fb->rtv_format = key->rtv_format;
    fb->dsv_format = key->dsv_format;
    fb->color_write_mask = key->color_write_mask;
    fb->blend_enable = key->blend_enable;
    fb->logic_op_enable = key->logic_op_enable;
    fb->logic_op = key->logic_op;
    fb->src_blend = key->src_blend;
    fb->dest_blend = key->dest_blend;
    fb->blend_op = key->blend_op;
    fb->src_blend_alpha = key->src_blend_alpha;
    fb->dest_blend_alpha = key->dest_blend_alpha;
    fb->blend_op_alpha = key->blend_op_alpha;
    fb->depth_enable = key->depth_enable;
    if (key->depth_enable) {
        fb->depth_write = key->depth_write;
        fb->depth_func = key->depth_func;
    }
    fb->stencil_enable = key->stencil_enable;
    if (key->stencil_enable) {
        fb->stencil_read_mask = key->stencil_read_mask;
        fb->stencil_write_mask = key->stencil_write_mask;
        fb->stencil_func = key->stencil_func;
        fb->stencil_fail_op = key->stencil_fail_op;
        fb->stencil_depth_fail_op = key->stencil_depth_fail_op;
        fb->stencil_pass_op = key->stencil_pass_op;
    }
    fb->geometry_shader = draw_geometry_shader_needed(key);
    if (fb->geometry_shader) {
        fb->primitive_mode = key->primitive_mode;
        fb->smooth_shading = key->smooth_shading;
        fb->first_vertex_provoking = key->first_vertex_provoking;
        fb->polygon_mode = key->polygon_mode;
        fb->line_expand_width = key->line_expand_width;
        fb->point_sprite_enable = key->point_sprite_enable;
        /* calc_depth_slope bakes it in. */
        fb->z_perspective = key->z_perspective;
        fb->cull_mode = key->cull_mode;
        fb->front_counter_clockwise = key->front_counter_clockwise;
    }
    fb->depth_output = pipeline_key_shader_depth_output(key);
    fb->depth_clip = !fb->depth_output && pipeline_key_fragment_zvalue(key);
    fb->zinterp_screen = key->tune_flags & PGRAPH_D3D12_TUNE_ZINTERP_SCREEN;
    fb->vs_class = key->vertex_shader_mode == 0 ?
                       PGRAPH_D3D12_FALLBACK_VS_FIXED :
                       PGRAPH_D3D12_FALLBACK_VS_PROGRAM;
    fb->ps_class = fallback_ps_class(key);
    return uber_ps_unsupported(key) == NULL &&
           uber_vs_unsupported(key) == NULL;
}

/*
 * The pipeline key an interpreter pipeline is generated from: the fixed
 * state of the fallback key, every texture coordinate, the geometry shader
 * selection and the depth interface. Without a geometry shader the
 * rasterizer culls nothing; the pixel shader culls (uber_cull_bits()).
 */
static void fill_fallback_pipeline_key(const PGRAPHD3D12FallbackKey *fb,
                                       PGRAPHD3D12PipelineKey *pk)
{
    memset(pk, 0, sizeof(*pk));
    pk->primitive_topology_type = fb->primitive_topology_type;
    pk->rtv_format = fb->rtv_format;
    pk->dsv_format = fb->dsv_format;
    pk->color_write_mask = fb->color_write_mask;
    pk->blend_enable = fb->blend_enable;
    pk->logic_op_enable = fb->logic_op_enable;
    pk->logic_op = fb->logic_op;
    pk->src_blend = fb->src_blend;
    pk->dest_blend = fb->dest_blend;
    pk->blend_op = fb->blend_op;
    pk->src_blend_alpha = fb->src_blend_alpha;
    pk->dest_blend_alpha = fb->dest_blend_alpha;
    pk->blend_op_alpha = fb->blend_op_alpha;
    pk->depth_enable = fb->depth_enable;
    pk->depth_write = fb->depth_write;
    pk->depth_func = fb->depth_func;
    pk->stencil_enable = fb->stencil_enable;
    pk->stencil_read_mask = fb->stencil_read_mask;
    pk->stencil_write_mask = fb->stencil_write_mask;
    pk->stencil_func = fb->stencil_func;
    pk->stencil_fail_op = fb->stencil_fail_op;
    pk->stencil_depth_fail_op = fb->stencil_depth_fail_op;
    pk->stencil_pass_op = fb->stencil_pass_op;
    if (fb->geometry_shader) {
        pk->primitive_mode = fb->primitive_mode;
        pk->smooth_shading = fb->smooth_shading;
        pk->first_vertex_provoking = fb->first_vertex_provoking;
        pk->polygon_mode = fb->polygon_mode;
        pk->line_expand_width = fb->line_expand_width;
        pk->point_sprite_enable = fb->point_sprite_enable;
        pk->z_perspective = fb->z_perspective;
        pk->cull_mode = fb->cull_mode;
        pk->front_counter_clockwise = fb->front_counter_clockwise;
    } else {
        pk->smooth_shading = true;
        pk->line_expand_width = 1;
        pk->polygon_mode = POLY_MODE_FILL;
        pk->cull_mode = D3D12_CULL_MODE_NONE;
    }
    pk->depth_clipping = fb->depth_clip;
    if (fb->zinterp_screen) {
        pk->tune_flags |= PGRAPH_D3D12_TUNE_ZINTERP_SCREEN;
    }
    /* A depth-tested key without SV_Depth (zdepth=native) keeps it so. */
    if (!fb->depth_output && pk->dsv_format != DXGI_FORMAT_UNKNOWN &&
        pk->depth_enable && !pk->z_perspective) {
        pk->tune_flags |= PGRAPH_D3D12_TUNE_ZDEPTH_NATIVE;
    }
    pk->texture_coord_mask = (1u << NV2A_MAX_TEXTURES) - 1;
    pk->vertex_shader_mode = fb->vs_class == PGRAPH_D3D12_FALLBACK_VS_PROGRAM;
}

/*
 * Builds the interpreter pipeline of a fallback key with the draw root
 * signature: interpreter vertex shader of its class, the geometry shader
 * of its selection and the interpreter pixel shader of its class. PSO
 * worker.
 */
static bool build_fallback_pso(PGRAPHD3D12State *r,
                               const PGRAPHD3D12FallbackKey *key,
                               ID3D12PipelineState **pso_out, Error **errp)
{
    PGRAPHD3D12PipelineKey pk;
    char *vs_source;
    char *gs_source;
    char *ps_source;

    *pso_out = NULL;
    fill_fallback_pipeline_key(key, &pk);
    vs_source = build_uber_vertex_shader(
        &pk, key->vs_class == PGRAPH_D3D12_FALLBACK_VS_PROGRAM);
    gs_source = key->geometry_shader ? build_draw_geometry_shader(&pk) :
                                       NULL;
    ps_source = build_uber_pixel_shader_class(
        &pk, key->ps_class == PGRAPH_D3D12_FALLBACK_PS_FULL);
    if (key->geometry_shader && gs_source == NULL) {
        g_free(vs_source);
        g_free(ps_source);
        error_setg(errp, "Direct3D 12 interpreter pipeline has no geometry "
                         "shader for its primitive");
        return false;
    }
    return create_draw_pso_from_sources(r->pg, &pk, NULL, vs_source,
                                        gs_source, ps_source, true, pso_out,
                                        NULL, errp);
}

/* Workers that take what draws wait for even where the precompile mode
 * parks them. */
#define PSO_DEMAND_WORKERS 3

/* Draws wait on demand builds and on interpreter pipelines. */
static bool pso_job_urgent(const PGRAPHD3D12PsoJob *job)
{
    return job->demand || job->kind == PGRAPH_D3D12_PSO_JOB_FALLBACK;
}

/*
 * The first queued job a draw waits for, or NULL. Such jobs queue ahead of
 * every precompile build (queue_urgent_pso_job()), so the search ends at
 * the first of those. pso_mutex held.
 */
static PGRAPHD3D12PsoJob *first_urgent_pso_job(PGRAPHD3D12State *r)
{
    PGRAPHD3D12PsoJob *it;

    QSIMPLEQ_FOREACH(it, &r->pso_jobs, entry) {
        if (pso_job_urgent(it)) {
            return it;
        }
        if (it->kind == PGRAPH_D3D12_PSO_JOB_BUILD) {
            break;
        }
    }
    return NULL;
}

/*
 * Queues a job draws wait for behind list loads, the uber test and earlier
 * such jobs, ahead of precompile builds, and wakes the workers. pso_mutex
 * held.
 */
static void queue_urgent_pso_job(PGRAPHD3D12State *r, PGRAPHD3D12PsoJob *job)
{
    PGRAPHD3D12PsoJob *after = NULL;
    PGRAPHD3D12PsoJob *it;

    QSIMPLEQ_FOREACH(it, &r->pso_jobs, entry) {
        if (it->kind == PGRAPH_D3D12_PSO_JOB_BUILD && !it->demand) {
            break;
        }
        after = it;
    }
    if (after != NULL) {
        QSIMPLEQ_INSERT_AFTER(&r->pso_jobs, after, job, entry);
    } else {
        QSIMPLEQ_INSERT_HEAD(&r->pso_jobs, job, entry);
    }
    qemu_cond_broadcast(&r->pso_cond);
}

static void *pso_worker_thread_fn(void *opaque)
{
    PGRAPHD3D12PsoWorker *w = opaque;
    PGRAPHD3D12State *r = w->r;
    PGRAPHState *pg = r->pg;

    pso_worker_thread = true;
    current_pso_worker = w;
    qemu_mutex_lock(&r->pso_mutex);
    w->handle = OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION,
                           FALSE, GetCurrentThreadId());
    w->priority = THREAD_PRIORITY_NORMAL;
    for (;;) {
        PGRAPHD3D12PsoJob *job = QSIMPLEQ_FIRST(&r->pso_jobs);
        DrawPsoBuildInfo info;
        Error *local_err = NULL;

        /* List loads own recorded keys, so they run even when stopping. */
        if (r->pso_workers_stop &&
            (job == NULL || job->kind != PGRAPH_D3D12_PSO_JOB_LOAD_LIST)) {
            break;
        }
        update_worker_priority(w);
        if (job != NULL && w->index >= r->pso_active_workers &&
            !r->pso_workers_stop) {
            /* Parked: the first few take what draws wait for, even behind
             * a list load or the uber test. */
            job = w->index < PSO_DEMAND_WORKERS ? first_urgent_pso_job(r) :
                                                  NULL;
        }
        if (job == NULL) {
            qemu_cond_wait(&r->pso_cond, &r->pso_mutex);
            continue;
        }
        QSIMPLEQ_REMOVE(&r->pso_jobs, job, PGRAPHD3D12PsoJob, entry);
        w->inflight = job;
        if (first_urgent_pso_job(r) != NULL) {
            /* For a parked worker. */
            qemu_cond_broadcast(&r->pso_cond);
        }
        if (job->kind == PGRAPH_D3D12_PSO_JOB_FALLBACK) {
            g_hash_table_remove(r->fallback_queued, &job->fallback_key);
            w->boosted = true;
            update_worker_priority(w);
            qemu_mutex_unlock(&r->pso_mutex);
            job->started_us = g_get_monotonic_time();
            if (!build_fallback_pso(r, &job->fallback_key,
                                    &job->pipeline_state, &local_err)) {
                job->error = g_strdup(local_err != NULL ?
                                          error_get_pretty(local_err) :
                                          "unknown pipeline compile error");
                error_free(local_err);
            }
            job->finished_us = g_get_monotonic_time();
            qemu_mutex_lock(&r->pso_mutex);
            w->inflight = NULL;
            w->boosted = false;
            if (!g_hash_table_contains(r->fallback_done, &job->fallback_key)) {
                g_hash_table_add(r->fallback_done,
                                 g_memdup2(&job->fallback_key,
                                           sizeof(job->fallback_key)));
            }
            QSIMPLEQ_INSERT_TAIL(&r->pso_results, job, entry);
            qemu_cond_broadcast(&r->pso_done_cond);
            continue;
        }
        if (job->kind == PGRAPH_D3D12_PSO_JOB_LOAD_LIST) {
            qemu_mutex_unlock(&r->pso_mutex);
            load_pipeline_list(r, job->list);
            /* Other threads read inflight under pso_mutex. */
            qemu_mutex_lock(&r->pso_mutex);
            w->inflight = NULL;
            g_free(job);
            continue;
        }
        if (job->kind == PGRAPH_D3D12_PSO_JOB_UBER_TEST) {
            unsigned int busy = 0;

            for (unsigned int i = 0; i < r->pso_num_workers; i++) {
                busy += &r->pso_workers[i] != w &&
                        r->pso_workers[i].inflight != NULL;
            }
            /* Timed at normal priority, like a build a draw waits for. */
            w->boosted = true;
            update_worker_priority(w);
            qemu_mutex_unlock(&r->pso_mutex);
            run_uber_test(r, job->test_id, busy);
            qemu_mutex_lock(&r->pso_mutex);
            w->inflight = NULL;
            w->boosted = false;
            g_free(job);
            continue;
        }
        g_hash_table_remove(r->pso_queued, &job->key);
        if (job->demand) {
            w->boosted = true;
            update_worker_priority(w);
        }
        qemu_mutex_unlock(&r->pso_mutex);

        if (!create_minimal_draw_pso(pg, &job->key, NULL,
                                     &job->pipeline_state, &info,
                                     &local_err)) {
            job->error = g_strdup(local_err != NULL ?
                                      error_get_pretty(local_err) :
                                      "unknown pipeline compile error");
            error_free(local_err);
        }
        job->finished_us = g_get_monotonic_time();

        qemu_mutex_lock(&r->pso_mutex);
        w->inflight = NULL;
        w->boosted = false;
        if (job->pipeline_state != NULL) {
            hash_set_add(r->pso_built, pipeline_key_hash64(&job->key));
            if (r->pso_run_active && job->run == r->pso_run) {
                r->pso_run_driver_us += info.driver_us;
                r->pso_run_dxil += info.dxil;
                r->pso_run_dxil_fallbacks += info.dxil_fallback;
            }
        }
        precompile_job_finished(r, job->run, job->pipeline_state != NULL);
        QSIMPLEQ_INSERT_TAIL(&r->pso_results, job, entry);
        qemu_cond_broadcast(&r->pso_done_cond);
    }
    qemu_mutex_unlock(&r->pso_mutex);
    return NULL;
}

/*
 * Ends the skipped draws' wait for a drained build of their key: a built
 * pipeline counts its ready time; after a failure the key's next draw takes
 * the synchronous build, which marks it failed only if it fails too.
 * Returns false if no draw was skipped for the key. FIFO thread.
 */
static bool finish_demand(PGRAPHD3D12State *r, const PGRAPHD3D12PsoJob *job)
{
    static unsigned int failures_logged;
    const int64_t *first_skip_us;

    if (g_hash_table_size(r->fallback_demand) == 0) {
        return false;
    }
    first_skip_us = g_hash_table_lookup(r->fallback_demand, &job->key);
    if (first_skip_us == NULL) {
        return false;
    }
    if (job->pipeline_state != NULL) {
        uint64_t us = job->finished_us > *first_skip_us ?
                          job->finished_us - *first_skip_us : 0;

        r->perf.fallback_ready++;
        r->perf.fallback_ready_us += us;
        r->perf.fallback_ready_max_us =
            MAX(r->perf.fallback_ready_max_us, us);
    } else if (!g_hash_table_contains(r->fallback_demand_failed, &job->key)) {
        g_hash_table_add(r->fallback_demand_failed,
                         g_memdup2(&job->key, sizeof(job->key)));
        if (failures_logged++ < 8) {
            fprintf(stderr,
                    "Direct3D 12 renderer: pipeline build for skipped draws "
                    "failed, the next draw builds it: %s\n",
                    job->error);
        }
    }
    g_hash_table_remove(r->fallback_demand, &job->key);
    return true;
}

/* Takes a worker's interpreter pipeline, or notes its failure. FIFO
 * thread. */
static void adopt_fallback_pso(PGRAPHD3D12State *r, PGRAPHD3D12PsoJob *job)
{
    FallbackPipelineEntry *entry =
        g_hash_table_lookup(r->fallback_pipelines, &job->fallback_key);
    uint64_t us = job->finished_us > job->started_us ?
                      job->finished_us - job->started_us : 0;

    r->perf.fallback_builds++;
    r->perf.fallback_build_us += us;
    r->perf.fallback_build_max_us = MAX(r->perf.fallback_build_max_us, us);
    if (entry == NULL) {
        /* Seeded from a pipeline list before any draw needed it. */
        entry = g_new0(FallbackPipelineEntry, 1);
        g_hash_table_insert(r->fallback_pipelines,
                            g_memdup2(&job->fallback_key,
                                      sizeof(job->fallback_key)),
                            entry);
        r->fallback_keys++;
    }
    entry->queued = false;
    if (job->pipeline_state != NULL) {
        entry->pipeline_state = job->pipeline_state;
        job->pipeline_state = NULL;
        return;
    }
    entry->failed = true;
    if (r->fallback_build_failures++ < 4) {
        fprintf(stderr,
                "Direct3D 12 renderer: interpreter pipeline build failed: "
                "%s\n",
                job->error);
    }
}

static void drain_completed_psos(PGRAPHD3D12State *r)
{
    QSIMPLEQ_HEAD(, PGRAPHD3D12PsoJob) done;
    PGRAPHD3D12PsoJob *job;

    QSIMPLEQ_INIT(&done);
    qemu_mutex_lock(&r->pso_mutex);
    QSIMPLEQ_CONCAT(&done, &r->pso_results);
    qemu_mutex_unlock(&r->pso_mutex);

    while ((job = QSIMPLEQ_FIRST(&done)) != NULL) {
        QSIMPLEQ_REMOVE_HEAD(&done, entry);
        if (job->kind == PGRAPH_D3D12_PSO_JOB_FALLBACK) {
            adopt_fallback_pso(r, job);
        } else if (job->pipeline_state != NULL) {
            bool demanded = finish_demand(r, job);

            if (g_hash_table_contains(r->draw_pipeline_cache, &job->key)) {
                /* A draw built it first; cached entries are never
                 * replaced because the current PSO may point at one. */
                D3D12_RELEASE(job->pipeline_state);
            } else {
                PipelineCacheEntry *entry = g_new0(PipelineCacheEntry, 1);

                entry->pipeline_state = job->pipeline_state;
                /* Skipped draws use it next, so a title switch before then
                 * keeps it. */
                entry->drawn = demanded;
                g_hash_table_insert(r->draw_pipeline_cache,
                                    g_memdup2(&job->key, sizeof(job->key)),
                                    entry);
                nv2a_profile_inc_counter(NV2A_PROF_PSO_COMPILED_ASYNC);
                r->pso_total_prewarmed++;
                r->perf.pso_prewarmed++;
            }
        } else if (!finish_demand(r, job) && !job->demand) {
            /* A draw that needs this state compiles it again and fails
             * there, so a prewarm failure is only reported; finish_demand()
             * reports a demand build's. */
            if (r->pso_prewarm_failed++ < 4) {
                fprintf(stderr,
                        "Direct3D 12 renderer: pipeline prewarm failed: "
                        "%s\n", job->error);
            }
        }
        g_free(job->error);
        g_free(job);
    }
}

/*
 * Takes a queued prewarm of key off the queue, or waits for the worker
 * building it, raising the worker to normal priority meanwhile so the
 * emulation threads cannot starve it. Returns true if the worker's result
 * may now be ready, including a build that finished after the caller's
 * last drain. The key is then marked built, so a list load does not
 * queue it while the draw builds it; *marked is set if this call marked it.
 */
static bool claim_queued_pso(PGRAPHD3D12State *r,
                             const PGRAPHD3D12PipelineKey *key, bool *marked)
{
    uint64_t hash = pipeline_key_hash64(key);
    PGRAPHD3D12PsoJob *job;
    bool waited = false;

    qemu_mutex_lock(&r->pso_mutex);
    job = g_hash_table_lookup(r->pso_queued, key);
    if (job != NULL) {
        g_hash_table_remove(r->pso_queued, key);
        QSIMPLEQ_REMOVE(&r->pso_jobs, job, PGRAPHD3D12PsoJob, entry);
        /* The draw builds it now. */
        precompile_job_finished(r, job->run, true);
        g_free(job);
    }
    for (;;) {
        PGRAPHD3D12PsoWorker *w = NULL;

        for (unsigned int i = 0; i < r->pso_num_workers; i++) {
            PGRAPHD3D12PsoJob *inflight = r->pso_workers[i].inflight;

            if (inflight != NULL &&
                inflight->kind == PGRAPH_D3D12_PSO_JOB_BUILD &&
                pipeline_key_equal(&inflight->key, key)) {
                w = &r->pso_workers[i];
                break;
            }
        }
        if (w == NULL) {
            break;
        }
        boost_pso_worker(w);
        qemu_cond_wait(&r->pso_done_cond, &r->pso_mutex);
        waited = true;
    }
    *marked = hash_set_add(r->pso_built, hash);
    qemu_mutex_unlock(&r->pso_mutex);
    /* Already marked: a worker built it and its result awaits a drain. */
    return waited || !*marked;
}

/* Drops queued builds; list loads stay, they own recorded keys, and with
 * keep_urgent so do the jobs draws wait for, which no title owns, and the
 * uber test, which runs once per id. pso_mutex held. */
static void purge_queued_psos(PGRAPHD3D12State *r, bool keep_urgent)
{
    QSIMPLEQ_HEAD(, PGRAPHD3D12PsoJob) keep;
    PGRAPHD3D12PsoJob *job;

    QSIMPLEQ_INIT(&keep);
    while ((job = QSIMPLEQ_FIRST(&r->pso_jobs)) != NULL) {
        QSIMPLEQ_REMOVE_HEAD(&r->pso_jobs, entry);
        if (job->kind == PGRAPH_D3D12_PSO_JOB_LOAD_LIST ||
            (keep_urgent && (pso_job_urgent(job) ||
                             job->kind == PGRAPH_D3D12_PSO_JOB_UBER_TEST))) {
            QSIMPLEQ_INSERT_TAIL(&keep, job, entry);
            continue;
        }
        if (job->kind == PGRAPH_D3D12_PSO_JOB_BUILD) {
            g_hash_table_remove(r->pso_queued, &job->key);
        } else if (job->kind == PGRAPH_D3D12_PSO_JOB_FALLBACK) {
            g_hash_table_remove(r->fallback_queued, &job->fallback_key);
        }
        g_free(job);
    }
    QSIMPLEQ_CONCAT(&r->pso_jobs, &keep);
}

/* Takes over a loaded list: its file keys join the recorded set. */
static void adopt_pipeline_list(PGRAPHD3D12State *r,
                                PGRAPHD3D12PipelineList *list)
{
    GHashTableIter iter;
    gpointer hash;

    filter_pending_records(list);
    g_hash_table_iter_init(&iter, list->file_keys);
    while (g_hash_table_iter_next(&iter, &hash, NULL)) {
        hash_set_add(r->pso_recorded, *(uint64_t *)hash);
    }
    g_hash_table_destroy(list->file_keys);
    list->file_keys = NULL;
    list->adopted = true;
}

static bool pipeline_list_loaded(PGRAPHD3D12State *r,
                                 PGRAPHD3D12PipelineList *list)
{
    bool loaded;

    qemu_mutex_lock(&r->pso_mutex);
    loaded = list->loaded;
    qemu_mutex_unlock(&r->pso_mutex);
    return loaded;
}

static void close_pipeline_list(PGRAPHD3D12State *r)
{
    PGRAPHD3D12PipelineList *list = r->pso_list;
    bool loaded;

    if (list == NULL) {
        return;
    }
    r->pso_list = NULL;
    qemu_mutex_lock(&r->pso_mutex);
    loaded = list->loaded;
    /* Otherwise the loader writes the records and frees the list. */
    list->abandoned = !loaded;
    qemu_mutex_unlock(&r->pso_mutex);
    if (loaded) {
        if (!list->adopted) {
            filter_pending_records(list);
        }
        write_pending_records(list);
        free_pipeline_list(list);
    }
}

void pgraph_d3d12_shader_cache_flush(PGRAPHState *pg, bool force)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    PGRAPHD3D12PipelineList *list = r != NULL ? r->pso_list : NULL;
    int64_t now;

    if (list == NULL) {
        return;
    }
    now = g_get_monotonic_time();
    if (!force && now - r->pso_list_flush_us < PIPELINE_LIST_FLUSH_US) {
        return;
    }
    r->pso_list_flush_us = now;
    if (!list->adopted) {
        if (!pipeline_list_loaded(r, list)) {
            return;
        }
        adopt_pipeline_list(r, list);
    }
    write_pending_records(list);
}

/* Buffers a newly built state for the title's list file. Key-only tune
 * bits are dropped, so every variant of a state shares one record. */
static void record_pipeline_key(PGRAPHD3D12State *r,
                                const PGRAPHD3D12PipelineKey *draw_key)
{
    PGRAPHD3D12PipelineList *list = r->pso_list;
    PGRAPHD3D12PipelineKey stored = *draw_key;
    const PGRAPHD3D12PipelineKey *key = &stored;
    uint64_t check;

    stored.tune_flags &= ~PGRAPH_D3D12_TUNE_KEY_ONLY_FLAGS;
    if (list == NULL || !g_config.perf.cache_shaders ||
        (list->adopted ? list->records :
                         g_hash_table_size(r->pso_recorded)) >=
            PIPELINE_LIST_MAX ||
        !hash_set_add(r->pso_recorded, pipeline_key_hash64(key))) {
        return;
    }
    check = pipeline_list_check(key);
    g_byte_array_append(list->pending, (const guint8 *)key, sizeof(*key));
    g_byte_array_append(list->pending, (const guint8 *)&check,
                        sizeof(check));
    if (list->adopted) {
        list->records++;
    }
}

/* Sets the worker count and priority of a precompile mode. */
static void apply_precompile_mode(PGRAPHD3D12State *r, bool foreground)
{
    int priority = foreground ? THREAD_PRIORITY_NORMAL :
                                THREAD_PRIORITY_BELOW_NORMAL;

    r->pso_foreground = foreground;
    qemu_mutex_lock(&r->pso_mutex);
    r->pso_active_workers = foreground ? r->pso_foreground_workers : 1;
    r->pso_worker_priority = priority;
    r->pso_run_workers = MAX(r->pso_run_workers, r->pso_active_workers);
    /* Busy workers change now; the others when they next take a job. */
    for (unsigned int i = 0; i < r->pso_num_workers; i++) {
        PGRAPHD3D12PsoWorker *w = &r->pso_workers[i];

        if (w->inflight == NULL || w->boosted || w->handle == NULL ||
            w->priority == priority) {
            continue;
        }
        if (SetThreadPriority(w->handle, priority)) {
            w->priority = priority;
        }
    }
    qemu_cond_broadcast(&r->pso_cond);
    qemu_mutex_unlock(&r->pso_mutex);
}

/*
 * Releases the precompiled PSOs no draw has used, so precompiled pipelines
 * of earlier titles do not stay resident. They were never current, so no
 * command list references them. FIFO thread.
 */
static void release_undrawn_psos(PGRAPHD3D12State *r)
{
    GArray *hashes = g_array_new(FALSE, FALSE, sizeof(uint64_t));
    GHashTableIter iter;
    gpointer key;
    gpointer value;

    drain_completed_psos(r);
    g_hash_table_iter_init(&iter, r->draw_pipeline_cache);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        PipelineCacheEntry *entry = value;
        uint64_t hash;

        if (entry->drawn) {
            continue;
        }
        hash = pipeline_key_hash64(key);
        g_array_append_val(hashes, hash);
        g_hash_table_iter_remove(&iter);
    }
    if (hashes->len != 0) {
        /* A later list may queue them again. */
        qemu_mutex_lock(&r->pso_mutex);
        for (guint i = 0; i < hashes->len; i++) {
            g_hash_table_remove(r->pso_built,
                                &g_array_index(hashes, uint64_t, i));
        }
        qemu_mutex_unlock(&r->pso_mutex);
        fprintf(stderr,
                "Direct3D 12 renderer: released %u unused precompiled "
                "pipelines\n",
                hashes->len);
    }
    g_array_free(hashes, TRUE);
}

/*
 * Switches the recorded and precompiled list to title_id: queued builds of
 * the previous title are dropped, built PSOs stay if a draw used them, and
 * a worker loads the new list.
 */
static void switch_shader_title(PGRAPHD3D12State *r, uint32_t title_id,
                                uint32_t request)
{
    PGRAPHState *pg = r->pg;
    bool cache = shader_disk_cache_enabled(r) &&
                 pgraph_d3d12_vertex_pulling();
    bool precompile = cache && g_config.perf.precompile_shaders;
    PGRAPHD3D12PipelineList *list = NULL;
    PGRAPHD3D12PsoJob *job;

    r->shader_title_request = request;

    /* The same title keeps its list and run, unless the settings now ask
     * for what it lacks or no longer want its precompile. */
    qemu_mutex_lock(&r->pso_mutex);
    if (r->shader_title_valid && title_id == r->shader_title &&
        (r->pso_list != NULL) == cache) {
        NV2AShaderPrecompileState state = qatomic_read(&pg->shader_cache.state);

        if (!precompile && (state == NV2A_SHADER_PRECOMPILE_LOADING ||
                            state == NV2A_SHADER_PRECOMPILE_COMPILING)) {
            /* A list still loading records but queues nothing for an
             * older run. */
            purge_queued_psos(r, true);
            end_precompile_run(r);
            r->pso_run++;
            r->pso_run_request = request;
            r->pso_run_total = 0;
            r->pso_run_done = 0;
            r->pso_run_failed = 0;
            publish_precompile_status(r, NV2A_SHADER_PRECOMPILE_IDLE);
            qemu_mutex_unlock(&r->pso_mutex);
            return;
        }
        if (!precompile || state != NV2A_SHADER_PRECOMPILE_IDLE) {
            r->pso_run_request = request;
            publish_precompile_status(r, state);
            qemu_mutex_unlock(&r->pso_mutex);
            return;
        }
    }
    qemu_mutex_unlock(&r->pso_mutex);

    fprintf(stderr,
            "Direct3D 12 renderer: shader title %08x -> %08x cache=%d "
            "precompile=%d\n",
            r->shader_title_valid ? r->shader_title : 0, title_id, cache,
            precompile);
    if (r->shader_title_valid && title_id != r->shader_title) {
        release_undrawn_psos(r);
    }
    close_pipeline_list(r);
    g_hash_table_remove_all(r->pso_recorded);
    r->pso_list_serial++;
    r->shader_title = title_id;
    r->shader_title_valid = true;
    if (cache) {
        list = g_new0(PGRAPHD3D12PipelineList, 1);
        list->title_id = title_id;
        list->precompile = precompile;
        list->scale = MAX(pg->surface_scale_factor, 1);
        list->tune_flags = r->tune_flags & PGRAPH_D3D12_TUNE_SHADER_FLAGS;
        list->uber_ps = pgraph_d3d12_fallback_mode(r->tune_flags) ==
                        PGRAPH_D3D12_FALLBACK_PS;
        list->fallback = fallback_mode_draws(
            pgraph_d3d12_fallback_mode(r->tune_flags));
        list->pending = g_byte_array_new();
        r->pso_list = list;
        r->pso_list_flush_us = g_get_monotonic_time();
    }

    qemu_mutex_lock(&r->pso_mutex);
    purge_queued_psos(r, true);
    end_precompile_run(r);
    r->pso_run++;
    r->pso_run_title = title_id;
    r->pso_run_request = request;
    r->pso_run_total = 0;
    r->pso_run_done = 0;
    r->pso_run_failed = 0;
    r->pso_run_recorded = 0;
    r->pso_run_workers = r->pso_active_workers;
    r->pso_run_start_us = g_get_monotonic_time();
    r->pso_run_driver_us = 0;
    r->pso_run_dxil = 0;
    r->pso_run_dxil_fallbacks = 0;
    if (list != NULL) {
        list->run = r->pso_run;
        job = g_new0(PGRAPHD3D12PsoJob, 1);
        job->kind = PGRAPH_D3D12_PSO_JOB_LOAD_LIST;
        job->list = list;
        QSIMPLEQ_INSERT_HEAD(&r->pso_jobs, job, entry);
        qemu_cond_broadcast(&r->pso_cond);
    }
    publish_precompile_status(r, precompile ?
                                     NV2A_SHADER_PRECOMPILE_LOADING :
                                     NV2A_SHADER_PRECOMPILE_IDLE);
    qemu_mutex_unlock(&r->pso_mutex);
}

bool pgraph_d3d12_shader_cache_pending(PGRAPHState *pg)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;

    return r->pso_workers_running &&
           (qatomic_read(&pg->shader_cache.request_gen) !=
                r->shader_title_request ||
            qatomic_read(&pg->shader_cache.foreground) != r->pso_foreground);
}

void pgraph_d3d12_shader_cache_process(PGRAPHState *pg)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    bool foreground = qatomic_read(&pg->shader_cache.foreground);
    uint32_t request = qatomic_load_acquire(&pg->shader_cache.request_gen);

    if (!r->pso_workers_running) {
        return;
    }
    if (foreground != r->pso_foreground) {
        apply_precompile_mode(r, foreground);
    }
    if (request != r->shader_title_request) {
        switch_shader_title(r, qatomic_read(&pg->shader_cache.title_id),
                            request);
    }
}

/*
 * Gets a skipped draw's key built: its queued precompile build moves ahead
 * as a demand build, the worker building it is raised, or a demand build
 * is queued, which no precompile run counts. A build already finished
 * awaits the next drain. FIFO thread; takes pso_mutex, never waits.
 */
static void queue_demand_pso(PGRAPHD3D12State *r,
                             const PGRAPHD3D12PipelineKey *key)
{
    uint64_t hash = pipeline_key_hash64(key);
    PGRAPHD3D12PsoJob *job;

    qemu_mutex_lock(&r->pso_mutex);
    job = g_hash_table_lookup(r->pso_queued, key);
    if (job != NULL) {
        if (!job->demand) {
            QSIMPLEQ_REMOVE(&r->pso_jobs, job, PGRAPHD3D12PsoJob, entry);
            job->demand = true;
            queue_urgent_pso_job(r, job);
        }
        qemu_mutex_unlock(&r->pso_mutex);
        return;
    }
    for (unsigned int i = 0; i < r->pso_num_workers; i++) {
        PGRAPHD3D12PsoJob *inflight = r->pso_workers[i].inflight;

        if (inflight != NULL &&
            inflight->kind == PGRAPH_D3D12_PSO_JOB_BUILD &&
            pipeline_key_equal(&inflight->key, key)) {
            boost_pso_worker(&r->pso_workers[i]);
            qemu_mutex_unlock(&r->pso_mutex);
            return;
        }
    }
    if (!g_hash_table_contains(r->pso_built, &hash)) {
        job = g_new0(PGRAPHD3D12PsoJob, 1);
        job->kind = PGRAPH_D3D12_PSO_JOB_BUILD;
        job->run = PGRAPH_D3D12_PSO_RUN_NONE;
        job->demand = true;
        job->key = *key;
        g_hash_table_insert(r->pso_queued, &job->key, job);
        queue_urgent_pso_job(r, job);
    }
    qemu_mutex_unlock(&r->pso_mutex);
}

/*
 * The interpreter pipeline entry of a fallback key, counting each new key;
 * with build set, a pipeline neither built, building nor failed is queued
 * to the workers. FIFO thread.
 */
static FallbackPipelineEntry *get_fallback_pipeline(
    PGRAPHD3D12State *r, const PGRAPHD3D12FallbackKey *fb, bool build)
{
    FallbackPipelineEntry *entry =
        g_hash_table_lookup(r->fallback_pipelines, fb);

    if (entry == NULL) {
        entry = g_new0(FallbackPipelineEntry, 1);
        g_hash_table_insert(r->fallback_pipelines,
                            g_memdup2(fb, sizeof(*fb)), entry);
        r->fallback_keys++;
    }
    if (build && entry->pipeline_state == NULL && !entry->queued &&
        !entry->failed && r->pso_workers_running) {
        bool pending;

        entry->queued = true;
        qemu_mutex_lock(&r->pso_mutex);
        /* A list load may have queued it, or a worker built it and its
         * result awaits the drain. */
        pending = g_hash_table_contains(r->fallback_queued, fb) ||
                  g_hash_table_contains(r->fallback_done, fb);
        for (unsigned int i = 0; i < r->pso_num_workers && !pending; i++) {
            PGRAPHD3D12PsoJob *inflight = r->pso_workers[i].inflight;

            pending = inflight != NULL &&
                      inflight->kind == PGRAPH_D3D12_PSO_JOB_FALLBACK &&
                      fallback_key_equal(&inflight->fallback_key, fb);
        }
        if (!pending) {
            PGRAPHD3D12PsoJob *job = g_new0(PGRAPHD3D12PsoJob, 1);

            job->kind = PGRAPH_D3D12_PSO_JOB_FALLBACK;
            job->run = PGRAPH_D3D12_PSO_RUN_NONE;
            job->fallback_key = *fb;
            g_hash_table_insert(r->fallback_queued, &job->fallback_key, job);
            queue_urgent_pso_job(r, job);
        }
        qemu_mutex_unlock(&r->pso_mutex);
    }
    return entry;
}

/*
 * Workers build without an input layout, framebuffer-fetch draws stay
 * synchronous, so do keys whose demand build failed, and a skipped draw
 * would drop its pixels from an occlusion count.
 */
static bool draw_pipeline_deferrable(PGRAPHD3D12State *r,
                                     const PGRAPHD3D12PipelineKey *key)
{
    return r->pso_workers_running && pgraph_d3d12_vertex_pulling() &&
           !key->shader_framebuffer_fetch &&
           !r->pg->zpass_pixel_count_enable &&
           (g_hash_table_size(r->fallback_demand_failed) == 0 ||
            !g_hash_table_contains(r->fallback_demand_failed, key));
}

/*
 * Skips a draw whose pipeline is not built (tune "fallback=skip"). The
 * first skip of a key gets its build ahead of the precompile; later draws
 * pick the pipeline up from the drain, and the first to use it records the
 * key. Each new key also counts the fallback key a fallback would have
 * drawn it with. FIFO thread.
 */
static void defer_draw_pipeline(PGRAPHD3D12State *r,
                                const PGRAPHD3D12PipelineKey *key)
{
    PGRAPHD3D12FallbackKey fb;
    int64_t now;

    r->perf.fallback_skipped++;
    if (g_hash_table_contains(r->fallback_demand, key)) {
        return;
    }
    now = g_get_monotonic_time();
    g_hash_table_insert(r->fallback_demand, g_memdup2(key, sizeof(*key)),
                        g_memdup2(&now, sizeof(now)));
    r->perf.fallback_demand++;
    queue_demand_pso(r, key);
    /* Skip mode only counts them; the interpreter modes build them. */
    if (fill_fallback_key(key, &fb)) {
        get_fallback_pipeline(r, &fb,
                              pgraph_d3d12_fallback_mode(r->tune_flags) >
                                  PGRAPH_D3D12_FALLBACK_SKIP);
    }
}

/* Makes a cached pipeline current, recording its key in the title's list
 * the first time the title uses it. */
static void note_cached_pipeline(PGRAPHD3D12State *r,
                                 const PGRAPHD3D12PipelineKey *key,
                                 PipelineCacheEntry *entry)
{
    if (entry->recorded_list != r->pso_list_serial) {
        entry->recorded_list = r->pso_list_serial;
        record_pipeline_key(r, key);
    }
    entry->drawn = true;
}

static void use_cached_pipeline(PGRAPHD3D12State *r,
                                const PGRAPHD3D12PipelineKey *key,
                                PipelineCacheEntry *entry)
{
    note_cached_pipeline(r, key, entry);
    r->draw_pipeline_state = entry->pipeline_state;
    r->draw_pipeline_key = *key;
    r->draw_pipeline_key_valid = true;
    r->draw_pipeline_fallback = false;
}

/* Modes that draw with interpreter pipelines. */
static bool fallback_mode_draws(PGRAPHD3D12FallbackMode mode)
{
    return mode == PGRAPH_D3D12_FALLBACK_ALL ||
           mode == PGRAPH_D3D12_FALLBACK_DEBUG;
}

/* Draws that may take an interpreter pipeline: those a worker can build
 * for, except keys whose demand build failed, which build synchronously
 * once more. */
static bool draw_pipeline_fallback_eligible(PGRAPHD3D12State *r,
                                            const PGRAPHD3D12PipelineKey *key)
{
    return r->pso_workers_running && pgraph_d3d12_vertex_pulling() &&
           !key->shader_framebuffer_fetch &&
           (g_hash_table_size(r->fallback_demand_failed) == 0 ||
            !g_hash_table_contains(r->fallback_demand_failed, key));
}

/*
 * Makes the interpreter pipeline of key's fallback key current if it is
 * built, queueing it otherwise. With demand, the key's own pipeline is
 * queued as a demand build; later draws take it from the drain. FIFO
 * thread.
 */
static bool use_fallback_pipeline(PGRAPHD3D12State *r,
                                  const PGRAPHD3D12PipelineKey *key,
                                  bool demand)
{
    PGRAPHD3D12FallbackKey fb;
    FallbackPipelineEntry *entry;

    if (!fill_fallback_key(key, &fb)) {
        r->perf.fallback_nokey++;
        return false;
    }
    entry = get_fallback_pipeline(r, &fb, true);
    if (entry->pipeline_state == NULL) {
        return false;
    }
    if (demand && !g_hash_table_contains(r->fallback_demand, key)) {
        int64_t now = g_get_monotonic_time();

        g_hash_table_insert(r->fallback_demand, g_memdup2(key, sizeof(*key)),
                            g_memdup2(&now, sizeof(now)));
        r->perf.fallback_demand++;
        queue_demand_pso(r, key);
    }
    r->draw_pipeline_state = entry->pipeline_state;
    r->draw_pipeline_key = *key;
    r->draw_pipeline_key_valid = true;
    r->draw_pipeline_fallback = true;
    return true;
}

static bool ensure_draw_pipeline_state(PGRAPHState *pg, bool *pending,
                                       Error **errp)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    PGRAPHD3D12PipelineKey key;
    PipelineCacheEntry *entry;
    PGRAPHD3D12FallbackMode mode;
    bool marked = false;

    if (!fill_pipeline_key(pg, &key)) {
        return false;
    }
    canonicalize_pipeline_key(&key);
    if (pgraph_d3d12_fallback_mode(r->tune_flags) ==
        PGRAPH_D3D12_FALLBACK_PS) {
        if (mark_uber_ps_key(&key)) {
            r->perf.fallback_uber_ps++;
        } else {
            r->perf.fallback_no_ps++;
        }
    }

    mode = pgraph_d3d12_fallback_mode(r->tune_flags);
    if (r->draw_pipeline_state != NULL && r->draw_pipeline_key_valid &&
        memcmp(&r->draw_pipeline_key, &key, sizeof(key)) == 0) {
        if (!r->draw_pipeline_fallback ||
            mode == PGRAPH_D3D12_FALLBACK_DEBUG) {
            return true;
        }
        if (mode == PGRAPH_D3D12_FALLBACK_ALL) {
            /* On the interpreter until the key's own pipeline is drained. */
            if (r->pso_workers_running) {
                drain_completed_psos(r);
            }
            entry = g_hash_table_lookup(r->draw_pipeline_cache, &key);
            if (entry != NULL) {
                use_cached_pipeline(r, &key, entry);
            }
            return true;
        }
    }

    if (r->pso_workers_running) {
        drain_completed_psos(r);
    }

    entry = g_hash_table_lookup(r->draw_pipeline_cache, &key);
    if (entry != NULL) {
        if (mode == PGRAPH_D3D12_FALLBACK_DEBUG &&
            draw_pipeline_fallback_eligible(r, &key) &&
            use_fallback_pipeline(r, &key, false)) {
            note_cached_pipeline(r, &key, entry);
            return true;
        }
        use_cached_pipeline(r, &key, entry);
        return true;
    }

    if (g_hash_table_contains(r->pso_failed, &key)) {
        /* Already failed; do not retry every draw. The interpreter still
         * draws it where it can. */
        if (fallback_mode_draws(mode) && r->pso_workers_running &&
            !key.shader_framebuffer_fetch &&
            use_fallback_pipeline(r, &key, false)) {
            return true;
        }
        *pending = true;
        return false;
    }

    if (fallback_mode_draws(mode) &&
        draw_pipeline_fallback_eligible(r, &key)) {
        if (use_fallback_pipeline(r, &key, true)) {
            return true;
        }
        /* Not built yet, or no fallback key: the synchronous build. */
        r->perf.fallback_sync++;
    }

    if (pgraph_d3d12_fallback_mode(r->tune_flags) ==
            PGRAPH_D3D12_FALLBACK_SKIP &&
        draw_pipeline_deferrable(r, &key)) {
        defer_draw_pipeline(r, &key);
        *pending = true;
        return false;
    }
    if (g_hash_table_size(r->fallback_demand) != 0) {
        /* Skip mode ended; this draw builds the key itself. */
        g_hash_table_remove(r->fallback_demand, &key);
    }
    if (g_hash_table_size(r->fallback_demand_failed) != 0) {
        /* Built or failed for good below. */
        g_hash_table_remove(r->fallback_demand_failed, &key);
    }

    if (r->pso_workers_running && claim_queued_pso(r, &key, &marked)) {
        drain_completed_psos(r);
        entry = g_hash_table_lookup(r->draw_pipeline_cache, &key);
        if (entry != NULL) {
            use_cached_pipeline(r, &key, entry);
            return true;
        }
    }

    /* Draws cannot be replayed later, so a missing PSO compiles
     * synchronously; only a new state stalls here. */
    ID3D12PipelineState *pipeline_state = NULL;
    DrawPsoBuildInfo info;
    int64_t create_start = g_get_monotonic_time();

    if (!create_minimal_draw_pso(pg, &key,
                                 r->vertex_attribute_descriptions,
                                 &pipeline_state, &info, errp)) {
        if (marked) {
            uint64_t hash = pipeline_key_hash64(&key);

            qemu_mutex_lock(&r->pso_mutex);
            g_hash_table_remove(r->pso_built, &hash);
            qemu_mutex_unlock(&r->pso_mutex);
        }
        g_hash_table_add(r->pso_failed, g_memdup2(&key, sizeof(key)));
        nv2a_profile_inc_counter(NV2A_PROF_PSO_COMPILE_FAILED);
        r->pso_total_failed++;
        return false;
    }
    r->perf.pso_creates++;
    r->perf.pso_create_us += g_get_monotonic_time() - create_start -
                             info.twin_us;
    r->perf.pso_driver_us += info.driver_us;
    r->perf.pso_dxil += info.dxil;
    r->perf.pso_dxil_fallbacks += info.dxil_fallback;
    entry = g_new0(PipelineCacheEntry, 1);
    entry->pipeline_state = pipeline_state;
    g_hash_table_insert(r->draw_pipeline_cache,
                        g_memdup2(&key, sizeof(key)), entry);
    r->pso_total_compiled++;
    use_cached_pipeline(r, &key, entry);
    return true;
}

typedef struct PGRAPHD3D12DrawConstants {
    float inline_value[NV2A_VERTEXSHADER_ATTRIBUTES][4];
    float vsh_constants[NV2A_VERTEXSHADER_CONSTANTS][4];
    float clip_range[4];
    float surface_info[4];
    float point_params[2][4];
    float fog_param[4];
    float ltctxa[NV2A_LTCTXA_COUNT][4];
    float ltctxb[NV2A_LTCTXB_COUNT][4];
    float ltc1[NV2A_LTC1_COUNT][4];
    float light_infinite_direction[NV2A_MAX_LIGHTS][4];
    float light_infinite_half_vector[NV2A_MAX_LIGHTS][4];
    float light_local_position[NV2A_MAX_LIGHTS][4];
    float light_local_attenuation[NV2A_MAX_LIGHTS][4];
    float fixed_function_info[4];
    /* Vertex pulling: per attribute x=buffer select, y=base, z=stride,
     * w=VERTEX_FETCH_* alignment and format bits. */
    uint32_t vertex_fetch[NV2A_VERTEXSHADER_ATTRIBUTES][4];
} PGRAPHD3D12DrawConstants;

typedef struct PGRAPHD3D12PixelConstants {
    float combiner_const[18][4];
    uint32_t color_key[4];
    uint32_t color_key_mask[4];
    float fog_color[4];
    float bump_mat[4][4];
    float bump_scale[4];
    float bump_offset[4];
    float tex_scale[4];
    float blend_color[4];
    float clip_range[4];
    float depth_info[4];
    uint32_t clip_region[8][4];
    /* Alpha ref, alpha func, alpha-kill stage mask, shadow compare func. */
    uint32_t alpha_test[4];
    float tex_size[4][4];            /* guest size of linear textures */
} PGRAPHD3D12PixelConstants;

QEMU_BUILD_BUG_ON(offsetof(PGRAPHD3D12PixelConstants, alpha_test) != 39 * 16);
QEMU_BUILD_BUG_ON(sizeof(PGRAPHD3D12PixelConstants) != 44 * 16);

static void fill_clip_range(PGRAPHState *pg, float clip_range[4])
{
    float zmax;
    uint32_t zclip_min = pgraph_reg_r(pg, NV_PGRAPH_ZCLIPMIN);
    uint32_t zclip_max = pgraph_reg_r(pg, NV_PGRAPH_ZCLIPMAX);

    switch (pg->surface_shape.zeta_format) {
    case NV097_SET_SURFACE_FORMAT_ZETA_Z16:
        zmax = pg->surface_shape.z_format ? f16_max : (float)0xffff;
        break;
    case NV097_SET_SURFACE_FORMAT_ZETA_Z24S8:
        zmax = pg->surface_shape.z_format ? f24_max : (float)0xffffff;
        break;
    default:
        zmax = (float)0xffffff;
        break;
    }

    clip_range[0] = 0.0f;
    clip_range[1] = zmax;
    clip_range[2] = *(float *)&zclip_min;
    clip_range[3] = *(float *)&zclip_max;
}

static void fill_depth_info(PGRAPHState *pg, float depth_info[4])
{
    uint32_t setup = pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER);
    uint32_t front_mode =
        GET_MASK(setup, NV_PGRAPH_SETUPRASTER_FRONTFACEMODE);
    unsigned int wscale = 1;
    unsigned int hscale = 1;

    pgraph_apply_anti_aliasing_factor(pg, &wscale, &hscale);
    pgraph_apply_scaling_factor(pg, &wscale, &hscale);

    depth_info[0] = (float)wscale;
    depth_info[1] = (float)hscale;
    depth_info[2] = 0.0f;
    depth_info[3] = 0.0f;

    if (polygon_offset_enabled_for_mode(pg, setup, front_mode)) {
        uint32_t bias_bits = pgraph_reg_r(pg, NV_PGRAPH_ZOFFSETBIAS);
        uint32_t factor_bits = pgraph_reg_r(pg, NV_PGRAPH_ZOFFSETFACTOR);

        depth_info[2] = *(float *)&bias_bits;
        depth_info[3] = *(float *)&factor_bits;
    }
}

static void fill_clip_regions(PGRAPHState *pg, uint32_t clip_region[8][4])
{
    for (unsigned int i = 0; i < 8; i++) {
        uint32_t x = pgraph_reg_r(pg, NV_PGRAPH_WINDOWCLIPX0 + i * 4);
        uint32_t y = pgraph_reg_r(pg, NV_PGRAPH_WINDOWCLIPY0 + i * 4);
        unsigned int x_min = GET_MASK(x, NV_PGRAPH_WINDOWCLIPX0_XMIN);
        unsigned int x_max = GET_MASK(x, NV_PGRAPH_WINDOWCLIPX0_XMAX) + 1;
        unsigned int y_min = GET_MASK(y, NV_PGRAPH_WINDOWCLIPY0_YMIN);
        unsigned int y_max = GET_MASK(y, NV_PGRAPH_WINDOWCLIPY0_YMAX) + 1;

        pgraph_apply_anti_aliasing_factor(pg, &x_min, &y_min);
        pgraph_apply_anti_aliasing_factor(pg, &x_max, &y_max);
        pgraph_apply_scaling_factor(pg, &x_min, &y_min);
        pgraph_apply_scaling_factor(pg, &x_max, &y_max);

        clip_region[i][0] = x_min;
        clip_region[i][1] = y_min;
        clip_region[i][2] = x_max;
        clip_region[i][3] = y_max;
    }
}

static float d3d12_u32_as_float(uint32_t value)
{
    float ret;

    memcpy(&ret, &value, sizeof(ret));
    return ret;
}

#define D3D12_DEBUG_VERTEX_SAMPLE_COUNT 4

static unsigned int collect_draw_sample_indices(PGRAPHState *pg,
                                                uint32_t *indices,
                                                unsigned int max_indices)
{
    unsigned int count = 0;

    if (pg->draw_arrays_length) {
        for (unsigned int batch = 0;
             batch < pg->draw_arrays_length && count < max_indices; batch++) {
            int32_t start = pg->draw_arrays_start[batch];
            int32_t draw_count = pg->draw_arrays_count[batch];

            if (start < 0 || draw_count <= 0) {
                continue;
            }
            for (int32_t i = 0; i < draw_count && count < max_indices; i++) {
                indices[count++] = (uint32_t)(start + i);
            }
        }
        return count;
    }

    if (pg->inline_elements_length) {
        count = MIN(pg->inline_elements_length, max_indices);
        for (unsigned int i = 0; i < count; i++) {
            indices[i] = pg->inline_elements[i];
        }
        return count;
    }

    if (pg->inline_buffer_length) {
        count = MIN(pg->inline_buffer_length, max_indices);
    } else if (pg->inline_array_length) {
        count = MIN(pg->inline_array_length, max_indices);
    }

    for (unsigned int i = 0; i < count; i++) {
        indices[i] = i;
    }

    return count;
}

static void append_draw_batch_details(GString *out, PGRAPHState *pg,
                                      const uint32_t *sample_indices,
                                      unsigned int num_sample_indices)
{
    g_string_append_printf(
        out,
        "draw_arrays_length=%u inline_elements_length=%u "
        "inline_buffer_length=%u inline_array_length=%u ",
        pg->draw_arrays_length, pg->inline_elements_length,
        pg->inline_buffer_length, pg->inline_array_length);

    if (pg->draw_arrays_length) {
        unsigned int shown = MIN(pg->draw_arrays_length, 4);

        g_string_append_printf(
            out, "draw_arrays_range=%u..%u draw_arrays_first=[",
            pg->draw_arrays_min_start, pg->draw_arrays_max_count);
        for (unsigned int i = 0; i < shown; i++) {
            g_string_append_printf(
                out, "%s(start=%d,count=%d)", i ? "," : "",
                pg->draw_arrays_start[i], pg->draw_arrays_count[i]);
        }
        if (shown < pg->draw_arrays_length) {
            g_string_append(out, ",...");
        }
        g_string_append(out, "] ");
    }

    if (pg->inline_elements_length) {
        unsigned int shown = MIN(pg->inline_elements_length, 8);

        g_string_append(out, "inline_elements_first=[");
        for (unsigned int i = 0; i < shown; i++) {
            g_string_append_printf(out, "%s%u", i ? "," : "",
                                   pg->inline_elements[i]);
        }
        if (shown < pg->inline_elements_length) {
            g_string_append(out, ",...");
        }
        g_string_append(out, "] ");
    }

    g_string_append(out, "sample_indices=[");
    for (unsigned int i = 0; i < num_sample_indices; i++) {
        g_string_append_printf(out, "%s%u", i ? "," : "",
                               sample_indices[i]);
    }
    g_string_append(out, "]");
}

static void append_vertex_sample_value(GString *out,
                                       const VertexAttribute *attr,
                                       const uint8_t *data)
{
    VertexAttribute decoded = *attr;
    unsigned int bytes = attr->size * attr->count;
    unsigned int raw_bytes = MIN(bytes, 16);

    if (bytes == 0 || attr->count > 4) {
        g_string_append_printf(out, " undecodable(bytes=%u,count=%u)", bytes,
                               attr->count);
        return;
    }

    pgraph_update_inline_value(&decoded, data);
    g_string_append_printf(out, " value=(%f,%f,%f,%f) raw=",
                           decoded.inline_value[0], decoded.inline_value[1],
                           decoded.inline_value[2], decoded.inline_value[3]);
    for (unsigned int i = 0; i < raw_bytes; i++) {
        g_string_append_printf(out, "%02x", data[i]);
    }
    if (raw_bytes < bytes) {
        g_string_append(out, "...");
    }
}

static void append_vertex_attr_samples(GString *out, PGRAPHState *pg,
                                       NV2AState *d,
                                       const VertexAttribute *attr,
                                       uint8_t attr_index,
                                       const uint32_t *sample_indices,
                                       unsigned int num_sample_indices)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    const uint8_t *source = d->vram_ptr;
    uint64_t source_size = memory_region_size(d->vram);
    const char *source_name = "vram";
    uint64_t base = r->vertex_attribute_offsets[attr_index];
    uint32_t stride = r->vertex_attribute_strides[attr_index];
    uint32_t element_size = attr->size * attr->count;

    if (r->converted_vertex_attribute_mask & (1u << attr_index)) {
        source = r->vertex_inline_upload_buffer.mapped;
        source_size = r->vertex_inline_upload_buffer.size;
        source_name = "converted_upload";
    } else if (!pg->draw_arrays_length && !pg->inline_elements_length &&
        pg->inline_array_length && !pg->inline_buffer_length) {
        source = (const uint8_t *)pg->inline_array;
        source_size = pg->inline_array_length * sizeof(pg->inline_array[0]);
        source_name = "inline_array";
        base = attr->inline_array_offset;
    } else if (!pg->draw_arrays_length && !pg->inline_elements_length &&
               pg->inline_buffer_length) {
        source = (const uint8_t *)attr->inline_buffer;
        source_size = pg->inline_buffer_length * 4 * sizeof(float);
        source_name = "inline_buffer";
        base = 0;
        stride = 4 * sizeof(float);
    }

    g_string_append_printf(
        out,
        " dma=%c guest_offset=0x%" HWADDR_PRIx
        " bound_source=%s bound_base=0x%" PRIx64
        " bound_stride=%u element_bytes=%u samples=[",
        attr->dma_select ? 'B' : 'A', (hwaddr)attr->offset, source_name,
        base, stride, element_size);

    if (source == NULL) {
        g_string_append(out, "source-null]");
        return;
    }

    if (stride == 0 || num_sample_indices == 0) {
        g_string_append(out, "uniform-or-empty]");
        return;
    }

    for (unsigned int i = 0; i < num_sample_indices; i++) {
        uint64_t offset = base + (uint64_t)sample_indices[i] * stride;

        g_string_append_printf(out, "%s{vertex=%u offset=0x%" PRIx64,
                               i ? "," : "", sample_indices[i], offset);
        if (element_size == 0 || offset >= source_size ||
            element_size > source_size - offset) {
            g_string_append_printf(out,
                                   " out_of_range source_size=0x%" PRIx64 "}",
                                   source_size);
            continue;
        }
        append_vertex_sample_value(out, attr, source + offset);
        g_string_append(out, "}");
    }

    g_string_append(out, "]");
}

static void report_programmable_vsh_fallback(
    PGRAPHState *pg, const PGRAPHD3D12PipelineKey *key)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    unsigned int start = key->transform_program_start;
    unsigned int final_slot = start;
    const uint32_t *first_token = NULL;
    const uint32_t *final_token = NULL;
    VertexAttribute *position = &pg->vertex_attributes[NV2A_VERTEX_ATTR_POSITION];
    int position_location =
        r->vertex_attribute_to_description_location[NV2A_VERTEX_ATTR_POSITION];
    g_autoptr(GString) decoded_tokens = g_string_new(NULL);
    g_autoptr(GString) attr_details = g_string_new(NULL);
    g_autoptr(GString) draw_details = g_string_new(NULL);
    g_autoptr(GString) texture_details = g_string_new(NULL);
    g_autoptr(GString) combiner_details = g_string_new(NULL);
    uint32_t sample_indices[D3D12_DEBUG_VERTEX_SAMPLE_COUNT];
    unsigned int num_sample_indices;
    float clip_range[4];
    unsigned int aa_width = 1;
    unsigned int aa_height = 1;
    unsigned int surface_width;
    unsigned int surface_height;
    float point_size;
    uint32_t state_key;
    unsigned int num_combiner_stages = MIN(key->combiner_control & 0xff, 8);

    if (key->vertex_shader_mode == 0) {
        return;
    }

    state_key = pipeline_key_hash(key);
    if (state_key_already_reported(
            r->reported_programmable_shader_state_keys,
            &r->num_reported_programmable_shader_state_keys,
            state_key)) {
        return;
    }

    if (key->transform_program_length > 0) {
        first_token = key->transform_program_data[0];
        final_token =
            key->transform_program_data[key->transform_program_length - 1];
        final_slot = start + key->transform_program_length - 1;
    }

    for (unsigned int i = 0; i < key->transform_program_length; i++) {
        const uint32_t *token = key->transform_program_data[i];

        g_string_append_printf(
            decoded_tokens,
            "\n  slot=%u raw={0x%08x,0x%08x,0x%08x,0x%08x} "
            "mac=%s(%u) ilu=%s(%u) out_mux=%u out_orb=%u "
            "out_addr=%u out_r=R%u mac_mask=0x%x ilu_mask=0x%x "
            "out_mask=0x%x final=%u\n",
            start + i, token[0], token[1], token[2], token[3],
            d3d12_vsh_get_field(token, FLD_MAC) <= MAC_ARL ?
                d3d12_vsh_mac_opcode[d3d12_vsh_get_field(token, FLD_MAC)] :
                "INVALID",
            d3d12_vsh_get_field(token, FLD_MAC),
            d3d12_vsh_ilu_opcode[d3d12_vsh_get_field(token, FLD_ILU)],
            d3d12_vsh_get_field(token, FLD_ILU),
            d3d12_vsh_get_field(token, FLD_OUT_MUX),
            d3d12_vsh_get_field(token, FLD_OUT_ORB),
            d3d12_vsh_get_field(token, FLD_OUT_ADDRESS),
            d3d12_vsh_get_field(token, FLD_OUT_R),
            d3d12_vsh_get_field(token, FLD_OUT_MAC_MASK),
            d3d12_vsh_get_field(token, FLD_OUT_ILU_MASK),
            d3d12_vsh_get_field(token, FLD_OUT_O_MASK),
            d3d12_vsh_get_field(token, FLD_FINAL));
        d3d12_vsh_append_token(decoded_tokens, token, start + i);
    }

    pgraph_apply_anti_aliasing_factor(pg, &aa_width, &aa_height);
    surface_width = MAX(pg->surface_binding_dim.width / aa_width, 1);
    surface_height = MAX(pg->surface_binding_dim.height / aa_height, 1);
    point_size = MAX(pgraph_reg_r(pg, NV_PGRAPH_POINTSIZE) / 8.0f, 1.0f) *
                 MAX(pg->surface_scale_factor, 1);
    fill_clip_range(pg, clip_range);
    num_sample_indices = collect_draw_sample_indices(
        pg, sample_indices, ARRAY_SIZE(sample_indices));
    append_draw_batch_details(draw_details, pg, sample_indices,
                              num_sample_indices);

    for (unsigned int i = 0; i < num_combiner_stages; i++) {
        g_string_append_printf(
            combiner_details,
            " [stage=%u rgb_in=0x%08x rgb_out=0x%08x "
            "alpha_in=0x%08x alpha_out=0x%08x]",
            i, key->rgb_inputs[i], key->rgb_outputs[i],
            key->alpha_inputs[i], key->alpha_outputs[i]);
    }

    for (uint8_t i = 0; i < key->num_attributes; i++) {
        uint8_t attr_index = key->attribute_indices[i];
        const VertexAttribute *attr = &pg->vertex_attributes[attr_index];

        g_string_append_printf(
            attr_details,
            " [attr=%u type=%s(%u) count=%u stride=%u dxgi=%u "
            "stream_location=%d uniform=%d swizzled=%d compressed=%d "
            "cpu_expanded=%d "
            "inline=(%f,%f,%f,%f)",
            attr_index, vertex_attribute_format_name(attr->format),
            attr->format, attr->count, attr->stride,
            (unsigned int)key->attribute_formats[i],
            r->vertex_attribute_to_description_location[attr_index],
            !!(key->uniform_attrs & (1u << attr_index)),
            !!(key->swizzle_attrs & (1u << attr_index)),
            !!(key->compressed_attrs & (1u << attr_index)),
            !!(r->converted_vertex_attribute_mask & (1u << attr_index)),
            attr->inline_value[0], attr->inline_value[1],
            attr->inline_value[2], attr->inline_value[3]);
        append_vertex_attr_samples(attr_details, pg, d, attr, attr_index,
                                   sample_indices, num_sample_indices);
        g_string_append(attr_details, "]");
    }

    for (unsigned int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        const PGRAPHD3D12TextureBinding *texture = r->texture_binding[i];

        if (!(key->texture_mask & (1u << i)) || texture == NULL) {
            continue;
        }

        g_string_append_printf(
            texture_details,
            " [stage=%u mode=%s(0x%x) input=%d dot_map=%u format=0x%x "
            "dxgi=%u size=%ux%ux%u linear=%d cubemap=%d texture3d=%d "
            "border_adjust=%d border_logical=%fx%fx%f "
            "border_inv_real=%fx%fx%f depth=%d snorm=%d "
            "key_pitch=%u key_bpp=%u coord_norm=%ux%u "
            "alphakill=%d colorkey=%u convolution=%s(%u) coord_used=%d "
            "sampler_filter=0x%08x "
            "address=0x%08x vram=0x%" HWADDR_PRIx
            "..0x%" HWADDR_PRIx " length=%zu pitch=%u filter=0x%08x "
            "address=0x%08x scale=%f surface_to_texture=%d "
            "source_surface=0x%" HWADDR_PRIx " source_draw_time=%d]",
            i, texture_shader_mode_name(texture_stage_mode(key, i)),
            texture_stage_mode(key, i), texture_stage_input(key, i),
            texture_stage_dot_map(key, i), key->texture_color_format[i],
            (unsigned int)key->texture_dxgi_format[i],
            key->texture_width[i], key->texture_height[i],
            key->texture_depth[i],
            !!(key->texture_linear_mask & (1u << i)),
            !!(key->texture_cubemap_mask & (1u << i)),
            !!(key->texture_3d_mask & (1u << i)),
            !!(key->texture_border_adjust_mask & (1u << i)),
            key->texture_border_logical_size[i][0],
            key->texture_border_logical_size[i][1],
            key->texture_border_logical_size[i][2],
            key->texture_border_inv_real_size[i][0],
            key->texture_border_inv_real_size[i][1],
            key->texture_border_inv_real_size[i][2],
            !!(key->texture_depth_mask & (1u << i)),
            !!(key->texture_snorm_mask & (1u << i)),
            key->texture_pitch[i], key->texture_bytes_per_pixel[i],
            key->texture_width[i], key->texture_height[i],
            !!(key->texture_alphakill_mask & (1u << i)),
            key->texture_colorkey_mode[i],
            texture_convolution_filter_name(
                key->texture_convolution_filter[i]),
            key->texture_convolution_filter[i],
            !!(key->texture_coord_mask & (1u << i)),
            pgraph_reg_r(pg, NV_PGRAPH_TEXFILTER0 + i * 4),
            pgraph_reg_r(pg, NV_PGRAPH_TEXADDRESS0 + i * 4),
            texture->vram_addr,
            texture->vram_addr + texture->length,
            texture->length, texture->shape.pitch,
            pgraph_reg_r(pg, NV_PGRAPH_TEXFILTER0 + i * 4),
            pgraph_reg_r(pg, NV_PGRAPH_TEXADDRESS0 + i * 4),
            r->texture_scale[i], texture->surface_to_texture,
            texture->source_surface_addr,
            texture->source_surface_draw_time);
    }

    fprintf(
        stderr,
        "Direct3D 12 renderer: programmable shader state observed "
        "(%u/%u): state_key=0x%08x, vsh_mode=%u, "
        "transform_program_start=%u, program_length=%u, final_slot=%u, "
        "first_token={0x%08x,0x%08x,0x%08x,0x%08x}, "
        "final_token={0x%08x,0x%08x,0x%08x,0x%08x}, "
        "csv0_c=0x%08x, csv0_d=0x%08x, primitive=%s (%u), "
        "fog={enable:%d,mode:%u,foggen:%u,param=(%f,%f),"
        "plane=(%f,%f,%f,%f)}, "
        "point_params={enable:%d,point_sprite:%d,raw_size:%f,scaled_size:%f,"
        "params=[%f,%f,%f,%f,%f,%f,%f,%f]}, "
        "draw_payload={%s}. "
        "surface=%ux%u, surface_scale=%u, clip=%u,%u %ux%u, "
        "POSITION={type:%s(%u), count:%u, stride:%u, "
        "stream_location:%d, uniform:%d, inline:(%f,%f,%f,%f)}. "
        "Draw constants: c0=(%f,%f,%f,%f), c1=(%f,%f,%f,%f), "
        "c59=(%f,%f,%f,%f), clipRange=(%f,%f,%f,%f), "
        "surfaceInfo=(%u,%u,%u,%f), active_attrs:%s. "
        "Pixel shader: combinectl=0x%08x stages=%u final0=0x%08x "
        "final1=0x%08x combiner:%s. "
        "Textures: mask=0x%x coord_mask=0x%x shaderprog=0x%08x "
        "shaderctl=0x%08x shaderclip=0x%08x "
        "window_clip_exclusive=%d stages:%s. "
        "D3D12 will compile these XVS tokens into the SM 6.4 vertex shader; "
        "if program_length is zero it falls back to screen-space POSITION. "
        "Decoded XVS token stream:%s\n",
        r->num_reported_programmable_shader_state_keys,
        PGRAPH_D3D12_REPORTED_STATE_KEYS, state_key,
        key->vertex_shader_mode, start, key->transform_program_length,
        final_slot,
        first_token ? first_token[0] : 0, first_token ? first_token[1] : 0,
        first_token ? first_token[2] : 0, first_token ? first_token[3] : 0,
        final_token ? final_token[0] : 0, final_token ? final_token[1] : 0,
        final_token ? final_token[2] : 0, final_token ? final_token[3] : 0,
        pgraph_reg_r(pg, NV_PGRAPH_CSV0_C),
        pgraph_reg_r(pg, NV_PGRAPH_CSV0_D),
        primitive_mode_name(pg->primitive_mode), pg->primitive_mode,
        key->fog_enable, key->fog_mode, key->fixed_function_foggen,
        d3d12_u32_as_float(pgraph_reg_r(pg, NV_PGRAPH_FOGPARAM0)),
        d3d12_u32_as_float(pgraph_reg_r(pg, NV_PGRAPH_FOGPARAM1)),
        d3d12_u32_as_float(pg->vsh_constants[NV_IGRAPH_XF_XFCTX_FOG][0]),
        d3d12_u32_as_float(pg->vsh_constants[NV_IGRAPH_XF_XFCTX_FOG][1]),
        d3d12_u32_as_float(pg->vsh_constants[NV_IGRAPH_XF_XFCTX_FOG][2]),
        d3d12_u32_as_float(pg->vsh_constants[NV_IGRAPH_XF_XFCTX_FOG][3]),
        key->point_params_enable, key->point_sprite_enable,
        pgraph_reg_r(pg, NV_PGRAPH_POINTSIZE) / 8.0f,
        MAX(pgraph_reg_r(pg, NV_PGRAPH_POINTSIZE) / 8.0f, 1.0f) *
            MAX(pg->surface_scale_factor, 1),
        pg->point_params[0], pg->point_params[1], pg->point_params[2],
        pg->point_params[3], pg->point_params[4], pg->point_params[5],
        pg->point_params[6], pg->point_params[7],
        draw_details->str,
        pg->surface_binding_dim.width, pg->surface_binding_dim.height,
        MAX(pg->surface_scale_factor, 1), pg->surface_shape.clip_x,
        pg->surface_shape.clip_y, pg->surface_shape.clip_width,
        pg->surface_shape.clip_height,
        vertex_attribute_format_name(position->format), position->format,
        position->count, position->stride, position_location,
        !!(key->uniform_attrs & (1u << NV2A_VERTEX_ATTR_POSITION)),
        position->inline_value[0], position->inline_value[1],
        position->inline_value[2], position->inline_value[3],
        d3d12_u32_as_float(pg->vsh_constants[0][0]),
        d3d12_u32_as_float(pg->vsh_constants[0][1]),
        d3d12_u32_as_float(pg->vsh_constants[0][2]),
        d3d12_u32_as_float(pg->vsh_constants[0][3]),
        d3d12_u32_as_float(pg->vsh_constants[1][0]),
        d3d12_u32_as_float(pg->vsh_constants[1][1]),
        d3d12_u32_as_float(pg->vsh_constants[1][2]),
        d3d12_u32_as_float(pg->vsh_constants[1][3]),
        d3d12_u32_as_float(pg->vsh_constants[59][0]),
        d3d12_u32_as_float(pg->vsh_constants[59][1]),
        d3d12_u32_as_float(pg->vsh_constants[59][2]),
        d3d12_u32_as_float(pg->vsh_constants[59][3]),
        clip_range[0], clip_range[1], clip_range[2], clip_range[3],
        surface_width, surface_height, MAX(pg->surface_scale_factor, 1),
        point_size, attr_details->str,
        key->combiner_control, num_combiner_stages,
        key->final_inputs_0, key->final_inputs_1,
        combiner_details->str, key->texture_mask,
        key->texture_coord_mask, key->shader_stage_program,
        key->other_stage_input, key->shader_clip_mode,
        key->window_clip_exclusive, texture_details->str,
        decoded_tokens->str);
}

static bool ensure_constant_upload_space(PGRAPHState *pg, bool draw_constants,
                                         Error **errp)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;

    if (r == NULL || r->uniform_upload_buffer.mapped == NULL) {
        error_setg(errp, "Direct3D 12 uniform upload buffer is not mapped");
        return false;
    }

    /* Constant uploads allocate from the fence-tracked ring themselves. */
    (void)draw_constants;
    return true;
}

static bool upload_draw_constants(PGRAPHState *pg, uint64_t *offset,
                                  Error **errp)
{
    PGRAPHD3D12DrawConstants constants;
    const void *chunks[] = { &constants };
    const uint64_t sizes[] = { sizeof(constants) };
    unsigned int aa_width = 1;
    unsigned int aa_height = 1;

    memset(&constants, 0, sizeof(constants));
    /* Shaders read inline values only for uniform attributes; streamed
     * attributes carry the provoking vertex here, which would defeat the
     * constant cache on every draw. */
    for (unsigned int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
        if (!(pg->uniform_attrs & (1u << i))) {
            continue;
        }
        memcpy(constants.inline_value[i], pg->vertex_attributes[i].inline_value,
               sizeof(constants.inline_value[i]));
    }
    memcpy(constants.vsh_constants, pg->vsh_constants,
           sizeof(constants.vsh_constants));
    memcpy(constants.ltctxa, pg->ltctxa, sizeof(pg->ltctxa));
    memcpy(constants.ltctxb, pg->ltctxb, sizeof(pg->ltctxb));
    memcpy(constants.ltc1, pg->ltc1, sizeof(pg->ltc1));
    memcpy(constants.point_params, pg->point_params, sizeof(pg->point_params));
    for (unsigned int i = 0; i < NV2A_MAX_LIGHTS; i++) {
        memcpy(constants.light_infinite_direction[i],
               pg->light_infinite_direction[i],
               sizeof(pg->light_infinite_direction[i]));
        memcpy(constants.light_infinite_half_vector[i],
               pg->light_infinite_half_vector[i],
               sizeof(pg->light_infinite_half_vector[i]));
        memcpy(constants.light_local_position[i],
               pg->light_local_position[i],
               sizeof(pg->light_local_position[i]));
        memcpy(constants.light_local_attenuation[i],
               pg->light_local_attenuation[i],
               sizeof(pg->light_local_attenuation[i]));
    }
    fill_clip_range(pg, constants.clip_range);

    pgraph_apply_anti_aliasing_factor(pg, &aa_width, &aa_height);
    /* The binding may be a non-multiple of the AA factor (odd clip offset),
     * so the guest size can be fractional. */
    constants.surface_info[0] =
        MAX((float)pg->surface_binding_dim.width / aa_width, 1.0f);
    constants.surface_info[1] =
        MAX((float)pg->surface_binding_dim.height / aa_height, 1.0f);
    constants.surface_info[2] = (float)MAX(pg->surface_scale_factor, 1);
    /* Register point size in guest pixels; each vertex shader path applies
     * its own minimum and the scaled viewport applies the scale once. */
    constants.surface_info[3] =
        pgraph_reg_r(pg, NV_PGRAPH_POINTSIZE) / 8.0f;
    constants.fog_param[0] =
        d3d12_u32_as_float(pgraph_reg_r(pg, NV_PGRAPH_FOGPARAM0));
    constants.fog_param[1] =
        d3d12_u32_as_float(pgraph_reg_r(pg, NV_PGRAPH_FOGPARAM1));
    constants.fixed_function_info[0] = pg->material_alpha;
    constants.fixed_function_info[1] = pg->specular_power;
    constants.fixed_function_info[2] = pg->specular_power_back;

    if (pgraph_d3d12_vertex_pulling()) {
        PGRAPHD3D12State *r = pg->d3d12_renderer_state;

        for (unsigned int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
            bool converted = r->converted_vertex_attribute_mask & (1u << i);
            bool inline_attr = r->vertex_fetch_inline_mask & (1u << i);
            int loc = r->vertex_attribute_to_description_location[i];
            uint32_t format_bits;
            uint64_t base;

            if (loc < 0) {
                continue;
            }
            /* The same sources as the pipeline key's format fields; set
             * in every mode, the static decode only tests the aligned bit. */
            format_bits = vertex_fetch_format_bits(
                r->vertex_attribute_descriptions[loc].Format,
                pg->compressed_attrs & (1u << i),
                pg->swizzle_attrs & (1u << i));
            if (converted) {
                base = r->vertex_attribute_offsets[i];
                constants.vertex_fetch[i][0] = 1;
            } else if (inline_attr) {
                base = r->vertex_fetch_ring_base +
                       r->vertex_attribute_offsets[i];
                constants.vertex_fetch[i][0] = 1;
            } else {
                base = r->vertex_attribute_offsets[i];
                constants.vertex_fetch[i][0] = 0;
            }
            constants.vertex_fetch[i][1] = (uint32_t)base;
            constants.vertex_fetch[i][2] = r->vertex_attribute_strides[i];
            constants.vertex_fetch[i][3] =
                (((base | r->vertex_attribute_strides[i]) & 3) == 0 ?
                     VERTEX_FETCH_ALIGNED : 0) |
                format_bits;
        }
    }

    {
        PGRAPHD3D12State *r = pg->d3d12_renderer_state;

        if (r->draw_constants_cache == NULL) {
            r->draw_constants_cache = g_malloc(sizeof(constants));
            r->draw_constants_cache_size = sizeof(constants);
        }
        if (r->draw_constants_cached &&
            memcmp(r->draw_constants_cache, &constants,
                   sizeof(constants)) == 0) {
            *offset = r->draw_constants_cache_offset;
            return true;
        }
    }

    if (!pgraph_d3d12_update_uniform_buffer(
            pg, chunks, sizes, ARRAY_SIZE(chunks),
            PGRAPH_D3D12_DRAW_CONSTANTS_ALIGNMENT, offset, errp)) {
        error_prepend(errp,
                      "Direct3D 12 draw constants upload failed for "
                      "uniform_attrs=0x%04x, swizzle_attrs=0x%04x, "
                      "vsh_mode=%u, transform_program_start=%u, "
                      "surface=%ux%u, aa=%ux%u, zeta_format=0x%x: ",
                      pg->uniform_attrs, pg->swizzle_attrs,
                      GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CSV0_D),
                               NV_PGRAPH_CSV0_D_MODE),
                      GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CSV0_C),
                               NV_PGRAPH_CSV0_C_CHEOPS_PROGRAM_START),
                      pg->surface_binding_dim.width,
                      pg->surface_binding_dim.height, aa_width, aa_height,
                      pg->surface_shape.zeta_format);
        return false;
    }

    {
        PGRAPHD3D12State *r = pg->d3d12_renderer_state;

        memcpy(r->draw_constants_cache, &constants, sizeof(constants));
        r->draw_constants_cache_offset = *offset;
        r->draw_constants_cached = true;
    }

    return true;
}

static bool upload_pixel_constants(PGRAPHState *pg, uint64_t *offset,
                                   Error **errp)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    PGRAPHD3D12PixelConstants constants;
    const void *chunks[] = { &constants };
    const uint64_t sizes[] = { sizeof(constants) };
    uint32_t fog_color;

    memset(&constants, 0, sizeof(constants));
    for (unsigned int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        constants.tex_scale[i] = 1.0f;
    }
    for (unsigned int i = 0; i < 9; i++) {
        uint32_t c0 = i == 8 ?
            pgraph_reg_r(pg, NV_PGRAPH_SPECFOGFACTOR0) :
            pgraph_reg_r(pg, NV_PGRAPH_COMBINEFACTOR0 + i * 4);
        uint32_t c1 = i == 8 ?
            pgraph_reg_r(pg, NV_PGRAPH_SPECFOGFACTOR1) :
            pgraph_reg_r(pg, NV_PGRAPH_COMBINEFACTOR1 + i * 4);

        pgraph_argb_pack32_to_rgba_float(c0, constants.combiner_const[i * 2]);
        pgraph_argb_pack32_to_rgba_float(c1,
                                         constants.combiner_const[i * 2 + 1]);
    }
    for (unsigned int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        uint32_t texfmt = pgraph_reg_r(pg, NV_PGRAPH_TEXFMT0 + i * 4);
        unsigned int color_format = GET_MASK(texfmt, NV_PGRAPH_TEXFMT0_COLOR);

        constants.color_key[i] =
            pgraph_reg_r(pg, NV_PGRAPH_COLORKEYCOLOR0 + i * 4);
        constants.color_key_mask[i] = texture_color_key_mask(color_format);
    }
    for (unsigned int i = 1; i < NV2A_MAX_TEXTURES; i++) {
        uint32_t m00 = pgraph_reg_r(pg, NV_PGRAPH_BUMPMAT00 + (i - 1) * 4);
        uint32_t m01 = pgraph_reg_r(pg, NV_PGRAPH_BUMPMAT01 + (i - 1) * 4);
        uint32_t m10 = pgraph_reg_r(pg, NV_PGRAPH_BUMPMAT10 + (i - 1) * 4);
        uint32_t m11 = pgraph_reg_r(pg, NV_PGRAPH_BUMPMAT11 + (i - 1) * 4);
        uint32_t scale = pgraph_reg_r(pg, NV_PGRAPH_BUMPSCALE1 +
                                          (i - 1) * 4);
        uint32_t bump_offset = pgraph_reg_r(pg, NV_PGRAPH_BUMPOFFSET1 +
                                                (i - 1) * 4);

        constants.bump_mat[i][0] = *(float *)&m00;
        constants.bump_mat[i][1] = *(float *)&m01;
        constants.bump_mat[i][2] = *(float *)&m10;
        constants.bump_mat[i][3] = *(float *)&m11;
        constants.bump_scale[i] = *(float *)&scale;
        constants.bump_offset[i] = *(float *)&bump_offset;
    }
    for (unsigned int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        if ((r->texture_active_mask & (1u << i)) &&
            r->texture_binding[i] != NULL && r->texture_binding[i]->valid) {
            constants.tex_scale[i] = MAX(r->texture_scale[i], 1.0f);
        }
        if (r->texture_binding[i] != NULL) {
            constants.tex_size[i][0] =
                (float)r->texture_binding[i]->shape.width;
            constants.tex_size[i][1] =
                (float)r->texture_binding[i]->shape.height;
        }
    }

    fog_color = pgraph_reg_r(pg, NV_PGRAPH_FOGCOLOR);
    constants.fog_color[0] =
        GET_MASK(fog_color, NV_PGRAPH_FOGCOLOR_RED) / 255.0f;
    constants.fog_color[1] =
        GET_MASK(fog_color, NV_PGRAPH_FOGCOLOR_GREEN) / 255.0f;
    constants.fog_color[2] =
        GET_MASK(fog_color, NV_PGRAPH_FOGCOLOR_BLUE) / 255.0f;
    constants.fog_color[3] =
        GET_MASK(fog_color, NV_PGRAPH_FOGCOLOR_ALPHA) / 255.0f;
    pgraph_argb_pack32_to_rgba_float(pgraph_reg_r(pg, NV_PGRAPH_BLENDCOLOR),
                                     constants.blend_color);
    fill_clip_range(pg, constants.clip_range);
    fill_depth_info(pg, constants.depth_info);
    fill_clip_regions(pg, constants.clip_region);
    {
        uint32_t alpha_func;

        /* As fill_alpha_test_key(); an invalid function skipped the draw. */
        if (!get_alpha_test_func(pg, &alpha_func) ||
            (r->tune_flags & PGRAPH_D3D12_TUNE_ATEST_OFF)) {
            alpha_func = 7;
        }
        /* ALWAYS ignores the reference; zero keeps the cache hit. */
        if (alpha_func != 7) {
            constants.alpha_test[0] =
                GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0),
                         NV_PGRAPH_CONTROL_0_ALPHAREF);
        }
        constants.alpha_test[1] = alpha_func;
        constants.alpha_test[2] = get_alphakill_mask(pg);
        constants.alpha_test[3] =
            GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_SHADOWCTL),
                     NV_PGRAPH_SHADOWCTL_SHADOW_ZFUNC);
    }

    if (r->pixel_constants_cache == NULL) {
        r->pixel_constants_cache = g_malloc(sizeof(constants));
        r->pixel_constants_cache_size = sizeof(constants);
    }
    if (r->pixel_constants_cached &&
        memcmp(r->pixel_constants_cache, &constants,
               sizeof(constants)) == 0) {
        *offset = r->pixel_constants_cache_offset;
        return true;
    }

    if (!pgraph_d3d12_update_uniform_buffer(
            pg, chunks, sizes, ARRAY_SIZE(chunks),
            PGRAPH_D3D12_DRAW_CONSTANTS_ALIGNMENT, offset, errp)) {
        error_prepend(errp,
                      "Direct3D 12 pixel constants upload failed for "
                      "combinectl=0x%08x, shaderprog=0x%08x: ",
                      pgraph_reg_r(pg, NV_PGRAPH_COMBINECTL),
                      pgraph_reg_r(pg, NV_PGRAPH_SHADERPROG));
        return false;
    }

    memcpy(r->pixel_constants_cache, &constants, sizeof(constants));
    r->pixel_constants_cache_offset = *offset;
    r->pixel_constants_cached = true;
    return true;
}

/* The b3 constants of a draw with an interpreter pipeline; they follow
 * from its key alone, so they upload when the key changes. The vertex
 * block is for fallback pipelines. */
static bool upload_uber_constants(PGRAPHState *pg, uint64_t *offset,
                                  Error **errp)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    PGRAPHD3D12UberConstants *constants;
    bool vs = r->draw_pipeline_fallback;
    const void *chunks[1];
    uint64_t sizes[1];

    if (r->uber_constants_cached && r->uber_constants_vs == vs &&
        memcmp(&r->uber_constants_key, &r->draw_pipeline_key,
               sizeof(r->draw_pipeline_key)) == 0) {
        *offset = r->uber_constants_offset;
        return true;
    }
    constants = g_new0(PGRAPHD3D12UberConstants, 1);
    fill_uber_ps_constants(&r->draw_pipeline_key, &constants->ps);
    if (vs) {
        constants->ps.control[3] = uber_cull_bits(&r->draw_pipeline_key);
        fill_uber_vs_constants(&r->draw_pipeline_key, &constants->vs);
    }
    chunks[0] = constants;
    sizes[0] = vs ? sizeof(*constants) : sizeof(constants->ps);
    if (!pgraph_d3d12_update_uniform_buffer(
            pg, chunks, sizes, ARRAY_SIZE(chunks),
            PGRAPH_D3D12_DRAW_CONSTANTS_ALIGNMENT, offset, errp)) {
        g_free(constants);
        error_prepend(errp, "Direct3D 12 interpreter constants upload "
                            "failed: ");
        return false;
    }
    g_free(constants);
    r->uber_constants_key = r->draw_pipeline_key;
    r->uber_constants_vs = vs;
    r->uber_constants_offset = *offset;
    r->uber_constants_cached = true;
    return true;
}

/* Diagnostic: the draw's shaders and the constants the GPU will read. */
static void capture_draw_shaders_and_constants(PGRAPHState *pg,
                                               uint64_t offset)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    const PGRAPHD3D12DrawConstants *dc;
    uint64_t key_hash;
    g_autofree char *dir = NULL;
    g_autofree char *vs_path = NULL;
    GString *line;

    if (r->capture_frames_left <= 0) {
        return;
    }
    key_hash = fast_hash((const uint8_t *)&r->draw_pipeline_key,
                         sizeof(r->draw_pipeline_key));
    dir = g_strdup_printf("%scapdump", xemu_settings_get_base_path());
    vs_path = g_strdup_printf("%s/shader_%016" PRIx64 "_vs.hlsl", dir,
                              key_hash);
    if (!g_file_test(vs_path, G_FILE_TEST_EXISTS)) {
        char *vs = build_minimal_vertex_shader(&r->draw_pipeline_key);
        char *ps = build_minimal_pixel_shader(&r->draw_pipeline_key);
        char *gs = build_draw_geometry_shader(&r->draw_pipeline_key);
        g_autofree char *ps_path = g_strdup_printf(
            "%s/shader_%016" PRIx64 "_ps.hlsl", dir, key_hash);
        g_autofree char *gs_path = g_strdup_printf(
            "%s/shader_%016" PRIx64 "_gs.hlsl", dir, key_hash);

        g_mkdir_with_parents(dir, 0755);
        if (vs) {
            g_file_set_contents(vs_path, vs, -1, NULL);
        }
        if (ps) {
            g_file_set_contents(ps_path, ps, -1, NULL);
        }
        if (gs) {
            g_file_set_contents(gs_path, gs, -1, NULL);
        }
        g_free(vs);
        g_free(ps);
        g_free(gs);
    }

    dc = (const PGRAPHD3D12DrawConstants *)
        ((const uint8_t *)r->uniform_upload_buffer.mapped + offset);
    line = g_string_new(NULL);
    g_string_append_printf(line, "CAP consts key=%016" PRIx64 " off=0x%"
                           PRIx64 " T0MAT=", key_hash, offset);
    for (unsigned int row = 0; row < 4; row++) {
        const float *m = dc->vsh_constants[NV_IGRAPH_XF_XFCTX_T0MAT + row];

        g_string_append_printf(line, "[%g,%g,%g,%g]", m[0], m[1], m[2], m[3]);
    }
    g_string_append(line, " fetch=");
    for (unsigned int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
        if (dc->vertex_fetch[i][2] == 0 && dc->vertex_fetch[i][1] == 0) {
            continue;
        }
        g_string_append_printf(line, "a%u(sel%u,0x%x,st%u,w0x%x) ", i,
                               dc->vertex_fetch[i][0], dc->vertex_fetch[i][1],
                               dc->vertex_fetch[i][2], dc->vertex_fetch[i][3]);
    }
    g_string_append_printf(line, "surf=(%g,%g,%g,%g)", dc->surface_info[0],
                           dc->surface_info[1], dc->surface_info[2],
                           dc->surface_info[3]);
    fprintf(stderr, "%s\n", line->str);
    g_string_free(line, TRUE);
}

static void record_draw_constants(PGRAPHState *pg, uint64_t offset)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    D3D12_GPU_VIRTUAL_ADDRESS address;

    if (!r->draw_pipeline_key_valid) {
        return;
    }
    capture_draw_shaders_and_constants(pg, offset);

    r->perf.draws++;
    if (pgraph_d3d12_vertex_pulling() &&
        r->vertex_ram_buffer.resource != NULL &&
        r->vertex_inline_upload_buffer.resource != NULL) {
        D3D12_GPU_VIRTUAL_ADDRESS vertex_ram_address =
            r->vertex_ram_buffer.resource->lpVtbl->GetGPUVirtualAddress(
                r->vertex_ram_buffer.resource);
        D3D12_GPU_VIRTUAL_ADDRESS vertex_scratch_address =
            r->vertex_inline_upload_buffer.resource->lpVtbl->
                GetGPUVirtualAddress(r->vertex_inline_upload_buffer.resource);

        /* Copies leave the buffer in COPY_DEST. */
        pgraph_d3d12_transition_resource(
            r, &r->vertex_ram_buffer,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        /* Growth changes the GPU address, so compare addresses, not
         * resource pointers. */
        if (r->record_vertex_ram_srv != vertex_ram_address) {
            r->command_list->lpVtbl->SetGraphicsRootShaderResourceView(
                r->command_list, PGRAPH_D3D12_ROOT_VERTEX_RAM_SRV,
                vertex_ram_address);
            r->record_vertex_ram_srv = vertex_ram_address;
        }
        if (r->record_vertex_scratch_srv != vertex_scratch_address) {
            r->command_list->lpVtbl->SetGraphicsRootShaderResourceView(
                r->command_list, PGRAPH_D3D12_ROOT_VERTEX_SCRATCH_SRV,
                vertex_scratch_address);
            r->record_vertex_scratch_srv = vertex_scratch_address;
        }
    }

    address = r->uniform_upload_buffer.resource->lpVtbl->GetGPUVirtualAddress(
        r->uniform_upload_buffer.resource) + offset;
    r->command_list->lpVtbl->SetGraphicsRootConstantBufferView(
        r->command_list, PGRAPH_D3D12_ROOT_DRAW_CONSTANTS, address);
}

static void record_pixel_constants(PGRAPHState *pg, uint64_t offset)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    D3D12_GPU_VIRTUAL_ADDRESS address;

    address = r->uniform_upload_buffer.resource->lpVtbl->GetGPUVirtualAddress(
        r->uniform_upload_buffer.resource) + offset;
    r->command_list->lpVtbl->SetGraphicsRootConstantBufferView(
        r->command_list, PGRAPH_D3D12_ROOT_PIXEL_CONSTANTS, address);
}

static void record_blend_factor(PGRAPHState *pg)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    float blend_factor[4];
    float alpha;

    if (!r->draw_pipeline_key_valid ||
        r->draw_pipeline_key.blend_constant_mode ==
            PGRAPH_D3D12_BLEND_CONSTANT_NONE) {
        return;
    }

    pgraph_argb_pack32_to_rgba_float(pgraph_reg_r(pg, NV_PGRAPH_BLENDCOLOR),
                                     blend_factor);
    if (r->draw_pipeline_key.blend_constant_mode ==
        PGRAPH_D3D12_BLEND_CONSTANT_ALPHA) {
        alpha = blend_factor[3];
        blend_factor[0] = alpha;
        blend_factor[1] = alpha;
        blend_factor[2] = alpha;
    }

    if (r->record_blend_factor_valid &&
        memcmp(r->record_blend_factor, blend_factor,
               sizeof(blend_factor)) == 0) {
        return;
    }

    r->command_list->lpVtbl->OMSetBlendFactor(r->command_list, blend_factor);
    memcpy(r->record_blend_factor, blend_factor, sizeof(blend_factor));
    r->record_blend_factor_valid = true;
}

static void record_viewport_and_scissor(PGRAPHState *pg)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    unsigned int vp_width = MAX(pg->surface_binding_dim.width, 1);
    unsigned int vp_height = MAX(pg->surface_binding_dim.height, 1);
    unsigned int xmin = pg->surface_shape.clip_x;
    unsigned int ymin = pg->surface_shape.clip_y;
    unsigned int scissor_width = pg->surface_shape.clip_width;
    unsigned int scissor_height = pg->surface_shape.clip_height;
    D3D12_VIEWPORT viewport;
    D3D12_RECT scissor;

    pgraph_apply_scaling_factor(pg, &vp_width, &vp_height);

    pgraph_apply_anti_aliasing_factor(pg, &xmin, &ymin);
    pgraph_apply_anti_aliasing_factor(pg, &scissor_width, &scissor_height);
    pgraph_apply_scaling_factor(pg, &xmin, &ymin);
    pgraph_apply_scaling_factor(pg, &scissor_width, &scissor_height);

    viewport = (D3D12_VIEWPORT){
        .TopLeftX = 0.0f,
        .TopLeftY = 0.0f,
        .Width = (FLOAT)vp_width,
        .Height = (FLOAT)vp_height,
        /* The pixel shader applies the guest clip range to fragment
         * depth; shader depth is only clamped to [0, 1]. */
        .MinDepth = 0.0f,
        .MaxDepth = 1.0f,
    };
    scissor = (D3D12_RECT){
        .left = (LONG)xmin,
        .top = (LONG)ymin,
        .right = (LONG)(xmin + scissor_width),
        .bottom = (LONG)(ymin + scissor_height),
    };

    if (r->record_viewport_valid &&
        memcmp(&r->record_viewport, &viewport, sizeof(viewport)) == 0 &&
        memcmp(&r->record_scissor, &scissor, sizeof(scissor)) == 0) {
        return;
    }

    r->command_list->lpVtbl->RSSetViewports(r->command_list, 1, &viewport);
    r->command_list->lpVtbl->RSSetScissorRects(r->command_list, 1, &scissor);
    r->record_viewport = viewport;
    r->record_scissor = scissor;
    r->record_viewport_valid = true;
}

/*
 * Colour clears restricted to a subset of channels: a triangle covering the
 * target whose blended output is the blend factor (the clear colour), with
 * the render-target write mask limited to the requested channels. The
 * scissor bounds it to the clear rectangle.
 */
static const char masked_clear_shader_source[] =
    "float4 vs_main(uint vertex_id : SV_VertexID) : SV_Position\n"
    "{\n"
    "    float2 uv = float2((vertex_id << 1) & 2, vertex_id & 2);\n"
    "    return float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);\n"
    "}\n"
    "float4 ps_main() : SV_Target\n"
    "{\n"
    "    return float4(1.0, 1.0, 1.0, 1.0);\n"
    "}\n";

ID3D12PipelineState *pgraph_d3d12_get_masked_clear_pipeline(
    PGRAPHState *pg, DXGI_FORMAT format, UINT8 write_mask, Error **errp)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc;
    D3D12_RENDER_TARGET_BLEND_DESC *rt_blend;
    IDxcCompiler3 *compiler = NULL;
    g_autoptr(GBytes) vs_bytes = NULL;
    g_autoptr(GBytes) ps_bytes = NULL;
    ID3D12PipelineState *pipeline_state = NULL;
    gsize bytecode_size;
    unsigned int count = r->masked_clear_pipeline_count;
    HRESULT hr;

    for (unsigned int i = 0; i < count; i++) {
        if (r->masked_clear_pipelines[i].format == format &&
            r->masked_clear_pipelines[i].write_mask == write_mask) {
            /* A NULL entry records a failed creation; do not retry. */
            if (r->masked_clear_pipelines[i].pipeline_state == NULL) {
                error_setg(errp,
                           "Direct3D 12 masked clear PSO creation failed "
                           "earlier for DXGI format %u, write mask 0x%x",
                           (unsigned int)format, write_mask);
            }
            return r->masked_clear_pipelines[i].pipeline_state;
        }
    }
    if (r->draw_root_signature == NULL ||
        count == ARRAY_SIZE(r->masked_clear_pipelines)) {
        error_setg(errp,
                   "Direct3D 12 has no masked clear pipeline slot for "
                   "DXGI format %u, write mask 0x%x",
                   (unsigned int)format, write_mask);
        return NULL;
    }

    if (!get_shader_bytecode(pg, &compiler, masked_clear_shader_source,
                             L"vs_main", L"vs_6_4", false, &vs_bytes, NULL,
                             errp) ||
        !get_shader_bytecode(pg, &compiler, masked_clear_shader_source,
                             L"ps_main", L"ps_6_4", false, &ps_bytes, NULL,
                             errp)) {
        D3D12_RELEASE(compiler);
        return NULL;
    }
    D3D12_RELEASE(compiler);

    memset(&desc, 0, sizeof(desc));
    desc.pRootSignature = r->draw_root_signature;
    desc.VS.pShaderBytecode = g_bytes_get_data(vs_bytes, &bytecode_size);
    desc.VS.BytecodeLength = bytecode_size;
    desc.PS.pShaderBytecode = g_bytes_get_data(ps_bytes, &bytecode_size);
    desc.PS.BytecodeLength = bytecode_size;

    rt_blend = &desc.BlendState.RenderTarget[0];
    rt_blend->BlendEnable = TRUE;
    rt_blend->LogicOpEnable = FALSE;
    rt_blend->SrcBlend = D3D12_BLEND_BLEND_FACTOR;
    rt_blend->DestBlend = D3D12_BLEND_ZERO;
    rt_blend->BlendOp = D3D12_BLEND_OP_ADD;
    rt_blend->SrcBlendAlpha = D3D12_BLEND_BLEND_FACTOR;
    rt_blend->DestBlendAlpha = D3D12_BLEND_ZERO;
    rt_blend->BlendOpAlpha = D3D12_BLEND_OP_ADD;
    rt_blend->LogicOp = D3D12_LOGIC_OP_NOOP;
    rt_blend->RenderTargetWriteMask = write_mask;

    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.RasterizerState.ConservativeRaster =
        D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    desc.DepthStencilState.DepthEnable = FALSE;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    desc.DepthStencilState.StencilEnable = FALSE;
    desc.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = format;
    desc.DSVFormat = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

    hr = r->device->lpVtbl->CreateGraphicsPipelineState(
        r->device, &desc, &IID_ID3D12PipelineState, (void **)&pipeline_state);
    if (FAILED(hr)) {
        pgraph_d3d12_report_info_queue(r,
                                       "masked clear pipeline state creation");
        error_setg(errp,
                   "Failed to create Direct3D 12 masked clear PSO: HRESULT "
                   "0x%08lx, DXGI format %u, write mask 0x%x",
                   (unsigned long)hr, (unsigned int)format, write_mask);
        pipeline_state = NULL;
    }

    r->masked_clear_pipelines[count].format = format;
    r->masked_clear_pipelines[count].write_mask = write_mask;
    r->masked_clear_pipelines[count].pipeline_state = pipeline_state;
    r->masked_clear_pipeline_count = count + 1;
    return pipeline_state;
}

/*
 * Z24S8 render targets sampled through a 32-bit colour format: rebuilds each
 * guest word, (depth << 8) | stencil, from the depth and stencil planes
 * (texture stages 0 and 1) and writes its bytes in memory order to an RGBA8
 * or BGRA8 target.
 */
static const char zeta_view_shader_source[] =
    "Texture2D<float> zeta_depth : register(t0);\n"
    "Texture2D<uint2> zeta_stencil : register(t1);\n"
    "float4 vs_main(uint vertex_id : SV_VertexID) : SV_Position\n"
    "{\n"
    "    float2 uv = float2((vertex_id << 1) & 2, vertex_id & 2);\n"
    "    return float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);\n"
    "}\n"
    "float4 ps_main(float4 position : SV_Position) : SV_Target\n"
    "{\n"
    "    int3 texel = int3(position.xy, 0);\n"
    "    uint depth = (uint)round(saturate(zeta_depth.Load(texel)) *\n"
    "                             16777215.0);\n"
    "    uint word = (depth << 8) | (zeta_stencil.Load(texel).y & 0xff);\n"
    "    float4 bytes = float4(word & 0xff, (word >> 8) & 0xff,\n"
    "                          (word >> 16) & 0xff, word >> 24) / 255.0;\n"
    "    return bytes.ZETA_VIEW_ORDER;\n"
    "}\n";

ID3D12PipelineState *pgraph_d3d12_get_zeta_view_pipeline(
    PGRAPHState *pg, DXGI_FORMAT format, Error **errp)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc;
    IDxcCompiler3 *compiler = NULL;
    g_autoptr(GBytes) vs_bytes = NULL;
    g_autoptr(GBytes) ps_bytes = NULL;
    ID3D12PipelineState *pipeline_state = NULL;
    g_autofree char *source = NULL;
    gsize bytecode_size;
    unsigned int slot;
    HRESULT hr;

    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
        slot = 0;
        break;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
        slot = 1;
        break;
    default:
        error_setg(errp, "Direct3D 12 zeta view has no pipeline for DXGI "
                   "format %u", (unsigned int)format);
        return NULL;
    }
    if (r->zeta_view_pipelines[slot] != NULL ||
        r->zeta_view_pipeline_failed[slot]) {
        if (r->zeta_view_pipelines[slot] == NULL) {
            error_setg(errp, "Direct3D 12 zeta view PSO creation failed "
                       "earlier for DXGI format %u", (unsigned int)format);
        }
        return r->zeta_view_pipelines[slot];
    }
    if (r->draw_root_signature == NULL) {
        error_setg(errp, "Direct3D 12 zeta view has no root signature");
        return NULL;
    }

    /* Memory byte order: RGBA8 stores r first, BGRA8 stores b first. */
    source = g_strdup_printf("#define ZETA_VIEW_ORDER %s\n%s",
                             slot == 0 ? "xyzw" : "zyxw",
                             zeta_view_shader_source);
    r->zeta_view_pipeline_failed[slot] = true;
    if (!get_shader_bytecode(pg, &compiler, source, L"vs_main", L"vs_6_4",
                             false, &vs_bytes, NULL, errp) ||
        !get_shader_bytecode(pg, &compiler, source, L"ps_main", L"ps_6_4",
                             false, &ps_bytes, NULL, errp)) {
        D3D12_RELEASE(compiler);
        return NULL;
    }
    D3D12_RELEASE(compiler);

    memset(&desc, 0, sizeof(desc));
    desc.pRootSignature = r->draw_root_signature;
    desc.VS.pShaderBytecode = g_bytes_get_data(vs_bytes, &bytecode_size);
    desc.VS.BytecodeLength = bytecode_size;
    desc.PS.pShaderBytecode = g_bytes_get_data(ps_bytes, &bytecode_size);
    desc.PS.BytecodeLength = bytecode_size;
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask =
        D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.BlendState.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_NOOP;
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.RasterizerState.ConservativeRaster =
        D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    desc.DepthStencilState.DepthEnable = FALSE;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    desc.DepthStencilState.StencilEnable = FALSE;
    desc.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = format;
    desc.DSVFormat = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

    hr = r->device->lpVtbl->CreateGraphicsPipelineState(
        r->device, &desc, &IID_ID3D12PipelineState, (void **)&pipeline_state);
    if (FAILED(hr)) {
        pgraph_d3d12_report_info_queue(r, "zeta view pipeline state creation");
        error_setg(errp,
                   "Failed to create Direct3D 12 zeta view PSO: HRESULT "
                   "0x%08lx, DXGI format %u",
                   (unsigned long)hr, (unsigned int)format);
        return NULL;
    }
    r->zeta_view_pipeline_failed[slot] = false;
    r->zeta_view_pipelines[slot] = pipeline_state;
    return pipeline_state;
}

void pgraph_d3d12_init_pipelines(PGRAPHState *pg, Error **errp)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    DrawRootSignatureDesc rs;
    ID3DBlob *signature = NULL;
    ID3DBlob *error_blob = NULL;
    uint32_t request;
    HRESULT hr;

    fill_draw_root_signature_desc(&rs, true);
    hr = D3D12SerializeRootSignature(&rs.desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                     &signature, &error_blob);
    if (FAILED(hr)) {
        error_setg(errp,
                   "Failed to serialize Direct3D 12 root signature: HRESULT 0x%08lx",
                   (unsigned long)hr);
        D3D12_RELEASE(error_blob);
        return;
    }

    hr = r->device->lpVtbl->CreateRootSignature(
        r->device, 0, signature->lpVtbl->GetBufferPointer(signature),
        signature->lpVtbl->GetBufferSize(signature), &IID_ID3D12RootSignature,
        (void **)&r->draw_root_signature);
    D3D12_RELEASE(signature);
    D3D12_RELEASE(error_blob);
    if (FAILED(hr)) {
        error_setg(errp,
                   "Failed to create Direct3D 12 root signature: HRESULT 0x%08lx",
                   (unsigned long)hr);
        return;
    }

    {
        /* For the stages an interpreter pixel shader leaves unbound; without
         * a free descriptor such draws keep their stale tables. */
        D3D12_SHADER_RESOURCE_VIEW_DESC null_desc = {
            .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
            .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
            .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
            .Texture2D.MipLevels = 1,
        };
        D3D12_CPU_DESCRIPTOR_HANDLE cpu;
        uint32_t index;

        r->uber_null_srv_gpu.ptr = 0;
        if (pgraph_d3d12_alloc_srv_descriptor(r, &index, NULL)) {
            r->cbv_srv_uav_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(
                r->cbv_srv_uav_heap, &cpu);
            r->cbv_srv_uav_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(
                r->cbv_srv_uav_heap, &r->uber_null_srv_gpu);
            cpu.ptr += (SIZE_T)index * r->cbv_srv_uav_descriptor_size;
            r->uber_null_srv_gpu.ptr +=
                (UINT64)index * r->cbv_srv_uav_descriptor_size;
            r->device->lpVtbl->CreateShaderResourceView(r->device, NULL,
                                                        &null_desc, cpu);
        }
    }

    r->draw_pipeline_cache =
        g_hash_table_new_full(pipeline_key_hash, pipeline_key_equal, g_free,
                              destroy_pipeline_entry);

    qemu_mutex_init(&r->shader_bytecode_mutex);
    qemu_cond_init(&r->shader_inflight_cond);
    r->shader_bytecode_cache =
        g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free,
                              (GDestroyNotify)g_bytes_unref);
    r->shader_inflight = g_hash_table_new(g_int64_hash, g_int64_equal);
    r->shader_cache_dir = g_strdup_printf("%sd3d12_shaders",
                                          xemu_settings_get_base_path());
    if (g_mkdir_with_parents(r->shader_cache_dir, 0755) != 0) {
        fprintf(stderr,
                "Direct3D 12 renderer: failed to create shader cache "
                "directory %s; the on-disk shader cache is disabled\n",
                r->shader_cache_dir);
        g_free(r->shader_cache_dir);
        r->shader_cache_dir = NULL;
    }

    r->pso_queued = g_hash_table_new(pipeline_key_hash, pipeline_key_equal);
    r->pso_failed = g_hash_table_new_full(pipeline_key_hash,
                                          pipeline_key_equal, g_free, NULL);
    r->pso_built = new_hash_set();
    r->pso_recorded = new_hash_set();
    r->fallback_demand = g_hash_table_new_full(
        pipeline_key_hash, pipeline_key_equal, g_free, g_free);
    r->fallback_demand_failed = g_hash_table_new_full(
        pipeline_key_hash, pipeline_key_equal, g_free, NULL);
    r->fallback_pipelines = g_hash_table_new_full(
        fallback_key_hash, fallback_key_equal, g_free,
        destroy_fallback_entry);
    r->fallback_queued =
        g_hash_table_new(fallback_key_hash, fallback_key_equal);
    r->fallback_done = g_hash_table_new_full(fallback_key_hash,
                                             fallback_key_equal, g_free,
                                             NULL);
    QSIMPLEQ_INIT(&r->pso_jobs);
    QSIMPLEQ_INIT(&r->pso_results);
    qemu_mutex_init(&r->pso_mutex);
    qemu_cond_init(&r->pso_cond);
    qemu_cond_init(&r->pso_done_cond);
    QSIMPLEQ_INIT(&r->stage_jobs);
    qemu_mutex_init(&r->stage_mutex);
    qemu_cond_init(&r->stage_cond);
    qemu_cond_init(&r->stage_done_cond);
    /* The compiler loads lazily; load it before two threads can race. */
    if (pgraph_d3d12_use_fxc()) {
        load_fxc(pg);
    }

    /* Foreground precompile uses every CPU but one; the background keeps
     * a single worker. */
    r->pso_foreground_workers =
        MIN(MAX(g_get_num_processors() - 1, 1u), PGRAPH_D3D12_MAX_PSO_WORKERS);
    r->pso_active_workers = 1;
    r->pso_worker_priority = THREAD_PRIORITY_BELOW_NORMAL;
    r->pso_workers_stop = false;
    for (unsigned int i = 0; i < r->pso_foreground_workers; i++) {
        PGRAPHD3D12PsoWorker *w = &r->pso_workers[i];
        char name[32];

        w->r = r;
        w->index = i;
        snprintf(name, sizeof(name), "nv2a.psocomp%u", i);
        qemu_thread_create(&w->thread, name, pso_worker_thread_fn, w,
                           QEMU_THREAD_JOINABLE);
        r->pso_num_workers++;
    }
    r->pso_workers_running = true;
    r->stage_stop = false;
    for (unsigned int i = 0; i < ARRAY_SIZE(r->stage_threads); i++) {
        char name[32];

        snprintf(name, sizeof(name), "nv2a.shcomp%u", i);
        qemu_thread_create(&r->stage_threads[i], name, shader_stage_thread,
                           pg, QEMU_THREAD_JOINABLE);
        r->num_stage_threads++;
    }
    fprintf(stderr,
            "Direct3D 12 renderer: %u pipeline workers, %u logical CPUs\n",
            r->pso_num_workers, g_get_num_processors());

    /* The title requested so far, or boot keys (title 0). The request is
     * read first, so a newer title is applied by the next FIFO pass. */
    qatomic_set(&pg->shader_cache.supported, true);
    request = qatomic_load_acquire(&pg->shader_cache.request_gen);
    switch_shader_title(r, qatomic_read(&pg->shader_cache.title_id), request);
}

void pgraph_d3d12_finalize_pipelines(PGRAPHState *pg)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    PGRAPHD3D12PsoJob *job;

    if (r == NULL) {
        return;
    }

    qatomic_set(&pg->shader_cache.supported, false);
    if (r->pso_workers_running) {
        fprintf(stderr,
                "Direct3D 12 renderer: pipeline stats: compiled=%"
                PRIu64 " prewarmed=%" PRIu64 " prewarm_failed=%" PRIu64
                " failed=%" PRIu64 "\n",
                r->pso_total_compiled, r->pso_total_prewarmed,
                r->pso_prewarm_failed, r->pso_total_failed);
        /* A list still loading is written out by its loader, which the
         * workers finish before they stop. */
        close_pipeline_list(r);
        qemu_mutex_lock(&r->pso_mutex);
        purge_queued_psos(r, false);
        end_precompile_run(r);
        r->pso_workers_stop = true;
        qemu_cond_broadcast(&r->pso_cond);
        qemu_mutex_unlock(&r->pso_mutex);
        for (unsigned int i = 0; i < r->pso_num_workers; i++) {
            qemu_thread_join(&r->pso_workers[i].thread);
            if (r->pso_workers[i].handle != NULL) {
                CloseHandle(r->pso_workers[i].handle);
                r->pso_workers[i].handle = NULL;
            }
        }
        r->pso_num_workers = 0;
        r->pso_workers_running = false;

        while ((job = QSIMPLEQ_FIRST(&r->pso_jobs)) != NULL) {
            QSIMPLEQ_REMOVE_HEAD(&r->pso_jobs, entry);
            g_free(job->error);
            g_free(job);
        }
        while ((job = QSIMPLEQ_FIRST(&r->pso_results)) != NULL) {
            QSIMPLEQ_REMOVE_HEAD(&r->pso_results, entry);
            D3D12_RELEASE(job->pipeline_state);
            g_free(job->error);
            g_free(job);
        }
        qemu_cond_destroy(&r->pso_done_cond);
        qemu_cond_destroy(&r->pso_cond);
        qemu_mutex_destroy(&r->pso_mutex);
    }
    if (r->num_stage_threads != 0) {
        qemu_mutex_lock(&r->stage_mutex);
        r->stage_stop = true;
        qemu_cond_broadcast(&r->stage_cond);
        qemu_mutex_unlock(&r->stage_mutex);
        for (unsigned int i = 0; i < r->num_stage_threads; i++) {
            qemu_thread_join(&r->stage_threads[i]);
        }
        r->num_stage_threads = 0;
        qemu_cond_destroy(&r->stage_done_cond);
        qemu_cond_destroy(&r->stage_cond);
        qemu_mutex_destroy(&r->stage_mutex);
    }
    if (r->pso_queued != NULL) {
        g_hash_table_destroy(r->pso_queued);
        r->pso_queued = NULL;
    }
    if (r->pso_built != NULL) {
        g_hash_table_destroy(r->pso_built);
        r->pso_built = NULL;
    }
    if (r->pso_recorded != NULL) {
        g_hash_table_destroy(r->pso_recorded);
        r->pso_recorded = NULL;
    }
    if (r->pso_failed != NULL) {
        g_hash_table_destroy(r->pso_failed);
        r->pso_failed = NULL;
    }
    if (r->fallback_queued != NULL) {
        /* Its jobs went with the queue. */
        g_hash_table_destroy(r->fallback_queued);
        r->fallback_queued = NULL;
    }
    if (r->fallback_done != NULL) {
        g_hash_table_destroy(r->fallback_done);
        r->fallback_done = NULL;
    }
    if (r->fallback_demand != NULL) {
        g_hash_table_destroy(r->fallback_demand);
        r->fallback_demand = NULL;
    }
    if (r->fallback_demand_failed != NULL) {
        g_hash_table_destroy(r->fallback_demand_failed);
        r->fallback_demand_failed = NULL;
    }

    if (r->draw_pipeline_cache != NULL) {
        g_hash_table_destroy(r->draw_pipeline_cache);
        r->draw_pipeline_cache = NULL;
        r->draw_pipeline_state = NULL;
        r->draw_pipeline_key_valid = false;
        r->draw_pipeline_fallback = false;
    } else {
        D3D12_RELEASE(r->draw_pipeline_state);
    }
    if (r->fallback_pipelines != NULL) {
        g_hash_table_destroy(r->fallback_pipelines);
        r->fallback_pipelines = NULL;
    }
    if (r->shader_bytecode_cache != NULL) {
        g_hash_table_destroy(r->shader_bytecode_cache);
        r->shader_bytecode_cache = NULL;
        r->shader_bytecode_bytes = 0;
        g_hash_table_destroy(r->shader_inflight);
        r->shader_inflight = NULL;
        qemu_cond_destroy(&r->shader_inflight_cond);
        qemu_mutex_destroy(&r->shader_bytecode_mutex);
    }
    g_free(r->shader_cache_dir);
    r->shader_cache_dir = NULL;
    for (unsigned int i = 0; i < r->masked_clear_pipeline_count; i++) {
        D3D12_RELEASE(r->masked_clear_pipelines[i].pipeline_state);
    }
    r->masked_clear_pipeline_count = 0;
    for (unsigned int i = 0; i < ARRAY_SIZE(r->zeta_view_pipelines); i++) {
        D3D12_RELEASE(r->zeta_view_pipelines[i]);
        r->zeta_view_pipeline_failed[i] = false;
    }
    D3D12_RELEASE(r->draw_root_signature);
    release_dxc_modules();
}

bool pgraph_d3d12_begin_draw_commands(PGRAPHState *pg, bool *pending,
                                      Error **errp)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    uint64_t draw_constants_offset = 0;
    uint64_t pixel_constants_offset = 0;
    uint64_t uber_constants_offset = 0;
    bool uber_ps;
    int64_t perf_start;

    *pending = false;

    if (r == NULL || r->draw_root_signature == NULL) {
        return false;
    }

    perf_start = g_get_monotonic_time();
    if (!pgraph_d3d12_bind_textures(d, errp)) {
        r->perf.texture_bind_us += g_get_monotonic_time() - perf_start;
        return false;
    }
    r->perf.texture_bind_us += g_get_monotonic_time() - perf_start;

    perf_start = g_get_monotonic_time();
    if (!ensure_draw_pipeline_state(pg, pending, errp)) {
        r->perf.pipeline_lookup_us += g_get_monotonic_time() - perf_start;
        return false;
    }
    r->perf.pipeline_lookup_us += g_get_monotonic_time() - perf_start;

    perf_start = g_get_monotonic_time();
    if (!ensure_constant_upload_space(pg, true, errp)) {
        r->perf.constant_upload_us += g_get_monotonic_time() - perf_start;
        return false;
    }

    if (!upload_draw_constants(pg, &draw_constants_offset, errp)) {
        r->perf.constant_upload_us += g_get_monotonic_time() - perf_start;
        return false;
    }
    if (!upload_pixel_constants(pg, &pixel_constants_offset, errp)) {
        r->perf.constant_upload_us += g_get_monotonic_time() - perf_start;
        return false;
    }
    uber_ps = pgraph_d3d12_interpreter_pipeline(r);
    if (r->draw_pipeline_fallback) {
        r->perf.fallback_draws++;
    }
    if (uber_ps && !upload_uber_constants(pg, &uber_constants_offset,
                                          errp)) {
        r->perf.constant_upload_us += g_get_monotonic_time() - perf_start;
        return false;
    }
    r->perf.constant_upload_us += g_get_monotonic_time() - perf_start;

    perf_start = g_get_monotonic_time();
    if (!pgraph_d3d12_begin_commands(pg, errp)) {
        r->perf.record_state_us += g_get_monotonic_time() - perf_start;
        return false;
    }

    if (r->draw_pipeline_key_valid &&
        r->draw_pipeline_key.shader_framebuffer_fetch &&
        !pgraph_d3d12_prepare_framebuffer_fetch(pg, errp)) {
        r->perf.record_state_us += g_get_monotonic_time() - perf_start;
        return false;
    }

    if (!r->record_root_signature_set) {
        r->command_list->lpVtbl->SetGraphicsRootSignature(
            r->command_list, r->draw_root_signature);
        r->record_root_signature_set = true;
    }
    pgraph_d3d12_record_texture_bindings(pg);
    record_draw_constants(pg, draw_constants_offset);
    record_pixel_constants(pg, pixel_constants_offset);
    if (uber_ps) {
        r->command_list->lpVtbl->SetGraphicsRootConstantBufferView(
            r->command_list, PGRAPH_D3D12_ROOT_UBER_CONSTANTS,
            r->uniform_upload_buffer.resource->lpVtbl->GetGPUVirtualAddress(
                r->uniform_upload_buffer.resource) +
                uber_constants_offset);
    }
    pgraph_d3d12_bind_render_targets(pg);
    record_viewport_and_scissor(pg);
    if (r->record_pipeline_state != r->draw_pipeline_state) {
        r->command_list->lpVtbl->SetPipelineState(r->command_list,
                                                  r->draw_pipeline_state);
        r->record_pipeline_state = r->draw_pipeline_state;
    }
    record_blend_factor(pg);
    if (r->draw_pipeline_key_valid && r->draw_pipeline_key.stencil_enable) {
        uint32_t stencil_ref = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1),
                                        NV_PGRAPH_CONTROL_1_STENCIL_REF);

        if (!r->record_stencil_ref_valid ||
            r->record_stencil_ref != stencil_ref) {
            r->command_list->lpVtbl->OMSetStencilRef(r->command_list,
                                                     stencil_ref);
            r->record_stencil_ref = stencil_ref;
            r->record_stencil_ref_valid = true;
        }
    }
    r->perf.record_state_us += g_get_monotonic_time() - perf_start;
    return true;
}
