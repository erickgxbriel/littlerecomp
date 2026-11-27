#include "vk_backend.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <unistd.h>

#include "android_adpf.h"
#include "android_app.h"
#include "core/diag.h"
#include "core/log.h"
#include "cpu_affinity.h"
#include "gpu/shaders.h"
#include "gpu/texture.h"
#include "gpu/vertex.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "vk_context.h"
#include "vk_shaders.h"
#include "vk_tex_neon.h"

namespace gpu {

namespace {

constexpr int kFrames = 3;
constexpr uint64_t kStaleFrames = 8;
constexpr uint64_t kConstRingSize = 12ull << 20;  // 12 MB dynamic UBOs (VC + DC)
constexpr uint64_t kStreamRingSize = 20ull << 20; // 20 MB staging / vertices / indices
constexpr uint32_t kTexDescPoolSize = 4096;
constexpr uint32_t kQueries = 2048;
constexpr uint32_t kNoQuery = ~0u;
constexpr uint64_t kIdleTexFrames = 600;
constexpr size_t kMaxCachedTex = 1536;
constexpr size_t kMaxTexVramBytes = 256ull << 20;

std::mutex g_stats_lock;
VulkanStats g_stats;

enum class RpEndReason : uint8_t { Sw, Tex, RtTex, Fb, Copy, Clear, Sub, Late, Oth, Count };

struct TexSlabAlloc {
    uint32_t slab_idx = UINT32_MAX;
    VkDeviceSize offset = 0;
    VkDeviceSize size = 0;
    bool valid() const { return slab_idx != UINT32_MAX && size > 0; }
};

struct GarbageItem {
    enum class Kind : uint8_t { Image, ImageView, Buffer, Memory, Framebuffer, DescriptorSet, TexSlab };
    Kind kind;
    uint64_t handle = 0;
    TexSlabAlloc slab{};
};

class TexSlabAllocator {
public:
    static constexpr VkDeviceSize kSlabSize = 16ull << 20;     // 16 MB DEVICE_LOCAL slab
    static constexpr VkDeviceSize kMaxSubAlloc = 4ull << 20;  // <= 4 MB sub-allocated; larger use dedicated alloc
    static constexpr size_t kMaxSlabs = 24;                   // up to 384 MB pooled VRAM

    bool Alloc(VkDevice dev, VkDeviceSize size, VkDeviceSize align, uint32_t mem_type,
               VkDeviceMemory& out_mem, TexSlabAlloc& out_alloc) {
        if (!dev || size == 0 || size > kMaxSubAlloc || mem_type == UINT32_MAX) return false;
        align = std::max<VkDeviceSize>(align, 1u);

        for (uint32_t si = 0; si < uint32_t(slabs_.size()); ++si) {
            Slab& s = slabs_[si];
            if (s.mem_type != mem_type) continue;
            for (size_t fi = 0; fi < s.free_list.size(); ++fi) {
                const VkDeviceSize b_off = s.free_list[fi].first;
                const VkDeviceSize b_sz = s.free_list[fi].second;
                const VkDeviceSize aligned = ((b_off + align - 1u) / align) * align;
                const VkDeviceSize pad = aligned - b_off;
                if (b_sz < pad + size) continue;

                const VkDeviceSize rem = b_sz - pad - size;
                if (pad > 0 && rem > 0) {
                    s.free_list[fi] = {b_off, pad};
                    s.free_list.insert(s.free_list.begin() + ptrdiff_t(fi + 1), {aligned + size, rem});
                } else if (pad > 0) {
                    s.free_list[fi] = {b_off, pad};
                } else if (rem > 0) {
                    s.free_list[fi] = {aligned + size, rem};
                } else {
                    s.free_list.erase(s.free_list.begin() + ptrdiff_t(fi));
                }
                out_mem = s.mem;
                out_alloc = {si, aligned, size};
                return true;
            }
        }

        if (slabs_.size() >= kMaxSlabs) return false;
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = kSlabSize;
        mai.memoryTypeIndex = mem_type;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        if (vkAllocateMemory(dev, &mai, nullptr, &mem) != VK_SUCCESS || mem == VK_NULL_HANDLE) {
            return false;
        }
        Slab s;
        s.mem = mem;
        s.mem_type = mem_type;
        if (kSlabSize > size) {
            s.free_list.push_back({size, kSlabSize - size});
        }
        const uint32_t si = uint32_t(slabs_.size());
        slabs_.push_back(std::move(s));
        out_mem = mem;
        out_alloc = {si, 0, size};
        return true;
    }

    void Free(const TexSlabAlloc& a) {
        if (!a.valid() || a.slab_idx >= slabs_.size()) return;
        auto& fl = slabs_[a.slab_idx].free_list;
        auto it = std::lower_bound(fl.begin(), fl.end(), std::make_pair(a.offset, VkDeviceSize(0)));
        it = fl.insert(it, {a.offset, a.size});
        if (it + 1 != fl.end() && it->first + it->second == (it + 1)->first) {
            it->second += (it + 1)->second;
            fl.erase(it + 1);
        }
        if (it != fl.begin() && (it - 1)->first + (it - 1)->second == it->first) {
            (it - 1)->second += it->second;
            fl.erase(it);
        }
    }

    void Shutdown(VkDevice dev) {
        for (Slab& s : slabs_) {
            if (s.mem && dev) vkFreeMemory(dev, s.mem, nullptr);
        }
        slabs_.clear();
    }

    size_t SlabCount() const { return slabs_.size(); }

private:
    struct Slab {
        VkDeviceMemory mem = VK_NULL_HANDLE;
        uint32_t mem_type = 0;
        std::vector<std::pair<VkDeviceSize, VkDeviceSize>> free_list;
    };
    std::vector<Slab> slabs_;
};

struct TexDescEntry {
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkImageView views[4]{};
    uint64_t last_used_present = 0;
};

struct Target {
    uint32_t data = 0;
    uint32_t gw = 0, gh = 0;
    uint32_t w = 0, h = 0;
    uint32_t bytes = 0;
    bool depth = false;
    bool transient_lazy = false;
    bool persistent_forced = false;
    bool ended_mid_frame = false;
    bool content_valid = false;
    bool drawn = false;

    VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView attach_view = VK_NULL_HANDLE; // color or depth+stencil
    VkImageView sample_view = VK_NULL_HANDLE; // color or depth-only aspect
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;

    // Feedback copy (when game samples currently bound RT)
    VkImage fb_image = VK_NULL_HANDLE;
    VkDeviceMemory fb_mem = VK_NULL_HANDLE;
    VkImageView fb_view = VK_NULL_HANDLE;
    VkImageLayout fb_layout = VK_IMAGE_LAYOUT_UNDEFINED;

    // Pending clear folded into LOAD_OP_CLEAR on next RenderPass open
    bool pending_clear_color = false;
    float clear_rgba[4]{};
    bool pending_clear_z = false;
    float clear_z = 0.0f;
    bool pending_clear_s = false;
    uint32_t clear_s = 0;

    uint64_t epoch = 0;
    uint64_t sig = 0;
    std::vector<uint8_t> read;
};

struct Tex {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    TexSlabAlloc slab_alloc{};
    VkImageView view = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
    TextureDesc d;
    uint32_t levels = 1;
    uint64_t sig = 0;
    uint64_t frame = 0;
    uint64_t sig_frame = 0;
    uint64_t epoch = 0;
    uint64_t palette = 0;
    size_t vram_bytes = 0;
    uint8_t mip_skip = 0;
    bool linear_ok = true;
};

struct PsoKey {
    uint64_t vs = 0;
    uint64_t ps = 0;
    uint32_t regs = 0;
    uint32_t elements[16]{};
    uint16_t strides[16]{};
    uint32_t topo = 0;
    uint32_t has_rt = 0;
    uint32_t has_ds = 0;
    uint32_t blend = 0;
    uint32_t src = 0;
    uint32_t dst = 0;
    uint32_t op = 0;
    uint32_t wmask = 0;
    uint32_t zen = 0;
    uint32_t zwrite = 0;
    uint32_t zfunc = 0;
    uint32_t sten = 0;
    uint32_t sfunc = 0;
    uint32_t smask = 0;
    uint32_t swmask = 0;
    uint32_t sfail = 0;
    uint32_t szfail = 0;
    uint32_t spass = 0;
    uint32_t cull = 0;
    uint32_t ccw = 0;
    uint32_t fill = 0;
    uint32_t zbias_on = 0;
    uint32_t sprite = 0;
};

struct RpKey {
    uint8_t has_rt = 0;
    uint8_t has_ds = 0;
    uint8_t rt_load = 0;
    uint8_t rt_store = 0;
    uint8_t ds_load = 0;
    uint8_t ds_store = 0;
    uint8_t s_load = 0;
    uint8_t s_store = 0;
    bool operator==(const RpKey& o) const { return std::memcmp(this, &o, sizeof(*this)) == 0; }
};

struct DrawShaders {
    bool prog = false;
    bool quad = false;
    bool point = false;
    bool sprite = false;
    bool fixed_transform = false;
    gpu::FixedKey fk{};
    uint64_t vkey = 0;
    uint64_t pkey = 0;
    VkShaderModule vs = VK_NULL_HANDLE;
    VkShaderModule fs = VK_NULL_HANDLE;
};

struct DrawTextures {
    TexInfo info[4]{};
    VkImageView views[4]{};
    bool linear_ok[4]{true, true, true, true};
    bool rt[4]{};
    float k[4]{1, 1, 1, 1};
    float wide[4]{};
};

float Bits(uint32_t v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

void Color(uint32_t argb, float* o) {
    o[0] = ((argb >> 16) & 255u) / 255.0f;
    o[1] = ((argb >> 8) & 255u) / 255.0f;
    o[2] = (argb & 255u) / 255.0f;
    o[3] = (argb >> 24) / 255.0f;
}

uint32_t GuestPitch(uint32_t width) { return (width * 4u + 63u) & ~63u; }

uint32_t SimpleState(const State& s, uint32_t method) { return s.simple[method >> 2]; }

VkFormat VertexVkFormat(uint32_t dxgi) {
    switch (dxgi) {
        case 2:  return VK_FORMAT_R32G32B32A32_SFLOAT;
        case 6:  return VK_FORMAT_R32G32B32_SFLOAT;
        case 13: return VK_FORMAT_R16G16B16A16_SNORM;
        case 14: return VK_FORMAT_R16G16B16A16_SINT;
        case 16: return VK_FORMAT_R32G32_SFLOAT;
        case 28: return VK_FORMAT_R8G8B8A8_UNORM;
        case 37: return VK_FORMAT_R16G16_SNORM;
        case 38: return VK_FORMAT_R16G16_SINT;
        case 41: return VK_FORMAT_R32_SFLOAT;
        case 42: return VK_FORMAT_R32_UINT;
        case 49: return VK_FORMAT_R8G8_UNORM;
        case 58: return VK_FORMAT_R16_SNORM;
        case 59: return VK_FORMAT_R16_SINT;
        case 61: return VK_FORMAT_R8_UNORM;
        case 87: return VK_FORMAT_B8G8R8A8_UNORM;
        default: return VK_FORMAT_UNDEFINED;
    }
}

VkBlendFactor BlendFactor(uint32_t v, bool alpha) {
    switch (v) {
        case 0: return VK_BLEND_FACTOR_ZERO;
        case 1: return VK_BLEND_FACTOR_ONE;
        case 0x300: return alpha ? VK_BLEND_FACTOR_SRC_ALPHA : VK_BLEND_FACTOR_SRC_COLOR;
        case 0x301: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case 0x302: return VK_BLEND_FACTOR_SRC_ALPHA;
        case 0x303: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case 0x304: return VK_BLEND_FACTOR_DST_ALPHA;
        case 0x305: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case 0x306: return alpha ? VK_BLEND_FACTOR_DST_ALPHA : VK_BLEND_FACTOR_DST_COLOR;
        case 0x307: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case 0x308: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        case 0x8001:
        case 0x8003: return alpha ? VK_BLEND_FACTOR_CONSTANT_ALPHA : VK_BLEND_FACTOR_CONSTANT_COLOR;
        case 0x8002:
        case 0x8004: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        default: return VK_BLEND_FACTOR_ONE;
    }
}

VkBlendOp BlendOpVk(uint32_t v) {
    switch (v) {
        case 0x800A: return VK_BLEND_OP_SUBTRACT;
        case 0x800B: return VK_BLEND_OP_REVERSE_SUBTRACT;
        case 0x8007: return VK_BLEND_OP_MIN;
        case 0x8008: return VK_BLEND_OP_MAX;
        default: return VK_BLEND_OP_ADD;
    }
}

VkCompareOp CmpOp(uint32_t v) {
    uint32_t n = (v >= 0x200u && v <= 0x207u) ? (v - 0x200u) : 7u;
    return static_cast<VkCompareOp>(n);
}

VkCompareOp ReversedCmpOp(uint32_t v) {
    static const uint8_t mirror[8] = {0, 4, 2, 6, 1, 5, 3, 7};
    return static_cast<VkCompareOp>(mirror[int(CmpOp(v))]);
}

VkStencilOp StencilOpVk(uint32_t v) {
    switch (v) {
        case 0: return VK_STENCIL_OP_ZERO;
        case 0x1E01: return VK_STENCIL_OP_REPLACE;
        case 0x1E02: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
        case 0x1E03: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
        case 0x150A: return VK_STENCIL_OP_INVERT;
        case 0x8507: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
        case 0x8508: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
        default: return VK_STENCIL_OP_KEEP;
    }
}

VkSamplerAddressMode AddressVk(uint32_t v) {
    switch (v) {
        case 2: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case 3:
        case 5: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case 4: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

VkBorderColor NearestBorderColor(uint32_t argb) {
    uint32_t a = (argb >> 24) & 0xFFu;
    if (a < 128u) return VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    uint32_t r = (argb >> 16) & 0xFFu;
    uint32_t g = (argb >> 8) & 0xFFu;
    uint32_t b = argb & 0xFFu;
    return (r + g + b >= 384u) ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
}

constexpr uint32_t kSigWholeMax = 512;
constexpr uint32_t kSigSamples = 16;

inline uint64_t Mix64(uint64_t h, uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    h = (h ^ (h >> 30)) * 0xbf58476d1ce4e5b9ull;
    return h ^ (h >> 27);
}

uint64_t MemSig(uint32_t data, uint32_t total) {
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(data));
    uint64_t sig = 0xcbf29ce484222325ull ^ total;
    if (total == 0) return sig;
    if (total <= kSigWholeMax) {
        uint32_t off = 0;
        for (; off + 8u <= total; off += 8u) {
            uint64_t w = 0;
            std::memcpy(&w, p + off, 8);
            sig = Mix64(sig, w);
        }
        if (off < total) {
            uint64_t tail = 0;
            std::memcpy(&tail, p + off, total - off);
            sig = Mix64(sig, tail);
        }
        return sig;
    }
    const uint32_t span = total - 16u;
    for (uint32_t i = 0; i < kSigSamples; ++i) {
        uint64_t w0 = 0, w1 = 0;
        const uint8_t* sample = p + (uint64_t(span) * i) / (kSigSamples - 1u);
        std::memcpy(&w0, sample, 8);
        std::memcpy(&w1, sample + 8, 8);
        sig = Mix64(sig, w0 ^ (w1 * 0x9e3779b185ebca87ull));
    }
    return sig;
}

bool Changed(const Target& t) {
    if (t.drawn || t.read.empty()) return false;
    uint32_t row = t.gw * 4u;
    uint32_t pitch = GuestPitch(t.gw);
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(t.data));
    for (uint32_t y = 0; y < t.gh; ++y) {
        if (std::memcmp(p + size_t(y) * pitch, &t.read[size_t(y) * row], row)) return true;
    }
    return false;
}

bool LatchCpuWrite(Target& t) {
    uint64_t e = DirtyEpoch(t.data);
    if (e <= t.epoch) return false;
    t.epoch = e;
    uint64_t sig = MemSig(t.data, t.bytes);
    if (sig == t.sig && !Changed(t)) return false;
    t.sig = sig;
    return true;
}

bool UnitTexcoords(const Assembled& a) {
    constexpr float kLo = -1e-4f;
    constexpr float kHi = 1.0001f;
    constexpr uint32_t kSmallDrawVertices = 4096;
    if (!a.vertices.empty() || !a.vertex_count || a.vertex_count > kSmallDrawVertices) return false;
    bool any = false;
    for (int r = 0; r < 16; ++r) {
        const VertexElement& e = a.elements[size_t(r)];
        if (!((a.regs >> r) & 1u) || e.format != 16u) continue;  // DXGI_FORMAT_R32G32_FLOAT
        const VertexBuffer& vb = a.buffers[e.buffer];
        if (!vb.src || !vb.stride || uint64_t(a.vertex_count) * vb.stride > vb.bytes) return false;
        for (uint32_t v = 0; v < a.vertex_count; ++v) {
            float uv[2];
            std::memcpy(uv, vb.src + size_t(v) * vb.stride + e.offset, sizeof(uv));
            if (!(uv[0] >= kLo && uv[0] <= kHi && uv[1] >= kLo && uv[1] <= kHi)) return false;
        }
        any = true;
    }
    return any;
}

bool AnyStageWraps(const State& s) {
    for (const auto& t : s.tss) {
        if (t[kTssAddressU] == 1u || t[kTssAddressV] == 1u) return true;
    }
    return false;
}

void MatMul(const float* x, const float* y, float* o) {
    for (int r = 0; r < 4; ++r) {
        for (int col = 0; col < 4; ++col) {
            o[r * 4 + col] = x[r * 4] * y[col] + x[r * 4 + 1] * y[4 + col] +
                             x[r * 4 + 2] * y[8 + col] + x[r * 4 + 3] * y[12 + col];
        }
    }
}

void Transpose(const float* m, float* o) {
    for (int r = 0; r < 4; ++r) {
        for (int col = 0; col < 4; ++col) o[col * 4 + r] = m[r * 4 + col];
    }
}

void InverseTranspose3x3(const float* wv, float* n) {
    float det = wv[0] * (wv[5] * wv[10] - wv[6] * wv[9]) - wv[1] * (wv[4] * wv[10] - wv[6] * wv[8]) +
                wv[2] * (wv[4] * wv[9] - wv[5] * wv[8]);
    float id = det != 0.0f ? 1.0f / det : 0.0f;
    for (int r = 0; r < 3; ++r) {
        for (int col = 0; col < 3; ++col) {
            int r1 = (r + 1) % 3, r2 = (r + 2) % 3;
            int c1 = (col + 1) % 3, c2 = (col + 2) % 3;
            n[r * 4 + col] = (wv[r1 * 4 + c1] * wv[r2 * 4 + c2] - wv[r1 * 4 + c2] * wv[r2 * 4 + c1]) * id;
        }
    }
}

void FillTransformConstants(const State& s, uint32_t gw, uint32_t gh, DrawConstants& c) {
    float wv[16], wvp[16];
    MatMul(s.transforms[256].data(), s.transforms[2].data(), wv);
    MatMul(wv, s.transforms[3].data(), wvp);
    const auto& v = s.viewport;
    float sx = float(v.w) / gw, sy = float(v.h) / gh;
    float ox = (2.0f * v.x + v.w) / gw - 1.0f;
    float oy = 1.0f - (2.0f * v.y + v.h) / gh;
    float vpm[16] = {sx, 0, 0, 0, 0, sy, 0, 0, 0, 0, v.max_z - v.min_z, 0, ox, oy, v.min_z, 1};
    float full[16];
    MatMul(wvp, vpm, full);
    Transpose(full, c.wvp);
    Transpose(wv, c.wv);
    float n[16] = {};
    InverseTranspose3x3(wv, n);
    Transpose(n, c.wvn);
    for (int t = 0; t < 4; ++t) Transpose(s.transforms[16 + size_t(t)].data(), c.tm[t]);
}

void FillLightConstants(const State& s, const FixedKey& fk, DrawConstants& c) {
    std::memcpy(c.mat, s.material.data(), sizeof(float) * 16);
    c.mat[4][0] = s.material[16];
    Color(s.rs[kRsAmbient], c.amb);
    const float* vm = s.transforms[2].data();
    for (int i = 0; i < 8; ++i) {
        if (!((fk.types >> (i * 2)) & 3u)) continue;
        float f[26];
        std::memcpy(f, s.lights[size_t(i)].data(), sizeof(f));
        auto& o = c.lt[i];
        for (int j = 0; j < 4; ++j) {
            o[0][j] = f[1 + j];
            o[1][j] = f[5 + j];
            o[2][j] = f[9 + j];
        }
        const float* pos = f + 13;
        const float* dir = f + 16;
        for (int j = 0; j < 3; ++j) {
            o[3][j] = pos[0] * vm[j] + pos[1] * vm[4 + j] + pos[2] * vm[8 + j] + vm[12 + j];
            o[4][j] = dir[0] * vm[j] + dir[1] * vm[4 + j] + dir[2] * vm[8 + j];
        }
        o[3][3] = f[19];
        o[4][3] = f[20];
        o[5][0] = f[21];
        o[5][1] = f[22];
        o[5][2] = f[23];
        o[6][0] = std::cos(f[24] * 0.5f);
        o[6][1] = std::cos(f[25] * 0.5f);
    }
}

void FillDepthConstants(const State& s, bool prog, uint32_t ds_w, uint32_t ds_gw, DrawConstants& c) {
    constexpr uint32_t kZUseW = 2, kMClipMin = 0x394, kMClipMax = 0x398;
    float units = SimpleState(s, kMSolidOffsetEnable) ? Bits(SimpleState(s, kMDepthOffset)) : 0.0f;
    if (s.rs[kRsZEnable] != kZUseW) {
        c.zparams[2] = units / s.zscale;
        return;
    }
    float near_w = 0.0f, far_w = s.zscale;
    if (!prog) {
        const float* m = s.transforms[3].data();
        float m33 = m[10], m34 = m[11], m43 = m[14], m44 = m[15];
        if (m33 != m34 && m33 != 0.0f) {
            near_w = m44 - m43 / m33 * m34;
            far_w = (m44 - m43) / (m33 - m34) * m34 + m44;
        } else {
            far_w = 1.0f;
        }
    } else if (SimpleState(s, kMClipMax)) {
        near_w = Bits(SimpleState(s, kMClipMin));
    }
    if (near_w <= 0.0f || near_w >= far_w) near_w = far_w * (1.0f / 65536.0f);
    c.zparams[0] = near_w / (far_w - near_w);
    c.zparams[1] = far_w;
    c.zparams[2] = prog ? units : units * far_w / s.zscale;
    (void)ds_w;
    (void)ds_gw;
}

void PipelineStageForLayout(VkImageLayout layout, VkPipelineStageFlags& stage, VkAccessFlags& access) {
    switch (layout) {
        case VK_IMAGE_LAYOUT_UNDEFINED:
            stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            access = 0;
            break;
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
            stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            access = VK_ACCESS_TRANSFER_WRITE_BIT;
            break;
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
            stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            access = VK_ACCESS_TRANSFER_READ_BIT;
            break;
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
            stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            break;
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
            stage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
            access = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            break;
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            access = VK_ACCESS_SHADER_READ_BIT;
            break;
        default:
            stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            access = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            break;
    }
}

void TransitionImageRaw(VkCommandBuffer cmd, VkImage img, VkImageAspectFlags aspect,
                        uint32_t levels, uint32_t layers, VkImageLayout& cur, VkImageLayout want) {
    if (cur == want || img == VK_NULL_HANDLE) return;
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = cur;
    b.newLayout = want;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {aspect, 0, levels, 0, layers};

    VkPipelineStageFlags src_stage = 0, dst_stage = 0;
    PipelineStageForLayout(cur, src_stage, b.srcAccessMask);
    PipelineStageForLayout(want, dst_stage, b.dstAccessMask);
    if (want == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
        b.dstAccessMask |= VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
    } else if (want == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
        b.dstAccessMask |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    }
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
    cur = want;
}

bool SpriteOnStage3(const State& s, uint32_t prim) {
    bool prog = (s.vs_handle & 1u) && s.vs && !s.vs->program.empty();
    bool stage3 = s.tss[3][kTssColorOp] != 1u;
    return prim == 1u && prog && !s.ps && s.rs[kRsPointSpriteEnable] && stage3;
}

State SpriteStageZero(const State& s) {
    State moved = s;
    moved.tss[0] = s.tss[3];
    moved.textures[0] = s.textures[3];
    moved.palettes[0] = s.palettes[3];
    for (int i = 1; i < 4; ++i) {
        moved.tss[i][kTssColorOp] = 1u;
        moved.tss[i][kTssAlphaOp] = 1u;
        moved.textures[i] = 0;
    }
    return moved;
}

class VulkanBackend final : public Backend {
public:
    ~VulkanBackend() override;
    bool Init(uint32_t width, uint32_t height) override;
    void Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color,
               float z, uint32_t stencil) override;
    void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) override;
    void Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects_va,
              uint32_t dst_points_va, float src_kx, float src_ky) override;
    void Present(const Surface& front) override;
    void Sync(uint32_t data_va, uint32_t size) override;
    void SyncLate(uint32_t data_va) override;
    bool DeliverLate(uint32_t data_va) override;
    void RecordLate(uint32_t data_va) override;
    void Flush() override;
    void SetGamma(const uint8_t* ramp) override;
    void ClearReport() override;
    void GetReport(uint32_t report) override;
    bool HasPendingReports() const override { return pending_reports_.load(std::memory_order_acquire) > 0; }
    void PollReports() override;
    void WaitGpuFrame(uint64_t n) override;

private:
    struct RingAlloc {
        VkBuffer buf = VK_NULL_HANDLE;
        uint64_t offset = 0;
        uint8_t* cpu = nullptr;
    };

