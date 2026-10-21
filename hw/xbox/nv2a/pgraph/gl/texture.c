/*
 * Geforce NV2A PGRAPH OpenGL Renderer
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2015 Jannik Vogel
 * Copyright (c) 2018-2024 Matt Borgerson
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

#include "qemu/fast-hash.h"
#include "hw/xbox/nv2a/nv2a_int.h"
#include "hw/xbox/nv2a/pgraph/swizzle.h"
#include "hw/xbox/nv2a/pgraph/s3tc.h"
#include "hw/xbox/nv2a/pgraph/texture.h"
#include "hw/xbox/nv2a/pgraph/texsigns.h"
#include "debug.h"
#include "renderer.h"
#include "qemu/xemu-uwp.h"

static TextureBinding* generate_texture(const TextureShape s, const uint8_t *texture_data, const uint8_t *palette_data);
static void texture_binding_destroy(gpointer data);
#ifdef XEMU_UWP
static void uwp_texture_cache_trim(PGRAPHGLState *r);
static bool uwp_texture_refill(TextureBinding *binding, const TextureShape s, const uint8_t *texture_data,
                               const uint8_t *palette_data);
#endif

struct pgraph_texture_possibly_dirty_struct {
    hwaddr addr, end;
};

static void mark_textures_possibly_dirty_visitor(Lru *lru, LruNode *node, void *opaque)
{
    struct pgraph_texture_possibly_dirty_struct *test =
        (struct pgraph_texture_possibly_dirty_struct *)opaque;

    struct TextureLruNode *tnode = container_of(node, TextureLruNode, node);
    if (tnode->binding == NULL || tnode->possibly_dirty) {
        return;
    }

    uintptr_t k_tex_addr = tnode->key.texture_vram_offset;
    uintptr_t k_tex_end = k_tex_addr + tnode->key.texture_length - 1;
    bool overlapping = !(test->addr > k_tex_end || k_tex_addr > test->end);

    if (tnode->key.palette_length > 0) {
        uintptr_t k_pal_addr = tnode->key.palette_vram_offset;
        uintptr_t k_pal_end = k_pal_addr + tnode->key.palette_length - 1;
        overlapping |= !(test->addr > k_pal_end || k_pal_addr > test->end);
    }

    tnode->possibly_dirty |= overlapping;
}

void pgraph_gl_mark_textures_possibly_dirty(NV2AState *d,
    hwaddr addr, hwaddr size)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    hwaddr end = TARGET_PAGE_ALIGN(addr + size) - 1;
    addr &= TARGET_PAGE_MASK;
    assert(end <= memory_region_size(d->vram));

    struct pgraph_texture_possibly_dirty_struct test = {
        .addr = addr,
        .end = end,
    };

    lru_visit_active(&r->texture_cache,
                     mark_textures_possibly_dirty_visitor,
                     &test);
}

static bool check_texture_dirty(NV2AState *d, hwaddr addr, hwaddr size)
{
    hwaddr end = TARGET_PAGE_ALIGN(addr + size);
    addr &= TARGET_PAGE_MASK;
    assert(end < memory_region_size(d->vram));
    return memory_region_test_and_clear_dirty(d->vram, addr, end - addr,
                                              DIRTY_MEMORY_NV2A_TEX);
}

// Check if any of the pages spanned by the a texture are dirty.
static bool check_texture_possibly_dirty(NV2AState *d,
                                         hwaddr texture_vram_offset,
                                         unsigned int length,
                                         hwaddr palette_vram_offset,
                                         unsigned int palette_length)
{
    bool possibly_dirty = false;
    if (check_texture_dirty(d, texture_vram_offset, length)) {
        possibly_dirty = true;
        pgraph_gl_mark_textures_possibly_dirty(d, texture_vram_offset, length);
    }
    if (palette_length && check_texture_dirty(d, palette_vram_offset,
                                                     palette_length)) {
        possibly_dirty = true;
        pgraph_gl_mark_textures_possibly_dirty(d, palette_vram_offset,
                                            palette_length);
    }
    return possibly_dirty;
}

static void apply_texture_parameters(PGRAPHGLState *r,
                                     TextureBinding *binding,
                                     const BasicColorFormatInfo *f,
                                     unsigned int dimensionality,
                                     unsigned int filter,
                                     unsigned int address,
                                     bool is_bordered,
                                     uint32_t border_color,
                                     uint32_t max_anisotropy)
{
    unsigned int min_filter = GET_MASK(filter, NV_PGRAPH_TEXFILTER0_MIN);
    unsigned int mag_filter = GET_MASK(filter, NV_PGRAPH_TEXFILTER0_MAG);
    unsigned int lod_bias =
        GET_MASK(filter, NV_PGRAPH_TEXFILTER0_MIPMAP_LOD_BIAS);
    unsigned int addru = GET_MASK(address, NV_PGRAPH_TEXADDRESS0_ADDRU);
    unsigned int addrv = GET_MASK(address, NV_PGRAPH_TEXADDRESS0_ADDRV);
    unsigned int addrp = GET_MASK(address, NV_PGRAPH_TEXADDRESS0_ADDRP);

    if (f->linear) {
        /* somtimes games try to set mipmap min filters on linear textures.
             * this could indicate a bug... */
        switch (min_filter) {
        case NV_PGRAPH_TEXFILTER0_MIN_BOX_NEARESTLOD:
        case NV_PGRAPH_TEXFILTER0_MIN_BOX_TENT_LOD:
            min_filter = NV_PGRAPH_TEXFILTER0_MIN_BOX_LOD0;
            break;
        case NV_PGRAPH_TEXFILTER0_MIN_TENT_NEARESTLOD:
        case NV_PGRAPH_TEXFILTER0_MIN_TENT_TENT_LOD:
            min_filter = NV_PGRAPH_TEXFILTER0_MIN_TENT_LOD0;
            break;
        }
    }

    if (min_filter != binding->min_filter) {
        glTexParameteri(binding->gl_target, GL_TEXTURE_MIN_FILTER,
                        pgraph_texture_min_filter_gl_map[min_filter]);
        binding->min_filter = min_filter;
    }
    if (mag_filter != binding->mag_filter) {
        glTexParameteri(binding->gl_target, GL_TEXTURE_MAG_FILTER,
                        pgraph_texture_mag_filter_gl_map[mag_filter]);
        binding->mag_filter = mag_filter;
    }
    if (lod_bias != binding->lod_bias) {
        binding->lod_bias = lod_bias;
        glTexParameterf(binding->gl_target, GL_TEXTURE_LOD_BIAS,
                        pgraph_convert_lod_bias_to_float(lod_bias));
    }

    /* Texture wrapping */
    assert(addru < ARRAY_SIZE(pgraph_texture_addr_gl_map));
    if (addru != binding->addru) {
        glTexParameteri(binding->gl_target, GL_TEXTURE_WRAP_S,
                        pgraph_texture_addr_gl_map[addru]);
        binding->addru = addru;
    }
    bool needs_border_color = binding->addru == NV_PGRAPH_TEXADDRESS0_ADDRU_BORDER;
    if (dimensionality > 1) {
        if (addrv != binding->addrv) {
            assert(addrv < ARRAY_SIZE(pgraph_texture_addr_gl_map));
            glTexParameteri(binding->gl_target, GL_TEXTURE_WRAP_T,
                            pgraph_texture_addr_gl_map[addrv]);
            binding->addrv = addrv;
        }
        needs_border_color = needs_border_color || binding->addrv == NV_PGRAPH_TEXADDRESS0_ADDRU_BORDER;
    }
    if (dimensionality > 2) {
        if (addrp != binding->addrp) {
            assert(addrp < ARRAY_SIZE(pgraph_texture_addr_gl_map));
            glTexParameteri(binding->gl_target, GL_TEXTURE_WRAP_R,
                            pgraph_texture_addr_gl_map[addrp]);
            binding->addrp = addrp;
        }
        needs_border_color = needs_border_color || binding->addrp == NV_PGRAPH_TEXADDRESS0_ADDRU_BORDER;
    }

    if (r->supported_extensions.texture_filter_anisotropic) {
        glTexParameterf(binding->gl_target, GL_TEXTURE_MAX_ANISOTROPY_EXT,
                        max_anisotropy);
    }

    if (!is_bordered && needs_border_color) {
        if (!binding->border_color_set || binding->border_color != border_color) {
            /* FIXME: Color channels might be wrong order */
            GLfloat gl_border_color[4];
            pgraph_argb_pack32_to_rgba_float(border_color, gl_border_color);
            glTexParameterfv(binding->gl_target, GL_TEXTURE_BORDER_COLOR,
                             gl_border_color);

            binding->border_color_set = true;
            binding->border_color = border_color;
        }
    }
}

