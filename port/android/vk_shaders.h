#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan.h>

#include "gpu/shaders.h"

namespace vk_sh {

// Generates GLSL 450 vertex shaders matching Xbox XDK/D3D8 semantics under a negative-height
// Vulkan viewport (0, H, W, -H).
std::string VertexProgramGlsl(const std::vector<uint32_t>& program, uint16_t regs, uint64_t decode,
                              bool quad, bool is_point, bool is_sprite, float point_max);
std::string FixedVertexGlsl(uint16_t regs, bool pretransformed, uint64_t decode, const gpu::FixedKey& fk,
                            bool quad, bool is_point, bool is_sprite, float point_max);

// Generates GLSL 450 fragment shader using mediump (FP16 on Mali Valhall) for combiners/fog/math
// while keeping highp on UVs, derivatives, and shadow comparisons.
std::string PixelGlsl(const gpu::PixelKey& pk, bool is_sprite);

// Utility fullscreen pass shaders.
std::string FullscreenVsGlsl();
std::string BlitPsGlsl();
std::string ClearPsGlsl();
std::string DepthCopyPsGlsl();

class ShaderCache {
public:
    bool Init(VkDevice device, const std::filesystem::path& cache_file);
    void Shutdown();
    void FlushToDisk();

    VkShaderModule GetOrCompile(uint64_t key, bool is_fragment, const std::string& glsl);
    VkShaderModule GetOrCompile(uint64_t key, bool is_fragment, const std::function<std::string()>& build_glsl);

    size_t ModuleCount() const { return modules_.size(); }
    size_t CompileMisses() const { return misses_; }
    size_t DiskHits() const { return disk_hits_; }

private:
    void LoadFromDisk();

    VkDevice device_ = VK_NULL_HANDLE;
    std::filesystem::path cache_file_;
    std::unordered_map<uint64_t, VkShaderModule> modules_;
    std::unordered_map<uint64_t, std::vector<uint32_t>> spv_cache_;
    bool dirty_ = false;
    size_t misses_ = 0;
    size_t disk_hits_ = 0;
    std::mutex lock_;
};

}  // namespace vk_sh
