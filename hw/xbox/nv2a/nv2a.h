/*
 * QEMU Geforce NV2A implementation
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2020-2021 Matt Borgerson
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

#ifndef HW_NV2A_H
#define HW_NV2A_H

#include "hw/xbox/nv2a/display_frame.h"

void nv2a_init(PCIBus *bus, int devfn, MemoryRegion *ram);
void nv2a_context_init(void);
int nv2a_get_framebuffer_surface(void);
void nv2a_uwp_wait_display(void);
void nv2a_release_framebuffer_surface(void);
bool nv2a_get_display_frame(NV2ADisplayFrame *frame);
void nv2a_release_display_frame(void);
void nv2a_set_surface_scale_factor(unsigned int scale);
unsigned int nv2a_get_surface_scale_factor(void);
const uint8_t *nv2a_get_dac_palette(void);
int nv2a_get_screen_off(void);

typedef enum NV2AShaderPrecompileState {
    NV2A_SHADER_PRECOMPILE_IDLE,
    NV2A_SHADER_PRECOMPILE_LOADING,
    NV2A_SHADER_PRECOMPILE_COMPILING,
    NV2A_SHADER_PRECOMPILE_DONE,
} NV2AShaderPrecompileState;

typedef struct NV2AShaderPrecompileStatus {
    NV2AShaderPrecompileState state;
    uint32_t title_id;
    /* Pipelines queued for the title; done were built and failed could
     * not be, and together they reach total. */
    unsigned int total;
    unsigned int done;
    unsigned int failed;
    /* Pipelines saved in the title's list from earlier runs. */
    unsigned int recorded;
    bool foreground;
} NV2AShaderPrecompileStatus;

/*
 * Per-title shader cache. The title (0 when unknown) selects the pipeline
 * list that is recorded and precompiled. These may be called from any
 * thread; renderers without a shader cache report an idle status.
 */
void nv2a_shader_cache_set_title(uint32_t title_id);
/* Applies changed perf.cache_shaders and perf.precompile_shaders settings
 * to the current title. */
void nv2a_shader_cache_settings_changed(void);
void nv2a_shader_precompile_status(NV2AShaderPrecompileStatus *status);
/* Reloads the custom texture pack from a changed display.custom_textures
 * folder; the toggles apply on their own. Any thread. */
void nv2a_custom_textures_settings_changed(void);
/* Foreground compiles on every spare CPU at normal priority, for a wait
 * screen; background (the default) uses one low-priority worker. */
void nv2a_shader_precompile_set_foreground(bool foreground);

#endif
