// Pure state machines and math of the Quest immersive input and screen placement. No OpenXR, SDL or
// Vulkan types, so quest_xr_logic_tests can run them anywhere.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace TriAevumQuestXr::Logic {

// ---- Stick-click chords (vr-camera.md §7, q3-spec §7.3), sampled every display frame.
//  - L3 or R3 alone, released within kTapSeconds, with neither the other stick click nor the menu
//    button going down meanwhile: a tap (L3 -> Select, R3 -> the in-game menu).
//  - L3 and R3 held together for kRecenterSeconds: recenter the screen; no tap.
//  - L3 held, then the menu button pressed: toggle the F1 settings window; no Start and no Select.
//  - The menu button is Start, except a press that began as the F1 chord (until it is released).
struct ChordOutput {
    bool SelectTap = false;
    bool MenuTap = false;  // R3 tap
    bool Recenter = false;
    bool ToggleSettings = false;
    bool StartHeld = false;
};

class StickChords {
  public:
    static constexpr double kTapSeconds = 0.4;
    static constexpr double kRecenterSeconds = 1.0;

    ChordOutput Update(double now, bool l3, bool r3, bool menu) {
        ChordOutput out;
        const bool l3Pressed = l3 && !mL3;
        const bool r3Pressed = r3 && !mR3;
        const bool menuPressed = menu && !mMenu;
        if (l3Pressed) {
            mL3Since = now;
            mL3Spoiled = r3;
            if (r3) mR3Spoiled = true;
        }
        if (r3Pressed) {
            mR3Since = now;
            mR3Spoiled = l3;
            if (l3) mL3Spoiled = true;
        }
        if (menuPressed) {
            if (l3) {
                out.ToggleSettings = true;
                mL3Spoiled = true;
                mMenuChord = true;
            }
            if (r3) mR3Spoiled = true;
        }
        if (l3 && r3) {
            if (mBothSince < 0.0) mBothSince = now;
            if (!mRecenterFired && now - mBothSince >= kRecenterSeconds) {
                out.Recenter = true;
                mRecenterFired = true;
            }
        } else {
            mBothSince = -1.0;
        }
        if (!l3 && !r3) mRecenterFired = false;
        if (!l3 && mL3 && !mL3Spoiled && now - mL3Since <= kTapSeconds) out.SelectTap = true;
        if (!r3 && mR3 && !mR3Spoiled && now - mR3Since <= kTapSeconds) out.MenuTap = true;
        if (!menu) mMenuChord = false;
        out.StartHeld = menu && !mMenuChord;
        mL3 = l3;
        mR3 = r3;
        mMenu = menu;
        return out;
    }

    void Reset() { *this = StickChords{}; }

  private:
    bool mL3 = false, mR3 = false, mMenu = false;
    bool mL3Spoiled = false, mR3Spoiled = false, mMenuChord = false, mRecenterFired = false;
    double mL3Since = 0.0, mR3Since = 0.0, mBothSince = -1.0;
};

// A synthetic button press that lasts long enough for a 30 Hz guest poll to see it.
struct Pulse {
    static constexpr double kSeconds = 0.12;
    double Until = -1.0;
    void Fire(double now) { Until = now + kSeconds; }
    bool Held(double now) const { return now < Until; }
};

// Presses shorter than the game's poll period: newly pressed bits stay pending until taken.
struct PressLatch {
    uint32_t Pending = 0;
    uint32_t Previous = 0;
    void Observe(uint32_t held) {
        Pending |= held & ~Previous;
        Previous = held;
    }
    uint32_t Take(uint32_t held) {
        const uint32_t result = held | Pending;
        Pending = 0;
        return result;
    }
    void Reset() { Pending = Previous = 0; }
};

// Analog value with press/release hysteresis (grips).
struct Hysteresis {
    float Press = 0.55F;
    float Release = 0.45F;
    bool On = false;
    bool Update(float value) {
        On = On ? value > Release : value >= Press;
        return On;
    }
};

inline int16_t AxisToSdl(float value) {
    if (!std::isfinite(value)) return 0;
    return static_cast<int16_t>(std::lround(std::clamp(value, -1.0F, 1.0F) * 32767.0F));
}

inline int16_t TriggerToSdl(float value) {
    if (!std::isfinite(value)) return 0;
    return static_cast<int16_t>(std::lround(std::clamp(value, 0.0F, 1.0F) * 32767.0F));
}

// ---- Screen placement: a world-locked quad facing the head's yaw, at head height.
struct Vec3 {
    float X = 0.0F, Y = 0.0F, Z = 0.0F;
};
struct Quat {
    float X = 0.0F, Y = 0.0F, Z = 0.0F, W = 1.0F;
};
struct ScreenPlacement {
    Quat Orientation;
    Vec3 Position;
    float YawRadians = 0.0F;
};

// OpenXR convention: -Z forward, +Y up. yaw 0 looks down -Z; positive yaw turns left.
inline float HeadYaw(const Quat& q) {
    // forward = R * (0, 0, -1), up = R * (0, 1, 0)
    const float fx = -2.0F * (q.X * q.Z + q.Y * q.W);
    const float fy = -2.0F * (q.Y * q.Z - q.X * q.W);
    const float fz = -(1.0F - 2.0F * (q.X * q.X + q.Y * q.Y));
    if (std::hypot(fx, fz) > 0.05F) return std::atan2(-fx, -fz);
    // Looking almost straight up or down: the head's up vector carries the yaw.
    const float ux = 2.0F * (q.X * q.Y - q.Z * q.W);
    const float uz = 2.0F * (q.Y * q.Z + q.X * q.W);
    return fy < 0.0F ? std::atan2(-ux, -uz) : std::atan2(ux, uz);
}

inline ScreenPlacement PlaceScreen(const Quat& headOrientation, const Vec3& headPosition, float distance) {
    ScreenPlacement placement;
    const float yaw = HeadYaw(headOrientation);
    placement.YawRadians = yaw;
    placement.Orientation = {0.0F, std::sin(yaw * 0.5F), 0.0F, std::cos(yaw * 0.5F)};
    placement.Position = {headPosition.X - std::sin(yaw) * distance, headPosition.Y,
                          headPosition.Z - std::cos(yaw) * distance};
    return placement;
}

inline float ScreenHeight(float width, uint32_t pixelWidth, uint32_t pixelHeight) {
    return pixelWidth == 0U ? 0.0F : width * static_cast<float>(pixelHeight) / static_cast<float>(pixelWidth);
}

}  // namespace TriAevumQuestXr::Logic
