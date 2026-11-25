#include "app/app.h"

#include <algorithm>
#include <optional>
#include <string>
#include <toml++/toml.hpp>

#include "common/kbm.h"

namespace {

// Older builds had one mouse sensitivity. Carry it into both axes only when neither new key is present, so an
// existing settings file does not silently slow down. The old key is removed on the next save.
void LoadSettings(const void* table) {
    const toml::table& t = *static_cast<const toml::table*>(table);

    // A file written before keyboard/mouse was opt-in has no kbm key. If it has any other input key, its owner
    // deliberately used the keyboard layer; keep it on instead of silently switching them to gamepad-only.
    if (!t.contains("kbm") && (t.contains("kbm_invert_y") || t.contains("kbm_sensitivity") ||
                               t.contains("kbm_sensitivity_x") || t.contains("kbm_sensitivity_y") ||
                               t.contains("modern_pad") || t.contains("hud_key_prompts"))) {
        kbm::enabled = true;
    }

    if (const toml::table* bindings = t["keybindings"].as_table()) {
        for (size_t i = 0; i < kbm::BindingCount(); ++i) {
            std::optional<std::string> name = (*bindings)[kbm::BindingSettingName(i)].value<std::string>();
            if (!name) continue;
            uint32_t code = kbm::KeyCodeFromName(name->c_str());
            if (code) kbm::SetBindingKeyCode(kbm::Binding(i), code);
        }
    }

    if (t.contains("kbm_sensitivity_x") || t.contains("kbm_sensitivity_y")) return;
    if (std::optional<int64_t> old = t["kbm_sensitivity"].value<int64_t>()) {
        uint32_t both = uint32_t(std::clamp<int64_t>(*old, 10, 300));
        kbm::sensitivity_x = both;
        kbm::sensitivity_y = both;
    }
}

void SaveSettings(void* table) {
    toml::table& t = *static_cast<toml::table*>(table);
    t.erase("kbm_sensitivity");

    toml::table bindings;
    for (size_t i = 0; i < kbm::BindingCount(); ++i) {
        const char* pad = nullptr;
        const char* key = nullptr;
        kbm::DescribeBinding(i, &pad, &key, nullptr);
        bindings.insert(kbm::BindingSettingName(i), key ? key : "Unbound");
    }
    t.insert_or_assign("keybindings", std::move(bindings));
}

}  // namespace

// what the runtime asks of the game; strings must be set, hooks may stay null
const app::GameInfo app::kGame = {
    .title = L"Scarface: The World Is Yours",  // window and splash
    .short_title = L"Scarface",                // message boxes
    .name = L"Scarface",                       // window classes, crash dumps, handover event
    .log = "scarface",                         // log file prefix
    .settings = "settings.toml",               // settings file beside the exe
    .splash = L"splash",                       // optional splash.png or splash.bmp beside the exe
    .missing = L"Put the game in the assets folder: its disc image (.iso), or default.xbe with its folders beside it.",
    .images = {{"default.xbe", "Scarface"}},  // each recompiled xbe and its exe, for title launches
    .options = {{"kbm", &kbm::enabled, nullptr, "Keyboard and mouse (opt-in)"},
                {"kbm_invert_y", &kbm::invert_y, nullptr, "Invert mouse Y"},
                {"kbm_sensitivity_x", nullptr, &kbm::sensitivity_x, "Mouse sensitivity X", 10, 300},
                {"kbm_sensitivity_y", nullptr, &kbm::sensitivity_y, "Mouse sensitivity Y", 10, 300},
                {"modern_pad", &kbm::modern_pad, nullptr, "Experimental: Modern gamepad controls (GTA-style)"},
                {"hud_key_prompts", &kbm::hud_prompts, nullptr, "Experimental: PC key prompts in the HUD"}},
    // reference only: the editable table lives in kbm.cpp and is shown by ui::BindingEditor()
    .controls =
        {{"Movement and camera",
          {{"W A S D", "Left stick: move / steer"},
           {"Mouse", "Right stick: look / camera"},
           {"Arrow keys", "D-pad: menus and weapon selection"}}},
         {"Face buttons and triggers",
          {{"Space", "A: confirm, sprint, accelerate"},
           {"T", "B: back, taunt, handbrake"},
           {"R", "X: reload, brake"},
           {"E", "Y: interact, enter or exit a vehicle"},
           {"Caps Lock", "LT: lock on"},
           {"Left click", "RT: fire"},
           {"Tab", "LB: SAT phone"},
           {"F", "RB: alternate fire, look back"}}},
         {"Other pad controls",
          {{"Left Ctrl", "L3: crouch"},
           {"C", "R3: recentre or change camera"},
           {"Backspace", "Back"},
           {"Esc", "Start: pause"}}}},
    .binding_count = kbm::BindingCount,
    .binding_describe = kbm::DescribeBinding,
    .binding_capture = kbm::BeginBindingCapture,
    .binding_capture_active = kbm::BindingCaptureActive,
    .binding_capture_cancel = kbm::CancelBindingCapture,
    .binding_reset = kbm::ResetBinding,
    .binding_reset_all = kbm::ResetAllBindings,
    .load = LoadSettings,                     // migrates the old single sensitivity to both axes
    .save = SaveSettings,                     // drops the old key once it has been migrated
    .init = nullptr,                          // after the settings load, before the window and boot
    .loaded = nullptr,                        // image in memory, before its entry point runs
    .window = kbm::Window,                    // the game window is up
    .message = kbm::Message,                  // window messages first; true takes one
    .ui = kbm::Ui,                            // F1 options overlay
    .pad = kbm::Pad,                          // changes a polled pad
    .rumble = kbm::Rumble,                    // changes a rumble before it is set: quiet it for keyboard players
    .title_id = "5655003F",                   // E:\UDATA save folder
};
