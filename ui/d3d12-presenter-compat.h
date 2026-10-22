/*
 * Copyright (c) 2026 Skitzelles - part of xemu for Xbox Dev Mode (github.com/skitzellez/xemu-uwp). GPL-2.0-or-later.
 *
 * New graphics presenter: the build it came from (another Xbox port of xemu)
 * also streams the picture and aims a light gun; ours has neither, so these
 * stand-ins keep the presenter code as written.
 */

#ifndef UI_D3D12_PRESENTER_COMPAT_H
#define UI_D3D12_PRESENTER_COMPAT_H

typedef void (*XemuStreamFrameRelease)(void *opaque);

static inline bool xemu_stream_is_active(void) { return false; }
static inline bool xemu_stream_wants_frame(void) { return false; }
static inline void xemu_stream_submit_bgra_borrowed(const uint8_t *bgra, int width, int height, int pitch,
                                                    int64_t capture_us, XemuStreamFrameRelease release,
                                                    void *opaque)
{
    release(opaque);
}
static inline void xemu_pointer_aim_set_display_rect(int x, int y, int w, int h) {}

#endif
