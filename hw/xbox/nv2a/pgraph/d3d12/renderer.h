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

#ifndef HW_XBOX_NV2A_PGRAPH_D3D12_RENDERER_H
#define HW_XBOX_NV2A_PGRAPH_D3D12_RENDERER_H

#include "qemu/osdep.h"
#include "qemu/queue.h"
#include "qemu/lru.h"
#include "qemu/thread.h"
#include "hw/xbox/nv2a/nv2a_int.h"
#include "hw/xbox/nv2a/pgraph/texture.h"
#include "hw/xbox/nv2a/pgraph/vsh_regs.h"

#include <windows.h>
#include <dxgi1_6.h>
#include <d3d12.h>

#ifndef D3D_SHADER_MODEL_6_1
#define D3D_SHADER_MODEL_6_1 ((D3D_SHADER_MODEL)0x61)
#endif
#ifndef D3D_SHADER_MODEL_6_2
#define D3D_SHADER_MODEL_6_2 ((D3D_SHADER_MODEL)0x62)
#endif
#ifndef D3D_SHADER_MODEL_6_3
#define D3D_SHADER_MODEL_6_3 ((D3D_SHADER_MODEL)0x63)
#endif
#ifndef D3D_SHADER_MODEL_6_4
#define D3D_SHADER_MODEL_6_4 ((D3D_SHADER_MODEL)0x64)
#endif

/* Packs the bits of value selected by mask into the low bits. */
static inline unsigned int pgraph_d3d12_gather_bits(uint32_t value,
                                                    uint32_t mask)
{
    unsigned int result = 0;
    unsigned int bit = 0;

    for (; mask != 0; mask >>= 1, value >>= 1) {
        if (mask & 1) {
            result |= (value & 1) << bit;
            bit++;
        }
    }
    return result;
}

/*
 * Swizzled (Morton order) 2D images: a square block of `block` pixels whose
 * bytes start at a block-aligned offset occupies one rectangle of the image
 * with identical pixel order. Stores that rectangle's origin in x and y;
 * returns false when the block does not map to one.
 */
static inline bool pgraph_d3d12_swizzled_block_origin(
    unsigned int width, unsigned int height, unsigned int bpp,
    unsigned int block, uint64_t byte_offset, unsigned int *x,
    unsigned int *y)
{
    uint64_t block_bytes = (uint64_t)block * block * bpp;
    uint32_t mask_x = 0, mask_y = 0;
    uint32_t bit = 1;
    uint32_t mask_bit = 1;
    bool done;

    if (block == 0 || bpp == 0 || (block & (block - 1)) != 0 ||
        block > width || block > height || byte_offset % block_bytes != 0 ||
        byte_offset + block_bytes > (uint64_t)width * height * bpp ||
        byte_offset / bpp > UINT32_MAX) {
        return false;
    }
    do {
        done = true;
        if (bit < width) {
            mask_x |= mask_bit;
            mask_bit <<= 1;
            done = false;
        }
        if (bit < height) {
            mask_y |= mask_bit;
            mask_bit <<= 1;
            done = false;
        }
        bit <<= 1;
    } while (!done);
    *x = pgraph_d3d12_gather_bits((uint32_t)(byte_offset / bpp), mask_x);
    *y = pgraph_d3d12_gather_bits((uint32_t)(byte_offset / bpp), mask_y);
    return *x + block <= width && *y + block <= height;
}

#define D3D12_RELEASE(obj) do { \
    if ((obj) != NULL) { \
        (obj)->lpVtbl->Release(obj); \
        (obj) = NULL; \
    } \
} while (0)

#define PGRAPH_D3D12_RTV_HEAP_SIZE 4096
#define PGRAPH_D3D12_DSV_HEAP_SIZE 1024
#define PGRAPH_D3D12_CBV_SRV_UAV_HEAP_SIZE 16384
#define PGRAPH_D3D12_SAMPLER_HEAP_SIZE 2048
#define PGRAPH_D3D12_TEXTURE_CACHE_SIZE 512
#define PGRAPH_D3D12_BUFFER_FENCE_MARKS 64
#define PGRAPH_D3D12_DRAWS_PER_SUBMIT 1024
#define PGRAPH_D3D12_REPORTED_STATE_KEYS 16
#define PGRAPH_D3D12_BLIT_DISPLAY_CANDIDATES 16
#define PGRAPH_D3D12_MASKED_CLEAR_PIPELINES 48
#define PGRAPH_D3D12_REPORTED_IMAGE_BLITS 64
/* Deep enough that draw-heavy frames never wait on an in-flight allocator. */
#define PGRAPH_D3D12_COMMAND_ALLOCATORS 64
#define PGRAPH_D3D12_DRAW_CONSTANTS_ALIGNMENT 256
#define PGRAPH_D3D12_ROOT_TEXTURE_SRV(stage) (stage)
#define PGRAPH_D3D12_ROOT_TEXTURE_SAMPLER(stage) (NV2A_MAX_TEXTURES + (stage))
#define PGRAPH_D3D12_ROOT_FRAMEBUFFER_SRV (NV2A_MAX_TEXTURES * 2)
#define PGRAPH_D3D12_FRAMEBUFFER_SRV_REGISTER NV2A_MAX_TEXTURES
#define PGRAPH_D3D12_ROOT_DRAW_CONSTANTS (PGRAPH_D3D12_ROOT_FRAMEBUFFER_SRV + 1)
#define PGRAPH_D3D12_ROOT_PIXEL_CONSTANTS (PGRAPH_D3D12_ROOT_DRAW_CONSTANTS + 1)
#define PGRAPH_D3D12_ROOT_VERTEX_RAM_SRV (PGRAPH_D3D12_ROOT_PIXEL_CONSTANTS + 1)
#define PGRAPH_D3D12_ROOT_VERTEX_SCRATCH_SRV (PGRAPH_D3D12_ROOT_VERTEX_RAM_SRV + 1)
/* First vertex of a pulled draw-arrays span (b2). SV_VertexID is not relied
 * on to include StartVertexLocation. */
#define PGRAPH_D3D12_ROOT_VERTEX_START (PGRAPH_D3D12_ROOT_VERTEX_SCRATCH_SRV + 1)
/* Interpreter tables (b3); the specialised shaders never read it, so it is
 * left unset. */
#define PGRAPH_D3D12_ROOT_UBER_CONSTANTS (PGRAPH_D3D12_ROOT_VERTEX_START + 1)
/* Vertex pulling table (b4), apart from the draw constants: it changes on
 * most draws, the 3 KB of shader constants seldom. */
#define PGRAPH_D3D12_ROOT_VERTEX_FETCH (PGRAPH_D3D12_ROOT_UBER_CONSTANTS + 1)
#define PGRAPH_D3D12_ROOT_PARAMETER_COUNT (NV2A_MAX_TEXTURES * 2 + 8)
/* Ranges per texture SRV table, all on its one descriptor: Texture2D in
 * space 0, TextureCube, Texture2DArray and Texture3D in spaces 1-3. */
#define PGRAPH_D3D12_TEXTURE_SRV_SPACES 4
#define PGRAPH_D3D12_VERTEX_RAM_SRV_REGISTER (NV2A_MAX_TEXTURES + 1)
#define PGRAPH_D3D12_VERTEX_SCRATCH_SRV_REGISTER (NV2A_MAX_TEXTURES + 2)

/*
 * Fetch vertices from buffer SRVs instead of the input assembler: the Xbox
 * UWP driver corrupts IA fetches from large buffers.
 */
static inline bool pgraph_d3d12_vertex_pulling(void)
{
#ifdef XEMU_UWP
    return true;
#else
    return getenv("XEMU_D3D12_VERTEX_PULL") != NULL &&
           getenv("XEMU_D3D12_VERTEX_PULL")[0] == '1';
#endif
}

typedef enum PGRAPHD3D12Diagnostic {
    PGRAPH_D3D12_DIAG_MISSING_PSO = 1ull << 0,
    PGRAPH_D3D12_DIAG_UNSUPPORTED_TOPOLOGY = 1ull << 1,
    PGRAPH_D3D12_DIAG_UNSUPPORTED_VERTEX_FORMAT = 1ull << 2,
    PGRAPH_D3D12_DIAG_MISSING_SURFACE = 1ull << 3,
    PGRAPH_D3D12_DIAG_PARTIAL_COLOR_CLEAR = 1ull << 4,
    PGRAPH_D3D12_DIAG_SURFACE_UPLOAD = 1ull << 5,
    PGRAPH_D3D12_DIAG_SURFACE_DOWNLOAD = 1ull << 6,
    PGRAPH_D3D12_DIAG_DXC_UNAVAILABLE = 1ull << 7,
    PGRAPH_D3D12_DIAG_UNSUPPORTED_DRAW_STATE = 1ull << 8,
    PGRAPH_D3D12_DIAG_DISPLAY_DOWNLOAD = 1ull << 9,
    PGRAPH_D3D12_DIAG_IMAGE_BLIT = 1ull << 10,
    PGRAPH_D3D12_DIAG_TEXTURE0 = 1ull << 11,
    PGRAPH_D3D12_DIAG_UNSUPPORTED_REPORT = 1ull << 12,
    PGRAPH_D3D12_DIAG_ZPASS_REPORTS = 1ull << 13,
    PGRAPH_D3D12_DIAG_ZPASS_QUERY = 1ull << 14,
    PGRAPH_D3D12_DIAG_MISSING_ZETA_FOR_DEPTH = 1ull << 15,
    PGRAPH_D3D12_DIAG_STENCIL_WITHOUT_STENCIL_SURFACE = 1ull << 16,
    PGRAPH_D3D12_DIAG_UNSUPPORTED_DEPTH_STENCIL = 1ull << 17,
    PGRAPH_D3D12_DIAG_UNSUPPORTED_BLEND_STATE = 1ull << 18,
    PGRAPH_D3D12_DIAG_UNSUPPORTED_RASTER_STATE = 1ull << 19,
    PGRAPH_D3D12_DIAG_UNSUPPORTED_ALPHA_TEST = 1ull << 20,
    PGRAPH_D3D12_DIAG_PROGRAMMABLE_VSH_FALLBACK = 1ull << 21,
    PGRAPH_D3D12_DIAG_PVIDEO_FORMAT = 1ull << 22,
    PGRAPH_D3D12_DIAG_PVIDEO_BOUNDS = 1ull << 23,
    PGRAPH_D3D12_DIAG_PVIDEO_PITCH = 1ull << 24,
    PGRAPH_D3D12_DIAG_DXIL_FALLBACK = 1ull << 25,
    PGRAPH_D3D12_DIAG_DXIL_UNSIGNED = 1ull << 26,
} PGRAPHD3D12Diagnostic;

typedef enum PGRAPHD3D12SurfaceDownloadReason {
    PGRAPH_D3D12_SURFACE_DOWNLOAD_INCOMPATIBLE,
    PGRAPH_D3D12_SURFACE_DOWNLOAD_OVERLAP_INVALIDATION,
    PGRAPH_D3D12_SURFACE_DOWNLOAD_TEXTURE_OVERLAP,
    PGRAPH_D3D12_SURFACE_DOWNLOAD_BLIT_OVERLAP,
    PGRAPH_D3D12_SURFACE_DOWNLOAD_MASKED_CLEAR,
    PGRAPH_D3D12_SURFACE_DOWNLOAD_DISPLAY_CPU,
    PGRAPH_D3D12_SURFACE_DOWNLOAD_REASON_COUNT,
} PGRAPHD3D12SurfaceDownloadReason;

typedef enum PGRAPHD3D12BlendConstantMode {
    PGRAPH_D3D12_BLEND_CONSTANT_NONE,
    PGRAPH_D3D12_BLEND_CONSTANT_RGBA,
    PGRAPH_D3D12_BLEND_CONSTANT_ALPHA,
} PGRAPHD3D12BlendConstantMode;

