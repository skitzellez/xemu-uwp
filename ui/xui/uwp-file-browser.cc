/*
 * Copyright (c) 2026 Skitzelles - part of xemu for Xbox Dev Mode (github.com/skitzellez/xemu-uwp). GPL-2.0-or-later.
 *
 * xemu for Xbox Dev Mode: the file browser behind xemu's "pick a file" buttons (EEPROM, BIOS, hard disk, memory
 * units, discs). SDL's WinRT dialog asks the system's file picker, which never opens on the console, so those
 * buttons did nothing (a player, 2026-10-26). This one is drawn in xemu's menu and works with the controller:
 * first the places (the console's Xbox files folder, the drives: USB drives are E:, F:, ...), then folders and
 * the files the button takes. A opens or picks, B goes up a folder (on the places: closes), Y shows all files.
 *
 * Copyright (c) 2026 Skitzelles - part of Game Vault. Licensed like the rest of xemu (GPL-2.0-or-later).
 */

#ifdef XEMU_UWP

#include <windows.h>
#include <glib.h>
#include <algorithm>
#include <string>
#include <vector>
#include "common.hh"
#include "misc.hh"

namespace {

struct Entry {
    std::string name, label;
    bool dir;
};

struct Browser {
    bool want_open = false, open = false, refocus = false, show_all = false, any_ok = false;
    UwpFileBrowserMode mode = UwpFileBrowserMode::Open;
    std::vector<std::string> exts;
    std::string dir; /* "" = the places; else ends with '\' */
    std::string system; /* the console's Xbox files folder */
    std::string save_name;
    std::string left; /* the folder just left, focused after going up */
    std::vector<Entry> entries;
    FileDialogCallback callback;
};

Browser b;

bool IsDir(const std::string &path)
{
    DWORD a = GetFileAttributesA(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool Lower(char a, char c)
{
    return tolower((unsigned char)a) < tolower((unsigned char)c);
}

bool Wanted(const std::string &name)
{
    if (b.show_all || b.exts.empty()) {
        return true;
    }
    size_t dot = name.rfind('.');
    if (dot == std::string::npos) {
        return false;
    }
    std::string ext = name.substr(dot + 1);
    for (const std::string &e : b.exts) {
        if (_stricmp(e.c_str(), ext.c_str()) == 0) {
            return true;
        }
    }
    return false;
}

std::string SizeText(unsigned long long size)
{
    char s[32];
    if (size >= (1ull << 30)) {
        snprintf(s, sizeof(s), "%.1f GB", size / 1073741824.0);
    } else if (size >= (1ull << 20)) {
        snprintf(s, sizeof(s), "%.1f MB", size / 1048576.0);
    } else {
        snprintf(s, sizeof(s), "%llu KB", (size + 1023) / 1024);
    }
    return s;
}

void List()
{
    b.entries.clear();
    b.refocus = true;
    if (b.dir.empty()) {
        if (!b.system.empty() && IsDir(b.system)) {
            b.entries.push_back({ b.system, "This console: Xbox files (BIOS, hard disk, EEPROM)", true });
        }
        DWORD mask = GetLogicalDrives();
        for (char d = 'C'; d <= 'Z'; d++) {
            if (!(mask & (1u << (d - 'A')))) {
                continue;
            }
            std::string root = std::string(1, d) + ":\\";
            WIN32_FIND_DATAA f;
            HANDLE h = FindFirstFileA((root + "*").c_str(), &f);
            if (h == INVALID_HANDLE_VALUE) {
                continue; /* not open to apps */
            }
            FindClose(h);
            char label[MAX_PATH + 1] = "";
            GetVolumeInformationA(root.c_str(), label, sizeof(label), NULL, NULL, NULL, NULL, 0);
            b.entries.push_back({ root, label[0] ? root + "  " + label : root + "  Drive", true });
        }
        return;
    }
    WIN32_FIND_DATAA f;
    HANDLE h = FindFirstFileA((b.dir + "*").c_str(), &f);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    std::vector<Entry> dirs, files;
    do {
        std::string name = f.cFileName;
        if (name == "." || name == ".." || (f.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN)) {
            continue;
        }
        if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            dirs.push_back({ name, "[" + name + "]", true });
        } else if (b.mode != UwpFileBrowserMode::Folder && Wanted(name)) {
            unsigned long long size = ((unsigned long long)f.nFileSizeHigh << 32) | f.nFileSizeLow;
            files.push_back({ name, name + "   " + SizeText(size), false });
        }
    } while (FindNextFileA(h, &f));
    FindClose(h);
    auto by_name = [](const Entry &x, const Entry &y) {
        return std::lexicographical_compare(x.name.begin(), x.name.end(), y.name.begin(), y.name.end(), Lower);
    };
    std::sort(dirs.begin(), dirs.end(), by_name);
    std::sort(files.begin(), files.end(), by_name);
    b.entries = dirs;
    b.entries.insert(b.entries.end(), files.begin(), files.end());
}

void Go(const std::string &dir)
{
    b.dir = dir;
    List();
}

void Up()
{
    if (b.dir.empty()) {
        return;
    }
    if (b.dir.size() <= 3 || _stricmp(b.dir.c_str(), b.system.c_str()) == 0) {
        b.left = b.dir;
        Go("");
        return;
    }
    std::string d = b.dir.substr(0, b.dir.size() - 1);
    size_t cut = d.rfind('\\');
    b.left = d.substr(cut + 1);
    Go(d.substr(0, cut + 1));
}

void Close()
{
    b.open = false;
    b.callback = nullptr;
    ImGui::CloseCurrentPopup();
}

void Pick(const std::string &path)
{
    FileDialogCallback cb = std::move(b.callback);
    Close();
    if (cb) {
        cb(path.c_str());
    }
}

/* "<name>.<ext>", or "<name> 2.<ext>", ... when taken */
std::string FreeName(const std::string &dir, const std::string &ext)
{
    std::string base = b.mode == UwpFileBrowserMode::Save && ext == "img" ? "memory unit" : "new file";
    std::string name = base + "." + ext;
    for (int n = 2; GetFileAttributesA((dir + name).c_str()) != INVALID_FILE_ATTRIBUTES; n++) {
        name = base + " " + std::to_string(n) + "." + ext;
    }
    return name;
}

} // namespace

void UwpFileBrowserOpen(UwpFileBrowserMode mode, const SDL_DialogFileFilter *filters, int nfilters,
                        const char *default_location, FileDialogCallback callback)
{
    b.mode = mode;
    b.callback = std::move(callback);
    b.exts.clear();
    b.any_ok = false;
    b.show_all = false;
    b.left.clear();
    for (int i = 0; i < nfilters; i++) {
        std::string p = filters[i].pattern ? filters[i].pattern : "";
        size_t start = 0;
        while (start <= p.size()) {
            size_t end = p.find(';', start);
            std::string e = p.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (e == "*") {
                b.any_ok = true;
            } else if (!e.empty()) {
                b.exts.push_back(e);
            }
            if (end == std::string::npos) {
                break;
            }
            start = end + 1;
        }
    }
    char *sys = xemu_uwp_system_folder();
    b.system = sys ? sys : "";
    g_free(sys);

    /* start where the current file is, else on the places */
    std::string start = default_location ? default_location : "";
    for (char &c : start) {
        if (c == '/') {
            c = '\\';
        }
    }
    if (!start.empty() && !IsDir(start)) {
        size_t cut = start.rfind('\\');
        start = cut == std::string::npos ? "" : start.substr(0, cut + 1);
    }
    if (!start.empty() && start.back() != '\\') {
        start += '\\';
    }
    Go(!start.empty() && IsDir(start) ? start : "");
    b.save_name = FreeName(b.dir, b.exts.empty() ? "bin" : b.exts[0]);
    b.want_open = true;
}

void UwpFileBrowserDraw()
{
    if (b.want_open) {
        ImGui::OpenPopup("###uwp_files");
        b.want_open = false;
        b.open = true;
    }
    if (!b.open) {
        return;
    }
    ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(vp->Size.x * 0.7f, vp->Size.y * 0.8f), ImGuiCond_Always);
    const char *title = b.mode == UwpFileBrowserMode::Save     ? "Save as###uwp_files" :
                        b.mode == UwpFileBrowserMode::Folder   ? "Choose a folder###uwp_files" :
                                                                 "Choose a file###uwp_files";
    if (!ImGui::BeginPopupModal(title, nullptr,
                                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                    ImGuiWindowFlags_NoSavedSettings)) {
        b.open = false;
        return;
    }

