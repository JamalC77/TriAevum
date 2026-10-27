// The guest side of the in-world first light (see oot3d_native_xr_session.h).
#include "oot3d_native_xr_session.h"

#include "oot3d_native_xr_view.h"

#if defined(TRIAEVUM_QUEST_OPENXR)
#include "quest_xr.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>

namespace Oot3dNativeGame {

namespace {

namespace W = XrWorld;
namespace M = XrMath;

double EnvNumber(const char* name, double fallback, double low, double high) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') return fallback;
    char* end = nullptr;
    const double parsed = std::strtod(value, &end);
    if (end == value || !std::isfinite(parsed)) return fallback;
    return std::clamp(parsed, low, high);
}

bool EnvIs(const char* name, std::string_view expected) {
    const char* value = std::getenv(name);
    return value != nullptr && std::string_view(value) == expected;
}

#if defined(TRIAEVUM_QUEST_OPENXR)
TriAevumQuestXr::LayerMode LayerFor(W::XrWorldState state) {
    switch (state) {
    case W::XrWorldState::World: return TriAevumQuestXr::LayerMode::World;
    case W::XrWorldState::Cinema: return TriAevumQuestXr::LayerMode::Cinema;
    case W::XrWorldState::Black: return TriAevumQuestXr::LayerMode::Black;
    }
    return TriAevumQuestXr::LayerMode::Cinema;
}
#endif

// TopScreen's grounded test (ApplyTopScreenFreeCameraData): playerY - floorHeight < 2.0 u.
constexpr double kGroundedEpsilonU = 2.0;
constexpr uint32_t kFlags1Hanging = 0x00002000U;
constexpr uint32_t kFlags1Climbing = 0x00200000U;  // decomp PLAYER_STATE1_CLIMBING_LADDER [unverified name]
constexpr uint32_t kFlags1Swimming = 0x08000000U;

}  // namespace

std::unique_ptr<XrWorldSession> XrWorldSession::Create() {
#if defined(TRIAEVUM_QUEST_OPENXR)
    if (!TriAevumQuestXr::Requested() || !TriAevumQuestXr::WorldRequested()) return nullptr;
    auto session = std::unique_ptr<XrWorldSession>(new XrWorldSession());
    session->mScale = EnvNumber("TRIAEVUM_XR_WORLD_SCALE", M::kDefaultScale, 25.0, 45.0);
    session->mTether.LeashM = EnvNumber("TRIAEVUM_XR_LEASH_M", M::kDefaultLeashM, 1.0, 10.0);
    session->mTether.Mode = M::TetherMode::Leash;
    XrWorldLog("TRIAEVUM_XR_WORLD_SESSION scale=%.1f leash_m=%.2f camera=native ui_cinema=%d force_world=%d",
               session->mScale, session->mTether.LeashM, EnvIs("TRIAEVUM_XR_UI_CINEMA", "0") ? 0 : 1,
               EnvIs("TRIAEVUM_XR_FORCE_WORLD", "1") ? 1 : 0);
    return session;
#else
    return nullptr;
#endif
}

