/*
 * Geforce NV2A PGRAPH OpenGL Renderer
 *
 * Copyright (c) 2015 espes
 * Copyright (c) 2015 Jannik Vogel
 * Copyright (c) 2020-2025 Matt Borgerson
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
#include "qemu/fast-hash.h"
#include "qemu/mstring.h"

#include "xemu-version.h"
#include "ui/xemu-settings.h"
#include "hw/xbox/nv2a/pgraph/util.h"
#include "debug.h"
#include "renderer.h"
#include "qemu/xemu-uwp.h"

static GLenum get_gl_primitive_mode(enum ShaderPolygonMode polygon_mode, enum ShaderPrimitiveMode primitive_mode)
{
    switch (primitive_mode) {
    case PRIM_TYPE_POINTS: return GL_POINTS;
    case PRIM_TYPE_LINES: return GL_LINES;
    case PRIM_TYPE_LINE_LOOP: return GL_LINE_LOOP;
    case PRIM_TYPE_LINE_STRIP: return GL_LINE_STRIP;
    case PRIM_TYPE_TRIANGLES: return GL_TRIANGLES;
    case PRIM_TYPE_TRIANGLE_STRIP: return GL_TRIANGLE_STRIP;
    case PRIM_TYPE_TRIANGLE_FAN: return GL_TRIANGLE_FAN;
    case PRIM_TYPE_QUADS: return GL_LINES_ADJACENCY;
    case PRIM_TYPE_QUAD_STRIP: return GL_LINE_STRIP_ADJACENCY;
    case PRIM_TYPE_POLYGON:
        if (polygon_mode == POLY_MODE_LINE) {
            return GL_LINE_LOOP;
        } else if (polygon_mode == POLY_MODE_FILL) {
            return GL_TRIANGLE_FAN;
        }

        assert(!"PRIM_TYPE_POLYGON with invalid polygon_mode");
        return 0;
    default:
        assert(!"Invalid primitive_mode");
        return 0;
    }
}

static GLuint create_gl_shader(GLenum gl_shader_type,
                               const char *code,
                               const char *name)
{
    GLint compiled = 0;

    NV2A_GL_DGROUP_BEGIN("Creating new %s", name);

    NV2A_DPRINTF("compile new %s, code:\n%s\n", name, code);

    GLuint shader = glCreateShader(gl_shader_type);
    glShaderSource(shader, 1, &code, 0);
    glCompileShader(shader);

    /* Check it compiled */
    compiled = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        GLchar* log;
        GLint log_length;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_length);
        log = g_malloc(log_length * sizeof(GLchar));
        glGetShaderInfoLog(shader, log_length, NULL, log);
        fprintf(stderr, "%s\n\n" "nv2a: %s compilation failed: %s\n", code, name, log);
        g_free(log);

        NV2A_GL_DGROUP_END();
        abort();
    }

    NV2A_GL_DGROUP_END();

    return shader;
}

static void set_texture_sampler_uniforms(ShaderBinding *binding)
{
    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        char samplerName[16];
        snprintf(samplerName, sizeof(samplerName), "texSamp%d", i);
        GLint texSampLoc =
            glGetUniformLocation(binding->gl_program, samplerName);
        if (texSampLoc >= 0) {
            glUniform1i(texSampLoc, i);
        }
    }
}

static void update_shader_uniform_locs(ShaderBinding *binding)
{
    char tmp[64];

    for (int i = 0; i < ARRAY_SIZE(binding->uniform_locs.vsh); i++) {
        const char *name = VshUniformInfo[i].name;
        if (VshUniformInfo[i].count > 1) {
            snprintf(tmp, sizeof(tmp), "%s[0]", name);
            name = tmp;
        }
        binding->uniform_locs.vsh[i] = glGetUniformLocation(binding->gl_program, name);
    }

    for (int i = 0; i < ARRAY_SIZE(binding->uniform_locs.psh); i++) {
        const char *name = PshUniformInfo[i].name;
        if (PshUniformInfo[i].count > 1) {
            snprintf(tmp, sizeof(tmp), "%s[0]", name);
            name = tmp;
        }
        binding->uniform_locs.psh[i] = glGetUniformLocation(binding->gl_program, name);
    }
}

static void shader_module_cache_entry_init(Lru *lru, LruNode *node,
                                           const void *key)
{
    ShaderModuleCacheEntry *module =
        container_of(node, ShaderModuleCacheEntry, node);
    memcpy(&module->key, key, sizeof(ShaderModuleCacheKey));

    const char *kind_str;
    MString *code;

    switch (module->key.kind) {
    case GL_VERTEX_SHADER:
        kind_str = "vertex shader";
        code = pgraph_glsl_gen_vsh(&module->key.vsh.state,
                                   module->key.vsh.glsl_opts);
        break;
    case GL_GEOMETRY_SHADER:
        kind_str = "geometry shader";
        code = pgraph_glsl_gen_geom(&module->key.geom.state,
                                    module->key.geom.glsl_opts);
        break;
    case GL_FRAGMENT_SHADER:
        kind_str = "fragment shader";
        code = pgraph_glsl_gen_psh(&module->key.psh.state,
                                   module->key.psh.glsl_opts);
        break;
    default:
        assert(!"Invalid shader module kind");
        kind_str = "unknown";
        code = NULL;
    }

    module->gl_shader =
        create_gl_shader(module->key.kind, mstring_get_str(code), kind_str);
    mstring_unref(code);
}

static void shader_module_cache_entry_post_evict(Lru *lru, LruNode *node)
{
    ShaderModuleCacheEntry *module =
        container_of(node, ShaderModuleCacheEntry, node);
    glDeleteShader(module->gl_shader);
}

static bool shader_module_cache_entry_compare(Lru *lru, LruNode *node,
                                              const void *key)
{
    ShaderModuleCacheEntry *module =
        container_of(node, ShaderModuleCacheEntry, node);
    return memcmp(&module->key, key, sizeof(ShaderModuleCacheKey));
}

static GLuint get_shader_module_for_key(PGRAPHGLState *r,
                                        const ShaderModuleCacheKey *key)
{
    uint64_t hash = fast_hash((void *)key, sizeof(ShaderModuleCacheKey));
    LruNode *node = lru_lookup(&r->shader_module_cache, hash, key);
    ShaderModuleCacheEntry *module =
        container_of(node, ShaderModuleCacheEntry, node);
    return module->gl_shader;
}

static void generate_shaders(PGRAPHGLState *r, ShaderBinding *binding)
{
    GLuint program = glCreateProgram();

    ShaderState *state = &binding->state;
    ShaderModuleCacheKey key;

    bool need_geometry_shader = pgraph_glsl_need_geom(&state->geom);
    if (need_geometry_shader) {
        memset(&key, 0, sizeof(key));
        key.kind = GL_GEOMETRY_SHADER;
        key.geom.state = state->geom;
        glAttachShader(program, get_shader_module_for_key(r, &key));
    }

    /* create the vertex shader */
    memset(&key, 0, sizeof(key));
    key.kind = GL_VERTEX_SHADER;
    key.vsh.state = state->vsh;
    key.vsh.glsl_opts.prefix_outputs = need_geometry_shader;
    glAttachShader(program, get_shader_module_for_key(r, &key));

    /* generate a fragment shader from register combiners */
    memset(&key, 0, sizeof(key));
    key.kind = GL_FRAGMENT_SHADER;
    key.psh.state = state->psh;
    glAttachShader(program, get_shader_module_for_key(r, &key));

    /* link the program */
    glLinkProgram(program);
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if(!linked) {
        GLchar log[2048];
        glGetProgramInfoLog(program, 2048, NULL, log);
        fprintf(stderr, "nv2a: shader linking failed: %s\n", log);
        abort();
    }

    glUseProgram(program);

    binding->gl_program = program;
    binding->gl_primitive_mode = get_gl_primitive_mode(
        state->geom.polygon_front_mode, state->geom.primitive_mode);
    binding->initialized = true;

    set_texture_sampler_uniforms(binding);

    /* validate the program */
    GLint valid = 0;
    glValidateProgram(program);
    glGetProgramiv(program, GL_VALIDATE_STATUS, &valid);
    if (!valid) {
        GLchar log[1024];
        glGetProgramInfoLog(program, 1024, NULL, log);
        fprintf(stderr, "nv2a: shader validation failed: %s\n", log);
        abort();
    }

    update_shader_uniform_locs(binding);
}

static const char *shader_gl_vendor = NULL;
static const char *shader_gl_version = NULL;

static void shader_create_cache_folder(void)
{
    char *shader_path = g_strdup_printf("%sshaders", xemu_settings_get_base_path());
    qemu_mkdir(shader_path);
    g_free(shader_path);
}

static char *shader_get_lru_cache_path(void)
{
    return g_strdup_printf("%s/shader_cache_list", xemu_settings_get_base_path());
}

static void shader_write_lru_list_entry_to_disk(Lru *lru, LruNode *node, void *opaque)
{
    FILE *lru_list_file = (FILE*) opaque;
    size_t written = fwrite(&node->hash, sizeof(uint64_t), 1, lru_list_file);
    if (written != 1) {
        fprintf(stderr, "nv2a: Failed to write shader list entry %llx to disk\n",
                (unsigned long long) node->hash);
    }
}

