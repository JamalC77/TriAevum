// oot3d_xr_camera_logic.h: the pure decisions of mode R, the in-world mode's head-driven game camera
// (quest-research/inworld-spec.md §4.2-4.4, §6, §7, W2). Header-only, std and oot3d_xr_math.h only, so
// oot3d_xr_camera_logic_tests runs it on any host (PC, or the headset through adb).
//
//  - the camera write of one Camera_Update from the head in the game world (§4.2);
//  - who owns the main camera this update and why: the §4.3 rows on the guest's own flags, so a first
//    person or door frame is native before the frame classifier has seen it;
//  - the cutscene-camera motion test that sends a Held cutscene to Cinema (§4.3);
//  - head inside geometry from one BgCheck segment per update (§4.4);
//  - the placement probes behind Link and their choice (§4.4);
//  - the stick reference psi_ref and the Circle Pad rotation (§7);
//  - R_always: actors near the head are drawn whatever the culling volume says (§6);
//  - the PC preview's head that walks a circle around Link (§2.7, W2 day 5).
//
// Frames are oot3d_xr_math.h's: game world in game units (u), yaw = atan2(dx, dz) (increasing turns left),
// the VR world is the game world through the mirror M. Pitch is up-positive (the decomp's OLib "geo"
// pitch: eye -> at = (cos p sin y, sin p, cos p cos y)).
#pragma once

#include "oot3d_xr_math.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace Oot3dNativeGame::XrCamera {

namespace M = Oot3dNativeGame::XrMath;

inline constexpr double kDefaultCullFovDegrees = 130.0;  // §6: play+0x198 while the head owns the camera
inline constexpr double kMinCullFovDegrees = 60.0;
inline constexpr double kMaxCullFovDegrees = 150.0;
inline constexpr double kPitchLimitDegrees = 60.0;        // §4.2
inline constexpr double kLookDistanceU = 100.0;           // at = head + 100 u * forward
inline constexpr double kAlwaysDrawRadiusM = 4.0;         // §6 R_always
inline constexpr double kCirclePadRadius = 154.0;         // ThreeDsRecomp::Input::kNativeStickMaximum

// ---------------------------------------------------------------------------------------------------
// The head in the game world (§4.2: head_w = ToGameWorld(R_rig * head_T(T_N)))

struct GameHead {
    bool Valid = false;
    M::Vec3 Position;      // u
    M::Vec3 Forward;       // unit gaze
    M::Vec3 LevelForward;  // unit horizontal gaze (stable looking straight up or down)
    double Yaw = 0.0;      // atan2 of the level gaze, game world
    double VrYaw = 0.0;    // the same in the VR world (psi_ref lives there, §7)
};

inline GameHead HeadInGame(const M::Rig& rig, const M::Pose& headT, bool mirrored) {
    GameHead head;
    const M::WorldHead vr = M::HeadToWorld(rig, headT);
    const M::Vec3 levelVr = M::NormalizedXZ(
        M::TrackingDirectionToWorld(rig, M::HorizontalForward(headT.Orientation)), {0.0, 0.0, 1.0});
    head.Position = M::ToGameWorld(vr.Position, mirrored);
    head.Forward = M::ToGameWorld(vr.Forward, mirrored);
    head.LevelForward = M::ToGameWorld(levelVr, mirrored);
    head.Yaw = std::atan2(head.LevelForward.X, head.LevelForward.Z);
    head.VrYaw = std::atan2(levelVr.X, levelVr.Z);
    head.Valid = M::IsFinite(head.Position) && M::IsFinite(head.Forward) && M::IsFinite(head.LevelForward) &&
                 std::isfinite(head.Yaw) && std::isfinite(head.VrYaw);
    return head;
}

// ---------------------------------------------------------------------------------------------------
// The camera write (§4.2): eye = head, at = head + 100 u * dir(yaw, pitch); yaw from the level gaze,
// pitch clamped to +-60 deg, roll never written. Binary angles round(rad * 32768 / pi).

struct CameraWrite {
    bool Valid = false;
    M::Vec3 Eye, At;
    double Yaw = 0.0, Pitch = 0.0;  // radians
    int16_t YawBin = 0, PitchBin = 0;
};

inline CameraWrite BuildCameraWrite(const M::Vec3& eye, const M::Vec3& forward, const M::Vec3& levelForward,
                                    double pitchLimitDegrees = kPitchLimitDegrees) {
    CameraWrite write;
    if (!M::IsFinite(eye) || !M::IsFinite(forward) || !M::IsFinite(levelForward) || !std::isfinite(pitchLimitDegrees))
        return write;
    if (!(M::LengthXZ(levelForward) > 1e-9)) return write;
    write.Yaw = std::atan2(levelForward.X, levelForward.Z);
    const double length = M::Length(forward);
    const double pitch = length > 1e-9 ? std::asin(std::clamp(forward.Y / length, -1.0, 1.0)) : 0.0;
    const double limit = M::DegreesToRadians(std::clamp(pitchLimitDegrees, 0.0, 89.0));
    write.Pitch = std::clamp(pitch, -limit, limit);
    const M::Vec3 direction{std::cos(write.Pitch) * std::sin(write.Yaw), std::sin(write.Pitch),
                            std::cos(write.Pitch) * std::cos(write.Yaw)};
    write.Eye = eye;
    write.At = eye + direction * kLookDistanceU;
    write.YawBin = M::RadiansToBinAngle(write.Yaw);
    write.PitchBin = M::RadiansToBinAngle(write.Pitch);
    write.Valid = true;
    return write;
}
inline CameraWrite BuildCameraWrite(const GameHead& head, double pitchLimitDegrees = kPitchLimitDegrees) {
    if (!head.Valid) return {};
    return BuildCameraWrite(head.Position, head.Forward, head.LevelForward, pitchLimitDegrees);
}

