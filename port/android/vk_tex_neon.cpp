#include "vk_tex_neon.h"

#include <algorithm>
#include <array>
#include <cstring>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace vk_tex {

namespace {

inline uint16_t ReadU16LE(const uint8_t* p) {
    uint16_t v = 0;
    std::memcpy(&v, p, 2);
    return v;
}

inline void Expand565(uint16_t c, uint8_t& r, uint8_t& g, uint8_t& b) {
    uint32_t r5 = (c >> 11) & 31u;
    uint32_t g6 = (c >> 5) & 63u;
    uint32_t b5 = c & 31u;
    r = static_cast<uint8_t>((r5 << 3) | (r5 >> 2));
    g = static_cast<uint8_t>((g6 << 2) | (g6 >> 4));
    b = static_cast<uint8_t>((b5 << 3) | (b5 >> 2));
}

inline void BuildColorPalette(uint16_t c0, uint16_t c1, bool allow_1bit_alpha, uint8_t pal[16]) {
    uint8_t r0, g0, b0, r1, g1, b1;
    Expand565(c0, r0, g0, b0);
    Expand565(c1, r1, g1, b1);

    pal[0] = r0; pal[1] = g0; pal[2] = b0; pal[3] = 255;
    pal[4] = r1; pal[5] = g1; pal[6] = b1; pal[7] = 255;

    if (!allow_1bit_alpha || c0 > c1) {
        pal[8]  = static_cast<uint8_t>((2u * r0 + r1 + 1u) / 3u);
        pal[9]  = static_cast<uint8_t>((2u * g0 + g1 + 1u) / 3u);
        pal[10] = static_cast<uint8_t>((2u * b0 + b1 + 1u) / 3u);
        pal[11] = 255;
        pal[12] = static_cast<uint8_t>((r0 + 2u * r1 + 1u) / 3u);
        pal[13] = static_cast<uint8_t>((g0 + 2u * g1 + 1u) / 3u);
        pal[14] = static_cast<uint8_t>((b0 + 2u * b1 + 1u) / 3u);
        pal[15] = 255;
    } else {
        pal[8]  = static_cast<uint8_t>((uint32_t(r0) + r1) >> 1);
        pal[9]  = static_cast<uint8_t>((uint32_t(g0) + g1) >> 1);
        pal[10] = static_cast<uint8_t>((uint32_t(b0) + b1) >> 1);
        pal[11] = 255;
        pal[12] = 0;
        pal[13] = 0;
        pal[14] = 0;
        pal[15] = 0;
    }
}

inline void BuildBC3AlphaTable(uint8_t a0, uint8_t a1, uint8_t alut[16]) {
    std::memset(alut, 0, 16);
    alut[0] = a0;
    alut[1] = a1;
    if (a0 > a1) {
        for (int i = 1; i <= 6; ++i) {
            alut[1 + i] = static_cast<uint8_t>(((7 - i) * uint32_t(a0) + i * uint32_t(a1) + 3u) / 7u);
        }
    } else {
        for (int i = 1; i <= 4; ++i) {
            alut[1 + i] = static_cast<uint8_t>(((5 - i) * uint32_t(a0) + i * uint32_t(a1) + 2u) / 5u);
        }
        alut[6] = 0;
        alut[7] = 255;
    }
}

#if defined(__aarch64__)

inline void StoreRowPartial(uint8_t* dst, uint8x16_t rgba, uint32_t valid_px) {
    alignas(16) uint8_t tmp[16];
    vst1q_u8(tmp, rgba);
    std::memcpy(dst, tmp, size_t(valid_px) * 4);
}

// Compute 16 LUT byte indices (4 pixels x 4 channels) for one 8-bit BC color index row.
inline uint8x16_t RowColorLutIndices(uint8_t row_bits) {
    static const uint8_t kChanOff[16] = {0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3};
    uint32_t b0 = (row_bits & 3u) << 2;
    uint32_t b1 = (row_bits & 0x0Cu);
    uint32_t b2 = (row_bits & 0x30u) >> 2;
    uint32_t b3 = (row_bits & 0xC0u) >> 4;
    uint8x16_t v = {
        uint8_t(b0), uint8_t(b0), uint8_t(b0), uint8_t(b0),
        uint8_t(b1), uint8_t(b1), uint8_t(b1), uint8_t(b1),
        uint8_t(b2), uint8_t(b2), uint8_t(b2), uint8_t(b2),
        uint8_t(b3), uint8_t(b3), uint8_t(b3), uint8_t(b3),
    };
    return vaddq_u8(v, vld1q_u8(kChanOff));
}

template <int AlphaMode>
void DecodeBC_NEON_Impl(const uint8_t* src, uint8_t* dst, uint32_t width, uint32_t height) {
    const uint32_t bw = (width + 3u) / 4u;
    const uint32_t bh = (height + 3u) / 4u;
    const uint32_t stride = width * 4u;
    const uint32_t block_bytes = (AlphaMode == 1) ? 8u : 16u;

    static const uint8_t kChanAlphaMask[16] = {0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255};
    const uint8x16_t alpha_mask = vld1q_u8(kChanAlphaMask);
    static const uint8_t kAlphaRowSpread[16] = {0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3};
    const uint8x16_t alpha_spread = vld1q_u8(kAlphaRowSpread);

    for (uint32_t by = 0; by < bh; ++by) {
        const uint32_t py0 = by * 4u;
        const uint32_t rows = std::min(4u, height - py0);

        for (uint32_t bx = 0; bx < bw; ++bx) {
            const uint8_t* blk = src + (size_t(by) * bw + bx) * block_bytes;
            const uint8_t* cblk = (AlphaMode == 1) ? blk : (blk + 8);

            alignas(16) uint8_t cpal[16];
            BuildColorPalette(ReadU16LE(cblk), ReadU16LE(cblk + 2), AlphaMode == 1, cpal);
            const uint8x16_t color_pal = vld1q_u8(cpal);

            // Decode 4 alpha rows if BC2 or BC3.
            uint8x16_t a_rows[4]{};
            if constexpr (AlphaMode == 2) {
                for (uint32_t r = 0; r < rows; ++r) {
                    uint16_t ar = ReadU16LE(blk + r * 2u);
                    uint8_t a0 = static_cast<uint8_t>((ar & 0xFu) * 17u);
                    uint8_t a1 = static_cast<uint8_t>(((ar >> 4) & 0xFu) * 17u);
                    uint8_t a2 = static_cast<uint8_t>(((ar >> 8) & 0xFu) * 17u);
                    uint8_t a3 = static_cast<uint8_t>(((ar >> 12) & 0xFu) * 17u);
                    uint8x16_t av = {0, 0, 0, a0, 0, 0, 0, a1, 0, 0, 0, a2, 0, 0, 0, a3};
                    a_rows[r] = av;
                }
            } else if constexpr (AlphaMode == 3) {
                alignas(16) uint8_t alut[16];
                BuildBC3AlphaTable(blk[0], blk[1], alut);
                const uint8x16_t alpha_pal = vld1q_u8(alut);
                uint64_t abits = 0;
                std::memcpy(&abits, blk + 2, 6);
                alignas(16) uint8_t a_idx[16];
                for (int k = 0; k < 16; ++k) {
                    a_idx[k] = static_cast<uint8_t>((abits >> (k * 3)) & 7u);
                }
                const uint8x16_t decoded_a = vqtbl1q_u8(alpha_pal, vld1q_u8(a_idx));
                for (uint32_t r = 0; r < rows; ++r) {
                    uint8x16_t row_off = vaddq_u8(alpha_spread, vdupq_n_u8(static_cast<uint8_t>(r * 4u)));
                    a_rows[r] = vqtbl1q_u8(decoded_a, row_off);
                }
            }

            const uint32_t px0 = bx * 4u;
            const uint32_t cols = std::min(4u, width - px0);
            uint8_t* out_base = dst + size_t(py0) * stride + size_t(px0) * 4u;

            for (uint32_t r = 0; r < rows; ++r) {
                uint8x16_t rgba = vqtbl1q_u8(color_pal, RowColorLutIndices(cblk[4u + r]));
                if constexpr (AlphaMode != 1) {
                    rgba = vbslq_u8(alpha_mask, a_rows[r], rgba);
                }
                if (cols == 4u) {
                    vst1q_u8(out_base + size_t(r) * stride, rgba);
                } else {
                    StoreRowPartial(out_base + size_t(r) * stride, rgba, cols);
                }
            }
        }
    }
}

#endif  // __aarch64__

}  // namespace

