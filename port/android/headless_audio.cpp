#include <chrono>
#include <thread>
#include <vector>

#include "audio/audio.h"

namespace audio {

void StartOutput(std::function<void(float*, uint32_t)> render) {
    std::thread([render] {
        std::vector<float> buf(kRate / 100 * 2);
        for (;;) {
            render(buf.data(), kRate / 100);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }).detach();
}

}  // namespace audio
