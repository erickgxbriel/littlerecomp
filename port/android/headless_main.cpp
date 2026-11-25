#include <algorithm>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "app/app.h"
#include "core/log.h"
#include "cpu/x86_ops.h"
#include "image_symbols.h"
#include "kernel/disc.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace fs = std::filesystem;

namespace app {
void SetExeDir(const std::filesystem::path& dir);
}
namespace dispatch {
void Init();
X86Func* Find(uint32_t guest);
}  // namespace dispatch
namespace fibers {
bool RunSelfTest();
}
namespace gpu {
uint64_t HeadlessFrames();
uint64_t HeadlessDraws();
uint64_t HeadlessClears();
}  // namespace gpu

X86_EXTERN_FUNC(sub_00028940);
X86_EXTERN_FUNC(__imp__sub_00028940);
X86_EXTERN_FUNC(sub_0032BE70);
X86_EXTERN_FUNC(__imp__sub_0032BE70);

namespace {

void CrashHandler(int sig, siginfo_t* info, void*) {
    uintptr_t addr = info ? reinterpret_cast<uintptr_t>(info->si_addr) : 0;
    uintptr_t base = reinterpret_cast<uintptr_t>(kernel::g_base);
    std::fprintf(stderr, "\n=== CRASH signal %d addr=%p", sig, reinterpret_cast<void*>(addr));
    if (base && addr >= base && addr < base + (1ull << 32)) {
        std::fprintf(stderr, " (guest_va=%08llX)", static_cast<unsigned long long>(addr - base));
    }
    std::fprintf(stderr, " ===\n");
    if (kernel::Thread* t = kernel::Current(); t && t->ctx) {
        LOGE("crash signal %d at %p on guest thread %X", sig, reinterpret_cast<void*>(addr), t->id);
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

bool CheckSymbolInterposition() {
    dispatch::Init();
    X86Func* f_logo = dispatch::Find(0x00028940u);
    X86Func* f_swap = dispatch::Find(0x0032BE70u);

    X86Func* tbl_logo = nullptr;
    X86Func* tbl_swap = nullptr;
    for (const X86FuncMapping* m = X86FuncMappings; m->host; ++m) {
        if (m->guest == 0x00028940u) tbl_logo = m->host;
        if (m->guest == 0x0032BE70u) tbl_swap = m->host;
    }

    bool ok_logo = (f_logo == &sub_00028940) && (tbl_logo == &sub_00028940) &&
                   (reinterpret_cast<const void*>(&sub_00028940) != reinterpret_cast<const void*>(&__imp__sub_00028940));
    bool ok_swap = (f_swap == &sub_0032BE70) && (tbl_swap == &sub_0032BE70) &&
                   (reinterpret_cast<const void*>(&sub_0032BE70) != reinterpret_cast<const void*>(&__imp__sub_0032BE70));

    std::printf("[interposition] sub_00028940: host=%p lib_imp=%p table=%p ok=%d\n",
                reinterpret_cast<const void*>(&sub_00028940),
                reinterpret_cast<const void*>(&__imp__sub_00028940),
                reinterpret_cast<const void*>(tbl_logo), ok_logo ? 1 : 0);
    std::printf("[interposition] sub_0032BE70: host=%p lib_imp=%p table=%p ok=%d\n",
                reinterpret_cast<const void*>(&sub_0032BE70),
                reinterpret_cast<const void*>(&__imp__sub_0032BE70),
                reinterpret_cast<const void*>(tbl_swap), ok_swap ? 1 : 0);
    return ok_logo && ok_swap;
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

void PrintUsage(const char* argv0) {
    std::fprintf(stderr,
                 "usage:\n"
                 "  %s --test-fiber-throw\n"
                 "  %s <path-to-iso-or-game-dir> [--data-dir <dir>] [--max-frames N] [--timeout S]\n",
                 argv0, argv0);
}

}  // namespace

int main(int argc, char** argv) {
    InstallCrashHandlers();

    bool test_fiber_throw = false;
    std::string input_path;
    fs::path data_dir = fs::current_path() / "scarface_hdd";
    uint64_t max_frames = 30;
    int timeout_sec = 30;

    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--test-fiber-throw")) {
            test_fiber_throw = true;
        } else if (!std::strcmp(argv[i], "--data-dir") && i + 1 < argc) {
            data_dir = argv[++i];
        } else if (!std::strcmp(argv[i], "--max-frames") && i + 1 < argc) {
            max_frames = static_cast<uint64_t>(std::strtoull(argv[++i], nullptr, 10));
        } else if (!std::strcmp(argv[i], "--timeout") && i + 1 < argc) {
            timeout_sec = std::atoi(argv[++i]);
        } else if (argv[i][0] == '-') {
            PrintUsage(argv[0]);
            return 2;
        } else {
            input_path = argv[i];
        }
    }

    if (test_fiber_throw) {
        bool fiber_ok = fibers::RunSelfTest();
        bool sym_ok = CheckSymbolInterposition();
        std::printf("[self-test] fibers+exceptions=%s interposition=%s -> %s\n",
                    fiber_ok ? "PASS" : "FAIL", sym_ok ? "PASS" : "FAIL",
                    (fiber_ok && sym_ok) ? "PASS" : "FAIL");
        return (fiber_ok && sym_ok) ? 0 : 1;
    }

    if (input_path.empty()) {
        PrintUsage(argv[0]);
        return 2;
    }

    std::error_code ec;
    fs::create_directories(data_dir, ec);
    app::SetExeDir(data_dir);
    logx::Init((data_dir / "logs").string(), app::kGame.log);
    app::Load();
    if (app::kGame.init) app::kGame.init();
    if (app::kGame.window) app::kGame.window(nullptr);

    fs::path src(input_path);
    std::vector<uint8_t> xbe;
    std::string game_dir;

    if (fs::is_regular_file(src, ec)) {
        auto disc = std::make_unique<kernel::Disc>();
        std::string err;
        if (!disc->Open(src, err)) {
            LOGE("disc: failed to open %s: %s", src.string().c_str(), err.c_str());
            return 1;
        }
        if (!disc->Load(IMAGE_XBE, xbe)) {
            LOGE("disc: %s missing from %s", IMAGE_XBE, src.string().c_str());
            return 1;
        }
        kernel::MountDisc(std::move(disc));
        game_dir = src.parent_path().string();
    } else if (fs::is_directory(src, ec)) {
        fs::path xbe_path = FindFileIgnoreCase(src, IMAGE_XBE);
        if (xbe_path.empty()) {
            LOGE("boot: %s not found in %s", IMAGE_XBE, src.string().c_str());
            return 1;
        }
        std::ifstream f(xbe_path, std::ios::binary);
        xbe.assign(std::istreambuf_iterator<char>(f), {});
        game_dir = src.string();
    } else {
        LOGE("boot: %s does not exist", src.string().c_str());
        return 1;
    }

    std::string hdd = app::Instance() > 1 ? "hdd" + std::to_string(app::Instance()) : "hdd";
    if (!kernel::Boot(std::move(xbe), IMAGE_XBE, game_dir, (data_dir / hdd).string())) {
        LOGE("boot: kernel::Boot failed");
        logx::Flush();
        return 1;
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_sec);
    for (;;) {
        uint64_t frames = gpu::HeadlessFrames();
        if (max_frames > 0 && frames >= max_frames) {
            LOGR("headless: reached %llu frames (draws=%llu clears=%llu)",
                 static_cast<unsigned long long>(frames),
                 static_cast<unsigned long long>(gpu::HeadlessDraws()),
                 static_cast<unsigned long long>(gpu::HeadlessClears()));
            logx::Flush();
            _exit(0);
        }
        if (timeout_sec > 0 && std::chrono::steady_clock::now() >= deadline) {
            LOGR("headless: timeout after %ds (frames=%llu draws=%llu clears=%llu)",
                 timeout_sec,
                 static_cast<unsigned long long>(frames),
                 static_cast<unsigned long long>(gpu::HeadlessDraws()),
                 static_cast<unsigned long long>(gpu::HeadlessClears()));
            logx::Flush();
            _exit(frames > 0 ? 0 : 1);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}