// The camera's own angles from eye -> at (a probe update keeps the camera where it is and writes these).
inline CameraWrite HoldCameraWrite(const M::Vec3& eye, const M::Vec3& at) {
    CameraWrite write;
    const M::Vec3 d = at - eye;
    if (!M::IsFinite(d) || !(M::Length(d) > 1e-6)) return write;
    write.Eye = eye;
    write.At = at;
    write.Yaw = std::atan2(d.X, d.Z);
    write.Pitch = std::atan2(d.Y, M::LengthXZ(d));
    write.YawBin = M::RadiansToBinAngle(write.Yaw);
    write.PitchBin = M::RadiansToBinAngle(write.Pitch);
    write.Valid = true;
    return write;
}

// The written pitch stops at +-60 deg but the head does not: looking past it, the eye's far edge (Quest 3:
// about 50 deg below and above the gaze; kEyeEdgeDegrees with margin) leaves the cull frustum
// (limit + fov / 2) once |pitch| > limit + fov / 2 - edge (75 deg at 130). The FOV then widens just enough to
// cover it, up to kMaxCullFovDegrees (straight down keeps a strip of about 5 deg toward his back).
inline constexpr double kEyeEdgeDegrees = 55.0;
inline double CullFovForPitch(double cullFovDegrees, double headPitch, double pitchLimitDegrees = kPitchLimitDegrees,
                              double eyeEdgeDegrees = kEyeEdgeDegrees) {
    if (!std::isfinite(cullFovDegrees)) return kDefaultCullFovDegrees;
    if (!std::isfinite(headPitch) || !std::isfinite(pitchLimitDegrees) || !std::isfinite(eyeEdgeDegrees))
        return cullFovDegrees;
    const double pitch = std::fabs(M::RadiansToDegrees(headPitch));
    const double limit = std::clamp(pitchLimitDegrees, 0.0, 89.0);
    if (pitch <= limit) return cullFovDegrees;
    const double needed = 2.0 * (std::min(pitch, 90.0) + eyeEdgeDegrees - limit);
    return std::clamp(needed, cullFovDegrees, std::max(cullFovDegrees, kMaxCullFovDegrees));
}
// The head's pitch (up-positive) from its gaze.
inline double HeadPitch(const GameHead& head) {
    const double length = M::Length(head.Forward);
    if (!head.Valid || !(length > 1e-9)) return 0.0;
    return std::asin(std::clamp(head.Forward.Y / length, -1.0, 1.0));
}

// TRIAEVUM_XR_CULL_FOV: degrees, clamped; anything unparsable keeps the default.
inline double ParseCullFov(const char* text) {
    if (text == nullptr || *text == '\0') return kDefaultCullFovDegrees;
    char* end = nullptr;
    const double value = std::strtod(text, &end);
    if (end == text || !std::isfinite(value)) return kDefaultCullFovDegrees;
    return std::clamp(value, kMinCullFovDegrees, kMaxCullFovDegrees);
}

// ---------------------------------------------------------------------------------------------------
// Ownership of the main camera (§4.3), decided at Camera_Update entry from the guest's own flags.
// Held = the compiled native Camera_Update runs; the picture still comes from the rig.

enum HeldReason : uint32_t {
    kHeldLayer = 1U << 0,            // the layer is not World (Cinema, Black without a placement probe)
    kHeldNoPlayer = 1U << 1,
    kHeldNoHead = 1U << 2,           // no rig or no head pose for the target time
    kHeldCinemaRow = 1U << 3,        // a Cinema/Black row of §4.3 (first person, door, Epona, crawl, fixed ...)
    kHeldSecondaryCamera = 1U << 4,  // play+0xA64 != 0
    kHeldCutscene = 1U << 5,         // flags1 0x20000000
    kHeldItem = 1U << 6,             // flags1 0x400, 0x10000000 (item get, item cutscene)
    kHeldSetting = 1U << 7,          // settings 0x14, 0x15, 0x23, 0x40, 0x46
    kHeldScene = 1U << 8,            // scene 0x45
    kHeldStatus = 1U << 9,           // camera status != 7
    kHeldState = 1U << 10,           // flags1 0x20, 0x200
    kHeldNotMain = 1U << 11,         // another camera than play+0x364 (never the host's)
    kHeldConfig = 1U << 12,          // TRIAEVUM_XR_CAMERA=held, or no TopScreen profile (mode L)
};
inline constexpr uint32_t kHeldStaticMask = kHeldSecondaryCamera | kHeldCutscene | kHeldItem;
// While a placement waits under black, its probes take the update whatever else holds it (the scene-entry
// walk-in's 0x20000000, a status != 7 just after a load, an item get ...): skipping up to five native camera
// updates under black is invisible. Only these hold the probes off: no main camera, no Link to probe from,
// a screen row (the classifier leaves World for it anyway) and mode L.
inline constexpr uint32_t kHeldProbeBlockMask = kHeldNotMain | kHeldNoPlayer | kHeldCinemaRow | kHeldConfig;