void pgraph_gl_shader_write_cache_reload_list(PGRAPHState *pg)
{
    PGRAPHGLState *r = pg->gl_renderer_state;

    if (!g_config.perf.cache_shaders) {
        qatomic_set(&r->shader_cache_writeback_pending, false);
        qemu_event_set(&r->shader_cache_writeback_complete);
        return;
    }

    char *shader_lru_path = shader_get_lru_cache_path();
    qemu_thread_join(&r->shader_disk_thread);

    FILE *lru_list = qemu_fopen(shader_lru_path, "wb");
    g_free(shader_lru_path);
    if (!lru_list) {
        fprintf(stderr, "nv2a: Failed to open shader LRU cache for writing\n");
        return;
    }

    lru_visit_active(&r->shader_cache, shader_write_lru_list_entry_to_disk, lru_list);
    fclose(lru_list);

    lru_flush(&r->shader_cache);

    qatomic_set(&r->shader_cache_writeback_pending, false);
    qemu_event_set(&r->shader_cache_writeback_complete);
}

bool pgraph_gl_shader_load_from_memory(ShaderBinding *binding)
{
    assert(glGetError() == GL_NO_ERROR);

    if (!binding->program) {
        return false;
    }

    GLuint gl_program = glCreateProgram();
    glProgramBinary(gl_program, binding->program_format, binding->program,
                    binding->program_size);
    GLint gl_error = glGetError();
    if (gl_error != GL_NO_ERROR) {
        NV2A_DPRINTF(
            "failed to load shader binary from disk: GL error code %d\n",
            gl_error);
        glDeleteProgram(gl_program);
        return false;
    }

    GLint link_status = GL_FALSE;
    glGetProgramiv(gl_program, GL_LINK_STATUS, &link_status);
    if (!link_status) {
        NV2A_DPRINTF(
            "failed to load shader binary from disk: link status is FALSE\n");
        glDeleteProgram(gl_program);
        return false;
    }

    glUseProgram(gl_program);

    g_free(binding->program);

    binding->program = NULL;
    binding->gl_program = gl_program;
    binding->gl_primitive_mode =
        get_gl_primitive_mode(binding->state.geom.polygon_front_mode,
                              binding->state.geom.primitive_mode);
    binding->initialized = true;

    set_texture_sampler_uniforms(binding);

    glValidateProgram(gl_program);
    GLint valid = 0;
    glGetProgramiv(gl_program, GL_VALIDATE_STATUS, &valid);
    if (!valid) {
        GLchar log[1024];
        glGetProgramInfoLog(gl_program, 1024, NULL, log);
        NV2A_DPRINTF("failed to load shader binary from disk: %s\n", log);
        glDeleteProgram(gl_program);
        return false;
    }

    update_shader_uniform_locs(binding);

    return true;
}

static char *shader_get_bin_directory(uint64_t hash)
{
    const char *cfg_dir = xemu_settings_get_base_path();
    char *shader_bin_dir =
        g_strdup_printf("%s/shaders/%04x", cfg_dir, (uint32_t)(hash >> 48));
    return shader_bin_dir;
}

static char *shader_get_binary_path(const char *shader_bin_dir, uint64_t hash)
{
    uint64_t bin_mask = (uint64_t)0xffff << 48;
    return g_strdup_printf("%s/%012" PRIx64, shader_bin_dir, hash & ~bin_mask);
}

static void shader_load_from_disk(PGRAPHState *pg, uint64_t hash)
{
    PGRAPHGLState *r = pg->gl_renderer_state;

    char *shader_bin_dir = shader_get_bin_directory(hash);
    char *shader_path = shader_get_binary_path(shader_bin_dir, hash);
    char *cached_xemu_version = NULL;
    char *cached_gl_vendor = NULL;
    char *cached_gl_version = NULL;
    void *program_buffer = NULL;

    uint64_t cached_xemu_version_len;
    uint64_t gl_vendor_len;
    uint64_t gl_version_len;
    GLenum program_binary_format;
    ShaderState state;
    size_t shader_size;

    g_free(shader_bin_dir);

    qemu_mutex_lock(&r->shader_cache_lock);
    if (lru_contains_hash(&r->shader_cache, hash)) {
        qemu_mutex_unlock(&r->shader_cache_lock);
        return;
    }
    qemu_mutex_unlock(&r->shader_cache_lock);

    FILE *shader_file = qemu_fopen(shader_path, "rb");
    if (!shader_file) {
        goto error;
    }

    size_t nread;
    #define READ_OR_ERR(data, data_len) \
        do { \
            nread = fread(data, data_len, 1, shader_file); \
            if (nread != 1) { \
                fclose(shader_file); \
                goto error; \
            } \
        } while (0)

    READ_OR_ERR(&cached_xemu_version_len, sizeof(cached_xemu_version_len));

    cached_xemu_version = g_malloc(cached_xemu_version_len +1);
    READ_OR_ERR(cached_xemu_version, cached_xemu_version_len);
    if (strcmp(cached_xemu_version, xemu_version) != 0) {
        fclose(shader_file);
        goto error;
    }

    READ_OR_ERR(&gl_vendor_len, sizeof(gl_vendor_len));

    cached_gl_vendor = g_malloc(gl_vendor_len);
    READ_OR_ERR(cached_gl_vendor, gl_vendor_len);
    if (strcmp(cached_gl_vendor, shader_gl_vendor) != 0) {
        fclose(shader_file);
        goto error;
    }

    READ_OR_ERR(&gl_version_len, sizeof(gl_version_len));

    cached_gl_version = g_malloc(gl_version_len);
    READ_OR_ERR(cached_gl_version, gl_version_len);
    if (strcmp(cached_gl_version, shader_gl_version) != 0) {
        fclose(shader_file);
        goto error;
    }

    READ_OR_ERR(&program_binary_format, sizeof(program_binary_format));
    READ_OR_ERR(&state, sizeof(state));
    READ_OR_ERR(&shader_size, sizeof(shader_size));

    program_buffer = g_malloc(shader_size);
    READ_OR_ERR(program_buffer, shader_size);

    #undef READ_OR_ERR

    fclose(shader_file);
    g_free(shader_path);
    g_free(cached_xemu_version);
    g_free(cached_gl_vendor);
    g_free(cached_gl_version);

    qemu_mutex_lock(&r->shader_cache_lock);
    LruNode *node = lru_lookup(&r->shader_cache, hash, &state);
    ShaderBinding *binding = container_of(node, ShaderBinding, node);

    /* If we happened to regenerate this shader already, then we may as well use the new one */
    if (binding->initialized) {
        qemu_mutex_unlock(&r->shader_cache_lock);
        return;
    }

    binding->program_format = program_binary_format;
    binding->program_size = shader_size;
    binding->program = program_buffer;
    binding->cached = true;
    qemu_mutex_unlock(&r->shader_cache_lock);
    return;

error:
    /* Delete the shader so it won't be loaded again */
    qemu_unlink(shader_path);
    g_free(shader_path);
    g_free(program_buffer);
    g_free(cached_xemu_version);
    g_free(cached_gl_vendor);
    g_free(cached_gl_version);
}

static void *shader_reload_lru_from_disk(void *arg)
{
    if (!g_config.perf.cache_shaders) {
        return NULL;
    }

    PGRAPHState *pg = (PGRAPHState*) arg;
    char *shader_lru_path = shader_get_lru_cache_path();

    FILE *lru_shaders_list = qemu_fopen(shader_lru_path, "rb");
    g_free(shader_lru_path);
    if (!lru_shaders_list) {
        return NULL;
    }

    uint64_t hash;
    while (fread(&hash, sizeof(uint64_t), 1, lru_shaders_list) == 1) {
        shader_load_from_disk(pg, hash);
    }

    return NULL;
}

static void shader_cache_entry_init(Lru *lru, LruNode *node, const void *state)
{
    ShaderBinding *binding = container_of(node, ShaderBinding, node);
    memcpy(&binding->state, state, sizeof(ShaderState));
    binding->initialized = false;
    binding->cached = false;
    binding->program = NULL;
    binding->save_thread = NULL;
#ifdef XEMU_UWP
    binding->uwp_warm_noted = false;
    binding->uwp_vsh_last = NULL;
    binding->uwp_psh_last = NULL;
    binding->uwp_last_program = 0;
#endif
}

static void shader_cache_entry_post_evict(Lru *lru, LruNode *node)
{
    ShaderBinding *binding = container_of(node, ShaderBinding, node);

    if (binding->save_thread) {
        qemu_thread_join(binding->save_thread);
        g_free(binding->save_thread);
    }

    glDeleteProgram(binding->gl_program);
    if (binding->program) {
        g_free(binding->program);
    }

    binding->cached = false;
    binding->save_thread = NULL;
    binding->program = NULL;
    memset(&binding->state, 0, sizeof(ShaderState));
#ifdef XEMU_UWP
    g_free(binding->uwp_vsh_last);
    g_free(binding->uwp_psh_last);
    binding->uwp_vsh_last = NULL;
    binding->uwp_psh_last = NULL;
    binding->uwp_last_program = 0;
#endif
}

static bool shader_cache_entry_compare(Lru *lru, LruNode *node, const void *key)
{
    ShaderBinding *binding = container_of(node, ShaderBinding, node);
    return memcmp(&binding->state, key, sizeof(ShaderState));
}

#ifdef XEMU_UWP
static void uwp_warm_start(PGRAPHGLState *r);

/*
 * The shader caches hold 51200 entries each, so nothing is ever freed. A game that keeps making new shader
 * variations (Ninja Gaiden: 857 in 100 s of fighting, about 3 MB each in the GL-on-D3D12 driver) used up the
 * console's 5 GB and crashed, even with old textures being freed. So, at most twice a second: with less than
 * 1 GB still allowed, the least recently used quarter of the linked programs and of the compiled shader
 * modules is freed. Called with shader_cache_lock held, after the program for the next draw was picked
 * (most recently used: never the one freed).
 */
