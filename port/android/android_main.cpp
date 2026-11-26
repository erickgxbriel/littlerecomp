#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include <android/native_window.h>
#include <jni.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <imgui.h>
#include <backends/imgui_impl_sdl3.h>

#include "android_adpf.h"
#include "android_app.h"
#include "audio/oboe_control.h"
#include "core/input.h"
#include "core/log.h"
#include "cpu_affinity.h"
#include "cpu/x86_ops.h"
#include "i18n.h"
#include "image_symbols.h"
#include "kernel/crypto.h"
#include "kernel/disc.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "touch_controls.h"
#include "vk_context.h"

namespace fs = std::filesystem;

namespace dispatch {
void Init();
X86Func* Find(uint32_t guest);
}  // namespace dispatch

namespace input {
void OnGamepadAdded(SDL_JoystickID id);
void OnGamepadRemoved(SDL_JoystickID id);
void UpdateFromMainThread();
bool HasPhysicalGamepad();
}  // namespace input

X86_EXTERN_FUNC(sub_00028940);
X86_EXTERN_FUNC(__imp__sub_00028940);
X86_EXTERN_FUNC(sub_0032BE70);
X86_EXTERN_FUNC(__imp__sub_0032BE70);

extern "C" __attribute__((visibility("default"))) JNIEXPORT void JNICALL
Java_com_scarfacedroid_app_ScarfaceActivity_nativeOnIsoPicked(JNIEnv* env, jclass, jstring juri, jstring jname) {
    if (!env || !juri) return;
    const char* u = env->GetStringUTFChars(juri, nullptr);
    const char* n = jname ? env->GetStringUTFChars(jname, nullptr) : nullptr;
    std::string uri = u ? u : "";
    std::string name = n ? n : "";
    if (u) env->ReleaseStringUTFChars(juri, u);
    if (n) env->ReleaseStringUTFChars(jname, n);
    if (!uri.empty()) {
        app::SetPickedIsoUri(uri, name);
    }
}

extern "C" __attribute__((visibility("default"))) JNIEXPORT void JNICALL
Java_com_scarfacedroid_app_ScarfaceActivity_nativeOnFolderPicked(JNIEnv* env, jclass, jstring jpath) {
    if (!env || !jpath) return;
    const char* p = env->GetStringUTFChars(jpath, nullptr);
    std::string path = p ? p : "";
    if (p) env->ReleaseStringUTFChars(jpath, p);
    if (!path.empty()) {
        app::SetPickedGameFolder(path);
    }
}

namespace app {

namespace {

void CallVoidActivityMethod(const char* name) {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    jobject act = static_cast<jobject>(SDL_GetAndroidActivity());
    if (!env || !act) return;
    jclass cls = env->GetObjectClass(act);
    jmethodID mid = cls ? env->GetMethodID(cls, name, "()V") : nullptr;
    if (mid) env->CallVoidMethod(act, mid);
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (cls) env->DeleteLocalRef(cls);
    env->DeleteLocalRef(act);
}

std::string JStringToStd(JNIEnv* env, jstring js) {
    if (!env || !js) return {};
    const char* raw = env->GetStringUTFChars(js, nullptr);
    std::string out = raw ? raw : "";
    if (raw) env->ReleaseStringUTFChars(js, raw);
    env->DeleteLocalRef(js);
    return out;
}

}  // namespace

void OpenIsoPicker() { CallVoidActivityMethod("openIsoPicker"); }
void OpenFolderPicker() { CallVoidActivityMethod("openFolderPicker"); }
void RequestAllFilesAccess() { CallVoidActivityMethod("requestAllFilesAccess"); }

bool HasAllFilesAccess() {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    jobject act = static_cast<jobject>(SDL_GetAndroidActivity());
    if (!env || !act) return true;
    jclass cls = env->GetObjectClass(act);
    jmethodID mid = cls ? env->GetMethodID(cls, "hasAllFilesAccess", "()Z") : nullptr;
    bool res = mid ? (env->CallBooleanMethod(act, mid) == JNI_TRUE) : true;
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (cls) env->DeleteLocalRef(cls);
    env->DeleteLocalRef(act);
    return res;
}

int OpenPersistedUriFd(const std::string& uri) {
    if (uri.empty()) return -1;
    JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    jobject act = static_cast<jobject>(SDL_GetAndroidActivity());
    if (!env || !act) return -1;
    jclass cls = env->GetObjectClass(act);
    jmethodID mid = cls ? env->GetMethodID(cls, "openPersistedUriFd", "(Ljava/lang/String;)I") : nullptr;
    int fd = -1;
    if (mid) {
        jstring juri = env->NewStringUTF(uri.c_str());
        fd = int(env->CallIntMethod(act, mid, juri));
        if (juri) env->DeleteLocalRef(juri);
    }
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        fd = -1;
    }
    if (cls) env->DeleteLocalRef(cls);
    env->DeleteLocalRef(act);
    return fd;
}

