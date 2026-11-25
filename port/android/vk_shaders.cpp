#include "vk_shaders.h"

#include <algorithm>
#include <bit>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>

#include <shaderc/shaderc.hpp>

#include "core/log.h"
#include "gpu/vertex.h"

namespace vk_sh {

namespace {

static_assert(sizeof(gpu::DrawConstants) == (4 + 18 + 1 + 4 + 4 + 4 + 4 + 1 + 4 + 4 + 4 + 16 + 5 + 1 + 56 + 1 + 1) * 16,
              "DrawConstants must stay std140 16-byte aligned");

std::string F(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return buf;
}

const char* kUniformDecls = R"(
layout(set = 0, binding = 0, std140) uniform VC {
    highp vec4 c[192];
};
layout(set = 0, binding = 1, std140) uniform DC {
    highp vec4 surf;
    highp vec4 fogp;
    highp vec4 fogcolor;
    highp vec4 misc;
    highp vec4 pc[18];
    highp vec4 tfactor;
    highp vec4 texsize[4];
    highp vec4 bumpmat[4];
    highp vec4 bumplum[4];
    highp mat4 wvp;
    highp vec4 ext;
    highp vec4 spread[4];
    highp mat4 wv;
    highp mat4 wvn;
    highp mat4 tm[4];
    highp vec4 mat[5];
    highp vec4 amb;
    highp vec4 lt[56];
    highp vec4 rtshift;
    highp vec4 zparams;
};
)";

std::string VsHeader(bool quad) {
    std::string s = "#version 450\nprecision highp float;\nprecision highp int;\n";
    s += kUniformDecls;
    s += R"(
layout(location = 0) out vec4 out_d0;
layout(location = 1) out vec4 out_d1;
layout(location = 2) out vec4 out_b0;
layout(location = 3) out vec4 out_b1;
layout(location = 4) out vec4 out_tc0;
layout(location = 5) out vec4 out_tc1;
layout(location = 6) out vec4 out_tc2;
layout(location = 7) out vec4 out_tc3;
layout(location = 8) out float out_fogamt;
)";
    if (quad) {
        s += "layout(location = 9) noperspective out vec4 out_quad_e;\n";
    }
    return s;
}

const char* kVsHelpers = R"(
vec4 mac_mul(vec4 a, vec4 b) {
    return vec4(
        (a.x == 0.0 || b.x == 0.0) ? 0.0 : a.x * b.x,
        (a.y == 0.0 || b.y == 0.0) ? 0.0 : a.y * b.y,
        (a.z == 0.0 || b.z == 0.0) ? 0.0 : a.z * b.z,
        (a.w == 0.0 || b.w == 0.0) ? 0.0 : a.w * b.w
    );
}
int mac_arl(float v) { return int(floor(v + 1e-3)); }
float magclamp(float v) {
    float m = clamp(abs(v), 5.42101e-20, 1.84467e+19);
    return v < 0.0 ? -m : m;
}
vec4 ilu_rcp(float v) { return vec4(1.0 / v); }
vec4 ilu_rcc(float v) { return vec4(magclamp(1.0 / v)); }
vec4 ilu_rsq(float v) {
    return v == 0.0 ? vec4(uintBitsToFloat(0x7F800000u)) : vec4(inversesqrt(abs(v)));
}
vec4 ilu_exp(float v) {
    float whole = floor(v);
    return vec4(exp2(whole), v - whole, exp2(v), 1.0);
}
vec4 ilu_log(float v) {
    float mag = abs(v);
    if (mag == 0.0) {
        float ninf = uintBitsToFloat(0xFF800000u);
        return vec4(ninf, 1.0, ninf, 1.0);
    }
    float l2 = log2(mag);
    float ex = floor(l2);
    return vec4(ex, mag / exp2(ex), l2, 1.0);
}
vec4 ilu_lit(vec4 s) {
    const float cap = 127.99609375;
    float diffuse = max(s.x, 0.0);
    float base = max(s.y, 0.0);
    float p = clamp(s.w, -cap, cap);
    float spec = (diffuse > 0.0) ? ((base > 0.0) ? exp2(p * log2(base)) : (p == 0.0 ? 1.0 : 0.0)) : 0.0;
    return vec4(1.0, diffuse, spec, 1.0);
}
float fogfactor(float d) {
    if (fogp.w == 3.0) return (fogp.y - d) / (fogp.y - fogp.x);
    if (fogp.w == 1.0) return exp(-d * fogp.z);
    if (fogp.w == 2.0) return exp(-(d * fogp.z) * (d * fogp.z));
    return 1.0;
}
float ReversedClip(float wz, float w) {
    if (zparams.x != 0.0) return zparams.x * (zparams.y - (w + zparams.z));
    return wz - zparams.z * w;
}
)";

std::string VsInputs(uint16_t regs, uint64_t decode, std::string& out_body) {
    std::string decls;
    bool packed = false;
    for (int r = 0; r < 16; ++r) {
        if (!((regs >> r) & 1)) continue;
        uint32_t d = static_cast<uint32_t>(decode >> (r * 4)) & 15u;
        const char* type = (d == gpu::kDecodeInt || d == gpu::kDecodeIntW1) ? "ivec4"
                         : (d == gpu::kDecodePacked)                         ? "uint"
                                                                             : "vec4";
        decls += F("layout(location = %d) in %s in_v%d;\n", r, type, r);
        packed |= (d == gpu::kDecodePacked);
        switch (d) {
            case gpu::kDecodeW1: out_body += F("    vec4 v%d = vec4(in_v%d.xyz, 1.0);\n", r, r); break;
            case gpu::kDecodeInt: out_body += F("    vec4 v%d = vec4(in_v%d);\n", r, r); break;
            case gpu::kDecodeIntW1: out_body += F("    vec4 v%d = vec4(vec3(in_v%d.xyz), 1.0);\n", r, r); break;
            case gpu::kDecodePacked: out_body += F("    vec4 v%d = UnpackNp3(in_v%d);\n", r, r); break;
            case gpu::kDecodeFloat2H: out_body += F("    vec4 v%d = vec4(in_v%d.xy, 0.0, in_v%d.z);\n", r, r, r); break;
            default: out_body += F("    vec4 v%d = in_v%d;\n", r, r); break;
        }
    }
    if (packed) {
        decls += R"(
vec4 UnpackNp3(uint u) {
    ivec3 v = ivec3(bitfieldExtract(int(u), 0, 11),
                    bitfieldExtract(int(u), 11, 11),
                    bitfieldExtract(int(u), 22, 10));
    return vec4(max(vec3(v) / vec3(1023.0, 1023.0, 511.0), vec3(-1.0)), 1.0);
}
)";
    }
    return decls;
}

