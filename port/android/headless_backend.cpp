#include <atomic>
#include <cstdint>
#include <memory>

#include "core/log.h"
#include "gpu/backend.h"
#include "gpu/texture.h"

namespace gpu {

namespace {

std::atomic<uint64_t> g_frames{0};
std::atomic<uint64_t> g_draws{0};
std::atomic<uint64_t> g_clears{0};

class HeadlessBackend final : public Backend {
public:
    bool Init(uint32_t width, uint32_t height) override {
        LOGR("renderer: headless %ux%u", width, height);
        return true;
    }

    void Clear(const State&, uint32_t, const uint8_t*, uint32_t, uint32_t, float, uint32_t) override {
        g_clears.fetch_add(1, std::memory_order_relaxed);
    }

    void Draw(const State&, uint32_t, uint32_t, uint32_t, const uint8_t*) override {
        g_draws.fetch_add(1, std::memory_order_relaxed);
    }

    void Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects_va, uint32_t dst_points_va,
              float, float) override {
        CopySurfaceMemory(src, dst, count, src_rects_va, dst_points_va);
    }

    void Present(const Surface&) override {
        uint64_t f = g_frames.fetch_add(1, std::memory_order_release) + 1;
        if (f == 1 || f % 10 == 0) {
            LOGR("renderer: frame %llu (draws=%llu clears=%llu)", (unsigned long long)f,
                 (unsigned long long)g_draws.load(std::memory_order_relaxed),
                 (unsigned long long)g_clears.load(std::memory_order_relaxed));
        }
    }

    void Sync(uint32_t, uint32_t) override {}
    void SyncLate(uint32_t) override {}
    void Flush() override {}
    void SetGamma(const uint8_t*) override {}
};

}  // namespace

std::unique_ptr<Backend> CreateBackend() { return std::make_unique<HeadlessBackend>(); }

uint64_t HeadlessFrames() { return g_frames.load(std::memory_order_acquire); }
uint64_t HeadlessDraws() { return g_draws.load(std::memory_order_acquire); }
uint64_t HeadlessClears() { return g_clears.load(std::memory_order_acquire); }

}  // namespace gpu
