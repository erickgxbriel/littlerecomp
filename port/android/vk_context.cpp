#include "vk_context.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#include <SDL3/SDL_vulkan.h>
#include <imgui.h>
#include <backends/imgui_impl_vulkan.h>

#include "android_app.h"
#include "core/log.h"

namespace vk {

namespace {

Context* g_active_ctx = nullptr;

void CheckVkResult(VkResult err) {
    if (err != VK_SUCCESS) {
        LOGE("vk: ImGui Vulkan error %d", int(err));
    }
}

uint64_t NowMs() {
    using clock = std::chrono::steady_clock;
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(clock::now().time_since_epoch()).count());
}

}  // namespace

Context* ActiveContext() { return g_active_ctx; }

bool Context::Init(SDL_Window* window, const std::filesystem::path& cache_dir) {
    window_ = window;
    cache_dir_ = cache_dir;

    Uint32 ext_count = 0;
    const char* const* sdl_exts = SDL_Vulkan_GetInstanceExtensions(&ext_count);
    if (!sdl_exts || ext_count == 0) {
        LOGE("vk: SDL_Vulkan_GetInstanceExtensions failed: %s", SDL_GetError());
        return false;
    }

    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "LittleRecomp";
    app_info.applicationVersion = VK_MAKE_VERSION(0, 3, 0);
    app_info.pEngineName = "XbeRecomp";
    app_info.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    app_info.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app_info;
    ici.enabledExtensionCount = ext_count;
    ici.ppEnabledExtensionNames = sdl_exts;

    if (vkCreateInstance(&ici, nullptr, &instance_) != VK_SUCCESS) {
        LOGE("vk: vkCreateInstance failed");
        return false;
    }

    if (!SDL_Vulkan_CreateSurface(window_, instance_, nullptr, &surface_)) {
        LOGE("vk: SDL_Vulkan_CreateSurface failed: %s", SDL_GetError());
        return false;
    }

    uint32_t gpu_count = 0;
    vkEnumeratePhysicalDevices(instance_, &gpu_count, nullptr);
    if (gpu_count == 0) {
        LOGE("vk: no Vulkan physical devices");
        return false;
    }
    std::vector<VkPhysicalDevice> gpus(gpu_count);
    vkEnumeratePhysicalDevices(instance_, &gpu_count, gpus.data());

    for (VkPhysicalDevice dev : gpus) {
        uint32_t q_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &q_count, nullptr);
        std::vector<VkQueueFamilyProperties> qprops(q_count);
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &q_count, qprops.data());
        for (uint32_t i = 0; i < q_count; ++i) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(dev, i, surface_, &present);
            if ((qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                physical_ = dev;
                queue_family_ = i;
                break;
            }
        }
        if (physical_ != VK_NULL_HANDLE) break;
    }

    if (physical_ == VK_NULL_HANDLE) {
        LOGE("vk: no suitable graphics+present queue family");
        return false;
    }

    VkPhysicalDeviceProperties props{};
    VkPhysicalDeviceFeatures feats{};
    vkGetPhysicalDeviceProperties(physical_, &props);
    vkGetPhysicalDeviceFeatures(physical_, &feats);
    vkGetPhysicalDeviceMemoryProperties(physical_, &mem_props_);

    gpu_name_ = props.deviceName;
    char ver[64];
    std::snprintf(ver, sizeof(ver), "%u.%u.%u",
                  VK_VERSION_MAJOR(props.apiVersion),
                  VK_VERSION_MINOR(props.apiVersion),
                  VK_VERSION_PATCH(props.apiVersion));
    vk_version_ = ver;

    has_bc_ = feats.textureCompressionBC == VK_TRUE;
    has_aniso_ = feats.samplerAnisotropy == VK_TRUE;
    max_aniso_ = has_aniso_ ? std::min(props.limits.maxSamplerAnisotropy, 16.0f) : 1.0f;
    has_wireframe_ = feats.fillModeNonSolid == VK_TRUE;
    has_precise_occlusion_ = feats.occlusionQueryPrecise == VK_TRUE;
    point_size_max_ = std::max(1.0f, props.limits.pointSizeRange[1]);
    min_ubo_align_ = std::max<VkDeviceSize>(16, props.limits.minUniformBufferOffsetAlignment);

    for (uint32_t i = 0; i < mem_props_.memoryTypeCount; ++i) {
        if (mem_props_.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) {
            has_lazy_memory_ = true;
            break;
        }
    }

    // Prefer D32_SFLOAT_S8_UINT for precision, fallback to D24_UNORM_S8_UINT cleanly on Mali/mobile.
    depth_format_ = VK_FORMAT_D24_UNORM_S8_UINT;
    for (VkFormat cand : {VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT}) {
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(physical_, cand, &fp);
        VkFormatFeatureFlags need = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        if ((fp.optimalTilingFeatures & need) == need) {
            depth_format_ = cand;
            break;
        }
    }

    LOGR("vk: GPU=%s Vulkan=%s driver=0x%X BC=%u ASTC=%u ETC2=%u lazyMem=%u depthFmt=%d pointMax=%.1f uboAlign=%llu aniso=%.1f",
         props.deviceName, ver, props.driverVersion,
         unsigned(has_bc_), feats.textureCompressionASTC_LDR, feats.textureCompressionETC2,
         unsigned(has_lazy_memory_), int(depth_format_), point_size_max_,
         static_cast<unsigned long long>(min_ubo_align_), max_aniso_);

    VkPhysicalDeviceFeatures enable_feats{};
    enable_feats.samplerAnisotropy = feats.samplerAnisotropy;
    enable_feats.depthBiasClamp = feats.depthBiasClamp;
    enable_feats.occlusionQueryPrecise = feats.occlusionQueryPrecise;
    enable_feats.independentBlend = feats.independentBlend;
    enable_feats.fillModeNonSolid = feats.fillModeNonSolid;
    enable_feats.textureCompressionBC = feats.textureCompressionBC;

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = queue_family_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    const char* dev_exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = dev_exts;
    dci.pEnabledFeatures = &enable_feats;

    if (vkCreateDevice(physical_, &dci, nullptr, &device_) != VK_SUCCESS) {
        LOGE("vk: vkCreateDevice failed");
        return false;
    }
    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);

    LoadPipelineCache();
    shader_cache_.Init(device_, cache_dir_ / "spv_cache.bin");

    VkCommandPoolCreateInfo cpci{};
    cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci.queueFamilyIndex = queue_family_;
    if (vkCreateCommandPool(device_, &cpci, nullptr, &cmd_pool_) != VK_SUCCESS) {
        LOGE("vk: vkCreateCommandPool failed");
        return false;
    }

    VkDescriptorPoolSize pool_sizes[] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64},
    };
    VkDescriptorPoolCreateInfo dpci{};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dpci.maxSets = 64;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = pool_sizes;
    if (vkCreateDescriptorPool(device_, &dpci, nullptr, &imgui_pool_) != VK_SUCCESS) {
        LOGE("vk: vkCreateDescriptorPool failed");
        return false;
    }

    if (!CreateSwapchain() || !CreateFrameSync()) {
        return false;
    }

    upscaler_.Init(device_, pipeline_cache_, shader_cache_, render_pass_);

    ImGui_ImplVulkan_InitInfo init_info{};
    init_info.Instance = instance_;
    init_info.PhysicalDevice = physical_;
    init_info.Device = device_;
    init_info.QueueFamily = queue_family_;
    init_info.Queue = queue_;
    init_info.PipelineCache = pipeline_cache_;
    init_info.DescriptorPool = imgui_pool_;
    init_info.RenderPass = render_pass_;
    init_info.MinImageCount = 2;
    init_info.ImageCount = std::max(2u, static_cast<uint32_t>(images_.size()));
    init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init_info.CheckVkResultFn = CheckVkResult;
    if (!ImGui_ImplVulkan_Init(&init_info)) {
        LOGE("vk: ImGui_ImplVulkan_Init failed");
        return false;
    }
    imgui_initialized_ = true;
    g_active_ctx = this;
    LOGR("vk: swapchain %ux%u images=%zu preTransform=0x%X",
         extent_.width, extent_.height, images_.size(), unsigned(pre_transform_));
    return true;
}

