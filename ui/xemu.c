/*
 * xemu SDL display driver
 *
 * Copyright (c) 2020-2025 Matt Borgerson
 *
 * Based on sdl2.c, sdl2-gl.c
 *
 * Copyright (c) 2003 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
/* Ported SDL 1.2 code to 2.0 by Dave Airlie. */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/thread.h"
#include "qemu/main-loop.h"
#include "qemu/rcu.h"
#include "qemu-version.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-block.h"
#include "qobject/qdict.h"
#include "ui/console.h"
#include "ui/input.h"
#include "ui/kbd-state.h"
#include "system/runstate.h"
#include "system/runstate-action.h"
#include "block/block.h"
#include "system/system.h"
#include "xui/xemu-hud.h"
#include "xemu-input.h"
#include "xemu-settings.h"
#include "xemu-snapshots.h"
#include "xemu-notifications.h"
#include "xemu-version.h"
#include "xemu-os-utils.h"

#include "data/xemu_64x64.png.h"

#include "hw/xbox/smbus.h" // For eject, drive tray
#include "hw/xbox/nv2a/nv2a.h"
#include "hw/xbox/nv2a/debug.h"
#ifdef XEMU_UWP
#include "xemu-uwp-audio.h"
#include <io.h>
#include <fcntl.h>
#endif
#include "qemu/xemu-uwp.h"
#include "system/block-backend.h"
#include "system/blockdev.h"
#include "xemu-uwp-dash.h"
#include <signal.h>
#include <cpuid.h>
#include <x86intrin.h>
#include "ui/xemu-notifications.h"

#include <stb_image.h>
#include <locale.h>
#include <glib/gstdio.h>
#include <math.h>
#include <SDL3/SDL.h>

#ifdef _WIN32
#include "xui/win32-dxgi-present.h"
#endif


#ifndef DEBUG_XEMU_C
#define DEBUG_XEMU_C 0
#endif

#if DEBUG_XEMU_C
#define DPRINTF(...) fprintf(stderr, __VA_ARGS__)
#else
#define DPRINTF(...)
#endif

uint64_t vblank_interval_ns = 16666666LL;
bool use_vblank_timer_thread = true;

struct xemu_console {
    DisplayChangeListener dcl;
    DisplaySurface *surface;
    DisplayOptions *opts;
    SDL_Window *real_window;
    int idx;
    int hidden;
    int ignore_hotkeys;
    SDL_GLContext winctx;
    QKbdState *kbd;
};

#ifdef _WIN32
#include "nvapi.h"
// Provide hint to prefer high-performance graphics for hybrid systems
// https://gpuopen.com/learn/amdpowerxpressrequesthighperformance/
__declspec(dllexport) DWORD AmdPowerXpressRequestHighPerformance = 1;
// https://docs.nvidia.com/gameworks/content/technologies/desktop/optimus.htm
__declspec(dllexport) DWORD NvOptimusEnablement = 1;
#endif

static int num_outputs;
static struct xemu_console *scon_list;
static SDL_Surface *guest_sprite_surface;
static int gui_grab; /* if true, all keyboard/mouse events are grabbed */
static bool alt_grab;
static bool ctrl_grab;
static int gui_saved_grab;
static int gui_fullscreen;
static int gui_grab_code = SDL_KMOD_LALT | SDL_KMOD_LCTRL;
static SDL_Cursor *sdl_cursor_normal;
static SDL_Cursor *sdl_cursor_hidden;
static int absolute_enabled;
static int guest_cursor;
static int guest_x, guest_y;
static SDL_Cursor *guest_sprite;
static Notifier mouse_mode_notifier;
static SDL_Window *m_window;
static SDL_GLContext m_context;
static QemuSemaphore display_init_sem;
static QemuSemaphore display_shutdown_sem;
static QEMUTimer *vblank_timer;
static QemuThread vblank_thread;
static bool qemu_exiting;
static int exit_status;

void tcg_register_init_ctx(void); // tcg.c

#if DEBUG_XEMU_C
static uint64_t lock_held_acc;
static uint64_t lock_start;
#endif

void xemu_main_loop_lock(void)
{
    qemu_mutex_lock_main_loop();
    bql_lock();
#if DEBUG_XEMU_C
    lock_start = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
#endif
}

void xemu_main_loop_unlock(void)
{
#if DEBUG_XEMU_C
    lock_held_acc += qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - lock_start;
#endif
    bql_unlock();
    qemu_mutex_unlock_main_loop();
}

SDL_Window *xemu_get_window(void)
{
    return m_window;
}

static struct xemu_console *get_scon_from_window(uint32_t window_id)
{
    int i;
    for (i = 0; i < num_outputs; i++) {
        if (scon_list[i].real_window == SDL_GetWindowFromID(window_id)) {
            return &scon_list[i];
        }
    }
    return NULL;
}

static void window_resize(struct xemu_console *scon)
{
    if (!scon->real_window) {
        return;
    }

    SDL_SetWindowSize(scon->real_window,
                      surface_width(scon->surface),
                      surface_height(scon->surface));
}

static void hide_cursor(struct xemu_console *scon)
{
    if (scon->opts->has_show_cursor && scon->opts->show_cursor) {
        return;
    }

    SDL_HideCursor();
    SDL_SetCursor(sdl_cursor_hidden);

    if (!qemu_input_is_absolute(scon->dcl.con)) {
        SDL_SetWindowRelativeMouseMode(scon->real_window, true);
    }
}

static void show_cursor(struct xemu_console *scon)
{
    if (scon->opts->has_show_cursor && scon->opts->show_cursor) {
        return;
    }

    if (!qemu_input_is_absolute(scon->dcl.con)) {
        SDL_SetWindowRelativeMouseMode(scon->real_window, false);
    }

    if (guest_cursor &&
        (gui_grab || qemu_input_is_absolute(scon->dcl.con) || absolute_enabled)) {
        SDL_SetCursor(guest_sprite);
    } else {
        SDL_SetCursor(sdl_cursor_normal);
    }

    SDL_ShowCursor();
}

static void grab_start(struct xemu_console *scon)
{
}

static void grab_end(struct xemu_console *scon)
{
    SDL_SetWindowKeyboardGrab(scon->real_window, false);
    SDL_SetWindowMouseGrab(scon->real_window, false);
    gui_grab = 0;
    show_cursor(scon);
}

static void absolute_mouse_grab(struct xemu_console *scon)
{
    float mouse_x, mouse_y;
    int scr_w, scr_h;
    SDL_GetMouseState(&mouse_x, &mouse_y);
    SDL_GetWindowSize(scon->real_window, &scr_w, &scr_h);
    if (mouse_x > 0 && mouse_x < scr_w - 1 &&
        mouse_y > 0 && mouse_y < scr_h - 1) {
        grab_start(scon);
    }
}

static void mouse_mode_change(Notifier *notify, void *data)
{
    if (qemu_input_is_absolute(scon_list[0].dcl.con)) {
        if (!absolute_enabled) {
            absolute_enabled = 1;
            SDL_SetWindowRelativeMouseMode(scon_list[0].real_window, false);
            absolute_mouse_grab(&scon_list[0]);
        }
    } else if (absolute_enabled) {
        if (!gui_fullscreen) {
            grab_end(&scon_list[0]);
        }
        absolute_enabled = 0;
    }
}

static void send_mouse_event(struct xemu_console *scon, int dx, int dy,
                                 int x, int y, int state)
{
    static uint32_t bmap[INPUT_BUTTON__MAX] = {
        [INPUT_BUTTON_LEFT]       = SDL_BUTTON_MASK(SDL_BUTTON_LEFT),
        [INPUT_BUTTON_MIDDLE]     = SDL_BUTTON_MASK(SDL_BUTTON_MIDDLE),
        [INPUT_BUTTON_RIGHT]      = SDL_BUTTON_MASK(SDL_BUTTON_RIGHT),
    };
    static uint32_t prev_state;

    if (prev_state != state) {
        qemu_input_update_buttons(scon->dcl.con, bmap, prev_state, state);
        prev_state = state;
    }

    if (qemu_input_is_absolute(scon->dcl.con)) {
        qemu_input_queue_abs(scon->dcl.con, INPUT_AXIS_X,
                             x, 0, surface_width(scon->surface));
        qemu_input_queue_abs(scon->dcl.con, INPUT_AXIS_Y,
                             y, 0, surface_height(scon->surface));
    } else {
        if (guest_cursor) {
            x -= guest_x;
            y -= guest_y;
            guest_x += x;
            guest_y += y;
            dx = x;
            dy = y;
        }
        qemu_input_queue_rel(scon->dcl.con, INPUT_AXIS_X, dx);
        qemu_input_queue_rel(scon->dcl.con, INPUT_AXIS_Y, dy);
    }
    qemu_input_event_sync();
}

static void set_full_screen(struct xemu_console *scon, bool set)
{
    gui_fullscreen = set;

    if (gui_fullscreen) {
        const SDL_DisplayMode *mode = NULL;
        SDL_DisplayMode **modes = NULL;
        if (g_config.display.window.fullscreen_exclusive) {
            SDL_DisplayID display = SDL_GetDisplayForWindow(scon->real_window);
            if (display) {
                int num_modes = 0;
                modes = SDL_GetFullscreenDisplayModes(display, &num_modes);
                if (modes && num_modes > 0) {
                    // First mode is the highest resolution, typically the native resolution
                    mode = modes[0];
                }
            }
            if (mode) {
                fprintf(stderr, "Selected exclusive fullscreen mode: %dx%d pixel_density=%f refresh_rate=%f\n", mode->w, mode->h, mode->pixel_density, mode->refresh_rate);
            } else {
                fprintf(stderr, "Failed to get fullscreen display mode: %s\n", SDL_GetError());
            }
        }
        SDL_SetWindowFullscreenMode(scon->real_window, mode);
        SDL_free(modes);
        SDL_SetWindowFullscreen(scon->real_window, true);
        gui_saved_grab = gui_grab;
        grab_start(scon);
    } else {
        if (!gui_saved_grab) {
            grab_end(scon);
        }
        SDL_SetWindowFullscreen(scon->real_window, false);
    }
}

static void toggle_full_screen(struct xemu_console *scon)
{
    set_full_screen(scon, !gui_fullscreen);
}

void xemu_toggle_fullscreen(void)
{
    toggle_full_screen(&scon_list[0]);
}

int xemu_is_fullscreen(void)
{
    return gui_fullscreen;
}

static int get_mod_state(void)
{
    SDL_Keymod mod = SDL_GetModState();

    if (alt_grab) {
        return (mod & (gui_grab_code | SDL_KMOD_LSHIFT)) ==
            (gui_grab_code | SDL_KMOD_LSHIFT);
    } else if (ctrl_grab) {
        return (mod & SDL_KMOD_RCTRL) == SDL_KMOD_RCTRL;
    } else {
        return (mod & gui_grab_code) == gui_grab_code;
    }
}

static void process_key(struct xemu_console *scon, SDL_KeyboardEvent *ev)
{
    int qcode;

    if (ev->scancode >= qemu_input_map_usb_to_qcode_len) {
        return;
    }
    qcode = qemu_input_map_usb_to_qcode[ev->scancode];
    qkbd_state_key_event(scon->kbd, qcode, ev->type == SDL_EVENT_KEY_DOWN);
}

static void handle_keydown(SDL_Event *ev)
{
    int win;
    struct xemu_console *scon = get_scon_from_window(ev->key.windowID);
    if (scon == NULL) return;
    int gui_key_modifier_pressed = get_mod_state();
    int gui_keysym = 0;

    if (!scon->ignore_hotkeys && gui_key_modifier_pressed && !ev->key.repeat) {
        switch (ev->key.scancode) {
        case SDL_SCANCODE_2:
        case SDL_SCANCODE_3:
        case SDL_SCANCODE_4:
        case SDL_SCANCODE_5:
        case SDL_SCANCODE_6:
        case SDL_SCANCODE_7:
        case SDL_SCANCODE_8:
        case SDL_SCANCODE_9:
            if (gui_grab) {
                grab_end(scon);
            }

            win = ev->key.scancode - SDL_SCANCODE_1;
            if (win < num_outputs) {
                scon_list[win].hidden = !scon_list[win].hidden;
                if (scon_list[win].real_window) {
                    if (scon_list[win].hidden) {
                        SDL_HideWindow(scon_list[win].real_window);
                    } else {
                        SDL_ShowWindow(scon_list[win].real_window);
                    }
                }
                gui_keysym = 1;
            }
            break;
        case SDL_SCANCODE_F:
            toggle_full_screen(scon);
            gui_keysym = 1;
            break;
        case SDL_SCANCODE_G:
            gui_keysym = 1;
            if (!gui_grab) {
                grab_start(scon);
            } else if (!gui_fullscreen) {
                grab_end(scon);
            }
            break;
        case SDL_SCANCODE_U:
            window_resize(scon);
            gui_keysym = 1;
            break;
        default:
            break;
        }
    }
    if (!gui_keysym) {
        process_key(scon, &ev->key);
    }
}

static void handle_keyup(SDL_Event *ev)
{
    struct xemu_console *scon = get_scon_from_window(ev->key.windowID);
    if (!scon) return;

    scon->ignore_hotkeys = false;
    process_key(scon, &ev->key);
}

static void handle_mousemotion(SDL_Event *ev)
{
    int max_x, max_y;
    struct xemu_console *scon = get_scon_from_window(ev->motion.windowID);

    if (!scon || !qemu_console_is_graphic(scon->dcl.con)) {
        return;
    }

    if (qemu_input_is_absolute(scon->dcl.con) || absolute_enabled) {
        int scr_w, scr_h;
        SDL_GetWindowSize(scon->real_window, &scr_w, &scr_h);
        max_x = scr_w - 1;
        max_y = scr_h - 1;
        if (gui_grab && !gui_fullscreen
            && (ev->motion.x == 0 || ev->motion.y == 0 ||
                ev->motion.x == max_x || ev->motion.y == max_y)) {
            grab_end(scon);
        }
        if (!gui_grab &&
            (ev->motion.x > 0 && ev->motion.x < max_x &&
             ev->motion.y > 0 && ev->motion.y < max_y)) {
            grab_start(scon);
        }
    }
    if (gui_grab || qemu_input_is_absolute(scon->dcl.con) || absolute_enabled) {
        send_mouse_event(scon, ev->motion.xrel, ev->motion.yrel,
                             ev->motion.x, ev->motion.y, ev->motion.state);
    }
}

static void handle_mousebutton(SDL_Event *ev)
{
    int buttonstate = SDL_GetMouseState(NULL, NULL);
    SDL_MouseButtonEvent *bev;
    struct xemu_console *scon = get_scon_from_window(ev->button.windowID);

    if (!scon || !qemu_console_is_graphic(scon->dcl.con)) {
        return;
    }

    bev = &ev->button;
    if (!gui_grab && !qemu_input_is_absolute(scon->dcl.con)) {
        if (ev->type == SDL_EVENT_MOUSE_BUTTON_UP && bev->button == SDL_BUTTON_LEFT) {
            /* start grabbing all events */
            grab_start(scon);
        }
    } else {
        if (ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
            buttonstate |= SDL_BUTTON_MASK(bev->button);
        } else {
            buttonstate &= ~SDL_BUTTON_MASK(bev->button);
        }
        send_mouse_event(scon, 0, 0, bev->x, bev->y, buttonstate);
    }
}

static void handle_mousewheel(SDL_Event *ev)
{
    struct xemu_console *scon = get_scon_from_window(ev->wheel.windowID);
    SDL_MouseWheelEvent *wev = &ev->wheel;
    InputButton btn;

    if (!scon || !qemu_console_is_graphic(scon->dcl.con)) {
        return;
    }

    if (wev->y > 0) {
        btn = INPUT_BUTTON_WHEEL_UP;
    } else if (wev->y < 0) {
        btn = INPUT_BUTTON_WHEEL_DOWN;
    } else {
        return;
    }

    qemu_input_queue_btn(scon->dcl.con, btn, true);
    qemu_input_event_sync();
    qemu_input_queue_btn(scon->dcl.con, btn, false);
    qemu_input_event_sync();
}

static void handle_windowevent(SDL_Event *ev)
{
    struct xemu_console *scon = get_scon_from_window(ev->window.windowID);
    bool allow_close = true;

    if (!scon) {
        return;
    }

    switch (ev->type) {
    case SDL_EVENT_WINDOW_RESIZED:
        {
            QemuUIInfo info;
            memset(&info, 0, sizeof(info));
            info.width = ev->window.data1;
            info.height = ev->window.data2;
            dpy_set_ui_info(scon->dcl.con, &info, true);

            if (!gui_fullscreen) {
                g_config.display.window.last_width = ev->window.data1;
                g_config.display.window.last_height = ev->window.data2;
            }

#ifdef _WIN32
            if (win32_dxgi_present_is_active()) {
                int width;
                int height;
                if (!SDL_GetWindowSizeInPixels(scon->real_window, &width,
                                               &height)) {
                    fprintf(stderr, "SDL_GetWindowSizeInPixels failed "
                                    "responding to resize event.\n");
                } else {
                    win32_dxgi_present_resize(width, height);
                }
            }
#endif
        }
        break;
#ifdef _WIN32
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        if (win32_dxgi_present_is_active()) {
            win32_dxgi_present_resize(ev->window.data1, ev->window.data2);
        }
        break;
#endif

    case SDL_EVENT_WINDOW_FOCUS_GAINED:
    case SDL_EVENT_WINDOW_MOUSE_ENTER:
        if (!gui_grab && (qemu_input_is_absolute(scon->dcl.con) || absolute_enabled)) {
            absolute_mouse_grab(scon);
        }
        /* If a new console window opened using a hotkey receives the
         * focus, SDL sends another KEYDOWN event to the new window,
         * closing the console window immediately after.
         *
         * Work around this by ignoring further hotkey events until a
         * key is released.
         */
        scon->ignore_hotkeys = get_mod_state();
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        if (gui_grab && !gui_fullscreen) {
            grab_end(scon);
        }
        break;
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        if (qemu_console_is_graphic(scon->dcl.con)) {
            if (scon->opts->has_window_close && !scon->opts->window_close) {
                allow_close = false;
            }
            if (allow_close) {
                shutdown_action = SHUTDOWN_ACTION_POWEROFF;
                qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_UI);
            }
        } else {
            SDL_HideWindow(scon->real_window);
            scon->hidden = true;
        }
        break;
    case SDL_EVENT_WINDOW_SHOWN:
        scon->hidden = false;
        break;
    case SDL_EVENT_WINDOW_HIDDEN:
        scon->hidden = true;
        break;
    }
}

static void mouse_warp(DisplayChangeListener *dcl,
                       int x, int y, bool on)
{
    struct xemu_console *scon = container_of(dcl, struct xemu_console, dcl);

    if (!qemu_console_is_graphic(scon->dcl.con)) {
        return;
    }

    if (on) {
        if (!guest_cursor) {
            show_cursor(scon);
        }
        if (gui_grab || qemu_input_is_absolute(scon->dcl.con) || absolute_enabled) {
            SDL_SetCursor(guest_sprite);
            if (!qemu_input_is_absolute(scon->dcl.con) && !absolute_enabled) {
                SDL_WarpMouseInWindow(scon->real_window, x, y);
            }
        }
    } else if (gui_grab) {
        hide_cursor(scon);
    }
    guest_cursor = on;
    guest_x = x, guest_y = y;
}

static void mouse_define(DisplayChangeListener *dcl,
                             QEMUCursor *c)
{

    if (guest_sprite) {
        SDL_DestroyCursor(guest_sprite);
    }

    if (guest_sprite_surface) {
        SDL_DestroySurface(guest_sprite_surface);
    }

    guest_sprite_surface =
        SDL_CreateSurfaceFrom(c->width, c->height, SDL_PIXELFORMAT_ARGB8888, c->data, c->width * 4);

    if (!guest_sprite_surface) {
        fprintf(stderr, "Failed to make rgb surface from %p\n", c);
        return;
    }
    guest_sprite = SDL_CreateColorCursor(guest_sprite_surface,
                                         c->hot_x, c->hot_y);
    if (!guest_sprite) {
        fprintf(stderr, "Failed to make color cursor from %p\n", c);
        return;
    }
    if (guest_cursor &&
        (gui_grab || qemu_input_is_absolute(dcl->con) || absolute_enabled)) {
        SDL_SetCursor(guest_sprite);
    }
}

static void xb_surface_gl_create_texture_format(DisplaySurface *surface)
{
    assert(QEMU_IS_ALIGNED(surface_stride(surface), surface_bytes_per_pixel(surface)));

    switch (surface_format(surface)) {
    case PIXMAN_BE_b8g8r8x8:
    case PIXMAN_BE_b8g8r8a8:
        surface->glformat = GL_BGRA_EXT;
        surface->gltype = GL_UNSIGNED_BYTE;
        break;
    case PIXMAN_BE_x8r8g8b8:
    case PIXMAN_BE_a8r8g8b8:
        surface->glformat = GL_RGBA;
        surface->gltype = GL_UNSIGNED_BYTE;
        break;
    case PIXMAN_r5g6b5:
        surface->glformat = GL_RGB;
        surface->gltype = GL_UNSIGNED_SHORT_5_6_5;
        break;
    default:
        g_assert_not_reached();
    }
}

static G_GNUC_UNUSED void xb_surface_gl_create_texture(DisplaySurface *surface)
{
    xb_surface_gl_create_texture_format(surface);
    if (!surface->texture) {
        glGenTextures(1, &surface->texture);
    }
    glBindTexture(GL_TEXTURE_2D, surface->texture);
    glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT,
                  surface_stride(surface) / surface_bytes_per_pixel(surface));
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB,
                 surface_width(surface),
                 surface_height(surface),
                 0, surface->glformat, surface->gltype,
                 surface_data(surface));
    glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT, 0);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
}

#ifdef XEMU_UWP
/*
 * The picture when the GPU shows no surface (a game drawing its screen with the CPU, the free BIOS's boot
 * screens, a game stuck before its first frame): one texture, kept, and refilled in place. Upstream makes a
 * new texture every frame and deletes it after; on the console's GL-on-D3D12 driver that leaked about 7 MB a
 * second at 4K (players' "never drew a frame" sessions climbed to the 5 GB limit and crashed in the driver).
 */
static GLuint uwp_fallback_texture(DisplaySurface *surface)
{
    static GLuint tex;
    static int tex_w, tex_h;
    static GLenum tex_format, tex_type;

    xb_surface_gl_create_texture_format(surface);
    int w =surface_width(surface), h = surface_height(surface);
    bool same = tex && w == tex_w && h == tex_h && surface->glformat == tex_format && surface->gltype == tex_type;
    if (!tex) {
        glGenTextures(1, &tex);
    }
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT, surface_stride(surface) / surface_bytes_per_pixel(surface));
    if (same) {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, surface->glformat, surface->gltype, surface_data(surface));
    } else {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, surface->glformat, surface->gltype, surface_data(surface));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        tex_w = w;
        tex_h = h;
        tex_format = surface->glformat;
        tex_type = surface->gltype;
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT, 0);
    return tex;
}
#endif

static void xb_surface_gl_destroy_texture(DisplaySurface *surface)
{
    if (!surface || !surface->texture) {
        return;
    }
    glDeleteTextures(1, &surface->texture);
    surface->texture = 0;
}

static bool xb_console_gl_check_format(DisplayChangeListener *dcl,
                                       pixman_format_code_t format)
{
    switch (format) {
    case PIXMAN_BE_b8g8r8x8:
    case PIXMAN_BE_b8g8r8a8:
    case PIXMAN_r5g6b5:
        return true;
    default:
        return false;
    }
}

static void gl_switch(DisplayChangeListener *dcl,
                      DisplaySurface *new_surface)
{
    struct xemu_console *scon = container_of(dcl, struct xemu_console, dcl);
    scon->surface = new_surface;
}

static float update_avg(float avg, float ms, float r) {
    if (fabs(avg-ms) > 0.25*avg) avg = ms;
    else avg = avg*(1.0-r)+ms*r;
    return avg;
}

static float fps = 1.0;

static void update_fps(void)
{
    static float avg = 1.0;
    static int64_t last_update = 0;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    if (!last_update) {
        last_update = now;
        return;
    }
    float ms = ((float)(now-last_update)/1000000.0);
    last_update = now;
    avg = update_avg(avg, ms, 0.5);
    fps = 1000.0/avg;
}

static void process_vblank(struct xemu_console *scon)
{
    assert(bql_locked());

    update_fps();

#if 0
    static uint64_t last_ns = 0;
    uint64_t now_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    uint64_t delta_ns = last_ns ? now_ns - last_ns : 0;
    fprintf(stderr, "%s delta_ns=%"PRId64"\n", __func__, delta_ns);
    last_ns = now_ns;
#endif

    graphic_hw_update(scon->dcl.con);
}

static void vblank_timer_callback(void *opaque)
{
    struct xemu_console *scon = (struct xemu_console *)opaque;

    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    process_vblank(scon);
    timer_mod_ns(vblank_timer, now + vblank_interval_ns);
}

static void *vblank_timer_thread(void *opaque)
{
    struct xemu_console *scon = (struct xemu_console *)opaque;
    int64_t next_vblank = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);

    while (!qatomic_read(&qemu_exiting)) {
        // Schedule next vblank at fixed interval (absolute deadline)
        next_vblank += vblank_interval_ns;

        // Wait until deadline
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        if (now < next_vblank) {
            SDL_DelayPrecise(next_vblank - now);
        } else if (now > next_vblank + vblank_interval_ns) {
            // We've fallen behind by more than one frame, reset to avoid
            // rapid-fire catch-up
            next_vblank = now;
        }

        if (!qatomic_read(&qemu_exiting)) {
            xemu_main_loop_lock();
            process_vblank(scon);
            xemu_main_loop_unlock();
        }
    }

    return NULL;
}

#if DEBUG_XEMU_C
static void report_stats(void)
{
    uint64_t now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    static uint64_t last_reported = 0;
    static int num_frames = 0;
    uint64_t delta_ms = now - last_reported;
    num_frames += 1;
    if (delta_ms >= 1000) {
        DPRINTF("[[ ");
        DPRINTF("vblank @%fHz avg", fps);
        DPRINTF(" - bql %"PRId64"ns/iter, %g%% time avg", lock_held_acc/num_frames, (double)lock_held_acc/(double)(delta_ms * 10000.0));
        DPRINTF(" ]]\n");
        lock_held_acc = 0;
        last_reported = now;
        num_frames = 0;
    }
}
#endif

/**
 * Renders the main interface. Usually called from the main thread,
 * but may sometimes be called from another thread.
 */
