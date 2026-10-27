// oot3d_native_xr_session.h: the guest side of the in-world mode (quest-research/inworld-spec.md §3.4, §4,
// §5, §7; W1 first light, W2 mode R). Immersive world mode only (TRIAEVUM_XR_VIEW=world); everything runs
// on the guest (game) thread except the atomics the render worker reports through.
//
//  - Tick-locked pacing: p_N = p_{N-1} + 3 on a 90 Hz display, presentation time from ticks, the target
//    display time T_N = DisplayTime(p_N + shown latency) the eyes are predicted for.
//  - The layer state (World / Cinema / Black) from the guest snapshot and the frame facts, with the layer
//    mode requests and TRIAEVUM_XR_CAMERA / TRIAEVUM_XR_FLAGS logs.
//  - The rig: placement (first, recenter, Cinema exit, doors, scene changes), the leash blink and idle
//    re-centre, the floor (grounded / swimming), snap turns; TRIAEVUM_XR_RIG on every change.
//  - Mode R (W2, TRIAEVUM_XR_CAMERA=r, the default with the TopScreen profile): at every main-camera
//    Camera_Update (VrCameraSource, oot3d_native_vr_camera.h) the owner decision of §4.3, the camera write
//    from the head predicted for T_N, the head-in-geometry segment and its wall fade, the placement probes
//    that hold a placement in Black, the cutscene-camera motion test, R_always for the actor culling.
//    Every rig change (turns, recenters, the placement a probe run chose, following Link, the floor) is
//    applied there too, before the head is computed (§4.5), so the frame a camera write culled is drawn
//    with the same rig; StampFrame then publishes (rig, epoch) with the frame. Without the hook (W1's
//    TRIAEVUM_XR_CAMERA=native, mode L) StampFrame applies them as in W1.
//  - The stick (§7): the Circle Pad of each guest refresh rotated from the camera yaw to psi_ref.
//  - The frame state each render command is drawn with, and TRIAEVUM_XR_TICK / _CAMWRITE every 5 s.
#pragma once

#include "oot3d_native_xr_guest_snapshot.h"
#include "oot3d_xr_camera_logic.h"
#include "oot3d_xr_world_logic.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>

namespace Oot3dNativeGame {

struct NativeActorVisibilityState;

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

// Who drives the main camera in world mode (TRIAEVUM_XR_CAMERA).
enum class XrCameraMode : uint8_t {
    Native,  // W1: no Camera_Update hook, no stick rotation (A/B against mode R)
    ModeR,   // W2: the head owns the camera through the TopScreen phases (§4.2)
    Held,    // mode L (ladder I2): the native camera, the rig live, the stick rotated (§7)
};
const char* XrCameraModeName(XrCameraMode mode);

// One main-camera Camera_Update as VrCameraSource read it (game world).
struct XrCameraGuest {
    bool MainCamera = false;
    uint32_t Camera = 0, PlayState = 0;
    XrMath::Vec3 CameraEye, CameraAt;  // the camera's current eye/at (+0x8C/+0x80)
    XrMath::Vec3 ViewEye, ViewAt;      // the play view (+0x1B8/+0x1C4): whichever camera drew last
    float ViewFov = 0.0F;              // play+0x198
    int16_t Status = 7, Setting = 0;
};
// What the host does with that update.
struct XrCameraUpdate {
    bool Own = false;
    bool Probe = false;                 // a placement probe: the camera holds where it is
    XrCamera::GeometrySegment Segment;  // the update's one BgCheck segment
    XrCamera::CameraWrite Write;
    float FovDegrees = 0.0F;
};

class XrWorldSession {
  public:
    // Null unless the immersive world mode is on (Quest build, TRIAEVUM_XR=1, TRIAEVUM_XR_VIEW=world).
    static std::unique_ptr<XrWorldSession> Create();

    // Window setup, once: the camera mode from TRIAEVUM_XR_CAMERA and whether the TopScreen profile (the
    // only one with an observable Camera_Update) is active. True: install a VrCameraSource.
    bool ConfigureCamera(bool topScreenProfile);
    XrCameraMode CameraMode() const { return mCameraMode; }
    void SetActorVisibility(NativeActorVisibilityState* actors) { mActors = actors; }

    // RunFrame start: the tick-locked presentation step. True: `elapsedSeconds` is the presentation time
    // of this frame from the display ticks; false: no display clock (use the steady clock).
    bool BeginPresentation(double* elapsedSeconds);
    void NoteRefreshesDue(uint32_t due);  // beats (presentations with 0 or 2+ refreshes outside overruns)
    // A RunFrame that held the guest (session not focused, the preparation screen): its presentation
    // time was discarded and its tick never waited for, so the pace restarts from the display clock.
    void NoteHeldFrame();

