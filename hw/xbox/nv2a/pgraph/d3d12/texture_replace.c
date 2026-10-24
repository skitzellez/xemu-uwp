/*
 * Geforce NV2A PGRAPH Direct3D 12 Renderer
 *
 * Copyright (c) 2026 danprice142 (https://github.com/danprice142/xemu-uwp)
 * Changed for xemu on Xbox Dev Mode by Skitzelles (https://github.com/skitzellez/xemu-uwp)
 *
 * Custom texture packs: loading replacements for guest textures and dumping
 * guest textures as files to make them. Under the custom textures folder:
 *
 *     load/textures/<TITLEID>/   replacements, in any subfolder layout
 *     dump/textures/<TITLEID>/   dumps
 *
 * A texture is named by its dimensions, the hash of its guest texels, the
 * hash of its palette when it has one, and its guest format:
 *
 *     tex_<width>x<height>_<hash16hex>_<fmt2hex>.png  (or .dds)
 *     tex_<width>x<height>_<hash16hex>_<palette16hex>_<fmt2hex>.png
 *
 * "$" in place of the palette hash matches the texels with any palette;
 * a file naming the palette wins over it.
 *
 * PNG replacements may come with tex_..._mip<N>.png levels; missing levels
 * are generated. DDS replacements carry their own levels and may be cube
 * maps, in BC1-BC5, BC7, R8G8B8A8 or B8G8R8A8. A replacement can have any
 * size; it is sampled as plain RGBA, so it holds the colours the shader
 * sees, which is what a dump contains.
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
#include "qemu/atomic.h"
#include "qemu/thread.h"
#include "hw/xbox/nv2a/pgraph/s3tc.h"
#include "ui/xemu-image.h"
#include "ui/xemu-notifications.h"
#include "ui/xemu-settings.h"
#include "renderer.h"

/* Decoded custom textures kept in memory, at most. */
#define TEXTURE_PACK_MEMORY_CAP (1024ull << 20)
#define TEXTURE_PACK_MAX_DIMENSION 16384u
#define TEXTURE_PACK_MAX_FOLDER_DEPTH 8

#define DDS_MAGIC 0x20534444u /* "DDS " */

#define DDSD_CAPS        0x1u
#define DDSD_HEIGHT      0x2u
#define DDSD_WIDTH       0x4u
#define DDSD_PITCH       0x8u
#define DDSD_PIXELFORMAT 0x1000u
#define DDSD_MIPMAPCOUNT 0x20000u

#define DDPF_FOURCC 0x4u
#define DDPF_RGB    0x40u

#define DDSCAPS_COMPLEX 0x8u
#define DDSCAPS_TEXTURE 0x1000u

#define DDSCAPS2_CUBEMAP           0x200u
#define DDSCAPS2_CUBEMAP_ALL_FACES 0xfc00u
#define DDSCAPS2_VOLUME            0x200000u

#define DDS_FOURCC(a, b, c, d) \
    ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | \
     ((uint32_t)(d) << 24))

#define DDS_DIMENSION_TEXTURE2D 3u
#define DDS_MISC_TEXTURECUBE 0x4u

#pragma pack(push, 1)
typedef struct DDSPixelFormat {
    uint32_t size;
    uint32_t flags;
    uint32_t four_cc;
    uint32_t rgb_bit_count;
    uint32_t r_bit_mask;
    uint32_t g_bit_mask;
    uint32_t b_bit_mask;
    uint32_t a_bit_mask;
} DDSPixelFormat;

typedef struct DDSHeader {
    uint32_t size;
    uint32_t flags;
    uint32_t height;
    uint32_t width;
    uint32_t pitch_or_linear_size;
    uint32_t depth;
    uint32_t mip_map_count;
    uint32_t reserved1[11];
    DDSPixelFormat pixel_format;
    uint32_t caps;
    uint32_t caps2;
    uint32_t caps3;
    uint32_t caps4;
    uint32_t reserved2;
} DDSHeader;

typedef struct DDSHeaderDX10 {
    uint32_t dxgi_format;
    uint32_t resource_dimension;
    uint32_t misc_flag;
    uint32_t array_size;
    uint32_t misc_flags2;
} DDSHeaderDX10;
#pragma pack(pop)

typedef struct DDSFormatLayout {
    uint32_t block_size;        /* 4 for block-compressed formats */
    uint32_t bytes_per_block;   /* or per pixel */
} DDSFormatLayout;

struct PGRAPHD3D12TextureReplace {
    /* Applied settings, compared with the configuration every frame. */
    bool load;
    bool prefetch;
    bool dump;
    char *root;
    /* pg->custom_textures_gen the root was read at. */
    uint32_t settings_gen;
    bool root_valid;
    uint32_t title_id;

    char *dump_dir;
    bool dump_dir_ready;
    GHashTable *dumped;         /* names dumped this session */
    GHashTable *dump_slots;     /* guest texture slot -> TextureSlot */
    unsigned int dump_count;

    /*
     * Pack files of the title: lower-case file name -> path. Written only
     * while no prefetch runs.
     */
    GHashTable *files;
    /* Name -> replacement, or the absent marker for unusable files. */
    QemuMutex lock;
    GHashTable *loaded;
    uint64_t loaded_bytes;
    bool cap_reported;

    QemuThread prefetch_thread;
    bool prefetch_running;
    bool prefetch_stop;
    unsigned int prefetch_total;
};

static const char absent_marker;
#define REPLACEMENT_ABSENT ((gpointer)&absent_marker)

/*
 * A texture slot whose content changes this often holds video or other
 * per-frame data, which is not dumped.
 */
#define DUMP_CHANGE_WINDOW_US (2 * G_USEC_PER_SEC)
#define DUMP_CHANGE_LIMIT 4

