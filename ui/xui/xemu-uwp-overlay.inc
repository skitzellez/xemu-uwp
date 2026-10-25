// Copyright (c) 2026 Skitzelles - part of the xemu UWP port, https://github.com/skitzellez/xemu-uwp
// GPL-2.0-or-later, like xemu.
// On-screen overlay with speed and load figures and a one-minute graph, shown when the launcher asks.

extern "C" int xemu_uwp_thread_busy(const char *name, int slot);
extern "C" void uwp_log(const char *fmt, ...);
extern "C" bool xemu_uwp_renderer_is_d3d12(void);

static void uwp_overlay(void)
{
    static float hist[240];            // one sample every 250 ms: one minute
    static int head = 0, filled = 0;
    static uint32_t next_sample = 0, next_cpu = 0;
    static int cpu = -1, gpu = -1;
    uint32_t now = SDL_GetTicks();
    if (now >= next_sample) {
        next_sample = now + 250;
        hist[head] = (float)g_nv2a_stats.increment_fps;
        head = (head + 1) % 240;
        if (filled < 240) {
            filled++;
        }
    }
    // first minute after start (the Xbox logo and the game's first load): every 250 ms one log
    // line with the frames shown, the slowest frame and how busy the two threads were
    static uint32_t next_boot = 0, seen = 0;
    if (now < 60000 && now >= next_boot) {
        next_boot = now + 250;
        uint32_t fc = g_nv2a_stats.frame_count, n = fc - seen;
        seen = fc;
        if (n > NV2A_PROF_NUM_FRAMES) {
            n = NV2A_PROF_NUM_FRAMES;
        }
        int worst = 0, over20 = 0, over34 = 0;
        for (uint32_t i = 0; i < n; i++) {
            int ms = g_nv2a_stats.frame_history[(g_nv2a_stats.frame_ptr + NV2A_PROF_NUM_FRAMES - 1 - i) %
                                                NV2A_PROF_NUM_FRAMES].mspf;
            worst = ms > worst ? ms : worst;
            over20 += ms > 20;
            over34 += ms > 34;
        }
        uwp_log("boot %5.2f s: frames %u, slowest %d ms, >20 ms %d, >34 ms %d, cpu %d%%, gpu %d%%",
                now / 1000.0, n, worst, over20, over34, xemu_uwp_thread_busy("vcpu", 1),
                xemu_uwp_thread_busy("pfifo", 1));
    }
    if (now >= next_cpu) {
        next_cpu = now + 1000;
        cpu = xemu_uwp_thread_busy("vcpu", 0);
        gpu = xemu_uwp_thread_busy("pfifo", 0);
    }
    if (filled == 0) {
        return;
    }

    // target: the game's best over the last 20 s, as 30 or 60
    float peak = 0, low = 1000;
    for (int i = 0; i < filled; i++) {
        float v = hist[(head - 1 - i + 480) % 240];
        if (i < 80 && v > peak) {
            peak = v;
        }
        if (v < low) {
            low = v;
        }
    }
    float target = peak > 45 ? 60.0f : 30.0f;
    float fps = hist[(head + 239) % 240];
    ImU32 col = fps >= target * 0.95f ? IM_COL32(90, 230, 120, 255)
              : fps >= target * 0.75f ? IM_COL32(250, 210, 60, 255)
                                      : IM_COL32(255, 80, 70, 255);

    ImDrawList *dl = ImGui::GetForegroundDrawList();
    ImGuiIO &io = ImGui::GetIO();
    float s = io.DisplaySize.y / 1080.0f;
    ImFont *font = ImGui::GetFont();
    ImVec2 o(30 * s, 30 * s);
    float w = 520 * s, h = 230 * s;
    dl->AddRectFilled(o, ImVec2(o.x + w, o.y + h), IM_COL32(0, 0, 0, 170), 10 * s);

    char line[128];
    snprintf(line, sizeof line, "FPS %.0f / %.0f", fps, target);
    dl->AddText(font, 52 * s, ImVec2(o.x + 16 * s, o.y + 8 * s), col, line);
    {
        /* which graphics draw the game, top right of the box: players' screenshots then say it */
        const char *gfx = xemu_uwp_renderer_is_d3d12() ? "D3D12" : "MESA";
        ImVec2 tw = font->CalcTextSizeA(22 * s, FLT_MAX, 0.0f, gfx);
        dl->AddText(font, 22 * s, ImVec2(o.x + w - 16 * s - tw.x, o.y + 8 * s), IM_COL32(150, 200, 255, 255), gfx);
    }
    snprintf(line, sizeof line, "lowest last min %.0f", low);
    dl->AddText(font, 24 * s, ImVec2(o.x + 300 * s, o.y + 30 * s), IM_COL32(200, 200, 200, 255), line);
    snprintf(line, sizeof line, "Xbox CPU %d%%   GPU %d%%%s", cpu, gpu,
             cpu >= 95 && fps < target * 0.95f ? "   CPU-limited" : "");
    dl->AddText(font, 28 * s, ImVec2(o.x + 16 * s, o.y + 70 * s),
                cpu >= 95 ? IM_COL32(255, 160, 90, 255) : IM_COL32(220, 220, 220, 255), line);

    // graph: one minute, target line dashed-ish
    ImVec2 g0(o.x + 16 * s, o.y + 112 * s), g1(o.x + w - 16 * s, o.y + h - 14 * s);
    float gh = g1.y - g0.y, gw = g1.x - g0.x, top = 66.0f;
    float ty = g1.y - gh * target / top;
    for (float x = g0.x; x < g1.x; x += 12 * s) {
        dl->AddLine(ImVec2(x, ty), ImVec2(x + 6 * s, ty), IM_COL32(255, 255, 255, 90), 1.5f * s);
    }
    ImVec2 prev;
    for (int i = 0; i < filled; i++) {
        float v = hist[(head - filled + i + 480) % 240];
        ImVec2 p(g1.x - gw * (filled - 1 - i) / 239.0f, g1.y - gh * (v > top ? top : v) / top);
        if (i) {
            ImU32 c = v >= target * 0.95f ? IM_COL32(90, 230, 120, 255)
                    : v >= target * 0.75f ? IM_COL32(250, 210, 60, 255)
                                          : IM_COL32(255, 80, 70, 255);
            dl->AddLine(prev, p, c, 2.5f * s);
        }
        prev = p;
    }
}
