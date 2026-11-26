#include "common/kbm.h"

#include <algorithm>
#include <array>
#include <cstring>

#include "core/input.h"
#include "kernel/memory.h"

namespace kbm {

bool enabled = false;
bool invert_y = false;
uint32_t sensitivity_x = 100;
uint32_t sensitivity_y = 100;
bool modern_pad = false;
bool hud_prompts = false;

namespace {

constexpr uint16_t kBack = 1 << 5;
constexpr uint16_t kLeftThumb = 1 << 6;
constexpr int kA = 0, kB = 1, kX = 2, kBlack = 4, kWhite = 5, kLT = 6, kRT = 7;

constexpr size_t kBindingCount = size_t(Binding::Count);

const std::array<const char*, kBindingCount> kBindingLabels = {
    "Left stick up",    "Left stick down",  "Left stick left",  "Left stick right",
    "D-pad up",         "D-pad down",       "D-pad left",       "D-pad right",
    "A",                "B",                "X",                "Y",
    "LB (white)",       "RB (black)",       "LT",               "RT",
    "Back",             "Start",            "L3 (left stick)",  "R3 (right stick)",
};

const std::array<const char*, kBindingCount> kBindingSettingNames = {
    "move_up",   "move_down",  "move_left",  "move_right", "dpad_up",    "dpad_down",
    "dpad_left", "dpad_right", "face_a",     "face_b",     "face_x",     "face_y",
    "lb",        "rb",         "lt",         "rt",         "back",       "start",
    "l3",        "r3",
};

bool InVehicleActionMap() {
    uint8_t* base = mem::Base();
    if (!base) return false;
    constexpr uint32_t kHeadVA = 0x471468;
    if (!mem::IsCommitted(kHeadVA)) return false;
    uint32_t node_va = *reinterpret_cast<const uint32_t*>(base + kHeadVA);
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
            if (std::strstr(name, "WheeledVehicleActionMap") || std::strstr(name, "BoatActionMap") ||
                std::strstr(name, "HelicopterActionMap") || std::strstr(name, "AirplaneActionMap")) {
                return true;
            }
            if (std::strstr(name, "CharacterActionMap")) return false;
        }
        node_va = next_va;
    }
    return false;
}

void ModernDriving(input::Pad& pad) {
    uint8_t gas = pad.analog[kRT], brake = pad.analog[kLT];
    uint8_t fire = pad.analog[kBlack], look = pad.analog[kWhite];
    uint8_t handbrake = pad.analog[kA], horn = pad.analog[kB];
    pad.analog[kA] = std::max(gas, horn);
    pad.analog[kX] = std::max(pad.analog[kX], brake);
    pad.analog[kB] = handbrake;
    pad.analog[kRT] = fire;
    pad.analog[kBlack] = look;
    pad.analog[kLT] = (pad.buttons & kLeftThumb) ? 0xFF : 0;
    pad.analog[kWhite] = (pad.buttons & kBack) ? 0xFF : 0;
}

}  // namespace

size_t BindingCount() { return kBindingCount; }

const char* BindingSettingName(size_t index) {
    return index < kBindingCount ? kBindingSettingNames[index] : "";
}

void DescribeBinding(size_t index, const char** pad, const char** key, bool* changed) {
    if (pad) *pad = index < kBindingCount ? kBindingLabels[index] : "";
    if (key) *key = "Unbound";
    if (changed) *changed = false;
}

void BeginBindingCapture(size_t) {}
bool BindingCaptureActive() { return false; }
void CancelBindingCapture() {}
void ResetBinding(size_t) {}
void ResetAllBindings() {}
uint32_t BindingKeyCode(Binding) { return 0; }
bool SetBindingKeyCode(Binding, uint32_t) { return false; }
uint32_t KeyCodeFromName(const char*) { return 0; }
const char* KeyCodeName(uint32_t) { return "Unbound"; }

bool DiagnosticCommandsEnabled() { return false; }
void Ui(double) {}
bool DiagnosticCommandsOpen() { return false; }
bool TakeDiagnosticCommand(char (&command)[256]) {
    command[0] = '\0';
    return false;
}

Device LastDevice() { return Device::Pad; }
PromptContext PromptContextNow() { return PromptContext::OnFoot; }
bool LookTutorialActive() { return false; }

void Window(void*) {}
bool Message(void*, uint32_t, uintptr_t, intptr_t, intptr_t*) { return false; }

void Pad(int, input::Pad& pad) {
    if (modern_pad && InVehicleActionMap()) ModernDriving(pad);
}

bool IsInVehicle() { return InVehicleActionMap(); }

void Rumble(int, uint16_t&, uint16_t&) {}

}  // namespace kbm