static void gl_render_frame(struct xemu_console *scon)
{
    static bool rendering;
    if (qatomic_xchg(&rendering, true) || qatomic_read(&qemu_exiting)) {
        return;
    }

    SDL_GL_MakeCurrent(scon->real_window, scon->winctx);

    bool flip_required = false;
    bool release_surface_texture = false;

    /* XXX: Note that this bypasses the usual VGA path in order to quickly
     * get the surface. This is simple and fast, at the cost of accuracy.
     * Ideally, this should go through the VGA code and opportunistically pull
     * the surface like this, but handle the VGA logic as well. For now, just
     * use this fast path to handle the common case.
     *
     * In the event the surface is not found in the surface cache, e.g. when
     * the guest code isn't using HW accelerated rendering, but just blitting
     * to the framebuffer, fall back to the VGA path.
     */
    UWP_PROF_BEGIN(t_getfb);
    GLuint tex = nv2a_get_framebuffer_surface();
    UWP_PROF_END(UI_GETFB, t_getfb);
    if (tex) {
        nv2a_uwp_wait_display();   /* the picture is drawn on the GPU before this context reads it */
    }

    assert(glGetError() == GL_NO_ERROR);

    if (tex == 0) {
        xemu_main_loop_lock();
        // FIXME: Don't upload if notdirty
#ifdef XEMU_UWP
        tex = uwp_fallback_texture(scon->surface);   /* kept: not released after the frame */
#else
        xb_surface_gl_create_texture(scon->surface);
        tex = scon->surface->texture;
        release_surface_texture = true;
#endif
        flip_required = true;
        xemu_main_loop_unlock();
    }

#ifdef _WIN32
    if (win32_dxgi_present_is_active()) {
        win32_dxgi_present_begin_frame();
    }
#endif

    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    xemu_snapshots_set_framebuffer_texture(tex, flip_required);
    xemu_hud_set_framebuffer_texture(tex, flip_required);

    /* FIXME: Finer locking. Event handlers in segments of the code expect
     * to be running on the main thread with the BQL. For now, acquire the
     * lock and perform rendering, but release before swap to avoid
     * possible lengthy blocking (for vsync).
     */
    xemu_main_loop_lock();
    xemu_hud_update();
    xemu_main_loop_unlock();

    UWP_PROF_BEGIN(t_hud);
    xemu_hud_render();
    UWP_PROF_END(UI_HUD, t_hud);
    UWP_PROF_BEGIN(t_finish);
    glFinish();
    UWP_PROF_END(UI_FINISH, t_finish);

    if (release_surface_texture) {
        xemu_main_loop_lock();
        xb_surface_gl_destroy_texture(scon->surface);
        xemu_main_loop_unlock();
    }

    nv2a_release_framebuffer_surface();

#ifdef _WIN32
    if (win32_dxgi_present_is_active()) {
        win32_dxgi_present_end_frame(g_config.display.window.vsync);
    } else {
        static bool warned = false;
        if (!warned) {
            fprintf(stderr,
                    "win32_dxgi present failed or unavailable, falling back to "
                    "SDL_GL_SwapWindow\n");
            warned = true;
        }
        UWP_PROF_BEGIN(t_swap);
        SDL_GL_SwapWindow(scon->real_window);
        UWP_PROF_END(UI_SWAP, t_swap);
    }
#else
    SDL_GL_SwapWindow(scon->real_window);
#endif
    assert(glGetError() == GL_NO_ERROR);

    qatomic_set(&rendering, false);

#if DEBUG_XEMU_C
    report_stats();
#endif
}

static bool event_watch_callback(void *userdata, SDL_Event *event)
{
    struct xemu_console *scon = (struct xemu_console *)userdata;

    if (event->type == SDL_EVENT_WINDOW_RESIZED) {
#ifdef _WIN32
        if (win32_dxgi_present_is_active()) {
            int width;
            int height;
            if (!SDL_GetWindowSizeInPixels(scon->real_window, &width,
                                           &height)) {
                fprintf(stderr, "SDL_GetWindowSizeInPixels failed "
                                "responding to resize event.\n");
            } else {
                win32_dxgi_present_resize(width, height);
            }
        }
#endif
        gl_render_frame(scon);
    } else if (event->type == SDL_EVENT_WINDOW_EXPOSED) {
        gl_render_frame(scon);
    }

    return true; // Ignored
}

static void poll_events(struct xemu_console *scon)
{
    SDL_Event ev1, *ev = &ev1;
    bool allow_close = true;

    int kbd = 0, mouse = 0;
    xemu_hud_should_capture_kbd_mouse(&kbd, &mouse);

    while (SDL_PollEvent(ev)) {
        xemu_main_loop_lock();

        // HUD must process events first so that if a controller is detached,
        // a latent rebind request can cancel before the state is freed
        xemu_hud_process_sdl_events(ev);
        xemu_input_process_sdl_events(ev);

        switch (ev->type) {
        case SDL_EVENT_KEY_DOWN:
            if (kbd) break;
            handle_keydown(ev);
            break;
        case SDL_EVENT_KEY_UP:
            if (kbd) break;
            handle_keyup(ev);
            break;
        case SDL_EVENT_QUIT:
            if (scon->opts->has_window_close && !scon->opts->window_close) {
                allow_close = false;
            }
            if (allow_close) {
                shutdown_action = SHUTDOWN_ACTION_POWEROFF;
                qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_UI);
            }
            break;
        case SDL_EVENT_MOUSE_MOTION:
            if (mouse) break;
            handle_mousemotion(ev);
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (mouse) break;
            handle_mousebutton(ev);
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            if (mouse) break;
            handle_mousewheel(ev);
            break;
        case SDL_EVENT_WINDOW_FIRST ... SDL_EVENT_WINDOW_LAST:
            handle_windowevent(ev);
            break;
        default:
            break;
        }

        xemu_main_loop_unlock();
    }

    xemu_main_loop_lock();
    xemu_input_update_controllers();
    xemu_main_loop_unlock();
}

static void display_very_early_init(DisplayOptions *o)
{
#ifdef __linux__
    /* SDL3 falls back to XWayland on compositors without fifo-v1 [1],
     * breaking HiDPI. Swap may block when occluded, but the BQL is released
     * before swap so emulation is unaffected.
     *
     * [1] https://github.com/libsdl-org/SDL/pull/9383
     */
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "wayland,x11");
#endif

#ifdef XEMU_UWP
    {
        /*
         * Give Mesa a folder for its shader cache. Without one it has no disk
         * cache, reports no program binary formats, and xemu's own shader
         * cache stays empty too: every start compiled every shader again.
         * (rodrigoandrigo/xemu's UWP port sets the same variable.)
         */
        char *base = SDL_GetPrefPath("xemu", "xemu");
        char *dir = g_strdup_printf("%smesa-shader-cache", base ? base : "");
        wchar_t *wdir = g_utf8_to_utf16(dir, -1, NULL, NULL, NULL);
        g_mkdir_with_parents(dir, 0700);
        if (wdir) {
            _wputenv_s(L"MESA_SHADER_CACHE_DIR", wdir);
        }
        g_free(wdir);
        g_free(dir);
        SDL_free(base);
    }
#endif

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "Failed to initialize SDL video subsystem: %s\n",
                SDL_GetError());
        exit(1);
    }

#ifdef SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR /* only available since SDL 2.0.8 */
    SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
#endif
    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");

    // Initialize rendering context
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(
        SDL_GL_CONTEXT_PROFILE_MASK,
        SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    char *title = g_strdup_printf("xemu | v%s"
#ifdef XEMU_DEBUG_BUILD
                                  " Debug"
#endif
                                  , xemu_version);

    // Decide window size
    int min_window_width = 640;
    int min_window_height = 480;
    int window_width = min_window_width;
    int window_height = min_window_height;

    const int res_table[][2] = {
        {640,  480},
        {720,  480},
        {1280, 720},
        {1280, 800},
        {1280, 960},
        {1920, 1080},
        {2560, 1440},
        {2560, 1600},
        {2560, 1920},
        {3840, 2160}
    };

    if (g_config.display.window.startup_size == CONFIG_DISPLAY_WINDOW_STARTUP_SIZE_LAST_USED) {
        window_width  = g_config.display.window.last_width;
        window_height = g_config.display.window.last_height;
    } else {
        window_width  = res_table[g_config.display.window.startup_size-1][0];
        window_height = res_table[g_config.display.window.startup_size-1][1];
    }

    if (window_width < min_window_width) {
        window_width = min_window_width;
    }
    if (window_height < min_window_height) {
        window_height = min_window_height;
    }

    SDL_WindowFlags window_flags = (SDL_WindowFlags)(SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);

    // Create main window
    m_window = SDL_CreateWindow(
        title, window_width, window_height,
        window_flags);
    if (m_window == NULL) {
        fprintf(stderr, "Failed to create main window: %s\n", SDL_GetError());
        SDL_Quit();
        exit(1);
    }
    g_free(title);
    SDL_SetWindowMinimumSize(m_window, min_window_width, min_window_height);

    const SDL_DisplayMode *disp_mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(m_window));
    if (disp_mode && (disp_mode->w < window_width || disp_mode->h < window_height)) {
        SDL_SetWindowSize(m_window, min_window_width, min_window_height);
        SDL_SetWindowPosition(m_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }

    m_context = SDL_GL_CreateContext(m_window);

    if (m_context != NULL && epoxy_gl_version() < 40) {
        SDL_GL_MakeCurrent(NULL, NULL);
        SDL_GL_DestroyContext(m_context);
        m_context = NULL;
    }

    if (m_context == NULL) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,
            "Unable to create OpenGL context",
            "Unable to create OpenGL context. This usually means the\r\n"
            "graphics device on this system does not support OpenGL 4.0.\r\n"
            "\r\n"
            "xemu cannot continue and will now exit.",
            m_window);
        SDL_DestroyWindow(m_window);
        SDL_Quit();
        exit(1);
    }

    int width, height, channels = 0;
    stbi_set_flip_vertically_on_load(0);
    unsigned char *icon_data = stbi_load_from_memory(xemu_64x64_data, xemu_64x64_size, &width, &height, &channels, 4);
    if (icon_data) {
        SDL_Surface *icon = SDL_CreateSurfaceFrom(width, height, SDL_PIXELFORMAT_RGBA32, icon_data, width*4);
        if (icon) {
            SDL_SetWindowIcon(m_window, icon);
        }
        // Note: Retaining the memory allocated by stbi_load. It's used in place
        // by the SDL surface.
    }

    fprintf(stderr, "CPU: %s\n", xemu_get_cpu_info());
    fprintf(stderr, "OS_Version: %s\n", xemu_get_os_info());
    fprintf(stderr, "GL_VENDOR: %s\n", glGetString(GL_VENDOR));
    fprintf(stderr, "GL_RENDERER: %s\n", glGetString(GL_RENDERER));
    fprintf(stderr, "GL_VERSION: %s\n", glGetString(GL_VERSION));
    fprintf(stderr, "GL_SHADING_LANGUAGE_VERSION: %s\n", glGetString(GL_SHADING_LANGUAGE_VERSION));
#ifdef XEMU_UWP
    {
        GLint formats = 0;
        glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &formats);
        uwp_log("GL program binary formats: %d (0 = shaders cannot be cached)", formats);
        const char *rend = (const char *)glGetString(GL_RENDERER);
        /*
         * The model from the processor's name: the D3D12 driver is called "SraKmd_arden" (the Series X chip's
         * code name) on a Series S too (seen in a Series S report, 2026-10-15).
         */
        extern const char *uwp_cpu_brand(char brand[49]);
        char brand[49];
        const char *cpu = uwp_cpu_brand(brand);
        const char *model = strstr(cpu, "Series X") ? " (Xbox Series X GPU: 52 compute units, 12 TFLOPS)"
                          : strstr(cpu, "Series S") ? " (Xbox Series S GPU: 20 compute units, 4 TFLOPS)" : "";
        uwp_log("GPU: %s | %s%s | OpenGL %s", (const char *)glGetString(GL_VENDOR), rend ? rend : "?", model,
                (const char *)glGetString(GL_VERSION));
    }
#endif

    // Initialize offscreen rendering context now
    nv2a_context_init();
    SDL_GL_MakeCurrent(NULL, NULL);
}

static void display_early_init(DisplayOptions *o)
{
    assert(o->type == DISPLAY_TYPE_XEMU);
    display_opengl = 1;

    SDL_GL_MakeCurrent(m_window, m_context);
    SDL_GL_SetSwapInterval(g_config.display.window.vsync ? 1 : 0);
#ifdef _WIN32
    win32_dxgi_present_init(m_window);
#endif
    xemu_hud_init(m_window, m_context);
}

static const DisplayChangeListenerOps dcl_gl_ops = {
    .dpy_name                = "xemu-gl",
    .dpy_gfx_switch          = gl_switch,
    .dpy_gfx_check_format    = xb_console_gl_check_format,
    .dpy_mouse_set           = mouse_warp,
    .dpy_cursor_define       = mouse_define,
};

static void display_init(DisplayState *ds, DisplayOptions *o)
{
    uint8_t data = 0;
    int i;

    assert(o->type == DISPLAY_TYPE_XEMU);
    SDL_GL_MakeCurrent(m_window, m_context);

    gui_fullscreen = o->has_full_screen && o->full_screen;
    gui_fullscreen |= g_config.display.window.fullscreen_on_startup;

    num_outputs = 1;
    scon_list = g_new0(struct xemu_console, num_outputs);
    for (i = 0; i < num_outputs; i++) {
        QemuConsole *con = qemu_console_lookup_by_index(i);
        assert(con != NULL);
        if (!qemu_console_is_graphic(con) &&
            qemu_console_get_index(con) != 0) {
            scon_list[i].hidden = true;
        }
        scon_list[i].idx = i;
        scon_list[i].opts = o;
        scon_list[i].dcl.ops = &dcl_gl_ops;
        scon_list[i].dcl.con = con;
        scon_list[i].kbd = qkbd_state_init(con);
        register_displaychangelistener(&scon_list[i].dcl);

#if defined(SDL_VIDEO_DRIVER_WINDOWS)
        HWND hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(scon_list[i].real_window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
        if (hwnd) {
            qemu_console_set_window_id(con, (uintptr_t)hwnd);
        }
#elif defined(SDL_VIDEO_DRIVER_X11)
        Window xwindow = (Window)SDL_GetNumberProperty(SDL_GetWindowProperties(scon_list[i].real_window), SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
        if (xwindow) {
            qemu_console_set_window_id(con, xwindow);
        }
#endif
    }

    scon_list[0].real_window = m_window;
    scon_list[0].winctx = m_context;

    mouse_mode_notifier.notify = mouse_mode_change;
    qemu_add_mouse_mode_change_notifier(&mouse_mode_notifier);

    sdl_cursor_hidden = SDL_CreateCursor(&data, &data, 8, 1, 0, 0);
    sdl_cursor_normal = SDL_GetCursor();

    // SDL_PollEvent may block during main window resize or drag operations.
    // Register event watch to handle rendering during these operations.
    SDL_AddEventWatch(event_watch_callback, &scon_list[0]);

    if (use_vblank_timer_thread) {
        qemu_thread_create(&vblank_thread, "vblank-timer", vblank_timer_thread,
                           &scon_list[0], QEMU_THREAD_JOINABLE);
    } else {
        vblank_timer = timer_new_ns(QEMU_CLOCK_REALTIME, vblank_timer_callback, &scon_list[0]);
        timer_mod_ns(vblank_timer, qemu_clock_get_ns(QEMU_CLOCK_REALTIME) + vblank_interval_ns);
    }

    /* Tell main thread to go ahead and create the app and enter the run loop */
    SDL_GL_MakeCurrent(NULL, NULL);
    qemu_sem_post(&display_init_sem);
}

static void display_finalize(void)
{
    if (use_vblank_timer_thread) {
        qemu_thread_join(&vblank_thread);
    }

    SDL_RemoveEventWatch(event_watch_callback, &scon_list[0]);
#ifdef _WIN32
    win32_dxgi_present_cleanup();
#endif
    SDL_GL_MakeCurrent(NULL, NULL);
    SDL_GL_DestroyContext(m_context);
    SDL_DestroyWindow(m_window);
    SDL_Quit();
}

static QemuDisplay qemu_display_xemu = {
    .type       = DISPLAY_TYPE_XEMU,
    .early_init = display_early_init,
    .init       = display_init,
};

static void register_xemu_display(void)
{
    qemu_display_register(&qemu_display_xemu);
}

type_init(register_xemu_display);

int gArgc;
char **gArgv;

#ifdef XEMU_UWP
/* Progress log for the Xbox port, in its own file (uwp_debug.log in LocalState\xemu\xemu). */
SDL_AudioStream *xemu_uwp_audio_stream; /* opened on the UI thread, used by the APU */
void uwp_log(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
/* a second copy, named after the game, in the folder Game Vault shares (see uwp_log_share) */
static FILE *uwp_log_copy;
static FILE *uwp_log_file;
void uwp_log(const char *fmt, ...)
{
    static FILE *f;
    if (!f) {
        char *base = SDL_GetPrefPath("xemu", "xemu");
        char *path = g_strdup_printf("%suwp_debug.log", base ? base : "");
        f = fopen(path, "w");
        g_free(path);
        SDL_free(base);
        if (!f) {
            return;
        }
        uwp_log_file = f;
    }
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = snprintf(line, sizeof(line), "[%8.3f] ", SDL_GetTicks() / 1000.0);
    vsnprintf(line + n, sizeof(line) - n, fmt, ap);
    va_end(ap);
    /*
     * Several threads log (the xemu.log reader among them): one whole line at a time. Never wait forever:
     * a crash report must still be written if the crashing thread died holding the lock.
     */
    static SRWLOCK lock = SRWLOCK_INIT;
    BOOLEAN locked = TryAcquireSRWLockExclusive(&lock);
    for (int i = 0; !locked && i < 200; i++) {
        Sleep(1);
        locked = TryAcquireSRWLockExclusive(&lock);
    }
    fputs(line, f);
    fputc('\n', f);
    fflush(f);
    if (uwp_log_copy) {
        fputs(line, uwp_log_copy);
        fputc('\n', uwp_log_copy);
        fflush(uwp_log_copy);
    }
    if (locked) {
        ReleaseSRWLockExclusive(&lock);
    }
}

/* The processor's name ("Microsoft Xbox Series S CPU" and so on), without leading spaces. */
const char *uwp_cpu_brand(char brand[49])
{
    unsigned int r[4];
    memset(brand, 0, 49);
    __get_cpuid(0x80000000, &r[0], &r[1], &r[2], &r[3]);
    if (r[0] >= 0x80000004) {
        for (unsigned int i = 0; i < 3; i++) {
            __get_cpuid(0x80000002 + i, &r[0], &r[1], &r[2], &r[3]);
            memcpy(brand + i * 16, r, 16);
        }
    }
    const char *b = brand;
    while (*b == ' ') {
        b++;
    }
    return b;
}

/*
 * xemu.log (stdout and stderr: failed checks, QEMU errors, the GL driver's messages) used to stay in
 * LocalState, was never part of a report, and was deleted at the next start. Both streams now go through
 * a pipe: a reader thread writes everything to xemu.log as before and also puts each line, prefixed
 * "xemu: ", into the progress log, so it lands in the game's shared log next to the fps lines and goes
 * out with the reports. Repeated lines are folded and only the first UWP_ERR_MAX lines are copied, so a
 * message in a loop cannot flood the log; xemu.log itself keeps everything.
 */
#define UWP_ERR_MAX 3000
static HANDLE uwp_err_read;
static FILE *uwp_err_file;

static void uwp_err_line(const char *line)
{
    static char last[512];
    static unsigned repeats, copied;
    if (!strcmp(line, last)) {
        repeats++;
        return;
    }
    if (repeats) {
        uwp_log("xemu: (the line above %u more times)", repeats);
        repeats = 0;
    }
    snprintf(last, sizeof(last), "%s", line);
    if (copied < UWP_ERR_MAX) {
        uwp_log("xemu: %s", line);
    } else if (copied == UWP_ERR_MAX) {
        uwp_log("xemu: (more lines only in xemu.log)");
    }
    copied++;
}

static DWORD WINAPI uwp_err_thread(LPVOID unused)
{
    char buf[4096], line[512];
    size_t len = 0;
    DWORD got;
    while (ReadFile(uwp_err_read, buf, sizeof(buf), &got, NULL) && got) {
        fwrite(buf, 1, got, uwp_err_file);
        fflush(uwp_err_file);
        for (DWORD i = 0; i < got; i++) {
            char c = buf[i];
            if (c == '\n' || len == sizeof(line) - 1) {
                while (len && line[len - 1] == '\r') {
                    len--;
                }
                line[len] = '\0';
                if (len) {
                    uwp_err_line(line);
                }
                len = 0;
                if (c == '\n') {
                    continue;
                }
            }
            line[len++] = c;
        }
    }
    return 0;
}

/* Called right after stdout and stderr were opened on xemu.log; on any failure they simply stay there. */
static void uwp_err_start(const char *log_path)
{
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, NULL, 1 << 16)) {
        return;
    }
    int fd = _open_osfhandle((intptr_t)wr, _O_BINARY | _O_WRONLY);
    FILE *file = fopen(log_path, "ab");
    if (fd < 0 || !file) {
        if (file) {
            fclose(file);
        }
        if (fd >= 0) {
            _close(fd);
        } else {
            CloseHandle(wr);
        }
        CloseHandle(rd);
        return;
    }
    uwp_err_read = rd;
    uwp_err_file = file;
    fflush(stdout);
    fflush(stderr);
    _dup2(fd, _fileno(stdout));
    _dup2(fd, _fileno(stderr));
    _close(fd);
    SetStdHandle(STD_OUTPUT_HANDLE, (HANDLE)_get_osfhandle(_fileno(stdout)));
    SetStdHandle(STD_ERROR_HANDLE, (HANDLE)_get_osfhandle(_fileno(stderr)));
    uwp_log("xemu.log lines follow as \"xemu: ...\"");   /* opens the progress log on this thread first */
    HANDLE t = CreateThread(NULL, 0, uwp_err_thread, NULL, 0, NULL);
    if (t) {
        CloseHandle(t);
    }
}

/* Before a crash report: give the reader thread a moment to pass on what was just written (an assert's text). */
static void uwp_err_drain(void)
{
    if (!uwp_err_read) {
        return;
    }
    /*
     * No PeekNamedPipe here: the reader thread sits in a blocking ReadFile on the same pipe, and Windows
     * queues the peek behind that read, so after a crash (no more output) it waited forever and the crash
     * report was never written: the picture froze with the sound going on (2026-10-16, two players).
     */
    fflush(stdout);
    fflush(stderr);
    Sleep(50);   /* the reader thread takes the last lines and splits them */
}

/*
 * Safety net for a crash: if writing the report or the summary takes longer than 8 s (something it needs is
 * stuck), xemu closes itself, so the player is back on the Home screen instead of a frozen picture.
 */
static HANDLE uwp_crash_started;

static DWORD WINAPI uwp_crash_guard(void *arg)
{
    WaitForSingleObject(uwp_crash_started, INFINITE);
    Sleep(8000);
    TerminateProcess(GetCurrentProcess(), 0xE0000001);
    return 0;
}

static void uwp_crash_guard_start(void)
{
    uwp_crash_started = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (uwp_crash_started) {
        CloseHandle(CreateThread(NULL, 0, uwp_crash_guard, NULL, 0, NULL));
    }
}

/* Profiler state, see include/qemu/xemu-uwp.h. */
int64_t uwp_prof_ticks[UWP_PROF__COUNT];
int uwp_prof_calls[UWP_PROF__COUNT];
int uwp_dl_site;
int uwp_dl_counts[UWP_DL__COUNT];
int uwp_trace_budget;
int uwp_gpu_blits;
extern int64_t uwp_prof_nv2a_totals[NV2A_PROF__COUNT];
extern int64_t uwp_prof_nv2a_frames;

#define UWP_PROF_MAX_THREADS 48
static struct {
    char name[32];
    DWORD tid;
    HANDLE handle;
    uint64_t last_cpu;
} uwp_prof_threads[UWP_PROF_MAX_THREADS];
static int uwp_prof_num_threads;

/*
 * The emulated Xbox CPU gets a processor core of its own. The console gives xemu three cores with two logical
 * processors each (SMT); left to the scheduler, the vcpu thread shares a core with the renderer, the UI or the
 * shader warm-up threads, and two busy threads on one core each run much slower. So: vcpu alone on one logical
 * processor of the first core, every other thread (the process default) on the other cores; the sibling of the
 * vcpu's processor stays free. Nothing is done with fewer than two cores. Testing: cpu-pin-off.txt next to
 * xemu.toml turns it off.
 */
static void uwp_pin_vcpu(void)
{
    typedef BOOL (WINAPI *Info_fn)(PSYSTEM_CPU_SET_INFORMATION, ULONG, PULONG, HANDLE, ULONG);
    typedef BOOL (WINAPI *Thread_fn)(HANDLE, const ULONG *, ULONG);
    typedef BOOL (WINAPI *Process_fn)(HANDLE, const ULONG *, ULONG);
    Info_fn info = (Info_fn)xemu_uwp_kernelbase_proc("GetSystemCpuSetInformation");
    Thread_fn thread = (Thread_fn)xemu_uwp_kernelbase_proc("SetThreadSelectedCpuSets");
    Process_fn process = (Process_fn)xemu_uwp_kernelbase_proc("SetProcessDefaultCpuSets");
    {
        char *own = SDL_GetPrefPath("xemu", "xemu");
        char *off = g_strdup_printf("%scpu-pin-off.txt", own ? own : "");
        bool skip = g_file_test(off, G_FILE_TEST_EXISTS);
        g_free(off);
        SDL_free(own);
        if (skip) {
            uwp_log("cpu: vcpu not given a core of its own (cpu-pin-off.txt)");
            return;
        }
    }
    static char buf[4096];
    ULONG len = 0;
    if (!info || !thread || !process ||
        !info((PSYSTEM_CPU_SET_INFORMATION)buf, sizeof(buf), &len, GetCurrentProcess(), 0)) {
        uwp_log("cpu: core sets not available, vcpu left to the scheduler");
        return;
    }
    ULONG ids[64], cores[64], n = 0;
    for (ULONG off = 0; off < len && n < 64;) {
        PSYSTEM_CPU_SET_INFORMATION s = (PSYSTEM_CPU_SET_INFORMATION)(buf + off);
        if (s->Size == 0) {
            break;
        }
        if (s->Type == CpuSetInformation && s->CpuSet.AllocatedToTargetProcess) {
            ids[n] = s->CpuSet.Id;
            cores[n++] = s->CpuSet.CoreIndex;
        }
        off += s->Size;
    }
    ULONG others[64], m = 0;
    for (ULONG i = 1; i < n; i++) {
        if (cores[i] != cores[0]) {
            others[m++] = ids[i];
        }
    }
    if (n < 2 || m == 0) {
        uwp_log("cpu: %lu processors for xemu on one core, vcpu left to the scheduler", (unsigned long)n);
        return;
    }
    bool ok_p = process(GetCurrentProcess(), others, m);
    bool ok_t = thread(GetCurrentThread(), ids, 1);
    uwp_log("cpu: vcpu alone on processor %lu (core %lu), %lu processors on other cores for the rest%s%s",
            (unsigned long)ids[0], (unsigned long)cores[0], (unsigned long)m, ok_t ? "" : " (vcpu: failed)",
            ok_p ? "" : " (rest: failed)");
}

void uwp_prof_register_thread(const char *name)
{
    if (!strcmp(name, "vcpu")) {
        static bool pinned;
        if (!pinned) {
            pinned = true;
            uwp_pin_vcpu();
        }
    }
    static SRWLOCK lock = SRWLOCK_INIT;
    HANDLE h = NULL;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                         GetCurrentProcess(), &h, 0, FALSE,
                         DUPLICATE_SAME_ACCESS)) {
        return;
    }
    AcquireSRWLockExclusive(&lock);
    /* a thread that registers twice (QEMU's name, then ours) is renamed */
    int slot = 0;
    while (slot < uwp_prof_num_threads &&
           uwp_prof_threads[slot].tid != GetCurrentThreadId()) {
        slot++;
    }
    if (slot < UWP_PROF_MAX_THREADS) {
        char *out = uwp_prof_threads[slot].name;
        for (int i = 0; name[i] && i < 31; i++) {
            *out++ = g_ascii_isalnum(name[i]) ? name[i] : '_';
        }
        *out = '\0';
    }
    if (slot == uwp_prof_num_threads && slot < UWP_PROF_MAX_THREADS) {
        uwp_prof_threads[slot].tid = GetCurrentThreadId();
        uwp_prof_threads[slot].handle = h;
        uwp_prof_num_threads++;
    } else {
        CloseHandle(h);
    }
    ReleaseSRWLockExclusive(&lock);
}

