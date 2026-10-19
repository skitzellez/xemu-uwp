/*
 * Geforce NV2A PGRAPH OpenGL Renderer
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2015 Jannik Vogel
 * Copyright (c) 2018-2025 Matt Borgerson
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

#include "hw/xbox/nv2a/pgraph/pgraph.h"
#include "ui/xemu-settings.h"
#include "hw/xbox/nv2a/nv2a_int.h"
#include "hw/xbox/nv2a/pgraph/swizzle.h"
#include "debug.h"
#include "renderer.h"
#include "qemu/xemu-uwp.h"

static void surface_download(NV2AState *d, SurfaceBinding *surface, bool force);
static void surface_download_to_buffer(NV2AState *d, SurfaceBinding *surface,
                                       bool swizzle, bool flip, bool downscale,
                                       uint8_t *pixels);
static void surface_get_dimensions(PGRAPHState *pg, unsigned int *width, unsigned int *height);
#ifdef XEMU_UWP
static bool uwp_wb_start(NV2AState *d, SurfaceBinding *surface);
static bool uwp_wb_flush_range(NV2AState *d, hwaddr addr, hwaddr len);
static void uwp_wb_flush_all(NV2AState *d);
static bool uwp_wb_overlaps(hwaddr addr, hwaddr len);
static void uwp_prefetch_color(NV2AState *d, SurfaceBinding *surface);
static bool uwp_zeta_wb_off(void);
static bool uwp_zeta_readpixels(void);
#endif

void pgraph_gl_set_surface_scale_factor(NV2AState *d, unsigned int scale)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    g_config.display.quality.surface_scale = scale < 1 ? 1 : scale;
#ifdef XEMU_UWP
    {   /* the player chose a scale in the menu: theirs from now on, exactly (ui/xemu.c uwp_auto_scale) */
        extern bool xemu_uwp_auto_scale_busy, xemu_uwp_auto_scale_off;
        extern unsigned int xemu_uwp_auto_scale_cap;
        if (!xemu_uwp_auto_scale_busy) {
            if (!xemu_uwp_auto_scale_off) {
                uwp_log("render scale: the player chose %ux; it stays exactly that this session", scale);
            }
            xemu_uwp_auto_scale_off = true;
            xemu_uwp_auto_scale_cap = 0;
        }
    }
#endif

    qemu_mutex_lock(&d->pfifo.lock);
    qatomic_set(&d->pfifo.halt, true);
    qemu_mutex_unlock(&d->pfifo.lock);

    qemu_mutex_lock(&d->pgraph.lock);
    qemu_event_reset(&r->dirty_surfaces_download_complete);
    qatomic_set(&r->download_dirty_surfaces_pending, true);
    qemu_mutex_unlock(&d->pgraph.lock);
    qemu_mutex_lock(&d->pfifo.lock);
    pfifo_kick(d);
    qemu_mutex_unlock(&d->pfifo.lock);
    qemu_event_wait(&r->dirty_surfaces_download_complete);

    qemu_mutex_lock(&d->pgraph.lock);
    qemu_event_reset(&d->pgraph.flush_complete);
    qatomic_set(&d->pgraph.flush_pending, true);
    qemu_mutex_unlock(&d->pgraph.lock);
    qemu_mutex_lock(&d->pfifo.lock);
    pfifo_kick(d);
    qemu_mutex_unlock(&d->pfifo.lock);
    qemu_event_wait(&d->pgraph.flush_complete);

    qemu_mutex_lock(&d->pfifo.lock);
    qatomic_set(&d->pfifo.halt, false);
    pfifo_kick(d);
    qemu_mutex_unlock(&d->pfifo.lock);
}

unsigned int pgraph_gl_get_surface_scale_factor(NV2AState *d)
{
    return d->pgraph.surface_scale_factor;
}

void pgraph_gl_reload_surface_scale_factor(PGRAPHState *pg)
{
    int factor = g_config.display.quality.surface_scale;
    pg->surface_scale_factor = factor < 1 ? 1 : factor;
#ifdef XEMU_UWP
    pg->surface_scale_factor = xemu_uwp_cap_surface_scale(pg->surface_scale_factor);
    {   /* held lower for now while the console's GPU is the limit (ui/xemu.c uwp_auto_scale) */
        extern unsigned int xemu_uwp_auto_scale_cap;
        if (xemu_uwp_auto_scale_cap && pg->surface_scale_factor > xemu_uwp_auto_scale_cap) {
            pg->surface_scale_factor = xemu_uwp_auto_scale_cap;
        }
    }
#endif
}

// FIXME: Move to common
static bool framebuffer_dirty(PGRAPHState *pg)
{
    bool shape_changed = memcmp(&pg->surface_shape, &pg->last_surface_shape,
                                sizeof(SurfaceShape)) != 0;
    if (!shape_changed || (!pg->surface_shape.color_format
            && !pg->surface_shape.zeta_format)) {
        return false;
    }
    return true;
}

void pgraph_gl_set_surface_dirty(PGRAPHState *pg, bool color, bool zeta)
{
    PGRAPHGLState *r = pg->gl_renderer_state;

    NV2A_DPRINTF("pgraph_set_surface_dirty(%d, %d) -- %d %d\n",
                 color, zeta,
                 pgraph_color_write_enabled(pg), pgraph_zeta_write_enabled(pg));
    /* FIXME: Does this apply to CLEARs too? */
    color = color && pgraph_color_write_enabled(pg);
    zeta = zeta && pgraph_zeta_write_enabled(pg);
    pg->surface_color.draw_dirty |= color;
    pg->surface_zeta.draw_dirty |= zeta;

    if (r->color_binding) {
        r->color_binding->draw_dirty |= color;
        r->color_binding->frame_time = pg->frame_time;
        r->color_binding->cleared = false;

    }

    if (r->zeta_binding) {
        r->zeta_binding->draw_dirty |= zeta;
        r->zeta_binding->frame_time = pg->frame_time;
        r->zeta_binding->cleared = false;

    }
}

static void init_render_to_texture(PGRAPHState *pg)
{
    PGRAPHGLState *r = pg->gl_renderer_state;

    const char *vs =
        "#version 330\n"
        "void main()\n"
        "{\n"
        "    float x = -1.0 + float((gl_VertexID & 1) << 2);\n"
        "    float y = -1.0 + float((gl_VertexID & 2) << 1);\n"
        "    gl_Position = vec4(x, y, 0, 1);\n"
        "}\n";
    const char *fs =
        "#version 330\n"
        "uniform sampler2D tex;\n"
        "uniform vec2 surface_size;\n"
        "layout(location = 0) out vec4 out_Color;\n"
        "void main()\n"
        "{\n"
        "    vec2 texCoord = gl_FragCoord.xy / textureSize(tex, 0).xy;\n"
        "    out_Color.rgba = texture(tex, texCoord);\n"
        "}\n";

    r->s2t_rndr.prog = pgraph_gl_compile_shader(vs, fs);
#ifdef XEMU_UWP
    /*
     * A Z24S8 depth surface read as an A8R8G8B8 / X8R8G8B8 texture (games do
     * this for depth-of-field and glare effects): the texel is the 32-bit
     * word (depth << 8 | stencil), so A, R, G are the depth's high, middle
     * and low byte. The stencil byte (B) is given as 0. Read as an R8G8B8A8
     * texture (rgba = 1; Conker: Live & Reloaded's glow and fog) the same word
     * gives R, G, B = high, middle, low byte and A = stencil.
     */
    const char *zeta_fs =
        "#version 330\n"
        "uniform sampler2D tex;\n"
        "uniform int rgba;\n"
        "layout(location = 0) out vec4 out_Color;\n"
        "void main()\n"
        "{\n"
        "    float d = texelFetch(tex, ivec2(gl_FragCoord.xy), 0).r;\n"
        "    uint v = min(uint(d * 16777215.0 + 0.5), 16777215u);\n"
        "    if (rgba != 0)\n"
        "        out_Color = vec4(float(v >> 16), float((v >> 8) & 255u),\n"
        "                         float(v & 255u), 0.0) / 255.0;\n"
        "    else\n"
        "        out_Color = vec4(float((v >> 8) & 255u), float(v & 255u), 0.0,\n"
        "                         float(v >> 16)) / 255.0;\n"
        "}\n";
    r->s2t_rndr.zeta_prog = pgraph_gl_compile_shader(vs, zeta_fs);
    r->s2t_rndr.zeta_tex_loc =
        glGetUniformLocation(r->s2t_rndr.zeta_prog, "tex");
    r->s2t_rndr.zeta_rgba_loc =
        glGetUniformLocation(r->s2t_rndr.zeta_prog, "rgba");
    /*
     * Reading a depth surface back to guest memory: glReadPixels of
     * GL_DEPTH_STENCIL returned one value for every pixel on the console's
     * GL-on-D3D12 (0x800000 for a whole 640x480 depth buffer), so games that
     * switch a depth buffer's memory between two shapes lost their depth and
     * drew black (Conker: Live & Reloaded, 640x480 and 32x32 at one address).
     * The depth is drawn into a colour image as the bytes of the word
     * (depth << 8 | stencil), lowest byte first, and that image is read
     * instead. The stencil byte is given as 0.
     */
    const char *zeta_dl_fs =
        "#version 330\n"
        "uniform sampler2D tex;\n"
        "layout(location = 0) out vec4 out_Color;\n"
        "void main()\n"
        "{\n"
        "    float d = texelFetch(tex, ivec2(gl_FragCoord.xy), 0).r;\n"
        "    uint v = min(uint(d * 16777215.0 + 0.5), 16777215u);\n"
        "    out_Color = vec4(0.0, float(v & 255u), float((v >> 8) & 255u),\n"
        "                     float(v >> 16)) / 255.0;\n"
        "}\n";
    r->s2t_rndr.zeta_dl_prog = pgraph_gl_compile_shader(vs, zeta_dl_fs);
    r->s2t_rndr.zeta_dl_tex_loc =
        glGetUniformLocation(r->s2t_rndr.zeta_dl_prog, "tex");
    glGenTextures(1, &r->s2t_rndr.zeta_dl_tex);
    r->s2t_rndr.zeta_dl_w = r->s2t_rndr.zeta_dl_h = 0;
#endif
    r->s2t_rndr.tex_loc = glGetUniformLocation(r->s2t_rndr.prog, "tex");
    r->s2t_rndr.surface_size_loc = glGetUniformLocation(r->s2t_rndr.prog,
                                                    "surface_size");

    glGenVertexArrays(1, &r->s2t_rndr.vao);
    glBindVertexArray(r->s2t_rndr.vao);
    glGenBuffers(1, &r->s2t_rndr.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, r->s2t_rndr.vbo);
    glBufferData(GL_ARRAY_BUFFER, 0, NULL, GL_STATIC_DRAW);
    glGenFramebuffers(1, &r->s2t_rndr.fbo);
}

