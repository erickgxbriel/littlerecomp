// The settings the launcher and the in-game options overlay share: display, this game's own options, advanced.
#include "app/app.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <imgui.h>

namespace app::ui {

namespace {

const ImVec4 kAccent(.569f, .620f, .510f, 1.0f);
const ImVec4 kAccentHover(.65f, .70f, .58f, 1.0f);
const ImVec4 kMuted(.58f, .62f, .55f, 1.0f);

// the display modes this monitor offers, without the refresh rates, smallest first
const std::vector<Mode>& DisplayModes() {
    static const std::vector<Mode> modes = [] {
        std::vector<Mode> out;
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        for (DWORD i = 0; EnumDisplaySettingsW(nullptr, i, &dm); i++) {
            if (dm.dmBitsPerPel != 32 || dm.dmPelsWidth < 640 || dm.dmPelsHeight < 480) continue;
            bool seen = false;
            for (const Mode& m : out) seen |= m.width == dm.dmPelsWidth && m.height == dm.dmPelsHeight;
            if (!seen) out.push_back({dm.dmPelsWidth, dm.dmPelsHeight});
        }
        std::sort(out.begin(), out.end(), [](const Mode& a, const Mode& b) {
            return uint64_t(a.width) * a.height < uint64_t(b.width) * b.height;
        });
        return out;
    }();
    return modes;
}

std::string SizeText(uint32_t width, uint32_t height) {
    char text[64];
    std::snprintf(text, sizeof(text), "%u x %u", width, height);
    return text;
}

bool OptionEnabled(const char* key) {
    for (const Option& o : kGame.options)
        if (o.flag && std::strcmp(o.key, key) == 0) return *o.flag;
    return false;
}

}  // namespace

void Style(float unit) {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.ScaleAllSizes(unit);
    s.WindowRounding = 0;
    s.ChildRounding = 0;
    s.WindowBorderSize = 0;
    s.WindowPadding = ImVec2(22 * unit, 16 * unit);
    s.FramePadding = ImVec2(10 * unit, 7 * unit);
    s.ItemSpacing = ImVec2(10 * unit, 9 * unit);
    s.ItemInnerSpacing = ImVec2(8 * unit, 6 * unit);
    s.FrameRounding = 4 * unit;
    s.GrabRounding = 4 * unit;
    s.PopupRounding = 6 * unit;
    s.ScrollbarRounding = 6 * unit;
    s.Colors[ImGuiCol_WindowBg] = ImVec4(.043f, .047f, .043f, 1.0f);
    s.Colors[ImGuiCol_PopupBg] = ImVec4(.065f, .072f, .062f, 1.0f);
    s.Colors[ImGuiCol_Text] = ImVec4(.89f, .91f, .86f, 1.0f);
    s.Colors[ImGuiCol_TextDisabled] = kMuted;
    s.Colors[ImGuiCol_FrameBg] = ImVec4(.115f, .130f, .105f, 1.0f);
    s.Colors[ImGuiCol_FrameBgHovered] = ImVec4(.160f, .180f, .145f, 1.0f);
    s.Colors[ImGuiCol_FrameBgActive] = ImVec4(.200f, .225f, .180f, 1.0f);
    s.Colors[ImGuiCol_Button] = ImVec4(.140f, .160f, .125f, 1.0f);
    s.Colors[ImGuiCol_ButtonHovered] = ImVec4(.200f, .230f, .180f, 1.0f);
    s.Colors[ImGuiCol_ButtonActive] = ImVec4(.250f, .290f, .220f, 1.0f);
    s.Colors[ImGuiCol_Header] = ImVec4(.200f, .230f, .180f, 1.0f);
    s.Colors[ImGuiCol_HeaderHovered] = ImVec4(.250f, .290f, .220f, 1.0f);
    s.Colors[ImGuiCol_HeaderActive] = ImVec4(.300f, .340f, .260f, 1.0f);
    s.Colors[ImGuiCol_Separator] = ImVec4(.220f, .250f, .200f, 1.0f);
    s.Colors[ImGuiCol_CheckMark] = kAccent;
    s.Colors[ImGuiCol_SliderGrab] = kAccent;
    s.Colors[ImGuiCol_SliderGrabActive] = kAccentHover;
    s.Colors[ImGuiCol_Tab] = ImVec4(.100f, .115f, .095f, 1.0f);
    s.Colors[ImGuiCol_TabHovered] = ImVec4(.240f, .270f, .210f, 1.0f);
    s.Colors[ImGuiCol_TabSelected] = ImVec4(.180f, .200f, .160f, 1.0f);
    s.Colors[ImGuiCol_TabDimmed] = ImVec4(.080f, .090f, .075f, 1.0f);
    s.Colors[ImGuiCol_TabDimmedSelected] = ImVec4(.140f, .160f, .125f, 1.0f);
    s.Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, .55f);
}

