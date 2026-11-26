#include "android_app.h"

#include <time.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

#include <imgui.h>
#include <toml++/toml.hpp>

#include "android_adpf.h"
#include "audio/oboe_control.h"
#include "common/kbm.h"
#include "core/input.h"
#include "core/log.h"
#include "cpu_affinity.h"
#include "i18n.h"
#include "touch_controls.h"
#include "vk_context.h"

namespace fs = std::filesystem;

namespace app {

namespace {

fs::path g_storage_dir = "/data/local/tmp/scarface_data";
void* g_window = nullptr;
std::atomic<uint32_t> g_desk_w{2340};
std::atomic<uint32_t> g_desk_h{1080};
std::atomic<bool> g_overlay_open{false};
std::atomic<bool> g_repin_guest{true};
std::atomic<float> g_battery_temp_c{-1.0f};
std::atomic<bool> g_open_credits_requested{false};

std::mutex g_status_lock;
RuntimeStatus g_status;

fs::path SettingsFile() { return ExeDir() / kGame.settings; }

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<uint8_t>(c)));
    return s;
}

bool HasIsoExt(const fs::path& p) {
    std::string ext = Lower(p.extension().string());
    return ext == ".iso" || ext == ".xiso";
}

std::string FindInDir(const fs::path& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return {};
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (entry.is_regular_file(ec) && Lower(entry.path().filename().string()) == "default.xbe") {
            return dir.string();
        }
    }
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (entry.is_regular_file(ec) && HasIsoExt(entry.path())) {
            return entry.path().string();
        }
    }
    return {};
}

const char* ThermalStatusLabel(int st) {
    if (st >= 3) return i18n::T(i18n::Str::ThermalCrit);
    if (st == 2) return i18n::T(i18n::Str::ThermalHot);
    if (st == 1) return i18n::T(i18n::Str::ThermalWarm);
    return i18n::T(i18n::Str::ThermalOk);
}

ImVec4 ThermalStatusColor(int st) {
    if (st >= 3) return ImVec4(1.0f, 0.35f, 0.35f, 1.0f);
    if (st == 2) return ImVec4(1.0f, 0.65f, 0.25f, 1.0f);
    if (st == 1) return ImVec4(1.0f, 0.90f, 0.45f, 1.0f);
    return ImVec4(0.55f, 0.95f, 0.62f, 1.0f);
}

void DrawLanguagePills() {
    auto& ex = AndroidSettings();
    auto pill = [&](const char* label, int lang_val) {
        bool selected = (ex.language == lang_val);
        if (selected) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.78f, 0.12f, 0.17f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.86f, 0.18f, 0.23f, 1.0f));
        }
        if (ImGui::Button(label)) {
            ex.language = lang_val;
            i18n::SetLanguage(static_cast<i18n::Lang>(ex.language), GetSystemLanguageCode());
            Save();
        }
        if (selected) ImGui::PopStyleColor(2);
    };
    pill("Auto", 0);
    ImGui::SameLine();
    pill("Português (BR)", 2);
    ImGui::SameLine();
    pill("English", 1);
}

void EnableTouchDragScroll() {
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && !ImGui::IsAnyItemActive() &&
        ImGui::IsMouseDragging(ImGuiMouseButton_Left, 6.0f)) {
        float max_y = ImGui::GetScrollMaxY();
        if (max_y > 0.0f) {
            float next_y = std::clamp(ImGui::GetScrollY() - ImGui::GetIO().MouseDelta.y, 0.0f, max_y);
            ImGui::SetScrollY(next_y);
        }
    }
}

}  // namespace

AndroidExtras& AndroidSettings() {
    static AndroidExtras extras;
    return extras;
}

bool EffectiveTouchEnabled() {
    const AndroidExtras& ex = AndroidSettings();
    if (!ex.touch_controls) return false;
    if (ex.auto_hide_touch_with_pad && GetStatus().has_physical_pad) return false;
    return true;
}

void SetStorageDir(const fs::path& dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    g_storage_dir = dir;
}

void SetWindowHandle(void* sdl_window) { g_window = sdl_window; }

void SetDesktopSize(uint32_t w, uint32_t h) {
    if (w > 0 && h > 0) {
        g_desk_w.store(w, std::memory_order_relaxed);
        g_desk_h.store(h, std::memory_order_relaxed);
    }
}

void SetStatusStage(const std::string& stage, bool failed, bool booted, bool needs_perm) {
    std::lock_guard<std::mutex> lock(g_status_lock);
    g_status.stage = stage;
    g_status.game_failed = failed;
    g_status.game_booted = booted;
    g_status.needs_all_files_perm = needs_perm;
}

