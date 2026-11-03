/*
 * xemu on Xbox Dev Mode: the TV's picture mode (HDMI), for frame generation at 120 Hz
 *
 * Copyright (c) 2026 Skitzelles - part of Game Vault, https://github.com/skitzellez/xbox-game-vault
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Windows.Graphics.Display.Core.HdmiDisplayInformation through its plain-C interface. MinGW's headers do not
 * have it, so the few interfaces used are declared here, laid out as in the Windows SDK 10.0.26100's
 * windows.graphics.display.core.h (same IDs and method order).
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <windows.h>
#include <roapi.h>
#include <winstring.h>

#include "xemu-uwp-hdmi.h"

void uwp_log(const char *fmt, ...);

typedef struct HdmiMode HdmiMode;
typedef struct HdmiModeVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(HdmiMode *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(HdmiMode *);
    ULONG (STDMETHODCALLTYPE *Release)(HdmiMode *);
    HRESULT (STDMETHODCALLTYPE *GetIids)(HdmiMode *, ULONG *, IID **);
    HRESULT (STDMETHODCALLTYPE *GetRuntimeClassName)(HdmiMode *, HSTRING *);
    HRESULT (STDMETHODCALLTYPE *GetTrustLevel)(HdmiMode *, int *);
    HRESULT (STDMETHODCALLTYPE *get_ResolutionWidthInRawPixels)(HdmiMode *, UINT32 *);
    HRESULT (STDMETHODCALLTYPE *get_ResolutionHeightInRawPixels)(HdmiMode *, UINT32 *);
    HRESULT (STDMETHODCALLTYPE *get_RefreshRate)(HdmiMode *, DOUBLE *);
    HRESULT (STDMETHODCALLTYPE *get_StereoEnabled)(HdmiMode *, boolean *);
    HRESULT (STDMETHODCALLTYPE *get_BitsPerPixel)(HdmiMode *, UINT16 *);
    HRESULT (STDMETHODCALLTYPE *IsEqual)(HdmiMode *, HdmiMode *, boolean *);
    HRESULT (STDMETHODCALLTYPE *get_ColorSpace)(HdmiMode *, int *);
    HRESULT (STDMETHODCALLTYPE *get_PixelEncoding)(HdmiMode *, int *);
    HRESULT (STDMETHODCALLTYPE *get_IsSdrLuminanceSupported)(HdmiMode *, boolean *);
    HRESULT (STDMETHODCALLTYPE *get_IsSmpte2084Supported)(HdmiMode *, boolean *);
    HRESULT (STDMETHODCALLTYPE *get_Is2086MetadataSupported)(HdmiMode *, boolean *);
} HdmiModeVtbl;
struct HdmiMode { const HdmiModeVtbl *lpVtbl; };

/* IVectorView<HdmiDisplayMode> */
typedef struct HdmiModes HdmiModes;
typedef struct HdmiModesVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(HdmiModes *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(HdmiModes *);
    ULONG (STDMETHODCALLTYPE *Release)(HdmiModes *);
    HRESULT (STDMETHODCALLTYPE *GetIids)(HdmiModes *, ULONG *, IID **);
    HRESULT (STDMETHODCALLTYPE *GetRuntimeClassName)(HdmiModes *, HSTRING *);
    HRESULT (STDMETHODCALLTYPE *GetTrustLevel)(HdmiModes *, int *);
    HRESULT (STDMETHODCALLTYPE *GetAt)(HdmiModes *, UINT32, HdmiMode **);
    HRESULT (STDMETHODCALLTYPE *get_Size)(HdmiModes *, UINT32 *);
} HdmiModesVtbl;
struct HdmiModes { const HdmiModesVtbl *lpVtbl; };

/* IAsyncOperation<bool>: only released, the switch is not waited for */
typedef struct AsyncOp AsyncOp;
typedef struct AsyncOpVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(AsyncOp *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(AsyncOp *);
    ULONG (STDMETHODCALLTYPE *Release)(AsyncOp *);
} AsyncOpVtbl;
struct AsyncOp { const AsyncOpVtbl *lpVtbl; };

