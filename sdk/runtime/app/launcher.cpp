// The launcher: before the game boots, pick its disc image (or extracted folder), check the xbe is this build's, set
// the display and input options, then start. An ImGui window over D3D11, gone once the game's own window comes up.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwmapi.h>
#include <objbase.h>
#include <shobjidl.h>
#include <xinput.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

#include <d3d11.h>
#include <imgui.h>
#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>

#include "app/app.h"
#include "core/input.h"
#include "core/log.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace app {

namespace {

// the window's colours, as game_code.cpp paints them
const ImVec4 kAccent(0.569f, 0.620f, 0.510f, 1.0f);
const ImVec4 kAccentHover(0.65f, 0.70f, 0.58f, 1.0f);
const ImVec4 kAccentDim(0.42f, 0.47f, 0.37f, 1.0f);
const ImVec4 kInk(0.045f, 0.050f, 0.043f, 1.0f);
const ImVec4 kDanger(0.85f, 0.42f, 0.38f, 1.0f);
const ImVec4 kWarn(0.88f, 0.68f, 0.35f, 1.0f);

constexpr float kWindowWidth = 760.0f, kWindowHeight = 600.0f;
constexpr const char* kMismatch = "Different release";

HWND g_hwnd = nullptr;
bool g_quit = false, g_destroyed = false, g_resize = false;
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain* g_swap = nullptr;
ID3D11RenderTargetView* g_target = nullptr;

std::string Narrow(const wchar_t* text) {
    if (!text) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    std::string out(size > 0 ? size_t(size - 1) : 0, '\0');
    if (size > 1) WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
    return out;
}

std::string Narrow(const std::wstring& text) { return Narrow(text.c_str()); }

// 96 DPI is 1.0: explicit sizes in ImGui's pixels
float UiUnit() { return g_hwnd ? float(GetDpiForWindow(g_hwnd)) / 96.0f : 1.0f; }

// a COM pointer that releases itself
template <class T>
struct Com {
    T* p = nullptr;
    ~Com() {
        if (p) p->Release();
    }
    T* operator->() const { return p; }
    T** Put() { return &p; }
    explicit operator bool() const { return p != nullptr; }
};

// the documented CLSIDs and IIDs, so linking uuid.lib is not needed
const GUID kFileOpenDialogClsid = {0xdc1c5a9c, 0xe88a, 0x4dde, {0xa5, 0xa1, 0x60, 0xf8, 0x2a, 0x20, 0xae, 0xf7}};
const GUID kFileOpenDialogIid = {0xd57c7288, 0xd4ad, 0x4768, {0xbe, 0x02, 0x9d, 0x96, 0x95, 0x32, 0xd9, 0x60}};

void CreateTarget() {
    ID3D11Texture2D* back = nullptr;
    if (SUCCEEDED(g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back)))) {
        g_device->CreateRenderTargetView(back, nullptr, &g_target);
        back->Release();
    }
}