inline std::string HeldNames(uint32_t held) {
    static constexpr std::pair<uint32_t, const char*> kNames[] = {
        {kHeldLayer, "layer"},       {kHeldNoPlayer, "no_player"},
        {kHeldNoHead, "no_head"},    {kHeldCinemaRow, "cinema_row"},
        {kHeldSecondaryCamera, "secondary_camera"}, {kHeldCutscene, "cutscene"},
        {kHeldItem, "item"},         {kHeldSetting, "setting"},
        {kHeldScene, "scene"},       {kHeldStatus, "status"},
        {kHeldState, "state"},       {kHeldNotMain, "not_main"},
        {kHeldConfig, "config"},
    };
    std::string text;
    for (const auto& [bit, name] : kNames) {
        if ((held & bit) == 0U) continue;
        if (!text.empty()) text += '|';
        text += name;
    }
    return text.empty() ? "none" : text;
}

inline bool HeldSetting(int16_t setting) {
    switch (setting) {
    case 0x14:
    case 0x15:
    case 0x23:
    case 0x40:
    case 0x46: return true;
    default: return false;
    }
}

// The §4.3 rows that end in Cinema or Black (the frame classifier switches the layer a frame later).
inline bool CinemaRow(uint32_t flags1, uint32_t flags2, int16_t setting, int32_t cutsceneIndex, int32_t gameMode) {
    if (gameMode != 0) return true;
    if ((flags1 & (0x00100000U | 0x00800000U)) != 0U) return true;  // first person, Epona
    if ((flags2 & 0x00040000U) != 0U) return true;                   // crawling
    if (setting == 0x16 || setting == 0x1E) return true;              // crawlspace
    if (setting >= 0x19 && setting <= 0x1B) return true;              // fixed, pivot, side-scroll
    if (setting == 0x1C || setting == 0x1D || setting == 0x2F) return true;  // doors, scene transition
    return cutsceneIndex >= 0xFFF0 && cutsceneIndex <= 0xFFFF;
}

inline constexpr uint32_t kFlags1Talking = 0x00000040U;
inline constexpr uint32_t kFlags1ZTargetMask = 0x00000010U | 0x00008000U | 0x00010000U | 0x00020000U;
inline constexpr uint32_t kFlags1FirstPerson = 0x00100000U;
inline constexpr int16_t kSettingShop = 0x17;

struct OwnerInput {
    bool MainCamera = true;    // camera == play+0x364
    bool LayerWorld = false;   // the session shows World (rig placed, no placement pending)
    bool Probing = false;      // a placement waits for its probes (the layer is Black)
    bool RigValid = false;
    bool HeadValid = false;    // a head pose for the target display time
    bool PlayerValid = false;
    bool ForceHeld = false;    // mode L
    uint32_t Flags1 = 0, Flags2 = 0;
    int16_t Setting = 0;
    int16_t Status = 7;
    int16_t Scene = -1;
    int16_t ActiveCamera = 0;
    int32_t CutsceneIndex = 0;
    int32_t GameMode = 0;
};

struct OwnerDecision {
    bool Vr = false;           // the host owns this update
    bool Probe = false;        // ... only to run a placement probe: no head is written
    uint32_t Held = 0;         // why the native camera owns (0 when Vr)
    bool RigFrozen = false;    // talk, shop, cutscene, item get, secondary camera: the tether waits
    bool Talking = false;
    bool Shop = false;
    bool StickBypass = false;  // talk, shop, first person, crawl (ocarina and Cinema are the caller's)
    bool ZTarget = false;      // flags1 0x10, 0x8000, 0x10000, 0x20000 (carved out of TopScreen's mask)
    bool HeldStatic = false;   // the rows whose camera motion may send the state to Cinema
    uint32_t ProbeHeld = 0;    // a probe update: the Held reasons it ran over (kHeldProbeBlockMask excluded)
};

inline OwnerDecision DecideOwner(const OwnerInput& in) {
    OwnerDecision d;
    if (!in.MainCamera) {
        d.Held = kHeldNotMain;
        return d;
    }
    uint32_t held = 0;
    if (!in.PlayerValid) held |= kHeldNoPlayer;
    if (CinemaRow(in.Flags1, in.Flags2, in.Setting, in.CutsceneIndex, in.GameMode)) held |= kHeldCinemaRow;
    if (in.ActiveCamera != 0) held |= kHeldSecondaryCamera;
    if ((in.Flags1 & 0x20000000U) != 0U) held |= kHeldCutscene;
    if ((in.Flags1 & (0x00000400U | 0x10000000U)) != 0U) held |= kHeldItem;
    if (HeldSetting(in.Setting)) held |= kHeldSetting;
    if (in.Scene == 0x45) held |= kHeldScene;
    if (in.Status != 7) held |= kHeldStatus;
    if ((in.Flags1 & (0x00000020U | 0x00000200U)) != 0U) held |= kHeldState;
    if (in.ForceHeld) held |= kHeldConfig;
    d.Talking = (in.Flags1 & kFlags1Talking) != 0U;
    d.Shop = in.Setting == kSettingShop;
    d.HeldStatic = (held & kHeldStaticMask) != 0U;
    d.RigFrozen = d.Talking || d.Shop || d.HeldStatic;
    const bool crawl = (in.Flags2 & 0x00040000U) != 0U || in.Setting == 0x16 || in.Setting == 0x1E;
    d.StickBypass = d.Talking || d.Shop || (in.Flags1 & kFlags1FirstPerson) != 0U || crawl;
    d.ZTarget = (in.Flags1 & kFlags1ZTargetMask) != 0U;
    if (!in.LayerWorld) {
        if (in.Probing && (held & kHeldProbeBlockMask) == 0U) {
            d.Vr = true;
            d.Probe = true;
            d.ProbeHeld = held;
            return d;
        }
        held |= kHeldLayer;
    } else if (!in.RigValid || !in.HeadValid) {
        held |= kHeldNoHead;
    }
    d.Held = held;
    d.Vr = held == 0U;
    return d;
}