static void uwp_shader_cache_trim(PGRAPHGLState *r)
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
    int kept = r->shader_cache.num_used, want = kept / 4, freed = 0;
    while (freed < want && lru_try_evict_one(&r->shader_cache)) {
        freed++;
    }
    int mkept = r->shader_module_cache.num_used, mwant = mkept / 4, mfreed = 0;
    while (mfreed < mwant && lru_try_evict_one(&r->shader_module_cache)) {
        mfreed++;
    }
    uwp_log("shader cache: %llu MB in use, %llu MB still allowed: %d of %d programs and %d of %d modules freed",
            (unsigned long long)(used >> 20), (unsigned long long)(left >> 20), freed, kept, mfreed, mkept);
}
#endif

void pgraph_gl_init_shaders(PGRAPHState *pg)
{
    PGRAPHGLState *r = pg->gl_renderer_state;

    qemu_mutex_init(&r->shader_cache_lock);
    qemu_event_init(&r->shader_cache_writeback_complete, false);

    if (!shader_gl_vendor) {
        shader_gl_vendor = (const char *) glGetString(GL_VENDOR);
    }
    if (!shader_gl_version) {
        shader_gl_version = (const char *) glGetString(GL_VERSION);
    }

    shader_create_cache_folder();

    /* FIXME: Make this configurable */
    const size_t shader_cache_size = 50*1024;
    lru_init(&r->shader_cache);
    r->shader_cache_entries = malloc(shader_cache_size * sizeof(ShaderBinding));
    assert(r->shader_cache_entries != NULL);
    for (int i = 0; i < shader_cache_size; i++) {
        lru_add_free(&r->shader_cache, &r->shader_cache_entries[i].node);
    }

    r->shader_cache.init_node = shader_cache_entry_init;
    r->shader_cache.compare_nodes = shader_cache_entry_compare;
    r->shader_cache.post_node_evict = shader_cache_entry_post_evict;

    qemu_thread_create(&r->shader_disk_thread, "pgraph.renderer_state->shader_cache",
                       shader_reload_lru_from_disk, pg, QEMU_THREAD_JOINABLE);

    /* FIXME: Make this configurable */
    const size_t shader_module_cache_size = 50*1024;
    lru_init(&r->shader_module_cache);
    r->shader_module_cache_entries =
        g_malloc_n(shader_module_cache_size, sizeof(ShaderModuleCacheEntry));
    assert(r->shader_module_cache_entries != NULL);
    for (int i = 0; i < shader_module_cache_size; i++) {
        lru_add_free(&r->shader_module_cache, &r->shader_module_cache_entries[i].node);
    }

    r->shader_module_cache.init_node = shader_module_cache_entry_init;
    r->shader_module_cache.compare_nodes = shader_module_cache_entry_compare;
    r->shader_module_cache.post_node_evict = shader_module_cache_entry_post_evict;
#ifdef XEMU_UWP
    uwp_warm_start(r);
#endif
}

void pgraph_gl_finalize_shaders(PGRAPHState *pg)
{
    PGRAPHGLState *r = pg->gl_renderer_state;

    // Clear out shader cache
    pgraph_gl_shader_write_cache_reload_list(pg); // FIXME: also flushes, rename for clarity
    free(r->shader_cache_entries);
    r->shader_cache_entries = NULL;

    lru_flush(&r->shader_module_cache);
    g_free(r->shader_module_cache_entries);
    r->shader_module_cache_entries = NULL;

    qemu_mutex_destroy(&r->shader_cache_lock);
}

static void *shader_write_to_disk(void *arg)
{
    ShaderBinding *binding = (ShaderBinding*) arg;

    char *shader_bin = shader_get_bin_directory(binding->node.hash);
    char *shader_path = shader_get_binary_path(shader_bin, binding->node.hash);

    static uint64_t gl_vendor_len;
    if (gl_vendor_len == 0) {
        gl_vendor_len = (uint64_t) (strlen(shader_gl_vendor) + 1);
    }

    static uint64_t gl_version_len;
    if (gl_version_len == 0) {
        gl_version_len = (uint64_t) (strlen(shader_gl_version) + 1);
    }

    static uint64_t xemu_version_len = 0;
    if (xemu_version_len == 0) {
        xemu_version_len = (uint64_t) (strlen(xemu_version) + 1);
    }

    qemu_mkdir(shader_bin);
    g_free(shader_bin);

    FILE *shader_file = qemu_fopen(shader_path, "wb");
    if (!shader_file) {
        goto error;
    }

    size_t written;
    #define WRITE_OR_ERR(data, data_size) \
        do { \
            written = fwrite(data, data_size, 1, shader_file); \
            if (written != 1) { \
                fclose(shader_file); \
                goto error; \
            } \
        } while (0)

    WRITE_OR_ERR(&xemu_version_len, sizeof(xemu_version_len));
    WRITE_OR_ERR(xemu_version, xemu_version_len);

    WRITE_OR_ERR(&gl_vendor_len, sizeof(gl_vendor_len));
    WRITE_OR_ERR(shader_gl_vendor, gl_vendor_len);

    WRITE_OR_ERR(&gl_version_len, sizeof(gl_version_len));
    WRITE_OR_ERR(shader_gl_version, gl_version_len);

    WRITE_OR_ERR(&binding->program_format, sizeof(binding->program_format));
    WRITE_OR_ERR(&binding->state, sizeof(binding->state));

    WRITE_OR_ERR(&binding->program_size, sizeof(binding->program_size));
    WRITE_OR_ERR(binding->program, binding->program_size);

    #undef WRITE_OR_ERR

    fclose(shader_file);

    g_free(shader_path);
    g_free(binding->program);
    binding->program = NULL;

    return NULL;

error:
    fprintf(stderr, "nv2a: Failed to write shader binary file to %s\n", shader_path);
    qemu_unlink(shader_path);
    g_free(shader_path);
    g_free(binding->program);
    binding->program = NULL;
    return NULL;
}

void pgraph_gl_shader_cache_to_disk(ShaderBinding *binding)
{
    if (binding->cached) {
        return;
    }

    GLint program_size;
    glGetProgramiv(binding->gl_program, GL_PROGRAM_BINARY_LENGTH, &program_size);

    if (binding->program) {
        g_free(binding->program);
        binding->program = NULL;
    }

    /* program_size might be zero on some systems, if no binary formats are supported */
    if (program_size == 0) {
        return;
    }

    binding->program = g_malloc(program_size);
    GLsizei program_size_copied;
    glGetProgramBinary(binding->gl_program, program_size, &program_size_copied,
                       &binding->program_format, binding->program);
    assert(glGetError() == GL_NO_ERROR);

    binding->program_size = program_size_copied;
    binding->cached = true;

    char name[24];
    snprintf(name, sizeof(name), "scache-%llx", (unsigned long long) binding->node.hash);
    binding->save_thread = g_malloc0(sizeof(QemuThread));
    qemu_thread_create(binding->save_thread, name, shader_write_to_disk, binding, QEMU_THREAD_JOINABLE);
}

#ifdef XEMU_UWP
/*
 * Every draw handed every uniform to GL again, the 192 vertex shader constants (3 KB) included: the GL
 * driver compared or re-uploaded them each time, a large part of its work in scenes with thousands of
 * draws a frame (Project Gotham Racing: 3,300). The values last given to this program are kept (last);
 * unchanged uniforms are skipped, and of a changed array only the changed elements are sent (array
 * elements have consecutive locations in Mesa). Test file uniform-all.txt: everything every draw.
 */
static bool uwp_uniform_all(void)
{
    static int on = -1;
    if (on < 0) {
        extern bool xemu_uwp_test_file_exists(const char *name); /* ui/xemu.c */
        on = xemu_uwp_test_file_exists("uniform-all.txt");
        if (on) {
            uwp_log("uniforms: all sent at every draw (uniform-all.txt)");
        }
    }
    return on;
}
#endif

static void apply_uniform_updates(const UniformInfo *info, int *locs,
                                  void *values, size_t count
#ifdef XEMU_UWP
                                  , void *last
#endif
                                  )
{
    for (int i = 0; i < count; i++) {
        if (locs[i] == -1) {
            continue;
        }

        void *value = (char*)values + info[i].val_offs;
#ifdef XEMU_UWP
        int loc = locs[i];
        size_t n = info[i].count;
        if (last) {
            void *prev = (char *)last + info[i].val_offs;
            size_t es = info[i].size;
            if (!memcmp(prev, value, es * n)) {
                continue;
            }
            size_t first = 0, end = n;
            while (first < n && !memcmp((char *)prev + first * es, (char *)value + first * es, es)) {
                first++;
            }
            while (end > first && !memcmp((char *)prev + (end - 1) * es, (char *)value + (end - 1) * es, es)) {
                end--;
            }
            memcpy((char *)prev + first * es, (char *)value + first * es, (end - first) * es);
            loc += first;
            value = (char *)value + first * es;
            n = end - first;
        }
#define UWP_LOC loc
#define UWP_N n
#else
#define UWP_LOC locs[i]
#define UWP_N info[i].count
#endif

        switch (info[i].type) {
        case UniformElementType_uint:
            glUniform1uiv(UWP_LOC, UWP_N, value);
            break;
        case UniformElementType_int:
            glUniform1iv(UWP_LOC, UWP_N, value);
            break;
        case UniformElementType_ivec2:
            glUniform2iv(UWP_LOC, UWP_N, value);
            break;
        case UniformElementType_ivec4:
            glUniform4iv(UWP_LOC, UWP_N, value);
            break;
        case UniformElementType_float:
            glUniform1fv(UWP_LOC, UWP_N, value);
            break;
        case UniformElementType_vec2:
            glUniform2fv(UWP_LOC, UWP_N, value);
            break;
        case UniformElementType_vec3:
            glUniform3fv(UWP_LOC, UWP_N, value);
            break;
        case UniformElementType_vec4:
            glUniform4fv(UWP_LOC, UWP_N, value);
            break;
        case UniformElementType_mat2:
            glUniformMatrix2fv(UWP_LOC, UWP_N, GL_FALSE, value);
            break;
        default:
            g_assert_not_reached();
        }
#undef UWP_LOC
#undef UWP_N
    }


#ifdef XEMU_UWP
    {
        /*
         * An error raised by any earlier GL call shows up here. Closing the
         * emulator for it helps nobody on a console: note it and carry on.
         */
        GLenum err = glGetError();
        if (err != GL_NO_ERROR) {
            if (xemu_uwp_counters[UWP_CNT_GL_ERRORS]++ < 20) {
                uwp_log("GL error 0x%x seen at the uniform update (0x505 = out of memory)", err);
            }
            while (glGetError() != GL_NO_ERROR) {
            }
        }
    }
#else
    assert(glGetError() == GL_NO_ERROR);
#endif
}