const char* kVsPrologueVars = R"(
    int a0 = 0;
    precise vec4 R0 = vec4(0.0), R1 = vec4(0.0), R2 = vec4(0.0), R3 = vec4(0.0);
    precise vec4 R4 = vec4(0.0), R5 = vec4(0.0), R6 = vec4(0.0), R7 = vec4(0.0);
    precise vec4 R8 = vec4(0.0), R9 = vec4(0.0), R10 = vec4(0.0), R11 = vec4(0.0);
    precise vec4 oPos = vec4(0.0);
    vec4 oD0 = vec4(0.0, 0.0, 0.0, 1.0), oD1 = vec4(0.0);
    vec4 oB0 = vec4(0.0, 0.0, 0.0, 1.0), oB1 = vec4(0.0);
    vec4 oFog = vec4(0.0), oPts = vec4(1.0);
    vec4 oT0 = vec4(0.0, 0.0, 0.0, 1.0), oT1 = vec4(0.0, 0.0, 0.0, 1.0);
    vec4 oT2 = vec4(0.0, 0.0, 0.0, 1.0), oT3 = vec4(0.0, 0.0, 0.0, 1.0);
)";

struct Tok {
    const uint32_t* t;
    uint32_t f(int sub, int bit, int len) const { return (t[sub] >> bit) & ((1u << len) - 1u); }
};

const char* kMask[16] = {"", "w", "z", "zw", "y", "yw", "yz", "yzw", "x", "xw", "xz", "xzw", "xy", "xyw", "xyz", "xyzw"};
const char* kLanes[5] = {"", "x", "xy", "xyz", "xyzw"};
const char* kOut[16] = {"oPos", "", "", "oD0", "oD1", "oFog", "oPts", "oB0", "oB1", "oT0", "oT1", "oT2", "oT3", "", "", ""};

std::string Src(const Tok& k, int which, uint16_t regs) {
    uint32_t neg, sw[4], reg, mux;
    if (which == 0) {
        neg = k.f(1, 8, 1);
        sw[0] = k.f(1, 6, 2), sw[1] = k.f(1, 4, 2), sw[2] = k.f(1, 2, 2), sw[3] = k.f(1, 0, 2);
        reg = k.f(2, 28, 4), mux = k.f(2, 26, 2);
    } else if (which == 1) {
        neg = k.f(2, 25, 1);
        sw[0] = k.f(2, 23, 2), sw[1] = k.f(2, 21, 2), sw[2] = k.f(2, 19, 2), sw[3] = k.f(2, 17, 2);
        reg = k.f(2, 13, 4), mux = k.f(2, 11, 2);
    } else {
        neg = k.f(2, 10, 1);
        sw[0] = k.f(2, 8, 2), sw[1] = k.f(2, 6, 2), sw[2] = k.f(2, 4, 2), sw[3] = k.f(2, 2, 2);
        reg = (k.f(2, 0, 2) << 2) | k.f(3, 30, 2), mux = k.f(3, 28, 2);
        uint32_t ilu = k.f(1, 25, 3);
        if (ilu >= 2 && ilu <= 6) sw[1] = sw[2] = sw[3] = sw[0];
    }
    std::string base;
    if (mux == 1) {
        base = (reg == 12) ? "oPos" : F("R%u", reg);
    } else if (mux == 2) {
        uint32_t v = k.f(1, 9, 4);
        base = ((regs >> v) & 1) ? F("v%u", v) : (v == 3 ? "vec4(1.0)" : "vec4(0.0,0.0,0.0,1.0)");
    } else {
        uint32_t cr = k.f(1, 13, 8);
        base = k.f(3, 1, 1) ? F("c[clamp(a0 + %u, 0, 191)]", cr) : F("c[%u]", cr);
    }
    const char* xyzw = "xyzw";
    return F("(%s(%s).%c%c%c%c)", neg ? "-" : "", base.c_str(), xyzw[sw[0]], xyzw[sw[1]], xyzw[sw[2]], xyzw[sw[3]]);
}

std::string MacExpr(uint32_t op, const std::string& a, const std::string& b, const std::string& c) {
    switch (op) {
        case 1: return a;
        case 2: return "mac_mul(" + a + ", " + b + ")";
        case 3: return "(" + a + " + " + c + ")";
        case 4: return "(mac_mul(" + a + ", " + b + ") + " + c + ")";
        case 5: return "vec4(dot(" + a + ".xyz, " + b + ".xyz))";
        case 6: return "vec4(dot(vec4(" + a + ".xyz, 1.0), " + b + "))";
        case 7: return "vec4(dot(" + a + ", " + b + "))";
        case 8: return "vec4(1.0, " + a + ".y * " + b + ".y, " + a + ".z, " + b + ".w)";
        case 9: return "min(" + a + ", " + b + ")";
        case 10: return "max(" + a + ", " + b + ")";
        case 11: return "vec4(lessThan(" + a + ", " + b + "))";
        case 12: return "vec4(greaterThanEqual(" + a + ", " + b + "))";
        case 13: return a;
        default: return "vec4(0.0)";
    }
}

std::string IluExpr(uint32_t op, const std::string& c) {
    switch (op) {
        case 1: return c;
        case 2: return "ilu_rcp(" + c + ".x)";
        case 3: return "ilu_rcc(" + c + ".x)";
        case 4: return "ilu_rsq(" + c + ".x)";
        case 5: return "ilu_exp(" + c + ".x)";
        case 6: return "ilu_log(" + c + ".x)";
        case 7: return "ilu_lit(" + c + ")";
        default: return "vec4(0.0)";
    }
}

std::string RegDest(uint32_t r) { return r == 12 ? "oPos" : F("R%u", r); }

std::string PretransformedEpilogue(bool quad, bool is_point, bool is_sprite, float point_max) {
    std::string s = "    precise vec4 p = oPos;\n    p.w = magclamp(p.w);\n    precise vec4 q;\n";
    if (quad) s += "    out_quad_e.xy = p.xy;\n";
    s += "    if (surf.w == 0.0 && abs(p.x - (surf.x - 0.5)) < 1e-3) p.x = surf.x;\n";
    s += "    if (surf.w == 0.0 && abs(p.y - (surf.y - 0.5)) < 1e-3) p.y = surf.y;\n";
    if (quad) s += "    out_quad_e.zw = p.xy;\n";
    s += R"(
    q.x = p.x * 2.0 / surf.x - 1.0;
    q.y = 1.0 - p.y * 2.0 / surf.y;
    q.z = p.z * surf.z;
    q.xyz *= p.w;
    q.w = p.w;
    q.z = ReversedClip(q.w - q.z, q.w);
    gl_Position = q;
    out_d0 = clamp(oD0, 0.0, 1.0);
    out_d1 = clamp(oD1, 0.0, 1.0);
    out_b0 = clamp(oB0, 0.0, 1.0);
    out_b1 = clamp(oB1, 0.0, 1.0);
    out_tc0 = oT0;
    out_tc1 = oT1;
    out_tc2 = oT2;
    out_tc3 = oT3;
    out_fogamt = fogfactor(oFog.x);
)";
    if (is_point) {
        if (is_sprite) {
            s += F("    gl_PointSize = clamp(oPts.x * max(surf.w, 1.0), 1.0, %.1f);\n", point_max);
        } else {
            s += F("    gl_PointSize = clamp(max(surf.w, 1.0), 1.0, %.1f);\n", point_max);
        }
    }
    s += "}\n";
    return s;
}

// Pixel shader combiner helpers (mediump FP16 by default)
struct In {
    uint32_t reg, chan, mod;
};
In ParseIn(uint32_t v) { return {v & 0xFu, v & 0x10u, v & 0xE0u}; }

