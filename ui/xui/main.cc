//
// xemu User Interface
//
// Copyright (C) 2020-2022 Matt Borgerson
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include <SDL3/SDL.h>
#include <epoxy/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <functional>
#include <assert.h>
#include <fpng.h>

#include <deque>
#include <vector>
#include <string>
#include <memory>

#include "actions.hh"
#include "common.hh"
#include "xemu-hud.h"
#include "misc.hh"
#include "gl-helpers.hh"
#include "input-manager.hh"
#include "snapshot-manager.hh"
#include "viewport-manager.hh"
#include "font-manager.hh"
#include "scene.hh"
#include "scene-manager.hh"
#include "main-menu.hh"
#include "popup-menu.hh"
#include "notifications.hh"
#include "monitor.hh"
#include "debug.hh"
#include "welcome.hh"
#include "menubar.hh"
#ifdef CONFIG_D3D12
#include <d3d12.h>
#include <imgui_impl_dx12.h>
#endif

#if defined(XEMU_UWP) && __has_include("xemu-uwp-overlay.inc")
/* the overlay */
#include "xemu-uwp-overlay.inc"
#define UWP_TEST_HUD 1
/* shown while Game Vault asks for it (ui/xemu.c) */
extern "C" bool xemu_uwp_overlay_on;
#endif
#ifdef XEMU_UWP
extern "C" const char *xemu_uwp_shot_wanted(void);
extern "C" void xemu_uwp_shot_save(const char *label, const void *png, size_t size);
extern "C" void xemu_uwp_shot_black(const char *label);
/* xemu's menu is on screen: the game gets no buttons (ui/xemu-input.c) */
extern "C" bool xemu_uwp_menu_open;
/* the start screen while the game's shaders are prepared (ui/xemu.c, uwp_prepare_tick) */
extern "C" bool xemu_uwp_prep_on;
extern "C" int xemu_uwp_prep_done, xemu_uwp_prep_total;

/* waiting for the game's first picture (ui/xemu.c, start watch) */
extern "C" bool xemu_uwp_starting(void);

/* the game's name from the disc file Game Vault passed: no folder, no extensions, no "(USA, Japan)" tags */
static std::string UwpGameName(void)
{
    const char *path = g_config.sys.files.dvd_path ? g_config.sys.files.dvd_path : "";
    const char *slash = strrchr(path, '/'), *back = strrchr(path, '\\');
    const char *base = slash > back ? slash + 1 : back ? back + 1 : path;
    std::string name;
    int depth = 0;
    for (const char *c = base; *c; c++) {
        if (*c == '(' || *c == '[') {
            depth++;
        } else if ((*c == ')' || *c == ']') && depth > 0) {
            depth--;
        } else if (!depth) {
            name += *c;
        }
    }
    for (const char *ext : { ".7z", ".zip", ".iso", ".cso", ".xiso", ".xbe" }) {
        size_t n = strlen(ext);
        if (name.size() > n && !strcasecmp(name.c_str() + name.size() - n, ext)) {
            name.resize(name.size() - n);
        }
    }
    while (!name.empty() && (name.back() == ' ' || name.back() == '.')) {
        name.pop_back();
    }
    return name;
}

/*
 * The green start screen (the Game Vault style logo, user's pick 2026-10-22): from the start until the game's first
 * picture, and while the game's graphics are prepared (xemu_uwp_prep_on). Drawn with shapes, no picture file, so it
 * looks the same on old and New graphics. Few shapes on purpose: it stays cheap on the console.
 */