// FIXME: Dirty tracking
// FIXME: Consider UBO to align with VK renderer
static void update_shader_uniforms(PGRAPHState *pg, ShaderBinding *binding)
{
    PGRAPHGLState *r = pg->gl_renderer_state;

#ifdef XEMU_UWP
    /* the first update for this program (or a new program in this binding) sends everything */
    bool fresh = binding->uwp_last_program != binding->gl_program || !binding->uwp_vsh_last || uwp_uniform_all();
    if (!binding->uwp_vsh_last) {
        binding->uwp_vsh_last = g_new0(VshUniformValues, 1);
        binding->uwp_psh_last = g_new0(PshUniformValues, 1);
    }
#endif
    VshUniformValues vsh_values;
    pgraph_glsl_set_vsh_uniform_values(pg, &binding->state.vsh,
                                  binding->uniform_locs.vsh, &vsh_values);
#ifdef XEMU_UWP
    if (fresh) {
        memcpy(binding->uwp_vsh_last, &vsh_values, sizeof(vsh_values));
    }
    apply_uniform_updates(VshUniformInfo, binding->uniform_locs.vsh,
                          &vsh_values, VshUniform__COUNT, fresh ? NULL : binding->uwp_vsh_last);
#else
    apply_uniform_updates(VshUniformInfo, binding->uniform_locs.vsh,
                          &vsh_values, VshUniform__COUNT);
#endif

    PshUniformValues psh_values;
    pgraph_glsl_set_psh_uniform_values(pg, binding->uniform_locs.psh, &psh_values);

    for (int i = 0; i < 4; i++) {
        if (r->texture_binding[i] != NULL) {
            float scale = r->texture_binding[i]->scale;
            psh_values.texScale[i] = scale;
        }
    }
#ifdef XEMU_UWP
    if (fresh) {
        memcpy(binding->uwp_psh_last, &psh_values, sizeof(psh_values));
    }
    apply_uniform_updates(PshUniformInfo, binding->uniform_locs.psh,
                          &psh_values, PshUniform__COUNT, fresh ? NULL : binding->uwp_psh_last);
    binding->uwp_last_program = binding->gl_program;
#else
    apply_uniform_updates(PshUniformInfo, binding->uniform_locs.psh,
                          &psh_values, PshUniform__COUNT);
#endif
}

#ifdef XEMU_UWP
/*
 * Shader warm-up. Mesa on the Xbox cannot keep compiled shaders between runs (no program
 * binaries, no disk cache), and the console's driver compiles each shader at the first draw
 * that uses it, for that draw's setup (target formats, vertex layout, texture types). Every
 * start therefore compiled the Xbox logo animation's shaders while it was on screen: frames
 * of 0.1 to 1.4 s, and every new track or level stalled the same way. The first draw of every
 * shader of a run is recorded per disc in uwp_warm_<disc>.bin (shader state + draw setup + when
 * it was first needed); the next start replays those draws off screen, with no pixels covered:
 * those needed in the first 30 s before the first frame, the rest in the background.
 */
#define UWP_WARM_MAGIC 0x364d5257u /* "WRM6" */
/* 2048 was full for big games (Ninja Gaiden Black: its attract demo still built 40-50 shaders a second);
 * the warm-up stops with 1.5 GB left anyway, so a longer list only helps where memory allows */
#define UWP_WARM_MAX 4096
#define UWP_WARM_LAYOUT 1
/* shaders first needed this soon after start are ready before the first frame; the rest
   are prepared in the background while the game runs */
#define UWP_WARM_BEFORE_MS 30000
#define UWP_WARM_UNITS 4

typedef struct UwpWarmPipe {
    uint32_t color_fmt, depth_fmt; /* GL internal formats, 0 = none */
    uint32_t prim;
    uint32_t blend_src_rgb, blend_dst_rgb, blend_src_a, blend_dst_a, blend_eq_rgb, blend_eq_a;
    uint32_t depth_func;
    uint8_t blend, depth_test, depth_mask, stencil, cull, color_mask, pad[2];
    /* the rest of what the console's driver builds into a pipeline */
    uint32_t front_face, cull_mode, provoking;
    uint8_t depth_clamp, poly_smooth, line_smooth, point_size, offset_fill, offset_line,
        offset_point, pad2;
    float line_width, offset_factor, offset_units;
    uint32_t stencil_func, stencil_mask, stencil_fail, stencil_zfail, stencil_zpass,
        stencil_write;
    struct {
        uint32_t type;
        int32_t size; /* 1-4, or GL_BGRA for Xbox colours */
        uint8_t enabled, normalized, integer, pad;
    } attr[16];
    struct {
        uint32_t target, fmt;
    } tex[UWP_WARM_UNITS];
} UwpWarmPipe;

typedef struct UwpWarmRecord {
    uint64_t check;
    uint32_t ms, pad; /* when it was first needed, ms after start */
    ShaderState state;
    UwpWarmPipe pipe;
} UwpWarmRecord;

static int64_t uwp_warm_start_ms = -1;
static int uwp_warm_count;

static char *uwp_warm_path(void)
{
    const char *disc = g_config.sys.files.dvd_path ? g_config.sys.files.dvd_path : "";
    const char *base = strrchr(disc, '/');
    const char *base2 = strrchr(disc, '\\');
    base = base2 > base ? base2 : base;
    base = base ? base + 1 : disc;
    return g_strdup_printf("%suwp_warm_%016llx.bin", xemu_settings_get_base_path(),
                           (unsigned long long)fast_hash((const uint8_t *)base, strlen(base)));
}

static void uwp_warm_header(uint32_t head[3])
{
    head[0] = UWP_WARM_MAGIC;
    head[1] = sizeof(UwpWarmRecord);
    /*
     * Not the xemu version: the list must survive updates (players would otherwise lose it
     * with every new build). Raise UWP_WARM_LAYOUT when ShaderState or the GLSL generators
     * change what a stored state means; a changed size is caught by head[1].
     */
    head[2] = UWP_WARM_LAYOUT;
}

static uint64_t uwp_warm_check(const UwpWarmRecord *rec)
{
    return fast_hash((const uint8_t *)&rec->ms, sizeof(*rec) - sizeof(rec->check));
}

/*
 * Lists written before VshState.uwp_attr_kinds existed (up to 0.9.104): the field was added at the end of
 * VshState, so every byte after it moved by 4. Such a list is converted (the new field 0 = attributes read
 * as before, which is what those shaders were built for) and written back, instead of being thrown away
 * - players would otherwise get first-play stutter again in every game after updating.
 */
QEMU_BUILD_BUG_ON(offsetof(VshState, uwp_attr_kinds) + 4 != sizeof(VshState));
QEMU_BUILD_BUG_ON(offsetof(ShaderState, vsh) != 0);
static bool uwp_warm_migrate(const char *path, char **data, gsize *len, const uint32_t want[3])
{
    const uint32_t *head = (const uint32_t *)*data;
    size_t ins = offsetof(UwpWarmRecord, state) + offsetof(VshState, uwp_attr_kinds);
    size_t used_new = offsetof(UwpWarmRecord, pipe) + sizeof(UwpWarmPipe);
    size_t used_old = used_new - 4;
    size_t old_size = ROUND_UP(used_old, __alignof__(UwpWarmRecord));
    if (head[0] != want[0] || head[2] != want[2] || head[1] != old_size || old_size == sizeof(UwpWarmRecord)) {
        return false;
    }
    int n = (*len - 3 * sizeof(uint32_t)) / old_size;
    GString *out = g_string_sized_new(3 * sizeof(uint32_t) + n * sizeof(UwpWarmRecord));
    g_string_append_len(out, (const char *)want, 3 * sizeof(uint32_t));
    int kept = 0;
    for (int i = 0; i < n; i++) {
        const uint8_t *old = (const uint8_t *)*data + 3 * sizeof(uint32_t) + (size_t)i * old_size;
        uint64_t check;
        memcpy(&check, old, sizeof(check));
        if (check != fast_hash(old + sizeof(check), old_size - sizeof(check))) {
            break; /* damaged from here on */
        }
        UwpWarmRecord rec;
        memset(&rec, 0, sizeof(rec));
        memcpy(&rec, old, ins);
        memcpy((uint8_t *)&rec + ins + 4, old + ins, used_old - ins);
        rec.check = uwp_warm_check(&rec);
        g_string_append_len(out, (const char *)&rec, sizeof(rec));
        kept++;
    }
    g_free(*data);
    *len = out->len;
    *data = g_string_free(out, false);
    g_file_set_contents(path, *data, *len, NULL);
    uwp_log("shader warm-up: list from an older build converted (%d of %d entries)", kept, n);
    return true;
}