    ImGui::TextUnformatted(b.dir.empty() ? "Where is the file?" : b.dir.c_str());
    ImGui::TextDisabled(b.any_ok && b.mode == UwpFileBrowserMode::Open ?
                            "A: open / choose    B: up a folder    Y: show all files" :
                            "A: open / choose    B: up a folder");
    ImGui::Separator();

    bool save_row = b.mode != UwpFileBrowserMode::Open && !b.dir.empty();
    float footer = ImGui::GetFrameHeightWithSpacing() * (save_row ? 2.2f : 1.2f);
    std::string go, pick;
    bool up = false;
    ImGui::BeginChild("###uwp_files_list", ImVec2(0, -footer), true);
    if (!b.dir.empty()) {
        if (b.refocus && b.left.empty()) {
            ImGui::SetKeyboardFocusHere();
        }
        if (ImGui::Selectable(".. (up a folder)")) {
            up = true;
        }
    }
    for (size_t i = 0; i < b.entries.size(); i++) {
        const Entry &e = b.entries[i];
        ImGui::PushID((int)i);
        if (b.refocus && ((b.left.empty() && b.dir.empty() && i == 0) ||
                          (!b.left.empty() && (e.name == b.left || e.name + "\\" == b.left)))) {
            ImGui::SetKeyboardFocusHere();
        }
        if (ImGui::Selectable(e.label.c_str())) {
            if (b.dir.empty()) {
                go = e.name;
            } else if (e.dir) {
                go = b.dir + e.name + "\\";
            } else if (b.mode == UwpFileBrowserMode::Save) {
                b.save_name = e.name;
            } else {
                pick = b.dir + e.name;
            }
        }
        ImGui::PopID();
    }
    if (b.entries.empty()) {
        ImGui::TextDisabled(b.dir.empty() ? "No drives found." : "Nothing here for this button.");
    }
    b.refocus = false;
    ImGui::EndChild();