uint32_t Context::FindMemoryType(uint32_t type_bits, VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred) const {
    if (preferred) {
        for (uint32_t i = 0; i < mem_props_.memoryTypeCount; ++i) {
            if ((type_bits & (1u << i)) &&
                ((mem_props_.memoryTypes[i].propertyFlags & (required | preferred)) == (required | preferred))) {
                return i;
            }
        }
    }
    for (uint32_t i = 0; i < mem_props_.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) && ((mem_props_.memoryTypes[i].propertyFlags & required) == required)) {
            return i;
        }
    }
    return UINT32_MAX;
}

bool Context::SupportsVertexFormat(VkFormat fmt) const {
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(physical_, fmt, &fp);
    return (fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) != 0;
}

void Context::LoadPipelineCache() {
    std::vector<uint8_t> blob;
    if (!cache_dir_.empty()) {
        std::ifstream f(cache_dir_ / "vk_pipeline_cache.bin", std::ios::binary);
        if (f) {
            blob.assign(std::istreambuf_iterator<char>(f), {});
        }
    }
    VkPipelineCacheCreateInfo pcci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    pcci.initialDataSize = blob.size();
    pcci.pInitialData = blob.empty() ? nullptr : blob.data();
    if (vkCreatePipelineCache(device_, &pcci, nullptr, &pipeline_cache_) != VK_SUCCESS) {
        LOGW("vk: vkCreatePipelineCache with initial data failed, creating empty cache");
        pcci.initialDataSize = 0;
        pcci.pInitialData = nullptr;
        vkCreatePipelineCache(device_, &pcci, nullptr, &pipeline_cache_);
    } else if (!blob.empty()) {
        LOGR("vk: loaded vk_pipeline_cache (%zu bytes)", blob.size());
    }
}