struct Ps {
    const gpu::PixelKey& k;
    int stage = 0;
    std::string e, f;
    bool clamp_sum = false, inv_v1 = false, inv_r0 = false;
    std::string code;

    std::string Var(uint32_t reg) {
        uint32_t flags = k.count >> 8;
        switch (reg) {
            case 0: return "vec4(0.0)";
            case 1: return F("pc[%d]", stage == 8 ? 16 : ((flags & 0x10u) ? stage * 2 : 0));
            case 2: return F("pc[%d]", stage == 8 ? 17 : ((flags & 0x100u) ? stage * 2 + 1 : 1));
            case 3: return "fogin";
            case 4: return "v0";
            case 5: return "v1";
            case 8: return "t0";
            case 9: return "t1";
            case 10: return "t2";
            case 11: return "t3";
            case 12: return "r0";
            case 13: return "r1";
            case 14:
                return F("%s(vec4(%s.rgb + %s.rgb, 0.0))", clamp_sum ? "saturate4" : "",
                         inv_v1 ? "(vec4(1.0) - v1)" : "v1", inv_r0 ? "(vec4(1.0) - r0)" : "r0");
            case 15: return "vec4(" + e + " * " + f + ", 0.0)";
            default: return "vec4(0.0)";
        }
    }

    std::string Input(In in, bool alpha) {
        std::string r = Var(in.reg);
        if (!alpha) r += in.chan ? ".aaa" : ".rgb";
        else r += in.chan ? ".a" : ".b";
        std::string one = alpha ? "1.0" : "vec3(1.0)";
        std::string half = alpha ? "0.5" : "vec3(0.5)";
        std::string zero = alpha ? "0.0" : "vec3(0.0)";
        switch (in.mod) {
            case 0x00: return "max(" + r + ", " + zero + ")";
            case 0x20: return "(" + one + " - clamp(" + r + ", " + zero + ", " + one + "))";
            case 0x40: return "(2.0 * max(" + r + ", " + zero + ") - " + one + ")";
            case 0x60: return "(-2.0 * max(" + r + ", " + zero + ") + " + one + ")";
            case 0x80: return "(max(" + r + ", " + zero + ") - " + half + ")";
            case 0xA0: return "(-max(" + r + ", " + zero + ") + " + half + ")";
            case 0xC0: return r;
            default: return "(-(" + r + "))";
        }
    }

    static std::string Map(const std::string& v, uint32_t m, bool alpha) {
        std::string half = alpha ? "0.5" : "vec3(0.5)";
        switch (m) {
            case 0x08: return "(" + v + " - " + half + ")";
            case 0x10: return "(" + v + " * 2.0)";
            case 0x18: return "((" + v + " - " + half + ") * 2.0)";
            case 0x20: return "(" + v + " * 4.0)";
            case 0x30: return "(" + v + " * 0.5)";
            default: return v;
        }
    }

    static const char* Dest(uint32_t reg) {
        switch (reg) {
            case 4: return "v0";
            case 5: return "v1";
            case 8: return "t0";
            case 9: return "t1";
            case 10: return "t2";
            case 11: return "t3";
            case 12: return "r0";
            case 13: return "r1";
            default: return nullptr;
        }
    }

    std::string Stage(uint32_t inputs, uint32_t output, bool alpha) {
        In d = ParseIn(inputs & 0xFFu), c = ParseIn((inputs >> 8) & 0xFFu), b = ParseIn((inputs >> 16) & 0xFFu),
           a = ParseIn(inputs >> 24);
        uint32_t cd_dst = output & 0xFu, ab_dst = (output >> 4) & 0xFu, sum_dst = (output >> 8) & 0xFu, fl = output >> 12;
        const char* m = alpha ? "a" : "rgb";
        const char* T = alpha ? "float" : "vec3";
        const char* n = alpha ? "a" : "c";
        std::string A = Input(a, alpha), B = Input(b, alpha), C = Input(c, alpha), D = Input(d, alpha);
        std::string ab = (fl & 2u) ? ("dot(" + A + ", " + B + ")") : ("(" + A + " * " + B + ")");
        std::string cd = (fl & 1u) ? ("dot(" + C + ", " + D + ")") : ("(" + C + " * " + D + ")");
        uint32_t map = fl & 0x38u;
        code += F("    %s ab%s = clamp(%s(%s), -1.0, 1.0);\n", T, n, T, Map(ab, map, alpha).c_str());
        code += F("    %s cd%s = clamp(%s(%s), -1.0, 1.0);\n", T, n, T, Map(cd, map, alpha).c_str());
        std::string sum = (fl & 4u)
                              ? F("((%s) ? %s(%s) : %s(%s))",
                                  ((k.count >> 8) & 1u) ? "r0.a >= 0.5"
                                                        : "(int(round(clamp(r0.a, 0.0, 1.0) * 255.0)) & 1) == 1",
                                  T, cd.c_str(), T, ab.c_str())
                              : ("(" + ab + " + " + cd + ")");
        code += F("    %s ms%s = clamp(%s(%s), -1.0, 1.0);\n", T, n, T, Map(sum, map, alpha).c_str());
        std::string post;
        if (const char* r = Dest(ab_dst)) {
            post += F("    %s.%s = ab%s;\n", r, m, n);
            if (!alpha && (fl & 0x80u)) post += F("    %s.a = abc.b;\n", r);
        }
        if (const char* r = Dest(cd_dst)) {
            post += F("    %s.%s = cd%s;\n", r, m, n);
            if (!alpha && (fl & 0x40u)) post += F("    %s.a = cdc.b;\n", r);
        }
        if (const char* r = Dest(sum_dst)) post += F("    %s.%s = ms%s;\n", r, m, n);
        return post;
    }
};

std::string DotInput(uint32_t map, uint32_t src) {
    switch (map) {
        case 1: return F("sbyte(t%u.rgb, 128.0, 128.0, 127.0)", src);
        case 2: return F("sbyte(t%u.rgb, -0.5, 255.5, 127.5)", src);
        case 3: return F("sbyte(t%u.rgb, 0.0, 256.0, 127.0)", src);
        default: return F("t%u.rgb", src);
    }
}

std::string TexUv(const gpu::TexInfo& t, int i, const std::string& uv) {
    std::string n = "(" + uv + ")";
    if (t.rect) n += F(" / texsize[%d].xy", i);
    n += F(" + texsize[%d].zw", i);
    if (!(t.flags & gpu::kTexTarget)) return n;
    std::string step = "dFdx(" + n + ") + dFdy(" + n + ")";
    return n + F(" + rtshift[%d] * (", i) + step + ")";
}

