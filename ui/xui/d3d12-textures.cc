// Copyright (c) 2026 Skitzelles - part of xemu for Xbox Dev Mode (github.com/skitzellez/xemu-uwp). GPL-2.0-or-later.
//
// New graphics: pictures for the menus as Direct3D 12 textures (see d3d12-textures.hh).
#include "qemu/osdep.h"
#include <d3d12.h>
#include <dxgi1_4.h>
#include <stdio.h>
#include <string.h>
#include <vector>

#include "ui/d3d12-presenter.h"
#include "d3d12-textures.hh"

/* The presenter keeps up to 3 frames in flight and waits for a frame's fence before it records into that frame
 * again, so a copy source written 3 frames ago is free. */
#define UPLOAD_SLOTS 3

struct XuiD3D12Texture {
    int width, height;
    UINT slot;
    ID3D12Resource *texture;
    ID3D12Resource *upload[UPLOAD_SLOTS];
    uint8_t *upload_mapped[UPLOAD_SLOTS];
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    unsigned next_upload;
    std::vector<uint8_t> pixels;
    bool dirty;
    bool readable;   /* PIXEL_SHADER_RESOURCE, else COPY_DEST */
};

struct Retired {
    XuiD3D12Texture *t;
    uint64_t frame;
};

static ID3D12Device *g_device;
static ID3D12DescriptorHeap *g_heap;
static UINT g_srv_size, g_heap_slots, g_next_slot = XEMU_D3D12_PRESENTER_SRV_COUNT;
static std::vector<UINT> g_free_slots;
static std::vector<XuiD3D12Texture *> g_textures;
static std::vector<Retired> g_retired;
static uint64_t g_frame;
static char g_adapter_name[128];

void XuiD3D12TexturesInit(void *device, void *srv_heap)
{
    g_device = (ID3D12Device *)device;
    g_heap = (ID3D12DescriptorHeap *)srv_heap;
    g_srv_size = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_heap_slots = g_heap->GetDesc().NumDescriptors;

    IDXGIFactory4 *factory = NULL;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        IDXGIAdapter1 *adapter = NULL;
        DXGI_ADAPTER_DESC1 desc;
        if (SUCCEEDED(factory->EnumAdapterByLuid(g_device->GetAdapterLuid(), IID_PPV_ARGS(&adapter))) &&
            SUCCEEDED(adapter->GetDesc1(&desc))) {
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, g_adapter_name, sizeof(g_adapter_name), NULL,
                                NULL);
        }
        if (adapter) {
            adapter->Release();
        }
        factory->Release();
    }
}

const char *XuiD3D12AdapterName(void)
{
    return g_adapter_name[0] ? g_adapter_name : "Direct3D 12";
}

static void release_texture(XuiD3D12Texture *t)
{
    for (int i = 0; i < UPLOAD_SLOTS; i++) {
        if (t->upload[i]) {
            t->upload[i]->Release();
        }
    }
    if (t->texture) {
        t->texture->Release();
    }
    if (t->slot) {
        g_free_slots.push_back(t->slot);
    }
    delete t;
}

static bool create_gpu_objects(XuiD3D12Texture *t)
{
    D3D12_HEAP_PROPERTIES heap = {};
    D3D12_RESOURCE_DESC desc = {};
    UINT64 upload_size = 0;
    HRESULT hr;

    if (!g_free_slots.empty()) {
        t->slot = g_free_slots.back();
        g_free_slots.pop_back();
    } else if (g_next_slot < g_heap_slots) {
        t->slot = g_next_slot++;
    } else {
        fprintf(stderr, "New graphics menus: no room for another picture\n");
        return false;
    }

    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = t->width;
    desc.Height = t->height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    hr = g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                                           IID_PPV_ARGS(&t->texture));
    if (FAILED(hr)) {
        fprintf(stderr, "New graphics menus: picture %dx%d not made (0x%08lx)\n", t->width, t->height,
                (unsigned long)hr);
        return false;
    }
    g_device->GetCopyableFootprints(&desc, 0, 1, 0, &t->footprint, NULL, NULL, &upload_size);

    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = upload_size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    for (int i = 0; i < UPLOAD_SLOTS; i++) {
        hr = g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                               NULL, IID_PPV_ARGS(&t->upload[i]));
        if (FAILED(hr) || FAILED(t->upload[i]->Map(0, NULL, (void **)&t->upload_mapped[i]))) {
            fprintf(stderr, "New graphics menus: picture upload buffer not made (0x%08lx)\n", (unsigned long)hr);
            return false;
        }
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_heap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += (SIZE_T)g_srv_size * t->slot;
    g_device->CreateShaderResourceView(t->texture, &srv, cpu);
    return true;
}