static void finalize_render_to_texture(PGRAPHState *pg)
{
    PGRAPHGLState *r = pg->gl_renderer_state;

    glDeleteProgram(r->s2t_rndr.prog);
    r->s2t_rndr.prog = 0;
#ifdef XEMU_UWP
    glDeleteProgram(r->s2t_rndr.zeta_prog);
    glDeleteProgram(r->s2t_rndr.zeta_dl_prog);
    glDeleteTextures(1, &r->s2t_rndr.zeta_dl_tex);
    r->s2t_rndr.zeta_prog = 0;
#endif

    glDeleteVertexArrays(1, &r->s2t_rndr.vao);
    r->s2t_rndr.vao = 0;

    glDeleteBuffers(1, &r->s2t_rndr.vbo);
    r->s2t_rndr.vbo = 0;

    glDeleteFramebuffers(1, &r->s2t_rndr.fbo);
    r->s2t_rndr.fbo = 0;
}

#ifdef XEMU_UWP
/*
 * A 16-bit depth surface sampled as a 16-bit depth texture (shadow maps): both
 * are GL_DEPTH_COMPONENT16 here, so pgraph_gl_render_surface_to_texture can
 * copy the image on the GPU instead of reading the surface back every frame.
 */
/* test file zeta-tex-upstream.txt: depth surfaces sampled as textures go through memory, as upstream does */
static bool uwp_zeta_tex_upstream(void)
{
    static int off = -1;
    if (off < 0) {
        extern bool xemu_uwp_test_file_exists(const char *name); /* ui/xemu.c */
        off = xemu_uwp_test_file_exists("zeta-tex-upstream.txt");
        if (off) {
            uwp_log("depth texture test: depth surfaces read as textures through memory (zeta-tex-upstream.txt)");
        }
    }
    return off;
}

static bool uwp_zeta_to_texture_ok(const SurfaceBinding *surface,
                                   const TextureShape *shape)
{
    if (surface->color || surface->swizzle || uwp_zeta_tex_upstream()) {
        return false;
    }
    /* the texture gets the surface's own GL format, see the copy below */
    return (surface->fmt.gl_internal_format == GL_DEPTH_COMPONENT16 &&
            shape->color_format ==
                NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_Y16_FIXED) ||
           (surface->fmt.gl_internal_format == GL_DEPTH24_STENCIL8 &&
            shape->color_format ==
                NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_X8_Y24_FIXED);
}

/* A 24-bit depth surface read as a color texture: drawn with zeta_prog. */
static bool uwp_zeta_as_color_ok(const SurfaceBinding *surface,
                                 const TextureShape *shape)
{
    return !surface->color && !surface->swizzle && !uwp_zeta_tex_upstream() &&
           surface->fmt.gl_internal_format == GL_DEPTH24_STENCIL8 &&
           (shape->color_format ==
                NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8R8G8B8 ||
            shape->color_format ==
                NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X8R8G8B8 ||
            shape->color_format ==
                NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_R8G8B8A8);
}
#endif

static bool surface_to_texture_can_fastpath(SurfaceBinding *surface,
                                            TextureShape *shape)
{
    // FIXME: Better checks/handling on formats and surface-texture compat

    int surface_fmt = surface->shape.color_format;
    int texture_fmt = shape->color_format;

    if (!surface->color) {
#ifdef XEMU_UWP
        return uwp_zeta_to_texture_ok(surface, shape) ||
               uwp_zeta_as_color_ok(surface, shape);
#endif
        // FIXME: Support zeta to color
        return false;
    }

    switch (surface_fmt) {
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_Z1R5G5B5: switch (texture_fmt) {
        case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X1R5G5B5: return true;
        default: break;
        }
        break;
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5: switch (texture_fmt) {
        case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_R5G6B5: return true;
        case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R5G6B5: return true;
        default: break;
        }
        break;
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_Z8R8G8B8: switch(texture_fmt) {
        case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X8R8G8B8: return true;
        case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_X8R8G8B8: return true;
        default: break;
        }
        break;
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8: switch (texture_fmt) {
        case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8B8G8R8: return true;
        case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_R8G8B8A8: return true;
        case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8R8G8B8: return true;
        case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8R8G8B8: return true;
        default: break;
        }
        break;
    default: break;
    }

    trace_nv2a_pgraph_surface_texture_compat_failed(
        surface_fmt, texture_fmt);
    return false;
}

static void render_surface_to(NV2AState *d, SurfaceBinding *surface,
                              int texture_unit, GLuint gl_target,
                              GLuint gl_texture, unsigned int width,
                              unsigned int height, int tex_color_format)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    glActiveTexture(GL_TEXTURE0 + texture_unit);
    glBindFramebuffer(GL_FRAMEBUFFER, r->s2t_rndr.fbo);

    GLenum draw_buffers[1] = { GL_COLOR_ATTACHMENT0 };
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, gl_target,
                           gl_texture, 0);
    glDrawBuffers(1, draw_buffers);
    assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    assert(glGetError() == GL_NO_ERROR);

    float color[] = { 0.0f, 0.0f, 0.0f, 0.0f };
    glBindTexture(GL_TEXTURE_2D, surface->gl_buffer);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, color);

    glBindVertexArray(r->s2t_rndr.vao);
    glBindBuffer(GL_ARRAY_BUFFER, r->s2t_rndr.vbo);
#ifdef XEMU_UWP
    if (!surface->color) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
        glUseProgram(r->s2t_rndr.zeta_prog);
        glProgramUniform1i(r->s2t_rndr.zeta_prog, r->s2t_rndr.zeta_tex_loc,
                           texture_unit);
        glProgramUniform1i(r->s2t_rndr.zeta_prog, r->s2t_rndr.zeta_rgba_loc,
                           tex_color_format ==
                               NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_R8G8B8A8);
    } else
#endif
    {
    glUseProgram(r->s2t_rndr.prog);
    glProgramUniform1i(r->s2t_rndr.prog, r->s2t_rndr.tex_loc,
                       texture_unit);
    glProgramUniform2f(r->s2t_rndr.prog,
                       r->s2t_rndr.surface_size_loc, width, height);
    }

    glViewport(0, 0, width, height);
    glColorMask(true, true, true, true);
    glDisable(GL_DITHER);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, gl_target, 0,
                           0);
    glBindFramebuffer(GL_FRAMEBUFFER, r->gl_framebuffer);
    glBindVertexArray(r->gl_vertex_array);
    glBindTexture(gl_target, gl_texture);
    glUseProgram(
        r->shader_binding ? r->shader_binding->gl_program : 0);
}

static void render_surface_to_texture_slow(NV2AState *d,
                                           SurfaceBinding *surface,
                                           TextureBinding *texture,
                                           TextureShape *texture_shape,
                                           int texture_unit)
{
    PGRAPHState *pg = &d->pgraph;

    const ColorFormatInfo *f = &kelvin_color_format_gl_map[texture_shape->color_format];
    assert(texture_shape->color_format < ARRAY_SIZE(kelvin_color_format_gl_map));
    nv2a_profile_inc_counter(NV2A_PROF_SURF_TO_TEX_FALLBACK);

    glActiveTexture(GL_TEXTURE0 + texture_unit);
    glBindTexture(texture->gl_target, texture->gl_texture);

    unsigned int width = surface->width,
                 height = surface->height;
    pgraph_apply_scaling_factor(pg, &width, &height);

    size_t bufsize = width * height * surface->fmt.bytes_per_pixel;

    uint8_t *buf = g_malloc(bufsize);
#ifdef XEMU_UWP
    uwp_dl_counts[UWP_DL_S2T_SLOW]++;
#endif
    surface_download_to_buffer(d, surface, false, false, false, buf);

    width = texture_shape->width;
    height = texture_shape->height;
    pgraph_apply_scaling_factor(pg, &width, &height);

    glTexImage2D(texture->gl_target, 0, f->gl_internal_format, width, height, 0,
                 f->gl_format, f->gl_type, buf);
    g_free(buf);
    glBindTexture(texture->gl_target, texture->gl_texture);
}

/* Note: This function is intended to be called before PGRAPH configures GL
 * state for rendering; it will configure GL state here but only restore a
 * couple of items.
 */
void pgraph_gl_render_surface_to_texture(NV2AState *d, SurfaceBinding *surface,
                                      TextureBinding *texture,
                                      TextureShape *texture_shape,
                                      int texture_unit)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    const ColorFormatInfo *f =
        &kelvin_color_format_gl_map[texture_shape->color_format];
    assert(texture_shape->color_format < ARRAY_SIZE(kelvin_color_format_gl_map));

    nv2a_profile_inc_counter(NV2A_PROF_SURF_TO_TEX);

    if (!surface_to_texture_can_fastpath(surface, texture_shape)) {
        render_surface_to_texture_slow(d, surface, texture,
                                              texture_shape, texture_unit);
        return;
    }

    unsigned int width = texture_shape->width, height = texture_shape->height;
    pgraph_apply_scaling_factor(pg, &width, &height);

#ifdef XEMU_UWP
    /*
     * Same pixel format on both sides: copy the image directly instead of
     * drawing it. On GL-on-D3D12 the draw (framebuffer switch, new texture
     * storage every time) costs about half a millisecond, and games do this
     * several times a frame.
     */
    bool zeta_tex = uwp_zeta_to_texture_ok(surface, texture_shape);
    if ((zeta_tex ||
         (surface->color &&
          surface->fmt.gl_internal_format == f->gl_internal_format)) &&
        (texture->gl_target == GL_TEXTURE_2D ||
         texture->gl_target == GL_TEXTURE_RECTANGLE)) {
        glActiveTexture(GL_TEXTURE0 + texture_unit);
        glBindTexture(texture->gl_target, texture->gl_texture);
        if (texture->s2t_width != width || texture->s2t_height != height) {
            glTexParameteri(texture->gl_target, GL_TEXTURE_BASE_LEVEL, 0);
            glTexParameteri(texture->gl_target, GL_TEXTURE_MAX_LEVEL, 0);
            glTexParameteri(texture->gl_target, GL_TEXTURE_MIN_FILTER,
                            GL_LINEAR);
            if (zeta_tex) {
                /* a depth texture: the surface's exact format, so that the
                 * image can be copied (sampling it still gives the depth) */
                glTexImage2D(texture->gl_target, 0,
                             surface->fmt.gl_internal_format, width, height,
                             0, surface->fmt.gl_format, surface->fmt.gl_type,
                             NULL);
            } else
            glTexImage2D(texture->gl_target, 0, f->gl_internal_format, width,
                         height, 0, f->gl_format, f->gl_type, NULL);
            texture->s2t_width = width;
            texture->s2t_height = height;
        }
        glCopyImageSubData(surface->gl_buffer, GL_TEXTURE_2D, 0, 0, 0, 0,
                           texture->gl_texture, texture->gl_target, 0, 0, 0, 0,
                           width, height, 1);
        return;
    }
