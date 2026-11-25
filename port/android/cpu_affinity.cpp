#include "cpu_affinity.h"

#include <sched.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>

#include "android_app.h"
#include "core/log.h"

namespace fs = std::filesystem;

namespace cpu_affinity {

namespace {

struct CpuInfo {
    int id = 0;
    uint64_t max_khz = 0;
};

std::once_flag g_init_once;
std::vector<int> g_online_cores;
std::vector<int> g_big_cores;
int g_guest_core = -1;
int g_render_core = -1;
uint64_t g_max_khz = 0;
uint64_t g_min_khz = 0;
bool g_heterogeneous = false;

bool ReadUintFile(const fs::path& p, uint64_t& out) {
    std::ifstream f(p);
    if (!f) return false;
    uint64_t v = 0;
    if (!(f >> v)) return false;
    out = v;
    return true;
}

std::string FormatSummaryUnlocked() {
    if (!g_heterogeneous || g_big_cores.empty()) {
        return "Homogeneous (" + std::to_string(g_online_cores.size()) + " CPUs)";
    }
    std::ostringstream os;
    os << "Big={";
    for (size_t i = 0; i < g_big_cores.size(); ++i) {
        if (i) os << ",";
        os << g_big_cores[i];
    }
    os << "} (" << (g_max_khz / 1000) << "MHz vs " << (g_min_khz / 1000) << "MHz) G=" << g_guest_core
       << " R=" << g_render_core;
    return os.str();
}

void ProbeTopology() {
    std::vector<CpuInfo> cpus;
    std::error_code ec;

    for (int id = 0; id < 64; ++id) {
        fs::path cpu_dir = "/sys/devices/system/cpu/cpu" + std::to_string(id);
        if (!fs::is_directory(cpu_dir, ec)) continue;

        uint64_t online = 1;
        fs::path online_path = cpu_dir / "online";
        if (fs::exists(online_path, ec)) {
            if (!ReadUintFile(online_path, online) || online == 0) continue;
        }
        g_online_cores.push_back(id);

        uint64_t khz = 0;
        if (!ReadUintFile(cpu_dir / "cpufreq/cpuinfo_max_freq", khz)) {
            ReadUintFile(cpu_dir / "cpufreq/scaling_max_freq", khz);
        }
        if (khz > 0) {
            cpus.push_back({id, khz});
        }
    }

    if (cpus.size() < 2) {
        LOGR("cpu_affinity: cpufreq sysfs unavailable (%zu online CPUs), keeping OS scheduling", g_online_cores.size());
        return;
    }

    std::sort(cpus.begin(), cpus.end(), [](const CpuInfo& a, const CpuInfo& b) {
        if (a.max_khz != b.max_khz) return a.max_khz > b.max_khz;
        return a.id < b.id;
    });

    g_max_khz = cpus.front().max_khz;
    g_min_khz = cpus.back().max_khz;
    if (g_max_khz <= g_min_khz) {
        LOGR("cpu_affinity: homogeneous CPU cluster (%llu kHz x%zu), keeping OS scheduling",
             static_cast<unsigned long long>(g_max_khz), cpus.size());
        return;
    }

    g_heterogeneous = true;

    // Collect top frequency tier first.
    for (const auto& c : cpus) {
        if (c.max_khz == g_max_khz) g_big_cores.push_back(c.id);
    }

    // On 1+3+4 SoCs (1 Prime core + N Performance cores + N Little cores),
    // include the middle Performance tier as well so Guest Main & Render each have a fast core
    // without landing on LITTLE efficiency cores.
    if (g_big_cores.size() < 2) {
        uint64_t second_khz = 0;
        for (const auto& c : cpus) {
            if (c.max_khz < g_max_khz && c.max_khz > g_min_khz) {
                second_khz = c.max_khz;
                break;
            }
        }
        if (second_khz > 0) {
            for (const auto& c : cpus) {
                if (c.max_khz == second_khz) g_big_cores.push_back(c.id);
            }
        }
    }

    if (!g_big_cores.empty()) {
        g_guest_core = g_big_cores[0];
        g_render_core = g_big_cores.size() >= 2 ? g_big_cores[1] : g_big_cores[0];
    }

    LOGR("cpu_affinity: detected %s", FormatSummaryUnlocked().c_str());
}

bool ApplyMask(const std::vector<int>& cores, const char* role) {
    if (cores.empty()) return false;
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int id : cores) {
        if (id >= 0 && id < CPU_SETSIZE) CPU_SET(id, &set);
    }
    int rc = sched_setaffinity(0, sizeof(set), &set);
    if (rc != 0) {
        LOGW("cpu_affinity: sched_setaffinity failed for %s", role);
        return false;
    }
    std::ostringstream os;
    os << "{";
    for (size_t i = 0; i < cores.size(); ++i) {
        if (i) os << ",";
        os << cores[i];
    }
    os << "}";
    LOGR("cpu_affinity: %s pinned to mask=%s (running on cpu=%d)", role, os.str().c_str(), sched_getcpu());
    return true;
}

bool PinRole(int preferred_core, bool enabled, const char* role) {
    Init();
    if (!enabled) {
        if (g_online_cores.empty()) return false;
        return ApplyMask(g_online_cores, role);
    }
    if (!g_heterogeneous || g_big_cores.empty()) return false;
    if (preferred_core >= 0 && ApplyMask({preferred_core}, role)) {
        return true;
    }
    return ApplyMask(g_big_cores, role);
}

}  // namespace

void Init() { std::call_once(g_init_once, ProbeTopology); }

bool PinGuestMainThread(bool enabled) { return PinRole(g_guest_core, enabled, "guest_main"); }

bool PinRenderThread(bool enabled) { return PinRole(g_render_core, enabled, "render"); }

void PinGuestMainThreadFromRuntime() { PinGuestMainThread(app::AndroidSettings().pin_big_cores); }

void PinRenderThreadFromRuntime() { PinRenderThread(app::AndroidSettings().pin_big_cores); }

static std::atomic<bool> g_render_repin{false};
void RequestRepinRender() { g_render_repin.store(true, std::memory_order_release); }
void PollRenderThreadPin() {
    if (g_render_repin.exchange(false, std::memory_order_acquire)) {
        PinRenderThread(app::AndroidSettings().pin_big_cores);
    }
}

bool IsHeterogeneous() {
    Init();
    return g_heterogeneous;
}

const std::vector<int>& BigCores() {
    Init();
    return g_big_cores;
}

std::string Summary() {
    Init();
    return FormatSummaryUnlocked();
}

}  // namespace cpu_affinity
