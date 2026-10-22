/*
 * xemu Direct3D 12 presenter
 *
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef UI_D3D12_PRESENTER_H
#define UI_D3D12_PRESENTER_H

#include <stdbool.h>
#include <stdint.h>
#include <SDL3/SDL.h>

#include "hw/xbox/nv2a/display_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct XemuD3D12Presenter XemuD3D12Presenter;

/* Leading slots of the shared SRV heap used by the presenter; the HUD
 * allocates its textures after them. */
#define XEMU_D3D12_PRESENTER_SRV_COUNT 12

XemuD3D12Presenter *xemu_d3d12_presenter_create(SDL_Window *window);
void xemu_d3d12_presenter_destroy(XemuD3D12Presenter *presenter);

bool xemu_d3d12_presenter_begin_frame(XemuD3D12Presenter *presenter,
                                      const NV2ADisplayFrame *frame);
bool xemu_d3d12_presenter_end_frame(XemuD3D12Presenter *presenter,
                                    bool vsync);
bool xemu_d3d12_presenter_request_screenshot(XemuD3D12Presenter *presenter,
                                             const char *path);
const char *xemu_d3d12_presenter_get_error(XemuD3D12Presenter *presenter);
/* Waits until the swapchain can take another frame. */
void xemu_d3d12_presenter_wait_frame_latency(XemuD3D12Presenter *presenter);

void *xemu_d3d12_presenter_get_device(XemuD3D12Presenter *presenter);
void *xemu_d3d12_presenter_get_command_queue(XemuD3D12Presenter *presenter);
void *xemu_d3d12_presenter_get_command_list(XemuD3D12Presenter *presenter);
void *xemu_d3d12_presenter_get_srv_heap(XemuD3D12Presenter *presenter);
uint64_t xemu_d3d12_presenter_get_font_srv_cpu(XemuD3D12Presenter *presenter);
uint64_t xemu_d3d12_presenter_get_font_srv_gpu(XemuD3D12Presenter *presenter);

#ifdef __cplusplus
}
#endif

#endif