/*
 * How busy a registered thread (by name, e.g. "vcpu", "pfifo") was since the previous call
 * for that slot, in percent of one core; -1 when there is no such thread. `slot` keeps
 * separate callers apart (0..7).
 */
int xemu_uwp_thread_busy(const char *name, int slot)
{
    static uint64_t last_cpu[8][UWP_PROF_MAX_THREADS], last_wall[8][UWP_PROF_MAX_THREADS];
    if (slot < 0 || slot >= 8) {
        return -1;
    }
    for (int i = 0; i < uwp_prof_num_threads; i++) {
        if (strncmp(uwp_prof_threads[i].name, name, strlen(name))) {
            continue;
        }
        FILETIME c, e, k, u, now;
        if (!GetThreadTimes(uwp_prof_threads[i].handle, &c, &e, &k, &u)) {
            return -1;
        }
        GetSystemTimeAsFileTime(&now);
        uint64_t cpu = (((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime) +
                       (((uint64_t)u.dwHighDateTime << 32) | u.dwLowDateTime);
        uint64_t wall = ((uint64_t)now.dwHighDateTime << 32) | now.dwLowDateTime;
        int pct = -1;
        if (last_wall[slot][i] && wall > last_wall[slot][i]) {
            pct = (int)((cpu - last_cpu[slot][i]) * 100 / (wall - last_wall[slot][i]));
        }
        last_cpu[slot][i] = cpu;
        last_wall[slot][i] = wall;
        return pct;
    }
    return -1;
}

/* One log line: CPU use per thread, time per probe, nv2a counters per frame. */
static void uwp_prof_report(double seconds)
{
    static int64_t last_ticks[UWP_PROF__COUNT];
    static int last_calls[UWP_PROF__COUNT];
    static int64_t last_nv2a[NV2A_PROF__COUNT], last_frames;
    static const char *const names[UWP_PROF__COUNT] = {
#define _X(x) #x,
        UWP_PROF_TIMERS_XMAC
#undef _X
    };
    char line[2048];
    int n = 0;
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);

    n += snprintf(line + n, sizeof(line) - n, "prof cpu%%:");
    for (int i = 0; i < uwp_prof_num_threads; i++) {
        FILETIME c, e, k, u;
        if (!GetThreadTimes(uwp_prof_threads[i].handle, &c, &e, &k, &u)) {
            continue;
        }
        uint64_t cpu = (((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime) +
                       (((uint64_t)u.dwHighDateTime << 32) | u.dwLowDateTime);
        double pct = (cpu - uwp_prof_threads[i].last_cpu) / 1e5 / seconds;
        if (pct >= 1.0 || i < 3) {
            n += snprintf(line + n, sizeof(line) - n, " %s %.0f",
                          uwp_prof_threads[i].name, pct);
        }
        uwp_prof_threads[i].last_cpu = cpu;
    }
    {
        /* the whole process, which also covers threads that are not ours
         * (graphics driver, audio) */
        static uint64_t last_total;
        FILETIME c, e, k, u;
        if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) {
            uint64_t total =
                (((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime) +
                (((uint64_t)u.dwHighDateTime << 32) | u.dwLowDateTime);
            n += snprintf(line + n, sizeof(line) - n, " ALL %.0f",
                          (total - last_total) / 1e5 / seconds);
            last_total = total;
        }
    }
    {
        /* main loop sleeps before timer deadlines (util/qemu-timer.c) */
        extern uint64_t xemu_uwp_ml_sleeps, xemu_uwp_ml_late_ns, xemu_uwp_ml_woken;
        uint64_t s = qatomic_xchg(&xemu_uwp_ml_sleeps, 0), l = qatomic_xchg(&xemu_uwp_ml_late_ns, 0),
                 w = qatomic_xchg(&xemu_uwp_ml_woken, 0);
        if (s) {
            n += snprintf(line + n, sizeof(line) - n, " | ml sleeps %llu (woken %llu), late avg %.0f us",
                          (unsigned long long)s, (unsigned long long)w, s > w ? l / 1e3 / (s - w) : 0.0);
        }
    }
    {
        /* GPU thread wake-ups after it ran out of work (hw/xbox/nv2a/pfifo.c) */
        extern uint64_t xemu_uwp_gpu_wakes, xemu_uwp_gpu_wake_ns, xemu_uwp_gpu_spin_wakes;
        uint64_t k = qatomic_xchg(&xemu_uwp_gpu_wakes, 0), ns = qatomic_xchg(&xemu_uwp_gpu_wake_ns, 0),
                 sp = qatomic_xchg(&xemu_uwp_gpu_spin_wakes, 0);
        if (k) {
            n += snprintf(line + n, sizeof(line) - n, " | gpu wakes %llu (spun %llu), avg %.0f us",
                          (unsigned long long)k, (unsigned long long)sp, ns / 1e3 / k);
        }
    }
    n += snprintf(line + n, sizeof(line) - n, " | wall%% (calls):");
    for (int i = 0; i < UWP_PROF__COUNT; i++) {
        int64_t t = uwp_prof_ticks[i];
        int c = uwp_prof_calls[i];
        if (c != last_calls[i]) {
            n += snprintf(line + n, sizeof(line) - n, " %s %.1f (%d)", names[i],
                          (t - last_ticks[i]) * 100.0 / freq.QuadPart / seconds,
                          c - last_calls[i]);
        }
        last_ticks[i] = t;
        last_calls[i] = c;
    }
    int64_t frames = uwp_prof_nv2a_frames - last_frames;
    n += snprintf(line + n, sizeof(line) - n, " | per frame (%d frames):",
                  (int)frames);
    for (int i = 0; i < NV2A_PROF__COUNT; i++) {
        int64_t v = uwp_prof_nv2a_totals[i];
        if (v != last_nv2a[i] && frames > 0 && n < (int)sizeof(line) - 64) {
            n += snprintf(line + n, sizeof(line) - n, " %s %.1f",
                          nv2a_profile_get_counter_name(i),
                          (double)(v - last_nv2a[i]) / frames);
        }
        last_nv2a[i] = v;
    }
    last_frames = uwp_prof_nv2a_frames;
    static int last_dl[UWP_DL__COUNT];
    n += snprintf(line + n, sizeof(line) - n, " | readbacks by site:");
    for (int i = 0; i < UWP_DL__COUNT; i++) {
        n += snprintf(line + n, sizeof(line) - n, " %d",
                      uwp_dl_counts[i] - last_dl[i]);
        last_dl[i] = uwp_dl_counts[i];
    }
    static int last_gpu_blits;
    n += snprintf(line + n, sizeof(line) - n, " | gpu blits %d",
                  uwp_gpu_blits - last_gpu_blits);
    last_gpu_blits = uwp_gpu_blits;
    uwp_log("%s", line);

    static int reports;
    if (++reports % 7 == 0) {
        uwp_trace_budget = 70; /* a frame or two of events, every ~20 s */
    }
}

/*
 * Sampling profiler for one thread (the emulated CPU, the renderer): about
 * 500 times a second it is stopped for a moment and the address it was
 * executing is noted. Every
 * 30 s the busiest places go to the log, as offsets into xemu.exe (to be
 * looked up in the build's symbols on the PC), as other modules by name, or
 * as "generated code" (the guest's own code, translated).
 */
#define UWP_SAMPLES 16384
#define UWP_STACK_FRAMES 10

/*
 * The name of the export a system DLL address belongs to (the nearest export
 * at or below it). win32u.dll and ntdll.dll export every system call stub by
 * name, so this says which kernel service a thread was in.
 */
static const char *uwp_export_name(HMODULE mod, uint64_t addr, uint32_t *off)
{
    const uint8_t *base = (const uint8_t *)mod;
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)
        (base + ((const IMAGE_DOS_HEADER *)base)->e_lfanew);
    const IMAGE_DATA_DIRECTORY *dd =
        &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dd->VirtualAddress) {
        return NULL;
    }
    const IMAGE_EXPORT_DIRECTORY *ed =
        (const IMAGE_EXPORT_DIRECTORY *)(base + dd->VirtualAddress);
    const uint32_t *names = (const uint32_t *)(base + ed->AddressOfNames);
    const uint16_t *ords = (const uint16_t *)(base + ed->AddressOfNameOrdinals);
    const uint32_t *funcs = (const uint32_t *)(base + ed->AddressOfFunctions);
    uint32_t rva = (uint32_t)(addr - (uint64_t)(uintptr_t)base), best = 0;
    const char *name = NULL;
    for (uint32_t i = 0; i < ed->NumberOfNames; i++) {
        uint32_t f = funcs[ords[i]];
        if (f <= rva && f >= best) {
            best = f;
            name = (const char *)(base + names[i]);
        }
    }
    if (off) {
        *off = rva - best;
    }
    return name;
}

/* Return addresses of a suspended thread, innermost first. Allocates nothing. */
static int uwp_unwind(CONTEXT *ctx, uint64_t *out, int max)
{
    int n = 0;
    while (n < max && ctx->Rip) {
        DWORD64 image = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx->Rip, &image, NULL);
        if (fn) {
            void *handler_data = NULL;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, ctx->Rip, fn, ctx,
                             &handler_data, &establisher, NULL);
        } else {
            /* a leaf function: the return address is on top of the stack */
            if (IsBadReadPtr((void *)ctx->Rsp, 8)) {
                break;
            }
            ctx->Rip = *(DWORD64 *)ctx->Rsp;
            ctx->Rsp += 8;
        }
        if (!ctx->Rip) {
            break;
        }
        out[n++] = ctx->Rip;
    }
    return n;
}

struct uwp_count {
    char name[72];
    int count;
};

static void uwp_count_add(struct uwp_count *tab, int max, const char *name)
{
    for (int i = 0; i < max; i++) {
        if (!tab[i].name[0]) {
            g_strlcpy(tab[i].name, name, sizeof(tab[i].name));
        }
        if (!strcmp(tab[i].name, name)) {
            tab[i].count++;
            return;
        }
    }
}

static void uwp_count_log(const char *title, struct uwp_count *tab, int max, int rows)
{
    char line[1500];
    int n = snprintf(line, sizeof(line), "%s:", title);
    for (int rank = 0; rank < rows && n < (int)sizeof(line) - 100; rank++) {
        int best = -1;
        for (int i = 0; i < max && tab[i].name[0]; i++) {
            if (tab[i].count > 0 && (best < 0 || tab[i].count > tab[best].count)) {
                best = i;
            }
        }
        if (best < 0) {
            break;
        }
        n += snprintf(line + n, sizeof(line) - n, " %s %d,", tab[best].name, tab[best].count);
        tab[best].count = 0;
    }
    uwp_log("%s", line);
}

static void uwp_module_name(HMODULE mod, char *out, int size)
{
    char path[MAX_PATH] = "?";
    GetModuleFileNameA(mod, path, sizeof(path));
    const char *base = strrchr(path, '\\');
    g_strlcpy(out, base ? base + 1 : path, size);
}

static DWORD WINAPI uwp_sampler_thread(void *arg)
{
    const char *target = arg; /* which thread to watch: "vcpu" or "pfifo" */
    uint64_t *samples = g_malloc(UWP_SAMPLES * sizeof(uint64_t));
    struct {
        uint64_t key;
        int count;
    } *top = g_malloc(4096 * sizeof(*top));
    HMODULE exe = GetModuleHandleW(NULL);
    uint64_t exe_base = (uint64_t)(uintptr_t)exe;
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)
        ((const uint8_t *)exe + ((const IMAGE_DOS_HEADER *)exe)->e_lfanew);
    uint64_t exe_size = nt->OptionalHeader.SizeOfImage;
    int num = 0;
    HANDLE vcpu = NULL;
    /* report interval: 30 s, or the seconds in sampler_seconds.txt next to the log (testing) */
    DWORD every = 30000;
    {
        char *own = SDL_GetPrefPath("xemu", "xemu");
        char *file = g_strdup_printf("%ssampler_seconds.txt", own ? own : "");
        gchar *text = NULL;
        if (g_file_get_contents(file, &text, NULL, NULL) && atoi(text) > 0) {
            every = atoi(text) * 1000;
        }
        g_free(text);
        g_free(file);
        SDL_free(own);
    }
    DWORD next_dump = GetTickCount() + every;
    /* for the renderer thread: who called, to tell what the graphics library asks the kernel for */
    bool want_stacks = true; /* both threads: which system calls and library functions the time goes to */
    uint64_t (*stacks)[UWP_STACK_FRAMES] =
        want_stacks ? g_malloc0(UWP_SAMPLES * sizeof(*stacks)) : NULL;

    rcu_register_thread(); /* for uwp_read_guest */

    for (;;) {
        Sleep(10); /* 500 a second cost the watched thread about a fifth */
        if (!vcpu) {
            for (int i = 0; i < uwp_prof_num_threads; i++) {
                if (!strcmp(uwp_prof_threads[i].name, target)) {
                    vcpu = uwp_prof_threads[i].handle;
                }
            }
            continue;
        }
        if (num < UWP_SAMPLES && SuspendThread(vcpu) != (DWORD)-1) {
            CONTEXT ctx;
            ctx.ContextFlags = want_stacks ? CONTEXT_FULL : CONTEXT_CONTROL;
            if (GetThreadContext(vcpu, &ctx)) {
                samples[num] = ctx.Rip;
                if (want_stacks) {
                    memset(stacks[num], 0, sizeof(stacks[num]));
                    if (ctx.Rip - exe_base >= exe_size) {
                        uwp_unwind(&ctx, stacks[num], UWP_STACK_FRAMES);
                    }
                }
                num++;
            }
            ResumeThread(vcpu);
        }
        if ((int)(GetTickCount() - next_dump) < 0) {
            continue;
        }
        next_dump = GetTickCount() + every;

        /* group: xemu.exe in 64-byte pieces, anything else per module */
        int groups = 0, jit = 0;
        for (int i = 0; i < num; i++) {
            uint64_t key;
            if (samples[i] - exe_base < exe_size) {
                key = (samples[i] - exe_base) >> 6;
            } else {
                HMODULE mod = NULL;
                if (!GetModuleHandleExW(
                        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                        (LPCWSTR)(uintptr_t)samples[i], &mod) || !mod) {
                    jit++;
                    continue;
                }
                key = (uint64_t)(uintptr_t)mod | (1ull << 63);
            }
            int g = 0;
            while (g < groups && top[g].key != key) {
                g++;
            }
            if (g == groups) {
                if (groups == 4096) {
                    continue;
                }
                top[groups].key = key;
                top[groups++].count = 0;
            }
            top[g].count++;
        }
        char line[1800];
        int n = snprintf(line, sizeof(line),
                         "%s samples %d: generated code %d", target, num, jit);
        for (int rank = 0; rank < 40 && n < (int)sizeof(line) - 80; rank++) {
            int best = -1;
            for (int g = 0; g < groups; g++) {
                if (top[g].count > 0 &&
                    (best < 0 || top[g].count > top[best].count)) {
                    best = g;
                }
            }
            if (best < 0) {
                break;
            }
            if (top[best].key >> 63) {
                wchar_t path[260];
                DWORD len = GetModuleFileNameW(
                    (HMODULE)(uintptr_t)(top[best].key & ~(1ull << 63)), path,
                    ARRAY_SIZE(path));
                const wchar_t *base = path + len;
                while (base > path && base[-1] != L'\\') {
                    base--;
                }
                n += snprintf(line + n, sizeof(line) - n, ", %ls %d", base,
                              top[best].count);
            } else {
                n += snprintf(line + n, sizeof(line) - n, ", +%llx %d",
                              (unsigned long long)(top[best].key << 6),
                              top[best].count);
            }
            top[best].count = 0;
        }
        uwp_log("%s", line);

        if (want_stacks) {
            /*
             * Samples outside xemu.exe: the export they are in (a system call
             * stub in win32u / ntdll says which kernel service), the first
             * caller inside the GL driver, and the first caller in xemu.exe.
             */
            static __thread struct uwp_count where[160], gallium[160], ours[160], chains[160]; /* one sampler thread each */
            memset(chains, 0, sizeof(chains));
            memset(where, 0, sizeof(where));
            memset(gallium, 0, sizeof(gallium));
            memset(ours, 0, sizeof(ours));
            for (int i = 0; i < num; i++) {
                HMODULE mod = NULL;
                if (samples[i] - exe_base < exe_size ||
                    !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                        (LPCWSTR)(uintptr_t)samples[i], &mod) || !mod) {
                    continue;
                }
                char name[72], modname[40];
                uwp_module_name(mod, modname, sizeof(modname));
                uint32_t off = 0;
                const char *exp = uwp_export_name(mod, samples[i], &off);
                if (exp && off < 0x400) {
                    snprintf(name, sizeof(name), "%.12s!%.50s", modname, exp);
                } else {
                    snprintf(name, sizeof(name), "%.20s+%llx", modname,
                             (unsigned long long)((samples[i] - (uint64_t)(uintptr_t)mod) & ~0xffull));
                }
                uwp_count_add(where, 160, name);
                bool got_gallium = false, got_ours = false;
                /* the return addresses inside the GL driver, innermost first: named on the PC with its .pdb */
                char chain[72];
                int clen = 0;
                chain[0] = '\0';
                for (int f = 0; f < UWP_STACK_FRAMES && stacks[i][f]; f++) {
                    uint64_t a = stacks[i][f];
                    if (a - exe_base < exe_size) {
                        if (!got_ours) {
                            snprintf(name, sizeof(name), "+%llx", (unsigned long long)((a - exe_base) & ~0x3full));
                            uwp_count_add(ours, 160, name);
                            got_ours = true;
                        }
                        break;
                    }
                    HMODULE m2 = NULL;
                    if (!got_gallium &&
                        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                           (LPCWSTR)(uintptr_t)a, &m2) && m2) {
                        uwp_module_name(m2, modname, sizeof(modname));
                        if (!g_ascii_strcasecmp(modname, "libgallium_wgl.dll")) {
                            snprintf(name, sizeof(name), "+%llx",
                                     (unsigned long long)((a - (uint64_t)(uintptr_t)m2) & ~0x7full));
                            uwp_count_add(gallium, 160, name);
                            got_gallium = true;
                        }
                    }
                    HMODULE m3 = NULL;
                    if (clen < (int)sizeof(chain) - 10 &&
                        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                           (LPCWSTR)(uintptr_t)a, &m3) && m3) {
                        uwp_module_name(m3, modname, sizeof(modname));
                        if (!g_ascii_strcasecmp(modname, "libgallium_wgl.dll")) {
                            clen += snprintf(chain + clen, sizeof(chain) - clen, "%s%llx", clen ? "<" : "",
                                             (unsigned long long)(a - (uint64_t)(uintptr_t)m3));
                        }
                    }
                }
                if (clen) {
                    uwp_count_add(chains, 160, chain);
                }
            }
            char title[96];
            snprintf(title, sizeof(title), "%s outside xemu, by export", target);
            uwp_count_log(title, where, 160, 24);
            snprintf(title, sizeof(title), "%s first caller in libgallium_wgl", target);
            uwp_count_log(title, gallium, 160, 24);
            snprintf(title, sizeof(title), "%s first caller in xemu.exe", target);
            uwp_count_log(title, ours, 160, 16);
            snprintf(title, sizeof(title), "%s call chains in libgallium_wgl (innermost first)", target);
            uwp_count_log(title, chains, 160, 12);
        }

        /* the same samples by guest address (start of the translated block) */
        if (jit) {
            extern uint32_t uwp_guest_pc_of(uintptr_t host_pc);
            extern int uwp_read_guest(uint32_t addr, uint8_t *buf, int len);
            int found = 0;
            groups = 0;
            for (int i = 0; i < num; i++) {
                uint32_t pc;
                if (samples[i] - exe_base < exe_size ||
                    !(pc = uwp_guest_pc_of((uintptr_t)samples[i]))) {
                    continue;
                }
                found++;
                int g = 0;
                while (g < groups && top[g].key != pc) {
                    g++;
                }
                if (g == groups) {
                    if (groups == 4096) {
                        continue;
                    }
                    top[groups].key = pc;
                    top[groups++].count = 0;
                }
                top[g].count++;
            }
            n = snprintf(line, sizeof(line), "%s guest blocks %d of %d (%d distinct)",
                         target, found, jit, groups);
            for (int rank = 0; rank < 48 && n < (int)sizeof(line) - 40; rank++) {
                int best = -1;
                for (int g = 0; g < groups; g++) {
                    if (top[g].count > 0 &&
                        (best < 0 || top[g].count > top[best].count)) {
                        best = g;
                    }
                }
                if (best < 0) {
                    break;
                }
                n += snprintf(line + n, sizeof(line) - n, ", %08x %d",
                              (unsigned)top[best].key, top[best].count);
                if (rank < 12) {
                    uint8_t code[64];
                    char hex[2 * sizeof(code) + 1];
                    if (uwp_read_guest((uint32_t)top[best].key, code,
                                       sizeof(code)) == 0) {
                        for (int k = 0; k < (int)sizeof(code); k++) {
                            snprintf(hex + 2 * k, 3, "%02x", code[k]);
                        }
                        uwp_log("%s guest code %08x x%d: %s", target,
                                (unsigned)top[best].key, top[best].count, hex);
                    }
                }
                top[best].count = 0;
            }
            uwp_log("%s", line);
        }
        num = 0;
    }
    return 0;
}

/*
 * Starting a game from a launcher (Game Vault): the app is opened through the
 * link xemu:?disc=<path of the disc image>. SDL (local patch) publishes each
 * activation as the hint SDL_WINRT_ACTIVATION_URI; this takes the disc path
 * out of it, or returns NULL when there is none. Free with g_free().
 */
#define UWP_ACTIVATION_HINT "SDL_WINRT_ACTIVATION_URI"

/*
 * Leave the emulator the same way the Exit menu item does. When xemu's main
 * returns, SDL opens the launcher's "launchOnExit" link, so this is also the
 * way back to Game Vault.
 */
static void uwp_session_summary(const char *how);
static void uwp_event(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
static const char *uwp_version_text(void);

static void uwp_quit(const char *why)
{
    uwp_log("quitting: %s", why);
    uwp_event("quit: %s", why);
    char *how = g_strdup_printf("quit normally (%s)", why);
    uwp_session_summary(how);
    g_free(how);
    shutdown_action = SHUTDOWN_ACTION_POWEROFF;
    qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_UI);
}

/*
 * Quick save states. One per disc, kept as a snapshot inside the hard disk
 * image like xemu's own snapshots (they show up in its Snapshots window too).
 *   View + RB: save      View + LB: load
 * and the launch link may say "state=1" to load the disc's state as soon as
 * the machine runs, which skips logos and menus.
 */
static bool uwp_load_state_wanted, uwp_save_state_wanted;

static char *uwp_state_name(void)
{
    const char *disc = g_config.sys.files.dvd_path;
    const char *base = disc ? disc : "";
    for (const char *c = base; *c; c++) {
        if (*c == '/' || *c == '\\') {
            base = c + 1;
        }
    }
    char *name = g_strdup_printf("quick %s", base[0] ? base : "no disc");
    if (strlen(name) > 120) {
        name[120] = '\0';
    }
    return name;
}

static void uwp_state(bool save)
{
    uwp_event(save ? "quick save (View + RB)" : "quick load (View + LB)");
    char *name = uwp_state_name();
    Error *err = NULL;
    xemu_main_loop_lock();
    bool was_running = runstate_is_running();
    if (save) {
        xemu_snapshots_save(name, &err);
    } else {
        xemu_snapshots_load(name, &err);
    }
    /*
     * xemu_snapshots_load stops the machine first and starts it again only
     * when the load worked: asking for a state that does not exist left the
     * game frozen. Nothing has been changed in that case, so carry on.
     */
    if (err && was_running && !runstate_is_running()) {
        vm_start();
    }
    xemu_main_loop_unlock();
    if (err) {
        uwp_log("state %s failed (%s): %s", save ? "save" : "load", name,
                error_get_pretty(err));
        xemu_queue_error_message(save ? "The state could not be saved"
                                      : "There is no saved state for this game yet (View + RB saves one)");
        error_free(err);
    } else {
        uwp_log("state %s: %s", save ? "saved" : "loaded", name);
        xemu_queue_notification(save ? "State saved" : "State loaded");
    }
    g_free(name);
}

/*
 * Pressing the emulated controller's buttons from outside, for testing and
 * for setup screens that wait for a button when nobody is at the console:
 * the launch link takes press=down,down,a (names below; "wait" is a pause of
 * half a second, a number is a pause of that many milliseconds). Each press
 * is held for 150 ms and followed by 350 ms of nothing.
 */
extern uint16_t xemu_uwp_injected_buttons;
static uint16_t uwp_press_queue[64];
static uint16_t uwp_press_pause[64];
static int16_t uwp_press_trigger[64];
static uint16_t uwp_press_hold_mask[64]; /* "<button>=<s>": button held, seconds in uwp_press_trigger */ /* > 0: right trigger seconds, < 0: left */
static int uwp_press_count, uwp_press_next;
static uint64_t uwp_press_until, uwp_press_release_at;