std::string TexFetch(const gpu::PixelKey& k, int i) {
    uint32_t mode = (k.tex_modes >> (i * 5)) & 0x1Fu;
    const gpu::TexInfo& t = k.tex[i];
    uint32_t in = i == 1 ? 0 : i == 2 ? (k.input_texture >> 16) & 0xFu : (k.input_texture >> 20) & 0xFu;
    uint32_t dm = i == 0 ? 0 : (k.dot_mapping >> ((i - 1) * 4)) & 0xFu;
    const std::string tc = F("tc%d", i);
    const char* q = tc.c_str();
    auto put = [&](const std::string& v) { return F("    vec4 t%d = %s;\n", i, v.c_str()); };
    auto dp = [&](int n, int coord, uint32_t map, uint32_t src) {
        return F("    highp float dp%d%s = dot(tc%d.xyz, vec3(%s));\n",
                 n, coord == n ? "" : "n", coord, DotInput(map, src).c_str());
    };
    auto tex2d = [&](const std::string& uv) {
        if (t.flags & gpu::kTexWide) return F("spreadtap(tex%d, %s, spread[%d])", i, uv.c_str(), i);
        return F("texture(tex%d, %s)", i, uv.c_str());
    };
    auto sample2 = [&](const std::string& uv) {
        if (t.kind == gpu::kTex2D) return tex2d(TexUv(t, i, uv));
        if (t.kind == gpu::kTexCube) return F("texture(tex%d, vec3(%s, 1.0))", i, uv.c_str());
        if (t.kind == gpu::kTex3D) return F("texture(tex%d, vec3(%s, 0.0))", i, uv.c_str());
        return std::string("vec4(0.0,0.0,0.0,1.0)");
    };
    auto sample3 = [&](const std::string& uvw) {
        if (t.kind == gpu::kTex2D) return tex2d(TexUv(t, i, "(" + uvw + ").xy"));
        if (t.kind == gpu::kTexCube) return F("texture(tex%d, %s)", i, uvw.c_str());
        if (t.kind == gpu::kTex3D) return F("texture(tex%d, %s)", i, uvw.c_str());
        return std::string("vec4(0.0,0.0,0.0,1.0)");
    };
    auto depth = [&](const std::string& ref) {
        std::string uv = TexUv(t, i, F("%s.xy / %s.w", q, q));
        return put(F("shadow_cmp(tex%d, %s, %s, %d)", i, uv.c_str(), ref.c_str(), t.shadow));
    };
    std::string s;
    switch (mode) {
        case 0: return put("vec4(0.0,0.0,0.0,1.0)");
        case 1: return t.shadow ? depth("0.0") : put(sample2(F("%s.xy / %s.w", q, q)));
        case 2: return t.shadow ? depth(F("%s.z / %s.w", q, q)) : put(sample3(F("%s.xyz / %s.w", q, q)));
        case 3: return put(sample3(tc + ".xyz"));
        case 4: return put(tc);
        case 5:
            s = put("vec4(0.0)");
            for (int j = 0; j < 4; ++j) {
                s += F("    if (%s.%c %s 0.0) discard;\n", q, "xyzw"[j], ((k.compare_mode >> (i * 4 + j)) & 1u) ? ">=" : "<");
            }
            return s;
        case 6:
        case 7:
            s = F("    vec2 duv%d = sbyte(t%u.bgr, 0.0, 256.0, 127.0).xy;\n", i, in);
            s += put(sample2(F("%s.xy + vec2(dot(bumpmat[%d].xz, duv%d), dot(bumpmat[%d].yw, duv%d))", q, i, i, i, i)));
            if (mode == 7) s += F("    t%d *= bumplum[%d].x * t%u.r + bumplum[%d].y;\n", i, i, in, i);
            return s;
        case 9: return dp(i, i, dm, in) + put(sample2(F("vec2(dp%d, dp%d)", i - 1, i)));
        case 0xA:
        case 0x11: return dp(i, i, dm, in) + put("vec4(0.0)");
        case 0xB:
            s = dp(i, i, dm, in) + dp(i, i + 1, (k.dot_mapping >> 8) & 0xFu, (k.input_texture >> 20) & 0xFu);
            return s + put(sample3(F("vec3(dp%d, dp%d, dp%dn)", i - 1, i, i)));
        case 0xC:
            s = dp(i, i, dm, in);
            s += F("    highp vec3 nrm%d = vec3(dp%d, dp%d, dp%d), eye%d = vec3(tc%d.w, tc%d.w, tc%d.w);\n",
                   i, i - 2, i - 1, i, i, i - 2, i - 1, i);
            s += F("    highp vec3 refl%d = 2.0 * nrm%d * dot(nrm%d, eye%d) / dot(nrm%d, nrm%d) - eye%d;\n",
                   i, i, i, i, i, i, i);
            return s + put(sample3(F("refl%d", i)));
        case 0xD:
        case 0xE: return dp(i, i, dm, in) + put(sample3(F("vec3(dp%d, dp%d, dp%d)", i - 2, i - 1, i)));
        case 0xF: return put(sample2(F("t%u.ar", in)));
        case 0x10: return put(sample2(F("t%u.gb", in)));
        default: return put("vec4(0.0)");
    }
}

std::string FfArg(uint32_t a, int stage, bool alpha) {
    std::string v;
    switch (a & 0xFu) {
        case 0: v = "v0"; break;
        case 1: v = "cur"; break;
        case 2: v = F("t%d", stage); break;
        case 3: v = "tfactor"; break;
        case 4: v = "v1"; break;
        case 5: v = "tmp"; break;
        default: v = "cur"; break;
    }
    if (a & 0x20u) v = "(" + v + ").aaaa";
    if (a & 0x10u) v = "(vec4(1.0) - " + v + ")";
    return "(" + v + ")" + (alpha ? ".a" : ".rgb");
}

std::string FfOp(uint32_t op, const std::string& a1, const std::string& a2, const std::string& a0, int stage, bool alpha) {
    switch (op) {
        case 2: return a1;
        case 3: return a2;
        case 4: return a1 + " * " + a2;
        case 5: return a1 + " * " + a2 + " * 2.0";
        case 6: return a1 + " * " + a2 + " * 4.0";
        case 7: return a1 + " + " + a2;
        case 8: return a1 + " + " + a2 + (alpha ? " - 0.5" : " - vec3(0.5)");
        case 9: return "(" + a1 + " + " + a2 + (alpha ? " - 0.5) * 2.0" : " - vec3(0.5)) * 2.0");
        case 10: return a1 + " - " + a2;
        case 11: return a1 + " + " + a2 + " * (" + (alpha ? "1.0" : "vec3(1.0)") + " - " + a1 + ")";
        case 12: return "mix(" + a2 + ", " + a1 + ", v0.a)";
        case 13: return "mix(" + a2 + ", " + a1 + ", cur.a)";
        case 14: return F("mix(%s, %s, t%d.a)", a2.c_str(), a1.c_str(), stage);
        case 15: return "mix(" + a2 + ", " + a1 + ", tfactor.a)";
        case 16: return F("%s + %s * (%s - t%d.a)", a1.c_str(), a2.c_str(), alpha ? "1.0" : "vec3(1.0)", stage);
        case 18: return alpha ? a1 : a1 + " + " + a2 + " * " + a1.substr(0, a1.size() - 4) + ".a";
        case 19: return alpha ? a1 : a1 + " * " + a2 + " + " + a1.substr(0, a1.size() - 4) + ".a";
        case 20: return alpha ? a1 : a1 + " + " + a2 + " * (1.0 - " + a1.substr(0, a1.size() - 4) + ".a)";
        case 21: return alpha ? a1 : "(vec3(1.0) - " + a1 + ") * " + a2 + " + " + a1.substr(0, a1.size() - 4) + ".a";
        case 22:
            return F("clamp(dot((%s - vec3(0.5)) * 2.0, (%s - vec3(0.5)) * 2.0), 0.0, 1.0)",
                     alpha ? ("vec3(" + a1 + ", 0.0, 0.0)").c_str() : a1.c_str(),
                     alpha ? ("vec3(" + a2 + ", 0.0, 0.0)").c_str() : a2.c_str());
        case 23: return a0 + " + " + a1 + " * " + a2;
        case 24: return "mix(" + a2 + ", " + a1 + ", " + a0 + ")";
        default: return alpha ? "cur.a" : "cur.rgb";
    }
}