#ifdef XEMU_UWP
/*
 * One cube face from the render target drawn there. At 1x a plain copy; above 1x the target is larger than
 * the face, so it is scaled down by a blit on the GPU. Before, cubes above 1x went through memory: a
 * read-back with a GPU wait per face (car reflections in racing games, every frame).
 */
static void uwp_cube_face_copy(PGRAPHState *pg, SurfaceBinding *src, TextureBinding *binding, int face,
                               unsigned int width, unsigned int height)
{
    unsigned int s = pg->surface_scale_factor;
    if (s == 1) {
        glCopyImageSubData(src->gl_buffer, GL_TEXTURE_2D, 0, 0, 0, 0, binding->gl_texture, GL_TEXTURE_CUBE_MAP,
                           0, 0, 0, face, width, height, 1);
        return;
    }
    static GLuint fbo[2];
    if (!fbo[0]) {
        glGenFramebuffers(2, fbo);
    }
    GLint prev_read, prev_draw;
    GLboolean prev_cmask[4];
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw);
    glGetBooleanv(GL_COLOR_WRITEMASK, prev_cmask);
    GLboolean prev_scissor = glIsEnabled(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo[0]);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, src->gl_buffer, 0);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo[1]);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_CUBE_MAP_POSITIVE_X + face,
                           binding->gl_texture, 0);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBlitFramebuffer(0, 0, width * s, height * s, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, 0, 0);
    glColorMask(prev_cmask[0], prev_cmask[1], prev_cmask[2], prev_cmask[3]);
    if (prev_scissor) {
        glEnable(GL_SCISSOR_TEST);
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, prev_read);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prev_draw);
    static unsigned logged;
    if (!logged++) {
        uwp_log("cube map: %ux%u faces scaled down from %ux render targets on the GPU", width, height, s);
    }
}

/*
 * A cube map whose six faces are all render targets (a game drawing its own
 * reflections every frame): find the surface behind each face, so the faces
 * can be copied on the GPU instead of being read back and uploaded again.
 * Returns false when any face is not a matching surface.
 */
/*
 * The faces of a cube map texture that are render targets (surfaces) of the same size and
 * format. Returns how many faces have one (0 = the cube cannot take this path). A face
 * without a surface is fine as long as no surface with unsaved drawing covers it: its
 * pixels in memory are then current. Games often redraw only some faces of an environment
 * cube each frame; before, one face without a surface sent all six through memory, and
 * each read-back stalls on the GPU here (GL on D3D12).
 */
static int find_cube_face_surfaces(NV2AState *d, const TextureShape *shape,
                                   hwaddr addr, size_t length,
                                   SurfaceBinding **faces, int *newest)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;
    const ColorFormatInfo *f = &kelvin_color_format_gl_map[shape->color_format];
    TextureShape face_shape = *shape;
    face_shape.cubemap = false;
    hwaddr face_len = length / 6;
    int count = 0;

    /* above 1x the faces are scaled down by a blit (uwp_cube_face_copy), which takes colour only */
    if (shape->border || (pg->surface_scale_factor != 1 && f->depth)) {
        return 0;
    }
    *newest = 0;
    for (int face = 0; face < 6; face++) {
        hwaddr face_addr = addr + face * face_len;
        SurfaceBinding *s = pgraph_gl_surface_get(d, face_addr);
        faces[face] = NULL;
        if (s == NULL) {
            SurfaceBinding *o;
            QTAILQ_FOREACH(o, &r->surfaces, entry) {
                if (o->draw_dirty && o->vram_addr < face_addr + face_len &&
                    face_addr < o->vram_addr + o->size) {
                    return 0;
                }
            }
            continue;
        }
        if (s->fmt.gl_internal_format != f->gl_internal_format || (pg->surface_scale_factor != 1 && !s->color) ||
            !pgraph_gl_check_surface_to_texture_compatibility(s, &face_shape)) {
            return 0;
        }
        faces[face] = s;
        *newest = MAX(*newest, s->draw_time);
        count++;
    }
    return count;
}
#endif