typedef struct TextureSlot {
    int64_t last_change_us;
    unsigned int changes;
    bool reported;
} TextureSlot;

static bool dds_format_layout(DXGI_FORMAT format, DDSFormatLayout *layout)
{
    switch (format) {
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC4_UNORM:
        *layout = (DDSFormatLayout){ 4, 8 };
        return true;
    case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC3_UNORM:
    case DXGI_FORMAT_BC5_UNORM:
    case DXGI_FORMAT_BC7_UNORM:
        *layout = (DDSFormatLayout){ 4, 16 };
        return true;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
        *layout = (DDSFormatLayout){ 1, 4 };
        return true;
    default:
        return false;
    }
}

/* Replacements use the linear-colour formats; sRGB files are read as is. */
static DXGI_FORMAT dds_unorm_format(DXGI_FORMAT format)
{
    switch (format) {
    case DXGI_FORMAT_BC1_UNORM_SRGB:
        return DXGI_FORMAT_BC1_UNORM;
    case DXGI_FORMAT_BC2_UNORM_SRGB:
        return DXGI_FORMAT_BC2_UNORM;
    case DXGI_FORMAT_BC3_UNORM_SRGB:
        return DXGI_FORMAT_BC3_UNORM;
    case DXGI_FORMAT_BC7_UNORM_SRGB:
        return DXGI_FORMAT_BC7_UNORM;
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    default:
        return format;
    }
}

static uint32_t layout_row_bytes(const DDSFormatLayout *layout,
                                 uint32_t width)
{
    uint32_t units = (width + layout->block_size - 1) / layout->block_size;

    return MAX(units, 1u) * layout->bytes_per_block;
}

static uint32_t layout_rows(const DDSFormatLayout *layout, uint32_t height)
{
    uint32_t rows = (height + layout->block_size - 1) / layout->block_size;

    return MAX(rows, 1u);
}

/* The pack name of a texture; any_palette gives the palette wildcard. */
static void texture_pack_name(char *buf, size_t size,
                              const TextureShape *shape,
                              const PGRAPHD3D12TexturePackHash *hash,
                              bool any_palette)
{
    if (!hash->has_palette) {
        snprintf(buf, size, "tex_%ux%u_%016" PRIx64 "_%02x", shape->width,
                 shape->height, hash->texels, shape->color_format);
    } else if (any_palette) {
        snprintf(buf, size, "tex_%ux%u_%016" PRIx64 "_$_%02x", shape->width,
                 shape->height, hash->texels, shape->color_format);
    } else {
        snprintf(buf, size, "tex_%ux%u_%016" PRIx64 "_%016" PRIx64 "_%02x",
                 shape->width, shape->height, hash->texels, hash->palette,
                 shape->color_format);
    }
}

static void free_replacement(gpointer value)
{
    PGRAPHD3D12ReplacementTexture *replacement = value;

    if (value == REPLACEMENT_ABSENT || value == NULL) {
        return;
    }
    g_free(replacement->data);
    g_free(replacement);
}

/*
 * Parses a DDS file. The subresources are copied out of the file, so the
 * caller keeps ownership of `file`.
 */