const char* kPsHelpers = R"(
vec4 saturate4(vec4 x) { return clamp(x, 0.0, 1.0); }
vec3 sbyte(vec3 u, float lo, float hi, float d) {
    vec3 b = u * 255.0;
    return (b - mix(vec3(lo), vec3(hi), greaterThanEqual(b, vec3(128.0)))) / d;
}
vec4 spreadtap(highp sampler2D t, highp vec2 uv, highp vec4 w) {
    vec4 c = vec4(0.0);
    highp int n = int(w.z);
    for (highp int y = 0; y < n; ++y) {
        for (highp int x = 0; x < n; ++x) {
            c += textureLod(t, uv + w.xy * (vec2(float(x), float(y)) - float(n - 1) * 0.5), 0.0);
        }
    }
    return c / float(n * n);
}
vec4 shadow_cmp(highp sampler2DShadow t, highp vec2 uv, highp float z, highp int fmt) {
    highp float mx = (fmt == 2) ? 65535.0 : 16777215.0;
    highp float ref = 1.0 - clamp(z, 0.0, mx) / mx;
    return vec4(texture(t, vec3(uv, ref)));
}
void alphatest(float a) {
    int f = int(misc.y);
    int v = int(round(clamp(a, 0.0, 1.0) * 255.0));
    int r = int(misc.x);
    bool ok = (f == 0) ? false
            : (f == 1) ? (v < r)
            : (f == 2) ? (v == r)
            : (f == 3) ? (v <= r)
            : (f == 4) ? (v > r)
            : (f == 5) ? (v != r)
            : (f == 6) ? (v >= r)
                       : true;
    if (!ok) discard;
}
highp vec4 unstretch(highp vec4 t, highp vec4 e) {
    highp vec2 d = e.zw - e.xy;
    highp vec2 g = vec2(dFdx(e.x), dFdy(e.y));
    return t + (d.x != 0.0 && g.x != 0.0 ? d.x / g.x * dFdx(t) : vec4(0.0))
             + (d.y != 0.0 && g.y != 0.0 ? d.y / g.y * dFdy(t) : vec4(0.0));
}
)";

const char* SamplerType(const gpu::TexInfo& t) {
    if (t.shadow) return "highp sampler2DShadow";
    if (t.kind == gpu::kTex3D) return "sampler3D";
    if (t.kind == gpu::kTexCube) return "samplerCube";
    return "sampler2D";
}

}  // namespace

std::string VertexProgramGlsl(const std::vector<uint32_t>& prog, uint16_t regs, uint64_t decode,
                              bool quad, bool is_point, bool is_sprite, float point_max) {
    std::string body;
    std::string s = VsHeader(quad);
    s += kVsHelpers;
    s += VsInputs(regs, decode, body);
    s += "void main() {\n";
    s += body;
    s += kVsPrologueVars;
    for (size_t n = 0; n + 4 <= prog.size(); n += 4) {
        Tok k{&prog[n]};
        uint32_t mac = k.f(1, 21, 4), ilu = k.f(1, 25, 3);
        if (mac || ilu) {
            std::string a = Src(k, 0, regs), b = Src(k, 1, regs), c = Src(k, 2, regs);
            s += "    {\n";
            if (mac) s += "        precise vec4 mr = " + MacExpr(mac, a, b, c) + ";\n";
            if (ilu) s += "        precise vec4 ir = " + IluExpr(ilu, c) + ";\n";
            uint32_t r = k.f(3, 20, 4), omask = k.f(3, 12, 4);
            if (mac == 13) {
                s += "        a0 = mac_arl(mr.x);\n";
            } else if (mac) {
                uint32_t m = k.f(3, 24, 4);
                if (ilu && r == 1) m = 0;
                if (m && r <= 12) s += F("        %s.%s = mr.%s;\n", RegDest(r).c_str(), kMask[m], kMask[m]);
            }
            if (ilu) {
                uint32_t m = k.f(3, 16, 4), ir = mac ? 1 : r;
                if (m && ir <= 12) s += F("        %s.%s = ir.%s;\n", RegDest(ir).c_str(), kMask[m], kMask[m]);
            }
            bool use_ilu = k.f(3, 2, 1) == 1;
            if (omask && k.f(3, 11, 1) && ((use_ilu && ilu) || (!use_ilu && mac && mac != 13))) {
                uint32_t o = k.f(3, 3, 8) & 0xFu;
                const char* src = use_ilu ? "ir" : "mr";
                if (kOut[o][0]) {
                    const char* msk = (o == 5) ? kLanes[std::popcount(omask)] : kMask[omask];
                    s += F("        %s.%s = %s.%s;\n", kOut[o], msk, src, msk);
                }
            }
            s += "    }\n";
        }
        if (k.f(3, 0, 1)) break;
    }
    s += PretransformedEpilogue(quad, is_point, is_sprite, point_max);
    return s;
}