#endif

    glActiveTexture(GL_TEXTURE0 + texture_unit);
    glBindTexture(texture->gl_target, texture->gl_texture);
    glTexParameteri(texture->gl_target, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(texture->gl_target, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(texture->gl_target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexImage2D(texture->gl_target, 0, f->gl_internal_format, width, height, 0,
                 f->gl_format, f->gl_type, NULL);
    glBindTexture(texture->gl_target, 0);
    render_surface_to(d, surface, texture_unit, texture->gl_target,
                             texture->gl_texture, width, height,
                             texture_shape->color_format);
    glBindTexture(texture->gl_target, texture->gl_texture);
    glUseProgram(
        r->shader_binding ? r->shader_binding->gl_program : 0);
}

bool pgraph_gl_check_surface_to_texture_compatibility(
    const SurfaceBinding *surface,
    const TextureShape *shape)
{
    // FIXME: Better checks/handling on formats and surface-texture compat

    if ((!surface->swizzle && surface->pitch != shape->pitch) ||
        surface->width != shape->width ||
        surface->height != shape->height) {
        return false;
    }

    int surface_fmt = surface->shape.color_format;
    int texture_fmt = shape->color_format;

    if (!surface->color) {
#ifdef XEMU_UWP
        return !shape->cubemap && shape->levels == 1 &&
               (uwp_zeta_to_texture_ok(surface, shape) ||
                uwp_zeta_as_color_ok(surface, shape));
#endif
        // FIXME: Support zeta to color
        return false;
    }

    if (shape->cubemap) {
        // FIXME: Support rendering surface to cubemap face
        return false;
    }

#ifndef XEMU_UWP
    if (shape->levels > 1) {
        // FIXME: Support rendering surface to mip levels
        return false;
    }
#else
    /*
     * On the Xbox port a surface read-back stalls on the GPU (GL on D3D12),
     * and games that render into mipmapped textures do it several times a
     * frame. Take the surface as the base level instead: the surface-to-texture
     * copy limits sampling to level 0, the only level the surface holds.
     */
#endif

    switch (surface_fmt) {
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_Z1R5G5B5: switch (texture_fmt) {
        case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X1R5G5B5: return true;
        default: break;
        }
        break;
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5: switch (texture_fmt) {
        case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_R5G6B5: return true;
        case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R5G6B5: return true;
        default: break;
        }
        break;
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_Z8R8G8B8: switch(texture_fmt) {
        case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X8R8G8B8: return true;
        case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_X8R8G8B8: return true;
        default: break;
        }
        break;
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8: switch (texture_fmt) {
        case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8B8G8R8: return true;
        case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_R8G8B8A8: return true;
        case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8R8G8B8: return true;
        case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8R8G8B8: return true;
        default: break;
        }
        break;
    default:
        break;
    }

    trace_nv2a_pgraph_surface_texture_compat_failed(
        surface_fmt, texture_fmt);
    return false;
}

static bool check_surface_overlaps_range(const SurfaceBinding *surface,
                                         hwaddr range_start, hwaddr range_len)
{
    hwaddr surface_end = surface->vram_addr + surface->size;
    hwaddr range_end = range_start + range_len;
    return !(surface->vram_addr >= range_end || range_start >= surface_end);
}

static void surface_access_callback(void *opaque, MemoryRegion *mr, hwaddr addr,
                                    hwaddr len, bool write)
{
    NV2AState *d = (NV2AState *)opaque;
    UWP_PROF_SCOPE(SURF_CPU_ACCESS);
    qemu_mutex_lock(&d->pgraph.lock);

    PGRAPHGLState *r = d->pgraph.gl_renderer_state;
    bool wait_for_downloads = false;

    SurfaceBinding *surface;
    QTAILQ_FOREACH(surface, &r->surfaces, entry) {
        if (!check_surface_overlaps_range(surface, addr, len)) {
            continue;
        }

        hwaddr offset = addr - surface->vram_addr;

        if (write) {
            trace_nv2a_pgraph_surface_cpu_write(surface->vram_addr, offset);
        } else {
            trace_nv2a_pgraph_surface_cpu_read(surface->vram_addr, offset);
        }

        if (surface->draw_dirty) {
            surface->download_pending = true;
            wait_for_downloads = true;
        }

        if (write) {
            surface->upload_pending = true;
        }
    }
#ifdef XEMU_UWP
    /* bytes still on their way there: a read would see old ones, a write would be overwritten */
    if (uwp_wb_overlaps(addr, len)) {
        wait_for_downloads = true;
    }
#endif

    qemu_mutex_unlock(&d->pgraph.lock);

    if (wait_for_downloads) {
        qemu_mutex_lock(&d->pfifo.lock);
        qemu_event_reset(&r->downloads_complete);
        qatomic_set(&r->downloads_pending, true);
        pfifo_kick(d);
        qemu_mutex_unlock(&d->pfifo.lock);
        qemu_event_wait(&r->downloads_complete);
    }
}

static void register_cpu_access_callback(NV2AState *d, SurfaceBinding *surface)
{
    if (tcg_enabled()) {
        if (surface->width && surface->height) {
            surface->access_cb = mem_access_callback_insert(
                qemu_get_cpu(0), d->vram, surface->vram_addr, surface->size,
                &surface_access_callback, d);
        } else {
            surface->access_cb = NULL;
        }
    }
}

static void unregister_cpu_access_callback(NV2AState *d,
                                           SurfaceBinding const *surface)
{
    if (tcg_enabled()) {
        mem_access_callback_remove_by_ref(qemu_get_cpu(0), surface->access_cb);
    }
}

static bool check_surfaces_overlap(const SurfaceBinding *surface,
                                   const SurfaceBinding *other_surface)
{
    return check_surface_overlaps_range(surface, other_surface->vram_addr,
                                        other_surface->size);
}

#ifdef XEMU_UWP
/*
 * A colour surface that is being thrown out because a new colour target takes
 * its memory normally has its pixels copied back to guest RAM first (a
 * read-back, and on GL-on-D3D12 a wait for the GPU). When the memory is
 * plainly being reused for something else, those pixels can no longer mean
 * anything to the guest:
 *  - the new target starts at the same address with another pitch, or
 *  - the two ranges overlap only partly (neither lies inside the other).
 * Halo 2 alternates 128x128 and 160x120 targets over one pool like this, four
 * read-backs a frame. A target that lies wholly inside another (a window in
 * the frame buffer) or keeps its pitch is still copied back.
 */
static bool uwp_evict_without_download(const SurfaceBinding *old,
                                       const SurfaceBinding *incoming)
{
    if (!old->color || !incoming->color || !old->draw_dirty) {
        return false;
    }
    hwaddr o0 = old->vram_addr, o1 = o0 + old->size;
    hwaddr n0 = incoming->vram_addr, n1 = n0 + incoming->size;
    if (o0 == n0) {
        return old->pitch != incoming->pitch;
    }
    bool old_in_new = n0 <= o0 && o1 <= n1;
    bool new_in_old = o0 <= n0 && n1 <= o1;
    return !old_in_new && !new_in_old;
}

/*
 * A game that uses one piece of memory for two depth buffers of different
 * shapes, a frame at a time (Conker: Live & Reloaded: 640x480 and a swizzled
 * 32x32 at the same address, twice a frame), made the big one go to guest
 * memory and back every time: a GPU wait each way, about half of the
 * graphics thread (title screen 10 fps instead of 30). The big depth image is
 * parked on the GPU instead and taken back when that same shape is bound again
 * within two frames. Anything else, or after two frames, writes it to guest
 * memory as before. Parking also starts the big one's bytes on their way to
 * guest memory in the background (uwp_wb_start), since other draws (Conker's
 * shiny cups and candles) read that memory. Test file zeta-park-off.txt: off.
 */
static SurfaceBinding uwp_parked;
static bool uwp_parked_valid;
static unsigned int uwp_parked_scale;

static bool uwp_zeta_park_off(void)
{
    static int off = -1;
    if (off < 0) {
        extern bool xemu_uwp_test_file_exists(const char *name); /* ui/xemu.c */
        /* shiny objects in Conker (cups, candles) read the parked buffer's memory: it is written back in the
         * background when parking (uwp_wb_start); without that write-back there is no park.
         * Test file zeta-park-off.txt: no parking */
        off = xemu_uwp_test_file_exists("zeta-park-off.txt") || uwp_zeta_wb_off() || uwp_zeta_readpixels();
        if (off) {
            uwp_log("depth park: off");
        }
    }
    return off;
}

/* write the parked depth image to guest memory (or drop it) and free it */
static void uwp_unpark(NV2AState *d, bool keep_data)
{
    if (!uwp_parked_valid) {
        return;
    }
    uwp_parked_valid = false;
    if (keep_data && d->pgraph.surface_scale_factor == uwp_parked_scale &&
        (uwp_zeta_wb_off() || uwp_zeta_readpixels())) {
        uwp_parked.download_pending = true;
        surface_download(d, &uwp_parked, true);
    }
    glDeleteTextures(1, &uwp_parked.gl_buffer);
    uwp_parked.gl_buffer = 0;
}

static bool uwp_park_zeta(NV2AState *d, SurfaceBinding *old, const SurfaceBinding *incoming)
{
    if (old->color || incoming->color || !old->draw_dirty || old->vram_addr != incoming->vram_addr ||
        (uint64_t)incoming->width * incoming->height >= (uint64_t)old->width * old->height ||
        uwp_zeta_park_off()) {
        return false;
    }
    uwp_unpark(d, true);
    uwp_wb_start(d, old);      /* the memory under the parked buffer gets its bytes, without a wait */
    uwp_parked = *old;
    uwp_parked_valid = true;
    uwp_parked_scale = d->pgraph.surface_scale_factor;
    old->gl_buffer = 0;      /* the texture lives on in the park; invalidate frees nothing */
    old->draw_dirty = false; /* and nothing is read back */
    static unsigned logged;
    if (!logged++) {
        uwp_log("depth park: %ux%u at %x kept on the GPU while a %ux%u depth buffer uses its memory",
                old->width, old->height, (unsigned)old->vram_addr, incoming->width, incoming->height);
    }
    return true;
}

static bool uwp_unpark_matches(PGRAPHState *pg, const SurfaceBinding *e)
{
    return uwp_parked_valid && !e->color && e->vram_addr == uwp_parked.vram_addr &&
           e->width == uwp_parked.width && e->height == uwp_parked.height &&
           e->pitch == uwp_parked.pitch && e->swizzle == uwp_parked.swizzle &&
           e->fmt.gl_internal_format == uwp_parked.fmt.gl_internal_format &&
           pg->surface_scale_factor == uwp_parked_scale;
}
#endif

static void invalidate_overlapping_surfaces(NV2AState *d, SurfaceBinding *surface)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    SurfaceBinding *other_surface, *next_surface;
    QTAILQ_FOREACH_SAFE(other_surface, &r->surfaces, entry, next_surface) {
        if (check_surfaces_overlap(surface, other_surface)) {
            trace_nv2a_pgraph_surface_evict_overlapping(
                other_surface->vram_addr, other_surface->width, other_surface->height,
                other_surface->pitch);
            UWP_DL_SITE(EVICT_OVERLAP);
#ifdef XEMU_UWP
            if (uwp_evict_without_download(other_surface, surface)) {
                other_surface->draw_dirty = false;
            }
#endif
            pgraph_gl_surface_download_if_dirty(d, other_surface);
            pgraph_gl_surface_invalidate(d, other_surface);
        }
    }
}

static SurfaceBinding *surface_put(NV2AState *d, hwaddr addr,
                                   SurfaceBinding *surface_in)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    assert(pgraph_gl_surface_get(d, addr) == NULL);

    invalidate_overlapping_surfaces(d, surface_in);

    SurfaceBinding *surface_out = g_malloc(sizeof(SurfaceBinding));
    assert(surface_out != NULL);
    *surface_out = *surface_in;

    register_cpu_access_callback(d, surface_out);

    QTAILQ_INSERT_TAIL(&r->surfaces, surface_out, entry);
#ifdef XEMU_UWP
    xemu_uwp_counters[UWP_CNT_SURF_NEW]++;
#endif

    return surface_out;
}

