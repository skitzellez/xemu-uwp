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
#include "font-manager.hh"
#include "viewport-manager.hh"
#include "xemu-hud.h"
#ifdef CONFIG_D3D12
#include <imgui_impl_dx12.h>
#endif

#include "data/Roboto-Medium.ttf.h"
#include "data/RobotoCondensed-Regular.ttf.h"
#include "data/font_awesome_6_1_1_solid.min.otf.h"
#include "data/abxy.ttf.h"
#ifdef XEMU_UWP
#include "gv-logo-glyph.h"
#endif

FontManager g_font_mgr;

FontManager::FontManager()
{
    m_last_viewport_scale = 1;
    m_last_pixel_density = 1;
}

void FontManager::Rebuild()
{
    ImGuiIO &io = ImGui::GetIO();

    io.Fonts->Clear();

    float scale = g_viewport_mgr.m_scale;
    float pixel_density = g_viewport_mgr.m_pixel_density;

    {
        ImFontConfig config;
        config.FontDataOwnedByAtlas = false;
        config.RasterizerDensity = pixel_density;
        m_default_font = io.Fonts->AddFontFromMemoryTTF(
            (void *)Roboto_Medium_data, Roboto_Medium_size,
            16.0f * scale, &config);
        m_menu_font_small = io.Fonts->AddFontFromMemoryTTF(
            (void *)RobotoCondensed_Regular_data, RobotoCondensed_Regular_size,
            22.0f * scale, &config);
    }
    {
        ImFontConfig config;
        config.FontDataOwnedByAtlas = false;
        config.RasterizerDensity = pixel_density;
        config.MergeMode = true;
        config.GlyphOffset =
            ImVec2(0, 13 * scale);
        config.GlyphMaxAdvanceX = 24.0f * scale;
        static const ImWchar icon_ranges[] = { 0xf900, 0xf903, 0 };
        io.Fonts->AddFontFromMemoryTTF((void *)abxy_data, abxy_size,
                                       40.0f * scale,
                                       &config, icon_ranges);
    }
    {
        ImFontConfig config;
        config.FontDataOwnedByAtlas = false;
        config.RasterizerDensity = pixel_density;
        m_menu_font_medium = io.Fonts->AddFontFromMemoryTTF(
            (void *)RobotoCondensed_Regular_data, RobotoCondensed_Regular_size,
            26.0f * scale, &config);
        m_menu_font = io.Fonts->AddFontFromMemoryTTF(
            (void *)RobotoCondensed_Regular_data, RobotoCondensed_Regular_size,
            34.0f * scale, &config);
    }
    {
        ImFontConfig config;
        config.FontDataOwnedByAtlas = false;
        config.RasterizerDensity = pixel_density;
        config.MergeMode = true;
        config.GlyphOffset =
            ImVec2(0, -3 * scale);
        config.GlyphMinAdvanceX = 32.0f * scale;
        static const ImWchar icon_ranges[] = { ICON_MIN_FA, ICON_MAX_FA, 0 };
        io.Fonts->AddFontFromMemoryTTF((void *)font_awesome_6_1_1_solid_min_data,
                                       font_awesome_6_1_1_solid_min_size,
                                       18.0f * scale,
                                       &config, icon_ranges);
    }
    {
        ImFontConfig config = ImFontConfig();
        config.OversampleH = config.OversampleV = 1;
        config.PixelSnapH = true;
        config.RasterizerDensity = pixel_density;
        config.SizePixels = 13.0f * scale;
        m_fixed_width_font = io.Fonts->AddFontDefault(&config);
    }
#ifdef XEMU_UWP
    {
        /* the big XEMU name on the start screen (main.cc); letters only, so the atlas stays small */
        ImFontConfig config;
        config.FontDataOwnedByAtlas = false;
        config.RasterizerDensity = pixel_density;
        static const ImWchar title_ranges[] = { 0x20, 0x5a, 0 };
        m_title_font = io.Fonts->AddFontFromMemoryTTF(
            (void *)Roboto_Medium_data, Roboto_Medium_size,
            84.0f * scale, &config, title_ranges);
    }
#endif

#ifdef XEMU_UWP
    {
        /* Game Vault's logo as a character of the menu font (U+E000, GV_LOGO_GLYPH in font-manager.hh): the icon of
         * "Return to Game Vault" in the popup menu. The atlas is built here, then the logo copied into its place. */
        float fsz = 34.0f * scale;
        int gsz = (int)(28.0f * scale + 0.5f);
        int rect = io.Fonts->AddCustomRectFontGlyph(m_menu_font, 0xE000, gsz, gsz, gsz + 6.0f * scale,
                                                    ImVec2(0, (fsz - gsz) * 0.5f));
        io.Fonts->Build();
        unsigned char *pixels = NULL;
        int tw = 0, th = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &tw, &th);
        const ImFontAtlasCustomRect *r = io.Fonts->GetCustomRectByIndex(rect);
        if (pixels && r && r->IsPacked()) {
            const int n = gv_logo_glyph_size;
            for (int y = 0; y < r->Height; y++) {
                for (int x = 0; x < r->Width; x++) {
                    /* a box filter over the logo pixels this texel covers */
                    int sx0 = x * n / r->Width, sx1 = (x + 1) * n / r->Width;
                    int sy0 = y * n / r->Height, sy1 = (y + 1) * n / r->Height;
                    unsigned sum[4] = { 0, 0, 0, 0 }, cnt = 0;
                    for (int sy = sy0; sy < (sy1 > sy0 ? sy1 : sy0 + 1); sy++) {
                        for (int sx = sx0; sx < (sx1 > sx0 ? sx1 : sx0 + 1); sx++) {
                            const unsigned char *s = gv_logo_glyph_rgba + 4 * (sy * n + sx);
                            for (int c = 0; c < 4; c++) {
                                sum[c] += s[c];
                            }
                            cnt++;
                        }
                    }
                    unsigned char *d = pixels + 4 * ((r->Y + y) * tw + r->X + x);
                    for (int c = 0; c < 4; c++) {
                        d[c] = (unsigned char)(sum[c] / cnt);
                    }
                }
            }
        }
    }
#endif

#ifdef CONFIG_D3D12
    if (xemu_hud_uses_d3d12()) {
        /* New graphics: the font texture is a device object; ImGui_ImplDX12_NewFrame makes it again */
        ImGui_ImplDX12_InvalidateDeviceObjects();
        return;
    }
#endif
    ImGui_ImplOpenGL3_DestroyFontsTexture();
    ImGui_ImplOpenGL3_CreateFontsTexture();
}

void FontManager::Update()
{
    if (g_viewport_mgr.m_scale != m_last_viewport_scale ||
        g_viewport_mgr.m_pixel_density != m_last_pixel_density) {
        Rebuild();
        m_last_viewport_scale = g_viewport_mgr.m_scale;
        m_last_pixel_density = g_viewport_mgr.m_pixel_density;
    }
}