std::string FixedVertexGlsl(uint16_t regs, bool pretransformed, uint64_t decode, const gpu::FixedKey& f,
                            bool quad, bool is_point, bool is_sprite, float point_max) {
    std::string body;
    std::string s = VsHeader(quad);
    s += kVsHelpers;
    s += VsInputs(regs, decode, body);
    s += "void main() {\n";
    s += body;
    s += kVsPrologueVars;
    auto v = [&](int r, const char* def) { return ((regs >> r) & 1) ? F("v%d", r) : std::string(def); };
    bool lit = !pretransformed && (f.light & 1u);
    if (pretransformed) {
        s += "    vec4 rhw = " + v(0, "vec4(0.0,0.0,0.0,1.0)") + ";\n";
        s += "    float w = (rhw.w == 0.0) ? 1.0 : (1.0 / rhw.w);\n";
        s += "    oPos = vec4(rhw.xy, rhw.z / surf.z, w);\n";
    } else {
        s += "    vec4 ip = vec4(" + v(0, "vec4(0.0,0.0,0.0,1.0)") + ".xyz, 1.0);\n";
        s += "    precise vec4 cp = ip * wvp;\n";
        s += "    vec3 vp = (ip * wv).xyz;\n";
        s += "    vec3 vn = (vec4(" + v(2, "vec4(0.0)") + ".xyz, 0.0) * wvn).xyz;\n";
        if (f.light & 8u) s += "    vn = dot(vn, vn) > 0.0 ? normalize(vn) : vn;\n";
    }
    if (lit) {
        auto src = [&](int field, const char* mat) {
            uint32_t m = (f.light >> (8 + field * 2)) & 3u;
            if (m == 1 && ((regs >> 3) & 1)) return std::string("v3");
            if (m == 2 && ((regs >> 4) & 1)) return std::string("v4");
            return std::string(mat);
        };
        s += "    vec4 md = " + src(0, "mat[0]") + ", ms = " + src(1, "mat[2]") + ", ma = " + src(2, "mat[1]") +
             ", me = " + src(3, "mat[3]") + ";\n";
        s += "    vec3 la = vec3(0.0), ld = vec3(0.0), ls = vec3(0.0);\n";
        s += (f.light & 4u) ? "    vec3 eye = -normalize(vp);\n" : "    vec3 eye = vec3(0.0, 0.0, 1.0);\n";
        for (int n = 0; n < 8; ++n) {
            uint32_t type = (f.types >> (n * 2)) & 3u;
            if (!type) continue;
            int b = n * 7;
            s += "    {\n        vec3 l;\n        float att = 1.0;\n";
            if (type == 3) {
                s += F("        l = -normalize(lt[%d].xyz);\n", b + 4);
            } else {
                s += F("        vec3 d = lt[%d].xyz - vp;\n        float dist = length(d);\n        l = d / max(dist, 1e-6);\n", b + 3);
                s += F("        att = dist > lt[%d].w ? 0.0 : 1.0 / max(lt[%d].x + lt[%d].y * dist + lt[%d].z * dist * dist, 1e-6);\n",
                       b + 3, b + 5, b + 5, b + 5);
                if (type == 2) {
                    s += F("        float rho = dot(-l, normalize(lt[%d].xyz));\n", b + 4);
                    s += F("        if (rho <= lt[%d].y) att = 0.0;\n", b + 6);
                    s += F("        else if (rho <= lt[%d].x) att *= pow(max((rho - lt[%d].y) / max(lt[%d].x - lt[%d].y, 1e-6), 0.0), max(lt[%d].w, 0.0));\n",
                           b + 6, b + 6, b + 6, b + 6, b + 4);
                }
            }
            s += F("        la += att * lt[%d].rgb;\n", b + 2);
            s += "        float nl = dot(vn, l);\n";
            s += F("        ld += att * lt[%d].rgb * max(nl, 0.0);\n", b);
            if (f.light & 2u) {
                s += F("        if (nl > 0.0) { float ndh = max(dot(vn, normalize(l + eye)), 0.0); ls += att * lt[%d].rgb * (mat[4].x > 0.0 ? pow(max(ndh, 1e-6), mat[4].x) : 1.0); }\n",
                       b + 1);
            }
            s += "    }\n";
        }
        s += "    oD0 = vec4(me.rgb + ma.rgb * (amb.rgb + la) + md.rgb * ld, md.a);\n";
        s += (f.light & 2u) ? "    oD1 = vec4(ms.rgb * ls, ms.a);\n" : "    oD1 = vec4(0.0);\n";
    } else {
        s += "    oD0 = " + v(3, "vec4(1.0)") + ";\n";
        s += "    oD1 = " + v(4, "vec4(0.0)") + ";\n";
    }
    s += "    oB0 = oD0;\n    oB1 = oD1;\n";
    for (int t = 0; t < 4; ++t) {
        uint32_t u = f.tex[t], set = u & 3u, gen = (u >> 4) & 7u, count = (u >> 8) & 7u, size = (u >> 12) & 7u;
        std::string in = v(9 + int(set), "vec4(0.0,0.0,0.0,1.0)");
        if (pretransformed || (!gen && !count)) {
            s += F("    oT%d = %s;\n", t, in.c_str());
            continue;
        }
        std::string c = gen == 1 ? "vec4(vn, 1.0)"
                      : gen == 2 ? "vec4(vp, 1.0)"
                      : gen == 3 ? "vec4(reflect(normalize(vp), vn), 1.0)"
                      : size == 1 ? F("vec4(%s.x, 1.0, 0.0, 0.0)", in.c_str())
                      : size == 2 ? F("vec4(%s.xy, 1.0, 0.0)", in.c_str())
                      : size == 3 ? F("vec4(%s.xyz, 1.0)", in.c_str())
                                  : in;
        if (!count) {
            s += F("    oT%d = %s;\n", t, c.c_str());
            continue;
        }
        s += F("    {\n        vec4 tc = %s * tm[%d];\n", c.c_str(), t);
        if (u & 0x800u) s += F("        oT%d = vec4(tc.xy, tc.z, tc[%u]);\n", t, std::min<uint32_t>(count, 4u) - 1u);
        else s += F("        oT%d = vec4(tc.xy, %s, 1.0);\n", t, count >= 3 ? "tc.z" : "0.0");
        s += "    }\n";
    }
    s += "    oFog = " + v(5, "vec4(0.0)") + ";\n";
    if (pretransformed) return s + PretransformedEpilogue(quad, is_point, is_sprite, point_max);

    s += "    vec2 sp = (vec2(cp.x, -cp.y) / cp.w + 1.0) * 0.5 * surf.xy;\n";
    if (quad) s += "    out_quad_e.xy = sp;\n";
    s += "    if (surf.w == 0.0 && abs(sp.x - (surf.x - 0.5)) < 1e-3) cp.x = cp.w;\n";
    s += "    if (surf.w == 0.0 && abs(sp.y - (surf.y - 0.5)) < 1e-3) cp.y = -cp.w;\n";
    if (quad) s += "    out_quad_e.zw = (vec2(cp.x, -cp.y) / cp.w + 1.0) * 0.5 * surf.xy;\n";
    s += R"(
    precise float wz = dot(ip, wvp[3] - wvp[2]);
    cp.z = ReversedClip(wz, cp.w);
    gl_Position = cp;
    out_d0 = clamp(oD0, 0.0, 1.0);
    out_d1 = clamp(oD1, 0.0, 1.0);
    out_b0 = out_d0;
    out_b1 = out_d1;
    out_tc0 = oT0;
    out_tc1 = oT1;
    out_tc2 = oT2;
    out_tc3 = oT3;
    out_fogamt = fogfactor(abs(cp.z));
)";
    if (is_point) {
        if (is_sprite) {
            s += F("    gl_PointSize = clamp(oPts.x * max(surf.w, 1.0), 1.0, %.1f);\n", point_max);
        } else {
            s += F("    gl_PointSize = clamp(max(surf.w, 1.0), 1.0, %.1f);\n", point_max);
        }
    }
    s += "}\n";
    return s;
}