static bool UwpStartScreen(void)
{
    static float alpha;
    static double since;
    const char *disc = g_config.sys.files.dvd_path;
    bool want = xemu_uwp_prep_on || (xemu_uwp_starting() && disc && *disc);
    double now = ImGui::GetTime();
    if (want && alpha == 0) {
        since = now;
    }
    float step = ImGui::GetIO().DeltaTime / 0.35f;   /* fades in and out in about a third of a second */
    alpha = want ? 1.0f : fmaxf(0.0f, alpha - step);
    if (alpha <= 0) {
        return false;
    }
    static std::string name;
    if (want) {
        name = UwpGameName();
    }

    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImDrawList *dl = ImGui::GetBackgroundDrawList();
    float W = vp->Size.x, H = vp->Size.y, k = H / 1080.0f;
    ImVec2 o = vp->Pos;
    auto col = [&](int r, int g, int b, float a) { return IM_COL32(r, g, b, (int)(255 * a * alpha)); };
    auto at = [&](float x, float y) { return ImVec2(o.x + x, o.y + y); };

    /* background: dark green, lighter behind the logo */
    dl->AddRectFilled(o, at(W, H), col(3, 9, 4, 1));
    for (int i = 0; i < 16; i++) {   /* many faint steps read as one smooth light */
        dl->AddCircleFilled(at(W / 2, H * 0.40f), H * (0.85f - i * 0.05f), col(17, 48, 26, 0.10f), 64);
    }
    for (int i = 0; i < 8; i++) {
        dl->AddCircleFilled(at(W / 2, 372 * k), (240 - i * 26) * k, col(108, 255, 58, 0.018f), 48);
    }

    /* the cartridge with the X: art units 0-100 (GameVault/art/xemu-logo/make_xemu_logo.py), m pixels each */
    float m = 3.2f * k, mx = W / 2 - 50 * m, my = 213 * k;
    auto p = [&](float x, float y) { return at(mx + x * m, my + y * m); };
    auto cart = [&]() {
        dl->PathLineTo(p(24, 10));
        dl->PathLineTo(p(76, 10));
        dl->PathBezierQuadraticCurveTo(p(84, 10), p(84, 18));
        for (ImVec2 v : { ImVec2(84, 74), ImVec2(78, 74), ImVec2(78, 90), ImVec2(22, 90), ImVec2(22, 74),
                          ImVec2(16, 74), ImVec2(16, 18) }) {
            dl->PathLineTo(p(v.x, v.y));
        }
        dl->PathBezierQuadraticCurveTo(p(16, 10), p(24, 10));
    };
    for (int i = 4; i >= 1; i--) {   /* glow: soft rounded blocks behind the rim (thick lines would spike at the corners) */
        float g = i * 2.5f;
        dl->AddRectFilled(p(16 - g, 10 - g), p(84 + g, 74 + g), col(108, 255, 58, 0.06f), (8 + g) * m);
        dl->AddRectFilled(p(22 - g, 70), p(78 + g, 90 + g), col(108, 255, 58, 0.06f), (2 + g) * m);
    }
    cart();
    dl->PathFillConcave(col(20, 46, 24, 1));
    cart();
    dl->PathStroke(col(70, 194, 26, 1), ImDrawFlags_Closed, 6 * m);
    cart();
    dl->PathStroke(col(212, 255, 176, 0.55f), ImDrawFlags_Closed, 1.6f * m);
    for (int x = 30; x <= 66; x += 9) {
        dl->AddRectFilled(p(x, 80), p(x + 5, 87), col(120, 220, 70, 1), 1 * m);
    }
    dl->AddCircleFilled(p(50, 44), 21 * m, col(12, 16, 26, 1), 48);
    dl->AddCircle(p(50, 44), 21 * m, col(108, 200, 70, 0.55f), 48, 3 * m);
    for (float deg : { 45.0f, -45.0f }) {   /* two blades, each a light and a dark facet */
        float a = deg * (float)M_PI / 180, ux = cosf(a), uy = sinf(a);
        ImVec2 t1 = p(50 + ux * 30, 44 + uy * 30), t2 = p(50 - ux * 30, 44 - uy * 30);
        ImVec2 s1 = p(50 - uy * 7, 44 + ux * 7), s2 = p(50 + uy * 7, 44 - ux * 7);
        dl->AddTriangleFilled(t1, s1, t2, col(150, 236, 100, 1));
        dl->AddTriangleFilled(t1, s2, t2, col(38, 120, 16, 1));
    }
    dl->AddCircleFilled(p(50, 44), 5 * m, col(12, 16, 26, 1), 24);
    dl->AddCircle(p(50, 44), 5 * m, col(180, 250, 140, 1), 24, 2 * m);

    /* XEMU / FOR GAME VAULT, letter-spaced */
    auto spaced = [&](ImFont *f, float size, float y, float gap, ImU32 c, const char *text) {
        float w = -gap;
        for (const char *t = text; *t; t++) {
            w += f->CalcTextSizeA(size, FLT_MAX, 0, t, t + 1).x + gap;
        }
        float x = W / 2 - w / 2;
        for (const char *t = text; *t; t++) {
            dl->AddText(f, size, at(x, y), c, t, t + 1);
            x += f->CalcTextSizeA(size, FLT_MAX, 0, t, t + 1).x + gap;
        }
    };
    ImFont *big = g_font_mgr.m_title_font, *small = g_font_mgr.m_menu_font_small;
    spaced(big, 84 * k, 560 * k, 26 * k, col(0, 0, 0, 0.6f), "XEMU");   /* shadow under the name */
    spaced(big, 84 * k, 556 * k, 26 * k, col(140, 240, 90, 1), "XEMU");
    spaced(small, 24 * k, 652 * k, 10 * k, col(159, 179, 200, 1), "FOR GAME VAULT");

    /* progress: the warm-up's real count, else an even fill that slows near the end until the first picture */
    float frac;
    if (xemu_uwp_prep_on) {
        int total = xemu_uwp_prep_total > 0 ? xemu_uwp_prep_total : 1;
        frac = (float)(xemu_uwp_prep_done < total ? xemu_uwp_prep_done : total) / total;
    } else {
        frac = want ? 0.92f * (1 - expf(-(float)(now - since) / 6.0f)) : 1.0f;
    }
    float bw = 400 * k, bx = W / 2 - bw / 2, by = 830 * k;
    dl->AddRectFilled(at(bx, by), at(bx + bw, by + 8 * k), col(255, 255, 255, 0.12f), 4 * k);
    dl->AddRectFilled(at(bx, by), at(bx + bw * frac, by + 8 * k), col(108, 255, 58, 1), 4 * k);

    std::string line = (xemu_uwp_prep_on ? "Getting " : "Starting ") + (name.empty() ? std::string("the game") : name) +
                       (xemu_uwp_prep_on ? " ready..." : "...");
    ImFont *mid = g_font_mgr.m_menu_font;
    float size = 34 * k;
    ImVec2 ts = mid->CalcTextSizeA(size, FLT_MAX, 0, line.c_str());
    if (ts.x > W * 0.8f) {   /* very long names get a smaller line, not a cut one */
        size *= W * 0.8f / ts.x;
        ts = mid->CalcTextSizeA(size, FLT_MAX, 0, line.c_str());
    }
    dl->AddText(mid, size, at(W / 2 - ts.x / 2, 868 * k), col(207, 216, 227, 1), line.c_str());
    if (xemu_uwp_prep_on) {
        const char *b = "B  Start now";
        ImVec2 bs = small->CalcTextSizeA(24 * k, FLT_MAX, 0, b);
        dl->AddText(small, 24 * k, at(W / 2 - bs.x / 2, 930 * k), col(159, 179, 200, 1), b);
    }
    return true;
}
static bool g_uwp_start_shown;
#endif
#include "compat.hh"
#if defined(_WIN32)
#include "update.hh"
#endif

