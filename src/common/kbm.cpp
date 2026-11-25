// Keyboard and mouse as an original Xbox pad. Defaults follow the official PC manual:
// On foot: WASD move, mouse look, Space/LMB fire, F/MMB alt fire, R reload, Shift sprint, Ctrl crouch,
// Enter wall cover, Caps/RMB lock-on, C/Home recenter, V rage, T taunt/provoke, E interact, 1/2 weapons, 3 holster,
// Tab SAT phone, Esc pause, mouse wheel cycle weapons. Arrow keys stand in for the pad's d-pad.
// Vehicles: W/P gas, S/;/R brake/reverse, A/D steer, Space handbrake, LMB/Ctrl/F fire, Caps/RMB lock,
// Z/MMB look back, C camera view, E enter/exit, T horn/taunt.
#include "common/kbm.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>

#include <imgui.h>

#include "app/app.h"
#include "core/input.h"
#include "core/log.h"
#include "game/hud_prompts.h"
#include "gpu/texturedumper.h"
#include "kernel/memory.h"

namespace kbm {

bool enabled = false;  // gamepad is the default; keyboard and mouse are opt-in
bool invert_y = false;
uint32_t sensitivity_x = 100;  // percent of the base look speed
uint32_t sensitivity_y = 100;  // percent of the base look speed
bool modern_pad = false;
bool hud_prompts = false;  // experimental; the original controller glyphs are the default

// Private diagnostic sessions only. This is deliberately not a settings option or a UI toggle: a normal player never
// sees it, and it cannot be persisted in settings.toml. The environment variable must be exactly "1".
bool DiagnosticCommandsEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("XBE_DIAGNOSTIC_SESSION");
        return value && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

namespace {

// The pad report gets read from guest threads; the window thread writes it.
std::mutex g_lock;
bool g_active = true;
bool g_keys[256] = {};
bool g_lmb = false, g_rmb = false, g_mmb = false;
int g_dx = 0, g_dy = 0;
int g_wheel = 0;
bool g_cursor_shown = true;
bool g_escape_latched = false;  // one virtual Start/B press per physical Esc press
void* g_hwnd = nullptr;  // the game window, for posting WM_SETCURSOR from the game thread
std::atomic<Device> g_device{Device::None};

// Diagnostic console: the window thread queues a command; a guest thread takes it and runs it through the game's own
// script evaluator. A version counter avoids holding a lock on the hot guest path when no command is pending.
std::atomic<bool> g_diagnostic_open{false};
std::atomic<bool> g_diagnostic_focus{false};  // set by F8 on the window thread, consumed by the render thread
std::mutex g_diagnostic_lock;
uint32_t g_diagnostic_version = 0;
std::atomic<uint32_t> g_diagnostic_queued{0};
std::string g_diagnostic_command;
char g_diagnostic_input[256] = "SGS_ActivateQueue(2,1);";

// Right stick units per mouse pixel at 100%. A flick across the screen is a full deflection.
constexpr int kBaseLook = 650;

// Xbox pad report bits
constexpr uint16_t kDpadUp = 1 << 0, kDpadDown = 1 << 1, kDpadLeft = 1 << 2, kDpadRight = 1 << 3;
constexpr uint16_t kStart = 1 << 4, kBack = 1 << 5, kLeftThumb = 1 << 6, kRightThumb = 1 << 7;
// analog[]: A B X Y black white left trigger right trigger
constexpr int kA = 0, kB = 1, kX = 2, kY = 3, kBlack = 4, kWhite = 5, kLT = 6, kRT = 7;

constexpr size_t kBindingCount = size_t(Binding::Count);

// The keyboard defaults follow the original Xbox pad layout, not the PC manual's context-sensitive shortcuts. That
// keeps every decision inside the game: a bound A key always sends A, in a menu, on foot and in a vehicle alike.
const std::array<uint32_t, kBindingCount> kDefaultBindings = {
    'W',       'S',       'A',       'D',       // left stick
    VK_UP,     VK_DOWN,   VK_LEFT,   VK_RIGHT,  // d-pad
    VK_SPACE,  'T',       'R',       'E',       // A, B, X, Y
    VK_TAB,    'F',       VK_CAPITAL, kMouseLmb,// LB, RB, LT, RT
    VK_BACK,   VK_ESCAPE, VK_LCONTROL, 'C',     // Back, Start, L3, R3
};
static_assert(kDefaultBindings.size() == kBindingCount);

// Guest threads read these every pad poll while the window thread writes them in the editor.
std::atomic<uint32_t> g_bindings[kBindingCount];
struct BindingInitializer {
    BindingInitializer() {
        for (size_t i = 0; i < kBindingCount; ++i) g_bindings[i].store(kDefaultBindings[i]);
    }
} g_binding_initializer;
size_t g_capture_binding = kBindingCount;  // window-thread only, like the ImGui editor that sets it

const char* const kBindingLabels[kBindingCount] = {
    "Left stick up",       "Left stick down",   "Left stick left",    "Left stick right",
    "D-pad up",            "D-pad down",        "D-pad left",         "D-pad right",
    "A",                   "B",                 "X",                  "Y",
    "LB (white)",          "RB (black)",        "LT",                 "RT",
    "Back",                "Start",             "L3 (left stick)",    "R3 (right stick)",
};

const char* const kBindingSettingNames[kBindingCount] = {
    "move_up",   "move_down", "move_left",  "move_right", "dpad_up",    "dpad_down",
    "dpad_left", "dpad_right", "face_a",    "face_b",     "face_x",     "face_y",
    "lb",        "rb",        "lt",         "rt",         "back",       "start",
    "l3",        "r3",
};

struct KeyName {
    uint32_t code;
    const char* name;
};

// Symbolic names in [keybindings]; the mouse codes are above the virtual-key range.
const KeyName kKeyNames[] = {
    {'A', "A"}, {'B', "B"}, {'C', "C"}, {'D', "D"}, {'E', "E"}, {'F', "F"}, {'G', "G"}, {'H', "H"},
    {'I', "I"}, {'J', "J"}, {'K', "K"}, {'L', "L"}, {'M', "M"}, {'N', "N"}, {'O', "O"}, {'P', "P"},
    {'Q', "Q"}, {'R', "R"}, {'S', "S"}, {'T', "T"}, {'U', "U"}, {'V', "V"}, {'W', "W"}, {'X', "X"},
    {'Y', "Y"}, {'Z', "Z"},
    {'0', "0"}, {'1', "1"}, {'2', "2"}, {'3', "3"}, {'4', "4"}, {'5', "5"}, {'6', "6"}, {'7', "7"},
    {'8', "8"}, {'9', "9"},
    {VK_SPACE, "SPACE"}, {VK_TAB, "TAB"}, {VK_RETURN, "ENTER"}, {VK_ESCAPE, "ESC"}, {VK_BACK, "BACK"},
    {VK_SHIFT, "SHIFT"}, {VK_LSHIFT, "LSHIFT"}, {VK_RSHIFT, "RSHIFT"},
    {VK_CONTROL, "CTRL"}, {VK_LCONTROL, "LCTRL"}, {VK_RCONTROL, "RCTRL"}, {VK_CAPITAL, "CAPS"},
    {VK_MENU, "ALT"}, {VK_LMENU, "LALT"}, {VK_RMENU, "RALT"},
    {VK_UP, "UP"}, {VK_DOWN, "DOWN"}, {VK_LEFT, "LEFT"}, {VK_RIGHT, "RIGHT"},
    {VK_PRIOR, "PGUP"}, {VK_NEXT, "PGDN"}, {VK_HOME, "HOME"}, {VK_END, "END"}, {VK_INSERT, "INS"},
    {VK_DELETE, "DEL"}, {VK_SNAPSHOT, "PRTSC"}, {VK_PAUSE, "PAUSE"},
    {VK_OEM_1, "SEMICOLON"}, {VK_OEM_2, "SLASH"}, {VK_OEM_3, "TILDE"}, {VK_OEM_4, "LBRACKET"},
    {VK_OEM_5, "BACKSLASH"}, {VK_OEM_6, "RBRACKET"}, {VK_OEM_7, "APOSTROPHE"}, {VK_OEM_PLUS, "PLUS"},
    {VK_OEM_MINUS, "MINUS"}, {VK_OEM_COMMA, "COMMA"}, {VK_OEM_PERIOD, "PERIOD"},
    {VK_NUMPAD0, "NUM0"}, {VK_NUMPAD1, "NUM1"}, {VK_NUMPAD2, "NUM2"}, {VK_NUMPAD3, "NUM3"},
    {VK_NUMPAD4, "NUM4"}, {VK_NUMPAD5, "NUM5"}, {VK_NUMPAD6, "NUM6"}, {VK_NUMPAD7, "NUM7"},
    {VK_NUMPAD8, "NUM8"}, {VK_NUMPAD9, "NUM9"}, {VK_ADD, "NUMPLUS"}, {VK_SUBTRACT, "NUMMINUS"},
    {kMouseLmb, "LMB"}, {kMouseRmb, "RMB"}, {kMouseMmb, "MMB"},
};

bool Held(int vk) { return g_keys[vk & 0xFF]; }

bool InputHeld(uint32_t code) {
    if (!code) return false;
    if (code == kMouseLmb) return g_lmb;
    if (code == kMouseRmb) return g_rmb;
    if (code == kMouseMmb) return g_mmb;
    if (code >= kMouseKeyBase || code >= 256) return false;
    return Held(int(code));
}

uint32_t KeyCodeFromNameImpl(const char* name) {
    if (!name || !*name) return 0;
    for (const KeyName& entry : kKeyNames)
        if (::_stricmp(name, entry.name) == 0) return entry.code;
    if (::_strnicmp(name, "VK_", 3) == 0) {
        unsigned value = 0;
        if (std::sscanf(name + 3, "%X", &value) == 1 && value >= 1 && value < 256) return uint32_t(value);
    }
    return 0;
}

const char* KeyCodeNameImpl(uint32_t code) {
    if (!code) return "Unbound";
    for (const KeyName& entry : kKeyNames)
        if (entry.code == code) return entry.name;
    // The capture accepts any virtual key. Keep one stable buffer per code so a whole table can be drawn in a frame.
    static char names[256][12] = {};
    char* text = names[code & 0xFF];
    std::snprintf(text, 12, "VK_%02X", code & 0xFF);
    return text;
}

int16_t AddStick(int16_t a, int16_t b) {
    return int16_t(std::clamp(int(a) + int(b), -32768, 32767));
}

// Optional GTA-style driving on a physical pad: the triggers are the pedals, the bumpers take over the
// trigger actions, A is the handbrake, B is the horn, and L3 locks on. X stays a secondary brake so the
// classic scheme still works. Back opens the SAT phone. On foot the original layout already follows the
// same twin-stick conventions, so nothing changes there. Called before any keyboard/mouse is merged in.
void ModernDriving(input::Pad& pad) {
    uint8_t gas = pad.analog[kRT], brake = pad.analog[kLT];
    uint8_t fire = pad.analog[kBlack], look = pad.analog[kWhite];
    uint8_t handbrake = pad.analog[kA], horn = pad.analog[kB];
    // The game has no separate horn button: in the Xbox pad report the horn is the same A button as the
    // accelerator. Pressing B therefore also gives a short throttle input; it is unavoidable without
    // changing the guest.
    pad.analog[kA] = std::max(gas, horn);  // RT accelerates, B sounds the horn
    pad.analog[kX] = std::max(pad.analog[kX], brake);
    pad.analog[kB] = handbrake;                               // A handbrakes
    pad.analog[kRT] = fire;                                   // bumper fires
    pad.analog[kBlack] = look;                                // other bumper looks back
    pad.analog[kLT] = (pad.buttons & kLeftThumb) ? 0xFF : 0;  // L3 locks on
    pad.analog[kWhite] = (pad.buttons & kBack) ? 0xFF : 0;    // Back opens the SAT phone
}

// did a real controller do anything since the last poll? stick drift and trigger noise are ignored
bool PadActive(const input::Pad& pad) {
    if (pad.buttons) return true;
    for (uint8_t a : pad.analog)
        if (a > 8) return true;
    return std::abs(int(pad.lx)) > 4000 || std::abs(int(pad.ly)) > 4000 || std::abs(int(pad.rx)) > 4000 ||
           std::abs(int(pad.ry)) > 4000;
}

// one of the bound keys is held, or the mouse moved or was clicked
bool KbmActive() {
    for (size_t i = 0; i < kBindingCount; ++i)
        if (InputHeld(g_bindings[i].load(std::memory_order_relaxed))) return true;
    return g_lmb || g_rmb || g_mmb || g_dx || g_dy || g_wheel;
}

// What the game's ActionMap stack says about the screen it is on.
struct ActionMaps {
    bool vehicle = false;       // a driving map is on top: steering, not walking
    bool menu = false;          // a front end map is on top: Esc backs out of a menu there, it does not pause
    bool dialog = false;        // a system dialog is on top: Enter dismisses it, like the pad's B
    bool look_tutorial = false; // the tutorial overlay remaps Enter to the pad's Start and Esc to B
};

// Reads the game's ActionMap stack.
// The list head is stored at guest VA 0x471468.
// Each ActionMap node: +0x04 name pointer (const char*), +0x20 prev, +0x24 next, +0x2A active flag (uint8_t).
ActionMaps ReadActionMaps() {
    ActionMaps maps;
    uint8_t* base = mem::Base();
    if (!base) return maps;

    constexpr uint32_t kHeadVA = 0x471468;
    if (!mem::IsCommitted(kHeadVA)) return maps;

    uint32_t node_va = *reinterpret_cast<const uint32_t*>(base + kHeadVA);
    static char s_last_top[64] = {};

    char top_name[64] = {};

    for (int i = 0; i < 32 && node_va != 0; ++i) {
        if (!mem::IsCommitted(node_va) || !mem::IsCommitted(node_va + 0x34)) break;

        uint8_t active = *(base + node_va + 0x2A);
        uint32_t next_va = *reinterpret_cast<const uint32_t*>(base + node_va + 0x24);
        uint32_t name_va = *reinterpret_cast<const uint32_t*>(base + node_va + 0x04);

        if (active && name_va != 0 && mem::IsCommitted(name_va)) {
            char name[64] = {};
            for (size_t c = 0; c < sizeof(name) - 1; ++c) {
                uint32_t char_va = name_va + static_cast<uint32_t>(c);
                if ((char_va & 0xFFF) == 0 && !mem::IsCommitted(char_va)) break;
                char ch = static_cast<char>(base[char_va]);
                name[c] = ch;
                if (ch == '\0') break;
            }
            name[sizeof(name) - 1] = '\0';

            if (top_name[0] == '\0') {
                std::strncpy(top_name, name, sizeof(top_name) - 1);
            }

            if (std::strstr(name, "HUDLookTutorialActionMap")) maps.look_tutorial = true;

            if (std::strstr(name, "WheeledVehicleActionMap") ||
                std::strstr(name, "BoatActionMap") ||
                std::strstr(name, "HelicopterActionMap") ||
                std::strstr(name, "AirplaneActionMap")) {
                maps.vehicle = true;
                break;
            }
            if (std::strstr(name, "CharacterActionMap")) {
                maps.vehicle = false;
                break;
            }
            // Front-end screens push their own maps, but only the topmost map decides how Esc and Enter behave.
            // Do not set maps.menu here: PauseActionMap / UnpauseActionMap can remain below CharacterActionMap,
            // and treating that as a menu would send pad B (taunt) when Esc should send Start (pause).
        }
        node_va = next_va;
    }

    // Only the topmost map decides how Esc and Enter behave. PauseActionMap and its friends make Esc the pad's
    // back (B); in the world it must stay Start. SystemDialogActionMap is pushed for notices such as
    // "Load profile successful"; the game dismisses those with B (V on the PC bindings), not A, so Enter sends
    // B there too. Checking the top avoids leaking those bindings into play when such a map is still in the stack.
    maps.menu = std::strstr(top_name, "Menu") != nullptr || std::strstr(top_name, "FrontEnd") != nullptr ||
                std::strstr(top_name, "Options") != nullptr || std::strstr(top_name, "Pause") != nullptr ||
                std::strstr(top_name, "Dialog") != nullptr;
    maps.dialog = std::strstr(top_name, "Dialog") != nullptr;

    if (top_name[0] != '\0' && std::strncmp(s_last_top, top_name, sizeof(s_last_top)) != 0) {
        std::strncpy(s_last_top, top_name, sizeof(s_last_top) - 1);
        LOGI("kbm: top action map '%s' (in_vehicle=%d menu=%d dialog=%d look_tutorial=%d)", s_last_top, maps.vehicle,
             maps.menu, maps.dialog, maps.look_tutorial);
    }

    return maps;
}

// The game hides the cursor while it plays. WM_SETCURSOR applies the wanted state on the window thread, so this
// only remembers it and posts that message when it changes.
void WantCursor(bool show) {
    if (show == g_cursor_shown) return;
    g_cursor_shown = show;
    // WM_SETCURSOR only arrives on mouse activity: ask the window thread to apply the change now, so a pad only
    // player does not keep a stale arrow on screen after the overlay closes
    if (g_hwnd)
        PostMessageW(static_cast<HWND>(g_hwnd), WM_SETCURSOR, reinterpret_cast<WPARAM>(g_hwnd),
                     MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
}

void UpdateCursor() {
    // the game owns the cursor while focused; the options overlay takes it back
    bool overlay = app::OverlayOpen() || DiagnosticCommandsOpen();
    if (!overlay && ImGui::GetCurrentContext()) overlay = ImGui::GetIO().WantCaptureMouse;
    WantCursor(!g_active || overlay);
}

// Hiding the OS cursor is not enough to keep it in the game: on a multi-monitor desktop it can slide onto the other
// screen, where a click takes focus away and raw mouse input stops. Clip it to the window's client area while the game
// owns the mouse. ClipCursor applies to the calling thread, so only the window thread calls this; the game thread asks
// for it through WM_SETCURSOR, posted by WantCursor when the cursor state changes.
void ApplyCursorClip(HWND hwnd) {
    if (!hwnd || g_cursor_shown || !g_active || ::IsIconic(hwnd)) {
        ::ClipCursor(nullptr);
        return;
    }
    RECT client{};
    if (!::GetClientRect(hwnd, &client) || client.right <= client.left || client.bottom <= client.top) return;
    POINT tl{client.left, client.top}, br{client.right, client.bottom};
    ::ClientToScreen(hwnd, &tl);
    ::ClientToScreen(hwnd, &br);
    RECT clip{tl.x, tl.y, br.x, br.y};
    ::ClipCursor(&clip);
}

void ClearInput() {
    for (bool& k : g_keys) k = false;
    g_lmb = g_rmb = g_mmb = false;
    g_dx = g_dy = 0;
    g_wheel = 0;
}

}  // namespace

namespace {

void QueueDiagnosticCommand(const char* command) {
    if (!command || !*command) return;
    std::lock_guard lock(g_diagnostic_lock);
    g_diagnostic_command = command;
    if (g_diagnostic_command.size() >= 256) g_diagnostic_command.resize(255);
    ++g_diagnostic_version;
    if (!g_diagnostic_version) ++g_diagnostic_version;  // 0 means none queued
    g_diagnostic_queued.store(g_diagnostic_version, std::memory_order_release);
}

// A diagnostic harness can put the first command in this file instead of synthesizing text into the ImGui field. The
// console still has to be opened and triggered with F8/F9, so nothing runs without an explicit in-game action.
void LoadDiagnosticCommandFile() {
    std::ifstream file(app::ExeDir() / "diagnostic_command.txt");
    std::string line;
    if (!std::getline(file, line)) return;
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    if (line.empty()) return;
    if (line.size() >= sizeof(g_diagnostic_input)) line.resize(sizeof(g_diagnostic_input) - 1);
    {
        std::lock_guard lock(g_diagnostic_lock);
        std::memcpy(g_diagnostic_input, line.c_str(), line.size() + 1);
    }
    LOGI("diagnostic: initial command from diagnostic_command.txt: %s", line.c_str());
}

void ToggleDiagnosticCommands() {
    bool open = g_diagnostic_open.load(std::memory_order_relaxed);
    g_diagnostic_open.store(!open, std::memory_order_relaxed);
    g_diagnostic_focus.store(!open, std::memory_order_relaxed);
}

}  // namespace

bool DiagnosticCommandsOpen() { return g_diagnostic_open.load(std::memory_order_relaxed); }

bool TakeDiagnosticCommand(char (&command)[256]) {
    uint32_t version = g_diagnostic_queued.load(std::memory_order_acquire);
    if (!version) return false;
    std::lock_guard lock(g_diagnostic_lock);
    if (version != g_diagnostic_queued.load(std::memory_order_relaxed)) {
        // A newer command was queued between the atomic read and the lock; take that one on the next expansion.
        return false;
    }
    if (g_diagnostic_command.empty() || g_diagnostic_command.size() >= sizeof(command)) {
        g_diagnostic_queued.store(0, std::memory_order_release);
        return false;
    }
    std::memcpy(command, g_diagnostic_command.c_str(), g_diagnostic_command.size() + 1);
    g_diagnostic_queued.store(0, std::memory_order_release);
    return true;
}

size_t BindingCount() { return kBindingCount; }

uint32_t KeyCodeFromName(const char* name) { return KeyCodeFromNameImpl(name); }

const char* KeyCodeName(uint32_t code) { return KeyCodeNameImpl(code); }

const char* BindingSettingName(size_t index) {
    return index < kBindingCount ? kBindingSettingNames[index] : "";
}

void DescribeBinding(size_t index, const char** pad, const char** key, bool* changed) {
    if (index >= kBindingCount) return;
    if (pad) *pad = kBindingLabels[index];
    if (key) *key = KeyCodeNameImpl(g_bindings[index].load(std::memory_order_relaxed));
    if (changed) *changed = g_bindings[index].load(std::memory_order_relaxed) != kDefaultBindings[index];
}

void BeginBindingCapture(size_t index) {
    if (index >= kBindingCount) return;
    g_capture_binding = index;
    ClearInput();
}

bool BindingCaptureActive() { return g_capture_binding < kBindingCount; }

void CancelBindingCapture() { g_capture_binding = kBindingCount; }

void ResetBinding(size_t index) {
    if (index >= kBindingCount) return;
    g_bindings[index].store(kDefaultBindings[index], std::memory_order_relaxed);
    CancelBindingCapture();
    ClearInput();
    hud_prompts::RefreshKeyLabels();
}

void ResetAllBindings() {
    for (size_t i = 0; i < kBindingCount; ++i)
        g_bindings[i].store(kDefaultBindings[i], std::memory_order_relaxed);
    CancelBindingCapture();
    ClearInput();
    hud_prompts::RefreshKeyLabels();
}

uint32_t BindingKeyCode(Binding binding) {
    size_t index = size_t(binding);
    return index < kBindingCount ? g_bindings[index].load(std::memory_order_relaxed) : 0;
}

bool SetBindingKeyCode(Binding binding, uint32_t code) {
    size_t index = size_t(binding);
    if (index >= kBindingCount) return false;
    if (code && code != kMouseLmb && code != kMouseRmb && code != kMouseMmb && code >= 256) return false;
    g_bindings[index].store(code, std::memory_order_relaxed);
    CancelBindingCapture();
    ClearInput();
    hud_prompts::RefreshKeyLabels();
    return true;
}

void Ui(double fps) {
    app::Overlay(fps);
    if (!DiagnosticCommandsEnabled()) {
        g_diagnostic_open.store(false, std::memory_order_relaxed);
        g_diagnostic_focus.store(false, std::memory_order_relaxed);
        return;
    }
    if (!g_diagnostic_open.load(std::memory_order_relaxed)) return;

    char input[sizeof(g_diagnostic_input)];
    {
        std::lock_guard lock(g_diagnostic_lock);
        std::memcpy(input, g_diagnostic_input, sizeof(input));
    }

    ImGui::SetNextWindowSize(ImVec2(560.0f, 170.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Diagnostic script command")) {
        ImGui::End();
        return;
    }
    ImGui::TextWrapped("Runs on the next in-game text update. Guest commands can change or overwrite saves.");
    if (g_diagnostic_focus.exchange(false, std::memory_order_relaxed)) ImGui::SetKeyboardFocusHere();
    ImGui::InputText("Command", input, sizeof(input));
    bool execute = ImGui::Button("Execute");
    ImGui::SameLine();
    bool close = ImGui::Button("Close");
    {
        std::lock_guard lock(g_diagnostic_lock);
        std::memcpy(g_diagnostic_input, input, sizeof(input));
    }
    if (execute) QueueDiagnosticCommand(input);
    if (close) g_diagnostic_open.store(false, std::memory_order_relaxed);
    ImGui::End();
}

void Window(void* hwnd) {
    g_hwnd = hwnd;
    if (DiagnosticCommandsEnabled()) LoadDiagnosticCommandFile();
    RAWINPUTDEVICE rid{};
    rid.usUsagePage = 0x01;  // generic desktop
    rid.usUsage = 0x02;      // mouse
    rid.dwFlags = 0;         // only while the window is focused
    rid.hwndTarget = static_cast<HWND>(hwnd);
    RegisterRawInputDevices(&rid, 1, sizeof(rid));
}

bool Message(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result) {
    switch (msg) {
        case WM_INPUT: {
            RAWINPUT ri{};
            UINT size = sizeof(ri);
            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) !=
                    UINT(-1) &&
                ri.header.dwType == RIM_TYPEMOUSE && !(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
                std::lock_guard lock(g_lock);
                g_dx += ri.data.mouse.lLastX;
                g_dy += ri.data.mouse.lLastY;
            }
            return false;
        }
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (g_capture_binding < kBindingCount) {
                // The binding editor owns the next press. Esc cancels; a repeat is ignored so one press is one binding.
                if (!(lp & (intptr_t(1) << 30))) {
                    if (wp == VK_ESCAPE) CancelBindingCapture();
                    else if (wp < 256)
                        SetBindingKeyCode(Binding(g_capture_binding), uint32_t(wp));
                }
                return true;
            }
            if (wp < 256) {
                std::lock_guard lock(g_lock);
                g_keys[wp] = true;
            }
            // In a diagnostic session only, F8 opens the script-command console and F9 executes what is in it. The
            // runtime's F1/F2/F3 keys stay untouched. The auto-repeat guard keeps one command per press.
            if (DiagnosticCommandsEnabled() && !(lp & (intptr_t(1) << 30))) {
                if (wp == VK_F8) {
                    ToggleDiagnosticCommands();
                    return true;
                }
                if (wp == VK_F9 && g_diagnostic_open.load(std::memory_order_relaxed)) {
                    char command[sizeof(g_diagnostic_input)];
                    {
                        std::lock_guard lock(g_diagnostic_lock);
                        std::memcpy(command, g_diagnostic_input, sizeof(command));
                    }
                    QueueDiagnosticCommand(command);
                    return true;
                }
            }
            return false;  // the runtime's own keys (F3) still work
        case WM_KEYUP:
        case WM_SYSKEYUP:
            if (g_capture_binding < kBindingCount) return true;
            if (wp < 256) {
                std::lock_guard lock(g_lock);
                g_keys[wp] = false;
            }
            return false;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP: {
            if (g_capture_binding < kBindingCount) {
                bool down = msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN;
                if (down) {
                    uint32_t code = msg == WM_LBUTTONDOWN ? kMouseLmb
                                    : msg == WM_RBUTTONDOWN ? kMouseRmb
                                                            : kMouseMmb;
                    SetBindingKeyCode(Binding(g_capture_binding), code);
                }
                return true;
            }
            std::lock_guard lock(g_lock);
            if (msg == WM_LBUTTONDOWN) g_lmb = true;
            if (msg == WM_LBUTTONUP) g_lmb = false;
            if (msg == WM_RBUTTONDOWN) g_rmb = true;
            if (msg == WM_RBUTTONUP) g_rmb = false;
            if (msg == WM_MBUTTONDOWN) g_mmb = true;
            if (msg == WM_MBUTTONUP) g_mmb = false;
            return false;
        }
        case WM_MOUSEWHEEL: {
            std::lock_guard lock(g_lock);
            g_wheel += GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
            return false;
        }
        case WM_ACTIVATE:
            {
                std::lock_guard lock(g_lock);
                g_active = LOWORD(wp) != WA_INACTIVE;
                if (!g_active) ClearInput();
            }
            UpdateCursor();
            return false;
        case WM_SETCURSOR:
            UpdateCursor();
            // ImGui is told not to change the OS cursor, so this is now the only place that sets it: hidden
            // during play, plain arrow for the options overlay and whenever the window is not active
            ::SetCursor(g_cursor_shown ? ::LoadCursor(nullptr, IDC_ARROW) : nullptr);
            ApplyCursorClip(static_cast<HWND>(hwnd));
            if (result) *result = TRUE;
            return true;
        case WM_SIZE:
        case WM_MOVE:
            // the clip is an absolute screen rectangle, so it has to follow the window
            ApplyCursorClip(static_cast<HWND>(hwnd));
            return false;
        case WM_DESTROY:
            ::ClipCursor(nullptr);
            return false;
        default:
            return false;
    }
}

void Pad(int port, input::Pad& pad) {
    if (port != 0) return;
    // the overlay can open or close between two mouse messages: keep the cursor state in step every poll
    UpdateCursor();
    // the options overlay and diagnostic console have the keyboard and mouse while up: a held key must not reach the
    // game, and a mouse movement made in them must not turn into a look when they close
    if (app::OverlayOpen() || DiagnosticCommandsOpen()) {
        std::lock_guard lock(g_lock);
        ClearInput();
        pad.connected = true;
        pad.buttons = 0;
        for (uint8_t& a : pad.analog) a = 0;
        pad.lx = pad.ly = pad.rx = pad.ry = 0;
        g_escape_latched = false;
        return;
    }
    // Remap only the physical pad, before the keyboard/mouse early-out so pad-only players get it too,
    // and before any synthesized input is added. The overlay case above already zeroes everything.
    if (modern_pad && ReadActionMaps().vehicle) ModernDriving(pad);

    std::lock_guard lock(g_lock);
    if (!enabled || !g_active) {
        // do not keep a look delta: turning the keyboard back on must not snap the camera
        g_dx = g_dy = g_wheel = 0;
        g_escape_latched = false;
        return;
    }

    bool pad_active = PadActive(pad);
    bool kbm_active = KbmActive();

    // Fixed emulation: the left stick and every pad button come straight from the binding table. The game decides
    // what each pad control means on the current screen, so no key is reinterpreted here by ActionMap.
    int16_t kx = 0, ky = 0;
    float mx = float(InputHeld(BindingKeyCode(Binding::MoveRight))) -
               float(InputHeld(BindingKeyCode(Binding::MoveLeft)));
    float my = float(InputHeld(BindingKeyCode(Binding::MoveUp))) -
               float(InputHeld(BindingKeyCode(Binding::MoveDown)));
    if (float len = std::sqrt(mx * mx + my * my); len > 0) {
        kx = int16_t(std::lround(mx / len * 32767.0f));
        ky = int16_t(std::lround(my / len * 32767.0f));
    }

    // Mouse look is always right-stick emulation and is not part of the binding table.
    int16_t krx = 0, kry = 0;
    int dx = g_dx, dy = g_dy;
    g_dx = g_dy = 0;
    if (dx || dy) {
        float sx = kBaseLook * (float(sensitivity_x) / 100.0f);
        float sy = kBaseLook * (float(sensitivity_y) / 100.0f);
        float rx = float(dx) * sx;
        float ry = invert_y ? float(dy) * sy : -float(dy) * sy;
        krx = int16_t(std::clamp<long>(std::lround(rx), -32768, 32767));
        kry = int16_t(std::clamp<long>(std::lround(ry), -32768, 32767));
    }

    uint16_t buttons = 0;
    uint8_t analog[8] = {};

    if (InputHeld(BindingKeyCode(Binding::DpadUp))) buttons |= kDpadUp;
    if (InputHeld(BindingKeyCode(Binding::DpadDown))) buttons |= kDpadDown;
    if (InputHeld(BindingKeyCode(Binding::DpadLeft))) buttons |= kDpadLeft;
    if (InputHeld(BindingKeyCode(Binding::DpadRight))) buttons |= kDpadRight;

    if (InputHeld(BindingKeyCode(Binding::A))) analog[kA] = 0xFF;
    if (InputHeld(BindingKeyCode(Binding::B))) analog[kB] = 0xFF;
    if (InputHeld(BindingKeyCode(Binding::X))) analog[kX] = 0xFF;
    if (InputHeld(BindingKeyCode(Binding::Y))) analog[kY] = 0xFF;
    if (InputHeld(BindingKeyCode(Binding::LB))) analog[kWhite] = 0xFF;
    if (InputHeld(BindingKeyCode(Binding::RB))) analog[kBlack] = 0xFF;
    if (InputHeld(BindingKeyCode(Binding::LT))) analog[kLT] = 0xFF;
    if (InputHeld(BindingKeyCode(Binding::RT))) analog[kRT] = 0xFF;

    if (InputHeld(BindingKeyCode(Binding::L3))) buttons |= kLeftThumb;
    if (InputHeld(BindingKeyCode(Binding::R3))) buttons |= kRightThumb;
    if (InputHeld(BindingKeyCode(Binding::Back))) buttons |= kBack;

    // Start is edge-triggered: a held key must not re-open and close the pause screen on successive polls.
    bool start = InputHeld(BindingKeyCode(Binding::Start));
    if (start && !g_escape_latched) buttons |= kStart;
    g_escape_latched = start;

    // The wheel no longer sends a pad control; clear it so it cannot linger as keyboard activity.
    g_wheel = 0;

    // either device can hold a button, the larger analog value wins, and the sticks add
    pad.connected = true;
    pad.buttons |= buttons;
    for (int i = 0; i < 8; i++) pad.analog[i] = std::max(pad.analog[i], analog[i]);
    pad.lx = AddStick(pad.lx, kx);
    pad.ly = AddStick(pad.ly, ky);
    pad.rx = AddStick(pad.rx, krx);
    pad.ry = AddStick(pad.ry, kry);

    bool feature_enabled = enabled && hud_prompts;
    if (kbm_active || pad_active) {
        Device now = kbm_active ? Device::KeyboardMouse : Device::Pad;
        if (g_device.exchange(now, std::memory_order_relaxed) != now) {
            LOGI("input: %s is driving", now == Device::KeyboardMouse ? "keyboard and mouse" : "controller");
            // The music library keeps the prompts it had on entry; other screens rewrite cached captions on a
            // device switch so a keyboard label never survives when a pad takes over.
            if (!hud_prompts::Frozen()) ApplyHudDeviceText(feature_enabled && now != Device::Pad);
        }
    }

    // Hide the pad artwork only while bracketed key prompts are driving the HUD. The music library uses the same
    // frozen decision as its text, so switching devices on that page does not half-convert its prompts.
    gpu::texturedumper::SetControllerGlyphOverride(hud_prompts::UseKeyLabels());
}

Device LastDevice() { return g_device.load(std::memory_order_relaxed); }

PromptContext PromptContextNow() {
    ActionMaps maps = ReadActionMaps();
    if (maps.menu) return PromptContext::Menu;
    if (maps.vehicle) return PromptContext::Vehicle;
    return PromptContext::OnFoot;
}

bool LookTutorialActive() { return ReadActionMaps().look_tutorial; }

void Rumble(int, uint16_t& left, uint16_t& right) {
    // A keyboard/mouse player has no pad in their hands: drop the motors so a connected controller does not
    // buzz on the desk. Because the last device to send input decides, picking the pad up restores rumble
    // as soon as a button or stick is used. Zero stops any motors still spinning from an earlier request.
    if (g_device.load(std::memory_order_relaxed) == Device::KeyboardMouse) {
        left = right = 0;
    }
}

}  // namespace kbm
