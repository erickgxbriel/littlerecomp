#include "android_adpf.h"

#include <dlfcn.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <mutex>
#include <string>

#include "core/log.h"

namespace adpf {

namespace {

struct APerformanceHintManager;
struct APerformanceHintSession;
struct AThermalManager;

using FnHintGetMgr = APerformanceHintManager* (*)();
using FnHintCreateSession = APerformanceHintSession* (*)(APerformanceHintManager*, const int32_t*, size_t, int64_t);
using FnHintUpdateTarget = int (*)(APerformanceHintSession*, int64_t);
using FnHintReportActual = int (*)(APerformanceHintSession*, int64_t);
using FnHintCloseSession = void (*)(APerformanceHintSession*);

using FnThermalAcquire = AThermalManager* (*)();
using FnThermalRelease = void (*)(AThermalManager*);
using FnThermalGetStatus = int (*)(AThermalManager*);

std::mutex g_lock;
void* g_libandroid = nullptr;
FnHintGetMgr g_fn_hint_mgr = nullptr;
FnHintCreateSession g_fn_hint_create = nullptr;
FnHintUpdateTarget g_fn_hint_target = nullptr;
FnHintReportActual g_fn_hint_report = nullptr;
FnHintCloseSession g_fn_hint_close = nullptr;

FnThermalAcquire g_fn_therm_acquire = nullptr;
FnThermalRelease g_fn_therm_release = nullptr;
FnThermalGetStatus g_fn_therm_status = nullptr;

APerformanceHintManager* g_hint_mgr = nullptr;
APerformanceHintSession* g_hint_session = nullptr;
AThermalManager* g_therm_mgr = nullptr;

int32_t g_guest_tid = 0;
int32_t g_render_tid = 0;
int64_t g_target_ns = 33333333ll;

struct ThermalZoneChoice {
    std::string temp_path;
    std::string type_name;
    bool is_gpu = false;
    int score = -1;
};

ThermalZoneChoice g_zone;
bool g_zone_probed = false;

std::chrono::steady_clock::time_point g_last_poll{};
ThermalInfo g_cached_thermal{};

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<uint8_t>(c)));
    return s;
}

void ProbeThermalZones() {
    if (g_zone_probed) return;
    g_zone_probed = true;

    DIR* dir = opendir("/sys/class/thermal");
    if (!dir) return;

    struct dirent* ent = nullptr;
    while ((ent = readdir(dir)) != nullptr) {
        if (std::strncmp(ent->d_name, "thermal_zone", 12) != 0) continue;
        std::string base = std::string("/sys/class/thermal/") + ent->d_name;
        std::string temp_path = base + "/temp";
        std::string type_path = base + "/type";
        if (access(temp_path.c_str(), R_OK) != 0) continue;

        std::ifstream tf(type_path);
        std::string tname;
        if (tf) std::getline(tf, tname);
        if (tname.empty()) continue;

        std::string lc = Lower(tname);
        int score = 0;
        bool is_gpu = false;
        if (lc.find("g3d") != std::string::npos || lc.find("gpu") != std::string::npos ||
            lc.find("mali") != std::string::npos || lc.find("kgsl") != std::string::npos ||
            lc.find("adreno") != std::string::npos) {
            score = 100;
            is_gpu = true;
        } else if (lc.find("soc") != std::string::npos || lc.find("ap") != std::string::npos ||
                   lc.find("big") != std::string::npos) {
            score = 50;
        } else if (lc.find("skin") != std::string::npos || lc.find("batt") != std::string::npos) {
            score = 20;
        } else {
            score = 10;
        }

        // Verify a plausible temperature value can actually be read
        std::ifstream vf(temp_path);
        long raw = 0;
        if (!(vf >> raw) || raw <= 0) continue;

        if (score > g_zone.score) {
            g_zone = {temp_path, tname, is_gpu, score};
        }
    }
    closedir(dir);
    if (g_zone.score >= 0) {
        LOGR("adpf: selected sysfs thermal sensor '%s' (%s, gpu=%d)",
             g_zone.type_name.c_str(), g_zone.temp_path.c_str(), int(g_zone.is_gpu));
    } else {
        LOGR("adpf: sysfs thermal zones blocked/unavailable under SELinux, using battery/AThermal fallback");
    }
}