typedef enum PGRAPHD3D12DepthFormat {
    PGRAPH_D3D12_DEPTH_FORMAT_D24,
    PGRAPH_D3D12_DEPTH_FORMAT_D16,
    PGRAPH_D3D12_DEPTH_FORMAT_F24,
    PGRAPH_D3D12_DEPTH_FORMAT_F16,
} PGRAPHD3D12DepthFormat;

typedef struct PGRAPHD3D12BufferFenceMark {
    uint64_t fence_value;
    uint64_t vhead;
} PGRAPHD3D12BufferFenceMark;

typedef struct PGRAPHD3D12Buffer {
    ID3D12Resource *resource;
    void *mapped;
    uint64_t offset;
    uint64_t size;
    D3D12_RESOURCE_STATES state;

    /*
     * Fence-tracked ring: physical offset = virtual % size, reservations
     * never straddle the end, marks retire space per completed submit.
     * Draw-transient allocations are not consumable until their Draw is
     * recorded.
     */
    uint64_t ring_vhead;
    uint64_t ring_vtail;
    uint64_t ring_recorded_vhead;
    PGRAPHD3D12BufferFenceMark ring_marks[PGRAPH_D3D12_BUFFER_FENCE_MARKS];
    uint32_t ring_mark_head;
    uint32_t ring_mark_count;
} PGRAPHD3D12Buffer;

typedef struct PGRAPHD3D12SurfaceFormatInfo {
    unsigned int bytes_per_pixel;
    DXGI_FORMAT dxgi_format;
    bool depth;
    bool stencil;
} PGRAPHD3D12SurfaceFormatInfo;

typedef struct PGRAPHD3D12SurfaceBinding {
    QTAILQ_ENTRY(PGRAPHD3D12SurfaceBinding) entry;
    MemAccessCallback *access_cb;
    NV2AState *nv2a;

    hwaddr vram_addr;
    uintptr_t dma_addr;
    uintptr_t dma_len;
    SurfaceShape shape;
    bool color;
    bool swizzle;

    unsigned int width;
    unsigned int height;
    unsigned int pitch;
    size_t size;

    bool cleared;
    int frame_time;
    /* Guest frame in which GPU/CPU content was last changed. */
    int write_frame_time;
    int draw_time;
    bool draw_dirty;
    bool download_pending;
    bool upload_pending;
    bool initialized;
    uint8_t download_failures;
    /* Row band already written back for draw_time by a ranged download. */
    uint32_t range_download_row_start;
    uint32_t range_download_row_end;
    int range_download_draw_time;
    bool range_download_valid;

    PGRAPHD3D12SurfaceFormatInfo fmt;
    ID3D12Resource *resource;
    D3D12_RESOURCE_STATES state;
    uint32_t descriptor_index;
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor;
    bool descriptor_allocated;
    bool stash_selected;
} PGRAPHD3D12SurfaceBinding;

typedef struct PGRAPHD3D12MemorySync {
    hwaddr addr;
    hwaddr size;
} PGRAPHD3D12MemorySync;

typedef struct PGRAPHD3D12TextureKey {
    TextureShape shape;
    hwaddr vram_addr;
    size_t length;
    hwaddr palette_vram_addr;
    size_t palette_length;
    DXGI_FORMAT resource_format;
    DXGI_FORMAT dxgi_format;
    unsigned int upload_bytes_per_pixel;
    unsigned int scale;
    bool converted;
    bool compressed;
    /* Runtime provenance; canonicalized out of the structural cache key. */
    bool surface_to_texture;
    /* Rendered from a Z24S8 surface; the resource allows render targets. */
    bool zeta_view;
} PGRAPHD3D12TextureKey;

/* Six cube faces of up to 16 levels each. */
#define PGRAPH_D3D12_REPLACEMENT_MAX_SUBRESOURCES 96

typedef struct PGRAPHD3D12ReplacementSubresource {
    size_t offset;              /* into the replacement's data */
    uint32_t row_pitch;         /* tightly packed */
    uint32_t rows;              /* pixel rows, or block rows */
    uint32_t width;
    uint32_t height;
} PGRAPHD3D12ReplacementSubresource;

/*
 * A custom texture from the title's texture pack, decoded once and kept
 * until the pack is reloaded. Subresources are face-major, mip-minor. It is
 * sampled as plain RGBA, without the guest format's channel swizzle.
 */
typedef struct PGRAPHD3D12ReplacementTexture {
    DXGI_FORMAT dxgi_format;
    uint32_t width;
    uint32_t height;
    uint32_t mip_levels;
    uint32_t faces;
    uint8_t *data;
    size_t data_size;
    PGRAPHD3D12ReplacementSubresource
        subresources[PGRAPH_D3D12_REPLACEMENT_MAX_SUBRESOURCES];
} PGRAPHD3D12ReplacementTexture;

typedef struct PGRAPHD3D12TextureReplace PGRAPHD3D12TextureReplace;

typedef struct PGRAPHD3D12TextureBinding {
    LruNode node;
    PGRAPHD3D12TextureKey key;
    /* Copies of key fields. */
    TextureShape shape;
    hwaddr vram_addr;
    size_t length;
    hwaddr palette_vram_addr;
    size_t palette_length;
    DXGI_FORMAT resource_format;
    DXGI_FORMAT dxgi_format;
    unsigned int upload_bytes_per_pixel;
    bool converted;
    bool compressed;
    float scale;
    bool surface_to_texture;
    hwaddr source_surface_addr;
    int source_surface_draw_time;
    ID3D12Resource *resource;
    D3D12_RESOURCE_STATES state;
    uint64_t data_hash;
    bool possibly_dirty;
    /* Custom texture the resource holds instead of the guest texels;
     * owned by the texture pack. */
    const PGRAPHD3D12ReplacementTexture *replacement;
    int draw_time;                 /* surface-to-texture refresh time */
    uint32_t srv_descriptor_index;
    D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu;
    D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu;
    bool descriptors_allocated;
    bool valid;
    uint64_t resource_bytes;       /* counted in texture_cache_bytes */
    /* Validated with no surface over its bytes at surface_free_gen; until
     * the surface layout changes only guest writes can make it stale. */
    bool surface_free;
    uint64_t surface_free_gen;
} PGRAPHD3D12TextureBinding;

/* Half-open guest byte ranges of a texture cache slot; all zero when free. */
typedef struct PGRAPHD3D12TextureRange {
    hwaddr start, end;
    hwaddr palette_start, palette_end;
} PGRAPHD3D12TextureRange;

typedef struct PGRAPHD3D12BlitDisplayCandidate {
    bool valid;
    hwaddr addr;
    uint64_t size;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bytes_per_pixel;
    uint32_t color_format;
    uint32_t operation;
    int draw_time;
    uint32_t sequence;
} PGRAPHD3D12BlitDisplayCandidate;

typedef struct PGRAPHD3D12ImageBlitReportKey {
    hwaddr source_addr;
    hwaddr dest_addr;
    uint64_t dest_size;
    uint32_t operation;
    uint32_t color_format;
} PGRAPHD3D12ImageBlitReportKey;

typedef struct PGRAPHD3D12PipelineKey {
    D3D12_PRIMITIVE_TOPOLOGY_TYPE primitive_topology_type;
    uint8_t primitive_mode;
    DXGI_FORMAT rtv_format;
    DXGI_FORMAT dsv_format;
    uint8_t color_write_mask;
    bool blend_enable;
    bool logic_op_enable;
    D3D12_LOGIC_OP logic_op;
    bool shader_framebuffer_fetch;
    bool shader_blend_enable;
    bool shader_logic_op_enable;
    uint8_t shader_blend_src_factor;
    uint8_t shader_blend_dest_factor;
    uint8_t shader_blend_equation;
    uint8_t shader_logic_op;
    D3D12_BLEND src_blend;
    D3D12_BLEND dest_blend;
    D3D12_BLEND_OP blend_op;
    D3D12_BLEND src_blend_alpha;
    D3D12_BLEND dest_blend_alpha;
    D3D12_BLEND_OP blend_op_alpha;
    PGRAPHD3D12BlendConstantMode blend_constant_mode;
    bool shader_src_blend_factor_enable;
    uint8_t shader_src_blend_factor;
    bool alpha_test_enable;
    uint8_t alpha_func;
    uint8_t alpha_ref;
    D3D12_CULL_MODE cull_mode;
    bool front_counter_clockwise;
    bool depth_enable;
    bool depth_write;
    D3D12_COMPARISON_FUNC depth_func;
    PGRAPHD3D12DepthFormat depth_format;
    bool z_perspective;
    bool depth_clipping;
    bool smooth_shading;
    bool first_vertex_provoking;
    /* >1 enables guest-pixel line expansion for scaled render targets. */
    uint8_t line_expand_width;
    /* Front polygon mode of polygon primitives; POLY_MODE_FILL otherwise. */
    uint8_t polygon_mode;
    /* PGRAPH_D3D12_TUNE_* bits that change generated shaders or select
     * separate PSOs. */
    uint32_t tune_flags;
    bool stencil_enable;
    uint8_t stencil_read_mask;
    uint8_t stencil_write_mask;
    D3D12_COMPARISON_FUNC stencil_func;
    D3D12_STENCIL_OP stencil_fail_op;
    D3D12_STENCIL_OP stencil_depth_fail_op;
    D3D12_STENCIL_OP stencil_pass_op;
    uint8_t texture_mask;
    unsigned int texture_color_format[NV2A_MAX_TEXTURES];
    DXGI_FORMAT texture_dxgi_format[NV2A_MAX_TEXTURES];
    uint32_t texture_width[NV2A_MAX_TEXTURES];
    uint32_t texture_height[NV2A_MAX_TEXTURES];
    uint32_t texture_depth[NV2A_MAX_TEXTURES];
    uint32_t texture_pitch[NV2A_MAX_TEXTURES];
    uint32_t texture_bytes_per_pixel[NV2A_MAX_TEXTURES];
    uint8_t texture_linear_mask;
    uint8_t texture_cubemap_mask;
    uint8_t texture_3d_mask;
    uint8_t texture_depth_mask;
    uint8_t texture_snorm_mask;
    uint8_t texture_surface_to_texture_mask;
    uint8_t texture_border_adjust_mask;
    float texture_border_logical_size[NV2A_MAX_TEXTURES][3];
    float texture_border_inv_real_size[NV2A_MAX_TEXTURES][3];
    uint8_t texture_coord_mask;
    uint8_t texture_convolution_filter[NV2A_MAX_TEXTURES];
    /* TEX_CHANNEL_* signed channels: the texture holds them offset by 128
     * (texsigns.c) and the pixel shader reads them as -1..1. */
    uint8_t texture_signs[NV2A_MAX_TEXTURES];
    uint32_t combiner_control;
    uint32_t shader_stage_program;
    uint32_t other_stage_input;
    uint32_t shader_clip_mode;
    bool window_clip_exclusive;
    uint8_t vertex_shader_mode;
    bool fog_enable;
    uint8_t fog_mode;
    uint8_t fixed_function_foggen;
    bool point_params_enable;
    bool point_sprite_enable;
    uint8_t fixed_function_skinning;
    bool fixed_function_normalization;
    bool fixed_function_lighting;
    bool fixed_function_local_eye;
    bool specular_enable;
    bool separate_specular;
    bool ignore_specular_alpha;
    uint8_t fixed_function_emission_src;
    uint8_t fixed_function_ambient_src;
    uint8_t fixed_function_diffuse_src;
    uint8_t fixed_function_specular_src;
    uint8_t fixed_function_light[NV2A_MAX_LIGHTS];
    uint8_t fixed_function_texture_matrix_enable_mask;
    uint8_t fixed_function_texgen[NV2A_MAX_TEXTURES][4];
    uint8_t transform_program_start;
    uint8_t transform_program_length;
    uint32_t transform_program_data[NV2A_MAX_TRANSFORM_PROGRAM_LENGTH]
                                   [VSH_TOKEN_SIZE];
    uint8_t shadow_depth_func;
    uint32_t final_inputs_0;
    uint32_t final_inputs_1;
    uint32_t rgb_inputs[8];
    uint32_t rgb_outputs[8];
    uint32_t alpha_inputs[8];
    uint32_t alpha_outputs[8];
    uint8_t texture_alphakill_mask;
    uint8_t texture_colorkey_mode[NV2A_MAX_TEXTURES];
    uint16_t uniform_attrs;
    uint16_t swizzle_attrs;
    uint16_t compressed_attrs;
    uint8_t num_attributes;
    uint8_t attribute_indices[NV2A_VERTEXSHADER_ATTRIBUTES];
    DXGI_FORMAT attribute_formats[NV2A_VERTEXSHADER_ATTRIBUTES];
} PGRAPHD3D12PipelineKey;