std::string GetSystemLanguageCode() {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    jobject act = static_cast<jobject>(SDL_GetAndroidActivity());
    if (!env || !act) return "en";
    jclass cls = env->GetObjectClass(act);
    jmethodID mid = cls ? env->GetMethodID(cls, "getSystemLanguage", "()Ljava/lang/String;") : nullptr;
    std::string out = "en";
    if (mid) {
        auto js = static_cast<jstring>(env->CallObjectMethod(act, mid));
        if (js) out = JStringToStd(env, js);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (cls) env->DeleteLocalRef(cls);
    env->DeleteLocalRef(act);
    return out;
}

float QueryBatteryTempC() {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    jobject act = static_cast<jobject>(SDL_GetAndroidActivity());
    if (!env || !act) return -1.0f;
    jclass cls = env->GetObjectClass(act);
    jmethodID mid = cls ? env->GetMethodID(cls, "getBatteryTempC", "()F") : nullptr;
    float temp = mid ? float(env->CallFloatMethod(act, mid)) : -1.0f;
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        temp = -1.0f;
    }
    if (cls) env->DeleteLocalRef(cls);
    env->DeleteLocalRef(act);
    return temp;
}

}  // namespace app

namespace {

constexpr char kExpectedXbeSha1[] = "9367786d1b3bc9b35208a29a7ddca29487af0f5d";

std::string ComputeSha1Hex(const std::vector<uint8_t>& data) {
    alignas(8) uint8_t ctx[128]{};
    uint8_t digest[20]{};
    crypto::ShaInit(ctx);
    crypto::ShaUpdate(ctx, data.data(), data.size());
    crypto::ShaFinal(ctx, digest);
    std::string hex;
    hex.reserve(40);
    for (uint8_t b : digest) {
        hex += "0123456789abcdef"[b >> 4];
        hex += "0123456789abcdef"[b & 15];
    }
    return hex;
}

void CrashHandler(int sig, siginfo_t* info, void*) {
    uintptr_t addr = info ? reinterpret_cast<uintptr_t>(info->si_addr) : 0;
    uintptr_t base = reinterpret_cast<uintptr_t>(kernel::g_base);
    if (base && addr >= base && addr < base + (1ull << 32)) {
        LOGE("CRASH signal %d addr=%p (guest_va=%08llX)", sig, reinterpret_cast<void*>(addr),
             static_cast<unsigned long long>(addr - base));
    } else {
        LOGE("CRASH signal %d addr=%p", sig, reinterpret_cast<void*>(addr));
    }
    if (kernel::Thread* t = kernel::Current(); t && t->ctx) {
        LOGE("crash on guest thread %X", t->id);
        kernel::LogGuestStack(*t->ctx, 64);
    }
    logx::Flush();
    _exit(128 + sig);
}

void InstallCrashHandlers() {
    struct sigaction sa{};
    sa.sa_sigaction = CrashHandler;
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    sigaction(SIGILL, &sa, nullptr);
}

bool VerifyInterposition() {
    dispatch::Init();
    X86Func* f_logo = dispatch::Find(0x00028940u);
    X86Func* f_swap = dispatch::Find(0x0032BE70u);
    bool ok = (f_logo == &sub_00028940) && (f_swap == &sub_0032BE70) &&
              (reinterpret_cast<const void*>(&sub_00028940) != reinterpret_cast<const void*>(&__imp__sub_00028940)) &&
              (reinterpret_cast<const void*>(&sub_0032BE70) != reinterpret_cast<const void*>(&__imp__sub_0032BE70));
    LOGR("interposition: check=%s sub_28940=%p imp=%p sub_32BE70=%p imp=%p",
         ok ? "PASS" : "FAIL",
         reinterpret_cast<const void*>(&sub_00028940), reinterpret_cast<const void*>(&__imp__sub_00028940),
         reinterpret_cast<const void*>(&sub_0032BE70), reinterpret_cast<const void*>(&__imp__sub_0032BE70));
    return ok;
}

fs::path FindFileIgnoreCase(const fs::path& dir, const char* target) {
    std::error_code ec;
    fs::path direct = dir / target;
    if (fs::exists(direct, ec)) return direct;
    auto lower = [](std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<uint8_t>(c)));
        return s;
    };
    std::string want = lower(target);
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (lower(entry.path().filename().string()) == want) return entry.path();
    }
    return {};
}