bool g_screenshot_pending;
const char *g_snapshot_pending_load_name;

float g_main_menu_height;

static ImGuiStyle g_base_style;
static float g_last_scale;
static int g_vsync;
static GLuint g_tex;
static bool g_flip_req;
static bool g_hud_ready;
/* New graphics (Direct3D 12 renderer): menus drawn by ImGui's D3D12 backend into the presenter's frame */
static bool g_hud_d3d12;
#ifdef CONFIG_D3D12
static ID3D12DescriptorHeap *g_d3d12_srv_heap;
#endif


static void InitializeStyle()
{
    g_font_mgr.Rebuild();

    ImGui::StyleColorsDark();
    ImVec4 *c = ImGui::GetStyle().Colors;
    c[ImGuiCol_Text]                  = ImVec4(0.94f, 0.94f, 0.94f, 1.00f);
    c[ImGuiCol_TextDisabled]          = ImVec4(0.86f, 0.93f, 0.89f, 0.28f);
    c[ImGuiCol_WindowBg]              = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
    c[ImGuiCol_ChildBg]               = ImVec4(0.06f, 0.06f, 0.06f, 0.98f);
    c[ImGuiCol_PopupBg]               = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
    c[ImGuiCol_Border]                = ImVec4(0.11f, 0.11f, 0.11f, 0.60f);
    c[ImGuiCol_BorderShadow]          = ImVec4(0.16f, 0.16f, 0.16f, 0.00f);
    c[ImGuiCol_FrameBg]               = ImVec4(0.18f, 0.18f, 0.18f, 1.00f);
    c[ImGuiCol_FrameBgHovered]        = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);
    c[ImGuiCol_FrameBgActive]         = ImVec4(0.28f, 0.71f, 0.25f, 1.00f);
    c[ImGuiCol_TitleBg]               = ImVec4(0.20f, 0.51f, 0.18f, 1.00f);
    c[ImGuiCol_TitleBgActive]         = ImVec4(0.26f, 0.66f, 0.23f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed]      = ImVec4(0.16f, 0.16f, 0.16f, 0.75f);
    c[ImGuiCol_MenuBarBg]             = ImVec4(0.14f, 0.14f, 0.14f, 0.00f);
    c[ImGuiCol_ScrollbarBg]           = ImVec4(0.16f, 0.16f, 0.16f, 0.00f);
    c[ImGuiCol_ScrollbarGrab]         = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.24f, 0.60f, 0.00f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.24f, 0.60f, 0.00f, 1.00f);
    c[ImGuiCol_CheckMark]             = ImVec4(0.26f, 0.66f, 0.23f, 1.00f);
    c[ImGuiCol_SliderGrab]            = ImVec4(0.90f, 0.90f, 0.90f, 1.00f);
    c[ImGuiCol_SliderGrabActive]      = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    c[ImGuiCol_Button]                = ImVec4(0.17f, 0.17f, 0.17f, 1.00f);
    c[ImGuiCol_ButtonHovered]         = ImVec4(0.24f, 0.60f, 0.00f, 1.00f);
    c[ImGuiCol_ButtonActive]          = ImVec4(0.26f, 0.66f, 0.23f, 1.00f);
    c[ImGuiCol_Header]                = ImVec4(0.24f, 0.60f, 0.00f, 1.00f);
    c[ImGuiCol_HeaderHovered]         = ImVec4(0.24f, 0.60f, 0.00f, 1.00f);
    c[ImGuiCol_HeaderActive]          = ImVec4(0.24f, 0.60f, 0.00f, 1.00f);
    c[ImGuiCol_Separator]             = ImVec4(1.00f, 1.00f, 1.00f, 0.25f);
    c[ImGuiCol_SeparatorHovered]      = ImVec4(0.13f, 0.87f, 0.16f, 0.78f);
    c[ImGuiCol_SeparatorActive]       = ImVec4(0.25f, 0.75f, 0.10f, 1.00f);
    c[ImGuiCol_ResizeGrip]            = ImVec4(0.47f, 0.83f, 0.49f, 0.04f);
    c[ImGuiCol_ResizeGripHovered]     = ImVec4(0.28f, 0.71f, 0.25f, 0.78f);
    c[ImGuiCol_ResizeGripActive]      = ImVec4(0.28f, 0.71f, 0.25f, 1.00f);
    c[ImGuiCol_Tab]                   = ImVec4(0.26f, 0.67f, 0.23f, 0.95f);
    c[ImGuiCol_TabHovered]            = ImVec4(0.24f, 0.60f, 0.00f, 1.00f);
    c[ImGuiCol_TabActive]             = ImVec4(0.24f, 0.60f, 0.00f, 1.00f);
    c[ImGuiCol_TabUnfocused]          = ImVec4(0.21f, 0.54f, 0.19f, 0.99f);
    c[ImGuiCol_TabUnfocusedActive]    = ImVec4(0.24f, 0.60f, 0.21f, 1.00f);
    c[ImGuiCol_PlotLines]             = ImVec4(0.86f, 0.93f, 0.89f, 0.63f);
    c[ImGuiCol_PlotLinesHovered]      = ImVec4(0.28f, 0.71f, 0.25f, 1.00f);
    c[ImGuiCol_PlotHistogram]         = ImVec4(0.86f, 0.93f, 0.89f, 0.63f);
    c[ImGuiCol_PlotHistogramHovered]  = ImVec4(0.28f, 0.71f, 0.25f, 1.00f);
    c[ImGuiCol_TextSelectedBg]        = ImVec4(0.26f, 0.66f, 0.23f, 1.00f);
    c[ImGuiCol_DragDropTarget]        = ImVec4(1.00f, 1.00f, 0.00f, 0.90f);
    c[ImGuiCol_NavHighlight]          = ImVec4(0.28f, 0.71f, 0.25f, 1.00f);
    c[ImGuiCol_NavWindowingHighlight] = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
    c[ImGuiCol_NavWindowingDimBg]     = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
    c[ImGuiCol_ModalWindowDimBg]      = ImVec4(0.16f, 0.16f, 0.16f, 0.73f);

    ImGuiStyle &s = ImGui::GetStyle();
    s.WindowRounding = 6.0;
    s.FrameRounding = 6.0;
    s.PopupRounding = 6.0;
    g_base_style = s;
}