void DecodeBC1Scalar(const uint8_t* src, uint8_t* dst, uint32_t width, uint32_t height) {
    const uint32_t bw = (width + 3u) / 4u;
    const uint32_t bh = (height + 3u) / 4u;
    for (uint32_t by = 0; by < bh; ++by) {
        for (uint32_t bx = 0; bx < bw; ++bx) {
            const uint8_t* blk = src + (size_t(by) * bw + bx) * 8u;
            uint8_t pal[16];
            BuildColorPalette(ReadU16LE(blk), ReadU16LE(blk + 2), true, pal);
            for (uint32_t y = 0; y < 4u && by * 4u + y < height; ++y) {
                uint8_t row = blk[4u + y];
                uint8_t* out = dst + (size_t(by * 4u + y) * width + bx * 4u) * 4u;
                for (uint32_t x = 0; x < 4u && bx * 4u + x < width; ++x) {
                    uint32_t idx = (row >> (x * 2u)) & 3u;
                    std::memcpy(out + x * 4u, pal + idx * 4u, 4);
                }
            }
        }
    }
}

void DecodeBC2Scalar(const uint8_t* src, uint8_t* dst, uint32_t width, uint32_t height) {
    const uint32_t bw = (width + 3u) / 4u;
    const uint32_t bh = (height + 3u) / 4u;
    for (uint32_t by = 0; by < bh; ++by) {
        for (uint32_t bx = 0; bx < bw; ++bx) {
            const uint8_t* blk = src + (size_t(by) * bw + bx) * 16u;
            uint8_t pal[16];
            BuildColorPalette(ReadU16LE(blk + 8), ReadU16LE(blk + 10), false, pal);
            for (uint32_t y = 0; y < 4u && by * 4u + y < height; ++y) {
                uint16_t arow = ReadU16LE(blk + y * 2u);
                uint8_t crow = blk[12u + y];
                uint8_t* out = dst + (size_t(by * 4u + y) * width + bx * 4u) * 4u;
                for (uint32_t x = 0; x < 4u && bx * 4u + x < width; ++x) {
                    uint32_t idx = (crow >> (x * 2u)) & 3u;
                    std::memcpy(out + x * 4u, pal + idx * 4u, 3);
                    out[x * 4u + 3u] = static_cast<uint8_t>(((arow >> (x * 4u)) & 0xFu) * 17u);
                }
            }
        }
    }
}