/* a small texture of this kind and format, made once */
static GLuint uwp_warm_texture(GLenum target, GLenum fmt)
{
    /* per thread: each warm-up thread has its own context */
    static __thread struct {
        GLenum target, fmt;
        GLuint tex;
    } made[64];
    static __thread int num;
    for (int i = 0; i < num; i++) {
        if (made[i].target == target && made[i].fmt == fmt) {
            return made[i].tex;
        }
    }
    if (num == ARRAY_SIZE(made)) {
        return 0;
    }
    while (glGetError() != GL_NO_ERROR) {
    }
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(target, tex);
    switch (target) {
    case GL_TEXTURE_1D:
        glTexStorage1D(target, 1, fmt, 4);
        break;
    case GL_TEXTURE_3D:
        glTexStorage3D(target, 1, fmt, 4, 4, 4);
        break;
    default: /* 2D, rectangle, cube map */
        glTexStorage2D(target, 1, fmt, target == GL_TEXTURE_CUBE_MAP ? 4 : 64,
                       target == GL_TEXTURE_CUBE_MAP ? 4 : 64);
        break;
    }
    if (target != GL_TEXTURE_RECTANGLE) {
        glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, 0);
    }
    glBindTexture(target, 0);
    if (glGetError() != GL_NO_ERROR) {
        glDeleteTextures(1, &tex);
        tex = 0;
    }
    made[num].target = target;
    made[num].fmt = fmt;
    made[num].tex = tex;
    num++;
    return tex;
}

static void uwp_warm_enable(GLenum cap, bool on)
{
    if (on) {
        glEnable(cap);
    } else {
        glDisable(cap);
    }
}

/* testing aid: why a replay was skipped (first 12 of a run) */
static void uwp_warm_why(const UwpWarmPipe *p, const char *what, unsigned int code)
{
    static int told;
    if (qatomic_fetch_inc(&told) >= 12) {
        return;
    }
    uwp_log("shader warm-up: skipped (%s 0x%x): color 0x%x depth 0x%x prim 0x%x tex %x:%x %x:%x "
            "%x:%x %x:%x", what, code, p->color_fmt, p->depth_fmt, p->prim, p->tex[0].target,
            p->tex[0].fmt, p->tex[1].target, p->tex[1].fmt, p->tex[2].target, p->tex[2].fmt,
            p->tex[3].target, p->tex[3].fmt);
}

static bool uwp_warm_replay(const UwpWarmPipe *p, GLuint fbo, GLuint vao)
{
    while (glGetError() != GL_NO_ERROR) {
    }
    GLuint color = p->color_fmt ? uwp_warm_texture(GL_TEXTURE_2D, p->color_fmt) : 0;
    GLuint depth = p->depth_fmt ? uwp_warm_texture(GL_TEXTURE_2D, p->depth_fmt) : 0;
    if ((p->color_fmt && !color) || (p->depth_fmt && !depth)) {
        uwp_warm_why(p, "target texture", p->color_fmt && !color ? p->color_fmt : p->depth_fmt);
        return false;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
    bool ds = p->depth_fmt == GL_DEPTH24_STENCIL8 || p->depth_fmt == GL_DEPTH32F_STENCIL8;
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, ds ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT,
                           GL_TEXTURE_2D, depth, 0);
    GLenum fb_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (fb_status != GL_FRAMEBUFFER_COMPLETE) {
        uwp_warm_why(p, "framebuffer", fb_status);
        return false;
    }
    for (int u = 0; u < UWP_WARM_UNITS; u++) {
        glActiveTexture(GL_TEXTURE0 + u);
        if (p->tex[u].target) {
            glBindTexture(p->tex[u].target, uwp_warm_texture(p->tex[u].target, p->tex[u].fmt));
        }
    }
    GLenum stage_err = glGetError();
    if (stage_err != GL_NO_ERROR) {
        uwp_warm_why(p, "textures", stage_err);
        return false;
    }
    glViewport(0, 0, 64, 64);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(p->color_mask & 1, p->color_mask & 2, p->color_mask & 4, p->color_mask & 8);
    if (p->blend) {
        glEnable(GL_BLEND);
        glBlendFuncSeparate(p->blend_src_rgb, p->blend_dst_rgb, p->blend_src_a, p->blend_dst_a);
        glBlendEquationSeparate(p->blend_eq_rgb, p->blend_eq_a);
    } else {
        glDisable(GL_BLEND);
    }
    if (p->depth_test) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(p->depth_func);
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    glDepthMask(p->depth_mask);
    if (p->stencil) {
        glEnable(GL_STENCIL_TEST);
    } else {
        glDisable(GL_STENCIL_TEST);
    }
    if (p->cull) {
        glEnable(GL_CULL_FACE);
    } else {
        glDisable(GL_CULL_FACE);
    }
    glFrontFace(p->front_face);
    glCullFace(p->cull_mode);
    glProvokingVertex(p->provoking);
    uwp_warm_enable(GL_DEPTH_CLAMP, p->depth_clamp);
    uwp_warm_enable(GL_POLYGON_SMOOTH, p->poly_smooth);
    uwp_warm_enable(GL_LINE_SMOOTH, p->line_smooth);
    uwp_warm_enable(GL_PROGRAM_POINT_SIZE, p->point_size);
    uwp_warm_enable(GL_POLYGON_OFFSET_FILL, p->offset_fill);
    uwp_warm_enable(GL_POLYGON_OFFSET_LINE, p->offset_line);
    uwp_warm_enable(GL_POLYGON_OFFSET_POINT, p->offset_point);
    glPolygonOffset(p->offset_factor, p->offset_units);
    glLineWidth(p->line_width > 0 ? p->line_width : 1);
    glStencilFunc(p->stencil_func, 0, p->stencil_mask);
    glStencilOp(p->stencil_fail, p->stencil_zfail, p->stencil_zpass);
    glStencilMask(p->stencil_write);
    stage_err = glGetError();
    if (stage_err != GL_NO_ERROR) {
        uwp_warm_why(p, "state", stage_err);
        return false;
    }
    glBindVertexArray(vao);
    for (int i = 0; i < 16; i++) {
        if (!p->attr[i].enabled) {
            glDisableVertexAttribArray(i);
            continue;
        }
        if (p->attr[i].integer) {
            glVertexAttribIPointer(i, p->attr[i].size, p->attr[i].type, 0, NULL);
        } else {
            glVertexAttribPointer(i, p->attr[i].size, p->attr[i].type, p->attr[i].normalized, 0,
                                  NULL);
        }
        glEnableVertexAttribArray(i);
    }
    stage_err = glGetError();
    if (stage_err != GL_NO_ERROR) {
        uwp_warm_why(p, "attributes", stage_err);
        return false;
    }
    /* all vertices are zeros: the triangles cover no pixels */
    glDrawArrays(p->prim, 0, 6);
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        uwp_warm_why(p, "draw", err);
        return false;
    }
    return true;
}

/* the list read at start, replayed by the warm-up thread */
static UwpWarmRecord *uwp_warm_list;
static int uwp_warm_list_len;

/*
 * What is in the list already (shader state + draw setup), so nothing goes in twice. A shader freed when
 * memory runs low (uwp_shader_cache_trim) comes back without its "listed" mark and was written again:
 * Ninja Gaiden's list was full (2048) with 171 repeats, and nothing new was learned after that.
 */
static GHashTable *uwp_warm_seen;
static uint64_t uwp_warm_body_hash(const UwpWarmRecord *rec)
{
    return fast_hash((const uint8_t *)&rec->state, sizeof(*rec) - offsetof(UwpWarmRecord, state));
}
static int uwp_warm_by_time(const void *a, const void *b)
{
    uint32_t x = ((const UwpWarmRecord *)a)->ms, y = ((const UwpWarmRecord *)b)->ms;
    return x < y ? -1 : x > y;
}
static bool uwp_warm_seen_add(const UwpWarmRecord *rec) /* false: it was there already */
{
    if (!uwp_warm_seen) {
        uwp_warm_seen = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
    }
    gint64 *key = g_new(gint64, 1);
    *key = (gint64)uwp_warm_body_hash(rec);
    if (g_hash_table_contains(uwp_warm_seen, key)) {
        g_free(key);
        return false;
    }
    g_hash_table_add(uwp_warm_seen, key);
    return true;
}
#define UWP_WARM_THREADS 3
extern GloContext *g_uwp_warm_context[UWP_WARM_THREADS]; /* renderer.c: one per warm-up thread */
static int uwp_warm_threads_left;
static char *uwp_warm_busy_path; /* exists while a warm-up runs */
static int uwp_warm_unfinished; /* warm-ups before this one that ended unfinished, in a row (in the busy file) */
static int uwp_warm_progress[UWP_WARM_THREADS]; /* next entry each thread will take */
static int uwp_warm_before; /* entries [0, uwp_warm_before) are needed before the first frame */

typedef struct UwpWarmJob {
    PGRAPHGLState *r;
    int index; /* this thread replays entries index, index + UWP_WARM_THREADS, ... */
} UwpWarmJob;

