/*
 * NV2A display frame handoff types
 *
 * Copyright (c) 2026
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#ifndef HW_XBOX_NV2A_DISPLAY_FRAME_H
#define HW_XBOX_NV2A_DISPLAY_FRAME_H

#include <stdbool.h>
#include <stdint.h>

typedef enum NV2ADisplayFrameType {
    NV2A_DISPLAY_FRAME_NONE,
    NV2A_DISPLAY_FRAME_D3D12_RESOURCE,
    NV2A_DISPLAY_FRAME_CPU_SURFACE,
} NV2ADisplayFrameType;

typedef struct NV2ADisplayFrameD3D12 {
    void *device;
    void *command_queue;
    void *resource;
    uint32_t resource_state;
    uint32_t dxgi_format;

    /* Host-resource dimensions, in physical texels. */
    uint32_t resource_width;
    uint32_t resource_height;
    uint32_t resource_scale;
    /* Guest source window, in unscaled surface pixels. */
    uint32_t source_width;
    uint32_t source_height;
    /* Resource and scanout row pitches, in guest bytes. */
    uint32_t resource_pitch;
    uint32_t source_pitch;
    uint32_t source_x;
    uint32_t source_y;
    uint32_t bytes_per_pixel;
    uint32_t color_format;
    uint64_t vram_addr;
    /* The presenter reads `resource` on its own queue: it makes that queue
     * wait for ready_fence >= ready_value (the renderer's copy) and signals
     * release_fence = release_value once its reading commands are queued.
     * Fences are ID3D12Fence pointers; NULL means nothing to synchronize. */
    void *ready_fence;
    uint64_t ready_value;
    void *release_fence;
    uint64_t release_value;
} NV2ADisplayFrameD3D12;

typedef struct NV2ADisplayFramePvideo {
    bool enabled;
    const void *rgba;
    uint32_t rgba_size;
    uint32_t width;
    uint32_t height;
    float in_x;
    float in_y;
    float scale_x;
    float scale_y;
    uint32_t out_x;
    uint32_t out_y;
    uint32_t out_width;
    uint32_t out_height;
    bool color_key_enabled;
    uint32_t color_key;
} NV2ADisplayFramePvideo;

typedef struct NV2ADisplayFrameCpuSurface {
    const void *data;
    uint32_t data_size;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bytes_per_pixel;
    uint32_t color_format;
    uint64_t vram_addr;
} NV2ADisplayFrameCpuSurface;

typedef struct NV2ADisplayFrame {
    NV2ADisplayFrameType type;
    /* Unscaled VGA raster dimensions (including interlace adjustment). */
    uint32_t display_width;
    uint32_t display_height;
    uint32_t display_scale;
    NV2ADisplayFrameD3D12 d3d12;
    NV2ADisplayFrameCpuSurface cpu;
    NV2ADisplayFramePvideo pvideo;
} NV2ADisplayFrame;

#endif
