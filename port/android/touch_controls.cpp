#include "touch_controls.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <vector>

#include <imgui.h>
#include <toml++/toml.hpp>

#include "android_app.h"
#include "i18n.h"

namespace app {
void ToggleOverlay();
}

namespace kbm {
bool IsInVehicle();
}

namespace touch {

namespace {

constexpr uint16_t kDpadUp = 1 << 0;
constexpr uint16_t kDpadDown = 1 << 1;
constexpr uint16_t kDpadLeft = 1 << 2;
constexpr uint16_t kDpadRight = 1 << 3;
constexpr uint16_t kStart = 1 << 4;
constexpr uint16_t kBack = 1 << 5;
constexpr uint16_t kL3 = 1 << 6;
constexpr uint16_t kR3 = 1 << 7;

enum class Group : int {
    LeftStick = 0,
    RightStick,
    Dpad,
    Face,
    LT,
    LB,
    RT,
    RB,
    L3,
    R3,
    Start,
    Back,
    Menu,
    Count
};

constexpr const char* kGroupKeys[int(Group::Count)] = {
    "left_stick", "right_stick", "dpad", "face", "lt", "lb", "rt", "rb", "l3", "r3", "start", "back", "menu",
};

constexpr const char* kGroupNames[int(Group::Count)] = {
    "L-Stick", "R-Stick", "D-Pad", "A/B/X/Y", "LT", "LB", "RT", "RB", "L3", "R3", "START", "BACK", "MENU",
};

struct GroupCfg {
    float nx = 0.0f;
    float ny = 0.0f;
    float scale = 1.0f;
    bool custom = false;
};

enum class Zone {
    None,
    MenuPill,
    BackPill,
    StartPill,
    L3Btn,
    R3Btn,
    BtnA,
    BtnB,
    BtnX,
    BtnY,
    BtnLT,
    BtnLB,
    BtnRT,
    BtnRB,
    DpadUp,
    DpadDown,
    DpadLeft,
    DpadRight,
    LeftStick,
    RightStick,
};

struct CircleBtn {
    Zone zone;
    Group group;
    const char* label;
    float cx, cy, r;
};

struct GroupBounds {
    float cx = 0.0f;
    float cy = 0.0f;
    float hit_r = 40.0f;
};

struct Layout {
    std::vector<CircleBtn> buttons;
    float ls_cx, ls_cy, ls_r;
    float rs_cx, rs_cy, rs_r;
    std::array<GroupBounds, int(Group::Count)> groups{};
};

std::mutex g_lock;
std::array<GroupCfg, int(Group::Count)> g_groups{};

struct Finger {
    SDL_FingerID id = 0;
    Zone zone = Zone::None;
    float origin_x = 0.0f;
    float origin_y = 0.0f;
    float cur_x = 0.0f;
    float cur_y = 0.0f;
};

std::vector<Finger> g_fingers;
input::Pad g_pad{};
bool g_active = false;

bool g_editing = false;
Group g_selected = Group::Face;
bool g_dragging = false;
SDL_FingerID g_drag_finger = 0;
float g_drag_off_x = 0.0f;
float g_drag_off_y = 0.0f;
float g_tb_x0 = 0.0f, g_tb_y0 = 0.0f, g_tb_x1 = 0.0f, g_tb_y1 = 0.0f;

std::filesystem::path LayoutFile() {
    return app::ExeDir() / "touch_layout.toml";
}

void Anchor(Group g, float def_x, float def_y, float w, float h, float& out_x, float& out_y, float& out_scale) {
    const GroupCfg& cfg = g_groups[int(g)];
    out_scale = std::clamp(cfg.scale, 0.60f, 1.60f);
    if (cfg.custom) {
        out_x = std::clamp(cfg.nx, 0.04f, 0.96f) * w;
        out_y = std::clamp(cfg.ny, 0.05f, 0.95f) * h;
    } else {
        out_x = def_x;
        out_y = def_y;
    }
}

Layout ComputeLayoutLocked(float w, float h) {
    Layout l{};
    const float safe_x = std::max(44.0f, w * 0.032f);
    const float base_face = std::clamp(h * 0.062f, 30.0f, 82.0f);
    const float base_dpad = std::clamp(h * 0.052f, 26.0f, 70.0f);
    const float base_trig = std::clamp(h * 0.052f, 26.0f, 70.0f);
    const float base_util = std::clamp(h * 0.042f, 22.0f, 56.0f);
    const float base_stick = std::clamp(h * 0.145f, 70.0f, 190.0f);

    float sc = 1.0f;

    // Left stick
    Anchor(Group::LeftStick, safe_x + base_stick * 1.15f, h * 0.74f, w, h, l.ls_cx, l.ls_cy, sc);
    l.ls_r = base_stick * sc;
    l.groups[int(Group::LeftStick)] = {l.ls_cx, l.ls_cy, l.ls_r};

    // Right stick
    Anchor(Group::RightStick, w - safe_x - base_stick * 1.40f, h * 0.77f, w, h, l.rs_cx, l.rs_cy, sc);
    l.rs_r = base_stick * sc;
    l.groups[int(Group::RightStick)] = {l.rs_cx, l.rs_cy, l.rs_r * 0.85f};

    // Top bar pills: BACK / START / MENU
    const float top_y = std::max(base_util + 12.0f, h * 0.060f);
    float cx = 0.0f, cy = 0.0f;

    Anchor(Group::Back, w * 0.43f, top_y, w, h, cx, cy, sc);
    l.buttons.push_back({Zone::BackPill, Group::Back, "BACK", cx, cy, base_util * sc});
    l.groups[int(Group::Back)] = {cx, cy, base_util * sc};

    Anchor(Group::Start, w * 0.57f, top_y, w, h, cx, cy, sc);
    l.buttons.push_back({Zone::StartPill, Group::Start, "START", cx, cy, base_util * sc});
    l.groups[int(Group::Start)] = {cx, cy, base_util * sc};

    Anchor(Group::Menu, w - safe_x - base_util * 1.0f, top_y, w, h, cx, cy, sc);
    l.buttons.push_back({Zone::MenuPill, Group::Menu, "MENU", cx, cy, base_util * sc});
    l.groups[int(Group::Menu)] = {cx, cy, base_util * sc};

    // Triggers
    const float trig_y = std::max(top_y + base_util + base_trig + 18.0f, h * 0.175f);

    Anchor(Group::LT, safe_x + base_trig * 1.0f, trig_y, w, h, cx, cy, sc);
    l.buttons.push_back({Zone::BtnLT, Group::LT, "LT", cx, cy, base_trig * sc});
    l.groups[int(Group::LT)] = {cx, cy, base_trig * sc};

    Anchor(Group::LB, safe_x + base_trig * 3.1f, trig_y, w, h, cx, cy, sc);
    l.buttons.push_back({Zone::BtnLB, Group::LB, "LB", cx, cy, base_trig * sc});
    l.groups[int(Group::LB)] = {cx, cy, base_trig * sc};

    Anchor(Group::RT, w - safe_x - base_trig * 1.0f, trig_y, w, h, cx, cy, sc);
    l.buttons.push_back({Zone::BtnRT, Group::RT, "RT", cx, cy, base_trig * sc});
    l.groups[int(Group::RT)] = {cx, cy, base_trig * sc};

    Anchor(Group::RB, w - safe_x - base_trig * 3.1f, trig_y, w, h, cx, cy, sc);
    l.buttons.push_back({Zone::BtnRB, Group::RB, "RB", cx, cy, base_trig * sc});
    l.groups[int(Group::RB)] = {cx, cy, base_trig * sc};

    // D-pad group
    Anchor(Group::Dpad, safe_x + base_dpad * 2.1f, h * 0.42f, w, h, cx, cy, sc);
    const float r_dp = base_dpad * sc;
    const float dp_off = r_dp * 1.45f;
    l.buttons.push_back({Zone::DpadUp, Group::Dpad, "^", cx, cy - dp_off, r_dp});
    l.buttons.push_back({Zone::DpadDown, Group::Dpad, "v", cx, cy + dp_off, r_dp});
    l.buttons.push_back({Zone::DpadLeft, Group::Dpad, "<", cx - dp_off, cy, r_dp});
    l.buttons.push_back({Zone::DpadRight, Group::Dpad, ">", cx + dp_off, cy, r_dp});
    l.groups[int(Group::Dpad)] = {cx, cy, dp_off + r_dp};

    // Face diamond group
    Anchor(Group::Face, w - safe_x - base_face * 2.35f, h * 0.46f, w, h, cx, cy, sc);
    const float r_fc = base_face * sc;
    const float fc_off = r_fc * 1.55f;
    l.buttons.push_back({Zone::BtnY, Group::Face, "Y", cx, cy - fc_off, r_fc});
    l.buttons.push_back({Zone::BtnB, Group::Face, "B", cx + fc_off, cy, r_fc});
    l.buttons.push_back({Zone::BtnA, Group::Face, "A", cx, cy + fc_off, r_fc});
    l.buttons.push_back({Zone::BtnX, Group::Face, "X", cx - fc_off, cy, r_fc});
    l.groups[int(Group::Face)] = {cx, cy, fc_off + r_fc};

    // L3 / R3
    Anchor(Group::L3, (safe_x + base_stick * 1.15f) + base_stick * 1.35f, h * 0.87f, w, h, cx, cy, sc);
    l.buttons.push_back({Zone::L3Btn, Group::L3, "L3", cx, cy, base_util * sc});
    l.groups[int(Group::L3)] = {cx, cy, base_util * sc};

    Anchor(Group::R3, (w - safe_x - base_stick * 1.40f) - base_stick * 1.35f, h * 0.87f, w, h, cx, cy, sc);
    l.buttons.push_back({Zone::R3Btn, Group::R3, "R3", cx, cy, base_util * sc});
    l.groups[int(Group::R3)] = {cx, cy, base_util * sc};

    return l;
}

Zone HitTestLocked(const Layout& l, float px, float py, bool enabled, float w, float h) {
    // Menu pill is always interactive even when virtual controls are hidden.
    for (const CircleBtn& b : l.buttons) {
        if (!enabled && b.zone != Zone::MenuPill) continue;
        float dx = px - b.cx;
        float dy = py - b.cy;
        float hit_r = b.r * 1.25f;
        if (dx * dx + dy * dy <= hit_r * hit_r) return b.zone;
    }
    if (!enabled) return Zone::None;

    auto near_circle = [&](float cx, float cy, float r) {
        float dx = px - cx;
        float dy = py - cy;
        float hr = r * 1.35f;
        return dx * dx + dy * dy <= hr * hr;
    };
    if (near_circle(l.ls_cx, l.ls_cy, l.ls_r)) return Zone::LeftStick;
    if (near_circle(l.rs_cx, l.rs_cy, l.rs_r)) return Zone::RightStick;
    if (!g_groups[int(Group::LeftStick)].custom && px < w * 0.44f && py > h * 0.45f) return Zone::LeftStick;
    if (!g_groups[int(Group::RightStick)].custom && px >= w * 0.44f && py > h * 0.24f) return Zone::RightStick;
    return Zone::None;
}

void RebuildPadLocked(const Layout& l) {
    input::Pad p{};
    bool any = false;
    float lx = 0.0f, ly = 0.0f;
    float rx = 0.0f, ry = 0.0f;

    for (const Finger& f : g_fingers) {
        any = true;
        switch (f.zone) {
            case Zone::BackPill: p.buttons |= kBack; break;
            case Zone::StartPill: p.buttons |= kStart; break;
            case Zone::L3Btn: p.buttons |= kL3; break;
            case Zone::R3Btn: p.buttons |= kR3; break;
            case Zone::DpadUp: p.buttons |= kDpadUp; break;
            case Zone::DpadDown: p.buttons |= kDpadDown; break;
            case Zone::DpadLeft: p.buttons |= kDpadLeft; break;
            case Zone::DpadRight: p.buttons |= kDpadRight; break;
            case Zone::BtnA: p.analog[0] = 255; break;
            case Zone::BtnB: p.analog[1] = 255; break;
            case Zone::BtnX: p.analog[2] = 255; break;
            case Zone::BtnY: p.analog[3] = 255; break;
            case Zone::BtnRB: p.analog[4] = 255; break;
            case Zone::BtnLB: p.analog[5] = 255; break;
            case Zone::BtnLT: p.analog[6] = 255; break;
            case Zone::BtnRT: p.analog[7] = 255; break;
            case Zone::LeftStick: {
                float dx = (f.cur_x - f.origin_x) / l.ls_r;
                float dy = (f.cur_y - f.origin_y) / l.ls_r;
                float len = std::sqrt(dx * dx + dy * dy);
                if (len > 1.0f) {
                    dx /= len;
                    dy /= len;
                }
                if (len > 0.08f) {
                    lx = std::clamp(dx, -1.0f, 1.0f);
                    ly = std::clamp(-dy, -1.0f, 1.0f);
                }
                break;
            }
            case Zone::RightStick: {
                float dx = (f.cur_x - f.origin_x) / l.rs_r;
                float dy = (f.cur_y - f.origin_y) / l.rs_r;
                float len = std::sqrt(dx * dx + dy * dy);
                if (len > 1.0f) {
                    dx /= len;
                    dy /= len;
                }
                if (len > 0.08f) {
                    rx = std::clamp(dx, -1.0f, 1.0f);
                    ry = std::clamp(-dy, -1.0f, 1.0f);
                }
                break;
            }
            default: break;
        }
    }

    p.lx = static_cast<int16_t>(std::lround(lx * 32767.0f));
    p.ly = static_cast<int16_t>(std::lround(ly * 32767.0f));
    p.rx = static_cast<int16_t>(std::lround(rx * 32767.0f));
    p.ry = static_cast<int16_t>(std::lround(ry * 32767.0f));
    p.connected = any;
    g_pad = p;
    g_active = any;
}

bool IsZonePressedLocked(Zone z) {
    for (const Finger& f : g_fingers)
        if (f.zone == z) return true;
    return false;
}

bool HandleEditorEventLocked(const SDL_Event& e, float w, float h) {
    const float px = e.tfinger.x * w;
    const float py = e.tfinger.y * h;

    if (e.type == SDL_EVENT_FINGER_DOWN) {
        if (px >= g_tb_x0 - 12.0f && px <= g_tb_x1 + 12.0f &&
            py >= g_tb_y0 - 12.0f && py <= g_tb_y1 + 12.0f) {
            return false;
        }
        const Layout l = ComputeLayoutLocked(w, h);
        float best_d2 = 1e30f;
        int best_g = -1;
        for (int i = 0; i < int(Group::Count); ++i) {
            const GroupBounds& b = l.groups[i];
            float dx = px - b.cx;
            float dy = py - b.cy;
            float hr = std::max(b.hit_r * 1.2f, 44.0f);
            float d2 = dx * dx + dy * dy;
            if (d2 <= hr * hr && d2 < best_d2) {
                best_d2 = d2;
                best_g = i;
            }
        }
        if (best_g >= 0) {
            g_selected = Group(best_g);
            g_dragging = true;
            g_drag_finger = e.tfinger.fingerID;
            g_drag_off_x = px - l.groups[best_g].cx;
            g_drag_off_y = py - l.groups[best_g].cy;
            return true;
        }
        return true;
    }

    if (e.type == SDL_EVENT_FINGER_MOTION) {
        if (g_dragging && e.tfinger.fingerID == g_drag_finger) {
            GroupCfg& cfg = g_groups[int(g_selected)];
            cfg.nx = std::clamp((px - g_drag_off_x) / w, 0.04f, 0.96f);
            cfg.ny = std::clamp((py - g_drag_off_y) / h, 0.05f, 0.95f);
            cfg.custom = true;
            return true;
        }
        return false;
    }

    if (g_dragging && e.tfinger.fingerID == g_drag_finger) {
        g_dragging = false;
        return true;
    }
    return false;
}

void SaveLayoutLocked() {
    toml::table root;
    for (int i = 0; i < int(Group::Count); ++i) {
        const GroupCfg& cfg = g_groups[i];
        toml::table entry;
        entry.insert("nx", double(cfg.nx));
        entry.insert("ny", double(cfg.ny));
        entry.insert("scale", double(cfg.scale));
        entry.insert("custom", cfg.custom);
        root.insert(kGroupKeys[i], std::move(entry));
    }
    std::ofstream f(LayoutFile());
    if (f) f << root << "\n";
}

}  // namespace

void LoadLayout() {
    std::lock_guard<std::mutex> lock(g_lock);
    toml::table root;
    try {
        root = toml::parse_file(LayoutFile().string());
    } catch (...) {
        return;
    }
    for (int i = 0; i < int(Group::Count); ++i) {
        if (const toml::table* entry = root[kGroupKeys[i]].as_table()) {
            GroupCfg& cfg = g_groups[i];
            cfg.nx = std::clamp(float((*entry)["nx"].value_or(double(cfg.nx))), 0.04f, 0.96f);
            cfg.ny = std::clamp(float((*entry)["ny"].value_or(double(cfg.ny))), 0.05f, 0.95f);
            cfg.scale = std::clamp(float((*entry)["scale"].value_or(double(cfg.scale))), 0.60f, 1.60f);
            cfg.custom = (*entry)["custom"].value_or(cfg.custom);
        }
    }
}

void SaveLayout() {
    std::lock_guard<std::mutex> lock(g_lock);
    SaveLayoutLocked();
}

void ResetLayout() {
    std::lock_guard<std::mutex> lock(g_lock);
    g_groups = {};
    g_dragging = false;
    std::error_code ec;
    std::filesystem::remove(LayoutFile(), ec);
}

void SetEditing(bool editing) {
    std::lock_guard<std::mutex> lock(g_lock);
    g_editing = editing;
    g_dragging = false;
    g_fingers.clear();
    g_pad = {};
    g_active = false;
    if (editing) g_selected = Group::Face;
}

bool IsEditing() {
    std::lock_guard<std::mutex> lock(g_lock);
    return g_editing;
}

bool HandleEvent(const SDL_Event& e, float view_w, float view_h, bool overlay_open, bool enabled) {
    if (e.type != SDL_EVENT_FINGER_DOWN && e.type != SDL_EVENT_FINGER_MOTION &&
        e.type != SDL_EVENT_FINGER_UP && e.type != SDL_EVENT_FINGER_CANCELED) {
        return false;
    }
    if (overlay_open || view_w <= 0.0f || view_h <= 0.0f) return false;

    const float px = e.tfinger.x * view_w;
    const float py = e.tfinger.y * view_h;

    bool toggle_menu = false;
    bool consumed = false;

    {
        std::lock_guard<std::mutex> lock(g_lock);
        if (g_editing) {
            return HandleEditorEventLocked(e, view_w, view_h);
        }
        const Layout layout = ComputeLayoutLocked(view_w, view_h);
        if (e.type == SDL_EVENT_FINGER_DOWN) {
            Zone z = HitTestLocked(layout, px, py, enabled, view_w, view_h);
            if (z != Zone::None) {
                g_fingers.push_back({e.tfinger.fingerID, z, px, py, px, py});
                if (z == Zone::MenuPill) toggle_menu = true;
                consumed = true;
            }
        } else if (e.type == SDL_EVENT_FINGER_MOTION) {
            for (Finger& f : g_fingers) {
                if (f.id == e.tfinger.fingerID) {
                    f.cur_x = px;
                    f.cur_y = py;
                    consumed = true;
                    break;
                }
            }
        } else {
            for (auto it = g_fingers.begin(); it != g_fingers.end(); ++it) {
                if (it->id == e.tfinger.fingerID) {
                    g_fingers.erase(it);
                    consumed = true;
                    break;
                }
            }
        }
        if (consumed) RebuildPadLocked(layout);
    }

    if (toggle_menu) {
        Clear();
        app::ToggleOverlay();
    }
    return consumed;
}

void Clear() {
    std::lock_guard<std::mutex> lock(g_lock);
    g_fingers.clear();
    g_pad = {};
    g_active = false;
}

void DrawOverlay(float view_w, float view_h, float opacity, bool enabled, bool overlay_open) {
    if (overlay_open || view_w <= 0.0f || view_h <= 0.0f) return;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    if (!dl) return;

    const bool auto_aim_fire_armed = app::AndroidSettings().touch_auto_aim_fire && !kbm::IsInVehicle();

    Layout layout{};
    bool editing = false;
    Group selected = Group::Face;
    float selected_scale = 1.0f;
    std::vector<Finger> fingers_copy;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        editing = g_editing;
        selected = g_selected;
        selected_scale = g_groups[int(selected)].scale;
        layout = ComputeLayoutLocked(view_w, view_h);
        fingers_copy = g_fingers;
    }