static void uwp_press(const char *list)
{
    static const struct {
        const char *name;
        uint16_t mask;
    } names[] = {
        { "a", CONTROLLER_BUTTON_A }, { "b", CONTROLLER_BUTTON_B },
        { "x", CONTROLLER_BUTTON_X }, { "y", CONTROLLER_BUTTON_Y },
        { "left", CONTROLLER_BUTTON_DPAD_LEFT }, { "up", CONTROLLER_BUTTON_DPAD_UP },
        { "right", CONTROLLER_BUTTON_DPAD_RIGHT }, { "down", CONTROLLER_BUTTON_DPAD_DOWN },
        { "back", CONTROLLER_BUTTON_BACK }, { "start", CONTROLLER_BUTTON_START },
        { "white", CONTROLLER_BUTTON_WHITE }, { "black", CONTROLLER_BUTTON_BLACK },
        { "ls", CONTROLLER_BUTTON_LSTICK }, { "rs", CONTROLLER_BUTTON_RSTICK },
    };
    /* "," or "-" between entries: Game Vault's xemu-extra.txt test file passes no commas */
    char **parts = g_strsplit_set(list, ",-", -1);
    if (uwp_press_next >= uwp_press_count) {
        uwp_press_count = uwp_press_next = 0;
    }
    for (int i = 0; parts[i] && uwp_press_count < ARRAY_SIZE(uwp_press_queue); i++) {
        uint16_t mask = 0, pause = 0;
        uwp_press_hold_mask[uwp_press_count] = 0;
        for (int n = 0; n < ARRAY_SIZE(names); n++) {
            if (!g_ascii_strcasecmp(parts[i], names[n].name)) {
                mask = names[n].mask;
            }
        }
        /* "<button>=<seconds>": hold a button down (a racing game's accelerator) */
        {
            const char *eq = strchr(parts[i], '=');
            bool held = false;
            for (int n = 0; eq && n < ARRAY_SIZE(names); n++) {
                if ((int)strlen(names[n].name) == eq - parts[i] &&
                    !g_ascii_strncasecmp(parts[i], names[n].name, eq - parts[i])) {
                    uwp_press_hold_mask[uwp_press_count] = names[n].mask;
                    uwp_press_queue[uwp_press_count] = 0;
                    uwp_press_pause[uwp_press_count] = 0;
                    uwp_press_trigger[uwp_press_count++] = atoi(eq + 1);
                    held = true;
                }
            }
            if (held) {
                continue;
            }
        }
        /* "rt=<seconds>" / "lt=<seconds>": hold a trigger fully down (racing games) */
        if (!g_ascii_strncasecmp(parts[i], "rt=", 3) || !g_ascii_strncasecmp(parts[i], "lt=", 3)) {
            int secs = atoi(parts[i] + 3);
            uwp_press_queue[uwp_press_count] = 0;
            uwp_press_pause[uwp_press_count] = 0;
            uwp_press_trigger[uwp_press_count++] =
                g_ascii_tolower(parts[i][0]) == 'r' ? secs : -secs;
            continue;
        }
        if (!mask) {
            pause = !g_ascii_strcasecmp(parts[i], "wait") ? 500 : (uint16_t)atoi(parts[i]);
            if (!pause) {
                continue;
            }
        }
        uwp_press_queue[uwp_press_count] = mask;
        uwp_press_trigger[uwp_press_count] = 0;
        uwp_press_pause[uwp_press_count++] = pause;
    }
    g_strfreev(parts);
    uwp_log("remote buttons: %s", list);
}

static void uwp_press_poll(void)
{
    uint64_t now = SDL_GetTicks();
    if (xemu_uwp_injected_buttons && now >= uwp_press_release_at) {
        xemu_uwp_injected_buttons = 0;
    }
    if (uwp_press_next >= uwp_press_count || now < uwp_press_until) {
        return;
    }
    if (uwp_load_state_wanted) {
        return;     /* a link with "state=1": buttons go to the loaded game */
    }
    uint16_t mask = uwp_press_queue[uwp_press_next];
    int16_t trigger = uwp_press_trigger[uwp_press_next];
    uint16_t pause = uwp_press_pause[uwp_press_next++];
    uint16_t hold = uwp_press_hold_mask[uwp_press_next - 1];
    if (hold) {
        extern uint16_t xemu_uwp_held_buttons;
        extern uint64_t xemu_uwp_held_until;
        xemu_uwp_held_buttons = hold;
        xemu_uwp_held_until = now + (uint64_t)trigger * 1000;
        uwp_log("remote buttons: button 0x%x held for %d s", hold, trigger);
    } else if (trigger) {
        extern uint64_t xemu_uwp_injected_rt_until, xemu_uwp_injected_lt_until;
        uint64_t until = now + (uint64_t)ABS(trigger) * 1000;
        if (trigger > 0) {
            xemu_uwp_injected_rt_until = until;
        } else {
            xemu_uwp_injected_lt_until = until;
        }
        uwp_log("remote buttons: %s trigger held for %d s", trigger > 0 ? "right" : "left",
                ABS(trigger));
    } else if (mask) {
        xemu_uwp_injected_buttons = mask;
        uwp_press_release_at = now + 150;
        uwp_press_until = now + 500;
    } else {
        uwp_press_until = now + pause;
    }
}

/* View + Menu (Back + Start) held together for two seconds quits. */
static void uwp_poll_quit_combo(void)
{
    static uint64_t held_since;
    static bool fired;
    static bool state_combo_was_down;
    bool held = false, want_save = false, want_load = false;
    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    for (int i = 0; ids && i < count; i++) {
        SDL_Gamepad *pad = SDL_GetGamepadFromID(ids[i]);
        if (pad && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_BACK) &&
            SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_START)) {
            held = true;
        }
        if (pad && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_BACK) &&
            !SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_START)) {
            want_save |= SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
            want_load |= SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
        }
    }
    SDL_free(ids);
    if ((want_save || want_load) && !state_combo_was_down) {
        uwp_state(want_save);
    }
    state_combo_was_down = want_save || want_load;
    if (!held) {
        held_since = 0;
        fired = false;
    } else if (!held_since) {
        held_since = SDL_GetTicks();
    } else if (!fired && SDL_GetTicks() - held_since >= 2000) {
        fired = true;
        uwp_quit("View + Menu held");
    }
}

/*
 * The console suspends an app that leaves the screen and may end it without
 * another word. Write out what would be lost: the virtual hard disk's cached
 * blocks (save games) and the settings.
 */
static bool uwp_machine_running;

/*
 * Why the process ended, when it ends by itself: an abort() (failed assert)
 * or a plain exit() leaves no crash dump on the console, and the last lines
 * of a buffered stderr are lost with it.
 */
/*
 * A plain summary at the end of every log, for whoever reads it (Game Vault puts it at the top of
 * the copy on the USB drive): how long the game ran, how fast, where it was slow, and how it
 * ended. Speed is sampled every 3 seconds from the game's own frame rate once frames are drawn.
 */
static struct {
    unsigned samples, slow, very_slow;   /* 3-second samples; below 25 fps; below 15 fps */
    double sum;
    int lowest, lowest_at;               /* fps and seconds into the session */
    int slow_from, worst_stretch_from, worst_stretch_len, stretch_len;
    bool written;
} uwp_session = { .lowest = 1000, .slow_from = -1 };
static uint64_t uwp_session_start_ms;

/*
 * Crash reports anyone can read. When xemu crashes or stops on an internal error, the log gets:
 *  - what went wrong, in words, and which part of the emulator it happened in;
 *  - where: the chain of functions that led there, with their names (xemu.sym in the package,
 *    made from the unstripped exe when the package is built; system DLLs by their exports);
 *  - the "black box": one line per second for the last minute (speed, memory, how busy the
 *    emulated CPU and the graphics thread were), kept in memory and written only then.
 * Everything here allocates nothing from the heap, which may be what broke.
 */
#define UWP_BOX_SECONDS 60
static struct uwp_box_sample {
    uint32_t sec;
    int16_t fps, vcpu, gpu;
    uint32_t mem_used, mem_left;   /* MB */
    char state[12];
} uwp_box[UWP_BOX_SECONDS];
static unsigned uwp_box_count;
uint64_t uwp_crash_test_at;   /* "crashtest=1" in the link (see uwp_take_activation_disc) */

static bool uwp_memory(uint64_t *used, uint64_t *left)
{
    typedef struct {
        ULONG64 AvailableCommit, PrivateCommitUsage, PeakPrivateCommitUsage, TotalCommitUsage;
    } UwpAppMemory;
    typedef BOOL (WINAPI *GetProcessInformation_fn)(HANDLE, int, LPVOID, DWORD);
    GetProcessInformation_fn fn = (GetProcessInformation_fn)
        xemu_uwp_kernelbase_proc("GetProcessInformation");
    UwpAppMemory m = { 0 };
    if (!fn || !fn(GetCurrentProcess(), 2 /* ProcessAppMemoryInfo */, &m, sizeof(m))) {
        return false;
    }
    *used = m.PrivateCommitUsage;
    *left = m.AvailableCommit;
    return true;
}

bool xemu_uwp_memory(uint64_t *used, uint64_t *left)
{
    return uwp_memory(used, left);
}

/*
 * The render scale actually used: the player's setting, held to what the TV can show (more lines than the screen
 * has only cost memory) and to what the console's memory allows. Each scale step multiplies every game picture by
 * scale x scale: at 10x a 4 MB picture needs 400 MB, and a player's session froze when that could not be had
 * (2026-10-16, 1440p TV, 5086 MB allowed). Measured: about 2.0 GB at 1x, about 4.0 GB at 5x, so ~90 MB per
 * scale squared on top of ~2 GB; 600 MB is kept back. Called from the graphics thread when surfaces are set up.
 */
unsigned int xemu_uwp_cap_surface_scale(unsigned int want)
{
    static unsigned screen_lines, last_want, last_used;
    if (want <= 1) {
        return 1;
    }
    if (!screen_lines) {
        const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
        screen_lines = mode && mode->h > 0 ? (unsigned)mode->h : 1080;
    }
    unsigned by_screen = (screen_lines + 479) / 480;   /* 1080 and 1440 lines: 3, 2160 (4K): 5 */
    uint64_t used = 0, left = 0;
    unsigned by_memory = 3;   /* when the memory cannot be read: safe for every game seen so far */
    if (uwp_memory(&used, &left)) {
        double room = (double)((used + left) >> 20) - 2000.0 - 600.0;
        by_memory = room > 90.0 ? (unsigned)sqrt(room / 90.0) : 1;
    }
    unsigned use = MIN(want, MIN(by_screen, by_memory));
    if (use < 1) {
        use = 1;
    }
    if (want != last_want || use != last_used) {
        last_want = want;
        last_used = use;
        if (use < want) {
            uwp_log("render scale: %ux asked, %ux used (the TV shows %u lines, so up to %ux helps; memory allows up "
                    "to %ux)", want, use, screen_lines, by_screen, by_memory);
        }
    }
    return use;
}

/*
 * Render scale that follows the game (the owner's wish: "if it starts to even slightly stutter it needs to
 * automatically adjust"). While the console's GPU is the limit - the graphics thread waits on it a third of the time
 * or more, frames run late, no shaders are being made (that stall is not the picture size) and the emulated CPU is not
 * the limit itself - for 3 seconds in a row, the picture is drawn one scale step smaller. After 60 calm seconds it
 * goes back up a step, up to the player's own setting, which is never changed (the step down is a separate cap read
 * in pgraph_gl_reload_surface_scale_factor). 30 s between changes: each one costs a short pause.
 */
unsigned int xemu_uwp_auto_scale_cap; /* 0: none */
/* off: Game Vault said so (autoscale=0), or the player changed the render scale in xemu's menu this session (they are
   trying a scale out: theirs is used exactly, see pgraph_gl_set_surface_scale_factor) */
bool xemu_uwp_auto_scale_off;
bool xemu_uwp_auto_scale_busy; /* the change below is ours, not the player's */
static void uwp_auto_scale(int fps, int late, int gpu_wait, int vcpu, long shaders)
{
    static int strained, calm;
    static uint64_t next_change, last_up;
    static bool stay;   /* went back up and had to come down again soon: no more steps up this session */
    uint64_t now = SDL_GetTicks();
    if (xemu_uwp_auto_scale_off) {
        return;
    }
    if (fps <= 0) {
        return;   /* nothing drawn yet (start-up, loading): no reading to go by */
    }
    /* the scale getter and setter hand the big lock over to the renderer; this is called without it (crashed at
       0:00 in the first test, assert bql_locked) */
    bool held = bql_locked();
    if (!held) {
        bql_lock();
    }
    unsigned cur = nv2a_get_surface_scale_factor();
    if (!held) {
        bql_unlock();
    }
    unsigned want = xemu_uwp_cap_surface_scale(g_config.display.quality.surface_scale);
    bool gpu_bound = gpu_wait >= 35 && late >= 3 && shaders == 0 && vcpu < 90;
    /* test only (autoscale_test.txt next to xemu.toml): strained while at the full setting, calm below it, so the
       scale steps down and back up every 30-60 s and the switching itself can be watched */
    static int test = -1;
    if (test < 0) {
        char *p = g_strdup_printf("%sautoscale_test.txt", xemu_settings_get_base_path());
        test = g_file_test(p, G_FILE_TEST_EXISTS) ? 1 : 0;
        g_free(p);
    }
    if (test) {
        gpu_bound = cur >= want;
        gpu_wait = gpu_bound ? 50 : 0;
        late = gpu_bound ? 5 : 0;
    }
    strained = gpu_bound ? strained + 1 : 0;
    calm = (gpu_wait < 10 && late == 0) ? calm + 1 : 0;
    if (now < next_change) {
        return;
    }
    unsigned set = 0;
    if (cur > 1 && strained >= 3) {
        set = cur - 1;
        uwp_log("render scale: %ux -> %ux for now (the console's GPU was the limit: %d fps, waiting on it %d%%)",
                cur, set, fps, gpu_wait);
        /* each change costs a pause (0.5-2 s seen), so a game too heavy for the setting must not bounce */
        if (last_up && now - last_up < 90000) {
            stay = true;
            uwp_log("render scale: staying at %ux for this session (%ux was too much again)", set, cur);
        }
    } else if (!stay && xemu_uwp_auto_scale_cap && cur < want && calm >= 60) {
        last_up = now;
        set = cur + 1;
        uwp_log("render scale: %ux -> %ux again (60 calm seconds; the setting is %ux)", cur, set, want);
    }
    if (!set) {
        return;
    }
    xemu_uwp_auto_scale_cap = set >= want ? 0 : set;
    strained = calm = 0;
    next_change = now + 30000;
    if (!held) {
        bql_lock();
    }
    xemu_uwp_auto_scale_busy = true;
    nv2a_set_surface_scale_factor(g_config.display.quality.surface_scale);   /* the player's value, unchanged */
    xemu_uwp_auto_scale_busy = false;
    if (!held) {
        bql_unlock();
    }
    uwp_log("render scale: the change held the picture for %u ms", (unsigned)(SDL_GetTicks() - now));
    /* said on screen too, so nobody wonders why the picture got softer (off: Game Vault, the game's details, LT) */
    char *msg = g_strdup_printf(set < cur ? "Render scale %ux for now: the console could not keep up at %ux"
                                          : "Render scale back to %ux", set, cur);
    xemu_queue_notification(msg);
    g_free(msg);
}

/*
 * A start that never shows a picture: no frame at all, not even the Xbox start-up animation (players saw a game
 * sit like that for 10 minutes, 2026-10-16). Counted once a second while xemu is in front: after 25 s with no
 * frame since the start, the emulated console is restarted once by itself; if it is still blank 25 s after that,
 * the log says so and nothing more is done. A frame ends the watch. uwp_watch_start() starts it again when the
 * launcher swaps the disc.
 */
static void uwp_event(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
uint64_t uwp_background_since_ms(bool set, bool clear);
static struct {
    bool on, restarted;
    unsigned frames, quiet;
} uwp_watch = { .on = true };

static void uwp_watch_start(void)
{
    uwp_watch.on = true;
    uwp_watch.restarted = false;
    uwp_watch.frames = g_nv2a_stats.frame_count;
    uwp_watch.quiet = 0;
}

/* nothing shown since the start (or the last restart): the session is stuck, not worth continuing */
static bool uwp_watch_no_picture(void)
{
    return uwp_watch.on && g_nv2a_stats.frame_count == uwp_watch.frames && uwp_watch.quiet >= 5;
}

static void uwp_watch_second(void)
{
    /* testing: watch-test.txt next to xemu.toml makes the watch ignore the frames, as if nothing were shown */
    static int fake = -1;
    if (fake < 0) {
        char *own = SDL_GetPrefPath("xemu", "xemu");
        char *f = g_strdup_printf("%swatch-test.txt", own ? own : "");
        fake = g_file_test(f, G_FILE_TEST_EXISTS);
        if (fake) {
            uwp_log("start watch: test file, frames ignored");
        }
        g_free(f);
        SDL_free(own);
    }
    if (!uwp_watch.on || uwp_background_since_ms(false, false) || !runstate_is_running()) {
        return;
    }
    if (fake) {
        uwp_watch.frames = g_nv2a_stats.frame_count;
    }
    if (g_nv2a_stats.frame_count != uwp_watch.frames) {
        uwp_watch.on = false;   /* a picture: the start worked */
        return;
    }
    if (++uwp_watch.quiet < 25) {
        return;
    }
    if (uwp_watch.restarted) {
        uwp_event("still no picture 25 s after the restart: this game may not start in xemu yet");
        uwp_watch.on = false;
        return;
    }
    uwp_event("no picture 25 s after the start: restarting the emulated console once");
    xemu_main_loop_lock();
    qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
    xemu_main_loop_unlock();
    uwp_watch.restarted = true;
    uwp_watch.frames = g_nv2a_stats.frame_count;
    uwp_watch.quiet = 0;
}

static bool uwp_file_exists(const char *path);

static void uwp_box_sample(void)
{
    uwp_watch_second();
    {
        /*
         * One line a second, everything that says how the game ran:
         * speed and stutter (the slowest frame, frames over 34 ms), the emulated Xbox CPU and the
         * graphics thread, all of xemu and the whole console's CPU, memory, what the graphics did
         * (objects drawn per frame, shaders built: the usual cause of stutter, textures loaded),
         * how long the graphics waited on the console's GPU, disc reading (loading), the sound,
         * and the buttons held.
         */
        static unsigned last_frames, last_dry;
        static int64_t last_sync;
        static uint64_t last_app, last_wall, last_sys_busy, last_sys_all;
        unsigned at = (unsigned)((SDL_GetTicks() - uwp_session_start_ms) / 1000);
        uint64_t u = 0, l = 0;
        uwp_memory(&u, &l);
        extern char uwp_held_now_for_sec[];
        extern volatile unsigned xemu_uwp_audio_dry;
        /* frames since the last line */
        unsigned fc = g_nv2a_stats.frame_count, n = fc - last_frames;
        last_frames = fc;
        if (n > NV2A_PROF_NUM_FRAMES) {
            n = NV2A_PROF_NUM_FRAMES;
        }
        int slowest = 0, late = 0;
        long draws = 0, shaders = 0, textures = 0;
        for (unsigned i = 0; i < n; i++) {
            const typeof(g_nv2a_stats.frame_history[0]) *f =
                &g_nv2a_stats.frame_history[(g_nv2a_stats.frame_ptr + NV2A_PROF_NUM_FRAMES - 1 - i) %
                                            NV2A_PROF_NUM_FRAMES];
            /* the very first frame counts from no previous frame at all: not a real frame time */
            if (f->mspf < 60000) {
                slowest = f->mspf > slowest ? f->mspf : slowest;
            }
            late += f->mspf > 34;
            draws += f->counters[NV2A_PROF_BEGIN_ENDS];
            shaders += f->counters[NV2A_PROF_SHADER_GEN];
            textures += f->counters[NV2A_PROF_TEX_UPLOAD];
        }
        /* CPU: all of xemu (as cores' worth) and the whole console */
        FILETIME c, e, k, us, now;
        GetSystemTimeAsFileTime(&now);
        uint64_t wall = ((uint64_t)now.dwHighDateTime << 32) | now.dwLowDateTime, app = 0;
        if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &us)) {
            app = (((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime) +
                  (((uint64_t)us.dwHighDateTime << 32) | us.dwLowDateTime);
        }
        double app_cores = last_wall && wall > last_wall ? (double)(app - last_app) / (wall - last_wall) : 0;
        char console[48] = "";
        typedef BOOL (WINAPI *GetSystemTimes_fn)(LPFILETIME, LPFILETIME, LPFILETIME);
        GetSystemTimes_fn st = (GetSystemTimes_fn)xemu_uwp_kernelbase_proc("GetSystemTimes");
        FILETIME idle, kern, user;
        if (st && st(&idle, &kern, &user)) {
            uint64_t i64 = ((uint64_t)idle.dwHighDateTime << 32) | idle.dwLowDateTime;
            uint64_t all = (((uint64_t)kern.dwHighDateTime << 32) | kern.dwLowDateTime) +
                           (((uint64_t)user.dwHighDateTime << 32) | user.dwLowDateTime);
            uint64_t busy = all - i64;
            if (last_sys_all && all > last_sys_all) {
                snprintf(console, sizeof(console), ", whole console %d%%",
                         (int)((busy - last_sys_busy) * 100 / (all - last_sys_all)));
            }
            last_sys_busy = busy;
            last_sys_all = all;
        }
        /* the graphics thread waiting for the console's GPU to finish (the GPU is the limit when this is high) */
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        int64_t sync = uwp_prof_ticks[UWP_PROF_PF_SYNC];
        int gpu_wait = last_wall && wall > last_wall
                           ? (int)((sync - last_sync) * 100.0 / freq.QuadPart / ((wall - last_wall) / 1e7)) : 0;
        last_sync = sync;
        unsigned dry = xemu_uwp_audio_dry;
        /* bytes the game read from the disc and the Xbox hard disk (hw/ide), DMA and PIO */
        extern uint64_t xemu_uwp_hdd_read_bytes, xemu_uwp_dvd_read_bytes, xemu_uwp_dvd_reads;
        static uint64_t last_hdd, last_dvd, last_reads;
        uint64_t reads = qatomic_read(&xemu_uwp_dvd_reads);
        uint64_t dvd = qatomic_read(&xemu_uwp_dvd_read_bytes), hdd = qatomic_read(&xemu_uwp_hdd_read_bytes);
        double disc_mb = (dvd - last_dvd) / (double)(1 << 20), hdd_mb = (hdd - last_hdd) / (double)(1 << 20);
        last_hdd = hdd;
        unsigned nreads = (unsigned)(reads - last_reads);
        last_reads = reads;
        int vcpu_busy = xemu_uwp_thread_busy("vcpu", 5), pfifo_busy = xemu_uwp_thread_busy("pfifo", 5);
        {
            /* vk-capture.txt put next to xemu.toml: the Vulkan renderer logs every draw of the next frame
             * (pgraph/vk/draw.c), then the file is removed */
            extern int xemu_uwp_vk_capture;
            gchar *dir = g_path_get_dirname(xemu_settings_get_path());
            gchar *marker = g_build_filename(dir, "vk-capture.txt", NULL);
            if (uwp_file_exists(marker)) {
                g_remove(marker);
                uwp_log("capture: the next frame's draws go to xemu.log (vk-capture.txt)");
                qatomic_set(&xemu_uwp_vk_capture, 2);
            }
            g_free(marker);
            g_free(dir);
        }
        uwp_log("SEC %u:%02u | %d fps, slowest frame %d ms, %d frames over 34 ms | CPU: game %d%%, graphics %d%%, "
                "xemu %.1f cores%s | memory %llu of %llu MB | per frame: %ld objects drawn; this second: %ld shaders "
                "built, %ld textures loaded | waiting on the GPU %d%% | disc %.1f MB/s (%u reads) | hard disk %.1f MB/s | sound %s | holding: %s",
                at / 60, at % 60, g_nv2a_stats.increment_fps, slowest, late, vcpu_busy,
                pfifo_busy, app_cores, console, (unsigned long long)(u >> 20),
                (unsigned long long)((u + l) >> 20), n ? draws / (long)n : 0, shaders, textures,
                gpu_wait > 100 ? 100 : gpu_wait, disc_mb, nreads, hdd_mb,
                dry != last_dry ? "RAN DRY (the game could not make sound fast enough)" : "ok",
                uwp_held_now_for_sec[0] ? uwp_held_now_for_sec : "no buttons");
        uwp_auto_scale(g_nv2a_stats.increment_fps, late, gpu_wait, vcpu_busy, shaders);
        last_dvd = dvd;
        last_dry = dry;
        last_app = app;
        last_wall = wall;
        static unsigned last_brief;
        if (at >= 300 && at / 300 != last_brief / 300) {
            extern void uwp_session_summary_brief(void);
            last_brief = at;
            uwp_session_summary_brief();
        }
    }
    struct uwp_box_sample *b = &uwp_box[uwp_box_count % UWP_BOX_SECONDS];
    uint64_t used = 0, left = 0;
    uwp_memory(&used, &left);
    b->sec = (uint32_t)((SDL_GetTicks() - uwp_session_start_ms) / 1000);
    b->fps = (int16_t)g_nv2a_stats.increment_fps;
    b->vcpu = (int16_t)xemu_uwp_thread_busy("vcpu", 6);
    b->gpu = (int16_t)xemu_uwp_thread_busy("pfifo", 7);
    b->mem_used = (uint32_t)(used >> 20);
    b->mem_left = (uint32_t)(left >> 20);
    snprintf(b->state, sizeof(b->state), "%s", RunState_str(runstate_get()));
    uwp_box_count++;
}

/* A function name for an address in xemu.exe, from xemu.sym ("<rva hex> <name>" lines, sorted). */
static char *uwp_sym_text;
static DWORD uwp_sym_size;

static void uwp_sym_load(void)
{
    static bool tried;
    if (tried) {
        return;
    }
    tried = true;
    wchar_t path[MAX_PATH];
    const char *base = SDL_GetBasePath();
    if (!base || MultiByteToWideChar(CP_UTF8, 0, base, -1, path, MAX_PATH - 16) <= 0) {
        return;
    }
    wcscat(path, L"xemu.sym");
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD size = GetFileSize(h, NULL), got = 0;
    char *text = size && size < (64u << 20)
                     ? xemu_uwp_virtual_alloc(NULL, size + 1, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE) : NULL;
    if (text && ReadFile(h, text, size, &got, NULL) && got == size) {
        text[size] = '\0';
        uwp_sym_text = text;
        uwp_sym_size = size;
    }
    CloseHandle(h);
}

static bool uwp_sym_name(uint64_t rva, char *out, size_t n, uint64_t *off)
{
    uwp_sym_load();
    const char *best = NULL;
    uint64_t best_rva = 0;
    for (const char *line = uwp_sym_text; line && *line;) {
        uint64_t a = strtoull(line, NULL, 16);
        if (a > rva) {
            break;
        }
        best = line;
        best_rva = a;
        const char *nl = strchr(line, '\n');
        line = nl ? nl + 1 : NULL;
    }
    if (!best) {
        return false;
    }
    const char *name = strchr(best, ' ');
    if (!name) {
        return false;
    }
    name++;
    size_t len = strcspn(name, "\r\n");
    if (len >= n) {
        len = n - 1;
    }
    memcpy(out, name, len);
    out[len] = '\0';
    *off = rva - best_rva;
    return true;
}

/* "xemu.exe: pgraph_gl_draw_arrays+0x1c" or "ntdll.dll: RtlFreeHeap+0x40" */
static void uwp_where(uint64_t addr, char *out, size_t n)
{
    HMODULE mod = NULL;
    char path[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(uintptr_t)addr, &mod) && mod) {
        GetModuleFileNameA(mod, path, sizeof(path));
    }
    const char *file = strrchr(path, '\\');
    file = file ? file + 1 : path;
    uint64_t rva = addr - (uint64_t)(uintptr_t)mod;
    char name[200];
    uint64_t off = 0;
    uint32_t off32 = 0;
    const char *exp;
    if (mod && mod == GetModuleHandleA(NULL) && uwp_sym_name(rva, name, sizeof(name), &off)) {
        snprintf(out, n, "%s: %s+0x%llx", file, name, (unsigned long long)off);
    } else if (mod && mod != GetModuleHandleA(NULL) && (exp = uwp_export_name(mod, addr, &off32))) {
        snprintf(out, n, "%s: %s+0x%x", file, exp, off32);
    } else {
        snprintf(out, n, "%s+0x%llx", file, (unsigned long long)rva);
    }
}