void pgraph_gl_bind_textures(NV2AState *d)
{
    int i;
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    NV2A_GL_DGROUP_BEGIN("%s", __func__);

    for (i=0; i<NV2A_MAX_TEXTURES; i++) {
        bool enabled = pgraph_is_texture_enabled(pg, i);
        /* FIXME: What happens if texture is disabled but stage is active? */

        glActiveTexture(GL_TEXTURE0 + i);
        if (!enabled) {
            glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
            glBindTexture(GL_TEXTURE_1D, 0);
            glBindTexture(GL_TEXTURE_2D, 0);
            glBindTexture(GL_TEXTURE_3D, 0);
            continue;
        }

        uint32_t filter = pgraph_reg_r(pg, NV_PGRAPH_TEXFILTER0 + i*4);
        uint32_t address = pgraph_reg_r(pg, NV_PGRAPH_TEXADDRESS0 + i*4);
        uint32_t border_color = pgraph_reg_r(pg, NV_PGRAPH_BORDERCOLOR0 + i*4);
        uint32_t max_anisotropy =
            1 << (GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_TEXCTL0_0 + i*4),
                           NV_PGRAPH_TEXCTL0_0_MAX_ANISOTROPY));

        TextureShape state = pgraph_get_texture_shape(pg, i);
        hwaddr texture_vram_offset, palette_vram_offset;
        size_t length, palette_length;

        length = pgraph_get_texture_length(pg, &state);
        texture_vram_offset = pgraph_get_texture_phys_addr(pg, i);
        palette_vram_offset = pgraph_get_texture_palette_phys_addr_length(pg, i, &palette_length);

#ifdef XEMU_UWP
        /*
         * A texture (or palette) said to reach past the end of video memory: left unbound for this draw, like a
         * disabled stage, instead of stopping xemu (a game's start menu hit this assert: the whole app closed).
         */
        if ((texture_vram_offset + length) >= memory_region_size(d->vram) ||
            (palette_vram_offset + palette_length) >= memory_region_size(d->vram)) {
            static int told;
            if (told++ < 3) {
                uwp_log("texture: stage %d at 0x%llx, %zu bytes (palette 0x%llx, %zu) lies past the end of video "
                        "memory: left out of this draw", i, (unsigned long long)texture_vram_offset, length,
                        (unsigned long long)palette_vram_offset, palette_length);
            }
            glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
            glBindTexture(GL_TEXTURE_1D, 0);
            glBindTexture(GL_TEXTURE_2D, 0);
            glBindTexture(GL_TEXTURE_3D, 0);
            continue;
        }
#endif
        assert((texture_vram_offset + length) < memory_region_size(d->vram));
        assert((palette_vram_offset + palette_length)
               < memory_region_size(d->vram));
        bool is_indexed = (state.color_format ==
                NV097_SET_TEXTURE_FORMAT_COLOR_SZ_I8_A8R8G8B8);
        bool possibly_dirty = false;
        bool possibly_dirty_checked = false;

        SurfaceBinding *surface = pgraph_gl_surface_get(d, texture_vram_offset);
        TextureBinding *tbind = r->texture_binding[i];
        int surface_time = surface ? surface->draw_time : 0;
#ifdef XEMU_UWP
        SurfaceBinding *cube_faces[6];
        int cube_count = state.cubemap ?
            find_cube_face_surfaces(d, &state, texture_vram_offset, length,
                                    cube_faces, &surface_time) : 0;
        /* all six faces drawn on the GPU: copied from there */
        bool cube_s2t = surface && cube_count == 6;
        /* some: those copied from the GPU, the others uploaded from memory */
        bool cube_mixed = cube_count > 0 && cube_count < 6 && state.levels == 1;
        if (cube_count == 0 && state.cubemap) {
            surface_time = surface ? surface->draw_time : 0;
        }
#else
        const bool cube_mixed = false;
#endif
        if (!pg->texture_dirty[i] && tbind) {
            bool reusable = false;
            if (cube_mixed) {
                possibly_dirty = check_texture_possibly_dirty(
                        d, texture_vram_offset, length, palette_vram_offset,
                        is_indexed ? palette_length : 0);
                possibly_dirty_checked = true;
                reusable = tbind->draw_time == surface_time && !possibly_dirty;
            } else if (surface && tbind->draw_time == surface_time) {
                reusable = true;
            } else if (!surface) {
                possibly_dirty = check_texture_possibly_dirty(
                        d,
                        texture_vram_offset,
                        length,
                        palette_vram_offset,
                        is_indexed ? palette_length : 0);
                possibly_dirty_checked = true;
                reusable = !possibly_dirty;
            }

            if (reusable) {
                glBindTexture(r->texture_binding[i]->gl_target,
                              r->texture_binding[i]->gl_texture);
                apply_texture_parameters(r,
                                         r->texture_binding[i],
                                         &kelvin_color_format_info_map[state.color_format],
                                         state.dimensionality,
                                         filter,
                                         address,
                                         state.border,
                                         border_color,
                                         max_anisotropy);
                continue;
            }
        }

        /*
         * Check active surfaces to see if this texture was a render target
         */
        bool surf_to_tex = false;
        if (surface != NULL) {
            surf_to_tex = pgraph_gl_check_surface_to_texture_compatibility(
                    surface, &state);

            if (surf_to_tex && surface->upload_pending) {
                pgraph_gl_upload_surface_data(d, surface, false);
            }
#ifdef XEMU_UWP
            if (cube_s2t) {
                surf_to_tex = true;
                for (int face = 0; face < 6; face++) {
                    if (cube_faces[face]->upload_pending) {
                        pgraph_gl_upload_surface_data(d, cube_faces[face],
                                                      false);
                    }
                }
            }
#endif
        }

