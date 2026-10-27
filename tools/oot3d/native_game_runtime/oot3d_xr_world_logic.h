// oot3d_xr_world_logic.h: the pure decisions of the in-world mode's first light (quest-research/
// inworld-spec.md §2.3, §2.7, §3.4, §4.3, W1). Header-only, std and oot3d_xr_math.h only, so
// oot3d_xr_world_logic_tests runs it on any host (PC, or the headset through adb).
//
//  - draw classes (§2.3) from facts the caller extracts from a draw view, and what each class gets in
//    an eye pass (game uniforms, eye rows, rotation-only sky rows, or left out);
//  - the eye-row rewrite with its per-camera chain cache (§2.2 "one rigid inverse per distinct V_g,d");
//  - the tick-locked pacing p_N (§3.4) and the display time of a tick;
//  - the W1 state classifier (§4.3 rows that need no hook): World / Cinema / Black with blockers;
//  - the PC preview heads (§2.7): identity, an offset from the frame camera, the 90/180 deg sweeps.
#pragma once

#include "oot3d_xr_math.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

namespace Oot3dNativeGame::XrWorld {

namespace M = Oot3dNativeGame::XrMath;

// ---------------------------------------------------------------------------------------------------
// Draw classes (§2.3)

enum class XrDrawClass : uint8_t {
    OffTarget,     // not drawn into the presented top-screen target (shadow2D, captures, RTT)
    Ui,            // PICA Ui domain: left out of the eyes (W4 replays it into ns5)
    Screen,        // Ui frame content over the whole target (washes, full-frame redraws, movies)
    OrthoFull,     // not perspective, viewport covers the target (gameplay fades, colour washes)
    OrthoOmitted,  // not perspective, partial viewport (CPU-projected flare): left out
    Sky,           // camera-centred atmosphere: rotation-only rows
    World,         // perspective with its own camera: full eye rows
    FrameCam,      // perspective without a captured camera: eye rows through the frame camera
    Fallback,      // unusable rows or no camera at all: left out
};
inline constexpr size_t kXrDrawClassCount = 9;

inline const char* XrDrawClassName(XrDrawClass value) {
    switch (value) {
    case XrDrawClass::OffTarget: return "offtarget";
    case XrDrawClass::Ui: return "ui";
    case XrDrawClass::Screen: return "screen";
    case XrDrawClass::OrthoFull: return "ortho_full";
    case XrDrawClass::OrthoOmitted: return "ortho_omitted";
    case XrDrawClass::Sky: return "sky";
    case XrDrawClass::World: return "full";
    case XrDrawClass::FrameCam: return "framecam";
    case XrDrawClass::Fallback: return "fallback";
    }
    return "unknown";
}

enum class XrDrawTreatment : uint8_t { GameUniforms, EyeRows, SkyRows, Omit };

inline XrDrawTreatment TreatmentOf(XrDrawClass value) {
    switch (value) {
    case XrDrawClass::OffTarget:
    case XrDrawClass::Screen:
    case XrDrawClass::OrthoFull: return XrDrawTreatment::GameUniforms;
    case XrDrawClass::World:
    case XrDrawClass::FrameCam: return XrDrawTreatment::EyeRows;
    case XrDrawClass::Sky: return XrDrawTreatment::SkyRows;
    case XrDrawClass::Ui:
    case XrDrawClass::OrthoOmitted:
    case XrDrawClass::Fallback: return XrDrawTreatment::Omit;
    }
    return XrDrawTreatment::Omit;
}

// What the classifier needs from one draw view, extracted by the caller (oot3d_native_xr_view.cpp).
struct XrDrawFacts {
    bool OnTopTarget = true;            // colour address == the presented transfer's input
    bool UiDomain = false;              // CompositionDomain == Ui
    bool ScreenContent = false;         // Ui over the whole target, untextured or only rendered copies
    bool Atmosphere = false;            // Composition.Layer == Atmosphere
    bool ViewportCoversTarget = false;  // viewport covers the colour target
    M::ProjectionStatus Projection = M::ProjectionStatus::NonFinite;
    bool HasWorldToView = false;        // LightBinding.WorldToView (view override, else the plan's)
    bool HasFrameCamera = false;        // the sample has a frame camera
    bool SkyScope = false;              // Composition.SourcePc may draw the sky (scope list)
    bool CameraCentred = false;         // camera-space model origin within 1 m * s of the camera
};

inline XrDrawClass ClassifyXrDraw(const XrDrawFacts& f) {
    if (!f.OnTopTarget) return XrDrawClass::OffTarget;
    if (f.UiDomain) return f.ScreenContent ? XrDrawClass::Screen : XrDrawClass::Ui;
    if (f.Projection == M::ProjectionStatus::NotPerspective || f.Projection == M::ProjectionStatus::NonNegativeC)
        return f.ViewportCoversTarget ? XrDrawClass::OrthoFull : XrDrawClass::OrthoOmitted;
    if (f.Projection != M::ProjectionStatus::Ok) return XrDrawClass::Fallback;
    if (!f.HasWorldToView) return f.HasFrameCamera ? XrDrawClass::FrameCam : XrDrawClass::Fallback;
    if (f.Atmosphere && f.SkyScope && f.CameraCentred) return XrDrawClass::Sky;
    return XrDrawClass::World;
}

// The sky scope list (§2.3 SkyScopePcs). Empty = every atmosphere scope except the excluded ones may
// be sky when camera-centred (W1 default until the W0 trace picks the list on the device).
struct XrSkyScopes {
    std::array<uint32_t, 16> Allowed{};
    size_t AllowedCount = 0;
    std::array<uint32_t, 16> Excluded{};
    size_t ExcludedCount = 0;
    bool Contains(uint32_t pc) const {
        for (size_t i = 0; i < ExcludedCount; ++i)
            if (Excluded[i] == pc) return false;
        if (AllowedCount == 0) return true;
        for (size_t i = 0; i < AllowedCount; ++i)
            if (Allowed[i] == pc) return true;
        return false;
    }
    // "0x2D9664,0x297508" (hex or decimal, comma or space separated); false on a malformed entry.
    bool ParseAllowed(std::string_view text) {
        AllowedCount = 0;
        while (!text.empty()) {
            const size_t end = text.find_first_of(", ");
            const std::string token(text.substr(0, end));
            text = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);
            if (token.empty()) continue;
            char* stop = nullptr;
            const unsigned long value = std::strtoul(token.c_str(), &stop, 0);
            if (stop == token.c_str() || *stop != '\0' || AllowedCount == Allowed.size()) return false;
            Allowed[AllowedCount++] = static_cast<uint32_t>(value);
        }
        return true;
    }
};