static void *uwp_warm_thread(void *arg)
{
    UwpWarmJob *job = arg;
    PGRAPHGLState *r = job->r;
    glo_set_current(g_uwp_warm_context[job->index]);

    GLuint fbo, vao, buf;
    glGenFramebuffers(1, &fbo);
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &buf);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, buf);
    void *zeros = g_malloc0(4096);
    glBufferData(GL_ARRAY_BUFFER, 4096, zeros, GL_STATIC_DRAW);
    g_free(zeros);

    int built = 0, drawn = 0, failed = 0;
    for (int i = job->index; i < uwp_warm_list_len; i += UWP_WARM_THREADS) {
        /*
         * Every shader made costs memory the game may need: a player's Munch's Oddysee list (1468 draws) took the
         * console's 5.1 GB in 25 s and xemu ended (2026-10-17). With less than 1.5 GB still allowed the warm-up
         * stops here; whatever is left gets made when the game draws it, as without a list.
         */
        uint64_t used, left;
        if ((i / UWP_WARM_THREADS) % 16 == 0 && xemu_uwp_memory(&used, &left) && left < (1536ull << 20)) {
            uwp_log("shader warm-up thread %d: stopped at entry %d of %d, memory nearly used (%llu MB in use, "
                    "%llu MB still allowed)", job->index, i, uwp_warm_list_len, (unsigned long long)(used >> 20),
                    (unsigned long long)(left >> 20));
            break;
        }
        qatomic_set(&uwp_warm_progress[job->index], i);
        UwpWarmRecord *rec = &uwp_warm_list[i];
        uint64_t hash = fast_hash((uint8_t *)&rec->state, sizeof(ShaderState));
        qemu_mutex_lock(&r->shader_cache_lock);
        LruNode *node = lru_lookup(&r->shader_cache, hash, &rec->state);
        ShaderBinding *binding = container_of(node, ShaderBinding, node);
        if (!binding->initialized) {
            generate_shaders(r, binding);
            built++;
        }
        GLuint program = binding->gl_program;
        qemu_mutex_unlock(&r->shader_cache_lock);
        glUseProgram(program);
        if (uwp_warm_replay(&rec->pipe, fbo, vao)) {
            drawn++;
        } else {
            failed++;
        }
        glFlush(); /* hand each draw to the driver now, so its compiles start in order */
    }
    glFinish();
    qatomic_set(&uwp_warm_progress[job->index], INT_MAX);
    glDeleteFramebuffers(1, &fbo);
    glDeleteVertexArrays(1, &vao);
    glDeleteBuffers(1, &buf);
    glo_set_current(NULL);
    uwp_log("shader warm-up thread %d: %d shaders built, %d draws replayed, %d skipped, "
            "done at %lld ms", job->index, built, drawn, failed,
            (long long)(qemu_clock_get_ms(QEMU_CLOCK_REALTIME) - uwp_warm_start_ms));
    if (qatomic_fetch_dec(&uwp_warm_threads_left) == 1) {
        qemu_unlink(uwp_warm_busy_path);
        g_free(uwp_warm_list);
        uwp_warm_list = NULL;
    }
    g_free(job);
    return NULL;
}

/* at renderer start: read this disc's list and replay it on a thread of its own */
static void uwp_warm_start(PGRAPHGLState *r)
{
    uwp_warm_start_ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    char *path = uwp_warm_path();
    gchar *data = NULL;
    gsize len = 0;
    uint32_t want[3];
    uwp_warm_header(want);
    /*
     * A warm-up that never finished (xemu ended in it). Once is usually the player quitting early or the
     * console closing xemu, and throwing the list away then cost the game all it had learned (every
     * shader compiled with a stall again). Twice in a row: the list itself may be what stops xemu, so
     * it is started anew. The busy file holds how many warm-ups before the last one ended unfinished.
     */
    char *busy = g_strdup_printf("%s.busy", path);
    if (g_file_test(busy, G_FILE_TEST_EXISTS)) {
        gchar *count = NULL;
        g_file_get_contents(busy, &count, NULL, NULL);
        uwp_warm_unfinished = (count ? atoi(count) : 0) + 1;
        g_free(count);
        if (uwp_warm_unfinished >= 2) {
            qemu_unlink(path);
            qemu_unlink(busy);
            uwp_warm_unfinished = 0;
            uwp_log("shader warm-up: the last two did not finish, starting a new list");
        } else {
            uwp_log("shader warm-up: the last one did not finish (xemu was closed during it); list kept");
        }
    }
    uwp_warm_busy_path = busy;
    if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
        /* none yet for this disc: start from the shared list for the game, if Game Vault put one there
         * (made on another console; the records are checked as for any list) */
        extern char *xemu_uwp_warm_seed(void); /* ui/xemu.c */
        char *seed = xemu_uwp_warm_seed();
        gchar *seed_data = NULL;
        gsize seed_len = 0;
        if (seed && g_file_get_contents(seed, &seed_data, &seed_len, NULL) && seed_len >= sizeof(want) &&
            seed_len <= 8 * 1024 * 1024 && g_file_set_contents(path, seed_data, seed_len, NULL)) {
            uwp_log("shader warm-up: starting from the shared list for this game (%u KB)",
                    (unsigned)(seed_len >> 10));
        }
        g_free(seed_data);
        g_free(seed);
    }
    if (!g_file_get_contents(path, &data, &len, NULL)) {
        uwp_log("shader warm-up: no list yet");
        g_free(path);
        return;
    }
    if (len >= sizeof(want) && uwp_warm_migrate(path, &data, &len, want)) {
        /* converted from the layout before the vertex attribute kinds (0.9.105) */
    }
    if (len < sizeof(want) || memcmp(data, want, sizeof(want))) {
        /* another xemu build: shader states may differ, start a new list */
        qemu_unlink(path);
        uwp_log("shader warm-up: list from another build, starting a new one");
        g_free(data);
        g_free(path);
        return;
    }
    int n = (len - sizeof(want)) / sizeof(UwpWarmRecord);
    uwp_warm_list = g_new(UwpWarmRecord, n > 0 ? n : 1);
    memcpy(uwp_warm_list, data + sizeof(want), n * sizeof(UwpWarmRecord));
    g_free(data);
    for (int i = 0; i < n; i++) {
        if (uwp_warm_list[i].check != uwp_warm_check(&uwp_warm_list[i])) {
            /* damaged from here on: keep the good part, start a new list */
            qemu_unlink(path);
            uwp_log("shader warm-up: damaged list after %d entries, starting a new one", i);
            n = i;
            break;
        }
    }
    g_free(path);
    /* the render scale is a uniform since 0.9.144 (shaders no longer differ by it): entries learned at another
       scale are the same shaders as at 1x, and the repeats this makes are dropped just below */
    for (int i = 0; i < n; i++) {
        if (uwp_warm_list[i].state.vsh.surface_scale_factor != 1) {
            uwp_warm_list[i].state.vsh.surface_scale_factor = 1;
            uwp_warm_list[i].check = uwp_warm_check(&uwp_warm_list[i]);
        }
        /* the specular power (0.9.237) and the point size and parameters (0.9.238) left the shader state
           (glsl/vsh.c): the same for older entries */
        VshState *v = &uwp_warm_list[i].state.vsh;
        static const float zero8[8];
        static const bool off4[4];
        if (v->specular_power != 0 || v->specular_power_back != 0 || v->point_size != 0 ||
            memcmp(v->point_params, zero8, sizeof(zero8)) ||
            memcmp(v->fixed_function.texture_matrix_enable, off4, sizeof(off4)) ||
            memcmp(v->fixed_function.texgen, (const enum VshTexgen[4][4]){ 0 }, sizeof(v->fixed_function.texgen))) {
            v->specular_power = 0;
            v->specular_power_back = 0;
            v->point_size = 0;
            memset(v->point_params, 0, sizeof(v->point_params));
            memset(v->fixed_function.texture_matrix_enable, 0, sizeof(off4));   /* a uniform since 0.9.247 */
            memset(v->fixed_function.texgen, 0, sizeof(v->fixed_function.texgen));   /* a uniform since 0.9.247 */
            uwp_warm_list[i].check = uwp_warm_check(&uwp_warm_list[i]);
        }
    }
    /* repeats written by earlier builds are dropped, and the list is written back without them, so
       their room goes to shaders not learned yet */
    int kept = 0;
    for (int i = 0; i < n; i++) {
        if (uwp_warm_seen_add(&uwp_warm_list[i])) {
            if (kept != i) {
                uwp_warm_list[kept] = uwp_warm_list[i];
            }
            kept++;
        }
    }
    if (kept < n) {
        char *wpath = uwp_warm_path();
        gsize wlen = sizeof(want) + kept * sizeof(UwpWarmRecord);
        char *out = g_malloc(wlen);
        memcpy(out, want, sizeof(want));
        memcpy(out + sizeof(want), uwp_warm_list, kept * sizeof(UwpWarmRecord));
        g_file_set_contents(wpath, out, wlen, NULL);
        g_free(out);
        g_free(wpath);
        uwp_log("shader warm-up: %d repeats taken out of the list (%d left)", n - kept, kept);
        n = kept;
    }
    /* in the order the game first needed them: each run adds its new shaders at the end, so one needed in
       the first seconds but learned on a later run would otherwise wait behind all the others */
    qsort(uwp_warm_list, n, sizeof(UwpWarmRecord), uwp_warm_by_time);
    uwp_warm_list_len = n;
    uwp_warm_count = n;
    uwp_warm_before = 0;
    while (uwp_warm_before < n && uwp_warm_list[uwp_warm_before].ms < UWP_WARM_BEFORE_MS) {
        uwp_warm_before++;
    }
    for (int t = 0; t < UWP_WARM_THREADS; t++) {
        uwp_warm_progress[t] = t;
    }

    /* mark them as listed, so the game's own first draws do not list them again */
    qemu_mutex_lock(&r->shader_cache_lock);
    for (int i = 0; i < n; i++) {
        uint64_t hash = fast_hash((uint8_t *)&uwp_warm_list[i].state, sizeof(ShaderState));
        LruNode *node = lru_lookup(&r->shader_cache, hash, &uwp_warm_list[i].state);
        container_of(node, ShaderBinding, node)->uwp_warm_noted = true;
    }
    qemu_mutex_unlock(&r->shader_cache_lock);

    if (n > 0 && g_uwp_warm_context[0]) {
        char count[16];
        snprintf(count, sizeof(count), "%d", uwp_warm_unfinished);
        g_file_set_contents(uwp_warm_busy_path, count, -1, NULL);
        /* the driver compiles in parallel: several threads finish before the logo needs them */
        uwp_warm_threads_left = UWP_WARM_THREADS;
        for (int t = 0; t < UWP_WARM_THREADS; t++) {
            UwpWarmJob *job = g_new(UwpWarmJob, 1);
            job->r = r;
            job->index = t;
            QemuThread thread;
            qemu_thread_create(&thread, "uwp.shader_warm", uwp_warm_thread, job,
                               QEMU_THREAD_DETACHED);
        }
        uwp_log("shader warm-up: replaying %d draws on %d threads (%d before the first frame)", n,
                UWP_WARM_THREADS, uwp_warm_before);
    }
}