bool XrWorldSession::BeginPresentation(double* elapsedSeconds) {
#if defined(TRIAEVUM_QUEST_OPENXR)
    const auto tick = TriAevumQuestXr::LatestTick();
    if (!tick.Valid || tick.PeriodNs <= 0) {
        mPacer.Restart();
        mState.TickPacing = false;
        return false;
    }
    const double period = static_cast<double>(tick.PeriodNs) * 1e-9;
    const uint32_t divisor = W::TickDivisor(1.0 / period);
    if (divisor != mPacer.Divisor()) {
        XrWorldLog("TRIAEVUM_XR_TICK_DIVISOR refresh_hz=%.1f divisor=%u eyes_hz=%.1f", 1.0 / period, divisor,
                   1.0 / (period * divisor));
        mPacer.SetDivisor(divisor);
        mPacer.Restart();
    }
    const uint64_t previousPace = mPacer.Previous();
    const auto step = mPacer.Next(tick.Tick);
    ++mPresentations;
    if (step.Overrun) ++mOverruns;
    if (step.Resynced) {
        // The pace led the display clock by more than 2d + margin (tick waits that timed out): pulled
        // back to latest + d, logged on the first one of a 5 s window.
        if (mResyncs++ == 0U)
            XrWorldLog("TRIAEVUM_XR_TICK resync pace_was=%llu latest=%llu pace=%llu",
                       static_cast<unsigned long long>(previousPace + divisor),
                       static_cast<unsigned long long>(tick.Tick), static_cast<unsigned long long>(step.PaceTick));
    }
    mLastOverrun = step.Overrun || step.Started || step.Resynced;
    const uint32_t latency = TriAevumQuestXr::ShownLatencyTicks();
    mState.PaceTick = step.PaceTick;
    mState.TargetTick = step.PaceTick + latency;
    mState.TargetDisplayTimeNs = W::TickDisplayTimeNs(tick.Tick, tick.DisplayTimeNs, tick.PeriodNs, mState.TargetTick);
    mState.TickPacing = true;
    // Presentation time from the ticks (§3.4 item 1). `divisor` ticks are exactly one 30 Hz guest
    // refresh when the display rate is a multiple of 30: the period's deviation from 1/90 s must not
    // drift the guest clock into a 0- or 2-refresh frame.
    constexpr double kGuestFrame = 1.0 / 30.0;
    double seconds = static_cast<double>(step.Delta) * period;
    if (std::fabs(divisor * period - kGuestFrame) <= 0.02 * kGuestFrame)
        seconds = static_cast<double>(step.Delta) * (kGuestFrame / divisor);
    *elapsedSeconds = std::min(seconds, 0.1);  // after a pause: the scheduler's catch-up cap
    return true;
#else
    (void)elapsedSeconds;
    return false;
#endif
}

void XrWorldSession::NoteRefreshesDue(uint32_t due) {
    if (mState.TickPacing && !mLastOverrun && due != 1U) ++mBeats;
}

void XrWorldSession::NoteHeldFrame() {
    // BeginPresentation already advanced p_N for this frame; nothing waited for that tick and the
    // scheduler restored its clock, so the next frame starts the pace again from the latest tick.
    mPacer.Restart();
    ++mHeldFrames;
}

void XrWorldSession::NoteHorizon(double milliseconds) {
    if (!std::isfinite(milliseconds)) return;
    mHorizonCount.fetch_add(1);
    mHorizonSum.store(mHorizonSum.load() + milliseconds);
    if (milliseconds > mHorizonMax.load()) mHorizonMax.store(milliseconds);
}

M::Vec3 XrWorldSession::LinkVrWorld(const XrGuestSnapshot& snapshot, bool floor) const {
    M::Vec3 link{snapshot.Position[0], snapshot.Position[1], snapshot.Position[2]};
    // Placement puts Link's floor on the real floor: his feet unless he is in the air.
    if (floor && std::isfinite(snapshot.FloorHeight) && link.Y - snapshot.FloorHeight < 3.0 * mScale &&
        link.Y >= snapshot.FloorHeight - kGroundedEpsilonU)
        link.Y = snapshot.FloorHeight;
    return M::ToVrWorld(link, mState.Mirrored);
}

bool XrWorldSession::HeadInTracking(M::Pose* head) const {
#if defined(TRIAEVUM_QUEST_OPENXR)
    const auto tick = TriAevumQuestXr::LatestTick();
    TriAevumQuestXr::HeadPose pose;
    if (!tick.Valid || !TriAevumQuestXr::LocateHead(tick.DisplayTimeNs, &pose)) return false;
    *head = M::PoseFromXr(pose.Orientation, pose.Position);
    return true;
#else
    (void)head;
    return false;
#endif
}