void DecodeBC3Scalar(const uint8_t* src, uint8_t* dst, uint32_t width, uint32_t height) {
    const uint32_t bw = (width + 3u) / 4u;
    const uint32_t bh = (height + 3u) / 4u;
    for (uint32_t by = 0; by < bh; ++by) {
        for (uint32_t bx = 0; bx < bw; ++bx) {
            const uint8_t* blk = src + (size_t(by) * bw + bx) * 16u;
            uint8_t alut[16];
            BuildBC3AlphaTable(blk[0], blk[1], alut);
            uint64_t abits = 0;
            std::memcpy(&abits, blk + 2, 6);
            uint8_t pal[16];
            BuildColorPalette(ReadU16LE(blk + 8), ReadU16LE(blk + 10), false, pal);
            for (uint32_t y = 0; y < 4u && by * 4u + y < height; ++y) {
                uint8_t crow = blk[12u + y];
                uint8_t* out = dst + (size_t(by * 4u + y) * width + bx * 4u) * 4u;
                for (uint32_t x = 0; x < 4u && bx * 4u + x < width; ++x) {
                    uint32_t idx = (crow >> (x * 2u)) & 3u;
                    uint32_t aidx = static_cast<uint32_t>((abits >> ((y * 4u + x) * 3u)) & 7u);
                    std::memcpy(out + x * 4u, pal + idx * 4u, 3);
                    out[x * 4u + 3u] = alut[aidx];
                }
            }
        }
    }
}

void DecodeBC1ToRGBA8(const uint8_t* src, uint8_t* dst, uint32_t width, uint32_t height) {
#if defined(__aarch64__)
    DecodeBC_NEON_Impl<1>(src, dst, width, height);
#else
    DecodeBC1Scalar(src, dst, width, height);
#endif
}

void DecodeBC2ToRGBA8(const uint8_t* src, uint8_t* dst, uint32_t width, uint32_t height) {
#if defined(__aarch64__)
    DecodeBC_NEON_Impl<2>(src, dst, width, height);
#else
    DecodeBC2Scalar(src, dst, width, height);
#endif
}

void DecodeBC3ToRGBA8(const uint8_t* src, uint8_t* dst, uint32_t width, uint32_t height) {
#if defined(__aarch64__)
    DecodeBC_NEON_Impl<3>(src, dst, width, height);
#else
    DecodeBC3Scalar(src, dst, width, height);
#endif
}

void ConvertBGRA8ToRGBA8(const uint8_t* src, uint8_t* dst, uint32_t pixel_count) {
    uint32_t i = 0;
#if defined(__aarch64__)
    for (; i + 16u <= pixel_count; i += 16u) {
        uint8x16x4_t px = vld4q_u8(src + size_t(i) * 4u);
        std::swap(px.val[0], px.val[2]);
        vst4q_u8(dst + size_t(i) * 4u, px);
    }
#endif
    for (; i < pixel_count; ++i) {
        const uint8_t* s = src + size_t(i) * 4u;
        uint8_t* d = dst + size_t(i) * 4u;
        uint8_t b = s[0], g = s[1], r = s[2], a = s[3];
        d[0] = r;
        d[1] = g;
        d[2] = b;
        d[3] = a;
    }
}

}  // namespace vk_tex