// ---------------------------------------------------------------------------------------------------
// The cutscene camera's motion (§4.3): a Held-static row goes to Cinema once the native camera moved more
// than 2 m * s or turned more than 45 deg within 1 s; never while talking. The trip holds until the row ends,
// so a cutscene that pauses its camera does not thrash between World and Cinema (risk 16).
//
// The hand-over is not a move: the native camera (or a subcamera copied from it) starts from the head the
// host wrote last and needs a while to reach its own view (Normal1's lerp, a OnePoint camera's first cut).
// The window opens once the view has settled: kSettleUpdates updates in a row each moving less than
// kStillMetres * s and kStillDegrees, or kSettleSeconds after the row began, whichever comes first (a
// scripted move that never rests still trips, at most kSettleSeconds late).

class CutsceneCameraMonitor {
  public:
    static constexpr double kWindowSeconds = 1.0;
    static constexpr double kMoveMetres = 2.0;
    static constexpr double kTurnDegrees = 45.0;
    static constexpr double kSettleSeconds = 0.75;
    static constexpr int kSettleUpdates = 5;
    static constexpr double kStillMetres = 0.05;
    static constexpr double kStillDegrees = 1.0;

    bool Update(bool active, double now, const M::Vec3& eye, double yaw, double scale) {
        if (!active) {
            mCount = 0;
            mNext = 0;
            mTripped = false;
            mActive = false;
            mSettled = false;
            mHasLast = false;
            mStill = 0;
            return false;
        }
        if (mTripped) return true;
        if (!M::IsFinite(eye) || !std::isfinite(yaw) || !std::isfinite(now)) return false;
        const double unit = scale > 0.0 && std::isfinite(scale) ? scale : M::kDefaultScale;
        if (!mActive) {
            mActive = true;
            mSince = now;
            mStill = 0;
            mHasLast = false;
            mSettled = false;
            mSettleSeconds = 0.0;
            mSettledStill = false;
        }
        if (!mSettled) {
            if (mHasLast) {
                const bool still = M::Length(eye - mLastEye) <= kStillMetres * unit &&
                                   std::fabs(M::WrapAngle(yaw - mLastYaw)) <= M::DegreesToRadians(kStillDegrees);
                mStill = still ? mStill + 1 : 0;
            }
            mLastEye = eye;
            mLastYaw = yaw;
            mHasLast = true;
            const bool settledStill = mStill >= kSettleUpdates;
            if (!settledStill && now - mSince < kSettleSeconds) return false;
            mSettled = true;
            mSettledStill = settledStill;
            mSettleSeconds = now - mSince;
        }
        const double moveLimit = kMoveMetres * unit;
        const double turnLimit = M::DegreesToRadians(kTurnDegrees);
        for (size_t i = 0; i < mCount; ++i) {
            const Sample& s = mSamples[i];
            if (now - s.Time > kWindowSeconds || s.Time > now) continue;
            if (M::Length(eye - s.Eye) > moveLimit || std::fabs(M::WrapAngle(yaw - s.Yaw)) > turnLimit) {
                mTripped = true;
                mTripMoved = M::Length(eye - s.Eye) / (moveLimit / kMoveMetres);
                mTripTurned = M::RadiansToDegrees(std::fabs(M::WrapAngle(yaw - s.Yaw)));
                return true;
            }
        }
        mSamples[mNext] = {now, eye, yaw};
        mNext = (mNext + 1) % mSamples.size();
        mCount = std::min(mCount + 1, mSamples.size());
        return false;
    }
    bool Tripped() const { return mTripped; }
    bool Settled() const { return mSettled; }
    double TripMovedM() const { return mTripMoved; }
    double TripTurnedDegrees() const { return mTripTurned; }
    // How long the hand-over took before the window opened, and whether it ended by resting (else by time).
    double SettleSeconds() const { return mSettleSeconds; }
    bool SettledStill() const { return mSettledStill; }

  private:
    struct Sample {
        double Time = 0.0;
        M::Vec3 Eye;
        double Yaw = 0.0;
    };
    std::array<Sample, 48> mSamples{};
    size_t mCount = 0, mNext = 0;
    bool mTripped = false;
    double mTripMoved = 0.0, mTripTurned = 0.0;
    bool mActive = false, mSettled = false, mHasLast = false, mSettledStill = false;
    double mSince = 0.0, mSettleSeconds = 0.0, mLastYaw = 0.0;
    M::Vec3 mLastEye;
    int mStill = 0;
};