void XrWorldSession::LogRig(const char* event, double distanceM) {
    M::Pose head;
    const bool hasHead = HeadInTracking(&head);
    const M::Vec3 linkT = mFloor.Initialized ? M::WorldToTracking(mRig, mTetherState.LastLinkWorld) : M::Vec3{};
    XrWorldLog("TRIAEVUM_XR_RIG event=%s link_t=%.2f,%.2f head_t=%.2f,%.2f psi=%.1f s=%.1f dist_m=%.2f epoch=%u "
               "anchor=%.1f,%.1f,%.1f seat_y=%.2f bounded=%d head=%d",
               event, linkT.X, linkT.Z, hasHead ? head.Position.X : 0.0, hasHead ? head.Position.Z : 0.0,
               M::RadiansToDegrees(mRig.Yaw), mRig.Scale, distanceM, mState.RigEpoch, mRig.Anchor.X, mRig.Anchor.Y,
               mRig.Anchor.Z, mSeatY, mBounded ? 1 : 0, hasHead ? 1 : 0);
}

void XrWorldSession::BumpEpoch(const char* event, double distanceM) {
    ++mState.RigEpoch;
    LogRig(event, distanceM);
}

void XrWorldSession::Place(const XrGuestSnapshot& snapshot, const char* reason, double now) {
#if defined(TRIAEVUM_QUEST_OPENXR)
    const auto area = TriAevumQuestXr::WorldPlayArea();
    mBounded = area.Bounded;
    mSeatY = area.FloorBelowOriginM;
    mTether.Area = {area.HalfX, area.HalfZ, area.Bounded};
#endif
    M::Pose head;
    if (!HeadInTracking(&head)) {
        static double lastLog = -10.0;
        if (now - lastLog >= 1.0) {  // retried every frame: logged once a second
            lastLog = now;
            XrWorldLog("TRIAEVUM_XR_RIG event=place_failed reason=%s cause=no_head", reason);
        }
        return;
    }
    if (!mBounded && !mCentreSet) {
        // Without bounds, c is the head at the first placement (§5.2).
        mCentre = {head.Position.X, 0.0, head.Position.Z};
        mCentreSet = true;
    } else if (mBounded) {
        mCentre = {};
    }
    M::PlacementConfig config;
    config.Scale = mScale;
    config.SeatY = mSeatY;
    M::PlacementInput input;
    input.LinkWorld = LinkVrWorld(snapshot, true);
    input.LinkFacingYaw = M::YawToVrWorld(M::BinAngleToRadians(snapshot.ShapeYaw), mState.Mirrored);
    input.Head = head;
    input.CentreT = mCentre;
    const auto result = M::SolvePlacement(input, config);
    if (!result.Valid) {
        XrWorldLog("TRIAEVUM_XR_RIG event=place_failed reason=%s cause=solve", reason);
        return;
    }
    mRig = result.NewRig;
    mRigValid = true;
    mTether.CentreT = mCentre;
    mTetherState = {};
    mTetherState.LastLinkWorld = LinkVrWorld(snapshot, false);
    M::ResetFloor(mFloor, mRig.Anchor.Y);
    mEyeHeightM = head.Position.Y + mSeatY;
    ++mPlacements;
    const std::string_view why(reason);
    const char* event = why == "door" ? "door" : why == "recenter" ? "recenter" : "place";
    ++mState.RigEpoch;
    XrWorldLog("TRIAEVUM_XR_RIG event=%s reason=%s link_t=%.2f,%.2f head_t=%.2f,%.2f psi=%.1f s=%.1f dist_m=%.2f "
               "near_centre=%d epoch=%u anchor=%.1f,%.1f,%.1f eye_height_m=%.2f seat_y=%.2f bounded=%d",
               event, reason, result.LinkT.X, result.LinkT.Z, head.Position.X, head.Position.Z,
               M::RadiansToDegrees(mRig.Yaw), mRig.Scale, result.DistanceM, result.NearCentre ? 1 : 0,
               mState.RigEpoch, mRig.Anchor.X, mRig.Anchor.Y, mRig.Anchor.Z, mEyeHeightM, mSeatY, mBounded ? 1 : 0);
#if defined(TRIAEVUM_QUEST_OPENXR)
    TriAevumQuestXr::Pulse(2, 60);
#endif
}