#ifdef XEMU_UWP
        /* a texture copied from a surface on the GPU never reads guest memory: no wait for write-backs there
         * (Conker: Live & Reloaded's pub, a 32x32 depth buffer read as a texture where a parked one is on its
         * way to memory) */
        if (!surf_to_tex) {
            int64_t t_wb = get_clock_realtime();
            if (pgraph_gl_uwp_wb_flush_range(d, texture_vram_offset, length)) {
                static unsigned logged;
                if (logged < 8) {
                    logged++;
                    uwp_log("texture at %x (%ux%u fmt 0x%x%s) waited %lld us for a depth write-back to land",
                            (unsigned)texture_vram_offset, state.width, state.height, state.color_format,
                            surface ? ", a surface there" : "", (long long)(get_clock_realtime() - t_wb) / 1000);
                }
            }
        }
#endif
        if (!surf_to_tex) {
            // FIXME: Restructure to support rendering surfaces to cubemap faces

            // Writeback any surfaces which this texture may index
            hwaddr tex_vram_end = texture_vram_offset + length - 1;
            QTAILQ_FOREACH(surface, &r->surfaces, entry) {
                hwaddr surf_vram_end = surface->vram_addr + surface->size - 1;
                bool overlapping = !(surface->vram_addr >= tex_vram_end
                                     || texture_vram_offset >= surf_vram_end);
#ifdef XEMU_UWP
                if (overlapping && cube_mixed) {
                    for (int face = 0; face < 6; face++) {
                        if (cube_faces[face] == surface) {
                            overlapping = false;
                        }
                    }
                }
#endif
                if (overlapping) {
#ifdef XEMU_UWP
                    if (surface->draw_dirty) {
                        UWP_TRACE("  tex miss unit %d: surface %s %ux%u pitch %u "
                                  "swz %d addr %x cfmt %d | tex %ux%u pitch %u "
                                  "fmt 0x%x levels %u cube %d addr %x len %u "
                                  "filter %x",
                                  i, surface->color ? "color" : "zeta",
                                  surface->width, surface->height,
                                  surface->pitch, surface->swizzle,
                                  (unsigned)surface->vram_addr,
                                  surface->shape.color_format, state.width,
                                  state.height, state.pitch,
                                  state.color_format, state.levels,
                                  state.cubemap,
                                  (unsigned)texture_vram_offset,
                                  (unsigned)length, filter);
                    }
                    UWP_DL_SITE(TEX_OVERLAP);
#endif
                    pgraph_gl_surface_download_if_dirty(d, surface);
                }
            }
        }

        TextureKey key;
        memset(&key, 0, sizeof(TextureKey));
        key.state = state;
        key.texture_vram_offset = texture_vram_offset;
        key.texture_length = length;
        if (is_indexed) {
            key.palette_vram_offset = palette_vram_offset;
            key.palette_length = palette_length;
        }

#ifdef XEMU_UWP
        uwp_texture_cache_trim(r);
#endif
        // Search for existing texture binding in cache
        uint64_t tex_binding_hash = fast_hash((uint8_t*)&key, sizeof(key));
        LruNode *found = lru_lookup(&r->texture_cache,
                                     tex_binding_hash, &key);
        TextureLruNode *key_out = container_of(found, TextureLruNode, node);
        possibly_dirty |= (key_out->binding == NULL) || key_out->possibly_dirty;

        if (!surf_to_tex && !possibly_dirty_checked) {
            possibly_dirty |= check_texture_possibly_dirty(
                    d,
                    texture_vram_offset,
                    length,
                    palette_vram_offset,
                    is_indexed ? palette_length : 0);
        }

        // Calculate hash of texture data, if necessary
        void *texture_data = (char*)d->vram_ptr + texture_vram_offset;
        void *palette_data = (char*)d->vram_ptr + palette_vram_offset;

        uint64_t tex_data_hash = 0;
        if (!surf_to_tex && possibly_dirty) {
            tex_data_hash = fast_hash(texture_data, length);
            if (is_indexed) {
                tex_data_hash ^= fast_hash(palette_data, palette_length);
            }
        }

        // Free existing binding, if texture data has changed
        bool must_destroy = (key_out->binding != NULL)
                            && possibly_dirty
                            && (key_out->binding->data_hash != tex_data_hash);
#ifdef XEMU_UWP
        /* new contents for a texture already on the GPU: written into it (uwp_texture_refill) */
        if (must_destroy && !surf_to_tex && uwp_texture_refill(key_out->binding, state, texture_data,
                                                                         palette_data)) {
            key_out->binding->data_hash = tex_data_hash;
            must_destroy = false;
        }
#endif
        if (must_destroy) {
            texture_binding_destroy(key_out->binding);
            key_out->binding = NULL;
        }

        if (key_out->binding == NULL) {
            // Must create the texture
            key_out->binding = generate_texture(state, texture_data, palette_data);
            UWP_TRACE("  tex upload unit %d: %ux%u fmt 0x%x levels %u addr %x "
                      "surf2tex %d", i, state.width, state.height,
                      state.color_format, state.levels,
                      (unsigned)texture_vram_offset, surf_to_tex);
            key_out->binding->data_hash = tex_data_hash;
            key_out->binding->scale = 1;
        } else {
            // Saved an upload! Reuse existing texture in graphics memory.
            glBindTexture(key_out->binding->gl_target,
                          key_out->binding->gl_texture);
        }

        key_out->possibly_dirty = false;
        TextureBinding *binding = key_out->binding;
        binding->refcnt++;

