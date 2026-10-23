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
#include "qemu/fast-hash.h"
#include "qapi/error.h"
#include "hw/display/vga_int.h"
#include "hw/xbox/nv2a/nv2a_int.h"
#include "hw/xbox/nv2a/pgraph/util.h"
#include "ui/xemu-settings.h"
#include "ui/xemu-notifications.h"
#include <glib/gstdio.h>
#include "renderer.h"
#include "system/cpus.h"
#include "cpu-stats-compat.h"

#include <math.h>

#ifdef CONFIG_OPENGL
#include <epoxy/gl.h>
#endif

static GPUProperties d3d12_gpu_properties;

#ifdef CONFIG_OPENGL
#define D3D12_DISPLAY_SOURCE_DUMPS 32
#define D3D12_DISPLAY_GL_READBACK_DUMPS 32
#endif

static bool d3d12_env_flag(const char *name)
{
    const char *value = getenv(name);

    if (value == NULL || value[0] == '\0' || !g_ascii_strcasecmp(value, "0") ||
        !g_ascii_strcasecmp(value, "false") ||
        !g_ascii_strcasecmp(value, "off") ||
        !g_ascii_strcasecmp(value, "no")) {
        return false;
    }

    return true;
}

static void pgraph_d3d12_init_debug_flags(PGRAPHState *pg)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;

    r->debug_verbose_reports = d3d12_env_flag("XEMU_D3D12_VERBOSE");
    r->debug_resource_dumps = d3d12_env_flag("XEMU_D3D12_DUMPS");
    r->debug_verbose_reports |= r->debug_resource_dumps;
}

bool pgraph_d3d12_verbose_reports(PGRAPHState *pg)
{
    PGRAPHD3D12State *r = pg ? pg->d3d12_renderer_state : NULL;

    return r != NULL && r->debug_verbose_reports;
}

bool pgraph_d3d12_resource_dumps(PGRAPHState *pg)
{
    PGRAPHD3D12State *r = pg ? pg->d3d12_renderer_state : NULL;

    return r != NULL && r->debug_resource_dumps;
}

static const char *display_color_format_name(uint32_t format)
{
    switch (format) {
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_Z1R5G5B5:
        return "LE_X1R5G5B5_Z1R5G5B5";
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5:
        return "LE_R5G6B5";
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_Z8R8G8B8:
        return "LE_X8R8G8B8_Z8R8G8B8";
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1A7R8G8B8_Z1A7R8G8B8:
        return "LE_X1A7R8G8B8_Z1A7R8G8B8";
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8:
        return "LE_A8R8G8B8";
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_B8:
        return "LE_B8";
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_G8B8:
        return "LE_G8B8";
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_O1R5G5B5:
        return "LE_X1R5G5B5_O1R5G5B5";
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_O8R8G8B8:
        return "LE_X8R8G8B8_O8R8G8B8";
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1A7R8G8B8_O1A7R8G8B8:
        return "LE_X1A7R8G8B8_O1A7R8G8B8";
    default:
        return "unknown";
    }
}

#ifdef CONFIG_OPENGL
static void d3d12_store_le16(uint8_t *p, uint16_t v)
{
    p[0] = v & 0xff;
    p[1] = v >> 8;
}

static void d3d12_store_le32(uint8_t *p, uint32_t v)
{
    p[0] = v & 0xff;
    p[1] = (v >> 8) & 0xff;
    p[2] = (v >> 16) & 0xff;
    p[3] = v >> 24;
}

static uint8_t scale_bits_to_u8(uint32_t value, uint32_t max_value)
{
    return (uint8_t)((value * 255u + max_value / 2u) / max_value);
}

static void display_surface_rgb(uint32_t color_format, const uint8_t *pixel,
                                uint8_t *r, uint8_t *g, uint8_t *b)
{
    uint16_t value16;

    switch (color_format) {
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_Z1R5G5B5:
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_O1R5G5B5:
        value16 = pixel[0] | ((uint16_t)pixel[1] << 8);
        *r = scale_bits_to_u8((value16 >> 10) & 0x1f, 0x1f);
        *g = scale_bits_to_u8((value16 >> 5) & 0x1f, 0x1f);
        *b = scale_bits_to_u8(value16 & 0x1f, 0x1f);
        break;
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5:
        value16 = pixel[0] | ((uint16_t)pixel[1] << 8);
        *r = scale_bits_to_u8((value16 >> 11) & 0x1f, 0x1f);
        *g = scale_bits_to_u8((value16 >> 5) & 0x3f, 0x3f);
        *b = scale_bits_to_u8(value16 & 0x1f, 0x1f);
        break;
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_Z8R8G8B8:
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_O8R8G8B8:
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8:
        *r = pixel[2];
        *g = pixel[1];
        *b = pixel[0];
        break;
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_B8:
        *r = pixel[0];
        *g = pixel[0];
        *b = pixel[0];
        break;
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_G8B8:
        *r = pixel[1];
        *g = pixel[0];
        *b = pixel[1];
        break;
    default:
        *r = pixel[0];
        *g = pixel[0];
        *b = pixel[0];
        break;
    }
}

static bool write_display_source_bmp(const char *path, uint32_t color_format,
                                     const uint8_t *source,
                                     uint32_t width, uint32_t height,
                                     uint32_t row_pitch,
                                     uint32_t bytes_per_pixel)
{
    uint32_t bmp_row_pitch = ROUND_UP(width * 3, 4);
    uint32_t image_size = bmp_row_pitch * height;
    uint8_t header[54];
    g_autofree uint8_t *row = g_malloc0(bmp_row_pitch);
    FILE *fp;

    memset(header, 0, sizeof(header));
    header[0] = 'B';
    header[1] = 'M';
    d3d12_store_le32(&header[2], sizeof(header) + image_size);
    d3d12_store_le32(&header[10], sizeof(header));
    d3d12_store_le32(&header[14], 40);
    d3d12_store_le32(&header[18], width);
    d3d12_store_le32(&header[22], (uint32_t)(-(int32_t)height));
    d3d12_store_le16(&header[26], 1);
    d3d12_store_le16(&header[28], 24);
    d3d12_store_le32(&header[34], image_size);

    fp = fopen(path, "wb");
    if (fp == NULL) {
        return false;
    }
    if (fwrite(header, sizeof(header), 1, fp) != 1) {
        fclose(fp);
        return false;
    }

    for (uint32_t y = 0; y < height; y++) {
        const uint8_t *src = source + (uint64_t)y * row_pitch;

        memset(row, 0, bmp_row_pitch);
        for (uint32_t x = 0; x < width; x++) {
            uint8_t r, g, b;

            display_surface_rgb(color_format,
                                src + (uint64_t)x * bytes_per_pixel,
                                &r, &g, &b);
            row[x * 3 + 0] = b;
            row[x * 3 + 1] = g;
            row[x * 3 + 2] = r;
        }
        if (fwrite(row, bmp_row_pitch, 1, fp) != 1) {
            fclose(fp);
            return false;
        }
    }

    fclose(fp);
    return true;
}

static bool display_source_signature_seen(uint64_t signature,
                                          uint32_t *slot)
{
    static uint64_t signatures[D3D12_DISPLAY_SOURCE_DUMPS];
    static uint32_t num_signatures;

    for (uint32_t i = 0; i < num_signatures; i++) {
        if (signatures[i] == signature) {
            return true;
        }
    }

    if (num_signatures < ARRAY_SIZE(signatures)) {
        *slot = num_signatures;
        signatures[num_signatures++] = signature;
        return false;
    }

    return true;
}

static bool display_gl_readback_signature_seen(uint64_t signature,
                                               uint32_t *slot)
{
    static uint64_t signatures[D3D12_DISPLAY_GL_READBACK_DUMPS];
    static uint32_t num_signatures;

    for (uint32_t i = 0; i < num_signatures; i++) {
        if (signatures[i] == signature) {
            return true;
        }
    }

    if (num_signatures < ARRAY_SIZE(signatures)) {
        *slot = num_signatures;
        signatures[num_signatures++] = signature;
        return false;
    }

    return true;
}

static uint32_t display_gl_label_hash(const char *label)
{
    uint32_t h = 2166136261u;

    while (*label) {
        h ^= (uint8_t)*label++;
        h *= 16777619u;
    }

    return h;
}

static void report_display_source_sample(NV2AState *d, const uint8_t *source,
                                         uint32_t width, uint32_t height,
                                         uint32_t row_pitch,
                                         uint32_t bytes_per_pixel,
                                         uint32_t visible_width)
{
    PGRAPHD3D12State *r = d->pgraph.d3d12_renderer_state;
    uint64_t total_nonzero_rgb = 0;
    uint64_t total_rgb_sum = 0;
    uint64_t fnv = 1469598103934665603ull;
    int first_nonzero_row = -1;
    int last_nonzero_row = -1;
    uint32_t sampled_rows = 0;
    uint32_t slot = 0;
    uint64_t signature;
    g_autoptr(GString) rows = g_string_new(NULL);
    g_autofree char *dump_dir = NULL;
    g_autofree char *dump_name = NULL;
    g_autofree char *dump_path = NULL;
    const char *local_app_data;
    bool wrote_dump = false;

    if (!pgraph_d3d12_resource_dumps(&d->pgraph) ||
        r == NULL || width == 0 || height == 0 ||
        bytes_per_pixel == 0 || bytes_per_pixel > 4) {
        return;
    }

    for (uint32_t y = 0; y < height; y++) {
        const uint8_t *row = source + (uint64_t)y * row_pitch;
        uint64_t row_nonzero_rgb = 0;
        uint64_t row_rgb_sum = 0;
        int first_nonzero = -1;
        int last_nonzero = -1;

        for (uint32_t x = 0; x < width; x++) {
            uint8_t rr, gg, bb;

            display_surface_rgb(r->display_surface_color_format,
                                row + (uint64_t)x * bytes_per_pixel,
                                &rr, &gg, &bb);
            fnv ^= rr;
            fnv *= 1099511628211ull;
            fnv ^= gg;
            fnv *= 1099511628211ull;
            fnv ^= bb;
            fnv *= 1099511628211ull;
            row_rgb_sum += rr + gg + bb;
            if (rr || gg || bb) {
                row_nonzero_rgb++;
                if (first_nonzero < 0) {
                    first_nonzero = x;
                }
                last_nonzero = x;
            }
        }

        total_nonzero_rgb += row_nonzero_rgb;
        total_rgb_sum += row_rgb_sum;
        if (row_nonzero_rgb) {
            if (first_nonzero_row < 0) {
                first_nonzero_row = y;
            }
            last_nonzero_row = y;
            if (sampled_rows < 24) {
                g_string_append_printf(
                    rows,
                    "%s{y=%u nonzero_rgb=%" PRIu64
                    " first=%d last=%d rgb_sum=%" PRIu64 "}",
                    sampled_rows ? "," : "", y, row_nonzero_rgb,
                    first_nonzero, last_nonzero, row_rgb_sum);
                sampled_rows++;
            }
        }
    }

    signature = fnv ^ ((uint64_t)r->display_surface_addr << 1) ^
                ((uint64_t)r->display_surface_color_format << 48) ^
                ((uint64_t)width << 32) ^ ((uint64_t)height << 16);
    if (display_source_signature_seen(signature, &slot)) {
        return;
    }

    local_app_data = getenv("LOCALAPPDATA");
    if (local_app_data != NULL && local_app_data[0] != '\0') {
        dump_dir = g_build_filename(local_app_data, "xemu-d3d12-test",
                                    "captures", NULL);
        if (g_mkdir_with_parents(dump_dir, 0755) == 0) {
            dump_name = g_strdup_printf(
                "d3d12-display-source-%02u-%08" HWADDR_PRIx
                "-%ux%u-%016" PRIx64 ".bmp",
                slot, r->display_surface_addr, width, height, fnv);
            dump_path = g_build_filename(dump_dir, dump_name, NULL);
            wrote_dump = write_display_source_bmp(
                dump_path, r->display_surface_color_format, source, width,
                height, row_pitch, bytes_per_pixel);
        }
    }

    fprintf(stderr,
            "Direct3D 12 renderer: display source sample (%u/%u): "
            "addr=0x%" HWADDR_PRIx ", size=%ux%u, pitch=%u, "
            "row_bytes=%u, bytes_per_pixel=%u, color_format=0x%x, "
            "visible_width=%u, pcrtc.start=0x%" HWADDR_PRIx
            ", nonzero_rgb=%" PRIu64 ", rgb_sum=%" PRIu64
            ", first_nonzero_row=%d, last_nonzero_row=%d, "
            "rgb_fnv=0x%016" PRIx64 ", rows=[%s], dump=%s%s%s\n",
            slot + 1, D3D12_DISPLAY_SOURCE_DUMPS,
            r->display_surface_addr, width, height, r->display_surface_pitch,
            row_pitch, bytes_per_pixel, r->display_surface_color_format,
            visible_width, d->pcrtc.start,
            total_nonzero_rgb, total_rgb_sum, first_nonzero_row,
            last_nonzero_row, fnv, rows->str,
            wrote_dump ? dump_path : "not-written",
            !wrote_dump && dump_path != NULL ? " path=" : "",
            !wrote_dump && dump_path != NULL ? dump_path : "");
}

#ifdef CONFIG_OPENGL
static void report_display_gl_texture_sample(NV2AState *d, const char *label,
                                             GLuint texture,
                                             uint32_t width, uint32_t height)
{
    PGRAPHD3D12State *r = d->pgraph.d3d12_renderer_state;
    const uint32_t bytes_per_pixel = 4;
    const uint32_t row_pitch = width * bytes_per_pixel;
    uint64_t total_nonzero_rgb = 0;
    uint64_t total_rgb_sum = 0;
    uint64_t fnv = 1469598103934665603ull;
    uint32_t sampled_rows = 0;
    uint32_t slot = 0;
    uint64_t signature;
    GLint prev_texture = 0;
    GLint prev_pack_alignment = 0;
    GLenum error;
    g_autoptr(GString) rows = g_string_new(NULL);
    g_autofree uint8_t *pixels = NULL;
    g_autofree char *dump_dir = NULL;
    g_autofree char *dump_name = NULL;
    g_autofree char *dump_path = NULL;
    const char *local_app_data;
    bool wrote_dump = false;

    if (!pgraph_d3d12_resource_dumps(&d->pgraph) ||
        r == NULL || texture == 0 || width == 0 || height == 0) {
        return;
    }

    pixels = g_malloc((uint64_t)row_pitch * height);

    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_texture);
    glGetIntegerv(GL_PACK_ALIGNMENT, &prev_pack_alignment);
    glBindTexture(GL_TEXTURE_2D, texture);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV,
                  pixels);
    glPixelStorei(GL_PACK_ALIGNMENT, prev_pack_alignment);
    glBindTexture(GL_TEXTURE_2D, prev_texture);

    error = glGetError();
    if (error != GL_NO_ERROR) {
        fprintf(stderr,
                "Direct3D 12 renderer: display GL texture readback failed: "
                "stage=%s, texture=%u, width=%u, height=%u, "
                "read_format=GL_BGRA, read_type=GL_UNSIGNED_INT_8_8_8_8_REV, "
                "gl_error=0x%x\n",
                label, texture, width, height, error);
        return;
    }

    for (uint32_t y = 0; y < height; y++) {
        const uint8_t *row = pixels + (uint64_t)y * row_pitch;
        uint64_t row_nonzero_rgb = 0;
        uint64_t row_rgb_sum = 0;
        int first_nonzero = -1;
        int last_nonzero = -1;

        for (uint32_t x = 0; x < width; x++) {
            uint8_t rr, gg, bb;

            display_surface_rgb(NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8,
                                row + (uint64_t)x * bytes_per_pixel,
                                &rr, &gg, &bb);
            fnv ^= rr;
            fnv *= 1099511628211ull;
            fnv ^= gg;
            fnv *= 1099511628211ull;
            fnv ^= bb;
            fnv *= 1099511628211ull;
            row_rgb_sum += rr + gg + bb;
            if (rr || gg || bb) {
                row_nonzero_rgb++;
                if (first_nonzero < 0) {
                    first_nonzero = x;
                }
                last_nonzero = x;
            }
        }

        total_nonzero_rgb += row_nonzero_rgb;
        total_rgb_sum += row_rgb_sum;
        if (row_nonzero_rgb && sampled_rows < 24) {
            g_string_append_printf(
                rows,
                "%s{y=%u nonzero_rgb=%" PRIu64
                " first=%d last=%d rgb_sum=%" PRIu64 "}",
                sampled_rows ? "," : "", y, row_nonzero_rgb,
                first_nonzero, last_nonzero, row_rgb_sum);
            sampled_rows++;
        }
    }

    signature = fnv ^ ((uint64_t)texture << 1) ^
                ((uint64_t)display_gl_label_hash(label) << 32) ^
                ((uint64_t)width << 16) ^ height;
    if (display_gl_readback_signature_seen(signature, &slot)) {
        return;
    }

    local_app_data = getenv("LOCALAPPDATA");
    if (local_app_data != NULL && local_app_data[0] != '\0') {
        dump_dir = g_build_filename(local_app_data, "xemu-d3d12-test",
                                    "captures", NULL);
        if (g_mkdir_with_parents(dump_dir, 0755) == 0) {
            dump_name = g_strdup_printf(
                "d3d12-gl-%s-%02u-tex%u-%ux%u-%016" PRIx64 ".bmp",
                label, slot, texture, width, height, fnv);
            dump_path = g_build_filename(dump_dir, dump_name, NULL);
            wrote_dump = write_display_source_bmp(
                dump_path, NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8,
                pixels, width, height, row_pitch, bytes_per_pixel);
        }
    }

    fprintf(stderr,
            "Direct3D 12 renderer: display GL texture sample (%u/%u): "
            "stage=%s, texture=%u, size=%ux%u, read_format=GL_BGRA, "
            "read_type=GL_UNSIGNED_INT_8_8_8_8_REV, source_addr=0x%"
            HWADDR_PRIx ", source_format=0x%x, nonzero_rgb=%" PRIu64
            ", rgb_sum=%" PRIu64 ", rgb_fnv=0x%016" PRIx64
            ", rows=[%s], dump=%s%s%s\n",
            slot + 1, D3D12_DISPLAY_GL_READBACK_DUMPS, label, texture,
            width, height, r->display_surface_addr,
            r->display_surface_color_format, total_nonzero_rgb,
            total_rgb_sum, fnv, rows->str,
            wrote_dump ? dump_path : "not-written",
            !wrote_dump && dump_path != NULL ? " path=" : "",
            !wrote_dump && dump_path != NULL ? dump_path : "");
}
#endif
#endif

