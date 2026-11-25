#pragma once
#include <cstdint>

namespace vk_tex {

// Tightly packed RGBA8 output (row_pitch = width * 4), matching VK_FORMAT_R8G8B8A8_UNORM.
// Uses ARM64 NEON on aarch64 and falls back to scalar elsewhere.
void DecodeBC1ToRGBA8(const uint8_t* src, uint8_t* dst, uint32_t width, uint32_t height);
void DecodeBC2ToRGBA8(const uint8_t* src, uint8_t* dst, uint32_t width, uint32_t height);
void DecodeBC3ToRGBA8(const uint8_t* src, uint8_t* dst, uint32_t width, uint32_t height);

// Converts packed BGRA8 pixels to packed RGBA8 pixels in-place or out-of-place.
void ConvertBGRA8ToRGBA8(const uint8_t* src, uint8_t* dst, uint32_t pixel_count);

// Scalar references (exposed for exact unit verification against NEON).
void DecodeBC1Scalar(const uint8_t* src, uint8_t* dst, uint32_t width, uint32_t height);
void DecodeBC2Scalar(const uint8_t* src, uint8_t* dst, uint32_t width, uint32_t height);
void DecodeBC3Scalar(const uint8_t* src, uint8_t* dst, uint32_t width, uint32_t height);

}  // namespace vk_tex