    const float a = editing ? std::max(0.75f, opacity) : std::clamp(opacity, 0.1f, 0.95f);
    const bool draw_all = enabled || editing;

    auto draw_centered_text = [&](float cx, float cy, const char* lbl, ImU32 col) {
        ImVec2 ts = ImGui::CalcTextSize(lbl);
        ImVec2 pos(cx - ts.x * 0.5f, cy - ts.y * 0.5f);
        dl->AddText(ImVec2(pos.x + 1.0f, pos.y + 1.0f), IM_COL32(0, 0, 0, int(a * 200)), lbl);
        dl->AddText(pos, col, lbl);
    };

    for (const CircleBtn& b : layout.buttons) {
        if (!draw_all && b.zone != Zone::MenuPill) continue;
        bool pressed = false;
        for (const Finger& f : fingers_copy) {
            if (f.zone == b.zone) {
                pressed = true;
                break;
            }
        }
        const bool is_fire_rt = (b.zone == Zone::BtnRT && auto_aim_fire_armed);
        const ImU32 text_col = IM_COL32(255, 255, 255, int(std::min(1.0f, a + 0.28f) * 255));
        const ImU32 dark_fill = IM_COL32(18, 22, 30, int(a * 165));

        if (b.zone == Zone::BtnA || b.zone == Zone::BtnB || b.zone == Zone::BtnX || b.zone == Zone::BtnY) {
            int cr = 255, cg = 255, cb = 255;
            if (b.zone == Zone::BtnA) { cr = 64; cg = 217; cb = 89; }
            else if (b.zone == Zone::BtnB) { cr = 242; cg = 68; cb = 68; }
            else if (b.zone == Zone::BtnX) { cr = 59; cg = 158; cb = 255; }
            else if (b.zone == Zone::BtnY) { cr = 245; cg = 199; cb = 49; }

            if (pressed) {
                dl->AddCircleFilled(ImVec2(b.cx, b.cy), b.r + 5.0f, IM_COL32(cr, cg, cb, int(a * 95)), 32);
            }
            ImU32 fill = pressed ? IM_COL32(cr, cg, cb, int(a * 175)) : dark_fill;
            ImU32 outer = IM_COL32(235, 240, 248, int(a * (pressed ? 235 : 135)));
            ImU32 inner = IM_COL32(cr, cg, cb, int(std::min(1.0f, a + 0.25f) * 255));
            dl->AddCircleFilled(ImVec2(b.cx, b.cy), b.r, fill, 32);
            dl->AddCircle(ImVec2(b.cx, b.cy), b.r, outer, 32, 1.5f);
            dl->AddCircle(ImVec2(b.cx, b.cy), std::max(4.0f, b.r - 3.5f), inner, 32, pressed ? 3.0f : 2.3f);
            draw_centered_text(b.cx, b.cy, b.label, pressed ? IM_COL32(255, 255, 255, 255) : inner);
        } else if (b.zone == Zone::DpadUp || b.zone == Zone::DpadDown ||
                   b.zone == Zone::DpadLeft || b.zone == Zone::DpadRight) {
            const float hs = b.r * 0.90f;
            const float rnd = b.r * 0.30f;
            ImVec2 p0(b.cx - hs, b.cy - hs);
            ImVec2 p1(b.cx + hs, b.cy + hs);
            ImU32 fill = pressed ? IM_COL32(255, 196, 64, int(a * 185)) : dark_fill;
            ImU32 border = pressed ? IM_COL32(255, 224, 120, int(a * 245)) : IM_COL32(215, 224, 235, int(a * 165));
            ImU32 tri_col = pressed ? IM_COL32(22, 24, 30, 245) : text_col;
            dl->AddRectFilled(p0, p1, fill, rnd);
            dl->AddRect(p0, p1, border, rnd, 0, 1.8f);

            const float aw = b.r * 0.42f;
            const float ah = b.r * 0.36f;
            ImVec2 t0, t1, t2;
            if (b.zone == Zone::DpadUp) {
                t0 = ImVec2(b.cx, b.cy - ah);
                t1 = ImVec2(b.cx - aw, b.cy + ah * 0.85f);
                t2 = ImVec2(b.cx + aw, b.cy + ah * 0.85f);
            } else if (b.zone == Zone::DpadDown) {
                t0 = ImVec2(b.cx, b.cy + ah);
                t1 = ImVec2(b.cx - aw, b.cy - ah * 0.85f);
                t2 = ImVec2(b.cx + aw, b.cy - ah * 0.85f);
            } else if (b.zone == Zone::DpadLeft) {
                t0 = ImVec2(b.cx - ah, b.cy);
                t1 = ImVec2(b.cx + ah * 0.85f, b.cy - aw);
                t2 = ImVec2(b.cx + ah * 0.85f, b.cy + aw);
            } else {
                t0 = ImVec2(b.cx + ah, b.cy);
                t1 = ImVec2(b.cx - ah * 0.85f, b.cy - aw);
                t2 = ImVec2(b.cx - ah * 0.85f, b.cy + aw);
            }
            dl->AddTriangleFilled(t0, t1, t2, tri_col);
        } else if (is_fire_rt) {
            // Armed Auto-Aim + Fire reticle button (on-foot)
            if (pressed) {
                dl->AddCircleFilled(ImVec2(b.cx, b.cy), b.r + 5.0f, IM_COL32(245, 72, 72, int(a * 110)), 32);
            }
            ImU32 fill = pressed ? IM_COL32(235, 68, 68, int(a * 195)) : IM_COL32(48, 16, 20, int(a * 175));
            ImU32 accent = pressed ? IM_COL32(255, 224, 120, 255) : IM_COL32(245, 82, 88, int(std::min(1.0f, a + 0.30f) * 255));
            dl->AddCircleFilled(ImVec2(b.cx, b.cy), b.r, fill, 32);
            dl->AddCircle(ImVec2(b.cx, b.cy), b.r, accent, 32, 2.5f);

            // Vector crosshair reticle above compact label
            const float ry = b.cy - b.r * 0.12f;
            const float rr = b.r * 0.36f;
            dl->AddCircle(ImVec2(b.cx, ry), rr, accent, 24, 1.8f);
            dl->AddLine(ImVec2(b.cx - rr * 1.45f, ry), ImVec2(b.cx - rr * 0.55f, ry), accent, 1.8f);
            dl->AddLine(ImVec2(b.cx + rr * 0.55f, ry), ImVec2(b.cx + rr * 1.45f, ry), accent, 1.8f);
            dl->AddLine(ImVec2(b.cx, ry - rr * 1.45f), ImVec2(b.cx, ry - rr * 0.55f), accent, 1.8f);
            dl->AddLine(ImVec2(b.cx, ry + rr * 0.55f), ImVec2(b.cx, ry + rr * 1.45f), accent, 1.8f);
            dl->AddCircleFilled(ImVec2(b.cx, ry), 2.2f, text_col, 12);
            draw_centered_text(b.cx, b.cy + b.r * 0.56f, "FIRE", text_col);
        } else if (b.zone == Zone::BtnLT || b.zone == Zone::BtnLB ||
                   b.zone == Zone::BtnRT || b.zone == Zone::BtnRB) {
            const float hw = b.r * 0.98f;
            const float hh = b.r * 0.66f;
            const float rnd = hh * 0.90f;
            ImVec2 p0(b.cx - hw, b.cy - hh);
            ImVec2 p1(b.cx + hw, b.cy + hh);
            ImU32 fill = pressed ? IM_COL32(255, 196, 64, int(a * 185)) : dark_fill;
            ImU32 border = pressed ? IM_COL32(255, 224, 120, int(a * 245)) : IM_COL32(215, 224, 235, int(a * 180));
            dl->AddRectFilled(p0, p1, fill, rnd);
            dl->AddRect(p0, p1, border, rnd, 0, 2.0f);
            dl->AddRect(ImVec2(p0.x + 2.5f, p0.y + 2.5f), ImVec2(p1.x - 2.5f, p1.y - 2.5f),
                        IM_COL32(255, 255, 255, int(a * 45)), std::max(2.0f, rnd - 2.5f), 0, 1.0f);
            draw_centered_text(b.cx, b.cy, b.label, text_col);
        } else if (b.zone == Zone::MenuPill) {
            const float hw = b.r * 1.52f;
            const float hh = b.r * 0.70f;
            const float rnd = hh * 0.90f;
            ImVec2 p0(b.cx - hw, b.cy - hh);
            ImVec2 p1(b.cx + hw, b.cy + hh);
            ImU32 fill = pressed ? IM_COL32(215, 52, 62, int(a * 200)) : IM_COL32(24, 26, 34, int(a * 170));
            ImU32 border = IM_COL32(225, 230, 238, int(a * 190));
            dl->AddRectFilled(p0, p1, fill, rnd);
            dl->AddRect(p0, p1, border, rnd, 0, 1.8f);
            // 3-bar hamburger icon on the left + MENU text cleanly spaced
            const float bx = b.cx - hw * 0.68f;
            const float bw = b.r * 0.34f;
            const float bgap = b.r * 0.21f;
            for (int row = -1; row <= 1; ++row) {
                float ly = b.cy + float(row) * bgap;
                dl->AddLine(ImVec2(bx, ly), ImVec2(bx + bw, ly), text_col, 2.2f);
            }
            draw_centered_text(b.cx + b.r * 0.34f, b.cy, "MENU", text_col);
        } else if (b.zone == Zone::BackPill || b.zone == Zone::StartPill) {
            const float hw = b.r * 1.22f;
            const float hh = b.r * 0.64f;
            const float rnd = hh * 0.90f;
            ImVec2 p0(b.cx - hw, b.cy - hh);
            ImVec2 p1(b.cx + hw, b.cy + hh);
            ImU32 fill = pressed ? IM_COL32(255, 196, 64, int(a * 180)) : dark_fill;
            ImU32 border = pressed ? IM_COL32(255, 224, 120, int(a * 240)) : IM_COL32(210, 218, 230, int(a * 165));
            dl->AddRectFilled(p0, p1, fill, rnd);
            dl->AddRect(p0, p1, border, rnd, 0, 1.6f);
            draw_centered_text(b.cx, b.cy, b.label, text_col);
        } else {
            // L3 / R3
            ImU32 fill = pressed ? IM_COL32(255, 196, 64, int(a * 175)) : dark_fill;
            ImU32 border = pressed ? IM_COL32(255, 224, 120, int(a * 240)) : IM_COL32(215, 222, 232, int(a * 175));
            dl->AddCircleFilled(ImVec2(b.cx, b.cy), b.r, fill, 28);
            dl->AddCircle(ImVec2(b.cx, b.cy), b.r, border, 28, 1.8f);
            dl->AddCircle(ImVec2(b.cx, b.cy), std::max(3.0f, b.r - 4.0f), IM_COL32(255, 255, 255, int(a * 50)), 28, 1.0f);
            draw_centered_text(b.cx, b.cy, b.label, text_col);
        }
    }