static PGRAPHD3D12ReplacementTexture *parse_dds(const char *path,
                                                const uint8_t *file,
                                                size_t file_size)
{
    const DDSHeader *header;
    size_t payload = 4 + sizeof(DDSHeader);
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    DDSFormatLayout layout;
    PGRAPHD3D12ReplacementTexture *rep;
    uint32_t mip_levels;
    uint32_t faces = 1;
    size_t offset;

    if (file_size < payload || ldl_le_p(file) != DDS_MAGIC) {
        fprintf(stderr, "Custom textures: %s is not a DDS file\n", path);
        return NULL;
    }
    header = (const DDSHeader *)(file + 4);
    if (header->size != sizeof(DDSHeader) ||
        header->pixel_format.size != sizeof(DDSPixelFormat)) {
        fprintf(stderr, "Custom textures: %s has a malformed DDS header\n",
                path);
        return NULL;
    }

    if (header->pixel_format.flags & DDPF_FOURCC) {
        switch (header->pixel_format.four_cc) {
        case DDS_FOURCC('D', 'X', '1', '0'): {
            const DDSHeaderDX10 *dx10;

            if (file_size < payload + sizeof(DDSHeaderDX10)) {
                fprintf(stderr, "Custom textures: %s is truncated\n", path);
                return NULL;
            }
            dx10 = (const DDSHeaderDX10 *)(file + payload);
            payload += sizeof(DDSHeaderDX10);
            format = dds_unorm_format((DXGI_FORMAT)dx10->dxgi_format);
            if (dx10->resource_dimension != DDS_DIMENSION_TEXTURE2D) {
                fprintf(stderr,
                        "Custom textures: %s is not a 2D texture or cube "
                        "map\n", path);
                return NULL;
            }
            if (dx10->misc_flag & DDS_MISC_TEXTURECUBE) {
                faces = 6;
            }
            if (dx10->array_size > 1) {
                fprintf(stderr,
                        "Custom textures: %s is a texture array, which is "
                        "not supported\n", path);
                return NULL;
            }
            break;
        }
        case DDS_FOURCC('D', 'X', 'T', '1'):
            format = DXGI_FORMAT_BC1_UNORM;
            break;
        case DDS_FOURCC('D', 'X', 'T', '2'):
        case DDS_FOURCC('D', 'X', 'T', '3'):
            format = DXGI_FORMAT_BC2_UNORM;
            break;
        case DDS_FOURCC('D', 'X', 'T', '4'):
        case DDS_FOURCC('D', 'X', 'T', '5'):
            format = DXGI_FORMAT_BC3_UNORM;
            break;
        case DDS_FOURCC('A', 'T', 'I', '1'):
        case DDS_FOURCC('B', 'C', '4', 'U'):
            format = DXGI_FORMAT_BC4_UNORM;
            break;
        case DDS_FOURCC('A', 'T', 'I', '2'):
        case DDS_FOURCC('B', 'C', '5', 'U'):
            format = DXGI_FORMAT_BC5_UNORM;
            break;
        default:
            break;
        }
    } else if ((header->pixel_format.flags & DDPF_RGB) &&
               header->pixel_format.rgb_bit_count == 32) {
        if (header->pixel_format.r_bit_mask == 0x00ff0000u &&
            header->pixel_format.g_bit_mask == 0x0000ff00u &&
            header->pixel_format.b_bit_mask == 0x000000ffu) {
            format = DXGI_FORMAT_B8G8R8A8_UNORM;
        } else if (header->pixel_format.r_bit_mask == 0x000000ffu &&
                   header->pixel_format.g_bit_mask == 0x0000ff00u &&
                   header->pixel_format.b_bit_mask == 0x00ff0000u) {
            format = DXGI_FORMAT_R8G8B8A8_UNORM;
        }
    }
    if (!dds_format_layout(format, &layout)) {
        fprintf(stderr,
                "Custom textures: %s uses an unsupported pixel format "
                "(dxgi=%u); use BC1-BC5, BC7 or 32-bit RGBA\n",
                path, (unsigned int)format);
        return NULL;
    }

    if (header->caps2 & DDSCAPS2_CUBEMAP) {
        if ((header->caps2 & DDSCAPS2_CUBEMAP_ALL_FACES) !=
            DDSCAPS2_CUBEMAP_ALL_FACES) {
            fprintf(stderr,
                    "Custom textures: cube map %s lacks some faces\n", path);
            return NULL;
        }
        faces = 6;
    }
    if (header->caps2 & DDSCAPS2_VOLUME) {
        fprintf(stderr,
                "Custom textures: %s is a volume texture, which is not "
                "supported\n", path);
        return NULL;
    }
    if (header->width == 0 || header->height == 0 ||
        header->width > TEXTURE_PACK_MAX_DIMENSION ||
        header->height > TEXTURE_PACK_MAX_DIMENSION) {
        fprintf(stderr, "Custom textures: %s has invalid dimensions %ux%u\n",
                path, header->width, header->height);
        return NULL;
    }

    mip_levels = (header->flags & DDSD_MIPMAPCOUNT) ?
                     MAX(header->mip_map_count, 1u) : 1u;
    mip_levels = MIN(mip_levels,
                     PGRAPH_D3D12_REPLACEMENT_MAX_SUBRESOURCES / faces);

    rep = g_new0(PGRAPHD3D12ReplacementTexture, 1);
    rep->dxgi_format = format;
    rep->width = header->width;
    rep->height = header->height;
    rep->mip_levels = mip_levels;
    rep->faces = faces;

    offset = payload;
    for (uint32_t face = 0; face < faces; face++) {
        for (uint32_t level = 0; level < mip_levels; level++) {
            uint32_t width = MAX(rep->width >> level, 1u);
            uint32_t height = MAX(rep->height >> level, 1u);
            PGRAPHD3D12ReplacementSubresource *sub =
                &rep->subresources[face * mip_levels + level];

            sub->width = width;
            sub->height = height;
            sub->row_pitch = layout_row_bytes(&layout, width);
            sub->rows = layout_rows(&layout, height);
            sub->offset = offset - payload;
            offset += (size_t)sub->row_pitch * sub->rows;
            if (offset > file_size) {
                fprintf(stderr,
                        "Custom textures: %s is truncated at face %u level "
                        "%u\n", path, face, level);
                g_free(rep);
                return NULL;
            }
        }
    }
    rep->data_size = offset - payload;
    rep->data = g_memdup2(file + payload, rep->data_size);
    return rep;
}

/* Box-filters an RGBA8 level to the next smaller one. */
static void downsample_rgba(const uint8_t *src, uint32_t src_width,
                            uint32_t src_height, uint8_t *dst,
                            uint32_t dst_width, uint32_t dst_height)
{
    for (uint32_t y = 0; y < dst_height; y++) {
        uint32_t y0 = MIN(y * 2, src_height - 1);
        uint32_t y1 = MIN(y * 2 + 1, src_height - 1);

        for (uint32_t x = 0; x < dst_width; x++) {
            uint32_t x0 = MIN(x * 2, src_width - 1);
            uint32_t x1 = MIN(x * 2 + 1, src_width - 1);
            const uint8_t *p00 = src + ((size_t)y0 * src_width + x0) * 4;
            const uint8_t *p01 = src + ((size_t)y0 * src_width + x1) * 4;
            const uint8_t *p10 = src + ((size_t)y1 * src_width + x0) * 4;
            const uint8_t *p11 = src + ((size_t)y1 * src_width + x1) * 4;
            uint8_t *out = dst + ((size_t)y * dst_width + x) * 4;

            for (unsigned int c = 0; c < 4; c++) {
                out[c] = (uint8_t)((p00[c] + p01[c] + p10[c] + p11[c] + 2) /
                                   4);
            }
        }
    }
}

static uint8_t *decode_png_file(const char *path, unsigned int *width,
                                unsigned int *height)
{
    g_autofree char *file = NULL;
    gsize size = 0;
    uint8_t *pixels;

    if (!g_file_get_contents(path, &file, &size, NULL)) {
        fprintf(stderr, "Custom textures: cannot read %s\n", path);
        return NULL;
    }
    pixels = xemu_image_decode_rgba((const uint8_t *)file, size, width,
                                    height);
    if (pixels == NULL) {
        fprintf(stderr, "Custom textures: cannot decode %s\n", path);
    }
    return pixels;
}