SurfaceBinding *pgraph_gl_surface_get(NV2AState *d, hwaddr addr)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    SurfaceBinding *surface;
    QTAILQ_FOREACH (surface, &r->surfaces, entry) {
        if (surface->vram_addr == addr) {
            return surface;
        }
    }

    return NULL;
}

SurfaceBinding *pgraph_gl_surface_get_within(NV2AState *d, hwaddr addr)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    SurfaceBinding *surface;
    QTAILQ_FOREACH (surface, &r->surfaces, entry) {
        if (addr >= surface->vram_addr &&
            addr < (surface->vram_addr + surface->size)) {
            return surface;
        }
    }

    return NULL;
}

void pgraph_gl_surface_invalidate(NV2AState *d, SurfaceBinding *surface)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    trace_nv2a_pgraph_surface_invalidated(surface->vram_addr);

    if (surface == r->color_binding) {
        assert(d->pgraph.surface_color.buffer_dirty);
        pgraph_gl_unbind_surface(d, true);
    }
    if (surface == r->zeta_binding) {
        assert(d->pgraph.surface_zeta.buffer_dirty);
        pgraph_gl_unbind_surface(d, false);
    }

    unregister_cpu_access_callback(d, surface);

    glDeleteTextures(1, &surface->gl_buffer);
#ifdef XEMU_UWP
    xemu_uwp_counters[UWP_CNT_SURF_DEL]++;
#endif

    QTAILQ_REMOVE(&r->surfaces, surface, entry);
    g_free(surface);
}

#ifdef XEMU_UWP
/*
 * Copy a whole color surface to another VRAM address without leaving the GPU:
 * the destination becomes a surface of its own, so a texture bound there is
 * filled from it directly. Guest memory is brought up to date only if the CPU
 * touches it (the usual surface access callback). Returns false when the
 * caller has to do the copy in guest memory instead.
 */
bool pgraph_gl_surface_copy_to(NV2AState *d, SurfaceBinding *src,
                               hwaddr dest_addr)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    if (check_surface_overlaps_range(src, dest_addr, src->size)) {
        return false;
    }

    SurfaceBinding *dest = NULL, *other;
    QTAILQ_FOREACH(other, &r->surfaces, entry) {
        if (!check_surface_overlaps_range(other, dest_addr, src->size)) {
            continue;
        }
        if (dest != NULL || other->vram_addr != dest_addr ||
            other == r->color_binding || other == r->zeta_binding ||
            !other->color || other->swizzle != src->swizzle ||
            other->width != src->width || other->height != src->height ||
            other->pitch != src->pitch ||
            other->fmt.gl_internal_format != src->fmt.gl_internal_format) {
            return false;
        }
        dest = other;
    }

    if (src->upload_pending) {
        pgraph_gl_upload_surface_data(d, src, false);
    }

    unsigned int width = src->width ? src->width : 1;
    unsigned int height = src->height ? src->height : 1;
    pgraph_apply_scaling_factor(pg, &width, &height);

    if (dest == NULL) {
        GLint last_texture_binding;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &last_texture_binding);

        SurfaceBinding entry = *src;
        entry.vram_addr = dest_addr;
        entry.access_cb = NULL;
        glGenTextures(1, &entry.gl_buffer);
        glBindTexture(GL_TEXTURE_2D, entry.gl_buffer);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, entry.fmt.gl_internal_format, width,
                     height, 0, entry.fmt.gl_format, entry.fmt.gl_type, NULL);
        glBindTexture(GL_TEXTURE_2D, last_texture_binding);
        dest = surface_put(d, dest_addr, &entry);
    }

    glCopyImageSubData(src->gl_buffer, GL_TEXTURE_2D, 0, 0, 0, 0,
                       dest->gl_buffer, GL_TEXTURE_2D, 0, 0, 0, 0,
                       width, height, 1);

    pg->draw_time++;
    dest->draw_time = pg->draw_time;
    dest->frame_time = pg->frame_time;
    dest->draw_dirty = true;
    dest->download_pending = false;
    dest->upload_pending = false;
    dest->cleared = false;
    return true;
}
#endif

static void surface_evict_old(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    const int surface_age_limit = 5;

    SurfaceBinding *s, *next;
    QTAILQ_FOREACH_SAFE(s, &r->surfaces, entry, next) {
        int last_used = d->pgraph.frame_time - s->frame_time;
        if (last_used >= surface_age_limit) {
            trace_nv2a_pgraph_surface_evict_reason("old", s->vram_addr);
            UWP_DL_SITE(EVICT_OLD);
            pgraph_gl_surface_download_if_dirty(d, s);
            pgraph_gl_surface_invalidate(d, s);
        }
    }
}

static bool check_surface_compatibility(SurfaceBinding *s1, SurfaceBinding *s2,
                                        bool strict)
{
    bool format_compatible =
        (s1->color == s2->color) &&
        (s1->fmt.gl_attachment == s2->fmt.gl_attachment) &&
        (s1->fmt.gl_internal_format == s2->fmt.gl_internal_format) &&
        (s1->pitch == s2->pitch);
    if (!format_compatible) {
        return false;
    }

    if (!strict) {
        return (s1->width >= s2->width) && (s1->height >= s2->height);
    } else {
        return (s1->width == s2->width) && (s1->height == s2->height);
    }
}

void pgraph_gl_surface_download_if_dirty(NV2AState *d,
                                           SurfaceBinding *surface)
{
    if (surface->draw_dirty) {
        surface_download(d, surface, true);
    }
}

static void bind_current_surface(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    if (r->color_binding) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, r->color_binding->fmt.gl_attachment,
                               GL_TEXTURE_2D, r->color_binding->gl_buffer, 0);
    }

    if (r->zeta_binding) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, r->zeta_binding->fmt.gl_attachment,
                               GL_TEXTURE_2D, r->zeta_binding->gl_buffer, 0);
    }

    if (r->color_binding || r->zeta_binding) {
        assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) ==
               GL_FRAMEBUFFER_COMPLETE);
    }
}

static void surface_copy_shrink_row(uint8_t *out, uint8_t *in,
                                    unsigned int width,
                                    unsigned int bytes_per_pixel,
                                    unsigned int factor)
{
    if (bytes_per_pixel == 4) {
        for (unsigned int x = 0; x < width; x++) {
            *(uint32_t *)out = *(uint32_t *)in;
            out += 4;
            in += 4 * factor;
        }
    } else if (bytes_per_pixel == 2) {
        for (unsigned int x = 0; x < width; x++) {
            *(uint16_t *)out = *(uint16_t *)in;
            out += 2;
            in += 2 * factor;
        }
    } else {
        for (unsigned int x = 0; x < width; x++) {
            memcpy(out, in, bytes_per_pixel);
            out += bytes_per_pixel;
            in += bytes_per_pixel * factor;
        }
    }
}

#ifdef XEMU_UWP
/* test file zeta-readpixels.txt: depth surfaces read back with glReadPixels, as upstream does */
static bool uwp_zeta_readpixels(void)
{
    static int on = -1;
    if (on < 0) {
        extern bool xemu_uwp_test_file_exists(const char *name); /* ui/xemu.c */
        on = xemu_uwp_test_file_exists("zeta-readpixels.txt");
        if (on) {
            uwp_log("depth read-back test: glReadPixels of the depth (zeta-readpixels.txt)");
        }
    }
    return on;
}

/* see zeta_dl_fs in pgraph_gl_init_surfaces: draws the depth surface as the colour image of its words into
 * zeta_dl_tex and leaves that image bound for reading; uwp_zeta_color_done puts the GL state back */
static void uwp_zeta_draw_as_color(NV2AState *d, SurfaceBinding *surface, GLint *prev_active, GLint *prev_tex)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;
    unsigned int w = pg->surface_scale_factor * surface->width;
    unsigned int h = pg->surface_scale_factor * surface->height;

    glGetIntegerv(GL_ACTIVE_TEXTURE, prev_active);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, prev_tex);

    glBindTexture(GL_TEXTURE_2D, r->s2t_rndr.zeta_dl_tex);
    if (r->s2t_rndr.zeta_dl_w != w || r->s2t_rndr.zeta_dl_h != h) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        r->s2t_rndr.zeta_dl_w = w;
        r->s2t_rndr.zeta_dl_h = h;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, r->s2t_rndr.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, r->s2t_rndr.zeta_dl_tex, 0);
    GLenum draw_buffers[1] = { GL_COLOR_ATTACHMENT0 };
    glDrawBuffers(1, draw_buffers);
    glReadBuffer(GL_COLOR_ATTACHMENT0);

    glBindTexture(GL_TEXTURE_2D, surface->gl_buffer);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
    glUseProgram(r->s2t_rndr.zeta_dl_prog);
    glProgramUniform1i(r->s2t_rndr.zeta_dl_prog, r->s2t_rndr.zeta_dl_tex_loc, 0);
    glBindVertexArray(r->s2t_rndr.vao);
    glBindBuffer(GL_ARRAY_BUFFER, r->s2t_rndr.vbo);
    glViewport(0, 0, w, h);
    glColorMask(true, true, true, true);
    glDisable(GL_DITHER);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

static void uwp_zeta_color_done(NV2AState *d, GLint prev_active, GLint prev_tex)
{
    PGRAPHGLState *r = d->pgraph.gl_renderer_state;
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, r->gl_framebuffer);
    glBindTexture(GL_TEXTURE_2D, prev_tex);
    glActiveTexture(prev_active);
    glUseProgram(r->shader_binding ? r->shader_binding->gl_program : 0);
}

static void uwp_zeta_download_via_color(NV2AState *d, SurfaceBinding *surface, bool flip, uint8_t *out)
{
    PGRAPHState *pg = &d->pgraph;
    unsigned int w = pg->surface_scale_factor * surface->width;
    unsigned int h = pg->surface_scale_factor * surface->height;
    GLint prev_active, prev_tex;
    uwp_zeta_draw_as_color(d, surface, &prev_active, &prev_tex);

    glo_readpixels(GL_RGBA, GL_UNSIGNED_BYTE, 4, pg->surface_scale_factor * surface->pitch, w, h, flip, out);

    static unsigned logged;
    if (logged < 6) {
        uint32_t lo = UINT32_MAX, hi = 0;
        for (unsigned int y = 0; y < h; y += 16) {
            const uint32_t *row = (const uint32_t *)(out + y * pg->surface_scale_factor * surface->pitch);
            for (unsigned int x = 0; x < w; x += 16) {
                lo = MIN(lo, row[x] >> 8);
                hi = MAX(hi, row[x] >> 8);
            }
        }
        logged++;
        uwp_log("depth read-back through a colour image %ux%u at %x: depth %06x..%06x",
                surface->width, surface->height, (unsigned)surface->vram_addr, lo, hi);
    }
    uwp_zeta_color_done(d, prev_active, prev_tex);
}

