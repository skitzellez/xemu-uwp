/*
 * Copyright (c) 2026 Skitzelles - part of xemu for Xbox Dev Mode (github.com/skitzellez/xemu-uwp). GPL-2.0-or-later.
 *
 * xemu UWP port: Xbox racing wheels. On the console a wheel is a
 * Windows.Gaming.Input.RacingWheel (and a UINavigationController), never an SDL
 * gamepad, so SDL alone does not see it. Here a wheel is read straight from
 * Windows.Gaming.Input through the plain-C interfaces and handed to
 * ui/xemu-input.c as an Xbox controller:
 *   steering        left stick X        throttle / brake   right / left trigger
 *   A B X Y         Accept Cancel Context1 Context2 (UI navigation; handbrake = X)
 *   Start / Back    Menu / View         d-pad              wheel or UI navigation d-pad
 *   Black / White   next / previous gear (paddles; the original Xbox pad has no bumpers)
 *   L3 / R3         Button1 / Button2   clutch             right stick down
 * The same layout as the other Game Vault emulators (docs/WHEEL-NOTES.md).
 * Nothing runs until xemu-input asks, and with no wheel nothing changes.
 */

#ifdef XEMU_UWP

#define COBJMACROS
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <windows.h>
#include <roapi.h>
#include <winstring.h>
#include <initguid.h>   /* the header's interface IDs are defined here (nothing else in xemu uses them) */
#include <windows.gaming.input.h>
#include <SDL3/SDL.h>

#include "xemu-input.h"

void uwp_log(const char *fmt, ...);

/* Windows.Gaming.Input.UINavigationController, which MinGW's header leaves out
 * (layouts from the Windows SDK): the wheel's A/B/X/Y and Menu/View */
typedef struct UINavReading {
    UINT64 Timestamp;
    UINT32 RequiredButtons; /* Menu 1, View 2, Accept 4, Cancel 8, Up 16, Down 32, Left 64, Right 128 */
    UINT32 OptionalButtons; /* Context1 1, Context2 2, Context3 4, Context4 8, PageUp 16, PageDown 32,
                               PageLeft 64, PageRight 128 */
} UINavReading;
typedef struct IUINav IUINav;
typedef struct IUINavVtbl {
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(IUINav *, REFIID, void **);
    ULONG(STDMETHODCALLTYPE *AddRef)(IUINav *);
    ULONG(STDMETHODCALLTYPE *Release)(IUINav *);
    HRESULT(STDMETHODCALLTYPE *GetIids)(IUINav *, ULONG *, IID **);
    HRESULT(STDMETHODCALLTYPE *GetRuntimeClassName)(IUINav *, HSTRING *);
    HRESULT(STDMETHODCALLTYPE *GetTrustLevel)(IUINav *, int *);
    HRESULT(STDMETHODCALLTYPE *GetCurrentReading)(IUINav *, UINavReading *);
} IUINavVtbl;
struct IUINav { const IUINavVtbl *lpVtbl; };
typedef struct IUINavStatics2 IUINavStatics2;
typedef struct IUINavStatics2Vtbl {
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(IUINavStatics2 *, REFIID, void **);
    ULONG(STDMETHODCALLTYPE *AddRef)(IUINavStatics2 *);
    ULONG(STDMETHODCALLTYPE *Release)(IUINavStatics2 *);
    HRESULT(STDMETHODCALLTYPE *GetIids)(IUINavStatics2 *, ULONG *, IID **);
    HRESULT(STDMETHODCALLTYPE *GetRuntimeClassName)(IUINavStatics2 *, HSTRING *);
    HRESULT(STDMETHODCALLTYPE *GetTrustLevel)(IUINavStatics2 *, int *);
    HRESULT(STDMETHODCALLTYPE *FromGameController)(IUINavStatics2 *, __x_ABI_CWindows_CGaming_CInput_CIGameController *,
                                                   IUINav **);
} IUINavStatics2Vtbl;
struct IUINavStatics2 { const IUINavStatics2Vtbl *lpVtbl; };
static const IID kIID_UINavStatics2 = {0xe0cb28e3, 0xb20b, 0x4b0b, {0x9e, 0xd4, 0xf3, 0xd5, 0x3c, 0xec, 0x0d, 0xe4}};