void pgraph_d3d12_warn_once(PGRAPHState *pg, PGRAPHD3D12Diagnostic diagnostic,
                            const char *fmt, ...)
{
    PGRAPHD3D12State *r = pg ? pg->d3d12_renderer_state : NULL;
    va_list ap;

    if (r != NULL) {
        if (r->reported_diagnostics & diagnostic) {
            return;
        }
        r->reported_diagnostics |= diagnostic;
    }

    fprintf(stderr, "Direct3D 12 renderer: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/*
 * <base>/pix_capture/go starts a three-sync programmatic capture, ended
 * explicitly: the frame-count API never finalizes its file on Xbox UWP.
 */
static void pgraph_d3d12_maybe_pix_capture(PGRAPHD3D12State *r)
{
    typedef struct {
        PCWSTR FileName;
    } PixGpuCaptureOpenParameters;
    typedef HRESULT (WINAPI *PixBeginFn)(
        const PixGpuCaptureOpenParameters *);
    typedef HRESULT (WINAPI *PixEndFn)(void);
    static unsigned int capture_index;
    static unsigned int frames_recording;
    static wchar_t wpath[1024];
    char *go_path;
    char *wpix_path;
    HRESULT hr;

    if (r->pix_capturer == NULL) {
        return;
    }

    if (frames_recording > 0) {
        if (--frames_recording == 0) {
            PixEndFn end_fn = (PixEndFn)GetProcAddress(
                r->pix_capturer, "EndProgrammaticGpuCapture");

            hr = end_fn != NULL ? end_fn() : E_FAIL;
            fprintf(stderr,
                    "Direct3D 12 renderer: PIX capture ended: "
                    "HRESULT=0x%08lx\n",
                    (unsigned long)hr);
        }
        return;
    }

    go_path = g_build_filename(xemu_settings_get_base_path(), "pix_capture",
                               "go", NULL);
    if (!g_file_test(go_path, G_FILE_TEST_EXISTS)) {
        g_free(go_path);
        return;
    }
    g_unlink(go_path);
    g_free(go_path);

    PixBeginFn begin_fn = (PixBeginFn)GetProcAddress(
        r->pix_capturer, "BeginProgrammaticGpuCapture");
    if (begin_fn == NULL) {
        fprintf(stderr,
                "Direct3D 12 renderer: PIX capturer is loaded but "
                "BeginProgrammaticGpuCapture was not found\n");
        return;
    }

    wpix_path = g_strdup_printf("%s%spix_capture%scapture_%u.wpix",
                                xemu_settings_get_base_path(),
                                G_DIR_SEPARATOR_S, G_DIR_SEPARATOR_S,
                                capture_index++);
    if (MultiByteToWideChar(CP_UTF8, 0, wpix_path, -1, wpath,
                            ARRAY_SIZE(wpath)) > 0) {
        PixGpuCaptureOpenParameters params = { wpath };

        hr = begin_fn(&params);
        fprintf(stderr,
                "Direct3D 12 renderer: PIX capture started: %s "
                "HRESULT=0x%08lx\n",
                wpix_path, (unsigned long)hr);
        if (SUCCEEDED(hr)) {
            frames_recording = 3;
        }
    }
    g_free(wpix_path);
}

/*
 * The tune file is a list of words separated by white space. A switch is a
 * whole word; a key=value word is found by its key, and its value runs to
 * the next white space.
 */
static const char *tune_next_word(const char **pos, size_t *len)
{
    const char *p = *pos;
    const char *start;

    while (*p != '\0' && g_ascii_isspace(*p)) {
        p++;
    }
    start = p;
    while (*p != '\0' && !g_ascii_isspace(*p)) {
        p++;
    }
    *pos = p;
    *len = p - start;
    return p != start ? start : NULL;
}

static bool tune_word(const char *text, const char *word)
{
    size_t word_len = strlen(word);
    const char *pos = text;
    const char *w;
    size_t len;

    while ((w = tune_next_word(&pos, &len)) != NULL) {
        if (len == word_len && memcmp(w, word, len) == 0) {
            return true;
        }
    }
    return false;
}

/* The value of the first word starting with key ("name="), or NULL. */
static const char *tune_value(const char *text, const char *key)
{
    size_t key_len = strlen(key);
    const char *pos = text;
    const char *w;
    size_t len;

    while ((w = tune_next_word(&pos, &len)) != NULL) {
        if (len >= key_len && memcmp(w, key, key_len) == 0) {
            return w + key_len;
        }
    }
    return NULL;
}

/* Whether a value from tune_value() is exactly expected. */
static bool tune_value_is(const char *value, const char *expected)
{
    size_t len = strlen(expected);

    return value != NULL && strncmp(value, expected, len) == 0 &&
           (value[len] == '\0' || g_ascii_isspace(value[len]));
}

/* PGRAPH_D3D12_TUNE_* bits named in the tune file text. */
static uint32_t pgraph_d3d12_tune_text_flags(const char *text)
{
    static const struct {
        const char *word;
        uint32_t flag;
    } switches[] = {
        { "masked_clear=cpu", PGRAPH_D3D12_TUNE_MASKED_CLEAR_CPU },
        { "zdepth=native", PGRAPH_D3D12_TUNE_ZDEPTH_NATIVE },
        { "quads=cpu", PGRAPH_D3D12_TUNE_QUADS_CPU },
        { "fc=r0", PGRAPH_D3D12_TUNE_FC_R0 },
        { "vtx=always", PGRAPH_D3D12_TUNE_VTX_ALWAYS },
        { "zview=readback", PGRAPH_D3D12_TUNE_ZVIEW_READBACK },
        { "zinterp=screen", PGRAPH_D3D12_TUNE_ZINTERP_SCREEN },
        { "stash=off", PGRAPH_D3D12_TUNE_STASH_OFF },
        { "alias=off", PGRAPH_D3D12_TUNE_ALIAS_OFF },
        { "s2t=sync", PGRAPH_D3D12_TUNE_S2T_SYNC },
        { "s2t=off", PGRAPH_D3D12_TUNE_S2T_OFF },
        { "s2tlevels=off", PGRAPH_D3D12_TUNE_S2T_LEVELS_OFF },
        { "report=sync", PGRAPH_D3D12_TUNE_REPORT_SYNC },
        { "atest=off", PGRAPH_D3D12_TUNE_ATEST_OFF },
        { "zclip=clamp", PGRAPH_D3D12_TUNE_ZCLIP_CLAMP },
        { "skipshadow", PGRAPH_D3D12_TUNE_SKIP_SHADOW },
        { "shadow=lit", PGRAPH_D3D12_TUNE_SHADOW_LIT },
        { "submit=sync", PGRAPH_D3D12_TUNE_SUBMIT_SYNC },
        { "psosalt", PGRAPH_D3D12_TUNE_PSO_SALT },
        { "vtxfmt=static", PGRAPH_D3D12_TUNE_VTXFMT_STATIC },
        { "twintest", PGRAPH_D3D12_TUNE_TWIN_PSOS },
    };
    uint32_t flags = 0;
    const char *fb;

    for (unsigned int i = 0; i < ARRAY_SIZE(switches); i++) {
        if (tune_word(text, switches[i].word)) {
            flags |= switches[i].flag;
        }
    }
    /* ZCLIPMIN/ZCLIPMAX clamp instead of cull by default: Halo 2's HUD frame,
     * crosshair and icons (zclip 256.. with the draws at depth 0, no depth
     * buffer) were culled here while the GL renderer draws them.
     * "zclip=cull" restores the cull. */
    if (!tune_word(text, "zclip=cull")) {
        flags |= PGRAPH_D3D12_TUNE_ZCLIP_CLAMP;
    }
    /* DXIL is the default: it compiles and builds pipelines faster on the
     * console driver. "dxbc" selects FXC bytecode instead. */
    if (!tune_word(text, "dxbc")) {
        flags |= PGRAPH_D3D12_TUNE_SHADER_DXIL;
    }
    if (tune_value(text, "ubertest=") != NULL) {
        flags |= PGRAPH_D3D12_TUNE_UBER_TEST;
    }
    /* No fallback= value: "all", the interpreter draws while a new
     * pipeline builds on a worker (Halo 2: worst freeze 3.9 s -> 0.13 s
     * once its pipelines are recorded). "fallback=off" or an unknown mode
     * keeps the synchronous build. */
    fb = tune_value(text, "fallback=");
    if (fb == NULL) {
        flags |= (uint32_t)PGRAPH_D3D12_FALLBACK_ALL
                 << PGRAPH_D3D12_TUNE_FALLBACK_SHIFT;
    } else if (tune_value_is(fb, "skip")) {
        flags |= (uint32_t)PGRAPH_D3D12_FALLBACK_SKIP
                 << PGRAPH_D3D12_TUNE_FALLBACK_SHIFT;
    } else if (tune_value_is(fb, "ps")) {
        flags |= (uint32_t)PGRAPH_D3D12_FALLBACK_PS
                 << PGRAPH_D3D12_TUNE_FALLBACK_SHIFT;
    } else if (tune_value_is(fb, "all")) {
        flags |= (uint32_t)PGRAPH_D3D12_FALLBACK_ALL
                 << PGRAPH_D3D12_TUNE_FALLBACK_SHIFT;
    } else if (tune_value_is(fb, "debug")) {
        flags |= (uint32_t)PGRAPH_D3D12_FALLBACK_DEBUG
                 << PGRAPH_D3D12_TUNE_FALLBACK_SHIFT;
    }
    return flags;
}

static const char *pgraph_d3d12_fallback_mode_name(
    PGRAPHD3D12FallbackMode mode)
{
    static const char *const names[] = { "off", "skip", "debug", "ps",
                                         "all" };

    return (unsigned int)mode < ARRAY_SIZE(names) ? names[mode] : "?";
}

/* PGRAPH_D3D12_TUNE_* bits of the tune file, or the defaults without one. */
static uint32_t pgraph_d3d12_tune_file_flags(void)
{
    g_autofree char *path = g_strdup_printf(
        "%sd3d12_tune.txt", xemu_settings_get_base_path());
    g_autofree gchar *text = NULL;

    if (!g_file_get_contents(path, &text, NULL, NULL)) {
        return pgraph_d3d12_tune_text_flags("");
    }
    return pgraph_d3d12_tune_text_flags(text);
}

/* Re-reads the diagnostic switches; a missing file means defaults. */
static void pgraph_d3d12_read_tune_file(PGRAPHD3D12State *r)
{
    g_autofree char *path = g_strdup_printf(
        "%sd3d12_tune.txt", xemu_settings_get_base_path());
    g_autofree gchar *text = NULL;
    bool head = true;
    uint32_t flags = pgraph_d3d12_tune_text_flags("");

    xbox_diag_log = false;
    if (g_file_get_contents(path, &text, NULL, NULL)) {
        xbox_diag_log = tune_word(text, "diag");
        head = !tune_word(text, "report_wait=last");
        flags = pgraph_d3d12_tune_text_flags(text);
        g_nv2a_stats.show_frame_number = tune_word(text, "framenum");
        r->evlog_draws = tune_word(text, "evdraw");
        /* A checkpoint origin implies the checkpoints. */
        r->cpk_enabled = tune_word(text, "cpk") ||
                         tune_value(text, "cpkxy=") != NULL;
        r->cpk_x = 0;
        r->cpk_y = -1;
        {
            const char *xy = tune_value(text, "cpkxy=");

            if (xy != NULL) {
                sscanf(xy, "%d,%d", &r->cpk_x, &r->cpk_y);
            }
        }
        {
            const char *zk = tune_value(text, "zcpk=");

            r->zcpk_key[0] = 0;
            r->zcpk_key[1] = 0;
            if (zk != NULL) {
                sscanf(zk, "%x,%x", &r->zcpk_key[0], &r->zcpk_key[1]);
            }
        }
        {
            const char *dk = tune_value(text, "dumpkey=");

            r->evlog_dump_key[0] = 0;
            r->evlog_dump_key[1] = 0;
            if (dk != NULL) {
                sscanf(dk, "%x,%x", &r->evlog_dump_key[0],
                       &r->evlog_dump_key[1]);
            }
        }
        {
            const char *ev = tune_value(text, "evlog=");

            if (ev != NULL) {
                char id[32] = { 0 };

                sscanf(ev, "%31s", id);
                if (strcmp(id, r->evlog_id) != 0) {
                    g_strlcpy(r->evlog_id, id, sizeof(r->evlog_id));
                    r->evlog_frames_left = 300;
                    memset(r->evlog_seen_ph, 0,
                           sizeof(r->evlog_seen_ph));
                    memset(r->evlog_seen_key, 0,
                           sizeof(r->evlog_seen_key));
                    fprintf(stderr, "EV begin id=%s frame=%u\n", id,
                            g_nv2a_stats.frame_count);
                }
            }
        }
        {
            /* vCPU sampling profile: prof=<id> starts a 10 s window. */
            static char prof_id[32];
            const char *prof = tune_value(text, "prof=");
            char id[32] = { 0 };

            if (prof != NULL && sscanf(prof, "%31s", id) == 1 &&
                strcmp(id, prof_id) != 0) {
                g_strlcpy(prof_id, id, sizeof(prof_id));
                xbox_prof_start(id, 10);
            }
        }
        if (tune_word(text, "iorewind=on")) {
            qatomic_set(&xbox_io_rewind, true);
        } else if (tune_word(text, "iorewind=off")) {
            qatomic_set(&xbox_io_rewind, false);
        }
        if (tune_word(text, "pgraphlock=off")) {
            qatomic_set(&g_nv2a->pgraph_read_unlocked, true);
        } else if (tune_word(text, "pgraphlock=on")) {
            qatomic_set(&g_nv2a->pgraph_read_unlocked, false);
        }
        if (tune_word(text, "ssenative=off")) {
            xbox_set_sse_native(false);
        } else if (tune_word(text, "ssenative=on")) {
            xbox_set_sse_native(true);
        }
        if (tune_word(text, "jcinline=off")) {
            xbox_tb_set_inline_lookup(false);
        } else if (tune_word(text, "jcinline=on")) {
            xbox_tb_set_inline_lookup(true);
        }
        if (tune_word(text, "fastmem=off")) {
            xbox_fastmem_set_codegen(false);
        } else if (tune_word(text, "fastmem=on")) {
            xbox_fastmem_set_codegen(true);
        }
        {
            /* Minimum vCPU softmmu TLB size, in index bits. */
            const char *tb = tune_value(text, "tlbbits=");
            int bits;

            if (tb != NULL && sscanf(tb, "%d", &bits) == 1 &&
                qemu_get_cpu(0) != NULL) {
                tlb_set_dyn_min_bits(qemu_get_cpu(0), bits);
            }
        }
        {
            const char *cap = tune_value(text, "capture=");

            if (cap != NULL) {
                char id[32] = { 0 };

                sscanf(cap, "%31s", id);
                if (strcmp(id, r->capture_id) != 0) {
                    g_strlcpy(r->capture_id, id, sizeof(r->capture_id));
                    r->capture_frames_left = 2;
                    r->capture_draws = 0;
                    fprintf(stderr, "CAP begin id=%s frame_time=%d\n", id,
                            -1);
                }
            }
        }
        {
            /* Interpreter shader timing, once per new id. */
            const char *ut = tune_value(text, "ubertest=");
            char id[32] = { 0 };

            if ((flags & PGRAPH_D3D12_TUNE_UBER_TEST) && ut != NULL &&
                sscanf(ut, "%31s", id) == 1 &&
                strcmp(id, r->uber_test_id) != 0) {
                g_strlcpy(r->uber_test_id, id, sizeof(r->uber_test_id));
                pgraph_d3d12_queue_uber_test(r, id);
            }
        }
    }
    if (head != r->tune_report_wait_head) {
        fprintf(stderr, "Direct3D 12 tune: report_wait=%s\n",
                head ? "head" : "last");
    }
    if (flags != r->tune_flags) {
        fprintf(stderr, "Direct3D 12 tune: flags=0x%x\n", flags);
    }
    /* Setting dxil again retries DXIL after a failure turned it off. */
    if (flags & ~r->tune_flags & PGRAPH_D3D12_TUNE_SHADER_DXIL) {
        qatomic_set(&r->dxil_disabled, false);
    }
    r->tune_report_wait_head = head;
    r->tune_flags = flags;
}

/* Periodic cost breakdown; percentages are of interval wall time. */
static void pgraph_d3d12_perf_report(NV2AState *d)
{
    PGRAPHD3D12State *r = d->pgraph.d3d12_renderer_state;
    int64_t now = g_get_monotonic_time();

    pgraph_d3d12_collect_gpu_timers(r);
    /* Runs on the FIFO thread; the UI thread only adds to these two. */
    r->perf.sync_wait_us += qatomic_xchg(&r->ui_sync_wait_us, 0);
    r->perf.syncs += qatomic_xchg(&r->ui_syncs, 0);
    int64_t pfifo_idle_us = qatomic_read(&d->pfifo.idle_us);
    int64_t vcpu_idle_us = qatomic_read(&qemu_vcpu_idle_us);
    int64_t vcpu_exec_us = qatomic_read(&qemu_vcpu_exec_us);
    int64_t vcpu_bql_us = qatomic_read(&qemu_vcpu_bql_us);
    uint32_t mmio_reads[4] = {
        qatomic_read(&d->mmio_reads.pfifo), qatomic_read(&d->mmio_reads.pgraph),
        qatomic_read(&d->mmio_reads.user), qatomic_read(&d->mmio_reads.ptimer),
    };
    int64_t elapsed;

    if (r->perf.last_report_us == 0) {
        r->perf.last_report_us = now;
        r->pfifo_idle_mark_us = pfifo_idle_us;
        r->vcpu_idle_mark_us = vcpu_idle_us;
        r->vcpu_exec_mark_us = vcpu_exec_us;
        r->vcpu_bql_mark_us = vcpu_bql_us;
        memcpy(r->mmio_read_marks, mmio_reads, sizeof(mmio_reads));
        return;
    }
    elapsed = now - r->perf.last_report_us;
    if (elapsed < 5 * 1000 * 1000) {
        return;
    }

    char twin[192] = "";
    {
        /* Twin pipelines: totals of driver time per kind, the twinned base
         * pipelines' own driver time, and the count of each kind. */
        uint64_t sets = qatomic_xchg(&r->twin_pso.sets, 0);
        uint64_t base_us = qatomic_xchg(&r->twin_pso.base_us, 0);
        uint64_t depth = qatomic_xchg(&r->twin_pso.depth, 0);
        uint64_t depth_us = qatomic_xchg(&r->twin_pso.depth_us, 0);
        uint64_t blend = qatomic_xchg(&r->twin_pso.blend, 0);
        uint64_t blend_us = qatomic_xchg(&r->twin_pso.blend_us, 0);
        uint64_t rtv = qatomic_xchg(&r->twin_pso.rtv, 0);
        uint64_t rtv_us = qatomic_xchg(&r->twin_pso.rtv_us, 0);
        uint64_t failed = qatomic_xchg(&r->twin_pso.failed, 0);

        if (sets != 0 || (r->tune_flags & PGRAPH_D3D12_TUNE_TWIN_PSOS)) {
            snprintf(twin, sizeof(twin),
                     " twin={n=%u depth=%.1fms blend=%.1fms rtv=%.1fms "
                     "base=%.1fms nd=%u nb=%u nr=%u fail=%u}",
                     (unsigned int)sets, depth_us / 1000.0,
                     blend_us / 1000.0, rtv_us / 1000.0, base_us / 1000.0,
                     (unsigned int)depth, (unsigned int)blend,
                     (unsigned int)rtv, (unsigned int)failed);
        }
    }
    char fallback[384] = "";
    {
        /* Keys newly skipped for, draws skipped, the time from a key's
         * first skipped draw to its pipeline, the fallback keys seen this
         * session, and the draws with and without the interpreter pixel
         * shader. */
        PGRAPHD3D12FallbackMode mode =
            pgraph_d3d12_fallback_mode(r->tune_flags);

        if (mode != PGRAPH_D3D12_FALLBACK_OFF || r->perf.fallback_demand ||
            r->perf.fallback_skipped || r->perf.fallback_ready ||
            r->perf.fallback_uber_ps || r->perf.fallback_no_ps ||
            r->perf.fallback_draws || r->perf.fallback_sync ||
            r->perf.fallback_builds) {
            snprintf(fallback, sizeof(fallback),
                     " fallback={mode=%s demand=%u skipped=%u "
                     "ready_ms_avg=%.1f ready_ms_max=%.1f fbkeys=%u "
                     "ps=%u nops=%u draws=%u sync=%u nokey=%u builds=%u "
                     "build_ms_avg=%.1f build_ms_max=%.1f}",
                     pgraph_d3d12_fallback_mode_name(mode),
                     r->perf.fallback_demand, r->perf.fallback_skipped,
                     r->perf.fallback_ready ?
                         r->perf.fallback_ready_us / 1000.0 /
                             r->perf.fallback_ready : 0.0,
                     r->perf.fallback_ready_max_us / 1000.0,
                     r->fallback_keys, r->perf.fallback_uber_ps,
                     r->perf.fallback_no_ps, r->perf.fallback_draws,
                     r->perf.fallback_sync, r->perf.fallback_nokey,
                     r->perf.fallback_builds,
                     r->perf.fallback_builds ?
                         r->perf.fallback_build_us / 1000.0 /
                             r->perf.fallback_builds : 0.0,
                     r->perf.fallback_build_max_us / 1000.0);
        }
    }

    xbox_diag_printf(
            "Direct3D 12 perf: %.1fs fps=%.1f guest_fps=%.1f "
            "draws=%u (%u/gframe) draw_setup=%u%% "
            "submits=%u src=[%u,%u,%u,%u,%u,%u,%u] sync_wait=%u%% "
            "flip_wait=%u%% render_idle=%u%% guest_idle=%u%% "
            "guest_exec=%u%% guest_bql=%u%% "
            "mmio_reads={pfifo=%u pgraph=%u user=%u ptimer=%u} "
            "ring_wait=%u%%(%u) "
            "allocator_wait=%u%%(%u) vertex_upload=%uKB/%u constants=%uMB "
            "stage={surf=%u%%[dl=%u%% ul=%u%% new=%u%%/%u] tex=%u%% "
            "pso=%u%% const=%u%% rec=%u%% vtx=%u%%} "
            "gpu_finishes=%u (%u%%; smp=%u rep=%u) reports=%u (%u%%) "
            "pvideo={conv=%u hit=%u cost=%u%%} "
            "surf_dl={inc=%u ovl=%u tex=%u blit=%u mask=%u cpu=%u "
            "alias=%u} stash={new=%u done=%u drop=%u adopt=%u cost=%u%%} "
            "restore={none=%u abort_c=%u abort_z=%u cover=%u} "
            "report_wait={n=%u break=%u avg=%ums max=%ums lag=%u mode=%s} "
            "gpu_busy=%u%% (%u lists) "
            "tex_stash={use=%u retry=%u} tex_fast=%u zeta_view=%u "
            "stash_done={ul=%u dl=%u tex=%u blit=%u range=%u cpu=%u sync=%u "
            "disp=%u cap=%u} "
            "surf_cb={calls=%" PRIu64 " waits=%" PRIu64
            " reg=%" PRIu64 " unreg=%" PRIu64 "} "
            "shader_compiles=%u (%ums) "
            "psos={new=%u %ums drv=%ums dxil=%u dxilfb=%u warm=%u}%s%s\n",
            elapsed / 1e6, r->perf.syncs / (elapsed / 1e6),
            r->perf.flips / (elapsed / 1e6),
            r->perf.draws,
            r->perf.flips != 0 ? r->perf.draws / r->perf.flips : 0,
            (unsigned int)(r->perf.draw_setup_us * 100 / elapsed),
            r->perf.submits,
            r->perf.submit_src[0], r->perf.submit_src[1],
            r->perf.submit_src[2], r->perf.submit_src[3],
            r->perf.submit_src[4], r->perf.submit_src[5],
            r->perf.submit_src[6],
            (unsigned int)(r->perf.sync_wait_us * 100 / elapsed),
            (unsigned int)(r->perf.flip_wait_us * 100 / elapsed),
            (unsigned int)((pfifo_idle_us - r->pfifo_idle_mark_us) * 100 /
                           elapsed),
            (unsigned int)((vcpu_idle_us - r->vcpu_idle_mark_us) * 100 /
                           elapsed),
            (unsigned int)((vcpu_exec_us - r->vcpu_exec_mark_us) * 100 /
                           elapsed),
            (unsigned int)((vcpu_bql_us - r->vcpu_bql_mark_us) * 100 /
                           elapsed),
            mmio_reads[0] - r->mmio_read_marks[0],
            mmio_reads[1] - r->mmio_read_marks[1],
            mmio_reads[2] - r->mmio_read_marks[2],
            mmio_reads[3] - r->mmio_read_marks[3],
            (unsigned int)(r->perf.ring_wait_us * 100 / elapsed),
            r->perf.ring_waits,
            (unsigned int)(r->perf.allocator_wait_us * 100 / elapsed),
            r->perf.allocator_waits,
            (unsigned int)(r->perf.vertex_upload_bytes >> 10),
            r->perf.vertex_upload_ranges,
            (unsigned int)(r->perf.constants_bytes >> 20),
            (unsigned int)(r->perf.surface_update_us * 100 / elapsed),
            (unsigned int)(r->perf.surface_download_us * 100 / elapsed),
            (unsigned int)(r->perf.surface_upload_us * 100 / elapsed),
            (unsigned int)(r->perf.surface_create_us * 100 / elapsed),
            r->perf.surface_creates,
            (unsigned int)(r->perf.texture_bind_us * 100 / elapsed),
            (unsigned int)(r->perf.pipeline_lookup_us * 100 / elapsed),
            (unsigned int)(r->perf.constant_upload_us * 100 / elapsed),
            (unsigned int)(r->perf.record_state_us * 100 / elapsed),
            (unsigned int)(r->perf.vertex_setup_us * 100 / elapsed),
            r->perf.downloads,
            (unsigned int)(r->perf.download_wait_us * 100 / elapsed),
            r->perf.finish_sampler, r->perf.finish_report,
            r->perf.report_flushes,
            (unsigned int)(r->perf.report_flush_us * 100 / elapsed),
            r->perf.pvideo_conversions, r->perf.pvideo_cache_hits,
            (unsigned int)(r->perf.pvideo_convert_us * 100 / elapsed),
            r->perf.surface_downloads[
                PGRAPH_D3D12_SURFACE_DOWNLOAD_INCOMPATIBLE],
            r->perf.surface_downloads[
                PGRAPH_D3D12_SURFACE_DOWNLOAD_OVERLAP_INVALIDATION],
            r->perf.surface_downloads[
                PGRAPH_D3D12_SURFACE_DOWNLOAD_TEXTURE_OVERLAP],
            r->perf.surface_downloads[
                PGRAPH_D3D12_SURFACE_DOWNLOAD_BLIT_OVERLAP],
            r->perf.surface_downloads[
                PGRAPH_D3D12_SURFACE_DOWNLOAD_MASKED_CLEAR],
            r->perf.surface_downloads[
                PGRAPH_D3D12_SURFACE_DOWNLOAD_DISPLAY_CPU],
            r->perf.surface_alias_copies, r->perf.readbacks_recorded,
            r->perf.readbacks_completed, r->perf.readbacks_dropped,
            r->perf.readbacks_adopted,
            (unsigned int)(r->perf.readback_complete_us * 100 / elapsed),
            r->perf.restore_none, r->perf.restore_abort_color,
            r->perf.restore_abort_zeta, r->perf.restore_cover,
            r->perf.report_waits, r->perf.report_wait_breaks,
            r->perf.report_waits ?
                (unsigned int)(r->perf.report_flush_us /
                               r->perf.report_waits / 1000) : 0,
            (unsigned int)(r->perf.report_wait_max_us / 1000),
            r->perf.report_waits ?
                (unsigned int)(r->perf.report_wait_lag /
                               r->perf.report_waits) : 0,
            r->tune_report_wait_head ? "head" : "last",
            r->gpu_timer_frequency ?
                (unsigned int)(r->perf.gpu_busy_ticks * 100000000ull /
                               r->gpu_timer_frequency /
                               (uint64_t)MAX(elapsed, 1)) : 0,
            r->perf.gpu_lists_timed,
            r->perf.tex_from_stash, r->perf.tex_stash_retry,
            r->perf.tex_fast, r->perf.tex_zeta_view,
            r->perf.stash_done_by[PGRAPH_D3D12_STASH_TRIGGER_UPLOAD],
            r->perf.stash_done_by[PGRAPH_D3D12_STASH_TRIGGER_DOWNLOAD],
            r->perf.stash_done_by[PGRAPH_D3D12_STASH_TRIGGER_TEXTURE],
            r->perf.stash_done_by[PGRAPH_D3D12_STASH_TRIGGER_BLIT],
            r->perf.stash_done_by[PGRAPH_D3D12_STASH_TRIGGER_RANGE],
            r->perf.stash_done_by[PGRAPH_D3D12_STASH_TRIGGER_CPU],
            r->perf.stash_done_by[PGRAPH_D3D12_STASH_TRIGGER_SYNC],
            r->perf.stash_done_by[PGRAPH_D3D12_STASH_TRIGGER_DISPLAY],
            r->perf.stash_done_by[PGRAPH_D3D12_STASH_TRIGGER_CAP],
            r->surface_callback_calls, r->surface_callback_waits,
            r->surface_callback_registers, r->surface_callback_unregisters,
            r->perf.shader_compiles,
            (unsigned int)(r->perf.shader_compile_us / 1000),
            r->perf.pso_creates,
            (unsigned int)(r->perf.pso_create_us / 1000),
            (unsigned int)(r->perf.pso_driver_us / 1000),
            r->perf.pso_dxil, r->perf.pso_dxil_fallbacks,
            r->perf.pso_prewarmed, twin, fallback);
    {
        /* vCPU softmmu cost: refills, and full flushes in total and on
         * behalf of surface watchpoints. */
        static int64_t fills_mark, cb_flush_mark, pflush_mark, lpflush_mark;
        static int64_t reset_dirty_mark, walks_mark;
        static int64_t tbc_marks[5];
        int64_t tbc[5] = {
            qatomic_read(&qemu_tb_invalidates),
            qatomic_read(&qemu_tb_jmp_flushes),
            qatomic_read(&qemu_tb_jc_misses),
            qatomic_read(&qemu_tb_lookup_ptrs),
            qatomic_read(&qemu_tb_gens),
        };
        {
            /* The four PGRAPH registers the vCPU waited longest on. */
            uint32_t *reads = d->mmio_reads.pgraph_addr_reads;
            uint32_t *waited = d->mmio_reads.pgraph_addr_wait_us;
            GString *top = g_string_new("");

            for (int k = 0; k < 4; k++) {
                int best = -1;

                for (int a = 0; a < 0x800; a++) {
                    if (reads[a] != 0 &&
                        (best < 0 || waited[a] > waited[best] ||
                         (waited[a] == waited[best] &&
                          reads[a] > reads[best]))) {
                        best = a;
                    }
                }
                if (best < 0) {
                    break;
                }
                g_string_append_printf(top, " 0x%04x:%u/%ums", best * 4,
                                       reads[best], waited[best] / 1000);
                reads[best] = 0;
                waited[best] = 0;
            }
            memset(reads, 0, sizeof(d->mmio_reads.pgraph_addr_reads));
            memset(waited, 0, sizeof(d->mmio_reads.pgraph_addr_wait_us));
            xbox_diag_printf("Direct3D 12 pgraph reads:%s\n", top->str);
            g_string_free(top, true);
        }
        static int64_t wait_marks[3];
        int64_t waits[3] = {
            qatomic_read(&d->mmio_reads.pfifo_wait_us),
            qatomic_read(&d->mmio_reads.pgraph_wait_us),
            qatomic_read(&d->mmio_reads.user_wait_us),
        };
        int64_t reset_dirty = qatomic_read(&tlb_reset_dirty_calls);
        int64_t walks = qatomic_read(&tlb_reset_dirty_walks);
        int64_t pflushes = qatomic_read(&tlb_page_flushes);
        int64_t lpflushes = qatomic_read(&tlb_large_page_flushes);
        static size_t full_flush_mark;
        CPUState *cpu = qemu_get_cpu(0);
        int64_t fills = qatomic_read(&qemu_vcpu_tlb_fills);
        int64_t cb_flushes = qatomic_read(&qemu_mem_access_cb_flushes);
        size_t full_flushes = cpu != NULL ?
            qatomic_read(&cpu->neg.tlb.c.full_flush_count) : 0;

        GString *sizes = g_string_new("");

        for (int i = 0; cpu != NULL && i < NB_MMU_MODES; i++) {
            CPUTLBDescFast *fast = &cpu->neg.tlb.f[i];

            g_string_append_printf(
                sizes, "%s%zu/%zu", i ? "," : "",
                (size_t)((qatomic_read(&fast->mask) >> CPU_TLB_ENTRY_BITS) +
                         1),
                qatomic_read(&cpu->neg.tlb.d[i].n_used_entries));
        }
        xbox_diag_printf(
                "Direct3D 12 cpu: tlb_fills=%" PRId64 " full_flushes=%zu "
                "cb_flushes=%" PRId64 " page_flushes=%" PRId64
                " large_page_flushes=%" PRId64 " reset_dirty=%" PRId64
                " walks=%" PRId64
                " mmio_wait_ms={pfifo=%" PRId64 " pgraph=%" PRId64
                " user=%" PRId64 "}"
                " lp=%016" PRIx64 "/%016" PRIx64 " tlb=[%s]\n",
                fills - fills_mark, full_flushes - full_flush_mark,
                cb_flushes - cb_flush_mark, pflushes - pflush_mark,
                lpflushes - lpflush_mark, reset_dirty - reset_dirty_mark,
                walks - walks_mark,
                (waits[0] - wait_marks[0]) / 1000,
                (waits[1] - wait_marks[1]) / 1000,
                (waits[2] - wait_marks[2]) / 1000,
                cpu != NULL ? (uint64_t)cpu->neg.tlb.d[5].large_page_addr : 0,
                cpu != NULL ? (uint64_t)cpu->neg.tlb.d[5].large_page_mask : 0,
                sizes->str);
        pflush_mark = pflushes;
        {
            static XboxFastmemStats mark;
            XboxFastmemStats cur = xbox_fastmem_stats;

            if (xbox_fastmem_active) {
                xbox_diag_printf(
                        "Direct3D 12 fastmem: faults=%" PRIu64 " maps=%"
                        PRIu64 " upgrades=%" PRIu64 " dirty=%" PRIu64
                        " patches=%" PRIu64 " unmaps=%" PRIu64
                        " protects=%" PRIu64 " flush_all=%" PRIu64
                        " sites=%" PRIu64 " why={inv=%" PRIu64 " mmio=%"
                        PRIu64 " watch=%" PRIu64 " notram=%" PRIu64
                        " code=%" PRIu64 " ro=%" PRIu64 " guard=%" PRIu64
                        " mapfail=%" PRIu64 "}\n",
                        cur.faults - mark.faults, cur.maps - mark.maps,
                        cur.upgrades - mark.upgrades,
                        cur.dirty - mark.dirty,
                        cur.patches - mark.patches,
                        cur.unmaps - mark.unmaps,
                        cur.protects - mark.protects,
                        cur.flush_all - mark.flush_all,
                        cur.sites - mark.sites,
                        cur.patch_invalid - mark.patch_invalid,
                        cur.patch_mmio - mark.patch_mmio,
                        cur.patch_watch - mark.patch_watch,
                        cur.patch_not_ram - mark.patch_not_ram,
                        cur.patch_code - mark.patch_code,
                        cur.patch_read_only - mark.patch_read_only,
                        cur.patch_guard - mark.patch_guard,
                        cur.patch_map_failed - mark.patch_map_failed);
            }
            mark = cur;
        }
        reset_dirty_mark = reset_dirty;
        walks_mark = walks;
        memcpy(wait_marks, waits, sizeof(waits));
        {
            static int64_t kind_marks[3], loop_marks[4], rest_marks[2];
            int64_t rest[2] = {
                qatomic_read(&qemu_tb_restores),
                qatomic_read(&qemu_tb_io_recompiles),
            };
            int64_t kinds[3] = {
                qatomic_read(&qemu_tb_jc_miss_kind[0]),
                qatomic_read(&qemu_tb_jc_miss_kind[1]),
                qatomic_read(&qemu_tb_jc_miss_kind[2]),
            };
            int64_t loops[4] = {
                qatomic_read(&qemu_tb_loop_kind[0]),
                qatomic_read(&qemu_tb_loop_kind[1]),
                qatomic_read(&qemu_tb_loop_kind[2]),
                qatomic_read(&qemu_tb_loop_kind[3]),
            };

            xbox_diag_printf("Direct3D 12 tbcache: invalidates=%" PRId64
                    " jmp_flushes=%" PRId64 " jc_misses=%" PRId64
                    " (empty=%" PRId64 " other_pc=%" PRId64
                    " same_pc=%" PRId64 " diff=0x%08x) lookup_ptrs=%"
                    PRId64 " gens=%" PRId64 " loop={chain=%" PRId64
                    " req=%" PRId64 " epi=%" PRId64 " span=%" PRId64 "}"
                    " restores=%" PRId64 " io_recompiles=%" PRId64 "\n",
                    tbc[0] - tbc_marks[0], tbc[1] - tbc_marks[1],
                    tbc[2] - tbc_marks[2], kinds[0] - kind_marks[0],
                    kinds[1] - kind_marks[1], kinds[2] - kind_marks[2],
                    qatomic_xchg(&qemu_tb_jc_flag_diff, 0),
                    tbc[3] - tbc_marks[3], tbc[4] - tbc_marks[4],
                    loops[0] - loop_marks[0], loops[1] - loop_marks[1],
                    loops[2] - loop_marks[2], loops[3] - loop_marks[3],
                    rest[0] - rest_marks[0], rest[1] - rest_marks[1]);
            memcpy(rest_marks, rest, sizeof(rest));
            memcpy(kind_marks, kinds, sizeof(kinds));
            memcpy(loop_marks, loops, sizeof(loops));
        }
        memcpy(tbc_marks, tbc, sizeof(tbc));
        lpflush_mark = lpflushes;
        g_string_free(sizes, true);
        fills_mark = fills;
        cb_flush_mark = cb_flushes;
        full_flush_mark = full_flushes;
    }
    pgraph_d3d12_report_surface_churn();
    memset(&r->perf, 0, sizeof(r->perf));
    pgraph_d3d12_read_tune_file(r);
    r->perf.last_report_us = now;
    r->pfifo_idle_mark_us = pfifo_idle_us;
    r->vcpu_idle_mark_us = vcpu_idle_us;
    r->vcpu_exec_mark_us = vcpu_exec_us;
    r->vcpu_bql_mark_us = vcpu_bql_us;
    memcpy(r->mmio_read_marks, mmio_reads, sizeof(mmio_reads));
}

static bool display_snapshot_released(PGRAPHD3D12State *r, uint64_t value)
{
    return value == 0 || r->display_release_fence == NULL ||
           r->display_release_fence->lpVtbl->GetCompletedValue(
               r->display_release_fence) >= value;
}

/*
 * The presenter reads a snapshot on its own queue until it signals the
 * hand-off's release value. A snapshot it may still read is parked instead
 * of overwritten; the oldest parked one is dropped when all are taken (the
 * presenter holds its own reference while reading).
 */
static void pgraph_d3d12_park_display_snapshot(PGRAPHState *pg)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    unsigned int slot = 0;

    if (r->display_snapshot_resource == NULL) {
        return;
    }
    for (unsigned int i = 0; i < ARRAY_SIZE(r->display_snapshot_spares); i++) {
        if (r->display_snapshot_spares[i].resource == NULL) {
            slot = i;
            break;
        }
        if (r->display_snapshot_spares[i].release_value <
            r->display_snapshot_spares[slot].release_value) {
            slot = i;
        }
    }
    if (r->display_snapshot_spares[slot].resource != NULL) {
        pgraph_d3d12_defer_release(pg, r->display_snapshot_spares[slot].resource,
                                   -1, -1, -1, -1);
    }
    r->display_snapshot_spares[slot].resource = r->display_snapshot_resource;
    r->display_snapshot_spares[slot].state = r->display_snapshot_state;
    r->display_snapshot_spares[slot].format = r->display_snapshot_format;
    r->display_snapshot_spares[slot].width = r->display_snapshot_width;
    r->display_snapshot_spares[slot].height = r->display_snapshot_height;
    r->display_snapshot_spares[slot].release_value =
        r->display_snapshot_release_value;
    r->display_snapshot_resource = NULL;
    r->display_snapshot_release_value = 0;
    r->display_snapshot_valid = false;
}

void pgraph_d3d12_release_display_snapshots(PGRAPHState *pg)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;

    for (unsigned int i = 0; i < ARRAY_SIZE(r->display_snapshot_spares); i++) {
        if (r->display_snapshot_spares[i].resource != NULL) {
            pgraph_d3d12_defer_release(
                pg, r->display_snapshot_spares[i].resource, -1, -1, -1, -1);
            r->display_snapshot_spares[i].resource = NULL;
        }
    }
}