typedef struct HdmiInfo HdmiInfo;
typedef struct HdmiInfoVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(HdmiInfo *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(HdmiInfo *);
    ULONG (STDMETHODCALLTYPE *Release)(HdmiInfo *);
    HRESULT (STDMETHODCALLTYPE *GetIids)(HdmiInfo *, ULONG *, IID **);
    HRESULT (STDMETHODCALLTYPE *GetRuntimeClassName)(HdmiInfo *, HSTRING *);
    HRESULT (STDMETHODCALLTYPE *GetTrustLevel)(HdmiInfo *, int *);
    HRESULT (STDMETHODCALLTYPE *GetSupportedDisplayModes)(HdmiInfo *, HdmiModes **);
    HRESULT (STDMETHODCALLTYPE *GetCurrentDisplayMode)(HdmiInfo *, HdmiMode **);
    HRESULT (STDMETHODCALLTYPE *SetDefaultDisplayModeAsync)(HdmiInfo *, void **);
    HRESULT (STDMETHODCALLTYPE *RequestSetCurrentDisplayModeAsync)(HdmiInfo *, HdmiMode *, AsyncOp **);
} HdmiInfoVtbl;
struct HdmiInfo { const HdmiInfoVtbl *lpVtbl; };

typedef struct HdmiStatics HdmiStatics;
typedef struct HdmiStaticsVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(HdmiStatics *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(HdmiStatics *);
    ULONG (STDMETHODCALLTYPE *Release)(HdmiStatics *);
    HRESULT (STDMETHODCALLTYPE *GetIids)(HdmiStatics *, ULONG *, IID **);
    HRESULT (STDMETHODCALLTYPE *GetRuntimeClassName)(HdmiStatics *, HSTRING *);
    HRESULT (STDMETHODCALLTYPE *GetTrustLevel)(HdmiStatics *, int *);
    HRESULT (STDMETHODCALLTYPE *GetForCurrentView)(HdmiStatics *, HdmiInfo **);
} HdmiStaticsVtbl;
struct HdmiStatics { const HdmiStaticsVtbl *lpVtbl; };

static const IID kIidHdmiStatics = {0x6ce6b260, 0xf42a, 0x4a15, {0x91, 0x4c, 0x7b, 0x8e, 0x2a, 0x5a, 0x65, 0xdf}};

static double s_hz;   /* the refresh rate in use, 0 = not known */

double xemu_uwp_hdmi_refresh_rate(void)
{
    return s_hz;
}

static HdmiInfo *hdmi_info(void)
{
    static const WCHAR cls[] = L"Windows.Graphics.Display.Core.HdmiDisplayInformation";
    HSTRING_HEADER h;
    HSTRING name;
    HdmiStatics *statics = NULL;
    HdmiInfo *info = NULL;
    HRESULT hr;

    if (FAILED(WindowsCreateStringReference(cls, (UINT32)wcslen(cls), &h, &name))) {
        return NULL;
    }
    hr = RoGetActivationFactory(name, &kIidHdmiStatics, (void **)&statics);
    if (FAILED(hr) || !statics) {
        uwp_log("hdmi: not available (0x%08lx)", (unsigned long)hr);
        return NULL;
    }
    hr = statics->lpVtbl->GetForCurrentView(statics, &info);
    statics->lpVtbl->Release(statics);
    if (FAILED(hr) || !info) {
        uwp_log("hdmi: no display information for this thread (0x%08lx)", (unsigned long)hr);
        return NULL;
    }
    return info;
}