static const char *uwp_thread_role(char *name, size_t n)
{
    DWORD me = GetCurrentThreadId();
    for (int i = 0; i < uwp_prof_num_threads; i++) {
        if (uwp_prof_threads[i].tid == me) {
            snprintf(name, n, "%s", uwp_prof_threads[i].name);
            if (!strncmp(name, "vcpu", 4)) {
                return "the emulated Xbox CPU (running the game's own code)";
            }
            if (!strncmp(name, "pfifo", 5)) {
                return "the graphics thread (drawing what the Xbox GPU was told to draw)";
            }
            if (!strncmp(name, "ui", 2)) {
                return "the screen thread (showing frames, controller, menus)";
            }
            return "a helper thread";
        }
    }
    snprintf(name, n, "thread %lu", me);
    return "a thread of the emulator or of the system (sound, disk, network)";
}

/*
 * The game cache on the Xbox hard disk (partitions X, Y, Z). Sector 4 of the disk (offset 0x800) is the
 * kernel's cache table: 0x97315286, a word, then one 12-byte entry per cache partition: title ID, partition
 * (0..2), in use. A title that finds itself there reuses that partition's files; one that does not gets a
 * partition formatted afresh. A session that crashes can leave half-written cache files, and the game then
 * says the disc may be dirty on every start (a player's racing game, 2026-10-16). So: a crash notes the game
 * (cache_check.txt next to the log), and its next start takes it out of the table first; Game Vault can also
 * ask for it (clearcache=1). tid= is the game's ID from Game Vault, e.g. VU-003.
 */
static char uwp_title_key[8];
static bool uwp_clear_cache_wanted;

static uint32_t uwp_title_id(const char *key)
{
    /* "VU-003" -> 0x56550003 */
    if (strlen(key) < 4 || key[2] != '-') {
        return 0;
    }
    return ((uint32_t)(uint8_t)key[0] << 24) | ((uint32_t)(uint8_t)key[1] << 16) | (uint32_t)(atoi(key + 3) & 0xffff);
}

static char *uwp_cache_mark_path(void)
{
    char *own = SDL_GetPrefPath("xemu", "xemu");
    char *path = g_strdup_printf("%scache_check.txt", own ? own : "");
    SDL_free(own);
    return path;
}

/* from the crash handlers: this game's cache may be half written */
static void uwp_cache_mark_crash(void)
{
    if (!uwp_title_key[0]) {
        return;
    }
    char *path = uwp_cache_mark_path();
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(uwp_title_key, f);
        fclose(f);
    }
    g_free(path);
}

/* in qemu_main after qemu_init (the main loop's lock held), before the game reaches its cache */
static void uwp_game_cache_check(void)
{
    if (!uwp_title_key[0]) {
        return;
    }
    const char *why = uwp_clear_cache_wanted ? "asked for in Game Vault" : NULL;
    char *path = uwp_cache_mark_path();
    gchar *mark = NULL;
    if (g_file_get_contents(path, &mark, NULL, NULL)) {
        if (!strcmp(g_strstrip(mark), uwp_title_key)) {
            why = why ? why : "the last session of this game crashed";
        }
        g_remove(path);
    }
    g_free(mark);
    g_free(path);
    if (!why) {
        return;
    }
    uint32_t tid = uwp_title_id(uwp_title_key);
    BlockBackend *blk = blk_by_name("ide0-hd0");
    uint8_t sec[512];
    if (!tid || !blk || blk_pread(blk, 0x800, sizeof(sec), sec, 0) < 0) {
        uwp_log("game cache: could not read the cache table (%s)", why);
        return;
    }
    if (ldl_le_p(sec) != 0x97315286) {
        uwp_log("game cache: no cache table on this hard disk yet (%s)", why);
        return;
    }
    int cleared = 0;
    for (int i = 0; i < 3; i++) {
        uint8_t *e = sec + 8 + 12 * i;
        if (ldl_le_p(e) == tid) {
            stl_le_p(e, 0);
            stl_le_p(e + 8, 0);
            cleared++;
        }
    }
    if (cleared && blk_pwrite(blk, 0x800, sizeof(sec), sec, 0) >= 0) {
        blk_flush(blk);
        uwp_event("game cache of %s cleared (%s): the game builds it again", uwp_title_key, why);
    } else {
        char held[64] = "";
        for (int i = 0; i < 3; i++) {
            uint32_t t = ldl_le_p(sec + 8 + 12 * i);
            if (t) {
                snprintf(held + strlen(held), sizeof(held) - strlen(held), "%s%c%c-%03u", held[0] ? ", " : "",
                         (char)(t >> 24), (char)(t >> 16), t & 0xffff);
            }
        }
        uwp_log("game cache: %s has none to clear (%s); the cache holds: %s", uwp_title_key, why,
                held[0] ? held : "nothing");
    }
}

static void uwp_crash_report(const char *what, CONTEXT *ctx, void *const *frames, int nframes)
{
    char name[40], line[320];
    if (uwp_crash_started) {
        SetEvent(uwp_crash_started);
    }
    uwp_cache_mark_crash();
    const char *role = uwp_thread_role(name, sizeof(name));
    uwp_err_drain();
    uwp_log("===== CRASH REPORT =====");
    uwp_log("What happened: %s", what);
    uwp_log("In: %s (%s)", role, name);
    uwp_log("Where it happened, innermost first:");
    uint64_t pcs[40];
    int n = 0;
    if (ctx) {
        CONTEXT copy = *ctx;
        pcs[n++] = copy.Rip;
        n += uwp_unwind(&copy, pcs + 1, 39);
    } else {
        for (int i = 0; i < nframes && n < 40; i++) {
            pcs[n++] = (uint64_t)(uintptr_t)frames[i];
        }
    }
    for (int i = 0; i < n; i++) {
        uwp_where(pcs[i], line, sizeof(line));
        uwp_log("  %2d  %s", i, line);
    }
    unsigned have = uwp_box_count < UWP_BOX_SECONDS ? uwp_box_count : UWP_BOX_SECONDS;
    uwp_log("The last %u seconds (time into the session: game fps, memory in use / still allowed, emulated CPU busy, "
            "graphics busy, state):", have);
    for (unsigned i = uwp_box_count - have; i < uwp_box_count; i++) {
        struct uwp_box_sample *b = &uwp_box[i % UWP_BOX_SECONDS];
        uwp_log("  %3u:%02u  %3d fps  %5u MB / %5u MB  cpu %3d%%  gpu %3d%%  %s", b->sec / 60, b->sec % 60,
                b->fps, b->mem_used, b->mem_left, b->vcpu, b->gpu, b->state);
    }
    uint64_t used = 0, left = 0;
    if (uwp_memory(&used, &left)) {
        uwp_log("Memory at the end: %llu MB in use, %llu MB still allowed%s", (unsigned long long)(used >> 20),
                (unsigned long long)(left >> 20),
                left < (200ull << 20) ? " (almost none left: running out of memory is the likely cause)" : "");
    }
    uwp_log("===== END OF CRASH REPORT =====");
    fflush(NULL);
}

static const char *uwp_exception_words(EXCEPTION_RECORD *r, char *out, size_t n)
{
    DWORD code = r ? r->ExceptionCode : 0;
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
        snprintf(out, n, "it tried to %s memory it may not touch (address 0x%llx)",
                 r->NumberParameters > 0 && r->ExceptionInformation[0] == 1 ? "write to"
                 : r->NumberParameters > 0 && r->ExceptionInformation[0] == 8 ? "run code in" : "read from",
                 r->NumberParameters > 1 ? (unsigned long long)r->ExceptionInformation[1] : 0ull);
        break;
    case EXCEPTION_STACK_OVERFLOW:
        snprintf(out, n, "it ran out of stack (a function called itself too deep)");
        break;
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        snprintf(out, n, "it ran an instruction this CPU does not have");
        break;
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        snprintf(out, n, "it divided by zero");
        break;
    case 0xC0000409:
        snprintf(out, n, "it stopped itself after finding damaged memory (fail-fast)");
        break;
    case 0xC0000374:
        snprintf(out, n, "its memory bookkeeping (heap) was damaged");
        break;
    default:
        snprintf(out, n, "exception 0x%08lx", code);
    }
    return out;
}

/*
 * The session recording, so a session can be read back without playing it:
 *  - "SEC" lines, one a second: speed, how busy the emulated CPU and the graphics thread were,
 *    memory, and the controller as it was held at that moment;
 *  - "INPUT" lines whenever the player's controller (port 1) changes: buttons pressed and let go,
 *    sticks and triggers rounded to a few levels (at most 10 lines a second for those);
 *  - small pictures of the screen (320x240 PNG) every 10 seconds and at every event, in
 *    "<game> screens" next to the log, named by the time into the session. Only the newest session
 *    of a game is kept, and a session stops at 360 pictures or 40 MB (from then on one a minute,
 *    and none past 600), so a long session cannot fill a drive.
 */
#define UWP_SHOTS_SOFT 360
#define UWP_SHOTS_HARD 600
#define UWP_SHOTS_BYTES (40u << 20)
static char *uwp_shot_dir;          /* "<publisher>\logs\<game> screens" */
static char uwp_shot_label[48];
static bool uwp_shot_pending;
static unsigned uwp_shot_count;
static uint64_t uwp_shot_bytes, uwp_next_shot;
char uwp_held_now_for_sec[96];      /* the controller as held now, for the SEC line */

static void uwp_shot_request(const char *label)
{
    snprintf(uwp_shot_label, sizeof(uwp_shot_label), "%s", label ? label : "");
    uwp_shot_pending = true;
}

/* asked by the UI (ui/xui/main.cc) once per frame: a picture to take now? */
const char *xemu_uwp_shot_wanted(void)
{
    if (!uwp_shot_pending || !uwp_shot_dir) {
        return NULL;
    }
    uwp_shot_pending = false;
    if (uwp_shot_count >= UWP_SHOTS_HARD) {
        return NULL;
    }
    return uwp_shot_label;
}

/*
 * The frame was all black: an event's picture is tried again on the next frames (about 5 s), the
 * 10-second picture is just skipped. Black pictures used to take one of the few report slots.
 */
static unsigned uwp_shot_black_tries;

void xemu_uwp_shot_black(const char *label)
{
    if (label[0] && uwp_shot_black_tries++ < 300) {
        uwp_shot_request(label);
    } else {
        uwp_shot_black_tries = 0;
    }
}

/*
 * "<minutes>-<seconds>-<tag>.png": the tag is the first few words of the event, letters and digits
 * only, joined by '-' ("000-00-BIOS-the-players-own.png"), cut at a word, never mid-word.
 */
static void uwp_shot_tag(const char *label, char *out, size_t n)
{
    size_t len = 0, word_end = 0;
    bool gap = false;
    for (const char *c = label; *c && len + 1 < n; c++) {
        if (g_ascii_isalnum(*c)) {
            if (gap && len) {
                word_end = len;
                if (len + 2 >= n || len >= 28) {
                    break;
                }
                out[len++] = '-';
            }
            out[len++] = *c;
            gap = false;
        } else if (*c != '\'') {
            gap = true;
        }
    }
    if (len >= 28 && word_end) {
        len = word_end;
    }
    out[len] = '\0';
}

void xemu_uwp_shot_save(const char *label, const void *png, size_t size)
{
    unsigned at = (unsigned)((SDL_GetTicks() - uwp_session_start_ms) / 1000);
    char name[96], tag[40];
    uwp_shot_black_tries = 0;
    uwp_shot_tag(label, tag, sizeof(tag));
    snprintf(name, sizeof(name), "%03u-%02u%s%s.png", at / 60, at % 60, tag[0] ? "-" : "", tag);
    char *path = g_strdup_printf("%s\\%s", uwp_shot_dir, name);
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(png, 1, size, f);
        fclose(f);
        uwp_shot_count++;
        uwp_shot_bytes += size;
        uwp_log("SCREEN %s", name);
    }
    g_free(path);
}

/* a new session: the folder of the game's pictures, emptied of the last session's */
static void uwp_shots_start(const char *share_log_path)
{
    char *dir = g_strdup(share_log_path);
    char *dot = strrchr(dir, '.');
    if (dot) {
        *dot = '\0';
    }
    uwp_shot_dir = g_strdup_printf("%s screens", dir);
    g_free(dir);
    g_mkdir_with_parents(uwp_shot_dir, 0755);
    GDir *d = g_dir_open(uwp_shot_dir, 0, NULL);
    for (const char *f; d && (f = g_dir_read_name(d));) {
        char *p = g_strdup_printf("%s\\%s", uwp_shot_dir, f);
        remove(p);
        g_free(p);
    }
    if (d) {
        g_dir_close(d);
    }
}

/* every 10 s a picture, fewer once the session has many */
static void uwp_shots_tick(void)
{
    if (!uwp_shot_dir || SDL_GetTicks() < uwp_next_shot) {
        return;
    }
    bool many = uwp_shot_count >= UWP_SHOTS_SOFT || uwp_shot_bytes >= UWP_SHOTS_BYTES;
    uwp_next_shot = SDL_GetTicks() + (many ? 60000 : 10000);
    uwp_shot_request("");
}

/* port 1's controller, from ui/xemu-input.c on every update */
static const char *uwp_stick_words(int x, int y, char *out, size_t n)
{
    int mag = (int)sqrt((double)x * x + (double)y * y);
    if (mag < 8000) {
        snprintf(out, n, "centre");
        return out;
    }
    static const char *const dirs[8] = { "right", "down-right", "down", "down-left", "left", "up-left", "up",
                                         "up-right" };
    double a = atan2((double)-y, (double)x);   /* screen y grows downwards: up is negative */
    int sector = ((int)lround(a / (M_PI / 4)) + 8) % 8;
    /* atan2 counts counter-clockwise from right: right, up-right, up, ... */
    static const int ccw_to_name[8] = { 0, 7, 6, 5, 4, 3, 2, 1 };
    snprintf(out, n, "%s %s", dirs[ccw_to_name[sector]], mag > 26000 ? "full" : "half");
    return out;
}

static const char *uwp_trigger_words(int v)
{
    return v < 4000 ? "off" : v < 24000 ? "half" : "full";
}

void xemu_uwp_note_input(uint32_t buttons, const int16_t *axis)
{
    static const char *const names[15] = { "A", "B", "X", "Y", "Left", "Up", "Right", "Down", "Back", "Start",
                                           "White", "Black", "LS-click", "RS-click", "Guide" };
    static uint32_t last_buttons;
    static char last_analog[96];
    static uint64_t next_analog;
    if (!uwp_session_start_ms) {
        return;
    }
    unsigned at = (unsigned)((SDL_GetTicks() - uwp_session_start_ms) / 100);
    uint32_t changed = buttons ^ last_buttons;
    if (changed) {
        char line[160] = "";
        for (int i = 0; i < 15; i++) {
            if (changed & (1u << i)) {
                size_t len = strlen(line);
                snprintf(line + len, sizeof(line) - len, " %s%s", buttons & (1u << i) ? "+" : "-", names[i]);
            }
        }
        uwp_log("INPUT %u:%02u.%u%s", at / 600, (at / 10) % 60, at % 10, line);
        last_buttons = buttons;
    }
    char ls[24], rs[24], analog[96];
    snprintf(analog, sizeof(analog), "LS %s, RS %s, LT %s, RT %s",
             uwp_stick_words(axis[CONTROLLER_AXIS_LSTICK_X], axis[CONTROLLER_AXIS_LSTICK_Y], ls, sizeof(ls)),
             uwp_stick_words(axis[CONTROLLER_AXIS_RSTICK_X], axis[CONTROLLER_AXIS_RSTICK_Y], rs, sizeof(rs)),
             uwp_trigger_words(axis[CONTROLLER_AXIS_LTRIG]), uwp_trigger_words(axis[CONTROLLER_AXIS_RTRIG]));
    if (strcmp(analog, last_analog) && SDL_GetTicks() >= next_analog) {
        uwp_log("INPUT %u:%02u.%u %s", at / 600, (at / 10) % 60, at % 10, analog);
        snprintf(last_analog, sizeof(last_analog), "%s", analog);
        next_analog = SDL_GetTicks() + 100;
    }
    /* what is held right now, for the SEC line */
    char held[96] = "";
    for (int i = 0; i < 15; i++) {
        if (buttons & (1u << i)) {
            size_t len = strlen(held);
            snprintf(held + len, sizeof(held) - len, "%s%s", len ? "+" : "", names[i]);
        }
    }
    snprintf(uwp_held_now_for_sec, sizeof(uwp_held_now_for_sec), "%s%s%s", held[0] ? held : "no buttons",
             strcmp(analog, "LS centre, RS centre, LT off, RT off") ? "; " : "",
             strcmp(analog, "LS centre, RS centre, LT off, RT off") ? analog : "");
}

/*
 * The story of the session, in plain words, so a log answers "what happened, and why" by itself:
 *  - events as they happen ("EVENT 2:16 went to the background ..."): the console's own events
 *    (Home screen, back, low memory, closing), controllers, quick save / load, quitting, and
 *    sudden changes of speed with what held it back;
 *  - every 30 seconds one line ("PERIOD ..."): average and lowest speed, what held it back (the
 *    emulated CPU, the graphics thread, or neither: then the game was waiting, on timing or on
 *    itself), and whether the player was playing;
 *  - the summary repeats both, so the first screen of a log tells the story.
 */
#define UWP_STORY_EVENTS 96
#define UWP_STORY_PERIODS 240
static char uwp_story_events[UWP_STORY_EVENTS][140];
static unsigned uwp_story_event_count;
static char uwp_story_periods[UWP_STORY_PERIODS][150];
static unsigned uwp_story_period_count;
static uint64_t uwp_background_since, uwp_foreground_at;
unsigned xemu_uwp_input_activity;   /* controller updates with something pressed (ui/xemu-input.c) */

static void uwp_event(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
static void uwp_event(const char *fmt, ...)
{
    char text[120];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    unsigned at = uwp_session_start_ms ? (unsigned)((SDL_GetTicks() - uwp_session_start_ms) / 1000) : 0;
    char *slot = uwp_story_events[uwp_story_event_count % UWP_STORY_EVENTS];
    snprintf(slot, sizeof(uwp_story_events[0]), "%3u:%02u  %s", at / 60, at % 60, text);
    uwp_story_event_count++;
    uwp_log("EVENT %s", slot);
    /* a picture of the screen at the moment, named after the event's first words */
    char label[40];
    snprintf(label, sizeof(label), "%s", text);
    for (char *c = label; *c; c++) {
        if (strchr("\\/:*?\"<>|;,()", *c)) {
            *c = ' ';
        }
    }
    uwp_shot_request(label);
}

/*
 * What held the speed back, from how busy the two threads were (percent of one processor core). The
 * emulated CPU thread runs the game's code without pause; below 100 % it was not given the core all the
 * time (other xemu threads, the console), so 40 % and more already means the game's CPU work was the limit.
 */
static const char *uwp_limit_words(int vcpu, int gpu)
{
    if (gpu >= 85) {
        return "the graphics thread";
    }
    if (vcpu >= 75) {
        return "the emulated CPU (the game's own work)";
    }
    if (vcpu >= 40 && gpu < 20) {
        return "the emulated CPU, which only got part of a processor core (waiting on the frame hand-off or "
               "other xemu threads)";
    }
    return "neither thread (the game was waiting: timing, loading, or itself)";
}

/* 3-second samples into 30-second periods, and sudden speed changes */
static struct {
    unsigned n, fps_sum, active_from;
    int fps_min, vcpu_sum, gpu_sum, start;
    double last_avg;
    uint64_t next_change_ok;
} uwp_period = { .fps_min = 1000, .start = -1 };

static void uwp_story_sample(int fps)
{
    int at = (int)((SDL_GetTicks() - uwp_session_start_ms) / 1000);
    int vcpu = xemu_uwp_thread_busy("vcpu", 4), gpu = xemu_uwp_thread_busy("pfifo", 4);
    if (uwp_period.start < 0) {
        uwp_period.start = at;
        uwp_period.active_from = xemu_uwp_input_activity;
    }
    /* a sudden drop against the last period's average */
    /* (not in the first seconds back from the background: that sample still holds the paused time) */
    bool settling = uwp_foreground_at && SDL_GetTicks() - uwp_foreground_at < 6000;
    if (uwp_period.last_avg >= 20 && fps < uwp_period.last_avg * 0.5 && SDL_GetTicks() >= uwp_period.next_change_ok &&
        !uwp_background_since && !settling) {
        char after[64] = "";
        if (uwp_foreground_at && SDL_GetTicks() - uwp_foreground_at < 30000) {
            snprintf(after, sizeof(after), ", %u s after coming back from the background",
                     (unsigned)((SDL_GetTicks() - uwp_foreground_at) / 1000));
        }
        uwp_event("speed dropped from %.0f to %d fps%s; held back by %s (CPU %d%%, graphics %d%%)",
                  uwp_period.last_avg, fps, after, uwp_limit_words(vcpu, gpu), vcpu, gpu);
        uwp_period.next_change_ok = SDL_GetTicks() + 30000;
    }
    uwp_period.n++;
    uwp_period.fps_sum += fps > 0 ? fps : 0;
    if (fps < uwp_period.fps_min) {
        uwp_period.fps_min = fps;
    }
    uwp_period.vcpu_sum += vcpu > 0 ? vcpu : 0;
    uwp_period.gpu_sum += gpu > 0 ? gpu : 0;
    if (uwp_period.n < 10) {
        return;
    }
    double avg = (double)uwp_period.fps_sum / uwp_period.n;
    int v = uwp_period.vcpu_sum / (int)uwp_period.n, g = uwp_period.gpu_sum / (int)uwp_period.n;
    unsigned presses = xemu_uwp_input_activity - uwp_period.active_from;
    char *line = uwp_story_periods[uwp_story_period_count % UWP_STORY_PERIODS];
    snprintf(line, sizeof(uwp_story_periods[0]), "%3d:%02d-%d:%02d  %3.0f fps (lowest %2d)  %-22s  %s",
             uwp_period.start / 60, uwp_period.start % 60, at / 60, at % 60, avg, uwp_period.fps_min,
             presses > 10 ? "player playing" : "no controller input",
             avg < 1 ? "nothing drawn (loading, paused or stuck)" : uwp_limit_words(v, g));
    uwp_story_period_count++;
    uwp_log("PERIOD %s (CPU %d%%, graphics %d%%)", line, v, g);
    uwp_period.last_avg = avg;
    uwp_period.n = uwp_period.fps_sum = 0;
    uwp_period.vcpu_sum = uwp_period.gpu_sum = 0;
    uwp_period.fps_min = 1000;
    uwp_period.start = -1;
}

uint64_t uwp_background_since_ms(bool set, bool clear)
{
    uint64_t was = uwp_background_since;
    if (set) {
        uwp_background_since = SDL_GetTicks();
    }
    if (clear) {
        uwp_background_since = 0;
        uwp_foreground_at = SDL_GetTicks();
        uwp_period.next_change_ok = 0;
    }
    return was;
}

static void uwp_story_print(void)
{
    unsigned first = uwp_story_event_count > UWP_STORY_EVENTS ? uwp_story_event_count - UWP_STORY_EVENTS : 0;
    uwp_log("What happened (time into the session):");
    for (unsigned i = first; i < uwp_story_event_count; i++) {
        uwp_log("  %s", uwp_story_events[i % UWP_STORY_EVENTS]);
    }
    first = uwp_story_period_count > UWP_STORY_PERIODS ? uwp_story_period_count - UWP_STORY_PERIODS : 0;
    uwp_log("Every 30 seconds (speed, the player, what held the speed back):");
    for (unsigned i = first; i < uwp_story_period_count; i++) {
        uwp_log("  %s", uwp_story_periods[i % UWP_STORY_PERIODS]);
    }
}

/* the start: the console, the TV, the memory the console grants (it tells App type Game from App) */
static void uwp_story_start(int w, int h)
{
    uint64_t used = 0, left = 0;
    uwp_memory(&used, &left);
    unsigned total = (unsigned)((used + left) >> 20);
    const char *disc = g_config.sys.files.dvd_path && g_config.sys.files.dvd_path[0]
                           ? g_config.sys.files.dvd_path : "(no disc: the dashboard)";
    uwp_event("started: %s", disc);
    uwp_event("screen %dx%d; memory allowed %u MB (%s)", w, h, total,
              total >= 3000 ? "App type Game: good" : "App type App? Set it to Game in Dev Home");
    uwp_event("BIOS: %s; hard disk: %s",
              strstr(g_config.sys.files.flashrom_path ? g_config.sys.files.flashrom_path : "", "SpiritBoot")
                  ? "the free one (offline only)" : "the player's own",
              g_config.sys.files.hdd_path && strstr(g_config.sys.files.hdd_path, "/xbox/")
                  ? "the copy on the USB drive (no disk on the console)" : "the console's");
}

static void uwp_session_sample(int fps)
{
    int at = (int)((SDL_GetTicks() - uwp_session_start_ms) / 1000);
    if (fps <= 0) {
        return;   /* nothing drawn: loading, a menu that waits, or paused */
    }
    uwp_session.samples++;
    uwp_session.sum += fps;
    if (fps < uwp_session.lowest) {
        uwp_session.lowest = fps;
        uwp_session.lowest_at = at;
    }
    if (fps < 25) {
        uwp_session.slow++;
        if (uwp_session.slow_from < 0) {
            uwp_session.slow_from = at;
            uwp_session.stretch_len = 0;
        }
        uwp_session.stretch_len += 3;
        if (uwp_session.stretch_len > uwp_session.worst_stretch_len) {
            uwp_session.worst_stretch_len = uwp_session.stretch_len;
            uwp_session.worst_stretch_from = uwp_session.slow_from;
        }
    } else {
        uwp_session.slow_from = -1;
    }
    if (fps < 15) {
        uwp_session.very_slow++;
    }
}

/* the session's start on a clock SDL cannot reset (SDL_Quit restarts SDL_GetTicks before atexit runs) */
static uint64_t uwp_session_start_tick;

static void uwp_session_summary_print(const char *how);

static void uwp_session_summary(const char *how)
{
    if (uwp_session.written) {
        return;
    }
    uwp_session.written = true;
    uwp_session_summary_print(how);
}

/*
 * When xemu goes to the background the console may close it without any warning, so a summary of the
 * session so far is written then too (the session is not marked as summed up: the real end still writes one).
 */
static void uwp_session_summary_so_far(void)
{
    if (!uwp_session.written && uwp_session_start_tick) {
        uwp_session_summary_print("not ended yet: xemu went to the background (if the console closes it there, "
                                  "this is the last summary)");
    }
}

static void uwp_session_summary_head(const char *how, bool story);

static void uwp_session_summary_print(const char *how)
{
    uwp_session_summary_head(how, true);
}

/*
 * Every 5 minutes of play a short summary (no event list): when the player closes the game from the Xbox
 * menu, the console ends xemu without a word, and the report used to have no summary at all.
 */
void uwp_session_summary_brief(void)
{
    if (!uwp_session.written && uwp_session_start_tick) {
        uwp_session_summary_head("not ended yet: still playing (a short summary every 5 minutes; if nothing follows, "
                                 "the console closed xemu)", false);
    }
}

static void uwp_session_summary_head(const char *how, bool story)
{
    int secs = uwp_session_start_tick ? (int)((GetTickCount64() - uwp_session_start_tick) / 1000) : 0;
    const char *disc = g_config.sys.files.dvd_path && g_config.sys.files.dvd_path[0]
                           ? g_config.sys.files.dvd_path : "(no disc)";
    uwp_log("===== SUMMARY =====");
    uwp_log("Game: %s", disc);
    uwp_log("Emulator: xemu for Dev Mode %s", uwp_version_text());
    uwp_log("Played: %d min %d s", secs / 60, secs % 60);
    if (uwp_session.samples) {
        uwp_log("Speed: average %.0f fps, lowest %d fps (at %d:%02d into the session)",
                uwp_session.sum / uwp_session.samples, uwp_session.lowest,
                uwp_session.lowest_at / 60, uwp_session.lowest_at % 60);
        uwp_log("Slow: %u s below 25 fps, %u s below 15 fps; longest slow stretch %d s (from %d:%02d)",
                uwp_session.slow * 3, uwp_session.very_slow * 3, uwp_session.worst_stretch_len,
                uwp_session.worst_stretch_from / 60, uwp_session.worst_stretch_from % 60);
    } else {
        uwp_log("Speed: the game never drew a frame");
    }
    uwp_log("Ended: %s", how);
    if (story) {
        uwp_story_print();
    }
    uwp_log("===== END OF SUMMARY =====");
    fflush(NULL);
}

static LONG WINAPI uwp_on_crash(EXCEPTION_POINTERS *e)
{
    char how[160], words[160];
    uwp_crash_report(uwp_exception_words(e ? e->ExceptionRecord : NULL, words, sizeof(words)),
                     e ? e->ContextRecord : NULL, NULL, 0);
    DWORD code = e && e->ExceptionRecord ? e->ExceptionRecord->ExceptionCode : 0;
    HMODULE mod = NULL;
    char name[MAX_PATH] = "?";
    void *where = e && e->ExceptionRecord ? e->ExceptionRecord->ExceptionAddress : NULL;
    if (where && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)where, &mod) && mod) {
        GetModuleFileNameA(mod, name, sizeof(name));
    }
    const char *base = strrchr(name, '\\');
    snprintf(how, sizeof(how), "CRASHED (exception 0x%08lx in %s+0x%llx)", code, base ? base + 1 : name,
             (unsigned long long)((uintptr_t)where - (uintptr_t)mod));
    uwp_session_summary(how);
    return EXCEPTION_CONTINUE_SEARCH;
}

