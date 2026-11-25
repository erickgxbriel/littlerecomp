#include "vk_upscaler.h"

#include <algorithm>

#include "core/log.h"
#include "gpu/shaders.h"

namespace vk {

namespace {

const char* kUpscaleVs = R"(#version 450
precision highp float;
layout(location = 0) out vec2 out_uv;
void main() {
    vec2 t = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(t * vec2(2.0, 2.0) + vec2(-1.0, -1.0), 0.0, 1.0);
    out_uv = t;
}
)";

const char* kUpscaleCommon = R"(
#version 450
precision mediump float;
layout(push_constant) uniform PC {
    highp vec4 src_size; // xy = size, zw = 1/size
    highp vec4 params;   // x = gamma_on, y = sharpness
} pc;
layout(set = 0, binding = 0) uniform highp sampler2D src_tex;
layout(set = 0, binding = 1) uniform highp sampler2D gamma_lut;
layout(location = 0) in highp vec2 in_uv;
layout(location = 0) out vec4 out_color;

vec3 ApplyGamma(vec3 c) {
    if (pc.params.x > 0.5) {
        c.r = textureLod(gamma_lut, vec2(c.r, 0.5), 0.0).r;
        c.g = textureLod(gamma_lut, vec2(c.g, 0.5), 0.0).g;
        c.b = textureLod(gamma_lut, vec2(c.b, 0.5), 0.0).b;
    }
    return c;
}
)";

// Mode 0: Lightweight bilinear sample + LUT
const char* kBilinearBody = R"(
void main() {
    vec4 c = textureLod(src_tex, in_uv, 0.0);
    out_color = vec4(ApplyGamma(c.rgb), 1.0);
}
)";

// Mode 1 (Default): Snapdragon Game Super Resolution v1 inspired single-pass FP16 edge-aware upscaler + sharpen
const char* kGsrV1Body = R"(
float LumaApprox(vec3 c) { return c.g * 0.6 + c.r * 0.3 + c.b * 0.1; }

void main() {
    highp vec2 inv = pc.src_size.zw;
    highp vec2 coord = in_uv * pc.src_size.xy - vec2(0.5);
    highp vec2 base = floor(coord);
    vec2 f = coord - base;
    highp vec2 p00 = (base + vec2(0.5)) * inv;

    // 4 central texels forming bilinear cell + 8 cross/edge neighbors for directional gradient & sharpening
    vec3 c00 = textureLod(src_tex, p00, 0.0).rgb;
    vec3 c10 = textureLod(src_tex, p00 + vec2(inv.x, 0.0), 0.0).rgb;
    vec3 c01 = textureLod(src_tex, p00 + vec2(0.0, inv.y), 0.0).rgb;
    vec3 c11 = textureLod(src_tex, p00 + inv, 0.0).rgb;

    vec3 n  = textureLod(src_tex, p00 + vec2(0.0, -inv.y), 0.0).rgb;
    vec3 n1 = textureLod(src_tex, p00 + vec2(inv.x, -inv.y), 0.0).rgb;
    vec3 w  = textureLod(src_tex, p00 + vec2(-inv.x, 0.0), 0.0).rgb;
    vec3 w1 = textureLod(src_tex, p00 + vec2(-inv.x, inv.y), 0.0).rgb;
    vec3 e  = textureLod(src_tex, p00 + vec2(2.0 * inv.x, 0.0), 0.0).rgb;
    vec3 e1 = textureLod(src_tex, p00 + vec2(2.0 * inv.x, inv.y), 0.0).rgb;
    vec3 s  = textureLod(src_tex, p00 + vec2(0.0, 2.0 * inv.y), 0.0).rgb;
    vec3 s1 = textureLod(src_tex, p00 + vec2(inv.x, 2.0 * inv.y), 0.0).rgb;

    // Directional edge contrast estimate
    float l00 = LumaApprox(c00), l10 = LumaApprox(c10);
    float l01 = LumaApprox(c01), l11 = LumaApprox(c11);
    float dx = abs((l10 - l00) + (l11 - l01));
    float dy = abs((l01 - l00) + (l11 - l10));
    float edge = clamp((dx + dy) * 2.5, 0.0, 1.0);

    // Smoothstep along edge-steepened fractional weights (edge-aware cubic/Hermite curve)
    vec2 w_sharp = clamp(f * f * (3.0 - 2.0 * f), 0.0, 1.0);
    vec2 w_blend = mix(f, w_sharp, edge);

    vec3 top = mix(c00, c10, w_blend.x);
    vec3 bot = mix(c01, c11, w_blend.x);
    vec3 base_col = mix(top, bot, w_blend.y);

    // Local 12-tap ring clamp + adaptive high-frequency GSR sharpening
    vec3 mn = min(min(c00, c10), min(c01, c11));
    vec3 mx = max(max(c00, c10), max(c01, c11));
    vec3 ring = (n + n1 + w + w1 + e + e1 + s + s1) * 0.125;
    float sharp_amt = pc.params.y * (1.0 - edge * 0.35);
    vec3 col = base_col + (base_col - ring) * sharp_amt;
    col = clamp(col, mn * 0.98, mx * 1.02 + vec3(0.002));
    out_color = vec4(ApplyGamma(clamp(col, 0.0, 1.0)), 1.0);
}
)";