    if (draw_all) {
        auto draw_stick = [&](Zone z, float default_cx, float default_cy, float r) {
            float cx = default_cx, cy = default_cy, kx = default_cx, ky = default_cy;
            bool active = false;
            if (!editing) {
                for (const Finger& f : fingers_copy) {
                    if (f.zone != z) continue;
                    active = true;
                    cx = f.origin_x;
                    cy = f.origin_y;
                    float dx = f.cur_x - cx;
                    float dy = f.cur_y - cy;
                    float len = std::sqrt(dx * dx + dy * dy);
                    if (len > r && len > 0.0f) {
                        dx = dx / len * r;
                        dy = dy / len * r;
                    }
                    kx = cx + dx;
                    ky = cy + dy;
                    break;
                }
            }
            ImU32 ring_col = active ? IM_COL32(255, 210, 90, int(a * 230)) : IM_COL32(218, 226, 236, int(a * 145));
            ImU32 inner_ring = IM_COL32(218, 226, 236, int(a * 55));
            ImU32 bg_col = IM_COL32(16, 20, 28, int(a * 115));
            ImU32 knob_outer = active ? IM_COL32(255, 204, 72, int(a * 215)) : IM_COL32(220, 228, 238, int(a * 165));
            ImU32 knob_inner = active ? IM_COL32(255, 235, 165, int(a * 195)) : IM_COL32(42, 48, 60, int(a * 185));

            dl->AddCircleFilled(ImVec2(cx, cy), r, bg_col, 40);
            dl->AddCircle(ImVec2(cx, cy), r, ring_col, 40, 2.0f);
            dl->AddCircle(ImVec2(cx, cy), r * 0.66f, inner_ring, 32, 1.2f);
            // Subtle center guide crosshair
            const float g = r * 0.16f;
            dl->AddLine(ImVec2(cx - g, cy), ImVec2(cx + g, cy), inner_ring, 1.2f);
            dl->AddLine(ImVec2(cx, cy - g), ImVec2(cx, cy + g), inner_ring, 1.2f);
            // Dual-layer concave stick cap
            const float kr = r * 0.36f;
            dl->AddCircleFilled(ImVec2(kx, ky), kr, knob_outer, 28);
            dl->AddCircleFilled(ImVec2(kx, ky), kr * 0.68f, knob_inner, 24);
            dl->AddCircle(ImVec2(kx, ky), kr, IM_COL32(255, 255, 255, int(a * 190)), 28, 1.5f);
        };

        draw_stick(Zone::LeftStick, layout.ls_cx, layout.ls_cy, layout.ls_r);
        draw_stick(Zone::RightStick, layout.rs_cx, layout.rs_cy, layout.rs_r * 0.85f);
    }