void xemu_hud_init(SDL_Window* window, void* sdl_gl_context)
{
    xemu_monitor_init();
    g_vsync = g_config.display.window.vsync;

    InitCustomRendering();

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.IniFilename = NULL;

    // Setup Platform/Renderer bindings
    ImGui_ImplSDL3_InitForOpenGL(window, sdl_gl_context);
    ImGui_ImplOpenGL3_Init("#version 150");
    ImPlot::CreateContext();

#if defined(_WIN32)
    if (!g_config.general.show_welcome && g_config.general.updates.check) {
        update_window.CheckForUpdates();
    }
#endif
#ifdef XEMU_UWP
    /*
     * The menu fonts and their texture now, while no other thread uses the graphics driver yet. Made on the first
     * frame instead, with the renderer thread already at work, Mesa crashed there on one Series S at 1440p (three
     * games, 2026-10-17: write to 0x68 under ImGui_ImplOpenGL3_CreateFontsTexture, 2 s into the session).
     */
    g_viewport_mgr.Update();
    g_font_mgr.Update();   /* builds the fonts and their texture (FontManager::Rebuild) */
#endif
    g_last_scale = g_viewport_mgr.m_scale;
    InitializeStyle();
    g_main_menu.SetNextViewIndex(g_config.general.last_viewed_menu_index);
    first_boot_window.is_open = g_config.general.show_welcome;
    g_hud_ready = true;
}