XuiD3D12Texture *XuiD3D12TextureNew(int width, int height)
{
    if (g_device == NULL || width <= 0 || height <= 0) {
        return NULL;
    }
    XuiD3D12Texture *t = new XuiD3D12Texture();
    t->width = width;
    t->height = height;
    t->pixels.assign((size_t)width * height * 4, 0);
    t->dirty = true;   /* never drawn before its first copy */
    if (!create_gpu_objects(t)) {
        release_texture(t);
        return NULL;
    }
    g_textures.push_back(t);
    return t;
}

void XuiD3D12TextureFree(XuiD3D12Texture *t)
{
    if (t == NULL) {
        return;
    }
    for (size_t i = 0; i < g_textures.size(); i++) {
        if (g_textures[i] == t) {
            g_textures.erase(g_textures.begin() + i);
            break;
        }
    }
    /* the frames still in flight may draw it */
    g_retired.push_back({ t, g_frame });
}

void XuiD3D12TextureUpdate(XuiD3D12Texture *t, const uint8_t *rgba)
{
    if (t == NULL) {
        return;
    }
    memcpy(t->pixels.data(), rgba, t->pixels.size());
    t->dirty = true;
}

ImTextureID XuiD3D12TextureId(XuiD3D12Texture *t)
{
    if (t == NULL) {
        return (ImTextureID)0;   /* dropped by xemu_hud_render_d3d12 */
    }
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_heap->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += (UINT64)g_srv_size * t->slot;
    return (ImTextureID)gpu.ptr;
}

int XuiD3D12TextureWidth(XuiD3D12Texture *t)
{
    return t ? t->width : 0;
}

int XuiD3D12TextureHeight(XuiD3D12Texture *t)
{
    return t ? t->height : 0;
}

static void transition(ID3D12GraphicsCommandList *cl, ID3D12Resource *r, D3D12_RESOURCE_STATES before,
                       D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    cl->ResourceBarrier(1, &b);
}

void XuiD3D12TexturesUpload(void *command_list)
{
    ID3D12GraphicsCommandList *cl = (ID3D12GraphicsCommandList *)command_list;

    g_frame++;
    for (size_t i = 0; i < g_retired.size();) {
        if (g_frame - g_retired[i].frame > UPLOAD_SLOTS) {
            release_texture(g_retired[i].t);
            g_retired.erase(g_retired.begin() + i);
        } else {
            i++;
        }
    }

    for (XuiD3D12Texture *t : g_textures) {
        if (!t->dirty) {
            continue;
        }
        unsigned n = t->next_upload++ % UPLOAD_SLOTS;
        uint8_t *dst = t->upload_mapped[n] + t->footprint.Offset;
        const size_t row = (size_t)t->width * 4;
        for (int y = 0; y < t->height; y++) {
            memcpy(dst + (size_t)y * t->footprint.Footprint.RowPitch, &t->pixels[y * row], row);
        }

        if (t->readable) {
            transition(cl, t->texture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        }
        D3D12_TEXTURE_COPY_LOCATION to = {}, from = {};
        to.pResource = t->texture;
        to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.SubresourceIndex = 0;
        from.pResource = t->upload[n];
        from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        from.PlacedFootprint = t->footprint;
        cl->CopyTextureRegion(&to, 0, 0, 0, &from, NULL);
        transition(cl, t->texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        t->readable = true;
        t->dirty = false;
    }
}
