/*
 * xemu image file helpers
 *
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include <fpng.h>
#include <mutex>
#include <vector>
#include "stb_image.h"
#include "xemu-image.h"

uint8_t *xemu_image_decode_rgba(const uint8_t *data, size_t size,
                                unsigned int *width, unsigned int *height)
{
    int w = 0, h = 0, channels = 0;
    uint8_t *pixels;

    if (data == NULL || size == 0 || size > INT_MAX) {
        return NULL;
    }
    pixels = stbi_load_from_memory(data, (int)size, &w, &h, &channels, 4);
    if (pixels == NULL || w <= 0 || h <= 0) {
        stbi_image_free(pixels);
        return NULL;
    }
    *width = (unsigned int)w;
    *height = (unsigned int)h;
    return pixels;
}

void xemu_image_free(uint8_t *pixels)
{
    stbi_image_free(pixels);
}

bool xemu_image_save_png_rgba(const char *path, const uint8_t *rgba,
                              unsigned int width, unsigned int height)
{
    static std::once_flag init;
    std::vector<uint8_t> png;

    std::call_once(init, [] { fpng::fpng_init(); });
    if (!fpng::fpng_encode_image_to_memory(rgba, width, height, 4, png)) {
        return false;
    }
    return g_file_set_contents(path, (const char *)png.data(),
                               (gssize)png.size(), NULL);
}
