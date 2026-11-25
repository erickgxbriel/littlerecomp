// The in-game options overlay: F1 brings up the same display, controls and advanced settings the launcher has, plus a
// tab for the game itself. The game's pads are held while it is up, and the settings are saved when it closes.
#include "app/app.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>

#include <imgui.h>

#include "core/log.h"

namespace app {

namespace {

const ImVec4 kAccent(0.569f, 0.620f, 0.510f, 1.0f);

std::atomic<bool> g_open{false};
std::atomic<bool> g_focus{false};

void SetOverlay(bool open) {
    if (g_open.exchange(open, std::memory_order_relaxed) == open) return;
    if (open) {
        g_focus.store(true, std::memory_order_relaxed);
        LOGI("Overlay: opened");
    } else {
        Save();
        LOGI("Overlay: closed, settings saved");
    }
    // the keyboard and mouse are hidden by the game while it plays: ask for the cursor back now
    if (HWND hwnd = static_cast<HWND>(Window()))
        PostMessageW(hwnd, WM_SETCURSOR, reinterpret_cast<WPARAM>(hwnd), MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
}

void GameTab() {
    Settings& s = Get();
    ImGui::TextUnformatted("This session's game");
    ImGui::TextWrapped("%s", s.game.empty() ? "(started from the folder beside the executable)" : s.game.c_str());
    ImGui::Spacing();
    ImGui::Checkbox("Show the launcher at start", &s.launcher);
    ImGui::TextDisabled("--no-launcher skips it, and --launcher shows it whatever this is set to.");
    ImGui::Spacing();
    ImGui::Checkbox("Skip the intro videos", &s.skip_intro);
    ImGui::TextDisabled("Starts at the menu without the publisher logos.");
    ImGui::Spacing();
    ImGui::Checkbox("Unlock all cheats", &s.all_cheats_unlocked);
    ImGui::TextDisabled("Every pause-menu cheat code is selectable from a new game; effects are still chosen manually.");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextWrapped("This game's own settings, such as the keyboard and mouse bindings, are in the Controls tab. "
                       "Changes made here are kept in settings.toml beside the game.");
}

}  // namespace

bool OverlayOpen() { return g_open.load(std::memory_order_relaxed); }

void ToggleOverlay() { SetOverlay(!g_open.load(std::memory_order_relaxed)); }

void Overlay(double fps) {
    HWND hwnd = static_cast<HWND>(Window());
    float unit = hwnd ? float(GetDpiForWindow(hwnd)) / 96.0f : 1.0f;

    // the optional frame rate counter sits in the corner whether or not the panel is up
    if (Get().show_fps) {
        char text[32];
        std::snprintf(text, sizeof(text), "%.0f fps", fps);
        ImVec2 at(12.0f * unit, 10.0f * unit);
        ImDrawList* draw = ImGui::GetForegroundDrawList();
        draw->AddText(ImVec2(at.x + 1.0f, at.y + 1.0f), IM_COL32(0, 0, 0, 190), text);
        draw->AddText(at, IM_COL32(235, 240, 226, 255), text);
    }

    if (!OverlayOpen()) return;
    ImGuiIO& io = ImGui::GetIO();

    // the backdrop dims the game and takes the mouse, so a click beside the panel cannot reach the game
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::Begin("##options-backdrop", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoBackground |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 150));
    ImGui::End();
    ImGui::PopStyleVar(2);

    float width = std::min(720.0f * unit, std::max(io.DisplaySize.x - 40.0f, 320.0f));
    float height = std::min(540.0f * unit, std::max(io.DisplaySize.y - 40.0f, 280.0f));
    ImGui::SetNextWindowPos(ImVec2((io.DisplaySize.x - width) * 0.5f, (io.DisplaySize.y - height) * 0.5f));
    ImGui::SetNextWindowSize(ImVec2(width, height));
    if (g_focus.exchange(false, std::memory_order_relaxed)) ImGui::SetNextWindowFocus();

    bool open = true, close = false;
    ImGui::Begin("Options", &open,
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove);

    ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
    ImGui::TextUnformatted("Options");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextDisabled("%.0f fps", fps);
    ImGui::Spacing();

    float footer = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y * 2;
    ImGui::BeginChild("##options-body", ImVec2(0, -footer), false);
    if (ImGui::BeginTabBar("##options-tabs")) {
        if (ImGui::BeginTabItem("Display")) {
            ui::Display(Get(), true);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Controls")) {
            ui::InputOptions();
            ui::BindingEditor();
            ui::ModernControls();
            ui::ControllerNote();
            ui::KeyBindings();
            ImGui::EndTabItem();
        }
        if (kGame.title_id && *kGame.title_id && ImGui::BeginTabItem("Saves")) {
            saves::Ui(true);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Game")) {
            GameTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Advanced")) {
            ui::Advanced(Get());
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();

    ImGui::TextDisabled("Changes are saved when these options close. F1 or Esc closes them.");
    ImGui::SameLine();
    float button = 160.0f * unit;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - button);
    if (ImGui::Button("Save and resume", ImVec2(button, 0))) close = true;

    ImGui::End();
    bool capturing = kGame.binding_capture_active && kGame.binding_capture_active();
    if (!open || close || (!capturing && ImGui::IsKeyPressed(ImGuiKey_Escape, false))) SetOverlay(false);
}

}  // namespace app