// Camera-centred (§2.3): the draw's camera-space model origin (its model-view translation) within
// `radius` game units of the camera.
inline bool CameraCentred(const std::array<float, 4>& row0, const std::array<float, 4>& row1,
                          const std::array<float, 4>& row2, double radius, double* distanceOut = nullptr) {
    const double x = row0[3], y = row1[3], z = row2[3];
    const double distance = std::sqrt(x * x + y * y + z * z);
    if (distanceOut != nullptr) *distanceOut = distance;
    return std::isfinite(distance) && distance <= radius;
}

// ---------------------------------------------------------------------------------------------------
// Eye rows (§2.2): F' = blockdiag(R+, I2) * P_eye,d * V_i * M * V_g,d^-1 (sky: rotation parts only)

struct XrEyeInput {
    bool Valid = false;
    M::Mat4 View;   // V_i: VR world (u) -> eye view (u)
    M::EyeFov Fov;  // the eye's tangents
};

// The chains of one eye and sample, one per distinct V_g,d (normally 1-3): V_i * M * V_g,d^-1, or the
// sky chain. Reset per eye per sample.
class XrChainCache {
  public:
    void Reset(const XrEyeInput& eye, bool mirrored) {
        mEye = eye;
        mMirrored = mirrored;
        mCount = 0;
        mNext = 0;
    }
    // False when V_g,d cannot be inverted (the draw falls back).
    bool Chain(const std::array<float, 16>& worldToView, bool sky, M::Mat4* out) {
        for (size_t i = 0; i < mCount; ++i) {
            if (mEntries[i].Sky == sky && std::memcmp(mEntries[i].Key.data(), worldToView.data(),
                                                      sizeof(float) * 16) == 0) {
                ++mHits;
                if (!mEntries[i].Ok) return false;
                *out = mEntries[i].Value;
                return true;
            }
        }
        Entry entry;
        entry.Key = worldToView;
        entry.Sky = sky;
        const M::Mat4 gameView = M::Mat4FromColumnMajor(worldToView);
        if (sky) {
            entry.Ok = M::SkyChain(mEye.View, gameView, mMirrored, &entry.Value);
        } else {
            M::Mat4 inverse;
            entry.Ok = M::InverseAffine(gameView, &inverse);
            if (entry.Ok) entry.Value = M::EyeChain(mEye.View, inverse, mMirrored);
        }
        ++mMisses;
        mEntries[mNext] = entry;
        mNext = (mNext + 1) % mEntries.size();
        mCount = std::min(mCount + 1, mEntries.size());
        if (!entry.Ok) return false;
        *out = entry.Value;
        return true;
    }
    const XrEyeInput& Eye() const { return mEye; }
    uint64_t Hits() const { return mHits; }
    uint64_t Misses() const { return mMisses; }