static bool pgraph_d3d12_ensure_display_snapshot(
    PGRAPHState *pg, PGRAPHD3D12SurfaceBinding *surface, Error **errp)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    unsigned int width = MAX(surface->width, 1);
    unsigned int height = MAX(surface->height, 1);
    D3D12_HEAP_PROPERTIES heap_props;
    D3D12_RESOURCE_DESC desc;
    HRESULT hr;

    pgraph_apply_scaling_factor(pg, &width, &height);

    /* The copy overwrites the snapshot: never one the presenter may read. */
    if (!display_snapshot_released(r, r->display_snapshot_release_value)) {
        pgraph_d3d12_park_display_snapshot(pg);
    }

    if (r->display_snapshot_resource != NULL &&
        r->display_snapshot_format == surface->fmt.dxgi_format &&
        r->display_snapshot_width == width &&
        r->display_snapshot_height == height) {
        return true;
    }

    if (r->display_snapshot_resource != NULL) {
        pgraph_d3d12_defer_release(pg, r->display_snapshot_resource,
                                   -1, -1, -1, -1);
        r->display_snapshot_resource = NULL;
    }

    for (unsigned int i = 0; i < ARRAY_SIZE(r->display_snapshot_spares); i++) {
        if (r->display_snapshot_spares[i].resource != NULL &&
            r->display_snapshot_spares[i].format == surface->fmt.dxgi_format &&
            r->display_snapshot_spares[i].width == width &&
            r->display_snapshot_spares[i].height == height &&
            display_snapshot_released(
                r, r->display_snapshot_spares[i].release_value)) {
            r->display_snapshot_resource = r->display_snapshot_spares[i].resource;
            r->display_snapshot_state = r->display_snapshot_spares[i].state;
            r->display_snapshot_format = r->display_snapshot_spares[i].format;
            r->display_snapshot_width = width;
            r->display_snapshot_height = height;
            r->display_snapshot_release_value = 0;
            r->display_snapshot_valid = false;
            r->display_snapshot_spares[i].resource = NULL;
            return true;
        }
    }
    r->display_snapshot_valid = false;
    r->display_snapshot_state = D3D12_RESOURCE_STATE_COPY_DEST;
    r->display_snapshot_format = DXGI_FORMAT_UNKNOWN;
    r->display_snapshot_width = 0;
    r->display_snapshot_height = 0;

    memset(&heap_props, 0, sizeof(heap_props));
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap_props.CreationNodeMask = 1;
    heap_props.VisibleNodeMask = 1;

    memset(&desc, 0, sizeof(desc));
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = surface->fmt.dxgi_format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    hr = r->device->lpVtbl->CreateCommittedResource(
        r->device, &heap_props, D3D12_HEAP_FLAG_NONE, &desc,
        r->display_snapshot_state, NULL, &IID_ID3D12Resource,
        (void **)&r->display_snapshot_resource);
    if (FAILED(hr)) {
        char detail[128];

        error_setg(errp,
                   "Failed to create Direct3D 12 display snapshot "
                   "resource: HRESULT %s (0x%08lx)%s, surface=0x%"
                   HWADDR_PRIx " %ux%u scaled=%ux%u dxgi=%u",
                   pgraph_d3d12_hresult_name(hr), (unsigned long)hr,
                   pgraph_d3d12_device_removed_detail(r, hr, detail,
                                                      sizeof(detail)),
                   surface->vram_addr, surface->width, surface->height,
                   width, height,
                   (unsigned int)surface->fmt.dxgi_format);
        return false;
    }

    r->display_snapshot_format = surface->fmt.dxgi_format;
    r->display_snapshot_width = width;
    r->display_snapshot_height = height;
    return true;
}