/* New graphics: same menus, ImGui's D3D12 backend on the presenter's device. The menu pictures drawn with OpenGL
 * (logo, controllers) are left out: InitCustomRendering is not called, so those helpers draw nothing. */
void xemu_hud_init_d3d12(SDL_Window *window, void *device,
                         void *command_queue, void *srv_heap,
                         uint64_t font_srv_cpu, uint64_t font_srv_gpu)
{
#ifdef CONFIG_D3D12
    xemu_monitor_init();
    g_vsync = g_config.display.window.vsync;
    InitCustomRenderingOff();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.IniFilename = NULL;

    ImGui_ImplSDL3_InitForD3D(window);
    ImGui_ImplDX12_InitInfo init_info;
    init_info.Device = (ID3D12Device *)device;
    init_info.CommandQueue = (ID3D12CommandQueue *)command_queue;
    init_info.NumFramesInFlight = 3;
    init_info.RTVFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    init_info.DSVFormat = DXGI_FORMAT_UNKNOWN;
    init_info.SrvDescriptorHeap = (ID3D12DescriptorHeap *)srv_heap;
    init_info.LegacySingleSrvCpuDescriptor.ptr = (SIZE_T)font_srv_cpu;
    init_info.LegacySingleSrvGpuDescriptor.ptr = (UINT64)font_srv_gpu;
    /*
     * The font texture keeps the one descriptor for good. ImGui's legacy single-descriptor wrapper allocates it
     * once in Init but frees it on every InvalidateDeviceObjects, and the font manager invalidates on each
     * rebuild: on a 4K screen the menus scale up and rebuild twice at start, the second free hit its assert and
     * xemu stopped before the first frame (players with New graphics on a 4K TV).
     */
    init_info.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo *info, D3D12_CPU_DESCRIPTOR_HANDLE *cpu,
                                        D3D12_GPU_DESCRIPTOR_HANDLE *gpu) {
        *cpu = info->LegacySingleSrvCpuDescriptor;
        *gpu = info->LegacySingleSrvGpuDescriptor;
    };
    init_info.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo *, D3D12_CPU_DESCRIPTOR_HANDLE,
                                       D3D12_GPU_DESCRIPTOR_HANDLE) {};
    g_d3d12_srv_heap = (ID3D12DescriptorHeap *)srv_heap;
    g_hud_d3d12 = true;
    if (!ImGui_ImplDX12_Init(&init_info)) {
        fprintf(stderr, "New graphics: the menus could not start (ImGui_ImplDX12_Init)\n");
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        g_hud_d3d12 = false;
        return;
    }
    ImPlot::CreateContext();