bool CreateDevice(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    for (D3D_DRIVER_TYPE type : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {
        if (SUCCEEDED(D3D11CreateDeviceAndSwapChain(nullptr, type, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd,
                                                    &g_swap, &g_device, nullptr, &g_context))) {
            CreateTarget();
            return g_target != nullptr;
        }
    }
    return false;
}

void CleanupDevice() {
    if (g_target) g_target->Release();
    if (g_swap) g_swap->Release();
    if (g_context) g_context->Release();
    if (g_device) g_device->Release();
    g_target = nullptr;
    g_swap = nullptr;
    g_context = nullptr;
    g_device = nullptr;
}

void Resize() {
    if (!g_swap) return;
    if (g_target) {
        g_target->Release();
        g_target = nullptr;
    }
    g_swap->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
    CreateTarget();
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui::GetCurrentContext() && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;
    switch (msg) {
        case WM_SIZE:
            if (wp != SIZE_MINIMIZED) g_resize = true;
            return 0;
        case WM_SYSCOMMAND:
            if ((wp & 0xFFF0) == SC_KEYMENU) return 0;  // Alt alone shows no menu
            break;
        case WM_CLOSE:
            g_quit = true;
            return 0;
        case WM_DESTROY:
            // no PostQuitMessage: the game's message loop runs on this same thread afterwards and would take it
            g_destroyed = true;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// a picked .iso or folder, as UTF-8; false when the dialog was cancelled
bool PickPath(bool folder, const wchar_t* title, std::string& out) {
    Com<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(kFileOpenDialogClsid, nullptr, CLSCTX_INPROC_SERVER, kFileOpenDialogIid,
                                reinterpret_cast<void**>(dialog.Put()))))
        return false;
    DWORD options = 0;
    dialog->GetOptions(&options);
    options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
    options |= folder ? FOS_PICKFOLDERS : (FOS_FILEMUSTEXIST | FOS_DONTADDTORECENT);
    dialog->SetOptions(options);
    dialog->SetTitle(title);
    if (!folder) {
        COMDLG_FILTERSPEC types[] = {{L"Xbox disc images", L"*.iso"}, {L"All files", L"*.*"}};
        dialog->SetFileTypes(2, types);
    }
    if (dialog->Show(g_hwnd) != S_OK) return false;
    Com<IShellItem> item;
    if (FAILED(dialog->GetResult(item.Put()))) return false;
    PWSTR wide = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &wide))) return false;
    out = Narrow(wide);
    CoTaskMemFree(wide);
    return !out.empty();
}

struct State {
    Settings settings = Get();
    std::string path = Get().game;
    GameProbe probe;
    GameFiles files;
    bool play = false;
    uint16_t pad_buttons = 0;  // last face-button report, for edge detection
};

void SetPath(State& s, std::string path) {
    s.path = std::move(path);
    s.settings.game = s.path;
    s.probe = ProbeGame(FromUtf8(s.path));
    if (s.probe.found)
        LOGR("Launcher: %s, %s", s.path.c_str(), s.probe.match ? "this build" : "another release");
    else
        LOGW("Launcher: %s: %s", s.path.c_str(), s.probe.error.c_str());
}

// checks the path once more and keeps the game it mounts; a failure returns to the window with the reason
void Start(State& s) {
    std::string error;
    GameFiles files;
    if (!LoadGameAt(FromUtf8(s.path), files, error)) {
        s.probe.found = false;
        s.probe.error = error;
        LOGE("Launcher: %s: %s", s.path.c_str(), error.c_str());
        return;
    }
    s.files = std::move(files);
    s.play = true;
}

// The ready game-code window uses A to press Start. Give the launcher's Play button the same reach: A on any pad
// starts the game, on the press rather than while held, and the mismatched-release confirmation stays in charge.
void PollGamepadPlay(State& s) {
    uint16_t now = input::HeldOnAnyController();
    uint16_t pressed = now & ~s.pad_buttons;
    s.pad_buttons = now;
    if (s.play || !(pressed & XINPUT_GAMEPAD_A) || ImGui::IsPopupOpen(kMismatch)) return;
    if (s.path.empty() || !s.probe.found) return;
    if (s.probe.match) Start(s);
    else ImGui::OpenPopup(kMismatch);
}