std::string PixelGlsl(const gpu::PixelKey& k, bool is_sprite) {
    std::string s = "#version 450\nprecision mediump float;\nprecision mediump int;\n";
    s += kUniformDecls;
    for (int i = 0; i < 4; ++i) {
        s += F("layout(set = 1, binding = %d) uniform %s tex%d;\n", i, SamplerType(k.tex[i]), i);
    }
    const char* interp = k.flat ? "flat " : "";
    s += F("layout(location = 0) %sin vec4 in_d0;\n", interp);
    s += F("layout(location = 1) %sin vec4 in_d1;\n", interp);
    s += F("layout(location = 2) %sin vec4 in_b0;\n", interp);
    s += F("layout(location = 3) %sin vec4 in_b1;\n", interp);
    s += "layout(location = 4) in highp vec4 in_tc0;\n";
    s += "layout(location = 5) in highp vec4 in_tc1;\n";
    s += "layout(location = 6) in highp vec4 in_tc2;\n";
    s += "layout(location = 7) in highp vec4 in_tc3;\n";
    s += "layout(location = 8) in float in_fogamt;\n";
    if (k.quad) s += "layout(location = 9) noperspective in highp vec4 in_quad_e;\n";
    s += "layout(location = 0) out vec4 out_color;\n";
    s += kPsHelpers;

    s += "void main() {\n";
    if (is_sprite) {
        s += "    highp vec4 tc0 = vec4(gl_PointCoord, 0.0, 1.0);\n";
        s += "    highp vec4 tc1 = in_tc1, tc2 = in_tc2;\n";
        s += "    highp vec4 tc3 = tc0;\n";
    } else if (k.quad) {
        s += "    highp vec4 tc0 = unstretch(in_tc0, in_quad_e);\n";
        s += "    highp vec4 tc1 = unstretch(in_tc1, in_quad_e);\n";
        s += "    highp vec4 tc2 = unstretch(in_tc2, in_quad_e);\n";
        s += "    highp vec4 tc3 = unstretch(in_tc3, in_quad_e);\n";
    } else {
        s += "    highp vec4 tc0 = in_tc0, tc1 = in_tc1, tc2 = in_tc2, tc3 = in_tc3;\n";
    }
    s += k.two_sided ? "    vec4 v0 = gl_FrontFacing ? in_d0 : in_b0, v1 = gl_FrontFacing ? in_d1 : in_b1;\n"
                     : "    vec4 v0 = in_d0, v1 = in_d1;\n";
    s += "    if (misc.w == 0.0) v1 = vec4(0.0, 0.0, 0.0, 1.0);\n";
    s += "    vec4 fogin = vec4(fogcolor.rgb, clamp(in_fogamt, 0.0, 1.0));\n";

    std::string body;
    if (k.programmable) {
        for (int i = 0; i < 4; ++i) body += TexFetch(k, i);
        for (int i = 0; i < 4; ++i) {
            uint32_t m = (k.tex_modes >> (i * 5)) & 0x1Fu;
            bool fetch = m && m != 4 && m != 5 && m != 0xA && m != 0x11;
            if (fetch && ((k.alphakill >> i) & 1u)) body += F("    if (t%d.a == 0.0) discard;\n", i);
        }
        uint32_t mode0 = k.tex_modes & 0x1Fu;
        body += F("    vec4 r0 = vec4(0.0, 0.0, 0.0, %s), r1 = vec4(0.0);\n", mode0 ? "t0.a" : "1.0");
        Ps ps{k};
        uint32_t n = std::min<uint32_t>(k.count & 0xFFu, 8u);
        for (uint32_t st = 0; st < n; ++st) {
            ps.stage = int(st);
            ps.code += "    {\n";
            std::string wc = ps.Stage(k.rgb_in[st], k.rgb_out[st], false);
            std::string wa = ps.Stage(k.a_in[st], k.a_out[st], true);
            ps.code += wc + wa + "    }\n";
        }
        body += ps.code;
        if (k.final_abcd || k.final_efg) {
            ps.stage = 8;
            uint32_t fl = k.final_efg & 0xFFu;
            ps.clamp_sum = fl & 0x80u;
            ps.inv_v1 = fl & 0x40u;
            ps.inv_r0 = fl & 0x20u;
            ps.e = ps.Input(ParseIn(k.final_efg >> 24), false);
            ps.f = ps.Input(ParseIn((k.final_efg >> 16) & 0xFFu), false);
            std::string A = ps.Input(ParseIn(k.final_abcd >> 24), false);
            std::string B = ps.Input(ParseIn((k.final_abcd >> 16) & 0xFFu), false);
            std::string C = ps.Input(ParseIn((k.final_abcd >> 8) & 0xFFu), false);
            std::string D = ps.Input(ParseIn(k.final_abcd & 0xFFu), false);
            std::string G = ps.Input(ParseIn((k.final_efg >> 8) & 0xFFu), true);
            body += "    vec4 outc;\n";
            body += "    outc.rgb = " + D + " + mix(" + C + ", " + B + ", " + A + ");\n";
            body += "    outc.a = " + G + ";\n";
        } else {
            body += "    vec4 outc = vec4(mix(fogin.rgb, r0.rgb, fogin.a), r0.a);\n";
        }
    } else {
        body += "    vec4 cur = v0, tmp = vec4(0.0);\n";
        for (int i = 0; i < 4; ++i) {
            const uint32_t* f = k.ff[i];
            if (f[0] <= 1) break;
            const gpu::TexInfo& t = k.tex[i];
            std::string uv = TexUv(t, i, ((k.proj >> i) & 1u) ? F("tc%d.xy / tc%d.w", i, i) : F("tc%d.xy", i));
            if (t.shadow) {
                body += F("    vec4 t%d = shadow_cmp(tex%d, ", i, i) + TexUv(t, i, F("tc%d.xy / tc%d.w", i, i)) +
                        F(", tc%d.z / tc%d.w, %d);\n", i, i, t.shadow);
            } else if (t.kind == gpu::kTex2D && (t.flags & gpu::kTexWide)) {
                body += F("    vec4 t%d = spreadtap(tex%d, %s, spread[%d]);\n", i, i, uv.c_str(), i);
            } else if (t.kind == gpu::kTex2D) {
                body += F("    vec4 t%d = texture(tex%d, %s);\n", i, i, uv.c_str());
            } else if (t.kind) {
                body += F("    vec4 t%d = texture(tex%d, tc%d.xyz);\n", i, i, i);
            } else {
                body += F("    vec4 t%d = vec4(1.0);\n", i);
            }
            if ((k.alphakill >> i) & 1u) body += F("    if (t%d.a == 0.0) discard;\n", i);
            const char* dst = (f[8] == 5) ? "tmp" : "cur";
            std::string rgb = FfOp(f[0], FfArg(f[1], i, false), FfArg(f[2], i, false), FfArg(f[3], i, false), i, false);
            std::string a = (f[4] <= 1) ? std::string("cur.a")
                                        : FfOp(f[4], FfArg(f[5], i, true), FfArg(f[6], i, true), FfArg(f[7], i, true), i, true);
            body += F("    { vec3 c3 = clamp(%s, 0.0, 1.0); float c1 = clamp(%s, 0.0, 1.0); %s = vec4(c3, c1); }\n",
                      rgb.c_str(), a.c_str(), dst);
        }
        body += "    cur.rgb = clamp(cur.rgb + v1.rgb, 0.0, 1.0);\n";
        body += "    vec4 outc = vec4(mix(fogin.rgb, cur.rgb, fogin.a), cur.a);\n";
    }
    if (k.alphatest) body += "    alphatest(outc.a);\n";
    body += "    outc.rgb = clamp(outc.rgb, 0.0, 1.0);\n";
    body += "    out_color = outc;\n}\n";
    s += body;
    return s;
}

std::string FullscreenVsGlsl() {
    return R"(#version 450
precision highp float;
layout(set = 0, binding = 1, std140) uniform DC {
    vec4 uvrect;
    vec4 opts;
};
layout(location = 0) out vec2 out_uv;
void main() {
    vec2 t = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(t * vec2(2.0, 2.0) + vec2(-1.0, -1.0), 0.0, 1.0);
    out_uv = uvrect.xy + t * uvrect.zw;
}
)";
}