static void mode_values(HdmiMode *m, UINT32 *w, UINT32 *h, double *hz, int *space, int *enc, UINT16 *bpp,
                        boolean *hdr)
{
    boolean stereo = 0;
    *w = *h = 0;
    *hz = 0;
    *space = *enc = 0;
    *bpp = 0;
    *hdr = 0;
    m->lpVtbl->get_ResolutionWidthInRawPixels(m, w);
    m->lpVtbl->get_ResolutionHeightInRawPixels(m, h);
    m->lpVtbl->get_RefreshRate(m, hz);
    m->lpVtbl->get_ColorSpace(m, space);
    m->lpVtbl->get_PixelEncoding(m, enc);
    m->lpVtbl->get_BitsPerPixel(m, bpp);
    m->lpVtbl->get_IsSmpte2084Supported(m, hdr);
    m->lpVtbl->get_StereoEnabled(m, &stereo);
    if (stereo) {
        *hz = -*hz;   /* never picked */
    }
}

bool xemu_uwp_hdmi_request_hz(double want_hz)
{
    HdmiInfo *info = hdmi_info();
    HdmiMode *cur = NULL, *best = NULL;
    HdmiModes *modes = NULL;
    UINT32 cw, ch, count = 0;
    double chz;
    int cspace, cenc;
    UINT16 cbpp;
    boolean chdr;
    int best_score = -1;
    bool asked = false;

    if (!info) {
        return false;
    }
    if (FAILED(info->lpVtbl->GetCurrentDisplayMode(info, &cur)) || !cur) {
        info->lpVtbl->Release(info);
        return false;
    }
    mode_values(cur, &cw, &ch, &chz, &cspace, &cenc, &cbpp, &chdr);
    s_hz = chz;
    uwp_log("hdmi: now %ux%u %.2f Hz", cw, ch, chz);
    if (chz > want_hz - 1.5 && chz < want_hz + 1.5) {
        cur->lpVtbl->Release(cur);
        info->lpVtbl->Release(info);
        return true;
    }
    if (SUCCEEDED(info->lpVtbl->GetSupportedDisplayModes(info, &modes)) && modes) {
        modes->lpVtbl->get_Size(modes, &count);
        uwp_log("hdmi: %u modes offered", count);
        for (UINT32 i = 0; i < count; i++) {
            HdmiMode *m = NULL;
            UINT32 w, h;
            double hz;
            int space, enc;
            UINT16 bpp;
            boolean hdr;
            if (FAILED(modes->lpVtbl->GetAt(modes, i, &m)) || !m) {
                continue;
            }
            mode_values(m, &w, &h, &hz, &space, &enc, &bpp, &hdr);
            uwp_log("hdmi: offered %ux%u %.2f Hz, %u bits, colour %d/%d%s", w, h, hz, (unsigned)bpp, space, enc,
                    hdr ? ", HDR" : "");
            /* the same picture as now, only faster: same size first, then the same colour settings */
            if (hz > want_hz - 1.5 && hz < want_hz + 1.5) {
                int score = (w == cw && h == ch ? 16 : 0) + (hdr == chdr ? 8 : 0) + (bpp == cbpp ? 4 : 0) +
                            (space == cspace ? 2 : 0) + (enc == cenc ? 1 : 0);
                if (score > best_score) {
                    if (best) {
                        best->lpVtbl->Release(best);
                    }
                    best = m;
                    best_score = score;
                    continue;
                }
            }
            m->lpVtbl->Release(m);
        }
        modes->lpVtbl->Release(modes);
    }
    if (best && best_score >= 16) {   /* never a different picture size */
        AsyncOp *op = NULL;
        HRESULT hr = info->lpVtbl->RequestSetCurrentDisplayModeAsync(info, best, &op);
        uwp_log("hdmi: asking for %ux%u %.0f Hz (0x%08lx)", cw, ch, want_hz, (unsigned long)hr);
        if (op) {
            op->lpVtbl->Release(op);
        }
        asked = SUCCEEDED(hr);
        if (asked) {
            s_hz = want_hz;
        }
    } else {
        uwp_log("hdmi: the TV offers no %.0f Hz mode at %ux%u", want_hz, cw, ch);
    }
    if (best) {
        best->lpVtbl->Release(best);
    }
    cur->lpVtbl->Release(cur);
    info->lpVtbl->Release(info);
    return asked;
}