std::atomic<bool> g_boot_in_flight{false};

void LaunchGameBootAsync() {
    if (g_boot_in_flight.exchange(true)) return;
    std::thread([] {
        pthread_setname_np(pthread_self(), "game_boot");
        adpf::RegisterGuestThread(gettid());

        std::vector<uint8_t> xbe;
        std::string game_dir;
        std::string display_path;

        // 1. Prefer persisted Android SAF URI if set
        const auto ex = app::AndroidSettings();
        if (!ex.game_uri.empty()) {
            int fd = app::OpenPersistedUriFd(ex.game_uri);
            if (fd >= 0) {
                app::SetStatusStage(i18n::T(i18n::Str::StatusLoadingGame));
                auto disc = std::make_unique<kernel::Disc>();
                std::string err;
                fs::path label = !ex.game_display_name.empty() ? fs::path(ex.game_display_name) : fs::path("saf.iso");
                if (!disc->OpenFd(fd, label, err)) {
                    if (err == "non_seekable_uri") {
                        app::SetStatusStage(i18n::T(i18n::Str::ErrNonSeekableUri), true, false);
                    } else if (err == "no Xbox disc image") {
                        app::SetStatusStage(i18n::T(i18n::Str::ErrNotXboxIso), true, false);
                    } else {
                        app::SetStatusStage(err, true, false);
                    }
                    LOGE("boot: SAF OpenFd failed (%s): %s", label.c_str(), err.c_str());
                    g_boot_in_flight.store(false);
                    return;
                }
                if (!disc->Load(IMAGE_XBE, xbe)) {
                    app::SetStatusStage(i18n::T(i18n::Str::ErrMissingXbeInIso), true, false);
                    LOGE("boot: %s missing inside SAF ISO", IMAGE_XBE);
                    g_boot_in_flight.store(false);
                    return;
                }
                kernel::MountDisc(std::move(disc));
                game_dir = app::ExeDir().string();
                display_path = !ex.game_display_name.empty() ? ex.game_display_name : ex.game_uri;
                app::SetStatusGamePath(display_path);
                LOGR("boot: loaded SAF ISO %s", display_path.c_str());
            } else {
                LOGR("boot: persisted SAF URI fd unavailable, falling back to filesystem scan");
            }
        }

        // 2. Filesystem path / legacy candidates fallback
        if (xbe.empty()) {
            std::string found;
            if (!app::FindGameAndroid(found)) {
                const bool has_manual_dir = !app::Get().game.empty() && !app::HasAllFilesAccess();
                if (has_manual_dir) {
                    app::SetStatusStage(i18n::T(i18n::Str::ErrPermissionNeeded), true, false, true);
                } else {
                    app::SetStatusStage(i18n::T(i18n::Str::StatusWaitingByog), false, false, false);
                }
                LOGR("boot: no BYOG game image found yet in %s", app::ExeDir().c_str());
                g_boot_in_flight.store(false);
                return;
            }

            app::SetStatusGamePath(found);
            app::SetStatusStage(i18n::T(i18n::Str::StatusLoadingGame));
            LOGR("boot: loading game from %s", found.c_str());

            std::error_code ec;
            fs::path src(found);
            if (fs::is_regular_file(src, ec)) {
                auto disc = std::make_unique<kernel::Disc>();
                std::string err;
                if (!disc->Open(src, err)) {
                    if (err == "no Xbox disc image") {
                        app::SetStatusStage(i18n::T(i18n::Str::ErrNotXboxIso), true, false);
                    } else {
                        app::SetStatusStage(err, true, false);
                    }
                    LOGE("boot: disc open failed: %s", err.c_str());
                    g_boot_in_flight.store(false);
                    return;
                }
                if (!disc->Load(IMAGE_XBE, xbe)) {
                    app::SetStatusStage(i18n::T(i18n::Str::ErrMissingXbeInIso), true, false);
                    LOGE("boot: %s missing inside %s", IMAGE_XBE, found.c_str());
                    g_boot_in_flight.store(false);
                    return;
                }
                kernel::MountDisc(std::move(disc));
                game_dir = src.parent_path().string();
            } else if (fs::is_directory(src, ec)) {
                fs::path xbe_path = FindFileIgnoreCase(src, IMAGE_XBE);
                if (xbe_path.empty()) {
                    app::SetStatusStage(i18n::T(i18n::Str::ErrMissingXbeInDir), true, false);
                    g_boot_in_flight.store(false);
                    return;
                }
                std::ifstream f(xbe_path, std::ios::binary);
                xbe.assign(std::istreambuf_iterator<char>(f), {});
                if (xbe.empty()) {
                    app::SetStatusStage(i18n::T(i18n::Str::ErrPermissionNeeded), true, false, !app::HasAllFilesAccess());
                    g_boot_in_flight.store(false);
                    return;
                }
                game_dir = src.string();
            } else {
                app::SetStatusStage(i18n::T(i18n::Str::StatusWaitingByog), true, false);
                g_boot_in_flight.store(false);
                return;
            }
        }

        const std::string sha = ComputeSha1Hex(xbe);
        LOGR("boot: default.xbe sha1=%s", sha.c_str());
        if (sha != kExpectedXbeSha1) {
            app::SetStatusWarning(i18n::T(i18n::Str::WarnSha1Mismatch));
            LOGR("boot: WARNING default.xbe sha1 mismatch (expected %s)", kExpectedXbeSha1);
        } else {
            app::SetStatusWarning("");
        }

        app::SetStatusStage(i18n::T(i18n::Str::StatusBootingGuest));
        if (!kernel::Boot(std::move(xbe), IMAGE_XBE, game_dir, (app::ExeDir() / "hdd").string())) {
            app::SetStatusStage("kernel::Boot failed", true, false);
            LOGE("boot: kernel::Boot failed");
            g_boot_in_flight.store(false);
            return;
        }

        app::SetStatusStage(i18n::T(i18n::Str::StatusRunningBeta), false, true);
        LOGR("boot: Scarface guest running cleanly on ARM64 (v0.2.0-beta)");
        g_boot_in_flight.store(false);
    }).detach();
}

