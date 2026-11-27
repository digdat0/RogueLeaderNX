// On-screen touch controls, for phones without a controller (Android; R2_TOUCH=1 shows them
// anywhere, for screenshots): a stick that starts where the left thumb lands, the GameCube's
// A / B / X / Y / Z / L / R / START, the d-pad (wingman commands) and a menu button, drawn over
// the picture with ImGui. Touches become port 1's controller, merged with a real one
// (input.cpp). They show after the screen is touched and hide when a controller or the
// keyboard is used, and never while the settings menu is open (the menu takes touches as
// mouse clicks).
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_video.h>
#include <aurora/aurora.h>
#include <dolphin/pad.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "../runtime.h"
#include "gx_bridge.h"

namespace rt::gfx {
namespace {

enum Ctl { Stick, A, B, X, Y, Z, L, R, Start, DUp, DDown, DLeft, DRight, MenuBtn, kCtlCount };
constexpr u16 kBits[kCtlCount] = {0, 0x0100, 0x0200, 0x0400, 0x0800, 0x0010, 0x0040, 0x0020, 0x1000,
                                  0x0008, 0x0004, 0x0001, 0x0002, 0};
const char* const kLabels[kCtlCount] = {"", "A", "B", "X", "Y", "Z", "L", "R", "START", "", "", "", "", ""};

struct Circle {
    float x, y, r;
};
struct Layout {
    Circle c[kCtlCount];
    float stickR, knobR;
};

// Positions in units of the screen's height (landscape), from the corners.
Layout layout(float w, float h) {
    Layout l{};
    const float u = std::min(w, h), m = 0.04f * u;
    l.stickR = 0.17f * u;
    l.knobR = 0.075f * u;
    l.c[Stick] = {m + 0.21f * u, h - m - 0.21f * u, l.stickR};
    const Circle a{w - m - 0.28f * u, h - m - 0.22f * u, 0.105f * u};
    l.c[A] = a;
    l.c[B] = {a.x - 0.19f * u, a.y + 0.11f * u, 0.07f * u};
    l.c[X] = {a.x + 0.20f * u, a.y - 0.06f * u, 0.07f * u};
    l.c[Y] = {a.x - 0.05f * u, a.y - 0.20f * u, 0.07f * u};
    l.c[Z] = {a.x + 0.20f * u, a.y - 0.23f * u, 0.055f * u};
    l.c[L] = {m + 0.10f * u, 0.32f * u, 0.075f * u};
    l.c[R] = {w - m - 0.10f * u, 0.32f * u, 0.075f * u};
    l.c[Start] = {w * 0.5f, m + 0.06f * u, 0.06f * u};
    const float dx = m + 0.36f * u, dy = 0.32f * u, ds = 0.075f * u, dr = 0.042f * u;
    l.c[DUp] = {dx, dy - ds, dr};
    l.c[DDown] = {dx, dy + ds, dr};
    l.c[DLeft] = {dx - ds, dy, dr};
    l.c[DRight] = {dx + ds, dy, dr};
    l.c[MenuBtn] = {m + 0.05f * u, m + 0.05f * u, 0.045f * u};
    return l;
}

struct Finger {
    SDL_FingerID id;
    int ctl;         // Stick, a button, or -1 (a touch on nothing)
    float x, y;      // where it is (screen points)
    float cx, cy;    // Stick: where the stick's middle is (where the thumb landed)
};

#ifdef __ANDROID__
constexpr bool kPlatform = true;
#else
constexpr bool kPlatform = false;
#endif
bool s_enabled = false, s_forced = false, s_visible = false;
float s_w = 0.f, s_h = 0.f;  // screen size in ImGui's points (the last frame's)
std::vector<Finger> s_fingers;

int hit(const Layout& l, float x, float y) {
    int best = -1;
    float bestD = 1e9f;
    for (int i = A; i < kCtlCount; i++) {
        const Circle& c = l.c[i];
        const float d = std::hypot(x - c.x, y - c.y);
        if (d <= c.r * 1.3f && d / c.r < bestD) best = i, bestD = d / c.r;
    }
    return best;
}

void screenSize() {
    if (s_w > 0.f) return;
    int w = 0, h = 0;
    if (SDL_Window* win = aurora_get_window()) SDL_GetWindowSize(win, &w, &h);
    s_w = float(w), s_h = float(h);
}

}  // namespace

void touchEvent(const void* p) {
    static bool init = false;
    if (!init) {
        init = true;
        s_forced = envFlag("R2_TOUCH");
        s_enabled = kPlatform || s_forced;
        s_visible = s_enabled;  // until a controller is used
    }
    if (!s_enabled) return;
    const SDL_Event& e = *static_cast<const SDL_Event*>(p);
    switch (e.type) {
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        if (!s_forced) s_visible = false, s_fingers.clear();
        return;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (!s_forced && std::abs(e.gaxis.value) > 20000) s_visible = false, s_fingers.clear();
        return;
    case SDL_EVENT_KEY_DOWN:
        if (!s_forced && e.key.key != SDLK_AC_BACK) s_visible = false, s_fingers.clear();
        return;
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_MOTION:
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_CANCELED:
        break;
    default:
        return;
    }
    if (menuOpen()) {
        s_fingers.clear();
        return;
    }
    screenSize();
    const SDL_TouchFingerEvent& t = e.tfinger;
    const float x = t.x * s_w, y = t.y * s_h;
    auto it = std::find_if(s_fingers.begin(), s_fingers.end(), [&](const Finger& f) { return f.id == t.fingerID; });
    if (e.type == SDL_EVENT_FINGER_UP || e.type == SDL_EVENT_FINGER_CANCELED) {
        if (it != s_fingers.end()) s_fingers.erase(it);
        return;
    }
    const Layout l = layout(s_w, s_h);
    if (e.type == SDL_EVENT_FINGER_DOWN) {
        s_visible = true;
        Finger f{t.fingerID, hit(l, x, y), x, y, 0.f, 0.f};
        if (f.ctl == MenuBtn) {
            // the menu, as the back button: menuEvent's own path
            SDL_Event k{};
            k.type = SDL_EVENT_KEY_DOWN;
            k.key.key = SDLK_AC_BACK;
            menuEvent(&k);
            s_fingers.clear();
            return;
        }
        // the lower left part of the screen (off the d-pad and L): the stick, centered there
        if (f.ctl < 0 && x < s_w * 0.5f && y > s_h * 0.45f) {
            f.ctl = Stick;
            f.cx = std::clamp(x, l.stickR * 0.6f, s_w * 0.5f);
            f.cy = std::clamp(y, s_h * 0.45f, s_h - l.stickR * 0.6f);
        }
        if (it != s_fingers.end()) *it = f;
        else s_fingers.push_back(f);
        return;
    }
    if (it == s_fingers.end()) return;  // motion
    it->x = x, it->y = y;
    if (it->ctl != Stick) {
        const int now = hit(l, x, y);  // a thumb slides from one button to the next
        if (now != MenuBtn) it->ctl = now;
    }
}

bool touchApply(void* padStatus) {
    if (!s_enabled || !s_visible || menuOpen()) return false;
    auto& st = *static_cast<PADStatus*>(padStatus);
    st.err = PAD_ERR_NONE;  // (the game asks for a controller on port 1 otherwise)
    screenSize();
    const Layout l = layout(s_w, s_h);
    for (const Finger& f : s_fingers) {
        if (f.ctl == Stick) {
            float dx = (f.x - f.cx) / l.stickR, dy = (f.y - f.cy) / l.stickR;
            const float len = std::hypot(dx, dy);
            if (len > 1.f) dx /= len, dy /= len;
            st.stickX = s8(std::lround(dx * 127.f));
            st.stickY = s8(std::lround(-dy * 127.f));
        } else if (f.ctl > 0 && f.ctl < kCtlCount) {
            st.button |= kBits[f.ctl];
            if (f.ctl == L) st.triggerLeft = 255;
            if (f.ctl == R) st.triggerRight = 255;
        }
    }
    return true;
}

void touchDraw() {
    ImGuiContext* ctx = ImGui::GetCurrentContext();
    if (!ctx || !ctx->WithinFrameScope) return;
    const ImGuiIO& io = ImGui::GetIO();
    s_w = io.DisplaySize.x, s_h = io.DisplaySize.y;
    if (!s_enabled || !s_visible || menuOpen() || s_w <= 0.f) return;
    const Layout l = layout(s_w, s_h);
    bool down[kCtlCount] = {};
    const Finger* stick = nullptr;
    for (const Finger& f : s_fingers) {
        if (f.ctl >= 0 && f.ctl < kCtlCount) down[f.ctl] = true;
        if (f.ctl == Stick) stick = &f;
    }
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    auto label = [&](const Circle& c, const char* s, float size, ImU32 col) {
        const ImVec2 ts = font->CalcTextSizeA(size, 1e9f, 0.f, s);
        dl->AddText(font, size, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), col, s);
    };
    auto button = [&](int i, ImU32 tint) {
        const Circle& c = l.c[i];
        const int fa = down[i] ? 110 : 34;
        dl->AddCircleFilled(ImVec2(c.x, c.y), c.r, (tint & 0x00FFFFFF) | (ImU32(fa) << 24), 40);
        dl->AddCircle(ImVec2(c.x, c.y), c.r, IM_COL32(255, 255, 255, down[i] ? 200 : 110), 40, std::max(1.5f, c.r * 0.05f));
    };
    // stick: its ring where it is held (else at rest), the knob under the thumb
    {
        const float cx = stick ? stick->cx : l.c[Stick].x, cy = stick ? stick->cy : l.c[Stick].y;
        dl->AddCircleFilled(ImVec2(cx, cy), l.stickR, IM_COL32(255, 255, 255, 22), 48);
        dl->AddCircle(ImVec2(cx, cy), l.stickR, IM_COL32(255, 255, 255, 90), 48, std::max(1.5f, l.stickR * 0.03f));
        float kx = cx, ky = cy;
        if (stick) {
            float dx = stick->x - cx, dy = stick->y - cy;
            const float len = std::hypot(dx, dy);
            if (len > l.stickR) dx *= l.stickR / len, dy *= l.stickR / len;
            kx += dx, ky += dy;
        }
        dl->AddCircleFilled(ImVec2(kx, ky), l.knobR, IM_COL32(255, 255, 255, stick ? 120 : 60), 32);
    }
    const ImU32 white = IM_COL32(255, 255, 255, 255), green = IM_COL32(40, 200, 140, 255), red = IM_COL32(230, 70, 70, 255);
    button(A, green);
    button(B, red);
    for (int i : {X, Y, Z, L, R, Start, DUp, DDown, DLeft, DRight, MenuBtn}) button(i, white);
    for (int i : {A, B, X, Y, Z, L, R}) label(l.c[i], kLabels[i], l.c[i].r * 0.95f, IM_COL32(255, 255, 255, 200));
    label(l.c[Start], "START", l.c[Start].r * 0.55f, IM_COL32(255, 255, 255, 200));
    // d-pad arrows, the menu's three lines
    for (int i : {DUp, DDown, DLeft, DRight}) {
        const Circle& c = l.c[i];
        const float s = c.r * 0.45f;
        ImVec2 p[3];
        switch (i) {
        case DUp: p[0] = {c.x, c.y - s}, p[1] = {c.x + s, c.y + s * 0.6f}, p[2] = {c.x - s, c.y + s * 0.6f}; break;
        case DDown: p[0] = {c.x, c.y + s}, p[1] = {c.x - s, c.y - s * 0.6f}, p[2] = {c.x + s, c.y - s * 0.6f}; break;
        case DLeft: p[0] = {c.x - s, c.y}, p[1] = {c.x + s * 0.6f, c.y - s}, p[2] = {c.x + s * 0.6f, c.y + s}; break;
        default: p[0] = {c.x + s, c.y}, p[1] = {c.x - s * 0.6f, c.y + s}, p[2] = {c.x - s * 0.6f, c.y - s}; break;
        }
        dl->AddTriangleFilled(p[0], p[1], p[2], IM_COL32(255, 255, 255, 200));
    }
    {
        const Circle& c = l.c[MenuBtn];
        for (int k = -1; k <= 1; k++)
            dl->AddLine(ImVec2(c.x - c.r * 0.45f, c.y + k * c.r * 0.35f), ImVec2(c.x + c.r * 0.45f, c.y + k * c.r * 0.35f),
                        IM_COL32(255, 255, 255, 210), std::max(1.5f, c.r * 0.1f));
    }
}

}  // namespace rt::gfx