void RebuildHintSessionLocked() {
    if (!g_hint_mgr || !g_fn_hint_create) return;
    int32_t tids[2]{};
    size_t count = 0;
    if (g_guest_tid > 0) tids[count++] = g_guest_tid;
    if (g_render_tid > 0 && g_render_tid != g_guest_tid) tids[count++] = g_render_tid;
    if (count == 0) return;

    if (g_hint_session && g_fn_hint_close) {
        g_fn_hint_close(g_hint_session);
        g_hint_session = nullptr;
    }
    g_hint_session = g_fn_hint_create(g_hint_mgr, tids, count, g_target_ns);
    if (g_hint_session) {
        LOGR("adpf: APerformanceHintSession active (threads=%zu target=%.2fms)",
             count, double(g_target_ns) / 1e6);
    }
}

}  // namespace

void Init() {
    std::lock_guard<std::mutex> lk(g_lock);
    if (!g_libandroid) {
        g_libandroid = dlopen("libandroid.so", RTLD_LAZY);
    }
    if (g_libandroid) {
        g_fn_hint_mgr = reinterpret_cast<FnHintGetMgr>(dlsym(g_libandroid, "APerformanceHint_getManager"));
        g_fn_hint_create = reinterpret_cast<FnHintCreateSession>(dlsym(g_libandroid, "APerformanceHint_createSession"));
        g_fn_hint_target = reinterpret_cast<FnHintUpdateTarget>(dlsym(g_libandroid, "APerformanceHint_updateTargetWorkDuration"));
        g_fn_hint_report = reinterpret_cast<FnHintReportActual>(dlsym(g_libandroid, "APerformanceHint_reportActualWorkDuration"));
        g_fn_hint_close = reinterpret_cast<FnHintCloseSession>(dlsym(g_libandroid, "APerformanceHint_closeSession"));

        g_fn_therm_acquire = reinterpret_cast<FnThermalAcquire>(dlsym(g_libandroid, "AThermal_acquireManager"));
        g_fn_therm_release = reinterpret_cast<FnThermalRelease>(dlsym(g_libandroid, "AThermal_releaseManager"));
        g_fn_therm_status = reinterpret_cast<FnThermalGetStatus>(dlsym(g_libandroid, "AThermal_getCurrentThermalStatus"));
    }
    if (g_fn_hint_mgr && !g_hint_mgr) {
        g_hint_mgr = g_fn_hint_mgr();
    }
    if (g_fn_therm_acquire && !g_therm_mgr) {
        g_therm_mgr = g_fn_therm_acquire();
    }
    ProbeThermalZones();
    LOGR("adpf: init hint_mgr=%d therm_mgr=%d", g_hint_mgr != nullptr ? 1 : 0, g_therm_mgr != nullptr ? 1 : 0);
}

void Shutdown() {
    std::lock_guard<std::mutex> lk(g_lock);
    if (g_hint_session && g_fn_hint_close) {
        g_fn_hint_close(g_hint_session);
        g_hint_session = nullptr;
    }
    if (g_therm_mgr && g_fn_therm_release) {
        g_fn_therm_release(g_therm_mgr);
        g_therm_mgr = nullptr;
    }
}

void RegisterGuestThread(pid_t tid) {
    if (tid <= 0) return;
    std::lock_guard<std::mutex> lk(g_lock);
    if (g_guest_tid == int32_t(tid)) return;
    g_guest_tid = int32_t(tid);
    RebuildHintSessionLocked();
}

void RegisterRenderThread(pid_t tid) {
    if (tid <= 0) return;
    std::lock_guard<std::mutex> lk(g_lock);
    if (g_render_tid == int32_t(tid)) return;
    g_render_tid = int32_t(tid);
    RebuildHintSessionLocked();
}