static void uwp_on_abort(int sig)
{
    /*
     * The assert text goes to a message box that an Xbox app cannot show, so
     * log where it happened instead: module + offset for every caller, to be
     * looked up in the unstripped exe (addr2line -e xemu_<ver>_unstripped.exe).
     */
    void *frames[40];
    USHORT n = RtlCaptureStackBackTrace(0, 40, frames, NULL);
    uwp_crash_report("a check inside the emulator failed (assert / abort)", NULL, frames, n);
    uwp_log("ABORT (signal %d): a check inside the emulator failed, thread %lu",
            sig, GetCurrentThreadId());
    for (USHORT i = 0; i < n; i++) {
        HMODULE mod = NULL;
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)frames[i], &mod) && mod) {
            GetModuleFileNameA(mod, name, sizeof(name));
        }
        const char *base = strrchr(name, '\\');
        uwp_log("  #%u %s+0x%llx", i, base ? base + 1 : name,
                (unsigned long long)((uintptr_t)frames[i] - (uintptr_t)mod));
    }
    uwp_session_summary("STOPPED on an internal error (the ABORT lines above say where)");
    fflush(NULL);
}

#ifdef XEMU_PGO_GEN
/*
 * Profile-guided build, first step (build-uwp-pgo with -fprofile-generate): the counters are written
 * to LocalState\pgo every 60 seconds and reset, because an app on the console is usually ended, not
 * closed, and the end-of-program write would never happen. Never in a release build.
 */
extern void __gcov_dump(void);
extern void __gcov_reset(void);
static DWORD WINAPI uwp_pgo_thread(void *arg)
{
    for (int n = 1;; n++) {
        Sleep(60000);
        __gcov_dump();
        __gcov_reset();
        uwp_log("profile for the optimized build written (%d)", n);
    }
    return 0;
}
#endif

static bool uwp_machine_started;
/* exit() before the machine ran: QEMU's own error text comes through the stderr pipe, which needs a moment */
static void uwp_on_early_exit(void)
{
    if (uwp_machine_started) {
        return;
    }
    fflush(stderr);
    Sleep(300);
    uwp_log("xemu stopped while starting (exit before the game ran); the lines above say why");
    fflush(NULL);
}

static void uwp_on_exit(void)
{
    uwp_session_summary("closed (quit, or the console closed the app)");
    uwp_log("exit() reached");
    fflush(NULL);
}

static void uwp_glib_print(const gchar *text)
{
    uwp_log("glib: %s", text);
}

/* How much memory the system still grants this app (the console ends an app that asks for more). */
unsigned int xemu_uwp_counters[UWP_CNT_MAX];

/*
 * The dashboard from the user's own disc. Xbox Live sign-up (and with it
 * online play through a replacement service) needs Microsoft's dashboard on
 * the hard disk, and the free hard disk image has none. Many Xbox Live games
 * carry its installer; when the game being started has one and the hard disk
 * has no dashboard, or an older one, it is unpacked onto C: before the
 * console boots (ui/xemu-uwp-dash.c). The link parameter dash=0 turns this off.
 */
static bool uwp_no_dash;

static int uwp_dash_blk_read(void *opaque, uint64_t offset, void *buf, size_t len)
{
    return blk_pread(opaque, offset, len, buf, 0) < 0;
}

static int uwp_dash_blk_write(void *opaque, uint64_t offset, const void *buf, size_t len)
{
    return blk_pwrite(opaque, offset, len, buf, 0) < 0;
}

void xemu_uwp_dashboard_from_disc(void)
{
    DriveInfo *hd = drive_get(IF_IDE, 0, 0), *cd = drive_get(IF_IDE, 0, 1);
    if (uwp_no_dash || !hd || !cd) {
        return;
    }
    BlockBackend *hdd_blk = blk_by_legacy_dinfo(hd), *disc_blk = blk_by_legacy_dinfo(cd);
    if (!hdd_blk || !disc_blk || !blk_is_inserted(disc_blk)) {
        return;
    }
    UwpDashIo hdd = { uwp_dash_blk_read, uwp_dash_blk_write, hdd_blk };
    UwpDashIo disc = { uwp_dash_blk_read, NULL, disc_blk };
    uint32_t have = 0, offer = 0;
    int installed = uwp_dash_installed(&hdd, &have);
    if (installed < 0) {
        uwp_log("dashboard: the hard disk has no Xbox C: partition, left alone");
        return;
    }
    if (!uwp_dash_on_disc(&disc, &offer)) {
        uwp_log("dashboard: %s on the hard disk; this disc carries no installer",
                installed ? "present" : "none");
        return;
    }
    if (installed && have >= offer) {
        uwp_log("dashboard: %08x on the hard disk, the disc offers %08x: nothing to do", have, offer);
        return;
    }
    uwp_log("dashboard: %s on the hard disk, installing %08x from the disc",
            installed ? "an older one" : "none", offer);
    if (uwp_dash_install(&disc, &hdd, uwp_log) > 0) {
        blk_flush(hdd_blk);
        xemu_queue_notification("Xbox dashboard installed from this disc");
    }
}

/* the package version (Major.Minor.Build.Revision); xemu_version is not set in this build */
static const char *uwp_version_text(void)
{
    static char text[32];
    if (text[0]) {
        return text;
    }
    typedef struct {
        UINT32 reserved, arch;
        UINT16 revision, build, minor, major;
        PWSTR name, publisher, resource, publisher_id;
    } UwpPackageId;
    typedef LONG (WINAPI *GetCurrentPackageId_fn)(UINT32 *, BYTE *);
    GetCurrentPackageId_fn fn = (GetCurrentPackageId_fn)xemu_uwp_kernelbase_proc("GetCurrentPackageId");
    union { UwpPackageId id; BYTE raw[1024]; } buf;
    UINT32 len = sizeof(buf);
    if (fn && fn(&len, buf.raw) == 0) {
        snprintf(text, sizeof(text), "%u.%u.%u.%u", buf.id.major, buf.id.minor, buf.id.build, buf.id.revision);
    } else {
        snprintf(text, sizeof(text), "%s", xemu_version);
    }
    return text;
}

/*
 * What the console gives xemu, once at start: CPU name (CPUID) and clock (the time-stamp counter measured
 * against the performance counter for 100 ms), processors xemu may use, memory, Windows build. The GPU line
 * comes from the GL context ("GPU: ..."). Temperatures and GPU clocks are not readable by apps on the Xbox.
 */
static void uwp_log_hardware(void)
{
    char brand[49];
    const char *b = uwp_cpu_brand(brand);
    LARGE_INTEGER f, q0, q1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&q0);
    uint64_t t0 = __rdtsc();
    Sleep(100);
    uint64_t t1 = __rdtsc();
    QueryPerformanceCounter(&q1);
    double mhz = (double)(t1 - t0) / ((double)(q1.QuadPart - q0.QuadPart) / f.QuadPart) / 1e6;
    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    MEMORYSTATUSEX ms = { .dwLength = sizeof(ms) };
    typedef BOOL (WINAPI *GlobalMemoryStatusEx_fn)(LPMEMORYSTATUSEX);
    GlobalMemoryStatusEx_fn mem = (GlobalMemoryStatusEx_fn)xemu_uwp_kernelbase_proc("GlobalMemoryStatusEx");
    bool have_mem = mem && mem(&ms);
    typedef LONG (WINAPI *RtlGetVersion_fn)(OSVERSIONINFOW *);
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    RtlGetVersion_fn ver = ntdll ? (RtlGetVersion_fn)GetProcAddress(ntdll, "RtlGetVersion") : NULL;
    OSVERSIONINFOW os = { .dwOSVersionInfoSize = sizeof(os) };
    bool have_os = ver && ver(&os) == 0;
    uwp_log("xemu for Dev Mode %s", uwp_version_text());
    /* the time-stamp counter ticks at a fixed rate, not the core clock: the clock comes from the model */
    const char *clock = strstr(b, "Series X") ? "8 AMD Zen 2 cores at up to 3.8 GHz (3.66 GHz with SMT)"
                        : strstr(b, "Series S") ? "8 AMD Zen 2 cores at up to 3.6 GHz (3.4 GHz with SMT)"
                                                : "clock not known for this model";
    uwp_log("CPU: %s: %s; %lu logical processors visible to xemu (time-stamp counter %.0f MHz)",
            b[0] ? b : "(unknown)", clock, (unsigned long)si.dwNumberOfProcessors, mhz);
    {
        /* which logical processors share a physical core (SMT): threads on the same core slow each other */
        typedef BOOL (WINAPI *GetSystemCpuSetInformation_fn)(PSYSTEM_CPU_SET_INFORMATION, ULONG, PULONG, HANDLE, ULONG);
        GetSystemCpuSetInformation_fn sets =
            (GetSystemCpuSetInformation_fn)xemu_uwp_kernelbase_proc("GetSystemCpuSetInformation");
        static char buf[4096];
        ULONG len = 0;
        if (sets && sets((PSYSTEM_CPU_SET_INFORMATION)buf, sizeof(buf), &len, GetCurrentProcess(), 0)) {
            char line[512];
            int n = snprintf(line, sizeof(line), "CPU sets (id core/logical, a=allocated to xemu, r=reserved):");
            for (ULONG off = 0; off < len && n < (int)sizeof(line) - 32;) {
                PSYSTEM_CPU_SET_INFORMATION s = (PSYSTEM_CPU_SET_INFORMATION)(buf + off);
                if (s->Size == 0) {
                    break;
                }
                if (s->Type == CpuSetInformation) {
                    n += snprintf(line + n, sizeof(line) - n, " %lu %u/%u%s%s", (unsigned long)s->CpuSet.Id,
                                  s->CpuSet.CoreIndex, s->CpuSet.LogicalProcessorIndex,
                                  s->CpuSet.AllocatedToTargetProcess ? "a" : "",
                                  s->CpuSet.Allocated && !s->CpuSet.AllocatedToTargetProcess ? "r" : "");
                }
                off += s->Size;
            }
            uwp_log("%s", line);
        } else {
            uwp_log("CPU sets: not available (%lu)", (unsigned long)GetLastError());
        }
    }
    if (have_mem) {
        uwp_log("memory the console gives games: %llu MB in all, %llu MB free", (unsigned long long)(ms.ullTotalPhys >> 20),
                (unsigned long long)(ms.ullAvailPhys >> 20));
    }
    if (have_os) {
        uwp_log("Windows build %lu.%lu.%lu", (unsigned long)os.dwMajorVersion, (unsigned long)os.dwMinorVersion,
                (unsigned long)os.dwBuildNumber);
    }
    uwp_log("settings: render scale %d, vsync %s", g_config.display.quality.surface_scale,
            g_config.display.window.vsync ? "on" : "off");
    uwp_log("not readable by apps on the Xbox: temperatures, GPU clock, fan speed");
    /* Mesa's shader disk cache (MESA_SHADER_CACHE_DIR, set in main): how much a session can reuse */
    const char *cache = getenv("MESA_SHADER_CACHE_DIR");
    if (cache) {
        unsigned files = 0;
        uint64_t bytes = 0;
        GQueue dirs = G_QUEUE_INIT;
        g_queue_push_tail(&dirs, g_strdup(cache));
        char *dir;
        while ((dir = g_queue_pop_head(&dirs))) {
            GDir *d = g_dir_open(dir, 0, NULL);
            const char *name;
            while (d && (name = g_dir_read_name(d))) {
                char *path = g_build_filename(dir, name, NULL);
                GStatBuf st;
                if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
                    g_queue_push_tail(&dirs, path);
                    continue;
                }
                if (g_stat(path, &st) == 0) {
                    files++;
                    bytes += (uint64_t)st.st_size;
                }
                g_free(path);
            }
            if (d) {
                g_dir_close(d);
            }
            g_free(dir);
        }
        uwp_log("shader disk cache: %u files, %llu MB in %s", files, (unsigned long long)(bytes >> 20), cache);
    }
}

static void uwp_log_memory(void)
{
    typedef struct {
        ULONG64 AvailableCommit, PrivateCommitUsage, PeakPrivateCommitUsage, TotalCommitUsage;
    } UwpAppMemory;
    typedef BOOL (WINAPI *GetProcessInformation_fn)(HANDLE, int, LPVOID, DWORD);
    GetProcessInformation_fn fn = (GetProcessInformation_fn)
        xemu_uwp_kernelbase_proc("GetProcessInformation");
    UwpAppMemory m = { 0 };
    if (fn && fn(GetCurrentProcess(), 2 /* ProcessAppMemoryInfo */, &m, sizeof(m))) {
        uwp_log("memory: %llu MB in use, %llu MB more allowed, peak %llu MB",
                (unsigned long long)(m.PrivateCommitUsage >> 20),
                (unsigned long long)(m.AvailableCommit >> 20),
                (unsigned long long)(m.PeakPrivateCommitUsage >> 20));
    }
    {
        static unsigned int last[UWP_CNT_MAX];
        unsigned int *c = xemu_uwp_counters;
        uwp_log("gpu objects: queries waiting %u (read early %u), textures +%u -%u, surfaces +%u -%u, "
                "stream buffers %u, vertex arrays realigned %u, GL errors %u",
                c[UWP_CNT_QUERIES_PENDING], c[UWP_CNT_QUERIES_FOLDED] - last[UWP_CNT_QUERIES_FOLDED],
                c[UWP_CNT_TEX_NEW] - last[UWP_CNT_TEX_NEW], c[UWP_CNT_TEX_DEL] - last[UWP_CNT_TEX_DEL],
                c[UWP_CNT_SURF_NEW] - last[UWP_CNT_SURF_NEW], c[UWP_CNT_SURF_DEL] - last[UWP_CNT_SURF_DEL],
                c[UWP_CNT_STREAM_WRAP] - last[UWP_CNT_STREAM_WRAP],
                c[UWP_CNT_VERTEX_REPACK] - last[UWP_CNT_VERTEX_REPACK], c[UWP_CNT_GL_ERRORS]);
        memcpy(last, c, sizeof(last));
    }
}

/*
 * The three system files as xemu.toml gave them (empty when there is no xemu.toml). What xemu
 * picks by itself at startup (Game Vault's folder, the free BIOS, a new blank disk) is never
 * written back: otherwise a path chosen once would stick, and a BIOS found later on the
 * player's USB drive, or a new package version with another install path, would be ignored.
 */
static char *uwp_toml_bootrom, *uwp_toml_bios, *uwp_toml_hdd, *uwp_toml_eeprom;
static bool uwp_toml_remembered;

static void uwp_save_settings(void)
{
    if (!uwp_toml_remembered) {
        xemu_settings_save();
        return;
    }
    const char **slot[] = { &g_config.sys.files.bootrom_path,
                            &g_config.sys.files.flashrom_path,
                            &g_config.sys.files.hdd_path,
                            &g_config.sys.files.eeprom_path };
    char *toml[] = { uwp_toml_bootrom, uwp_toml_bios, uwp_toml_hdd, uwp_toml_eeprom };
    char *now[4];
    for (int i = 0; i < 4; i++) {
        now[i] = g_strdup(*slot[i] ? *slot[i] : "");
        xemu_settings_set_string(slot[i], toml[i]);
    }
    xemu_settings_save();
    for (int i = 0; i < 4; i++) {
        xemu_settings_set_string(slot[i], now[i]);
        g_free(now[i]);
    }
}

static void uwp_save_for_background(void)
{
    if (!uwp_machine_running) {
        return;
    }
    xemu_main_loop_lock();
    int ret = bdrv_flush_all();
    xemu_main_loop_unlock();
    uwp_save_settings();
    uwp_log("going to the background: disks flushed (%d), settings saved", ret);
}

/*
 * Runs inside SDL's event pump at the moment the system announces the
 * suspension (the polling loop would only see the event after the app has
 * been resumed, which may be never). No xemu lock is held here.
 */