void XrWorldSession::LogFlags(const XrGuestSnapshot& snapshot, const char* reason) {
    XrWorldLog("TRIAEVUM_XR_FLAGS reason=%s flags1=0x%08X flags2=0x%08X setting=0x%02X status=%d active_camera=%d "
               "cutscene=0x%X scene=0x%X entrance=0x%X game_mode=%d",
               reason, snapshot.Flags1, snapshot.Flags2, static_cast<unsigned>(static_cast<uint16_t>(snapshot.CameraSetting)),
               snapshot.CameraStatus, snapshot.ActiveCamera, static_cast<unsigned>(snapshot.CutsceneIndex),
               static_cast<unsigned>(static_cast<uint16_t>(snapshot.Scene)), static_cast<unsigned>(snapshot.Entrance),
               snapshot.SaveGameMode);
}

void XrWorldSession::StampFrame(const XrGuestSnapshot& snapshot, const XrFrameFacts& facts,
                                const XrHostSignals& host) {
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - mStart).count();
    const double dt = mLastStamp < 0.0 ? 0.0 : std::clamp(now - mLastStamp, 0.0, 0.2);
    mLastStamp = now;
    if (facts.HasCamera) mState.Mirrored = facts.Mirrored;

    W::XrWorldSignals signals;
    signals.Now = now;
    signals.Gameplay = host.Gameplay && snapshot.SaveGameMode == 0;
    signals.HostGui = host.HostGui;
    signals.UiDraws = facts.NonHudUi;
    signals.UiRuleEnabled = !EnvIs("TRIAEVUM_XR_UI_CINEMA", "0");
    signals.PlayerValid = snapshot.PlayerValid;
    signals.SessionReady = mState.TickPacing;
    {
        // The render safety (§2.3) as a timed hold: Cinema for 5 s (doubling per repeat, 60 s cap, a
        // scene change ends it), then the reported flag and the render worker's count restart.
        const bool sceneChanged = snapshot.PlayerValid && snapshot.Scene != mSafetyScene;
        if (snapshot.PlayerValid) mSafetyScene = snapshot.Scene;
        const auto safety = mSafety.Update(mRenderSafety.load(), now, sceneChanged);
        if (safety.Tripped)
            XrWorldLog("TRIAEVUM_XR_VIEW render_safety=tripped hold_s=%.0f scene=0x%X", safety.HoldSeconds,
                       static_cast<unsigned>(static_cast<uint16_t>(snapshot.Scene)));
        if (safety.Cleared) {
            mRenderSafety.store(false);
            mRenderSafetyReset.store(true);
            XrWorldLog("TRIAEVUM_XR_VIEW render_safety=cleared reason=%s hold_s=%.0f", safety.ClearReason,
                       safety.HoldSeconds);
        }
        signals.RenderSafety = safety.Blocking;
    }
    signals.Flags1 = snapshot.Flags1;
    signals.Flags2 = snapshot.Flags2;
    signals.CameraSetting = snapshot.CameraValid ? snapshot.CameraSetting : 0;
    signals.CameraStatus = snapshot.CameraValid ? snapshot.CameraStatus : 7;
    signals.ActiveCamera = snapshot.ActiveCamera;
    signals.CutsceneIndex = snapshot.CutsceneIndex;
    signals.Scene = snapshot.Scene;
    signals.Entrance = snapshot.Entrance;
    signals.Link = {snapshot.Position[0], snapshot.Position[1], snapshot.Position[2]};
    signals.Scale = mScale;
    static const bool forceWorld = EnvIs("TRIAEVUM_XR_FORCE_WORLD", "1");
    if (forceWorld) {
        // Bring-up: every Cinema blocker off but the session and the player.
        signals.Gameplay = true;
        signals.HostGui = false;
        signals.UiDraws = false;
        signals.Flags1 = signals.Flags2 = 0;
        signals.CutsceneIndex = 0;
        signals.RenderSafety = false;
        if (W::XrDoorSetting(signals.CameraSetting) || (signals.CameraSetting >= 0x16 && signals.CameraSetting <= 0x1E))
            signals.CameraSetting = 0;
    }
    const auto step = mClassifier.Update(signals);

    if (snapshot.PlayerValid && (snapshot.Flags1 != mLastFlags1 || snapshot.Flags2 != mLastFlags2 ||
                                 snapshot.CameraSetting != mLastSetting)) {
        LogFlags(snapshot, "edge");
        mLastFlags1 = snapshot.Flags1;
        mLastFlags2 = snapshot.Flags2;
        mLastSetting = snapshot.CameraSetting;
    }
    const uint32_t visible = step.Blockers & (W::kXrCinemaMask | W::kXrBlackMask | W::kXrHoldRig);
    if (step.Changed || visible != mLastBlockers) {
        XrWorldLog("TRIAEVUM_XR_CAMERA state=%s owner=native blockers=0x%08X (%s) flags1=0x%08X flags2=0x%08X "
                   "setting=0x%02X status=%d ui_pc=0x%08X player=%d scene=0x%X",
                   W::XrWorldStateName(step.State), step.Blockers, W::XrBlockerNames(step.Blockers).c_str(),
                   snapshot.Flags1, snapshot.Flags2,
                   static_cast<unsigned>(static_cast<uint16_t>(snapshot.CameraSetting)), snapshot.CameraStatus,
                   facts.UiSourcePc, snapshot.PlayerValid ? 1 : 0,
                   static_cast<unsigned>(static_cast<uint16_t>(snapshot.Scene)));
        mLastBlockers = visible;
    }

    if (step.Place) {
        Place(snapshot, step.PlaceReason, now);
    } else if (step.State == W::XrWorldState::World && !mRigValid && snapshot.PlayerValid) {
        // A placement that found no head (tracking lost) is retried each frame; Black meanwhile.
        Place(snapshot, "retry", now);
    }