typedef struct PGRAPHD3D12QueryReport {
    unsigned int issue_frame;   /* diagnostic */
    QSIMPLEQ_ENTRY(PGRAPHD3D12QueryReport) entry;
    bool clear;
    uint32_t parameter;
    /* Report DMA object current when GET_REPORT ran. */
    uint32_t dma_report;
    uint32_t query_count;
    /* Queue fence covering this report's query resolves (0 = none). */
    uint64_t fence_value;
} PGRAPHD3D12QueryReport;

/*
 * Interpreter fallback modes (tune "fallback=<mode>"). off: a missing
 * pipeline builds synchronously. skip: the draw is skipped while a worker
 * builds it. ps: validation, every draw the interpreter pixel shader can
 * take uses it with the specialised vertex shader. all: a missing pipeline
 * is drawn with the interpreter pipeline of its fallback key while a worker
 * builds it, or built synchronously if that is not ready either. debug:
 * every draw with a fallback key uses its interpreter pipeline once built.
 */
typedef enum PGRAPHD3D12FallbackMode {
    PGRAPH_D3D12_FALLBACK_OFF,
    PGRAPH_D3D12_FALLBACK_SKIP,
    PGRAPH_D3D12_FALLBACK_DEBUG,
    PGRAPH_D3D12_FALLBACK_PS,
    PGRAPH_D3D12_FALLBACK_ALL,
} PGRAPHD3D12FallbackMode;

/* Interpreter vertex and pixel shader classes. */
#define PGRAPH_D3D12_FALLBACK_VS_FIXED      0
#define PGRAPH_D3D12_FALLBACK_VS_PROGRAM    1
/* Projective, cube, pass-through and clip-plane stages only. */
#define PGRAPH_D3D12_FALLBACK_PS_COMMON     0
/* Bump, dot-product, dependent and convolution stages too. */
#define PGRAPH_D3D12_FALLBACK_PS_FULL       1

/*
 * The fixed state an interpreter pipeline serves: what D3D12 cannot change
 * per draw, the geometry shader selection and the shader classes. Derived
 * from a canonical pipeline key; unused fields and padding are zero.
 */
typedef struct PGRAPHD3D12FallbackKey {
    D3D12_PRIMITIVE_TOPOLOGY_TYPE primitive_topology_type;
    DXGI_FORMAT rtv_format;
    DXGI_FORMAT dsv_format;
    uint8_t color_write_mask;
    bool blend_enable;
    bool logic_op_enable;
    D3D12_LOGIC_OP logic_op;
    D3D12_BLEND src_blend;
    D3D12_BLEND dest_blend;
    D3D12_BLEND_OP blend_op;
    D3D12_BLEND src_blend_alpha;
    D3D12_BLEND dest_blend_alpha;
    D3D12_BLEND_OP blend_op_alpha;
    bool depth_enable;
    bool depth_write;
    D3D12_COMPARISON_FUNC depth_func;
    bool stencil_enable;
    uint8_t stencil_read_mask;
    uint8_t stencil_write_mask;
    D3D12_COMPARISON_FUNC stencil_func;
    D3D12_STENCIL_OP stencil_fail_op;
    D3D12_STENCIL_OP stencil_depth_fail_op;
    D3D12_STENCIL_OP stencil_pass_op;
    /* Geometry shader selection, zero without one. With a geometry shader
     * the rasterized primitives are its own, so culling stays fixed;
     * otherwise the interpreter culls on the facing it is given. */
    bool geometry_shader;
    uint8_t primitive_mode;
    bool smooth_shading;
    bool first_vertex_provoking;
    uint8_t polygon_mode;
    uint8_t line_expand_width;
    bool point_sprite_enable;
    bool z_perspective;
    D3D12_CULL_MODE cull_mode;
    bool front_counter_clockwise;
    /* Pixel shader outputs: SV_Target with an RTV format, and SV_Depth. */
    bool depth_output;
    /* Pixel shader inputs: depth for the clip test only, and screen-space
     * depth interpolation. */
    bool depth_clip;
    bool zinterp_screen;
    uint8_t vs_class;
    uint8_t ps_class;
} PGRAPHD3D12FallbackKey;

typedef struct PGRAPHD3D12PipelineList PGRAPHD3D12PipelineList;

typedef enum PGRAPHD3D12PsoJobKind {
    PGRAPH_D3D12_PSO_JOB_BUILD,
    /* Read a title's pipeline list and queue its keys. */
    PGRAPH_D3D12_PSO_JOB_LOAD_LIST,
    /* Measurement: time the interpreter shaders (tune "ubertest=<id>"). */
    PGRAPH_D3D12_PSO_JOB_UBER_TEST,
    /* Build the interpreter pipeline of a fallback key. */
    PGRAPH_D3D12_PSO_JOB_FALLBACK,
} PGRAPHD3D12PsoJobKind;

/* Run of a build queued by a draw rather than a precompile run; runs are
 * numbered from 1, so it never counts toward one. */
#define PGRAPH_D3D12_PSO_RUN_NONE 0

/* Pipeline prewarm job; the worker fills pipeline_state or error. */
typedef struct PGRAPHD3D12PsoJob {
    QSIMPLEQ_ENTRY(PGRAPHD3D12PsoJob) entry;
    PGRAPHD3D12PsoJobKind kind;
    /* Precompile run the job belongs to. */
    uint32_t run;
    /* A skipped draw waits for this build: it goes ahead of precompile
     * builds and runs at normal priority, on a parked worker if need be. */
    bool demand;
    /* An interpreter pipeline of a list's recorded key: it builds after
     * the precompile builds, at their priority, until a draw needs it. */
    bool preload;
    PGRAPHD3D12PipelineList *list;
    PGRAPHD3D12PipelineKey key;
    PGRAPHD3D12FallbackKey fallback_key;
    ID3D12PipelineState *pipeline_state;
    char *error;
    /* When the worker finished the build. */
    int64_t finished_us;
    /* When a worker took a fallback job. */
    int64_t started_us;
    /* Tune id of an uber test job, for its log lines. */
    char test_id[32];
} PGRAPHD3D12PsoJob;

#define PGRAPH_D3D12_MAX_PSO_WORKERS 6

typedef struct PGRAPHD3D12PsoWorker {
    PGRAPHD3D12State *r;
    unsigned int index;
    QemuThread thread;
    /* For priority changes made by other threads. */
    HANDLE handle;
    /* The rest is protected by pso_mutex. */
    PGRAPHD3D12PsoJob *inflight;
    int priority;
    /* Raised to normal priority while a draw waits on its work. */
    bool boosted;
    /* Owner of the shader compile this worker waits for; set under
     * pso_mutex, cleared without it. */
    struct PGRAPHD3D12PsoWorker *waiting_on;
} PGRAPHD3D12PsoWorker;

/* One shader stage of a pipeline, compiled on the calling thread or on a
 * stage helper. */
typedef struct PGRAPHD3D12ShaderStage {
    QSIMPLEQ_ENTRY(PGRAPHD3D12ShaderStage) entry;
    const char *source;
    LPCWSTR entry_point;
    LPCWSTR target;
    GBytes *bytes;
    int64_t compile_us;
    Error *err;
    bool ok;
    bool done;
    /* DXC even where FXC is the default. */
    bool dxil;
    /* The bytecode came from the in-memory cache. */
    bool memory_hit;
} PGRAPHD3D12ShaderStage;

#define PGRAPH_D3D12_STAGE_HELPERS 2

/*
 * Resource and owned descriptor slots released once the GPU passes
 * fence_value. Descriptor indices are -1 when not owned.
 */
typedef struct PGRAPHD3D12DeferredRelease {
    QSIMPLEQ_ENTRY(PGRAPHD3D12DeferredRelease) entry;
    ID3D12Resource *resource;
    int32_t srv_descriptor_index;
    int32_t rtv_descriptor_index;
    int32_t dsv_descriptor_index;
    int32_t sampler_descriptor_index;
    uint64_t fence_value;
} PGRAPHD3D12DeferredRelease;