void Context::SaveCaches() {
    shader_cache_.FlushToDisk();
    if (device_ == VK_NULL_HANDLE || pipeline_cache_ == VK_NULL_HANDLE || cache_dir_.empty()) return;
    size_t sz = 0;
    if (vkGetPipelineCacheData(device_, pipeline_cache_, &sz, nullptr) != VK_SUCCESS || sz == 0) return;
    std::vector<uint8_t> data(sz);
    if (vkGetPipelineCacheData(device_, pipeline_cache_, &sz, data.data()) != VK_SUCCESS || sz == 0) return;
    std::error_code ec;
    std::filesystem::create_directories(cache_dir_, ec);
    std::filesystem::path dst = cache_dir_ / "vk_pipeline_cache.bin";
    std::filesystem::path tmp = dst;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return;
        f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(sz));
    }
    std::filesystem::rename(tmp, dst, ec);
    LOGR("vk: saved vk_pipeline_cache (%zu bytes)", sz);
}

int Context::BeginPublishSlot(uint32_t width, uint32_t height, MailboxFrame*& out_slot) {
    out_slot = nullptr;
    if (device_ == VK_NULL_HANDLE || width == 0 || height == 0) return -1;

    int chosen = -1;
    {
        std::lock_guard<std::mutex> lk(mailbox_mutex_);
        for (int i = 0; i < kMailboxSlots; ++i) {
            if (i == latest_slot_) continue;
            bool in_flight = false;
            for (const Frame& f : frames_) {
                if (f.mailbox_slot == i) {
                    in_flight = true;
                    break;
                }
            }
            if (!in_flight) {
                chosen = i;
                break;
            }
        }
        writing_slot_ = chosen;
    }
    if (chosen < 0) return -1;

    MailboxFrame& slot = mailbox_[size_t(chosen)];
    if (slot.width != width || slot.height != height || slot.image == VK_NULL_HANDLE) {
        if (slot.view != VK_NULL_HANDLE) vkDestroyImageView(device_, slot.view, nullptr);
        if (slot.image != VK_NULL_HANDLE) vkDestroyImage(device_, slot.image, nullptr);
        if (slot.mem != VK_NULL_HANDLE) vkFreeMemory(device_, slot.mem, nullptr);
        slot.view = VK_NULL_HANDLE;
        slot.image = VK_NULL_HANDLE;
        slot.mem = VK_NULL_HANDLE;

        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_R8G8B8A8_UNORM;
        ici.extent = {width, height, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(device_, &ici, nullptr, &slot.image) != VK_SUCCESS) return -1;

        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(device_, slot.image, &req);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (mai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(device_, &mai, nullptr, &slot.mem) != VK_SUCCESS) {
            vkDestroyImage(device_, slot.image, nullptr);
            slot.image = VK_NULL_HANDLE;
            return -1;
        }
        vkBindImageMemory(device_, slot.image, slot.mem, 0);

        VkImageViewCreateInfo ivci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        ivci.image = slot.image;
        ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivci.format = VK_FORMAT_R8G8B8A8_UNORM;
        ivci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(device_, &ivci, nullptr, &slot.view) != VK_SUCCESS) return -1;

        slot.width = width;
        slot.height = height;
    }

    if (slot.gamma_image == VK_NULL_HANDLE) {
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_R8G8B8A8_UNORM;
        ici.extent = {256, 1, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(device_, &ici, nullptr, &slot.gamma_image) == VK_SUCCESS) {
            VkMemoryRequirements req{};
            vkGetImageMemoryRequirements(device_, slot.gamma_image, &req);
            VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            mai.allocationSize = req.size;
            mai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (mai.memoryTypeIndex != UINT32_MAX && vkAllocateMemory(device_, &mai, nullptr, &slot.gamma_mem) == VK_SUCCESS) {
                vkBindImageMemory(device_, slot.gamma_image, slot.gamma_mem, 0);
                VkImageViewCreateInfo ivci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
                ivci.image = slot.gamma_image;
                ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
                ivci.format = VK_FORMAT_R8G8B8A8_UNORM;
                ivci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                vkCreateImageView(device_, &ivci, nullptr, &slot.gamma_view);
            }
        }
    }

    out_slot = &slot;
    return chosen;
}

