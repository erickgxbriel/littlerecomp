#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace input {
struct Pad;
}

// the host side: window, settings
namespace app {

enum class DisplayMode : uint32_t { Fullscreen, Borderless, Windowed };
enum class AntiAliasing : uint32_t { Off, Fxaa };
inline constexpr uint32_t kFpsCapRefresh = 1;  // fps_cap: the monitor's refresh rate
inline constexpr uint32_t kExperimentalFps = 60;  // fixed cap while the experimental mode is on

struct Settings {
    DisplayMode display_mode = DisplayMode::Borderless;
    uint32_t width = 0, height = 0;  // 0 is the desktop mode
    uint32_t render_percent = 100;   // of the output height
    uint32_t fps_cap = 60;           // 0 unlimited, kFpsCapRefresh the monitor's refresh rate
    AntiAliasing antialiasing = AntiAliasing::Off;  // post-process pass on the final image
    bool vsync = true;
    uint32_t aniso = 0;              // 0 as the game asks
    bool custom_textures = true;     // images in textures\replacement\ beside the exe replace the game's
    bool dump_textures = false;      // each texture once into textures\dumps\ beside the exe
    uint32_t texture_dump_format = 0;  // 0 PNG, 1 DDS
    std::string join_ip;                   // co-op: a host past the LAN the system link search also asks
    std::string game;                      // disc image (.iso) or extracted game folder, UTF-8: the launcher's choice
    bool launcher = true;                  // show the launcher at start
    bool show_fps = false;                 // a small frame rate counter in the corner
    bool skip_intro = false;               // skip the publisher logos before the menu
    bool all_cheats_unlocked = false;      // make every pause-menu cheat selectable from a new game
    bool experimental_60fps = false;       // fixed 60 fps mode: skip the guest's 30 Hz wait, see docs/experimental-fps.md
};

Settings& Get();
void Load();
void Save();
void LogSettings();  // every setting, as the settings file has them
void LogSystem();    // the machine and the settings, at a run's start
void ApplyDisplay();  // any thread
struct Mode {
    uint32_t width, height;
};
Mode Desktop();
Mode Output();
float RenderScale(uint32_t guest_height);
// the output is 3:2 or wider: the console reports a widescreen TV and the frame is shown 16:9, else 4:3 as on a 4:3 TV
bool Widescreen();

std::filesystem::path ExeDir();
// the game in assets (game_files.cpp): its folder and this executable's xbe, from the extracted files or a disc
// image (.iso), which it mounts as D:; false after telling the player what is missing
struct GameFiles {
    std::filesystem::path dir;
    std::vector<uint8_t> xbe;
};
bool FindGame(GameFiles& game);
std::string Sha1(const std::vector<uint8_t>& data);  // lower case hex
// what a chosen path holds: the game's xbe, and whether it is this build's; reads only, nothing is mounted
struct GameProbe {
    std::string sha;     // empty when the game's xbe could not be read
    bool found = false;  // the folder or image holds the game's xbe
    bool match = false;  // that xbe is the one recompiled into this executable
    std::string error;   // why it was not found, when it was not
};
GameProbe ProbeGame(const std::filesystem::path& path);
// the game at a folder or disc image: reads its xbe and mounts an image as D:; false with the reason
bool LoadGameAt(const std::filesystem::path& path, GameFiles& game, std::string& error);
// host paths as UTF-8, for the settings file and the launcher
std::string Utf8Path(const std::filesystem::path& path);
std::filesystem::path FromUtf8(const std::string& text);
// the launcher (launcher.cpp): pick the game, its display and input settings; fills game and mounts the image,
// false when the player closed it without starting
bool Launcher(GameFiles& game);
bool LauncherWanted();            // the settings ask for it, or --launcher; --no-launcher wins
void LauncherCommandLine();       // --game <path>: this run's game in place of the settings'
// the recompiled code, <exe>.dll, built from the game in assets when missing or out of date (game_code.cpp) and
// loaded; false after telling the player why the game cannot start, or when they chose not to start it yet
bool PrepareGameCode(const GameFiles& game);
// RecompLauncher's one-time setup: --prepare selects it, and --game <path> supplies the copy.
bool PrepareCommandLine();
bool PrepareHeadless(const GameFiles& game);  // no window or dialogs; prints progress lines to stdout
void HeadlessMessage(const char* text, bool error = false);
int Instance();  // --instance N on the command line, 0 without: a second copy on this PC for system link tests
void* Window();  // HWND
// boot splash: GameInfo::splash .bmp or .png beside the exe, up before the game window
bool OpenSplash();        // window thread, at launch; false without one, and the game window opens at once
void SplashFirstFrame();  // the game's first Swap: waits out the splash's minimum, then opens the game window
void CloseSplash();       // window thread
void OpenGameWindow(bool apply = true);  // any thread but the window thread's; returns once the window is up
// relaunched: the previous instance keeps its window up until this one has shown its first frames (handover.cpp)
bool StartHandover();   // window thread, at launch; true: the game window starts hidden
bool HandingOver();
void FinishHandover();  // any thread but the window thread's: opens the game window, lets the old instance go

// held around ImGui: the window thread's messages and the renderer's frame
std::recursive_mutex& UiLock();

// what the game repo defines once
struct Option {  // a setting in the settings file, flag or value
    const char* key;
    bool* flag;
    uint32_t* value;
    const char* label = nullptr;  // the launcher's text; the key when null
    uint32_t min = 0, max = 0;    // a value's slider range; equal bounds show a number box
};
struct ControlHint {  // a keyboard binding the Controls tab lists as a reference
    const char* keys;   // "W A S D"
    const char* action; // "Move / steer"
};
struct ControlSection {  // one group of hints, such as the bindings on foot
    const char* title;
    std::vector<ControlHint> hints;
};
struct GameInfo {
    const wchar_t* title;        // window and splash
    const wchar_t* short_title;  // message boxes
    const wchar_t* name;         // window classes, crash dumps, handover event
    const char* log;             // log prefix
    const char* settings;        // settings file beside the exe
    const wchar_t* splash;       // .bmp or .png beside the exe
    const wchar_t* missing;      // message when neither the game files nor a disc image are found
    std::vector<std::pair<const char*, const char*>> images;  // xbe file name, executable name
    std::vector<Option> options;
    std::vector<ControlSection> controls;  // the default bindings the Controls tab lists; may stay empty
    // game-owned keyboard-to-pad binding editor, called by the shared Controls tab. The SDK stays generic: it knows
    // only these callbacks, not the game's binding enum or key codes.
    size_t (*binding_count)();
    void (*binding_describe)(size_t index, const char** pad, const char** key, bool* changed);
    void (*binding_capture)(size_t index);
    bool (*binding_capture_active)();
    void (*binding_capture_cancel)();
    void (*binding_reset)(size_t index);
    void (*binding_reset_all)();
    // the settings file's toml::table, for what options cannot hold
    void (*load)(const void* table);
    void (*save)(void* table);
    void (*init)();                // after the settings load, before the window and boot
    void (*loaded)();              // image in memory, before its entry runs
    void (*window)(void* hwnd);    // the game window is up, before boot
    // window thread, each message after ImGui's; true returns result instead of the host's handling
    bool (*message)(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result);
    void (*ui)(double fps);        // each frame inside the ImGui frame, UiLock held
    void (*pad)(int port, input::Pad& pad);  // a polled pad before the game gets it
    void (*rumble)(int port, uint16_t& left, uint16_t& right);
    // xbe file name, SHA-1: other releases the executable runs, each from its own [xbe] named config
    std::vector<std::pair<const char*, const char*>> releases;
    // this title's save id, the folder under E:\UDATA; empty: no Saves tab
    const char* title_id = nullptr;
};
extern const GameInfo kGame;

// sleeps out the rest of the frame for the fps cap
void Pace();

// the in-game options overlay (overlay.cpp): F1 toggles it, and the game's pads are held while it is up
void Overlay(double fps);  // kGame.ui: each frame inside the ImGui frame, UiLock held
bool OverlayOpen();
void ToggleOverlay();  // any thread; the window thread's F1 calls this

namespace saves {

// the Saves tab (saves.cpp): list E:\UDATA\<title id>, import an Xbox save or convert a PC/PS2 save, delete, open.
// game_running disables every command: the pre-game launcher may manage saves, the in-game overlay may not.
void Ui(bool game_running);
// --import-save <folder|save file> --save-slot 1|2|3|4 before the launcher; the target is required. Xbox saves are
// rebound to that profile and PC/PS2 saves are converted into it. Occupied profiles are rejected.
bool CommandLine();

}  // namespace saves

namespace ui {

// the launcher's and the overlay's shared settings widgets (settings_ui.cpp)
void Style(float unit);
bool LoadFont(float unit);  // Segoe UI at this scale; false when the font is missing
void Display(Settings& s, bool live);  // live: apply a mode or resolution change as it is made
void InputOptions();                   // this game's own settings, kGame.options
void BindingEditor();                  // game-owned keyboard-to-pad map, when kGame supplies the callbacks
void ControllerNote();
void ModernControls();                 // active GTA-style pad bindings, shown when the preset is enabled
void KeyBindings();                    // kGame.controls, the default keyboard and mouse bindings
void Advanced(Settings& s);

}  // namespace ui

}  // namespace app