static void uwp_event(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
extern uint64_t uwp_background_since_ms(bool set, bool clear);

static bool uwp_event_watch(void *userdata, SDL_Event *event)
{
    switch (event->type) {
    case SDL_EVENT_WILL_ENTER_BACKGROUND:
        uwp_background_since_ms(true, false);
        uwp_event("went to the background (Xbox button, Home screen or another app); the console may pause or "
                  "close xemu now");
        uwp_save_for_background();
        uwp_session_summary_so_far();
        fflush(NULL);
        break;
    case SDL_EVENT_DID_ENTER_FOREGROUND: {
        uint64_t since = uwp_background_since_ms(false, true);
        uwp_event("came back to the game after %u s in the background",
                  since ? (unsigned)((SDL_GetTicks() - since) / 1000) : 0);
        break;
    }
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        uwp_event("the game lost focus (the guide, a notification or another app over it)");
        break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        uwp_event("the game has focus again");
        break;
    case SDL_EVENT_LOW_MEMORY:
        uwp_event("the console warned that memory is low");
        break;
    case SDL_EVENT_TERMINATING:
        uwp_event("the console is closing xemu");
        break;
    case SDL_EVENT_GAMEPAD_ADDED:
        uwp_event("controller connected");
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        uwp_event("controller disconnected");
        break;
    default:
        break;
    }
    return true;
}

/* Game Vault's folder on the games drive, from "home=" in the link (see uwp_home_disk) */
static char *uwp_home;

/* the overlay: the launcher switches it with each game */
bool xemu_uwp_overlay_on;

/* A link value with the %XX escaping undone and forward slashes (as xemu.toml uses). */
static char *uwp_unescape(const char *in)
{
    char *result = g_malloc(strlen(in) + 1);
    char *out = result;
    for (; *in; in++) {
        if (in[0] == '%' && g_ascii_isxdigit(in[1]) && g_ascii_isxdigit(in[2])) {
            *out = (char)(g_ascii_xdigit_value(in[1]) * 16 + g_ascii_xdigit_value(in[2]));
            in += 2;
        } else {
            *out = *in;
        }
        if (*out == '\\') {
            *out = '/';
        }
        out++;
    }
    *out = '\0';
    if (!result[0]) {
        g_free(result);
        return NULL;
    }
    return result;
}

/*
 * The test and measuring options of the link (quit, press, crashtest, dash, noflush, skipdraws, state)
 * are honoured only when test-links.txt lies next to the log (LocalState/xemu/xemu), which only the
 * console's Device Portal can put there: any app on the console can open an xemu: link, and must not
 * crash it or press its buttons. A launcher needs disc, home and the return link only.
 */
/* a test file next to the log (LocalState/xemu/xemu); only the Device Portal can put one there */
bool xemu_uwp_test_file_exists(const char *name);
bool xemu_uwp_test_file_exists(const char *name)
{
    char *base = SDL_GetPrefPath("xemu", "xemu");
    char *file = g_strdup_printf("%s%s", base ? base : "", name);
    bool found = g_file_test(file, G_FILE_TEST_EXISTS);
    g_free(file);
    SDL_free(base);
    return found;
}

static bool uwp_test_links_allowed(void)
{
    char *base = SDL_GetPrefPath("xemu", "xemu");
    char *file = g_strdup_printf("%stest-links.txt", base ? base : "");
    bool allowed = g_file_test(file, G_FILE_TEST_EXISTS);
    g_free(file);
    SDL_free(base);
    return allowed;
}

/*
 * A drive this app may not open itself (the console remembers a refused consent per app, e.g. a big USB hard
 * disk): Game Vault, which can, shares the disc image with SharedStorageAccessManager and passes the token as
 * disctoken=. It is used only for that disc and only when opening it the normal way fails (block/file-win32.c
 * via xemu_uwp_create_file_w), read-only; the handle comes from SDL_WinRTOpenSharedFile (our SDL3 fork).
 */
static char *uwp_share_token, *uwp_share_disc;
char *xemu_uwp_rtc_base; /* link rtcbase= (testing), read by system/vl.c */
static char uwp_warm_key[17]; /* link warm=: the game's ID for a shared shader warm-up list */
static char *uwp_system_folder(void);

/* The shared warm-up list for the game being started (games\xbox-system\warm\<ID>.bin), or NULL. */
char *xemu_uwp_warm_seed(void);
char *xemu_uwp_warm_seed(void)
{
    if (!uwp_warm_key[0]) {
        return NULL;
    }
    char *sys = uwp_system_folder();
    if (!sys) {
        return NULL;
    }
    char *path = g_strdup_printf("%swarm\\%s.bin", sys, uwp_warm_key);
    g_free(sys);
    return path;
}
static HANDLE uwp_share_handle;

void xemu_uwp_open_note(LPCWSTR name, const char *what)
{
    char *n = g_utf16_to_utf8(name, -1, NULL, NULL, NULL);
    uwp_log("open %s: %s", n ? n : "?", what);
    g_free(n);
}

HANDLE xemu_uwp_shared_file(LPCWSTR name, DWORD access)
{
    if (!uwp_share_token || !uwp_share_disc || (access & GENERIC_WRITE)) {
        return INVALID_HANDLE_VALUE;
    }
    char *n = g_utf16_to_utf8(name, -1, NULL, NULL, NULL);
    bool same = n && !g_ascii_strcasecmp(n, uwp_share_disc);
    if (!same && n) {
        /* the same path with the other kind of slashes */
        for (char *c = n; *c; c++) {
            *c = *c == '\\' ? '/' : *c;
        }
        same = !g_ascii_strcasecmp(n, uwp_share_disc);
    }
    g_free(n);
    if (!same) {
        return INVALID_HANDLE_VALUE;
    }
    if (!uwp_share_handle) {
        typedef void *(SDLCALL *open_fn)(const char *, bool);
        HMODULE sdl = GetModuleHandleA("SDL3.dll");
        open_fn open = sdl ? (open_fn)GetProcAddress(sdl, "SDL_WinRTOpenSharedFile") : NULL;
        uwp_log("disc: asking for Game Vault's share");
        uwp_share_handle = open ? (HANDLE)open(uwp_share_token, false) : NULL;
        uwp_log("disc: opened through Game Vault's share: %s", uwp_share_handle ? "yes" : open ? "no" : "no (old SDL)");
        if (!uwp_share_handle) {
            uwp_share_handle = INVALID_HANDLE_VALUE;
        }
    }
    HANDLE dup = INVALID_HANDLE_VALUE;
    if (uwp_share_handle != INVALID_HANDLE_VALUE &&
        !DuplicateHandle(GetCurrentProcess(), uwp_share_handle, GetCurrentProcess(), &dup, 0, FALSE,
                         DUPLICATE_SAME_ACCESS)) {
        dup = INVALID_HANDLE_VALUE;
    }
    return dup;
}

static char *uwp_take_activation_disc(void)
{
    const char *hint = SDL_GetHint(UWP_ACTIVATION_HINT);
    if (!hint || !hint[0]) {
        return NULL;
    }
    char *uri = g_strdup(hint);
    SDL_SetHint(UWP_ACTIVATION_HINT, "");

    char *disc = NULL, *share_token = NULL;
    bool fps = false;
    bool tests = uwp_test_links_allowed(), refused = false;
    char *query = strchr(uri, '?');
    for (char *p = query ? query + 1 : NULL; p && *p;) {
        char *end = strchr(p, '&');
        if (end) {
            *end = '\0';
        }
        if (!tests && (!strcmp(p, "quit=1") || !strncmp(p, "state=", 6) || !strncmp(p, "press=", 6) ||
                       !strncmp(p, "rtcbase=", 8) ||
                       !strcmp(p, "crashtest=1") || !strcmp(p, "dash=0") || !strcmp(p, "noflush=1") ||
                       !strcmp(p, "skipdraws=1"))) {
            refused = true;
            p = end ? end + 1 : NULL;
            continue;
        }
        if (!strcmp(p, "quit=1")) {
            uwp_quit("asked by link");
        }
        if (!strcmp(p, "state=1")) {
            uwp_load_state_wanted = true;
        }
        if (!strncmp(p, "press=", 6)) {
            uwp_press(p + 6);
        }
        if (!strcmp(p, "crashtest=1")) {
            /* checks the crash report: a deliberate bad memory read on the screen thread, 20 s from now */
            extern uint64_t uwp_crash_test_at;
            uwp_crash_test_at = SDL_GetTicks() + 20000;
            uwp_log("crash test: xemu will crash on purpose in 20 s");
        }
        if (!strcmp(p, "dash=0")) {
            uwp_no_dash = true;
        }
        if (!strcmp(p, "noflush=1")) {
            extern bool xemu_uwp_skip_cb_flush;
            xemu_uwp_skip_cb_flush = true;
            uwp_log("measuring: no TLB flush when a surface watch is added or removed");
        }
        if (!strcmp(p, "skipdraws=1")) {
            extern bool xemu_uwp_skip_draws;
            xemu_uwp_skip_draws = true;
            uwp_log("measuring: draws are skipped");
        }
        if (!strcmp(p, "state=save")) {
            uwp_save_state_wanted = true;
        }
        if (!strcmp(p, "fps=1")) {
            fps = true;
        }
        if (!strcmp(p, "autoscale=0")) {
            /* Game Vault, an Xbox game's details, LT: the player's render scale exactly, never lowered */
            extern bool xemu_uwp_auto_scale_off;
            xemu_uwp_auto_scale_off = true;
            uwp_log("link: render scale adjusting by itself is off (the player's choice)");
        }
        if (!strcmp(p, "widescreen=1") || !strcmp(p, "widescreen=0")) {
            /* the console's 16:9 setting for this session (Game Vault, Menu > Picture): games that
             * support widescreen then draw 16:9 and fill the TV (hw/xbox/smbus_storage.c) */
            extern int xemu_uwp_widescreen;
            xemu_uwp_widescreen = p[11] == '1';
            uwp_log("link: widescreen %s", xemu_uwp_widescreen ? "on" : "off");
        }
        if (!strncmp(p, "disctoken=", 10)) {
            /* letters, digits, '-' and braces only (a GUID-like token), at most 100 characters */
            const char *t = p + 10;
            size_t len = strlen(t);
            bool ok = len > 0 && len <= 100;
            for (const char *c = t; ok && *c; c++) {
                ok = g_ascii_isalnum(*c) || *c == '-' || *c == '{' || *c == '}';
            }
            if (ok) {
                g_free(share_token);
                share_token = g_strdup(t);
            } else {
                uwp_log("link: disc token refused");
            }
        }
        if (!strncmp(p, "rtcbase=", 8)) {
            /* testing: start the console's clock on this day (YYYY-MM-DD) instead of today (system/vl.c) */
            extern char *xemu_uwp_rtc_base;
            const char *v = p + 8;
            bool ok = strlen(v) == 10 && v[4] == '-' && v[7] == '-';
            for (int i = 0; ok && i < 10; i++) {
                ok = i == 4 || i == 7 || g_ascii_isdigit(v[i]);
            }
            if (ok) {
                g_free(xemu_uwp_rtc_base);
                xemu_uwp_rtc_base = g_strdup(v);
                uwp_log("link: clock starts on %s", v);
            }
        }
        if (!strncmp(p, "tid=", 4)) {
            /* the game's ID from Game Vault (two letters, '-', digits): its cache entry (see uwp_game_cache_check) */
            const char *v = p + 4;
            bool ok = strlen(v) >= 4 && strlen(v) < sizeof(uwp_title_key) && v[2] == '-' && g_ascii_isalpha(v[0]) &&
                      g_ascii_isalpha(v[1]);
            for (const char *c = v + 3; ok && *c; c++) {
                ok = g_ascii_isdigit(*c);
            }
            if (ok) {
                g_strlcpy(uwp_title_key, v, sizeof(uwp_title_key));
            }
        }
        if (!strcmp(p, "clearcache=1")) {
            uwp_clear_cache_wanted = true;
        }
        if (!strncmp(p, "warm=", 5)) {
            /* the game's ID (from Game Vault, e.g. VU-003): a shared shader warm-up list may lie in the
             * publisher's games\xbox-system\warm folder (gl/shaders.c uses it when this disc has no list yet) */
            const char *v = p + 5;
            size_t len = strlen(v);
            bool ok = len > 0 && len <= 16;
            for (const char *c = v; ok && *c; c++) {
                ok = g_ascii_isalnum(*c) || *c == '-';
            }
            if (ok) {
                g_strlcpy(uwp_warm_key, v, sizeof(uwp_warm_key));
            }
        }
        if (!strncmp(p, "fit=", 4)) {
            /* how the picture fills the TV, chosen in Game Vault (Menu > Picture): for this session only */
            const char *v = p + 4;
            int fit = !strcmp(v, "stretch") ? CONFIG_DISPLAY_UI_FIT_STRETCH
                      : !strcmp(v, "scale")  ? CONFIG_DISPLAY_UI_FIT_SCALE
                      : !strcmp(v, "center") ? CONFIG_DISPLAY_UI_FIT_CENTER : -1;
            if (fit >= 0) {
                g_config.display.ui.fit = fit;
                uwp_log("link: picture %s", v);
            } else {
                uwp_log("link: picture setting refused");
            }
        }
        if (!strncmp(p, "home=", 5)) {
            /* saves are moved into it: only "<drive>:/Game Vault", the folder Game Vault makes */
            char *home = uwp_unescape(p + 5);
            if (home && g_ascii_isalpha(home[0]) && !strcmp(home + 1, ":/Game Vault")) {
                g_free(uwp_home);
                uwp_home = home;
            } else {
                uwp_log("link: home folder refused");
                g_free(home);
            }
        }
        if (!strncmp(p, "netcred=", 8)) {
            /* Game Vault's sign-in for a network disc (block/uwp-smb.c). Any app can open an xemu link, so only a
               "netcred-<id>.txt" in the shared Publishers folder is taken (it is deleted once read) */
            char *path = g_uri_unescape_string(p + 8, NULL);
            const char *base = path ? MAX(strrchr(path, '\\'), strrchr(path, '/')) : NULL;
            bool ok = base && g_str_has_prefix(base + 1, "netcred-") && g_str_has_suffix(base, ".txt") &&
                      !strstr(path, "..") && (strstr(path, "\\Publishers\\") || strstr(path, "/Publishers/"));
            if (ok) {
                extern void xemu_uwp_smb_set_credentials_file(const char *path);
                xemu_uwp_smb_set_credentials_file(path);
            } else {
                uwp_log("link: network sign-in refused (not Game Vault's file)");
            }
            g_free(path);
        }
        if (!strncmp(p, "disc=", 5)) {
            /* undo the %XX escaping; xemu.toml uses forward slashes */
            disc = g_malloc(strlen(p) + 1);
            char *out = disc;
            for (const char *in = p + 5; *in; in++) {
                if (in[0] == '%' && g_ascii_isxdigit(in[1]) &&
                    g_ascii_isxdigit(in[2])) {
                    *out = (char)(g_ascii_xdigit_value(in[1]) * 16 +
                                  g_ascii_xdigit_value(in[2]));
                    in += 2;
                } else {
                    *out = *in;
                }
                if (*out == '\\') {
                    *out = '/';
                }
                out++;
            }
            *out = '\0';
        }
        p = end ? end + 1 : NULL;
    }
    g_free(uri);
    if (refused) {
        uwp_log("link: test options ignored (no test-links.txt)");
    }
    if (disc) {
        xemu_uwp_overlay_on = fps;
    }
    if (disc && !disc[0]) {
        g_free(disc);
        disc = NULL;
    }
    if (disc) {
        /* a new disc: a token from an earlier link no longer applies */
        g_free(uwp_share_token);
        g_free(uwp_share_disc);
        if (uwp_share_handle && uwp_share_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(uwp_share_handle);
        }
        uwp_share_handle = NULL;
        uwp_share_token = share_token;
        uwp_share_disc = share_token ? g_strdup(disc) : NULL;
        share_token = NULL;
    }
    g_free(share_token);
    return disc;
}

/* Vulkan test switch, read at start (vk-zwrite-off.txt next to xemu.toml), used by pgraph/vk/draw.c */
bool xemu_uwp_vk_zwrite_off;
/* Vulkan test switch (vk-pal-white.txt): palette textures drawn solid white, used by pgraph/vk/texture.c */
bool xemu_uwp_vk_pal_white;
/* Vulkan test switch (vk-pal-opaque.txt): palette textures without transparency, used by pgraph/vk/texture.c */
bool xemu_uwp_vk_pal_opaque;
int xemu_uwp_vk_tex_test; /* vk-tex-test.txt words: nomip (1), noaniso (2), nearest (4) */

/* Before the machine is created: boot with the disc the launcher asked for. */
/*
 * Setting itself up. The launcher (Game Vault, same publisher) puts the boot
 * ROM, the BIOS and the hard disk image it found on the user's USB drive into
 * the publisher's shared folder "games\xbox-system". A path from xemu.toml
 * wins when its file exists (someone who set things up by hand); otherwise
 * the file from that folder, or one lying next to xemu.toml, is used, so no
 * xemu.toml has to be written or uploaded at all.
 */
static bool uwp_file_exists(const char *path)
{
    if (!path || !path[0]) {
        return false;
    }
    wchar_t *w = g_utf8_to_utf16(path, -1, NULL, NULL, NULL);
    HANDLE h = w ? xemu_uwp_create_file_w(w, GENERIC_READ,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE,
                                          OPEN_EXISTING, 0)
                 : INVALID_HANDLE_VALUE;
    g_free(w);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    CloseHandle(h);
    return true;
}

/* ...\Local\Packages\<name>_<publisher id>\LocalState\... -> ...\Local\Publishers\<publisher id>\games\xbox-system\ */
static char *uwp_system_folder(void)
{
    char *base = SDL_GetPrefPath("xemu", "xemu");
    char *result = NULL;
    for (char *c = base; c && *c; c++) {
        if (*c == '/') {
            *c = '\\';
        }
    }
    char *packages = base ? strstr(base, "\\Packages\\") : NULL;
    if (packages) {
        char *name = packages + strlen("\\Packages\\");
        char *end = strchr(name, '\\');
        char *id = NULL;
        for (char *c = name; end && c < end; c++) {
            if (*c == '_') {
                id = c + 1;
            }
        }
        if (id) {
            result = g_strdup_printf("%.*s\\Publishers\\%.*s\\games\\xbox-system\\",
                                     (int)(packages - base), base,
                                     (int)(end - id), id);
        }
    }
    SDL_free(base);
    return result;
}

/*
 * The log also goes to the publisher's shared folder as games\logs\<disc name>.log, written as it
 * goes (so a crash leaves it complete). Game Vault copies these to "Game Vault\logs" on the USB drive,
 * where a player can pick one up to send with a report.
 */
static void uwp_log_share(const char *disc)
{
    char *sys = uwp_system_folder();
    if (!sys || uwp_log_copy || !uwp_log_file) {
        g_free(sys);
        return;
    }
    char *cut = strstr(sys, "xbox-system\\");
    if (cut) {
        *cut = '\0';
    }
    char *dir = g_strdup_printf("%slogs", sys);
    g_mkdir_with_parents(dir, 0755);
    const char *base = disc;
    for (const char *c = disc; *c; c++) {
        if (*c == '/' || *c == '\\') {
            base = c + 1;
        }
    }
    char *name = g_strdup(base);
    char *dot = strrchr(name, '.');
    if (dot) {
        *dot = '\0';
    }
    char *path = g_strdup_printf("%s\\%s.log", dir, name);
    FILE *copy = fopen(path, "w");
    if (copy) {
        /* what was logged before the disc was known */
        char *own = SDL_GetPrefPath("xemu", "xemu");
        char *log = g_strdup_printf("%suwp_debug.log", own ? own : "");
        gchar *text = NULL;
        gsize len = 0;
        fflush(uwp_log_file);
        if (g_file_get_contents(log, &text, &len, NULL)) {
            fwrite(text, 1, len, copy);
        }
        g_free(text);
        g_free(log);
        SDL_free(own);
        uwp_log_copy = copy;
        uwp_shots_start(path);
    }
    g_free(path);
    g_free(name);
    g_free(dir);
    g_free(sys);
}

static void uwp_set_path(const char **setting, const char *what, char *path)
{
    for (char *c = path; *c; c++) {
        if (*c == '\\') {
            *c = '/';
        }
    }
    uwp_log("%s: %s", what, path);
    xemu_settings_set_string(setting, path);
}

/* Keeps the setting when its file exists, else looks for the names in the two folders. */
static bool uwp_fill_one(const char **setting, const char *what,
                         const char *folder, const char *own,
                         const char *const *names)
{
    if (uwp_file_exists(*setting)) {
        return true;
    }
    const char *dirs[] = { folder, own };
    for (int d = 0; d < 2; d++) {
        for (int i = 0; dirs[d] && names[i]; i++) {
            char *path = g_strdup_printf("%s%s", dirs[d], names[i]);
            if (uwp_file_exists(path)) {
                uwp_set_path(setting, what, path);
                g_free(path);
                return true;
            }
            g_free(path);
        }
    }
    return false;
}

/*
 * A new, empty hard disk from the copy of the xemu project's copyright-free blank image
 * (github.com/xemu-project/xemu-hdd-image, release 1.0) that comes with the package. Made in
 * the publisher's folder when possible, so it survives reinstalling xemu.
 */
static bool uwp_new_hdd(const char *folder, const char *own, const char *name)
{
    char *blank = g_strdup_printf("%sxbox_hdd_blank.qcow2", SDL_GetBasePath());
    gchar *data = NULL;
    gsize size = 0;
    bool ok = false;
    if (!g_file_get_contents(blank, &data, &size, NULL) || size < 4 ||
        memcmp(data, "QFI\xfb", 4)) {
        uwp_log("hard disk: the blank image in the package is missing or damaged (%s)", blank);
    } else {
        const char *dirs[] = { folder, own };
        for (int d = 0; d < 2 && !ok; d++) {
            if (!dirs[d]) {
                continue;
            }
            g_mkdir_with_parents(dirs[d], 0755);
            char *path = g_strdup_printf("%s%s", dirs[d], name);
            GError *err = NULL;
            if (g_file_set_contents(path, data, size, &err)) {
                uwp_log("hard disk: none yet, made a new blank one");
                uwp_set_path(&g_config.sys.files.hdd_path, "hard disk", path);
                ok = true;
            } else {
                uwp_log("hard disk: could not make %s: %s", path, err->message);
                g_error_free(err);
            }
            g_free(path);
        }
    }
    g_free(data);
    g_free(blank);
    return ok;
}

/*
 * The free SpiritBoot BIOS shipped in the package (GPL-2.0, github.com/Mainkill1/SpiritBoot,
 * release v1.1.0, see spiritboot\README.txt next to xemu.exe). It boots without the MCPX boot
 * ROM. It cannot do Xbox Live / Insignia (no Microsoft keys), and its saves do not work with a
 * Microsoft BIOS and the other way round, which is why it gets a hard disk of its own.
 */
static bool uwp_use_free_bios(void)
{
    bool big = g_config.sys.mem_limit == CONFIG_SYS_MEM_LIMIT_128;
    char *path = g_strdup_printf("%sspiritboot\\SpiritBoot-v1.1.0-%s.bin",
                                 SDL_GetBasePath(), big ? "128MB" : "64MB");
    bool ok = uwp_file_exists(path);
    if (ok) {
        uwp_set_path(&g_config.sys.files.flashrom_path, "BIOS (the free SpiritBoot)", path);
        if (!uwp_file_exists(g_config.sys.files.bootrom_path)) {
            xemu_settings_set_string(&g_config.sys.files.bootrom_path, "");
        }
    } else {
        uwp_log("BIOS: the free SpiritBoot is missing from the package (%s)", path);
    }
    g_free(path);
    return ok;
}

/*
 * The hard disk and the games USB drive. Game Vault sends its folder on that drive as "home" with
 * every game and keeps a copy of the console's hard disk in <home>\xbox (exFAT / NTFS drives),
 * brought up to date after playing. xemu always plays from the console's disk, which is fast; only
 * a console with no disk at all (new, or reset) plays from the stick's copy until Game Vault has
 * put it back on the console. Each console disk has an ID (a random one in "<disk>.id" next to
 * it), which Game Vault copies along, so the stick's copy is always matched to the right disk.
 * The EEPROM (the Xbox's own keys) stays on the console: xemu opens it with calls a FAT32 / exFAT
 * stick refuses. Game Vault can only reach the publisher's folder, not xemu's own storage, so a
 * disk set up by hand in xemu's own folder (xemu.toml) is first moved over there (same drive, so a
 * rename), and the EEPROM with it, so Game Vault can keep a copy of it on the stick.
 */
static bool uwp_under(const char *path, const char *dir)
{
    if (!path || !dir) {
        return false;
    }
    for (; *dir; path++, dir++) {
        char a = *path == '/' ? '\\' : g_ascii_tolower(*path);
        char b = *dir == '/' ? '\\' : g_ascii_tolower(*dir);
        if (a != b) {
            return false;
        }
    }
    return true;
}

/* the file name of a path with either kind of slash */
static const char *uwp_base_name(const char *path)
{
    const char *base = path;
    for (const char *c = path; *c; c++) {
        if (*c == '/' || *c == '\\') {
            base = c + 1;
        }
    }
    return base;
}

static wchar_t *uwp_wide_path(const char *path)
{
    wchar_t *w = g_utf8_to_utf16(path, -1, NULL, NULL, NULL);
    for (wchar_t *c = w; c && *c; c++) {
        if (*c == L'/') {
            *c = L'\\';
        }
    }
    return w;
}

static bool uwp_move_file(const char *from, const char *to)
{
    typedef BOOL (WINAPI *move_fn)(LPCWSTR, LPCWSTR);
    move_fn move = (move_fn)xemu_uwp_kernelbase_proc("MoveFileFromAppW");
    wchar_t *wf = uwp_wide_path(from), *wt = uwp_wide_path(to);
    bool ok = wf && wt && move && move(wf, wt);
    g_free(wf);
    g_free(wt);
    return ok;
}

/* A small file read through the file broker (on a FAT32 / exFAT stick the C library is refused). */
static char *uwp_read_small(const char *path)
{
    wchar_t *w = uwp_wide_path(path);
    HANDLE h = w ? xemu_uwp_create_file_w(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                          OPEN_EXISTING, 0)
                 : INVALID_HANDLE_VALUE;
    g_free(w);
    if (h == INVALID_HANDLE_VALUE) {
        return NULL;
    }
    char buf[128];
    DWORD got = 0;
    BOOL ok = ReadFile(h, buf, sizeof(buf) - 1, &got, NULL);
    CloseHandle(h);
    if (!ok || !got) {
        return NULL;
    }
    buf[got] = '\0';
    return g_strstrip(g_strdup(buf));
}

/* The ID of a console disk, made the first time. */
static char *uwp_disk_id(const char *disk)
{
    char *file = g_strdup_printf("%s.id", disk);
    char *id = uwp_read_small(file);
    if (!id) {
        id = g_uuid_string_random();
        if (!g_file_set_contents(file, id, -1, NULL)) {
            uwp_log("hard disk: could not write its ID (%s)", file);
        }
    }
    g_free(file);
    return id;
}

/* Into the publisher's folder; a file of that name already there (a spare disk the setup made but
 * xemu.toml did not use) is kept as "<name>.unused". */
static void uwp_move_to_folder(const char **setting, const char *what, const char *folder,
                               const char *own, const char *name)
{
    if (!uwp_under(*setting, own) || !uwp_file_exists(*setting)) {
        return;
    }
    g_mkdir_with_parents(folder, 0755);
    char *to = g_strdup_printf("%s%s", folder, name);
    if (uwp_file_exists(to)) {
        char *spare = g_strdup_printf("%s.unused", to);
        for (int n = 2; uwp_file_exists(spare); n++) {
            g_free(spare);
            spare = g_strdup_printf("%s.unused%d", to, n);
        }
        char *id = g_strdup_printf("%s.id", to), *spare_id = g_strdup_printf("%s.id", spare);
        uwp_move_file(to, spare);
        uwp_move_file(id, spare_id);
        uwp_log("%s: the one in the publisher's folder was not in use, kept as %s", what, spare);
        g_free(id);
        g_free(spare_id);
        g_free(spare);
    }
    char *from = g_strdup(*setting);
    if (uwp_move_file(from, to)) {
        uwp_log("%s moved to the publisher's folder, where Game Vault can copy it to the USB drive", what);
        uwp_set_path(setting, what, to);
    } else {
        uwp_log("%s: could not move it to %s (error %lu)", what, to, GetLastError());
    }
    g_free(from);
    g_free(to);
}

/*
 * A file moved over from xemu's own storage keeps permissions only xemu has, so Game Vault cannot
 * read it. A fresh copy gets the publisher's folder's own permissions, which both apps share; done
 * once per file ("<file>.shared" marks it).
 */
static void uwp_share_file(const char *path)
{
    char *mark = g_strdup_printf("%s.shared", path);
    if (!uwp_file_exists(path) || uwp_file_exists(mark)) {
        g_free(mark);
        return;
    }
    typedef BOOL (WINAPI *copy_fn)(LPCWSTR, LPCWSTR, BOOL);
    typedef BOOL (WINAPI *delete_fn)(LPCWSTR);
    copy_fn copy = (copy_fn)xemu_uwp_kernelbase_proc("CopyFileFromAppW");
    delete_fn del = (delete_fn)xemu_uwp_kernelbase_proc("DeleteFileFromAppW");
    char *tmp = g_strdup_printf("%s.copy", path);
    wchar_t *wp = uwp_wide_path(path), *wt = uwp_wide_path(tmp);
    if (copy && del && copy(wp, wt, FALSE)) {
        if (del(wp) && uwp_move_file(tmp, path)) {
            g_file_set_contents(mark, "", 0, NULL);
            uwp_log("%s: copied once so that Game Vault may read it", path);
        } else {
            uwp_log("%s: could not replace it with its copy (error %lu)", path, GetLastError());
        }
    } else {
        uwp_log("%s: could not copy it (error %lu)", path, GetLastError());
    }
    g_free(wp);
    g_free(wt);
    g_free(tmp);
    g_free(mark);
}

static void uwp_home_disk(const char *folder, const char *own)
{
    if (uwp_under(g_config.sys.files.hdd_path, uwp_home)) {
        return;   /* no disk on the console at all: the stick's is in use already */
    }
    if (folder && own) {
        char *base = g_strdup(uwp_base_name(g_config.sys.files.hdd_path));
        uwp_move_to_folder(&g_config.sys.files.hdd_path, "hard disk", folder, own, base);
        uwp_move_to_folder(&g_config.sys.files.eeprom_path, "EEPROM", folder, own, "eeprom.bin");
        g_free(base);
    }
    if (!folder || !uwp_under(g_config.sys.files.hdd_path, folder)) {
        uwp_log("hard disk: set up by hand outside xemu's folders, so it stays where it is");
        return;
    }
    uwp_share_file(g_config.sys.files.hdd_path);
    if (uwp_under(g_config.sys.files.eeprom_path, folder)) {
        uwp_share_file(g_config.sys.files.eeprom_path);
    }
    /* played from the console; Game Vault keeps the stick's copy matched to it by this ID */
    char *id = uwp_disk_id(g_config.sys.files.hdd_path);
    uwp_log("hard disk ID: %s", id);
    g_free(id);
}

/*
 * Which BIOS and hard disk to use, with nothing for the player to do:
 *  - the player's own files (from xemu.toml, or what Game Vault found on their USB drive and
 *    put in the publisher's folder): their BIOS with the boot ROM, their hard disk;
 *  - otherwise the free BIOS with its own hard disk, xbox_hdd_free.qcow2;
 *  - a hard disk that does not exist yet is made from the blank image.
 * xemu.toml can choose the free BIOS on purpose: flashrom_path = 'spiritboot'.
 */
static void uwp_fill_system_files(void)
{
    static const char *const mcpx[] = { "mcpx_1.0.bin", "mcpx.bin", NULL };
    static const char *const bios[] = { "bios.bin", "Complex_4627.bin", NULL };
    static const char *const own_hdd[] = { "xbox_hdd.qcow2", NULL };
    static const char *const free_hdd[] = { "xbox_hdd_free.qcow2", NULL };
    char *folder = uwp_system_folder();
    char *own = SDL_GetPrefPath("xemu", "xemu");

    uwp_toml_bootrom = g_strdup(g_config.sys.files.bootrom_path ? g_config.sys.files.bootrom_path : "");
    uwp_toml_bios = g_strdup(g_config.sys.files.flashrom_path ? g_config.sys.files.flashrom_path : "");
    uwp_toml_hdd = g_strdup(g_config.sys.files.hdd_path ? g_config.sys.files.hdd_path : "");
    uwp_toml_eeprom = g_strdup(g_config.sys.files.eeprom_path ? g_config.sys.files.eeprom_path : "");
    uwp_toml_remembered = true;

    bool asked_free = !g_ascii_strcasecmp(uwp_toml_bios, "spiritboot");
    bool toml_bios = uwp_file_exists(uwp_toml_bios);
    bool have_mcpx = uwp_fill_one(&g_config.sys.files.bootrom_path, "boot ROM", folder, own, mcpx);
    bool own_bios = !asked_free &&
                    uwp_fill_one(&g_config.sys.files.flashrom_path, "BIOS", folder, own, bios);
    if (own_bios && !have_mcpx && !toml_bios) {
        /* a Microsoft BIOS does not start without the boot ROM */
        uwp_log("BIOS: found one, but no boot ROM (MCPX) to start it with");
        own_bios = false;
    }
    bool free_bios = !own_bios && uwp_use_free_bios();
    if (!own_bios && !free_bios) {
        uwp_log("BIOS: none (run the Xbox setup in Game Vault)");
    }
    uwp_log("Xbox BIOS in use: %s", own_bios ? "the player's own" : free_bios ? "free (offline only)" : "none");

    if (!uwp_file_exists(uwp_toml_hdd)) {
        const char *const *names = free_bios ? free_hdd : own_hdd;
        xemu_settings_set_string(&g_config.sys.files.hdd_path, "");
        char *usb = uwp_home ? g_strdup_printf("%s/xbox/%s", uwp_home, names[0]) : NULL;
        if (uwp_fill_one(&g_config.sys.files.hdd_path, "hard disk", folder, own, names)) {
            /* the console's own */
        } else if (usb && uwp_file_exists(usb)) {
            /* a console without a disk (new, or reset) and the player's stick: the stick's disk */
            uwp_set_path(&g_config.sys.files.hdd_path, "hard disk (on the USB drive, none on the console)", usb);
        } else {
            uwp_new_hdd(folder, own, names[0]);
        }
        g_free(usb);
    }
    if (!uwp_file_exists(g_config.sys.files.eeprom_path)) {
        static const char *const eeprom[] = { "eeprom.bin", NULL };
        xemu_settings_set_string(&g_config.sys.files.eeprom_path, "");
        uwp_fill_one(&g_config.sys.files.eeprom_path, "EEPROM", folder, own, eeprom);
    }
    if (uwp_home) {
        uwp_home_disk(folder, own);
    }
    g_free(folder);
    SDL_free(own);

    /* what a console needs, whatever the settings file says (or if there is none) */
    g_config.general.show_welcome = false;
    g_config.general.updates.check = false;
    /* OpenGL, except on a console with use-vulkan.txt next to xemu.toml: the Vulkan renderer through Mesa's
     * Vulkan-on-D3D12 driver (pgraph/vk/instance.c), under test - it keeps a shader cache across sessions */
    {
        gchar *dir = g_path_get_dirname(xemu_settings_get_path());
        gchar *marker = g_build_filename(dir, "use-vulkan.txt", NULL);
        bool vulkan = uwp_file_exists(marker);
        g_config.display.renderer = vulkan ? CONFIG_DISPLAY_RENDERER_VULKAN : CONFIG_DISPLAY_RENDERER_OPENGL;
        if (vulkan) {
            uwp_log("renderer: Vulkan (use-vulkan.txt)");
        }
        g_free(marker);
        marker = g_build_filename(dir, "display-upstream.txt", NULL);
        {
            extern bool xemu_uwp_display_upstream; /* pgraph/gl/display.c */
            xemu_uwp_display_upstream = uwp_file_exists(marker);
            if (xemu_uwp_display_upstream) {
                uwp_log("display test: upstream's surface choice, no stale-surface rule (display-upstream.txt)");
            }
        }
        g_free(marker);
        marker = g_build_filename(dir, "display-cpu-wait.txt", NULL);
        {
            extern bool xemu_uwp_display_cpu_wait; /* pgraph/gl/display.c */
            xemu_uwp_display_cpu_wait = uwp_file_exists(marker);
            if (xemu_uwp_display_cpu_wait) {
                uwp_log("display test: upstream's CPU waits for each shown frame (display-cpu-wait.txt)");
            }
        }
        g_free(marker);
        marker = g_build_filename(dir, "vk-zwrite-off.txt", NULL);
        xemu_uwp_vk_zwrite_off = uwp_file_exists(marker);
        if (xemu_uwp_vk_zwrite_off) {
            uwp_log("Vulkan test: no depth writes while the depth test is off (vk-zwrite-off.txt)");
        }
        g_free(marker);
        marker = g_build_filename(dir, "vk-pal-white.txt", NULL);
        xemu_uwp_vk_pal_white = uwp_file_exists(marker);
        if (xemu_uwp_vk_pal_white) {
            uwp_log("Vulkan test: palette textures solid white (vk-pal-white.txt)");
        }
        g_free(marker);
        marker = g_build_filename(dir, "vk-pal-opaque.txt", NULL);
        xemu_uwp_vk_pal_opaque = uwp_file_exists(marker);
        if (xemu_uwp_vk_pal_opaque) {
            uwp_log("Vulkan test: palette textures without transparency (vk-pal-opaque.txt)");
        }
        g_free(marker);
        marker = g_build_filename(dir, "vk-tex-test.txt", NULL);
        {
            gchar *words = NULL;
            if (g_file_get_contents(marker, &words, NULL, NULL)) {
                xemu_uwp_vk_tex_test = (strstr(words, "nomip") ? 1 : 0) | (strstr(words, "noaniso") ? 2 : 0) |
                                       (strstr(words, "nearest") ? 4 : 0);
                uwp_log("Vulkan test: texture sampling %s(vk-tex-test.txt)", words);
                g_free(words);
            }
        }
        g_free(marker);
        g_free(dir);
    }
    g_config.display.window.fullscreen_on_startup = true;
    /* Network on, through xemu's own NAT: what an online service such as Insignia needs, and the
     * only backend that works in an app container besides the UDP tunnel (no packet capture). */
    if (g_config.net.backend == CONFIG_NET_BACKEND_NAT) {
        g_config.net.enable = true;
    }
}

static void uwp_startup_disc(void)
{
    /* the activation arrives through the window's event queue */
    for (int i = 0; i < 200 && !SDL_GetHint(UWP_ACTIVATION_HINT); i++) {
        SDL_PumpEvents();
        SDL_Delay(10);
    }
    char *disc = uwp_take_activation_disc();
    if (disc) {
        uwp_log("disc from launcher: %s", disc);
        uwp_log_share(disc);
        xemu_settings_set_string(&g_config.sys.files.dvd_path, disc);
        g_free(disc);
    }
    uwp_fill_system_files();
}

/*
 * While running (the app was left open and the launcher starts a game): the
 * same disc just continues; another disc is swapped in and the console reset.
 */
static void uwp_poll_activation(void)
{
    char *disc = uwp_take_activation_disc();
    if (!disc) {
        return;
    }
    const char *current = g_config.sys.files.dvd_path;
    if (current && !g_ascii_strcasecmp(current, disc) && !uwp_watch_no_picture()) {
        uwp_log("launcher asked for the disc that is already running");
    } else if (current && !g_ascii_strcasecmp(current, disc)) {
        /* the same game picked again while it never showed anything: start it again, not the stuck session */
        uwp_event("the game was picked again and had shown nothing yet: restarting it");
        xemu_main_loop_lock();
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
        xemu_main_loop_unlock();
        uwp_watch_start();
    } else {
        Error *err = NULL;
        xemu_main_loop_lock();
        xemu_load_disc(disc, &err);
        if (err) {
            uwp_log("could not load %s: %s", disc, error_get_pretty(err));
            error_free(err);
        } else {
            uwp_log("disc changed by launcher: %s", disc);
            qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
            uwp_watch_start();
        }
        xemu_main_loop_unlock();
    }
    g_free(disc);
}
#endif

static void *qemu_main(void *opaque)
{
#ifdef XEMU_UWP
    uwp_log("qemu_init start");
#endif
    qemu_init(gArgc, gArgv);
#ifdef XEMU_UWP
    uwp_game_cache_check();
#endif
#ifdef XEMU_UWP
    uwp_log("qemu_init done, entering main loop");
#endif
    exit_status = qemu_main_loop();
    qatomic_set(&qemu_exiting, true);
    bql_unlock();
    qemu_mutex_unlock_main_loop();

    qemu_sem_wait(&display_shutdown_sem);
    bql_lock();
    qemu_cleanup(exit_status);
    bql_unlock();

    return NULL;
}

#ifdef _WIN32
static const wchar_t *get_executable_name(void)
{
    static wchar_t exe_name[MAX_PATH] = { 0 };
    static bool initialized = false;

    if (!initialized) {
        wchar_t full_path[MAX_PATH];
        DWORD length = GetModuleFileNameW(NULL, full_path, MAX_PATH);
        if (length == 0 || length == MAX_PATH) {
            return NULL;
        }

        wchar_t *last_slash = wcsrchr(full_path, L'\\');
        if (last_slash) {
            wcsncpy_s(exe_name, MAX_PATH, last_slash + 1, _TRUNCATE);
        } else {
            wcsncpy_s(exe_name, MAX_PATH, full_path, _TRUNCATE);
        }

        initialized = true;
    }

    return exe_name;
}

static void setup_nvidia_profile(void)
{
    const wchar_t *exe_name = get_executable_name();
    if (exe_name == NULL) {
        fprintf(stderr, "Failed to get current executable name\n");
        return;
    }

    if (nvapi_init()) {
        nvapi_setup_profile((NvApiProfileOpts){
            .profile_name = L"xemu",
            .executable_name = exe_name,
            .threaded_optimization = false,
            .present_method = OGL_CPL_PREFER_DXPRESENT_PREFER_DISABLED,
        });
        nvapi_finalize();
    }
}
#endif

static void init_sdl_app_metadata(void)
{
    SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_NAME_STRING, "xemu");
    SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_VERSION_STRING,
                               xemu_version);
    SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_IDENTIFIER_STRING,
                               "app.xemu.xemu");
    SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_URL_STRING,
                               "https://xemu.app");
}