    if (!editing) return;

    // Editor selection outlines
    for (int i = 0; i < int(Group::Count); ++i) {
        const GroupBounds& gb = layout.groups[i];
        bool is_sel = (i == int(selected));
        ImU32 ring = is_sel ? IM_COL32(255, 210, 64, 240) : IM_COL32(180, 195, 215, 90);
        dl->AddCircle(ImVec2(gb.cx, gb.cy), gb.hit_r + 6.0f, ring, 36, is_sel ? 3.5f : 1.5f);
    }

    // Compact floating editor toolbar at top center
    ImGui::SetNextWindowPos(ImVec2(view_w * 0.5f, 14.0f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.88f);
    ImGuiWindowFlags tb_flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove;
    if (ImGui::Begin("##TouchEditorBar", nullptr, tb_flags)) {
        ImVec2 wp = ImGui::GetWindowPos();
        ImVec2 ws = ImGui::GetWindowSize();
        {
            std::lock_guard<std::mutex> lock(g_lock);
            g_tb_x0 = wp.x;
            g_tb_y0 = wp.y;
            g_tb_x1 = wp.x + ws.x;
            g_tb_y1 = wp.y + ws.y;
        }

        ImGui::Text("%s %s (%s %d%%)", i18n::T(i18n::Str::TouchEditHint),
                    kGroupNames[int(selected)], i18n::T(i18n::Str::TouchEditSize),
                    int(std::lround(selected_scale * 100.0f)));
        ImGui::SameLine();
        if (ImGui::Button(" - ")) {
            std::lock_guard<std::mutex> lock(g_lock);
            g_groups[int(g_selected)].scale = std::max(0.60f, g_groups[int(g_selected)].scale - 0.10f);
        }
        ImGui::SameLine();
        if (ImGui::Button(" + ")) {
            std::lock_guard<std::mutex> lock(g_lock);
            g_groups[int(g_selected)].scale = std::min(1.60f, g_groups[int(g_selected)].scale + 0.10f);
        }
        ImGui::SameLine();
        if (ImGui::Button(i18n::T(i18n::Str::BtnResetTouchLayout))) {
            ResetLayout();
        }
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.14f, 0.19f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.86f, 0.20f, 0.25f, 1.0f));
        if (ImGui::Button(i18n::T(i18n::Str::TouchEditSaveExit))) {
            SaveLayout();
            SetEditing(false);
        }
        ImGui::PopStyleColor(2);
    }
    ImGui::End();
}

input::Pad GetPad() {
    std::lock_guard<std::mutex> lock(g_lock);
    return g_pad;
}

bool IsActive() {
    std::lock_guard<std::mutex> lock(g_lock);
    return g_active;
}

}  // namespace touch