/*
 * Depth written back to guest memory in the background. A depth buffer that is
 * thrown out (or parked) is drawn into the colour image and copied into a pixel
 * buffer; the GPU does that while emulation goes on, and the bytes reach guest
 * memory when something needs that memory (a texture, an upload, another
 * read-back, all oldest first) or at the next frames' ends. Before, each of
 * these was a read-back with a full GPU wait (Conker: Live & Reloaded, about 60
 * a second in its menus). Test file zeta-wb-off.txt: wait as before.
 */
#define UWP_WB_MAX 32
typedef struct UwpWb {
    GLuint pbo;
    GLsync fence;
    hwaddr addr;
    size_t size, pbo_size;
    unsigned int width, height, pitch, scale;
    bool swizzle;
    uint64_t frame;
} UwpWb;
static UwpWb uwp_wb[UWP_WB_MAX];
static int uwp_wb_head, uwp_wb_count;

static bool uwp_zeta_wb_off(void)
{
    static int off = -1;
    if (off < 0) {
        extern bool xemu_uwp_test_file_exists(const char *name); /* ui/xemu.c */
        off = xemu_uwp_test_file_exists("zeta-wb-off.txt");
        if (off) {
            uwp_log("depth write-back test: waits for the GPU as before (zeta-wb-off.txt)");
        }
    }
    return off;
}

/* the oldest write-back: into guest memory (waits for the GPU when not done and wait is set) */
static bool uwp_wb_finish_oldest(NV2AState *d, bool wait)
{
    if (!uwp_wb_count) {
        return false;
    }
    UwpWb *e = &uwp_wb[uwp_wb_head];
    if (!wait && glClientWaitSync(e->fence, 0, 0) == GL_TIMEOUT_EXPIRED) {
        return false;
    }
    glClientWaitSync(e->fence, GL_SYNC_FLUSH_COMMANDS_BIT, 2000000000ull);
    glDeleteSync(e->fence);
    e->fence = 0;

    glBindBuffer(GL_PIXEL_PACK_BUFFER, e->pbo);
    const uint8_t *src = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)e->width * e->scale *
                                          e->height * e->scale * 4, GL_MAP_READ_BIT);
    if (src) {
        /* the linear image at the surface's pitch, scaled down when drawn larger */
        uint8_t *lin = g_malloc0(e->size);
        unsigned int sw = e->width * e->scale;
        for (unsigned int y = 0; y < e->height; y++) {
            const uint8_t *in = src + (size_t)y * e->scale * sw * 4;
            uint8_t *out = lin + (size_t)y * e->pitch;
            if (e->scale == 1) {
                memcpy(out, in, (size_t)e->width * 4);
            } else {
                for (unsigned int x = 0; x < e->width; x++) {
                    memcpy(out + x * 4, in + (size_t)x * e->scale * 4, 4);
                }
            }
        }
        if (e->swizzle) {
            swizzle_rect(lin, e->width, e->height, d->vram_ptr + e->addr, e->pitch, 4);
        } else {
            for (unsigned int y = 0; y < e->height; y++) {
                memcpy(d->vram_ptr + e->addr + (size_t)y * e->pitch, lin + (size_t)y * e->pitch,
                       (size_t)e->width * 4);
            }
        }
        g_free(lin);
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        memory_region_set_client_dirty(d->vram, e->addr, (hwaddr)e->pitch * e->height, DIRTY_MEMORY_VGA);
        memory_region_set_client_dirty(d->vram, e->addr, (hwaddr)e->pitch * e->height, DIRTY_MEMORY_NV2A_TEX);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    uwp_wb_head = (uwp_wb_head + 1) % UWP_WB_MAX;
    uwp_wb_count--;
    return true;
}

/* a write-back still on its way to guest memory in [addr, addr+len)? (pgraph lock held) */
static bool uwp_wb_overlaps(hwaddr addr, hwaddr len)
{
    for (int i = 0; i < uwp_wb_count; i++) {
        UwpWb *e = &uwp_wb[(uwp_wb_head + i) % UWP_WB_MAX];
        if (addr < e->addr + e->size && e->addr < addr + len) {
            return true;
        }
    }
    return false;
}

/* everything that reads guest memory in [addr, addr+len) calls this first; true when one landed there */
static bool uwp_wb_flush_range(NV2AState *d, hwaddr addr, hwaddr len)
{
    int last = -1;
    for (int i = 0; i < uwp_wb_count; i++) {
        UwpWb *e = &uwp_wb[(uwp_wb_head + i) % UWP_WB_MAX];
        if (addr < e->addr + e->size && e->addr < addr + len) {
            last = i;
        }
    }
    for (int i = 0; i <= last; i++) {
        uwp_wb_finish_oldest(d, true);
    }
    return last >= 0;
}

void pgraph_gl_uwp_wb_flush_range(NV2AState *d, hwaddr addr, hwaddr len)
{
    uwp_wb_flush_range(d, addr, len);
}

/* at a frame's end: the finished ones, and any older than two frames */
void pgraph_gl_uwp_wb_frame(NV2AState *d)
{
    while (uwp_wb_count) {
        UwpWb *e = &uwp_wb[uwp_wb_head];
        bool old = d->pgraph.frame_time - e->frame > 2;
        if (!uwp_wb_finish_oldest(d, old)) {
            break;
        }
    }
}

static void uwp_wb_flush_all(NV2AState *d)
{
    while (uwp_wb_finish_oldest(d, true)) {
    }
}

/* starts the write-back of a Z24S8 surface; false when it is not one (or the test switch is on) */
static bool uwp_wb_start(NV2AState *d, SurfaceBinding *surface)
{
    PGRAPHState *pg = &d->pgraph;
    if (surface->color || surface->fmt.gl_internal_format != GL_DEPTH24_STENCIL8 || uwp_zeta_wb_off() ||
        uwp_zeta_readpixels() || !surface->width || !surface->height) {
        return false;
    }
    /* no wait for older write-backs to the same memory: they land first anyway (oldest first) */
    if (uwp_wb_count == UWP_WB_MAX) {
        uwp_wb_finish_oldest(d, true);
    }
    UwpWb *e = &uwp_wb[(uwp_wb_head + uwp_wb_count) % UWP_WB_MAX];
    unsigned int w = pg->surface_scale_factor * surface->width;
    unsigned int h = pg->surface_scale_factor * surface->height;
    size_t bytes = (size_t)w * h * 4;

    GLint prev_active, prev_tex;
    uwp_zeta_draw_as_color(d, surface, &prev_active, &prev_tex);
    if (!e->pbo) {
        glGenBuffers(1, &e->pbo);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, e->pbo);
    if (e->pbo_size < bytes) {
        glBufferData(GL_PIXEL_PACK_BUFFER, bytes, NULL, GL_STREAM_READ);
        e->pbo_size = bytes;
    }
    GLint rl, pa;
    glGetIntegerv(GL_PACK_ROW_LENGTH, &rl);
    glGetIntegerv(GL_PACK_ALIGNMENT, &pa);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glPixelStorei(GL_PACK_ROW_LENGTH, rl);
    glPixelStorei(GL_PACK_ALIGNMENT, pa);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    e->fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
    uwp_zeta_color_done(d, prev_active, prev_tex);

    e->addr = surface->vram_addr;
    e->size = surface->size;
    e->width = surface->width;
    e->height = surface->height;
    e->pitch = surface->pitch;
    e->scale = pg->surface_scale_factor;
    e->swizzle = surface->swizzle;
    e->frame = pg->frame_time;
    uwp_wb_count++;

    static unsigned logged;
    if (!logged++) {
        uwp_log("depth write-back in the background: %ux%u at %x", surface->width, surface->height,
                (unsigned)surface->vram_addr);
    }
    return true;
}

static bool uwp_color_prefetch_off(void)
{
    static int off = -1;
    if (off < 0) {
        extern bool xemu_uwp_test_file_exists(const char *name); /* ui/xemu.c */
        off = xemu_uwp_test_file_exists("color-prefetch-off.txt");
        if (off) {
            uwp_log("colour read-ahead test: off (color-prefetch-off.txt)");
        }
    }
    return off;
}

/*
 * Small swizzled colour targets that the game reads with the CPU (Conker: Live
 * & Reloaded draws 32x32 ones, about 25 a second in its talking scenes, and
 * reads each back). The read waited for the whole GPU queue, about 7 ms of
 * the emulated CPU's time each. When the target is unbound its bytes start on
 * their way to guest memory in the background; the CPU's read then only waits
 * when they have not landed yet (surface_access_callback).
 * Test file color-prefetch-off.txt: read when the CPU asks, as before.
 */
static void uwp_prefetch_color(NV2AState *d, SurfaceBinding *surface)
{
    PGRAPHState *pg = &d->pgraph;
    if (!surface || !surface->color || !surface->draw_dirty || !surface->swizzle ||
        surface->size > 16384 || surface->fmt.bytes_per_pixel != 4 || pg->surface_scale_factor != 1 ||
        !surface->width || !surface->height || uwp_color_prefetch_off()) {
        return;
    }
    if (uwp_wb_count == UWP_WB_MAX) {
        uwp_wb_finish_oldest(d, true);
    }
    UwpWb *e = &uwp_wb[(uwp_wb_head + uwp_wb_count) % UWP_WB_MAX];
    size_t bytes = (size_t)surface->width * surface->height * 4;

    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, surface->fmt.gl_attachment, GL_TEXTURE_2D, surface->gl_buffer, 0);
    if (!e->pbo) {
        glGenBuffers(1, &e->pbo);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, e->pbo);
    if (e->pbo_size < bytes) {
        glBufferData(GL_PIXEL_PACK_BUFFER, bytes, NULL, GL_STREAM_READ);
        e->pbo_size = bytes;
    }
    GLint rl, pa;
    glGetIntegerv(GL_PACK_ROW_LENGTH, &rl);
    glGetIntegerv(GL_PACK_ALIGNMENT, &pa);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, surface->width, surface->height, surface->fmt.gl_format, surface->fmt.gl_type, NULL);
    glPixelStorei(GL_PACK_ROW_LENGTH, rl);
    glPixelStorei(GL_PACK_ALIGNMENT, pa);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    e->fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
    glFramebufferTexture2D(GL_FRAMEBUFFER, surface->fmt.gl_attachment, GL_TEXTURE_2D, 0, 0);
    bind_current_surface(d);

    e->addr = surface->vram_addr;
    e->size = surface->size;
    e->width = surface->width;
    e->height = surface->height;
    e->pitch = surface->pitch;
    e->scale = 1;
    e->swizzle = surface->swizzle;
    e->frame = pg->frame_time;
    uwp_wb_count++;
    surface->draw_dirty = false;

    static unsigned logged;
    if (!logged++) {
        uwp_log("colour read-ahead: %ux%u at %x", surface->width, surface->height, (unsigned)surface->vram_addr);
    }
}
#endif