#ifdef XEMU_UWP
/*
 * Xbox (UWP) build: SDL's WinRT backend owns the app's CoreWindow, so the
 * program starts in WinMain and SDL_RunApp calls xemu's main on the UI thread.
 */
#define SDL_MAIN_HANDLED
#include <SDL3/SDL_main.h>

static int xemu_uwp_main(int argc, char **argv);

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show)
{
    static char *uwp_argv[] = { (char *)"xemu.exe", NULL };

    /* Windows.Gaming.Input is the only controller API that works in Xbox apps */
    SDL_SetHint(SDL_HINT_JOYSTICK_WGI, "1");
    return SDL_RunApp(1, uwp_argv, xemu_uwp_main, NULL);
}

static int xemu_uwp_main(int argc, char **argv)
#else
int main(int argc, char **argv)
#endif
{
    QemuThread thread;

    setlocale(LC_NUMERIC, "C");

#ifdef XEMU_UWP
    {
        /* The install folder is read-only: log next to xemu.toml in LocalState */
        init_sdl_app_metadata();
        char *base = SDL_GetPrefPath("xemu", "xemu");
        if (base) {
            char *log_path = g_strdup_printf("%sxemu.log", base);
            /* append mode for both streams so they can't overwrite each other */
            remove(log_path);
            freopen(log_path, "a", stdout);
            freopen(log_path, "a", stderr);
            setvbuf(stdout, NULL, _IONBF, 0);
            setvbuf(stderr, NULL, _IONBF, 0);
            uwp_err_start(log_path);
            g_free(log_path);
            /*
             * Mesa's shader cache (a Mesa build with the D3D12 DXIL / pipeline disk cache): shaders built in
             * one session load from disk in the next instead of pausing the game. Set before the GL driver
             * loads; both the C runtime's and Windows' copies of the environment.
             */
            char *cache = g_strdup_printf("%smesa_shader_cache", base);
            g_mkdir_with_parents(cache, 0755);
            _putenv_s("MESA_SHADER_CACHE_DIR", cache);
            SetEnvironmentVariableA("MESA_SHADER_CACHE_DIR", cache);
            g_free(cache);
            SDL_free(base);
        }
    }
#elif defined(_WIN32)
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        // Launched with a console. If stdout and stderr are not associated with
        // an output stream, redirect to parent console.
        if (_fileno(stdout) == -2) {
            freopen("CONOUT$", "w+", stdout);
        }
        if (_fileno(stderr) == -2) {
            freopen("CONOUT$", "w+", stderr);
        }
    } else {
        // Launched without a console. Redirect stdout and stderr to a log file.
        HANDLE logfile = CreateFileA("xemu.log",
            GENERIC_WRITE, FILE_SHARE_WRITE|FILE_SHARE_READ,
            NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (logfile != INVALID_HANDLE_VALUE) {
            freopen("xemu.log", "a", stdout);
            freopen("xemu.log", "a", stderr);
        }
    }

    _set_error_mode(_OUT_TO_STDERR);
#endif

    fprintf(stderr, "xemu_version: %s\n", xemu_version);
    fprintf(stderr, "xemu_commit: %s\n", xemu_commit);
    fprintf(stderr, "xemu_date: %s\n", xemu_date);

    init_sdl_app_metadata();

    gArgc = argc;
    gArgv = argv;

    for (int i = 1; i < argc; i++) {
        if (argv[i] && strcmp(argv[i], "-config_path") == 0) {
            argv[i] = NULL;
            if (i < argc - 1 && argv[i+1]) {
                xemu_settings_set_path(argv[i+1]);
                argv[i+1] = NULL;
            }
            break;
        }
    }

    if (!xemu_settings_load()) {
        const char *err_msg = xemu_settings_get_error_message();
        fprintf(stderr, "%s", err_msg);
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,
            "Failed to load xemu config file", err_msg,
            m_window);
        SDL_Quit();
        exit(1);
    }
#ifdef XEMU_UWP
    atexit(uwp_save_settings);
#else
    atexit(xemu_settings_save);
#endif

#ifdef _WIN32
    if (g_config.display.setup_nvidia_profile) {
        setup_nvidia_profile();
    }
#endif

    display_very_early_init(NULL);
#ifdef XEMU_UWP
    uwp_log("display_very_early_init done");
    /*
     * From here on a failure is logged: an error that ends xemu while it starts (exit, abort, crash) used to
     * leave nothing but "qemu_init start" (games on a 1 TB drive, 2026-10-16).
     */
    signal(SIGABRT, uwp_on_abort);
    uwp_crash_guard_start();
    SetUnhandledExceptionFilter(uwp_on_crash);
    atexit(uwp_on_early_exit);
    uwp_startup_disc();
#endif

#ifdef XEMU_UWP
    /* List disc images on the drives (USB sticks show up as E:, F:, ...) */
    {
        DWORD mask = GetLogicalDrives();
        for (char d = 'D'; d <= 'Z'; d++) {
            if (!(mask & (1u << (d - 'A')))) {
                continue;
            }
            const char *patterns[] = { "%c:\\*.iso", "%c:\\*\\*.iso" };
            for (int p = 0; p < 2; p++) {
                char pat[32];
                snprintf(pat, sizeof(pat), patterns[p], d);
                if (p == 1) {
                    /* one folder level down */
                    char root[8];
                    snprintf(root, sizeof(root), "%c:\\*", d);
                    WIN32_FIND_DATAA dir;
                    HANDLE hd = FindFirstFileA(root, &dir);
                    if (hd == INVALID_HANDLE_VALUE) {
                        continue;
                    }
                    do {
                        if (!(dir.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
                            dir.cFileName[0] == '.') {
                            continue;
                        }
                        char sub[MAX_PATH];
                        snprintf(sub, sizeof(sub), "%c:\\%s\\*.iso", d, dir.cFileName);
                        WIN32_FIND_DATAA f;
                        HANDLE hf = FindFirstFileA(sub, &f);
                        if (hf != INVALID_HANDLE_VALUE) {
                            do {
                                uwp_log("iso: %c:\\%s\\%s (%llu MB)", d, dir.cFileName, f.cFileName,
                                        (((unsigned long long)f.nFileSizeHigh << 32) | f.nFileSizeLow) >> 20);
                            } while (FindNextFileA(hf, &f));
                            FindClose(hf);
                        }
                    } while (FindNextFileA(hd, &dir));
                    FindClose(hd);
                    continue;
                }
                WIN32_FIND_DATAA f;
                HANDLE hf = FindFirstFileA(pat, &f);
                if (hf != INVALID_HANDLE_VALUE) {
                    do {
                        uwp_log("iso: %c:\\%s (%llu MB)", d, f.cFileName,
                                (((unsigned long long)f.nFileSizeHigh << 32) | f.nFileSizeLow) >> 20);
                    } while (FindNextFileA(hf, &f));
                    FindClose(hf);
                }
            }
        }
    }

    /*
     * Audio: SDL3-uwp's WASAPI device open never completes on the Xbox, so
     * the APU's stream is played through XAudio2 instead (xemu-uwp-audio.c).
     * The QEMU thread picks the stream up in mcpx_apu_monitor_init().
     */
    xemu_uwp_audio_stream = xemu_uwp_audio_start();
#endif
    qemu_sem_init(&display_init_sem, 0);
    qemu_sem_init(&display_shutdown_sem, 0);
    qemu_thread_create(&thread, "qemu_main", qemu_main,
                       NULL, QEMU_THREAD_JOINABLE);
#ifdef XEMU_UWP
    while (qemu_sem_timedwait(&display_init_sem, 10) != 0) {
        SDL_PumpEvents();
    }
#else
    qemu_sem_wait(&display_init_sem);
#endif
#ifdef XEMU_UWP
    uwp_log("display_init done, UI thread continuing");
#endif

    gui_grab = 0;
    if (gui_fullscreen) {
        grab_start(0);
        set_full_screen(&scon_list[0], gui_fullscreen);
    }

    /*
     * FIXME: May want to create a callback mechanism for main QEMU thread
     * to just run functions to avoid TLS bugs and locking issues.
     */
    tcg_register_init_ctx();
    qemu_set_current_aio_context(qemu_get_aio_context());

    xemu_main_loop_lock();
    xemu_input_init();
    xemu_main_loop_unlock();

    struct xemu_console *scon = &scon_list[0];
#ifdef XEMU_UWP
    uwp_log("UI loop starting");
    uwp_machine_running = true;
    setvbuf(stderr, NULL, _IONBF, 0);
    signal(SIGABRT, uwp_on_abort);
    SetUnhandledExceptionFilter(uwp_on_crash);
    uwp_session_start_ms = SDL_GetTicks();
    uwp_session_start_tick = GetTickCount64();
    {
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(scon->real_window, &w, &h);
        uwp_story_start(w, h);
    }
    uwp_machine_started = true;
    atexit(uwp_on_exit);
#ifdef XEMU_PGO_GEN
    CloseHandle(CreateThread(NULL, 0, uwp_pgo_thread, NULL, 0, NULL));
    uwp_log("PROFILING BUILD: counters go to LocalState/pgo every minute (slower than a normal build)");
#endif
    g_set_printerr_handler(uwp_glib_print);
    uwp_log_memory();
    uwp_log_hardware();
    SDL_AddEventWatch(uwp_event_watch, NULL);
    UWP_PROF_THREAD("ui");
    CloseHandle(CreateThread(NULL, 0, uwp_sampler_thread, (void *)"vcpu", 0,
                             NULL));
    CloseHandle(CreateThread(NULL, 0, uwp_sampler_thread, (void *)"pfifo", 0,
                             NULL));
    uint64_t uwp_frames = 0, uwp_next_report = SDL_GetTicks() + 3000;
    uint64_t uwp_started_ms = SDL_GetTicks();
#endif
    while (!qatomic_read(&qemu_exiting)) {
        poll_events(scon);
        gl_render_frame(scon);
#ifdef XEMU_UWP
        uwp_frames++;
        uwp_poll_activation();
        if ((uwp_frames & 7) == 0) {
            uwp_poll_quit_combo();
        }
        uwp_press_poll();
        /*
         * Asked for by the launch link. Not before the game has had 45 seconds: a state
         * loaded while the console is still booting can leave the audio processor (DSP)
         * running a program that is not there (Halo 2: assertion in read_memory_p).
         */
        if (uwp_load_state_wanted && SDL_GetTicks() > uwp_started_ms + 45000 &&
            runstate_is_running()) {
            uwp_load_state_wanted = false;
            uwp_state(false);
        }
        if (uwp_save_state_wanted && runstate_is_running()) {
            uwp_save_state_wanted = false;
            uwp_state(true);
        }
        static uint64_t uwp_next_box;
        if (uwp_crash_test_at && SDL_GetTicks() >= uwp_crash_test_at) {
            uwp_crash_test_at = 0;
            volatile int *bad = (volatile int *)(uintptr_t)0x10;
            *bad = 1;
        }
        uwp_shots_tick();
        if (SDL_GetTicks() >= uwp_next_box) {
            uwp_next_box = SDL_GetTicks() + 1000;
            uwp_box_sample();
        }
        if (SDL_GetTicks() >= uwp_next_report) {
            int w = 0, h = 0;
            SDL_GetWindowSizeInPixels(scon->real_window, &w, &h);
            int pads = 0;
            SDL_free(SDL_GetGamepads(&pads));
            ControllerState *p1 = xemu_input_get_bound(0);
            uwp_log("%llu UI frames, window %dx%d, runstate %s, game fps %d, "
                    "gamepads %d, port1 %s",
                    (unsigned long long)uwp_frames, w, h,
                    RunState_str(runstate_get()), g_nv2a_stats.increment_fps,
                    pads, p1 ? p1->name : "(none)");
            uwp_prof_report((SDL_GetTicks() - (uwp_next_report - 3000)) / 1000.0);
            uwp_session_sample(g_nv2a_stats.increment_fps);
            uwp_story_sample(g_nv2a_stats.increment_fps);
            uwp_log_memory();   /* every 3 s: a log that just stops shows how close to the limit it was */
            {
                extern unsigned int xemu_uwp_prim_hist[16];
                static const char *const names[16] = { "points", "lines", "lineloop", "linestrip", "tris",
                    "tristrip", "trifan", "quads", "quadstrip", "polygon", "lines_adj", "linestrip_adj",
                    "tris_adj", "tristrip_adj", "patches", "other" };
                char line[400] = "";
                for (int i = 0; i < 16; i++) {
                    if (xemu_uwp_prim_hist[i]) {
                        g_snprintf(line + strlen(line), sizeof(line) - strlen(line), " %s %u", names[i], xemu_uwp_prim_hist[i]);
                        xemu_uwp_prim_hist[i] = 0;
                    }
                }
                uwp_log("draw primitives:%s", line);
            }
            uwp_next_report = SDL_GetTicks() + 3000;
        }
#endif
    }
#ifdef XEMU_UWP
    /* every way out (xemu's own menu, the console, a link) passes here: the summary before anything else */
    uwp_session_summary("quit normally");
    fflush(NULL);
#endif
    qemu_sem_post(&display_shutdown_sem);
    qemu_thread_join(&thread);
    display_finalize();
    return exit_status;
}

void xemu_eject_disc(Error **errp)
{
    Error *error = NULL;

    xbox_smc_eject_button();
    xemu_settings_set_string(&g_config.sys.files.dvd_path, "");

    // Xbox software may request that the drive open, but do it now anyway
    qmp_eject("ide0-cd1", NULL, true, false, &error);
    if (error) {
        error_propagate(errp, error);
    }

    xbox_smc_update_tray_state();
}

#ifdef XEMU_UWP
/*
 * A full dump of an Xbox disc (7.3 GB) has the game partition 0x18300000
 * bytes in, after the DVD-Video part; the console's kernel wants the drive to
 * start at the game partition, which is what an XISO file is. Instead of
 * making the user convert dumps on a PC, look where the partition starts and
 * let the block layer skip to it. Returns the file name to open: the path
 * itself, or a json: description of the same file read from that offset.
 * Free with g_free().
 */
char *xemu_uwp_disc_filename(const char *path);
char *xemu_uwp_disc_filename(const char *path)
{
    static const char magic[] = "MICROSOFT*XBOX*MEDIA";
    static const uint64_t bases[] = { 0, 0x18300000ull, 0x0FD90000ull,
                                      0x02080000ull };
    uint64_t found = 0;

    if (!path || !path[0] || !strncmp(path, "json:", 5)) {
        return g_strdup(path ? path : "");
    }
    if (g_str_has_prefix(path, "smb://")) {
        /* a disc on a PC's shared folder or a NAS (block/uwp-smb.c): the same look for a full dump's game
           partition, over the network */
        extern bool xemu_uwp_smb_peek(const char *url, uint64_t offset, void *buf, uint32_t len);
        for (int i = 0; i < ARRAY_SIZE(bases) && !found; i++) {
            char head[20];
            if (xemu_uwp_smb_peek(path, bases[i] + 0x10000, head, sizeof(head)) && !memcmp(head, magic, sizeof(head))) {
                found = bases[i];
            }
        }
        if (!found) {
            return g_strdup(path);
        }
        uwp_log("full disc dump on the network: game partition at 0x%llx", (unsigned long long)found);
        GString *j = g_string_new(NULL);
        g_string_printf(j, "json:{\"driver\":\"raw\",\"offset\":%llu,\"file\":{\"driver\":\"smb\",\"url\":\"",
                        (unsigned long long)found);
        for (const char *c = path; *c; c++) {
            if (*c == '"' || *c == '\\') {
                g_string_append_c(j, '\\');
            }
            g_string_append_c(j, *c);
        }
        g_string_append(j, "\"}}");
        return g_string_free(j, FALSE);
    }
    /* Opened like the block layer opens it, so a FAT32 / exFAT USB drive works. */
    wchar_t *wpath = g_utf8_to_utf16(path, -1, NULL, NULL, NULL);
    HANDLE h = wpath ? xemu_uwp_create_file_w(wpath, GENERIC_READ,
                                              FILE_SHARE_READ | FILE_SHARE_WRITE,
                                              OPEN_EXISTING, 0)
                     : INVALID_HANDLE_VALUE;
    g_free(wpath);
    if (h != INVALID_HANDLE_VALUE) {
        for (int i = 0; i < ARRAY_SIZE(bases); i++) {
            char head[20];
            LARGE_INTEGER pos;
            DWORD got = 0;
            pos.QuadPart = bases[i] + 0x10000;
            if (SetFilePointerEx(h, pos, NULL, FILE_BEGIN) &&
                ReadFile(h, head, sizeof(head), &got, NULL) &&
                got == sizeof(head) && !memcmp(head, magic, sizeof(head))) {
                found = bases[i];
                break;
            }
        }
        CloseHandle(h);
    } else {
        /* error 5 on a drive that needs the broker: the console refused this app file access */
        uwp_log("disc: cannot open %s (error %lu)", path, GetLastError());
    }
    if (!found) {
        return g_strdup(path);
    }
    uwp_log("full disc dump: game partition at 0x%llx", (unsigned long long)found);
    GString *name = g_string_new(NULL);
    g_string_printf(name, "json:{\"driver\":\"raw\",\"offset\":%llu,\"file\":"
                    "{\"driver\":\"file\",\"filename\":\"",
                    (unsigned long long)found);
    for (const char *c = path; *c; c++) {
        if (*c == '"' || *c == '\\') {
            g_string_append_c(name, '\\');
        }
        g_string_append_c(name, *c);
    }
    g_string_append(name, "\"}}");
    return g_string_free(name, FALSE);
}
#endif

void xemu_load_disc(const char *path, Error **errp)
{
    Error *error = NULL;

    // Ensure an eject sequence is always triggered so Xbox software reloads
    xbox_smc_eject_button();
    xemu_settings_set_string(&g_config.sys.files.dvd_path, "");

#ifdef XEMU_UWP
    char *filename = xemu_uwp_disc_filename(path);
    qmp_blockdev_change_medium("ide0-cd1", NULL, filename, "raw", false, false,
                               false, 0, &error);
    g_free(filename);
#else
    qmp_blockdev_change_medium("ide0-cd1", NULL, path, "raw", false, false,
                               false, 0, &error);
#endif
    if (error) {
        error_propagate(errp, error);
    } else {
        xemu_settings_set_string(&g_config.sys.files.dvd_path, path);
    }

    xbox_smc_update_tray_state();
}