/*
 * Decodes a PNG replacement with a full mip chain: _mip<N> files where the
 * pack has them at the right size, generated levels otherwise.
 */
static PGRAPHD3D12ReplacementTexture *load_png(
    PGRAPHD3D12TextureReplace *tr, const char *name, const char *path)
{
    unsigned int width, height;
    uint8_t *base = decode_png_file(path, &width, &height);
    PGRAPHD3D12ReplacementTexture *rep;
    uint32_t mip_levels = 1;
    size_t size = 0;

    if (base == NULL) {
        return NULL;
    }
    if (width > TEXTURE_PACK_MAX_DIMENSION ||
        height > TEXTURE_PACK_MAX_DIMENSION) {
        fprintf(stderr, "Custom textures: %s is larger than %u pixels\n",
                path, TEXTURE_PACK_MAX_DIMENSION);
        xemu_image_free(base);
        return NULL;
    }
    while ((width >> mip_levels) != 0 || (height >> mip_levels) != 0) {
        mip_levels++;
    }
    mip_levels = MIN(mip_levels, 16u);

    rep = g_new0(PGRAPHD3D12ReplacementTexture, 1);
    rep->dxgi_format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rep->width = width;
    rep->height = height;
    rep->mip_levels = mip_levels;
    rep->faces = 1;
    for (uint32_t level = 0; level < mip_levels; level++) {
        PGRAPHD3D12ReplacementSubresource *sub = &rep->subresources[level];

        sub->width = MAX(width >> level, 1u);
        sub->height = MAX(height >> level, 1u);
        sub->row_pitch = sub->width * 4;
        sub->rows = sub->height;
        sub->offset = size;
        size += (size_t)sub->row_pitch * sub->rows;
    }
    rep->data_size = size;
    rep->data = g_malloc(size);
    memcpy(rep->data, base, (size_t)width * height * 4);
    xemu_image_free(base);

    for (uint32_t level = 1; level < mip_levels; level++) {
        PGRAPHD3D12ReplacementSubresource *sub = &rep->subresources[level];
        const PGRAPHD3D12ReplacementSubresource *prev =
            &rep->subresources[level - 1];
        g_autofree char *mip_name =
            g_strdup_printf("%s_mip%u.png", name, level);
        const char *mip_path = g_hash_table_lookup(tr->files, mip_name);
        bool loaded = false;

        if (mip_path != NULL) {
            unsigned int mip_width, mip_height;
            uint8_t *mip = decode_png_file(mip_path, &mip_width, &mip_height);

            if (mip != NULL && mip_width == sub->width &&
                mip_height == sub->height) {
                memcpy(rep->data + sub->offset, mip,
                       (size_t)sub->row_pitch * sub->rows);
                loaded = true;
            } else if (mip != NULL) {
                fprintf(stderr,
                        "Custom textures: %s is %ux%u, level %u needs "
                        "%ux%u; generating it instead\n",
                        mip_path, mip_width, mip_height, level, sub->width,
                        sub->height);
            }
            xemu_image_free(mip);
        }
        if (!loaded) {
            downsample_rgba(rep->data + prev->offset, prev->width,
                            prev->height, rep->data + sub->offset,
                            sub->width, sub->height);
        }
    }
    return rep;
}

static PGRAPHD3D12ReplacementTexture *load_replacement_file(
    PGRAPHD3D12TextureReplace *tr, const char *name, const char *path)
{
    g_autofree uint8_t *file = NULL;
    gsize size = 0;

    if (g_str_has_suffix(path, ".png") || g_str_has_suffix(path, ".PNG")) {
        return load_png(tr, name, path);
    }
    if (!g_file_get_contents(path, (char **)&file, &size, NULL)) {
        fprintf(stderr, "Custom textures: cannot read %s\n", path);
        return NULL;
    }
    return parse_dds(path, file, size);
}

/* The pack file for a name: a DDS before a PNG. Caller holds no lock. */
static const char *pack_file_path(PGRAPHD3D12TextureReplace *tr,
                                  const char *name)
{
    char file_name[96];
    const char *path;

    snprintf(file_name, sizeof(file_name), "%s.dds", name);
    path = g_hash_table_lookup(tr->files, file_name);
    if (path == NULL) {
        snprintf(file_name, sizeof(file_name), "%s.png", name);
        path = g_hash_table_lookup(tr->files, file_name);
    }
    return path;
}

/*
 * Records a decoded replacement, or the absent marker for NULL. Returns
 * the entry to use: an entry another thread stored first wins. Takes
 * ownership of `rep`.
 */
static gpointer store_replacement(PGRAPHD3D12TextureReplace *tr,
                                  const char *name,
                                  PGRAPHD3D12ReplacementTexture *rep)
{
    gpointer existing;

    qemu_mutex_lock(&tr->lock);
    existing = g_hash_table_lookup(tr->loaded, name);
    if (existing != NULL) {
        qemu_mutex_unlock(&tr->lock);
        free_replacement(rep);
        return existing;
    }
    if (rep != NULL &&
        tr->loaded_bytes + rep->data_size > TEXTURE_PACK_MEMORY_CAP) {
        if (!tr->cap_reported) {
            tr->cap_reported = true;
            fprintf(stderr,
                    "Custom textures: %" PRIu64 " MiB are loaded; further "
                    "textures keep their original look\n",
                    tr->loaded_bytes >> 20);
            xemu_queue_notification(
                "Custom textures: memory limit reached, some textures are "
                "not replaced");
        }
        /* Not read again on every bind. */
        g_hash_table_insert(tr->loaded, g_strdup(name), REPLACEMENT_ABSENT);
        qemu_mutex_unlock(&tr->lock);
        free_replacement(rep);
        return REPLACEMENT_ABSENT;
    }
    if (rep != NULL) {
        tr->loaded_bytes += rep->data_size;
    }
    g_hash_table_insert(tr->loaded, g_strdup(name),
                        rep != NULL ? (gpointer)rep : REPLACEMENT_ABSENT);
    qemu_mutex_unlock(&tr->lock);
    return rep != NULL ? (gpointer)rep : REPLACEMENT_ABSENT;
}

