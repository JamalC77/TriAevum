// Pure state machines and math of the Quest immersive input and screen placement. No OpenXR, SDL or
// Vulkan types, so quest_xr_logic_tests can run them anywhere.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace TriAevumQuestXr::Logic {

// ---- Stick-click chords (vr-camera.md §7, q3-spec §7.3), sampled every display frame.
//  - L3 or R3 alone, released within kTapSeconds, with neither the other stick click nor the menu
//    button going down meanwhile: a tap (L3 -> Select, R3 -> the in-game menu).
//  - L3 and R3 held together for kRecenterSeconds: recenter the screen; no tap.
//  - L3 held, then the menu button pressed: toggle the F1 settings window; no Start and no Select.
//  - The menu button is Start, except a press that began as the F1 chord (until it is released).
//  - Deliberate menu (in-world, while the world shows; the right stick then steers Link when the left
//    one is idle): an R3 tap no longer opens the in-game menu. It opens on R3 held alone for
//    kMenuHoldSeconds with the right stick centred the whole time (rightStickIdle), fired once while
//    still held. A click while steering (stick deflected at any point of the hold), a short click, or
//    any other stick click or the menu button during the hold never opens it.
struct ChordOutput {
    bool SelectTap = false;
    bool MenuTap = false;  // R3 tap (deliberate mode: the R3 hold)
    bool Recenter = false;
    bool ToggleSettings = false;
    bool StartHeld = false;
};

class StickChords {
  public:
    static constexpr double kTapSeconds = 0.4;
    static constexpr double kRecenterSeconds = 1.0;
    static constexpr double kMenuHoldSeconds = 0.7;

    ChordOutput Update(double now, bool l3, bool r3, bool menu, bool deliberateMenu = false,
                       bool rightStickIdle = true) {
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
            mR3HoldFired = false;
            if (l3) mL3Spoiled = true;
        }
        if (deliberateMenu && r3 && !rightStickIdle) mR3Spoiled = true;
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
        if (deliberateMenu) {
            if (r3 && !mR3Spoiled && !mR3HoldFired && now - mR3Since >= kMenuHoldSeconds) {
                out.MenuTap = true;
                mR3HoldFired = true;
            }
        } else if (!r3 && mR3 && !mR3Spoiled && !mR3HoldFired && now - mR3Since <= kTapSeconds) {
            out.MenuTap = true;
        }
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
    bool mR3HoldFired = false;  // the deliberate hold fired: its release is no tap
    double mL3Since = 0.0, mR3Since = 0.0, mBothSince = -1.0;
};

// ---- Movement stick in-world (single-controller play): the Circle Pad takes the left stick, or the
// right stick when the left one is idle or its controller is not active; with both pushed, the larger
// deflection wins (a tie keeps the left). kStickIdle is the radius under which a stick counts as idle
// (the game's own dead zone is larger, so idle drift never moves Link).
// RightStickMove (inworld-spec §7): WhenLeftIdle is W1's policy (the right stick has no other job in
// world mode). Once the right stick has one (W2's snap turn and D-pad flicks), its caller passes
// WhenLeftInactive while that job is armed: the right stick then moves Link only while the left
// controller is not active (Jamal's one-controller case), so a flick never both turns and walks.
enum class StickSource : uint8_t { None, Left, Right };

inline const char* StickSourceName(StickSource source) {
    switch (source) {
    case StickSource::Left: return "left";
    case StickSource::Right: return "right";
    default: return "none";
    }
}

struct MoveStick {
    float X = 0.0F, Y = 0.0F;
    StickSource Source = StickSource::None;  // None: both sticks idle (X/Y = the left stick as is)
};

inline constexpr float kStickIdle = 0.15F;

inline bool StickIdle(float x, float y) {
    if (!std::isfinite(x) || !std::isfinite(y)) return true;
    return std::hypot(x, y) <= kStickIdle;
}

enum class RightStickMove : uint8_t { WhenLeftIdle, WhenLeftInactive };

// The right stick's jobs while the world shows. Flicks armed (W2: mode R, mode L): flicks only while the
// left controller is active, and the right stick moves Link only while it is not; flicks off (the native
// A/B baseline, W1): the right stick moves Link while the left stick is idle, never flicks.
struct RightStickJob {
    bool Flicks = false;
    RightStickMove Move = RightStickMove::WhenLeftIdle;
};
inline RightStickJob WorldRightStickJob(bool worldShown, bool flicksArmed, bool leftActive) {
    RightStickJob job;
    job.Move = flicksArmed ? RightStickMove::WhenLeftInactive : RightStickMove::WhenLeftIdle;
    job.Flicks = worldShown && flicksArmed && leftActive;
    return job;
}

