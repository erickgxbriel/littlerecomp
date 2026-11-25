#pragma once
#include <cstdint>

#include <vulkan/vulkan.h>

#include "vk_shaders.h"

namespace vk {

enum class UpscalerMode : int {
    Bilinear = 0,
    SnapdragonGSRv1 = 1,
    FSR1 = 2,
};

// Single-pass FP16 (mediump) upscaler + Gamma LUT composite executed inside Context::RenderPass()
// directly on the swapchain tile, avoiding intermediate fullscreen RAM render targets on TBDR Mali GPUs.
class Upscaler {
public:
    static constexpr int kFrames = 2;

    bool Init(VkDevice device, VkPipelineCache vk_cache, vk_sh::ShaderCache& shaders, VkRenderPass swapchain_rp);
    bool RecreatePipelines(VkRenderPass swapchain_rp);
    void Shutdown();

    void Draw(VkCommandBuffer cmd, uint32_t frame_idx, VkImageView src_view, VkImageView gamma_lut_view,
              uint32_t src_w, uint32_t src_h, bool gamma_enabled, VkExtent2D swap_extent,
              bool widescreen, int mode);

    bool Ready() const { return ready_; }

private:
    bool BuildPipelines(VkRenderPass rp);
    void DestroyPipelines();

    VkDevice device_ = VK_NULL_HANDLE;
    VkPipelineCache vk_cache_ = VK_NULL_HANDLE;
    vk_sh::ShaderCache* shaders_ = nullptr;

    VkSampler linear_sampler_ = VK_NULL_HANDLE;
    VkSampler point_sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipe_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool desc_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet sets_[kFrames]{};

    VkShaderModule vs_mod_ = VK_NULL_HANDLE;
    VkShaderModule fs_mods_[3]{};
    VkPipeline pipelines_[3]{};
    bool ready_ = false;
};

}  // namespace vk