void Context::EndPublishSlot(int slot, bool gamma_enabled, uint64_t generation) {
    if (slot < 0 || slot >= kMailboxSlots) return;
    std::lock_guard<std::mutex> lk(mailbox_mutex_);
    mailbox_[size_t(slot)].gamma_enabled = gamma_enabled;
    mailbox_[size_t(slot)].generation = generation;
    latest_slot_ = slot;
    latest_gen_ = generation;
    writing_slot_ = -1;
    mailbox_cv_.notify_all();
}

void Context::InvalidatePublished() {
    std::lock_guard<std::mutex> lk(mailbox_mutex_);
    latest_slot_ = -1;
    latest_gen_ = 0;
    last_presented_gen_ = 0;
    for (Frame& f : frames_) f.mailbox_slot = -1;
}

void Context::DestroyMailbox() {
    InvalidatePublished();
    for (auto& slot : mailbox_) {
        if (slot.view != VK_NULL_HANDLE) vkDestroyImageView(device_, slot.view, nullptr);
        if (slot.image != VK_NULL_HANDLE) vkDestroyImage(device_, slot.image, nullptr);
        if (slot.mem != VK_NULL_HANDLE) vkFreeMemory(device_, slot.mem, nullptr);
        if (slot.gamma_view != VK_NULL_HANDLE) vkDestroyImageView(device_, slot.gamma_view, nullptr);
        if (slot.gamma_image != VK_NULL_HANDLE) vkDestroyImage(device_, slot.gamma_image, nullptr);
        if (slot.gamma_mem != VK_NULL_HANDLE) vkFreeMemory(device_, slot.gamma_mem, nullptr);
        slot = MailboxFrame{};
    }
}

void Context::WaitIdle() {
    if (device_ == VK_NULL_HANDLE) return;
    std::lock_guard<std::mutex> q(queue_mutex_);
    vkDeviceWaitIdle(device_);
}

