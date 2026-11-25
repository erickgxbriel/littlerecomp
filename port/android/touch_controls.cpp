#include "touch_controls.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>

#include <imgui.h>

namespace app {
void ToggleOverlay();
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
    const char* label;
    float cx, cy, r;
};

struct Layout {
    std::vector<CircleBtn> buttons;
    float ls_cx, ls_cy, ls_r;
    float rs_cx, rs_cy, rs_r;
};

Layout ComputeLayout(float w, float h) {
    Layout l{};
    const float safe_x = std::max(44.0f, w * 0.032f);
    const float r_face = std::clamp(h * 0.062f, 30.0f, 82.0f);
    const float r_dpad = std::clamp(h * 0.052f, 26.0f, 70.0f);
    const float r_trig = std::clamp(h * 0.052f, 26.0f, 70.0f);
    const float r_util = std::clamp(h * 0.042f, 22.0f, 56.0f);

    l.ls_r = std::clamp(h * 0.145f, 70.0f, 190.0f);
    l.ls_cx = safe_x + l.ls_r * 1.15f;
    l.ls_cy = h * 0.74f;

    l.rs_r = l.ls_r;
    l.rs_cx = w - safe_x - l.rs_r * 1.40f;
    l.rs_cy = h * 0.77f;

    // Top bar: BACK/START center, MENU pinned cleanly to top-right corner
    const float top_y = std::max(r_util + 12.0f, h * 0.060f);
    l.buttons.push_back({Zone::BackPill, "BACK", w * 0.43f, top_y, r_util});
    l.buttons.push_back({Zone::StartPill, "START", w * 0.57f, top_y, r_util});
    l.buttons.push_back({Zone::MenuPill, "MENU", w - safe_x - r_util * 1.0f, top_y, r_util});

    // Top-left & top-right triggers placed with generous clearance below top bar & above face buttons
    const float trig_y = std::max(top_y + r_util + r_trig + 18.0f, h * 0.175f);
    l.buttons.push_back({Zone::BtnLT, "LT", safe_x + r_trig * 1.0f, trig_y, r_trig});
    l.buttons.push_back({Zone::BtnLB, "LB", safe_x + r_trig * 3.1f, trig_y, r_trig});

    l.buttons.push_back({Zone::BtnRT, "RT", w - safe_x - r_trig * 1.0f, trig_y, r_trig});
    l.buttons.push_back({Zone::BtnRB, "RB", w - safe_x - r_trig * 3.1f, trig_y, r_trig});

    // D-pad (left middle)
    const float dp_cx = safe_x + r_dpad * 2.1f;
    const float dp_cy = h * 0.42f;
    const float dp_off = r_dpad * 1.45f;
    l.buttons.push_back({Zone::DpadUp, "^", dp_cx, dp_cy - dp_off, r_dpad});
    l.buttons.push_back({Zone::DpadDown, "v", dp_cx, dp_cy + dp_off, r_dpad});
    l.buttons.push_back({Zone::DpadLeft, "<", dp_cx - dp_off, dp_cy, r_dpad});
    l.buttons.push_back({Zone::DpadRight, ">", dp_cx + dp_off, dp_cy, r_dpad});

    // Face diamond (right middle: Xbox colors/positions: Y top, B right, A bottom, X left)
    const float fc_cx = w - safe_x - r_face * 2.35f;
    const float fc_cy = h * 0.46f;
    const float fc_off = r_face * 1.55f;
    l.buttons.push_back({Zone::BtnY, "Y", fc_cx, fc_cy - fc_off, r_face});
    l.buttons.push_back({Zone::BtnB, "B", fc_cx + fc_off, fc_cy, r_face});
    l.buttons.push_back({Zone::BtnA, "A", fc_cx, fc_cy + fc_off, r_face});
    l.buttons.push_back({Zone::BtnX, "X", fc_cx - fc_off, fc_cy, r_face});

    // L3 / R3 at bottom inner edges
    l.buttons.push_back({Zone::L3Btn, "L3", l.ls_cx + l.ls_r * 1.35f, h * 0.87f, r_util});
    l.buttons.push_back({Zone::R3Btn, "R3", l.rs_cx - l.rs_r * 1.35f, h * 0.87f, r_util});

    return l;
}

struct Finger {
    SDL_FingerID id = 0;
    Zone zone = Zone::None;
    float origin_x = 0.0f;
    float origin_y = 0.0f;
    float cur_x = 0.0f;
    float cur_y = 0.0f;
};