    struct Frame {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkCommandBuffer upload_cmd = VK_NULL_HANDLE;
        bool upload_recording = false;
        VkFence fence = VK_NULL_HANDLE;
        std::mutex fence_mutex;
        bool submitted = false;
        uint64_t fence_serial = 0;

        VkBuffer const_buf = VK_NULL_HANDLE;
        VkDeviceMemory const_mem = VK_NULL_HANDLE;
        uint8_t* const_map = nullptr;
        uint64_t const_used = 0;

        VkBuffer stream_buf = VK_NULL_HANDLE;
        VkDeviceMemory stream_mem = VK_NULL_HANDLE;
        uint8_t* stream_map = nullptr;
        uint64_t stream_used = 0;

        VkDescriptorSet ubo_set = VK_NULL_HANDLE;
        std::unordered_map<uint64_t, VkDeviceSize> vb_cache;
        std::vector<GarbageItem> garbage;
    };

    struct Late {
        VkBuffer rb[kFrames]{};
        VkDeviceMemory rb_mem[kFrames]{};
        uint8_t* rb_map[kFrames]{};
        VkImage down = VK_NULL_HANDLE;
        VkDeviceMemory down_mem = VK_NULL_HANDLE;
        VkImageView down_view = VK_NULL_HANDLE;
        VkImageLayout down_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        uint64_t serial[kFrames]{};
        uint64_t frame[kFrames]{};
        uint64_t delivered = 0;
        uint32_t gw = 0, gh = 0;
        uint32_t next = 0;
    };

    struct Query {
        uint32_t index = 0;
        float samples_per_pixel = 1.0f;
    };
    struct Report {
        uint32_t va = 0;
        std::vector<Query> queries;
        uint64_t serial = 0;
    };

    Frame& CurFrame() { return frames_[frame_idx_ % kFrames]; }

    bool CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags req,
                      VkMemoryPropertyFlags pref, VkBuffer& buf, VkDeviceMemory& mem, void** map = nullptr);
    bool CreateImage2D(uint32_t w, uint32_t h, uint32_t levels, uint32_t layers, VkImageType type,
                       VkImageCreateFlags flags, VkFormat fmt, VkImageUsageFlags usage,
                       VkMemoryPropertyFlags req, VkMemoryPropertyFlags pref,
                       VkImage& img, VkDeviceMemory& mem);
    bool CreateDummies();
    void DeferDestroy(GarbageItem::Kind k, uint64_t handle);
    void DeferView(VkImageView& v);
    void DeferSlab(TexSlabAlloc& s);
    void InvalidateTexDescriptorsForView(VkImageView view);
    void InvalidateBoundState();
    template <typename T>
    void Defer(GarbageItem::Kind k, T& h) {
        if (h != VK_NULL_HANDLE) {
            DeferDestroy(k, reinterpret_cast<uint64_t>(h));
            h = VK_NULL_HANDLE;
        }
    }
    void CollectGarbage(Frame& f);

    void BeginCommandBuffer();
    VkCommandBuffer BeginUploadCmd();
    void SubmitCurrent(bool wait, uint64_t* wait_bucket = nullptr);
    void AdvanceCompletedSerial(uint64_t ser);
    bool WaitFrameIfNeeded(Frame& f, uint64_t max_serial, uint64_t* wait_ns = nullptr);
    void WaitSerial(uint64_t serial);
    void PollCompletedSerial();
    void DeliverCompletedReports();

    RingAlloc AllocConst(uint64_t size, uint64_t align);
    RingAlloc AllocStream(uint64_t size, uint64_t align);

    VkRenderPass GetRenderPass(const RpKey& key);
    VkFramebuffer GetFramebuffer(Target* rt, Target* ds, VkRenderPass rp, uint32_t w, uint32_t h);
    void EndRenderPass(RpEndReason reason = RpEndReason::Oth);
    bool EnsureRenderPass(Target* rt, Target* ds);

    uint64_t SizedTargetKey(uint32_t data, bool depth, uint32_t gw, uint32_t gh) const;
    Target* FindTarget(const Surface& s, bool depth);
    Target* EnsureTarget(const Surface& s, bool depth);
    bool CreateTargetResources(Target& t);
    void ReleaseTarget(Target& t);
    void UpdateTargetUsed(uint32_t data);
    bool TargetStale(uint32_t data) const;
    void PromoteDepthToPersistent(Target& ds);
    void UploadTarget(Target& t, const Surface& s);
    VkImageView EnsureFeedback(Target& t);

    bool PrepareBindTargets(const State& s, Target*& out_rt, Target*& out_ds, bool zs_sampled);
    void PrepareDrawTextures(const State& s, DrawTextures& stages, bool prog);
    VkImageView BindTargetTexture(const State& s, int stage, const TextureDesc& d, Target& t,
                                  DrawTextures& stages, bool box);
    VkImageView BindGuestTexture(const State& s, int stage, const TextureDesc& d,
                                 const FormatInfo& fi, DrawTextures& stages);
    bool CreateTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi);
    void UploadTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, const uint8_t* src,
                       const uint32_t* palette, bool same);

    VkSampler GetStageSampler(const State& s, int i, const DrawTextures& stages, bool clamp, bool fixed, bool lin_ok);
    VkDescriptorSet GetOrCreateTexSet(const VkImageView views[4], const VkSampler samplers[4]);

    VkPipeline GetDrawPipeline(const PsoKey& k, const Assembled& a, VkShaderModule vs, VkShaderModule fs, VkRenderPass rp);
    VkPipeline GetUtilPipeline(const char* tag, VkShaderModule vs, VkShaderModule fs, VkFormat color_fmt,
                               bool is_depth, uint8_t color_mask, bool alpha_over, VkRenderPass rp);

    void DrawFullscreenUtil(VkPipeline pipe, const float dc_data[8], VkImageView src_view,
                            VkSampler smp, VkExtent2D ext, const VkRect2D* scissor_override = nullptr);
    bool DepthCopy(const State& s);
    void ClearChannels(Target& t, Target* ds_hint, const float col[4], uint8_t mask, const std::vector<VkRect2D>& rects);
    Target* ShrinkTarget(Target& src, uint32_t w, uint32_t h, float mul, bool keep_copy = false,
                         VkImage* reuse_img = nullptr, VkDeviceMemory* reuse_mem = nullptr,
                         VkImageView* reuse_view = nullptr, VkImageLayout* reuse_layout = nullptr);
    bool ReadBackImage(VkImage img, VkImageLayout& layout, uint32_t w, uint32_t h, std::vector<uint8_t>& out_bgra);
    void ReleaseOldTextures(bool aggressive = false);
    void ApplySettings();

    void UploadConstants(const State& s, const DrawShaders& sh, const PsoKey& k, const DrawTextures& stages,
                         uint32_t gw, uint32_t gh, uint32_t& vc_off, uint32_t& dc_off);

    vk::Context* ctx_ = nullptr;
    VkDevice dev_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout ubo_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout tex_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipe_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool ubo_pool_ = VK_NULL_HANDLE;
    VkDescriptorPool tex_desc_pool_ = VK_NULL_HANDLE;
    std::unordered_map<uint64_t, TexDescEntry> tex_desc_cache_;

    Frame frames_[kFrames]{};
    uint64_t frame_idx_ = 0;
    uint64_t next_serial_ = 1;
    std::atomic<uint64_t> completed_serial_{0};
    VkSemaphore timeline_sem_ = VK_NULL_HANDLE;

    VkSampler blit_linear_ = VK_NULL_HANDLE;
    VkSampler blit_point_ = VK_NULL_HANDLE;
    VkImage dummy_2d_ = VK_NULL_HANDLE, dummy_3d_ = VK_NULL_HANDLE, dummy_cube_ = VK_NULL_HANDLE, dummy_shadow_ = VK_NULL_HANDLE;
    VkDeviceMemory dummy_mem_[4]{};
    VkImageView dummy_2d_view_ = VK_NULL_HANDLE, dummy_3d_view_ = VK_NULL_HANDLE;
    VkImageView dummy_cube_view_ = VK_NULL_HANDLE, dummy_shadow_view_ = VK_NULL_HANDLE;

    bool r32f_linear_ = false;
    bool depth_linear_ = false;

    std::unordered_map<uint64_t, VkRenderPass> render_passes_;
    VkRenderPass compat_rps_[2][2]{};
    std::unordered_map<uint64_t, VkFramebuffer> framebuffers_;
    std::unordered_map<uint64_t, VkPipeline> psos_;
    std::unordered_map<uint64_t, VkPipeline> util_psos_;
    std::unordered_map<uint64_t, VkSampler> samplers_;
    std::unordered_map<uint64_t, VkShaderModule> vs_l1_;
    std::unordered_map<uint64_t, VkShaderModule> fs_l1_;

    bool in_rp_ = false;
    Target* rp_rt_ = nullptr;
    Target* rp_ds_ = nullptr;
    Target* cur_draw_rt_ = nullptr;
    Target* cur_draw_ds_ = nullptr;
    VkPipeline bound_pso_ = VK_NULL_HANDLE;
    VkDescriptorSet bound_tex_set_ = VK_NULL_HANDLE;
    uint32_t bound_vc_off_ = UINT32_MAX;
    uint32_t bound_dc_off_ = UINT32_MAX;
    bool dyn_valid_ = false;
    VkViewport bound_vp_{};
    VkRect2D bound_sc_{};
    float bound_bf_[4]{};
    uint32_t bound_sref_ = 0;
    float bound_slope_ = 0.0f;

    std::vector<uint8_t> tex_tmp_;
    std::vector<uint8_t> late_row_tmp_;

    std::unordered_map<uint64_t, Target> targets_;
    std::unordered_map<uint32_t, uint64_t> target_used_;
    std::unordered_set<uint32_t> persistent_depth_addrs_;
    std::unordered_map<uint64_t, Tex> textures_;
    TexSlabAllocator tex_slab_;
    size_t tex_vram_bytes_ = 0;

    std::unordered_map<uint32_t, Late> late_;
    std::mutex late_lock_;
    std::atomic<uint64_t> presents_seen_{0};
    std::atomic<uint64_t> present_serial_[kFrames + 1]{};

    VkQueryPool query_pool_ = VK_NULL_HANDLE;
    uint32_t query_next_ = 0;
    std::vector<Query> counted_;
    std::deque<Report> reports_;
    std::atomic<uint32_t> pending_reports_{0};
    uint32_t report_idle_polls_ = 0;

    uint8_t ramp_[768]{};
    bool ramp_dirty_ = false;
    bool gamma_enabled_ = false;

    uint32_t guest_w_ = 640, guest_h_ = 480;
    float scale_ = 1.5f;
    bool native_aux_rt_ = true;
    bool half_aux_rt_ = true;
    bool low_vram_tex_ = false;
    int last_thermal_status_ = 0;
    std::chrono::steady_clock::time_point last_present_tp_ = std::chrono::steady_clock::now();
    bool stall_logged_ = false;
    Assembled asm_{};
    decltype(State::vsc) last_vsc_{};
    uint32_t last_vc_off_ = UINT32_MAX;
    DrawConstants last_dc_{};
    uint32_t last_dc_off_ = UINT32_MAX;

    uint64_t presents_ = 0;
    uint64_t total_draws_ = 0;
    uint64_t total_clears_ = 0;
    uint64_t total_rp_opens_ = 0;
    uint64_t sec_neon_ = 0;
    uint64_t sec_texups_ = 0;
    uint64_t sec_cpu_ns_ = 0;
    uint64_t sec_wait_ns_ = 0;
    uint64_t sec_wait_begin_ns_ = 0;
    uint64_t sec_wait_flush_ns_ = 0;
    uint64_t sec_wait_sync_ns_ = 0;
    std::atomic<uint64_t> sec_wait_gpuf_ns_{0};
    uint64_t sec_draws_ = 0;
    uint64_t sec_rps_ = 0;
    uint64_t sec_frames_ = 0;
    uint64_t sec_submits_ = 0;
    uint64_t sec_stream_bytes_ = 0;
    uint64_t sec_syncs_ = 0;
    uint64_t sec_lates_ = 0;
    uint64_t sec_flushes_ = 0;
    uint64_t sec_rp_end_[size_t(RpEndReason::Count)]{};
    uint64_t last_cache_save_present_ = 0;
    size_t last_saved_pso_count_ = 0;
    size_t last_saved_spv_count_ = 0;

    std::chrono::steady_clock::time_point fps_t_ = std::chrono::steady_clock::now();
    uint64_t fps_frames_ = 0;
    float fps_val_ = 0.0f;
    std::chrono::steady_clock::time_point sec_t_ = std::chrono::steady_clock::now();
    uint32_t perf_log_secs_ = 0;
};

bool VulkanBackend::CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags req,
                                 VkMemoryPropertyFlags pref, VkBuffer& buf, VkDeviceMemory& mem, void** map) {
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = std::max<VkDeviceSize>(size, 64);
    bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(dev_, &bci, nullptr, &buf) != VK_SUCCESS) return false;

    VkMemoryRequirements mr{};
    vkGetBufferMemoryRequirements(dev_, buf, &mr);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = ctx_->FindMemoryType(mr.memoryTypeBits, req, pref);
    if (mai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(dev_, &mai, nullptr, &mem) != VK_SUCCESS) {
        vkDestroyBuffer(dev_, buf, nullptr);
        buf = VK_NULL_HANDLE;
        return false;
    }
    vkBindBufferMemory(dev_, buf, mem, 0);
    if (map && vkMapMemory(dev_, mem, 0, size, 0, map) != VK_SUCCESS) {
        return false;
    }
    return true;
}

bool VulkanBackend::CreateImage2D(uint32_t w, uint32_t h, uint32_t levels, uint32_t layers, VkImageType type,
                                  VkImageCreateFlags flags, VkFormat fmt, VkImageUsageFlags usage,
                                  VkMemoryPropertyFlags req, VkMemoryPropertyFlags pref,
                                  VkImage& img, VkDeviceMemory& mem) {
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.flags = flags;
    ici.imageType = type;
    ici.format = fmt;
    ici.extent = {std::max(1u, w), std::max(1u, h), (type == VK_IMAGE_TYPE_3D) ? std::max(1u, layers) : 1u};
    ici.mipLevels = std::max(1u, levels);
    ici.arrayLayers = (type == VK_IMAGE_TYPE_3D) ? 1u : std::max(1u, layers);
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(dev_, &ici, nullptr, &img) != VK_SUCCESS) {
        LOGE("vk_backend: vkCreateImage(%ux%u fmt=%d) failed", w, h, int(fmt));
        return false;
    }

    VkMemoryRequirements mr{};
    vkGetImageMemoryRequirements(dev_, img, &mr);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = ctx_->FindMemoryType(mr.memoryTypeBits, req, pref);
    VkResult arc = VK_SUCCESS;
    if (mai.memoryTypeIndex == UINT32_MAX || (arc = vkAllocateMemory(dev_, &mai, nullptr, &mem)) != VK_SUCCESS) {
        LOGE("vk_backend: vkAllocateMemory(size=%llu, type=%u) failed rc=%d",
             static_cast<unsigned long long>(mai.allocationSize), mai.memoryTypeIndex, int(arc));
        vkDestroyImage(dev_, img, nullptr);
        img = VK_NULL_HANDLE;
        return false;
    }
    vkBindImageMemory(dev_, img, mem, 0);
    return true;
}

void VulkanBackend::DeferDestroy(GarbageItem::Kind k, uint64_t handle) {
    if (handle) CurFrame().garbage.push_back({k, handle});
}

void VulkanBackend::InvalidateTexDescriptorsForView(VkImageView view) {
    if (view == VK_NULL_HANDLE) return;
    for (auto it = tex_desc_cache_.begin(); it != tex_desc_cache_.end();) {
        const TexDescEntry& e = it->second;
        if (e.views[0] == view || e.views[1] == view || e.views[2] == view || e.views[3] == view) {
            if (bound_tex_set_ == e.set) bound_tex_set_ = VK_NULL_HANDLE;
            DeferDestroy(GarbageItem::Kind::DescriptorSet, reinterpret_cast<uint64_t>(e.set));
            it = tex_desc_cache_.erase(it);
        } else {
            ++it;
        }
    }
}

void VulkanBackend::DeferView(VkImageView& v) {
    if (v != VK_NULL_HANDLE) {
        InvalidateTexDescriptorsForView(v);
        DeferDestroy(GarbageItem::Kind::ImageView, reinterpret_cast<uint64_t>(v));
        v = VK_NULL_HANDLE;
    }
}

void VulkanBackend::DeferSlab(TexSlabAlloc& s) {
    if (s.valid()) {
        GarbageItem item{};
        item.kind = GarbageItem::Kind::TexSlab;
        item.slab = s;
        CurFrame().garbage.push_back(item);
        s = TexSlabAlloc{};
    }
}

void VulkanBackend::InvalidateBoundState() {
    bound_pso_ = VK_NULL_HANDLE;
    bound_tex_set_ = VK_NULL_HANDLE;
    bound_vc_off_ = UINT32_MAX;
    bound_dc_off_ = UINT32_MAX;
    dyn_valid_ = false;
}

void VulkanBackend::CollectGarbage(Frame& f) {
    for (const GarbageItem& g : f.garbage) {
        switch (g.kind) {
            case GarbageItem::Kind::Framebuffer:
                vkDestroyFramebuffer(dev_, reinterpret_cast<VkFramebuffer>(g.handle), nullptr);
                break;
            case GarbageItem::Kind::ImageView:
                vkDestroyImageView(dev_, reinterpret_cast<VkImageView>(g.handle), nullptr);
                break;
            case GarbageItem::Kind::Image:
                vkDestroyImage(dev_, reinterpret_cast<VkImage>(g.handle), nullptr);
                break;
            case GarbageItem::Kind::Buffer:
                vkDestroyBuffer(dev_, reinterpret_cast<VkBuffer>(g.handle), nullptr);
                break;
            case GarbageItem::Kind::Memory:
                vkFreeMemory(dev_, reinterpret_cast<VkDeviceMemory>(g.handle), nullptr);
                break;
            case GarbageItem::Kind::TexSlab:
                tex_slab_.Free(g.slab);
                break;
            case GarbageItem::Kind::DescriptorSet:
                if (tex_desc_pool_) {
                    VkDescriptorSet ds = reinterpret_cast<VkDescriptorSet>(g.handle);
                    vkFreeDescriptorSets(dev_, tex_desc_pool_, 1, &ds);
                }
                break;
        }
    }
    f.garbage.clear();
}

void VulkanBackend::AdvanceCompletedSerial(uint64_t ser) {
    uint64_t prev = completed_serial_.load(std::memory_order_relaxed);
    while (ser > prev &&
           !completed_serial_.compare_exchange_weak(prev, ser, std::memory_order_release, std::memory_order_relaxed)) {
    }
}

bool VulkanBackend::WaitFrameIfNeeded(Frame& f, uint64_t max_serial, uint64_t* wait_ns) {
    if (timeline_sem_ != VK_NULL_HANDLE) {
        std::lock_guard<std::mutex> flk(f.fence_mutex);
        if (!f.submitted || f.fence_serial > max_serial) return false;
        uint64_t done = completed_serial_.load(std::memory_order_acquire);
        if (done < f.fence_serial) {
            PollCompletedSerial();
            done = completed_serial_.load(std::memory_order_acquire);
        }
        if (done < f.fence_serial) {
            auto t0 = wait_ns ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            ctx_->WaitTimelineSemaphore(timeline_sem_, f.fence_serial, UINT64_MAX);
            if (wait_ns) {
                *wait_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now() - t0)
                                         .count());
            }
            AdvanceCompletedSerial(f.fence_serial);
        }
        f.submitted = false;
        return true;
    }

    std::lock_guard<std::mutex> flk(f.fence_mutex);
    if (!f.submitted || f.fence_serial > max_serial) return false;
    auto t0 = wait_ns ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    vkWaitForFences(dev_, 1, &f.fence, VK_TRUE, UINT64_MAX);
    if (wait_ns) {
        *wait_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now() - t0)
                                 .count());
    }
    AdvanceCompletedSerial(f.fence_serial);
    f.submitted = false;
    return true;
}

void VulkanBackend::PollCompletedSerial() {
    if (timeline_sem_ != VK_NULL_HANDLE) {
        uint64_t done = 0;
        if (ctx_->GetSemaphoreCounter(timeline_sem_, done)) {
            AdvanceCompletedSerial(done);
        }
        return;
    }
    for (Frame& f : frames_) {
        std::lock_guard<std::mutex> flk(f.fence_mutex);
        if (f.submitted && vkGetFenceStatus(dev_, f.fence) == VK_SUCCESS) {
            AdvanceCompletedSerial(f.fence_serial);
            f.submitted = false;
        }
    }
}