/* Queue-ordered snapshot of the display surface for the presenter. */
static bool pgraph_d3d12_record_display_snapshot(
    NV2AState *d, PGRAPHD3D12SurfaceBinding *surface, Error **errp)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    D3D12_TEXTURE_COPY_LOCATION dst;
    D3D12_TEXTURE_COPY_LOCATION src;
    int source_draw_time = surface->draw_time;

    /* draw_time is the content serial; several writes can land in one flip. */
    if (r->display_snapshot_valid &&
        r->display_snapshot_source == (const void *)surface &&
        r->display_snapshot_draw_time == source_draw_time) {
        return true;
    }

    if (!pgraph_d3d12_ensure_display_snapshot(pg, surface, errp)) {
        return false;
    }

    if (!pgraph_d3d12_begin_commands(pg, errp)) {
        return false;
    }

    pgraph_d3d12_transition_raw_resource(r, surface->resource,
                                         &surface->state,
                                         D3D12_RESOURCE_STATE_COPY_SOURCE);
    pgraph_d3d12_transition_raw_resource(r, r->display_snapshot_resource,
                                         &r->display_snapshot_state,
                                         D3D12_RESOURCE_STATE_COPY_DEST);

    memset(&dst, 0, sizeof(dst));
    dst.pResource = r->display_snapshot_resource;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;

    memset(&src, 0, sizeof(src));
    src.pResource = surface->resource;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;

    r->command_list->lpVtbl->CopyTextureRegion(r->command_list, &dst, 0, 0,
                                               0, &src, NULL);

    pgraph_d3d12_transition_raw_resource(
        r, r->display_snapshot_resource, &r->display_snapshot_state,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    pgraph_d3d12_transition_raw_resource(
        r, surface->resource, &surface->state,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    r->perf.submit_src[4]++;
    if (!pgraph_d3d12_submit_commands_async(pg, errp)) {
        return false;
    }

    pgraph_d3d12_evlog(
        r, "snap src=0x%" HWADDR_PRIx " cur=0x%" HWADDR_PRIx
        " sdt=%d dt=%d since_flip=%d pcrtc=0x%" HWADDR_PRIx,
        surface->vram_addr,
        r->color_binding ? r->color_binding->vram_addr : 0,
        source_draw_time, pg->draw_time,
        pg->draw_time - r->evlog_flip_draw_time,
        (hwaddr)d->pcrtc.start);
    r->display_snapshot_ready_value = r->submitted_fence_value;
    r->display_snapshot_valid = true;
    r->display_snapshot_source = surface;
    r->display_snapshot_draw_time = source_draw_time;
    g_nv2a_stats.snapshot_frame = g_nv2a_stats.frame_count;
    return true;
}

static void pgraph_d3d12_capture_display_frame(NV2AState *d);

static void pgraph_d3d12_sync(NV2AState *d)
{
    PGRAPHD3D12State *r = d->pgraph.d3d12_renderer_state;
    VGADisplayParams vga_display_params;
    hwaddr display_addr;
    bool download = r != NULL && r->display_sync_download;
    bool selected;

    d->vga.get_params(&d->vga, &vga_display_params);
    display_addr = d->pcrtc.start + vga_display_params.line_offset;
    selected = pgraph_d3d12_select_display_surface(
        d, display_addr, d->pcrtc.start, vga_display_params.line_offset,
        download);
    if (selected && !download && r != NULL && r->display_surface_valid &&
        r->display_surface_binding != NULL &&
        r->display_surface_binding->resource != NULL) {
        Error *local_err = NULL;
        PGRAPHD3D12SurfaceBinding *surface = r->display_surface_binding;
        bool force_upload = !tcg_enabled();

        if (r->display_surface_pitch != 0 &&
            surface->pitch != r->display_surface_pitch) {
            pgraph_d3d12_surface_download_if_dirty(d, surface);
            goto sync_complete;
        }

        if (surface->draw_dirty && !surface->upload_pending) {
            force_upload = false;
        }

        if (!pgraph_d3d12_upload_surface_data_with_state(
                d, surface, force_upload,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &local_err)) {
            fprintf(stderr,
                    "Direct3D 12 renderer: display frame surface upload "
                    "failed during sync: addr=0x%" HWADDR_PRIx
                    " %ux%u pitch=%u format=0x%x upload_pending=%d "
                    "draw_dirty=%d: %s\n",
                    surface->vram_addr, surface->width, surface->height,
                    surface->pitch, surface->shape.color_format,
                    surface->upload_pending, surface->draw_dirty,
                    local_err != NULL ? error_get_pretty(local_err) :
                                        "unknown error");
            error_free(local_err);
            r->display_surface_valid = false;
        } else {
            local_err = NULL;
        }

        if (!r->display_surface_valid) {
            goto sync_complete;
        }

        if (!pgraph_d3d12_record_display_snapshot(d, surface, &local_err)) {
            fprintf(stderr,
                    "Direct3D 12 renderer: display frame snapshot failed "
                    "during sync: %s\n",
                    local_err != NULL ? error_get_pretty(local_err) :
                                        "unknown error");
            error_free(local_err);
            r->display_snapshot_valid = false;
        }
    }
sync_complete:
    if (r != NULL) {
        pgraph_d3d12_capture_display_frame(d);
        pgraph_d3d12_maybe_pix_capture(r);
        pgraph_d3d12_perf_report(d);
        pgraph_d3d12_shader_cache_flush(&d->pgraph, false);
        r->display_sync_download = true;
    }

    qatomic_set(&d->pgraph.sync_pending, false);
    qemu_event_set(&d->pgraph.sync_complete);
}

static bool pgraph_d3d12_active_targets_restored(PGRAPHD3D12State *r,
                                                 bool had_color,
                                                 bool had_zeta)
{
    return (!had_color ||
            (r->color_binding != NULL &&
             r->color_binding->resource != NULL &&
             !r->color_binding->upload_pending)) &&
           (!had_zeta ||
            (r->zeta_binding != NULL &&
             r->zeta_binding->resource != NULL &&
             !r->zeta_binding->upload_pending));
}

/*
 * Cap the scale so a scaled staging copy of the largest guest target
 * (1024x1024x4, color and zeta) fits the adapter budget. Scales up to 4 are
 * always offered: a failed allocation rolls the change back.
 */
static unsigned int pgraph_d3d12_max_surface_scale(const PGRAPHD3D12State *r)
{
    const uint64_t worst_guest_surface_bytes = 1024ull * 1024 * 4;
    const unsigned int min_offered_scale = 4;
    uint64_t budget_bytes = 0;
    unsigned int scale = min_offered_scale;

    if (r != NULL) {
        budget_bytes = MAX(r->video_memory_budget,
                           (uint64_t)r->dedicated_video_memory);
    }
    if (budget_bytes == 0) {
        /* No adapter information: only the D3D12 dimension limit applies. */
        budget_bytes = UINT64_MAX;
    }

    while (scale < 16) {
        unsigned int next = scale + 1;
        uint64_t scaled_bytes =
            worst_guest_surface_bytes * next * next;

        if (1024ull * next > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
            scaled_bytes > budget_bytes / 8) {
            break;
        }
        scale = next;
    }
    return scale;
}

static unsigned int pgraph_d3d12_clamp_surface_scale(const PGRAPHD3D12State *r,
                                                     unsigned int requested)
{
    unsigned int max_scale = pgraph_d3d12_max_surface_scale(r);
    unsigned int clamped = MAX(requested, 1);

    if (clamped > max_scale) {
        fprintf(stderr,
                "Direct3D 12 renderer: surface scale %u exceeds what the "
                "adapter budget (%llu MiB) can transfer reliably; clamping "
                "to %u\n",
                clamped,
                r != NULL ?
                    (unsigned long long)(MAX(r->video_memory_budget,
                                             (uint64_t)
                                                 r->dedicated_video_memory) >>
                                         20) : 0,
                max_scale);
        clamped = max_scale;
    }
    return clamped;
}

static void pgraph_d3d12_flush(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = d->pgraph.d3d12_renderer_state;
    unsigned int old_scale = MAX(pg->surface_scale_factor, 1);
    unsigned int new_scale = pgraph_d3d12_clamp_surface_scale(
        r, g_config.display.quality.surface_scale);
    bool scale_changed = old_scale != new_scale;
    bool had_color = r != NULL && r->color_binding != NULL;
    bool had_zeta = r != NULL && r->zeta_binding != NULL;
    bool was_clearing = pg->clearing;
    /* Guest reset and state load replace guest memory, so GPU images of the
     * old contents, live or stashed, are discarded rather than written over
     * it. A renderer switch writes them back; a scale change already did
     * before requesting this flush. */
    bool discard = r != NULL &&
        pg->renderer_switch_phase == PGRAPH_RENDERER_SWITCH_PHASE_IDLE &&
        !r->surface_scale_flush_pending;
    Error *local_err = NULL;

    if (r != NULL) {
        r->surface_scale_flush_pending = false;
    }

    if (discard) {
        pgraph_d3d12_surface_discard_all(d);
    } else {
        /* Stashed images are older than any live surface over them. */
        pgraph_d3d12_complete_all_readbacks(d,
                                            PGRAPH_D3D12_STASH_TRIGGER_SYNC);
        if (!pgraph_d3d12_surface_flush_checked(d, &local_err)) {
            if (scale_changed) {
                /* The user asked for the new scale: one surface that
                 * cannot be written back must not veto it. */
                fprintf(stderr,
                        "Direct3D 12 renderer: surface scale %u -> %u drops "
                        "surfaces that could not be preserved: %s\n",
                        old_scale, new_scale,
                        local_err != NULL ? error_get_pretty(local_err) :
                                            "unknown error");
                error_free(local_err);
                local_err = NULL;
                pgraph_d3d12_surface_flush_lossy(d);
            } else {
                fprintf(stderr,
                        "Direct3D 12 renderer: surface flush could not "
                        "preserve old-scale surfaces: %s\n",
                        local_err != NULL ? error_get_pretty(local_err) :
                                            "unknown error");
                error_free(local_err);
                local_err = NULL;
                goto sync_vertex_ram;
            }
        }
    }
    if (r != NULL) {
        if (scale_changed && r->framebuffer_copy_resource != NULL) {
            /* Queue is drained; the SRV descriptor slot is reused. */
            pgraph_d3d12_defer_release(pg, r->framebuffer_copy_resource,
                                       -1, -1, -1, -1);
            r->framebuffer_copy_resource = NULL;
            r->record_framebuffer_srv_gpu = 0;
        }
        if (scale_changed) {
            r->framebuffer_copy_valid = false;
            r->framebuffer_copy_state = D3D12_RESOURCE_STATE_COMMON;
            r->framebuffer_copy_format = DXGI_FORMAT_UNKNOWN;
            r->framebuffer_copy_width = 0;
            r->framebuffer_copy_height = 0;
        }
        if (scale_changed && r->display_snapshot_resource != NULL) {
            pgraph_d3d12_defer_release(pg, r->display_snapshot_resource,
                                       -1, -1, -1, -1);
            r->display_snapshot_resource = NULL;
        }
        if (scale_changed) {
            pgraph_d3d12_release_display_snapshots(pg);
        }
        if (scale_changed) {
            r->display_snapshot_valid = false;
            r->display_snapshot_state = D3D12_RESOURCE_STATE_COPY_DEST;
            r->display_snapshot_format = DXGI_FORMAT_UNKNOWN;
            r->display_snapshot_width = 0;
            r->display_snapshot_height = 0;
        }

        /* Surface invalidation queues completed resources after its finish. */
        pgraph_d3d12_process_deferred_releases(pg);

        if (scale_changed) {
            /* Pending occlusion counts were gathered at the old scale. */
            pgraph_d3d12_flush_reports(d);
            /* Pooled images have old-scale sizes nothing will request. */
            pgraph_d3d12_purge_surface_pool(pg);
            pgraph_d3d12_flush_scale_dependent_textures(pg);
            if (r->pso_failed != NULL) {
                /* Memory-pressure failures are not deterministic PSO state. */
                g_hash_table_remove_all(r->pso_failed);
            }
        }

        if (scale_changed &&
            !pgraph_d3d12_reset_surface_transfer_buffers(
                pg, 16ULL * 1024 * 1024, &local_err)) {
            fprintf(stderr,
                    "Direct3D 12 renderer: surface-scale transfer buffer "
                    "reset failed: %s\n",
                    local_err != NULL ? error_get_pretty(local_err) :
                                        "unknown error");
            error_free(local_err);
            local_err = NULL;
        }
    }
    /* Applied only after every old-scale surface is flushed. */
    pg->surface_scale_factor = new_scale;
    /* Keep the persisted setting on the effective, budget-clamped value. */
    g_config.display.quality.surface_scale = new_scale;
    if (had_color || had_zeta) {
        /* Restore the previously active targets, bypassing write-mask gating. */
        pg->clearing = true;
        pgraph_d3d12_surface_update(d, true, had_color, had_zeta);
        pg->clearing = was_clearing;

        if (!pgraph_d3d12_active_targets_restored(r, had_color, had_zeta)) {
            fprintf(stderr,
                    "Direct3D 12 renderer: surface scale %u could not "
                    "recreate the active %s%s target; rolling back to %u\n",
                    new_scale, had_color ? "color" : "",
                    had_zeta ? (had_color ? "/zeta" : "zeta") : "",
                    old_scale);

            if (pgraph_d3d12_surface_flush_checked(d, &local_err)) {
                pgraph_d3d12_process_deferred_releases(pg);
                error_free(local_err);
                local_err = NULL;
                (void)pgraph_d3d12_reset_surface_transfer_buffers(
                    pg, 16ULL * 1024 * 1024, &local_err);
                if (local_err != NULL) {
                    fprintf(stderr,
                            "Direct3D 12 renderer: rollback transfer-buffer "
                            "reset failed: %s\n",
                            error_get_pretty(local_err));
                    error_free(local_err);
                    local_err = NULL;
                }
                pg->surface_scale_factor = old_scale;
                g_config.display.quality.surface_scale = old_scale;
                pg->clearing = true;
                pgraph_d3d12_surface_update(d, true, had_color, had_zeta);
                pg->clearing = was_clearing;
                if (!pgraph_d3d12_active_targets_restored(
                        r, had_color, had_zeta)) {
                    fprintf(stderr,
                            "Direct3D 12 renderer: rollback to surface scale "
                            "%u also failed to restore the active targets\n",
                            old_scale);
                }
            } else {
                fprintf(stderr,
                        "Direct3D 12 renderer: could not flush failed scale "
                        "%u before rollback: %s\n",
                        new_scale,
                        local_err != NULL ? error_get_pretty(local_err) :
                                            "unknown error");
                error_free(local_err);
                local_err = NULL;
            }
        }
    }
    pgraph_d3d12_mark_textures_possibly_dirty(d, 0,
                                              memory_region_size(d->vram));
sync_vertex_ram:
    /* Guest RAM may have been replaced (reset, state load). */
    pgraph_d3d12_invalidate_vertex_ram_buffer(d);
    pgraph_d3d12_finish(&d->pgraph);
    qatomic_set(&d->pgraph.flush_pending, false);
    qemu_event_set(&d->pgraph.flush_complete);
}

static void pgraph_d3d12_process_pending(NV2AState *d)
{
    PGRAPHD3D12State *r = d->pgraph.d3d12_renderer_state;

    if (qatomic_read(&r->downloads_pending) ||
        qatomic_read(&r->download_dirty_surfaces_pending) ||
        qatomic_read(&d->pgraph.sync_pending) ||
        qatomic_read(&d->pgraph.flush_pending) ||
        pgraph_d3d12_shader_cache_pending(&d->pgraph)) {
        qemu_mutex_unlock(&d->pfifo.lock);
        qemu_mutex_lock(&d->pgraph.lock);
        if (pgraph_d3d12_shader_cache_pending(&d->pgraph)) {
            pgraph_d3d12_shader_cache_process(&d->pgraph);
        }
        if (qatomic_read(&r->downloads_pending)) {
            pgraph_d3d12_process_pending_downloads(d);
        }
        if (qatomic_read(&r->download_dirty_surfaces_pending)) {
            pgraph_d3d12_download_dirty_surfaces(d);
        }
        if (qatomic_read(&d->pgraph.sync_pending)) {
            pgraph_d3d12_sync(d);
        }
        if (qatomic_read(&d->pgraph.flush_pending)) {
            pgraph_d3d12_flush(d);
        }
        qemu_mutex_unlock(&d->pgraph.lock);
        qemu_mutex_lock(&d->pfifo.lock);
    }
}

/* First vertices of one attribute, from whichever source the draw uses. */
static void capture_vertex_samples(PGRAPHState *pg, GString *line,
                                   unsigned int index, const char *name)
{
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    VertexAttribute *attr = &pg->vertex_attributes[index];
    unsigned int comps = attr->count;

    if (comps == 0) {
        g_string_append_printf(line, " %s=inline(%g,%g,%g,%g)", name,
                               attr->inline_value[0], attr->inline_value[1],
                               attr->inline_value[2], attr->inline_value[3]);
        return;
    }
    g_string_append_printf(line, " %s[fmt%u x%u]=", name, attr->format,
                           comps);
    for (unsigned int v = 0; v < 4; v++) {
        const uint8_t *p = NULL;

        if (pg->inline_buffer_length) {
            if (v < pg->inline_buffer_length && attr->inline_buffer) {
                const float *f = attr->inline_buffer + v * 4;

                g_string_append_printf(line, "(%g,%g,%g,%g)", f[0], f[1],
                                       f[2], f[3]);
            }
            continue;
        }
        if (pg->inline_array_length) {
            unsigned int offset = 0, vsize, aoff = 0;

            for (int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
                VertexAttribute *a = &pg->vertex_attributes[i];

                if (a->count == 0) {
                    continue;
                }
                offset = ROUND_UP(offset, a->size);
                if ((unsigned int)i == index) {
                    aoff = offset;
                }
                offset += a->size * a->count;
                offset = ROUND_UP(offset, a->size);
            }
            vsize = offset;
            if (vsize && (v + 1) * vsize <= pg->inline_array_length * 4) {
                p = (const uint8_t *)pg->inline_array + v * vsize + aoff;
            }
        } else {
            hwaddr len = 0;
            uint8_t *base = (uint8_t *)nv_dma_map(
                d, attr->dma_select ? pg->dma_vertex_b : pg->dma_vertex_a,
                &len);
            uint32_t element = pg->inline_elements_length ?
                (v < pg->inline_elements_length ? pg->inline_elements[v] :
                                                  UINT32_MAX) :
                (pg->draw_arrays_length ? pg->draw_arrays_start[0] + v :
                                          UINT32_MAX);
            uint64_t off = attr->offset + (uint64_t)element * attr->stride;

            if (element != UINT32_MAX && base != NULL &&
                off + attr->size * comps <= len) {
                p = base + off;
            }
        }
        if (p == NULL) {
            continue;
        }
        g_string_append(line, "(");
        for (unsigned int c = 0; c < comps && c < 4; c++) {
            if (attr->format == NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F) {
                float f;

                memcpy(&f, p + c * 4, 4);
                g_string_append_printf(line, "%s%g", c ? "," : "", f);
            } else {
                uint32_t u = 0;

                memcpy(&u, p + c * attr->size, MIN(attr->size, 4));
                g_string_append_printf(line, "%s#%x", c ? "," : "", u);
            }
        }
        g_string_append(line, ")");
    }
}

/* One line per draw while a capture is active. */
/* A float register's value, for the capture line. */
static double capture_reg_float(PGRAPHState *pg, unsigned int reg)
{
    uint32_t bits = pgraph_reg_r(pg, reg);
    float value;

    memcpy(&value, &bits, sizeof(value));
    return value;
}

void pgraph_d3d12_capture_draw(PGRAPHState *pg, unsigned int topology)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    GString *line;

    if (r == NULL || r->capture_frames_left <= 0 ||
        r->capture_draws >= 4000) {
        return;
    }
    r->capture_draws++;
    line = g_string_new(NULL);
    g_string_append_printf(
        line,
        "CAP draw #%u prim=%u topo=%u c=0x%" HWADDR_PRIx ":%ux%u:f%u z=0x%"
        HWADDR_PRIx ":%ux%u stype=%u ctl0=%08x ctl1=%08x ctl2=%08x "
        "ctl3=%08x blend=%08x setup=%08x csv0d=%08x comb=%08x "
        "fc=%08x/%08x shprog=%08x shctl=%08x zclip=%g/%g zco=%08x zofs=%g/%g "
        "zfmt=%u/%u",
        r->capture_draws, pg->primitive_mode, topology,
        r->color_binding ? r->color_binding->vram_addr : 0,
        r->color_binding ? r->color_binding->width : 0,
        r->color_binding ? r->color_binding->height : 0,
        r->color_binding ? (unsigned int)r->color_binding->fmt.dxgi_format :
                           0,
        r->zeta_binding ? r->zeta_binding->vram_addr : 0,
        r->zeta_binding ? r->zeta_binding->width : 0,
        r->zeta_binding ? r->zeta_binding->height : 0,
        pg->surface_type,
        pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0),
        pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1),
        pgraph_reg_r(pg, NV_PGRAPH_CONTROL_2),
        pgraph_reg_r(pg, NV_PGRAPH_CONTROL_3),
        pgraph_reg_r(pg, NV_PGRAPH_BLEND),
        pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER),
        pgraph_reg_r(pg, NV_PGRAPH_CSV0_D),
        pgraph_reg_r(pg, NV_PGRAPH_COMBINECTL),
        pgraph_reg_r(pg, NV_PGRAPH_COMBINESPECFOG0),
        pgraph_reg_r(pg, NV_PGRAPH_COMBINESPECFOG1),
        pgraph_reg_r(pg, NV_PGRAPH_SHADERPROG),
        pgraph_reg_r(pg, NV_PGRAPH_SHADERCTL),
        capture_reg_float(pg, NV_PGRAPH_ZCLIPMIN),
        capture_reg_float(pg, NV_PGRAPH_ZCLIPMAX),
        pgraph_reg_r(pg, NV_PGRAPH_ZCOMPRESSOCCLUDE),
        capture_reg_float(pg, NV_PGRAPH_ZOFFSETBIAS),
        capture_reg_float(pg, NV_PGRAPH_ZOFFSETFACTOR),
        pg->surface_shape.zeta_format, pg->surface_shape.z_format);
    for (unsigned int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        PGRAPHD3D12TextureBinding *b = r->texture_binding[i];

        if (!pgraph_is_texture_stage_active(pg, i) ||
            !pgraph_is_texture_enabled(pg, i)) {
            continue;
        }
        g_string_append_printf(
            line, " | t%u fmt=%08x ctl0=%08x ctl1=%08x addr=%08x filt=%08x",
            i, pgraph_reg_r(pg, NV_PGRAPH_TEXFMT0 + i * 4),
            pgraph_reg_r(pg, NV_PGRAPH_TEXCTL0_0 + i * 4),
            pgraph_reg_r(pg, NV_PGRAPH_TEXCTL1_0 + i * 4),
            pgraph_reg_r(pg, NV_PGRAPH_TEXADDRESS0 + i * 4),
            pgraph_reg_r(pg, NV_PGRAPH_TEXFILTER0 + i * 4));
        if (b != NULL) {
            PGRAPHD3D12SurfaceBinding *sf;
            NV2AState *d = container_of(pg, NV2AState, pgraph);
            g_autofree char *dir = g_strdup_printf(
                "%scapdump", xemu_settings_get_base_path());
            g_autofree char *path = g_strdup_printf(
                "%s/%08" PRIx64 "_%ux%u_cf%02x_p%08" PRIx64 "_%u.bin", dir,
                (uint64_t)b->vram_addr, b->shape.width, b->shape.height,
                b->shape.color_format, (uint64_t)b->palette_vram_addr,
                (unsigned int)b->palette_length);

            /* Raw guest texel and palette bytes, once per file name. */
            if (!g_file_test(path, G_FILE_TEST_EXISTS) &&
                b->vram_addr + b->length <= memory_region_size(d->vram) &&
                b->palette_vram_addr + b->palette_length <=
                    memory_region_size(d->vram)) {
                GByteArray *blob = g_byte_array_new();

                g_mkdir_with_parents(dir, 0755);
                g_byte_array_append(blob, d->vram_ptr + b->vram_addr,
                                    b->length);
                if (b->palette_length) {
                    g_byte_array_append(blob,
                                        d->vram_ptr + b->palette_vram_addr,
                                        b->palette_length);
                }
                g_file_set_contents(path, (const char *)blob->data,
                                    blob->len, NULL);
                g_byte_array_free(blob, TRUE);
            }

            QTAILQ_FOREACH(sf, &r->surfaces, entry) {
                if (sf->vram_addr < b->vram_addr + b->length &&
                    b->vram_addr < sf->vram_addr + sf->size) {
                    g_string_append_printf(
                        line, " OVL[surf 0x%" HWADDR_PRIx "+0x%" PRIx64
                        " %ux%u dirty=%d]", sf->vram_addr,
                        (uint64_t)sf->size, sf->width, sf->height,
                        sf->draw_dirty);
                }
            }
            QTAILQ_FOREACH(sf, &r->stashed_surfaces, entry) {
                if (sf->vram_addr < b->vram_addr + b->length &&
                    b->vram_addr < sf->vram_addr + sf->size) {
                    g_string_append_printf(
                        line, " OVL[stash 0x%" HWADDR_PRIx "+0x%" PRIx64
                        " %ux%u]", sf->vram_addr, (uint64_t)sf->size,
                        sf->width, sf->height);
                }
            }
            g_string_append_printf(
                line,
                " va=0x%" HWADDR_PRIx " cf=0x%x %ux%ux%u p=%u lv=%u "
                "min=%u dx=%u s2t=%d src=0x%" HWADDR_PRIx " sc=%.2f",
                b->vram_addr, b->shape.color_format, b->shape.width,
                b->shape.height, b->shape.depth, b->shape.pitch,
                b->shape.levels, b->shape.min_mipmap_level,
                (unsigned int)b->dxgi_format, b->surface_to_texture,
                b->source_surface_addr, b->scale);
        } else {
            g_string_append(line, " unbound");
        }
    }
    g_string_append_printf(
        line, " || csv1a=%08x csv1b=%08x tm0=%d vsh=%u ia=%u ib=%u da=%u "
        "ie=%u",
        pgraph_reg_r(pg, NV_PGRAPH_CSV1_A), pgraph_reg_r(pg, NV_PGRAPH_CSV1_B),
        pg->texture_matrix_enable[0],
        GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CSV0_D), NV_PGRAPH_CSV0_D_MODE),
        pg->inline_array_length, pg->inline_buffer_length,
        pg->draw_arrays_length, pg->inline_elements_length);
    if (pg->texture_matrix_enable[0]) {
        g_string_append(line, " T0MAT=");
        for (unsigned int row = 0; row < 4; row++) {
            const float *m = (const float *)
                pg->vsh_constants[NV_IGRAPH_XF_XFCTX_T0MAT + row];

            g_string_append_printf(line, "[%g,%g,%g,%g]", m[0], m[1], m[2],
                                   m[3]);
        }
    }
    capture_vertex_samples(pg, line, NV2A_VERTEX_ATTR_POSITION, "pos");
    capture_vertex_samples(pg, line, NV2A_VERTEX_ATTR_TEXTURE0, "uv0");
    capture_vertex_samples(pg, line, NV2A_VERTEX_ATTR_DIFFUSE, "d0");
    fprintf(stderr, "%s\n", line->str);
    g_string_free(line, TRUE);
}