    if (save_row && b.mode == UwpFileBrowserMode::Save) {
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
        ImGui::InputText("###uwp_files_name", &b.save_name);
        ImGui::SameLine();
        bool taken = GetFileAttributesA((b.dir + b.save_name).c_str()) != INVALID_FILE_ATTRIBUTES;
        if (ImGui::Button("Save here") && !b.save_name.empty() && !taken) {
            pick = b.dir + b.save_name;
        }
        if (taken) {
            ImGui::SameLine();
            ImGui::TextDisabled("(that name is taken)");
        }
    } else if (save_row) {
        if (ImGui::Button("Use this folder")) {
            pick = b.dir;
        }
    }
    if (b.any_ok && b.mode == UwpFileBrowserMode::Open) {
        if (ImGui::Checkbox("Show all files", &b.show_all)) {
            b.left.clear();
            List();
        }
        ImGui::SameLine();
    }
    if (ImGui::Button("Cancel")) {
        Close();
        ImGui::EndPopup();
        return;
    }

    bool typing = ImGui::GetIO().WantTextInput;
    if (!typing && b.any_ok && b.mode == UwpFileBrowserMode::Open &&
        ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false)) {
        b.show_all = !b.show_all;
        b.left.clear();
        List();
    }
    if (!typing && (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) ||
                    ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
                    ImGui::IsKeyPressed(ImGuiKey_Backspace, false))) {
        if (b.dir.empty()) {
            Close();
            ImGui::EndPopup();
            return;
        }
        up = true;
    }

    if (!pick.empty()) {
        Pick(pick);
    } else if (!go.empty()) {
        b.left.clear();
        Go(go);
    } else if (up) {
        Up();
    }
    ImGui::EndPopup();
}

#endif