#ifdef XEMU_UWP
        if (cube_s2t) {
            if (binding->draw_time < surface_time) {
                UWP_PROF_BEGIN(t_s2t);
                if (binding->s2t_width != state.width) {
                    /* the faces only provide the base level */
                    glTexParameteri(binding->gl_target, GL_TEXTURE_BASE_LEVEL, 0);
                    glTexParameteri(binding->gl_target, GL_TEXTURE_MAX_LEVEL, 0);
                    binding->s2t_width = state.width;
                    binding->s2t_height = state.height;
                }
                for (int face = 0; face < 6; face++) {
                    uwp_cube_face_copy(pg, cube_faces[face], binding, face, state.width, state.height);
                }
                binding->draw_time = surface_time;
                binding->scale = 1;
                UWP_PROF_END(PF_S2T, t_s2t);
            }
        } else if (cube_mixed) {
            if (binding->draw_time < surface_time) {
                UWP_TRACE("  cube mixed unit %d: %d of 6 faces from the GPU, addr %x",
                          i, cube_count, (unsigned)texture_vram_offset);
                UWP_PROF_BEGIN(t_s2t);
                for (int face = 0; face < 6; face++) {
                    if (!cube_faces[face]) {
                        continue;
                    }
                    if (cube_faces[face]->upload_pending) {
                        pgraph_gl_upload_surface_data(d, cube_faces[face], false);
                        glBindTexture(binding->gl_target, binding->gl_texture);
                    }
                    uwp_cube_face_copy(pg, cube_faces[face], binding, face, state.width, state.height);
                }
                binding->draw_time = surface_time;
                binding->scale = 1;
                UWP_PROF_END(PF_S2T, t_s2t);
            }
        } else
#endif
        if (surf_to_tex && binding->draw_time < surface->draw_time) {

            trace_nv2a_pgraph_surface_render_to_texture(
                surface->vram_addr, surface->width, surface->height);
            UWP_PROF_BEGIN(t_s2t);
            pgraph_gl_render_surface_to_texture(d, surface, binding, &state, i);
            UWP_PROF_END(PF_S2T, t_s2t);
            binding->draw_time = surface->draw_time;
            binding->scale = pg->surface_scale_factor;
        }

        apply_texture_parameters(r,
                                 binding,
                                 &kelvin_color_format_info_map[state.color_format],
                                 state.dimensionality,
                                 filter,
                                 address,
                                 state.border,
                                 border_color,
                                 max_anisotropy);

        if (r->texture_binding[i]) {
            if (r->texture_binding[i]->gl_target != binding->gl_target) {
                glBindTexture(r->texture_binding[i]->gl_target, 0);
            }
            texture_binding_destroy(r->texture_binding[i]);
        }
        r->texture_binding[i] = binding;
        pg->texture_dirty[i] = false;
    }
    NV2A_GL_DGROUP_END();
}

static enum S3TC_DECOMPRESS_FORMAT
gl_internal_format_to_s3tc_enum(GLint gl_internal_format)
{
    switch (gl_internal_format) {
    case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT:
        return S3TC_DECOMPRESS_FORMAT_DXT1;
    case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT:
        return S3TC_DECOMPRESS_FORMAT_DXT3;
    case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:
        return S3TC_DECOMPRESS_FORMAT_DXT5;
    default:
        assert(!"Invalid gl_internal_format");
    }
}

static void get_converted_gl_format(const TextureShape *s,
                                    GLint *gl_internal_format,
                                    GLenum *gl_format,
                                    GLenum *gl_type)
{
    unsigned int bit_depth = pgraph_get_converted_bit_depth(s);

    if (bit_depth == 8) {
        *gl_internal_format = GL_RGBA8;
        *gl_format = GL_RGBA;
        *gl_type = GL_UNSIGNED_BYTE;
    } else if (bit_depth == 16) {
        *gl_internal_format = GL_RGBA16;
        *gl_format = GL_RGBA;
        *gl_type = GL_UNSIGNED_SHORT;
    } else {
        assert(false);
    }
}