void pgraph_d3d12_capture_clear(PGRAPHState *pg, uint32_t parameter,
                                const char *path)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;

    if (r == NULL || r->capture_frames_left <= 0) {
        return;
    }
    fprintf(stderr,
            "CAP clear param=%08x path=%s rectx=%08x recty=%08x "
            "color=%08x zs=%08x ctl0=%08x c=0x%" HWADDR_PRIx ":%ux%u:f%u "
            "z=0x%" HWADDR_PRIx " sfmt=%u\n",
            parameter, path, pgraph_reg_r(pg, NV_PGRAPH_CLEARRECTX),
            pgraph_reg_r(pg, NV_PGRAPH_CLEARRECTY),
            pgraph_reg_r(pg, NV_PGRAPH_COLORCLEARVALUE),
            pgraph_reg_r(pg, NV_PGRAPH_ZSTENCILCLEARVALUE),
            pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0),
            r->color_binding ? r->color_binding->vram_addr : 0,
            r->color_binding ? r->color_binding->width : 0,
            r->color_binding ? r->color_binding->height : 0,
            r->color_binding ?
                (unsigned int)r->color_binding->fmt.dxgi_format : 0,
            r->zeta_binding ? r->zeta_binding->vram_addr : 0,
            pg->surface_shape.color_format);
}

static void pgraph_d3d12_flip_stall(NV2AState *d)
{
    /* Custom texture settings and the running title apply between
     * frames. */
    if (pgraph_d3d12_texture_replace_update(&d->pgraph)) {
        pgraph_d3d12_flush_textures(&d->pgraph);
    }
#ifdef XEMU_UWP
    /* One frame in flight: wait only for the previous flip's fence. */
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    Error *local_err = NULL;
    int64_t wait_start;

    if (r == NULL || r->fence == NULL) {
        return;
    }

    r->perf.submit_src[5]++;
    if (r->command_list_open &&
        !pgraph_d3d12_submit_commands_async(pg, &local_err)) {
        error_free(local_err);
        pgraph_d3d12_finish(pg);
        return;
    }

    wait_start = g_get_monotonic_time();
    {
        uint64_t wait_value = r->last_flip_fence_value;

        /* PGRAPH registers stay accessible to the vCPU during the wait;
         * nothing else issues GPU work while the FIFO thread is here. */
        qemu_mutex_unlock(&d->pgraph.lock);
        pgraph_d3d12_wait_for_fence_unlocked(pg, wait_value);
        qemu_mutex_lock(&d->pgraph.lock);
    }
    pgraph_d3d12_buffers_retire_completed(r);
    pgraph_d3d12_process_deferred_releases(pg);
    r->perf.flip_wait_us += g_get_monotonic_time() - wait_start;
    r->perf.flips++;
    r->last_flip_fence_value = r->submitted_fence_value;
    if (r->evlog_frames_left > 0) {
        pgraph_d3d12_evlog(r, "flip c=0x%" HWADDR_PRIx " dt=%d",
                           r->color_binding ?
                               r->color_binding->vram_addr : 0,
                           pg->draw_time);
        r->evlog_flip_draw_time = pg->draw_time;
        pgraph_d3d12_process_color_checkpoints(r);
        pgraph_d3d12_process_depth_checkpoints(r);
        if (--r->evlog_frames_left == 0) {
            pgraph_d3d12_evlog_flush();
        }
    }
    if (r->capture_frames_left > 0) {
        r->capture_frames_left--;
        fprintf(stderr, "CAP flip left=%d pcrtc=0x%x\n",
                r->capture_frames_left, (unsigned int)d->pcrtc.start);
    }
#else
    pgraph_d3d12_finish(&d->pgraph);
#endif
}

/*
 * The triggers run with pfifo.lock and pg->lock held and the FIFO idle, so
 * all GPU work happens there. The wait hooks run after the FIFO is released
 * and must not touch renderer state.
 */
static void pgraph_d3d12_pre_savevm_trigger(NV2AState *d)
{
    /* Stashed and dirty images belong in the saved guest memory; the
     * surfaces stay resident at their current scale. */
    pgraph_d3d12_download_dirty_surfaces(d);
    pgraph_d3d12_finish(&d->pgraph);
    pgraph_d3d12_flush_reports(d);
}

static void pgraph_d3d12_pre_savevm_wait(NV2AState *d)
{
    (void)d;
}

static void pgraph_d3d12_pre_shutdown_trigger(NV2AState *d)
{
    pgraph_d3d12_surface_flush(d);
    pgraph_d3d12_finish(&d->pgraph);
    pgraph_d3d12_flush_reports(d);
    pgraph_d3d12_shader_cache_flush(&d->pgraph, true);
}

static void pgraph_d3d12_pre_shutdown_wait(NV2AState *d)
{
    (void)d;
}

static void pgraph_d3d12_set_surface_scale_factor(NV2AState *d,
                                                  unsigned int scale)
{
    PGRAPHD3D12State *r = d->pgraph.d3d12_renderer_state;

    g_config.display.quality.surface_scale =
        pgraph_d3d12_clamp_surface_scale(r, scale);

    /* The flush handler applies the new scale on the pgraph thread. */
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
    r->surface_scale_flush_pending = true;
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

    /* Tell the user when a lower scale than requested is in effect. */
    if (MAX(d->pgraph.surface_scale_factor, 1) < MAX(scale, 1)) {
        g_autofree char *msg = g_strdup_printf(
            "Internal resolution %ux needs more graphics memory than is "
            "available; using %ux",
            MAX(scale, 1), MAX(d->pgraph.surface_scale_factor, 1));
        xemu_queue_notification(msg);
    }
}

static unsigned int pgraph_d3d12_get_surface_scale_factor(NV2AState *d)
{
    return MAX(d->pgraph.surface_scale_factor, 1);
}

#ifdef CONFIG_OPENGL
static bool d3d12_display_compile_gl_stage(GLuint shader, const char *name)
{
    GLint ok = GL_FALSE;

    glCompileShader(shader);
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok) {
        return true;
    }

    GLint log_len = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_len);
    if (log_len > 1) {
        g_autofree char *log = g_malloc0(log_len);
        glGetShaderInfoLog(shader, log_len, NULL, log);
        fprintf(stderr,
                "Direct3D 12 renderer: display GL bridge failed to compile "
                "%s shader: %s\n",
                name, log);
    } else {
        fprintf(stderr,
                "Direct3D 12 renderer: display GL bridge failed to compile "
                "%s shader with no driver log\n",
                name);
    }

    return false;
}

