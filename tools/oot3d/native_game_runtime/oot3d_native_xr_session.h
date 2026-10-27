// oot3d_native_xr_session.h: the guest side of the in-world mode's first light (quest-research/inworld-spec.md
// §3.4, §4.1, §4.3, §5.2-5.4, W1 day 4). Immersive world mode only (TRIAEVUM_XR_VIEW=world); everything runs on
// the guest (game) thread except the atomics the render worker reports through.
//
//  - Tick-locked pacing: p_N = p_{N-1} + 3 on a 90 Hz display, presentation time from ticks, the target
//    display time T_N = DisplayTime(p_N + shown latency) the eyes are predicted for.
//  - The W1 state (World / Cinema / Black) from the guest snapshot and the frame facts, with the layer
//    mode requests and TRIAEVUM_XR_CAMERA / TRIAEVUM_XR_FLAGS logs.
//  - The rig: placement (first, recenter, Cinema exit, doors, scene changes), the leash blink and idle
//    re-centre, the floor (grounded / swimming); TRIAEVUM_XR_RIG on every change.
//  - The frame state each render command is drawn with, and TRIAEVUM_XR_TICK every 5 s.
#pragma once

#include "oot3d_native_xr_guest_snapshot.h"
#include "oot3d_xr_world_logic.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>

namespace Oot3dNativeGame {

// What the render command of one host frame needs (captured by value, inworld-spec §4.5).
struct XrFrameState {
    bool World = false;  // render the eyes (the world shows, the rig is placed)
    XrWorld::XrWorldState Mode = XrWorld::XrWorldState::Cinema;
    XrMath::Rig Rig;
    bool Mirrored = false;
    uint64_t PaceTick = 0;    // p_N
    uint64_t TargetTick = 0;  // p_N + shown latency
    int64_t TargetDisplayTimeNs = 0;
    uint32_t RigEpoch = 0;
    bool TickPacing = false;  // the end-of-frame wait is WaitDisplayTick(PaceTick)
};

// The frame facts the classifier needs from a completed visual frame (oot3d_native_xr_view.cpp).
struct XrFrameFacts {
    bool NonHudUi = false;  // PICA Ui draws on the top target besides the gameplay HUD
    uint32_t UiSourcePc = 0;
    bool HasCamera = false;
    bool Mirrored = false;
};

// Host-side signals of the frame.
struct XrHostSignals {
    bool Gameplay = true;  // the native game mode is a gameplay input mode
    bool HostGui = false;  // F1, the menu bar, the in-game menu
};

class XrWorldSession {
  public:
    // Null unless the immersive world mode is on (Quest build, TRIAEVUM_XR=1, TRIAEVUM_XR_VIEW=world).
    static std::unique_ptr<XrWorldSession> Create();

    // RunFrame start: the tick-locked presentation step. True: `elapsedSeconds` is the presentation time
    // of this frame from the display ticks; false: no display clock (use the steady clock).
    bool BeginPresentation(double* elapsedSeconds);
    void NoteRefreshesDue(uint32_t due);  // beats (presentations with 0 or 2+ refreshes outside overruns)

    // Once per completed visual frame (PresentVisualFrame, beside the actor interactions).
    void StampFrame(const XrGuestSnapshot& snapshot, const XrFrameFacts& facts, const XrHostSignals& host);
    XrFrameState FrameState() const { return mState; }

    // Render worker reports (atomics).
    void NoteRenderSafety(bool fallbackFrames) { mRenderSafety.store(fallbackFrames); }
    void NoteTickWait(bool reached) {
        if (!reached) mPacerFallbacks.fetch_add(1);
    }
    void NoteHorizon(double milliseconds);
    void NotePredictFailure() { mPredictFailures.fetch_add(1); }

    // Every 5 s (guest thread): TRIAEVUM_XR_TICK.
    void LogPeriodic(double audioBufferedMs);
    double Scale() const { return mScale; }

  private:
    XrWorldSession() = default;
    void Place(const XrGuestSnapshot& snapshot, const char* reason, double now);
    void BumpEpoch(const char* event, double distanceM);
    void LogRig(const char* event, double distanceM);
    void LogFlags(const XrGuestSnapshot& snapshot, const char* reason);
    XrMath::Vec3 LinkVrWorld(const XrGuestSnapshot& snapshot, bool floor) const;
    bool HeadInTracking(XrMath::Pose* head) const;

    XrFrameState mState;
    XrWorld::XrTickPacer mPacer;
    XrWorld::XrWorldClassifier mClassifier;
    XrWorld::XrWorldState mRequested = XrWorld::XrWorldState::Cinema;
    bool mRigValid = false;
    XrMath::Rig mRig;
    XrMath::TetherConfig mTether;
    XrMath::TetherState mTetherState;
    XrMath::FloorConfig mFloorConfig;
    XrMath::FloorState mFloor;
    XrMath::Vec3 mCentre;      // c in T
    bool mCentreSet = false;   // unbounded spaces: the head at the first placement
    double mScale = XrMath::kDefaultScale;
    double mSeatY = 0.0;       // LOCAL: the floor 1.65 m below the origin
    double mEyeHeightM = 1.6;  // the head over the virtual floor at the last placement
    bool mBounded = false;
    double mLastStamp = -1.0;
    uint32_t mLastFlags1 = 0, mLastFlags2 = 0;
    int16_t mLastSetting = -1;
    uint32_t mLastBlockers = 0;
    bool mLastOverrun = false;
    std::chrono::steady_clock::time_point mStart = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point mNextLog = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    // Counters (5 s windows).
    uint64_t mOverruns = 0, mBeats = 0, mPresentations = 0, mPlacements = 0, mBlinks = 0;
    std::atomic<uint64_t> mPacerFallbacks{0}, mPredictFailures{0};
    std::atomic<uint64_t> mHorizonCount{0};
    std::atomic<double> mHorizonSum{0.0}, mHorizonMax{0.0};
    std::atomic<bool> mRenderSafety{false};
};

}  // namespace Oot3dNativeGame
