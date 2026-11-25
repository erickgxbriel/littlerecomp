// Mixer output through Oboe (AAudio) with a lock-free SPSC ring buffer so the real-time audio callback
// never blocks on DirectSound's recursive mutex.
#include <oboe/Oboe.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <thread>
#include <vector>

#include "audio/audio.h"
#include "audio/oboe_control.h"
#include "core/log.h"

namespace audio {

namespace {

constexpr uint32_t kRingFrames = 8192;  // power of 2 (~170 ms at 48 kHz)
constexpr uint32_t kRingMask = kRingFrames - 1;
constexpr uint32_t kChunkFrames = kRate / 100;   // 480 frames = 10 ms
constexpr uint32_t kTargetBuffered = kChunkFrames * 3;  // 30 ms prebuffer

float g_ring[kRingFrames * 2] = {};
std::atomic<uint32_t> g_read_pos{0};
std::atomic<uint32_t> g_write_pos{0};

std::atomic<bool> g_paused{false};
std::atomic<bool> g_reopen{false};
std::atomic<uint64_t> g_callbacks{0};
std::atomic<uint64_t> g_underruns{0};
std::atomic<uint32_t> g_rms_bits{0};
std::atomic<int> g_sample_rate{0};
std::atomic<int> g_burst{0};
std::atomic<bool> g_aaudio{false};
std::atomic<bool> g_open{false};

class OboeCallback final : public oboe::AudioStreamDataCallback, public oboe::AudioStreamErrorCallback {
public:
    oboe::DataCallbackResult onAudioReady(oboe::AudioStream*, void* audio_data, int32_t num_frames) override {
        float* out = static_cast<float*>(audio_data);
        if (num_frames <= 0) return oboe::DataCallbackResult::Continue;

        const uint32_t want = static_cast<uint32_t>(num_frames);
        if (g_paused.load(std::memory_order_acquire)) {
            std::memset(out, 0, size_t(want) * 2 * sizeof(float));
            return oboe::DataCallbackResult::Continue;
        }

        const uint32_t r = g_read_pos.load(std::memory_order_relaxed);
        const uint32_t w = g_write_pos.load(std::memory_order_acquire);
        const uint32_t avail = w - r;
        const uint32_t copy = std::min(avail, want);

        if (copy > 0) {
            const uint32_t idx = r & kRingMask;
            const uint32_t first = std::min(copy, kRingFrames - idx);
            std::memcpy(out, &g_ring[size_t(idx) * 2], size_t(first) * 2 * sizeof(float));
            if (copy > first) {
                std::memcpy(out + size_t(first) * 2, g_ring, size_t(copy - first) * 2 * sizeof(float));
            }
            g_read_pos.store(r + copy, std::memory_order_release);
        }

        if (copy < want) {
            std::memset(out + size_t(copy) * 2, 0, size_t(want - copy) * 2 * sizeof(float));
            g_underruns.fetch_add(1, std::memory_order_relaxed);
        }

        g_callbacks.fetch_add(1, std::memory_order_relaxed);
        return oboe::DataCallbackResult::Continue;
    }