static void *prefetch_thread(void *opaque)
{
    PGRAPHD3D12TextureReplace *tr = opaque;
    GHashTableIter iter;
    gpointer key, value;
    unsigned int loaded = 0;

    g_hash_table_iter_init(&iter, tr->files);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        const char *file_name = key;
        g_autofree char *name = NULL;
        bool known;

        if (qatomic_read(&tr->prefetch_stop)) {
            return NULL;
        }
        /* Levels load with their base file. */
        if (strstr(file_name, "_mip") != NULL) {
            continue;
        }
        name = g_strndup(file_name, strlen(file_name) - 4);
        /* The DDS wins over a PNG of the same name. */
        if (pack_file_path(tr, name) != value) {
            continue;
        }
        qemu_mutex_lock(&tr->lock);
        known = g_hash_table_contains(tr->loaded, name);
        qemu_mutex_unlock(&tr->lock);
        if (known) {
            continue;
        }
        if (store_replacement(tr, name,
                              load_replacement_file(tr, name, value)) !=
            REPLACEMENT_ABSENT) {
            loaded++;
        }
        if (qatomic_read(&tr->cap_reported)) {
            break;
        }
    }
    fprintf(stderr, "Custom textures: prefetched %u of %u files for %08x\n",
            loaded, tr->prefetch_total, tr->title_id);
    {
        g_autofree char *msg = g_strdup_printf(
            "Custom textures: %u loaded", loaded);

        xemu_queue_notification(msg);
    }
    return NULL;
}

static void stop_prefetch(PGRAPHD3D12TextureReplace *tr)
{
    if (tr->prefetch_running) {
        qatomic_set(&tr->prefetch_stop, true);
        qemu_thread_join(&tr->prefetch_thread);
        tr->prefetch_running = false;
        qatomic_set(&tr->prefetch_stop, false);
    }
}

static void index_folder(GHashTable *files, const char *dir,
                         unsigned int depth)
{
    GDir *folder;
    const char *entry;

    if (depth > TEXTURE_PACK_MAX_FOLDER_DEPTH ||
        (folder = g_dir_open(dir, 0, NULL)) == NULL) {
        return;
    }
    while ((entry = g_dir_read_name(folder)) != NULL) {
        g_autofree char *path = g_build_filename(dir, entry, NULL);
        g_autofree char *lower = NULL;

        if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
            index_folder(files, path, depth + 1);
            continue;
        }
        lower = g_ascii_strdown(entry, -1);
        if (g_str_has_prefix(lower, "tex_") &&
            (g_str_has_suffix(lower, ".png") ||
             g_str_has_suffix(lower, ".dds")) &&
            !g_hash_table_contains(files, lower)) {
            g_hash_table_insert(files, g_steal_pointer(&lower),
                                g_steal_pointer(&path));
        }
    }
    g_dir_close(folder);
}

/* The default root holds load/ and dump/ beside the system files. */
static char *default_root(void)
{
#ifdef CONFIG_UWP
    return g_strdup(XEMU_UWP_DEV_FILES_SYSTEM_DIR);
#else
    g_autofree char *base = g_strdup(xemu_settings_get_base_path());
    size_t len = strlen(base);

    while (len > 1 && (base[len - 1] == '/' || base[len - 1] == '\\')) {
        base[--len] = '\0';
    }
    return g_steal_pointer(&base);
#endif
}

static void unload_pack(PGRAPHD3D12TextureReplace *tr)
{
    stop_prefetch(tr);
    g_hash_table_remove_all(tr->files);
    g_hash_table_remove_all(tr->loaded);
    g_hash_table_remove_all(tr->dumped);
    g_hash_table_remove_all(tr->dump_slots);
    tr->loaded_bytes = 0;
    tr->cap_reported = false;
    tr->dump_count = 0;
    g_free(tr->dump_dir);
    tr->dump_dir = NULL;
    tr->dump_dir_ready = false;
}

void pgraph_d3d12_init_texture_replace(PGRAPHState *pg)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    PGRAPHD3D12TextureReplace *tr = g_new0(PGRAPHD3D12TextureReplace, 1);

    qemu_mutex_init(&tr->lock);
    tr->files = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                      g_free);
    tr->loaded = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                       free_replacement);
    tr->dumped = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                       NULL);
    tr->dump_slots = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                           g_free, g_free);
    tr->root = g_strdup("");
    r->texture_replace = tr;
}

void pgraph_d3d12_finalize_texture_replace(PGRAPHState *pg)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    PGRAPHD3D12TextureReplace *tr = r->texture_replace;

    if (tr == NULL) {
        return;
    }
    unload_pack(tr);
    g_hash_table_destroy(tr->files);
    g_hash_table_destroy(tr->loaded);
    g_hash_table_destroy(tr->dumped);
    g_hash_table_destroy(tr->dump_slots);
    qemu_mutex_destroy(&tr->lock);
    g_free(tr->root);
    g_free(tr);
    r->texture_replace = NULL;
}

