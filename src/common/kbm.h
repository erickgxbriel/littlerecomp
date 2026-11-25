// Keyboard and mouse as an original Xbox pad, merged with a real controller when one is connected. Keyboard and mouse
// are opt-in: the gamepad is the default. Each bound key sends one fixed Xbox pad control and the game decides what
// that control means on the current screen. Defaults: WASD left stick, mouse right stick, arrows d-pad, Space A, T B,
// R X, E Y, Tab/F bumpers, Caps/LMB triggers, Backspace Back, Esc Start, Left Ctrl L3 and C R3. Mouse look always
// drives the right stick.
#pragma once
#include <cstddef>
#include <cstdint>

namespace input {
struct Pad;
}

namespace kbm {

// Fixed Xbox-pad controls the keyboard and mouse can be bound to. Mouse look always drives the right stick, so it is
// not in the table: binding a key to a pad control keeps the game's own context handling instead of reinterpreting
// keys for each screen.
enum class Binding : uint32_t {
    MoveUp,
    MoveDown,
    MoveLeft,
    MoveRight,
    DpadUp,
    DpadDown,
    DpadLeft,
    DpadRight,
    A,
    B,
    X,
    Y,
    LB,
    RB,
    LT,
    RT,
    Back,
    Start,
    L3,
    R3,
    Count
};

// Key codes are either a Win32 virtual key (1..255) or one of the mouse codes below. 0 means unbound.
constexpr uint32_t kMouseKeyBase = 0x100;
constexpr uint32_t kMouseLmb = kMouseKeyBase + 1;
constexpr uint32_t kMouseRmb = kMouseKeyBase + 2;
constexpr uint32_t kMouseMmb = kMouseKeyBase + 3;

size_t BindingCount();                                                        // Count, as a size_t
const char* BindingSettingName(size_t index);                                 // settings.toml key
void DescribeBinding(size_t index, const char** pad, const char** key, bool* changed);
void BeginBindingCapture(size_t index);                                       // next key or mouse button is the binding
bool BindingCaptureActive();
void CancelBindingCapture();
void ResetBinding(size_t index);
void ResetAllBindings();
uint32_t BindingKeyCode(Binding binding);                                     // for settings persistence
bool SetBindingKeyCode(Binding binding, uint32_t code);
uint32_t KeyCodeFromName(const char* name);                                   // 0 for an unknown name
const char* KeyCodeName(uint32_t code);                                       // stable pointer for the settings UI

// shown in the settings file by app::kGame.options
extern bool enabled;
extern bool invert_y;
extern uint32_t sensitivity_x;  // percent of the base look speed, horizontal
extern uint32_t sensitivity_y;  // percent of the base look speed, vertical
extern bool modern_pad;         // GTA-style driving on a physical pad, off by default
extern bool hud_prompts;        // show PC key labels in HUD prompts while keyboard/mouse drives the game

// Private diagnostic session hook. It is not a player option: only XBE_DIAGNOSTIC_SESSION=1 enables it, and it is
// never written to settings.toml or shown in the options overlay. Commands are queued on the window thread and
// executed by the guest hook on the next text expansion, never from the window thread itself.
bool DiagnosticCommandsEnabled();
void Ui(double fps);  // kGame.ui: options overlay; diagnostic console only in a diagnostic session
bool DiagnosticCommandsOpen();
bool TakeDiagnosticCommand(char (&command)[256]);

// the device that last did something, for prompts that name a button
enum class Device : uint32_t { None, Pad, KeyboardMouse };
Device LastDevice();

// Which pad action a HUD prompt is currently describing, so one pad token can map to the right PC key
// in each situation (X is Enter in a menu, Shift on foot, W while driving).
enum class PromptContext : uint32_t { OnFoot, Vehicle, Menu };
PromptContext PromptContextNow();

// The look tutorial is a separate ActionMap, not a PromptContext. Its Start/B glyphs have meanings that the generic
// context labels cannot express, so prompt expansion checks this when the source line is unavailable.
bool LookTutorialActive();

void Window(void* hwnd);  // kGame.window: raw mouse input is registered on the game window
bool Message(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result);
void Pad(int port, input::Pad& pad);
void Rumble(int port, uint16_t& left, uint16_t& right);  // kGame.rumble: silence a pad the player is not using

}  // namespace kbm