void Apply60HzHint(SDL_Window* window) {
    SDL_PropertiesID props = SDL_GetWindowProperties(window);
    void* ptr = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr);
    if (!ptr) return;
    ANativeWindow* native_win = static_cast<ANativeWindow*>(ptr);
    int rc = ANativeWindow_setFrameRate(native_win, 60.0f, ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE);
    LOGR("display: ANativeWindow_setFrameRate(60Hz) rc=%d", rc);
}

void ApplyMobileTheme(float scale) {
    ImGui::StyleColorsDark();
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 10.0f;
    st.ChildRounding = 8.0f;
    st.FrameRounding = 6.0f;
    st.GrabRounding = 6.0f;
    st.TabRounding = 6.0f;
    st.WindowPadding = ImVec2(12.0f, 8.0f);
    st.FramePadding = ImVec2(8.0f, 5.0f);
    st.ItemSpacing = ImVec2(8.0f, 5.0f);
    st.ScrollbarSize = 14.0f;
    st.Colors[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.08f, 0.10f, 0.94f);
    st.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.45f, 0.08f, 0.11f, 1.0f);
    st.Colors[ImGuiCol_Tab] = ImVec4(0.16f, 0.16f, 0.20f, 1.0f);
    st.Colors[ImGuiCol_TabHovered] = ImVec4(0.70f, 0.14f, 0.18f, 1.0f);
    st.Colors[ImGuiCol_TabSelected] = ImVec4(0.78f, 0.14f, 0.19f, 1.0f);
    st.Colors[ImGuiCol_CheckMark] = ImVec4(0.95f, 0.28f, 0.32f, 1.0f);
    st.Colors[ImGuiCol_SliderGrab] = ImVec4(0.85f, 0.20f, 0.25f, 1.0f);
    st.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.98f, 0.30f, 0.34f, 1.0f);
    st.ScaleAllSizes(scale);
}