// ---------------------------------------------------------------------------------------------------
// Head inside geometry (§4.4): one BgCheck segment q -> head_w per update, q = the last head position that
// tested clear. Camera_BGCheckInfo extends the ray 8 u past its end; on a miss it backs the end off by 1 u
// toward q, on a hit it returns the hit pushed 1 u out along the polygon's normal. The penetration is the
// part of the segment behind the collided point; a hit beyond the head (a wall just ahead of the motion)
// is no penetration.
//
// q is only trusted while the head moves continuously through the game world. Where it may not have:
//  - a placement (the probes chose the spot): Reset, q = the placed head;
//  - a turn pivots on the head (head_w does not move): q is kept;
//  - a blink (leash, idle re-centre, floor) moves the head in the game world: ReseedHard. The next test runs
//    from Link's head (L + 50/38 u, a point Link stands in); clear: q = the head; a hit: ReseedHit, the
//    caller re-places (Black + probes) instead of trusting a head that may have landed in a wall;
//  - updates the host did not own (Held rows, a missed head pose) let the head walk around a pillar while q
//    stood still: ReseedSoft, the next test runs from Link's head; clear: q = the head; a hit keeps the last
//    fade and the test after it runs from q again.
// While q's test fails, every other update runs the same rescue test from Link's head: it can only clear a
// fade (Link line of sight never darkens the world, §4.4: it would black out behind every fence), so a head
// that left a wall in Link's open space is never held black by a stale q.

struct GeometrySegment {
    M::Vec3 From, To;
};
struct GeometryConfig {
    double ClearU = 1.0;            // p <= 1 u (the miss nudge) is clear: q moves to the head
    double ToleranceU = 0.5;
    double FadeStartU = 5.0;        // p > 5 u fades ...
    double FullFadeM = 0.3;         // ... to black at 0.3 m * s
    double BelowFloorM = 0.2;       // the head below Link's floor + 0.2 m * s: black
};
enum class GeometryTest : uint8_t { FromQ, FromLink };
inline const char* GeometryTestName(GeometryTest test) { return test == GeometryTest::FromLink ? "link" : "q"; }
enum class GeometryReseed : uint8_t { None, Soft, Hard };
inline const char* GeometryReseedName(GeometryReseed reseed) {
    switch (reseed) {
    case GeometryReseed::None: return "none";
    case GeometryReseed::Soft: return "soft";
    case GeometryReseed::Hard: return "hard";
    }
    return "?";
}
struct GeometryStep {
    double Fade = 0.0;
    double PenetrationU = 0.0;
    bool Clear = false;
    bool BelowFloor = false;
    GeometryTest Test = GeometryTest::FromQ;
    GeometryReseed Reseed = GeometryReseed::None;  // the reseed this test answered
    bool ReseedHit = false;  // a hard reseed hit geometry: re-place (the head may be inside a wall)
};

class HeadInGeometry {
  public:
    void Reset(const M::Vec3& clear) {
        mValid = M::IsFinite(clear);
        mQ = clear;
        mReseed = GeometryReseed::None;
        mRescueNext = false;
        mLastFade = 0.0;
    }
    void ReseedHard() { mReseed = GeometryReseed::Hard; }
    void ReseedSoft() {
        if (mReseed == GeometryReseed::None) mReseed = GeometryReseed::Soft;
    }
    void Invalidate() { mValid = false; }
    bool Valid() const { return mValid; }
    const M::Vec3& Q() const { return mQ; }
    GeometryReseed PendingReseed() const { return mReseed; }
    double LastFade() const { return mLastFade; }
    // This update's segment: from Link's head for a reseed or the rescue test, else from q (the head itself
    // before any q). `linkHead`: L + (0, 50 u adult / 38 u child, 0).
    GeometrySegment Segment(const M::Vec3& head, const M::Vec3& linkHead) {
        const bool fromLink = (mReseed != GeometryReseed::None || mRescueNext) && M::IsFinite(linkHead);
        mTest = fromLink ? GeometryTest::FromLink : GeometryTest::FromQ;
        mFrom = fromLink ? linkHead : (mValid ? mQ : head);
        mTestReseed = fromLink ? mReseed : GeometryReseed::None;
        mHasSegment = true;
        return {mFrom, head};
    }
    // `collided`: BgCheckInfo's end of the last Segment(); `floorY`: Link's floor (u); `scale`: u per metre.
    GeometryStep Update(const M::Vec3& head, const M::Vec3& collided, double floorY, double scale,
                        const GeometryConfig& config = {}) {
        GeometryStep step;
        step.Fade = mLastFade;
        if (!M::IsFinite(head)) return step;
        const double s = std::isfinite(scale) && scale > 0.0 ? scale : M::kDefaultScale;
        if (!mHasSegment) Segment(head, {std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0});
        mHasSegment = false;
        step.Test = mTest;
        step.Reseed = mTestReseed;
        const M::Vec3 from = mFrom;
        const M::Vec3 segment = head - from;
        const double length = M::Length(segment);
        double penetration = 0.0;
        if (length > 1e-6 && M::IsFinite(collided)) {
            const double along = M::Dot(collided - from, segment) / length;
            penetration = std::max(0.0, length - along);
        }
        step.PenetrationU = penetration;
        const double depthFade =
            penetration > config.FadeStartU ? std::clamp(penetration / (config.FullFadeM * s), 0.0, 1.0) : 0.0;
        double fade = 0.0;
        if (penetration <= config.ClearU + config.ToleranceU) {
            step.Clear = true;
            mValid = true;
            mQ = head;
            mReseed = GeometryReseed::None;
            mRescueNext = false;
        } else if (mTest == GeometryTest::FromQ) {
            fade = depthFade;
            mRescueNext = true;  // the rescue test from Link's head next update
        } else {
            // From Link's head and not clear: a hard reseed asks for a re-placement (and shows the depth until
            // it comes); a soft reseed or the rescue keeps the last fade and hands back to q.
            if (mTestReseed == GeometryReseed::Hard) {
                step.ReseedHit = true;
                fade = depthFade;
            } else {
                fade = mLastFade;
            }
            mReseed = GeometryReseed::None;
            mRescueNext = false;
        }
        if (std::isfinite(floorY) && head.Y < floorY + config.BelowFloorM * s) {
            step.BelowFloor = true;
            fade = 1.0;
        }
        step.Fade = fade;
        mLastFade = fade;
        return step;
    }