static void surface_download_to_buffer(NV2AState *d, SurfaceBinding *surface,
                                       bool swizzle, bool flip, bool downscale,
                                       uint8_t *pixels)
{
    PGRAPHState *pg = &d->pgraph;

    swizzle &= surface->swizzle;
    downscale &= (pg->surface_scale_factor != 1);

    if (!surface->width || !surface->height) {
        return;
    }

    trace_nv2a_pgraph_surface_download(
        surface->color ? "COLOR" : "ZETA",
        surface->swizzle ? "sz" : "lin", surface->vram_addr,
        surface->width, surface->height, surface->pitch,
        surface->fmt.bytes_per_pixel);

    /*  Bind destination surface to framebuffer */
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           0, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D,
                           0, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                           GL_TEXTURE_2D, 0, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, surface->fmt.gl_attachment,
                           GL_TEXTURE_2D, surface->gl_buffer, 0);

    assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);

    /* Read surface into memory */
    uint8_t *gl_read_buf = pixels;

    uint8_t *swizzle_buf = pixels;
    if (swizzle) {
        /* FIXME: Allocate big buffer up front and re-alloc if necessary.
         * FIXME: Consider swizzle in shader
         */
        assert(pg->surface_scale_factor == 1 || downscale);
        swizzle_buf = (uint8_t *)g_malloc(surface->size);
        gl_read_buf = swizzle_buf;
    }

    if (downscale) {
        pg->scale_buf = (uint8_t *)g_realloc(
            pg->scale_buf, pg->surface_scale_factor * pg->surface_scale_factor *
                               surface->size);
        gl_read_buf = pg->scale_buf;
    }

#ifdef XEMU_UWP
    if (!surface->color && surface->fmt.gl_internal_format == GL_DEPTH24_STENCIL8 &&
        !uwp_zeta_readpixels()) {
        uwp_zeta_download_via_color(d, surface, flip, gl_read_buf);
    } else
#endif
    glo_readpixels(
        surface->fmt.gl_format, surface->fmt.gl_type, surface->fmt.bytes_per_pixel,
        pg->surface_scale_factor * surface->pitch,
        pg->surface_scale_factor * surface->width,
        pg->surface_scale_factor * surface->height, flip, gl_read_buf);


    /* FIXME: Replace this with a hw accelerated version */
    if (downscale) {
        assert(surface->pitch >= (surface->width * surface->fmt.bytes_per_pixel));
        uint8_t *out = swizzle_buf, *in = pg->scale_buf;
        for (unsigned int y = 0; y < surface->height; y++) {
            surface_copy_shrink_row(out, in, surface->width,
                                    surface->fmt.bytes_per_pixel,
                                    pg->surface_scale_factor);
            in += surface->pitch * pg->surface_scale_factor *
                  pg->surface_scale_factor;
            out += surface->pitch;
        }
    }

    if (swizzle) {
        swizzle_rect(swizzle_buf, surface->width, surface->height, pixels,
                     surface->pitch, surface->fmt.bytes_per_pixel);
        g_free(swizzle_buf);
    }

    /* Re-bind original framebuffer target */
    glFramebufferTexture2D(GL_FRAMEBUFFER, surface->fmt.gl_attachment,
                           GL_TEXTURE_2D, 0, 0);
    bind_current_surface(d);
}

static void surface_download(NV2AState *d, SurfaceBinding *surface, bool force)
{
    if (!(surface->download_pending || force) || !surface->width ||
        !surface->height) {
        return;
    }
#ifdef XEMU_UWP
    uwp_wb_flush_range(d, surface->vram_addr, surface->size);
    if (!force && !surface->draw_dirty) {
        surface->download_pending = false;   /* its bytes came with a write-back */
        return;
    }
#endif

    /* FIXME: Respect write enable at last TOU? */

    nv2a_profile_inc_counter(NV2A_PROF_SURF_DOWNLOAD);
#ifdef XEMU_UWP
    {
        uwp_dl_counts[uwp_dl_site]++;
        UWP_TRACE("  readback site %d: %s %ux%u pitch %u swz %d addr %x "
                  "cfmt %d zfmt %d",
                  uwp_dl_site, surface->color ? "color" : "zeta",
                  surface->width, surface->height, surface->pitch,
                  surface->swizzle, (unsigned)surface->vram_addr,
                  surface->shape.color_format, surface->shape.zeta_format);
    }
#endif

    UWP_PROF_BEGIN(t0);
    surface_download_to_buffer(d, surface, true, false, true,
                               d->vram_ptr + surface->vram_addr);
    UWP_PROF_END(PF_SURF_DOWN, t0);

    memory_region_set_client_dirty(d->vram, surface->vram_addr,
                                   surface->pitch * surface->height,
                                   DIRTY_MEMORY_VGA);
    memory_region_set_client_dirty(d->vram, surface->vram_addr,
                                   surface->pitch * surface->height,
                                   DIRTY_MEMORY_NV2A_TEX);

    surface->download_pending = false;
    surface->draw_dirty = false;
}

void pgraph_gl_process_pending_downloads(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

#ifdef XEMU_UWP
    uwp_wb_flush_all(d);
#endif
    SurfaceBinding *surface;
    QTAILQ_FOREACH(surface, &r->surfaces, entry) {
        UWP_DL_SITE(CPU_ACCESS);
        surface_download(d, surface, false);
    }

    qatomic_set(&r->downloads_pending, false);
    qemu_event_set(&r->downloads_complete);
}

void pgraph_gl_download_dirty_surfaces(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    SurfaceBinding *surface;
    QTAILQ_FOREACH(surface, &r->surfaces, entry) {
        UWP_DL_SITE(ALL_DIRTY);
        pgraph_gl_surface_download_if_dirty(d, surface);
    }

    qatomic_set(&r->download_dirty_surfaces_pending, false);
    qemu_event_set(&r->dirty_surfaces_download_complete);
}

static void surface_copy_expand_row(uint8_t *out, uint8_t *in,
                                    unsigned int width,
                                    unsigned int bytes_per_pixel,
                                    unsigned int factor)
{
    if (bytes_per_pixel == 4) {
        for (unsigned int x = 0; x < width; x++) {
            for (unsigned int i = 0; i < factor; i++) {
                *(uint32_t *)out = *(uint32_t *)in;
                out += bytes_per_pixel;
            }
            in += bytes_per_pixel;
        }
    } else if (bytes_per_pixel == 2) {
        for (unsigned int x = 0; x < width; x++) {
            for (unsigned int i = 0; i < factor; i++) {
                *(uint16_t *)out = *(uint16_t *)in;
                out += bytes_per_pixel;
            }
            in += bytes_per_pixel;
        }
    } else {
        for (unsigned int x = 0; x < width; x++) {
            for (unsigned int i = 0; i < factor; i++) {
                memcpy(out, in, bytes_per_pixel);
                out += bytes_per_pixel;
            }
            in += bytes_per_pixel;
        }
    }
}

static void surface_copy_expand(uint8_t *out, uint8_t *in, unsigned int width,
                                unsigned int height,
                                unsigned int bytes_per_pixel,
                                unsigned int factor)
{
    size_t out_pitch = width * bytes_per_pixel * factor;

    for (unsigned int y = 0; y < height; y++) {
        surface_copy_expand_row(out, in, width, bytes_per_pixel, factor);
        uint8_t *row_in = out;
        for (unsigned int i = 1; i < factor; i++) {
            out += out_pitch;
            memcpy(out, row_in, out_pitch);
        }
        in += width * bytes_per_pixel;
        out += out_pitch;
    }
}

void pgraph_gl_upload_surface_data(NV2AState *d, SurfaceBinding *surface,
                                bool force)
{
#ifdef XEMU_UWP
    if (surface->upload_pending || force) {
        uwp_wb_flush_range(d, surface->vram_addr, surface->size);
    }
#endif
    if (!(surface->upload_pending || force)) {
        return;
    }

    nv2a_profile_inc_counter(NV2A_PROF_SURF_UPLOAD);

    trace_nv2a_pgraph_surface_upload(
                 surface->color ? "COLOR" : "ZETA",
                 surface->swizzle ? "sz" : "lin", surface->vram_addr,
                 surface->width, surface->height, surface->pitch,
                 surface->fmt.bytes_per_pixel);

    PGRAPHState *pg = &d->pgraph;

    surface->upload_pending = false;
    surface->draw_time = pg->draw_time;

    if (!surface->width || !surface->height) {
        return;
    }

    // FIXME: Don't query GL for texture binding
    GLint last_texture_binding;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &last_texture_binding);

    // FIXME: Replace with FBO to not disturb current state
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           0, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D,
                           0, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                           GL_TEXTURE_2D, 0, 0);

    uint8_t *data = d->vram_ptr;
    uint8_t *buf = data + surface->vram_addr;

    if (surface->swizzle) {
        buf = (uint8_t*)g_malloc(surface->size);
        unswizzle_rect(data + surface->vram_addr,
                       surface->width, surface->height,
                       buf,
                       surface->pitch,
                       surface->fmt.bytes_per_pixel);
    }

    /* FIXME: Replace this scaling */

    // This is VRAM so we can't do this inplace!
    uint8_t *optimal_buf = buf;
    unsigned int optimal_pitch = surface->width * surface->fmt.bytes_per_pixel;

    if (surface->pitch != optimal_pitch) {
        optimal_buf = (uint8_t *)g_malloc(surface->height * optimal_pitch);

        uint8_t *src = buf;
        uint8_t *dst = optimal_buf;
        unsigned int irow;
        for (irow = 0; irow < surface->height; irow++) {
            memcpy(dst, src, optimal_pitch);
            src += surface->pitch;
            dst += optimal_pitch;
        }
    }

    uint8_t *gl_read_buf = optimal_buf;
    unsigned int width = surface->width, height = surface->height;

    if (pg->surface_scale_factor > 1) {
        pgraph_apply_scaling_factor(pg, &width, &height);
        pg->scale_buf = (uint8_t *)g_realloc(
            pg->scale_buf, width * height * surface->fmt.bytes_per_pixel);
        gl_read_buf = pg->scale_buf;
        uint8_t *out = gl_read_buf, *in = optimal_buf;
        surface_copy_expand(out, in, surface->width, surface->height,
                            surface->fmt.bytes_per_pixel,
                            d->pgraph.surface_scale_factor);
    }

    int prev_unpack_alignment;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &prev_unpack_alignment);
    if (unlikely((width * surface->fmt.bytes_per_pixel) % 4 != 0)) {
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    } else {
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    }

    UWP_PROF_BEGIN(t0);
    glBindTexture(GL_TEXTURE_2D, surface->gl_buffer);
    glTexImage2D(GL_TEXTURE_2D, 0, surface->fmt.gl_internal_format, width,
                 height, 0, surface->fmt.gl_format, surface->fmt.gl_type,
                 gl_read_buf);
    UWP_PROF_END(PF_SURF_UP, t0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, prev_unpack_alignment);
    if (optimal_buf != buf) {
        g_free(optimal_buf);
    }
    if (surface->swizzle) {
        g_free(buf);
    }

    // Rebind previous framebuffer binding
    glBindTexture(GL_TEXTURE_2D, last_texture_binding);

    bind_current_surface(d);
}