// Mode 2: Single-pass FP16 FSR 1.0 style directional EASU + lightweight RCAS
const char* kFsr1Body = R"(
float LumaFsr(vec3 c) { return dot(c, vec3(0.25, 0.50, 0.25)); }

void main() {
    highp vec2 inv = pc.src_size.zw;
    highp vec2 coord = in_uv * pc.src_size.xy - vec2(0.5);
    highp vec2 fp = floor(coord);
    vec2 pp = coord - fp;
    highp vec2 p = (fp + vec2(0.5)) * inv;

    // Core 2x2 quad + cross neighbors
    vec3 b = textureLod(src_tex, p + vec2(0.0, -inv.y), 0.0).rgb;
    vec3 c = textureLod(src_tex, p + vec2(inv.x, -inv.y), 0.0).rgb;
    vec3 e = textureLod(src_tex, p + vec2(-inv.x, 0.0), 0.0).rgb;
    vec3 f = textureLod(src_tex, p, 0.0).rgb;
    vec3 g = textureLod(src_tex, p + vec2(inv.x, 0.0), 0.0).rgb;
    vec3 h = textureLod(src_tex, p + vec2(2.0 * inv.x, 0.0), 0.0).rgb;
    vec3 i = textureLod(src_tex, p + vec2(-inv.x, inv.y), 0.0).rgb;
    vec3 j = textureLod(src_tex, p + vec2(0.0, inv.y), 0.0).rgb;
    vec3 k = textureLod(src_tex, p + inv, 0.0).rgb;
    vec3 l = textureLod(src_tex, p + vec2(2.0 * inv.x, inv.y), 0.0).rgb;
    vec3 n = textureLod(src_tex, p + vec2(0.0, 2.0 * inv.y), 0.0).rgb;
    vec3 o = textureLod(src_tex, p + vec2(inv.x, 2.0 * inv.y), 0.0).rgb;

    // Estimate dominant gradient vector around central cell
    float lf = LumaFsr(f), lg = LumaFsr(g), lj = LumaFsr(j), lk = LumaFsr(k);
    vec2 dir = vec2((LumaFsr(h) - LumaFsr(e)) + (LumaFsr(l) - LumaFsr(i)),
                    (LumaFsr(n) - LumaFsr(b)) + (LumaFsr(o) - LumaFsr(c)));
    float len2 = dot(dir, dir) + 1e-4;
    dir *= inversesqrt(len2);

    // Soft Lanczos-style weights along fractional offset modulated by gradient anisotropy
    vec2 aniso = abs(dir) * 0.35 + vec2(1.0);
    vec2 u = clamp((pp - 0.5) * aniso + 0.5, 0.0, 1.0);
    u = u * u * (3.0 - 2.0 * u);

    vec3 upscaled = mix(mix(f, g, u.x), mix(j, k, u.x), u.y);

    // Inline RCAS cross sharpening (4 outer points e/g/b/j relative to upscaled)
    vec3 mn = min(min(f, g), min(j, k));
    vec3 mx = max(max(f, g), max(j, k));
    vec3 cross_avg = (e + g + b + j) * 0.25;
    float l_min = min(min(lf, lg), min(lj, lk));
    float l_max = max(max(lf, lg), max(lj, lk));
    float amp = clamp(min(l_min, 1.0 - l_max) / max(l_max, 1e-3), 0.0, 1.0);
    float rcas_w = pc.params.y * 0.45 * amp;
    vec3 sharpened = clamp((upscaled - cross_avg * rcas_w) / max(1.0 - rcas_w, 0.25), mn, mx);

    out_color = vec4(ApplyGamma(clamp(sharpened, 0.0, 1.0)), 1.0);
}
)";