bool pgraph_d3d12_texture_replace_update(PGRAPHState *pg)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    PGRAPHD3D12TextureReplace *tr = r != NULL ? r->texture_replace : NULL;
    bool load = g_config.display.custom_textures.load;
    bool dump = g_config.display.custom_textures.dump;
    bool prefetch = load && g_config.display.custom_textures.prefetch;
    /* The folder string is only read when something changed. */
    const char *path = g_config.display.custom_textures.path;
    uint32_t settings_gen = qatomic_read(&pg->custom_textures_gen);
    uint32_t title_id = qatomic_read(&pg->shader_cache.title_id);
    g_autofree char *title = NULL;
    g_autofree char *load_dir = NULL;
    bool was_active;

    if (tr == NULL) {
        return false;
    }
    if (load == tr->load && dump == tr->dump && prefetch == tr->prefetch &&
        title_id == tr->title_id && tr->root_valid &&
        settings_gen == tr->settings_gen) {
        return false;
    }

    was_active = tr->load || tr->dump;
    unload_pack(tr);
    tr->load = load;
    tr->dump = dump;
    tr->prefetch = prefetch;
    tr->title_id = title_id;
    g_free(tr->root);
    tr->root = (path != NULL && path[0] != '\0') ? g_strdup(path) :
                                                   default_root();
    tr->settings_gen = settings_gen;
    tr->root_valid = true;
    if (title_id == 0 || (!load && !dump)) {
        return was_active;
    }

    title = g_strdup_printf("%08X", title_id);
    if (dump) {
        tr->dump_dir = g_build_filename(tr->root, "dump", "textures", title,
                                        NULL);
        fprintf(stderr, "Custom textures: dumping to %s\n", tr->dump_dir);
    }
    if (load) {
        load_dir = g_build_filename(tr->root, "load", "textures", title,
                                    NULL);
        index_folder(tr->files, load_dir, 0);
        fprintf(stderr, "Custom textures: %u files in %s\n",
                g_hash_table_size(tr->files), load_dir);
        if (prefetch && g_hash_table_size(tr->files) != 0) {
            tr->prefetch_total = g_hash_table_size(tr->files);
            qemu_thread_create(&tr->prefetch_thread, "nv2a.texpack",
                               prefetch_thread, tr, QEMU_THREAD_JOINABLE);
            tr->prefetch_running = true;
        }
    }
    return true;
}

bool pgraph_d3d12_texture_replace_eligible(const TextureShape *shape,
                                           DXGI_FORMAT dxgi_format)
{
    if (shape->dimensionality != 2 || shape->border) {
        return false;
    }
    switch (shape->color_format) {
    /* Video frames change every frame. */
    case NV097_SET_TEXTURE_FORMAT_COLOR_LC_IMAGE_CR8YB8CB8YA8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LC_IMAGE_YB8CR8YA8CB8:
        return false;
    default:
        break;
    }
    /* Colour formats only: depth, signed and 16-bit data are not images. */
    switch (dxgi_format) {
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC3_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B5G6R5_UNORM:
    case DXGI_FORMAT_B5G5R5A1_UNORM:
    case DXGI_FORMAT_B4G4R4A4_UNORM:
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_R8G8_UNORM:
        return true;
    default:
        return false;
    }
}

const PGRAPHD3D12ReplacementTexture *pgraph_d3d12_texture_replacement_lookup(
    PGRAPHState *pg, const TextureShape *shape,
    const PGRAPHD3D12TexturePackHash *hash)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    PGRAPHD3D12TextureReplace *tr = r->texture_replace;
    char name[64];
    const char *path;
    gpointer entry;
    const PGRAPHD3D12ReplacementTexture *rep;

    if (tr == NULL || !tr->load || g_hash_table_size(tr->files) == 0) {
        return NULL;
    }
    texture_pack_name(name, sizeof(name), shape, hash, false);
    path = pack_file_path(tr, name);
    if (path == NULL && hash->has_palette) {
        /* The same texels under any palette share one wildcard file. */
        texture_pack_name(name, sizeof(name), shape, hash, true);
        path = pack_file_path(tr, name);
    }
    if (path == NULL) {
        return NULL;
    }

    qemu_mutex_lock(&tr->lock);
    entry = g_hash_table_lookup(tr->loaded, name);
    qemu_mutex_unlock(&tr->lock);
    if (entry == NULL) {
        entry = store_replacement(tr, name,
                                  load_replacement_file(tr, name, path));
        if (entry != REPLACEMENT_ABSENT) {
            rep = entry;
            fprintf(stderr,
                    "Custom textures: %s -> %ux%u dxgi=%u levels=%u\n", name,
                    rep->width, rep->height, (unsigned int)rep->dxgi_format,
                    rep->mip_levels);
        }
    }
    if (entry == REPLACEMENT_ABSENT) {
        return NULL;
    }
    rep = entry;
    /* A cube map replaces only a cube map, and the other way round. */
    if (rep->faces != (shape->cubemap ? 6u : 1u)) {
        return NULL;
    }
    return rep;
}