bool Context::CreateSwapchain() {
    VkSurfaceCapabilitiesKHR caps{};
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &caps) != VK_SUCCESS) {
        LOGE("vk: vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed");
        return false;
    }

    uint32_t fmt_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &fmt_count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(fmt_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &fmt_count, formats.data());

    swap_format_ = formats.empty() ? VK_FORMAT_R8G8B8A8_UNORM : formats[0].format;
    swap_color_space_ = formats.empty() ? VK_COLOR_SPACE_SRGB_NONLINEAR_KHR : formats[0].colorSpace;
    for (const auto& f : formats) {
        if (f.format == VK_FORMAT_R8G8B8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            swap_format_ = f.format;
            swap_color_space_ = f.colorSpace;
            break;
        }
    }

    if (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) {
        pre_transform_ = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    } else {
        pre_transform_ = caps.currentTransform;
    }

    int w = 1280, h = 720;
    SDL_GetWindowSizeInPixels(window_, &w, &h);
    if (pre_transform_ == VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR && w > 0 && h > 0) {
        extent_.width = std::clamp(uint32_t(w), caps.minImageExtent.width, caps.maxImageExtent.width);
        extent_.height = std::clamp(uint32_t(h), caps.minImageExtent.height, caps.maxImageExtent.height);
    } else if (caps.currentExtent.width != UINT32_MAX && caps.currentExtent.width > 0 && caps.currentExtent.height > 0) {
        extent_ = caps.currentExtent;
    } else {
        extent_.width = std::clamp(uint32_t(w), caps.minImageExtent.width, caps.maxImageExtent.width);
        extent_.height = std::clamp(uint32_t(h), caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (extent_.width == 0 || extent_.height == 0) return false;

    uint32_t image_count = std::max(caps.minImageCount + 1, 3u);
    if (caps.maxImageCount > 0 && image_count > caps.maxImageCount) {
        image_count = caps.maxImageCount;
    }

    VkCompositeAlphaFlagBitsKHR comp_alpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    if (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) {
        comp_alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    }

    VkSwapchainCreateInfoKHR sci{};
    sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    sci.surface = surface_;
    sci.minImageCount = image_count;
    sci.imageFormat = swap_format_;
    sci.imageColorSpace = swap_color_space_;
    sci.imageExtent = extent_;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = pre_transform_;
    sci.compositeAlpha = comp_alpha;
    sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    sci.clipped = VK_TRUE;

    if (vkCreateSwapchainKHR(device_, &sci, nullptr, &swapchain_) != VK_SUCCESS) {
        LOGE("vk: vkCreateSwapchainKHR failed");
        return false;
    }

    uint32_t count = 0;
    vkGetSwapchainImagesKHR(device_, swapchain_, &count, nullptr);
    images_.resize(count);
    vkGetSwapchainImagesKHR(device_, swapchain_, &count, images_.data());

    VkAttachmentDescription color_att{};
    color_att.format = swap_format_;
    color_att.samples = VK_SAMPLE_COUNT_1_BIT;
    color_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color_att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color_att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color_att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color_att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference color_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;

    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = 0;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rpci{};
    rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpci.attachmentCount = 1;
    rpci.pAttachments = &color_att;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &subpass;
    rpci.dependencyCount = 1;
    rpci.pDependencies = &dep;
    if (vkCreateRenderPass(device_, &rpci, nullptr, &render_pass_) != VK_SUCCESS) {
        LOGE("vk: vkCreateRenderPass failed");
        return false;
    }

    image_views_.resize(count, VK_NULL_HANDLE);
    framebuffers_.resize(count, VK_NULL_HANDLE);
    for (uint32_t i = 0; i < count; ++i) {
        VkImageViewCreateInfo ivci{};
        ivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        ivci.image = images_[i];
        ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivci.format = swap_format_;
        ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ivci.subresourceRange.levelCount = 1;
        ivci.subresourceRange.layerCount = 1;
        if (vkCreateImageView(device_, &ivci, nullptr, &image_views_[i]) != VK_SUCCESS) return false;

        VkFramebufferCreateInfo fbci{};
        fbci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbci.renderPass = render_pass_;
        fbci.attachmentCount = 1;
        fbci.pAttachments = &image_views_[i];
        fbci.width = extent_.width;
        fbci.height = extent_.height;
        fbci.layers = 1;
        if (vkCreateFramebuffer(device_, &fbci, nullptr, &framebuffers_[i]) != VK_SUCCESS) return false;
    }

    render_finished_.assign(count, VK_NULL_HANDLE);
    VkSemaphoreCreateInfo sci_sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (uint32_t i = 0; i < count; ++i) {
        if (vkCreateSemaphore(device_, &sci_sem, nullptr, &render_finished_[i]) != VK_SUCCESS) return false;
    }
    return true;
}

void Context::DestroySwapchain() {
    for (VkSemaphore s : render_finished_)
        if (s != VK_NULL_HANDLE) vkDestroySemaphore(device_, s, nullptr);
    render_finished_.clear();
    for (VkFramebuffer fb : framebuffers_)
        if (fb != VK_NULL_HANDLE) vkDestroyFramebuffer(device_, fb, nullptr);
    framebuffers_.clear();
    for (VkImageView iv : image_views_)
        if (iv != VK_NULL_HANDLE) vkDestroyImageView(device_, iv, nullptr);
    image_views_.clear();
    if (render_pass_ != VK_NULL_HANDLE) {
        vkDestroyRenderPass(device_, render_pass_, nullptr);
        render_pass_ = VK_NULL_HANDLE;
    }
    if (swapchain_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
    images_.clear();
}

bool Context::CreateFrameSync() {
    VkCommandBufferAllocateInfo cbai{};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = cmd_pool_;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;

    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (auto& f : frames_) {
        if (vkAllocateCommandBuffers(device_, &cbai, &f.cmd) != VK_SUCCESS) return false;
        if (vkCreateSemaphore(device_, &sci, nullptr, &f.image_available) != VK_SUCCESS) return false;
        if (vkCreateFence(device_, &fci, nullptr, &f.in_flight) != VK_SUCCESS) return false;
    }
    return true;
}

void Context::DestroyFrameSync() {
    for (auto& f : frames_) {
        if (f.in_flight != VK_NULL_HANDLE) {
            vkDestroyFence(device_, f.in_flight, nullptr);
            f.in_flight = VK_NULL_HANDLE;
        }
        if (f.image_available != VK_NULL_HANDLE) {
            vkDestroySemaphore(device_, f.image_available, nullptr);
            f.image_available = VK_NULL_HANDLE;
        }
    }
}

bool Context::RecreateSwapchain() {
    if (suspended_ || device_ == VK_NULL_HANDLE || surface_ == VK_NULL_HANDLE) return false;
    WaitIdle();
    DestroySwapchain();
    if (!CreateSwapchain()) return false;
    upscaler_.RecreatePipelines(render_pass_);
    if (imgui_initialized_) {
        ImGui_ImplVulkan_SetMinImageCount(std::max(2u, static_cast<uint32_t>(images_.size())));
    }
    needs_recreate_ = false;
    return true;
}

void Context::NotifyResized() { needs_recreate_ = true; }

bool Context::Suspend() {
    if (suspended_) return true;
    suspended_ = true;
    WaitIdle();
    SaveCaches();
    DestroySwapchain();
    if (surface_ != VK_NULL_HANDLE && instance_ != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }
    return true;
}

bool Context::Resume(SDL_Window* window) {
    window_ = window;
    if (!suspended_) return true;
    if (!SDL_Vulkan_CreateSurface(window_, instance_, nullptr, &surface_)) {
        LOGE("vk: Resume SDL_Vulkan_CreateSurface failed: %s", SDL_GetError());
        return false;
    }
    suspended_ = false;
    return RecreateSwapchain();
}

bool Context::RenderFrame(const std::function<void()>& build_imgui, bool ui_active) {
    if (suspended_ || device_ == VK_NULL_HANDLE || swapchain_ == VK_NULL_HANDLE) return false;
    if (needs_recreate_ && !RecreateSwapchain()) return false;

    // Wait briefly for a new guest frame unless UI/touch is active or periodic HUD refresh is due.
    int snap_slot = -1;
    MailboxFrame snap{};
    {
        std::unique_lock<std::mutex> lk(mailbox_mutex_);
        const uint64_t now = NowMs();
        const uint64_t ui_interval_ms = ui_active ? 16u : 200u;
        const bool need_tick = (now - last_hud_tick_ms_) >= ui_interval_ms;
        if (!need_tick && latest_gen_ > 0 && latest_gen_ == last_presented_gen_) {
            const uint64_t remain = ui_interval_ms - (now - last_hud_tick_ms_);
            mailbox_cv_.wait_for(lk, std::chrono::milliseconds(std::min<uint64_t>(remain, 4u)),
                                 [&] { return latest_gen_ != last_presented_gen_; });
            if (latest_gen_ == last_presented_gen_ && (NowMs() - last_hud_tick_ms_) < ui_interval_ms) {
                return true;  // Skip duplicate swapchain upscale when guest frame hasn't advanced.
            }
        }
        if (latest_slot_ >= 0) {
            snap_slot = latest_slot_;
            snap = mailbox_[size_t(snap_slot)];
            last_presented_gen_ = snap.generation;
        }
        last_hud_tick_ms_ = NowMs();
    }

    Frame& f = frames_[frame_idx_];
    vkWaitForFences(device_, 1, &f.in_flight, VK_TRUE, UINT64_MAX);
    {
        std::lock_guard<std::mutex> lk(mailbox_mutex_);
        f.mailbox_slot = -1;
    }

    uint32_t img_idx = 0;
    VkResult acq = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, f.image_available, VK_NULL_HANDLE, &img_idx);
    if (acq == VK_ERROR_SURFACE_LOST_KHR) {
        Suspend();
        Resume(window_);
        return false;
    }
    if (acq == VK_ERROR_OUT_OF_DATE_KHR) {
        needs_recreate_ = true;
        return false;
    }
    if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) {
        LOGE("vk: vkAcquireNextImageKHR failed (%d)", int(acq));
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(mailbox_mutex_);
        f.mailbox_slot = snap_slot;
    }

    vkResetFences(device_, 1, &f.in_flight);
    vkResetCommandBuffer(f.cmd, 0);

    ImGui_ImplVulkan_NewFrame();
    build_imgui();
    ImGui::Render();
    ImDrawData* draw_data = ImGui::GetDrawData();

    VkCommandBufferBeginInfo cbbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(f.cmd, &cbbi);

    VkClearValue clear{};
    clear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    VkRenderPassBeginInfo rpbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rpbi.renderPass = render_pass_;
    rpbi.framebuffer = framebuffers_[img_idx];
    rpbi.renderArea.extent = extent_;
    rpbi.clearValueCount = 1;
    rpbi.pClearValues = &clear;
    vkCmdBeginRenderPass(f.cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

    if (snap_slot >= 0 && snap.view != VK_NULL_HANDLE) {
        upscaler_.Draw(f.cmd, frame_idx_, snap.view, snap.gamma_view, snap.width, snap.height,
                       snap.gamma_enabled, extent_, app::Widescreen(), app::AndroidSettings().upscaler);
    }
    if (draw_data) ImGui_ImplVulkan_RenderDrawData(draw_data, f.cmd);

    vkCmdEndRenderPass(f.cmd);
    vkEndCommandBuffer(f.cmd);

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &f.image_available;
    si.pWaitDstStageMask = &wait_stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &f.cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &render_finished_[img_idx];

    VkResult pres = VK_SUCCESS;
    {
        std::lock_guard<std::mutex> qlock(queue_mutex_);
        if (vkQueueSubmit(queue_, 1, &si, f.in_flight) != VK_SUCCESS) {
            LOGE("vk: vkQueueSubmit failed");
            return false;
        }
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &render_finished_[img_idx];
        pi.swapchainCount = 1;
        pi.pSwapchains = &swapchain_;
        pi.pImageIndices = &img_idx;
        pres = vkQueuePresentKHR(queue_, &pi);
    }

    if (pres == VK_ERROR_SURFACE_LOST_KHR) {
        Suspend();
        Resume(window_);
    } else if (pres == VK_ERROR_OUT_OF_DATE_KHR) {
        needs_recreate_ = true;
    }

    frame_idx_ = (frame_idx_ + 1) % kFramesInFlight;
    return true;
}

void Context::Shutdown() {
    g_active_ctx = nullptr;
    WaitIdle();
    SaveCaches();
    upscaler_.Shutdown();
    shader_cache_.Shutdown();
    if (imgui_initialized_) {
        ImGui_ImplVulkan_Shutdown();
        imgui_initialized_ = false;
    }
    DestroyMailbox();
    DestroyFrameSync();
    DestroySwapchain();
    if (pipeline_cache_ != VK_NULL_HANDLE) {
        vkDestroyPipelineCache(device_, pipeline_cache_, nullptr);
        pipeline_cache_ = VK_NULL_HANDLE;
    }
    if (imgui_pool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_, imgui_pool_, nullptr);
        imgui_pool_ = VK_NULL_HANDLE;
    }
    if (cmd_pool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device_, cmd_pool_, nullptr);
        cmd_pool_ = VK_NULL_HANDLE;
    }
    if (device_ != VK_NULL_HANDLE) {
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    if (surface_ != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }
}

}  // namespace vk