  private:
    struct Entry {
        std::array<float, 16> Key{};
        bool Sky = false;
        bool Ok = false;
        M::Mat4 Value;
    };
    XrEyeInput mEye;
    bool mMirrored = false;
    std::array<Entry, 8> mEntries{};
    size_t mCount = 0, mNext = 0;
    uint64_t mHits = 0, mMisses = 0;
};

// The eye's rows for one draw (f = its f[0..3]). False: the rows cannot be built (fallback).
inline bool XrDrawRows(const M::Rows& f, const std::array<float, 16>& worldToView, bool sky, XrChainCache& cache,
                       M::Rows* out) {
    M::Mat4 chain;
    if (!cache.Eye().Valid || !cache.Chain(worldToView, sky, &chain)) return false;
    return M::ComposeEyeRows(M::DecomposeGameProjection(f), cache.Eye().Fov, chain, out);
}

// ---------------------------------------------------------------------------------------------------
// Tick-locked pacing (§3.4): p_N = p_{N-1} + d; a frame that starts with less than `margin` ticks left
// resets the phase to latest + d (an overrun) instead of waiting for the next multiple.

struct XrTickStep {
    uint64_t PaceTick = 0;  // p_N
    uint64_t Delta = 0;     // p_N - p_{N-1} (ticks of presentation time this frame)
    bool Overrun = false;
    bool Started = false;   // first step after (re)start: no previous tick
};

class XrTickPacer {
  public:
    static constexpr uint64_t kMargin = 2;
    void SetDivisor(uint32_t divisor) { mDivisor = std::max<uint32_t>(1, divisor); }
    uint32_t Divisor() const { return mDivisor; }
    void Restart() { mStarted = false; }
    uint64_t Previous() const { return mPrevious; }
    XrTickStep Next(uint64_t latestTick) {
        XrTickStep step;
        if (!mStarted) {
            mStarted = true;
            mPrevious = latestTick + mDivisor;
            step.PaceTick = mPrevious;
            step.Delta = mDivisor;
            step.Started = true;
            return step;
        }
        uint64_t pace = mPrevious + mDivisor;
        if (latestTick + kMargin > pace) {
            pace = latestTick + mDivisor;
            step.Overrun = true;
        }
        step.PaceTick = pace;
        step.Delta = pace - mPrevious;
        mPrevious = pace;
        return step;
    }