void VulkanBackend::BeginCommandBuffer() {
    Frame& f = CurFrame();
    const uint64_t wb = sec_wait_ns_;
    WaitFrameIfNeeded(f, UINT64_MAX, &sec_wait_ns_);
    sec_wait_begin_ns_ += (sec_wait_ns_ - wb);
    CollectGarbage(f);
    vkResetCommandPool(dev_, f.pool, 0);
    f.upload_recording = false;
    f.vb_cache.clear();
    f.const_used = 0;
    f.stream_used = 0;

    // Deliver any finished visibility queries BEFORE resetting this frame's query slice.
    DeliverCompletedReports();

    VkCommandBufferBeginInfo cbbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(f.cmd, &cbbi);

    if (query_pool_) {
        const uint32_t per_frame = kQueries / kFrames;
        const uint32_t q_base = uint32_t(frame_idx_ % kFrames) * per_frame;
        vkCmdResetQueryPool(f.cmd, query_pool_, q_base, per_frame);
        query_next_ = q_base;
    }

    InvalidateBoundState();
    last_vc_off_ = UINT32_MAX;
    last_dc_off_ = UINT32_MAX;
}

VkCommandBuffer VulkanBackend::BeginUploadCmd() {
    Frame& f = CurFrame();
    if (!f.upload_recording) {
        VkCommandBufferBeginInfo cbbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(f.upload_cmd, &cbbi);
        f.upload_recording = true;
    }
    return f.upload_cmd;
}

void VulkanBackend::SubmitCurrent(bool wait, uint64_t* wait_bucket) {
    EndRenderPass(RpEndReason::Sub);
    Frame& f = CurFrame();
    sec_submits_++;
    sec_stream_bytes_ += f.stream_used;
    vkEndCommandBuffer(f.cmd);
    VkCommandBuffer cmds[2]{};
    uint32_t cmd_count = 0;
    if (f.upload_recording) {
        vkEndCommandBuffer(f.upload_cmd);
        f.upload_recording = false;
        cmds[cmd_count++] = f.upload_cmd;
    }
    cmds[cmd_count++] = f.cmd;
    {
        std::lock_guard<std::mutex> flk(f.fence_mutex);
        f.fence_serial = next_serial_++;
        f.submitted = true;

        if (timeline_sem_ != VK_NULL_HANDLE) {
            VkTimelineSemaphoreSubmitInfo tssi{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
            tssi.signalSemaphoreValueCount = 1;
            tssi.pSignalSemaphoreValues = &f.fence_serial;

            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            si.pNext = &tssi;
            si.commandBufferCount = cmd_count;
            si.pCommandBuffers = cmds;
            si.signalSemaphoreCount = 1;
            si.pSignalSemaphores = &timeline_sem_;
            {
                std::lock_guard<std::mutex> q(ctx_->QueueMutex());
                VkResult rc = vkQueueSubmit(ctx_->Queue(), 1, &si, VK_NULL_HANDLE);
                if (rc != VK_SUCCESS) {
                    LOGE("vk_backend: vkQueueSubmit(timeline) failed rc=%d (cmds=%u)", int(rc), cmd_count);
                }
            }
            if (wait) {
                auto wt0 = std::chrono::steady_clock::now();
                ctx_->WaitTimelineSemaphore(timeline_sem_, f.fence_serial, UINT64_MAX);
                uint64_t wns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                            std::chrono::steady_clock::now() - wt0)
                                            .count());
                sec_wait_ns_ += wns;
                if (wait_bucket) *wait_bucket += wns;
                AdvanceCompletedSerial(f.fence_serial);
                f.submitted = false;
            }
        } else {
            vkResetFences(dev_, 1, &f.fence);

            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            si.commandBufferCount = cmd_count;
            si.pCommandBuffers = cmds;
            {
                std::lock_guard<std::mutex> q(ctx_->QueueMutex());
                VkResult rc = vkQueueSubmit(ctx_->Queue(), 1, &si, f.fence);
                if (rc != VK_SUCCESS) {
                    LOGE("vk_backend: vkQueueSubmit failed rc=%d (cmds=%u)", int(rc), cmd_count);
                }
            }
            if (wait) {
                auto wt0 = std::chrono::steady_clock::now();
                vkWaitForFences(dev_, 1, &f.fence, VK_TRUE, UINT64_MAX);
                uint64_t wns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                            std::chrono::steady_clock::now() - wt0)
                                            .count());
                sec_wait_ns_ += wns;
                if (wait_bucket) *wait_bucket += wns;
                AdvanceCompletedSerial(f.fence_serial);
                f.submitted = false;
            }
        }
    }
    if (wait) CollectGarbage(f);
    frame_idx_++;
    BeginCommandBuffer();
}

void VulkanBackend::WaitSerial(uint64_t serial) {
    if (serial == 0 || completed_serial_.load(std::memory_order_acquire) >= serial) return;
    if (timeline_sem_ != VK_NULL_HANDLE) {
        if (serial < next_serial_) {
            PollCompletedSerial();
            if (completed_serial_.load(std::memory_order_acquire) < serial) {
                ctx_->WaitTimelineSemaphore(timeline_sem_, serial, UINT64_MAX);
                AdvanceCompletedSerial(serial);
            }
            return;
        }
        Flush();
        return;
    }
    for (Frame& f : frames_) {
        WaitFrameIfNeeded(f, serial);
    }
    if (completed_serial_.load(std::memory_order_acquire) < serial) Flush();
}

VulkanBackend::RingAlloc VulkanBackend::AllocConst(uint64_t size, uint64_t align) {
    Frame& f = CurFrame();
    uint64_t a = std::max<uint64_t>(align, ctx_->MinUboAlignment());
    uint64_t off = (f.const_used + a - 1) & ~(a - 1);
    if (off + size <= kConstRingSize) {
        f.const_used = off + size;
        return {f.const_buf, off, f.const_map + off};
    }
    LOGE("vk_backend: const ring overflow (used=%llu, size=%llu)",
         static_cast<unsigned long long>(f.const_used), static_cast<unsigned long long>(size));
    return {f.const_buf, 0, f.const_map};
}

VulkanBackend::RingAlloc VulkanBackend::AllocStream(uint64_t size, uint64_t align) {
    Frame& f = CurFrame();
    uint64_t a = std::max<uint64_t>(align, 16);
    uint64_t off = (f.stream_used + a - 1) & ~(a - 1);
    if (off + size <= kStreamRingSize) {
        f.stream_used = off + size;
        return {f.stream_buf, off, f.stream_map + off};
    }
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    void* map = nullptr;
    if (!CreateBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                            VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0,
                      buf, mem, &map)) {
        return {};
    }
    // Note: Wait for idle when overflowing to avoid unmap tracking complexity; keep mapped until CollectGarbage.
    DeferDestroy(GarbageItem::Kind::Buffer, reinterpret_cast<uint64_t>(buf));
    DeferDestroy(GarbageItem::Kind::Memory, reinterpret_cast<uint64_t>(mem));
    return {buf, 0, static_cast<uint8_t*>(map)};
}

