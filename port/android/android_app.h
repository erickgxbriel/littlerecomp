#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>

#include "app/app.h"

namespace gpu {

struct VulkanStats {
    float guest_fps = 0.0f;
    float cpu_ms = 0.0f;
    float scale = 1.5f;
    uint64_t frames = 0;
    uint64_t draws = 0;
    uint64_t clears = 0;
    uint64_t rp_opens = 0;
    uint64_t neon_decodes_sec = 0;
    uint64_t tex_uploads_sec = 0;
    size_t cached_textures = 0;
    size_t tex_vram_mb = 0;
    size_t pso_count = 0;
    size_t spv_count = 0;
    size_t lazy_depths = 0;
    size_t persistent_depths = 0;
    int thermal_status = 0;
};

VulkanStats GetVulkanStats();

}  // namespace gpu

namespace app {

struct AndroidExtras {
    bool touch_controls = true;
    float touch_opacity = 0.55f;
    float render_scale = 1.5f;  // 1.0, 1.5 (default), 2.0
    int upscaler = 1;           // 0 = Bilinear, 1 = Snapdragon GSR v1 (FP16), 2 = FSR 1.0 (FP16)
    bool pin_big_cores = true;
    int hud_mode = 0;           // 0 = Off, 1 = FPS Only, 2 = Basic (FPS + Temp), 3 = Advanced
    int language = 0;           // 0 = Auto, 1 = EN, 2 = PT-BR
    bool native_aux_rt = true;  // Keep off-screen auxiliary RTs at 1.0x scale
    bool thermal_drs = false;   // Step down scale when SoC thermal status is Severe
    std::string game_uri;
    std::string game_display_name;
};

AndroidExtras& AndroidSettings();
void SetStorageDir(const std::filesystem::path& dir);
void SetWindowHandle(void* sdl_window);
void SetDesktopSize(uint32_t w, uint32_t h);

bool FindGameAndroid(std::string& out_path);

struct RuntimeStatus {
    std::string stage = "Initializing...";
    std::string warning;
    std::string game_path;
    std::string gpu_name = "Unknown GPU";
    std::string vulkan_ver;
    bool interposition_ok = false;
    bool game_booted = false;
    bool game_failed = false;
    bool needs_all_files_perm = false;
    bool retry_requested = false;
    bool has_physical_pad = false;
};

void SetStatusStage(const std::string& stage, bool failed = false, bool booted = false, bool needs_perm = false);
void SetStatusWarning(const std::string& warning);
void SetStatusGamePath(const std::string& path);
void SetStatusGpu(const std::string& name, const std::string& vk_ver);
void SetStatusInterposition(bool ok);
void SetStatusPhysicalPad(bool connected);
void RequestBootRetry();
void SetPickedIsoUri(const std::string& uri, const std::string& display_name);
void SetPickedGameFolder(const std::string& path);
bool ConsumeRetryRequest();
RuntimeStatus GetStatus();

void SetCachedBatteryTempC(float temp_c);
float CachedBatteryTempC();

void DrawAndroidOverlay(float view_w, float view_h, float ui_fps);

// Implemented in android_main.cpp (JNI bridge to ScarfaceActivity)
void OpenIsoPicker();
void OpenFolderPicker();
bool HasAllFilesAccess();
void RequestAllFilesAccess();
int OpenPersistedUriFd(const std::string& uri);
std::string GetSystemLanguageCode();
float QueryBatteryTempC();

}  // namespace app
