// Thread-safe SDL3 physical gamepad input merged with Android multi-touch virtual controls.
// All SDL event pumping and gamepad device opens/reads happen exclusively on the SDL main thread;
// guest threads read a lock-protected snapshot via input::Poll().
#include "core/input.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <mutex>

#include <SDL3/SDL.h>

#include "android_app.h"
#include "app/app.h"
#include "core/log.h"
#include "touch_controls.h"

namespace kbm {
bool IsInVehicle();
}

namespace input {

namespace {

std::mutex g_lock;
SDL_Gamepad* g_pads[4] = {};
std::array<Pad, 4> g_hw_snapshot{};
bool g_initialized = false;

Pad ReadHardwarePad(SDL_Gamepad* pad) {
    Pad p;
    if (!pad) return p;
    p.connected = true;
    constexpr SDL_GamepadButton kButtons[] = {
        SDL_GAMEPAD_BUTTON_DPAD_UP,    SDL_GAMEPAD_BUTTON_DPAD_DOWN,  SDL_GAMEPAD_BUTTON_DPAD_LEFT,
        SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SDL_GAMEPAD_BUTTON_START,      SDL_GAMEPAD_BUTTON_BACK,
        SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK};
    for (int i = 0; i < 8; i++) p.buttons |= uint16_t(SDL_GetGamepadButton(pad, kButtons[i]) << i);
    constexpr SDL_GamepadButton kAnalog[] = {
        SDL_GAMEPAD_BUTTON_SOUTH,          SDL_GAMEPAD_BUTTON_EAST,
        SDL_GAMEPAD_BUTTON_WEST,           SDL_GAMEPAD_BUTTON_NORTH,
        SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER};
    for (int i = 0; i < 6; i++) p.analog[i] = SDL_GetGamepadButton(pad, kAnalog[i]) ? 0xFF : 0;
    auto axis = [&](SDL_GamepadAxis a) { return int(SDL_GetGamepadAxis(pad, a)); };
    p.analog[6] = uint8_t(std::max(axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER), 0) >> 7);
    p.analog[7] = uint8_t(std::max(axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER), 0) >> 7);
    p.lx = int16_t(axis(SDL_GAMEPAD_AXIS_LEFTX));
    p.ly = int16_t(-1 - axis(SDL_GAMEPAD_AXIS_LEFTY));
    p.rx = int16_t(axis(SDL_GAMEPAD_AXIS_RIGHTX));
    p.ry = int16_t(-1 - axis(SDL_GAMEPAD_AXIS_RIGHTY));
    return p;
}

int16_t AddStick(int16_t a, int16_t b) {
    return int16_t(std::clamp(int(a) + int(b), -32768, 32767));
}

void OpenPad(SDL_JoystickID id) {
    for (SDL_Gamepad* pad : g_pads)
        if (pad && SDL_GetGamepadID(pad) == id) return;
    SDL_Gamepad** slot = std::find(std::begin(g_pads), std::end(g_pads), nullptr);
    if (slot == std::end(g_pads)) return;
    *slot = SDL_OpenGamepad(id);
    const char* name = *slot ? SDL_GetGamepadName(*slot) : SDL_GetError();
    LOGI("input: controller %d %s: %s", int(slot - g_pads) + 1, *slot ? "plugged in" : "failed to open", name ? name : "");
}

void ClosePad(SDL_JoystickID id) {
    for (int i = 0; i < 4; i++) {
        if (!g_pads[i] || SDL_GetGamepadID(g_pads[i]) != id) continue;
        SDL_CloseGamepad(g_pads[i]);
        g_pads[i] = nullptr;
        g_hw_snapshot[size_t(i)] = {};
        LOGI("input: controller %d unplugged", i + 1);
    }
}

}  // namespace

void OnGamepadAdded(SDL_JoystickID id) {
    std::lock_guard<std::mutex> lock(g_lock);
    OpenPad(id);
}

void OnGamepadRemoved(SDL_JoystickID id) {
    std::lock_guard<std::mutex> lock(g_lock);
    ClosePad(id);
}

void UpdateFromMainThread() {
    std::lock_guard<std::mutex> lock(g_lock);
    if (!g_initialized) return;
    for (int i = 0; i < 4; ++i) {
        g_hw_snapshot[size_t(i)] = ReadHardwarePad(g_pads[i]);
    }
}

bool HasPhysicalGamepad() {
    std::lock_guard<std::mutex> lock(g_lock);
    for (int i = 0; i < 4; ++i)
        if (g_pads[i] != nullptr) return true;
    return false;
}

void Init() {
    std::lock_guard<std::mutex> lock(g_lock);
    if (g_initialized) return;
    g_initialized = true;
    int count = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
        for (int i = 0; i < count; i++) OpenPad(ids[i]);
        SDL_free(ids);
    }
}

Pad Poll(int port) {
    Pad p;
    if (port >= 0 && port < 4) {
        std::lock_guard<std::mutex> lock(g_lock);
        p = g_hw_snapshot[size_t(port)];
    }

    if (port == 0 && app::EffectiveTouchEnabled()) {
        Pad tp = touch::GetPad();
        p.connected = true;
        p.buttons |= tp.buttons;
        for (int i = 0; i < 8; ++i) p.analog[i] = std::max(p.analog[i], tp.analog[i]);
        if (app::AndroidSettings().touch_auto_aim_fire && tp.analog[7] > 0 && !kbm::IsInVehicle()) {
            p.analog[6] = 255;
        }
        p.lx = AddStick(p.lx, tp.lx);
        p.ly = AddStick(p.ly, tp.ly);
        p.rx = AddStick(p.rx, tp.rx);
        p.ry = AddStick(p.ry, tp.ry);
    }

    if (app::OverlayOpen() || touch::IsEditing()) {
        p.connected = p.connected || port == 0;
        p.buttons = 0;
        for (uint8_t& a : p.analog) a = 0;
        p.lx = p.ly = p.rx = p.ry = 0;
    }
    if (app::kGame.pad) app::kGame.pad(port, p);
    return p;
}

uint16_t HeldOnAnyController() {
    std::lock_guard<std::mutex> lock(g_lock);
    uint16_t held = 0;
    for (const Pad& p : g_hw_snapshot) {
        if (!p.connected) continue;
        if (p.analog[0] > 128) held |= 0x1000;
        if (p.analog[1] > 128) held |= 0x2000;
        if (p.analog[2] > 128) held |= 0x4000;
        if (p.analog[3] > 128) held |= 0x8000;
    }
    return held;
}

void Rumble(int port, uint16_t left, uint16_t right) {
    if (!app::AndroidSettings().controller_rumble) {
        left = 0;
        right = 0;
    }
    if (app::kGame.rumble) app::kGame.rumble(port, left, right);
    std::lock_guard<std::mutex> lock(g_lock);
    if (port >= 0 && port < 4 && g_pads[port]) SDL_RumbleGamepad(g_pads[port], left, right, 0);
}

}  // namespace input