static void compare_surfaces(SurfaceBinding *s1, SurfaceBinding *s2)
{
    #define DO_CMP(fld) \
        if (s1->fld != s2->fld) \
            trace_nv2a_pgraph_surface_compare_mismatch( \
                #fld, (long int)s1->fld, (long int)s2->fld);
    DO_CMP(shape.clip_x)
    DO_CMP(shape.clip_width)
    DO_CMP(shape.clip_y)
    DO_CMP(shape.clip_height)
    DO_CMP(gl_buffer)
    DO_CMP(fmt.bytes_per_pixel)
    DO_CMP(fmt.gl_attachment)
    DO_CMP(fmt.gl_internal_format)
    DO_CMP(fmt.gl_format)
    DO_CMP(fmt.gl_type)
    DO_CMP(color)
    DO_CMP(swizzle)
    DO_CMP(vram_addr)
    DO_CMP(width)
    DO_CMP(height)
    DO_CMP(pitch)
    DO_CMP(size)
    DO_CMP(dma_addr)
    DO_CMP(dma_len)
    DO_CMP(frame_time)
    DO_CMP(draw_time)
    #undef DO_CMP
}

static void populate_surface_binding_entry_sized(NV2AState *d, bool color,
                                                 unsigned int width,
                                                 unsigned int height,
                                                 SurfaceBinding *entry)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    Surface *surface;
    hwaddr dma_address;
    SurfaceFormatInfo fmt;

    if (color) {
        surface = &pg->surface_color;
        dma_address = pg->dma_color;
        assert(pg->surface_shape.color_format != 0);
        assert(pg->surface_shape.color_format <
               ARRAY_SIZE(kelvin_surface_color_format_gl_map));
        fmt = kelvin_surface_color_format_gl_map[pg->surface_shape.color_format];
        if (fmt.bytes_per_pixel == 0) {
            fprintf(stderr, "nv2a: unimplemented color surface format 0x%x\n",
                    pg->surface_shape.color_format);
            abort();
        }
    } else {
        surface = &pg->surface_zeta;
        dma_address = pg->dma_zeta;
        assert(pg->surface_shape.zeta_format != 0);
        assert(pg->surface_shape.zeta_format <
               ARRAY_SIZE(kelvin_surface_zeta_float_format_gl_map));
        const SurfaceFormatInfo *map =
            pg->surface_shape.z_format ? kelvin_surface_zeta_float_format_gl_map :
                                         kelvin_surface_zeta_fixed_format_gl_map;
        fmt = map[pg->surface_shape.zeta_format];
    }

    DMAObject dma = nv_dma_load(d, dma_address);
    /* There's a bunch of bugs that could cause us to hit this function
     * at the wrong time and get a invalid dma object.
     * Check that it's sane. */
    assert(dma.dma_class == NV_DMA_IN_MEMORY_CLASS);
    // assert(dma.address + surface->offset != 0);
    assert(surface->offset <= dma.limit);
    assert(surface->offset + surface->pitch * height <= dma.limit + 1);
    assert(surface->pitch % fmt.bytes_per_pixel == 0);
    assert((dma.address & ~0x07FFFFFF) == 0);

    entry->shape = (color || !r->color_binding) ? pg->surface_shape :
                                                   r->color_binding->shape;
    entry->gl_buffer = 0;
    entry->fmt = fmt;
    entry->color = color;
    entry->swizzle =
        (pg->surface_type == NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE);
    entry->vram_addr = dma.address + surface->offset;
    entry->width = width;
    entry->height = height;
    entry->pitch = surface->pitch;
    entry->size = height * MAX(surface->pitch, width * fmt.bytes_per_pixel);
    entry->upload_pending = true;
    entry->download_pending = false;
    entry->draw_dirty = false;
    entry->dma_addr = dma.address;
    entry->dma_len = dma.limit;
    entry->frame_time = pg->frame_time;
    entry->draw_time = pg->draw_time;
    entry->cleared = false;
}

static void populate_surface_binding_entry(NV2AState *d, bool color,
                                                  SurfaceBinding *entry)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    unsigned int width, height;

    if (color || !r->color_binding) {
        surface_get_dimensions(pg, &width, &height);
        pgraph_apply_anti_aliasing_factor(pg, &width, &height);

        /* Since we determine surface dimensions based on the clipping
         * rectangle, make sure to include the surface offset as well.
         */
        if (pg->surface_type != NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE) {
            width += pg->surface_shape.clip_x;
            height += pg->surface_shape.clip_y;
        }
    } else {
        width = r->color_binding->width;
        height = r->color_binding->height;
    }

    populate_surface_binding_entry_sized(d, color, width, height, entry);
}

static void update_surface_part(NV2AState *d, bool upload, bool color)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    SurfaceBinding entry;
    populate_surface_binding_entry(d, color, &entry);

    Surface *surface = color ? &pg->surface_color : &pg->surface_zeta;

    bool mem_dirty = !tcg_enabled() && memory_region_test_and_clear_dirty(
                                           d->vram, entry.vram_addr, entry.size,
                                           DIRTY_MEMORY_NV2A);

    if (upload && (surface->buffer_dirty || mem_dirty)) {
        pgraph_gl_unbind_surface(d, color);

        SurfaceBinding *found = pgraph_gl_surface_get(d, entry.vram_addr);
        if (found != NULL) {
            /* FIXME: Support same color/zeta surface target? In the mean time,
             * if the surface we just found is currently bound, just unbind it.
             */
            SurfaceBinding *other = (color ? r->zeta_binding
                                           : r->color_binding);
#ifdef XEMU_UWP
            if (!color && found->color && r->color_binding != NULL &&
                (found == other || pg->frame_time - found->frame_time <= 1)) {
                /*
                 * The depth buffer set to memory that is a color image: the
                 * color target itself, or one drawn this frame or the last
                 * (seen: two 160x120 blur targets, each named as the depth
                 * buffer while the other is drawn). Such passes cannot be
                 * using depth. Making a depth surface there would throw the
                 * color surface away, with two read-backs, every frame:
                 * draw without a depth buffer instead.
                 * Only while a color target is bound, though: a pass that
                 * writes depth alone (Halo 2 does one while loading a
                 * level) would be left with nothing to draw into, which
                 * ends in the assert in pgraph_gl_draw_begin.
                 */
                surface->buffer_dirty = false;
                return;
            }
#endif
            if (found == other) {
                NV2A_UNIMPLEMENTED("Same color & zeta surface offset");
                pgraph_gl_unbind_surface(d, !color);
            }
        }

        trace_nv2a_pgraph_surface_target(
            color ? "COLOR" : "ZETA", entry.vram_addr,
            entry.swizzle ? "sz" : "ln",
            pg->surface_shape.anti_aliasing,
            pg->surface_shape.clip_x,
            pg->surface_shape.clip_width, pg->surface_shape.clip_y,
            pg->surface_shape.clip_height);

        bool should_create = true;

        if (found != NULL) {
            bool is_compatible =
                check_surface_compatibility(found, &entry, false);

#define TRACE_ARGS found->vram_addr, found->width, found->height, \
            found->swizzle ? "sz" : "ln", \
            found->shape.anti_aliasing, found->shape.clip_x, \
            found->shape.clip_width, found->shape.clip_y, \
            found->shape.clip_height, found->pitch
            if (found->color) {
                trace_nv2a_pgraph_surface_match_color(TRACE_ARGS);
            } else {
                trace_nv2a_pgraph_surface_match_zeta(TRACE_ARGS);
            }
#undef TRACE_ARGS

            assert(!(entry.swizzle && pg->clearing));

            if (found->swizzle != entry.swizzle) {
                /* Clears should only be done on linear surfaces. Avoid
                 * synchronization by allowing (1) a surface marked swizzled to
                 * be cleared under the assumption the entire surface is
                 * destined to be cleared and (2) a fully cleared linear surface
                 * to be marked swizzled. Strictly match size to avoid
                 * pathological cases.
                 */
                is_compatible &= (pg->clearing || found->cleared) &&
                    check_surface_compatibility(found, &entry, true);
                if (is_compatible) {
                    trace_nv2a_pgraph_surface_migrate_type(
                        entry.swizzle ? "swizzled" : "linear");
                }
            }

            if (is_compatible && color &&
                !check_surface_compatibility(found, &entry, true)) {
                SurfaceBinding zeta_entry;
                populate_surface_binding_entry_sized(
                    d, !color, found->width, found->height, &zeta_entry);
                hwaddr color_end = found->vram_addr + found->size;
                hwaddr zeta_end = zeta_entry.vram_addr + zeta_entry.size;
                is_compatible &= found->vram_addr >= zeta_end ||
                                 zeta_entry.vram_addr >= color_end;
            }

#ifdef XEMU_UWP
            /*
             * Games render to a smaller color target with the main depth
             * buffer still set (seen: 512x384 color, 640x480 depth, twice a
             * frame). The pitch matched above, so the larger depth image is
             * that same memory with the color target's pixels in its top
             * left corner: keep it bound, as the console does, instead of
             * reading it back and building a smaller one. The drawing area
             * stays the color target's.
             */
            bool zeta_larger = false;
            /* test file zeta-equal.txt: upstream's rule, the depth buffer only kept at the colour target's size */
            static int uwp_zeta_equal = -1;
            if (uwp_zeta_equal < 0) {
                extern bool xemu_uwp_test_file_exists(const char *name); /* ui/xemu.c */
                uwp_zeta_equal = xemu_uwp_test_file_exists("zeta-equal.txt");
                if (uwp_zeta_equal) {
                    uwp_log("depth buffer test: kept only at the colour target's size (zeta-equal.txt)");
                }
            }
            if (is_compatible && !color && r->color_binding && uwp_zeta_equal) {
                is_compatible &= (found->width == r->color_binding->width) &&
                                 (found->height == r->color_binding->height);
            } else if (is_compatible && !color && r->color_binding) {
                is_compatible &= (found->width >= r->color_binding->width) &&
                                 (found->height >= r->color_binding->height);
                zeta_larger = is_compatible &&
                              (found->width != r->color_binding->width ||
                               found->height != r->color_binding->height);
            }
#else
            if (is_compatible && !color && r->color_binding) {
                is_compatible &= (found->width == r->color_binding->width) &&
                                 (found->height == r->color_binding->height);
            }
#endif

            if (is_compatible) {
#ifdef XEMU_UWP
                if (!zeta_larger)
#endif
                {
                /* FIXME: Refactor */
                pg->surface_binding_dim.width = found->width;
                pg->surface_binding_dim.clip_x = found->shape.clip_x;
                pg->surface_binding_dim.clip_width = found->shape.clip_width;
                pg->surface_binding_dim.height = found->height;
                pg->surface_binding_dim.clip_y = found->shape.clip_y;
                pg->surface_binding_dim.clip_height = found->shape.clip_height;
                }
                found->upload_pending |= mem_dirty;
                pg->surface_zeta.buffer_dirty |= color;
                should_create = false;
            } else {
                trace_nv2a_pgraph_surface_evict_reason(
                    "incompatible", found->vram_addr);
                compare_surfaces(found, &entry);
#ifdef XEMU_UWP
                if (found->draw_dirty) {
                    UWP_TRACE("  evict %s addr %x: had %ux%u pitch %u swz %d "
                              "ifmt %x | wanted %ux%u pitch %u swz %d ifmt %x "
                              "| color binding %ux%u, clearing %d",
                              color ? "color" : "zeta",
                              (unsigned)found->vram_addr, found->width,
                              found->height, found->pitch, found->swizzle,
                              found->fmt.gl_internal_format, entry.width,
                              entry.height, entry.pitch, entry.swizzle,
                              entry.fmt.gl_internal_format,
                              r->color_binding ? r->color_binding->width : 0,
                              r->color_binding ? r->color_binding->height : 0,
                              pg->clearing);
                }
#endif
                UWP_DL_SITE(EVICT_INCOMPATIBLE);
#ifdef XEMU_UWP
                if (uwp_evict_without_download(found, &entry)) {
                    found->draw_dirty = false;
                } else if (!uwp_park_zeta(d, found, &entry) && uwp_wb_start(d, found)) {
                    found->draw_dirty = false;   /* its bytes reach guest memory in the background */
                }
#endif
                pgraph_gl_surface_download_if_dirty(d, found);
                pgraph_gl_surface_invalidate(d, found);
            }
        }

#ifdef XEMU_UWP
        bool uwp_from_park = should_create && uwp_unpark_matches(pg, &entry);
        if (uwp_from_park) {
            entry.gl_buffer = uwp_parked.gl_buffer;
            uwp_parked.gl_buffer = 0;
            uwp_parked_valid = false;
            found = surface_put(d, entry.vram_addr, &entry);
            found->upload_pending = false;
            found->draw_dirty = true;
            found->cleared = false;
            pg->surface_binding_dim.width = entry.width;
            pg->surface_binding_dim.clip_x = entry.shape.clip_x;
            pg->surface_binding_dim.clip_width = entry.shape.clip_width;
            pg->surface_binding_dim.height = entry.height;
            pg->surface_binding_dim.clip_y = entry.shape.clip_y;
            pg->surface_binding_dim.clip_height = entry.shape.clip_height;
        } else
#endif
        if (should_create) {
            glGenTextures(1, &entry.gl_buffer);
            glBindTexture(GL_TEXTURE_2D, entry.gl_buffer);
            NV2A_GL_DLABEL(GL_TEXTURE, entry.gl_buffer,
                           "%s format: %0X, width: %d, height: %d "
                           "(addr %" HWADDR_PRIx ")",
                           color ? "color" : "zeta",
                           color ? pg->surface_shape.color_format
                                 : pg->surface_shape.zeta_format,
                           entry.width, entry.height, surface->offset);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            unsigned int width = entry.width ? entry.width : 1;
            unsigned int height = entry.height ? entry.height : 1;
            pgraph_apply_scaling_factor(pg, &width, &height);
            glTexImage2D(GL_TEXTURE_2D, 0, entry.fmt.gl_internal_format, width,
                         height, 0, entry.fmt.gl_format, entry.fmt.gl_type,
                         NULL);
            found = surface_put(d, entry.vram_addr, &entry);

            /* FIXME: Refactor */
            pg->surface_binding_dim.width = entry.width;
            pg->surface_binding_dim.clip_x = entry.shape.clip_x;
            pg->surface_binding_dim.clip_width = entry.shape.clip_width;
            pg->surface_binding_dim.height = entry.height;
            pg->surface_binding_dim.clip_y = entry.shape.clip_y;
            pg->surface_binding_dim.clip_height = entry.shape.clip_height;

            if (color && r->zeta_binding && (r->zeta_binding->width != entry.width || r->zeta_binding->height != entry.height)) {
                pg->surface_zeta.buffer_dirty = true;
            }
        }

#define TRACE_ARGS found->vram_addr, found->width, found->height, \
                   found->swizzle ? "sz" : "ln", found->shape.anti_aliasing, \
                   found->shape.clip_x, found->shape.clip_width, \
                   found->shape.clip_y, found->shape.clip_height, found->pitch

        if (color) {
            if (should_create) {
                trace_nv2a_pgraph_surface_create_color(TRACE_ARGS);
            } else {
                trace_nv2a_pgraph_surface_hit_color(TRACE_ARGS);
            }

            r->color_binding = found;
        } else {
            if (should_create) {
                trace_nv2a_pgraph_surface_create_zeta(TRACE_ARGS);
            } else {
                trace_nv2a_pgraph_surface_hit_zeta(TRACE_ARGS);
            }
            r->zeta_binding = found;
        }
#undef TRACE_ARGS

        glFramebufferTexture2D(GL_FRAMEBUFFER, entry.fmt.gl_attachment,
                               GL_TEXTURE_2D, found->gl_buffer, 0);
        assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) ==
               GL_FRAMEBUFFER_COMPLETE);

        surface->buffer_dirty = false;
    }

    if (!upload && surface->draw_dirty) {
        if (!tcg_enabled()) {
            /* FIXME: Cannot monitor for reads/writes; flush now */
            surface_download(d,
                             color ? r->color_binding :
                                     r->zeta_binding,
                             true);
        }

        surface->write_enabled_cache = false;
        surface->draw_dirty = false;
    }
}