static void upload_gl_texture(GLenum gl_target,
                              const TextureShape s,
                              const uint8_t *texture_data,
                              const uint8_t *palette_data)
{
    ColorFormatInfo f = kelvin_color_format_gl_map[s.color_format];
    nv2a_profile_inc_counter(NV2A_PROF_TEX_UPLOAD);

    unsigned int adjusted_width = s.width;
    unsigned int adjusted_height = s.height;
    unsigned int adjusted_pitch = s.pitch;
    unsigned int adjusted_depth = s.depth;
    if (!f.linear && s.border) {
        adjusted_width = MAX(16, adjusted_width * 2);
        adjusted_height = MAX(16, adjusted_height * 2);
        adjusted_pitch = adjusted_width * (s.pitch / s.width);
        adjusted_depth = MAX(16, s.depth * 2);
    }

    switch(gl_target) {
    case GL_TEXTURE_1D:
        assert(!"Invalid 1D gl target texture");
        break;
    case GL_TEXTURE_2D:
        if (f.linear) {
            /* Can't handle strides unaligned to pixels */
            assert(s.pitch % f.bytes_per_pixel == 0);

            uint8_t *converted = pgraph_convert_texture_data(
                s, texture_data, palette_data, adjusted_width, adjusted_height, 1,
                adjusted_pitch, 0, NULL);

            GLint gl_internal_format = f.gl_internal_format;
            GLenum gl_format = f.gl_format;
            GLenum gl_type = f.gl_type;
            if (converted) {
                get_converted_gl_format(&s, &gl_internal_format, &gl_format,
                                        &gl_type);
            }

            glPixelStorei(GL_UNPACK_ROW_LENGTH,
                          converted ? 0 : adjusted_pitch / f.bytes_per_pixel);
            glTexImage2D(GL_TEXTURE_2D, 0, gl_internal_format,
                         adjusted_width, adjusted_height, 0,
                         gl_format, gl_type,
                         converted ? converted : texture_data);

            if (converted) {
              g_free(converted);
            }

            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            break;
        }
        /* fallthru */
    case GL_TEXTURE_CUBE_MAP_POSITIVE_X:
    case GL_TEXTURE_CUBE_MAP_NEGATIVE_X:
    case GL_TEXTURE_CUBE_MAP_POSITIVE_Y:
    case GL_TEXTURE_CUBE_MAP_NEGATIVE_Y:
    case GL_TEXTURE_CUBE_MAP_POSITIVE_Z:
    case GL_TEXTURE_CUBE_MAP_NEGATIVE_Z: {

        unsigned int width = adjusted_width, height = adjusted_height;

        int level;
        for (level = 0; level < s.levels; level++) {
            width = MAX(width, 1);
            height = MAX(height, 1);

            if (f.gl_format == 0) { /* compressed */
                 // https://docs.microsoft.com/en-us/windows/win32/direct3d10/d3d10-graphics-programming-guide-resources-block-compression#virtual-size-versus-physical-size
                unsigned int block_size =
                    f.gl_internal_format == GL_COMPRESSED_RGBA_S3TC_DXT1_EXT ?
                        8 : 16;
                unsigned int physical_width = (width + 3) & ~3,
                             physical_height = (height + 3) & ~3;
#ifdef XEMU_UWP
                /*
                 * The GPU reads DXT blocks itself: hand them over as they are instead of
                 * unpacking every texture on the CPU (4-8x the data, and the unpacking was
                 * a large part of loading). Not for a bordered cube map, whose faces are
                 * cut out of the unpacked picture below; that depends only on the texture,
                 * so all of its levels go the same way.
                 */
                if (!(s.cubemap && adjusted_width != s.width)) {
                    glCompressedTexImage2D(
                        gl_target, level, f.gl_internal_format, width, height, 0,
                        physical_width / 4 * physical_height / 4 * block_size,
                        texture_data);
                    texture_data +=
                        physical_width / 4 * physical_height / 4 * block_size;
                    width /= 2;
                    height /= 2;
                    continue;
                }
#endif
                uint8_t *converted = s3tc_decompress_2d(
                    gl_internal_format_to_s3tc_enum(f.gl_internal_format),
                    texture_data, width, height);
                texsigns_inplace_to_unsigned_rgba(converted,
                    width * height * 4, s.channel_signs);
                unsigned int tex_width = width;
                unsigned int tex_height = height;

                if (s.cubemap && adjusted_width != s.width) {
                    // FIXME: Consider preserving the border.
                    // There does not seem to be a way to reference the border
                    // texels in a cubemap, so they are discarded.
                    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 4);
                    glPixelStorei(GL_UNPACK_SKIP_ROWS, 4);
                    tex_width = s.width;
                    tex_height = s.height;
                    if (physical_width == width) {
                        glPixelStorei(GL_UNPACK_ROW_LENGTH, adjusted_width);
                    }
                }

                glTexImage2D(gl_target, level, GL_RGBA8, tex_width, tex_height, 0,
                             GL_RGBA, GL_UNSIGNED_INT_8_8_8_8_REV, converted);
                g_free(converted);
                if (s.cubemap && adjusted_width != s.width) {
                    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
                    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
                    if (physical_width == width) {
                        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                    }
                }
                texture_data +=
                    physical_width / 4 * physical_height / 4 * block_size;
            } else {
                unsigned int pitch = width * f.bytes_per_pixel;
                uint8_t *unswizzled = (uint8_t*)g_malloc(height * pitch);
                unswizzle_rect(texture_data, width, height,
                               unswizzled, pitch, f.bytes_per_pixel);
                uint8_t *converted = pgraph_convert_texture_data(
                    s, unswizzled, palette_data, width, height, 1, pitch, 0,
                    NULL);

                GLint gl_internal_format = f.gl_internal_format;
                GLenum gl_format = f.gl_format;
                GLenum gl_type = f.gl_type;
                if (converted) {
                    get_converted_gl_format(&s, &gl_internal_format, &gl_format,
                                            &gl_type);
                }

                uint8_t *pixel_data = converted ? converted : unswizzled;
                unsigned int tex_width = width;
                unsigned int tex_height = height;

                if (s.cubemap && adjusted_width != s.width) {
                    // FIXME: Consider preserving the border.
                    // There does not seem to be a way to reference the border
                    // texels in a cubemap, so they are discarded.
                    glPixelStorei(GL_UNPACK_ROW_LENGTH, adjusted_width);
                    tex_width = s.width;
                    tex_height = s.height;
                    pixel_data += 4 * f.bytes_per_pixel + 4 * pitch;
                }

                glTexImage2D(gl_target, level, gl_internal_format, tex_width,
                             tex_height, 0, gl_format, gl_type, pixel_data);
                if (s.cubemap && s.border) {
                    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                }
                if (converted) {
                    g_free(converted);
                }
                g_free(unswizzled);

                texture_data += width * height * f.bytes_per_pixel;
            }

            width /= 2;
            height /= 2;
        }

        break;
    }
    case GL_TEXTURE_3D: {

        unsigned int width = adjusted_width;
        unsigned int height = adjusted_height;
        unsigned int depth = adjusted_depth;

        assert(f.linear == false);

        int level;
        for (level = 0; level < s.levels; level++) {
            if (f.gl_format == 0) { /* compressed */
                width = MAX(width, 1);
                height = MAX(height, 1);
                unsigned int physical_width = (width + 3) & ~3,
                             physical_height = (height + 3) & ~3;
                depth = MAX(depth, 1);

                unsigned int block_size;
                if (f.gl_internal_format == GL_COMPRESSED_RGBA_S3TC_DXT1_EXT) {
                    block_size = 8;
                } else {
                    block_size = 16;
                }

                size_t texture_size = physical_width/4 * physical_height/4 * depth * block_size;

                uint8_t *converted = s3tc_decompress_3d(
                    gl_internal_format_to_s3tc_enum(f.gl_internal_format),
                    texture_data, width, height, depth);
                texsigns_inplace_to_unsigned_rgba(converted,
                    width * height * depth * 4, s.channel_signs);

                glTexImage3D(gl_target, level,  GL_RGBA8,
                             width, height, depth, 0,
                             GL_RGBA, GL_UNSIGNED_INT_8_8_8_8_REV,
                             converted);

                g_free(converted);

                texture_data += texture_size;
            } else {
                width = MAX(width, 1);
                height = MAX(height, 1);
                depth = MAX(depth, 1);

                unsigned int row_pitch = width * f.bytes_per_pixel;
                unsigned int slice_pitch = row_pitch * height;
                uint8_t *unswizzled = (uint8_t*)g_malloc(slice_pitch * depth);
                unswizzle_box(texture_data, width, height, depth, unswizzled,
                               row_pitch, slice_pitch, f.bytes_per_pixel);

                uint8_t *converted = pgraph_convert_texture_data(
                    s, unswizzled, palette_data, width, height, depth,
                    row_pitch, slice_pitch, NULL);

                GLint gl_internal_format = f.gl_internal_format;
                GLenum gl_format = f.gl_format;
                GLenum gl_type = f.gl_type;
                if (converted) {
                    get_converted_gl_format(&s, &gl_internal_format, &gl_format,
                                            &gl_type);
                }

                glTexImage3D(gl_target, level, gl_internal_format,
                             width, height, depth, 0,
                             gl_format, gl_type,
                             converted ? converted : unswizzled);

                if (converted) {
                    g_free(converted);
                }
                g_free(unswizzled);

                texture_data += width * height * depth * f.bytes_per_pixel;
            }

            width /= 2;
            height /= 2;
            depth /= 2;
        }
        break;
    }
    default:
        assert(!"Invalid gl target texture type");
        break;
    }
}

