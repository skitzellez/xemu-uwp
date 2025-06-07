#ifndef HW_XBOX_TEXSIGNS_H
#define HW_XBOX_TEXSIGNS_H

void texsigns_init_conversion(void);
/* the sign bits of a NV_PGRAPH_TEXFILTER0 value as TEX_CHANNEL_* (0 with test file signed-tex-off.txt) */
uint8_t texsigns_channel_signs(uint32_t filter);
void texsigns_inplace_to_unsigned_rgba(uint8_t *data, int len, int channel_signs);

uint8_t *texsigns_convert_argb1555(
    const uint8_t *data,
    int width,
    int height,
    int depth,
    int row_pitch,
    int slice_pitch,
    int channel_signs,
    size_t *out_converted_size);

uint8_t *texsigns_convert_xrgb1555(
    const uint8_t *data,
    int width,
    int height,
    int depth,
    int row_pitch,
    int slice_pitch,
    int channel_signs,
    size_t *out_converted_size);

uint8_t *texsigns_convert_argb4444(
    const uint8_t *data,
    int width,
    int height,
    int depth,
    int row_pitch,
    int slice_pitch,
    int channel_signs,
    size_t *out_converted_size);

uint8_t *texsigns_convert_rgb565(
    const uint8_t *data,
    int width,
    int height,
    int depth,
    int row_pitch,
    int slice_pitch,
    int channel_signs,
    size_t *out_converted_size);

uint8_t *texsigns_convert_rgb655(
    const uint8_t *data,
    int width,
    int height,
    int depth,
    int row_pitch,
    int slice_pitch,
    int channel_signs,
    size_t *out_converted_size);

uint8_t *texsigns_convert_y8(
    const uint8_t *data,
    int width,
    int height,
    int depth,
    int row_pitch,
    int slice_pitch,
    int channel_signs,
    size_t *out_converted_size);

uint8_t *texsigns_convert_y16(
    const uint8_t *data,
    int width,
    int height,
    int depth,
    int row_pitch,
    int slice_pitch,
    size_t *out_converted_size);

uint8_t *texsigns_convert_ay8(
    const uint8_t *data,
    int width,
    int height,
    int depth,
    int row_pitch,
    int slice_pitch,
    int channel_signs,
    size_t *out_converted_size);

uint8_t *texsigns_convert_a8(
    const uint8_t *data,
    int width,
    int height,
    int depth,
    int row_pitch,
    int slice_pitch,
    int channel_signs,
    size_t *out_converted_size);

uint8_t *texsigns_convert_ay88(
    const uint8_t *data,
    int width,
    int height,
    int depth,
    int row_pitch,
    int slice_pitch,
    int channel_signs,
    size_t *out_converted_size);

uint8_t *texsigns_convert_rb88(
    const uint8_t *data,
    int width,
    int height,
    int depth,
    int row_pitch,
    int slice_pitch,
    int channel_signs,
    size_t *out_converted_size);

uint8_t *texsigns_convert_gb88(
    const uint8_t *data,
    int width,
    int height,
    int depth,
    int row_pitch,
    int slice_pitch,
    int channel_signs,
    size_t *out_converted_size);

uint8_t *texsigns_convert_r16b16(
    const uint8_t *data,
    int width,
    int height,
    int depth,
    int row_pitch,
    int slice_pitch,
    size_t *out_converted_size);

uint8_t *texsigns_convert_argb8888(
    const uint8_t *data,
    int width,
    int height,
    int depth,
    int row_pitch,
    int slice_pitch,
    int channel_signs,
    size_t *out_converted_size);

uint8_t *texsigns_convert_xrgb8888(
    const uint8_t *data,
    int width,
    int height,
    int depth,
    int row_pitch,
    int slice_pitch,
    int channel_signs,
    size_t *out_converted_size);

uint8_t *texsigns_convert_abgr8888(
    const uint8_t *data,
    int width,
    int height,
    int depth,
    int row_pitch,
    int slice_pitch,
    int channel_signs,
    size_t *out_converted_size);

uint8_t *texsigns_convert_bgra8888(
    const uint8_t *data,
    int width,
    int height,
    int depth,
    int row_pitch,
    int slice_pitch,
    int channel_signs,
    size_t *out_converted_size);

uint8_t *texsigns_convert_rgba8888(
    const uint8_t *data,
    int width,
    int height,
    int depth,
    int row_pitch,
    int slice_pitch,
    int channel_signs,
    size_t *out_converted_size);

uint8_t *texsigns_convert_i8_argb8888(
    const uint8_t *data,
    const uint8_t *palette_data,
    int width,
    int height,
    int depth,
    int row_pitch,
    int slice_pitch,
    int channel_signs,
    size_t *out_converted_size);

#endif