struct UpscalePush {
    float src_w, src_h, inv_w, inv_h;
    float gamma_on, sharpness, r0, r1;
};

}  // namespace

bool Upscaler::Init(VkDevice device, VkPipelineCache vk_cache, vk_sh::ShaderCache& shaders, VkRenderPass swapchain_rp) {
    device_ = device;
    vk_cache_ = vk_cache;
    shaders_ = &shaders;

    VkSamplerCreateInfo lsi{};
    lsi.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    lsi.magFilter = VK_FILTER_LINEAR;
    lsi.minFilter = VK_FILTER_LINEAR;
    lsi.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    lsi.addressModeU = lsi.addressModeV = lsi.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(device_, &lsi, nullptr, &linear_sampler_) != VK_SUCCESS) return false;

    VkSamplerCreateInfo psi = lsi;
    psi.magFilter = VK_FILTER_NEAREST;
    psi.minFilter = VK_FILTER_NEAREST;
    if (vkCreateSampler(device_, &psi, nullptr, &point_sampler_) != VK_SUCCESS) return false;

    VkDescriptorSetLayoutBinding binds[2]{};
    binds[0].binding = 0;
    binds[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binds[0].descriptorCount = 1;
    binds[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    binds[1].binding = 1;
    binds[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binds[1].descriptorCount = 1;
    binds[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo dslci{};
    dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslci.bindingCount = 2;
    dslci.pBindings = binds;
    if (vkCreateDescriptorSetLayout(device_, &dslci, nullptr, &set_layout_) != VK_SUCCESS) return false;

    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pcr.offset = 0;
    pcr.size = sizeof(UpscalePush);

    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &set_layout_;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(device_, &plci, nullptr, &pipe_layout_) != VK_SUCCESS) return false;

    VkDescriptorPoolSize dps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kFrames * 2};
    VkDescriptorPoolCreateInfo dpci{};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = kFrames;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &dps;
    if (vkCreateDescriptorPool(device_, &dpci, nullptr, &desc_pool_) != VK_SUCCESS) return false;

    VkDescriptorSetLayout layouts[kFrames] = {set_layout_, set_layout_};
    VkDescriptorSetAllocateInfo dsai{};
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = desc_pool_;
    dsai.descriptorSetCount = kFrames;
    dsai.pSetLayouts = layouts;
    if (vkAllocateDescriptorSets(device_, &dsai, sets_) != VK_SUCCESS) return false;

    vs_mod_ = shaders_->GetOrCompile(gpu::Hash("upscale_vs_v1", 13), false, kUpscaleVs);
    fs_mods_[0] = shaders_->GetOrCompile(gpu::Hash("upscale_bi_v1", 13), true, std::string(kUpscaleCommon) + kBilinearBody);
    fs_mods_[1] = shaders_->GetOrCompile(gpu::Hash("upscale_gsr_v1", 14), true, std::string(kUpscaleCommon) + kGsrV1Body);
    fs_mods_[2] = shaders_->GetOrCompile(gpu::Hash("upscale_fsr_v1", 14), true, std::string(kUpscaleCommon) + kFsr1Body);

    if (!vs_mod_ || !fs_mods_[0] || !fs_mods_[1] || !fs_mods_[2]) {
        LOGE("vk_upscaler: failed compiling upscaler shaders");
        return false;
    }

    ready_ = BuildPipelines(swapchain_rp);
    return ready_;
}

bool Upscaler::BuildPipelines(VkRenderPass rp) {
    DestroyPipelines();
    if (rp == VK_NULL_HANDLE) return false;

    for (int i = 0; i < 3; ++i) {
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vs_mod_;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fs_mods_[i];
        stages[1].pName = "main";

        VkPipelineVertexInputStateCreateInfo vis{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ias{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ias.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vps.viewportCount = 1;
        vps.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineColorBlendAttachmentState cba{};
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cbs.attachmentCount = 1;
        cbs.pAttachments = &cba;

        VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dyns{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dyns.dynamicStateCount = 2;
        dyns.pDynamicStates = dyn;

        VkGraphicsPipelineCreateInfo gpci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gpci.stageCount = 2;
        gpci.pStages = stages;
        gpci.pVertexInputState = &vis;
        gpci.pInputAssemblyState = &ias;
        gpci.pViewportState = &vps;
        gpci.pRasterizationState = &rs;
        gpci.pMultisampleState = &ms;
        gpci.pColorBlendState = &cbs;
        gpci.pDynamicState = &dyns;
        gpci.layout = pipe_layout_;
        gpci.renderPass = rp;

        if (vkCreateGraphicsPipelines(device_, vk_cache_, 1, &gpci, nullptr, &pipelines_[i]) != VK_SUCCESS) {
            LOGE("vk_upscaler: failed creating pipeline %d", i);
            return false;
        }
    }
    ready_ = true;
    return true;
}

bool Upscaler::RecreatePipelines(VkRenderPass swapchain_rp) {
    return BuildPipelines(swapchain_rp);
}

void Upscaler::DestroyPipelines() {
    for (int i = 0; i < 3; ++i) {
        if (pipelines_[i] != VK_NULL_HANDLE) {
            vkDestroyPipeline(device_, pipelines_[i], nullptr);
            pipelines_[i] = VK_NULL_HANDLE;
        }
    }
    ready_ = false;
}

void Upscaler::Shutdown() {
    if (device_ == VK_NULL_HANDLE) return;
    DestroyPipelines();
    if (desc_pool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_, desc_pool_, nullptr);
        desc_pool_ = VK_NULL_HANDLE;
    }
    if (pipe_layout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_, pipe_layout_, nullptr);
        pipe_layout_ = VK_NULL_HANDLE;
    }
    if (set_layout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr);
        set_layout_ = VK_NULL_HANDLE;
    }
    if (linear_sampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(device_, linear_sampler_, nullptr);
        linear_sampler_ = VK_NULL_HANDLE;
    }
    if (point_sampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(device_, point_sampler_, nullptr);
        point_sampler_ = VK_NULL_HANDLE;
    }
    device_ = VK_NULL_HANDLE;
}

void Upscaler::Draw(VkCommandBuffer cmd, uint32_t frame_idx, VkImageView src_view, VkImageView gamma_lut_view,
                    uint32_t src_w, uint32_t src_h, bool gamma_enabled, VkExtent2D swap_extent,
                    bool widescreen, int mode) {
    if (!ready_ || src_view == VK_NULL_HANDLE || src_w == 0 || src_h == 0) return;
    int idx = std::clamp(mode, 0, 2);
    if (pipelines_[idx] == VK_NULL_HANDLE) return;

    VkDescriptorImageInfo imgs[2]{};
    imgs[0].sampler = linear_sampler_;
    imgs[0].imageView = src_view;
    imgs[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imgs[1].sampler = point_sampler_;
    imgs[1].imageView = (gamma_lut_view != VK_NULL_HANDLE) ? gamma_lut_view : src_view;
    imgs[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet writes[2]{};
    for (uint32_t b = 0; b < 2; ++b) {
        writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[b].dstSet = sets_[frame_idx % kFrames];
        writes[b].dstBinding = b;
        writes[b].descriptorCount = 1;
        writes[b].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[b].pImageInfo = &imgs[b];
    }
    vkUpdateDescriptorSets(device_, 2, writes, 0, nullptr);

    const float aspect = widescreen ? (16.0f / 9.0f) : (4.0f / 3.0f);
    const float sw = float(swap_extent.width);
    const float sh = float(swap_extent.height);
    const float vw = std::min(sw, sh * aspect);
    const float vh = vw / aspect;
    const float vx = (sw - vw) * 0.5f;
    const float vy = (sh - vh) * 0.5f;

    VkViewport vp{vx, vy, vw, vh, 0.0f, 1.0f};
    VkRect2D sc{{0, 0}, swap_extent};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);

    UpscalePush pc{
        float(src_w), float(src_h), 1.0f / float(src_w), 1.0f / float(src_h),
        gamma_enabled ? 1.0f : 0.0f,
        (idx == 1) ? 0.35f : (idx == 2) ? 0.55f : 0.0f,
        0.0f, 0.0f
    };

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines_[idx]);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_layout_, 0, 1,
                            &sets_[frame_idx % kFrames], 0, nullptr);
    vkCmdPushConstants(cmd, pipe_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

}  // namespace vk