#ifdef XEMU_UWP
/*
 * A linear 2D texture whose memory changed (videos, pictures the game draws with the CPU) was thrown away and
 * made anew: a new GPU allocation each time on the console's driver (Hot Wheels: Stunt Track Challenge, two
 * 640x480 pictures a frame). The new contents are written into the same texture instead, converted the same
 * way as upload_gl_texture does; the shape is the same (it is part of the cache key). Test file
 * tex-refill-off.txt: made anew as before.
 */
static bool uwp_texture_refill(TextureBinding *binding, const TextureShape s, const uint8_t *texture_data,
                               const uint8_t *palette_data)
{
    static int off = -1;
    if (off < 0) {
        extern bool xemu_uwp_test_file_exists(const char *name); /* ui/xemu.c */
        off = xemu_uwp_test_file_exists("tex-refill-off.txt");
        if (off) {
            uwp_log("textures: changed ones made anew (tex-refill-off.txt)");
        }
    }
    ColorFormatInfo f = kelvin_color_format_gl_map[s.color_format];
    /* not one a surface was copied into (its storage may have another size or format) */
    if (off || binding->s2t_width || binding->gl_target != GL_TEXTURE_2D || !f.linear || s.cubemap ||
        s.dimensionality != 2 ||
        f.gl_format == 0 || s.pitch % f.bytes_per_pixel) {
        return false;
    }
    UWP_PROF_BEGIN(t0);
    uint8_t *converted = pgraph_convert_texture_data(s, texture_data, palette_data, s.width, s.height, 1, s.pitch,
                                                     0, NULL);
    /* converted data has its own layout (RGBA8 or RGBA16, as upload_gl_texture gives it), not the table's */
    GLint conv_ifmt;
    GLenum gl_format = f.gl_format, gl_type = f.gl_type;
    if (converted) {
        get_converted_gl_format(&s, &conv_ifmt, &gl_format, &gl_type);
    }
    glBindTexture(GL_TEXTURE_2D, binding->gl_texture);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, converted ? 0 : s.pitch / f.bytes_per_pixel);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, s.width, s.height, gl_format, gl_type,
                    converted ? converted : texture_data);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    g_free(converted);
    UWP_PROF_END(PF_TEX_UP, t0);
    nv2a_profile_inc_counter(NV2A_PROF_TEX_UPLOAD);
    static unsigned logged;
    if (!logged++) {
        uwp_log("textures: a changed %ux%u picture written into its texture again", s.width, s.height);
    }
    return true;
}
#endif

static TextureBinding* generate_texture(const TextureShape s,
                                        const uint8_t *texture_data,
                                        const uint8_t *palette_data)
{
    ColorFormatInfo f = kelvin_color_format_gl_map[s.color_format];

    /* Create a new opengl texture */
    GLuint gl_texture;
    glGenTextures(1, &gl_texture);
#ifdef XEMU_UWP
    xemu_uwp_counters[UWP_CNT_TEX_NEW]++;
#endif

    GLenum gl_target;
    if (s.cubemap) {
        assert(f.linear == false);
        assert(s.dimensionality == 2);
        gl_target = GL_TEXTURE_CUBE_MAP;
    } else {
        if (f.linear) {
            gl_target = GL_TEXTURE_2D;
            assert(s.dimensionality == 2);
        } else {
            switch(s.dimensionality) {
            case 1: gl_target = GL_TEXTURE_1D; break;
            case 2: gl_target = GL_TEXTURE_2D; break;
            case 3: gl_target = GL_TEXTURE_3D; break;
            default:
                NV2A_GL_DPRINTF(true, "Invalid texture dimensionality: %u", (unsigned)s.dimensionality);
                assert(!"Invalid texture dimensionality");
                break;
            }
        }
    }

    glBindTexture(gl_target, gl_texture);

    NV2A_GL_DLABEL(GL_TEXTURE, gl_texture,
                   "offset: 0x%08lx, format: 0x%02X%s, %d dimensions%s, "
                   "width: %d, height: %d, depth: %d",
                   texture_data - g_nv2a->vram_ptr,
                   s.color_format, f.linear ? "" : " (SZ)",
                   s.dimensionality, s.cubemap ? " (Cubemap)" : "",
                   s.width, s.height, s.depth);

    if (gl_target == GL_TEXTURE_CUBE_MAP) {
        unsigned int block_size;
        if (f.gl_internal_format == GL_COMPRESSED_RGBA_S3TC_DXT1_EXT) {
            block_size = 8;
        } else {
            block_size = 16;
        }

        size_t length = 0;
        unsigned int w = s.width;
        unsigned int h = s.height;
        if (!f.linear && s.border) {
            w = MAX(16, w * 2);
            h = MAX(16, h * 2);
        }

        int level;
        for (level = 0; level < s.levels; level++) {
            if (f.gl_format == 0) {
                length += w/4 * h/4 * block_size;
            } else {
                length += w * h * f.bytes_per_pixel;
            }

            w /= 2;
            h /= 2;
        }

        length = (length + NV2A_CUBEMAP_FACE_ALIGNMENT - 1) & ~(NV2A_CUBEMAP_FACE_ALIGNMENT - 1);

        upload_gl_texture(GL_TEXTURE_CUBE_MAP_POSITIVE_X,
                          s, texture_data + 0 * length, palette_data);
        upload_gl_texture(GL_TEXTURE_CUBE_MAP_NEGATIVE_X,
                          s, texture_data + 1 * length, palette_data);
        upload_gl_texture(GL_TEXTURE_CUBE_MAP_POSITIVE_Y,
                          s, texture_data + 2 * length, palette_data);
        upload_gl_texture(GL_TEXTURE_CUBE_MAP_NEGATIVE_Y,
                          s, texture_data + 3 * length, palette_data);
        upload_gl_texture(GL_TEXTURE_CUBE_MAP_POSITIVE_Z,
                          s, texture_data + 4 * length, palette_data);
        upload_gl_texture(GL_TEXTURE_CUBE_MAP_NEGATIVE_Z,
                          s, texture_data + 5 * length, palette_data);
    } else {
        UWP_PROF_BEGIN(t0);
        upload_gl_texture(gl_target, s, texture_data, palette_data);
        UWP_PROF_END(PF_TEX_UP, t0);
    }

    /* Linear textures don't support mipmapping */
    if (!f.linear) {
        glTexParameteri(gl_target, GL_TEXTURE_BASE_LEVEL,
            s.min_mipmap_level);
        glTexParameteri(gl_target, GL_TEXTURE_MAX_LEVEL,
            s.levels - 1);
    }

    if (!s.channel_signs) {
        if (f.gl_swizzle_mask[0] != 0 || f.gl_swizzle_mask[1] != 0
            || f.gl_swizzle_mask[2] != 0 || f.gl_swizzle_mask[3] != 0) {
            glTexParameteriv(gl_target, GL_TEXTURE_SWIZZLE_RGBA,
                             (const GLint *)f.gl_swizzle_mask);
        }
    }

    TextureBinding* ret = (TextureBinding *)g_malloc(sizeof(TextureBinding));
    ret->gl_target = gl_target;
    ret->gl_texture = gl_texture;
    ret->refcnt = 1;
    ret->draw_time = 0;
    ret->data_hash = 0;
    ret->min_filter = 0xFFFFFFFF;
    ret->mag_filter = 0xFFFFFFFF;
    ret->lod_bias = 0xFFFFFFFF;
    ret->addru = 0xFFFFFFFF;
    ret->addrv = 0xFFFFFFFF;
    ret->addrp = 0xFFFFFFFF;
    ret->border_color_set = false;
#ifdef XEMU_UWP
    ret->s2t_width = 0;
    ret->s2t_height = 0;
    ret->s2t_ifmt = 0;
#endif
    return ret;
}