#if defined(TRIAEVUM_QUEST_OPENXR)
    const uint32_t recenters = TriAevumQuestXr::TakeWorldRecenter();
    if (recenters != 0U && step.State == W::XrWorldState::World && snapshot.PlayerValid) {
        const uint32_t before = mState.RigEpoch;
        Place(snapshot, "recenter", now);
        if (mState.RigEpoch != before) TriAevumQuestXr::RequestBlink(mState.RigEpoch, 150);
    }
#endif

    // Following Link (§5.3, §5.4): not while a Held row freezes the rig (talks, item gets, subcameras).
    if (step.State == W::XrWorldState::World && mRigValid && snapshot.PlayerValid &&
        (step.Blockers & W::kXrHoldRig) == 0U && dt > 0.0) {
        M::Pose head;
        if (HeadInTracking(&head)) {
            const M::Vec3 linkWorld = LinkVrWorld(snapshot, false);
            const double facing = M::YawToVrWorld(M::BinAngleToRadians(snapshot.ShapeYaw), mState.Mirrored);
            const auto tether = M::UpdateTether(mTetherState, mTether, mRig, linkWorld, facing, head.Position, dt);
            if (tether.Event == M::RigEvent::Blink || tether.Event == M::RigEvent::Idle) {
                mRig = tether.NewRig;
                ++mBlinks;
                BumpEpoch(M::RigEventName(tether.Event), tether.HeadDistanceM);
#if defined(TRIAEVUM_QUEST_OPENXR)
                TriAevumQuestXr::RequestBlink(mState.RigEpoch, 120);
#endif
            }
            M::FloorInput floor;
            floor.LinkY = linkWorld.Y;
            floor.FloorY = std::isfinite(snapshot.FloorHeight) ? snapshot.FloorHeight
                                                                : std::numeric_limits<double>::quiet_NaN();
            floor.Hanging = (snapshot.Flags1 & kFlags1Hanging) != 0U;
            floor.Climbing = (snapshot.Flags1 & kFlags1Climbing) != 0U;
            floor.Swimming = (snapshot.Flags1 & kFlags1Swimming) != 0U;
            floor.WaterY = std::isfinite(snapshot.WaterY) ? snapshot.WaterY : std::numeric_limits<double>::quiet_NaN();
            floor.EyeHeightM = mEyeHeightM;
            floor.Scale = mScale;
            floor.Dt = dt;
            const auto floorStep = M::UpdateFloor(mFloor, mFloorConfig, floor);
            if (std::isfinite(floorStep.AnchorY)) mRig.Anchor.Y = floorStep.AnchorY;
            if (floorStep.Blink) {
                ++mBlinks;
                BumpEpoch("floor", 0.0);
#if defined(TRIAEVUM_QUEST_OPENXR)
                TriAevumQuestXr::RequestBlink(mState.RigEpoch, 120);
#endif
            }
        }
    }

    const bool world = step.State == W::XrWorldState::World && mRigValid;
    const W::XrWorldState shown = step.State == W::XrWorldState::World && !mRigValid ? W::XrWorldState::Black : step.State;
    if (shown != mRequested) {
#if defined(TRIAEVUM_QUEST_OPENXR)
        TriAevumQuestXr::RequestLayerMode(LayerFor(shown), 150, mState.RigEpoch);
#endif
        mRequested = shown;
    }
    mState.Mode = shown;
    mState.World = world;
    mState.Rig = mRig;
}