inline MoveStick PickMoveStick(float lx, float ly, bool leftActive, float rx, float ry,
                               RightStickMove policy = RightStickMove::WhenLeftIdle) {
    if (!std::isfinite(lx) || !std::isfinite(ly)) lx = ly = 0.0F;
    if (!std::isfinite(rx) || !std::isfinite(ry)) rx = ry = 0.0F;
    if (!leftActive) lx = ly = 0.0F;
    if (leftActive && policy == RightStickMove::WhenLeftInactive) rx = ry = 0.0F;
    const float left = std::hypot(lx, ly), right = std::hypot(rx, ry);
    if (left <= kStickIdle && right <= kStickIdle) return {lx, ly, StickSource::None};
    if (right > left) return {rx, ry, StickSource::Right};
    return {lx, ly, StickSource::Left};
}

// The stick that last drove Link: Update returns true when it changes (left <-> right); idle
// periods do not count as a change.
struct StickSourceTracker {
    StickSource Last = StickSource::None;
    bool Update(StickSource source) {
        if (source == StickSource::None || source == Last) return false;
        Last = source;
        return true;
    }
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

inline float WrapRadians(float angle) {
    if (!std::isfinite(angle)) return 0.0F;
    constexpr float kTwoPi = 6.28318530718F;
    angle = std::fmod(angle + 3.14159265359F, kTwoPi);
    if (angle < 0.0F) angle += kTwoPi;
    return angle - 3.14159265359F;
}

// ---- In-world HUD (inworld-spec W4, minimal): a lazily body-locked quad in LOCAL. Its position follows
// the head every display frame (DistanceM ahead along the HUD yaw, PitchRadians below the horizon); its
// yaw stays put while the head turns less than DeadzoneRadians away and then catches up smoothly
// (exponential, CatchUpSeconds) until within SettleRadians, so the hearts and rings sit in the periphery
// instead of being glued to the face. The quad faces the head (yaw, then pitch).
// ---- HUD sizing defaults. The quad sits a little nearer than Link (the HUD is composited over the
// world, so stereo depth should agree: in front of Link, the usual subject), is ~39 deg wide (the
// hearts and rings read like a TV's corners), and the follow dead zone is measured so the quad's
// outer EDGE stays within kHudMaxEdgeFromGaze of the gaze before the catch-up starts (Quest 3's
// horizontal half-FOV is ~50 deg; the hearts in the top-left corner must not leave it while turning).
inline constexpr float kHudPlaceMarginM = 0.3F;           // HUD distance = place distance - this
inline constexpr float kHudMinDefaultDistanceM = 1.0F;
inline constexpr float kHudMaxDefaultDistanceM = 2.5F;
inline constexpr float kHudWidthPerDistance = 0.70F;      // width / distance: ~38.6 deg wide
inline constexpr float kHudMaxEdgeFromGazeRadians = 0.69813170F;  // 40 deg
inline constexpr float kHudMinDeadzoneRadians = 0.08726646F;      // 5 deg

// The default HUD distance for a head-to-Link placement distance (TRIAEVUM_XR_PLACE_M; 1.8 -> 1.5 m).
inline float DefaultHudDistanceM(float placeDistanceM) {
    const float place = std::isfinite(placeDistanceM) && placeDistanceM > 0.0F ? placeDistanceM : 1.8F;
    return std::clamp(place - kHudPlaceMarginM, kHudMinDefaultDistanceM, kHudMaxDefaultDistanceM);
}
inline float DefaultHudWidthM(float distanceM) { return distanceM * kHudWidthPerDistance; }
// Half the quad's horizontal angle seen from the head (at its centre distance).
inline float HudHalfAngleRadians(float widthM, float distanceM) {
    if (!(widthM > 0.0F) || !(distanceM > 0.0F)) return 0.0F;
    return std::atan(widthM * 0.5F / distanceM);
}
// The centre dead zone that keeps the quad's edge within kHudMaxEdgeFromGaze of the gaze (>= 5 deg).
inline float DefaultHudDeadzoneRadians(float widthM, float distanceM) {
    return std::max(kHudMinDeadzoneRadians, kHudMaxEdgeFromGazeRadians - HudHalfAngleRadians(widthM, distanceM));
}

struct HudFollowConfig {
    float DistanceM = 2.5F;               // the runtime default is DefaultHudDistanceM
    float PitchRadians = -0.13962634F;    // -8 deg: centred slightly below eye level
    float DeadzoneRadians = 0.61086524F;  // 35 deg (the runtime default is DefaultHudDeadzoneRadians)
    float CatchUpSeconds = 0.3F;
    float SettleRadians = 0.034906585F;   // 2 deg
};

struct HudFollow {
    bool Valid = false;
    bool Following = false;
    float Yaw = 0.0F;
    uint32_t Placements = 0;  // snaps (first frame, Reset) and catch-ups started