#define MAX_WHEELS 4
typedef __x_ABI_CWindows_CGaming_CInput_CIRacingWheel IWheel;
static __x_ABI_CWindows_CGaming_CInput_CIRacingWheelStatics *s_statics;
static IUINavStatics2 *s_nav_statics;
static bool s_tried;
static IWheel *s_wheels[MAX_WHEELS];
static IUINav *s_navs[MAX_WHEELS];
static int s_count;
static uint64_t s_next_check;
static bool s_first_input[MAX_WHEELS];

static void *statics(const WCHAR *cls, UINT32 len, REFIID iid)
{
    HSTRING_HEADER h;
    HSTRING name;
    void *out = NULL;
    if (SUCCEEDED(WindowsCreateStringReference(cls, len, &h, &name))) {
        RoGetActivationFactory(name, iid, &out);
    }
    return out;
}

static void drop_wheels(void)
{
    for (int i = 0; i < s_count; i++) {
        if (s_navs[i]) {
            s_navs[i]->lpVtbl->Release(s_navs[i]);
        }
        __x_ABI_CWindows_CGaming_CInput_CIRacingWheel_Release(s_wheels[i]);
        s_navs[i] = NULL;
        s_wheels[i] = NULL;
    }
    s_count = 0;
}

/* the wheels plugged in, looked up at most once a second (a list call each frame would be wasteful) */
int xemu_uwp_wheel_count(void)
{
    uint64_t now = SDL_GetTicks();
    if (now < s_next_check) {
        return s_count;
    }
    s_next_check = now + 1000;
    if (!s_tried) {
        s_tried = true;
        static const WCHAR wheel[] = L"Windows.Gaming.Input.RacingWheel";
        static const WCHAR nav[] = L"Windows.Gaming.Input.UINavigationController";
        s_statics = statics(wheel, (UINT32)wcslen(wheel), &IID___x_ABI_CWindows_CGaming_CInput_CIRacingWheelStatics);
        s_nav_statics = statics(nav, (UINT32)wcslen(nav), &kIID_UINavStatics2);
        if (!s_statics) {
            uwp_log("racing wheels: Windows.Gaming.Input.RacingWheel not available");
        }
    }
    if (!s_statics) {
        return 0;
    }
    __FIVectorView_1_Windows__CGaming__CInput__CRacingWheel *list = NULL;
    UINT32 n = 0;
    if (FAILED(__x_ABI_CWindows_CGaming_CInput_CIRacingWheelStatics_get_RacingWheels(s_statics, &list)) || !list) {
        return s_count;
    }
    __FIVectorView_1_Windows__CGaming__CInput__CRacingWheel_get_Size(list, &n);
    if (n > MAX_WHEELS) {
        n = MAX_WHEELS;
    }
    /* only a change in the number counts: the same wheel may come back as a different pointer each time */
    if ((int)n != s_count) {
        int before = s_count;
        drop_wheels();
        for (UINT32 i = 0; i < n; i++) {
            IWheel *w = NULL;
            if (FAILED(__FIVectorView_1_Windows__CGaming__CInput__CRacingWheel_GetAt(list, i, &w)) || !w) {
                break;
            }
            s_wheels[s_count] = w;   /* the reference GetAt gave is kept */
            s_navs[s_count] = NULL;
            __x_ABI_CWindows_CGaming_CInput_CIGameController *gc = NULL;
            if (s_nav_statics &&
                SUCCEEDED(__x_ABI_CWindows_CGaming_CInput_CIRacingWheel_QueryInterface(
                    w, &IID___x_ABI_CWindows_CGaming_CInput_CIGameController, (void **)&gc)) && gc) {
                s_nav_statics->lpVtbl->FromGameController(s_nav_statics, gc, &s_navs[s_count]);
                __x_ABI_CWindows_CGaming_CInput_CIGameController_Release(gc);
            }
            s_first_input[s_count] = true;
            s_count++;
        }
        uwp_log("racing wheels: %d connected (was %d)%s", s_count, before,
                s_count && !s_navs[0] ? ", no UI navigation (A/B/X/Y from the wheel buttons only)" : "");
    }
    __FIVectorView_1_Windows__CGaming__CInput__CRacingWheel_Release(list);
    return s_count;
}