typedef struct PGRAPHD3D12State {
    PGRAPHState *pg;
    IDXGIFactory1 *factory;
    IDXGIAdapter1 *adapter;
    ID3D12Device *device;
    /* Loaded only when present next to the executable. */
    HMODULE pix_capturer;
    ID3D12CommandQueue *direct_queue;
    ID3D12CommandAllocator *command_allocator;
    ID3D12CommandAllocator
        *command_allocators[PGRAPH_D3D12_COMMAND_ALLOCATORS];
    uint64_t
        command_allocator_fence_values[PGRAPH_D3D12_COMMAND_ALLOCATORS];
    uint32_t command_allocator_index;
    ID3D12GraphicsCommandList *command_list;
    ID3D12Fence *fence;
    HANDLE fence_event;
    uint64_t fence_value;
    uint64_t submitted_fence_value;
    bool command_list_open;

    ID3D12DescriptorHeap *rtv_heap;
    ID3D12DescriptorHeap *dsv_heap;
    ID3D12DescriptorHeap *cbv_srv_uav_heap;
    ID3D12DescriptorHeap *sampler_heap;
    UINT rtv_descriptor_size;
    UINT dsv_descriptor_size;
    UINT cbv_srv_uav_descriptor_size;
    UINT sampler_descriptor_size;
    uint32_t next_rtv_descriptor;
    uint32_t next_dsv_descriptor;
    uint32_t free_rtv_descriptors[PGRAPH_D3D12_RTV_HEAP_SIZE];
    uint32_t free_dsv_descriptors[PGRAPH_D3D12_DSV_HEAP_SIZE];
    uint32_t num_free_rtv_descriptors;
    uint32_t num_free_dsv_descriptors;
    uint32_t free_cbv_srv_uav_descriptors[PGRAPH_D3D12_CBV_SRV_UAV_HEAP_SIZE];
    uint32_t num_free_cbv_srv_uav_descriptors;
    uint32_t free_sampler_descriptors[PGRAPH_D3D12_SAMPLER_HEAP_SIZE];
    uint32_t num_free_sampler_descriptors;
    uint32_t next_cbv_srv_uav_descriptor;
    uint32_t next_sampler_descriptor;

    PGRAPHD3D12Buffer upload_buffer;
    PGRAPHD3D12Buffer readback_buffer;
    PGRAPHD3D12Buffer vertex_ram_buffer;
    PGRAPHD3D12Buffer index_upload_buffer;
    PGRAPHD3D12Buffer vertex_inline_upload_buffer;
    PGRAPHD3D12Buffer uniform_upload_buffer;
    PGRAPHD3D12Buffer query_readback_buffer;
    unsigned long *uploaded_bitmap;
    size_t bitmap_size;

    ID3D12RootSignature *draw_root_signature;
    ID3D12PipelineState *draw_pipeline_state;
    PGRAPHD3D12PipelineKey draw_pipeline_key;
    bool draw_pipeline_key_valid;
    /* What the previous draw's pipeline was chosen from, beyond the guest
     * state behind state_generation (ensure_draw_pipeline_state reuse). */
    struct {
        bool valid;
        uint32_t generation;
        uint32_t tune_flags;
        unsigned int primitive_mode;
        uint16_t uniform_attrs, swizzle_attrs, compressed_attrs;
        const void *color_binding, *zeta_binding;
        DXGI_FORMAT color_format, zeta_format;
        int num_attributes;
        uint32_t attribute_bits[NV2A_VERTEXSHADER_ATTRIBUTES];
        int attribute_location[NV2A_VERTEXSHADER_ATTRIBUTES];
        struct {
            const void *binding, *replacement;
            TextureShape shape;
            DXGI_FORMAT dxgi_format;
            unsigned int upload_bytes_per_pixel;
            bool surface_to_texture;
        } textures[NV2A_MAX_TEXTURES];
    } pso_reuse;
    /* draw_pipeline_state is the interpreter pipeline of the key's fallback
     * key, not the key's own. */
    bool draw_pipeline_fallback;
    GHashTable *draw_pipeline_cache;
    /* In-memory bytecode by source hash, bounded by bytes; the disk
     * cache backs it. Compiles in flight are in shader_inflight. */
    GHashTable *shader_bytecode_cache;
    size_t shader_bytecode_bytes;
    GHashTable *shader_inflight;
    QemuCond shader_inflight_cond;
    QemuMutex shader_bytecode_mutex;
    char *shader_cache_dir;
    /* Helpers compiling a synchronous miss's stages concurrently. */
    QemuThread stage_threads[PGRAPH_D3D12_STAGE_HELPERS];
    unsigned int num_stage_threads;
    QemuMutex stage_mutex;
    QemuCond stage_cond;
    QemuCond stage_done_cond;
    bool stage_stop;
    QSIMPLEQ_HEAD(, PGRAPHD3D12ShaderStage) stage_jobs;
    /* Channel-masked colour clear pipelines, by RTV format and write mask. */
    struct {
        DXGI_FORMAT format;
        UINT8 write_mask;
        ID3D12PipelineState *pipeline_state;
    } masked_clear_pipelines[PGRAPH_D3D12_MASKED_CLEAR_PIPELINES];
    unsigned int masked_clear_pipeline_count;
    /* Z24S8 sampled as RGBA8 and BGRA8 bytes. */
    ID3D12PipelineState *zeta_view_pipelines[2];
    bool zeta_view_pipeline_failed[2];

    /*
     * Background PSO compilers. They build the pipelines a title recorded
     * in earlier sessions; a live draw's miss still compiles synchronously,
     * taking a queued job off the queue or waiting for the one in flight,
     * unless the fallback mode skips the draw and queues a demand build.
     * Workers past pso_active_workers park; the first few still take the
     * jobs draws wait for.
     */
    PGRAPHD3D12PsoWorker pso_workers[PGRAPH_D3D12_MAX_PSO_WORKERS];
    unsigned int pso_num_workers;
    unsigned int pso_foreground_workers;
    QemuMutex pso_mutex;
    QemuCond pso_cond;
    QemuCond pso_done_cond;
    bool pso_workers_running;
    bool pso_workers_stop;
    unsigned int pso_active_workers;    /* pso_mutex */
    int pso_worker_priority;            /* pso_mutex */
    QSIMPLEQ_HEAD(, PGRAPHD3D12PsoJob) pso_jobs;
    QSIMPLEQ_HEAD(, PGRAPHD3D12PsoJob) pso_results;
    GHashTable *pso_queued;             /* key -> queued job, pso_mutex */
    /* Hashes of every key with a PSO built, being drained or being built
     * by a draw, pso_mutex. */
    GHashTable *pso_built;
    GHashTable *pso_failed;
    /* Precompile run of the current title (pso_mutex). */
    uint32_t pso_run;
    uint32_t pso_run_title;
    uint32_t pso_run_request;
    bool pso_run_active;
    unsigned int pso_run_total;
    unsigned int pso_run_done;
    unsigned int pso_run_failed;
    /* Keys in the title's list when it loaded. */
    unsigned int pso_run_recorded;
    unsigned int pso_run_workers;
    int64_t pso_run_start_us;
    /* Driver time of the run's built PSOs (the create call that built
     * each), how many of them are DXIL, and how many asked for DXIL but
     * were built from DXBC. */
    int64_t pso_run_driver_us;
    unsigned int pso_run_dxil;
    unsigned int pso_run_dxil_fallbacks;
    /* Shader cache title and mode as applied by the FIFO thread. */
    uint32_t shader_title;
    uint32_t shader_title_request;
    bool shader_title_valid;
    bool pso_foreground;
    /* The title's pipeline list and the key hashes recorded in it (FIFO
     * thread only); the serial changes with every title switch. */
    PGRAPHD3D12PipelineList *pso_list;
    GHashTable *pso_recorded;
    uint32_t pso_list_serial;
    int64_t pso_list_flush_us;
    uint64_t pso_total_prewarmed;
    uint64_t pso_prewarm_failed;
    uint64_t pso_total_compiled;
    uint64_t pso_total_failed;
    uint64_t pso_total_skipped_draws;
    /*
     * Interpreter fallback (tune "fallback=<mode>"). FIFO thread only:
     * fallback_demand maps each key a draw was skipped for to its first
     * skip time until its build is drained; fallback_pipelines maps the
     * fallback keys seen to their interpreter pipelines, which are kept
     * until finalize whatever the title; fallback_demand_failed holds the
     * keys whose demand build failed, which the next draw builds itself.
     * fallback_queued (fallback key -> queued job) is under pso_mutex.
     */
    GHashTable *fallback_demand;
    GHashTable *fallback_demand_failed;
    GHashTable *fallback_pipelines;
    GHashTable *fallback_queued;
    unsigned int fallback_keys;
    unsigned int fallback_build_failures;
    /* Null Texture2D view for the stages an interpreter pixel shader
     * declares but the draw leaves unbound. */
    D3D12_GPU_DESCRIPTOR_HANDLE uber_null_srv_gpu;
    /* Fallback keys whose build a worker finished (pso_mutex), so a list
     * load does not queue them again. */
    GHashTable *fallback_done;
    /* Last b3 upload: the key it was filled from, whether it holds the
     * vertex block, and its offset; dropped with the other constant
     * caches. */
    PGRAPHD3D12PipelineKey uber_constants_key;
    bool uber_constants_vs;
    bool uber_constants_cached;
    uint64_t uber_constants_offset;
    ID3D12QueryHeap *query_heap;
    uint32_t max_queries_in_flight;
    uint32_t num_queries_in_flight;
    uint32_t num_queries_resolved;
    uint32_t num_results_consumed;
    /* Query resolves up to this count are on the queue behind this fence. */
    uint32_t resolved_submitted_count;
    uint64_t resolved_submitted_fence;
    /* Time-box for report-driven submissions (see stamp_pending_reports). */
    int64_t last_report_submit_us;
    bool query_in_flight;
    uint64_t zpass_pixel_count_result;
    QSIMPLEQ_HEAD(, PGRAPHD3D12QueryReport) report_queue;
    QSIMPLEQ_HEAD(, PGRAPHD3D12DeferredRelease) deferred_releases;
    uint32_t draws_since_submit;
    /* Fence recorded at the previous guest flip (one frame in flight). */
    uint64_t last_flip_fence_value;
    /* Last uploaded draw-constants block; reset on submit and ring growth. */
    void *draw_constants_cache;
    size_t draw_constants_cache_size;
    uint64_t draw_constants_cache_offset;
    bool draw_constants_cached;
    /* pg->vsh_constants_generation when the cache last took them, +1 (0 = never) */
    uint32_t draw_constants_vsh_generation;
    uint32_t vertex_fetch_cache[NV2A_VERTEXSHADER_ATTRIBUTES][4];
    uint64_t vertex_fetch_offset;
    bool vertex_fetch_cached;
    void *pixel_constants_cache;
    size_t pixel_constants_cache_size;
    uint64_t pixel_constants_cache_offset;
    bool pixel_constants_cached;
    /* Perf telemetry, printed from the sync path. */
    struct {
        int64_t last_report_us;
        uint32_t syncs;
        uint32_t submits;
        uint32_t draws;
        uint64_t sync_wait_us;
        uint32_t ring_waits;
        uint64_t ring_wait_us;
        uint32_t allocator_waits;
        uint64_t allocator_wait_us;
        uint32_t vertex_upload_ranges;
        uint64_t vertex_upload_bytes;
        uint64_t constants_bytes;
        uint64_t surface_update_ticks; /* uwp_prof_now ticks */
        uint64_t surface_download_us;
        uint64_t surface_upload_us;
        uint64_t surface_create_us;
        uint32_t surface_creates;
        uint32_t readbacks_recorded;
        uint32_t readbacks_completed;
        uint32_t readbacks_dropped;
        uint32_t readbacks_adopted;
        /* Recreated targets whose restore found no stash (none), stopped
         * at a newer colour or zeta stash it could not patch (abort_c,
         * abort_z), or block-copied from a covering stash (cover). */
        uint32_t restore_none;
        uint32_t restore_abort_color;
        uint32_t restore_abort_zeta;
        uint32_t restore_cover;
        /* Texture binds copied from a stash, and binds restarted because
         * the overlap write-back retired the chosen stash. */
        uint32_t tex_from_stash;
        uint32_t tex_stash_retry;
        /* Stage binds that skipped revalidation. */
        uint32_t tex_fast;
        /* Textures rebuilt on the GPU from a Z24S8 surface. */
        uint32_t tex_zeta_view;
        uint32_t stash_done_by[10];
        uint64_t readback_complete_us;
        uint64_t flip_wait_us;
        uint64_t texture_bind_us;
        uint64_t pipeline_lookup_us;
        uint64_t constant_upload_us;
        uint64_t record_state_us;
        uint64_t vertex_setup_ticks; /* uwp_prof_now ticks */
        /* the parts of vertex setup for indexed draws (uwp_prof_now ticks),
         * and how often a draw repeats the previous draw's state */
        uint64_t vtx_scan_ticks, vtx_bind_ticks, vtx_sync_ticks,
            vtx_index_ticks;
        uint32_t vtx_draws, vtx_same, const_same, pso_same, pso_calls,
            pso_reused;
        /* draws whose draw constants changed in: inline values, vertex
         * shader constants, lighting, misc (clip/surface/fog/ff), vertex fetch */
        uint32_t const_chg[5];
        uint32_t downloads;
        /* draws merged into the held-back draw before them; held draws */
        uint32_t merged_draws, merge_holds;
        /* merge_try refusals by reason (draw.c) */
        uint32_t merge_fail[14];
        uint32_t hold_fail[9];
        uint64_t download_bytes;
        uint64_t download_wait_us;
        uint32_t shader_compiles;
        uint64_t shader_compile_us;
        uint32_t pso_creates;
        uint64_t pso_create_us;
        /* Of pso_creates: the CreateGraphicsPipelineState call that built
         * each, how many are DXIL, and how many asked for DXIL but were
         * built from DXBC. */
        uint64_t pso_driver_us;
        uint32_t pso_dxil;
        uint32_t pso_dxil_fallbacks;
        uint32_t pso_prewarmed;
        /* Interpreter fallback: keys newly skipped for, draws skipped, and
         * of the keys whose pipeline became available, the count and the
         * total and longest time from the first skipped draw. */
        uint32_t fallback_demand;
        uint32_t fallback_skipped;
        uint32_t fallback_ready;
        uint64_t fallback_ready_us;
        uint64_t fallback_ready_max_us;
        /* fallback=ps: draws with the interpreter pixel shader, and draws
         * whose pixel shader it cannot replace. */
        uint32_t fallback_uber_ps;
        uint32_t fallback_no_ps;
        /* fallback=all/debug: draws with an interpreter pipeline, misses
         * built synchronously because it was not ready, misses without a
         * fallback key, and interpreter builds with their total and
         * longest worker time. */
        uint32_t fallback_draws;
        uint32_t fallback_sync;
        uint32_t fallback_nokey;
        uint32_t fallback_builds;
        uint64_t fallback_build_us;
        uint64_t fallback_build_max_us;
        uint32_t report_flushes;
        uint64_t report_flush_us;
        /* Unlocked report fence waits: count, longest, and how many
         * submissions the GPU still had to finish when each began. */
        uint32_t report_waits;
        uint32_t report_wait_breaks;
        /* GPU time spent executing renderer command lists, from
         * timestamps at the start and end of each list. */
        uint64_t gpu_busy_ticks;
        uint32_t gpu_lists_timed;
        uint64_t report_wait_max_us;
        uint64_t report_wait_lag;
        uint32_t flips;
        uint32_t submit_src[7];
        uint64_t draw_setup_us;
        uint32_t finish_sampler;
        uint32_t finish_texture_oom;
        uint32_t finish_report;
        uint32_t pvideo_conversions;
        uint32_t pvideo_cache_hits;
        uint64_t pvideo_convert_us;
        uint32_t surface_downloads[
            PGRAPH_D3D12_SURFACE_DOWNLOAD_REASON_COUNT];
        /* Aliased render targets carried over by GPU copy instead of a
         * write-back and re-upload. */
        uint32_t surface_alias_copies;
    } perf;
    int64_t pfifo_idle_mark_us;
    int64_t vcpu_idle_mark_us;
    int64_t vcpu_exec_mark_us;
    int64_t vcpu_bql_mark_us;
    uint32_t mmio_read_marks[4];
    /*
     * The Xbox UWP driver does not make same-list buffer copies visible to
     * the input assembler despite correct barriers; UWP submits before drawing.
     */
    bool vertex_ram_copies_in_list;

    /*
     * Last-recorded state for skipping redundant binds; reset when the list
     * is opened. Pointer comparisons are safe because slots are release-
     * deferred behind the next fence.
     */
    bool record_root_signature_set;
    ID3D12PipelineState *record_pipeline_state;
    D3D12_GPU_VIRTUAL_ADDRESS record_vertex_ram_srv;
    D3D12_GPU_VIRTUAL_ADDRESS record_vertex_scratch_srv;
    uint32_t record_vertex_start;
    bool record_vertex_start_valid;
    uint64_t record_texture_srv_gpu[NV2A_MAX_TEXTURES];
    uint64_t record_texture_sampler_gpu[NV2A_MAX_TEXTURES];
    uint64_t record_framebuffer_srv_gpu;
    uint64_t record_rtv_ptr;
    uint64_t record_dsv_ptr;
    bool record_render_targets_valid;
    D3D12_VIEWPORT record_viewport;
    D3D12_RECT record_scissor;
    bool record_viewport_valid;
    float record_blend_factor[4];
    bool record_blend_factor_valid;
    uint32_t record_stencil_ref;
    bool record_stencil_ref_valid;
    D3D_PRIMITIVE_TOPOLOGY record_topology;
    bool record_topology_valid;
    QTAILQ_HEAD(, PGRAPHD3D12SurfaceBinding) surfaces;
    /* Frame in which expire_old_surfaces() last ran. */
    int surface_expire_frame_time;
    /* Old render targets whose pixels are copied into the new target that
     * aliases them; applied once the new target's own upload is done. */
    struct {
        PGRAPHD3D12SurfaceBinding *source;
        PGRAPHD3D12SurfaceBinding *target;
        unsigned int x;
        unsigned int y;
    } alias_copies[8];
    unsigned int alias_copy_count;
    /* Released surface images kept for reuse by same-shape surfaces. */
    struct {
        ID3D12Resource *resource;
        DXGI_FORMAT format;
        unsigned int width;
        unsigned int height;
        uint64_t bytes;
        bool color;
        D3D12_RESOURCE_STATES state;
        int frame_time;
    } surface_pool[32];
    unsigned int surface_pool_count;
    uint64_t surface_pool_bytes;
    /* Displaced surfaces whose images are kept until their bytes are
     * needed in guest memory or handed to a same-shaped successor. */
    QTAILQ_HEAD(, PGRAPHD3D12SurfaceBinding) stashed_surfaces;
    unsigned int stashed_surface_count;
    uint64_t stashed_surface_bytes;
    /* Guest ranges a vCPU touched while waiting for PFIFO (pg->lock). */
    struct {
        hwaddr addr;
        hwaddr len;
        bool write;
    } cpu_access_ranges[8];
    unsigned int cpu_access_range_count;
    bool cpu_access_range_overflow;
    PGRAPHD3D12SurfaceBinding *color_binding;
    PGRAPHD3D12SurfaceBinding *zeta_binding;
    uint64_t surface_callback_calls;
    uint64_t surface_callback_waits;
    uint64_t surface_callback_registers;
    uint64_t surface_callback_unregisters;
    bool downloads_pending;
    QemuEvent downloads_complete;
    bool download_dirty_surfaces_pending;
    QemuEvent dirty_surfaces_download_complete;
    /* The pending flush applies a surface scale change, not a reset or
     * state load. */
    bool surface_scale_flush_pending;
    Lru texture_cache;
    PGRAPHD3D12TextureBinding *texture_cache_entries;
    /* Guest bytes of each texture_cache_entries slot, packed so a dirty mark
     * scans 512 small rows instead of walking the cache's lists. */
    PGRAPHD3D12TextureRange *texture_cache_ranges;
    uint64_t texture_cache_bytes;
    PGRAPHD3D12TextureBinding *texture_binding[NV2A_MAX_TEXTURES];
    /* Custom texture loading and dumping (texture_replace.c). */
    PGRAPHD3D12TextureReplace *texture_replace;
    D3D12_GPU_DESCRIPTOR_HANDLE texture_sampler_gpu[NV2A_MAX_TEXTURES];
    /* Registers behind texture_binding, captured when it was validated. */
    uint32_t texture_stage_regs[NV2A_MAX_TEXTURES][8];
    /* Sampler registers behind texture_sampler_gpu, per valid stage bit. */
    uint32_t texture_sampler_regs[NV2A_MAX_TEXTURES][4];
    uint8_t texture_sampler_regs_valid;
    float texture_scale[NV2A_MAX_TEXTURES];
    GHashTable *sampler_cache;
    uint8_t texture_active_mask;
    /* Bumped when a surface or stash is added or changes its guest range. */
    uint64_t surface_layout_gen;
    ID3D12Resource *framebuffer_copy_resource;
    D3D12_RESOURCE_STATES framebuffer_copy_state;
    DXGI_FORMAT framebuffer_copy_format;
    uint32_t framebuffer_copy_width;
    uint32_t framebuffer_copy_height;
    bool framebuffer_copy_valid;
    bool framebuffer_copy_descriptor_allocated;
    uint32_t framebuffer_copy_srv_descriptor_index;
    D3D12_CPU_DESCRIPTOR_HANDLE framebuffer_copy_srv_cpu;
    D3D12_GPU_DESCRIPTOR_HANDLE framebuffer_copy_srv_gpu;

    uint32_t *expanded_index_scratch;
    size_t expanded_index_scratch_capacity;

    D3D12_INPUT_ELEMENT_DESC vertex_attribute_descriptions[NV2A_VERTEXSHADER_ATTRIBUTES];
    int vertex_attribute_to_description_location[NV2A_VERTEXSHADER_ATTRIBUTES];
    int num_active_vertex_attribute_descriptions;
    D3D12_VERTEX_BUFFER_VIEW vertex_buffer_views[NV2A_VERTEXSHADER_ATTRIBUTES];
    int num_active_vertex_buffer_views;
    hwaddr vertex_attribute_offsets[NV2A_VERTEXSHADER_ATTRIBUTES];
    uint32_t vertex_attribute_strides[NV2A_VERTEXSHADER_ATTRIBUTES];
    uint64_t vertex_attribute_spans[NV2A_VERTEXSHADER_ATTRIBUTES];
    /* Vertex-pulling source resolution for the current draw. */
    uint16_t vertex_fetch_inline_mask;
    uint64_t vertex_fetch_ring_base;
    uint16_t converted_vertex_attribute_mask;
    /* vertexFetch[].w of attributes pulled from RAM in a format the input
     * assembler lacks (3-component bytes and shorts, S32K); 0 for the rest */
    uint32_t vertex_fetch_raw_bits[NV2A_VERTEXSHADER_ATTRIBUTES];
    bool raw_fetch_off;         /* tune "rawfmt=off": CPU conversion */
    PGRAPHD3D12MemorySync vertex_ram_buffer_syncs[NV2A_VERTEXSHADER_ATTRIBUTES];
    size_t num_vertex_ram_buffer_syncs;

    D3D_FEATURE_LEVEL feature_level;
    D3D_SHADER_MODEL shader_model;
    D3D12_RESOURCE_BINDING_TIER resource_binding_tier;
    bool output_merger_logic_op;

    LUID adapter_luid;
    SIZE_T dedicated_video_memory;
    /* Allocatable local memory: the DXGI budget where it exceeds the
     * nominal dedicated size, as on unified-memory consoles. */
    uint64_t video_memory_budget;
    /* Buffer copies of a D24_UNORM_S8_UINT depth plane hold 32-bit float
     * depth instead of 24-bit UNORM; probed before the first transfer.
     * Read-backs and uploads are probed apart: the console driver reads the
     * plane back as float but takes uploads as UNORM. */
    bool zeta_depth_plane_probed;
    bool zeta_depth_plane_float;
    bool zeta_depth_upload_float;
    char adapter_description[128];
    bool debug_verbose_reports;
    bool debug_resource_dumps;
    bool debug_layer_enabled;
    uint64_t reported_diagnostics;
    uint64_t reported_unsupported_topology_mask;
    uint64_t reported_unsupported_color_surface_formats;
    uint64_t reported_unsupported_zeta_surface_formats;
    uint64_t reported_unsupported_blit_color_formats;
    uint64_t reported_unsupported_blit_operations;
    uint64_t reported_unsupported_texture_color_formats[NV2A_MAX_TEXTURES];
    uint32_t reported_texture_skip_reasons[NV2A_MAX_TEXTURES];
    bool reported_texture_bindings[NV2A_MAX_TEXTURES];
    uint32_t reported_unsupported_blend_state_keys[PGRAPH_D3D12_REPORTED_STATE_KEYS];
    uint32_t num_reported_unsupported_blend_state_keys;
    uint32_t reported_unsupported_raster_state_keys[PGRAPH_D3D12_REPORTED_STATE_KEYS];
    uint32_t num_reported_unsupported_raster_state_keys;
    uint32_t reported_unsupported_depth_stencil_state_keys[
        PGRAPH_D3D12_REPORTED_STATE_KEYS];
    uint32_t num_reported_unsupported_depth_stencil_state_keys;
    uint32_t reported_unsupported_alpha_state_keys[
        PGRAPH_D3D12_REPORTED_STATE_KEYS];
    uint32_t num_reported_unsupported_alpha_state_keys;
    uint32_t reported_programmable_shader_state_keys[PGRAPH_D3D12_REPORTED_STATE_KEYS];
    uint32_t num_reported_programmable_shader_state_keys;
    bool reported_missing_display_surface;
    hwaddr reported_missing_display_surface_addr;
    bool reported_blit_display_surface;
    hwaddr reported_blit_display_surface_addr;
    bool reported_display_gl_bridge;
    bool reported_raw_pcrtc_display_surface;
    hwaddr reported_raw_pcrtc_display_surface_addr;
    uint32_t reported_raw_pcrtc_display_surface_format;
    bool reported_pcrtc_scanout_surface;
    hwaddr reported_pcrtc_scanout_surface_addr;
    hwaddr reported_pcrtc_scanout_addr;
    uint32_t reported_pcrtc_scanout_line_offset;
    bool reported_display_cpu_scanout_fallback;
    hwaddr reported_display_cpu_scanout_addr;
    uint32_t reported_display_cpu_scanout_pitch;
    uint32_t reported_display_cpu_scanout_resource_pitch;
    bool reported_pvideo_overlay;
    hwaddr reported_pvideo_base;
    hwaddr reported_pvideo_offset;
    uint32_t reported_pvideo_size_in;
    uint32_t reported_pvideo_size_out;
    uint32_t num_reported_successful_image_blits;
    PGRAPHD3D12ImageBlitReportKey
        reported_successful_image_blits[PGRAPH_D3D12_REPORTED_IMAGE_BLITS];

    bool blit_display_valid;
    hwaddr blit_display_addr;
    uint64_t blit_display_size;
    uint32_t blit_display_width;
    uint32_t blit_display_height;
    uint32_t blit_display_pitch;
    uint32_t blit_display_bytes_per_pixel;
    uint32_t blit_display_color_format;
    int blit_display_draw_time;
    uint32_t blit_display_next_candidate;
    uint32_t blit_display_sequence;
    PGRAPHD3D12BlitDisplayCandidate
        blit_display_candidates[PGRAPH_D3D12_BLIT_DISPLAY_CANDIDATES];

    bool display_surface_valid;
    bool display_sync_download;
    /* Per-sync copy for the presenter; the live surface keeps taking draws. */
    ID3D12Resource *display_snapshot_resource;
    D3D12_RESOURCE_STATES display_snapshot_state;
    DXGI_FORMAT display_snapshot_format;
    uint32_t display_snapshot_width;
    uint32_t display_snapshot_height;
    bool display_snapshot_valid;
    /* Surface write serial captured in display_snapshot_resource. */
    const void *display_snapshot_source;
    /* Renderer queue value covering the snapshot copy. */
    uint64_t display_snapshot_ready_value;
    /* Last presenter hand-off of the current snapshot, and snapshots the
     * presenter may still read, reused once display_release_fence has
     * passed their value. */
    uint64_t display_snapshot_release_value;
    struct {
        ID3D12Resource *resource;
        D3D12_RESOURCE_STATES state;
        DXGI_FORMAT format;
        uint32_t width;
        uint32_t height;
        uint64_t release_value;
    } display_snapshot_spares[4];
    ID3D12Fence *display_release_fence;
    uint64_t display_release_next;
    int display_snapshot_draw_time;
    PGRAPHD3D12SurfaceBinding *display_surface_binding;
    hwaddr display_surface_addr;
    uint32_t display_surface_width;
    uint32_t display_surface_height;
    uint32_t display_surface_pitch;
    uint32_t display_surface_x_offset;
    uint32_t display_surface_y_offset;
    uint32_t display_surface_bytes_per_pixel;
    uint32_t display_surface_color_format;
    /*
     * The frame the presenter shows next, chosen by pgraph_d3d12_sync on the
     * FIFO thread under pg->lock. The UI thread copies only this under the
     * lock and never dereferences surface bindings.
     */
    struct {
        bool valid;            /* false: no frame can be produced */
        bool display;          /* a display surface is selected */
        bool cpu;              /* present from guest memory */
        ID3D12Resource *resource;
        D3D12_RESOURCE_STATES resource_state;
        DXGI_FORMAT dxgi_format;
        uint32_t resource_width;
        uint32_t resource_height;
        uint32_t resource_pitch;
        uint64_t ready_value;
        uint64_t release_value;
        unsigned int scale;
        hwaddr addr;
        uint32_t width;
        uint32_t height;
        uint32_t pitch;
        uint32_t x_offset;
        uint32_t y_offset;
        uint32_t bytes_per_pixel;
        uint32_t color_format;
    } display_frame;
    /* Diagnostic switch read from d3d12_tune.txt in the base directory:
     * the idle FIFO waits for the first queued report (default) or, with
     * "report_wait=last", for the last one. */
    bool tune_report_wait_head;
    /* Diagnostic switches from the same file (PGRAPH_D3D12_TUNE_*). */
#define PGRAPH_D3D12_TUNE_MASKED_CLEAR_CPU  (1u << 0)
#define PGRAPH_D3D12_TUNE_ZDEPTH_NATIVE     (1u << 1)
#define PGRAPH_D3D12_TUNE_QUADS_CPU         (1u << 2)
#define PGRAPH_D3D12_TUNE_FC_R0             (1u << 3)
#define PGRAPH_D3D12_TUNE_VTX_ALWAYS        (1u << 4)
#define PGRAPH_D3D12_TUNE_ZVIEW_READBACK    (1u << 5)
#define PGRAPH_D3D12_TUNE_ZINTERP_SCREEN    (1u << 6)
#define PGRAPH_D3D12_TUNE_STASH_OFF         (1u << 7)
#define PGRAPH_D3D12_TUNE_ALIAS_OFF         (1u << 8)
#define PGRAPH_D3D12_TUNE_S2T_SYNC          (1u << 9)
#define PGRAPH_D3D12_TUNE_S2T_OFF           (1u << 10)
#define PGRAPH_D3D12_TUNE_S2T_LEVELS_OFF    (1u << 11)
#define PGRAPH_D3D12_TUNE_REPORT_SYNC       (1u << 12)
#define PGRAPH_D3D12_TUNE_ATEST_OFF         (1u << 13)
#define PGRAPH_D3D12_TUNE_ZCLIP_CLAMP       (1u << 14)
#define PGRAPH_D3D12_TUNE_SKIP_SHADOW       (1u << 15)
#define PGRAPH_D3D12_TUNE_SHADOW_LIT        (1u << 16)
#define PGRAPH_D3D12_TUNE_SUBMIT_SYNC       (1u << 17)
/* Draw shaders compiled to DXIL where FXC is available; set unless the tune
 * file names "dxbc". */
#define PGRAPH_D3D12_TUNE_SHADER_DXIL       (1u << 18)
/* Changes only the pipeline key, forcing fresh PSOs for timing. */
#define PGRAPH_D3D12_TUNE_PSO_SALT          (1u << 19)
/* Pulled attributes decode with per-format code in each vertex shader
 * instead of the format in the draw constants. */
#define PGRAPH_D3D12_TUNE_VTXFMT_STATIC     (1u << 20)
/* Measurement only, never part of a pipeline key: "twintest" builds each
 * new draw pipeline again with one fixed-state field changed and releases
 * it; "ubertest=<id>" times the interpreter shaders once per new id. */
#define PGRAPH_D3D12_TUNE_TWIN_PSOS         (1u << 21)
#define PGRAPH_D3D12_TUNE_UBER_TEST         (1u << 22)
/* PGRAPHD3D12FallbackMode; never part of a pipeline key. */
#define PGRAPH_D3D12_TUNE_FALLBACK_SHIFT    23
#define PGRAPH_D3D12_TUNE_FALLBACK_MASK \
    (7u << PGRAPH_D3D12_TUNE_FALLBACK_SHIFT)
/* Pipeline key only, never from the tune file: fallback=ps draws the key
 * with the interpreter pixel shader. */
#define PGRAPH_D3D12_TUNE_UBER_PS           (1u << 26)
/* Separate PSOs of one recorded state; never recorded in pipeline lists. */
#define PGRAPH_D3D12_TUNE_KEY_ONLY_FLAGS \
    (PGRAPH_D3D12_TUNE_SHADER_DXIL | PGRAPH_D3D12_TUNE_PSO_SALT | \
     PGRAPH_D3D12_TUNE_UBER_PS)
#define PGRAPH_D3D12_TUNE_SHADER_FLAGS \
    (PGRAPH_D3D12_TUNE_ZDEPTH_NATIVE | PGRAPH_D3D12_TUNE_FC_R0 | \
     PGRAPH_D3D12_TUNE_ZINTERP_SCREEN | \
     PGRAPH_D3D12_TUNE_SHADOW_LIT | PGRAPH_D3D12_TUNE_VTXFMT_STATIC | \
     PGRAPH_D3D12_TUNE_KEY_ONLY_FLAGS)
    uint32_t tune_flags;
    /* Set by the first DXIL compile or pipeline failure; pipelines then use
     * DXBC until the dxil tune flag is cleared and set again. */
    bool dxil_disabled;
    /* Diagnostic draw capture: "capture=<id>" in the tune file logs every
     * draw and clear of the next frames once per new id. */
    char capture_id[32];
    /* Event log window (tune evlog=<id>), in guest frames. */
    char evlog_id[32];
    unsigned int evlog_frames_left;
    bool evlog_draws;
    /* tune "drawstat": a summary of the draws of a few guest frames */
    bool drawstat;
    /* Draw merging (tune "merge=off" disables): an indexed triangle draw
     * whose index list is already in the index ring is held back, so the
     * following draws that differ only in vertex addresses and indices can
     * append their (rebased) indices and share its one DrawIndexedInstanced.
     * Recorded by pgraph_d3d12_merge_emit() before anything else is. */
    bool merge_off;
    bool merge_nosync;
    struct {
        bool active;
        uint32_t generation;    /* pg->merge_generation when held */
        uint64_t submits;       /* command lists closed when held */
        uint64_t index_offset;  /* in the index ring */
        uint32_t index_count;
        uint32_t primitive_mode;
        uint32_t draws;
        PGRAPHState *pg;
        uint16_t attr_mask;     /* attributes read from RAM */
        uint16_t dma_mask;      /* of those, read through DMA object B */
        /* guest attribute offsets and strides of the held draw */
        uint64_t offsets[NV2A_VERTEXSHADER_ATTRIBUTES];
        uint32_t strides[NV2A_VERTEXSHADER_ATTRIBUTES];
        /* RAM pages the merged draws read, copied to the GPU once when the
         * held draw is recorded (page-rounded, coalesced) */
        PGRAPHD3D12MemorySync syncs[32];
        unsigned int num_syncs;
        /* draw_begin's surface update, put off while a merge may follow */
        bool surface_update_deferred;
        bool surface_color_write, surface_zeta_write;
    } merge;
    uint64_t submits;           /* command lists closed */
    /* set while a held draw's vertex copies are made: the staging ring grows
     * instead of submitting the list the held draw belongs to */
    bool ring_no_submit;
    /* Per-draw hash of the guest vertex bytes read, and the number of
     * mirror ranges the draw uploaded. */
    uint64_t evlog_vtx_hash;
    uint32_t evlog_vtx_uploads;
    /* Pixel-constant hashes already dumped in this event-log window. */
    uint32_t evlog_seen_ph[4096];
    uint32_t evlog_seen_key[1024];
    /* Draw constants of the first draw per frame of these keys. */
    uint32_t evlog_dump_key[2];
    int evlog_flip_draw_time;
    /* Diagnostic colour checkpoints (tune "cpk"): a 64x32 corner of
     * the colour target read back after every draw. */
    bool cpk_enabled;
    /* Guest-pixel origin of the checkpoint box; negative y is the
     * bottom edge. */
    int cpk_x;
    int cpk_y;
    ID3D12Resource *cpk_buffer;
    uint8_t *cpk_mapped;
    uint32_t cpk_next;
    uint32_t cpk_done;
    uint32_t cpk_draw_index;
    uint32_t cpk_last_frame;
    uint32_t cpk_last_hash;
    struct {
        uint64_t fence;
        uint32_t frame;
        uint32_t draw;
        uint32_t row_pitch;
        uint32_t row_bytes;
        uint32_t rows;
    } cpk_meta[4096];
    /* Diagnostic depth checkpoints (tune "zcpk=<key>,<key>"): the
     * zeta depth plane read back before the first draw of each key. */
    uint32_t zcpk_key[2];
    unsigned int zcpk_frame[2];
    ID3D12Resource *zcpk_buffer;
    uint8_t *zcpk_mapped;
    uint32_t zcpk_next;
    uint32_t zcpk_done;
    struct {
        uint64_t fence;
        uint32_t frame;
        uint32_t key;
        uint32_t draw;
        uint32_t row_pitch;
        uint32_t width;
        uint32_t height;
    } zcpk_meta[8];
    unsigned int evlog_dump_frame[2];
    int capture_frames_left;
    uint32_t capture_draws;
    /* Timestamp pairs bracketing renderer command lists; a slot is busy
     * until the fence of the list that wrote it has passed. */
#define PGRAPH_D3D12_GPU_TIMER_SLOTS 128
    ID3D12QueryHeap *gpu_timer_heap;
    ID3D12Resource *gpu_timer_readback;
    const uint64_t *gpu_timer_mapped;
    uint64_t gpu_timer_frequency;
    uint64_t gpu_timer_fence[PGRAPH_D3D12_GPU_TIMER_SLOTS];
    int gpu_timer_slot;                 /* slot of the open list, or -1 */
    unsigned int gpu_timer_next;
    /* UI-thread sync wait time, folded into perf by the FIFO thread. */
    uint64_t ui_sync_wait_us;
    uint64_t ui_syncs;
    /* Twin pipelines (tune "twintest"): base pipelines twinned and their
     * driver time, then the count and driver time of each twin kind. PSO
     * workers and the FIFO thread add; the perf report takes them. */
    struct {
        uint64_t sets;
        uint64_t base_us;
        uint64_t depth;
        uint64_t depth_us;
        uint64_t blend;
        uint64_t blend_us;
        uint64_t rtv;
        uint64_t rtv_us;
        uint64_t failed;
    } twin_pso;
    /* Id of the last uber test queued (tune "ubertest=<id>"). */
    char uber_test_id[32];
    uint8_t *display_pvideo_rgba;
    size_t display_pvideo_rgba_capacity;
    bool display_pvideo_cache_valid;
    uint64_t display_pvideo_generation;
    uint64_t display_pvideo_hash;
    hwaddr display_pvideo_base;
    hwaddr display_pvideo_offset;
    uint32_t display_pvideo_width;
    uint32_t display_pvideo_height;
    uint32_t display_pvideo_pitch;
    uint32_t gl_display_source_texture;
    uint32_t gl_display_source_width;
    uint32_t gl_display_source_height;
    uint32_t gl_display_source_color_format;
    uint32_t gl_display_pvideo_texture;
    uint32_t gl_display_texture;
    uint32_t gl_display_texture_width;
    uint32_t gl_display_texture_height;
    uint32_t gl_display_texture_color_format;
    uint32_t gl_display_fbo;
    uint32_t gl_display_vao;
    uint32_t gl_display_vbo;
    uint32_t gl_display_program;
    int32_t gl_display_tex_loc;
    int32_t gl_display_size_loc;
    int32_t gl_display_line_offset_loc;
    int32_t gl_display_pvideo_enable_loc;
    int32_t gl_display_pvideo_tex_loc;
    int32_t gl_display_pvideo_in_pos_loc;
    int32_t gl_display_pvideo_pos_loc;
    int32_t gl_display_pvideo_scale_loc;
    int32_t gl_display_pvideo_color_key_enable_loc;
    int32_t gl_display_pvideo_color_key_loc;
} PGRAPHD3D12State;