    // The next frame snaps the HUD in front of the head (entering the world, a recenter).
    void Reset() {
        Valid = false;
        Following = false;
    }

    ScreenPlacement Update(const Quat& head, const Vec3& headPosition, float dt, const HudFollowConfig& config) {
        const float headYaw = HeadYaw(head);
        const float safeDt = std::isfinite(dt) && dt > 0.0F ? dt : 0.0F;
        if (!Valid || !std::isfinite(Yaw)) {
            Yaw = headYaw;
            Valid = true;
            Following = false;
            ++Placements;
        } else {
            const float delta = WrapRadians(headYaw - Yaw);
            if (!Following && std::fabs(delta) > config.DeadzoneRadians) {
                Following = true;
                ++Placements;
            }
            if (Following) {
                const float k = config.CatchUpSeconds > 0.0F ? 1.0F - std::exp(-safeDt / config.CatchUpSeconds)
                                                             : (safeDt > 0.0F ? 1.0F : 0.0F);
                Yaw = WrapRadians(Yaw + delta * k);
                if (std::fabs(WrapRadians(headYaw - Yaw)) <= config.SettleRadians) Following = false;
            }
        }
        const float pitch = std::isfinite(config.PitchRadians) ? config.PitchRadians : 0.0F;
        const float distance = std::isfinite(config.DistanceM) ? config.DistanceM : 0.0F;
        ScreenPlacement placement;
        placement.YawRadians = Yaw;
        const float sy = std::sin(Yaw * 0.5F), cy = std::cos(Yaw * 0.5F);
        const float sp = std::sin(pitch * 0.5F), cp = std::cos(pitch * 0.5F);
        placement.Orientation = {cy * sp, sy * cp, -sy * sp, cy * cp};  // RotY(yaw) * RotX(pitch)
        const float horizontal = std::cos(pitch) * distance;
        placement.Position = {headPosition.X - std::sin(Yaw) * horizontal, headPosition.Y + std::sin(pitch) * distance,
                              headPosition.Z - std::cos(Yaw) * horizontal};
        return placement;
    }
};

// ---- In-world right stick (inworld-spec §5.5, §7), sampled every display frame while the world shows:
// X flicks are snap turns (+1 right, -1 left), Y flicks press the D-pad (+1 up, -1 down). A flick fires
// once past kTrigger on the dominant axis and re-arms below kRearm. After Reset (the world was not shown)
// a stick still held must come back to rest before it can fire.
struct FlickOutput {
    int Turn = 0;
    int Vertical = 0;
};
class StickFlicks {
  public:
    static constexpr float kTrigger = 0.7F;
    static constexpr float kRearm = 0.3F;
    FlickOutput Update(float x, float y) {
        if (!std::isfinite(x)) x = 0.0F;
        if (!std::isfinite(y)) y = 0.0F;
        const float ax = std::fabs(x), ay = std::fabs(y);
        FlickOutput out;
        if (mArmedX && ax > kTrigger && ax >= ay) {
            mArmedX = false;
            out.Turn = x > 0.0F ? 1 : -1;
        } else if (!mArmedX && ax < kRearm) {
            mArmedX = true;
        }
        if (mArmedY && ay > kTrigger && ay > ax) {
            mArmedY = false;
            out.Vertical = y > 0.0F ? 1 : -1;
        } else if (!mArmedY && ay < kRearm) {
            mArmedY = true;
        }
        return out;
    }
    void Reset() { mArmedX = mArmedY = false; }