  private:
    bool mValid = false;
    M::Vec3 mQ;
    GeometryReseed mReseed = GeometryReseed::None;
    bool mRescueNext = false;
    double mLastFade = 0.0;
    // The last Segment() (Update measures against it).
    bool mHasSegment = false;
    GeometryTest mTest = GeometryTest::FromQ;
    GeometryReseed mTestReseed = GeometryReseed::None;
    M::Vec3 mFrom;
};

// ---------------------------------------------------------------------------------------------------
// Placement probes (§4.4): while Black, one probe per update from Link's head (L + 50 u adult / 38 u child)
// toward back, back-left and back-right; then, if none leaves the minimum, left and right. d = the farthest
// clear distance minus 0.3 m (capped later at the wanted d0 by SolvePlacement), at least 0.7 m; nothing
// clear: 0.7 m straight behind. Directions are game-world yaws relative to "behind Link" (facing + pi);
// yaw increases to the left, so Link's left (facing + 90 deg) is offset -90 and his back-left is -35.

struct ProbeDirection {
    const char* Name;
    double OffsetDegrees;
};
inline constexpr std::array<ProbeDirection, 5> kProbeDirections{{
    {"back", 0.0}, {"back_left", -35.0}, {"back_right", 35.0}, {"left", -90.0}, {"right", 90.0}}};

inline double LinkHeadHeightU(bool child) { return child ? 38.0 : 50.0; }

struct ProbeConfig {
    double MarginM = 0.3;
    double MinM = 0.7;
    double MissToleranceU = 2.0;  // a clear distance within this of the probe's length is a miss
};
struct ProbeRequest {
    int Index = -1;
    M::Vec3 From, To;  // game world
};
struct ProbeChoice {
    bool Probed = false;     // at least one probe ran
    bool Fallback = false;   // nothing was clear enough: MinM straight behind
    int Index = -1;
    const char* Name = "none";
    double OffsetYaw = 0.0;  // radians, game world, relative to behind Link
    double DistanceM = std::numeric_limits<double>::infinity();
    std::array<double, 5> ClearM{};
    int Count = 0;
};

class PlacementProbes {
  public:
    void Start(double lengthM) {
        *this = PlacementProbes{};
        mActive = true;
        mLengthM = std::isfinite(lengthM) && lengthM > 0.0 ? lengthM : 2.3;
        mClear.fill(std::numeric_limits<double>::quiet_NaN());
    }
    void Cancel() { mActive = false; }
    bool Active() const { return mActive; }
    double LengthM() const { return mLengthM; }
    int Count() const { return mCount; }
    bool Done(const ProbeConfig& config = {}) const {
        if (!mActive) return false;
        if (!Fed(0) || !Fed(1) || !Fed(2)) return false;
        if (Best(0, 3).second - config.MarginM >= config.MinM) return true;
        return Fed(3) && Fed(4);
    }
    // The next probe of the plan from Link's head along facing + pi + offset (game world).
    bool Next(const M::Vec3& linkHead, double linkFacingYaw, double scale, ProbeRequest* out,
              const ProbeConfig& config = {}) const {
        if (!mActive || out == nullptr || !M::IsFinite(linkHead) || !std::isfinite(linkFacingYaw)) return false;
        int index = -1;
        for (int i = 0; i < 3 && index < 0; ++i)
            if (!Fed(i)) index = i;
        if (index < 0 && Best(0, 3).second - config.MarginM < config.MinM)
            for (int i = 3; i < 5 && index < 0; ++i)
                if (!Fed(i)) index = i;
        if (index < 0) return false;
        const double s = std::isfinite(scale) && scale > 0.0 ? scale : M::kDefaultScale;
        const double yaw = linkFacingYaw + M::kPi + M::DegreesToRadians(kProbeDirections[static_cast<size_t>(index)].OffsetDegrees);
        out->Index = index;
        out->From = linkHead;
        out->To = linkHead + M::YawDirection(yaw) * (mLengthM * s);
        return true;
    }
    // The measured end of probe `index` (BgCheckInfo's collided point).
    void Feed(int index, const M::Vec3& from, const M::Vec3& collided, double scale, const ProbeConfig& config = {}) {
        if (!mActive || index < 0 || index >= 5) return;
        const double s = std::isfinite(scale) && scale > 0.0 ? scale : M::kDefaultScale;
        double clear = M::IsFinite(collided) && M::IsFinite(from) ? M::LengthXZ(collided - from) / s : 0.0;
        if (!std::isfinite(clear)) clear = 0.0;
        if (clear >= mLengthM - config.MissToleranceU / s) clear = mLengthM;
        clear = std::clamp(clear, 0.0, mLengthM);
        if (!Fed(index)) ++mCount;
        mClear[static_cast<size_t>(index)] = clear;
    }
    ProbeChoice Choose(const ProbeConfig& config = {}) const {
        ProbeChoice choice;
        for (size_t i = 0; i < mClear.size(); ++i) choice.ClearM[i] = mClear[i];
        choice.Count = mCount;
        if (mCount == 0) return choice;
        choice.Probed = true;
        for (const auto& [first, last] : {std::pair{0, 3}, std::pair{3, 5}}) {
            const auto [index, clear] = Best(first, last);
            if (index >= 0 && clear - config.MarginM >= config.MinM) {
                choice.Index = index;
                choice.Name = kProbeDirections[static_cast<size_t>(index)].Name;
                choice.OffsetYaw = M::DegreesToRadians(kProbeDirections[static_cast<size_t>(index)].OffsetDegrees);
                choice.DistanceM = clear - config.MarginM;
                return choice;
            }
        }
        choice.Fallback = true;
        choice.Index = 0;
        choice.Name = "fallback";
        choice.OffsetYaw = 0.0;
        choice.DistanceM = config.MinM;
        return choice;
    }