/* Whether the current draw pipeline runs the interpreter shaders, which
 * read b3 and declare every texture stage. */
static inline bool pgraph_d3d12_interpreter_pipeline(
    const PGRAPHD3D12State *r)
{
    return r->draw_pipeline_key_valid &&
           (r->draw_pipeline_fallback ||
            (r->draw_pipeline_key.tune_flags & PGRAPH_D3D12_TUNE_UBER_PS));
}

void pgraph_d3d12_warn_once(PGRAPHState *pg, PGRAPHD3D12Diagnostic diagnostic,
                            const char *fmt, ...) G_GNUC_PRINTF(3, 4);
bool pgraph_d3d12_verbose_reports(PGRAPHState *pg);
bool pgraph_d3d12_resource_dumps(PGRAPHState *pg);
void pgraph_d3d12_init_device(PGRAPHState *pg, Error **errp);
void pgraph_d3d12_finalize_device(PGRAPHState *pg);
const char *pgraph_d3d12_hresult_name(HRESULT hr);
const char *pgraph_d3d12_device_removed_detail(PGRAPHD3D12State *r,
                                               HRESULT hr, char *buf,
                                               size_t buflen);
void pgraph_d3d12_report_info_queue(PGRAPHD3D12State *r,
                                    const char *operation);
