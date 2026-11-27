#pragma once

#include <cstdint>
#include <sys/types.h>

namespace adpf {

struct ThermalInfo {
    float temp_c = -1.0f;
    bool is_gpu_sensor = false;
    int thermal_status = 0;  // AThermalStatus: 0=NONE, 1=LIGHT, 2=MODERATE, 3=SEVERE, 4=CRITICAL+
    bool thermal_status_valid = false;
};

void Init();
void Shutdown();

void RegisterGuestThread(pid_t tid);
void RegisterRenderThread(pid_t tid);

void UpdateTargetFps(uint32_t fps);
void ReportFrameWorkNs(int64_t actual_ns);

ThermalInfo PollThermal(float battery_fallback_c);
float ComputeEffectiveScale(float requested_scale, bool drs_enabled, int thermal_status,
                            float current_fps = 0.0f, uint32_t target_fps = 30u);

}  // namespace adpf