/*
 * At the game's first draw: wait for the warm-up threads (at most 8 s), so the logo animation
 * starts once its shaders are compiled instead of freezing on its first frames. The screen
 * stays black a little longer; the time to the game stays about the same.
 */
static bool uwp_warm_waited;

static void uwp_warm_wait(void)
{
    uwp_warm_waited = true;
    int64_t t0 = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    if (!qatomic_read(&uwp_warm_threads_left)) {
        return;
    }
    for (;;) {
        /* done when every thread has moved past the early entries (or finished) */
        bool early_done = true;
        for (int t = 0; t < UWP_WARM_THREADS; t++) {
            if (qatomic_read(&uwp_warm_progress[t]) < uwp_warm_before) {
                early_done = false;
            }
        }
        if (early_done || !qatomic_read(&uwp_warm_threads_left) ||
            qemu_clock_get_ms(QEMU_CLOCK_REALTIME) - t0 >= 8000) {
            break;
        }
        g_usleep(2000);
    }
    uwp_log("shader warm-up: first draw waited %lld ms for it",
            (long long)(qemu_clock_get_ms(QEMU_CLOCK_REALTIME) - t0));
}

/* called just before each game draw: note the first draw of a shader for the next start */
void uwp_warm_draw(PGRAPHGLState *r)
{
    ShaderBinding *b = r->shader_binding;
    if (!b || b->uwp_warm_noted) {
        return;
    }
    b->uwp_warm_noted = true;
    if (uwp_warm_start_ms < 0 || uwp_warm_count >= UWP_WARM_MAX) {
        return;
    }

    UwpWarmRecord *rec = g_malloc0(sizeof(UwpWarmRecord));
    UwpWarmPipe *p = &rec->pipe;
    memcpy(&rec->state, &b->state, sizeof(ShaderState));
    rec->ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME) - uwp_warm_start_ms;
    p->prim = b->gl_primitive_mode;

    GLint v, type;
    glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                          GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
    if (type == GL_TEXTURE) {
        glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                              GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &v);
        glGetTextureLevelParameteriv(v, 0, GL_TEXTURE_INTERNAL_FORMAT, &v);
        p->color_fmt = v;
    }
    glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                          GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
    if (type == GL_TEXTURE) {
        glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                              GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &v);
        glGetTextureLevelParameteriv(v, 0, GL_TEXTURE_INTERNAL_FORMAT, &v);
        p->depth_fmt = v;
    }
    p->blend = glIsEnabled(GL_BLEND);
    glGetIntegerv(GL_BLEND_SRC_RGB, &v);
    p->blend_src_rgb = v;
    glGetIntegerv(GL_BLEND_DST_RGB, &v);
    p->blend_dst_rgb = v;
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &v);
    p->blend_src_a = v;
    glGetIntegerv(GL_BLEND_DST_ALPHA, &v);
    p->blend_dst_a = v;
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &v);
    p->blend_eq_rgb = v;
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &v);
    p->blend_eq_a = v;
    p->depth_test = glIsEnabled(GL_DEPTH_TEST);
    glGetIntegerv(GL_DEPTH_FUNC, &v);
    p->depth_func = v;
    GLboolean bv[4];
    glGetBooleanv(GL_DEPTH_WRITEMASK, bv);
    p->depth_mask = bv[0];
    glGetBooleanv(GL_COLOR_WRITEMASK, bv);
    p->color_mask = (bv[0] ? 1 : 0) | (bv[1] ? 2 : 0) | (bv[2] ? 4 : 0) | (bv[3] ? 8 : 0);
    p->stencil = glIsEnabled(GL_STENCIL_TEST);
    p->cull = glIsEnabled(GL_CULL_FACE);
    glGetIntegerv(GL_FRONT_FACE, &v);
    p->front_face = v;
    glGetIntegerv(GL_CULL_FACE_MODE, &v);
    p->cull_mode = v;
    glGetIntegerv(GL_PROVOKING_VERTEX, &v);
    p->provoking = v;
    p->depth_clamp = glIsEnabled(GL_DEPTH_CLAMP);
    p->poly_smooth = glIsEnabled(GL_POLYGON_SMOOTH);
    p->line_smooth = glIsEnabled(GL_LINE_SMOOTH);
    p->point_size = glIsEnabled(GL_PROGRAM_POINT_SIZE);
    p->offset_fill = glIsEnabled(GL_POLYGON_OFFSET_FILL);
    p->offset_line = glIsEnabled(GL_POLYGON_OFFSET_LINE);
    p->offset_point = glIsEnabled(GL_POLYGON_OFFSET_POINT);
    glGetFloatv(GL_LINE_WIDTH, &p->line_width);
    glGetFloatv(GL_POLYGON_OFFSET_FACTOR, &p->offset_factor);
    glGetFloatv(GL_POLYGON_OFFSET_UNITS, &p->offset_units);
    glGetIntegerv(GL_STENCIL_FUNC, &v);
    p->stencil_func = v;
    glGetIntegerv(GL_STENCIL_VALUE_MASK, &v);
    p->stencil_mask = v;
    glGetIntegerv(GL_STENCIL_FAIL, &v);
    p->stencil_fail = v;
    glGetIntegerv(GL_STENCIL_PASS_DEPTH_FAIL, &v);
    p->stencil_zfail = v;
    glGetIntegerv(GL_STENCIL_PASS_DEPTH_PASS, &v);
    p->stencil_zpass = v;
    glGetIntegerv(GL_STENCIL_WRITEMASK, &v);
    p->stencil_write = v;
    for (int i = 0; i < 16; i++) {
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &v);
        if (!v) {
            continue;
        }
        p->attr[i].enabled = 1;
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_SIZE, &v);
        p->attr[i].size = v;
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_TYPE, &v);
        p->attr[i].type = v;
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &v);
        p->attr[i].normalized = v;
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_INTEGER, &v);
        p->attr[i].integer = v;
    }
    for (int u = 0; u < UWP_WARM_UNITS; u++) {
        TextureBinding *t = r->texture_binding[u];
        if (t && t->gl_texture) {
            glGetTextureLevelParameteriv(t->gl_texture, 0, GL_TEXTURE_INTERNAL_FORMAT, &v);
            p->tex[u].target = t->gl_target;
            p->tex[u].fmt = v;
        }
    }
    while (glGetError() != GL_NO_ERROR) {
    }
    rec->check = uwp_warm_check(rec);
    if (!uwp_warm_seen_add(rec)) {
        g_free(rec);
        return;
    }

    char *path = uwp_warm_path();
    FILE *f = qemu_fopen(path, "ab");
    g_free(path);
    if (f) {
        /* in append mode the position reads 0 until the first write: go to the end first */
        fseek(f, 0, SEEK_END);
        if (ftell(f) == 0) {
            uint32_t head[3];
            uwp_warm_header(head);
            fwrite(head, sizeof(head), 1, f);
        }
        if (fwrite(rec, sizeof(*rec), 1, f) == 1) {
            uwp_warm_count++;
        }
        fclose(f);
    }
    g_free(rec);
}
#endif

#ifdef XEMU_UWP
/*
 * Measuring aid (test file variant-log.txt): which parts of the shader state make new shaders. Each shader the
 * game itself has to build is compared with the most alike of the last UWP_VAR_KEEP built, and the fields that
 * differ are counted; every 30 s the log gets the most frequent ones. Off without the file (costs memory).
 */