  private:
    uint32_t mDivisor = 3;
    uint64_t mPrevious = 0;
    bool mStarted = false;
};

// DisplayTime(k), extrapolated from the latest tick (§3.4 item 2).
inline int64_t TickDisplayTimeNs(uint64_t latestTick, int64_t latestDisplayTimeNs, int64_t periodNs, uint64_t tick) {
    return latestDisplayTimeNs + (static_cast<int64_t>(tick) - static_cast<int64_t>(latestTick)) * periodNs;
}

// Eyes per guest frame at a display rate: 90 Hz -> 3, 72 -> 2 (2.4 rounds down), 120 -> 4.
inline uint32_t TickDivisor(double displayHz, double guestHz = 30.0) {
    if (!(displayHz > 0.0) || !(guestHz > 0.0)) return 3;
    return static_cast<uint32_t>(std::max(1.0, std::floor(displayHz / guestHz + 0.25)));
}

// ---------------------------------------------------------------------------------------------------
// The W1 state classifier (§4.3 rows that need no Camera_Update hook; §0 first light). One update per
// completed guest frame.
//  - Cinema: menus, pause, file select, F1, text boxes (non-HUD Ui draws for 0.3 s), cutscenes, first
//    person, crawl, fixed/pivot/side-scroll cameras, Epona, no player, the render safety.
//  - Black: doors (settings 0x1C/0x1D), scene transitions (0x2F), a scene or entrance change, a Link
//    teleport; held while the door setting lasts (3 s cap, then Cinema), then placed and World.
//  - World needs 0.5 s without Cinema blockers; Cinema exit is Black -> placement -> World.
//  - Held rows of §4.3 (talking, item get, secondary camera) stay World in W1 with the rig frozen.

enum class XrWorldState : uint8_t { World, Cinema, Black };
inline const char* XrWorldStateName(XrWorldState state) {
    switch (state) {
    case XrWorldState::World: return "world";
    case XrWorldState::Cinema: return "cinema";
    case XrWorldState::Black: return "black";
    }
    return "unknown";
}

enum XrBlocker : uint32_t {
    kXrBlockGameMode = 1U << 0,     // not gameplay: title, file select, game over
    kXrBlockHostGui = 1U << 1,      // F1, the menu bar, the in-game menu
    kXrBlockUi = 1U << 2,           // non-HUD PICA Ui draws for >= 0.3 s (text boxes, pause) until W4
    kXrBlockCutscene = 1U << 3,     // flags1 0x20000000, cutscene index >= 0xFFF0
    kXrBlockFirstPerson = 1U << 4,  // flags1 0x100000 (W2-W4)
    kXrBlockCrawl = 1U << 5,        // flags2 0x40000, settings 0x16/0x1E
    kXrBlockFixedCamera = 1U << 6,  // settings 0x19-0x1B
    kXrBlockEpona = 1U << 7,        // flags1 0x800000
    kXrBlockNoPlayer = 1U << 8,     // no player (or camera status != 7) for > 2 s
    kXrBlockRenderSafety = 1U << 9, // fallback draws in 30 consecutive eye frames
    kXrBlockSession = 1U << 10,     // the XR session cannot show the world (no ticks, not running)
    kXrBlackDoor = 1U << 16,        // door / scene-transition setting
    kXrBlackScene = 1U << 17,       // scene or entrance change
    kXrBlackTeleport = 1U << 18,    // |dL| > 1.5 m * s in one update
    kXrHoldRig = 1U << 24,          // Held rows: talking, item get, secondary camera (rig frozen)
};
inline constexpr uint32_t kXrCinemaMask = 0xFFFFU;
inline constexpr uint32_t kXrBlackMask = 0xFF0000U;

