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
#include "renderer.h"

void pgraph_d3d12_transition_raw_resource(PGRAPHD3D12State *r,
                                          ID3D12Resource *resource,
                                          D3D12_RESOURCE_STATES *current_state,
                                          D3D12_RESOURCE_STATES new_state)
{
    D3D12_RESOURCE_BARRIER barrier;

    if (r == NULL || resource == NULL || current_state == NULL ||
        *current_state == new_state) {
        return;
    }

    assert(r->command_list_open);

    memset(&barrier, 0, sizeof(barrier));
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = *current_state;
    barrier.Transition.StateAfter = new_state;

    r->command_list->lpVtbl->ResourceBarrier(r->command_list, 1, &barrier);
    *current_state = new_state;
}

void pgraph_d3d12_transition_resource(PGRAPHD3D12State *r,
                                      PGRAPHD3D12Buffer *buffer,
                                      D3D12_RESOURCE_STATES new_state)
{
    if (buffer == NULL) {
        return;
    }

    pgraph_d3d12_transition_raw_resource(r, buffer->resource, &buffer->state,
                                         new_state);
}