  private:
    bool mArmedX = false, mArmedY = false;
};

// ---- The display clock's tick (inworld-spec §3.4): display periods since the session began, from the
// runtime's predicted display times, not a count of xrWaitFrame calls. A late wait that skips periods
// advances the tick by the periods that passed (counted in Skipped); a stopped session (Pause) does not
// count its gap. The tick stays monotonic.
struct DisplayTickClock {
    uint64_t Tick = 0;
    uint64_t Skipped = 0;  // periods skipped by late waits (the owner resets it per stats window)
    int64_t LastDisplayTime = 0;
    bool Running = false;
    // One successful xrWaitFrame: the tick of its display frame.
    uint64_t Advance(int64_t displayTime, int64_t period) {
        uint64_t step = 1;
        if (Running && period > 0 && displayTime > LastDisplayTime) {
            const double periods = static_cast<double>(displayTime - LastDisplayTime) / static_cast<double>(period);
            if (std::isfinite(periods) && periods > 1.5) step = static_cast<uint64_t>(std::llround(periods));
        }
        Skipped += step - 1U;
        Tick += step;
        LastDisplayTime = displayTime;
        Running = true;
        return Tick;
    }
    void Pause() { Running = false; }
};

// ---- Phase timing (TRIAEVUM_XR_PERF): a histogram of 0.25 ms buckets up to 128 ms (the last bucket is
// open), from which the 5 s window's p95 is read. The runtime keeps one per phase in atomics; these are
// the bucket index and the percentile over a plain copy of the counts.
constexpr double kPerfBucketMs = 0.25;
constexpr size_t kPerfBuckets = 512;

inline size_t PerfBucket(double milliseconds) {
    if (!(milliseconds > 0.0)) return 0;
    const double bucket = std::floor(milliseconds / kPerfBucketMs);
    return bucket >= static_cast<double>(kPerfBuckets - 1) ? kPerfBuckets - 1 : static_cast<size_t>(bucket);
}

// The upper edge of the bucket where the cumulative count reaches `fraction` of `total`, never above the
// window's maximum (the open last bucket reports the maximum). 0 for an empty window.
inline double PerfPercentileMs(const uint32_t* counts, size_t buckets, uint64_t total, double fraction,
                               double maxMs) {
    if (total == 0U || counts == nullptr || buckets == 0U) return 0.0;
    const double wanted = std::clamp(fraction, 0.0, 1.0) * static_cast<double>(total);
    uint64_t cumulative = 0;
    for (size_t i = 0; i < buckets; ++i) {
        cumulative += counts[i];
        if (static_cast<double>(cumulative) >= wanted && cumulative != 0U) {
            if (i + 1 == buckets) return maxMs;
            return std::min(static_cast<double>(i + 1) * kPerfBucketMs, maxMs);
        }
    }
    return maxMs;
}

// ---- The in-frame copy's swapchain images (in-world, TRIAEVUM_XR_FRAME_COPY). OpenXR lets an application
// wait only one image of a swapchain at a time ("the app must release before waiting on the next acquired
// swapchain image"), so each role (eyes, HUD) has two swapchains, set 0 and set 1, and each set holds at
// most one image: Empty -> acquired (Unwaited) -> waited (Ready) -> claimed by a frame that drew a pair
// (Claimed; the copy is recorded into that frame's own command buffer) -> committed with the frame's render
// value (Written) -> released by the XR thread once the render timeline reached it (Empty again). While one
// set's pair is still on the GPU the other set is Ready for the next frame. A claim that will not be
// published (abandoned, or an older renderer claim when a newer one commits) is an orphan: it is never
// released; once its writer is GPU-complete the still-waited image becomes Ready again (the next claim
// discards its content), so an orphan can neither be shown nor block a set.
template <typename Payload>
class HeldImageSets {
  public:
    enum class Stage : uint8_t { Empty, Unwaited, Ready, Claimed, Written };
    struct Entry {
        int Set = -1;
        int Index = -1;
        Stage State = Stage::Empty;
        uint64_t Token = 0;
        uint64_t RenderValue = 0;
        bool Orphan = false;
        bool Renderer = false;  // claimed by the renderer's frame (not the XR queue's fallback copy)
        Payload Data{};
    };
    struct ClaimResult {
        int Set = -1;
        int Index = -1;
        explicit operator bool() const { return Set >= 0; }
    };