bool LoadFont(float unit) {
    wchar_t windows[MAX_PATH] = {};
    GetWindowsDirectoryW(windows, MAX_PATH);
    std::filesystem::path font = std::filesystem::path(windows) / "Fonts" / "segoeui.ttf";
    std::error_code error;
    ImFont* loaded = nullptr;
    if (std::filesystem::is_regular_file(font, error))
        loaded = ImGui::GetIO().Fonts->AddFontFromFileTTF(Utf8Path(font).c_str(), std::round(17.0f * unit));
    if (!loaded) ImGui::GetIO().Fonts->AddFontDefault();
    return loaded != nullptr;
}

void Display(Settings& s, bool live) {
    bool changed = false;
    const char* modes[] = {"Borderless (fullscreen)", "Fullscreen", "Windowed"};
    int mode = int(s.display_mode);
    if (ImGui::Combo("Display mode", &mode, modes, 3)) {
        s.display_mode = DisplayMode(mode);
        changed = true;
    }
    ImGui::TextDisabled("Borderless fills the screen without changing its mode; fullscreen switches the monitor.");
    ImGui::Spacing();

    Mode desktop = Desktop();
    std::string current = s.width ? SizeText(s.width, s.height)
                                  : "Desktop (" + SizeText(desktop.width, desktop.height) + ")";
    if (ImGui::BeginCombo("Resolution", current.c_str())) {
        if (ImGui::Selectable("Desktop", s.width == 0 && s.height == 0)) {
            s.width = s.height = 0;
            changed = true;
        }
        for (const Mode& m : DisplayModes()) {
            std::string label = SizeText(m.width, m.height);
            if (ImGui::Selectable(label.c_str(), s.width == m.width && s.height == m.height)) {
                s.width = m.width;
                s.height = m.height;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::TextDisabled("Desktop is %s.", SizeText(desktop.width, desktop.height).c_str());
    ImGui::Spacing();

    int render = int(s.render_percent);
    if (ImGui::SliderInt("Render scale", &render, 50, 200, "%d%%")) s.render_percent = uint32_t(render);
    ImGui::TextDisabled("The 3D image is drawn at this percentage of the output height.");
    ImGui::Spacing();

    const char* aa[] = {"Off", "FXAA"};
    int aa_index = int(s.antialiasing);
    if (ImGui::Combo("Antialiasing", &aa_index, aa, 2)) s.antialiasing = AntiAliasing(aa_index);
    ImGui::TextDisabled("FXAA smooths jagged edges in the final image after gamma correction.");
    ImGui::Spacing();

    struct Cap {
        uint32_t value;
        const char* label;
    };
    static const Cap caps[] = {{kFpsCapRefresh, "Monitor refresh rate"}, {0, "Unlimited"}, {30, "30 fps"},
                               {60, "60 fps"},                         {75, "75 fps"},   {120, "120 fps"},
                               {144, "144 fps"},                       {240, "240 fps"}};
    char custom[32];
    const char* label = nullptr;
    for (const Cap& c : caps)
        if (c.value == s.fps_cap) label = c.label;
    if (!label) {
        std::snprintf(custom, sizeof(custom), "%u fps", s.fps_cap);
        label = custom;
    }
    if (ImGui::BeginCombo("Frame rate", label)) {
        for (const Cap& c : caps)
            if (ImGui::Selectable(c.label, c.value == s.fps_cap)) s.fps_cap = c.value;
        ImGui::EndCombo();
    }
    if (s.experimental_60fps)
        ImGui::TextDisabled("Experimental mode skips the game's 30 Hz wait and fixes the rate at 60 fps.");
    else
        ImGui::TextDisabled("This game renders at about 30 fps on its own; the cap can only lower it, not raise it.");
    ImGui::Checkbox("Experimental: 60 fps mode", &s.experimental_60fps);
    ImGui::TextWrapped("Skips the guest's 30 Hz vblank wait and ignores the frame rate above while enabled. Off by "
                       "default; higher rates accelerate the game's animations, so this mode is capped at 60.");
    ImGui::Spacing();
    ImGui::Checkbox("V-Sync", &s.vsync);
    ImGui::Checkbox("Show the frame rate", &s.show_fps);
    ImGui::TextDisabled("A small fps counter in the top left of the game's picture.");

    // the launcher's window does not exist yet, so only the game applies a change as it is made
    if (changed && live) ApplyDisplay();
}

void InputOptions() {
    bool any = false;
    for (const Option& o : kGame.options) {
        any = true;
        const char* label = o.label ? o.label : o.key;
        if (o.flag) {
            ImGui::Checkbox(label, o.flag);
        } else if (o.value) {
            int value = int(*o.value);
            if (o.max > o.min) {
                if (ImGui::SliderInt(label, &value, int(o.min), int(o.max))) *o.value = uint32_t(value);
            } else if (ImGui::InputInt(label, &value, 1, 10)) {
                *o.value = uint32_t(std::max(value, 0));
            }
        }
    }
    if (!any) ImGui::TextDisabled("This game has no input settings.");
}

void BindingEditor() {
    if (!kGame.binding_count || !kGame.binding_describe || !kGame.binding_capture) return;

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextColored(kAccent, "Keyboard to Xbox pad bindings");
    ImGui::TextWrapped("A gamepad is the default. Turn on Keyboard and mouse above to use these fixed bindings: each "
                       "key sends one Xbox pad control, and the game decides what that control means on the current "
                       "screen. Mouse look always drives the right stick.");
    bool keyboard = OptionEnabled("kbm");
    if (!keyboard) ImGui::TextDisabled("Keyboard and mouse are off; the saved bindings below are used when enabled.");

    bool capturing = kGame.binding_capture_active && kGame.binding_capture_active();
    if (capturing) {
        ImGui::TextColored(kAccent, "Press a key or mouse button for the selected control. Esc cancels.");
        if (kGame.binding_capture_cancel && ImGui::Button("Cancel capture")) kGame.binding_capture_cancel();
    }

    if (!keyboard) ImGui::BeginDisabled();
    if (ImGui::BeginTable("##binding-editor", 3,
                          ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Xbox pad control", ImGuiTableColumnFlags_WidthStretch, 0.50f);
        ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthStretch, 0.32f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 64.0f);
        ImGui::TableHeadersRow();
        size_t count = kGame.binding_count();
        for (size_t i = 0; i < count; ++i) {
            const char* pad = "?", *key = "?", *unused = nullptr;
            bool changed = false;
            kGame.binding_describe(i, &pad, &key, &changed);
            (void)unused;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(pad);
            ImGui::TableSetColumnIndex(1);
            std::string label = changed ? std::string("* ") + key : std::string(key);
            char id[32];
            std::snprintf(id, sizeof(id), "##bind-%zu", i);
            if (ImGui::Button(label.c_str(), ImVec2(-1, 0))) kGame.binding_capture(i);
            ImGui::TableSetColumnIndex(2);
            std::snprintf(id, sizeof(id), "Reset##bind-%zu", i);
            if (kGame.binding_reset && ImGui::Button(id)) kGame.binding_reset(i);
        }
        ImGui::EndTable();
    }
    if (kGame.binding_reset_all && ImGui::Button("Reset all bindings to Xbox defaults"))
        kGame.binding_reset_all();
    if (!keyboard) ImGui::EndDisabled();
    ImGui::TextDisabled("An asterisk marks a changed binding. These are saved with the other settings.");
}

void ControllerNote() {
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextWrapped("A gamepad is the default. Controllers (Xbox, PlayStation and Switch) are detected "
                       "automatically. Keyboard and mouse are opt-in and work alongside a connected controller; "
                       "whichever you touch last drives the game.");
    ImGui::TextWrapped("Experimental PC key prompts replace HUD controller glyphs with the keys from the binding "
                       "table while the keyboard or mouse is active. The game's context-sensitive prompts can still "
                       "be misleading, so the original glyphs are the default. Touching a controller brings them "
                       "straight back.");
}

void ModernControls() {
    bool enabled = false;
    for (const Option& o : kGame.options)
        if (o.flag && std::strcmp(o.key, "modern_pad") == 0) enabled = *o.flag;
    if (!enabled) return;

    static const ControlHint kDriving[] = {
        {"RT", "Accelerate"},
        {"LT", "Brake / reverse"},
        {"A", "Handbrake"},
        {"B", "Horn (the game shares this input with accelerate)"},
        {"RB", "Fire from the vehicle"},
        {"LB", "Look back"},
        {"L3", "Lock target"},
        {"Back", "SAT phone menu"},
        {"X", "Original brake, available as a secondary pedal"},
        {"Left stick", "Steer"},
        {"Right stick", "Camera"},
        {"Y / d-pad / R3", "Enter or exit / weapons / camera view"},
    };

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextColored(kAccent, "Experimental: modern gamepad controls - active bindings");
    ImGui::TextWrapped("This preset is experimental: only the physical pad is remapped, and only while driving. On "
                       "foot the original pad layout applies; keyboard and mouse bindings are unchanged.");
    if (ImGui::BeginTable("##modern-controls", 2,
                          ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Pad", ImGuiTableColumnFlags_WidthStretch, 0.38f);
        ImGui::TableSetupColumn("Action while driving", ImGuiTableColumnFlags_WidthStretch, 0.62f);
        for (const ControlHint& hint : kDriving) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(hint.keys);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextWrapped("%s", hint.action);
        }
        ImGui::EndTable();
    }
}

void KeyBindings() {
    if (kGame.controls.empty()) return;
    for (const ControlSection& section : kGame.controls) {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextDisabled("%s", section.title);
        if (ImGui::BeginTable(section.title, 2,
                              ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableSetupColumn("Keys", ImGuiTableColumnFlags_WidthStretch, 0.38f);
            ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch, 0.62f);
            for (const ControlHint& hint : section.hints) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(hint.keys);
                ImGui::TableSetColumnIndex(1);
                ImGui::TextWrapped("%s", hint.action);
            }
            ImGui::EndTable();
        }
    }
}

void Advanced(Settings& s) {
    struct Aniso {
        uint32_t value;
        const char* label;
    };
    static const Aniso levels[] = {{0, "As the game asks"}, {2, "2x"}, {4, "4x"}, {8, "8x"}, {16, "16x"}};
    char custom[32];
    const char* label = nullptr;
    for (const Aniso& a : levels)
        if (a.value == s.aniso) label = a.label;
    if (!label) {
        std::snprintf(custom, sizeof(custom), "%ux", s.aniso);
        label = custom;
    }
    if (ImGui::BeginCombo("Anisotropic filtering", label)) {
        for (const Aniso& a : levels)
            if (ImGui::Selectable(a.label, a.value == s.aniso)) s.aniso = a.value;
        ImGui::EndCombo();
    }
    ImGui::Spacing();
    ImGui::Checkbox("Load replacement textures from textures\\replacement", &s.custom_textures);
    ImGui::Checkbox("Dump the game's textures once to textures\\dumps", &s.dump_textures);
    if (s.dump_textures) {
        const char* formats[] = {"PNG", "DDS"};
        int format = int(std::min(s.texture_dump_format, 1u));
        if (ImGui::Combo("Dump format", &format, formats, 2)) s.texture_dump_format = uint32_t(format);
    }
}

}  // namespace app::ui