static bool d3d12_display_ensure_gl_objects(PGRAPHState *pg)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    static const char *vs =
        "#version 330\n"
        "void main()\n"
        "{\n"
        "    float x = -1.0 + float((gl_VertexID & 1) << 2);\n"
        "    float y = -1.0 + float((gl_VertexID & 2) << 1);\n"
        "    gl_Position = vec4(x, y, 0, 1);\n"
        "}\n";
    static const char *fs =
        "#version 330\n"
        "uniform sampler2D tex;\n"
        "uniform bool pvideo_enable;\n"
        "uniform sampler2D pvideo_tex;\n"
        "uniform vec2 pvideo_in_pos;\n"
        "uniform vec4 pvideo_pos;\n"
        "uniform vec3 pvideo_scale;\n"
        "uniform bool pvideo_color_key_enable;\n"
        "uniform vec3 pvideo_color_key;\n"
        "uniform vec2 display_size;\n"
        "uniform float line_offset;\n"
        "layout(location = 0) out vec4 out_Color;\n"
        "void main()\n"
        "{\n"
        "    vec2 texCoord = gl_FragCoord.xy / display_size;\n"
        "    float rel = display_size.y / textureSize(tex, 0).y / "
        "line_offset;\n"
        "    texCoord.y = rel * (1.0 - texCoord.y);\n"
        "    out_Color = texture(tex, texCoord);\n"
        "    if (pvideo_enable) {\n"
        "        vec2 screenCoord = gl_FragCoord.xy - 0.5;\n"
        "        vec4 output_region = vec4(pvideo_pos.xy, "
        "pvideo_pos.xy + pvideo_pos.zw);\n"
        "        bvec4 clip = bvec4(lessThan(screenCoord, "
        "output_region.xy), greaterThan(screenCoord, output_region.zw));\n"
        "        if (!any(clip) && (!pvideo_color_key_enable || "
        "out_Color.rgb == pvideo_color_key)) {\n"
        "            vec2 out_xy = (screenCoord - pvideo_pos.xy) * "
        "pvideo_scale.z;\n"
        "            vec2 in_st = (pvideo_in_pos + out_xy * "
        "pvideo_scale.xy) / textureSize(pvideo_tex, 0);\n"
        "            in_st.y *= -1.0;\n"
        "            out_Color = texture(pvideo_tex, in_st);\n"
        "        }\n"
        "    }\n"
        "}\n";

    if (r->gl_display_program == 0) {
        GLuint vertex_shader = glCreateShader(GL_VERTEX_SHADER);
        GLuint fragment_shader = glCreateShader(GL_FRAGMENT_SHADER);
        GLuint program = 0;

        glShaderSource(vertex_shader, 1, &vs, NULL);
        glShaderSource(fragment_shader, 1, &fs, NULL);
        if (!d3d12_display_compile_gl_stage(vertex_shader, "vertex") ||
            !d3d12_display_compile_gl_stage(fragment_shader, "fragment")) {
            glDeleteShader(vertex_shader);
            glDeleteShader(fragment_shader);
            return false;
        }

        program = glCreateProgram();
        glAttachShader(program, vertex_shader);
        glAttachShader(program, fragment_shader);
        glLinkProgram(program);
        glDeleteShader(vertex_shader);
        glDeleteShader(fragment_shader);

        GLint linked = GL_FALSE;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        if (!linked) {
            GLint log_len = 0;
            glGetProgramiv(program, GL_INFO_LOG_LENGTH, &log_len);
            if (log_len > 1) {
                g_autofree char *log = g_malloc0(log_len);
                glGetProgramInfoLog(program, log_len, NULL, log);
                fprintf(stderr,
                        "Direct3D 12 renderer: display GL bridge failed to "
                        "link display shader: %s\n",
                        log);
            } else {
                fprintf(stderr,
                        "Direct3D 12 renderer: display GL bridge failed to "
                        "link display shader with no driver log\n");
            }
            glDeleteProgram(program);
            return false;
        }

        r->gl_display_program = program;
        r->gl_display_tex_loc = glGetUniformLocation(program, "tex");
        r->gl_display_size_loc = glGetUniformLocation(program, "display_size");
        r->gl_display_line_offset_loc =
            glGetUniformLocation(program, "line_offset");
        r->gl_display_pvideo_enable_loc =
            glGetUniformLocation(program, "pvideo_enable");
        r->gl_display_pvideo_tex_loc =
            glGetUniformLocation(program, "pvideo_tex");
        r->gl_display_pvideo_in_pos_loc =
            glGetUniformLocation(program, "pvideo_in_pos");
        r->gl_display_pvideo_pos_loc =
            glGetUniformLocation(program, "pvideo_pos");
        r->gl_display_pvideo_scale_loc =
            glGetUniformLocation(program, "pvideo_scale");
        r->gl_display_pvideo_color_key_enable_loc =
            glGetUniformLocation(program, "pvideo_color_key_enable");
        r->gl_display_pvideo_color_key_loc =
            glGetUniformLocation(program, "pvideo_color_key");
    }

    if (r->gl_display_source_texture == 0) {
        GLuint texture = 0;
        glGenTextures(1, &texture);
        r->gl_display_source_texture = texture;
    }

    if (r->gl_display_texture == 0) {
        GLuint texture = 0;
        glGenTextures(1, &texture);
        r->gl_display_texture = texture;
    }

    if (r->gl_display_pvideo_texture == 0) {
        GLuint texture = 0;
        glGenTextures(1, &texture);
        r->gl_display_pvideo_texture = texture;
    }

    if (r->gl_display_fbo == 0) {
        GLuint fbo = 0;
        glGenFramebuffers(1, &fbo);
        r->gl_display_fbo = fbo;
    }

    if (r->gl_display_vao == 0) {
        GLuint vao = 0;
        glGenVertexArrays(1, &vao);
        r->gl_display_vao = vao;
    }

    if (r->gl_display_vbo == 0) {
        GLuint vbo = 0;
        glGenBuffers(1, &vbo);
        r->gl_display_vbo = vbo;
        glBindBuffer(GL_ARRAY_BUFFER, r->gl_display_vbo);
        glBufferData(GL_ARRAY_BUFFER, 0, NULL, GL_STATIC_DRAW);
    }

    if (glGetError() != GL_NO_ERROR) {
        pgraph_d3d12_warn_once(
            pg, PGRAPH_D3D12_DIAG_DISPLAY_DOWNLOAD,
            "display GL bridge object creation failed: source_texture=%u, "
            "display_texture=%u, pvideo_texture=%u, fbo=%u, vao=%u, "
            "vbo=%u, program=%u",
            r->gl_display_source_texture, r->gl_display_texture,
            r->gl_display_pvideo_texture, r->gl_display_fbo,
            r->gl_display_vao, r->gl_display_vbo, r->gl_display_program);
        return false;
    }

    return true;
}

static bool display_gl_format(PGRAPHState *pg, PGRAPHD3D12State *r,
                              GLenum *internal_format, GLenum *format,
                              GLenum *type)
{
    switch (r->display_surface_color_format) {
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_Z1R5G5B5:
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_O1R5G5B5:
        *internal_format = GL_RGB5_A1;
        *format = GL_BGRA;
        *type = GL_UNSIGNED_SHORT_1_5_5_5_REV;
        return true;
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5:
        *internal_format = GL_RGB565;
        *format = GL_RGB;
        *type = GL_UNSIGNED_SHORT_5_6_5;
        return true;
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_Z8R8G8B8:
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8:
        *internal_format = GL_RGBA8;
        *format = GL_BGRA;
        *type = GL_UNSIGNED_INT_8_8_8_8_REV;
        return true;
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_B8:
        *internal_format = GL_R8;
        *format = GL_RED;
        *type = GL_UNSIGNED_BYTE;
        return true;
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_G8B8:
        *internal_format = GL_RG8;
        *format = GL_RG;
        *type = GL_UNSIGNED_BYTE;
        return true;
    default:
        pgraph_d3d12_warn_once(
            pg, PGRAPH_D3D12_DIAG_DISPLAY_DOWNLOAD,
            "display GL upload has no mapping for color_format=0x%x, "
            "width=%u, height=%u, pitch=%u, bytes_per_pixel=%u",
            r->display_surface_color_format, r->display_surface_width,
            r->display_surface_height, r->display_surface_pitch,
            r->display_surface_bytes_per_pixel);
        return false;
    }
}

#endif

static void convert_pvideo_cr8yb8cb8ya8_into(const uint8_t *data,
                                             unsigned int width,
                                             unsigned int height,
                                             unsigned int pitch,
                                             uint8_t *converted_data)
{
    for (unsigned int y = 0; y < height; y++) {
        const uint8_t *line = data + (uint64_t)y * pitch;
        uint32_t row_offset = y * width;

        unsigned int x = 0;

        /* YUY2 shares Cb/Cr per pixel pair; decode both at once. */
        for (; x + 1 < width; x += 2) {
            const uint8_t *packed = line + (uint64_t)x * 2;
            uint8_t *pixel0 = converted_data +
                              ((uint64_t)row_offset + x) * 4;
            uint8_t *pixel1 = pixel0 + 4;
            int c0 = (int)packed[0] - 16;
            int d = (int)packed[1] - 128;
            int c1 = (int)packed[2] - 16;
            int e = (int)packed[3] - 128;
            int red = 409 * e + 128;
            int green = -100 * d - 208 * e + 128;
            int blue = 516 * d + 128;

            pixel0[0] = cliptobyte((298 * c0 + red) >> 8);
            pixel0[1] = cliptobyte((298 * c0 + green) >> 8);
            pixel0[2] = cliptobyte((298 * c0 + blue) >> 8);
            pixel0[3] = 255;
            pixel1[0] = cliptobyte((298 * c1 + red) >> 8);
            pixel1[1] = cliptobyte((298 * c1 + green) >> 8);
            pixel1[2] = cliptobyte((298 * c1 + blue) >> 8);
            pixel1[3] = 255;
        }
        if (x < width) {
            uint8_t *pixel = converted_data +
                             ((uint64_t)row_offset + x) * 4;

            convert_yuy2_to_rgb(line, x, &pixel[0], &pixel[1], &pixel[2]);
            pixel[3] = 255;
        }
    }
}

#ifdef CONFIG_OPENGL
static uint8_t *convert_pvideo_cr8yb8cb8ya8(const uint8_t *data,
                                            unsigned int width,
                                            unsigned int height,
                                            unsigned int pitch)
{
    uint8_t *converted_data = g_malloc((uint64_t)width * height * 4);

    convert_pvideo_cr8yb8cb8ya8_into(data, width, height, pitch,
                                     converted_data);
    return converted_data;
}
#endif

static float pvideo_calculate_scale(unsigned int din_dout,
                                    unsigned int output_size)
{
    float calculated_in = din_dout * (output_size - 1);
    calculated_in = floorf(calculated_in / (1 << 20) + 0.5f);
    return (calculated_in + 1.0f) / output_size;
}

static void report_pvideo_overlay(NV2AState *d, hwaddr base, hwaddr offset,
                                  int in_width, int in_height, int in_pitch,
                                  unsigned int out_x, unsigned int out_y,
                                  unsigned int out_width,
                                  unsigned int out_height, float scale_x,
                                  float scale_y, bool color_key_enabled,
                                  unsigned int color_key)
{
    PGRAPHD3D12State *r = d->pgraph.d3d12_renderer_state;
    uint32_t size_in = d->pvideo.regs[NV_PVIDEO_SIZE_IN];
    uint32_t size_out = d->pvideo.regs[NV_PVIDEO_SIZE_OUT];

    if (!pgraph_d3d12_verbose_reports(&d->pgraph)) {
        return;
    }

    if (r->reported_pvideo_overlay &&
        r->reported_pvideo_base == base &&
        r->reported_pvideo_offset == offset &&
        r->reported_pvideo_size_in == size_in &&
        r->reported_pvideo_size_out == size_out) {
        return;
    }

    r->reported_pvideo_overlay = true;
    r->reported_pvideo_base = base;
    r->reported_pvideo_offset = offset;
    r->reported_pvideo_size_in = size_in;
    r->reported_pvideo_size_out = size_out;

    fprintf(stderr,
            "Direct3D 12 renderer: PVIDEO overlay active: "
            "base=0x%" HWADDR_PRIx ", offset=0x%" HWADDR_PRIx
            ", source=0x%" HWADDR_PRIx "..0x%" HWADDR_PRIx
            " %dx%d pitch=%d, output=%u,%u %ux%u, "
            "scale=(%f,%f), color_key_enable=%d, color_key=0x%06x, "
            "format=0x%x, buffer=0x%08x\n",
            base, offset, base + offset,
            base + offset + (uint64_t)in_pitch * in_height,
            in_width, in_height, in_pitch, out_x, out_y, out_width,
            out_height, scale_x, scale_y, color_key_enabled, color_key,
            GET_MASK(d->pvideo.regs[NV_PVIDEO_FORMAT],
                     NV_PVIDEO_FORMAT_COLOR),
            d->pvideo.regs[NV_PVIDEO_BUFFER]);
}

static bool pvideo_source_in_bounds(NV2AState *d, hwaddr base, hwaddr offset,
                                    hwaddr limit, uint64_t source_size)
{
    uint64_t vram_size = memory_region_size(d->vram);
    hwaddr source_addr;

    if (offset > limit || source_size > limit - offset) {
        return false;
    }
    if (base > vram_size || offset > vram_size - base) {
        return false;
    }

    source_addr = base + offset;
    return source_size <= vram_size - source_addr;
}

static uint64_t pvideo_yuy2_required_row_bytes(unsigned int width)
{
    return (((uint64_t)width + 1u) & ~1ull) * 2u;
}

#ifdef CONFIG_OPENGL
static void d3d12_display_bind_pvideo_overlay(NV2AState *d,
                                              uint32_t display_height)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    bool enabled =
        (d->pvideo.regs[NV_PVIDEO_BUFFER] & NV_PVIDEO_BUFFER_0_USE) &&
        d->pvideo.regs[NV_PVIDEO_SIZE_IN] != 0xFFFFFFFF;

    glUniform1i(r->gl_display_pvideo_enable_loc, enabled);
    if (!enabled) {
        return;
    }

    hwaddr base = d->pvideo.regs[NV_PVIDEO_BASE];
    hwaddr limit = d->pvideo.regs[NV_PVIDEO_LIMIT];
    hwaddr offset = d->pvideo.regs[NV_PVIDEO_OFFSET];
    int in_width =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_SIZE_IN],
                 NV_PVIDEO_SIZE_IN_WIDTH);
    int in_height =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_SIZE_IN],
                 NV_PVIDEO_SIZE_IN_HEIGHT);
    int in_s =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_POINT_IN],
                 NV_PVIDEO_POINT_IN_S);
    int in_t =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_POINT_IN],
                 NV_PVIDEO_POINT_IN_T);
    int in_pitch =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_FORMAT],
                 NV_PVIDEO_FORMAT_PITCH);
    int in_color =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_FORMAT],
                 NV_PVIDEO_FORMAT_COLOR);
    unsigned int out_width =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_SIZE_OUT],
                 NV_PVIDEO_SIZE_OUT_WIDTH);
    unsigned int out_height =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_SIZE_OUT],
                 NV_PVIDEO_SIZE_OUT_HEIGHT);
    unsigned int out_x =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_POINT_OUT],
                 NV_PVIDEO_POINT_OUT_X);
    unsigned int out_y =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_POINT_OUT],
                 NV_PVIDEO_POINT_OUT_Y);
    unsigned int ds_dx = d->pvideo.regs[NV_PVIDEO_DS_DX];
    unsigned int dt_dy = d->pvideo.regs[NV_PVIDEO_DT_DY];
    float scale_x = ds_dx == NV_PVIDEO_DIN_DOUT_UNITY ?
        1.0f : pvideo_calculate_scale(ds_dx, out_width);
    float scale_y = dt_dy == NV_PVIDEO_DIN_DOUT_UNITY ?
        1.0f : pvideo_calculate_scale(dt_dy, out_height);
    uint64_t required_row_bytes;
    uint64_t source_size;
    g_autofree uint8_t *tex_rgba = NULL;

    if (in_width <= 0 || in_height <= 0 || in_pitch <= 0 ||
        out_width == 0 || out_height == 0) {
        glUniform1i(r->gl_display_pvideo_enable_loc, false);
        return;
    }

    if (in_color != NV_PVIDEO_FORMAT_COLOR_LE_CR8YB8CB8YA8) {
        pgraph_d3d12_warn_once(
            pg, PGRAPH_D3D12_DIAG_PVIDEO_FORMAT,
            "PVIDEO overlay disabled: unsupported color format=0x%x, "
            "size_in=0x%08x, size_out=0x%08x, format=0x%08x",
            in_color, d->pvideo.regs[NV_PVIDEO_SIZE_IN],
            d->pvideo.regs[NV_PVIDEO_SIZE_OUT],
            d->pvideo.regs[NV_PVIDEO_FORMAT]);
        glUniform1i(r->gl_display_pvideo_enable_loc, false);
        return;
    }

    if ((unsigned int)in_width > out_width) {
        in_width = floorf((float)out_width * scale_x + 0.5f);
    }
    if ((unsigned int)in_height > out_height) {
        in_height = floorf((float)out_height * scale_y + 0.5f);
    }

    if (in_width <= 0 || in_height <= 0) {
        glUniform1i(r->gl_display_pvideo_enable_loc, false);
        return;
    }

    required_row_bytes = pvideo_yuy2_required_row_bytes((uint32_t)in_width);
    if ((uint64_t)in_pitch < required_row_bytes) {
        pgraph_d3d12_warn_once(
            pg, PGRAPH_D3D12_DIAG_PVIDEO_PITCH,
            "PVIDEO overlay disabled: pitch too small for YUY2 source row: "
            "base=0x%" HWADDR_PRIx ", offset=0x%" HWADDR_PRIx
            ", limit=0x%" HWADDR_PRIx ", in=%dx%d, pitch=%d, "
            "required_row_bytes=%" PRIu64 ", format=0x%08x",
            base, offset, limit, in_width, in_height, in_pitch,
            required_row_bytes, d->pvideo.regs[NV_PVIDEO_FORMAT]);
        glUniform1i(r->gl_display_pvideo_enable_loc, false);
        return;
    }

    source_size = (uint64_t)in_pitch * (uint32_t)in_height;
    if (!pvideo_source_in_bounds(d, base, offset, limit, source_size)) {
        pgraph_d3d12_warn_once(
            pg, PGRAPH_D3D12_DIAG_PVIDEO_BOUNDS,
            "PVIDEO overlay disabled: source out of VRAM bounds: "
            "base=0x%" HWADDR_PRIx ", offset=0x%" HWADDR_PRIx
            ", limit=0x%" HWADDR_PRIx ", source_size=0x%" PRIx64
            ", vram_size=0x%" PRIx64 ", in=%dx%d pitch=%d",
            base, offset, limit, source_size,
            (uint64_t)memory_region_size(d->vram), in_width, in_height,
            in_pitch);
        glUniform1i(r->gl_display_pvideo_enable_loc, false);
        return;
    }

    pgraph_apply_scaling_factor(pg, &out_x, &out_y);
    pgraph_apply_scaling_factor(pg, &out_width, &out_height);

    out_y = MAX((int)display_height - 1 - (int)(out_y + out_height), 0);

    unsigned int color_key_enabled =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_FORMAT],
                 NV_PVIDEO_FORMAT_DISPLAY);
    unsigned int color_key = d->pvideo.regs[NV_PVIDEO_COLOR_KEY] & 0xFFFFFF;

    report_pvideo_overlay(d, base, offset, in_width, in_height, in_pitch,
                          out_x, out_y, out_width, out_height, scale_x,
                          scale_y, color_key_enabled, color_key);

    tex_rgba = convert_pvideo_cr8yb8cb8ya8(
        d->vram_ptr + base + offset, in_width, in_height, in_pitch);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, r->gl_display_pvideo_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, in_width, in_height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, tex_rgba);

    glUniform1i(r->gl_display_pvideo_tex_loc, 1);
    glUniform2f(r->gl_display_pvideo_in_pos_loc, in_s / 16.0f, in_t / 8.0f);
    glUniform4f(r->gl_display_pvideo_pos_loc,
                out_x, out_y, out_width, out_height);
    glUniform3f(r->gl_display_pvideo_scale_loc,
                scale_x, scale_y, 1.0f / MAX(pg->surface_scale_factor, 1));
    glUniform1i(r->gl_display_pvideo_color_key_enable_loc,
                color_key_enabled);
    glUniform3f(r->gl_display_pvideo_color_key_loc,
                GET_MASK(color_key, NV_PVIDEO_COLOR_KEY_RED) / 255.0f,
                GET_MASK(color_key, NV_PVIDEO_COLOR_KEY_GREEN) / 255.0f,
                GET_MASK(color_key, NV_PVIDEO_COLOR_KEY_BLUE) / 255.0f);
}
#endif

