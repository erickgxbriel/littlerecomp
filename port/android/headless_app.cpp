#include "app/app.h"

#include <cstdlib>
#include <filesystem>
#include <mutex>

namespace app {

namespace {
std::filesystem::path g_exe_dir = std::filesystem::current_path();
}

void SetExeDir(const std::filesystem::path& dir) { g_exe_dir = dir; }

Settings& Get() {
    static Settings s;
    return s;
}

void Load() {}
void Save() {}
void LogSettings() {}
void LogSystem() {}
void ApplyDisplay() {}
Mode Desktop() { return {1280, 720}; }
Mode Output() { return Desktop(); }
float RenderScale(uint32_t) { return 1.0f; }
bool Widescreen() { return true; }

std::filesystem::path ExeDir() { return g_exe_dir; }

int Instance() {
    static const int n = [] {
        const char* e = std::getenv("INSTANCE");
        return e ? std::atoi(e) : 0;
    }();
    return n;
}

void* Window() { return nullptr; }

bool OpenSplash() { return false; }
void SplashFirstFrame() {}
void CloseSplash() {}
void OpenGameWindow(bool) {}
bool StartHandover() { return false; }
bool HandingOver() { return false; }
void FinishHandover() {}

std::recursive_mutex& UiLock() {
    static std::recursive_mutex m;
    return m;
}

void Pace() {}
void Overlay(double) {}
bool OverlayOpen() { return false; }
void ToggleOverlay() {}

}  // namespace app

namespace cpu_affinity {
void PinGuestMainThreadFromRuntime() {}
void PinRenderThreadFromRuntime() {}
}  // namespace cpu_affinity
