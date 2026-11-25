#pragma once
#include <cstdint>

namespace audio {

void SetPaused(bool paused);

struct AudioStats {
    uint64_t callbacks = 0;
    uint64_t underruns = 0;
    float rms = 0.0f;
    int sample_rate = 0;
    int frames_per_burst = 0;
    bool aaudio = false;
    bool open = false;
};

AudioStats GetStats();

}  // namespace audio