void pgraph_d3d12_init_command_objects(PGRAPHState *pg, Error **errp);
void pgraph_d3d12_finalize_command_objects(PGRAPHState *pg);
bool pgraph_d3d12_begin_commands(PGRAPHState *pg, Error **errp);
bool pgraph_d3d12_submit_commands(PGRAPHState *pg, Error **errp);
bool pgraph_d3d12_submit_commands_async(PGRAPHState *pg, Error **errp);
bool pgraph_d3d12_wait_for_submitted_fence(PGRAPHState *pg, uint64_t value);
bool pgraph_d3d12_finish(PGRAPHState *pg);
bool pgraph_d3d12_wait_for_fence_unlocked(PGRAPHState *pg, uint64_t value);
void pgraph_d3d12_collect_gpu_timers(PGRAPHD3D12State *r);
void pgraph_d3d12_capture_draw(PGRAPHState *pg, unsigned int topology);
void pgraph_d3d12_report_surface_churn(void);
void pgraph_d3d12_evlog(PGRAPHD3D12State *r, const char *fmt, ...)
    G_GNUC_PRINTF(2, 3);
void pgraph_d3d12_evlog_flush(void);
void pgraph_d3d12_process_color_checkpoints(PGRAPHD3D12State *r);
void pgraph_d3d12_record_depth_checkpoint(PGRAPHState *pg, uint32_t key,
                                          uint32_t draw);
