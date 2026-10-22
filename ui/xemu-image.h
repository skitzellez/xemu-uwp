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

#ifndef XEMU_IMAGE_H
#define XEMU_IMAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Decodes a PNG (or another format the image loader knows) to tightly
 * packed RGBA8. Returns NULL on failure; release with xemu_image_free().
 * Safe to call from any thread.
 */
uint8_t *xemu_image_decode_rgba(const uint8_t *data, size_t size,
                                unsigned int *width, unsigned int *height);
void xemu_image_free(uint8_t *pixels);

/* Writes tightly packed RGBA8 pixels to a PNG file. */
bool xemu_image_save_png_rgba(const char *path, const uint8_t *rgba,
                              unsigned int width, unsigned int height);

#ifdef __cplusplus
}
#endif

#endif