void LoadUiFont(ImGuiIO& io, float scale) {
    static const ImWchar kLatin1Range[] = {
        0x0020, 0x00FF,
        0x2010, 0x2027,
        0,
    };
    const char* candidates[] = {
        "/system/fonts/Roboto-Regular.ttf",
        "/system/fonts/NotoSans-Regular.ttf",
        "/system/fonts/DroidSans.ttf",
    };
    const float font_px = std::round(15.0f * scale);
    for (const char* path : candidates) {
        std::error_code ec;
        if (fs::is_regular_file(path, ec)) {
            ImFont* f = io.Fonts->AddFontFromFileTTF(path, font_px, nullptr, kLatin1Range);
            if (f) {
                LOGR("ui: loaded TrueType font %s (%.1fpx)", path, font_px);
                return;
            }
        }
    }
    ImFontConfig font_cfg;
    font_cfg.SizePixels = 13.0f * scale;
    io.Fonts->AddFontDefault(&font_cfg);
    LOGR("ui: using fallback ImGui default bitmap font");
}

}  // namespace

int main(int, char**) {
    InstallCrashHandlers();

    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_EVENTS)) {
        LOGE("sdl: SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    const char* ext_path = SDL_GetAndroidExternalStoragePath();
    if (!ext_path || !*ext_path) ext_path = SDL_GetAndroidInternalStoragePath();
    fs::path storage = (ext_path && *ext_path) ? fs::path(ext_path) : fs::path("/data/local/tmp/scarface_data");
    app::SetStorageDir(storage);

    logx::Init((app::ExeDir() / "logs").string(), app::kGame.log);
    LOGR("LittleRecomp v0.2.0-beta starting, storage=%s", app::ExeDir().string().c_str());

    adpf::Init();
    cpu_affinity::Init();
    app::Load();
    touch::LoadLayout();
    i18n::SetLanguage(static_cast<i18n::Lang>(app::AndroidSettings().language), app::GetSystemLanguageCode());
    if (app::kGame.init) app::kGame.init();

    SDL_Window* window = SDL_CreateWindow("LittleRecomp", 0, 0,
                                          SDL_WINDOW_VULKAN | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_RESIZABLE);
    if (!window) {
        LOGE("sdl: SDL_CreateWindow failed: %s", SDL_GetError());
        return 1;
    }
    app::SetWindowHandle(window);
    Apply60HzHint(window);

    int px_w = 1280, px_h = 720;
    SDL_GetWindowSizeInPixels(window, &px_w, &px_h);
    app::SetDesktopSize(uint32_t(px_w), uint32_t(px_h));

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

    float scale = std::clamp(float(px_h) / 560.0f, 1.35f, 2.5f);
    ApplyMobileTheme(scale);
    LoadUiFont(io, scale);

    if (!ImGui_ImplSDL3_InitForVulkan(window)) {
        LOGE("imgui: ImGui_ImplSDL3_InitForVulkan failed");
        return 1;
    }

    vk::Context vk;
    if (!vk.Init(window, app::ExeDir() / "cache")) {
        LOGE("vk: Context::Init failed");
        return 1;
    }
    app::SetStatusGpu(vk.GpuName(), vk.VulkanVersion());

    input::Init();
    if (app::kGame.window) app::kGame.window(window);

    bool interpose_ok = VerifyInterposition();
    app::SetStatusInterposition(interpose_ok);

    LaunchGameBootAsync();

    auto last_batt_poll = std::chrono::steady_clock::now() - std::chrono::seconds(5);
    bool running = true;
    while (running) {
        bool had_ui_event = false;
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_FINGER_DOWN || e.type == SDL_EVENT_FINGER_UP ||
                e.type == SDL_EVENT_FINGER_MOTION || e.type == SDL_EVENT_KEY_DOWN ||
                e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP ||
                e.type == SDL_EVENT_MOUSE_MOTION) {
                had_ui_event = true;
            }
            if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_TERMINATING) {
                running = false;
                break;
            }
            if (e.type == SDL_EVENT_WILL_ENTER_BACKGROUND) {
                LOGR("lifecycle: entering background");
                audio::SetPaused(true);
                touch::Clear();
                vk.Suspend();
                continue;
            }
            if (e.type == SDL_EVENT_DID_ENTER_FOREGROUND) {
                LOGR("lifecycle: entering foreground");
                vk.Resume(window);
                Apply60HzHint(window);
                audio::SetPaused(false);
                continue;
            }
            if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED || e.type == SDL_EVENT_WINDOW_RESIZED) {
                int w = 0, h = 0;
                SDL_GetWindowSizeInPixels(window, &w, &h);
                if (w > 0 && h > 0) {
                    app::SetDesktopSize(uint32_t(w), uint32_t(h));
                    vk.NotifyResized();
                }
            }
            if (e.type == SDL_EVENT_GAMEPAD_ADDED) {
                input::OnGamepadAdded(e.gdevice.which);
                if (app::AndroidSettings().auto_hide_touch_with_pad) {
                    touch::Clear();
                }
            } else if (e.type == SDL_EVENT_GAMEPAD_REMOVED) {
                input::OnGamepadRemoved(e.gdevice.which);
            } else if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat &&
                       (e.key.key == SDLK_AC_BACK || e.key.key == SDLK_ESCAPE)) {
                if (touch::IsEditing()) {
                    touch::SaveLayout();
                    touch::SetEditing(false);
                } else {
                    app::ToggleOverlay();
                }
                touch::Clear();
                continue;
            }

            if (app::OverlayOpen() || !app::GetStatus().game_booted) {
                ImGui_ImplSDL3_ProcessEvent(&e);
            } else if (touch::IsEditing()) {
                ImGui_ImplSDL3_ProcessEvent(&e);
                touch::HandleEvent(e, float(vk.Width()), float(vk.Height()),
                                   app::OverlayOpen(), app::EffectiveTouchEnabled());
            } else if (!touch::HandleEvent(e, float(vk.Width()), float(vk.Height()),
                                           app::OverlayOpen(), app::EffectiveTouchEnabled())) {
                ImGui_ImplSDL3_ProcessEvent(&e);
            }
        }

        if (!running) break;

        input::UpdateFromMainThread();
        app::SetStatusPhysicalPad(input::HasPhysicalGamepad());

        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_batt_poll).count() >= 2) {
            app::SetCachedBatteryTempC(app::QueryBatteryTempC());
            last_batt_poll = now;
        }

        if (app::ConsumeRetryRequest()) {
            LaunchGameBootAsync();
        }

        const bool ui_active = app::OverlayOpen() || !app::GetStatus().game_booted || touch::IsActive() || touch::IsEditing() || had_ui_event;
        vk.RenderFrame([&] {
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();

            const auto& ex = app::AndroidSettings();
            if (app::GetStatus().game_booted) {
                touch::DrawOverlay(float(vk.Width()), float(vk.Height()), ex.touch_opacity,
                                   app::EffectiveTouchEnabled(), app::OverlayOpen());
            }
            app::DrawAndroidOverlay(float(vk.Width()), float(vk.Height()), ImGui::GetIO().Framerate);
        }, ui_active);
    }

    app::Save();
    adpf::Shutdown();
    logx::Flush();
    vk.Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
