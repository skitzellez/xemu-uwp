/*
 * Copyright (c) 2026 Skitzelles - part of xemu for Xbox Dev Mode (github.com/skitzellez/xemu-uwp). GPL-2.0-or-later.
 *
 * New graphics presenter: the build it came from (danprice142's Xbox port of
 * xemu, github.com/danprice142/xemu-uwp)
 * also streams the picture and aims a light gun; ours has neither, so these
 * stand-ins keep the presenter code as written.
 */

#ifndef UI_D3D12_PRESENTER_COMPAT_H
#define UI_D3D12_PRESENTER_COMPAT_H

typedef void (*XemuStreamFrameRelease)(void *opaque);

/*
 * Testing (latency-probe.txt in xemu's folder): the picture's way here is used to time a button press to the
 * screen. Every shown frame is copied back, compared with the one before on a sparse grid, and while a test
 * press (press= in the link) is under 3 s old the change is logged with the time since the press. The first
 * big change after a press is when the game showed it.
 */
extern "C" {
extern int xemu_uwp_latency_probe;          /* ui/xemu.c: 1 when latency-probe.txt exists (-1: not looked yet) */
extern int64_t xemu_uwp_probe_press_us;     /* ui/xemu.c: when the last test press started (monotonic us) */
void uwp_log(const char *fmt, ...);
}
static inline bool xemu_stream_is_active(void) { return xemu_uwp_latency_probe == 1; }
static inline bool xemu_stream_wants_frame(void) { return xemu_uwp_latency_probe == 1; }
static inline void xemu_stream_submit_bgra_borrowed(const uint8_t *bgra, int width, int height, int pitch,
                                                    int64_t capture_us, XemuStreamFrameRelease release,
                                                    void *opaque)
{
    enum { GX = 64, GY = 36 };
    static uint8_t prev[GX * GY];
    static bool have_prev = false;
    uint8_t now[GX * GY];
    int64_t sum = 0;
    for (int y = 0; y < GY; y++) {
        const uint8_t *row = bgra + (size_t)((y * 2 + 1) * height / (GY * 2)) * pitch;
        for (int x = 0; x < GX; x++) {
            const uint8_t *px = row + (size_t)((x * 2 + 1) * width / (GX * 2)) * 4;
            uint8_t l = (uint8_t)((px[0] + 2 * px[1] + px[2]) / 4);
            now[y * GX + x] = l;
            sum += have_prev ? (l > prev[y * GX + x] ? l - prev[y * GX + x] : prev[y * GX + x] - l) : 0;
        }
    }
    memcpy(prev, now, sizeof(prev));
    have_prev = true;
    release(opaque);
    if (!xemu_uwp_probe_press_us) {
        return;
    }
    int64_t since = capture_us - xemu_uwp_probe_press_us;
    if (since >= -200000 && since < 1500000) {
        uwp_log("probe: %+lld ms diff %lld", (long long)(since / 1000), (long long)(sum * 100 / (GX * GY)));
    }
}
static inline void xemu_pointer_aim_set_display_rect(int x, int y, int w, int h) {}

#endif