static void texture_binding_destroy(gpointer data)
{
    TextureBinding *binding = (TextureBinding *)data;
    assert(binding->refcnt > 0);
    binding->refcnt--;
    if (binding->refcnt == 0) {
        glDeleteTextures(1, &binding->gl_texture);
#ifdef XEMU_UWP
        xemu_uwp_counters[UWP_CNT_TEX_DEL]++;
#endif
        g_free(binding);
    }
}

#ifdef XEMU_UWP
/*
 * The cache holds up to 4096 textures and frees one only when it is full. A game with many big textures
 * (Ninja Gaiden: 557 kept after 80 s of play, about 5 MB each, almost none freed) used up all of the
 * console's 5 GB that way and crashed. So, at most twice a second: with less than 1 GB still allowed, the
 * least recently used quarter of the cache is freed. A texture that is bound keeps its own reference.
 */
static void uwp_texture_cache_trim(PGRAPHGLState *r)
{
    static int64_t next_check;
    int64_t now = g_get_monotonic_time();
    if (now < next_check) {
        return;
    }
    next_check = now + 500000;
    uint64_t used, left;
    if (!xemu_uwp_memory(&used, &left) || left > (1024ull << 20)) {
        return;
    }
    int kept = r->texture_cache.num_used, want = kept / 4, freed = 0;
    while (freed < want && lru_try_evict_one(&r->texture_cache)) {
        freed++;
    }
    uwp_log("texture cache: %llu MB in use, %llu MB still allowed: %d of %d textures freed",
            (unsigned long long)(used >> 20), (unsigned long long)(left >> 20), freed, kept);
}
#endif

/* functions for texture LRU cache */
static void texture_cache_entry_init(Lru *lru, LruNode *node, const void *key)
{
    TextureLruNode *tnode = container_of(node, TextureLruNode, node);
    memcpy(&tnode->key, key, sizeof(TextureKey));

    tnode->binding = NULL;
    tnode->possibly_dirty = false;
}

static void texture_cache_entry_post_evict(Lru *lru, LruNode *node)
{
    TextureLruNode *tnode = container_of(node, TextureLruNode, node);
    if (tnode->binding) {
        texture_binding_destroy(tnode->binding);
        tnode->binding = NULL;
        tnode->possibly_dirty = false;
    }
}

static bool texture_cache_entry_compare(Lru *lru, LruNode *node,
                                        const void *key)
{
    TextureLruNode *tnode = container_of(node, TextureLruNode, node);
    return memcmp(&tnode->key, key, sizeof(TextureKey));
}

void pgraph_gl_init_textures(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

#ifdef XEMU_UWP
    /*
     * 512 is too few for scenes with many detailed objects at once (a grid of race cars):
     * textures were evicted and rebuilt hundreds of times a frame, DXT ones unpacked on the
     * CPU each time. The guest's 64/128 MB bound what the entries can hold.
     */
    const size_t texture_cache_size = 4096;
#else
    const size_t texture_cache_size = 512;
#endif
    lru_init(&r->texture_cache);
    r->texture_cache_entries = malloc(texture_cache_size * sizeof(TextureLruNode));
    assert(r->texture_cache_entries != NULL);
    for (int i = 0; i < texture_cache_size; i++) {
        lru_add_free(&r->texture_cache, &r->texture_cache_entries[i].node);
    }

    r->texture_cache.init_node = texture_cache_entry_init;
    r->texture_cache.compare_nodes = texture_cache_entry_compare;
    r->texture_cache.post_node_evict = texture_cache_entry_post_evict;
}

void pgraph_gl_finalize_textures(PGRAPHState *pg)
{
    PGRAPHGLState *r = pg->gl_renderer_state;

    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        r->texture_binding[i] = NULL;
    }

    lru_flush(&r->texture_cache);
    free(r->texture_cache_entries);

    r->texture_cache_entries = NULL;
}