bool pgraph_d3d12_texture_dump_wanted(PGRAPHState *pg,
                                      const TextureShape *shape,
                                      hwaddr vram_addr,
                                      const PGRAPHD3D12TexturePackHash *hash)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    PGRAPHD3D12TextureReplace *tr = r->texture_replace;
    int64_t key = (int64_t)(((uint64_t)vram_addr << 24) ^
                            ((uint64_t)shape->color_format << 56) ^
                            ((uint64_t)shape->width << 12) ^ shape->height);
    int64_t now = g_get_monotonic_time();
    TextureSlot *slot;
    char name[64];

    if (tr == NULL || tr->dump_dir == NULL) {
        return false;
    }
    texture_pack_name(name, sizeof(name), shape, hash, false);
    if (g_hash_table_contains(tr->dumped, name)) {
        return false;
    }

    /* New content for this slot: count how quickly it keeps changing. */
    slot = g_hash_table_lookup(tr->dump_slots, &key);
    if (slot == NULL) {
        slot = g_new0(TextureSlot, 1);
        slot->last_change_us = now - DUMP_CHANGE_WINDOW_US;
        g_hash_table_insert(tr->dump_slots, g_memdup2(&key, sizeof(key)),
                            slot);
    }
    slot->changes = now - slot->last_change_us < DUMP_CHANGE_WINDOW_US ?
                        slot->changes + 1 : 0;
    slot->last_change_us = now;
    if (slot->changes >= DUMP_CHANGE_LIMIT) {
        if (!slot->reported) {
            slot->reported = true;
            fprintf(stderr,
                    "Custom textures: the %ux%u texture at 0x%" HWADDR_PRIx
                    " changes every frame and is not dumped\n",
                    shape->width, shape->height, vram_addr);
        }
        return false;
    }
    return true;
}

/*
 * Channel sources of the pixel shader's per-format swizzle
 * (texture_swizzle_expression() in pipeline.c): texel R, G, B, A, then the
 * constants 0 and 1.
 */
enum { SRC_R, SRC_G, SRC_B, SRC_A, SRC_0, SRC_1 };

static const uint8_t *format_swizzle(unsigned int color_format)
{
    static const uint8_t identity[4] = { SRC_R, SRC_G, SRC_B, SRC_A };
    static const uint8_t luminance[4] = { SRC_R, SRC_R, SRC_R, SRC_1 };
    static const uint8_t intensity[4] = { SRC_R, SRC_R, SRC_R, SRC_R };
    static const uint8_t alpha[4] = { SRC_1, SRC_1, SRC_1, SRC_R };
    static const uint8_t luminance_alpha[4] = { SRC_R, SRC_R, SRC_R, SRC_G };
    static const uint8_t g8b8[4] = { SRC_R, SRC_G, SRC_R, SRC_G };
    static const uint8_t r8b8[4] = { SRC_G, SRC_R, SRC_R, SRC_G };
    static const uint8_t opaque[4] = { SRC_R, SRC_G, SRC_B, SRC_1 };
    static const uint8_t b8g8r8a8[4] = { SRC_G, SRC_B, SRC_A, SRC_R };
    static const uint8_t r8g8b8a8[4] = { SRC_A, SRC_B, SRC_G, SRC_R };

    switch (color_format) {
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_Y8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_Y8:
        return luminance;
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_AY8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_AY8:
        return intensity;
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8:
        return alpha;
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8Y8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8Y8:
        return luminance_alpha;
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_G8B8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_G8B8:
        return g8b8;
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R8B8:
        return r8b8;
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_X1R5G5B5:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X1R5G5B5:
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_X8R8G8B8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X8R8G8B8:
        return opaque;
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_B8G8R8A8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_B8G8R8A8:
        return b8g8r8a8;
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R8G8B8A8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_R8G8B8A8:
        return r8g8b8a8;
    default:
        return identity;
    }
}

static uint8_t expand_bits(uint32_t value, unsigned int bits)
{
    return (uint8_t)((value * 255 + ((1u << bits) - 1) / 2) /
                     ((1u << bits) - 1));
}

/* The texel the sampler returns for an upload-format pixel. */
static void read_texel(DXGI_FORMAT format, const uint8_t *p, uint8_t t[4])
{
    uint32_t v;

    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
        memcpy(t, p, 4);
        break;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
        t[0] = p[2];
        t[1] = p[1];
        t[2] = p[0];
        t[3] = p[3];
        break;
    case DXGI_FORMAT_B5G6R5_UNORM:
        v = lduw_le_p(p);
        t[0] = expand_bits((v >> 11) & 0x1f, 5);
        t[1] = expand_bits((v >> 5) & 0x3f, 6);
        t[2] = expand_bits(v & 0x1f, 5);
        t[3] = 255;
        break;
    case DXGI_FORMAT_B5G5R5A1_UNORM:
        v = lduw_le_p(p);
        t[0] = expand_bits((v >> 10) & 0x1f, 5);
        t[1] = expand_bits((v >> 5) & 0x1f, 5);
        t[2] = expand_bits(v & 0x1f, 5);
        t[3] = (v & 0x8000) ? 255 : 0;
        break;
    case DXGI_FORMAT_B4G4R4A4_UNORM:
        v = lduw_le_p(p);
        t[0] = expand_bits((v >> 8) & 0xf, 4);
        t[1] = expand_bits((v >> 4) & 0xf, 4);
        t[2] = expand_bits(v & 0xf, 4);
        t[3] = expand_bits((v >> 12) & 0xf, 4);
        break;
    case DXGI_FORMAT_R8_UNORM:
        t[0] = p[0];
        t[1] = 0;
        t[2] = 0;
        t[3] = 255;
        break;
    case DXGI_FORMAT_R8G8_UNORM:
        t[0] = p[0];
        t[1] = p[1];
        t[2] = 0;
        t[3] = 255;
        break;
    default:
        memset(t, 0, 4);
        break;
    }
}

static unsigned int upload_bytes_per_pixel(DXGI_FORMAT format)
{
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
        return 4;
    case DXGI_FORMAT_B5G6R5_UNORM:
    case DXGI_FORMAT_B5G5R5A1_UNORM:
    case DXGI_FORMAT_B4G4R4A4_UNORM:
    case DXGI_FORMAT_R8G8_UNORM:
        return 2;
    case DXGI_FORMAT_R8_UNORM:
        return 1;
    default:
        return 0;
    }
}