void pgraph_gl_unbind_surface(NV2AState *d, bool color)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    if (color) {
        if (r->color_binding) {
#ifdef XEMU_UWP
            uwp_prefetch_color(d, r->color_binding);
#endif
            glFramebufferTexture2D(GL_FRAMEBUFFER,
                                   GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_2D, 0, 0);
            r->color_binding = NULL;
        }
    } else {
        if (r->zeta_binding) {
            glFramebufferTexture2D(GL_FRAMEBUFFER,
                                   GL_DEPTH_ATTACHMENT,
                                   GL_TEXTURE_2D, 0, 0);
            glFramebufferTexture2D(GL_FRAMEBUFFER,
                                   GL_DEPTH_STENCIL_ATTACHMENT,
                                   GL_TEXTURE_2D, 0, 0);
            r->zeta_binding = NULL;
        }
    }
}

void pgraph_gl_surface_update(NV2AState *d, bool upload, bool color_write,
                           bool zeta_write)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    pg->surface_shape.z_format =
        GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER),
                 NV_PGRAPH_SETUPRASTER_Z_FORMAT);

#ifdef XEMU_UWP
    if (uwp_parked_valid && pg->frame_time - uwp_parked.frame_time > 2) {
        uwp_unpark(d, true);
    }
#endif

    color_write = color_write &&
            (pg->clearing || pgraph_color_write_enabled(pg));
    zeta_write = zeta_write && (pg->clearing || pgraph_zeta_write_enabled(pg));

    if (upload) {
        bool fb_dirty = framebuffer_dirty(pg);
        if (fb_dirty) {
            memcpy(&pg->last_surface_shape, &pg->surface_shape,
                   sizeof(SurfaceShape));
            pg->surface_color.buffer_dirty = true;
            pg->surface_zeta.buffer_dirty = true;
        }

        if (pg->surface_color.buffer_dirty) {
            pgraph_gl_unbind_surface(d, true);
        }

        if (color_write) {
            update_surface_part(d, true, true);
        }

        if (pg->surface_zeta.buffer_dirty) {
            pgraph_gl_unbind_surface(d, false);
        }

        if (zeta_write) {
            update_surface_part(d, true, false);
        }
    } else {
        if ((color_write || pg->surface_color.write_enabled_cache)
            && pg->surface_color.draw_dirty) {
            update_surface_part(d, false, true);
        }
        if ((zeta_write || pg->surface_zeta.write_enabled_cache)
            && pg->surface_zeta.draw_dirty) {
            update_surface_part(d, false, false);
        }
    }

    if (upload) {
        pg->draw_time++;
    }

    bool swizzle = (pg->surface_type == NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE);

    if (r->color_binding) {
        r->color_binding->frame_time = pg->frame_time;
        if (upload) {
            pgraph_gl_upload_surface_data(d, r->color_binding, false);
            r->color_binding->draw_time = pg->draw_time;
            r->color_binding->swizzle = swizzle;
        }
    }

    if (r->zeta_binding) {
        r->zeta_binding->frame_time = pg->frame_time;
        if (upload) {
            pgraph_gl_upload_surface_data(d, r->zeta_binding, false);
            r->zeta_binding->draw_time = pg->draw_time;
            r->zeta_binding->swizzle = swizzle;
        }
    }

    // Sanity check color and zeta dimensions match
    if (r->color_binding && r->zeta_binding) {
#ifdef XEMU_UWP
        /* the depth buffer may be the larger one, see update_surface_part */
        assert((r->color_binding->width <= r->zeta_binding->width)
               && (r->color_binding->height <= r->zeta_binding->height));
#else
        assert((r->color_binding->width == r->zeta_binding->width)
               && (r->color_binding->height == r->zeta_binding->height));
#endif
    }

    surface_evict_old(d);
}

// FIXME: Move to common
static void surface_get_dimensions(PGRAPHState *pg, unsigned int *width,
                                   unsigned int *height)
{
    bool swizzle = (pg->surface_type == NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE);
    if (swizzle) {
        *width = 1 << pg->surface_shape.log_width;
        *height = 1 << pg->surface_shape.log_height;
    } else {
        *width = pg->surface_shape.clip_width;
        *height = pg->surface_shape.clip_height;
    }
}

void pgraph_gl_init_surfaces(PGRAPHState *pg)
{
    PGRAPHGLState *r = pg->gl_renderer_state;

    pgraph_gl_reload_surface_scale_factor(pg);
    glGenFramebuffers(1, &r->gl_framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, r->gl_framebuffer);
    QTAILQ_INIT(&r->surfaces);
    r->downloads_pending = false;
    qemu_event_init(&r->downloads_complete, false);
    qemu_event_init(&r->dirty_surfaces_download_complete, false);

    init_render_to_texture(pg);
}

static void flush_surfaces(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    /* Clear last surface shape to force recreation of buffers at next draw */
    pg->surface_color.draw_dirty = false;
    pg->surface_zeta.draw_dirty = false;
    memset(&pg->last_surface_shape, 0, sizeof(pg->last_surface_shape));
    pgraph_gl_unbind_surface(d, true);
    pgraph_gl_unbind_surface(d, false);

#ifdef XEMU_UWP
    uwp_unpark(d, false);
    uwp_wb_flush_all(d);
#endif
    SurfaceBinding *s, *next;
    QTAILQ_FOREACH_SAFE(s, &r->surfaces, entry, next) {
        // FIXME: We should download all surfaces to ram, but need to
        //        investigate corruption issue
        // pgraph_gl_surface_download_if_dirty(d, s);
        pgraph_gl_surface_invalidate(d, s);
    }
}

void pgraph_gl_finalize_surfaces(PGRAPHState *pg)
{
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PGRAPHGLState *r = pg->gl_renderer_state;

    flush_surfaces(d);
    glDeleteFramebuffers(1, &r->gl_framebuffer);
    r->gl_framebuffer = 0;

    finalize_render_to_texture(pg);
}

void pgraph_gl_surface_flush(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    bool update_surface = (r->color_binding || r->zeta_binding);

    flush_surfaces(d);

    pgraph_gl_reload_surface_scale_factor(pg);

    if (update_surface) {
        pgraph_gl_surface_update(d, true, true, true);
    }
}