void SetStatusWarning(const std::string& warning) {
    std::lock_guard<std::mutex> lock(g_status_lock);
    g_status.warning = warning;
}

void SetStatusGamePath(const std::string& path) {
    std::lock_guard<std::mutex> lock(g_status_lock);
    g_status.game_path = path;
}

void SetStatusGpu(const std::string& name, const std::string& vk_ver) {
    std::lock_guard<std::mutex> lock(g_status_lock);
    g_status.gpu_name = name;
    g_status.vulkan_ver = vk_ver;
}

void SetStatusInterposition(bool ok) {
    std::lock_guard<std::mutex> lock(g_status_lock);
    g_status.interposition_ok = ok;
}

void SetStatusPhysicalPad(bool connected) {
    std::lock_guard<std::mutex> lock(g_status_lock);
    g_status.has_physical_pad = connected;
}

void RequestBootRetry() {
    std::lock_guard<std::mutex> lock(g_status_lock);
    g_status.retry_requested = true;
}

void SetPickedIsoUri(const std::string& uri, const std::string& display_name) {
    AndroidExtras& ex = AndroidSettings();
    ex.game_uri = uri;
    ex.game_display_name = display_name;
    Get().game.clear();
    Save();
    SetStatusWarning("");
    SetStatusStage(i18n::T(i18n::Str::StatusLoadingGame), false, false, false);
    RequestBootRetry();
    g_overlay_open.store(false, std::memory_order_release);
}

void SetPickedGameFolder(const std::string& path) {
    AndroidExtras& ex = AndroidSettings();
    ex.game_uri.clear();
    ex.game_display_name.clear();
    Get().game = path;
    Save();
    SetStatusWarning("");
    SetStatusStage(i18n::T(i18n::Str::StatusLoadingGame), false, false, false);
    RequestBootRetry();
    g_overlay_open.store(false, std::memory_order_release);
}

bool ConsumeRetryRequest() {
    std::lock_guard<std::mutex> lock(g_status_lock);
    bool r = g_status.retry_requested;
    g_status.retry_requested = false;
    return r;
}

RuntimeStatus GetStatus() {
    std::lock_guard<std::mutex> lock(g_status_lock);
    return g_status;
}

void SetCachedBatteryTempC(float temp_c) {
    g_battery_temp_c.store(temp_c, std::memory_order_relaxed);
}

float CachedBatteryTempC() {
    return g_battery_temp_c.load(std::memory_order_relaxed);
}

Settings& Get() {
    static Settings s = [] {
        Settings init;
        init.show_fps = false;
        init.fps_cap = 60;
        init.vsync = true;
        return init;
    }();
    return s;
}

void Load() {
    Settings& s = Get();
    AndroidExtras& ex = AndroidSettings();
    toml::table t;
    try {
        t = toml::parse_file(SettingsFile().string());
    } catch (...) {
        return;
    }
    s.width = t["width"].value_or(s.width);
    s.height = t["height"].value_or(s.height);
    s.render_percent = t["render_percent"].value_or(s.render_percent);
    s.fps_cap = t["fps_cap"].value_or(s.fps_cap);
    s.vsync = t["vsync"].value_or(s.vsync);
    s.aniso = t["anisotropic_filtering"].value_or(s.aniso);
    s.game = t["game"].value_or(s.game);
    s.show_fps = t["show_fps"].value_or(s.show_fps);
    s.skip_intro = t["skip_intro"].value_or(s.skip_intro);
    s.all_cheats_unlocked = t["all_cheats_unlocked"].value_or(s.all_cheats_unlocked);
    s.experimental_60fps = t["experimental_60fps"].value_or(s.experimental_60fps);

    ex.touch_controls = t["android_touch_controls"].value_or(ex.touch_controls);
    ex.touch_opacity = static_cast<float>(t["android_touch_opacity"].value_or(double(ex.touch_opacity)));
    ex.render_scale = std::clamp(static_cast<float>(t["android_render_scale"].value_or(double(ex.render_scale))), 0.60f, 2.25f);
    ex.upscaler = std::clamp(int(t["android_upscaler"].value_or(int64_t(ex.upscaler))), 0, 2);
    ex.aspect_mode = std::clamp(int(t["android_aspect_mode"].value_or(int64_t(ex.aspect_mode))), 0, 1);
    ex.pin_big_cores = t["android_pin_big_cores"].value_or(ex.pin_big_cores);
    ex.hud_mode = std::clamp(int(t["android_hud_mode"].value_or(int64_t(s.show_fps ? 2 : ex.hud_mode))), 0, 3);
    ex.language = std::clamp(int(t["android_language"].value_or(int64_t(ex.language))), 0, 2);
    ex.native_aux_rt = t["android_native_aux_rt"].value_or(ex.native_aux_rt);
    ex.thermal_drs = t["android_thermal_drs"].value_or(ex.thermal_drs);
    ex.touch_auto_aim_fire = t["android_touch_auto_aim_fire"].value_or(ex.touch_auto_aim_fire);
    ex.swapchain_prerotate = t["android_prerotate"].value_or(ex.swapchain_prerotate);
    ex.low_vram_tex = t["android_low_vram_tex"].value_or(ex.low_vram_tex);
    ex.controller_rumble = t["android_rumble"].value_or(ex.controller_rumble);
    ex.auto_hide_touch_with_pad = t["android_auto_hide_touch"].value_or(ex.auto_hide_touch_with_pad);
    ex.game_uri = t["android_game_uri"].value_or(ex.game_uri);
    ex.game_display_name = t["android_game_display_name"].value_or(ex.game_display_name);

    for (const Option& o : kGame.options) {
        if (o.flag) *o.flag = t[o.key].value_or(*o.flag);
        else *o.value = t[o.key].value_or(*o.value);
    }
    if (kGame.load) kGame.load(&t);
}