void XrWorldSession::LogPeriodic(double audioBufferedMs) {
    const auto now = std::chrono::steady_clock::now();
    if (now < mNextLog) return;
    mNextLog = now + std::chrono::seconds(5);
#if defined(TRIAEVUM_QUEST_OPENXR)
    const auto pairs = TriAevumQuestXr::TakePairStats();
#else
    struct {
        uint64_t Pairs = 0, LatePairs = 0;
        uint32_t LatencyTicks = 0;
    } pairs;
#endif
    const uint64_t horizons = mHorizonCount.exchange(0);
    const double horizonSum = mHorizonSum.exchange(0.0);
    const double horizonMax = mHorizonMax.exchange(0.0);
    XrWorldLog("TRIAEVUM_XR_TICK pairs=%llu late_pairs=%llu overruns=%llu beats=%llu presentations=%llu lat_ticks=%u "
               "horizon_ms_mean=%.1f horizon_ms_max=%.1f pacer_fallbacks=%llu predict_failures=%llu "
               "audio_buffered_ms=%.0f divisor=%u pacing=%d state=%s placements=%llu blinks=%llu epoch=%u "
               "resyncs=%llu held_frames=%llu render_safety=%d",
               static_cast<unsigned long long>(pairs.Pairs), static_cast<unsigned long long>(pairs.LatePairs),
               static_cast<unsigned long long>(mOverruns), static_cast<unsigned long long>(mBeats),
               static_cast<unsigned long long>(mPresentations), pairs.LatencyTicks,
               horizons != 0U ? horizonSum / static_cast<double>(horizons) : 0.0, horizonMax,
               static_cast<unsigned long long>(mPacerFallbacks.exchange(0)),
               static_cast<unsigned long long>(mPredictFailures.exchange(0)), audioBufferedMs, mPacer.Divisor(),
               mState.TickPacing ? 1 : 0, W::XrWorldStateName(mState.Mode),
               static_cast<unsigned long long>(mPlacements), static_cast<unsigned long long>(mBlinks),
               mState.RigEpoch, static_cast<unsigned long long>(mResyncs),
               static_cast<unsigned long long>(mHeldFrames), mSafety.Holding() ? 1 : 0);
    if (mState.World) {
        M::Pose head;
        const bool hasHead = HeadInTracking(&head);
        const M::Vec3 linkT = M::WorldToTracking(mRig, mTetherState.LastLinkWorld);
        XrWorldLog("TRIAEVUM_XR_RIG event=status link_t=%.2f,%.2f,%.2f head_t=%.2f,%.2f,%.2f dist_m=%.2f psi=%.1f "
                   "floor_mode=%d anchor_y=%.1f epoch=%u",
                   linkT.X, linkT.Y, linkT.Z, hasHead ? head.Position.X : 0.0, hasHead ? head.Position.Y : 0.0,
                   hasHead ? head.Position.Z : 0.0,
                   hasHead ? M::DistanceXZ(linkT, head.Position) : -1.0, M::RadiansToDegrees(mRig.Yaw),
                   static_cast<int>(mFloor.Mode), mRig.Anchor.Y, mState.RigEpoch);
    }
    mOverruns = mBeats = mPresentations = mPlacements = mBlinks = mResyncs = mHeldFrames = 0;
}

}  // namespace Oot3dNativeGame