void TabGame(State& s) {
    float unit = UiUnit();
    ImGui::TextUnformatted("Game disc image");
    ImGui::TextDisabled("The .iso made from your own copy of the game, or a folder holding default.xbe and its data.");
    ImGui::Spacing();

    char buffer[1024];
    std::snprintf(buffer, sizeof(buffer), "%s", s.path.c_str());
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##game", buffer, sizeof(buffer), ImGuiInputTextFlags_ReadOnly);

    ImGui::Spacing();
    if (ImGui::Button("Browse for the disc image...", ImVec2(230 * unit, 0))) {
        std::string picked;
        if (PickPath(false, L"Choose the Scarface disc image (.iso)", picked)) SetPath(s, std::move(picked));
    }
    ImGui::SameLine();
    if (ImGui::Button("Use an extracted folder...", ImVec2(210 * unit, 0))) {
        std::string picked;
        if (PickPath(true, L"Choose the folder holding default.xbe", picked)) SetPath(s, std::move(picked));
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (s.path.empty()) {
        ImGui::TextDisabled("No game chosen yet.");
    } else if (!s.probe.found) {
        ImGui::PushStyleColor(ImGuiCol_Text, kDanger);
        ImGui::TextWrapped("This path cannot be used: %s.", s.probe.error.c_str());
        ImGui::PopStyleColor();
    } else if (s.probe.match) {
        ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
        ImGui::TextUnformatted("Ready to play.");
        ImGui::PopStyleColor();
        ImGui::TextDisabled("default.xbe matches the release this build was made from.");
        ImGui::TextDisabled("SHA-1 %s", s.probe.sha.c_str());
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
        ImGui::TextWrapped("This is a different release of the game than this build was made from, and will most likely "
                           "crash. Play asks before starting it.");
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::Checkbox("Skip the intro videos", &s.settings.skip_intro);
    ImGui::TextDisabled("Starts at the menu without the publisher logos.");
    ImGui::Spacing();
    ImGui::Checkbox("Unlock all cheats", &s.settings.all_cheats_unlocked);
    ImGui::TextDisabled("Every pause-menu cheat code is selectable from a new game; effects are still chosen manually.");
}

void TabDisplay(State& s) { ui::Display(s.settings, false); }

void TabControls(State& s) {
    (void)s;
    ui::InputOptions();
    ui::BindingEditor();
    ui::ModernControls();
    ui::ControllerNote();
    ui::KeyBindings();
}

void TabAdvanced(State& s) {
    ui::Advanced(s.settings);
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::Checkbox("Show this launcher at start", &s.settings.launcher);
    ImGui::TextDisabled("Settings are kept in settings.toml beside the game. --no-launcher skips this window.");
}

void Draw(State& s) {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("##launcher", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
    static const std::string title = Narrow(kGame.title);
    ImGui::TextUnformatted(title.c_str());
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextDisabled("PC");
    ImGui::TextDisabled("Native recompilation for Windows");
    ImGui::Spacing();

    float unit = UiUnit();
    float footer = 40 * unit + ImGui::GetStyle().WindowPadding.y * 2 + ImGui::GetStyle().ItemSpacing.y;

    ImGui::BeginChild("##body", ImVec2(0, -footer), false);
    if (ImGui::BeginTabBar("##tabs")) {
        if (ImGui::BeginTabItem("Game")) {
            TabGame(s);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Display")) {
            TabDisplay(s);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Controls")) {
            TabControls(s);
            ImGui::EndTabItem();
        }
        if (kGame.title_id && *kGame.title_id && ImGui::BeginTabItem("Saves")) {
            saves::Ui(false);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Advanced")) {
            TabAdvanced(s);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();

    bool ready = !s.path.empty() && s.probe.found;
    float play_width = 130 * unit, quit_width = 90 * unit, gap = 10 * unit;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - play_width - quit_width - gap);
    if (ImGui::Button("Quit", ImVec2(quit_width, 40 * unit))) g_quit = true;
    ImGui::SameLine(0, gap);
    if (!ready) ImGui::BeginDisabled();
    ImGui::PushStyleColor(ImGuiCol_Button, kAccentDim);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccent);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccentHover);
    ImGui::PushStyleColor(ImGuiCol_Text, kInk);
    if (ImGui::Button("Play (A)", ImVec2(play_width, 40 * unit))) {
        if (s.probe.match) Start(s);
        else if (s.probe.found) ImGui::OpenPopup(kMismatch);
    }
    ImGui::PopStyleColor(4);
    if (!ready) ImGui::EndDisabled();

    if (ImGui::BeginPopupModal(kMismatch, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("This disc image is a different release of the game than this build was made from, and "
                           "will most likely crash.");
        ImGui::Spacing();
        ImGui::TextWrapped("Start it anyway?");
        ImGui::Spacing();
        if (ImGui::Button("Start anyway", ImVec2(140 * unit, 0))) {
            ImGui::CloseCurrentPopup();
            Start(s);
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100 * unit, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    ImGui::End();
}

void Frame(State& s) {
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    PollGamepadPlay(s);
    Draw(s);
    ImGui::Render();
    const float clear[4] = {0.043f, 0.047f, 0.043f, 1.0f};
    g_context->OMSetRenderTargets(1, &g_target, nullptr);
    g_context->ClearRenderTargetView(g_target, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_swap->Present(1, 0);
}

// the backbuffer beside the exe, so the launcher can be looked at where a screen grab is not available:
// XBE_LAUNCHER_SHOT=<file>
bool SaveShot(const std::filesystem::path& file) {
    Com<ID3D11Texture2D> back;
    if (FAILED(g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(back.Put())))) return false;
    D3D11_TEXTURE2D_DESC desc{};
    back->GetDesc(&desc);
    D3D11_TEXTURE2D_DESC stage = desc;
    stage.Usage = D3D11_USAGE_STAGING;
    stage.BindFlags = 0;
    stage.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stage.MiscFlags = 0;
    Com<ID3D11Texture2D> copy;
    if (FAILED(g_device->CreateTexture2D(&stage, nullptr, copy.Put()))) return false;
    g_context->CopyResource(copy.p, back.p);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(g_context->Map(copy.p, 0, D3D11_MAP_READ, 0, &mapped))) return false;
    std::vector<uint8_t> pixels(size_t(desc.Width) * desc.Height * 4);
    for (uint32_t y = 0; y < desc.Height; y++) {
        const uint8_t* row = static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch;
        for (uint32_t x = 0; x < desc.Width; x++) {  // RGBA to the BMP's BGRA
            uint8_t* out = &pixels[(size_t(y) * desc.Width + x) * 4];
            out[0] = row[x * 4 + 2];
            out[1] = row[x * 4 + 1];
            out[2] = row[x * 4 + 0];
            out[3] = 255;
        }
    }
    g_context->Unmap(copy.p, 0);
    FILE* f = _wfopen(file.c_str(), L"wb");
    if (!f) return false;
    uint8_t header[54] = {'B', 'M'};  // BITMAPFILEHEADER and BITMAPINFOHEADER, top down 32 bit
    auto put = [&](int at, uint32_t value) { std::memcpy(header + at, &value, 4); };
    put(2, 54 + uint32_t(pixels.size()));
    put(10, 54);
    put(14, 40);
    put(18, desc.Width);
    put(22, uint32_t(-int32_t(desc.Height)));
    header[26] = 1;
    header[28] = 32;
    put(34, uint32_t(pixels.size()));
    std::fwrite(header, 1, sizeof(header), f);
    std::fwrite(pixels.data(), 1, pixels.size(), f);
    std::fclose(f);
    LOGI("Launcher: wrote %s (%ux%u)", Utf8Path(file).c_str(), desc.Width, desc.Height);
    return true;
}

bool HasArgument(const wchar_t* name) {
    const wchar_t* line = GetCommandLineW();
    size_t length = std::wcslen(name);
    for (const wchar_t* at = line; (at = std::wcsstr(at, name)); at += length) {
        bool start = at == line || at[-1] == L' ' || at[-1] == L'\t' || at[-1] == L'"';
        bool end = at[length] == 0 || at[length] == L' ' || at[length] == L'\t' || at[length] == L'"';
        if (start && end) return true;
    }
    return false;
}

}  // namespace

bool LauncherWanted() {
    if (Instance() > 0) return false;  // a second copy for system link tests goes straight to the game
    if (HasArgument(L"--launcher")) return true;
    if (HasArgument(L"--no-launcher")) return false;
    return Get().launcher || Get().game.empty();
}

bool PrepareCommandLine() { return HasArgument(L"--prepare"); }

void LauncherCommandLine() {
    const wchar_t* line = GetCommandLineW();
    const wchar_t* at = std::wcsstr(line, L"--game");
    if (!at) return;
    bool start = at == line || at[-1] == L' ' || at[-1] == L'\t' || at[-1] == L'"';
    if (!start || (at[6] != L' ' && at[6] != L'\t')) return;
    at += 6;
    while (*at == L' ' || *at == L'\t') at++;
    std::wstring value;
    if (*at == L'"') {
        for (at++; *at && *at != L'"'; at++) value += *at;
    } else {
        for (; *at && *at != L' ' && *at != L'\t'; at++) value += *at;
    }
    if (!value.empty()) Get().game = Utf8Path(std::filesystem::path(value));
}

bool Launcher(GameFiles& game) {
    State state;
    if (!state.path.empty()) {
        state.probe = ProbeGame(FromUtf8(state.path));
        if (state.probe.found)
            LOGR("Launcher: opening with %s, %s", state.path.c_str(),
                 state.probe.match ? "this build" : "another release");
        else
            LOGW("Launcher: opening with %s: %s", state.path.c_str(), state.probe.error.c_str());
    } else {
        LOGR("Launcher: no game chosen");
    }

    bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    // The game-code window normally starts SDL later. The launcher can be the first window a player sees, so it has to
    // start controller input itself or HeldOnAnyController() would never see a pad here.
    input::Init();
    HINSTANCE instance = GetModuleHandleW(nullptr);
    std::wstring class_name = std::wstring(kGame.name) + L"Launcher";
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = class_name.c_str();
    RegisterClassExW(&wc);

    UINT dpi = GetDpiForSystem();
    float unit = float(dpi) / 96.0f;
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rect{0, 0, LONG(kWindowWidth * unit), LONG(kWindowHeight * unit)};
    AdjustWindowRectExForDpi(&rect, style, FALSE, 0, dpi);
    int width = rect.right - rect.left, height = rect.bottom - rect.top;
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    g_hwnd = CreateWindowExW(0, class_name.c_str(), kGame.title, style,
                             work.left + (work.right - work.left - width) / 2,
                             work.top + (work.bottom - work.top - height) / 2, width, height, nullptr, nullptr,
                             instance, nullptr);
    if (!g_hwnd) {
        LOGE("Launcher: the window could not be created (%lu)", GetLastError());
        if (com) CoUninitialize();
        return false;
    }
    BOOL dark = TRUE;
    if (FAILED(DwmSetWindowAttribute(g_hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark))))
        DwmSetWindowAttribute(g_hwnd, 19, &dark, sizeof(dark));

    if (!CreateDevice(g_hwnd)) {
        LOGE("Launcher: no Direct3D 11 device");
        MessageBoxW(g_hwnd, L"Direct3D 11 could not be started, see the log in the logs folder.", kGame.short_title,
                    MB_ICONERROR);
        DestroyWindow(g_hwnd);
        g_hwnd = nullptr;
        if (com) CoUninitialize();
        return false;
    }

    dpi = GetDpiForWindow(g_hwnd);
    unit = float(dpi) / 96.0f;
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    ui::Style(unit);
    ui::LoadFont(unit);
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    ShowWindow(g_hwnd, SW_SHOW);
    SetForegroundWindow(g_hwnd);

    MSG msg;
    const wchar_t* shot = _wgetenv(L"XBE_LAUNCHER_SHOT");
    bool try_play = _wgetenv(L"XBE_LAUNCHER_PLAY") != nullptr;
    int frames = 0;
    while (!g_quit && !g_destroyed && !state.play) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                g_quit = true;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (g_quit || g_destroyed || state.play) break;
        if (g_resize) {
            g_resize = false;
            Resize();
        }
        if (g_target) Frame(state);
        // the harness hooks: a picture of the window, then Play as a player would press it
        if (++frames == 30 && shot && g_target) SaveShot(shot);
        if (try_play && frames == 45 && !state.path.empty() && state.probe.match) Start(state);
    }

    bool start = false;
    if (state.play) {
        Get() = state.settings;
        Get().game = state.path;
        Save();
        game = std::move(state.files);
        start = true;
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDevice();
    if (IsWindow(g_hwnd)) DestroyWindow(g_hwnd);
    g_hwnd = nullptr;
    g_quit = g_destroyed = g_resize = false;
    if (com) CoUninitialize();
    return start;
}

}  // namespace app