#ifdef XEMU_UWP
    g_viewport_mgr.Update();
    g_font_mgr.Update();
#endif
    g_last_scale = g_viewport_mgr.m_scale;
    InitializeStyle();
    g_main_menu.SetNextViewIndex(g_config.general.last_viewed_menu_index);
    first_boot_window.is_open = g_config.general.show_welcome;
    if (!ImGui_ImplDX12_CreateDeviceObjects()) {
        fprintf(stderr, "New graphics: the menus could not start (ImGui_ImplDX12_CreateDeviceObjects)\n");
    }
    g_hud_ready = true;
#endif
}

bool xemu_hud_is_initialized(void)
{
    return g_hud_ready;
}

bool xemu_hud_uses_d3d12(void)
{
    return g_hud_d3d12;
}

void xemu_hud_cleanup(void)
{
    if (!g_hud_ready) {
        return;
    }
#ifdef CONFIG_D3D12
    if (g_hud_d3d12) {
        ImGui_ImplDX12_Shutdown();
    } else
#endif
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    g_hud_ready = false;
}

void xemu_hud_process_sdl_events(SDL_Event *event)
{
    if (!g_hud_ready) {
        return;   /* New graphics: the menus start with the first frame */
    }
    // Ignore inputs that are consumed by rebinding
    if (g_main_menu.ConsumeRebindEvent(event)) {
        return;
    }

    ImGui_ImplSDL3_ProcessEvent(event);
}

void xemu_hud_should_capture_kbd_mouse(int *kbd, int *mouse)
{
    if (!g_hud_ready) {
        if (kbd) *kbd = 0;
        if (mouse) *mouse = 0;
        return;
    }
    ImGuiIO& io = ImGui::GetIO();
    if (kbd) *kbd = io.WantCaptureKeyboard;
    if (mouse) *mouse = io.WantCaptureMouse;
}

void xemu_hud_set_framebuffer_texture(GLuint tex, bool flip)
{
    g_tex = tex;
    g_flip_req = flip;
}

