#include "vk_tex_neon.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

uint32_t XorShift32(uint32_t& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

std::vector<uint8_t> MakeTestSrc(size_t bytes, uint32_t seed) {
    std::vector<uint8_t> v(bytes);
    for (size_t i = 0; i + 4 <= bytes; i += 4) {
        uint32_t x = XorShift32(seed);
        std::memcpy(v.data() + i, &x, 4);
    }
    return v;
}

int CheckBC1(uint32_t w, uint32_t h, bool force_alpha_mode) {
    size_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    auto src = MakeTestSrc(bw * bh * 8, 0x12345678u ^ w ^ (h << 8));
    if (force_alpha_mode) {
        // Force c0 <= c1 on every block to exercise 3-color + transparent index 3 path.
        for (size_t b = 0; b < bw * bh; ++b) {
            uint8_t* p = src.data() + b * 8;
            uint16_t c0, c1;
            std::memcpy(&c0, p, 2);
            std::memcpy(&c1, p + 2, 2);
            if (c0 > c1) {
                std::memcpy(p, &c1, 2);
                std::memcpy(p + 2, &c0, 2);
            }
        }
    }
    std::vector<uint8_t> ref(size_t(w) * h * 4, 0xAA), neon(size_t(w) * h * 4, 0x55);
    vk_tex::DecodeBC1Scalar(src.data(), ref.data(), w, h);
    vk_tex::DecodeBC1ToRGBA8(src.data(), neon.data(), w, h);
    int diffs = 0;
    for (size_t i = 0; i < ref.size(); ++i) {
        if (ref[i] != neon[i]) ++diffs;
    }
    return diffs;
}

int CheckBC2(uint32_t w, uint32_t h) {
    size_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    auto src = MakeTestSrc(bw * bh * 16, 0xCAFEBABEu ^ w);
    std::vector<uint8_t> ref(size_t(w) * h * 4, 0xAA), neon(size_t(w) * h * 4, 0x55);
    vk_tex::DecodeBC2Scalar(src.data(), ref.data(), w, h);
    vk_tex::DecodeBC2ToRGBA8(src.data(), neon.data(), w, h);
    int diffs = 0;
    for (size_t i = 0; i < ref.size(); ++i) {
        if (ref[i] != neon[i]) ++diffs;
    }
    return diffs;
}

int CheckBC3(uint32_t w, uint32_t h) {
    size_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    auto src = MakeTestSrc(bw * bh * 16, 0xDEADBEEFu ^ h);
    // Alternate a0 > a1 and a0 <= a1 explicitly across blocks.
    for (size_t b = 0; b < bw * bh; ++b) {
        uint8_t* p = src.data() + b * 16;
        if ((b & 1) && p[0] <= p[1]) std::swap(p[0], p[1]);
        if (!(b & 1) && p[0] > p[1]) std::swap(p[0], p[1]);
    }
    std::vector<uint8_t> ref(size_t(w) * h * 4, 0xAA), neon(size_t(w) * h * 4, 0x55);
    vk_tex::DecodeBC3Scalar(src.data(), ref.data(), w, h);
    vk_tex::DecodeBC3ToRGBA8(src.data(), neon.data(), w, h);
    int diffs = 0;
    for (size_t i = 0; i < ref.size(); ++i) {
        if (ref[i] != neon[i]) ++diffs;
    }
    return diffs;
}

int CheckBGRA(uint32_t count) {
    auto src = MakeTestSrc(size_t(count) * 4, 0x99887766u);
    std::vector<uint8_t> neon = src;
    vk_tex::ConvertBGRA8ToRGBA8(neon.data(), neon.data(), count);  // in-place test too
    int diffs = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (neon[i * 4 + 0] != src[i * 4 + 2] || neon[i * 4 + 1] != src[i * 4 + 1] ||
            neon[i * 4 + 2] != src[i * 4 + 0] || neon[i * 4 + 3] != src[i * 4 + 3]) {
            ++diffs;
        }
    }
    return diffs;
}

}  // namespace

int main() {
    int bc1 = 0, bc2 = 0, bc3 = 0, bgra = 0;
    const uint32_t sizes[][2] = {{1, 1}, {2, 2}, {3, 1}, {5, 7}, {64, 64}, {512, 512}};
    for (const auto& s : sizes) {
        bc1 += CheckBC1(s[0], s[1], false);
        bc1 += CheckBC1(s[0], s[1], true);
        bc2 += CheckBC2(s[0], s[1]);
        bc3 += CheckBC3(s[0], s[1]);
    }
    bgra += CheckBGRA(1);
    bgra += CheckBGRA(17);
    bgra += CheckBGRA(4099);

    std::printf("NEON correctness: BC1=%s (%d) BC2=%s (%d) BC3=%s (%d) BGRA=%s (%d)\n",
                bc1 == 0 ? "PASS" : "FAIL", bc1,
                bc2 == 0 ? "PASS" : "FAIL", bc2,
                bc3 == 0 ? "PASS" : "FAIL", bc3,
                bgra == 0 ? "PASS" : "FAIL", bgra);

    // Throughput benchmark on 512x512 textures
    constexpr uint32_t kW = 512, kH = 512, kIters = 100;
    auto src1 = MakeTestSrc((kW / 4) * (kH / 4) * 8, 1);
    auto src3 = MakeTestSrc((kW / 4) * (kH / 4) * 16, 2);
    std::vector<uint8_t> dst(size_t(kW) * kH * 4);

    auto bench = [&](const char* name, auto fn) {
        auto t0 = std::chrono::steady_clock::now();
        for (uint32_t i = 0; i < kIters; ++i) fn();
        double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        double mb = (double(kW) * kH * 4.0 * kIters) / (1024.0 * 1024.0);
        std::printf("Bench %-8s: %.2f ms for %u x 512x512 RGBA8 (%.0f MB/s out)\n",
                    name, sec * 1000.0, kIters, mb / sec);
    };
    bench("BC1_NEON", [&] { vk_tex::DecodeBC1ToRGBA8(src1.data(), dst.data(), kW, kH); });
    bench("BC2_NEON", [&] { vk_tex::DecodeBC2ToRGBA8(src3.data(), dst.data(), kW, kH); });
    bench("BC3_NEON", [&] { vk_tex::DecodeBC3ToRGBA8(src3.data(), dst.data(), kW, kH); });
    bench("BGRA_NEON", [&] { vk_tex::ConvertBGRA8ToRGBA8(dst.data(), dst.data(), kW * kH); });

    return (bc1 || bc2 || bc3 || bgra) ? 1 : 0;
}