    // Once per completed visual frame (PresentVisualFrame, beside the actor interactions). `sequence` is
    // the visual frame's (the camera write it was drawn with carried the sequence being recorded).
    void StampFrame(const XrGuestSnapshot& snapshot, const XrFrameFacts& facts, const XrHostSignals& host,
                    uint64_t sequence = 0);
    XrFrameState FrameState() const { return mState; }

    // Mode R, at every main-camera Camera_Update entry (guest thread, VrCameraSource). `recording` is the
    // visual-frame sequence being recorded.
    XrCameraUpdate BeginCameraUpdate(const XrGuestSnapshot& snapshot, const XrCameraGuest& camera,
                                     uint64_t recording);
    // The update's BgCheck result: the collided end of the segment Begin returned.
    void CommitCameraUpdate(const XrMath::Vec3& collided);
    void NoteCameraQuake(bool active);
    // FovScalar of the main view: the cull FOV while the head owned the last main-camera update and the
    // hook still runs (the world shows).
    bool CameraFov(float loaded, float* degrees);
    // The FovScalar's loaded value for the native path: a cull FOV the host left in play+0x198 with no
    // native camera update since (the pause screen stops Camera_Update) becomes the last native FOV.
    float NativeLoadedFov(float loaded);

    // §7: the Circle Pad of one guest refresh (SampleGuestRefreshInput), rotated in place.
    void RotateStick(const XrGuestSnapshot& snapshot, bool ocarina, int16_t& x, int16_t& y);

    // Render worker reports (atomics).
    void NoteRenderSafety(bool fallbackFrames) { mRenderSafety.store(fallbackFrames); }
    // Render worker: true once after a render-safety hold ended (the fallback frame count restarts).
    bool TakeRenderSafetyReset() { return mRenderSafetyReset.exchange(false); }
    void NoteTickWait(bool reached) {
        if (!reached) mPacerFallbacks.fetch_add(1);
    }
    void NoteHorizon(double milliseconds);
    void NotePredictFailure() { mPredictFailures.fetch_add(1); }

    // Every 5 s (guest thread): TRIAEVUM_XR_TICK, TRIAEVUM_XR_CAMWRITE.
    void LogPeriodic(double audioBufferedMs);
    double Scale() const { return mScale; }

  private:
    struct ProbeOutcome {
        bool Used = false;
        XrCamera::ProbeChoice Choice;
        bool TimedOut = false;
    };
    XrWorldSession() = default;
    double Now() const;
    void Place(const XrGuestSnapshot& snapshot, const char* reason, double now, const ProbeOutcome* probes = nullptr);
    void StartPlacement(const XrGuestSnapshot& snapshot, const char* reason, double now);
    void FinishPendingPlacement(const XrGuestSnapshot& snapshot, double now, bool force);
    void ApplyRigInputs(const XrGuestSnapshot& snapshot, double now, double dt);
    // Following Link (§5.3) and the floor (§5.4): at Camera_Update while the hook runs, else at the stamp.
    void FollowLink(const XrGuestSnapshot& snapshot, double dt, bool frozen);
    void BumpEpoch(const char* event, double distanceM);
    void LogRig(const char* event, double distanceM);
    void LogFlags(const XrGuestSnapshot& snapshot, const char* reason);
    void LogDecision(const XrCamera::OwnerDecision& decision, const XrGuestSnapshot& snapshot,
                     const XrCameraGuest& camera);
    void SetWallFade(double fade, const char* cause);
    void PublishActorHead(bool valid, const XrMath::Vec3& head);
    XrMath::Vec3 LinkVrWorld(const XrGuestSnapshot& snapshot, bool floor) const;
    bool HeadInTracking(XrMath::Pose* head) const;
    bool HeadAt(int64_t displayTimeNs, XrMath::Pose* head) const;
    bool HookLive(double now) const { return mModeR && mLastCameraUpdate >= 0.0 && now - mLastCameraUpdate < 0.25; }
    double ProbeLengthM() const;
    XrMath::Vec3 LinkHeadGame(const XrGuestSnapshot& snapshot) const;

