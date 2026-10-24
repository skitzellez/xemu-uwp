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

static void create_descriptor_heap(PGRAPHD3D12State *r,
                                   D3D12_DESCRIPTOR_HEAP_TYPE type,
                                   UINT count,
                                   D3D12_DESCRIPTOR_HEAP_FLAGS flags,
                                   ID3D12DescriptorHeap **heap,
                                   Error **errp)
{
    D3D12_DESCRIPTOR_HEAP_DESC desc;
    HRESULT hr;

    memset(&desc, 0, sizeof(desc));
    desc.Type = type;
    desc.NumDescriptors = count;
    desc.Flags = flags;

    hr = r->device->lpVtbl->CreateDescriptorHeap(
        r->device, &desc, &IID_ID3D12DescriptorHeap, (void **)heap);
    if (FAILED(hr)) {
        error_setg(errp,
                   "Failed to create Direct3D 12 descriptor heap type %d: "
                   "HRESULT 0x%08lx",
                   type, (unsigned long)hr);
    }
}

void pgraph_d3d12_init_descriptor_heaps(PGRAPHState *pg, Error **errp)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;

    create_descriptor_heap(r, D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
                           PGRAPH_D3D12_RTV_HEAP_SIZE,
                           D3D12_DESCRIPTOR_HEAP_FLAG_NONE, &r->rtv_heap,
                           errp);
    if (*errp) {
        return;
    }

    create_descriptor_heap(r, D3D12_DESCRIPTOR_HEAP_TYPE_DSV,
                           PGRAPH_D3D12_DSV_HEAP_SIZE,
                           D3D12_DESCRIPTOR_HEAP_FLAG_NONE, &r->dsv_heap,
                           errp);
    if (*errp) {
        return;
    }

    create_descriptor_heap(r, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                           PGRAPH_D3D12_CBV_SRV_UAV_HEAP_SIZE,
                           D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
                           &r->cbv_srv_uav_heap, errp);
    if (*errp) {
        return;
    }

    create_descriptor_heap(r, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
                           PGRAPH_D3D12_SAMPLER_HEAP_SIZE,
                           D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
                           &r->sampler_heap, errp);
    if (*errp) {
        return;
    }

    r->rtv_descriptor_size = r->device->lpVtbl->GetDescriptorHandleIncrementSize(
        r->device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    r->dsv_descriptor_size = r->device->lpVtbl->GetDescriptorHandleIncrementSize(
        r->device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    r->cbv_srv_uav_descriptor_size =
        r->device->lpVtbl->GetDescriptorHandleIncrementSize(
            r->device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    r->sampler_descriptor_size =
        r->device->lpVtbl->GetDescriptorHandleIncrementSize(
            r->device, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    r->next_rtv_descriptor = 0;
    r->next_dsv_descriptor = 0;
    r->num_free_rtv_descriptors = 0;
    r->num_free_dsv_descriptors = 0;
    r->next_cbv_srv_uav_descriptor = 0;
    r->next_sampler_descriptor = 0;
    r->num_free_cbv_srv_uav_descriptors = 0;
    r->num_free_sampler_descriptors = 0;
}

void pgraph_d3d12_finalize_descriptor_heaps(PGRAPHState *pg)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;

    if (r == NULL) {
        return;
    }

    D3D12_RELEASE(r->sampler_heap);
    D3D12_RELEASE(r->cbv_srv_uav_heap);
    D3D12_RELEASE(r->dsv_heap);
    D3D12_RELEASE(r->rtv_heap);
    r->next_rtv_descriptor = 0;
    r->next_dsv_descriptor = 0;
    r->num_free_rtv_descriptors = 0;
    r->num_free_dsv_descriptors = 0;
    r->next_cbv_srv_uav_descriptor = 0;
    r->next_sampler_descriptor = 0;
    r->num_free_cbv_srv_uav_descriptors = 0;
    r->num_free_sampler_descriptors = 0;
}

bool pgraph_d3d12_alloc_srv_descriptor(PGRAPHD3D12State *r, uint32_t *index,
                                       Error **errp)
{
    if (r->num_free_cbv_srv_uav_descriptors > 0) {
        *index = r->free_cbv_srv_uav_descriptors[
            --r->num_free_cbv_srv_uav_descriptors];
        return true;
    }

    if (r->next_cbv_srv_uav_descriptor >=
        PGRAPH_D3D12_CBV_SRV_UAV_HEAP_SIZE) {
        error_setg(errp, "Direct3D 12 CBV/SRV/UAV descriptor heap exhausted");
        return false;
    }

    *index = r->next_cbv_srv_uav_descriptor++;
    return true;
}

void pgraph_d3d12_free_srv_descriptor(PGRAPHD3D12State *r, uint32_t index)
{
    assert(r->num_free_cbv_srv_uav_descriptors <
           PGRAPH_D3D12_CBV_SRV_UAV_HEAP_SIZE);
    r->free_cbv_srv_uav_descriptors[r->num_free_cbv_srv_uav_descriptors++] =
        index;
}

bool pgraph_d3d12_alloc_sampler_descriptor(PGRAPHD3D12State *r,
                                           uint32_t *index, Error **errp)
{
    if (r->num_free_sampler_descriptors > 0) {
        *index = r->free_sampler_descriptors[
            --r->num_free_sampler_descriptors];
        return true;
    }

    if (r->next_sampler_descriptor >= PGRAPH_D3D12_SAMPLER_HEAP_SIZE) {
        error_setg(errp, "Direct3D 12 sampler descriptor heap exhausted");
        return false;
    }

    *index = r->next_sampler_descriptor++;
    return true;
}

void pgraph_d3d12_free_sampler_descriptor(PGRAPHD3D12State *r,
                                          uint32_t index)
{
    assert(r->num_free_sampler_descriptors < PGRAPH_D3D12_SAMPLER_HEAP_SIZE);
    r->free_sampler_descriptors[r->num_free_sampler_descriptors++] = index;
}

void pgraph_d3d12_free_rtv_descriptor(PGRAPHD3D12State *r, uint32_t index)
{
    assert(r->num_free_rtv_descriptors < PGRAPH_D3D12_RTV_HEAP_SIZE);
    r->free_rtv_descriptors[r->num_free_rtv_descriptors++] = index;
}

void pgraph_d3d12_free_dsv_descriptor(PGRAPHD3D12State *r, uint32_t index)
{
    assert(r->num_free_dsv_descriptors < PGRAPH_D3D12_DSV_HEAP_SIZE);
    r->free_dsv_descriptors[r->num_free_dsv_descriptors++] = index;
}