std::string BlitPsGlsl() {
    return R"(#version 450
precision mediump float;
layout(set = 0, binding = 1, std140) uniform DC {
    highp vec4 uvrect;
    highp vec4 opts; // x=alpha, y=taps, z=mul, w=0
};
layout(set = 1, binding = 0) uniform highp sampler2D src_tex;
layout(location = 0) in highp vec2 in_uv;
layout(location = 0) out vec4 out_color;
void main() {
    highp vec2 px = fwidth(in_uv);
    highp int n = max(int(opts.y), 1);
    vec4 c = vec4(0.0);
    for (highp int y = 0; y < n; ++y) {
        for (highp int x = 0; x < n; ++x) {
            highp vec2 off = px * ((vec2(float(x), float(y)) + 0.5) / float(n) - 0.5);
            c += textureLod(src_tex, in_uv + off, 0.0);
        }
    }
    c /= float(n * n);
    c.rgb *= opts.z;
    out_color = vec4(c.rgb, opts.x > 0.0 ? c.a : 1.0);
}
)";
}

std::string ClearPsGlsl() {
    return R"(#version 450
precision mediump float;
layout(set = 0, binding = 1, std140) uniform DC {
    vec4 uvrect;
    vec4 colour;
};
layout(location = 0) in highp vec2 in_uv;
layout(location = 0) out vec4 out_color;
void main() {
    out_color = colour;
}
)";
}

std::string DepthCopyPsGlsl() {
    return R"(#version 450
precision highp float;
layout(set = 1, binding = 0) uniform highp sampler2D src_depth;
layout(location = 0) in highp vec2 in_uv;
void main() {
    gl_FragDepth = textureLod(src_depth, in_uv, 0.0).r;
}
)";
}

namespace {

constexpr uint32_t kSpvCacheMagic = 0x43565053u;  // "SPVC"
constexpr uint32_t kSpvCacheVer = 3u;

}  // namespace

bool ShaderCache::Init(VkDevice device, const std::filesystem::path& cache_file) {
    std::lock_guard<std::mutex> lk(lock_);
    device_ = device;
    cache_file_ = cache_file;
    LoadFromDisk();
    return true;
}

void ShaderCache::LoadFromDisk() {
    if (cache_file_.empty()) return;
    std::ifstream f(cache_file_, std::ios::binary);
    if (!f) return;
    uint32_t hdr[2] = {};
    if (!f.read(reinterpret_cast<char*>(hdr), sizeof(hdr)) || hdr[0] != kSpvCacheMagic || hdr[1] != kSpvCacheVer) {
        return;
    }
    uint64_t key = 0;
    uint32_t words = 0;
    while (f.read(reinterpret_cast<char*>(&key), sizeof(key)) && f.read(reinterpret_cast<char*>(&words), sizeof(words))) {
        if (words == 0 || words > (4u << 20)) break;
        std::vector<uint32_t> spv(words);
        if (!f.read(reinterpret_cast<char*>(spv.data()), std::streamsize(words) * 4)) break;
        spv_cache_.emplace(key, std::move(spv));
    }
    LOGR("vk_sh: loaded %zu cached SPIR-V shaders from %s", spv_cache_.size(), cache_file_.c_str());
}

void ShaderCache::FlushToDisk() {
    std::lock_guard<std::mutex> lk(lock_);
    if (!dirty_ || cache_file_.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(cache_file_.parent_path(), ec);
    std::filesystem::path tmp = cache_file_;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return;
        const uint32_t hdr[2] = {kSpvCacheMagic, kSpvCacheVer};
        f.write(reinterpret_cast<const char*>(hdr), sizeof(hdr));
        for (const auto& [k, spv] : spv_cache_) {
            uint32_t words = static_cast<uint32_t>(spv.size());
            f.write(reinterpret_cast<const char*>(&k), sizeof(k));
            f.write(reinterpret_cast<const char*>(&words), sizeof(words));
            f.write(reinterpret_cast<const char*>(spv.data()), std::streamsize(words) * 4);
        }
    }
    std::filesystem::rename(tmp, cache_file_, ec);
    dirty_ = false;
    LOGR("vk_sh: saved %zu SPIR-V shaders to %s", spv_cache_.size(), cache_file_.c_str());
}

void ShaderCache::Shutdown() {
    FlushToDisk();
    std::lock_guard<std::mutex> lk(lock_);
    for (auto& [k, mod] : modules_) {
        if (mod != VK_NULL_HANDLE) vkDestroyShaderModule(device_, mod, nullptr);
    }
    modules_.clear();
    spv_cache_.clear();
    device_ = VK_NULL_HANDLE;
}

VkShaderModule ShaderCache::GetOrCompile(uint64_t key, bool is_fragment, const std::string& glsl) {
    return GetOrCompile(key, is_fragment, [&glsl]() { return glsl; });
}

VkShaderModule ShaderCache::GetOrCompile(uint64_t key, bool is_fragment,
                                         const std::function<std::string()>& build_glsl) {
    const uint64_t full_key = key ^ (is_fragment ? 0x9E3779B97F4A7C15ull : 0x517CC1B727220A95ull);
    std::lock_guard<std::mutex> lk(lock_);
    if (auto it = modules_.find(full_key); it != modules_.end()) {
        return it->second;
    }

    const std::vector<uint32_t>* spv_ptr = nullptr;
    if (auto sit = spv_cache_.find(full_key); sit != spv_cache_.end()) {
        spv_ptr = &sit->second;
        ++disk_hits_;
    } else {
        const std::string glsl = build_glsl();
        static shaderc::Compiler compiler;
        shaderc::CompileOptions opts;
        opts.SetSourceLanguage(shaderc_source_language_glsl);
        opts.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_1);
        opts.SetOptimizationLevel(shaderc_optimization_level_performance);
        const shaderc_shader_kind kind = is_fragment ? shaderc_fragment_shader : shaderc_vertex_shader;
        auto res = compiler.CompileGlslToSpv(glsl, kind, is_fragment ? "nv2a.frag" : "nv2a.vert", opts);
        if (res.GetCompilationStatus() != shaderc_compilation_status_success) {
            LOGE("vk_sh: shaderc %s error (key=%016llX):\n%s\n--- SOURCE ---\n%s",
                 is_fragment ? "FS" : "VS", static_cast<unsigned long long>(full_key),
                 res.GetErrorMessage().c_str(), glsl.c_str());
            modules_[full_key] = VK_NULL_HANDLE;
            return VK_NULL_HANDLE;
        }
        std::vector<uint32_t> spv(res.cbegin(), res.cend());
        auto [ins, _] = spv_cache_.emplace(full_key, std::move(spv));
        spv_ptr = &ins->second;
        dirty_ = true;
        ++misses_;
    }

    VkShaderModuleCreateInfo smci{};
    smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = spv_ptr->size() * sizeof(uint32_t);
    smci.pCode = spv_ptr->data();
    VkShaderModule mod = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device_, &smci, nullptr, &mod) != VK_SUCCESS) {
        LOGE("vk_sh: vkCreateShaderModule failed (%016llX)", static_cast<unsigned long long>(full_key));
        mod = VK_NULL_HANDLE;
    }
    modules_[full_key] = mod;
    return mod;
}

}  // namespace vk_sh