void xemu_hud_update(void)
{
    if (!g_hud_ready) {
        return;
    }
    ImGuiIO& io = ImGui::GetIO();
    uint32_t now = SDL_GetTicks();

    g_viewport_mgr.Update();
    g_font_mgr.Update();
    if (g_last_scale != g_viewport_mgr.m_scale) {
        ImGuiStyle &style = ImGui::GetStyle();
        style = g_base_style;
        style.ScaleAllSizes(g_viewport_mgr.m_scale);
        g_last_scale = g_viewport_mgr.m_scale;
    }

    if (!first_boot_window.is_open && !g_hud_d3d12) {   /* New graphics: the presenter draws the picture */
        int ww, wh;
        SDL_GetWindowSizeInPixels(xemu_get_window(), &ww, &wh);
        RenderFramebuffer(g_tex, ww, wh, g_flip_req);
#ifdef XEMU_UWP
        /* the session recording's pictures of the screen (ui/xemu.c) */
        const char *label = xemu_uwp_shot_wanted();
        std::vector<uint8_t> png;
        if (label && g_tex && RenderFramebufferToPng(g_tex, g_flip_req, png, 320, 240)) {
            xemu_uwp_shot_save(label, png.data(), png.size());
        } else if (label) {
            xemu_uwp_shot_black(label);
        }
#endif
    }

#ifdef CONFIG_D3D12
    if (g_hud_d3d12) {
        ImGui_ImplDX12_NewFrame();
    } else
#endif
    ImGui_ImplOpenGL3_NewFrame();
    io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
    ImGui_ImplSDL3_NewFrame();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    g_input_mgr.Update();

    ImGui::NewFrame();
    ProcessKeyboardShortcuts();

#ifdef UWP_TEST_HUD
    if (xemu_uwp_overlay_on) {
        uwp_overlay();
    }
#endif
#ifdef XEMU_UWP
    g_uwp_start_shown = UwpStartScreen();
#endif

#if defined(CONFIG_RENDERDOC)
    if (g_capture_renderdoc_frame) {
        nv2a_dbg_renderdoc_capture_frames(1, false);
        g_capture_renderdoc_frame = false;
    }
#endif

    bool menubar = g_config.display.ui.show_menubar && !first_boot_window.is_open;
#ifdef XEMU_UWP
    menubar = menubar && !g_uwp_start_shown;   /* the start screen stands alone */
#endif
    if (menubar) {
        // Auto-hide main menu after 5s of inactivity
        static uint32_t last_check = 0;
        float alpha = 1.0;
        const uint32_t timeout = 5000;
        const float fade_duration = 1000.0;
        bool menu_wakeup = g_input_mgr.MouseMoved();
        if (menu_wakeup) {
            last_check = now;
        }
        if ((now-last_check) > timeout) {
            if (g_config.display.ui.use_animations) {
                float t = fmin((float)((now-last_check)-timeout)/fade_duration, 1);
                alpha = 1-t;
                if (t >= 1) {
                    alpha = 0;
                }
            } else {
                alpha = 0;
            }
        }
        if (alpha > 0.0) {
            ImVec4 tc = ImGui::GetStyle().Colors[ImGuiCol_Text];
            tc.w = alpha;
            ImGui::PushStyleColor(ImGuiCol_Text, tc);
            ImGui::SetNextWindowBgAlpha(alpha);
            ShowMainMenu();
            ImGui::PopStyleColor();
        } else {
            g_main_menu_height = 0;
        }
    }

    static uint32_t last_mouse_move = 0;
    if (g_input_mgr.MouseMoved()) {
        last_mouse_move = now;
    }

    // FIXME: Handle time wrap around
    if (g_config.display.ui.hide_cursor && (now - last_mouse_move) > 3000) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_None);
    }

    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow) &&
        !g_scene_mgr.IsDisplayingScene()) {

        // If the guide button is pressed, wake the ui
        bool menu_button = false;
        uint32_t buttons = g_input_mgr.CombinedButtons();
        if (buttons & CONTROLLER_BUTTON_GUIDE) {
            menu_button = true;
        }

        // Allow controllers without a guide button to also work
        if ((buttons & CONTROLLER_BUTTON_BACK) &&
            (buttons & CONTROLLER_BUTTON_START)) {
            menu_button = true;
        }

        if (ImGui::IsKeyPressed(ImGuiKey_F1)) {
            g_scene_mgr.PushScene(g_main_menu);
        } else if (ImGui::IsKeyPressed(ImGuiKey_F2)) {
            g_scene_mgr.PushScene(g_popup_menu);
        } else if (menu_button ||
                   (ImGui::IsMouseClicked(ImGuiMouseButton_Right) &&
                    !ImGui::IsAnyItemFocused() && !ImGui::IsAnyItemHovered())) {
            g_scene_mgr.PushScene(g_popup_menu);
        } else if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            xemu_toggle_fullscreen();
        }

        bool mod_key_down = ImGui::IsKeyDown(ImGuiKey_ModShift);
        for (int f_key = 0; f_key < 4; ++f_key) {
            if (ImGui::IsKeyPressed((enum ImGuiKey)(ImGuiKey_F5 + f_key))) {
                ActionActivateBoundSnapshot(f_key, mod_key_down);
                break;
            }
        }
    }

    first_boot_window.Draw();
    monitor_window.Draw();
    apu_window.Draw();
    video_window.Draw();
    compatibility_reporter_window.Draw();