static void pgraph_d3d12_fill_display_frame_pvideo(NV2AState *d,
                                                   NV2ADisplayFrame *frame)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    bool enabled =
        (d->pvideo.regs[NV_PVIDEO_BUFFER] & NV_PVIDEO_BUFFER_0_USE) &&
        d->pvideo.regs[NV_PVIDEO_SIZE_IN] != 0xFFFFFFFF;

    frame->pvideo.enabled = false;
    if (!enabled) {
        /* A stopped overlay may be rewritten before it is re-enabled. */
        r->display_pvideo_cache_valid = false;
        return;
    }

    hwaddr base = d->pvideo.regs[NV_PVIDEO_BASE];
    hwaddr limit = d->pvideo.regs[NV_PVIDEO_LIMIT];
    hwaddr offset = d->pvideo.regs[NV_PVIDEO_OFFSET];
    int in_width =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_SIZE_IN],
                 NV_PVIDEO_SIZE_IN_WIDTH);
    int in_height =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_SIZE_IN],
                 NV_PVIDEO_SIZE_IN_HEIGHT);
    int in_s =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_POINT_IN],
                 NV_PVIDEO_POINT_IN_S);
    int in_t =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_POINT_IN],
                 NV_PVIDEO_POINT_IN_T);
    int in_pitch =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_FORMAT],
                 NV_PVIDEO_FORMAT_PITCH);
    int in_color =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_FORMAT],
                 NV_PVIDEO_FORMAT_COLOR);
    unsigned int out_width =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_SIZE_OUT],
                 NV_PVIDEO_SIZE_OUT_WIDTH);
    unsigned int out_height =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_SIZE_OUT],
                 NV_PVIDEO_SIZE_OUT_HEIGHT);
    unsigned int out_x =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_POINT_OUT],
                 NV_PVIDEO_POINT_OUT_X);
    unsigned int out_y =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_POINT_OUT],
                 NV_PVIDEO_POINT_OUT_Y);
    unsigned int ds_dx = d->pvideo.regs[NV_PVIDEO_DS_DX];
    unsigned int dt_dy = d->pvideo.regs[NV_PVIDEO_DT_DY];
    float scale_x = ds_dx == NV_PVIDEO_DIN_DOUT_UNITY ?
        1.0f : pvideo_calculate_scale(ds_dx, out_width);
    float scale_y = dt_dy == NV_PVIDEO_DIN_DOUT_UNITY ?
        1.0f : pvideo_calculate_scale(dt_dy, out_height);
    uint64_t required_row_bytes;
    uint64_t source_size;
    uint64_t rgba_size;
    uint64_t generation;
    uint64_t source_hash;

    if (in_width <= 0 || in_height <= 0 || in_pitch <= 0 ||
        out_width == 0 || out_height == 0) {
        r->display_pvideo_cache_valid = false;
        return;
    }

    if (in_color != NV_PVIDEO_FORMAT_COLOR_LE_CR8YB8CB8YA8) {
        pgraph_d3d12_warn_once(
            pg, PGRAPH_D3D12_DIAG_PVIDEO_FORMAT,
            "PVIDEO overlay disabled: unsupported color format=0x%x, "
            "size_in=0x%08x, size_out=0x%08x, format=0x%08x",
            in_color, d->pvideo.regs[NV_PVIDEO_SIZE_IN],
            d->pvideo.regs[NV_PVIDEO_SIZE_OUT],
            d->pvideo.regs[NV_PVIDEO_FORMAT]);
        r->display_pvideo_cache_valid = false;
        return;
    }

    if ((unsigned int)in_width > out_width) {
        in_width = floorf((float)out_width * scale_x + 0.5f);
    }
    if ((unsigned int)in_height > out_height) {
        in_height = floorf((float)out_height * scale_y + 0.5f);
    }

    if (in_width <= 0 || in_height <= 0) {
        r->display_pvideo_cache_valid = false;
        return;
    }

    required_row_bytes = pvideo_yuy2_required_row_bytes((uint32_t)in_width);
    if ((uint64_t)in_pitch < required_row_bytes) {
        pgraph_d3d12_warn_once(
            pg, PGRAPH_D3D12_DIAG_PVIDEO_PITCH,
            "PVIDEO overlay disabled: pitch too small for YUY2 source row: "
            "base=0x%" HWADDR_PRIx ", offset=0x%" HWADDR_PRIx
            ", limit=0x%" HWADDR_PRIx ", in=%dx%d, pitch=%d, "
            "required_row_bytes=%" PRIu64 ", format=0x%08x",
            base, offset, limit, in_width, in_height, in_pitch,
            required_row_bytes, d->pvideo.regs[NV_PVIDEO_FORMAT]);
        r->display_pvideo_cache_valid = false;
        return;
    }

    source_size = (uint64_t)in_pitch * (uint32_t)in_height;
    if (!pvideo_source_in_bounds(d, base, offset, limit, source_size)) {
        pgraph_d3d12_warn_once(
            pg, PGRAPH_D3D12_DIAG_PVIDEO_BOUNDS,
            "PVIDEO overlay disabled: source out of VRAM bounds: "
            "base=0x%" HWADDR_PRIx ", offset=0x%" HWADDR_PRIx
            ", limit=0x%" HWADDR_PRIx ", source_size=0x%" PRIx64
            ", vram_size=0x%" PRIx64 ", in=%dx%d pitch=%d",
            base, offset, limit, source_size,
            (uint64_t)memory_region_size(d->vram), in_width, in_height,
            in_pitch);
        r->display_pvideo_cache_valid = false;
        return;
    }

    /* Overlay placement is in guest coordinates; the presenter scales it. */

    unsigned int color_key_enabled =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_FORMAT],
                 NV_PVIDEO_FORMAT_DISPLAY);
    unsigned int color_key = d->pvideo.regs[NV_PVIDEO_COLOR_KEY] & 0xFFFFFF;

    report_pvideo_overlay(d, base, offset, in_width, in_height, in_pitch,
                          out_x, out_y, out_width, out_height, scale_x,
                          scale_y, color_key_enabled, color_key);

    rgba_size = (uint64_t)in_width * (uint32_t)in_height * 4;
    if (rgba_size > UINT32_MAX || rgba_size > SIZE_MAX) {
        pgraph_d3d12_warn_once(
            pg, PGRAPH_D3D12_DIAG_PVIDEO_BOUNDS,
            "PVIDEO overlay disabled: converted RGBA frame is too large: "
            "in=%dx%d, rgba_size=0x%" PRIx64,
            in_width, in_height, rgba_size);
        r->display_pvideo_cache_valid = false;
        return;
    }
    if (rgba_size > r->display_pvideo_rgba_capacity) {
        r->display_pvideo_rgba =
            g_realloc(r->display_pvideo_rgba, (size_t)rgba_size);
        r->display_pvideo_rgba_capacity = (size_t)rgba_size;
    }
    generation = qatomic_read(&d->pvideo.frame_generation);
    /* The overlay shows the buffer's current bytes: in-place CPU or 2D
     * writes need no new NV_PVIDEO_BUFFER submission. */
    source_hash = fast_hash(
        d->vram_ptr + base + offset,
        (size_t)((uint64_t)(in_height - 1) * (uint32_t)in_pitch +
                 required_row_bytes));
    if (!r->display_pvideo_cache_valid ||
        r->display_pvideo_generation != generation ||
        r->display_pvideo_hash != source_hash ||
        r->display_pvideo_base != base ||
        r->display_pvideo_offset != offset ||
        r->display_pvideo_width != (uint32_t)in_width ||
        r->display_pvideo_height != (uint32_t)in_height ||
        r->display_pvideo_pitch != (uint32_t)in_pitch) {
        int64_t convert_start = g_get_monotonic_time();

        convert_pvideo_cr8yb8cb8ya8_into(
            d->vram_ptr + base + offset, in_width, in_height, in_pitch,
            r->display_pvideo_rgba);
        r->perf.pvideo_convert_us +=
            g_get_monotonic_time() - convert_start;
        r->perf.pvideo_conversions++;

        /* Do not cache the result if a newer frame arrived mid-conversion. */
        r->display_pvideo_cache_valid =
            qatomic_read(&d->pvideo.frame_generation) == generation;
        r->display_pvideo_generation = generation;
        r->display_pvideo_hash = source_hash;
        r->display_pvideo_base = base;
        r->display_pvideo_offset = offset;
        r->display_pvideo_width = (uint32_t)in_width;
        r->display_pvideo_height = (uint32_t)in_height;
        r->display_pvideo_pitch = (uint32_t)in_pitch;
    } else {
        r->perf.pvideo_cache_hits++;
    }

    frame->pvideo.enabled = true;
    frame->pvideo.rgba = r->display_pvideo_rgba;
    frame->pvideo.rgba_size = (uint32_t)rgba_size;
    frame->pvideo.width = (uint32_t)in_width;
    frame->pvideo.height = (uint32_t)in_height;
    frame->pvideo.in_x = in_s / 16.0f;
    frame->pvideo.in_y = in_t / 8.0f;
    frame->pvideo.scale_x = scale_x;
    frame->pvideo.scale_y = scale_y;
    frame->pvideo.out_x = out_x;
    frame->pvideo.out_y = out_y;
    frame->pvideo.out_width = out_width;
    frame->pvideo.out_height = out_height;
    frame->pvideo.color_key_enabled = color_key_enabled;
    frame->pvideo.color_key = color_key;
}

#ifdef CONFIG_OPENGL
static int pgraph_d3d12_upload_display_gl_texture(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = d->pgraph.d3d12_renderer_state;
    GLenum internal_format;
    GLenum format;
    GLenum type;
    uint32_t source_width;
    uint32_t source_height;
    uint32_t display_width;
    uint32_t display_height;
    uint32_t bytes_per_pixel;
    uint32_t row_bytes;
    uint64_t source_size;
    uint32_t visible_width = 0;
    const uint8_t *source;
    g_autofree uint8_t *tight_rows = NULL;
    VGADisplayParams vga_display_params;
    float line_offset;
    GLint prev_unpack_alignment;
    GLint prev_unpack_row_length;
    GLint prev_unpack_skip_pixels;
    GLint prev_unpack_skip_rows;

    if (r == NULL || !r->display_surface_valid) {
        return 0;
    }

    source_width = r->display_surface_width;
    source_height = r->display_surface_height;
    bytes_per_pixel = r->display_surface_bytes_per_pixel;
    if (!source_width || !source_height || !bytes_per_pixel ||
        !display_gl_format(pg, r, &internal_format, &format, &type) ||
        !d3d12_display_ensure_gl_objects(pg)) {
        return 0;
    }

    d->vga.get_resolution(&d->vga, (int *)&display_width,
                          (int *)&display_height);
    d->vga.get_params(&d->vga, &vga_display_params);
    if (d->vga.cr[NV_PRMCIO_INTERLACE_MODE] !=
        NV_PRMCIO_INTERLACE_MODE_DISABLED) {
        display_height *= 2;
    }
    pgraph_apply_scaling_factor(pg, &display_width, &display_height);

    row_bytes = source_width * bytes_per_pixel;
    source_size = (uint64_t)r->display_surface_pitch * source_height;
    if (r->display_surface_addr > memory_region_size(d->vram) ||
        source_size > memory_region_size(d->vram) - r->display_surface_addr) {
        pgraph_d3d12_warn_once(
            pg, PGRAPH_D3D12_DIAG_DISPLAY_DOWNLOAD,
            "display GL upload source is out of VRAM bounds: addr=0x%"
            HWADDR_PRIx "..0x%" HWADDR_PRIx ", vram_size=0x%" PRIx64
            ", width=%u, height=%u, pitch=%u, bytes_per_pixel=%u, "
            "color_format=0x%x",
            r->display_surface_addr, r->display_surface_addr + source_size,
            (uint64_t)memory_region_size(d->vram), source_width,
            source_height,
            r->display_surface_pitch, bytes_per_pixel,
            r->display_surface_color_format);
        return 0;
    }

    source = d->vram_ptr + r->display_surface_addr;
    tight_rows = g_malloc((uint64_t)row_bytes * source_height);
    for (uint32_t y = 0; y < source_height; y++) {
        memcpy(tight_rows + (uint64_t)y * row_bytes,
               source + (uint64_t)y * r->display_surface_pitch, row_bytes);
    }
    report_display_source_sample(d, tight_rows, source_width, source_height,
                                 row_bytes, bytes_per_pixel, visible_width);

    glBindTexture(GL_TEXTURE_2D, r->gl_display_source_texture);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &prev_unpack_alignment);
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &prev_unpack_row_length);
    glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &prev_unpack_skip_pixels);
    glGetIntegerv(GL_UNPACK_SKIP_ROWS, &prev_unpack_skip_rows);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);

    if (r->gl_display_source_width != source_width ||
        r->gl_display_source_height != source_height ||
        r->gl_display_source_color_format !=
            r->display_surface_color_format) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, internal_format, source_width,
                     source_height, 0, format, type, tight_rows);
        r->gl_display_source_width = source_width;
        r->gl_display_source_height = source_height;
        r->gl_display_source_color_format =
            r->display_surface_color_format;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, source_width,
                        source_height, format, type, tight_rows);
    }

    glPixelStorei(GL_UNPACK_ALIGNMENT, prev_unpack_alignment);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, prev_unpack_row_length);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, prev_unpack_skip_pixels);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, prev_unpack_skip_rows);

    if (glGetError() != GL_NO_ERROR) {
        pgraph_d3d12_warn_once(
            pg, PGRAPH_D3D12_DIAG_DISPLAY_DOWNLOAD,
            "display GL source upload failed: texture=%u, width=%u, "
            "height=%u, pitch=%u, row_bytes=%u, bytes_per_pixel=%u, "
            "color_format=0x%x",
            r->gl_display_source_texture, source_width, source_height,
            r->display_surface_pitch, row_bytes,
            bytes_per_pixel, r->display_surface_color_format);
        return 0;
    }

    report_display_gl_texture_sample(d, "source-upload",
                                     r->gl_display_source_texture,
                                     source_width, source_height);

    if (!display_width || !display_height) {
        pgraph_d3d12_warn_once(
            pg, PGRAPH_D3D12_DIAG_DISPLAY_DOWNLOAD,
            "display GL bridge got zero VGA output size: width=%u, "
            "height=%u, pcrtc.start=0x%" HWADDR_PRIx
            ", vga.line_offset=%u, source=%ux%u pitch=%u format=0x%x",
            display_width, display_height, d->pcrtc.start,
            vga_display_params.line_offset, source_width, source_height,
            r->display_surface_pitch, r->display_surface_color_format);
        return 0;
    }

    line_offset = vga_display_params.line_offset ?
        (float)r->display_surface_pitch /
            (float)vga_display_params.line_offset :
        1.0f;

    glBindFramebuffer(GL_FRAMEBUFFER, r->gl_display_fbo);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, r->gl_display_texture);
    if (r->gl_display_texture_width != display_width ||
        r->gl_display_texture_height != display_height ||
        r->gl_display_texture_color_format !=
            r->display_surface_color_format) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, display_width,
                     display_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        r->gl_display_texture_width = display_width;
        r->gl_display_texture_height = display_height;
        r->gl_display_texture_color_format = r->display_surface_color_format;
    }

    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, r->gl_display_texture, 0);
    GLenum draw_buffers[1] = { GL_COLOR_ATTACHMENT0 };
    glDrawBuffers(1, draw_buffers);
    GLenum fbo_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (fbo_status != GL_FRAMEBUFFER_COMPLETE) {
        pgraph_d3d12_warn_once(
            pg, PGRAPH_D3D12_DIAG_DISPLAY_DOWNLOAD,
            "display GL bridge output FBO is incomplete: status=0x%x, "
            "output_texture=%u, output=%ux%u, source_texture=%u, "
            "source=%ux%u, source_format=0x%x",
            fbo_status, r->gl_display_texture, display_width, display_height,
            r->gl_display_source_texture, source_width, source_height,
            r->display_surface_color_format);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, 0, 0);
        return 0;
    }

    glBindTexture(GL_TEXTURE_2D, r->gl_display_source_texture);
    glBindVertexArray(r->gl_display_vao);
    glUseProgram(r->gl_display_program);
    glUniform1i(r->gl_display_tex_loc, 0);
    glUniform2f(r->gl_display_size_loc, display_width, display_height);
    glUniform1f(r->gl_display_line_offset_loc, line_offset);
    d3d12_display_bind_pvideo_overlay(d, display_height);
    glActiveTexture(GL_TEXTURE0);
    glViewport(0, 0, display_width, display_height);
    glColorMask(true, true, true, true);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glUseProgram(0);
    glBindVertexArray(0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, 0, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    if (glGetError() != GL_NO_ERROR) {
        pgraph_d3d12_warn_once(
            pg, PGRAPH_D3D12_DIAG_DISPLAY_DOWNLOAD,
            "display GL bridge draw failed: source_texture=%u, source=%ux%u, "
            "output_texture=%u, output=%ux%u, source_format=0x%x, "
            "line_offset=%f",
            r->gl_display_source_texture, source_width, source_height,
            r->gl_display_texture, display_width, display_height,
            r->display_surface_color_format, line_offset);
        return 0;
    }

    report_display_gl_texture_sample(d, "bridge-output",
                                     r->gl_display_texture, display_width,
                                     display_height);

    if (pgraph_d3d12_verbose_reports(pg) && !r->reported_display_gl_bridge) {
        r->reported_display_gl_bridge = true;
        fprintf(stderr,
                "Direct3D 12 renderer: display GL bridge active: "
                "source_texture=%u source=%ux%u source_addr=0x%"
                HWADDR_PRIx " source_pitch=%u source_bpp=%u "
                "source_format=0x%x, output_texture=%u output=%ux%u, "
                "pcrtc.start=0x%" HWADDR_PRIx ", vga.line_offset=%u, "
                "visible_width=%u, computed_line_offset=%f, "
                "returned_texture=bridge-output\n",
                r->gl_display_source_texture, source_width, source_height,
                r->display_surface_addr, r->display_surface_pitch,
                bytes_per_pixel, r->display_surface_color_format,
                r->gl_display_texture, display_width, display_height,
                d->pcrtc.start, vga_display_params.line_offset,
                visible_width, line_offset);
    }

    return r->gl_display_texture;
}

static void pgraph_d3d12_finalize_display_gl(PGRAPHState *pg)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;

    if (r == NULL) {
        return;
    }

    if (r->gl_display_source_texture != 0) {
        GLuint texture = r->gl_display_source_texture;
        glDeleteTextures(1, &texture);
        r->gl_display_source_texture = 0;
    }
    if (r->gl_display_texture != 0) {
        GLuint texture = r->gl_display_texture;
        glDeleteTextures(1, &texture);
        r->gl_display_texture = 0;
    }
    if (r->gl_display_pvideo_texture != 0) {
        GLuint texture = r->gl_display_pvideo_texture;
        glDeleteTextures(1, &texture);
        r->gl_display_pvideo_texture = 0;
    }
    if (r->gl_display_program != 0) {
        glDeleteProgram(r->gl_display_program);
        r->gl_display_program = 0;
    }
    if (r->gl_display_vao != 0) {
        GLuint vao = r->gl_display_vao;
        glDeleteVertexArrays(1, &vao);
        r->gl_display_vao = 0;
    }
    if (r->gl_display_vbo != 0) {
        GLuint vbo = r->gl_display_vbo;
        glDeleteBuffers(1, &vbo);
        r->gl_display_vbo = 0;
    }
    if (r->gl_display_fbo != 0) {
        GLuint fbo = r->gl_display_fbo;
        glDeleteFramebuffers(1, &fbo);
        r->gl_display_fbo = 0;
    }
}
#endif