/* One face's base level as tightly packed RGBA8 after the swizzle. */
static uint8_t *face_to_rgba(const TextureShape *shape, DXGI_FORMAT format,
                             const uint8_t *data, uint32_t row_pitch)
{
    uint32_t width = shape->width;
    uint32_t height = shape->height;
    const uint8_t *sel = format_swizzle(shape->color_format);
    uint8_t *rgba;

    switch (format) {
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC3_UNORM:
        rgba = s3tc_decompress_2d(
            format == DXGI_FORMAT_BC1_UNORM ? S3TC_DECOMPRESS_FORMAT_DXT1 :
            format == DXGI_FORMAT_BC2_UNORM ? S3TC_DECOMPRESS_FORMAT_DXT3 :
                                              S3TC_DECOMPRESS_FORMAT_DXT5,
            data, width, height);
        break;
    default: {
        unsigned int bpp = upload_bytes_per_pixel(format);

        if (bpp == 0) {
            return NULL;
        }
        rgba = g_malloc((size_t)width * height * 4);
        for (uint32_t y = 0; y < height; y++) {
            const uint8_t *row = data + (size_t)y * row_pitch;

            for (uint32_t x = 0; x < width; x++) {
                read_texel(format, row + (size_t)x * bpp,
                           rgba + ((size_t)y * width + x) * 4);
            }
        }
        break;
    }
    }

    for (size_t i = 0; i < (size_t)width * height; i++) {
        uint8_t *p = rgba + i * 4;
        uint8_t t[4];

        memcpy(t, p, 4);
        for (unsigned int c = 0; c < 4; c++) {
            p[c] = sel[c] == SRC_0 ? 0 : sel[c] == SRC_1 ? 255 : t[sel[c]];
        }
    }
    return rgba;
}

static bool write_cube_dds(const char *path, uint32_t width, uint32_t height,
                           uint8_t *const faces[6])
{
    DDSHeader header;
    DDSHeaderDX10 dx10;
    GByteArray *out;
    bool ok;

    memset(&header, 0, sizeof(header));
    header.size = sizeof(header);
    header.flags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT |
                   DDSD_PITCH;
    header.height = height;
    header.width = width;
    header.pitch_or_linear_size = width * 4;
    header.pixel_format.size = sizeof(header.pixel_format);
    header.pixel_format.flags = DDPF_FOURCC;
    header.pixel_format.four_cc = DDS_FOURCC('D', 'X', '1', '0');
    header.caps = DDSCAPS_TEXTURE | DDSCAPS_COMPLEX;
    header.caps2 = DDSCAPS2_CUBEMAP | DDSCAPS2_CUBEMAP_ALL_FACES;
    memset(&dx10, 0, sizeof(dx10));
    dx10.dxgi_format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dx10.resource_dimension = DDS_DIMENSION_TEXTURE2D;
    dx10.misc_flag = DDS_MISC_TEXTURECUBE;
    dx10.array_size = 1;

    out = g_byte_array_new();
    g_byte_array_append(out, (const guint8 *)"DDS ", 4);
    g_byte_array_append(out, (const guint8 *)&header, sizeof(header));
    g_byte_array_append(out, (const guint8 *)&dx10, sizeof(dx10));
    for (unsigned int face = 0; face < 6; face++) {
        g_byte_array_append(out, faces[face], width * height * 4);
    }
    ok = g_file_set_contents(path, (const char *)out->data, out->len, NULL);
    g_byte_array_unref(out);
    return ok;
}

void pgraph_d3d12_dump_texture(PGRAPHState *pg, const TextureShape *shape,
                               const PGRAPHD3D12TexturePackHash *hash,
                               DXGI_FORMAT dxgi_format,
                               const uint8_t *const *faces,
                               uint32_t row_pitch)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    PGRAPHD3D12TextureReplace *tr = r->texture_replace;
    unsigned int face_count = shape->cubemap ? 6 : 1;
    uint8_t *rgba[6] = { NULL };
    char name[64];
    g_autofree char *file_name = NULL;
    g_autofree char *path = NULL;
    bool ok = true;

    if (tr == NULL || tr->dump_dir == NULL) {
        return;
    }
    texture_pack_name(name, sizeof(name), shape, hash, false);
    if (!g_hash_table_add(tr->dumped, g_strdup(name))) {
        return;
    }
    if (!tr->dump_dir_ready) {
        tr->dump_dir_ready = true;
        g_mkdir_with_parents(tr->dump_dir, 0755);
    }
    file_name = g_strdup_printf("%s.%s", name, shape->cubemap ? "dds" : "png");
    path = g_build_filename(tr->dump_dir, file_name, NULL);
    if (g_file_test(path, G_FILE_TEST_EXISTS)) {
        return;
    }

    for (unsigned int face = 0; face < face_count && ok; face++) {
        rgba[face] = face_to_rgba(shape, dxgi_format, faces[face], row_pitch);
        ok = rgba[face] != NULL;
    }
    if (ok) {
        ok = shape->cubemap ?
                 write_cube_dds(path, shape->width, shape->height, rgba) :
                 xemu_image_save_png_rgba(path, rgba[0], shape->width,
                                          shape->height);
        if (!ok) {
            fprintf(stderr, "Custom textures: cannot write %s\n", path);
        } else if (++tr->dump_count <= 8 || tr->dump_count % 100 == 0) {
            fprintf(stderr, "Custom textures: dumped %u textures (%s)\n",
                    tr->dump_count, file_name);
        }
    }
    for (unsigned int face = 0; face < face_count; face++) {
        g_free(rgba[face]);
    }
}