#if defined(_WIN32)
    update_window.Draw();
#endif
    g_scene_mgr.Draw();
#ifdef XEMU_UWP
    xemu_uwp_menu_open = g_scene_mgr.IsDisplayingScene();
#endif
    if (!first_boot_window.is_open) notification_manager.Draw();
    g_snapshot_mgr.Draw();

    // static bool show_demo = true;
    // if (show_demo) ImGui::ShowDemoWindow(&show_demo);
}

void xemu_hud_render()
{
    if (g_hud_d3d12) {
        return;   /* xemu_hud_render_d3d12 */
    }
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    if (g_vsync != g_config.display.window.vsync) {
        g_vsync = g_config.display.window.vsync;
        SDL_GL_SetSwapInterval(g_vsync ? 1 : 0);
    }

    if (g_screenshot_pending) {
        SaveScreenshot(g_tex, g_flip_req);
        g_screenshot_pending = false;
    }
}

/* New graphics: the menus into the presenter's command list (ui/xemu.c, d3d12_render_frame) */
void xemu_hud_render_d3d12(void *command_list)
{
#ifdef CONFIG_D3D12
    ID3D12GraphicsCommandList *cl = (ID3D12GraphicsCommandList *)command_list;
    if (!g_hud_d3d12 || cl == NULL || g_d3d12_srv_heap == NULL) {
        return;
    }
    ImGui::Render();
    /* pictures that only exist as OpenGL textures (controller, logo, thumbnails) are 0 here: D3D12 would read
     * descriptor 0, so those draws are dropped */
    ImDrawData *dd = ImGui::GetDrawData();
    for (int i = 0; i < dd->CmdListsCount; i++) {
        for (ImDrawCmd &cmd : dd->CmdLists[i]->CmdBuffer) {
            if (cmd.UserCallback == NULL && cmd.GetTexID() == (ImTextureID)0) {
                cmd.ElemCount = 0;
            }
        }
    }
    ID3D12DescriptorHeap *heaps[] = { g_d3d12_srv_heap };
    cl->SetDescriptorHeaps(1, heaps);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cl);
    g_vsync = g_config.display.window.vsync;
#endif
}

/* New graphics: the presenter saves the screenshot; this names the file */
char *xemu_hud_take_screenshot_path(void)
{
    char fname[128];

    if (!g_screenshot_pending) {
        return NULL;
    }
    g_screenshot_pending = false;

    time_t t = time(NULL);
    struct tm *tmp = localtime(&t);
    if (tmp) {
        strftime(fname, sizeof(fname), "xemu-%Y-%m-%d-%H-%M-%S.png", tmp);
    } else {
        strcpy(fname, "xemu.png");
    }
    const char *output_dir = g_config.general.screenshot_dir;
    if (!strlen(output_dir)) {
        output_dir = ".";
    }
    return g_strdup_printf("%s/%s", output_dir, fname);
}
