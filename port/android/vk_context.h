#pragma once
#include <array>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <vulkan/vulkan.h>

#include "vk_shaders.h"
#include "vk_upscaler.h"

namespace vk {

class Context;
Context* ActiveContext();

struct MailboxFrame {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkImage gamma_image = VK_NULL_HANDLE;
    VkDeviceMemory gamma_mem = VK_NULL_HANDLE;
    VkImageView gamma_view = VK_NULL_HANDLE;
    uint32_t width = 0;
    uint32_t height = 0;
    bool gamma_enabled = false;
    uint64_t generation = 0;
};

class Context {
public:
    static constexpr int kFramesInFlight = 2;
    static constexpr int kMailboxSlots = 4;

    bool Init(SDL_Window* window, const std::filesystem::path& cache_dir);
    void Shutdown();

    bool Suspend();
    bool Resume(SDL_Window* window);
    void NotifyResized();

    bool RenderFrame(const std::function<void()>& build_imgui, bool ui_active);
    void WaitIdle();

    // Thread-safe mailbox for VulkanBackend (render thread) -> Context swapchain composite (main thread).
    // BeginPublish returns an unused slot index [0..kMailboxSlots-1] and ensures its image matches w x h.
    int BeginPublishSlot(uint32_t width, uint32_t height, MailboxFrame*& out_slot);
    void EndPublishSlot(int slot, bool gamma_enabled, uint64_t generation);
    void InvalidatePublished();

    void SaveCaches();

    uint32_t FindMemoryType(uint32_t type_bits, VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred = 0) const;

    uint32_t Width() const { return extent_.width; }
    uint32_t Height() const { return extent_.height; }
    const std::string& GpuName() const { return gpu_name_; }
    const std::string& VulkanVersion() const { return vk_version_; }

    VkInstance Instance() const { return instance_; }
    VkPhysicalDevice PhysicalDevice() const { return physical_; }
    VkDevice Device() const { return device_; }
    uint32_t QueueFamily() const { return queue_family_; }
    VkQueue Queue() const { return queue_; }
    std::mutex& QueueMutex() { return queue_mutex_; }

    VkPipelineCache PipelineCache() const { return pipeline_cache_; }
    vk_sh::ShaderCache& Shaders() { return shader_cache_; }
    VkRenderPass RenderPass() const { return render_pass_; }
    uint32_t ImageCount() const { return static_cast<uint32_t>(images_.size()); }
    VkDescriptorPool ImGuiDescriptorPool() const { return imgui_pool_; }

    // Hardware capabilities discovered dynamically at Init (for universal APK compatibility).
    bool HasBC() const { return has_bc_; }
    bool HasAnisotropy() const { return has_aniso_; }
    float MaxAnisotropy() const { return max_aniso_; }
    bool HasWireframe() const { return has_wireframe_; }
    bool HasPreciseOcclusion() const { return has_precise_occlusion_; }
    bool HasLazyMemory() const { return has_lazy_memory_; }
    VkFormat DepthFormat() const { return depth_format_; }
    float MaxPointSize() const { return point_size_max_; }
    VkDeviceSize MinUboAlignment() const { return min_ubo_align_; }
    bool SupportsVertexFormat(VkFormat fmt) const;

private:
    bool CreateSwapchain();
    void DestroySwapchain();
    bool CreateFrameSync();
    void DestroyFrameSync();
    bool RecreateSwapchain();
    void LoadPipelineCache();
    void DestroyMailbox();

    SDL_Window* window_ = nullptr;
    std::filesystem::path cache_dir_;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    uint32_t queue_family_ = 0;
    VkQueue queue_ = VK_NULL_HANDLE;
    std::mutex queue_mutex_;

    VkPhysicalDeviceMemoryProperties mem_props_{};
    bool has_bc_ = false;
    bool has_aniso_ = false;
    float max_aniso_ = 1.0f;
    bool has_wireframe_ = false;
    bool has_precise_occlusion_ = false;
    bool has_lazy_memory_ = false;
    VkFormat depth_format_ = VK_FORMAT_D24_UNORM_S8_UINT;
    float point_size_max_ = 1.0f;
    VkDeviceSize min_ubo_align_ = 256;

    VkPipelineCache pipeline_cache_ = VK_NULL_HANDLE;
    vk_sh::ShaderCache shader_cache_;
    Upscaler upscaler_;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swap_format_ = VK_FORMAT_R8G8B8A8_UNORM;
    VkColorSpaceKHR swap_color_space_ = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D extent_{};
    VkSurfaceTransformFlagBitsKHR pre_transform_ = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;

    std::vector<VkImage> images_;
    std::vector<VkImageView> image_views_;
    VkRenderPass render_pass_ = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> framebuffers_;

    VkCommandPool cmd_pool_ = VK_NULL_HANDLE;
    struct Frame {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkSemaphore image_available = VK_NULL_HANDLE;
        VkFence in_flight = VK_NULL_HANDLE;
        int mailbox_slot = -1;
    };
    Frame frames_[kFramesInFlight]{};
    std::vector<VkSemaphore> render_finished_;
    uint32_t frame_idx_ = 0;

    std::mutex mailbox_mutex_;
    std::condition_variable mailbox_cv_;
    std::array<MailboxFrame, kMailboxSlots> mailbox_{};
    int latest_slot_ = -1;
    int writing_slot_ = -1;
    uint64_t latest_gen_ = 0;
    uint64_t last_presented_gen_ = 0;
    uint64_t last_hud_tick_ms_ = 0;

    VkDescriptorPool imgui_pool_ = VK_NULL_HANDLE;
    bool imgui_initialized_ = false;
    bool suspended_ = false;
    bool needs_recreate_ = false;

    std::string gpu_name_;
    std::string vk_version_;
};

}  // namespace vk