    explicit HeldImageSets(size_t sets = 2) { Reset(sets); }
    void Reset(size_t sets) {
        mEntries.assign(std::max<size_t>(1, sets), Entry{});
        for (size_t i = 0; i < mEntries.size(); ++i) mEntries[i].Set = static_cast<int>(i);
    }
    void Clear() { Reset(mEntries.size()); }
    size_t Sets() const { return mEntries.size(); }
    const Entry& At(size_t set) const { return mEntries[set]; }

    bool NeedsAcquire(size_t set) const { return set < mEntries.size() && mEntries[set].State == Stage::Empty; }
    void Acquired(size_t set, int index) {
        if (!NeedsAcquire(set)) return;
        mEntries[set].State = Stage::Unwaited;
        mEntries[set].Index = index;
    }
    // The set's only image is acquired and not yet waited (no other image of that swapchain is waited).
    bool NeedsWait(size_t set) const { return set < mEntries.size() && mEntries[set].State == Stage::Unwaited; }
    bool Waited(size_t set) {
        if (!NeedsWait(set)) return false;
        mEntries[set].State = Stage::Ready;
        return true;
    }
    bool HasState(Stage state) const {
        return std::any_of(mEntries.begin(), mEntries.end(), [state](const Entry& e) { return e.State == state; });
    }
    bool HasReady() const { return HasState(Stage::Ready); }
    // A renderer frame holds a claim it has not committed yet: its pair is newer than any XR-queue publication.
    bool HasRendererClaim() const {
        return std::any_of(mEntries.begin(), mEntries.end(),
                           [](const Entry& e) { return e.State == Stage::Claimed && e.Renderer; });
    }
    // Claims a Ready image for `token` (non-zero); none when no set is ready.
    ClaimResult Claim(uint64_t token, bool renderer = true) {
        if (token == 0U) return {};
        for (auto& e : mEntries)
            if (e.State == Stage::Ready) {
                e.State = Stage::Claimed;
                e.Token = token;
                e.Renderer = renderer;
                e.Orphan = false;
                return {e.Set, e.Index};
            }
        return {};
    }
    // The claim of `token` was written by a submission that signals `renderValue` (0: already complete).
    // With `orphanOlder` (the renderer, whose claims commit in order), older renderer claims become orphans
    // of the same value: their content is no publication's. The XR queue's own claims are never touched.
    bool Commit(uint64_t token, uint64_t renderValue, const Payload& data, bool orphanOlder = true) {
        bool found = false;
        for (auto& e : mEntries) {
            if (e.State != Stage::Claimed) continue;
            if (e.Token == token) {
                e.State = Stage::Written;
                e.RenderValue = renderValue;
                e.Data = data;
                found = true;
            } else if (orphanOlder && e.Renderer && e.Token < token) {
                e.State = Stage::Written;
                e.RenderValue = renderValue;
                e.Orphan = true;
            }
        }
        return found;
    }
    // The claim of `token` will not be published (a failed copy, a frame without its pair): an orphan whose
    // writer completes at `renderValue` (0: never written), Ready again from then on.
    bool Abandon(uint64_t token, uint64_t renderValue) {
        for (auto& e : mEntries)
            if (e.State == Stage::Claimed && e.Token == token) {
                e.State = Stage::Written;
                e.RenderValue = renderValue;
                e.Orphan = true;
                return true;
            }
        return false;
    }
    // Pops the written images whose render value the timeline reached (`completed`), in set order; their sets
    // become Empty. Completed orphans become Ready again instead (still waited, never released).
    std::vector<Entry> TakeReleasable(uint64_t completed) {
        std::vector<Entry> out;
        for (auto& e : mEntries) {
            if (e.State != Stage::Written || e.RenderValue > completed) continue;
            if (e.Orphan) {
                e.State = Stage::Ready;
                e.Token = 0;
                e.RenderValue = 0;
                e.Orphan = false;
                e.Renderer = false;
                ++mRecycled;
                continue;
            }
            out.push_back(e);
            const int set = e.Set;
            e = Entry{};
            e.Set = set;
        }
        return out;
    }
    uint64_t TakeRecycled() { return std::exchange(mRecycled, uint64_t{0}); }

  private:
    std::vector<Entry> mEntries;
    uint64_t mRecycled = 0;
};

}  // namespace TriAevumQuestXr::Logic
