#pragma once
#include <string>
#include <vector>

namespace cpu_affinity {

// Dynamically inspects /sys/devices/system/cpu/cpu*/cpufreq/cpuinfo_max_freq.
// Never hardcodes SoC-specific core indices.
void Init();

// Pins calling thread to a performance core when heterogeneous big/little cores are detected,
// or resets affinity to all online cores when enabled == false.
bool PinGuestMainThread(bool enabled);
bool PinRenderThread(bool enabled);

// Called from sdk/runtime hooks (guarded by #ifdef __ANDROID__).
void PinGuestMainThreadFromRuntime();
void PinRenderThreadFromRuntime();

void RequestRepinRender();
void PollRenderThreadPin();

bool IsHeterogeneous();
const std::vector<int>& BigCores();
std::string Summary();

}  // namespace cpu_affinity