void pgraph_d3d12_process_depth_checkpoints(PGRAPHD3D12State *r);
void pgraph_d3d12_capture_clear(PGRAPHState *pg, uint32_t parameter,
                                const char *path);
void pgraph_d3d12_release_display_snapshots(PGRAPHState *pg);
bool pgraph_d3d12_wait_for_fence_interruptible(PGRAPHState *pg,
                                               uint64_t value,
                                               bool (*interrupted)(void *),
                                               void *opaque);
bool pgraph_d3d12_wait_for_fence(PGRAPHState *pg, uint64_t fence_value,
                                 const char *operation);
bool pgraph_d3d12_ring_alloc(PGRAPHState *pg, PGRAPHD3D12Buffer *buffer,
                             uint64_t size, uint64_t alignment,
                             uint64_t *offset, Error **errp);
void pgraph_d3d12_buffers_on_submit(PGRAPHD3D12State *r,
                                    uint64_t fence_value);
void pgraph_d3d12_discard_unrecorded_draw_uploads(PGRAPHD3D12State *r);
void pgraph_d3d12_draw_uploads_recorded(PGRAPHD3D12State *r);
void pgraph_d3d12_buffers_retire_completed(PGRAPHD3D12State *r);
bool pgraph_d3d12_ensure_buffer_size(PGRAPHState *pg,
                                     PGRAPHD3D12Buffer *buffer,
                                     uint64_t required_size,
                                     D3D12_HEAP_TYPE heap_type,
                                     D3D12_RESOURCE_STATES initial_state,
                                     bool map_cpu, const char *label,
                                     Error **errp);
bool pgraph_d3d12_reset_surface_transfer_buffers(PGRAPHState *pg,
                                                  uint64_t target_size,
                                                  Error **errp);
void pgraph_d3d12_init_reports(PGRAPHState *pg, Error **errp);
void pgraph_d3d12_finalize_reports(PGRAPHState *pg);
void pgraph_d3d12_clear_report_value(NV2AState *d);
void pgraph_d3d12_get_report(NV2AState *d, uint32_t parameter);
void pgraph_d3d12_process_pending_reports(NV2AState *d);
void pgraph_d3d12_process_pending_reports_fifo(NV2AState *d);
void pgraph_d3d12_flush_reports(NV2AState *d);
void pgraph_d3d12_prepare_zpass_queries(NV2AState *d);
bool pgraph_d3d12_begin_zpass_query(NV2AState *d);
void pgraph_d3d12_end_zpass_query(NV2AState *d);
void pgraph_d3d12_flush_pending_query_resolves(PGRAPHD3D12State *r);
void pgraph_d3d12_init_descriptor_heaps(PGRAPHState *pg, Error **errp);
void pgraph_d3d12_finalize_descriptor_heaps(PGRAPHState *pg);
bool pgraph_d3d12_alloc_srv_descriptor(PGRAPHD3D12State *r, uint32_t *index,
                                       Error **errp);
void pgraph_d3d12_free_srv_descriptor(PGRAPHD3D12State *r, uint32_t index);
bool pgraph_d3d12_alloc_sampler_descriptor(PGRAPHD3D12State *r,
                                           uint32_t *index, Error **errp);
void pgraph_d3d12_free_sampler_descriptor(PGRAPHD3D12State *r,
                                          uint32_t index);
void pgraph_d3d12_free_rtv_descriptor(PGRAPHD3D12State *r, uint32_t index);
void pgraph_d3d12_free_dsv_descriptor(PGRAPHD3D12State *r, uint32_t index);
void pgraph_d3d12_defer_release(PGRAPHState *pg, ID3D12Resource *resource,
                                int32_t srv_descriptor_index,
                                int32_t rtv_descriptor_index,
                                int32_t dsv_descriptor_index,
                                int32_t sampler_descriptor_index);
void pgraph_d3d12_process_deferred_releases(PGRAPHState *pg);
void pgraph_d3d12_init_textures(NV2AState *d);
void pgraph_d3d12_finalize_textures(PGRAPHState *pg);
void pgraph_d3d12_flush_scale_dependent_textures(PGRAPHState *pg);
/* Rebinds every texture, e.g. after the custom texture settings change. */
void pgraph_d3d12_flush_textures(PGRAPHState *pg);

void pgraph_d3d12_init_texture_replace(PGRAPHState *pg);
void pgraph_d3d12_finalize_texture_replace(PGRAPHState *pg);
/*
 * Applies custom texture setting and title changes; FIFO thread, once per
 * frame. Returns true when cached textures must be bound again.
 */
bool pgraph_d3d12_texture_replace_update(PGRAPHState *pg);
/*
 * A texture's identity in a texture pack: the hashes of its texels and of
 * its palette. A pack file can name the palette or match any palette.
 */
typedef struct PGRAPHD3D12TexturePackHash {
    uint64_t texels;
    uint64_t palette;
    bool has_palette;
} PGRAPHD3D12TexturePackHash;

/* Whether a guest texture can have a custom replacement or be dumped. */
bool pgraph_d3d12_texture_replace_eligible(const TextureShape *shape,
                                           DXGI_FORMAT dxgi_format);
/* The title's custom texture for these guest texels, or NULL. */
const PGRAPHD3D12ReplacementTexture *pgraph_d3d12_texture_replacement_lookup(
    PGRAPHState *pg, const TextureShape *shape,
    const PGRAPHD3D12TexturePackHash *hash);
/* Whether dumping is on and this texture has not been dumped yet. */
bool pgraph_d3d12_texture_dump_wanted(PGRAPHState *pg,
                                      const TextureShape *shape,
                                      hwaddr vram_addr,
                                      const PGRAPHD3D12TexturePackHash *hash);
/*
 * Saves a texture as the shader samples it: face base levels in the
 * upload format, tightly packed rows of row_pitch bytes.
 */
void pgraph_d3d12_dump_texture(PGRAPHState *pg, const TextureShape *shape,
                               const PGRAPHD3D12TexturePackHash *hash,
                               DXGI_FORMAT dxgi_format,
                               const uint8_t *const *faces,
                               uint32_t row_pitch);