void Save() {
    const Settings& s = Get();
    const AndroidExtras& ex = AndroidSettings();
    toml::table t{{"width", int64_t(s.width)},
                  {"height", int64_t(s.height)},
                  {"render_percent", int64_t(s.render_percent)},
                  {"fps_cap", int64_t(s.fps_cap)},
                  {"vsync", s.vsync},
                  {"anisotropic_filtering", int64_t(s.aniso)},
                  {"game", s.game},
                  {"show_fps", s.show_fps},
                  {"skip_intro", s.skip_intro},
                  {"all_cheats_unlocked", s.all_cheats_unlocked},
                  {"experimental_60fps", s.experimental_60fps},
                  {"android_touch_controls", ex.touch_controls},
                  {"android_touch_opacity", double(ex.touch_opacity)},
                  {"android_render_scale", double(ex.render_scale)},
                  {"android_upscaler", int64_t(ex.upscaler)},
                  {"android_aspect_mode", int64_t(ex.aspect_mode)},
                  {"android_pin_big_cores", ex.pin_big_cores},
                  {"android_hud_mode", int64_t(ex.hud_mode)},
                  {"android_language", int64_t(ex.language)},
                  {"android_native_aux_rt", ex.native_aux_rt},
                  {"android_thermal_drs", ex.thermal_drs},
                  {"android_touch_auto_aim_fire", ex.touch_auto_aim_fire},
                  {"android_prerotate", ex.swapchain_prerotate},
                  {"android_low_vram_tex", ex.low_vram_tex},
                  {"android_rumble", ex.controller_rumble},
                  {"android_auto_hide_touch", ex.auto_hide_touch_with_pad},
                  {"android_game_uri", ex.game_uri},
                  {"android_game_display_name", ex.game_display_name}};
    for (const Option& o : kGame.options) {
        if (o.flag) t.insert(o.key, *o.flag);
        else t.insert(o.key, int64_t(*o.value));
    }
    if (kGame.save) kGame.save(&t);
    std::ofstream f(SettingsFile());
    if (f) f << t << "\n";
}

void LogSettings() {}
void LogSystem() {}
void ApplyDisplay() {}

Mode Desktop() { return {g_desk_w.load(std::memory_order_relaxed), g_desk_h.load(std::memory_order_relaxed)}; }
Mode Output() { return Desktop(); }
float RenderScale(uint32_t) { return AndroidSettings().render_scale; }
bool Widescreen() { return true; }

std::filesystem::path ExeDir() { return g_storage_dir; }

int Instance() { return 0; }
void* Window() { return g_window; }

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

void Overlay(double) {}
bool OverlayOpen() { return g_overlay_open.load(std::memory_order_acquire); }
void ToggleOverlay() { g_overlay_open.store(!g_overlay_open.load(std::memory_order_acquire), std::memory_order_release); }

void Pace() {
    if (g_repin_guest.exchange(false, std::memory_order_acquire)) {
        cpu_affinity::PinGuestMainThread(AndroidSettings().pin_big_cores);
    }

    static int64_t next_ns = 0;
    uint32_t cap = Get().experimental_60fps ? kExperimentalFps : 30u;

    auto now_ns = []() -> int64_t {
        struct timespec ts{};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return int64_t(ts.tv_sec) * 1000000000ll + ts.tv_nsec;
    };

    int64_t now = now_ns();
    if (!cap) {
        next_ns = 0;
        return;
    }
    const int64_t period = 1000000000ll / int64_t(cap);
    if (next_ns == 0 || now >= next_ns) {
        next_ns = now + period;
        return;
    }
    if (next_ns > now + period) next_ns = now + period;
    const int64_t sleep_target = next_ns - 350000ll;
    if (now < sleep_target) {
        struct timespec req{sleep_target / 1000000000ll, sleep_target % 1000000000ll};
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &req, nullptr);
    }
    while (now_ns() < next_ns) std::this_thread::yield();
    next_ns += period;
}