#define UWP_VAR_KEEP 1024
typedef struct UwpVarField {
    const char *name;
    size_t off, len;
    unsigned count;
} UwpVarField;
#define UWP_VF(f) { #f, offsetof(ShaderState, f), sizeof(((ShaderState *)0)->f), 0 }
static UwpVarField uwp_var_fields[] = {
    UWP_VF(vsh.surface_scale_factor), UWP_VF(vsh.compressed_attrs), UWP_VF(vsh.uniform_attrs),
    UWP_VF(vsh.swizzle_attrs), UWP_VF(vsh.fog_enable), UWP_VF(vsh.fog_mode), UWP_VF(vsh.specular_enable),
    UWP_VF(vsh.separate_specular), UWP_VF(vsh.ignore_specular_alpha), UWP_VF(vsh.specular_power),
    UWP_VF(vsh.specular_power_back), UWP_VF(vsh.point_params_enable), UWP_VF(vsh.point_size),
    UWP_VF(vsh.point_params), UWP_VF(vsh.smooth_shading), UWP_VF(vsh.z_perspective),
    UWP_VF(vsh.is_fixed_function), UWP_VF(vsh.fixed_function.normalization),
    UWP_VF(vsh.fixed_function.texture_matrix_enable), UWP_VF(vsh.fixed_function.texgen),
    UWP_VF(vsh.fixed_function.foggen), UWP_VF(vsh.fixed_function.skinning), UWP_VF(vsh.fixed_function.lighting),
    UWP_VF(vsh.fixed_function.light), UWP_VF(vsh.fixed_function.emission_src),
    UWP_VF(vsh.fixed_function.ambient_src), UWP_VF(vsh.fixed_function.diffuse_src),
    UWP_VF(vsh.fixed_function.specular_src), UWP_VF(vsh.fixed_function.local_eye),
    UWP_VF(vsh.programmable.program_data), UWP_VF(vsh.programmable.program_length), UWP_VF(vsh.uwp_attr_kinds),
    UWP_VF(geom), UWP_VF(psh.combiner_control), UWP_VF(psh.shader_stage_program), UWP_VF(psh.other_stage_input),
    UWP_VF(psh.final_inputs_0), UWP_VF(psh.final_inputs_1), UWP_VF(psh.rgb_inputs), UWP_VF(psh.rgb_outputs),
    UWP_VF(psh.alpha_inputs), UWP_VF(psh.alpha_outputs), UWP_VF(psh.point_sprite), UWP_VF(psh.rect_tex),
    UWP_VF(psh.snorm_tex), UWP_VF(psh.compare_mode), UWP_VF(psh.alphakill), UWP_VF(psh.colorkey_mode),
    UWP_VF(psh.conv_tex), UWP_VF(psh.tex_x8y24), UWP_VF(psh.dim_tex), UWP_VF(psh.tex_cubemap),
    UWP_VF(psh.border_logical_size), UWP_VF(psh.border_inv_real_size), UWP_VF(psh.shadow_map),
    UWP_VF(psh.shadow_depth_func), UWP_VF(psh.alpha_test), UWP_VF(psh.alpha_func),
    UWP_VF(psh.window_clip_exclusive), UWP_VF(psh.smooth_shading), UWP_VF(psh.depth_clipping),
    UWP_VF(psh.z_perspective), UWP_VF(psh.surface_zeta_format), UWP_VF(psh.depth_format),
};
#define UWP_VAR_NFIELDS (sizeof(uwp_var_fields) / sizeof(uwp_var_fields[0]))

static void uwp_variant_note(const ShaderState *st)
{
    static int on = -1;
    static ShaderState *keep;
    static int kept, next;
    static unsigned total, only_one;
    static int64_t last_log;
    if (on < 0) {
        extern bool xemu_uwp_test_file_exists(const char *name); /* ui/xemu.c */
        on = xemu_uwp_test_file_exists("variant-log.txt");
        if (on) {
            keep = g_new0(ShaderState, UWP_VAR_KEEP);
            uwp_log("shader variants: measuring which state makes new shaders (variant-log.txt)");
        }
    }
    if (!on) {
        return;
    }
    /* the most alike: fewest differing fields */
    int best = -1, best_n = INT_MAX;
    bool best_diff[UWP_VAR_NFIELDS];
    for (int k = 0; k < kept; k++) {
        bool diff[UWP_VAR_NFIELDS];
        int n = 0;
        for (size_t f = 0; f < UWP_VAR_NFIELDS && n < best_n; f++) {
            UwpVarField *v = &uwp_var_fields[f];
            diff[f] = memcmp((const uint8_t *)st + v->off, (const uint8_t *)&keep[k] + v->off, v->len) != 0;
            n += diff[f];
        }
        if (n < best_n) {
            best_n = n;
            best = k;
            memcpy(best_diff, diff, sizeof(diff));
        }
    }
    if (best >= 0) {
        total++;
        only_one += best_n == 1;
        for (size_t f = 0; f < UWP_VAR_NFIELDS; f++) {
            uwp_var_fields[f].count += best_diff[f];
        }
    }
    keep[next] = *st;
    next = (next + 1) % UWP_VAR_KEEP;
    kept = MIN(kept + 1, UWP_VAR_KEEP);

    int64_t now = g_get_monotonic_time();
    if (total && now - last_log > 30 * G_USEC_PER_SEC) {
        last_log = now;
        GString *g = g_string_new(NULL);
        bool listed[UWP_VAR_NFIELDS] = { 0 };
        for (int shown = 0; shown < 12; shown++) {
            int top = -1;
            for (size_t f = 0; f < UWP_VAR_NFIELDS; f++) {
                if (uwp_var_fields[f].count && (top < 0 || uwp_var_fields[f].count > uwp_var_fields[top].count) &&
                    !listed[f]) {
                    top = f;
                }
            }
            if (top < 0) {
                break;
            }
            listed[top] = true;
            g_string_append_printf(g, " %s %u,", uwp_var_fields[top].name, uwp_var_fields[top].count);
        }
        uwp_log("shader variants: %u new, %u differ in one field only; fields that differed:%s", total, only_one,
                g->str);
        g_string_free(g, true);
    }
}
#endif

void pgraph_gl_bind_shaders(PGRAPHState *pg)
{
    PGRAPHGLState *r = pg->gl_renderer_state;

#ifdef XEMU_UWP
    if (!uwp_warm_waited) {
        uwp_warm_wait();
    }
#endif

    bool binding_changed = false;
    if (r->shader_binding &&
        !pgraph_glsl_check_shader_state_dirty(pg, &r->shader_binding->state)) {
        nv2a_profile_inc_counter(NV2A_PROF_SHADER_BIND_NOTDIRTY);
        goto update_uniforms;
    }

    ShaderBinding *old_binding = r->shader_binding;
    ShaderState state = pgraph_glsl_get_shader_state(pg);

    NV2A_GL_DGROUP_BEGIN("%s (%s)", __func__,
                         state.vsh.is_fixed_function ? "FF" : "PROG");

    qemu_mutex_lock(&r->shader_cache_lock);

    uint64_t shader_state_hash =
        fast_hash((uint8_t *)&state, sizeof(ShaderState));

    LruNode *node = lru_lookup(&r->shader_cache, shader_state_hash, &state);
    ShaderBinding *binding = container_of(node, ShaderBinding, node);

    if (!binding->initialized && !pgraph_gl_shader_load_from_memory(binding)) {
        nv2a_profile_inc_counter(NV2A_PROF_SHADER_GEN);
#ifdef XEMU_UWP
        uwp_variant_note(&state);
#endif
        UWP_PROF_BEGIN(t0);
        generate_shaders(r, binding);
        UWP_PROF_END(PF_SHADER_GEN, t0);
        if (g_config.perf.cache_shaders) {
            pgraph_gl_shader_cache_to_disk(binding);
        }
    }
    assert(binding->initialized);
    r->shader_binding = binding;
    pg->program_data_dirty = false;
#ifdef XEMU_UWP
    uwp_shader_cache_trim(r);
#endif

    qemu_mutex_unlock(&r->shader_cache_lock);

    binding_changed = (r->shader_binding != old_binding);
    if (binding_changed) {
        nv2a_profile_inc_counter(NV2A_PROF_SHADER_BIND);
        glUseProgram(r->shader_binding->gl_program);
    }

    // FIXME: Clear only the registers that are consumed by this binding.
    pgraph_clear_dirty_reg_map(pg);

    NV2A_GL_DGROUP_END();

update_uniforms:
    assert(r->shader_binding);
    assert(r->shader_binding->initialized);
    update_shader_uniforms(pg, r->shader_binding);
}

GLuint pgraph_gl_compile_shader(const char *vs_src, const char *fs_src)
{
    GLint status;
    char err_buf[512];

    // Compile vertex shader
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &vs_src, NULL);
    glCompileShader(vs);
    glGetShaderiv(vs, GL_COMPILE_STATUS, &status);
    if (status != GL_TRUE) {
        glGetShaderInfoLog(vs, sizeof(err_buf), NULL, err_buf);
        err_buf[sizeof(err_buf)-1] = '\0';
        fprintf(stderr, "Vertex shader compilation failed: %s\n", err_buf);
        exit(1);
    }

    // Compile fragment shader
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &fs_src, NULL);
    glCompileShader(fs);
    glGetShaderiv(fs, GL_COMPILE_STATUS, &status);
    if (status != GL_TRUE) {
        glGetShaderInfoLog(fs, sizeof(err_buf), NULL, err_buf);
        err_buf[sizeof(err_buf)-1] = '\0';
        fprintf(stderr, "Fragment shader compilation failed: %s\n", err_buf);
        exit(1);
    }

    // Link vertex and fragment shaders
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glUseProgram(prog);

    // Flag shaders for deletion (will still be retained for lifetime of prog)
    glDeleteShader(vs);
    glDeleteShader(fs);

    return prog;
}