std::mutex g_lock;
std::vector<Finger> g_fingers;
input::Pad g_pad{};
bool g_active = false;

Zone HitTest(const Layout& l, float px, float py, bool enabled, float w, float h) {
    // Menu pill is always interactive even when virtual controls are hidden.
    for (const CircleBtn& b : l.buttons) {
        if (!enabled && b.zone != Zone::MenuPill) continue;
        float dx = px - b.cx;
        float dy = py - b.cy;
        float hit_r = b.r * 1.25f;
        if (dx * dx + dy * dy <= hit_r * hit_r) return b.zone;
    }
    if (!enabled) return Zone::None;
    if (px < w * 0.44f && py > h * 0.45f) return Zone::LeftStick;
    if (px >= w * 0.44f && py > h * 0.24f) return Zone::RightStick;
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
                    ly = std::clamp(-dy, -1.0f, 1.0f);  // +Y is up
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

}  // namespace

bool HandleEvent(const SDL_Event& e, float view_w, float view_h, bool overlay_open, bool enabled) {
    if (e.type != SDL_EVENT_FINGER_DOWN && e.type != SDL_EVENT_FINGER_MOTION &&
        e.type != SDL_EVENT_FINGER_UP && e.type != SDL_EVENT_FINGER_CANCELED) {
        return false;
    }
    if (overlay_open || view_w <= 0.0f || view_h <= 0.0f) return false;

    const float px = e.tfinger.x * view_w;
    const float py = e.tfinger.y * view_h;
    const Layout layout = ComputeLayout(view_w, view_h);

    bool toggle_menu = false;
    bool consumed = false;

    {
        std::lock_guard<std::mutex> lock(g_lock);
        if (e.type == SDL_EVENT_FINGER_DOWN) {
            Zone z = HitTest(layout, px, py, enabled, view_w, view_h);
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

    const Layout layout = ComputeLayout(view_w, view_h);
    const float a = std::clamp(opacity, 0.1f, 0.95f);

    std::lock_guard<std::mutex> lock(g_lock);
    for (const CircleBtn& b : layout.buttons) {
        if (!enabled && b.zone != Zone::MenuPill) continue;
        bool pressed = IsZonePressedLocked(b.zone);
        ImU32 fill = pressed ? IM_COL32(255, 196, 64, int(a * 170)) : IM_COL32(24, 28, 36, int(a * 140));
        ImU32 border = pressed ? IM_COL32(255, 224, 120, int(a * 240)) : IM_COL32(220, 226, 235, int(a * 185));
        ImU32 text_col = IM_COL32(255, 255, 255, int(std::min(1.0f, a + 0.25f) * 255));

        dl->AddCircleFilled(ImVec2(b.cx, b.cy), b.r, fill, 28);
        dl->AddCircle(ImVec2(b.cx, b.cy), b.r, border, 28, 2.0f);
        ImVec2 ts = ImGui::CalcTextSize(b.label);
        dl->AddText(ImVec2(b.cx - ts.x * 0.5f, b.cy - ts.y * 0.5f), text_col, b.label);
    }

    if (!enabled) return;

    auto draw_stick = [&](Zone z, float default_cx, float default_cy, float r) {
        float cx = default_cx, cy = default_cy, kx = default_cx, ky = default_cy;
        bool active = false;
        for (const Finger& f : g_fingers) {
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
        ImU32 ring_col = active ? IM_COL32(255, 210, 90, int(a * 220)) : IM_COL32(220, 226, 235, int(a * 130));
        ImU32 bg_col = IM_COL32(20, 24, 30, int(a * 100));
        ImU32 knob_col = active ? IM_COL32(255, 210, 90, int(a * 200)) : IM_COL32(235, 240, 245, int(a * 140));
        dl->AddCircleFilled(ImVec2(cx, cy), r, bg_col, 36);
        dl->AddCircle(ImVec2(cx, cy), r, ring_col, 36, 2.0f);
        dl->AddCircleFilled(ImVec2(kx, ky), r * 0.36f, knob_col, 24);
    };

    draw_stick(Zone::LeftStick, layout.ls_cx, layout.ls_cy, layout.ls_r);
    draw_stick(Zone::RightStick, layout.rs_cx, layout.rs_cy, layout.rs_r * 0.85f);
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