bool FindGameAndroid(std::string& out_path) {
    std::error_code ec;
    const Settings& s = Get();
    if (!s.game.empty() && fs::exists(s.game, ec)) {
        if (fs::is_directory(s.game, ec)) {
            std::string found = FindInDir(s.game);
            if (!found.empty()) {
                out_path = found;
                return true;
            }
        } else if (HasIsoExt(s.game)) {
            out_path = s.game;
            return true;
        }
    }

    const fs::path ext = ExeDir();
    const fs::path candidates[] = {
        ext / "scarface.iso",
        ext / "scarface.xiso",
        ext / "assets" / "scarface.iso",
        "/sdcard/Download/Xbox/scarface.iso",
        "/sdcard/Download/xbox/scarface.iso",
        "/sdcard/Scarface/scarface.iso",
        "/data/local/tmp/scarface.iso",
    };
    for (const fs::path& c : candidates) {
        if (fs::is_regular_file(c, ec)) {
            out_path = c.string();
            return true;
        }
    }

    for (const fs::path& dir : {ext / "assets", ext, fs::path("/sdcard/Scarface")}) {
        std::string found = FindInDir(dir);
        if (!found.empty()) {
            out_path = found;
            return true;
        }
    }
    return false;
}

void DrawAndroidOverlay(float view_w, float view_h, float ui_fps) {
    RuntimeStatus st = GetStatus();
    Settings& s = Get();
    AndroidExtras& ex = AndroidSettings();
    audio::AudioStats ast = audio::GetStats();
    gpu::VulkanStats vst = gpu::GetVulkanStats();
    adpf::ThermalInfo th = adpf::PollThermal(CachedBatteryTempC());

    static const char* kShortUp[3] = {"Bilinear", "GSR", "FSR"};

    // 1. Performance / GPU Temp HUD (Top-left corner, away from top-right MENU)
    if (st.game_booted && ex.hud_mode > 0) {
        const float safe_x = std::max(28.0f, view_w * 0.022f);
        ImGui::SetNextWindowPos(ImVec2(safe_x, 14.0f), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.68f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 5.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 2.0f));
        ImGuiWindowFlags hud_flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs;
        if (ImGui::Begin("##PerfHUD", nullptr, hud_flags)) {
            const char* sensor_lbl = th.is_gpu_sensor ? i18n::T(i18n::Str::HudGpu) : i18n::T(i18n::Str::HudBat);
            if (ex.hud_mode == 1) {
                ImGui::Text("%.1f FPS", vst.guest_fps);
            } else if (ex.hud_mode == 2) {
                ImGui::Text("%.1f FPS", vst.guest_fps);
                if (th.temp_c > 0.0f) {
                    ImGui::SameLine();
                    ImGui::Text("| %s %.0f°C", sensor_lbl, th.temp_c);
                }
                if (th.thermal_status_valid) {
                    ImGui::SameLine();
                    ImGui::TextColored(ThermalStatusColor(th.thermal_status), "(%s)", ThermalStatusLabel(th.thermal_status));
                }
            } else {
                const float frame_ms = (vst.guest_fps > 0.5f) ? (1000.0f / vst.guest_fps) : 0.0f;
                const int res_p = int(std::round(480.0f * vst.scale));
                const char* up_short = kShortUp[std::clamp(ex.upscaler, 0, 2)];
                ImGui::Text("%.1f FPS (%.1fms) | CPU %.1fms", vst.guest_fps, frame_ms, vst.cpu_ms);
                if (th.temp_c > 0.0f) {
                    ImGui::SameLine();
                    ImGui::Text("| %s %.0f°C", sensor_lbl, th.temp_c);
                }
                if (th.thermal_status_valid) {
                    ImGui::SameLine();
                    ImGui::TextColored(ThermalStatusColor(th.thermal_status), "(%s)", ThermalStatusLabel(th.thermal_status));
                }
                ImGui::TextDisabled("%dp (%.1fx %s) | VRAM %zuMB", res_p, vst.scale, up_short, vst.tex_vram_mb);
            }
        }
        ImGui::End();
        ImGui::PopStyleVar(2);
    }

    // 2. Non-intrusive SHA-1 mismatch warning pill while booting
    if (st.game_booted && !st.warning.empty() && !OverlayOpen()) {
        // Shown quietly only in Options or during boot wizard.
    }

    // 3. Setup Wizard & BYOG Tutorial when game hasn't booted yet
    if (!st.game_booted && !OverlayOpen()) {
        ImGui::SetNextWindowPos(ImVec2(view_w * 0.5f, view_h * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(std::clamp(view_w * 0.84f, 680.0f, 1780.0f),
                                        std::clamp(view_h * 0.88f, 520.0f, 980.0f)),
                                 ImGuiCond_Always);
        if (ImGui::Begin("##SetupWizard", nullptr,
                         ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize)) {
            ImGui::TextColored(ImVec4(0.95f, 0.25f, 0.30f, 1.0f), "%s", i18n::T(i18n::Str::WizardTitle));
            const float pad_x = ImGui::GetStyle().FramePadding.x * 2.0f;
            const float sp_x = ImGui::GetStyle().ItemSpacing.x;
            const float lang_w = ImGui::CalcTextSize("Auto").x + ImGui::CalcTextSize("Português (BR)").x +
                                 ImGui::CalcTextSize("English").x + pad_x * 3.0f + sp_x * 2.5f;
            if (ImGui::GetContentRegionAvail().x > lang_w + ImGui::GetItemRectSize().x + sp_x) {
                ImGui::SameLine();
                ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - lang_w);
            }
            DrawLanguagePills();
            ImGui::TextWrapped("%s", i18n::T(i18n::Str::WizardSubtitle));
            ImGui::Separator();

            // 3-Step Installation Tutorial Card
            ImGui::BeginChild("##TutorialBox", ImVec2(0, ImGui::GetContentRegionAvail().y * 0.54f), true);
            EnableTouchDragScroll();
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.40f, 1.0f), "%s", i18n::T(i18n::Str::WizardStep1Title));
            ImGui::TextWrapped("%s", i18n::T(i18n::Str::WizardStep1Desc));
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.40f, 1.0f), "%s", i18n::T(i18n::Str::WizardStep2Title));
            ImGui::TextWrapped("%s", i18n::T(i18n::Str::WizardStep2Desc));
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.40f, 1.0f), "%s", i18n::T(i18n::Str::WizardStep3Title));
            ImGui::TextWrapped("%s", i18n::T(i18n::Str::WizardStep3Desc));
            ImGui::EndChild();

            ImGui::Spacing();
            const float btn_h = std::clamp(view_h * 0.085f, 52.0f, 84.0f);
            const float avail_w = ImGui::GetContentRegionAvail().x;
            const float half_w = (avail_w - ImGui::GetStyle().ItemSpacing.x) * 0.5f;

            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.14f, 0.19f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.86f, 0.20f, 0.25f, 1.0f));
            if (ImGui::Button(i18n::T(i18n::Str::BtnSelectIso), ImVec2(half_w, btn_h))) {
                OpenIsoPicker();
            }
            ImGui::PopStyleColor(2);

            ImGui::SameLine();
            if (ImGui::Button(i18n::T(i18n::Str::BtnSelectFolder), ImVec2(half_w, btn_h))) {
                if (!HasAllFilesAccess()) {
                    RequestAllFilesAccess();
                    SetStatusStage(i18n::T(i18n::Str::ErrPermissionNeeded), true, false, true);
                } else {
                    OpenFolderPicker();
                }
            }

            if (st.needs_all_files_perm || (!HasAllFilesAccess() && !s.game.empty() && ex.game_uri.empty())) {
                if (ImGui::Button(i18n::T(i18n::Str::BtnGrantAllFiles), ImVec2(avail_w, btn_h * 0.78f))) {
                    RequestAllFilesAccess();
                }
            }

            ImGui::Spacing();
            ImVec4 st_col = st.game_failed ? ImVec4(1.0f, 0.45f, 0.45f, 1.0f) : ImVec4(0.65f, 0.95f, 0.70f, 1.0f);
            ImGui::Text("%s", i18n::T(i18n::Str::WizardStatusLabel));
            ImGui::SameLine();
            ImGui::TextColored(st_col, "%s", st.stage.c_str());

            const std::string& chosen = !ex.game_display_name.empty() ? ex.game_display_name
                                      : (!st.game_path.empty() ? st.game_path : s.game);
            if (!chosen.empty()) {
                ImGui::Text("%s %s", i18n::T(i18n::Str::WizardSelectedFile), chosen.c_str());
            }
            ImGui::Text("%s %s (%s) | %s", i18n::T(i18n::Str::WizardHardwareLabel),
                        st.gpu_name.c_str(), st.vulkan_ver.c_str(), cpu_affinity::Summary().c_str());

            if (ImGui::Button(i18n::T(i18n::Str::BtnRetryBoot))) {
                RequestBootRetry();
            }
            ImGui::SameLine();
            if (ImGui::Button(i18n::T(i18n::Str::BtnOptions))) {
                g_open_credits_requested.store(false, std::memory_order_relaxed);
                ToggleOverlay();
            }
            ImGui::SameLine();
            if (ImGui::Button(i18n::T(i18n::Str::TabCredits))) {
                g_open_credits_requested.store(true, std::memory_order_relaxed);
                ToggleOverlay();
            }

            ImGui::Separator();
            ImGui::TextDisabled("%s", i18n::T(i18n::Str::WizardCreditsFooter));
        }
        ImGui::End();
    }

    if (!OverlayOpen()) return;

    // 4. Tabbed Options Overlay
    ImGui::SetNextWindowPos(ImVec2(view_w * 0.5f, view_h * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::clamp(view_w * 0.80f, 680.0f, 1780.0f),
                                    std::clamp(view_h * 0.88f, 480.0f, 980.0f)),
                             ImGuiCond_Always);
    if (ImGui::Begin(i18n::T(i18n::Str::OptionsTitle), nullptr,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::TextDisabled("%s (%s) | %s", st.gpu_name.c_str(), st.vulkan_ver.c_str(), cpu_affinity::Summary().c_str());
        if (!st.warning.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.35f, 1.0f), "%s", st.warning.c_str());
        }
        ImGui::Separator();

        const float footer_h = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y * 2.0f + 4.0f;
        bool open_credits = g_open_credits_requested.exchange(false, std::memory_order_relaxed);
        if (ImGui::BeginTabBar("##OptionsTabs")) {
            if (ImGui::BeginTabItem(i18n::T(i18n::Str::TabGraphics))) {
                if (ImGui::BeginChild("##TabBody", ImVec2(0, -footer_h), false)) {
                    EnableTouchDragScroll();
                    ImGui::Text("%s:", i18n::T(i18n::Str::LblScale));
                    ImGui::SameLine();
                    if (ImGui::RadioButton("0.6x", std::fabs(ex.render_scale - 0.60f) < 0.08f)) {
                        ex.render_scale = 0.60f;
                        Save();
                    }
                    ImGui::SameLine();
                    if (ImGui::RadioButton("0.75x", std::fabs(ex.render_scale - 0.75f) < 0.08f)) {
                        ex.render_scale = 0.75f;
                        Save();
                    }
                    ImGui::SameLine();
                    if (ImGui::RadioButton(i18n::T(i18n::Str::LblScale10), std::fabs(ex.render_scale - 1.0f) < 0.1f)) {
                        ex.render_scale = 1.0f;
                        Save();
                    }
                    ImGui::SameLine();
                    if (ImGui::RadioButton(i18n::T(i18n::Str::LblScale15), std::fabs(ex.render_scale - 1.5f) < 0.1f)) {
                        ex.render_scale = 1.5f;
                        Save();
                    }
                    ImGui::SameLine();
                    if (ImGui::RadioButton(i18n::T(i18n::Str::LblScale20), std::fabs(ex.render_scale - 2.0f) < 0.1f)) {
                        ex.render_scale = 2.0f;
                        Save();
                    }
                    ImGui::SameLine();
                    if (ImGui::RadioButton(i18n::T(i18n::Str::LblScale225), std::fabs(ex.render_scale - 2.25f) < 0.1f)) {
                        ex.render_scale = 2.25f;
                        Save();
                    }

                    ImGui::Text("%s:", i18n::T(i18n::Str::LblUpscaler));
                    ImGui::SameLine();
                    if (ImGui::RadioButton("Snapdragon GSR v1", ex.upscaler == 1)) { ex.upscaler = 1; Save(); }
                    ImGui::SameLine();
                    if (ImGui::RadioButton("FSR 1.0", ex.upscaler == 2)) { ex.upscaler = 2; Save(); }
                    ImGui::SameLine();
                    if (ImGui::RadioButton("Bilinear", ex.upscaler == 0)) { ex.upscaler = 0; Save(); }

                    ImGui::Text("%s:", i18n::T(i18n::Str::LblAspect));
                    ImGui::SameLine();
                    if (ImGui::RadioButton(i18n::T(i18n::Str::Aspect16x9), ex.aspect_mode == 0)) {
                        ex.aspect_mode = 0;
                        Save();
                    }
                    ImGui::SameLine();
                    if (ImGui::RadioButton(i18n::T(i18n::Str::AspectFullscreen), ex.aspect_mode == 1)) {
                        ex.aspect_mode = 1;
                        Save();
                    }
                    ImGui::SameLine(0.0f, 20.0f);
                    if (ImGui::Checkbox(i18n::T(i18n::Str::LblVsync), &s.vsync)) {
                        Save();
                    }
                    ImGui::SameLine(0.0f, 20.0f);
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.14f, 0.19f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.86f, 0.20f, 0.25f, 1.0f));
                    if (ImGui::Button(i18n::T(i18n::Str::BtnLowEndPreset))) {
                        ex.render_scale = 0.75f;
                        ex.upscaler = 1;
                        s.aniso = 0;
                        ex.native_aux_rt = true;
                        ex.thermal_drs = true;
                        ex.low_vram_tex = true;
                        s.experimental_60fps = false;
                        Save();
                    }
                    ImGui::PopStyleColor(2);

                    const float max_aniso = vk::ActiveContext() ? vk::ActiveContext()->MaxAnisotropy() : 16.0f;
                    ImGui::Text("%s:", i18n::T(i18n::Str::LblAniso));
                    ImGui::SameLine();
                    if (ImGui::RadioButton(i18n::T(i18n::Str::AnisoOff), s.aniso <= 1)) {
                        s.aniso = 0;
                        Save();
                    }
                    for (uint32_t lvl : {2u, 4u, 8u, 16u}) {
                        if (float(lvl) > max_aniso + 0.1f) break;
                        ImGui::SameLine();
                        char lbl[16];
                        std::snprintf(lbl, sizeof(lbl), "%ux", lvl);
                        if (ImGui::RadioButton(lbl, s.aniso == lvl)) {
                            s.aniso = lvl;
                            Save();
                        }
                    }

                    ImGui::Separator();
                    if (ImGui::BeginTable("##GfxToggles", 2, ImGuiTableFlags_SizingStretchSame)) {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        if (ImGui::Checkbox(i18n::T(i18n::Str::LblExperimental60Fps), &s.experimental_60fps)) Save();
                        ImGui::TableSetColumnIndex(1);
                        if (ImGui::Checkbox(i18n::T(i18n::Str::LblNativeAuxRt), &ex.native_aux_rt)) Save();
                        ImGui::TextDisabled("  %s", i18n::T(i18n::Str::LblNativeAuxRtHint));

                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        if (ImGui::Checkbox(i18n::T(i18n::Str::LblPinBigCores), &ex.pin_big_cores)) {
                            g_repin_guest.store(true, std::memory_order_release);
                            cpu_affinity::RequestRepinRender();
                            Save();
                        }
                        ImGui::TableSetColumnIndex(1);
                        if (ImGui::Checkbox(i18n::T(i18n::Str::LblThermalDrs), &ex.thermal_drs)) Save();
                        ImGui::TextDisabled("  %s", i18n::T(i18n::Str::LblThermalDrsHint));

                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        if (ImGui::Checkbox(i18n::T(i18n::Str::LblPreRotate), &ex.swapchain_prerotate)) Save();
                        ImGui::TableSetColumnIndex(1);
                        if (ImGui::Checkbox(i18n::T(i18n::Str::LblLowVramTex), &ex.low_vram_tex)) Save();
                        ImGui::TextDisabled("  %s", i18n::T(i18n::Str::LblLowVramTexHint));
                        ImGui::EndTable();
                    }

                    ImGui::Separator();
                    ImGui::Text("%s:", i18n::T(i18n::Str::LblHudMode));
                    ImGui::SameLine();
                    if (ImGui::RadioButton(i18n::T(i18n::Str::HudModeOff), ex.hud_mode == 0)) {
                        ex.hud_mode = 0;
                        s.show_fps = false;
                        Save();
                    }
                    ImGui::SameLine();
                    if (ImGui::RadioButton(i18n::T(i18n::Str::HudModeFpsOnly), ex.hud_mode == 1)) {
                        ex.hud_mode = 1;
                        s.show_fps = true;
                        Save();
                    }
                    ImGui::SameLine();
                    if (ImGui::RadioButton(i18n::T(i18n::Str::HudModeBasic), ex.hud_mode == 2)) {
                        ex.hud_mode = 2;
                        s.show_fps = true;
                        Save();
                    }
                    ImGui::SameLine();
                    if (ImGui::RadioButton(i18n::T(i18n::Str::HudModeAdvanced), ex.hud_mode == 3)) {
                        ex.hud_mode = 3;
                        s.show_fps = true;
                        Save();
                    }
                }
                ImGui::EndChild();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem(i18n::T(i18n::Str::TabControls))) {
                if (ImGui::BeginChild("##TabBody", ImVec2(0, -footer_h), false)) {
                    EnableTouchDragScroll();
                    if (ImGui::Checkbox(i18n::T(i18n::Str::LblTouchControls), &ex.touch_controls)) Save();
                    if (ImGui::Checkbox(i18n::T(i18n::Str::LblAutoHideTouch), &ex.auto_hide_touch_with_pad)) Save();
                    if (ImGui::SliderFloat(i18n::T(i18n::Str::LblTouchOpacity), &ex.touch_opacity, 0.15f, 0.90f, "%.2f")) Save();
                    if (ImGui::Checkbox(i18n::T(i18n::Str::LblAutoAimFire), &ex.touch_auto_aim_fire)) Save();
                    ImGui::TextDisabled("  %s", i18n::T(i18n::Str::LblAutoAimFireHint));
                    if (ImGui::Checkbox(i18n::T(i18n::Str::LblModernPad), &kbm::modern_pad)) Save();
                    if (ImGui::Checkbox(i18n::T(i18n::Str::LblControllerRumble), &ex.controller_rumble)) {
                        if (!ex.controller_rumble) input::Rumble(0, 0, 0);
                        Save();
                    }
                    ImGui::Separator();
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.14f, 0.19f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.86f, 0.20f, 0.25f, 1.0f));
                    if (ImGui::Button(i18n::T(i18n::Str::BtnEditTouchLayout))) {
                        Save();
                        ToggleOverlay();
                        touch::SetEditing(true);
                    }
                    ImGui::PopStyleColor(2);
                    ImGui::SameLine();
                    if (ImGui::Button(i18n::T(i18n::Str::BtnResetTouchLayout))) {
                        touch::ResetLayout();
                    }
                    ImGui::Separator();
                    ImGui::Text("%s %s", i18n::T(i18n::Str::LblPhysicalGamepad),
                                st.has_physical_pad ? i18n::T(i18n::Str::GamepadConnected)
                                                    : i18n::T(i18n::Str::GamepadDisconnected));
                }
                ImGui::EndChild();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem(i18n::T(i18n::Str::TabGameLang))) {
                if (ImGui::BeginChild("##TabBody", ImVec2(0, -footer_h), false)) {
                    EnableTouchDragScroll();
                    ImGui::Text("%s:", i18n::T(i18n::Str::LblUiLanguage));
                    ImGui::SameLine();
                    DrawLanguagePills();
                    ImGui::Separator();

                    if (ImGui::Checkbox(i18n::T(i18n::Str::LblSkipIntro), &s.skip_intro)) Save();
                    if (ImGui::Checkbox(i18n::T(i18n::Str::LblUnlockCheats), &s.all_cheats_unlocked)) Save();
                    ImGui::Separator();

                    if (ImGui::Button(i18n::T(i18n::Str::BtnSelectIso))) {
                        OpenIsoPicker();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button(i18n::T(i18n::Str::BtnSelectFolder))) {
                        if (!HasAllFilesAccess()) RequestAllFilesAccess();
                        else OpenFolderPicker();
                    }

                    static char path_buf[512] = {};
                    if (path_buf[0] == '\0' && !s.game.empty()) {
                        std::snprintf(path_buf, sizeof(path_buf), "%s", s.game.c_str());
                    }
                    ImGui::InputText(i18n::T(i18n::Str::LblCurrentGamePath), path_buf, sizeof(path_buf));
                    if (ImGui::Button(i18n::T(i18n::Str::LblApplyPathRetry))) {
                        ex.game_uri.clear();
                        ex.game_display_name.clear();
                        s.game = path_buf;
                        Save();
                        RequestBootRetry();
                    }
                }
                ImGui::EndChild();
                ImGui::EndTabItem();
            }

            ImGuiTabItemFlags cred_flags = open_credits ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
            if (ImGui::BeginTabItem(i18n::T(i18n::Str::TabCredits), nullptr, cred_flags)) {
                if (ImGui::BeginChild("##TabBody", ImVec2(0, -footer_h), false)) {
                    EnableTouchDragScroll();
                    ImGui::TextColored(ImVec4(0.95f, 0.25f, 0.30f, 1.0f), "%s", i18n::T(i18n::Str::CreditsHeading));
                    ImGui::Separator();
                    ImGui::TextWrapped("%s", i18n::T(i18n::Str::CreditsBody));
                    ImGui::Separator();
                    ImGui::Text("%s GPU=%s | Vulkan=%s | %s", i18n::T(i18n::Str::CreditsTechInfo),
                                st.gpu_name.c_str(), st.vulkan_ver.c_str(), cpu_affinity::Summary().c_str());
                    ImGui::Text("Storage: %s", ExeDir().c_str());
                }
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

        ImGui::Separator();
        if (ImGui::Button(i18n::T(i18n::Str::LblSaveSettings))) {
            Save();
        }
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.14f, 0.19f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.86f, 0.20f, 0.25f, 1.0f));
        if (ImGui::Button(i18n::T(i18n::Str::LblResume))) {
            Save();
            ToggleOverlay();
        }
        ImGui::PopStyleColor(2);
    }
    ImGui::End();
}

}  // namespace app