  private:
    bool Fed(int index) const { return std::isfinite(mClear[static_cast<size_t>(index)]); }
    // The farthest fed probe in [first, last): (index, clear); (-1, -inf) when none.
    std::pair<int, double> Best(int first, int last) const {
        std::pair<int, double> best{-1, -std::numeric_limits<double>::infinity()};
        for (int i = first; i < last; ++i)
            if (Fed(i) && mClear[static_cast<size_t>(i)] > best.second) best = {i, mClear[static_cast<size_t>(i)]};
        return best;
    }
    bool mActive = false;
    double mLengthM = 2.3;
    std::array<double, 5> mClear{};
    int mCount = 0;
};

// The facing yaw SolvePlacement needs (VR world) so that Jamal lands along probe `choice` from Link.
inline double PlacementFacingYaw(double linkFacingYawGame, const ProbeChoice& choice, bool mirrored) {
    return M::YawToVrWorld(M::WrapAngle(linkFacingYawGame + (choice.Probed ? choice.OffsetYaw : 0.0)), mirrored);
}

// ---------------------------------------------------------------------------------------------------
// The stick (§7): the Circle Pad of each guest refresh rotated by theta = psi_ref - yaw_cam, so the stick
// pushes Link relative to where Jamal looks whoever owns the camera. psi_ref (VR world):
//  - head-latched (default): the head yaw latched when the stick leaves the deadzone, then following the
//    head with tau 0.5 s while held; Z-targeting latches it at the L press and holds it;
//  - head: the head yaw with tau 0.1 s;
//  - line: the yaw of Link - head beyond 0.6 m * s (else the head);
//  - native: no rotation (the game's own camera-relative stick).
// The controller option (left aim yaw) needs the aim pose (G11, W5) and is not offered yet.

enum class MoveReference : uint8_t { HeadLatched, Head, Line, Native };
inline const char* MoveReferenceName(MoveReference value) {
    switch (value) {
    case MoveReference::HeadLatched: return "head-latched";
    case MoveReference::Head: return "head";
    case MoveReference::Line: return "line";
    case MoveReference::Native: return "native";
    }
    return "?";
}
inline bool ParseMoveReference(std::string_view text, MoveReference* out) {
    if (text.empty() || text == "head-latched" || text == "latched") *out = MoveReference::HeadLatched;
    else if (text == "head") *out = MoveReference::Head;
    else if (text == "line") *out = MoveReference::Line;
    else if (text == "native" || text == "off") *out = MoveReference::Native;
    else return false;
    return true;
}

struct StickInput {
    double Dt = 0.0;
    bool HeadValid = false;
    double HeadYaw = 0.0;         // VR world
    double LineYaw = 0.0;         // VR world, Link - head
    double LineDistanceM = 0.0;
    bool StickActive = false;     // the Circle Pad is outside its deadzone
    bool ZTarget = false;
    bool Bypass = false;          // talk, shop, first person, crawl, ocarina, Cinema/Black
};
struct StickStep {
    bool Rotate = false;
    double PsiRef = 0.0;          // VR world
    bool Latched = false;         // the reference latched this refresh (log edge)
};

inline double ApproachAngle(double current, double target, double dt, double tau) {
    if (!std::isfinite(current)) return target;
    if (!(tau > 0.0) || !std::isfinite(dt)) return target;
    return M::WrapAngle(current + M::WrapAngle(target - current) * (1.0 - std::exp(-std::max(dt, 0.0) / tau)));
}

class StickReference {
  public:
    static constexpr double kLatchedTauS = 0.5;
    static constexpr double kHeadTauS = 0.1;
    static constexpr double kLineMinM = 0.6;