bool VulkanBackend::CreateDummies() {
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(dev_, &si, nullptr, &blit_linear_) != VK_SUCCESS) return false;
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    if (vkCreateSampler(dev_, &si, nullptr, &blit_point_) != VK_SUCCESS) return false;

    if (!CreateImage2D(1, 1, 1, 1, VK_IMAGE_TYPE_2D, 0, VK_FORMAT_R8G8B8A8_UNORM,
                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, dummy_2d_, dummy_mem_[0])) return false;
    if (!CreateImage2D(1, 1, 1, 1, VK_IMAGE_TYPE_3D, 0, VK_FORMAT_R8G8B8A8_UNORM,
                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, dummy_3d_, dummy_mem_[1])) return false;
    if (!CreateImage2D(1, 1, 1, 6, VK_IMAGE_TYPE_2D, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT, VK_FORMAT_R8G8B8A8_UNORM,
                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, dummy_cube_, dummy_mem_[2])) return false;
    if (!CreateImage2D(1, 1, 1, 1, VK_IMAGE_TYPE_2D, 0, ctx_->DepthFormat(),
                       VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, dummy_shadow_, dummy_mem_[3])) return false;

    auto mk_view = [&](VkImage img, VkImageViewType vt, VkFormat fmt, VkImageAspectFlags asp, uint32_t layers, VkImageView& out) {
        VkImageViewCreateInfo iv{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        iv.image = img;
        iv.viewType = vt;
        iv.format = fmt;
        iv.subresourceRange = {asp, 0, 1, 0, layers};
        return vkCreateImageView(dev_, &iv, nullptr, &out) == VK_SUCCESS;
    };
    if (!mk_view(dummy_2d_, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, 1, dummy_2d_view_)) return false;
    if (!mk_view(dummy_3d_, VK_IMAGE_VIEW_TYPE_3D, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, 1, dummy_3d_view_)) return false;
    if (!mk_view(dummy_cube_, VK_IMAGE_VIEW_TYPE_CUBE, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, 6, dummy_cube_view_)) return false;
    if (!mk_view(dummy_shadow_, VK_IMAGE_VIEW_TYPE_2D, ctx_->DepthFormat(), VK_IMAGE_ASPECT_DEPTH_BIT, 1, dummy_shadow_view_)) return false;

    // Upload 1x1 white pixels and clear dummy shadow
    BeginCommandBuffer();
    VkCommandBuffer cmd = CurFrame().cmd;
    RingAlloc white = AllocStream(16, 16);
    std::memset(white.cpu, 255, 16);
    auto clear_and_read = [&](VkImage img, VkImageAspectFlags asp, uint32_t layers) {
        VkImageLayout l = VK_IMAGE_LAYOUT_UNDEFINED;
        TransitionImageRaw(cmd, img, asp, 1, layers, l, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        if (asp == VK_IMAGE_ASPECT_COLOR_BIT) {
            for (uint32_t layer = 0; layer < layers; ++layer) {
                VkBufferImageCopy r{};
                r.imageSubresource = {asp, 0, layer, 1};
                r.imageExtent = {1, 1, 1};
                vkCmdCopyBufferToImage(cmd, white.buf, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
            }
        }
        TransitionImageRaw(cmd, img, asp, 1, layers, l, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    };
    clear_and_read(dummy_2d_, VK_IMAGE_ASPECT_COLOR_BIT, 1);
    clear_and_read(dummy_3d_, VK_IMAGE_ASPECT_COLOR_BIT, 1);
    clear_and_read(dummy_cube_, VK_IMAGE_ASPECT_COLOR_BIT, 6);
    {
        VkImageLayout l = VK_IMAGE_LAYOUT_UNDEFINED;
        TransitionImageRaw(cmd, dummy_shadow_, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 1, 1,
                           l, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    SubmitCurrent(true);
    return true;
}

bool VulkanBackend::Init(uint32_t width, uint32_t height) {
    ctx_ = vk::ActiveContext();
    if (!ctx_ || ctx_->Device() == VK_NULL_HANDLE) {
        LOGE("vk_backend: no active vk::Context");
        return false;
    }
    dev_ = ctx_->Device();
    g_reversed_depth = true;
    guest_w_ = width;
    guest_h_ = height;
    scale_ = app::RenderScale(height);
    native_aux_rt_ = app::AndroidSettings().native_aux_rt;
    half_aux_rt_ = app::AndroidSettings().half_aux_rt;
    low_vram_tex_ = app::AndroidSettings().low_vram_tex;
    adpf::RegisterRenderThread(gettid());

    // Probe linear filtering capability for R32F and chosen depth format
    {
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(ctx_->PhysicalDevice(), VK_FORMAT_R32_SFLOAT, &fp);
        r32f_linear_ = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
        vkGetPhysicalDeviceFormatProperties(ctx_->PhysicalDevice(), ctx_->DepthFormat(), &fp);
        depth_linear_ = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
    }

    VkDescriptorSetLayoutBinding ubo_binds[2]{};
    ubo_binds[0].binding = 0;
    ubo_binds[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    ubo_binds[0].descriptorCount = 1;
    ubo_binds[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

    ubo_binds[1].binding = 1;
    ubo_binds[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    ubo_binds[1].descriptorCount = 1;
    ubo_binds[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo ubo_dsl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ubo_dsl.bindingCount = 2;
    ubo_dsl.pBindings = ubo_binds;
    if (vkCreateDescriptorSetLayout(dev_, &ubo_dsl, nullptr, &ubo_set_layout_) != VK_SUCCESS) return false;

    VkDescriptorSetLayoutBinding tex_binds[4]{};
    for (uint32_t i = 0; i < 4; ++i) {
        tex_binds[i].binding = i;
        tex_binds[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        tex_binds[i].descriptorCount = 1;
        tex_binds[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }

    VkDescriptorSetLayoutCreateInfo tex_dsl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    tex_dsl.bindingCount = 4;
    tex_dsl.pBindings = tex_binds;
    if (vkCreateDescriptorSetLayout(dev_, &tex_dsl, nullptr, &tex_set_layout_) != VK_SUCCESS) return false;

    VkDescriptorSetLayout set_layouts[2] = {ubo_set_layout_, tex_set_layout_};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 2;
    plci.pSetLayouts = set_layouts;
    if (vkCreatePipelineLayout(dev_, &plci, nullptr, &pipe_layout_) != VK_SUCCESS) return false;

    VkDescriptorPoolSize ubo_ps{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, kFrames * 2};
    VkDescriptorPoolCreateInfo ubo_pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    ubo_pci.maxSets = kFrames;
    ubo_pci.poolSizeCount = 1;
    ubo_pci.pPoolSizes = &ubo_ps;
    if (vkCreateDescriptorPool(dev_, &ubo_pci, nullptr, &ubo_pool_) != VK_SUCCESS) return false;

    VkDescriptorPoolSize tex_ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kTexDescPoolSize * 4};
    VkDescriptorPoolCreateInfo tex_pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    tex_pci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    tex_pci.maxSets = kTexDescPoolSize;
    tex_pci.poolSizeCount = 1;
    tex_pci.pPoolSizes = &tex_ps;
    if (vkCreateDescriptorPool(dev_, &tex_pci, nullptr, &tex_desc_pool_) != VK_SUCCESS) return false;

    for (int i = 0; i < kFrames; ++i) {
        Frame& f = frames_[i];
        VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cpci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        cpci.queueFamilyIndex = ctx_->QueueFamily();
        if (vkCreateCommandPool(dev_, &cpci, nullptr, &f.pool) != VK_SUCCESS) return false;

        VkCommandBuffer cmds[2]{};
        VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cbai.commandPool = f.pool;
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 2;
        if (vkAllocateCommandBuffers(dev_, &cbai, cmds) != VK_SUCCESS) return false;
        f.cmd = cmds[0];
        f.upload_cmd = cmds[1];

        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (vkCreateFence(dev_, &fci, nullptr, &f.fence) != VK_SUCCESS) return false;

        if (!CreateBuffer(kConstRingSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0,
                          f.const_buf, f.const_mem, reinterpret_cast<void**>(&f.const_map))) {
            return false;
        }
        if (!CreateBuffer(kStreamRingSize,
                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0,
                          f.stream_buf, f.stream_mem, reinterpret_cast<void**>(&f.stream_map))) {
            return false;
        }

        VkDescriptorSetAllocateInfo ubo_dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ubo_dsai.descriptorPool = ubo_pool_;
        ubo_dsai.descriptorSetCount = 1;
        ubo_dsai.pSetLayouts = &ubo_set_layout_;
        if (vkAllocateDescriptorSets(dev_, &ubo_dsai, &f.ubo_set) != VK_SUCCESS) return false;

        VkDescriptorBufferInfo b0{f.const_buf, 0, sizeof(State::vsc)};
        VkDescriptorBufferInfo b1{f.const_buf, 0, sizeof(DrawConstants)};
        VkWriteDescriptorSet uw[2]{
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, f.ubo_set, 0, 0, 1,
             VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, nullptr, &b0, nullptr},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, f.ubo_set, 1, 0, 1,
             VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, nullptr, &b1, nullptr},
        };
        vkUpdateDescriptorSets(dev_, 2, uw, 0, nullptr);
    }

    compat_rps_[1][0] = GetRenderPass({1, 0, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, 0, 0, 0, 0});
    compat_rps_[0][1] = GetRenderPass({0, 1, 0, 0, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE,
                                       VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE});
    compat_rps_[1][1] = GetRenderPass({1, 1, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE,
                                       VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE,
                                       VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE});

    VkQueryPoolCreateInfo qpci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qpci.queryType = VK_QUERY_TYPE_OCCLUSION;
    qpci.queryCount = kQueries;
    vkCreateQueryPool(dev_, &qpci, nullptr, &query_pool_);

    if (!CreateDummies()) return false;

    for (int i = 0; i < 256; ++i) {
        ramp_[i] = ramp_[256 + i] = ramp_[512 + i] = uint8_t(i);
    }

    if (ctx_->HasTimelineSemaphore()) {
        VkSemaphoreTypeCreateInfo stci{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        stci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        stci.initialValue = 0;
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &stci};
        if (vkCreateSemaphore(dev_, &sci, nullptr, &timeline_sem_) != VK_SUCCESS) {
            timeline_sem_ = VK_NULL_HANDLE;
        }
    }

    LOGR("vk_backend: initialized guest=%ux%u scale=%.2f bc=%u lazy_depth=%u timeline_sem=%u r32f_lin=%u depth_lin=%u",
         width, height, scale_, unsigned(ctx_->HasBC()), unsigned(ctx_->HasLazyMemory()),
         unsigned(timeline_sem_ != VK_NULL_HANDLE), unsigned(r32f_linear_), unsigned(depth_linear_));
    return true;
}

VulkanBackend::~VulkanBackend() {
    if (!dev_) return;
    if (ctx_) ctx_->InvalidatePublished();
    {
        if (ctx_) ctx_->WaitIdle();
        else vkDeviceWaitIdle(dev_);
    }

    for (auto& [k, t] : targets_) ReleaseTarget(t);
    targets_.clear();
    for (auto& [k, tx] : textures_) {
        if (tx.view) vkDestroyImageView(dev_, tx.view, nullptr);
        if (tx.image) vkDestroyImage(dev_, tx.image, nullptr);
        if (tx.mem) vkFreeMemory(dev_, tx.mem, nullptr);
    }
    textures_.clear();

    for (auto& [addr, l] : late_) {
        for (int i = 0; i < kFrames; ++i) {
            if (l.rb_map[i]) vkUnmapMemory(dev_, l.rb_mem[i]);
            if (l.rb[i]) vkDestroyBuffer(dev_, l.rb[i], nullptr);
            if (l.rb_mem[i]) vkFreeMemory(dev_, l.rb_mem[i], nullptr);
        }
        if (l.down_view) vkDestroyImageView(dev_, l.down_view, nullptr);
        if (l.down) vkDestroyImage(dev_, l.down, nullptr);
        if (l.down_mem) vkFreeMemory(dev_, l.down_mem, nullptr);
    }
    late_.clear();

    for (auto& [k, p] : psos_) if (p) vkDestroyPipeline(dev_, p, nullptr);
    for (auto& [k, p] : util_psos_) if (p) vkDestroyPipeline(dev_, p, nullptr);
    for (auto& [k, s] : samplers_) if (s) vkDestroySampler(dev_, s, nullptr);
    for (auto& [k, fb] : framebuffers_) if (fb) vkDestroyFramebuffer(dev_, fb, nullptr);
    for (auto& [k, rp] : render_passes_) if (rp) vkDestroyRenderPass(dev_, rp, nullptr);
    if (query_pool_) vkDestroyQueryPool(dev_, query_pool_, nullptr);

    if (blit_linear_) vkDestroySampler(dev_, blit_linear_, nullptr);
    if (blit_point_) vkDestroySampler(dev_, blit_point_, nullptr);
    if (dummy_2d_view_) vkDestroyImageView(dev_, dummy_2d_view_, nullptr);
    if (dummy_3d_view_) vkDestroyImageView(dev_, dummy_3d_view_, nullptr);
    if (dummy_cube_view_) vkDestroyImageView(dev_, dummy_cube_view_, nullptr);
    if (dummy_shadow_view_) vkDestroyImageView(dev_, dummy_shadow_view_, nullptr);
    if (dummy_2d_) vkDestroyImage(dev_, dummy_2d_, nullptr);
    if (dummy_3d_) vkDestroyImage(dev_, dummy_3d_, nullptr);
    if (dummy_cube_) vkDestroyImage(dev_, dummy_cube_, nullptr);
    if (dummy_shadow_) vkDestroyImage(dev_, dummy_shadow_, nullptr);
    for (VkDeviceMemory m : dummy_mem_) if (m) vkFreeMemory(dev_, m, nullptr);

    tex_desc_cache_.clear();
    vs_l1_.clear();
    fs_l1_.clear();

    for (Frame& f : frames_) {
        CollectGarbage(f);
        if (f.const_map) vkUnmapMemory(dev_, f.const_mem);
        if (f.const_buf) vkDestroyBuffer(dev_, f.const_buf, nullptr);
        if (f.const_mem) vkFreeMemory(dev_, f.const_mem, nullptr);
        if (f.stream_map) vkUnmapMemory(dev_, f.stream_mem);
        if (f.stream_buf) vkDestroyBuffer(dev_, f.stream_buf, nullptr);
        if (f.stream_mem) vkFreeMemory(dev_, f.stream_mem, nullptr);
        if (f.fence) vkDestroyFence(dev_, f.fence, nullptr);
        if (f.pool) vkDestroyCommandPool(dev_, f.pool, nullptr);
    }
    tex_slab_.Shutdown(dev_);
    if (timeline_sem_) {
        vkDestroySemaphore(dev_, timeline_sem_, nullptr);
        timeline_sem_ = VK_NULL_HANDLE;
    }
    if (tex_desc_pool_) vkDestroyDescriptorPool(dev_, tex_desc_pool_, nullptr);
    if (ubo_pool_) vkDestroyDescriptorPool(dev_, ubo_pool_, nullptr);
    if (pipe_layout_) vkDestroyPipelineLayout(dev_, pipe_layout_, nullptr);
    if (ubo_set_layout_) vkDestroyDescriptorSetLayout(dev_, ubo_set_layout_, nullptr);
    if (tex_set_layout_) vkDestroyDescriptorSetLayout(dev_, tex_set_layout_, nullptr);
}

VkRenderPass VulkanBackend::GetRenderPass(const RpKey& k) {
    uint64_t hk = Hash(&k, sizeof(k));
    if (auto it = render_passes_.find(hk); it != render_passes_.end()) return it->second;

    VkAttachmentDescription atts[2]{};
    uint32_t att_count = 0;
    VkAttachmentReference color_ref{VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depth_ref{VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

    if (k.has_rt) {
        VkAttachmentDescription& a = atts[att_count];
        a.format = VK_FORMAT_R8G8B8A8_UNORM;
        a.samples = VK_SAMPLE_COUNT_1_BIT;
        a.loadOp = static_cast<VkAttachmentLoadOp>(k.rt_load);
        a.storeOp = static_cast<VkAttachmentStoreOp>(k.rt_store);
        a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        a.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color_ref.attachment = att_count++;
    }
    if (k.has_ds) {
        VkAttachmentDescription& a = atts[att_count];
        a.format = ctx_->DepthFormat();
        a.samples = VK_SAMPLE_COUNT_1_BIT;
        a.loadOp = static_cast<VkAttachmentLoadOp>(k.ds_load);
        a.storeOp = static_cast<VkAttachmentStoreOp>(k.ds_store);
        a.stencilLoadOp = static_cast<VkAttachmentLoadOp>(k.s_load);
        a.stencilStoreOp = static_cast<VkAttachmentStoreOp>(k.s_store);
        a.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        a.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth_ref.attachment = att_count++;
    }

    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = k.has_rt ? 1 : 0;
    sub.pColorAttachments = k.has_rt ? &color_ref : nullptr;
    sub.pDepthStencilAttachment = k.has_ds ? &depth_ref : nullptr;

    VkSubpassDependency deps[2]{};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT |
                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                           VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                            VK_ACCESS_TRANSFER_WRITE_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    deps[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpci.attachmentCount = att_count;
    rpci.pAttachments = atts;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &sub;
    rpci.dependencyCount = 2;
    rpci.pDependencies = deps;

    VkRenderPass rp = VK_NULL_HANDLE;
    if (vkCreateRenderPass(dev_, &rpci, nullptr, &rp) != VK_SUCCESS) {
        LOGE("vk_backend: vkCreateRenderPass failed");
        return VK_NULL_HANDLE;
    }
    render_passes_[hk] = rp;
    return rp;
}

VkFramebuffer VulkanBackend::GetFramebuffer(Target* rt, Target* ds, VkRenderPass rp, uint32_t w, uint32_t h) {
    uint64_t key_data[5] = {
        rt ? reinterpret_cast<uint64_t>(rt->attach_view) : 0,
        ds ? reinterpret_cast<uint64_t>(ds->attach_view) : 0,
        uint64_t(w) | (uint64_t(h) << 32),
        uint64_t(rt ? 1 : 0) | (uint64_t(ds ? 1 : 0) << 1),
        0
    };
    uint64_t hk = Hash(key_data, sizeof(key_data));
    if (auto it = framebuffers_.find(hk); it != framebuffers_.end()) return it->second;

    VkImageView views[2]{};
    uint32_t n = 0;
    if (rt) views[n++] = rt->attach_view;
    if (ds) views[n++] = ds->attach_view;

    VkFramebufferCreateInfo fbci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fbci.renderPass = rp;
    fbci.attachmentCount = n;
    fbci.pAttachments = views;
    fbci.width = w;
    fbci.height = h;
    fbci.layers = 1;
    VkFramebuffer fb = VK_NULL_HANDLE;
    if (vkCreateFramebuffer(dev_, &fbci, nullptr, &fb) != VK_SUCCESS) {
        LOGE("vk_backend: vkCreateFramebuffer failed %ux%u", w, h);
        return VK_NULL_HANDLE;
    }
    framebuffers_[hk] = fb;
    return fb;
}

void VulkanBackend::EndRenderPass(RpEndReason reason) {
    if (!in_rp_) return;
    sec_rp_end_[size_t(reason)]++;
    vkCmdEndRenderPass(CurFrame().cmd);
    if (rp_ds_) rp_ds_->ended_mid_frame = true;
    in_rp_ = false;
    rp_rt_ = nullptr;
    rp_ds_ = nullptr;
    InvalidateBoundState();
}

bool VulkanBackend::EnsureRenderPass(Target* rt, Target* ds) {
    if (!rt && !ds) return false;
    if (in_rp_ && rp_rt_ == rt && rp_ds_ == ds) return true;
    EndRenderPass(RpEndReason::Sw);

    // If reusing a lazy transient depth target without clearing after an earlier pass closed,
    // remember to promote this depth address to persistent for future draws.
    if (ds && ds->transient_lazy && ds->ended_mid_frame && !ds->pending_clear_z && !ds->pending_clear_s && ds->content_valid) {
        PromoteDepthToPersistent(*ds);
    }

    VkCommandBuffer cmd = CurFrame().cmd;
    if (rt) {
        TransitionImageRaw(cmd, rt->image, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1,
                           rt->layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    }
    if (ds) {
        TransitionImageRaw(cmd, ds->image, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 1, 1,
                           ds->layout, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    }

    RpKey rk{};
    if (rt) {
        rk.has_rt = 1;
        bool need_clear = rt->pending_clear_color || !rt->content_valid;
        rk.rt_load = need_clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        rk.rt_store = VK_ATTACHMENT_STORE_OP_STORE;
    }
    if (ds) {
        rk.has_ds = 1;
        bool lazy = ds->transient_lazy;
        if (lazy) {
            // Lazy memory cannot meaningfully LOAD prior discarded data across RPs; clear cleanly if not pending clear.
            rk.ds_load = (ds->pending_clear_z || !ds->content_valid || ds->ended_mid_frame)
                         ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
            rk.ds_store = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            rk.s_load = (ds->pending_clear_s || !ds->content_valid || ds->ended_mid_frame)
                        ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
            rk.s_store = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        } else {
            rk.ds_load = (ds->pending_clear_z || !ds->content_valid) ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
            rk.ds_store = VK_ATTACHMENT_STORE_OP_STORE;
            rk.s_load = (ds->pending_clear_s || !ds->content_valid) ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
            rk.s_store = VK_ATTACHMENT_STORE_OP_STORE;
        }
    }

    VkRenderPass rp = GetRenderPass(rk);
    if (!rp) return false;
    uint32_t w = rt ? rt->w : ds->w;
    uint32_t h = rt ? rt->h : ds->h;
    VkFramebuffer fb = GetFramebuffer(rt, ds, rp, w, h);
    if (!fb) return false;

    VkClearValue cv[2]{};
    uint32_t n = 0;
    if (rt) {
        std::memcpy(cv[n].color.float32, rt->pending_clear_color ? rt->clear_rgba : std::array<float, 4>{0, 0, 0, 0}.data(), 16);
        rt->pending_clear_color = false;
        rt->content_valid = true;
        n++;
    }
    if (ds) {
        cv[n].depthStencil.depth = ds->pending_clear_z ? ds->clear_z : 0.0f;
        cv[n].depthStencil.stencil = ds->pending_clear_s ? ds->clear_s : 0u;
        ds->pending_clear_z = false;
        ds->pending_clear_s = false;
        ds->ended_mid_frame = false;
        ds->content_valid = true;
        n++;
    }

    VkRenderPassBeginInfo rpbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rpbi.renderPass = rp;
    rpbi.framebuffer = fb;
    rpbi.renderArea = {{0, 0}, {w, h}};
    rpbi.clearValueCount = n;
    rpbi.pClearValues = cv;
    vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

    in_rp_ = true;
    rp_rt_ = rt;
    rp_ds_ = ds;
    InvalidateBoundState();
    total_rp_opens_++;
    sec_rps_++;
    return true;
}

uint64_t VulkanBackend::SizedTargetKey(uint32_t data, bool depth, uint32_t gw, uint32_t gh) const {
    uint64_t key = (uint64_t(data) << 1) | (depth ? 1ull : 0ull);
    if (depth && (gw != guest_w_ || gh != guest_h_)) {
        key |= (uint64_t(gw) << 34) | (uint64_t(gh) << 48);
    }
    return key;
}

void VulkanBackend::UpdateTargetUsed(uint32_t data) {
    target_used_[data] = presents_;
}

bool VulkanBackend::TargetStale(uint32_t data) const {
    auto it = target_used_.find(data);
    uint64_t used = (it == target_used_.end()) ? 0 : it->second;
    return (presents_ - used) > kStaleFrames;
}

void VulkanBackend::ReleaseTarget(Target& t) {
    if (rp_rt_ == &t || rp_ds_ == &t) EndRenderPass();
    // Invalidate cached framebuffers referencing this target's view lazily? Better: defer destroying view/fb
    // Wait: remove matching framebuffers from framebuffers_ cleanly on target release:
    for (auto it = framebuffers_.begin(); it != framebuffers_.end();) {
        // Simpler: destroy all framebuffers when any target releases (only happens on scale change or Shutdown),
        // deferring them cleanly on CurFrame().garbage!
        DeferDestroy(GarbageItem::Kind::Framebuffer, reinterpret_cast<uint64_t>(it->second));
        it = framebuffers_.erase(it);
    }
    if (t.sample_view == t.attach_view) t.sample_view = VK_NULL_HANDLE;
    DeferView(t.attach_view);
    DeferView(t.sample_view);
    Defer(GarbageItem::Kind::Image, t.image);
    Defer(GarbageItem::Kind::Memory, t.mem);
    DeferView(t.fb_view);
    Defer(GarbageItem::Kind::Image, t.fb_image);
    Defer(GarbageItem::Kind::Memory, t.fb_mem);
}

bool VulkanBackend::CreateTargetResources(Target& t) {
    const bool is_aux = native_aux_rt_ && (t.gw < guest_w_ || t.gh < guest_h_);
    const float aux_cap = half_aux_rt_ ? 0.5f : 1.0f;
    const float eff_scale = is_aux ? std::min(aux_cap, scale_) : scale_;
    t.w = std::max(is_aux ? 32u : 1u, uint32_t(std::lround(t.gw * eff_scale)));
    t.h = std::max(is_aux ? 32u : 1u, uint32_t(std::lround(t.gh * eff_scale)));
    if (t.depth) {
        t.fmt = ctx_->DepthFormat();
        bool force_pers = t.persistent_forced || persistent_depth_addrs_.count(t.data) > 0;
        if (!force_pers && ctx_->HasLazyMemory()) {
            t.transient_lazy = true;
            if (!CreateImage2D(t.w, t.h, 1, 1, VK_IMAGE_TYPE_2D, 0, t.fmt,
                               VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                               VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                               t.image, t.mem)) {
                t.transient_lazy = false;
            }
        } else {
            t.transient_lazy = false;
        }
        if (!t.image) {
            if (!CreateImage2D(t.w, t.h, 1, 1, VK_IMAGE_TYPE_2D, 0, t.fmt,
                               VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                               VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, t.image, t.mem)) {
                return false;
            }
        }
        VkImageViewCreateInfo iv{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        iv.image = t.image;
        iv.viewType = VK_IMAGE_VIEW_TYPE_2D;
        iv.format = t.fmt;
        iv.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(dev_, &iv, nullptr, &t.attach_view) != VK_SUCCESS) return false;
        if (!t.transient_lazy) {
            iv.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            if (vkCreateImageView(dev_, &iv, nullptr, &t.sample_view) != VK_SUCCESS) return false;
        }
    } else {
        t.fmt = VK_FORMAT_R8G8B8A8_UNORM;
        t.transient_lazy = false;
        if (!CreateImage2D(t.w, t.h, 1, 1, VK_IMAGE_TYPE_2D, 0, t.fmt,
                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, t.image, t.mem)) {
            return false;
        }
        VkImageViewCreateInfo iv{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        iv.image = t.image;
        iv.viewType = VK_IMAGE_VIEW_TYPE_2D;
        iv.format = t.fmt;
        iv.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(dev_, &iv, nullptr, &t.attach_view) != VK_SUCCESS) return false;
        t.sample_view = t.attach_view;
    }
    t.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    t.content_valid = false;
    return true;
}

void VulkanBackend::PromoteDepthToPersistent(Target& ds) {
    if (!ds.depth || !ds.transient_lazy) return;
    persistent_depth_addrs_.insert(ds.data);
    ds.persistent_forced = true;
    LOGR("vk_backend: promoting lazy depth %08X (%ux%u) to persistent DEVICE_LOCAL", ds.data, ds.gw, ds.gh);
    ReleaseTarget(ds);
    ds.transient_lazy = false;
    ds.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    ds.content_valid = false;
    ds.ended_mid_frame = false;
    CreateTargetResources(ds);
}

Target* VulkanBackend::FindTarget(const Surface& s, bool depth) {
    if (!s.header) return nullptr;
    uint32_t data = mem::VirtualFromPhysical(s.data);
    uint32_t gw = 0, gh = 0;
    SurfaceSize(s, gw, gh);
    if (!gw || !gh) return nullptr;
    uint64_t key = SizedTargetKey(data, depth, gw, gh);
    auto it = targets_.find(key);
    if (it == targets_.end() || it->second.gw != gw || it->second.gh != gh) return nullptr;
    UpdateTargetUsed(data);
    return &it->second;
}

Target* VulkanBackend::EnsureTarget(const Surface& s, bool depth) {
    if (!s.header) return nullptr;
    uint32_t data = mem::VirtualFromPhysical(s.data);
    uint32_t gw = 0, gh = 0;
    SurfaceSize(s, gw, gh);
    if (!gw || !gh) return nullptr;
    uint64_t key = SizedTargetKey(data, depth, gw, gh);
    UpdateTargetUsed(data);
    auto it = targets_.find(key);
    if (it != targets_.end() && it->second.gw == gw && it->second.gh == gh) return &it->second;
    if (it != targets_.end()) {
        ReleaseTarget(it->second);
        targets_.erase(it);
    }
    Target t;
    t.data = data;
    t.gw = gw;
    t.gh = gh;
    t.depth = depth;
    t.bytes = depth ? gw * gh * 4u : GuestPitch(gw) * gh;
    t.epoch = DirtyEpoch(data);
    t.sig = MemSig(data, t.bytes);
    if (!CreateTargetResources(t)) return nullptr;
    auto [ins, _] = targets_.emplace(key, std::move(t));
    return &ins->second;
}

void VulkanBackend::UploadTarget(Target& t, const Surface& s) {
    TextureDesc d;
    if (!Describe(s.header, d) || Format(d.format).depth) return;
    std::vector<uint8_t> bgra, rgba;
    uint32_t pitch = 0;
    bool over = !t.drawn && d.width == t.gw && d.height == t.gh && t.read.size() == size_t(t.gw) * t.gh * 4u;
    if (over) {
        pitch = t.gw * 4u;
        bgra.assign(t.read.size(), 0);
        uint32_t gpitch = GuestPitch(t.gw);
        const uint8_t* guest = static_cast<const uint8_t*>(kernel::HostPtr(t.data));
        for (uint32_t y = 0; y < t.gh; ++y) {
            for (uint32_t x = 0; x < t.gw; ++x) {
                const uint8_t* c = guest + size_t(y) * gpitch + x * 4u;
                size_t i = (size_t(y) * t.gw + x) * 4u;
                if (!std::memcmp(c, &t.read[i], 4)) continue;
                std::memcpy(&t.read[i], c, 4);
                std::memcpy(&bgra[i], c, 3);
                bgra[i + 3] = 255;
            }
        }
    } else {
        DecodeLevel(d, 0, 0, static_cast<const uint8_t*>(kernel::HostPtr(d.data)), nullptr, bgra, pitch);
    }

    rgba.resize(size_t(d.width) * d.height * 4u);
    if (pitch == d.width * 4u) {
        vk_tex::ConvertBGRA8ToRGBA8(bgra.data(), rgba.data(), d.width * d.height);
    } else {
        for (uint32_t y = 0; y < d.height; ++y) {
            vk_tex::ConvertBGRA8ToRGBA8(&bgra[size_t(y) * pitch], &rgba[size_t(y) * d.width * 4u], d.width);
        }
    }

    EndRenderPass(RpEndReason::Tex);
    VkCommandBuffer cmd = CurFrame().cmd;

    VkImage src_img = VK_NULL_HANDLE;
    VkDeviceMemory src_mem = VK_NULL_HANDLE;
    VkImageView src_view = VK_NULL_HANDLE;
    if (!CreateImage2D(d.width, d.height, 1, 1, VK_IMAGE_TYPE_2D, 0, VK_FORMAT_R8G8B8A8_UNORM,
                       VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, src_img, src_mem)) return;

    VkImageViewCreateInfo ivci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    ivci.image = src_img;
    ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ivci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(dev_, &ivci, nullptr, &src_view);

    // Copy RGBA pixels via stream ring (use dedicated temporary buffer if > remaining stream ring)
    uint64_t bytes = rgba.size();
    VkBuffer stg_buf = VK_NULL_HANDLE;
    VkDeviceMemory stg_mem = VK_NULL_HANDLE;
    uint64_t stg_off = 0;
    if (CurFrame().stream_used + bytes + 64 <= kStreamRingSize) {
        RingAlloc a = AllocStream(bytes, 16);
        std::memcpy(a.cpu, rgba.data(), bytes);
        stg_buf = a.buf;
        stg_off = a.offset;
    } else {
        void* p = nullptr;
        if (CreateBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0,
                         stg_buf, stg_mem, &p)) {
            std::memcpy(p, rgba.data(), bytes);
            vkUnmapMemory(dev_, stg_mem);
        }
    }

    VkImageLayout sl = VK_IMAGE_LAYOUT_UNDEFINED;
    TransitionImageRaw(cmd, src_img, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, sl, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy bic{};
    bic.bufferOffset = stg_off;
    bic.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bic.imageExtent = {d.width, d.height, 1};
    vkCmdCopyBufferToImage(cmd, stg_buf, src_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
    TransitionImageRaw(cmd, src_img, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, sl, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    if (EnsureRenderPass(&t, nullptr)) {
        VkShaderModule vs = ctx_->Shaders().GetOrCompile(Hash("util_vs", 7), false,
                                                         [] { return vk_sh::FullscreenVsGlsl(); });
        VkShaderModule fs = ctx_->Shaders().GetOrCompile(Hash("util_blit", 9), true,
                                                         [] { return vk_sh::BlitPsGlsl(); });
        VkPipeline pipe = GetUtilPipeline("blit", vs, fs, t.fmt, false, 0xF, over, compat_rps_[1][0]);
        float dc[8] = {0, 0, 1, 1, 1.0f, 1.0f, 1.0f, 0};
        DrawFullscreenUtil(pipe, dc, src_view, blit_linear_, {t.w, t.h});
        EndRenderPass(RpEndReason::Tex);
    }

    DeferView(src_view);
    Defer(GarbageItem::Kind::Image, src_img);
    Defer(GarbageItem::Kind::Memory, src_mem);
    if (stg_mem != VK_NULL_HANDLE) {
        Defer(GarbageItem::Kind::Buffer, stg_buf);
        Defer(GarbageItem::Kind::Memory, stg_mem);
    }

    t.epoch = DirtyEpoch(t.data);
    t.sig = MemSig(t.data, t.bytes);
}

VkImageView VulkanBackend::EnsureFeedback(Target& t) {
    EndRenderPass(RpEndReason::Fb);
    if (!t.fb_image) {
        if (!CreateImage2D(t.w, t.h, 1, 1, VK_IMAGE_TYPE_2D, 0, t.fmt,
                           VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, t.fb_image, t.fb_mem)) {
            return dummy_2d_view_;
        }
        VkImageViewCreateInfo iv{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        iv.image = t.fb_image;
        iv.viewType = VK_IMAGE_VIEW_TYPE_2D;
        iv.format = t.fmt;
        iv.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(dev_, &iv, nullptr, &t.fb_view);
        t.fb_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    }
    VkCommandBuffer cmd = CurFrame().cmd;
    TransitionImageRaw(cmd, t.image, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, t.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    TransitionImageRaw(cmd, t.fb_image, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, t.fb_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy cpy{};
    cpy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    cpy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    cpy.extent = {t.w, t.h, 1};
    vkCmdCopyImage(cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t.fb_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cpy);
    TransitionImageRaw(cmd, t.fb_image, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, t.fb_layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return t.fb_view ? t.fb_view : dummy_2d_view_;
}

bool VulkanBackend::CreateTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi) {
    DeferView(tx.view);
    Defer(GarbageItem::Kind::Image, tx.image);
    DeferSlab(tx.slab_alloc);
    Defer(GarbageItem::Kind::Memory, tx.mem);
    tex_vram_bytes_ = (tex_vram_bytes_ > tx.vram_bytes) ? (tex_vram_bytes_ - tx.vram_bytes) : 0;

    tx = Tex{};
    tx.d = d;
    uint32_t skip = 0;
    if (low_vram_tex_ && !d.linear && !d.cube && d.depth == 1 && d.levels >= 3 && d.width >= 128 && d.height >= 128) {
        skip = 1u;
    }
    tx.mip_skip = uint8_t(skip);
    tx.levels = d.levels - skip;
    uint32_t w = std::max(1u, d.width >> skip), h = std::max(1u, d.height >> skip);
    bool hw_bc = fi.compressed && ctx_->HasBC();

    if (hw_bc) {
        tx.fmt = (fi.host == Host::BC1) ? VK_FORMAT_BC1_RGBA_UNORM_BLOCK
               : (fi.host == Host::BC2) ? VK_FORMAT_BC2_UNORM_BLOCK
                                        : VK_FORMAT_BC3_UNORM_BLOCK;
        if ((w & 3u) || (h & 3u)) {
            w = (w + 3u) & ~3u;
            h = (h + 3u) & ~3u;
            tx.levels = 1;
        }
        tx.linear_ok = true;
    } else if (fi.host == Host::R32F) {
        tx.fmt = VK_FORMAT_R32_SFLOAT;
        tx.linear_ok = r32f_linear_;
    } else {
        // Both uncompressed formats (converted BGRA->RGBA8) and NEON-decoded BC1/2/3 use RGBA8
        // to preserve Mali-G68 AFBC compression compatibility.
        tx.fmt = VK_FORMAT_R8G8B8A8_UNORM;
        tx.linear_ok = true;
    }

    VkImageType type = (d.depth > 1) ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    VkImageCreateFlags flags = d.cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    uint32_t layers = d.cube ? 6u : d.depth;

    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.flags = flags;
    ici.imageType = type;
    ici.format = tx.fmt;
    ici.extent = {std::max(1u, w), std::max(1u, h), (type == VK_IMAGE_TYPE_3D) ? std::max(1u, layers) : 1u};
    ici.mipLevels = std::max(1u, tx.levels);
    ici.arrayLayers = (type == VK_IMAGE_TYPE_3D) ? 1u : std::max(1u, layers);
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(dev_, &ici, nullptr, &tx.image) != VK_SUCCESS) {
        LOGE("vk_backend: vkCreateImage(%ux%u fmt=%d) failed", w, h, int(tx.fmt));
        return false;
    }

    VkMemoryRequirements mr{};
    vkGetImageMemoryRequirements(dev_, tx.image, &mr);
    const uint32_t mem_type = ctx_->FindMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    auto try_bind = [&]() -> bool {
        if (mem_type == UINT32_MAX) return false;
        VkDeviceMemory slab_mem = VK_NULL_HANDLE;
        if (tex_slab_.Alloc(dev_, mr.size, mr.alignment, mem_type, slab_mem, tx.slab_alloc)) {
            if (vkBindImageMemory(dev_, tx.image, slab_mem, tx.slab_alloc.offset) == VK_SUCCESS) return true;
            tex_slab_.Free(tx.slab_alloc);
            tx.slab_alloc = {};
        }
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = mr.size;
        mai.memoryTypeIndex = mem_type;
        if (vkAllocateMemory(dev_, &mai, nullptr, &tx.mem) == VK_SUCCESS && tx.mem != VK_NULL_HANDLE) {
            if (vkBindImageMemory(dev_, tx.image, tx.mem, 0) == VK_SUCCESS) return true;
            vkFreeMemory(dev_, tx.mem, nullptr);
            tx.mem = VK_NULL_HANDLE;
        }
        return false;
    };

    if (!try_bind()) {
        ReleaseOldTextures(true);
        if (!try_bind()) {
            vkDestroyImage(dev_, tx.image, nullptr);
            tx.image = VK_NULL_HANDLE;
            return false;
        }
    }

    tx.vram_bytes = mr.size;
    tex_vram_bytes_ += tx.vram_bytes;

    VkImageViewCreateInfo ivci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    ivci.image = tx.image;
    ivci.viewType = d.cube ? VK_IMAGE_VIEW_TYPE_CUBE : (d.depth > 1 ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D);
    ivci.format = tx.fmt;
    ivci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, tx.levels, 0, d.cube ? 6u : 1u};
    if (vkCreateImageView(dev_, &ivci, nullptr, &tx.view) != VK_SUCCESS) return false;
    tx.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return true;
}

void VulkanBackend::UploadTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi,
                                  const uint8_t* guest_src, const uint32_t* palette, bool same) {
    // New textures (!same) are recorded in a pre-pass upload command buffer submitted right before f.cmd,
    // eliminating mid-frame TBDR RenderPass breaks when streaming world/character textures.
    const bool use_upload_cmd = (in_rp_ && !same);
    VkCommandBuffer cmd = use_upload_cmd ? BeginUploadCmd() : VK_NULL_HANDLE;
    if (!use_upload_cmd) {
        EndRenderPass(RpEndReason::Tex);
        cmd = CurFrame().cmd;
    }

    const uint32_t faces = d.cube ? 6u : 1u;
    const bool hw_bc = fi.compressed && ctx_->HasBC();

    TransitionImageRaw(cmd, tx.image, VK_IMAGE_ASPECT_COLOR_BIT, tx.levels, faces,
                       tx.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    struct MipPlan {
        uint32_t f, l, gl, lw, lh, lz;
        size_t off, bytes;
    };
    std::vector<MipPlan> plans;
    plans.reserve(faces * tx.levels);
    std::vector<VkBufferImageCopy> copies;
    copies.reserve(faces * tx.levels);

    size_t total_staging = 0;
    for (uint32_t f = 0; f < faces; ++f) {
        for (uint32_t l = 0; l < tx.levels; ++l) {
            uint32_t gl = l + tx.mip_skip;
            uint32_t lw = d.linear ? d.width : std::max(d.width >> gl, 1u);
            uint32_t lh = d.linear ? d.height : std::max(d.height >> gl, 1u);
            uint32_t lz = d.linear ? 1u : std::max(d.depth >> gl, 1u);
            size_t mip_bytes = hw_bc ? LevelSize(d, gl) : size_t(lw) * lh * lz * 4u;
            size_t off = (total_staging + 15u) & ~size_t(15u);
            total_staging = off + mip_bytes;
            plans.push_back({f, l, gl, lw, lh, lz, off, mip_bytes});
            VkBufferImageCopy c{};
            c.bufferOffset = off;
            c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, f, 1};
            c.imageExtent = {lw, lh, lz};
            copies.push_back(c);
        }
    }

    if (total_staging > 0) {
        VkBuffer buf = VK_NULL_HANDLE;
        uint64_t base_off = 0;
        VkDeviceMemory temp_mem = VK_NULL_HANDLE;
        uint8_t* dst_base = nullptr;
        if (CurFrame().stream_used + total_staging + 64 <= kStreamRingSize) {
            RingAlloc a = AllocStream(total_staging, 16);
            buf = a.buf;
            base_off = a.offset;
            dst_base = a.cpu;
        } else {
            void* p = nullptr;
            if (CreateBuffer(total_staging, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0,
                             buf, temp_mem, &p)) {
                dst_base = static_cast<uint8_t*>(p);
            }
        }

        if (buf != VK_NULL_HANDLE && dst_base) {
            for (const MipPlan& m : plans) {
                uint8_t* dst = dst_base + m.off;
                if (fi.compressed) {
                    const uint8_t* src_lvl = guest_src + (d.cube ? m.f * FaceSize(d) : 0u) + LevelOffset(d, m.gl);
                    if (hw_bc) {
                        std::memcpy(dst, src_lvl, m.bytes);
                    } else {
                        sec_neon_++;
                        const uint32_t slice_src_bytes =
                            std::max(1u, (m.lw + 3u) / 4u) * std::max(1u, (m.lh + 3u) / 4u) * fi.bits;
                        for (uint32_t z = 0; z < m.lz; ++z) {
                            const uint8_t* s_slice = src_lvl + size_t(z) * slice_src_bytes;
                            uint8_t* d_slice = dst + size_t(z) * m.lw * m.lh * 4u;
                            if (fi.host == Host::BC1) vk_tex::DecodeBC1ToRGBA8(s_slice, d_slice, m.lw, m.lh);
                            else if (fi.host == Host::BC2) vk_tex::DecodeBC2ToRGBA8(s_slice, d_slice, m.lw, m.lh);
                            else vk_tex::DecodeBC3ToRGBA8(s_slice, d_slice, m.lw, m.lh);
                        }
                    }
                } else {
                    uint32_t pitch = 0;
                    DecodeLevel(d, m.gl, m.f, guest_src, palette, tex_tmp_, pitch);
                    if (fi.host == Host::BGRA8) {
                        if (pitch == m.lw * 4u) {
                            vk_tex::ConvertBGRA8ToRGBA8(tex_tmp_.data(), dst, m.lw * m.lh * m.lz);
                        } else {
                            for (uint32_t row = 0; row < m.lh * m.lz; ++row) {
                                vk_tex::ConvertBGRA8ToRGBA8(&tex_tmp_[size_t(row) * pitch],
                                                            dst + size_t(row) * m.lw * 4u, m.lw);
                            }
                        }
                    } else {
                        std::memcpy(dst, tex_tmp_.data(), std::min(m.bytes, tex_tmp_.size()));
                    }
                }
            }
            if (temp_mem != VK_NULL_HANDLE) {
                vkUnmapMemory(dev_, temp_mem);
                Defer(GarbageItem::Kind::Buffer, buf);
                Defer(GarbageItem::Kind::Memory, temp_mem);
            }
            for (auto& c : copies) c.bufferOffset += base_off;
            vkCmdCopyBufferToImage(cmd, buf, tx.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   uint32_t(copies.size()), copies.data());
        } else if (temp_mem != VK_NULL_HANDLE) {
            vkDestroyBuffer(dev_, buf, nullptr);
            vkFreeMemory(dev_, temp_mem, nullptr);
        }
    }

    TransitionImageRaw(cmd, tx.image, VK_IMAGE_ASPECT_COLOR_BIT, tx.levels, faces,
                       tx.layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    sec_texups_++;
}

VkImageView VulkanBackend::BindTargetTexture(const State& s, int stage, const TextureDesc& d, Target& t,
                                             DrawTextures& stages, bool) {
    TexInfo& info = stages.info[stage];
    if (&t == cur_draw_ds_) return dummy_2d_view_;
    bool feedback = (&t == cur_draw_rt_);
    stages.rt[stage] = true;
    info.flags |= kTexTarget;
    info.kind = kTex2D;
    if (t.depth) {
        if (t.transient_lazy) PromoteDepthToPersistent(t);
        info.shadow = (d.format == 0x2Cu || d.format == 0x30u) ? 2 : 1;
        stages.linear_ok[stage] = depth_linear_;
    } else if (LatchCpuWrite(t)) {
        Surface su;
        su.header = s.textures[size_t(stage)];
        UploadTarget(t, su);
    }

    Target* rt = FindTarget(s.rt, false);
    float k = float(t.w) / (t.gw * stages.k[stage]);
    if (!t.depth && k > 1.01f && rt && rt != &t && (t.gw > rt->gw || t.gh > rt->gh)) {
        stages.wide[stage] = k;
        info.flags |= kTexWide;
    }
    if (feedback) return EnsureFeedback(t);

    if (t.layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        EndRenderPass(RpEndReason::RtTex);
        VkImageAspectFlags asp = t.depth ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) : VK_IMAGE_ASPECT_COLOR_BIT;
        TransitionImageRaw(CurFrame().cmd, t.image, asp, 1, 1, t.layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    return t.sample_view ? t.sample_view : dummy_2d_view_;
}

struct StagePalette {
    uint32_t colours[256]{};
    uint32_t va = 0;
    uint64_t hash = 0;
};

void ReadStagePalette(const State& s, int stage, const TextureDesc& d, StagePalette& pal) {
    if (d.format != 0x0Bu || !s.palettes[size_t(stage)]) return;
    uint32_t header = s.palettes[size_t(stage)];
    pal.va = mem::VirtualFromPhysical(kernel::R32(header + 4u));
    uint32_t size_code = kernel::R32(header) >> 30;
    size_t bytes = (256u >> size_code) * 4u;
    std::memcpy(pal.colours, kernel::HostPtr(pal.va), bytes);
    pal.hash = Hash(pal.colours, sizeof(pal.colours));
}

VkImageView VulkanBackend::BindGuestTexture(const State& s, int stage, const TextureDesc& d,
                                            const FormatInfo& fi, DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    StagePalette pal;
    ReadStagePalette(s, stage, d, pal);
    const uint64_t key = (uint64_t(pal.va) << 32) | d.data;
    const uint32_t* colours = pal.va ? pal.colours : nullptr;

    Tex& tx = textures_[key];
    const bool same = tx.image && tx.d.format == d.format && tx.d.width == d.width && tx.d.height == d.height &&
                      tx.d.depth == d.depth && tx.d.levels == d.levels && tx.d.cube == d.cube && tx.d.pitch == d.pitch;
    if (!same && !CreateTexture(tx, d, fi)) {
        textures_.erase(key);
        return d.cube ? dummy_cube_view_ : (d.depth > 1 ? dummy_3d_view_ : dummy_2d_view_);
    }

    // Check guest memory at most once per frame; static BCn world textures re-verify every 4 frames unless DirtyEpoch bumped.
    if (tx.frame != presents_ + 1 || !same || tx.palette != pal.hash) {
        tx.frame = presents_ + 1;
        const uint64_t cur_epoch = DirtyEpoch(d.data);
        const bool need_hash = !same || tx.palette != pal.hash || cur_epoch != tx.epoch ||
                               !fi.compressed || (presents_ - tx.sig_frame) >= 4u;
        tx.palette = pal.hash;
        if (need_hash) {
            tx.epoch = cur_epoch;
            tx.sig_frame = presents_;
            uint32_t total = TotalSize(d);
            uint64_t sig = MemSig(d.data, total) ^ cur_epoch ^ pal.hash;
            if (sig != tx.sig || !same) {
                tx.sig = sig;
                const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(d.data));
                UploadTexture(tx, d, fi, p, colours, same);
            }
        }
    }

    if (fi.depth) {
        info.shadow = (d.format == 0x2Cu || d.format == 0x30u) ? 2 : 1;
        // R32F depth texture doesn't work with sampler2DShadow hardware compare if uploaded as R32_SFLOAT!
        // Note: on Vulkan, R32_SFLOAT does NOT have a depth aspect for sampler2DShadow unless in D32F!
        //Wait: let's handle R32_SFLOAT depth textures vs RenderTarget depth safely!
        // Wait: guest rarely binds CPU-uploaded R32_SFLOAT as hardware shadow; render target shadows are BindTargetTexture!
        if (tx.fmt == VK_FORMAT_R32_SFLOAT) info.shadow = 0;
    }
    stages.linear_ok[stage] = tx.linear_ok;
    if (tx.layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        EndRenderPass();
        TransitionImageRaw(CurFrame().cmd, tx.image, VK_IMAGE_ASPECT_COLOR_BIT, tx.levels,
                           d.cube ? 6u : 1u, tx.layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    return tx.view;
}

void VulkanBackend::PrepareDrawTextures(const State& s, DrawTextures& stages, bool) {
    for (int i = 0; i < 4; ++i) {
        stages.info[i] = {};
        stages.views[i] = dummy_2d_view_;
        stages.linear_ok[i] = true;
        stages.rt[i] = false;
        stages.k[i] = 1.0f;
        stages.wide[i] = 0.0f;

        TextureDesc d;
        if (!s.textures[size_t(i)] || !Describe(s.textures[size_t(i)], d)) continue;
        const FormatInfo& fi = Format(d.format);
        stages.info[i].kind = d.cube ? kTexCube : (d.depth > 1 ? kTex3D : kTex2D);
        stages.info[i].rect = d.linear;

        auto tit = targets_.find(SizedTargetKey(d.data, fi.depth, d.width, d.height));
        if (tit != targets_.end() && !d.cube && d.depth == 1 && !fi.compressed) {
            stages.views[i] = BindTargetTexture(s, i, d, tit->second, stages, true);
        } else {
            stages.views[i] = BindGuestTexture(s, i, d, fi, stages);
        }
    }
}

bool VulkanBackend::PrepareBindTargets(const State& s, Target*& out_rt, Target*& out_ds, bool zs_sampled) {
    Target* rt = EnsureTarget(s.rt, false);
    // Keep the active depth target attached across !ZEnable / !StencilEnable draws (HUD, particles, decals)
    // unless a shader stage actively samples this depth buffer, avoiding TBDR RenderPass ping-pong & lazy spills.
    Target* ds = (!zs_sampled && s.zs.header) ? EnsureTarget(s.zs, true) : nullptr;
    if (rt && ds && (rt->w != ds->w || rt->h != ds->h)) ds = nullptr;
    if (!rt && !ds) return false;
    if (rt && LatchCpuWrite(*rt)) UploadTarget(*rt, s.rt);
    if (rt) rt->drawn = true;
    if (ds) ds->drawn = true;
    out_rt = rt;
    out_ds = ds;
    return true;
}

VkSampler VulkanBackend::GetStageSampler(const State& s, int i, const DrawTextures& stages,
                                         bool clamp, bool fixed, bool lin_ok) {
    const auto& t = s.tss[size_t(i)];
    uint32_t mag = t[kTssMagFilter], min = t[kTssMinFilter], mip = t[kTssMipFilter];
    auto lin = [&](uint32_t f) { return lin_ok && f >= 2u; };

    VkSamplerCreateInfo sd{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sd.minFilter = lin(min) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sd.magFilter = lin(mag) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sd.mipmapMode = (lin_ok && mip == 2u) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;

    const uint32_t user_aniso = app::Get().aniso;
    bool aniso = (min == 3u || mag == 3u) || (user_aniso > 1u && lin(min) && mip && !stages.info[i].shadow);
    if (ctx_->HasAnisotropy() && aniso && lin_ok && !stages.info[i].shadow) {
        uint32_t game_want = (min == 3u || mag == 3u) ? std::max<uint32_t>(t[kTssMaxAnisotropy], 1u) : 1u;
        uint32_t want = std::max(game_want, user_aniso);
        sd.anisotropyEnable = want > 1u ? VK_TRUE : VK_FALSE;
        sd.maxAnisotropy = std::min(float(want), ctx_->MaxAnisotropy());
    }

    if (fixed && (((t[kTssTexCoordIndex] >> 16) & 7u) || (t[kTssTextureTransformFlags] & 7u))) clamp = false;
    sd.addressModeU = AddressVk((clamp && t[kTssAddressU] == 1u) ? 3u : t[kTssAddressU]);
    sd.addressModeV = AddressVk((clamp && t[kTssAddressV] == 1u) ? 3u : t[kTssAddressV]);
    sd.addressModeW = AddressVk(t[kTssAddressW]);
    sd.mipLodBias = Bits(t[kTssMipLodBias]);
    sd.minLod = float(t[kTssMaxMipLevel]);
    sd.maxLod = mip ? VK_LOD_CLAMP_NONE : sd.minLod;
    sd.borderColor = NearestBorderColor(t[kTssBorderColor]);

    if (stages.info[i].shadow) {
        sd.compareEnable = VK_TRUE;
        sd.compareOp = CmpOp(s.rs[kRsShadowFunc]);
        sd.anisotropyEnable = VK_FALSE;
    }

    uint64_t hk = Hash(&sd, sizeof(sd));
    if (auto it = samplers_.find(hk); it != samplers_.end()) return it->second;
    VkSampler smp = VK_NULL_HANDLE;
    if (vkCreateSampler(dev_, &sd, nullptr, &smp) != VK_SUCCESS) return blit_linear_;
    samplers_[hk] = smp;
    return smp;
}

VkDescriptorSet VulkanBackend::GetOrCreateTexSet(const VkImageView views[4], const VkSampler samplers[4]) {
    uint64_t key_data[8]{};
    for (int i = 0; i < 4; ++i) {
        key_data[i] = reinterpret_cast<uint64_t>(views[i]);
        key_data[4 + i] = reinterpret_cast<uint64_t>(samplers[i]);
    }
    uint64_t hk = Hash(key_data, sizeof(key_data));
    if (auto it = tex_desc_cache_.find(hk); it != tex_desc_cache_.end()) {
        TexDescEntry& e = it->second;
        if (std::memcmp(e.views, views, sizeof(e.views)) == 0) {
            e.last_used_present = presents_;
            return e.set;
        }
    }

    auto evict_lru = [&] {
        if (tex_desc_cache_.empty()) return false;
        std::vector<std::pair<uint64_t, uint64_t>> entries;
        entries.reserve(tex_desc_cache_.size());
        for (const auto& [k_it, e_it] : tex_desc_cache_) {
            entries.emplace_back(e_it.last_used_present, k_it);
        }
        std::sort(entries.begin(), entries.end());
        size_t to_evict = std::max<size_t>(entries.size() / 2u, 1u);
        size_t evicted = 0;
        for (size_t i = 0; i < to_evict; ++i) {
            auto f_it = tex_desc_cache_.find(entries[i].second);
            if (f_it == tex_desc_cache_.end()) continue;
            if (f_it->second.set == bound_tex_set_) bound_tex_set_ = VK_NULL_HANDLE;
            DeferDestroy(GarbageItem::Kind::DescriptorSet, reinterpret_cast<uint64_t>(f_it->second.set));
            tex_desc_cache_.erase(f_it);
            ++evicted;
        }
        LOGR("vk_backend: LRU-evicted %zu texture descriptor sets (remaining=%zu)", evicted, tex_desc_cache_.size());
        return evicted > 0;
    };

    auto reset_pool = [&] {
        Flush();
        ctx_->WaitIdle();
        for (Frame& f : frames_) CollectGarbage(f);
        tex_desc_cache_.clear();
        vkResetDescriptorPool(dev_, tex_desc_pool_, 0);
        bound_tex_set_ = VK_NULL_HANDLE;
        LOGR("vk_backend: full flush & descriptor pool reset completed");
    };

    if (tex_desc_cache_.size() >= kTexDescPoolSize - 64u) {
        evict_lru();
    }

    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = tex_desc_pool_;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &tex_set_layout_;
    if (vkAllocateDescriptorSets(dev_, &dsai, &set) != VK_SUCCESS) {
        evict_lru();
        if (vkAllocateDescriptorSets(dev_, &dsai, &set) != VK_SUCCESS) {
            reset_pool();
            if (vkAllocateDescriptorSets(dev_, &dsai, &set) != VK_SUCCESS) {
                LOGE("vk_backend: persistent texture descriptor pool exhausted");
                return VK_NULL_HANDLE;
            }
        }
    }

    VkDescriptorImageInfo imgs[4]{};
    VkWriteDescriptorSet writes[4]{};
    for (int i = 0; i < 4; ++i) {
        imgs[i] = {samplers[i], views[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, uint32_t(i), 0, 1,
                     VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &imgs[i], nullptr, nullptr};
    }
    vkUpdateDescriptorSets(dev_, 4, writes, 0, nullptr);
    tex_desc_cache_[hk] = {set, {views[0], views[1], views[2], views[3]}, presents_};
    return set;
}

VkPipeline VulkanBackend::GetDrawPipeline(const PsoKey& k, const Assembled& a,
                                          VkShaderModule vs, VkShaderModule fs, VkRenderPass rp) {
    uint64_t hk = Hash(&k, sizeof(k), Hash(&rp, sizeof(rp)));
    if (auto it = psos_.find(hk); it != psos_.end()) return it->second;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";

    VkVertexInputBindingDescription bindings[16]{};
    VkVertexInputAttributeDescription attrs[16]{};
    uint32_t nb = a.buffer_count;
    uint32_t na = 0;
    for (uint32_t i = 0; i < nb; ++i) {
        bindings[i] = {i, a.buffers[i].stride, VK_VERTEX_INPUT_RATE_VERTEX};
    }
    for (uint32_t r = 0; r < 16; ++r) {
        if (!((k.regs >> r) & 1u)) continue;
        uint32_t e = k.elements[r];
        VkFormat fmt = VertexVkFormat(e & 0xFFu);
        if (fmt == VK_FORMAT_UNDEFINED) fmt = VK_FORMAT_R32G32B32A32_SFLOAT;
        attrs[na++] = {r, (e >> 8) & 0xFFu, fmt, e >> 16};
    }

    VkPipelineVertexInputStateCreateInfo vis{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vis.vertexBindingDescriptionCount = nb;
    vis.pVertexBindingDescriptions = bindings;
    vis.vertexAttributeDescriptionCount = na;
    vis.pVertexAttributeDescriptions = attrs;

    const uint32_t base_topo = k.topo & 0xFu;
    VkPipelineInputAssemblyStateCreateInfo ias{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    if (base_topo == uint32_t(Topology::Points)) ias.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    else if (base_topo == uint32_t(Topology::Lines)) ias.topology = a.strip ? VK_PRIMITIVE_TOPOLOGY_LINE_STRIP : VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    else ias.topology = a.strip ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vps.viewportCount = 1;
    vps.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = (k.fill == 0x1B01u && ctx_->HasWireframe() && base_topo != 0) ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    rs.cullMode = (k.cull && base_topo == uint32_t(Topology::Triangles)) ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    rs.frontFace = k.ccw ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    rs.depthBiasEnable = k.zbias_on ? VK_TRUE : VK_FALSE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = k.zen ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = k.zwrite ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = ReversedCmpOp(k.zfunc);
    ds.stencilTestEnable = k.sten ? VK_TRUE : VK_FALSE;
    VkStencilOpState st{StencilOpVk(k.sfail), StencilOpVk(k.spass), StencilOpVk(k.szfail),
                        CmpOp(k.sfunc), uint8_t(k.smask), uint8_t(k.swmask), 0};
    ds.front = ds.back = st;

    VkPipelineColorBlendAttachmentState cba{};
    cba.blendEnable = k.blend ? VK_TRUE : VK_FALSE;
    cba.srcColorBlendFactor = BlendFactor(k.src, false);
    cba.dstColorBlendFactor = BlendFactor(k.dst, false);
    cba.colorBlendOp = BlendOpVk(k.op);
    cba.srcAlphaBlendFactor = BlendFactor(k.src, true);
    cba.dstAlphaBlendFactor = BlendFactor(k.dst, true);
    cba.alphaBlendOp = BlendOpVk(k.op);
    cba.colorWriteMask = k.wmask & 0xFu;

    VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cbs.attachmentCount = k.has_rt ? 1 : 0;
    cbs.pAttachments = k.has_rt ? &cba : nullptr;

    VkDynamicState dyn[5] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
        VK_DYNAMIC_STATE_BLEND_CONSTANTS,
        VK_DYNAMIC_STATE_STENCIL_REFERENCE,
        VK_DYNAMIC_STATE_DEPTH_BIAS,
    };
    VkPipelineDynamicStateCreateInfo dyns{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyns.dynamicStateCount = 5;
    dyns.pDynamicStates = dyn;

    VkGraphicsPipelineCreateInfo gpci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gpci.stageCount = 2;
    gpci.pStages = stages;
    gpci.pVertexInputState = &vis;
    gpci.pInputAssemblyState = &ias;
    gpci.pViewportState = &vps;
    gpci.pRasterizationState = &rs;
    gpci.pMultisampleState = &ms;
    gpci.pDepthStencilState = k.has_ds ? &ds : nullptr;
    gpci.pColorBlendState = &cbs;
    gpci.pDynamicState = &dyns;
    gpci.layout = pipe_layout_;
    gpci.renderPass = rp;

    VkPipeline pso = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(dev_, ctx_->PipelineCache(), 1, &gpci, nullptr, &pso) != VK_SUCCESS) {
        LOGE("vk_backend: vkCreateGraphicsPipelines failed (key=%016llX)", static_cast<unsigned long long>(hk));
    }
    psos_[hk] = pso;
    return pso;
}

VkPipeline VulkanBackend::GetUtilPipeline(const char* tag, VkShaderModule vs, VkShaderModule fs, VkFormat,
                                          bool is_depth, uint8_t color_mask, bool alpha_over, VkRenderPass rp) {
    uint64_t seed = (uint64_t(is_depth) << 16) | (uint64_t(alpha_over) << 8) | color_mask;
    uint64_t hk = Hash(&rp, sizeof(rp), Hash(tag, std::strlen(tag), seed));
    if (auto it = util_psos_.find(hk); it != util_psos_.end()) return it->second;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
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

    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = is_depth ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = is_depth ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;

    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = color_mask & 0xFu;
    if (alpha_over) {
        cba.blendEnable = VK_TRUE;
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.colorBlendOp = VK_BLEND_OP_ADD;
        cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.alphaBlendOp = VK_BLEND_OP_ADD;
    }

    VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cbs.attachmentCount = is_depth ? 0 : 1;
    cbs.pAttachments = is_depth ? nullptr : &cba;

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
    gpci.pDepthStencilState = &ds;
    gpci.pColorBlendState = &cbs;
    gpci.pDynamicState = &dyns;
    gpci.layout = pipe_layout_;
    gpci.renderPass = rp;

    VkPipeline pipe = VK_NULL_HANDLE;
    vkCreateGraphicsPipelines(dev_, ctx_->PipelineCache(), 1, &gpci, nullptr, &pipe);
    util_psos_[hk] = pipe;
    return pipe;
}

void VulkanBackend::DrawFullscreenUtil(VkPipeline pipe, const float dc_data[8], VkImageView src_view,
                                       VkSampler smp, VkExtent2D ext, const VkRect2D* scissor_override) {
    if (!pipe) return;
    RingAlloc dc = AllocConst(32, 0);
    std::memcpy(dc.cpu, dc_data, 32);

    VkImageView views[4] = {src_view ? src_view : dummy_2d_view_, dummy_2d_view_, dummy_2d_view_, dummy_2d_view_};
    VkSampler smps[4] = {smp ? smp : blit_linear_, blit_linear_, blit_linear_, blit_linear_};
    VkDescriptorSet tex_set = GetOrCreateTexSet(views, smps);
    if (!tex_set) return;

    VkCommandBuffer cmd = CurFrame().cmd;
    VkViewport vp{0.0f, 0.0f, float(ext.width), float(ext.height), 0.0f, 1.0f};
    VkRect2D sc = scissor_override ? *scissor_override : VkRect2D{{0, 0}, ext};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    VkDescriptorSet ubo_set = CurFrame().ubo_set;
    uint32_t dyn[2] = {0, uint32_t(dc.offset)};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_layout_, 0, 1, &ubo_set, 2, dyn);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_layout_, 1, 1, &tex_set, 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    InvalidateBoundState();
}

bool VulkanBackend::DepthCopy(const State& s) {
    TextureDesc d;
    if (!s.rt.header || !s.textures[0] || !Describe(s.textures[0], d) || Format(d.format).depth ||
        d.cube || d.depth != 1) return false;
    uint32_t gw = 0, gh = 0;
    SurfaceSize(s.rt, gw, gh);
    auto di = targets_.find(SizedTargetKey(mem::VirtualFromPhysical(s.rt.data), true, gw, gh));
    auto si = targets_.find(SizedTargetKey(d.data, true, d.width, d.height));
    if (di == targets_.end() || si == targets_.end() || di == si) return false;

    Target& src = si->second;
    Target& dst = di->second;
    if (src.transient_lazy) PromoteDepthToPersistent(src);
    if (dst.transient_lazy) PromoteDepthToPersistent(dst);

    EndRenderPass();
    VkCommandBuffer cmd = CurFrame().cmd;
    TransitionImageRaw(cmd, src.image, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 1, 1,
                       src.layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    dst.pending_clear_z = true;
    dst.clear_z = 0.0f;
    if (!EnsureRenderPass(nullptr, &dst)) return false;

    VkShaderModule vs = ctx_->Shaders().GetOrCompile(Hash("util_vs", 7), false, [] { return vk_sh::FullscreenVsGlsl(); });
    VkShaderModule fs = ctx_->Shaders().GetOrCompile(Hash("util_dcopy", 10), true, [] { return vk_sh::DepthCopyPsGlsl(); });
    VkPipeline pipe = GetUtilPipeline("dcopy", vs, fs, ctx_->DepthFormat(), true, 0, false, compat_rps_[0][1]);
    float dc[8] = {0, 0, 1, 1, 0, 0, 0, 0};
    DrawFullscreenUtil(pipe, dc, src.sample_view, blit_point_, {dst.w, dst.h});
    dst.drawn = true;
    EndRenderPass();
    return true;
}

void VulkanBackend::ClearChannels(Target& t, Target* ds_hint, const float col[4], uint8_t mask,
                                  const std::vector<VkRect2D>& rects) {
    Target* use_ds = (in_rp_ && rp_rt_ == &t) ? rp_ds_
                   : (ds_hint && ds_hint->w == t.w && ds_hint->h == t.h ? ds_hint : nullptr);
    if (!EnsureRenderPass(&t, use_ds)) return;
    VkShaderModule vs = ctx_->Shaders().GetOrCompile(Hash("util_vs", 7), false, [] { return vk_sh::FullscreenVsGlsl(); });
    VkShaderModule fs = ctx_->Shaders().GetOrCompile(Hash("util_clear", 10), true, [] { return vk_sh::ClearPsGlsl(); });
    VkPipeline pipe = GetUtilPipeline("clear", vs, fs, t.fmt, false, mask, false, compat_rps_[1][use_ds ? 1 : 0]);
    float dc[8] = {0, 0, 1, 1, col[0], col[1], col[2], col[3]};
    for (const VkRect2D& r : rects) {
        if (r.extent.width == 0 || r.extent.height == 0) continue;
        DrawFullscreenUtil(pipe, dc, dummy_2d_view_, blit_point_, {t.w, t.h}, &r);
    }
}

void VulkanBackend::Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags,
                          uint32_t color, float z, uint32_t stencil) {
    total_clears_++;
    Target* rt = (flags & kClearTarget) ? EnsureTarget(s.rt, false) : nullptr;
    Target* ds = (flags & (kClearZ | kClearStencil)) ? EnsureTarget(s.zs, true) : nullptr;
    if (!rt && !ds) return;

    auto build_rects = [&](const Target& t) {
        std::vector<VkRect2D> rs;
        float sc = float(t.w) / float(t.gw);
        if (count && rects) {
            for (uint32_t i = 0; i < count; ++i) {
                int32_t r[4];
                std::memcpy(r, rects + size_t(i) * 16u, 16);
                int32_t x0 = std::clamp(int32_t(r[0] * sc), 0, int32_t(t.w));
                int32_t y0 = std::clamp(int32_t(r[1] * sc), 0, int32_t(t.h));
                int32_t x1 = std::clamp(int32_t(r[2] * sc), 0, int32_t(t.w));
                int32_t y1 = std::clamp(int32_t(r[3] * sc), 0, int32_t(t.h));
                if (x1 > x0 && y1 > y0) rs.push_back({{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}});
            }
        } else {
            const auto& v = s.viewport;
            int32_t x0 = std::clamp(int32_t(v.x * sc), 0, int32_t(t.w));
            int32_t y0 = std::clamp(int32_t(v.y * sc), 0, int32_t(t.h));
            int32_t x1 = std::clamp(int32_t((v.x + v.w) * sc), 0, int32_t(t.w));
            int32_t y1 = std::clamp(int32_t((v.y + v.h) * sc), 0, int32_t(t.h));
            if (x1 > x0 && y1 > y0) rs.push_back({{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}});
        }
        return rs;
    };

    if (rt) {
        if (LatchCpuWrite(*rt)) UploadTarget(*rt, s.rt);
        float col[4];
        Color(color, col);
        uint8_t mask = uint8_t((flags & kClearTarget) >> 4);
        std::vector<VkRect2D> rs = build_rects(*rt);
        bool full = (rs.size() == 1 && rs[0].offset.x == 0 && rs[0].offset.y == 0 &&
                     rs[0].extent.width == rt->w && rs[0].extent.height == rt->h);
        if (mask == 0xF && full && !in_rp_) {
            rt->pending_clear_color = true;
            std::memcpy(rt->clear_rgba, col, sizeof(col));
            rt->content_valid = true;
        } else if (mask == 0xF && in_rp_ && rp_rt_ == rt && !rs.empty()) {
            VkClearAttachment ca{VK_IMAGE_ASPECT_COLOR_BIT, 0, {}};
            std::memcpy(ca.clearValue.color.float32, col, 16);
            std::vector<VkClearRect> crs;
            for (const auto& r : rs) crs.push_back({r, 0, 1});
            vkCmdClearAttachments(CurFrame().cmd, 1, &ca, uint32_t(crs.size()), crs.data());
        } else if (mask != 0xF && !rs.empty()) {
            Target* ds_hint = ds ? ds : (in_rp_ && rp_rt_ == rt ? rp_ds_ : FindTarget(s.zs, true));
            ClearChannels(*rt, ds_hint, col, mask, rs);
        } else if (!rs.empty()) {
            if (EnsureRenderPass(rt, ds && ds->w == rt->w && ds->h == rt->h ? ds : nullptr)) {
                VkClearAttachment ca{VK_IMAGE_ASPECT_COLOR_BIT, 0, {}};
                std::memcpy(ca.clearValue.color.float32, col, 16);
                std::vector<VkClearRect> crs;
                for (const auto& r : rs) crs.push_back({r, 0, 1});
                vkCmdClearAttachments(CurFrame().cmd, 1, &ca, uint32_t(crs.size()), crs.data());
            }
        }
        rt->drawn = true;
    }

    if (ds) {
        float rev_z = 1.0f - std::clamp(z, 0.0f, 1.0f);
        std::vector<VkRect2D> rs = build_rects(*ds);
        bool full = (rs.size() == 1 && rs[0].offset.x == 0 && rs[0].offset.y == 0 &&
                     rs[0].extent.width == ds->w && rs[0].extent.height == ds->h);
        if (full && !in_rp_) {
            if (flags & kClearZ) {
                ds->pending_clear_z = true;
                ds->clear_z = rev_z;
            }
            if (flags & kClearStencil) {
                ds->pending_clear_s = true;
                ds->clear_s = stencil & 0xFFu;
            }
            ds->content_valid = true;
        } else if (!rs.empty() && EnsureRenderPass(in_rp_ ? rp_rt_ : rt, ds)) {
            VkImageAspectFlags asp = ((flags & kClearZ) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0u) |
                                     ((flags & kClearStencil) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0u);
            VkClearAttachment ca{asp, 0, {}};
            ca.clearValue.depthStencil = {rev_z, stencil & 0xFFu};
            std::vector<VkClearRect> crs;
            for (const auto& r : rs) crs.push_back({r, 0, 1});
            vkCmdClearAttachments(CurFrame().cmd, 1, &ca, uint32_t(crs.size()), crs.data());
        }
        ds->drawn = true;
    }
}

void VulkanBackend::UploadConstants(const State& s, const DrawShaders& sh, const PsoKey& k, const DrawTextures& stages,
                                    uint32_t gw, uint32_t gh, uint32_t& vc_off, uint32_t& dc_off) {
    if (last_vc_off_ == UINT32_MAX || std::memcmp(last_vsc_.data(), s.vsc.data(), sizeof(s.vsc))) {
        RingAlloc a = AllocConst(sizeof(s.vsc), 0);
        std::memcpy(a.cpu, s.vsc.data(), sizeof(s.vsc));
        std::memcpy(last_vsc_.data(), s.vsc.data(), sizeof(s.vsc));
        last_vc_off_ = uint32_t(a.offset);
    }
    vc_off = last_vc_off_;

    DrawConstants c{};
    c.surf[0] = float(gw);
    c.surf[1] = float(gh);
    c.surf[2] = 1.0f / s.zscale;
    if ((k.topo & 0xFu) == uint32_t(Topology::Points)) c.surf[3] = std::max(1.0f, cur_draw_rt_ ? float(cur_draw_rt_->w) / cur_draw_rt_->gw : scale_);
    Color(s.rs[kRsFogColor], c.fog_color);
    c.misc[0] = float(SimpleState(s, kMAlphaRef) & 0xFFu);
    c.misc[1] = SimpleState(s, kMAlphaTestEnable) ? float(CmpOp(SimpleState(s, kMAlphaFunc))) : 7.0f;
    c.misc[2] = float(CmpOp(s.rs[kRsShadowFunc]));
    c.misc[3] = s.rs[kRsSpecularEnable] ? 1.0f : 0.0f;

    if (s.rs[kRsFogEnable]) {
        if (sh.prog && s.rs[kRsFogTableMode] == 0) {
            c.fog[0] = 1.0f; c.fog[1] = 0.0f; c.fog[2] = 0.0f; c.fog[3] = 3.0f;
        } else {
            c.fog[0] = Bits(s.rs[kRsFogStart]);
            c.fog[1] = Bits(s.rs[kRsFogEnd]);
            c.fog[2] = Bits(s.rs[kRsFogDensity]);
            c.fog[3] = float(s.rs[kRsFogTableMode]);
        }
    }
    for (int i = 0; i < 8; ++i) {
        Color(SimpleState(s, kMPsConstant0 + i * 4), c.pc[i * 2]);
        Color(SimpleState(s, kMPsConstant1 + i * 4), c.pc[i * 2 + 1]);
    }
    Color(SimpleState(s, kMPsFinalConstant0), c.pc[16]);
    Color(SimpleState(s, kMPsFinalConstant1), c.pc[17]);
    Color(s.rs[kRsTextureFactor], c.tfactor);

    bool prog = sh.prog;
    uint32_t ds_w = cur_draw_ds_ ? cur_draw_ds_->w : 0, ds_gw = cur_draw_ds_ ? cur_draw_ds_->gw : 0;
    FillDepthConstants(s, prog, ds_w, ds_gw, c);
    c.zparams[3] = 1.0f;
    if (app::AndroidSettings().aspect_mode == 2 && ctx_ && cur_draw_rt_ &&
        cur_draw_rt_->gw == guest_w_ && cur_draw_rt_->gh == guest_h_ && s.rs[kRsZEnable] != 0) {
        const float disp_aspect = float(ctx_->Width()) / std::max(1.0f, float(ctx_->Height()));
        if (disp_aspect > (16.0f / 9.0f) + 0.02f) {
            c.zparams[3] = (16.0f / 9.0f) / disp_aspect;
        }
    }

    float rt_shift = 0.0f;
    if (!sh.prog || ReadsViewportOffset(s.vs->program)) {
        const Target* ref = cur_draw_rt_ ? cur_draw_rt_ : cur_draw_ds_;
        rt_shift = ref && ref->gw ? (-0.5f * float(ref->w) / float(ref->gw)) : (-0.5f * scale_);
    }

    for (int i = 0; i < 4; ++i) {
        TextureDesc d;
        if (s.textures[size_t(i)] && Describe(s.textures[size_t(i)], d)) {
            c.texsize[i][0] = float(d.width);
            c.texsize[i][1] = float(d.height);
            if (stages.rt[i] && stages.wide[i] > 0.0f) {
                float k_wide = stages.k[i];
                float n = std::ceil(3.0f * stages.wide[i] * 0.5f);
                c.spread[i][0] = 3.0f / (n * float(d.width) * k_wide);
                c.spread[i][1] = 3.0f / (n * float(d.height) * k_wide);
                c.spread[i][2] = n;
            }
        }
        if (stages.rt[i]) c.rtshift[i] = rt_shift;
        const auto& t = s.tss[size_t(i)];
        c.bump[i][0] = Bits(t[kTssBumpEnvMat00]);
        c.bump[i][1] = Bits(t[kTssBumpEnvMat01]);
        c.bump[i][2] = Bits(t[kTssBumpEnvMat10]);
        c.bump[i][3] = Bits(t[kTssBumpEnvMat11]);
        c.bumpl[i][0] = Bits(t[kTssBumpEnvLScale]);
        c.bumpl[i][1] = Bits(t[kTssBumpEnvLOffset]);
    }
    if (sh.fixed_transform) {
        FillTransformConstants(s, gw, gh, c);
        FillLightConstants(s, sh.fk, c);
    }

    if (last_dc_off_ == UINT32_MAX || std::memcmp(&last_dc_, &c, sizeof(c))) {
        RingAlloc a = AllocConst(sizeof(c), 0);
        std::memcpy(a.cpu, &c, sizeof(c));
        last_dc_ = c;
        last_dc_off_ = uint32_t(a.offset);
    }
    dc_off = last_dc_off_;
}

void VulkanBackend::Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) {
    if (SpriteOnStage3(s, prim)) {
        Draw(SpriteStageZero(s), prim, start, count, indices);
        return;
    }
    report_idle_polls_ = 0;
    const auto cpu_t0 = std::chrono::steady_clock::now();
    const uint64_t wait_before = sec_wait_ns_;

    total_draws_++;
    if (DepthCopy(s)) {
        sec_cpu_ns_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now() - cpu_t0)
                                    .count()) -
                       (sec_wait_ns_ - wait_before);
        return;
    }

    Assembled& a = asm_;
    if (!Assemble(s, prim, start, count, indices, a)) return;

    if (CurFrame().stream_used > kStreamRingSize - (2ull << 20) ||
        CurFrame().const_used > kConstRingSize - (256ull << 10)) {
        SubmitCurrent(false);
    }

    bool zs_sampled = false;
    if (s.zs.header) {
        uint32_t zs_va = mem::VirtualFromPhysical(s.zs.data);
        for (int i = 0; i < 4; ++i) {
            TextureDesc td;
            if (s.textures[size_t(i)] && Describe(s.textures[size_t(i)], td) &&
                Format(td.format).depth && td.data == zs_va) {
                zs_sampled = true;
                break;
            }
        }
    }

    Target* rt = nullptr;
    Target* ds = nullptr;
    if (!PrepareBindTargets(s, rt, ds, zs_sampled)) return;
    cur_draw_rt_ = rt;
    cur_draw_ds_ = ds;

    DrawShaders sh{};
    sh.prog = (s.vs_handle & 1u) && s.vs && !s.vs->program.empty();
    sh.point = (a.topology == Topology::Points);
    sh.quad = (count <= 6u && !s.rs[kRsZEnable] && !sh.point);
    sh.fixed_transform = !sh.prog && !a.pretransformed;
    sh.sprite = (sh.point && sh.prog && s.rs[kRsPointSpriteEnable]);
    if (!sh.prog) sh.fk = MakeFixedKey(s);

    if (sh.prog && !s.vs->hash) {
        s.vs->hash = Hash(s.vs->program.data(), s.vs->program.size() * 4u) | 1ull;
    }
    uint64_t seed = sh.prog ? (s.vs->hash ^ a.regs) : Hash(&s.vs_handle, 4, a.regs | 0x10000ull);
    sh.vkey = Hash(&a.decode, 8, seed);
    if (!sh.prog && a.pretransformed) sh.vkey ^= 0x5A5Aull;
    if (sh.quad) sh.vkey ^= 0xC3C30000ull;
    if (sh.point) sh.vkey ^= 0x1E1E0000ull;
    if (sh.sprite) sh.vkey ^= 0x3C3C0000ull;
    if (!sh.prog) sh.vkey = Hash(&sh.fk, sizeof(sh.fk), sh.vkey);

    if (auto it = vs_l1_.find(sh.vkey); it != vs_l1_.end()) {
        sh.vs = it->second;
    } else {
        sh.vs = ctx_->Shaders().GetOrCompile(sh.vkey, false, [&] {
            return sh.prog
                ? vk_sh::VertexProgramGlsl(s.vs->program, a.regs, a.decode, sh.quad, sh.point, sh.sprite,
                                           ctx_->MaxPointSize())
                : vk_sh::FixedVertexGlsl(a.regs, a.pretransformed, a.decode, sh.fk, sh.quad, sh.point, sh.sprite,
                                         ctx_->MaxPointSize());
        });
        if (sh.vs) vs_l1_[sh.vkey] = sh.vs;
    }
    if (!sh.vs) return;

    DrawTextures stages{};
    PrepareDrawTextures(s, stages, sh.prog);

    PixelKey pk = MakePixelKey(s, stages.info);
    pk.quad = sh.quad ? 1u : 0u;
    sh.pkey = Hash(&pk, sizeof(pk)) ^ (sh.sprite ? 0xAA55AA55ull : 0ull);
    if (auto it = fs_l1_.find(sh.pkey); it != fs_l1_.end()) {
        sh.fs = it->second;
    } else {
        sh.fs = ctx_->Shaders().GetOrCompile(sh.pkey, true, [&] { return vk_sh::PixelGlsl(pk, sh.sprite); });
        if (sh.fs) fs_l1_[sh.pkey] = sh.fs;
    }
    if (!sh.fs) return;

    bool clamp_uv = AnyStageWraps(s) && UnitTexcoords(a);
    VkSampler smps[4]{};
    for (int i = 0; i < 4; ++i) {
        smps[i] = GetStageSampler(s, i, stages, clamp_uv, !sh.prog, stages.linear_ok[i]);
    }
    VkDescriptorSet tex_set = GetOrCreateTexSet(stages.views, smps);
    if (!tex_set) return;

    if (!EnsureRenderPass(rt, ds)) return;

    // Build PSO key & bind pipeline
    PsoKey k{};
    k.vs = sh.vkey;
    k.ps = sh.pkey;
    k.regs = a.regs;
    for (int r = 0; r < 16; ++r) {
        if ((a.regs >> r) & 1u) {
            const VertexElement& e = a.elements[size_t(r)];
            k.elements[r] = e.format | (uint32_t(e.buffer) << 8) | (uint32_t(e.offset) << 16);
        }
    }
    for (uint32_t i = 0; i < a.buffer_count && i < 16; ++i) k.strides[i] = uint16_t(a.buffers[i].stride);
    k.topo = uint32_t(a.topology) | (a.strip ? 0x10u : 0u);
    k.has_rt = rt ? 1u : 0u;
    k.has_ds = ds ? 1u : 0u;
    k.blend = SimpleState(s, kMBlendEnable) != 0;
    if (k.blend) {
        k.src = SimpleState(s, kMSrcBlend);
        k.dst = SimpleState(s, kMDestBlend);
        k.op = SimpleState(s, kMBlendOp);
    }
    uint32_t cw = SimpleState(s, kMColorWriteEnable);
    k.wmask = ((cw >> 16) & 1u) | (((cw >> 8) & 1u) << 1) | ((cw & 1u) << 2) | (((cw >> 24) & 1u) << 3);
    if (ds) {
        k.zen = s.rs[kRsZEnable] != 0;
        if (k.zen) {
            k.zwrite = SimpleState(s, kMZWriteEnable) != 0;
            k.zfunc = SimpleState(s, kMZFunc);
        }
        k.sten = s.rs[kRsStencilEnable] != 0;
        if (k.sten) {
            k.sfunc = SimpleState(s, kMStencilFunc);
            k.smask = SimpleState(s, kMStencilMask) & 0xFFu;
            k.swmask = SimpleState(s, kMStencilWriteMask) & 0xFFu;
            k.sfail = s.rs[kRsStencilFail];
            k.szfail = SimpleState(s, kMStencilZFail);
            k.spass = SimpleState(s, kMStencilPass);
        }
        k.zbias_on = (a.topology == Topology::Triangles && SimpleState(s, kMSolidOffsetEnable)) ? 1u : 0u;
    }
    uint32_t cull = s.rs[kRsCullMode];
    k.cull = (cull != 0 && a.topology == Topology::Triangles) ? 1u : 0u;
    k.ccw = (cull == 0x900u) ? 1u : 0u;
    k.fill = s.rs[kRsFillMode];
    k.sprite = sh.sprite ? 1u : 0u;

    VkRenderPass compat_rp = compat_rps_[rt ? 1 : 0][ds ? 1 : 0];
    VkPipeline pso = GetDrawPipeline(k, a, sh.vs, sh.fs, compat_rp);
    if (!pso) return;

    VkCommandBuffer cmd = CurFrame().cmd;
    if (pso != bound_pso_) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pso);
        bound_pso_ = pso;
    }

    uint32_t gw = 0, gh = 0;
    SurfaceSize(rt ? s.rt : s.zs, gw, gh);
    uint32_t vc_off = 0, dc_off = 0;
    UploadConstants(s, sh, k, stages, gw, gh, vc_off, dc_off);

    if (!dyn_valid_ || vc_off != bound_vc_off_ || dc_off != bound_dc_off_) {
        uint32_t dyn[2] = {vc_off, dc_off};
        VkDescriptorSet ubo_set = CurFrame().ubo_set;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_layout_, 0, 1, &ubo_set, 2, dyn);
        bound_vc_off_ = vc_off;
        bound_dc_off_ = dc_off;
    }
    if (!dyn_valid_ || tex_set != bound_tex_set_) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_layout_, 1, 1, &tex_set, 0, nullptr);
        bound_tex_set_ = tex_set;
    }

    // Dynamic viewport (negative height -> top-left matches D3D8), scissor, blend constants, stencil ref, depth bias
    const float W = float(rt ? rt->w : ds->w), H = float(rt ? rt->h : ds->h);
    VkViewport vp{0.0f, H, W, -H, 0.0f, 1.0f};
    float sc_f = W / float(gw);
    int32_t x0 = int32_t(s.viewport.x * sc_f), y0 = int32_t(s.viewport.y * sc_f);
    int32_t x1 = int32_t((s.viewport.x + s.viewport.w) * sc_f), y1 = int32_t((s.viewport.y + s.viewport.h) * sc_f);
    if (s.scissor_count) {
        x0 = std::max(x0, int32_t(s.scissors[0] * sc_f));
        y0 = std::max(y0, int32_t(s.scissors[1] * sc_f));
        x1 = std::min(x1, int32_t(s.scissors[2] * sc_f));
        y1 = std::min(y1, int32_t(s.scissors[3] * sc_f));
    }
    x0 = std::clamp(x0, 0, int32_t(W)); y0 = std::clamp(y0, 0, int32_t(H));
    x1 = std::clamp(x1, 0, int32_t(W)); y1 = std::clamp(y1, 0, int32_t(H));
    VkRect2D scissor{{x0, y0}, {uint32_t(std::max(0, x1 - x0)), uint32_t(std::max(0, y1 - y0))}};

    if (!dyn_valid_ || std::memcmp(&vp, &bound_vp_, sizeof(vp)) != 0) {
        vkCmdSetViewport(cmd, 0, 1, &vp);
        bound_vp_ = vp;
    }
    if (!dyn_valid_ || std::memcmp(&scissor, &bound_sc_, sizeof(scissor)) != 0) {
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        bound_sc_ = scissor;
    }

    float bf[4];
    Color(SimpleState(s, kMBlendColor), bf);
    if (!dyn_valid_ || std::memcmp(bf, bound_bf_, sizeof(bf)) != 0) {
        vkCmdSetBlendConstants(cmd, bf);
        std::memcpy(bound_bf_, bf, sizeof(bf));
    }
    uint32_t sref = SimpleState(s, kMStencilRef) & 0xFFu;
    if (!dyn_valid_ || sref != bound_sref_) {
        vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, sref);
        bound_sref_ = sref;
    }
    float slope = k.zbias_on && ds ? (-Bits(SimpleState(s, kMSlopeScale)) * float(ds->w) / float(ds->gw)) : 0.0f;
    if (!dyn_valid_ || slope != bound_slope_) {
        vkCmdSetDepthBias(cmd, 0.0f, 0.0f, slope);
        bound_slope_ = slope;
    }
    dyn_valid_ = true;

    // Vertex / Index upload (dedup raw immutable slices within the current frame)
    if (a.buffer_count > 0) {
        VkBuffer vbufs[16]{};
        VkDeviceSize voffs[16]{};
        Frame& cf = CurFrame();
        const bool can_cache_vb = a.vertices.empty();
        for (uint32_t i = 0; i < a.buffer_count; ++i) {
            const uint32_t raw_bytes = a.buffers[i].bytes;
            const uint32_t bytes = std::max(raw_bytes, 16u);
            if (can_cache_vb && raw_bytes > 0 && a.buffers[i].src &&
                cf.stream_used + bytes + 64 <= kStreamRingSize) {
                uint64_t vh = uint64_t(uintptr_t(a.buffers[i].src)) ^ (uint64_t(raw_bytes) << 33);
                if (raw_bytes <= 65536u) {
                    vh = Hash(a.buffers[i].src, raw_bytes, vh);
                } else {
                    const uint8_t* p = a.buffers[i].src;
                    for (uint32_t si = 0; si < 16u; ++si) {
                        uint32_t so = uint32_t(uint64_t(raw_bytes - 64u) * si / 15u);
                        vh = Hash(p + so, 64u, vh);
                    }
                }
                auto vbit = cf.vb_cache.find(vh);
                if (vbit != cf.vb_cache.end()) {
                    vbufs[i] = cf.stream_buf;
                    voffs[i] = vbit->second;
                    continue;
                }
                RingAlloc va = AllocStream(bytes, 16);
                std::memcpy(va.cpu, a.buffers[i].src, raw_bytes);
                if (va.buf == cf.stream_buf) cf.vb_cache[vh] = va.offset;
                vbufs[i] = va.buf;
                voffs[i] = va.offset;
            } else {
                RingAlloc va = AllocStream(bytes, 16);
                if (raw_bytes && a.buffers[i].src) std::memcpy(va.cpu, a.buffers[i].src, raw_bytes);
                vbufs[i] = va.buf;
                voffs[i] = va.offset;
            }
        }
        vkCmdBindVertexBuffers(cmd, 0, a.buffer_count, vbufs, voffs);
    }

    uint32_t q = kNoQuery;
    if (s.simple[kMCountEnable >> 2] && query_pool_) {
        const uint32_t per_frame = kQueries / kFrames;
        const uint32_t q_base = uint32_t(frame_idx_ % kFrames) * per_frame;
        if (query_next_ < q_base + per_frame) {
            q = query_next_++;
            vkCmdBeginQuery(cmd, query_pool_, q, 0);
        }
    }

    if (a.index_count) {
        uint32_t ibytes = a.index_count * (a.index16 ? 2u : 4u);
        RingAlloc ia = AllocStream(ibytes, 4);
        std::memcpy(ia.cpu, a.index_src, ibytes);
        vkCmdBindIndexBuffer(cmd, ia.buf, ia.offset, a.index16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, a.index_count, 1, 0, a.base_vertex, 0);
    } else {
        vkCmdDraw(cmd, a.draw_count, 1, 0, 0);
    }

    if (q != kNoQuery) {
        vkCmdEndQuery(cmd, query_pool_, q);
        float area = 1.0f;
        const Target* ref = rt ? rt : ds;
        if (ref && ref->gw && ref->gh) area = float(ref->w) * float(ref->h) / (float(ref->gw) * float(ref->gh));
        counted_.push_back({q, area});
    }

    sec_draws_++;
    sec_cpu_ns_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - cpu_t0)
                                .count()) -
                   (sec_wait_ns_ - wait_before);
}

void VulkanBackend::Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects_va,
                         uint32_t dst_points_va, float src_kx, float src_ky) {
    Target* src_t = FindTarget(src, false);
    if (!src_t || !src_t->drawn) {
        if (CopySurfaceMemory(src, dst, count, src_rects_va, dst_points_va)) {
            if (Target* dst_t = FindTarget(dst, false)) UploadTarget(*dst_t, dst);
        }
        return;
    }
    if (LatchCpuWrite(*src_t)) UploadTarget(*src_t, src);
    Target* dst_t = EnsureTarget(dst, false);
    if (!dst_t || src_t == dst_t) return;

    EndRenderPass(RpEndReason::Copy);
    VkCommandBuffer cmd = CurFrame().cmd;

    bool same_scale = (src_kx == 1.0f && src_ky == 1.0f && src_t->fmt == dst_t->fmt &&
                       src_t->w == dst_t->w && src_t->h == dst_t->h);
    uint32_t n = (count && src_rects_va) ? count : 1u;

    if (same_scale) {
        TransitionImageRaw(cmd, src_t->image, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1,
                           src_t->layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        TransitionImageRaw(cmd, dst_t->image, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1,
                           dst_t->layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        float sc = float(dst_t->w) / float(dst_t->gw);
        for (uint32_t i = 0; i < n; ++i) {
            int32_t x0 = 0, y0 = 0, x1 = int32_t(src_t->gw), y1 = int32_t(src_t->gh), dx = 0, dy = 0;
            if (count && src_rects_va) {
                uint32_t r = src_rects_va + i * 16u;
                x0 = int32_t(kernel::R32(r)); y0 = int32_t(kernel::R32(r + 4));
                x1 = int32_t(kernel::R32(r + 8)); y1 = int32_t(kernel::R32(r + 12));
                dx = x0; dy = y0;
                if (dst_points_va) {
                    uint32_t p = dst_points_va + i * 8u;
                    dx = int32_t(kernel::R32(p)); dy = int32_t(kernel::R32(p + 4));
                }
            }
            uint32_t sx = uint32_t(std::clamp(int32_t(x0 * sc), 0, int32_t(src_t->w)));
            uint32_t sy = uint32_t(std::clamp(int32_t(y0 * sc), 0, int32_t(src_t->h)));
            uint32_t ex = uint32_t(std::clamp(int32_t(x1 * sc), 0, int32_t(src_t->w)));
            uint32_t ey = uint32_t(std::clamp(int32_t(y1 * sc), 0, int32_t(src_t->h)));
            uint32_t ox = uint32_t(std::clamp(int32_t(dx * sc), 0, int32_t(dst_t->w)));
            uint32_t oy = uint32_t(std::clamp(int32_t(dy * sc), 0, int32_t(dst_t->h)));
            if (ex <= sx || ey <= sy) continue;
            uint32_t cw = std::min(ex - sx, dst_t->w - ox);
            uint32_t ch = std::min(ey - sy, dst_t->h - oy);
            if (!cw || !ch) continue;
            VkImageCopy c{};
            c.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            c.srcOffset = {int32_t(sx), int32_t(sy), 0};
            c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            c.dstOffset = {int32_t(ox), int32_t(oy), 0};
            c.extent = {cw, ch, 1};
            vkCmdCopyImage(cmd, src_t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           dst_t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        }
        dst_t->content_valid = true;
        dst_t->drawn = true;
        return;
    }

    // Scaled / format-converted Copy by fullscreen blit
    TransitionImageRaw(cmd, src_t->image, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1,
                       src_t->layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (!EnsureRenderPass(dst_t, nullptr)) return;
    VkShaderModule vs = ctx_->Shaders().GetOrCompile(Hash("util_vs", 7), false, [] { return vk_sh::FullscreenVsGlsl(); });
    VkShaderModule fs = ctx_->Shaders().GetOrCompile(Hash("util_blit", 9), true, [] { return vk_sh::BlitPsGlsl(); });
    VkPipeline pipe = GetUtilPipeline("blit", vs, fs, dst_t->fmt, false, 0xF, false, compat_rps_[1][0]);

    float gw = src_t->gw * src_kx, gh = src_t->gh * src_ky;
    float src_sc = float(src_t->w) / gw, dst_sc = float(dst_t->w) / float(dst_t->gw);
    float taps = std::ceil(std::max(src_sc / dst_sc, 1.0f) - 1e-3f);

    for (uint32_t i = 0; i < n; ++i) {
        float x0 = 0, y0 = 0, x1 = std::min(gw, float(dst_t->gw)), y1 = std::min(gh, float(dst_t->gh)), dx = 0, dy = 0;
        if (count && src_rects_va) {
            uint32_t r = src_rects_va + i * 16u;
            x0 = float(int32_t(kernel::R32(r))); y0 = float(int32_t(kernel::R32(r + 4)));
            x1 = float(int32_t(kernel::R32(r + 8))); y1 = float(int32_t(kernel::R32(r + 12)));
            dx = x0; dy = y0;
            if (dst_points_va) {
                uint32_t p = dst_points_va + i * 8u;
                dx = float(int32_t(kernel::R32(p))); dy = float(int32_t(kernel::R32(p + 4)));
            }
        }
        if (x1 <= x0 || y1 <= y0) continue;
        float vx = dx * dst_sc, vy = dy * dst_sc, vw = (x1 - x0) * dst_sc, vh = (y1 - y0) * dst_sc;
        int32_t sx0 = std::clamp(int32_t(vx), 0, int32_t(dst_t->w));
        int32_t sy0 = std::clamp(int32_t(vy), 0, int32_t(dst_t->h));
        int32_t sx1 = std::clamp(int32_t(vx + vw), 0, int32_t(dst_t->w));
        int32_t sy1 = std::clamp(int32_t(vy + vh), 0, int32_t(dst_t->h));
        if (sx1 <= sx0 || sy1 <= sy0) continue;

        float dc[8] = {x0 / gw, y0 / gh, (x1 - x0) / gw, (y1 - y0) / gh, 1.0f, taps, 1.0f, 0.0f};
        RingAlloc dca = AllocConst(32, 0);
        std::memcpy(dca.cpu, dc, 32);
        VkImageView views[4] = {src_t->sample_view, dummy_2d_view_, dummy_2d_view_, dummy_2d_view_};
        VkSampler smps[4] = {blit_linear_, blit_linear_, blit_linear_, blit_linear_};
        VkDescriptorSet tex_set = GetOrCreateTexSet(views, smps);
        if (!tex_set) continue;
        VkViewport vp{vx, vy, vw, vh, 0.0f, 1.0f};
        VkRect2D sc{{sx0, sy0}, {uint32_t(sx1 - sx0), uint32_t(sy1 - sy0)}};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        VkDescriptorSet ubo_set = CurFrame().ubo_set;
        uint32_t dyn[2] = {0, uint32_t(dca.offset)};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_layout_, 0, 1, &ubo_set, 2, dyn);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_layout_, 1, 1, &tex_set, 0, nullptr);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }
    dst_t->drawn = true;
    EndRenderPass(RpEndReason::Copy);
}

Target* VulkanBackend::ShrinkTarget(Target& src, uint32_t w, uint32_t h, float mul, bool keep_copy,
                                    VkImage* reuse_img, VkDeviceMemory* reuse_mem,
                                    VkImageView* reuse_view, VkImageLayout* reuse_layout) {
    (void)src; (void)w; (void)h; (void)mul; (void)keep_copy;
    (void)reuse_img; (void)reuse_mem; (void)reuse_view; (void)reuse_layout;
    return nullptr;
}

bool VulkanBackend::ReadBackImage(VkImage src_img, VkImageLayout& layout, uint32_t w, uint32_t h,
                                  std::vector<uint8_t>& out_bgra) {
    EndRenderPass(RpEndReason::Sub);
    VkCommandBuffer cmd = CurFrame().cmd;
    TransitionImageRaw(cmd, src_img, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    uint64_t bytes = uint64_t(w) * h * 4u;
    VkBuffer rb = VK_NULL_HANDLE;
    VkDeviceMemory rb_mem = VK_NULL_HANDLE;
    void* p = nullptr;
    if (!CreateBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 0,
                      rb, rb_mem, &p)) {
        if (!CreateBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0,
                          rb, rb_mem, &p)) {
            return false;
        }
    }

    VkBufferImageCopy c{};
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(cmd, src_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb, 1, &c);
    sec_syncs_++;
    SubmitCurrent(true, &sec_wait_sync_ns_);

    VkMappedMemoryRange mr{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    mr.memory = rb_mem;
    mr.offset = 0;
    mr.size = VK_WHOLE_SIZE;
    vkInvalidateMappedMemoryRanges(dev_, 1, &mr);

    out_bgra.resize(bytes);
    vk_tex::ConvertBGRA8ToRGBA8(static_cast<const uint8_t*>(p), out_bgra.data(), w * h);
    vkUnmapMemory(dev_, rb_mem);
    vkDestroyBuffer(dev_, rb, nullptr);
    vkFreeMemory(dev_, rb_mem, nullptr);
    return true;
}

void VulkanBackend::Sync(uint32_t data_va, uint32_t) {
    auto it = targets_.find(uint64_t(data_va) << 1);
    if (it == targets_.end() || !it->second.drawn || TargetStale(data_va)) return;
    Target& t = it->second;

    VkImage read_img = t.image;
    VkDeviceMemory temp_mem = VK_NULL_HANDLE;
    VkImage temp_img = VK_NULL_HANDLE;
    VkImageView temp_view = VK_NULL_HANDLE;
    VkImageLayout temp_layout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (t.w != t.gw || t.h != t.gh) {
        EndRenderPass();
        if (!CreateImage2D(t.gw, t.gh, 1, 1, VK_IMAGE_TYPE_2D, 0, VK_FORMAT_R8G8B8A8_UNORM,
                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, temp_img, temp_mem)) return;
        VkImageViewCreateInfo iv{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        iv.image = temp_img; iv.viewType = VK_IMAGE_VIEW_TYPE_2D; iv.format = VK_FORMAT_R8G8B8A8_UNORM;
        iv.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(dev_, &iv, nullptr, &temp_view);

        Target dummy_t;
        dummy_t.w = t.gw; dummy_t.h = t.gh; dummy_t.fmt = VK_FORMAT_R8G8B8A8_UNORM;
        dummy_t.image = temp_img; dummy_t.attach_view = temp_view; dummy_t.layout = temp_layout;
        TransitionImageRaw(CurFrame().cmd, t.image, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1,
                           t.layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if (EnsureRenderPass(&dummy_t, nullptr)) {
            VkShaderModule vs = ctx_->Shaders().GetOrCompile(Hash("util_vs", 7), false, [] { return vk_sh::FullscreenVsGlsl(); });
            VkShaderModule fs = ctx_->Shaders().GetOrCompile(Hash("util_blit", 9), true, [] { return vk_sh::BlitPsGlsl(); });
            VkPipeline pipe = GetUtilPipeline("blit", vs, fs, dummy_t.fmt, false, 0xF, false, compat_rps_[1][0]);
            float dc[8] = {0, 0, 1, 1, 1.0f, 1.0f, 1.0f, 0};
            DrawFullscreenUtil(pipe, dc, t.sample_view, blit_linear_, {t.gw, t.gh});
            EndRenderPass();
        }
        temp_layout = dummy_t.layout;
        read_img = temp_img;
    }

    std::vector<uint8_t> px;
    bool ok = ReadBackImage(read_img, (read_img == temp_img) ? temp_layout : t.layout, t.gw, t.gh, px);
    if (temp_view) {
        uint64_t kd[5] = {reinterpret_cast<uint64_t>(temp_view), 0,
                          uint64_t(t.gw) | (uint64_t(t.gh) << 32), 1, 0};
        if (auto fit = framebuffers_.find(Hash(kd, sizeof(kd))); fit != framebuffers_.end()) {
            vkDestroyFramebuffer(dev_, fit->second, nullptr);
            framebuffers_.erase(fit);
        }
        vkDestroyImageView(dev_, temp_view, nullptr);
    }
    if (temp_img) vkDestroyImage(dev_, temp_img, nullptr);
    if (temp_mem) vkFreeMemory(dev_, temp_mem, nullptr);
    if (!ok) return;

    uint32_t pitch = GuestPitch(t.gw);
    uint8_t* guest = static_cast<uint8_t*>(kernel::HostPtr(data_va));
    for (uint32_t y = 0; y < t.gh; ++y) std::memcpy(guest + size_t(y) * pitch, &px[size_t(y) * t.gw * 4u], t.gw * 4u);
    t.read = std::move(px);
    t.drawn = false;
    t.sig = MemSig(t.data, t.bytes);
}

void VulkanBackend::SyncLate(uint32_t data_va) {
    if (!DeliverLate(data_va)) Sync(data_va, 0);
    RecordLate(data_va);
}

bool VulkanBackend::DeliverLate(uint32_t data_va) {
    PollCompletedSerial();
    std::lock_guard<std::mutex> lk(late_lock_);
    auto it = late_.find(data_va);
    if (it == late_.end()) return false;
    Late& l = it->second;
    uint64_t done = completed_serial_.load(std::memory_order_acquire);
    uint64_t now = presents_seen_.load(std::memory_order_acquire);
    int best = -1;
    for (int i = 0; i < kFrames; ++i) {
        if (l.serial[i] && l.serial[i] <= done && (now - l.frame[i]) <= uint64_t(kFrames) + 1) {
            if (best < 0 || l.serial[i] > l.serial[best]) best = i;
        }
    }
    if (best < 0) return false;
    if (l.serial[best] > l.delivered) {
        VkMappedMemoryRange mr{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        mr.memory = l.rb_mem[best];
        mr.offset = 0;
        mr.size = VK_WHOLE_SIZE;
        vkInvalidateMappedMemoryRanges(dev_, 1, &mr);

        uint32_t pitch = GuestPitch(l.gw);
        uint8_t* dst = static_cast<uint8_t*>(kernel::HostPtr(data_va));
        late_row_tmp_.resize(size_t(l.gw) * 4u);
        for (uint32_t y = 0; y < l.gh; ++y) {
            vk_tex::ConvertBGRA8ToRGBA8(l.rb_map[best] + size_t(y) * l.gw * 4u, late_row_tmp_.data(), l.gw);
            std::memcpy(dst + size_t(y) * pitch, late_row_tmp_.data(), l.gw * 4u);
        }
        l.delivered = l.serial[best];
    }
    return true;
}

void VulkanBackend::RecordLate(uint32_t data_va) {
    auto it = targets_.find(uint64_t(data_va) << 1);
    if (it == targets_.end() || TargetStale(data_va)) return;
    Target& t = it->second;
    t.sig = MemSig(t.data, t.bytes);
    t.read.clear();

    std::lock_guard<std::mutex> lk(late_lock_);
    Late& l = late_[data_va];
    if (l.gw != t.gw || l.gh != t.gh) {
        for (int i = 0; i < kFrames; ++i) {
            if (l.rb_map[i]) { vkUnmapMemory(dev_, l.rb_mem[i]); l.rb_map[i] = nullptr; }
            Defer(GarbageItem::Kind::Buffer, l.rb[i]);
            Defer(GarbageItem::Kind::Memory, l.rb_mem[i]);
        }
        Defer(GarbageItem::Kind::ImageView, l.down_view);
        Defer(GarbageItem::Kind::Image, l.down);
        Defer(GarbageItem::Kind::Memory, l.down_mem);
        l = Late{};
        l.gw = t.gw; l.gh = t.gh;
    }

    uint32_t slot = l.next;
    PollCompletedSerial();
    if (l.serial[slot] > completed_serial_.load(std::memory_order_acquire)) return;

    sec_lates_++;
    EndRenderPass(RpEndReason::Late);
    VkCommandBuffer cmd = CurFrame().cmd;
    VkImage src_img = t.image;
    if (t.w != t.gw || t.h != t.gh) {
        if (!l.down) {
            if (!CreateImage2D(t.gw, t.gh, 1, 1, VK_IMAGE_TYPE_2D, 0, VK_FORMAT_R8G8B8A8_UNORM,
                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, l.down, l.down_mem)) return;
            VkImageViewCreateInfo iv{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            iv.image = l.down; iv.viewType = VK_IMAGE_VIEW_TYPE_2D; iv.format = VK_FORMAT_R8G8B8A8_UNORM;
            iv.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCreateImageView(dev_, &iv, nullptr, &l.down_view);
            l.down_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        }
        Target dummy_t;
        dummy_t.w = l.gw; dummy_t.h = l.gh; dummy_t.fmt = VK_FORMAT_R8G8B8A8_UNORM;
        dummy_t.image = l.down; dummy_t.attach_view = l.down_view; dummy_t.layout = l.down_layout;
        TransitionImageRaw(cmd, t.image, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1,
                           t.layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if (EnsureRenderPass(&dummy_t, nullptr)) {
            VkShaderModule vs = ctx_->Shaders().GetOrCompile(Hash("util_vs", 7), false, [] { return vk_sh::FullscreenVsGlsl(); });
            VkShaderModule fs = ctx_->Shaders().GetOrCompile(Hash("util_blit", 9), true, [] { return vk_sh::BlitPsGlsl(); });
            VkPipeline pipe = GetUtilPipeline("blit", vs, fs, dummy_t.fmt, false, 0xF, false, compat_rps_[1][0]);
            float dc[8] = {0, 0, 1, 1, 1.0f, 1.0f, 1.0f, 0};
            DrawFullscreenUtil(pipe, dc, t.sample_view, blit_linear_, {l.gw, l.gh});
            EndRenderPass(RpEndReason::Late);
        }
        l.down_layout = dummy_t.layout;
        TransitionImageRaw(cmd, l.down, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1,
                           l.down_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        src_img = l.down;
    } else {
        TransitionImageRaw(cmd, t.image, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1,
                           t.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    }

    uint64_t bytes = uint64_t(l.gw) * l.gh * 4u;
    if (!l.rb[slot]) {
        if (!CreateBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 0,
                          l.rb[slot], l.rb_mem[slot], reinterpret_cast<void**>(&l.rb_map[slot]))) {
            return;
        }
    }
    VkBufferImageCopy bic{};
    bic.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bic.imageExtent = {l.gw, l.gh, 1};
    vkCmdCopyImageToBuffer(cmd, src_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, l.rb[slot], 1, &bic);

    l.serial[slot] = next_serial_;
    l.frame[slot] = presents_;
    l.next = (slot + 1) % kFrames;
}

void VulkanBackend::Flush() {
    if (!CurFrame().cmd) return;
    sec_flushes_++;
    SubmitCurrent(true, &sec_wait_flush_ns_);
}

void VulkanBackend::SetGamma(const uint8_t* ramp) {
    std::memcpy(ramp_, ramp, sizeof(ramp_));
    bool identity = true;
    for (int i = 0; i < 256; ++i) {
        if (ramp_[i] != i || ramp_[256 + i] != i || ramp_[512 + i] != i) {
            identity = false;
            break;
        }
    }
    gamma_enabled_ = !identity;
    ramp_dirty_ = true;
}

void VulkanBackend::ClearReport() {
    counted_.clear();
}

void VulkanBackend::GetReport(uint32_t report_va) {
    if (!report_va) return;
    if (counted_.empty()) {
        kernel::W32(report_va + 0u, uint32_t(presents_));
        kernel::W32(report_va + 4u, 0);
        kernel::W32(report_va + 8u, 0);
        std::atomic_thread_fence(std::memory_order_release);
        kernel::W32(report_va + 12u, 0);
        return;
    }
    reports_.push_back({report_va, std::move(counted_), next_serial_});
    counted_.clear();
    pending_reports_.store(uint32_t(reports_.size()), std::memory_order_release);
    report_idle_polls_ = 0;
}

void VulkanBackend::WaitGpuFrame(uint64_t n) {
    const uint64_t want = present_serial_[n % (kFrames + 1)].load(std::memory_order_acquire);
    if (want == 0 || completed_serial_.load(std::memory_order_acquire) >= want) return;
    auto t0 = std::chrono::steady_clock::now();
    for (Frame& f : frames_) {
        WaitFrameIfNeeded(f, want);
    }
    uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now() - t0)
                               .count());
    sec_wait_gpuf_ns_.fetch_add(ns, std::memory_order_relaxed);
}

void VulkanBackend::ReleaseOldTextures(bool aggressive) {
    if (!aggressive) {
        if ((presents_ % 60u) != 0) return;
        if (textures_.size() <= kMaxCachedTex && tex_vram_bytes_ <= kMaxTexVramBytes) return;
    } else {
        Flush();
        ctx_->WaitIdle();
    }
    const uint64_t stale_gap = aggressive ? 4u : kIdleTexFrames;
    size_t freed = 0;
    for (auto it = textures_.begin(); it != textures_.end();) {
        if (it->second.image != VK_NULL_HANDLE && it->second.frame + stale_gap < presents_) {
            tex_vram_bytes_ = (tex_vram_bytes_ > it->second.vram_bytes) ? (tex_vram_bytes_ - it->second.vram_bytes) : 0;
            DeferView(it->second.view);
            Defer(GarbageItem::Kind::Image, it->second.image);
            DeferSlab(it->second.slab_alloc);
            Defer(GarbageItem::Kind::Memory, it->second.mem);
            it = textures_.erase(it);
            ++freed;
        } else {
            ++it;
        }
    }
    if (aggressive) {
        for (Frame& f : frames_) CollectGarbage(f);
        LOGR("vk_backend: aggressive VRAM eviction freed %zu textures (remaining=%zu, vram=%zuMB)",
             freed, textures_.size(), tex_vram_bytes_ / (1024u * 1024u));
    }
}

void VulkanBackend::ApplySettings() {
    const auto& ex = app::AndroidSettings();
    const uint32_t fps_target = app::Get().experimental_60fps ? app::kExperimentalFps : 30u;
    adpf::UpdateTargetFps(fps_target);

    adpf::ThermalInfo th = adpf::PollThermal(app::CachedBatteryTempC());
    last_thermal_status_ = th.thermal_status;

    float req_scale = app::RenderScale(guest_h_);
    float want = adpf::ComputeEffectiveScale(req_scale, ex.thermal_drs, last_thermal_status_, fps_val_, fps_target);
    bool want_aux = ex.native_aux_rt;
    bool want_half_aux = ex.half_aux_rt;

    if (std::fabs(want - scale_) > 1e-4f || want_aux != native_aux_rt_ || want_half_aux != half_aux_rt_) {
        LOGR("vk_backend: reconfiguring targets scale %.2fx -> %.2fx aux_rt=%d half=%d (therm=%d fps=%.1f)",
             scale_, want, int(want_aux), int(want_half_aux), last_thermal_status_, fps_val_);
        Flush();
        ctx_->WaitIdle();
        ctx_->InvalidatePublished();
        cur_draw_rt_ = cur_draw_ds_ = rp_rt_ = rp_ds_ = nullptr;
        for (auto& [k, t] : targets_) ReleaseTarget(t);
        targets_.clear();
        CollectGarbage(CurFrame());
        scale_ = want;
        native_aux_rt_ = want_aux;
        half_aux_rt_ = want_half_aux;
    }

    const bool want_low_vram = ex.low_vram_tex;
    if (want_low_vram != low_vram_tex_) {
        LOGR("vk_backend: switching low_vram_tex %d -> %d (clearing texture cache)", int(low_vram_tex_), int(want_low_vram));
        Flush();
        ctx_->WaitIdle();
        for (auto& [tk, tx] : textures_) {
            if (tx.view) vkDestroyImageView(dev_, tx.view, nullptr);
            if (tx.image) vkDestroyImage(dev_, tx.image, nullptr);
            tex_slab_.Free(tx.slab_alloc);
            if (tx.mem) vkFreeMemory(dev_, tx.mem, nullptr);
        }
        textures_.clear();
        tex_vram_bytes_ = 0;
        tex_desc_cache_.clear();
        vkResetDescriptorPool(dev_, tex_desc_pool_, 0);
        bound_tex_set_ = VK_NULL_HANDLE;
        for (Frame& fr : frames_) CollectGarbage(fr);
        low_vram_tex_ = want_low_vram;
    }
}

void VulkanBackend::DeliverCompletedReports() {
    PollCompletedSerial();
    uint64_t done = completed_serial_.load(std::memory_order_acquire);
    while (!reports_.empty() && reports_.front().serial <= done) {
        const Report& r = reports_.front();
        double pixels = 0.0;
        if (query_pool_) {
            for (const Query& q : r.queries) {
                uint64_t count = 0;
                if (vkGetQueryPoolResults(dev_, query_pool_, q.index, 1, sizeof(count), &count, sizeof(count),
                                          VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS) {
                    pixels += double(count) / q.samples_per_pixel;
                }
            }
        }
        kernel::W32(r.va + 0u, uint32_t(presents_));
        kernel::W32(r.va + 4u, 0);
        kernel::W32(r.va + 8u, uint32_t(std::lround(pixels)));
        std::atomic_thread_fence(std::memory_order_release);
        kernel::W32(r.va + 12u, 0);
        reports_.pop_front();
    }
    pending_reports_.store(uint32_t(reports_.size()), std::memory_order_release);
    if (reports_.empty()) report_idle_polls_ = 0;
}

void VulkanBackend::PollReports() {
    DeliverCompletedReports();
    if (presents_ > 0 && !stall_logged_) {
        auto stalled_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - last_present_tp_)
                              .count();
        if (stalled_ms >= 5000) {
            stall_logged_ = true;
            LOGE("vk_backend: Present stalled %lld ms (presents=%llu reports=%zu tex=%zu tdesc=%zu const=%llu/%llu gamma=%d)",
                 static_cast<long long>(stalled_ms),
                 static_cast<unsigned long long>(presents_),
                 reports_.size(), textures_.size(), tex_desc_cache_.size(),
                 static_cast<unsigned long long>(CurFrame().const_used),
                 static_cast<unsigned long long>(kConstRingSize),
                 int(gamma_enabled_));
        }
    }
    if (reports_.empty()) {
        report_idle_polls_ = 0;
        return;
    }
    // If guest is idle/spinning on a report still sitting in the unsubmitted command buffer,
    // kick it to the GPU after ~4 ms of queue idleness so it can never deadlock.
    if (reports_.front().serial == next_serial_ && CurFrame().cmd != VK_NULL_HANDLE) {
        if (++report_idle_polls_ >= 4u) {
            report_idle_polls_ = 0;
            SubmitCurrent(false);
        }
    } else {
        report_idle_polls_ = 0;
    }
}

void VulkanBackend::Present(const Surface& front) {
    cpu_affinity::PollRenderThreadPin();
    report_idle_polls_ = 0;
    if (!dev_ || !ctx_) return;
    if (CurFrame().pool == VK_NULL_HANDLE) return;
    const auto p_t0 = std::chrono::steady_clock::now();
    const uint64_t wait_before = sec_wait_ns_;
    if (CurFrame().cmd == VK_NULL_HANDLE) BeginCommandBuffer();

    Target* t = EnsureTarget(front, false);
    if (t && LatchCpuWrite(*t)) UploadTarget(*t, front);
    EndRenderPass(RpEndReason::Sub);

    int pub_slot = -1;
    vk::MailboxFrame* mb = nullptr;
    if (t) {
        pub_slot = ctx_->BeginPublishSlot(t->w, t->h, mb);
        if (pub_slot >= 0 && mb && mb->image != VK_NULL_HANDLE) {
            VkCommandBuffer cmd = CurFrame().cmd;
            TransitionImageRaw(cmd, t->image, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1,
                               t->layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            VkImageLayout dst_l = VK_IMAGE_LAYOUT_UNDEFINED;
            TransitionImageRaw(cmd, mb->image, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1,
                               dst_l, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkImageCopy cpy{};
            cpy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            cpy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            cpy.extent = {t->w, t->h, 1};
            vkCmdCopyImage(cmd, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           mb->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cpy);
            TransitionImageRaw(cmd, mb->image, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1,
                               dst_l, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

            if (gamma_enabled_ && mb->gamma_image != VK_NULL_HANDLE) {
                RingAlloc ga = AllocStream(256u * 4u, 16);
                for (int i = 0; i < 256; ++i) {
                    ga.cpu[i * 4 + 0] = ramp_[i];
                    ga.cpu[i * 4 + 1] = ramp_[256 + i];
                    ga.cpu[i * 4 + 2] = ramp_[512 + i];
                    ga.cpu[i * 4 + 3] = 255;
                }
                VkImageLayout gl = VK_IMAGE_LAYOUT_UNDEFINED;
                TransitionImageRaw(cmd, mb->gamma_image, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1,
                                   gl, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                VkBufferImageCopy gbic{};
                gbic.bufferOffset = ga.offset;
                gbic.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                gbic.imageExtent = {256, 1, 1};
                vkCmdCopyBufferToImage(cmd, ga.buf, mb->gamma_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &gbic);
                TransitionImageRaw(cmd, mb->gamma_image, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1,
                                   gl, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            }
        }
    }

    SubmitCurrent(false);
    const uint64_t submitted_serial = next_serial_ - 1;
    present_serial_[presents_ % (kFrames + 1)].store(submitted_serial, std::memory_order_release);

    if (pub_slot >= 0) {
        ctx_->EndPublishSlot(pub_slot, gamma_enabled_, presents_ + 1);
    }

    DeliverCompletedReports();

    // Reset ended_mid_frame markers cleanly for next frame
    for (auto& [k, tg] : targets_) tg.ended_mid_frame = false;

    presents_++;
    presents_seen_.store(presents_, std::memory_order_release);
    fps_frames_++;
    sec_frames_++;
    auto now = std::chrono::steady_clock::now();
    last_present_tp_ = now;
    stall_logged_ = false;
    uint64_t p_ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now - p_t0).count());
    adpf::ReportFrameWorkNs(int64_t(p_ns));
    uint64_t w_ns = sec_wait_ns_ - wait_before;
    if (p_ns > w_ns) sec_cpu_ns_ += (p_ns - w_ns);
    double el = std::chrono::duration<double>(now - fps_t_).count();
    if (el >= 0.5) {
        fps_val_ = float(fps_frames_ / el);
        fps_frames_ = 0;
        fps_t_ = now;
    }

    // Periodically save newly compiled shaders/pipelines to disk after warm-up
    if ((presents_ - last_cache_save_present_) >= 180 &&
        (psos_.size() != last_saved_pso_count_ || ctx_->Shaders().ModuleCount() != last_saved_spv_count_)) {
        ctx_->SaveCaches();
        last_cache_save_present_ = presents_;
        last_saved_pso_count_ = psos_.size();
        last_saved_spv_count_ = ctx_->Shaders().ModuleCount();
    }

    double sec_el = std::chrono::duration<double>(now - sec_t_).count();
    if (sec_el >= 1.0) {
        size_t lazy_d = 0, pers_d = 0;
        for (const auto& [k, tg] : targets_) {
            if (tg.depth) {
                if (tg.transient_lazy) lazy_d++;
                else pers_d++;
            }
        }
        const double fdiv_stats = std::max<double>(double(sec_frames_), 1.0);
        {
            std::lock_guard<std::mutex> lk(g_stats_lock);
            g_stats.guest_fps = fps_val_;
            g_stats.cpu_ms = static_cast<float>((double(sec_cpu_ns_) / 1e6) / fdiv_stats);
            g_stats.scale = scale_;
            g_stats.frames = presents_;
            g_stats.draws = total_draws_;
            g_stats.clears = total_clears_;
            g_stats.rp_opens = total_rp_opens_;
            g_stats.neon_decodes_sec = sec_neon_;
            g_stats.tex_uploads_sec = sec_texups_;
            g_stats.cached_textures = textures_.size();
            g_stats.tex_vram_mb = tex_vram_bytes_ / (1024u * 1024u);
            g_stats.pso_count = psos_.size();
            g_stats.spv_count = ctx_->Shaders().ModuleCount();
            g_stats.lazy_depths = lazy_d;
            g_stats.persistent_depths = pers_d;
            g_stats.thermal_status = last_thermal_status_;
        }
        const uint64_t wait_gpuf_ns = sec_wait_gpuf_ns_.exchange(0, std::memory_order_relaxed);
        const uint64_t g_cap_ns = g_perf_capture_ns.exchange(0, std::memory_order_relaxed);
        const uint64_t g_wait_ns = g_perf_wait_ns.exchange(0, std::memory_order_relaxed);
        (void)g_perf_waits.exchange(0, std::memory_order_relaxed);
        uint64_t g_wk[6]{};
        for (int i = 0; i < 6; ++i) g_wk[i] = g_perf_wait_kind_ns[i].exchange(0, std::memory_order_relaxed);
        const uint64_t g_sb = g_perf_sync_block.exchange(0, std::memory_order_relaxed);
        const uint64_t g_sl = g_perf_sync_late.exchange(0, std::memory_order_relaxed);

        ++perf_log_secs_;
        if (perf_log_secs_ <= 15 || (perf_log_secs_ % 5) == 0) {
            const double fdiv = std::max<double>(double(sec_frames_), 1.0);
            const double cpu_ms = (double(sec_cpu_ns_) / 1e6) / fdiv;
            const double wait_ms = (double(sec_wait_ns_) / 1e6) / fdiv;
            const double draws_f = double(sec_draws_) / fdiv;
            const double rp_f = double(sec_rps_) / fdiv;
            LOGR("vk_perf: %.1f fps scale=%.1fx cpu=%.1fms wait=%.1fms draws/f=%.0f rp/f=%.1f neon/s=%llu texup/s=%llu tex=%zu (%zuMB) pso=%zu spv=%zu depth(L=%zu,P=%zu)",
                 fps_val_, scale_, cpu_ms, wait_ms, draws_f, rp_f,
                 static_cast<unsigned long long>(sec_neon_), static_cast<unsigned long long>(sec_texups_),
                 textures_.size(), g_stats.tex_vram_mb, psos_.size(), ctx_->Shaders().ModuleCount(), lazy_d, pers_d);
            LOGR("vk_perf_detail: sub/f=%.2f stream=%.2fMB/f w[beg=%.1f fl=%.1f sy=%.1f gpuf=%.1f] "
                 "guest[cap=%.1f wait=%.1f(rb=%.1f res=%.1f fnc=%.1f fl=%.1f cp=%.1f frm=%.1f) sb=%.2f sl=%.2f] "
                 "sync/f=%.2f fl/f=%.2f late/f=%.2f rp_end[sw=%llu tx=%llu rt=%llu fb=%llu cp=%llu cl=%llu sub=%llu lt=%llu oth=%llu]",
                 double(sec_submits_) / fdiv,
                 (double(sec_stream_bytes_) / (1024.0 * 1024.0)) / fdiv,
                 (double(sec_wait_begin_ns_) / 1e6) / fdiv,
                 (double(sec_wait_flush_ns_) / 1e6) / fdiv,
                 (double(sec_wait_sync_ns_) / 1e6) / fdiv,
                 (double(wait_gpuf_ns) / 1e6) / fdiv,
                 (double(g_cap_ns) / 1e6) / fdiv,
                 (double(g_wait_ns) / 1e6) / fdiv,
                 (double(g_wk[0]) / 1e6) / fdiv,
                 (double(g_wk[1]) / 1e6) / fdiv,
                 (double(g_wk[2]) / 1e6) / fdiv,
                 (double(g_wk[3]) / 1e6) / fdiv,
                 (double(g_wk[4]) / 1e6) / fdiv,
                 (double(g_wk[5]) / 1e6) / fdiv,
                 double(g_sb) / fdiv,
                 double(g_sl) / fdiv,
                 double(sec_syncs_) / fdiv,
                 double(sec_flushes_) / fdiv,
                 double(sec_lates_) / fdiv,
                 static_cast<unsigned long long>(sec_rp_end_[size_t(RpEndReason::Sw)]),
                 static_cast<unsigned long long>(sec_rp_end_[size_t(RpEndReason::Tex)]),
                 static_cast<unsigned long long>(sec_rp_end_[size_t(RpEndReason::RtTex)]),
                 static_cast<unsigned long long>(sec_rp_end_[size_t(RpEndReason::Fb)]),
                 static_cast<unsigned long long>(sec_rp_end_[size_t(RpEndReason::Copy)]),
                 static_cast<unsigned long long>(sec_rp_end_[size_t(RpEndReason::Clear)]),
                 static_cast<unsigned long long>(sec_rp_end_[size_t(RpEndReason::Sub)]),
                 static_cast<unsigned long long>(sec_rp_end_[size_t(RpEndReason::Late)]),
                 static_cast<unsigned long long>(sec_rp_end_[size_t(RpEndReason::Oth)]));
        }
        sec_neon_ = 0;
        sec_texups_ = 0;
        sec_cpu_ns_ = 0;
        sec_wait_ns_ = 0;
        sec_wait_begin_ns_ = 0;
        sec_wait_flush_ns_ = 0;
        sec_wait_sync_ns_ = 0;
        sec_draws_ = 0;
        sec_rps_ = 0;
        sec_frames_ = 0;
        sec_submits_ = 0;
        sec_stream_bytes_ = 0;
        sec_syncs_ = 0;
        sec_lates_ = 0;
        sec_flushes_ = 0;
        std::memset(sec_rp_end_, 0, sizeof(sec_rp_end_));
        sec_t_ = now;
    }

    ReleaseOldTextures(false);
    ApplySettings();
}

}  // namespace

VulkanStats GetVulkanStats() {
    std::lock_guard<std::mutex> lk(g_stats_lock);
    return g_stats;
}

std::unique_ptr<Backend> CreateBackend() { return std::make_unique<VulkanBackend>(); }

uint64_t HeadlessFrames() { return GetVulkanStats().frames; }
uint64_t HeadlessDraws() { return GetVulkanStats().draws; }
uint64_t HeadlessClears() { return GetVulkanStats().clears; }

}  // namespace gpu