    void onErrorAfterClose(oboe::AudioStream*, oboe::Result error) override {
        LOGW("audio: Oboe stream closed with error %s, scheduling reopen", oboe::convertToText(error));
        g_open.store(false, std::memory_order_release);
        g_reopen.store(true, std::memory_order_release);
    }
};

OboeCallback g_callback;
std::mutex g_stream_lock;
std::shared_ptr<oboe::AudioStream> g_stream;

bool OpenStream() {
    std::lock_guard<std::mutex> lock(g_stream_lock);
    if (g_stream) {
        g_stream->stop();
        g_stream->close();
        g_stream.reset();
    }

    oboe::AudioStreamBuilder builder;
    builder.setDirection(oboe::Direction::Output)
        ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
        ->setSharingMode(oboe::SharingMode::Shared)
        ->setFormat(oboe::AudioFormat::Float)
        ->setChannelCount(oboe::ChannelCount::Stereo)
        ->setSampleRate(int32_t(kRate))
        ->setUsage(oboe::Usage::Game)
        ->setContentType(oboe::ContentType::Music)
        ->setDataCallback(&g_callback)
        ->setErrorCallback(&g_callback);

    oboe::Result r = builder.openStream(g_stream);
    if (r != oboe::Result::OK || !g_stream) {
        LOGE("audio: Oboe openStream failed: %s", oboe::convertToText(r));
        g_open.store(false, std::memory_order_release);
        return false;
    }

    g_sample_rate.store(g_stream->getSampleRate(), std::memory_order_relaxed);
    g_burst.store(g_stream->getFramesPerBurst(), std::memory_order_relaxed);
    g_aaudio.store(g_stream->getAudioApi() == oboe::AudioApi::AAudio, std::memory_order_relaxed);

    if (!g_paused.load(std::memory_order_acquire)) {
        r = g_stream->requestStart();
        if (r != oboe::Result::OK) {
            LOGE("audio: Oboe requestStart failed: %s", oboe::convertToText(r));
            g_stream->close();
            g_stream.reset();
            g_open.store(false, std::memory_order_release);
            return false;
        }
    }

    g_open.store(true, std::memory_order_release);
    LOGR("audio: Oboe started (%s, %d Hz, burst=%d, bufCap=%d)",
         g_stream->getAudioApi() == oboe::AudioApi::AAudio ? "AAudio" : "OpenSLES",
         g_stream->getSampleRate(), g_stream->getFramesPerBurst(), g_stream->getBufferCapacityInFrames());
    return true;
}

}  // namespace

void SetPaused(bool paused) {
    bool prev = g_paused.exchange(paused, std::memory_order_acq_rel);
    if (prev == paused) return;
    std::lock_guard<std::mutex> lock(g_stream_lock);
    if (!g_stream) return;
    if (paused) {
        g_stream->requestPause();
    } else {
        g_stream->requestStart();
    }
}

AudioStats GetStats() {
    AudioStats s;
    s.callbacks = g_callbacks.load(std::memory_order_relaxed);
    s.underruns = g_underruns.load(std::memory_order_relaxed);
    uint32_t bits = g_rms_bits.load(std::memory_order_relaxed);
    std::memcpy(&s.rms, &bits, sizeof(float));
    s.sample_rate = g_sample_rate.load(std::memory_order_relaxed);
    s.frames_per_burst = g_burst.load(std::memory_order_relaxed);
    s.aaudio = g_aaudio.load(std::memory_order_relaxed);
    s.open = g_open.load(std::memory_order_relaxed);
    return s;
}

void StartOutput(std::function<void(float*, uint32_t)> render) {
    std::thread([render = std::move(render)]() {
        pthread_setname_np(pthread_self(), "audio_mix");
        OpenStream();

        std::vector<float> tmp(size_t(kChunkFrames) * 2);
        auto next_heartbeat = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        int heartbeats = 0;

        for (;;) {
            if (g_reopen.exchange(false, std::memory_order_acq_rel)) {
                OpenStream();
            }

            if (g_paused.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }

            uint32_t w = g_write_pos.load(std::memory_order_relaxed);
            uint32_t r = g_read_pos.load(std::memory_order_acquire);
            bool rendered = false;

            while ((w - r) < kTargetBuffered && (kRingFrames - (w - r)) >= kChunkFrames) {
                render(tmp.data(), kChunkFrames);

                double sum_sq = 0.0;
                for (float v : tmp) sum_sq += double(v) * double(v);
                float rms = static_cast<float>(std::sqrt(sum_sq / double(tmp.size())));
                uint32_t bits = 0;
                std::memcpy(&bits, &rms, sizeof(float));
                g_rms_bits.store(bits, std::memory_order_relaxed);

                const uint32_t idx = w & kRingMask;
                const uint32_t first = std::min(kChunkFrames, kRingFrames - idx);
                std::memcpy(&g_ring[size_t(idx) * 2], tmp.data(), size_t(first) * 2 * sizeof(float));
                if (kChunkFrames > first) {
                    std::memcpy(g_ring, tmp.data() + size_t(first) * 2, size_t(kChunkFrames - first) * 2 * sizeof(float));
                }
                w += kChunkFrames;
                g_write_pos.store(w, std::memory_order_release);
                rendered = true;
            }

            auto now = std::chrono::steady_clock::now();
            if (heartbeats < 5 && now >= next_heartbeat) {
                AudioStats st = GetStats();
                LOGR("audio: heartbeat cb=%llu underruns=%llu rms=%.4f api=%s",
                     static_cast<unsigned long long>(st.callbacks),
                     static_cast<unsigned long long>(st.underruns), st.rms,
                     st.aaudio ? "AAudio" : "OpenSLES");
                next_heartbeat = now + std::chrono::seconds(2);
                heartbeats++;
            }

            if (!rendered) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }).detach();
}

}  // namespace audio