    XrFrameState mState;
    XrWorld::XrTickPacer mPacer;
    XrWorld::XrWorldClassifier mClassifier;
    XrWorld::XrRenderSafetyHold mSafety;
    int16_t mSafetyScene = -1;
    XrWorld::XrWorldState mRequested = XrWorld::XrWorldState::Cinema;
    bool mRigValid = false;
    XrMath::Rig mRig;
    uint32_t mRigEpoch = 0;  // the rig's epoch; published as mState.RigEpoch with mRig at the stamp
    // The classifier's last state and blockers (StampFrame), for the decisions taken at Camera_Update.
    XrWorld::XrWorldState mClassifierState = XrWorld::XrWorldState::Cinema;
    uint32_t mClassifierBlockers = 0;
    XrMath::TetherConfig mTether;
    XrMath::TetherState mTetherState;
    XrMath::FloorConfig mFloorConfig;
    XrMath::FloorState mFloor;
    XrMath::Vec3 mCentre;      // c in T
    bool mCentreSet = false;   // unbounded spaces: the head at the first placement
    double mScale = XrMath::kDefaultScale;
    double mPlaceM = XrMath::kDefaultPlaceDistanceM;  // TRIAEVUM_XR_PLACE_M: head to Link at placements
    double mSeatY = 0.0;       // LOCAL: the floor 1.65 m below the origin; seated: plus the seat offset
    double mEyeHeightM = 1.6;  // the head over the virtual floor at the last placement
    bool mBounded = false;
    bool mSeated = false;      // TRIAEVUM_XR_SEATED=1 (§5.6)
    double mLastStamp = -1.0;
    uint32_t mLastFlags1 = 0, mLastFlags2 = 0;
    int16_t mLastSetting = -1;
    uint32_t mLastBlockers = 0;
    bool mLastOverrun = false;
    std::chrono::steady_clock::time_point mStart = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point mNextLog = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    // Counters (5 s windows).
    uint64_t mOverruns = 0, mBeats = 0, mPresentations = 0, mPlacements = 0, mBlinks = 0, mResyncs = 0,
             mHeldFrames = 0;
    std::atomic<uint64_t> mPacerFallbacks{0}, mPredictFailures{0};
    std::atomic<uint64_t> mHorizonCount{0};
    std::atomic<double> mHorizonSum{0.0}, mHorizonMax{0.0};
    std::atomic<bool> mRenderSafety{false};
    std::atomic<bool> mRenderSafetyReset{false};

    // ---- Mode R and the W2 rig inputs (guest thread)
    XrCameraMode mCameraMode = XrCameraMode::Native;
    bool mModeR = false;                // the hook is installed
    bool mStickEnabled = false;
    double mCullFov = XrCamera::kDefaultCullFovDegrees;
    XrCamera::MoveReference mMoveRef = XrCamera::MoveReference::HeadLatched;
    double mTurnDegrees = XrMath::kDefaultSnapDegrees;
    bool mSmoothTurn = true;
    double mSmoothTurnDegreesPerSecond = 120.0;
    double mLastCameraUpdate = -1.0;
    double mLastRigInputs = -1.0;
    XrCamera::OwnerDecision mDecision;
    bool mDecisionLogged = false;
    XrCamera::OwnerDecision mLoggedDecision;
    bool mOwnedLast = false;            // the last main-camera update was head-owned (FovScalar); cleared
                                        // when the world stops showing or the hook stops running
    bool mLastUpdateHeadOwned = false;  // ... the same, never cleared by the stamp (the soft reseed)
    XrGuestSnapshot mCameraSnapshot;    // the snapshot of the running Camera_Update
    bool mFirstWriteLogged = false;
    XrCamera::CameraWrite mWrite;
    XrCamera::GameHead mHeadGame;
    XrMath::Vec3 mProbeFrom;
    int mProbeIndex = -1;
    XrCamera::HeadInGeometry mGeometry;
    bool mGeometryResetPending = true;  // a placement: q restarts at the next written head (§4.4)
    double mWallFade = 0.0;
    double mWriteFov = XrCamera::kDefaultCullFovDegrees;  // the FOV of the last head write (pitch-widened)
    float mLastHostFov = 0.0F;                             // ... as written to play+0x198
    float mNativeFov = 0.0F;                               // the last native FOV the FovScalar loaded
    bool mNativeFovValid = false, mFovRestoring = false;
    XrCamera::PlacementProbes mProbes;
    struct PendingPlacement {
        bool Active = false;
        const char* Reason = "";
        double Since = 0.0;
        uint32_t HeldBypassed = 0;  // Held reasons the probes ran over (§4.4: they never wait for them)
        uint32_t HeldUpdates = 0;
    } mPending;
    XrCamera::CutsceneCameraMonitor mCutsceneMonitor;
    bool mCutsceneMoving = false;
    uint32_t mHeldStaticRun = 0;        // consecutive Held-static main-camera updates
    XrCamera::StickReference mStick;
    double mLastStickTime = -1.0;
    double mLastStickLog = -1.0;
    uint64_t mRecordingSequence = 0;
    NativeActorVisibilityState* mActors = nullptr;
    bool mActorHeadPublished = false;
    std::array<uint64_t, 13> mHeldEntries{};
    // 5 s counters.
    uint64_t mCamUpdates = 0, mCamOwned = 0, mCamProbes = 0, mCamHeld = 0, mHeadFailures = 0, mFovHooks = 0,
             mWallFadeUpdates = 0, mStickRotations = 0, mStickBypassed = 0, mTurns = 0, mQuakes = 0,
             mSeqSamples = 0, mProbeRuns = 0, mFovWidened = 0, mFovRestored = 0, mReseeds = 0, mRescues = 0,
             mReseedHits = 0;
    double mSeqLagSum = 0.0, mThetaAbsMax = 0.0, mFovMax = 0.0;
    float mFovLoaded = 0.0F;
};

}  // namespace Oot3dNativeGame
