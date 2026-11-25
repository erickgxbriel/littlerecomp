#include "gpu/history.h"

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <mutex>

namespace gpu {
namespace {

struct Entry {
    uint32_t kind = 0, index = 0, from = 0, to = 0, pc = 0;
};

std::mutex g_lock;
std::deque<Entry> g_ring;
uint64_t g_seq = 0;
constexpr size_t kKeep = 16384;

// XBE_TRACE_TSS=1 prints every write as it happens
bool Tracing() {
    static const bool on = [] {
        const char* v = std::getenv("XBE_TRACE_TSS");
        return v && *v && *v != '0';
    }();
    return on;
}

const char* TssName(uint32_t type) {
    switch (type) {
        case 0: return "AddressU";
        case 1: return "AddressV";
        case 2: return "AddressW";
        case 3: return "MagFilter";
        case 4: return "MinFilter";
        case 5: return "MipFilter";
        case 6: return "MipLodBias";
        case 7: return "MaxMipLevel";
        case 8: return "MaxAnisotropy";
        case 9: return "ColorKeyOp";
        case 10: return "ColorSign";
        case 11: return "AlphaKill";
        case 12: return "ColorOp";
        case 13: return "ColorArg0";
        case 14: return "ColorArg1";
        case 15: return "ColorArg2";
        case 16: return "AlphaOp";
        case 17: return "AlphaArg0";
        case 18: return "AlphaArg1";
        case 19: return "AlphaArg2";
        case 20: return "ResultArg";
        case 21: return "TextureTransformFlags";
        case 27: return "TexCoordIndex";
        case 28: return "BorderColor";
        case 30: return "ColorKeyColor";
        default: return "State";
    }
}

// stage 0 of a draw: ColorOp in the low byte, then the three colour arguments
void FormatDraw(char* buf, size_t n, const Entry& e) {
    std::snprintf(buf, n, "draw prim %u s0 c%u/%u,%u,%u tf %08X", e.index, e.from & 0xFF, (e.from >> 8) & 0xFF,
                  (e.from >> 16) & 0xFF, (e.from >> 24) & 0xFF, e.to);
}

void Format(char* buf, size_t n, uint64_t seq, const Entry& e) {
    char what[160];
    switch (e.kind) {
        case kWriteTextureStage:
            std::snprintf(what, sizeof(what), "tss  s%u %s(%u) %u -> %u", e.index / 32, TssName(e.index % 32),
                          e.index % 32, e.from, e.to);
            break;
        case kWriteTextureFactor:
            std::snprintf(what, sizeof(what), "tf   %08X -> %08X", e.from, e.to);
            break;
        case kWriteTexture:
            std::snprintf(what, sizeof(what), "tex  s%u %08X -> %08X", e.index, e.from, e.to);
            break;
        case kWritePixelShader:
            std::snprintf(what, sizeof(what), "ps   %08X -> %08X", e.from, e.to);
            break;
        case kWriteMirror:
            std::snprintf(what, sizeof(what), "mirror s%u %s(%u) state %u title %u", e.index / 32,
                          TssName(e.index % 32), e.index % 32, e.from, e.to);
            break;
        case kWriteDraw:
            FormatDraw(what, sizeof(what), e);
            break;
        default:
            std::snprintf(what, sizeof(what), "unknown %u", e.kind);
            break;
    }
    if (e.kind == kWriteDraw) std::snprintf(buf, n, "%06llu %s", static_cast<unsigned long long>(seq), what);
    else std::snprintf(buf, n, "%06llu %-46s pc %08X", static_cast<unsigned long long>(seq), what, e.pc);
}

}  // namespace

void RecordWrite(uint32_t kind, uint32_t index, uint32_t from, uint32_t to, uint32_t pc) {
    std::lock_guard<std::mutex> lock(g_lock);
    g_ring.push_back({kind, index, from, to, pc});
    if (g_ring.size() > kKeep) g_ring.pop_front();
    ++g_seq;
    if (Tracing()) {
        char line[256];
        Format(line, sizeof(line), g_seq, g_ring.back());
        std::printf("%s\n", line);
        std::fflush(stdout);
    }
}

void ClearWrites() {
    std::lock_guard<std::mutex> lock(g_lock);
    g_ring.clear();
    g_seq = 0;
}

void DumpWrites(const std::wstring& path) {
    std::lock_guard<std::mutex> lock(g_lock);
#ifdef _WIN32
    FILE* f = _wfopen(path.c_str(), L"w");
#else
    FILE* f = std::fopen(std::filesystem::path(path).c_str(), "w");
#endif
    if (!f) return;
    std::fprintf(f, "# state writes of the captured frame, in order, with the title's return address\n");
    std::fprintf(f, "# tss: texture stage state, tf: texture factor, tex: SetTexture, ps: SetPixelShader\n");
    std::fprintf(f, "# draw: stage 0 colour op and arguments, and the texture factor, as the draw saw them\n");
    std::fprintf(f, "# mirror: the title's deferred table held a value the draw did not get through the API\n");
    std::fprintf(f, "# c<n>: D3DTOP, arguments are D3DTA (0 diffuse, 1 current, 2 texture, 3 tfactor)\n");
    char line[256];
    uint64_t seq = g_seq >= g_ring.size() ? g_seq - g_ring.size() + 1 : 1;
    for (const Entry& e : g_ring) Format(line, sizeof(line), seq++, e), std::fprintf(f, "%s\n", line);
    std::fclose(f);
}

}  // namespace gpu