bool pgraph_d3d12_bind_textures(NV2AState *d, Error **errp);
void pgraph_d3d12_record_texture_bindings(PGRAPHState *pg);
void pgraph_d3d12_mark_textures_possibly_dirty(NV2AState *d, hwaddr addr,
                                               hwaddr size);
void pgraph_d3d12_init_surfaces(PGRAPHState *pg);
void pgraph_d3d12_finalize_surfaces(PGRAPHState *pg);
void pgraph_d3d12_surface_flush(NV2AState *d);
bool pgraph_d3d12_surface_flush_checked(NV2AState *d, Error **errp);
/* Flushes every surface, dropping those whose contents cannot be preserved. */
void pgraph_d3d12_surface_flush_lossy(NV2AState *d);
void pgraph_d3d12_surface_discard_all(NV2AState *d);
PGRAPHD3D12SurfaceBinding *pgraph_d3d12_surface_get(NV2AState *d,
                                                     hwaddr addr);
void pgraph_d3d12_process_pending_downloads(NV2AState *d);
void pgraph_d3d12_download_dirty_surfaces(NV2AState *d);
void pgraph_d3d12_surface_download_if_dirty(
    NV2AState *d, PGRAPHD3D12SurfaceBinding *surface);
bool pgraph_d3d12_surface_download_if_dirty_checked(
    NV2AState *d, PGRAPHD3D12SurfaceBinding *surface, Error **errp);
bool pgraph_d3d12_surface_download_if_dirty_checked_reason(
    NV2AState *d, PGRAPHD3D12SurfaceBinding *surface,
    PGRAPHD3D12SurfaceDownloadReason reason, Error **errp);
bool pgraph_d3d12_download_surfaces_checked_reason(
    NV2AState *d, PGRAPHD3D12SurfaceBinding *const *surfaces,
    unsigned int count, PGRAPHD3D12SurfaceDownloadReason reason,
    Error **errp);
bool pgraph_d3d12_record_surface_readbacks(
    NV2AState *d, PGRAPHD3D12SurfaceBinding *const *surfaces,
    unsigned int count, PGRAPHD3D12SurfaceDownloadReason reason,
    Error **errp);
/* Who needed a stashed image in guest memory. */
typedef enum PGRAPHD3D12StashTrigger {
    PGRAPH_D3D12_STASH_TRIGGER_UPLOAD,
    PGRAPH_D3D12_STASH_TRIGGER_DOWNLOAD,
    PGRAPH_D3D12_STASH_TRIGGER_TEXTURE,
    PGRAPH_D3D12_STASH_TRIGGER_BLIT,
    PGRAPH_D3D12_STASH_TRIGGER_RANGE,
    PGRAPH_D3D12_STASH_TRIGGER_CPU,
    PGRAPH_D3D12_STASH_TRIGGER_SYNC,
    PGRAPH_D3D12_STASH_TRIGGER_DISPLAY,
    PGRAPH_D3D12_STASH_TRIGGER_CAP,
    PGRAPH_D3D12_STASH_TRIGGER_FINAL,
} PGRAPHD3D12StashTrigger;

bool pgraph_d3d12_complete_readbacks_in_range(
    NV2AState *d, hwaddr addr, uint64_t size,
    PGRAPHD3D12StashTrigger trigger);
void pgraph_d3d12_complete_all_readbacks(NV2AState *d,
                                         PGRAPHD3D12StashTrigger trigger);
/* Discards every stashed image without writing it back (reset, loadvm). */
void pgraph_d3d12_drop_all_stashes(NV2AState *d);
/* Discards stashes lying wholly inside bytes a CPU write replaces. Older
 * stashes overlapping the range must have been written back first. */
void pgraph_d3d12_drop_readbacks_within(NV2AState *d, hwaddr addr,
                                        uint64_t size);
/* Hands every pooled surface image to the deferred-release queue. */
void pgraph_d3d12_purge_surface_pool(PGRAPHState *pg);
bool pgraph_d3d12_download_surfaces_in_range_if_dirty(
    PGRAPHState *pg, hwaddr start, hwaddr size, Error **errp);
void pgraph_d3d12_surface_mark_cpu_owned(
    PGRAPHD3D12SurfaceBinding *surface);
bool pgraph_d3d12_upload_surface_data(NV2AState *d,
                                       PGRAPHD3D12SurfaceBinding *surface,
                                       bool force, Error **errp);
bool pgraph_d3d12_upload_surface_data_with_state(
    NV2AState *d, PGRAPHD3D12SurfaceBinding *surface, bool force,
    D3D12_RESOURCE_STATES final_state, Error **errp);
bool pgraph_d3d12_prepare_framebuffer_fetch(PGRAPHState *pg, Error **errp);
void pgraph_d3d12_surface_update(NV2AState *d, bool upload,
                                 bool color_write, bool zeta_write);
bool pgraph_d3d12_download_display_surface(NV2AState *d, hwaddr display_addr,
                                           hwaddr pcrtc_start,
                                           uint32_t line_offset);
bool pgraph_d3d12_select_display_surface(NV2AState *d, hwaddr display_addr,
                                         hwaddr pcrtc_start,
                                         uint32_t line_offset,
                                         bool download);
void pgraph_d3d12_clear_surface(NV2AState *d, uint32_t parameter);
void pgraph_d3d12_set_surface_dirty(PGRAPHState *pg, bool color, bool zeta);
void pgraph_d3d12_bind_render_targets(PGRAPHState *pg);
void pgraph_d3d12_init_buffers(NV2AState *d, Error **errp);
void pgraph_d3d12_finalize_buffers(PGRAPHState *pg);
bool pgraph_d3d12_buffer_has_space_for(PGRAPHD3D12Buffer *buffer,
                                       uint64_t size, uint64_t alignment);
uint64_t pgraph_d3d12_append_to_buffer(PGRAPHD3D12Buffer *buffer,
                                       const void **data,
                                       const uint64_t *sizes,
                                       size_t count, uint64_t alignment);
bool pgraph_d3d12_copy_buffer(PGRAPHState *pg, PGRAPHD3D12Buffer *dst,
                              uint64_t dst_offset, PGRAPHD3D12Buffer *src,
                              uint64_t src_offset, uint64_t size,
                              D3D12_RESOURCE_STATES final_state,
                              Error **errp);
bool pgraph_d3d12_upload_staged_buffer(PGRAPHState *pg,
                                       PGRAPHD3D12Buffer *dst,
                                       PGRAPHD3D12Buffer *src, uint64_t size,
                                       D3D12_RESOURCE_STATES final_state,
                                       Error **errp);
void pgraph_d3d12_invalidate_vertex_ram_buffer(NV2AState *d);
bool pgraph_d3d12_vertex_ram_range_uploaded(PGRAPHState *pg, hwaddr offset,
                                            uint64_t size);
bool pgraph_d3d12_update_index_buffer(PGRAPHState *pg, const void *data,
                                      uint64_t size, uint64_t *offset,
                                      Error **errp);
bool pgraph_d3d12_update_vertex_inline_buffer(PGRAPHState *pg,
                                              const void **data,
                                              const uint64_t *sizes,
                                              size_t count, uint64_t *offset,
                                              Error **errp);
bool pgraph_d3d12_upload_vertex_inline_buffer(
    PGRAPHState *pg, const void **data, const uint64_t *sizes, size_t count,
    uint64_t alignment, uint64_t *offset, Error **errp);
bool pgraph_d3d12_update_uniform_buffer(PGRAPHState *pg, const void **data,
                                        const uint64_t *sizes, size_t count,
                                        uint64_t alignment, uint64_t *offset,
                                        Error **errp);
bool pgraph_d3d12_update_vertex_ram_buffer(PGRAPHState *pg, hwaddr offset,
                                            const void *data, uint64_t size,
                                            Error **errp);
bool pgraph_d3d12_update_vertex_ram_buffer_async(PGRAPHState *pg,
                                                 hwaddr offset,
                                                 const void *data,
                                                 uint64_t size,
                                                 Error **errp);
void pgraph_d3d12_init_pipelines(PGRAPHState *pg, Error **errp);
void pgraph_d3d12_finalize_pipelines(PGRAPHState *pg);
/* Shader cache requests from nv2a_shader_cache_set_title() and the
 * precompile mode, applied on the FIFO thread with pg->lock held. */
bool pgraph_d3d12_shader_cache_pending(PGRAPHState *pg);
void pgraph_d3d12_shader_cache_process(PGRAPHState *pg);
/* Writes recorded pipeline keys out, at most every few seconds unless
 * forced. FIFO thread, or any thread while the FIFO is held. */
void pgraph_d3d12_shader_cache_flush(PGRAPHState *pg, bool force);
/* The fallback mode in a set of tune flags. */
static inline PGRAPHD3D12FallbackMode pgraph_d3d12_fallback_mode(
    uint32_t tune_flags)
{
    return (PGRAPHD3D12FallbackMode)((tune_flags &
                                      PGRAPH_D3D12_TUNE_FALLBACK_MASK) >>
                                     PGRAPH_D3D12_TUNE_FALLBACK_SHIFT);
}
/* Measurement: queues the interpreter shader timing (tune "ubertest") to a
 * PSO worker. FIFO thread. */
void pgraph_d3d12_queue_uber_test(PGRAPHD3D12State *r, const char *id);
/* Interpreter shader sources of the uber test; g_free the result. */
char *pgraph_d3d12_uber_test_vs_source(void);
char *pgraph_d3d12_uber_test_ps_source(void);
ID3D12PipelineState *pgraph_d3d12_get_masked_clear_pipeline(
    PGRAPHState *pg, DXGI_FORMAT format, UINT8 write_mask, Error **errp);
ID3D12PipelineState *pgraph_d3d12_get_zeta_view_pipeline(
    PGRAPHState *pg, DXGI_FORMAT format, Error **errp);
bool pgraph_d3d12_begin_draw_commands(PGRAPHState *pg, bool *pending,
                                      Error **errp);
bool pgraph_d3d12_bind_vertex_attributes(NV2AState *d,
                                         unsigned int min_element,
                                         unsigned int max_element,
                                         bool inline_data,
                                         unsigned int inline_stride,
                                         unsigned int provoking_element,
                                         Error **errp);
void pgraph_d3d12_bind_vertex_attributes_inline(NV2AState *d);
void pgraph_d3d12_merge_emit(PGRAPHD3D12State *r);
uint32_t pgraph_d3d12_vertex_fetch_raw_bits(const VertexAttribute *attr,
                                            bool swizzle);
void pgraph_d3d12_merge_flush(NV2AState *d);
void pgraph_d3d12_update_provoking_attribute_values(
    NV2AState *d, unsigned int provoking_element, bool inline_data,
    unsigned int inline_stride);
void pgraph_d3d12_record_vertex_buffers(PGRAPHState *pg, uint16_t inline_map,
                                        uint64_t offset);
void pgraph_d3d12_record_inline_vertex_buffers(PGRAPHState *pg,
                                               uint64_t offset);
void pgraph_d3d12_draw_begin(NV2AState *d);
void pgraph_d3d12_draw_end(NV2AState *d);
void pgraph_d3d12_flush_draw(NV2AState *d);
void pgraph_d3d12_image_blit(NV2AState *d);
void pgraph_d3d12_invalidate_blit_display_candidates(PGRAPHD3D12State *r,
                                                     hwaddr addr,
                                                     uint64_t size);
void pgraph_d3d12_transition_resource(PGRAPHD3D12State *r,
                                      PGRAPHD3D12Buffer *buffer,
                                      D3D12_RESOURCE_STATES new_state);
void pgraph_d3d12_transition_raw_resource(PGRAPHD3D12State *r,
                                          ID3D12Resource *resource,
                                          D3D12_RESOURCE_STATES *current_state,
                                          D3D12_RESOURCE_STATES new_state);

#endif