struct XrWorldSignals {
    double Now = 0.0;  // seconds
    bool Gameplay = true;
    bool HostGui = false;
    bool UiDraws = false;
    bool UiRuleEnabled = true;
    bool PlayerValid = true;
    bool SessionReady = true;
    bool RenderSafety = false;
    uint32_t Flags1 = 0, Flags2 = 0;
    int16_t CameraSetting = 0;
    int16_t CameraStatus = 7;
    int16_t ActiveCamera = 0;
    int32_t CutsceneIndex = 0;
    int16_t Scene = -1;
    int32_t Entrance = -1;
    M::Vec3 Link;          // game world (u)
    double Scale = M::kDefaultScale;
};

struct XrClassifierStep {
    XrWorldState State = XrWorldState::Cinema;
    bool Changed = false;
    bool Place = false;          // Black -> World: place the rig (§5.2) now
    const char* PlaceReason = "";
    uint32_t Blockers = 0;       // this update's blockers (log)
};

inline uint32_t XrCinemaBlockers(const XrWorldSignals& s) {
    uint32_t b = 0;
    if (!s.Gameplay) b |= kXrBlockGameMode;
    if (s.HostGui) b |= kXrBlockHostGui;
    if ((s.Flags1 & 0x20000000U) != 0U || (s.CutsceneIndex >= 0xFFF0 && s.CutsceneIndex <= 0xFFFF))
        b |= kXrBlockCutscene;
    if ((s.Flags1 & 0x00100000U) != 0U) b |= kXrBlockFirstPerson;
    if ((s.Flags2 & 0x00040000U) != 0U || s.CameraSetting == 0x16 || s.CameraSetting == 0x1E) b |= kXrBlockCrawl;
    if (s.CameraSetting >= 0x19 && s.CameraSetting <= 0x1B) b |= kXrBlockFixedCamera;
    if ((s.Flags1 & 0x00800000U) != 0U) b |= kXrBlockEpona;
    if (!s.SessionReady) b |= kXrBlockSession;
    if (s.RenderSafety) b |= kXrBlockRenderSafety;
    return b;
}

inline bool XrDoorSetting(int16_t setting) { return setting == 0x1C || setting == 0x1D || setting == 0x2F; }

inline uint32_t XrHeldRows(const XrWorldSignals& s) {
    const bool held = (s.Flags1 & (0x00000040U | 0x00000400U | 0x10000000U)) != 0U || s.ActiveCamera != 0;
    return held ? static_cast<uint32_t>(kXrHoldRig) : 0U;
}

class XrWorldClassifier {
  public:
    static constexpr double kUiSeconds = 0.3;
    static constexpr double kNoPlayerSeconds = 2.0;
    static constexpr double kEnterWorldSeconds = 0.5;
    static constexpr double kBlackMinimumSeconds = 0.15;
    static constexpr double kDoorCapSeconds = 3.0;
    static constexpr double kTeleportMetres = 1.5;

    XrWorldState State() const { return mState; }