    StickStep Update(MoveReference mode, const StickInput& in) {
        StickStep step;
        if (mode == MoveReference::Native || in.Bypass || !in.HeadValid || !std::isfinite(in.HeadYaw)) {
            mLatched = mZLatched = mHeadValid = false;
            return step;
        }
        if (!mHeadValid) {
            mHead = in.HeadYaw;
            mHeadValid = true;
        } else {
            mHead = ApproachAngle(mHead, in.HeadYaw, in.Dt, kHeadTauS);
        }
        if (in.ZTarget) {
            if (!mZLatched) {
                mZ = in.HeadYaw;
                mZLatched = true;
                step.Latched = true;
            }
            mLatched = false;
            step.PsiRef = mZ;
            step.Rotate = in.StickActive;
            return step;
        }
        mZLatched = false;
        switch (mode) {
        case MoveReference::Head: step.PsiRef = mHead; break;
        case MoveReference::Line:
            step.PsiRef = in.LineDistanceM > kLineMinM && std::isfinite(in.LineYaw) ? in.LineYaw : in.HeadYaw;
            break;
        default:
            if (!in.StickActive) {
                mLatched = false;
                return step;
            }
            if (!mLatched) {
                mPsi = in.HeadYaw;
                mLatched = true;
                step.Latched = true;
            } else {
                mPsi = ApproachAngle(mPsi, in.HeadYaw, in.Dt, kLatchedTauS);
            }
            step.PsiRef = mPsi;
            break;
        }
        step.Rotate = in.StickActive;
        return step;
    }
    void Reset() { mLatched = mZLatched = mHeadValid = false; }

  private:
    bool mLatched = false, mZLatched = false, mHeadValid = false;
    double mPsi = 0.0, mZ = 0.0, mHead = 0.0;
};

// theta = psi_ref - yaw_cam, both in the VR world: in a mirrored frame the game's camera yaw is negated
// (the game mirrors the stick's X there too [unverified, W5 Master Quest check]).
inline double StickTheta(double psiRefVr, double cameraYawGame, bool mirrored) {
    return M::WrapAngle(psiRefVr - M::YawToVrWorld(cameraYawGame, mirrored));
}

// (x', y') = R(theta) (x, y): pushing up moves Link along psi_ref. Stick angle a = atan2(-x, y) is the
// game's (stick right = yaw - 90 deg); R(theta) adds theta to it. The result keeps its length, clamped to
// the pad radius.
inline void RotateCirclePad(int16_t& x, int16_t& y, double theta, double radius = kCirclePadRadius) {
    if (!std::isfinite(theta) || (x == 0 && y == 0)) return;
    const double c = std::cos(theta), s = std::sin(theta);
    double rx = x * c - y * s;
    double ry = x * s + y * c;
    const double length = std::hypot(rx, ry);
    if (length > radius && length > 0.0) {
        rx *= radius / length;
        ry *= radius / length;
    }
    x = static_cast<int16_t>(std::clamp(std::lround(rx), -32767L, 32767L));
    y = static_cast<int16_t>(std::clamp(std::lround(ry), -32767L, 32767L));
}

// The stick leaves its deadzone (native units; the host deadzone was applied before).
inline bool StickActive(int16_t x, int16_t y, int deadzone = 10) {
    return static_cast<int32_t>(x) * x + static_cast<int32_t>(y) * y > deadzone * deadzone;
}

// ---------------------------------------------------------------------------------------------------
// R_always (§6): an actor within 4 m * s of the head is drawn even when the culling volume rejects it.

inline bool NearHead(const M::Vec3& actor, const M::Vec3& head, double radiusU) {
    if (!M::IsFinite(actor) || !M::IsFinite(head) || !(radiusU > 0.0)) return false;
    const M::Vec3 d = actor - head;
    return M::Dot(d, d) <= radiusU * radiusU;
}

// ---------------------------------------------------------------------------------------------------
// PC preview (§2.7 W2: HEAD=circle:radius_m,period_s): the head walks a circle around Link at eye height,
// starting behind him, always looking at his head. Game world.

struct CircleHeadConfig {
    double RadiusM = 1.2;
    double PeriodSeconds = 8.0;
    double EyeHeightM = 1.6;
};

inline GameHead CirclePreviewHead(const M::Vec3& link, double linkFacingYaw, double linkHeadU,
                                  const CircleHeadConfig& config, double seconds, double scale) {
    GameHead head;
    const double s = std::isfinite(scale) && scale > 0.0 ? scale : M::kDefaultScale;
    const double period = config.PeriodSeconds > 0.0 ? config.PeriodSeconds : 8.0;
    const double phase = std::fmod(std::max(seconds, 0.0), period) / period;
    const double angle = linkFacingYaw + M::kPi + 2.0 * M::kPi * phase;
    head.Position = link + M::YawDirection(angle) * (config.RadiusM * s) + M::Vec3{0.0, config.EyeHeightM * s, 0.0};
    const M::Vec3 target = link + M::Vec3{0.0, linkHeadU, 0.0};
    const M::Vec3 toward = target - head.Position;
    const double length = M::Length(toward);
    if (!(length > 1e-6) || !M::IsFinite(head.Position)) return head;
    head.Forward = toward * (1.0 / length);
    head.LevelForward = M::NormalizedXZ(toward, M::YawDirection(angle + M::kPi));
    head.Yaw = std::atan2(head.LevelForward.X, head.LevelForward.Z);
    head.VrYaw = head.Yaw;
    head.Valid = true;
    return head;
}
// The 45-degree step of the circle at `seconds` (the screenshot points of W2 day 5), 0..7.
inline int CircleStep(double seconds, double periodSeconds) {
    const double period = periodSeconds > 0.0 ? periodSeconds : 8.0;
    const double phase = std::fmod(std::max(seconds, 0.0), period) / period;
    return std::clamp(static_cast<int>(std::floor(phase * 8.0)), 0, 7);
}

}  // namespace Oot3dNativeGame::XrCamera