static int16_t axis_from(double v, double lo, double hi, int16_t out_lo, int16_t out_hi)
{
    if (v < lo) {
        v = lo;
    }
    if (v > hi) {
        v = hi;
    }
    return (int16_t)(out_lo + (v - lo) / (hi - lo) * (out_hi - out_lo));
}

/* one wheel as an Xbox controller; false when it is gone */
bool xemu_uwp_wheel_read(int index, uint16_t *buttons, int16_t *axis)
{
    if (index < 0 || index >= s_count) {
        return false;
    }
    __x_ABI_CWindows_CGaming_CInput_CRacingWheelReading r;
    memset(&r, 0, sizeof(r));
    if (FAILED(__x_ABI_CWindows_CGaming_CInput_CIRacingWheel_GetCurrentReading(s_wheels[index], &r))) {
        return false;
    }
    UINavReading nav;
    memset(&nav, 0, sizeof(nav));
    if (s_navs[index]) {
        s_navs[index]->lpVtbl->GetCurrentReading(s_navs[index], &nav);
    }
    uint32_t wb = (uint32_t)r.Buttons, req = nav.RequiredButtons, opt = nav.OptionalButtons;
    uint16_t b = 0;
    if (req & 4) b |= CONTROLLER_BUTTON_A;
    if (req & 8) b |= CONTROLLER_BUTTON_B;
    if ((opt & 1) || r.Handbrake > 0.5) b |= CONTROLLER_BUTTON_X;
    if (opt & 2) b |= CONTROLLER_BUTTON_Y;
    if (req & 1) b |= CONTROLLER_BUTTON_START;
    if (req & 2) b |= CONTROLLER_BUTTON_BACK;
    if ((wb & 0x04) || (req & 16)) b |= CONTROLLER_BUTTON_DPAD_UP;
    if ((wb & 0x08) || (req & 32)) b |= CONTROLLER_BUTTON_DPAD_DOWN;
    if ((wb & 0x10) || (req & 64)) b |= CONTROLLER_BUTTON_DPAD_LEFT;
    if ((wb & 0x20) || (req & 128)) b |= CONTROLLER_BUTTON_DPAD_RIGHT;
    if ((wb & 0x02) || (opt & 128)) b |= CONTROLLER_BUTTON_BLACK;   /* next gear */
    if ((wb & 0x01) || (opt & 64)) b |= CONTROLLER_BUTTON_WHITE;    /* previous gear */
    if (wb & 0x40) b |= CONTROLLER_BUTTON_LSTICK;                    /* Button1 */
    if (wb & 0x80) b |= CONTROLLER_BUTTON_RSTICK;                    /* Button2 */
    *buttons = b;
    memset(axis, 0, sizeof(int16_t) * CONTROLLER_AXIS__COUNT);
    axis[CONTROLLER_AXIS_LSTICK_X] = axis_from(r.Wheel, -1, 1, -32768, 32767);
    axis[CONTROLLER_AXIS_RTRIG] = axis_from(r.Throttle, 0, 1, 0, 32767);
    axis[CONTROLLER_AXIS_LTRIG] = axis_from(r.Brake, 0, 1, 0, 32767);
    axis[CONTROLLER_AXIS_RSTICK_Y] = axis_from(r.Clutch, 0, 1, 0, -32768);   /* stick down */
    if (s_first_input[index] && (wb || req || opt || r.Throttle > 0.1 || r.Brake > 0.1 ||
                                 r.Wheel > 0.1 || r.Wheel < -0.1)) {
        /* for reports: which wheel buttons land where, once per connection */
        s_first_input[index] = false;
        uwp_log("racing wheel %d: first input: wheel %.2f throttle %.2f brake %.2f buttons 0x%x nav 0x%x/0x%x",
                index + 1, r.Wheel, r.Throttle, r.Brake, wb, req, opt);
    }
    return true;
}

#endif