    XrClassifierStep Update(const XrWorldSignals& s) {
        XrClassifierStep step;
        // Debounced blockers.
        if (s.UiDraws && s.UiRuleEnabled) {
            if (mUiSince < 0.0) mUiSince = s.Now;
        } else {
            mUiSince = -1.0;
        }
        const bool playerMissing = !s.PlayerValid || s.CameraStatus != 7;
        if (playerMissing) {
            if (mNoPlayerSince < 0.0) mNoPlayerSince = s.Now;
        } else {
            mNoPlayerSince = -1.0;
        }
        uint32_t blockers = XrCinemaBlockers(s);
        if (mUiSince >= 0.0 && s.Now - mUiSince >= kUiSeconds) blockers |= kXrBlockUi;
        if (mNoPlayerSince >= 0.0 && s.Now - mNoPlayerSince > kNoPlayerSeconds) blockers |= kXrBlockNoPlayer;
        // Black triggers (edges against the previous update).
        uint32_t black = 0;
        if (XrDoorSetting(s.CameraSetting)) black |= kXrBlackDoor;
        if (mHasPrevious && s.PlayerValid && (s.Scene != mScene || s.Entrance != mEntrance)) black |= kXrBlackScene;
        if (mHasPrevious && s.PlayerValid && mPreviousPlayerValid) {
            const double moved = M::Length(s.Link - mLink);
            if (std::isfinite(moved) && moved > kTeleportMetres * s.Scale) black |= kXrBlackTeleport;
        }
        mHasPrevious = true;
        mScene = s.Scene;
        mEntrance = s.Entrance;
        mPreviousPlayerValid = s.PlayerValid;
        if (s.PlayerValid) mLink = s.Link;
        step.Blockers = blockers | black | XrHeldRows(s);

        const XrWorldState before = mState;
        if (blockers != 0U) mClearSince = -1.0;
        else if (mClearSince < 0.0) mClearSince = s.Now;
        switch (mState) {
        case XrWorldState::World:
            if (blockers != 0U) {
                Enter(XrWorldState::Cinema, s.Now);
            } else if (black != 0U) {
                Enter(XrWorldState::Black, s.Now);
                mPlaceReason = (black & kXrBlackDoor) ? "door" : (black & kXrBlackScene) ? "scene" : "teleport";
            }
            break;
        case XrWorldState::Cinema:
            // A door still running (the 3 s cap sent it here) keeps the screen until it ends.
            if ((black & kXrBlackDoor) != 0U) mClearSince = s.Now;
            if (blockers == 0U && mClearSince >= 0.0 && s.Now - mClearSince >= kEnterWorldSeconds) {
                Enter(XrWorldState::Black, s.Now);
                mPlaceReason = mEverWorld ? "cinema_exit" : "first";
            }
            break;
        case XrWorldState::Black:
            if (blockers != 0U) {
                Enter(XrWorldState::Cinema, s.Now);
            } else if ((black & kXrBlackDoor) != 0U && s.Now - mSince > kDoorCapSeconds) {
                Enter(XrWorldState::Cinema, s.Now);  // a door that does not end: the screen
            } else if ((black & kXrBlackDoor) == 0U && s.Now - mSince >= kBlackMinimumSeconds && s.PlayerValid) {
                Enter(XrWorldState::World, s.Now);
                step.Place = true;
                step.PlaceReason = mPlaceReason;
                mEverWorld = true;
            }
            break;
        }
        step.State = mState;
        step.Changed = mState != before;
        return step;
    }

  private:
    void Enter(XrWorldState state, double now) {
        mState = state;
        mSince = now;
        // Cinema is left only after 0.5 s clear counted from its entry (whatever sent it there).
        if (state == XrWorldState::Cinema) mClearSince = now;
    }
    XrWorldState mState = XrWorldState::Cinema;
    double mSince = 0.0;
    double mUiSince = -1.0, mNoPlayerSince = -1.0, mClearSince = -1.0;
    bool mHasPrevious = false, mPreviousPlayerValid = false, mEverWorld = false;
    int16_t mScene = -1;
    int32_t mEntrance = -1;
    M::Vec3 mLink;
    const char* mPlaceReason = "first";
};

inline std::string XrBlockerNames(uint32_t blockers) {
    static constexpr std::pair<uint32_t, const char*> kNames[] = {
        {kXrBlockGameMode, "game_mode"},     {kXrBlockHostGui, "host_gui"},     {kXrBlockUi, "ui"},
        {kXrBlockCutscene, "cutscene"},      {kXrBlockFirstPerson, "first_person"},
        {kXrBlockCrawl, "crawl"},            {kXrBlockFixedCamera, "fixed_camera"},
        {kXrBlockEpona, "epona"},            {kXrBlockNoPlayer, "no_player"},
        {kXrBlockRenderSafety, "render_safety"}, {kXrBlockSession, "session"},
        {kXrBlackDoor, "door"},              {kXrBlackScene, "scene"},          {kXrBlackTeleport, "teleport"},
        {kXrHoldRig, "held"},
    };
    std::string text;
    for (const auto& [bit, name] : kNames) {
        if ((blockers & bit) == 0U) continue;
        if (!text.empty()) text += '|';
        text += name;
    }
    return text.empty() ? "none" : text;
}

