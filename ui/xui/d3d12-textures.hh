// Copyright (c) 2026 Skitzelles - part of xemu for Xbox Dev Mode (github.com/skitzellez/xemu-uwp). GPL-2.0-or-later.
//
// New graphics: pictures for the menus (controller, memory unit, logo, icon) as Direct3D 12 textures in the
// presenter's descriptor heap. The pixels are made on the CPU; changed ones are copied to the GPU in the menu
// pass of the frame, before ImGui draws.
#pragma once
#include <stdint.h>
#include <imgui.h>

struct XuiD3D12Texture;

void XuiD3D12TexturesInit(void *device, void *srv_heap);
/* RGBA8, rows top to bottom as the texture reads them (v = 0 first) */
XuiD3D12Texture *XuiD3D12TextureNew(int width, int height);
void XuiD3D12TextureFree(XuiD3D12Texture *t);
/* takes a copy; the GPU gets it with the next frame's menus */
void XuiD3D12TextureUpdate(XuiD3D12Texture *t, const uint8_t *rgba);
ImTextureID XuiD3D12TextureId(XuiD3D12Texture *t);
int XuiD3D12TextureWidth(XuiD3D12Texture *t);
int XuiD3D12TextureHeight(XuiD3D12Texture *t);
/* once per frame, on the presenter's command list before ImGui_ImplDX12_RenderDrawData */
void XuiD3D12TexturesUpload(void *command_list);
/* the graphics chip's name, for the About page */
const char *XuiD3D12AdapterName(void);