static int pgraph_d3d12_get_framebuffer_surface(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    int surface = 0;

    if (r != NULL) {
        r->display_sync_download = true;
    }
    qemu_mutex_lock(&d->pfifo.lock);
    qemu_event_reset(&d->pgraph.sync_complete);
    qatomic_set(&pg->sync_pending, true);
    pfifo_kick(d);
    qemu_mutex_unlock(&d->pfifo.lock);
    qemu_event_wait(&d->pgraph.sync_complete);

#ifdef CONFIG_OPENGL
    surface = pgraph_d3d12_upload_display_gl_texture(d);
#endif

    return surface;
}

static bool pgraph_d3d12_fill_cpu_display_frame(NV2AState *d,
                                                NV2ADisplayFrame *frame,
                                                hwaddr addr, uint32_t width,
                                                uint32_t height,
                                                uint32_t pitch,
                                                uint32_t bytes_per_pixel,
                                                uint32_t color_format)
{
    uint64_t vram_size = memory_region_size(d->vram);
    uint64_t row_bytes;
    uint64_t source_size;

    if (frame == NULL || width == 0 || height == 0 ||
        bytes_per_pixel == 0 || pitch == 0) {
        return false;
    }

    row_bytes = (uint64_t)width * bytes_per_pixel;
    if (row_bytes > pitch || addr >= vram_size) {
        return false;
    }

    source_size = (uint64_t)(height - 1) * pitch + row_bytes;
    if (source_size > UINT32_MAX || source_size > vram_size - addr) {
        return false;
    }

    frame->type = NV2A_DISPLAY_FRAME_CPU_SURFACE;
    /* Raw PCRTC data is not rendered at the internal surface scale. */
    frame->display_scale = 1;
    frame->cpu.data = d->vram_ptr + addr;
    frame->cpu.data_size = (uint32_t)source_size;
    frame->cpu.width = width;
    frame->cpu.height = height;
    frame->cpu.pitch = pitch;
    frame->cpu.bytes_per_pixel = bytes_per_pixel;
    frame->cpu.color_format = color_format;
    frame->cpu.vram_addr = addr;
    return true;
}

static void pgraph_d3d12_report_cpu_scanout_fallback(
    NV2AState *d, const PGRAPHD3D12SurfaceBinding *surface)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;

    if (!pgraph_d3d12_verbose_reports(pg) || surface == NULL) {
        return;
    }

    if (r->reported_display_cpu_scanout_fallback &&
        r->reported_display_cpu_scanout_addr == r->display_surface_addr &&
        r->reported_display_cpu_scanout_pitch == r->display_surface_pitch &&
        r->reported_display_cpu_scanout_resource_pitch == surface->pitch) {
        return;
    }

    r->reported_display_cpu_scanout_fallback = true;
    r->reported_display_cpu_scanout_addr = r->display_surface_addr;
    r->reported_display_cpu_scanout_pitch = r->display_surface_pitch;
    r->reported_display_cpu_scanout_resource_pitch = surface->pitch;

    fprintf(stderr,
            "Direct3D 12 renderer: display frame using CPU scanout fallback "
            "because PCRTC row pitch does not match tracked render-target "
            "pitch: scanout=0x%" HWADDR_PRIx " %ux%u scanout_pitch=%u, "
            "surface=0x%" HWADDR_PRIx "..0x%" HWADDR_PRIx " %ux%u "
            "surface_pitch=%u bytes_per_pixel=%u format=%s (0x%x), "
            "source_offset=(%u,%u), draw_dirty=%d, download_pending=%d\n",
            r->display_surface_addr, r->display_surface_width,
            r->display_surface_height, r->display_surface_pitch,
            surface->vram_addr, surface->vram_addr + surface->size,
            surface->width, surface->height, surface->pitch,
            surface->fmt.bytes_per_pixel,
            display_color_format_name(surface->shape.color_format),
            surface->shape.color_format, r->display_surface_x_offset,
            r->display_surface_y_offset, surface->draw_dirty,
            surface->download_pending);
}

/* Guest bytes the video overlay reads, for writing stashes back first. */
static bool pgraph_d3d12_pvideo_source_range(NV2AState *d, hwaddr *addr,
                                             uint64_t *size)
{
    uint64_t vram_size = memory_region_size(d->vram);
    hwaddr base = d->pvideo.regs[NV_PVIDEO_BASE];
    hwaddr offset = d->pvideo.regs[NV_PVIDEO_OFFSET];
    uint64_t in_height = GET_MASK(d->pvideo.regs[NV_PVIDEO_SIZE_IN],
                                  NV_PVIDEO_SIZE_IN_HEIGHT);
    uint64_t in_pitch = GET_MASK(d->pvideo.regs[NV_PVIDEO_FORMAT],
                                 NV_PVIDEO_FORMAT_PITCH);
    hwaddr start = base + offset;

    if (!(d->pvideo.regs[NV_PVIDEO_BUFFER] & NV_PVIDEO_BUFFER_0_USE) ||
        d->pvideo.regs[NV_PVIDEO_SIZE_IN] == 0xFFFFFFFF ||
        in_height == 0 || in_pitch == 0 || start >= vram_size) {
        return false;
    }
    *addr = start;
    *size = MIN(in_pitch * in_height, vram_size - start);
    return true;
}

/*
 * Chooses what the next presented frame shows. Runs at the end of every
 * sync on the FIFO thread with pg->lock held; the presenter only ever sees
 * the snapshot resource, which lives until a later sync replaces it, or
 * guest memory.
 */
static void pgraph_d3d12_capture_display_frame(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    PGRAPHD3D12SurfaceBinding *surface = r->display_surface_binding;
    hwaddr pvideo_addr;
    uint64_t pvideo_size;

    memset(&r->display_frame, 0, sizeof(r->display_frame));
    r->display_frame.valid = true;
    r->display_frame.scale = MAX(pg->surface_scale_factor, 1);
    r->display_frame.addr = r->display_surface_addr;
    r->display_frame.width = r->display_surface_width;
    r->display_frame.height = r->display_surface_height;
    r->display_frame.pitch = r->display_surface_pitch;
    r->display_frame.x_offset = r->display_surface_x_offset;
    r->display_frame.y_offset = r->display_surface_y_offset;
    r->display_frame.bytes_per_pixel = r->display_surface_bytes_per_pixel;
    r->display_frame.color_format = r->display_surface_color_format;

    /* The overlay is composited from guest memory. */
    if (pgraph_d3d12_pvideo_source_range(d, &pvideo_addr, &pvideo_size)) {
        pgraph_d3d12_complete_readbacks_in_range(
            d, pvideo_addr, pvideo_size, PGRAPH_D3D12_STASH_TRIGGER_DISPLAY);
    }
    if (!r->display_surface_valid) {
        return;
    }
    r->display_frame.display = true;

    if (surface != NULL && surface->resource == NULL) {
        r->display_frame.valid = false;
        return;
    }
    if (surface != NULL) {
        r->display_frame.resource_pitch = surface->pitch;
    }
    if (surface != NULL && r->display_snapshot_valid &&
        r->display_snapshot_resource != NULL &&
        !(r->display_surface_pitch != 0 &&
          surface->pitch != r->display_surface_pitch)) {
        r->display_frame.resource = r->display_snapshot_resource;
        r->display_frame.resource_state = r->display_snapshot_state;
        r->display_frame.dxgi_format = r->display_snapshot_format;
        r->display_frame.resource_width = r->display_snapshot_width;
        r->display_frame.resource_height = r->display_snapshot_height;
        r->display_frame.ready_value = r->display_snapshot_ready_value;
        if (r->display_release_fence != NULL) {
            r->display_frame.release_value = ++r->display_release_next;
            r->display_snapshot_release_value =
                r->display_frame.release_value;
        }
        return;
    }

    /* Everything else presents from guest memory: a raw scanout, a pitch
     * the tracked surface does not have, or a failed snapshot. */
    if (surface != NULL) {
        if (r->display_surface_pitch != 0 &&
            surface->pitch != r->display_surface_pitch) {
            pgraph_d3d12_report_cpu_scanout_fallback(d, surface);
        }
        pgraph_d3d12_surface_download_if_dirty(d, surface);
    }
    if (r->display_surface_height != 0 && r->display_surface_pitch != 0) {
        pgraph_d3d12_complete_readbacks_in_range(
            d, r->display_surface_addr,
            (uint64_t)(r->display_surface_height - 1) *
                    r->display_surface_pitch +
                (uint64_t)r->display_surface_width *
                    r->display_surface_bytes_per_pixel,
            PGRAPH_D3D12_STASH_TRIGGER_DISPLAY);
    }
    r->display_frame.cpu = true;
}

static bool pgraph_d3d12_get_display_frame(NV2AState *d,
                                           NV2ADisplayFrame *frame)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    int display_width = 0;
    int display_height = 0;
    typeof(r->display_frame) f;

    if (frame == NULL || r == NULL) {
        return false;
    }

    memset(frame, 0, sizeof(*frame));
    frame->type = NV2A_DISPLAY_FRAME_NONE;
    frame->d3d12.device = r->device;
    frame->d3d12.command_queue = r->direct_queue;

    /* A NONE frame lets the presenter create its swapchain and HUD early. */
    if (frame->d3d12.device == NULL ||
        frame->d3d12.command_queue == NULL) {
        return false;
    }

    qemu_mutex_lock(&d->pfifo.lock);
    r->display_sync_download = false;
    qemu_event_reset(&d->pgraph.sync_complete);
    qatomic_set(&pg->sync_pending, true);
    pfifo_kick(d);
    qemu_mutex_unlock(&d->pfifo.lock);
    {
        int64_t wait_start = g_get_monotonic_time();

        qemu_event_wait(&d->pgraph.sync_complete);
        qatomic_add(&r->ui_sync_wait_us,
                    g_get_monotonic_time() - wait_start);
        qatomic_inc(&r->ui_syncs);
    }

    /* The FIFO thread runs again from here: take only the frame it chose. */
    qemu_mutex_lock(&d->pgraph.lock);
    f = r->display_frame;
    qemu_mutex_unlock(&d->pgraph.lock);

    d->vga.get_resolution(&d->vga, &display_width, &display_height);
    if (display_width > 0 && display_height > 0) {
        if (d->vga.cr[NV_PRMCIO_INTERLACE_MODE] !=
            NV_PRMCIO_INTERLACE_MODE_DISABLED) {
            display_height *= 2;
        }
        frame->display_width = (uint32_t)display_width;
        frame->display_height = (uint32_t)display_height;
        frame->display_scale = f.scale;
    }

    if (!f.valid) {
        return false;
    }
    if (!f.display) {
        return true;
    }

    if (f.cpu) {
        if (!pgraph_d3d12_fill_cpu_display_frame(
                d, frame, f.addr, f.width, f.height, f.pitch,
                f.bytes_per_pixel, f.color_format)) {
            return false;
        }
        pgraph_d3d12_fill_display_frame_pvideo(d, frame);
        return true;
    }

    frame->type = NV2A_DISPLAY_FRAME_D3D12_RESOURCE;
    if (frame->display_width == 0 || frame->display_height == 0) {
        frame->display_width = f.width;
        frame->display_height = f.height;
        frame->display_scale = f.scale;
    }
    frame->d3d12.resource = f.resource;
    frame->d3d12.resource_state = f.resource_state;
    frame->d3d12.dxgi_format = f.dxgi_format;
    frame->d3d12.resource_width = f.resource_width;
    frame->d3d12.resource_height = f.resource_height;
    frame->d3d12.resource_scale = f.scale;
    frame->d3d12.source_width = f.width;
    frame->d3d12.source_height = f.height;
    frame->d3d12.resource_pitch = f.resource_pitch;
    frame->d3d12.source_pitch = f.pitch;
    frame->d3d12.source_x = f.x_offset;
    frame->d3d12.source_y = f.y_offset;
    frame->d3d12.bytes_per_pixel = f.bytes_per_pixel;
    frame->d3d12.color_format = f.color_format;
    frame->d3d12.vram_addr = f.addr;
    frame->d3d12.ready_fence = r->fence;
    frame->d3d12.ready_value = f.ready_value;
    frame->d3d12.release_fence =
        f.release_value != 0 ? r->display_release_fence : NULL;
    frame->d3d12.release_value = f.release_value;
    pgraph_d3d12_fill_display_frame_pvideo(d, frame);

    return true;
}

static GPUProperties *pgraph_d3d12_get_gpu_properties(void)
{
    return &d3d12_gpu_properties;
}

static void pgraph_d3d12_init(NV2AState *d, Error **errp)
{
    PGRAPHState *pg = &d->pgraph;

    pg->d3d12_renderer_state = g_malloc0(sizeof(*pg->d3d12_renderer_state));
    pg->d3d12_renderer_state->pg = pg;
    pg->d3d12_renderer_state->tune_report_wait_head = true;
    /* PSO variant bits apply from the first draw, so the title's pipeline
     * list precompiles in the same form, and twintest measures that
     * precompile; so does the fallback mode, for the first draws' misses.
     * The rest wait for the first perf report. */
    pg->d3d12_renderer_state->tune_flags =
        pgraph_d3d12_tune_file_flags() &
        (PGRAPH_D3D12_TUNE_KEY_ONLY_FLAGS | PGRAPH_D3D12_TUNE_VTXFMT_STATIC |
         PGRAPH_D3D12_TUNE_TWIN_PSOS | PGRAPH_D3D12_TUNE_FALLBACK_MASK);
    if (pg->d3d12_renderer_state->tune_flags != 0) {
        fprintf(stderr, "Direct3D 12 tune: flags=0x%x\n",
                pg->d3d12_renderer_state->tune_flags);
    }
    pgraph_d3d12_init_debug_flags(pg);
    pg->surface_scale_factor = MAX(g_config.display.quality.surface_scale, 1);

    pgraph_d3d12_init_device(pg, errp);
    if (*errp) {
        pgraph_d3d12_finalize_device(pg);
        g_free(pg->d3d12_renderer_state);
        pg->d3d12_renderer_state = NULL;
        return;
    }

    /* Re-clamp now that the adapter memory budget is known. */
    pg->surface_scale_factor = pgraph_d3d12_clamp_surface_scale(
        pg->d3d12_renderer_state, pg->surface_scale_factor);
    g_config.display.quality.surface_scale = pg->surface_scale_factor;

    pgraph_d3d12_init_command_objects(pg, errp);
    if (*errp) {
        pgraph_d3d12_finalize_command_objects(pg);
        pgraph_d3d12_finalize_device(pg);
        g_free(pg->d3d12_renderer_state);
        pg->d3d12_renderer_state = NULL;
        return;
    }

    pgraph_d3d12_init_descriptor_heaps(pg, errp);
    if (*errp) {
        pgraph_d3d12_finalize_descriptor_heaps(pg);
        pgraph_d3d12_finalize_command_objects(pg);
        pgraph_d3d12_finalize_device(pg);
        g_free(pg->d3d12_renderer_state);
        pg->d3d12_renderer_state = NULL;
        return;
    }

    pgraph_d3d12_init_surfaces(pg);
    pgraph_d3d12_init_textures(d);

    pgraph_d3d12_init_buffers(d, errp);
    if (*errp) {
        pgraph_d3d12_finalize_buffers(pg);
        pgraph_d3d12_finalize_textures(pg);
        pgraph_d3d12_finalize_surfaces(pg);
        pgraph_d3d12_finalize_descriptor_heaps(pg);
        pgraph_d3d12_finalize_command_objects(pg);
        pgraph_d3d12_finalize_device(pg);
        g_free(pg->d3d12_renderer_state);
        pg->d3d12_renderer_state = NULL;
        return;
    }

    pgraph_d3d12_init_reports(pg, errp);
    if (*errp) {
        pgraph_d3d12_finalize_reports(pg);
        pgraph_d3d12_finalize_textures(pg);
        pgraph_d3d12_finalize_surfaces(pg);
        pgraph_d3d12_finalize_buffers(pg);
        pgraph_d3d12_finalize_descriptor_heaps(pg);
        pgraph_d3d12_finalize_command_objects(pg);
        pgraph_d3d12_finalize_device(pg);
        g_free(pg->d3d12_renderer_state);
        pg->d3d12_renderer_state = NULL;
        return;
    }

    pgraph_d3d12_init_pipelines(pg, errp);
    if (*errp) {
        pgraph_d3d12_finalize_pipelines(pg);
        pgraph_d3d12_finalize_reports(pg);
        pgraph_d3d12_finalize_textures(pg);
        pgraph_d3d12_finalize_surfaces(pg);
        pgraph_d3d12_finalize_buffers(pg);
        pgraph_d3d12_finalize_descriptor_heaps(pg);
        pgraph_d3d12_finalize_command_objects(pg);
        pgraph_d3d12_finalize_device(pg);
        g_free(pg->d3d12_renderer_state);
        pg->d3d12_renderer_state = NULL;
        return;
    }
}

static void pgraph_d3d12_finalize(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;

    pgraph_d3d12_surface_flush(d);
    pgraph_d3d12_flush_reports(d);
#ifdef CONFIG_OPENGL
    pgraph_d3d12_finalize_display_gl(pg);
#endif
    pgraph_d3d12_finalize_pipelines(pg);
    pgraph_d3d12_finalize_reports(pg);
    pgraph_d3d12_finalize_textures(pg);
    pgraph_d3d12_finalize_surfaces(pg);
    pgraph_d3d12_finalize_buffers(pg);
    pgraph_d3d12_finalize_descriptor_heaps(pg);
    pgraph_d3d12_finalize_command_objects(pg);
    pgraph_d3d12_finalize_device(pg);
    g_free(pg->d3d12_renderer_state->display_pvideo_rgba);
    g_free(pg->d3d12_renderer_state);
    pg->d3d12_renderer_state = NULL;
}

static PGRAPHRenderer pgraph_d3d12_renderer = {
    .type = CONFIG_DISPLAY_RENDERER_D3D12,
    .name = "Direct3D 12",
    .ops = {
        .init = pgraph_d3d12_init,
        .finalize = pgraph_d3d12_finalize,
        .clear_report_value = pgraph_d3d12_clear_report_value,
        .clear_surface = pgraph_d3d12_clear_surface,
        .draw_begin = pgraph_d3d12_draw_begin,
        .draw_end = pgraph_d3d12_draw_end,
        .flip_stall = pgraph_d3d12_flip_stall,
        .flush_draw = pgraph_d3d12_flush_draw,
        .get_report = pgraph_d3d12_get_report,
        .image_blit = pgraph_d3d12_image_blit,
        .pre_savevm_trigger = pgraph_d3d12_pre_savevm_trigger,
        .pre_savevm_wait = pgraph_d3d12_pre_savevm_wait,
        .pre_shutdown_trigger = pgraph_d3d12_pre_shutdown_trigger,
        .pre_shutdown_wait = pgraph_d3d12_pre_shutdown_wait,
        .process_pending = pgraph_d3d12_process_pending,
        .process_pending_reports = pgraph_d3d12_process_pending_reports,
        .process_pending_reports_fifo =
            pgraph_d3d12_process_pending_reports_fifo,
        .surface_update = pgraph_d3d12_surface_update,
        .set_surface_scale_factor = pgraph_d3d12_set_surface_scale_factor,
        .get_surface_scale_factor = pgraph_d3d12_get_surface_scale_factor,
        .get_framebuffer_surface = pgraph_d3d12_get_framebuffer_surface,
        .get_display_frame = pgraph_d3d12_get_display_frame,
        .get_gpu_properties = pgraph_d3d12_get_gpu_properties,
    }
};

static void __attribute__((constructor)) register_renderer(void)
{
    pgraph_renderer_register(&pgraph_d3d12_renderer);
}