// ---------------------------------------------------------------------------------------------------
// PC preview heads (§2.7): eye poses for TRIAEVUM_XR_PREVIEW_HEAD without a headset.

enum class XrPreviewHeadKind : uint8_t { Identity, Offset, Sweep90, Sweep180 };
struct XrPreviewHead {
    XrPreviewHeadKind Kind = XrPreviewHeadKind::Identity;
    double YawDegrees = 0.0, X = 0.0, Y = 0.0, Z = 0.0;  // Offset: relative to the frame camera (m)
    double PeriodSeconds = 8.0;                         // sweeps
    // "identity", "sweep90", "sweep180" or "yaw,x,y,z"; false on anything else.
    bool Parse(std::string_view text) {
        if (text.empty() || text == "identity") {
            Kind = XrPreviewHeadKind::Identity;
            return true;
        }
        if (text == "sweep90" || text == "sweep180") {
            Kind = text == "sweep90" ? XrPreviewHeadKind::Sweep90 : XrPreviewHeadKind::Sweep180;
            return true;
        }
        double values[4] = {};
        size_t count = 0;
        std::string rest(text);
        char* cursor = rest.data();
        while (count < 4) {
            char* stop = nullptr;
            values[count] = std::strtod(cursor, &stop);
            if (stop == cursor || !std::isfinite(values[count])) return false;
            ++count;
            cursor = stop;
            if (*cursor == ',') ++cursor;
            else break;
        }
        if (count != 4 || *cursor != '\0') return false;
        Kind = XrPreviewHeadKind::Offset;
        YawDegrees = values[0];
        X = values[1];
        Y = values[2];
        Z = values[3];
        return true;
    }
    // The head's yaw (degrees, positive turns left) and position (m) in the frame camera's space at time t.
    double YawAt(double t) const {
        if (Kind == XrPreviewHeadKind::Offset) return YawDegrees;
        if (Kind == XrPreviewHeadKind::Identity) return 0.0;
        const double amplitude = Kind == XrPreviewHeadKind::Sweep90 ? 90.0 : 180.0;
        const double phase = std::fmod(std::max(t, 0.0), PeriodSeconds) / PeriodSeconds;  // 0..1
        // Triangle wave -A..A: a constant turn rate, the case that shows screen-locked draws.
        const double tri = phase < 0.5 ? phase * 4.0 - 1.0 : 3.0 - phase * 4.0;
        return amplitude * tri;
    }
};

// V_i for a preview eye: the frame camera moved by the head (camera space, metres * s) and turned by yaw,
// then offset sideways by half the IPD. All in game units; the view looks down -Z.
inline M::Mat4 PreviewEyeView(const M::Mat4& frameView, double yawDegrees, const M::Vec3& headOffsetCamera,
                              double eyeOffsetX) {
    // Eye in the camera's space: T(head) * RotY(yaw) * T(eye x); view = inverse of that, applied after V_g.
    const M::Mat4 eyeInCamera = M::Translation(headOffsetCamera) * M::RotationY(M::DegreesToRadians(yawDegrees)) *
                                M::Translation({eyeOffsetX, 0.0, 0.0});
    M::Mat4 inverse;
    if (!M::InverseAffine(eyeInCamera, &inverse)) return frameView;
    return inverse * frameView;
}

}  // namespace Oot3dNativeGame::XrWorld