void UpdateTargetFps(uint32_t fps) {
    int64_t target = (fps >= 55u) ? 16666666ll : 33333333ll;
    std::lock_guard<std::mutex> lk(g_lock);
    if (target == g_target_ns) return;
    g_target_ns = target;
    if (g_hint_session && g_fn_hint_target) {
        g_fn_hint_target(g_hint_session, g_target_ns);
    }
}

void ReportFrameWorkNs(int64_t actual_ns) {
    if (actual_ns <= 0) return;
    std::lock_guard<std::mutex> lk(g_lock);
    if (g_hint_session && g_fn_hint_report) {
        // Clamp outliers so shader warmups don't skew ADPF window excessively
        int64_t clamped = std::clamp<int64_t>(actual_ns, 500000ll, 100000000ll);
        g_fn_hint_report(g_hint_session, clamped);
    }
}

ThermalInfo PollThermal(float battery_fallback_c) {
    std::lock_guard<std::mutex> lk(g_lock);
    auto now = std::chrono::steady_clock::now();
    if (g_last_poll.time_since_epoch().count() != 0 &&
        std::chrono::duration_cast<std::chrono::milliseconds>(now - g_last_poll).count() < 1000) {
        return g_cached_thermal;
    }
    g_last_poll = now;

    ThermalInfo info{};
    if (g_therm_mgr && g_fn_therm_status) {
        int st = g_fn_therm_status(g_therm_mgr);
        if (st >= 0 && st <= 6) {
            info.thermal_status = st;
            info.thermal_status_valid = true;
        }
    }

    if (g_zone.score >= 0) {
        std::ifstream vf(g_zone.temp_path);
        double val = 0.0;
        if (vf >> val && val > 0.0) {
            if (val > 250.0) val *= 0.001;
            info.temp_c = float(val);
            info.is_gpu_sensor = g_zone.is_gpu;
        }
    }
    if (info.temp_c <= 0.0f && battery_fallback_c > 0.0f) {
        info.temp_c = battery_fallback_c;
        info.is_gpu_sensor = false;
    }

    g_cached_thermal = info;
    return info;
}

float ComputeEffectiveScale(float requested_scale, bool drs_enabled, int thermal_status) {
    if (!drs_enabled) return requested_scale;

    static int s_tier = 0;  // 0 = full requested, 1 = -0.5x step, 2 = floor 1.0x
    static float s_last_req = -1.0f;
    static std::chrono::steady_clock::time_point s_hot_since{};
    static std::chrono::steady_clock::time_point s_cool_since{};

    if (std::abs(requested_scale - s_last_req) > 0.05f) {
        s_last_req = requested_scale;
        s_tier = 0;
        s_hot_since = {};
        s_cool_since = {};
    }

    auto now = std::chrono::steady_clock::now();
    if (thermal_status >= 3) {  // SEVERE or worse
        s_cool_since = {};
        if (s_hot_since.time_since_epoch().count() == 0) {
            s_hot_since = now;
        } else if (std::chrono::duration_cast<std::chrono::seconds>(now - s_hot_since).count() >= 3) {
            if (s_tier < 2) {
                ++s_tier;
                LOGR("adpf: Thermal DRS stepping down tier=%d (status=%d)", s_tier, thermal_status);
            }
            s_hot_since = now;
        }
    } else if (thermal_status <= 1) {  // NONE or LIGHT
        s_hot_since = {};
        if (s_tier > 0) {
            if (s_cool_since.time_since_epoch().count() == 0) {
                s_cool_since = now;
            } else if (std::chrono::duration_cast<std::chrono::seconds>(now - s_cool_since).count() >= 15) {
                --s_tier;
                LOGR("adpf: Thermal DRS restoring tier=%d (status=%d)", s_tier, thermal_status);
                s_cool_since = now;
            }
        }
    } else {
        s_hot_since = {};
        s_cool_since = {};
    }

    if (s_tier == 0) return requested_scale;
    if (s_tier == 1) return (requested_scale > 1.6f) ? 1.5f : 1.0f;
    return 1.0f;
}

}  // namespace adpf
