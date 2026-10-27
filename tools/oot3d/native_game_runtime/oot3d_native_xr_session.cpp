// The guest side of the in-world mode (see oot3d_native_xr_session.h).
#include "oot3d_native_xr_session.h"

#include "oot3d_native_a32_actor_visibility.h"
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
#include <utility>

namespace Oot3dNativeGame {

namespace {

namespace W = XrWorld;
namespace M = XrMath;
namespace C = XrCamera;

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

std::string_view EnvText(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr ? std::string_view(value) : std::string_view();
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
constexpr uint32_t kFlags2Ocarina = 0x08000000U;   // [unverified name]; TopScreen's ocarina state wins
constexpr double kPlacementProbeTimeoutS = 0.6;

const char* Owner(const C::OwnerDecision& d) { return d.Vr ? (d.Probe ? "vr_probe" : "vr") : "native"; }

double Degrees(double radians) { return M::RadiansToDegrees(radians); }

}  // namespace

const char* XrCameraModeName(XrCameraMode mode) {
    switch (mode) {
    case XrCameraMode::Native: return "native";
    case XrCameraMode::ModeR: return "r";
    case XrCameraMode::Held: return "held";
    }
    return "?";
}

std::unique_ptr<XrWorldSession> XrWorldSession::Create() {
#if defined(TRIAEVUM_QUEST_OPENXR)
    if (!TriAevumQuestXr::Requested() || !TriAevumQuestXr::WorldRequested()) return nullptr;
    auto session = std::unique_ptr<XrWorldSession>(new XrWorldSession());
    session->mScale = EnvNumber("TRIAEVUM_XR_WORLD_SCALE", M::kDefaultScale, 25.0, 45.0);
    session->mSeated = EnvIs("TRIAEVUM_XR_SEATED", "1");
    // Head to Link at every placement, recenter, door, Cinema exit, blink landing (§5.2, §5.3).
    session->mPlaceM = M::ClampPlaceDistanceM(EnvNumber("TRIAEVUM_XR_PLACE_M", M::kDefaultPlaceDistanceM,
                                                        M::kMinPlaceDistanceM, M::kMaxPlaceDistanceM));
    session->mTether.PlaceDistanceM = session->mPlaceM;
    // The leash keeps W1's running room past the landing point unless set (2.5 m seated, §5.6); never
    // inside the landing.
    session->mTether.LeashM = std::max(
        EnvNumber("TRIAEVUM_XR_LEASH_M", M::DefaultLeashForPlaceM(session->mPlaceM, session->mSeated), 1.0, 10.0),
        session->mPlaceM + M::kLeashMarginM);
    session->mTether.Mode = EnvIs("TRIAEVUM_XR_TETHER", "glide") ? M::TetherMode::Glide : M::TetherMode::Leash;
    XrWorldLog("TRIAEVUM_XR_WORLD_SESSION scale=%.1f place_m=%.2f leash_m=%.2f tether=%s seated=%d ui_cinema=%d "
               "force_world=%d",
               session->mScale, session->mPlaceM, session->mTether.LeashM,
               session->mTether.Mode == M::TetherMode::Glide ? "glide" : "leash", session->mSeated ? 1 : 0,
               EnvIs("TRIAEVUM_XR_UI_CINEMA", "0") ? 0 : 1, EnvIs("TRIAEVUM_XR_FORCE_WORLD", "1") ? 1 : 0);
    return session;
#else
    return nullptr;
#endif
}

bool XrWorldSession::ConfigureCamera(bool topScreenProfile) {
    // TRIAEVUM_XR_CAMERA: r (default) | native (W1, for A/B) | held (mode L: native camera, stick rotated).
    const std::string_view requested = EnvText("TRIAEVUM_XR_CAMERA");
    const char* reason = "default";
    if (requested == "native") {
        mCameraMode = XrCameraMode::Native;
        reason = "env";
    } else if (requested == "held") {
        mCameraMode = XrCameraMode::Held;
        reason = "env";
    } else {
        if (!requested.empty() && requested != "r") reason = "env_unknown";
        mCameraMode = topScreenProfile ? XrCameraMode::ModeR : XrCameraMode::Held;
        if (!topScreenProfile) reason = "ui_profile";
    }
    mModeR = mCameraMode == XrCameraMode::ModeR;
    mStickEnabled = mCameraMode != XrCameraMode::Native;
    const std::string_view move = EnvText("TRIAEVUM_XR_MOVE_REF");
    if (!move.empty()) {
        if (!C::ParseMoveReference(move, &mMoveRef)) {
            XrWorldLog("TRIAEVUM_XR_STICK move_ref=\"%.*s\" invalid (head-latched|head|line|native): head-latched",
                       static_cast<int>(move.size()), move.data());
            mMoveRef = C::MoveReference::HeadLatched;
        }
        mStickEnabled = mMoveRef != C::MoveReference::Native;
    }
    mCullFov = C::ParseCullFov(std::getenv("TRIAEVUM_XR_CULL_FOV"));
    const std::string_view turn = EnvText("TRIAEVUM_XR_TURN");
    mSmoothTurn = turn == "smooth";
    if (!turn.empty() && !mSmoothTurn) mTurnDegrees = EnvNumber("TRIAEVUM_XR_TURN", M::kDefaultSnapDegrees, 10.0, 90.0);
    XrWorldLog("TRIAEVUM_XR_WORLD camera=%s reason=%s ui_profile=%s cull_fov=%.0f move_ref=%s stick=%d turn=%s "
               "turn_deg=%.0f",
               XrCameraModeName(mCameraMode), reason, topScreenProfile ? "topscreen" : "other", mCullFov,
               C::MoveReferenceName(mMoveRef), mStickEnabled ? 1 : 0, mSmoothTurn ? "smooth" : "snap", mTurnDegrees);
    return mModeR;
}

double XrWorldSession::Now() const {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - mStart).count();
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

bool XrWorldSession::HeadAt(int64_t displayTimeNs, M::Pose* head) const {
#if defined(TRIAEVUM_QUEST_OPENXR)
    TriAevumQuestXr::HeadPose pose;
    if (displayTimeNs <= 0 || !TriAevumQuestXr::LocateHead(displayTimeNs, &pose)) return false;
    *head = M::PoseFromXr(pose.Orientation, pose.Position);
    return true;
#else
    (void)displayTimeNs;
    (void)head;
    return false;
#endif
}

bool XrWorldSession::HeadInTracking(M::Pose* head) const {
#if defined(TRIAEVUM_QUEST_OPENXR)
    const auto tick = TriAevumQuestXr::LatestTick();
    return tick.Valid && HeadAt(tick.DisplayTimeNs, head);
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

double XrWorldSession::ProbeLengthM() const {
    // How far a placement puts Link ahead of Jamal (TRIAEVUM_XR_PLACE_M) plus the probe's 0.3 m margin.
    return mPlaceM + C::ProbeConfig{}.MarginM;
}

void XrWorldSession::Place(const XrGuestSnapshot& snapshot, const char* reason, double now,
                           const ProbeOutcome* probes) {
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
    if (mSeated) {
        // §5.6: y_seat = 1.60 m - the seated eye height at recenter (over the space's floor).
        mSeatY += M::SeatedYOffset(head.Position.Y + mSeatY);
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
    config.PlaceDistanceM = mPlaceM;
    M::PlacementInput input;
    input.LinkWorld = LinkVrWorld(snapshot, true);
    const double facingGame = M::BinAngleToRadians(snapshot.ShapeYaw);
    input.LinkFacingYaw = M::YawToVrWorld(facingGame, mState.Mirrored);
    if (probes != nullptr && probes->Used && probes->Choice.Probed) {
        // §4.4: the probed direction (Jamal lands along it from Link) and distance (capped at d0 there).
        input.LinkFacingYaw = C::PlacementFacingYaw(facingGame, probes->Choice, mState.Mirrored);
        input.ProbeDistanceM = probes->Choice.DistanceM;
    }
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
               "near_centre=%d epoch=%u anchor=%.1f,%.1f,%.1f eye_height_m=%.2f seat_y=%.2f bounded=%d probed=%d",
               event, reason, result.LinkT.X, result.LinkT.Z, head.Position.X, head.Position.Z,
               M::RadiansToDegrees(mRig.Yaw), mRig.Scale, result.DistanceM, result.NearCentre ? 1 : 0,
               mState.RigEpoch, mRig.Anchor.X, mRig.Anchor.Y, mRig.Anchor.Z, mEyeHeightM, mSeatY, mBounded ? 1 : 0,
               probes != nullptr && probes->Used && probes->Choice.Probed ? 1 : 0);
    if (probes != nullptr && probes->Used) {
        const auto& c = probes->Choice;
        const auto clear = [&](size_t i) { return std::isfinite(c.ClearM[i]) ? c.ClearM[i] : -1.0; };
        XrWorldLog("TRIAEVUM_XR_PROBE reason=%s choice=%s offset_deg=%.0f dist_m=%.2f placed_m=%.2f "
                   "clear_m=%.2f,%.2f,%.2f,%.2f,%.2f probes=%d fallback=%d timeout=%d",
                   reason, c.Name, Degrees(c.OffsetYaw), std::isfinite(c.DistanceM) ? c.DistanceM : -1.0,
                   result.DistanceM, clear(0), clear(1), clear(2), clear(3), clear(4), c.Count, c.Fallback ? 1 : 0,
                   probes->TimedOut ? 1 : 0);
    }
    mGeometryEpoch = UINT32_MAX;  // q restarts at the placed head (§4.4)
    SetWallFade(0.0);
#if defined(TRIAEVUM_QUEST_OPENXR)
    TriAevumQuestXr::Pulse(2, 60);
#endif
}

void XrWorldSession::StartPlacement(const XrGuestSnapshot& snapshot, const char* reason, double now) {
    if (!HookLive(now)) {
        // No Camera_Update reaches the hook (mode L, another profile, the game not updating): unprobed.
        Place(snapshot, reason, now);
        return;
    }
    mPending = {true, reason, now};
    mProbes.Start(ProbeLengthM());
    ++mProbeRuns;
    XrWorldLog("TRIAEVUM_XR_PROBE start reason=%s length_m=%.2f", reason, mProbes.LengthM());
}

void XrWorldSession::FinishPendingPlacement(const XrGuestSnapshot& snapshot, double now, bool force) {
    if (!mPending.Active) return;
    const bool done = mProbes.Done();
    const bool timedOut = now - mPending.Since >= kPlacementProbeTimeoutS || !HookLive(now);
    if (!done && !timedOut && !force) return;
    ProbeOutcome outcome;
    outcome.Used = true;
    outcome.Choice = mProbes.Choose();
    outcome.TimedOut = !done;
    const uint32_t before = mState.RigEpoch;
    Place(snapshot, mPending.Reason, now, &outcome);
    if (mState.RigEpoch == before) return;  // no head yet: kept pending (the probes stay valid)
    mPending.Active = false;
    mProbes.Cancel();
}

void XrWorldSession::SetWallFade(double fade) {
    fade = std::clamp(std::isfinite(fade) ? fade : 0.0, 0.0, 1.0);
    if ((fade > 0.0) != (mWallFade > 0.0))
        XrWorldLog("TRIAEVUM_XR_GEOMETRY fade=%.2f q=%.1f,%.1f,%.1f head=%.1f,%.1f,%.1f", fade, mGeometry.Q().X,
                   mGeometry.Q().Y, mGeometry.Q().Z, mHeadGame.Position.X, mHeadGame.Position.Y, mHeadGame.Position.Z);
    if (fade == mWallFade) return;
    mWallFade = fade;
#if defined(TRIAEVUM_QUEST_OPENXR)
    TriAevumQuestXr::SetWallFade(static_cast<float>(fade));
#endif
}

void XrWorldSession::SetVignette(double strength) {
    strength = std::clamp(std::isfinite(strength) ? strength : 0.0, 0.0, 1.0);
    if ((strength > 0.05) != (mVignette > 0.05))
        XrWorldLog("TRIAEVUM_XR_VIGNETTE strength=%.2f anchor_mps=%.2f yaw_dps=%.0f", strength, mAnchorSpeedMps,
                   Degrees(mYawRate));
    if (std::fabs(strength - mVignette) < 1e-3) return;
    mVignette = strength;
#if defined(TRIAEVUM_QUEST_OPENXR)
    TriAevumQuestXr::SetVignette(static_cast<float>(strength));
#endif
}

void XrWorldSession::PublishActorHead(bool valid, const M::Vec3& head) {
    if (mActors == nullptr) return;
    mActors->VrAlways = valid;
    if (valid) {
        mActors->VrHead = {static_cast<float>(head.X), static_cast<float>(head.Y), static_cast<float>(head.Z)};
        mActors->VrAlwaysRadius = static_cast<float>(C::kAlwaysDrawRadiusM * mScale);
    }
    mActorHeadPublished = valid;
}

void XrWorldSession::LogFlags(const XrGuestSnapshot& snapshot, const char* reason) {
    XrWorldLog("TRIAEVUM_XR_FLAGS reason=%s flags1=0x%08X flags2=0x%08X setting=0x%02X status=%d active_camera=%d "
               "cutscene=0x%X scene=0x%X entrance=0x%X game_mode=%d",
               reason, snapshot.Flags1, snapshot.Flags2, static_cast<unsigned>(static_cast<uint16_t>(snapshot.CameraSetting)),
               snapshot.CameraStatus, snapshot.ActiveCamera, static_cast<unsigned>(snapshot.CutsceneIndex),
               static_cast<unsigned>(static_cast<uint16_t>(snapshot.Scene)), static_cast<unsigned>(snapshot.Entrance),
               snapshot.SaveGameMode);
}

void XrWorldSession::LogDecision(const C::OwnerDecision& d, const XrGuestSnapshot& snapshot,
                                 const XrCameraGuest& camera) {
    const bool changed = !mDecisionLogged || d.Vr != mLoggedDecision.Vr || d.Probe != mLoggedDecision.Probe ||
                         d.Held != mLoggedDecision.Held || d.RigFrozen != mLoggedDecision.RigFrozen ||
                         d.Talking != mLoggedDecision.Talking || d.ZTarget != mLoggedDecision.ZTarget;
    if (!changed) return;
    // Held entries by reason (the W2 exit's list).
    const uint32_t entered = d.Held & ~(mDecisionLogged ? mLoggedDecision.Held : 0U);
    for (size_t bit = 0; bit < mHeldEntries.size(); ++bit)
        if ((entered & (1U << bit)) != 0U) ++mHeldEntries[bit];
    XrWorldLog("TRIAEVUM_XR_CAMERA owner=%s held=0x%04X (%s) rig_frozen=%d talking=%d shop=%d z_target=%d "
               "layer=%s pending=%d flags1=0x%08X flags2=0x%08X setting=0x%02X status=%d active_camera=%d "
               "scene=0x%X cutscene=0x%X",
               Owner(d), d.Held, C::HeldNames(d.Held).c_str(), d.RigFrozen ? 1 : 0, d.Talking ? 1 : 0,
               d.Shop ? 1 : 0, d.ZTarget ? 1 : 0, W::XrWorldStateName(mState.Mode), mPending.Active ? 1 : 0,
               snapshot.Flags1, snapshot.Flags2, static_cast<unsigned>(static_cast<uint16_t>(camera.Setting)),
               camera.Status, snapshot.ActiveCamera, static_cast<unsigned>(static_cast<uint16_t>(snapshot.Scene)),
               static_cast<unsigned>(snapshot.CutsceneIndex));
    if (d.Vr && !d.Probe && (!mLoggedDecision.Vr || mLoggedDecision.Probe || !mDecisionLogged)) mFirstWriteLogged = false;
    mLoggedDecision = d;
    mDecisionLogged = true;
}

// Snap and smooth turns and recenters (§5.5, §5.2), taken where the rig is stamped: at Camera_Update when
// the hook is live (in step with the camera write and culling, §4.5), else at the frame stamp.
void XrWorldSession::ApplyRigInputs(const XrGuestSnapshot& snapshot, double now, double dt) {
    mLastRigInputs = now;
#if defined(TRIAEVUM_QUEST_OPENXR)
    const int32_t turns = TriAevumQuestXr::TakeSnapTurns();
    const bool worldShown = mState.Mode == W::XrWorldState::World && mRigValid && !mPending.Active;
    const uint32_t recenters = TriAevumQuestXr::TakeWorldRecenter();
    if (recenters != 0U && worldShown && snapshot.PlayerValid) {
        // Black at once; the placement that follows (probed when the hook is live) fades it in.
        TriAevumQuestXr::RequestBlink(mState.RigEpoch + 1U, 150);
        if (HookLive(now)) {
            StartPlacement(snapshot, "recenter", now);
        } else {
            const uint32_t before = mState.RigEpoch;
            Place(snapshot, "recenter", now);
            if (mState.RigEpoch != before) TriAevumQuestXr::RequestBlink(mState.RigEpoch, 150);
        }
        return;
    }
    mYawRate = 0.0;
    if (!worldShown) return;
    M::Pose head;
    if (!HeadInTracking(&head)) return;
    if (!mSmoothTurn && turns != 0) {
        const int direction = turns > 0 ? 1 : -1;
        mRig = M::SnapTurn(mRig, head.Position, direction, M::DegreesToRadians(mTurnDegrees));
        ++mTurns;
        ++mBlinks;
        BumpEpoch("turn", 0.0);
        XrWorldLog("TRIAEVUM_XR_TURN dir=%s step_deg=%.0f psi=%.1f flicks=%d", direction > 0 ? "right" : "left",
                   mTurnDegrees, Degrees(mRig.Yaw), turns);
        TriAevumQuestXr::RequestBlink(mState.RigEpoch, 80);
        TriAevumQuestXr::Pulse(1, 25);
    } else if (mSmoothTurn && dt > 0.0) {
        const float axis = TriAevumQuestXr::WorldTurnAxis();
        if (std::fabs(axis) > 0.2F) {
            const double rate = -static_cast<double>(axis) * M::DegreesToRadians(mSmoothTurnDegreesPerSecond);
            mRig = M::ApplyYawTurn(mRig, head.Position, rate * dt);
            mYawRate = rate;
        }
    }
#else
    (void)snapshot;
    (void)now;
    (void)dt;
#endif
}

XrCameraUpdate XrWorldSession::BeginCameraUpdate(const XrGuestSnapshot& snapshot, const XrCameraGuest& camera,
                                                 uint64_t recording) {
    XrCameraUpdate update;
    if (!camera.MainCamera) return update;  // secondary cameras run native, unlogged
    const double now = Now();
    const double dt = mLastCameraUpdate < 0.0 ? 0.0 : std::clamp(now - mLastCameraUpdate, 0.0, 0.2);
    mLastCameraUpdate = now;
    mRecordingSequence = recording;
    ++mCamUpdates;
    ApplyRigInputs(snapshot, now, dt);

    // The head predicted for this frame's target display time T_N (§3.4 item 2, §4.2).
    M::Pose headT;
    int64_t target = mState.TickPacing ? mState.TargetDisplayTimeNs : 0;
#if defined(TRIAEVUM_QUEST_OPENXR)
    if (target <= 0) {
        const auto tick = TriAevumQuestXr::LatestTick();
        target = tick.Valid ? tick.DisplayTimeNs : 0;
    }
#endif
    const bool headValid = mRigValid && HeadAt(target, &headT);
    if (mRigValid && !headValid) ++mHeadFailures;
    mHeadGame = headValid ? C::HeadInGame(mRig, headT, mState.Mirrored) : C::GameHead{};

    C::OwnerInput in;
    in.MainCamera = true;
    in.LayerWorld = mState.Mode == W::XrWorldState::World && mRigValid && !mPending.Active;
    in.Probing = mPending.Active && mProbes.Active();
    in.RigValid = mRigValid;
    in.HeadValid = mHeadGame.Valid;
    in.PlayerValid = snapshot.PlayerValid;
    in.ForceHeld = !mModeR;
    in.Flags1 = snapshot.Flags1;
    in.Flags2 = snapshot.Flags2;
    in.Setting = camera.Setting;
    in.Status = camera.Status;
    in.Scene = snapshot.Scene;
    in.ActiveCamera = snapshot.ActiveCamera;
    in.CutsceneIndex = snapshot.CutsceneIndex;
    in.GameMode = snapshot.SaveGameMode;
    mDecision = C::DecideOwner(in);
    // The cutscene camera's motion (§4.3): the view the game drew last, never while talking. The view
    // at the first Held-static update is still the head the host wrote: sampling starts once the native
    // camera has run one update, so the hand-over itself is never a move.
    mHeldStaticRun = mDecision.HeldStatic ? mHeldStaticRun + 1U : 0U;
    const M::Vec3 view = camera.ViewEye;
    const M::Vec3 look = camera.ViewAt - camera.ViewEye;
    const bool wasMoving = mCutsceneMoving;
    mCutsceneMoving = mCutsceneMonitor.Update(mDecision.HeldStatic && !mDecision.Talking && mHeldStaticRun >= 2U,
                                              now, view, std::atan2(look.X, look.Z), mScale);
    if (mCutsceneMoving && !wasMoving)
        XrWorldLog("TRIAEVUM_XR_CAMERA cutscene_camera_moved moved_m=%.2f turned_deg=%.1f held=0x%04X (%s)",
                   mCutsceneMonitor.TripMovedM(), mCutsceneMonitor.TripTurnedDegrees(), mDecision.Held,
                   C::HeldNames(mDecision.Held).c_str());
    LogDecision(mDecision, snapshot, camera);

    mOwnedLast = false;
    mProbeIndex = -1;
    if (mDecision.Vr && mDecision.Probe) {
        // A placement probe from Link's head (§4.4); the camera holds where it is.
        ++mCamProbes;
        const M::Vec3 link{snapshot.Position[0], snapshot.Position[1], snapshot.Position[2]};
        const M::Vec3 linkHead = link + M::Vec3{0.0, C::LinkHeadHeightU(snapshot.LinkAge != 0), 0.0};
        C::ProbeRequest request;
        if (mProbes.Next(linkHead, M::BinAngleToRadians(snapshot.ShapeYaw), mScale, &request)) {
            mProbeIndex = request.Index;
            mProbeFrom = request.From;
            update.Segment = {request.From, request.To};
        } else {
            update.Segment = {camera.CameraEye, camera.CameraEye};
        }
        update.Write = C::HoldCameraWrite(camera.CameraEye, camera.CameraAt);
        update.FovDegrees = camera.ViewFov;
        update.Own = update.Write.Valid;
        update.Probe = true;
    } else if (mDecision.Vr) {
        // The head owns the camera (§4.2): culling, billboards, Z-targets and audio follow it.
        update.Write = C::BuildCameraWrite(mHeadGame);
        if (update.Write.Valid) {
            if (mGeometryEpoch != mState.RigEpoch || !mGeometry.Valid()) {
                mGeometry.Reset(mHeadGame.Position);
                mGeometryEpoch = mState.RigEpoch;
            }
            update.Segment = mGeometry.Segment(mHeadGame.Position);
            update.FovDegrees = static_cast<float>(mCullFov);
            update.Own = true;
            mOwnedLast = true;
            ++mCamOwned;
            mWrite = update.Write;
            if (!mFirstWriteLogged) {
                mFirstWriteLogged = true;
                XrWorldLog("TRIAEVUM_XR_CAMWRITE first eye=%.1f,%.1f,%.1f at=%.1f,%.1f,%.1f yaw=%.1f pitch=%.1f "
                           "fov=%.0f head_t=%.2f,%.2f,%.2f target_ns=%lld epoch=%u",
                           mWrite.Eye.X, mWrite.Eye.Y, mWrite.Eye.Z, mWrite.At.X, mWrite.At.Y, mWrite.At.Z,
                           Degrees(mWrite.Yaw), Degrees(mWrite.Pitch), mCullFov, headT.Position.X, headT.Position.Y,
                           headT.Position.Z, static_cast<long long>(target), mState.RigEpoch);
            }
        }
    }
    if (!update.Own) {
        ++mCamHeld;
        SetWallFade(0.0);
    }
    PublishActorHead(mOwnedLast, mHeadGame.Position);
    return update;
}

void XrWorldSession::CommitCameraUpdate(const M::Vec3& collided) {
    if (mProbeIndex >= 0) {
        mProbes.Feed(mProbeIndex, mProbeFrom, collided, mScale);
        mProbeIndex = -1;
        return;
    }
    if (!mOwnedLast) return;
    // Link's floor in the game world is the rig's a.y (the mirror keeps y).
    const auto step = mGeometry.Update(mHeadGame.Position, collided, mRig.Anchor.Y, mScale);
    if (step.Fade > 0.0) ++mWallFadeUpdates;
    SetWallFade(step.Fade);
}

void XrWorldSession::NoteCameraQuake(bool active) {
    if (!active || !mOwnedLast) return;
    // §5.7: camera shake is ignored in the picture; a light pulse instead (at most every 0.5 s).
    static double lastPulse = -10.0;
    const double now = Now();
    ++mQuakes;
    if (now - lastPulse < 0.5) return;
    lastPulse = now;
#if defined(TRIAEVUM_QUEST_OPENXR)
    TriAevumQuestXr::Pulse(2, 20);
#endif
}

bool XrWorldSession::CameraFov(float loaded, float* degrees) {
    if (!mOwnedLast || degrees == nullptr) return false;
    ++mFovHooks;
    mFovLoaded = loaded;
    *degrees = static_cast<float>(mCullFov);
    return true;
}

void XrWorldSession::RotateStick(const XrGuestSnapshot& snapshot, bool ocarina, int16_t& x, int16_t& y) {
    if (!mStickEnabled) return;
    const double now = Now();
    const double dt = mLastStickTime < 0.0 ? 0.0 : std::clamp(now - mLastStickTime, 0.0, 0.2);
    mLastStickTime = now;
    C::StickInput in;
    in.Dt = dt;
    in.StickActive = C::StickActive(x, y);
    const auto decision = C::DecideOwner([&] {
        C::OwnerInput o;
        o.LayerWorld = true;
        o.PlayerValid = snapshot.PlayerValid;
        o.Flags1 = snapshot.Flags1;
        o.Flags2 = snapshot.Flags2;
        o.Setting = snapshot.CameraSetting;
        return o;
    }());
    in.ZTarget = decision.ZTarget;
    in.Bypass = !mState.World || mState.Mode != W::XrWorldState::World || !snapshot.PlayerValid ||
                !snapshot.CameraValid || decision.StickBypass || ocarina ||
                (snapshot.Flags2 & kFlags2Ocarina) != 0U || mPending.Active;
    M::Pose head;
    if (!in.Bypass && mRigValid && HeadInTracking(&head)) {
        const auto game = C::HeadInGame(mRig, head, mState.Mirrored);
        in.HeadValid = game.Valid;
        in.HeadYaw = game.VrYaw;
        const M::Vec3 linkVr = LinkVrWorld(snapshot, false);
        const M::Vec3 headVr = M::TrackingToWorld(mRig, head.Position);
        in.LineYaw = M::YawOf(linkVr - headVr);
        in.LineDistanceM = M::DistanceXZ(linkVr, headVr) / mScale;
    }
    const auto step = mStick.Update(mMoveRef, in);
    if (!step.Rotate) {
        if (in.Bypass && in.StickActive) ++mStickBypassed;
        return;
    }
    const double cameraYaw = M::BinAngleToRadians(snapshot.CameraYaw);  // +0x17E, the yaw Player reads
    const double theta = C::StickTheta(step.PsiRef, cameraYaw, mState.Mirrored);
    const int16_t beforeX = x, beforeY = y;
    C::RotateCirclePad(x, y, theta);
    ++mStickRotations;
    mThetaAbsMax = std::max(mThetaAbsMax, std::fabs(Degrees(theta)));
    static const bool inputLog = EnvIs("TRIAEVUM_XR_INPUT_LOG", "1");
    if (inputLog && (step.Latched || now - mLastStickLog >= 0.5)) {
        mLastStickLog = now;
        const double stickDir = std::atan2(-static_cast<double>(beforeX), static_cast<double>(beforeY));
        XrWorldLog("TRIAEVUM_XR_STICK theta=%.1f yaw_cam=%.1f yaw_cam_184=%.1f yaw_dir=%.1f psi_ref=%.1f mode=%s "
                   "latched=%d z=%d in=%d,%d out=%d,%d",
                   Degrees(theta), Degrees(cameraYaw), Degrees(M::BinAngleToRadians(snapshot.CameraInputYaw)),
                   Degrees(M::WrapAngle(step.PsiRef + stickDir)), Degrees(step.PsiRef), C::MoveReferenceName(mMoveRef),
                   step.Latched ? 1 : 0, in.ZTarget ? 1 : 0, beforeX, beforeY, x, y);
    }
}

void XrWorldSession::StampFrame(const XrGuestSnapshot& snapshot, const XrFrameFacts& facts,
                                const XrHostSignals& host, uint64_t sequence) {
    const double now = Now();
    const double dt = mLastStamp < 0.0 ? 0.0 : std::clamp(now - mLastStamp, 0.0, 0.2);
    mLastStamp = now;
    if (facts.HasCamera) mState.Mirrored = facts.Mirrored;
    const bool hookLive = HookLive(now);
    if (hookLive && sequence != 0U && mRecordingSequence != 0U) {
        // The camera write of the update before this stamp carried the sequence being recorded; 0 = this
        // visual frame was drawn from it (§4.5 consistency, measured on the device).
        mSeqLagSum += static_cast<double>(static_cast<int64_t>(mRecordingSequence) - static_cast<int64_t>(sequence));
        ++mSeqSamples;
    }

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
    // W2: cutscenes stay in the world while the hook runs them Held; without the hook (mode L, another
    // profile) the W1 rows apply.
    signals.ModeR = mModeR;
    signals.CutsceneCameraMoving = mModeR && mCutsceneMoving;
    static const bool forceWorld = EnvIs("TRIAEVUM_XR_FORCE_WORLD", "1");
    if (forceWorld) {
        // Bring-up: every Cinema blocker off but the session and the player.
        signals.Gameplay = true;
        signals.HostGui = false;
        signals.UiDraws = false;
        signals.Flags1 = signals.Flags2 = 0;
        signals.CutsceneIndex = 0;
        signals.RenderSafety = false;
        signals.CutsceneCameraMoving = false;
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
        XrWorldLog("TRIAEVUM_XR_CAMERA state=%s owner=%s blockers=0x%08X (%s) flags1=0x%08X flags2=0x%08X "
                   "setting=0x%02X status=%d ui_pc=0x%08X player=%d scene=0x%X camera=%s",
                   W::XrWorldStateName(step.State), hookLive ? Owner(mDecision) : "native", step.Blockers,
                   W::XrBlockerNames(step.Blockers).c_str(), snapshot.Flags1, snapshot.Flags2,
                   static_cast<unsigned>(static_cast<uint16_t>(snapshot.CameraSetting)), snapshot.CameraStatus,
                   facts.UiSourcePc, snapshot.PlayerValid ? 1 : 0,
                   static_cast<unsigned>(static_cast<uint16_t>(snapshot.Scene)), XrCameraModeName(mCameraMode));
        mLastBlockers = visible;
    }

    if (step.State != W::XrWorldState::World && mPending.Active) {
        XrWorldLog("TRIAEVUM_XR_PROBE cancelled reason=%s state=%s probes=%d", mPending.Reason,
                   W::XrWorldStateName(step.State), mProbes.Count());
        mPending.Active = false;
        mProbes.Cancel();
    }
    if (step.Place) {
        StartPlacement(snapshot, step.PlaceReason, now);
    } else if (step.State == W::XrWorldState::World && !mRigValid && !mPending.Active && snapshot.PlayerValid) {
        // A placement that found no head (tracking lost) is retried each frame; Black meanwhile.
        Place(snapshot, "retry", now);
    }
    if (mPending.Active && snapshot.PlayerValid) FinishPendingPlacement(snapshot, now, false);
    // Turns and recenters: at Camera_Update while the hook runs, else here.
    if (!hookLive) ApplyRigInputs(snapshot, now, dt);

    // Following Link (§5.3, §5.4): not while a Held row freezes the rig (talks, item gets, subcameras).
    mAnchorSpeedMps = 0.0;
    const bool frozen = (step.Blockers & W::kXrHoldRig) != 0U || (hookLive && mDecision.RigFrozen);
    if (step.State == W::XrWorldState::World && mRigValid && !mPending.Active && snapshot.PlayerValid && !frozen &&
        dt > 0.0) {
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
            } else if (tether.Event == M::RigEvent::Glide) {
                mRig = tether.NewRig;
                mAnchorSpeedMps = std::max(mAnchorSpeedMps, tether.AnchorSpeedMps);
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
            mAnchorSpeedMps = std::max(mAnchorSpeedMps, floorStep.SpeedMps);
            if (floorStep.Blink) {
                ++mBlinks;
                BumpEpoch("floor", 0.0);
#if defined(TRIAEVUM_QUEST_OPENXR)
                TriAevumQuestXr::RequestBlink(mState.RigEpoch, 120);
#endif
            }
        }
    }

    const bool world = step.State == W::XrWorldState::World && mRigValid && !mPending.Active;
    const W::XrWorldState shown = step.State == W::XrWorldState::World && !world ? W::XrWorldState::Black : step.State;
    if (shown != mRequested) {
#if defined(TRIAEVUM_QUEST_OPENXR)
        TriAevumQuestXr::RequestLayerMode(LayerFor(shown), 150, mState.RigEpoch);
#endif
        mRequested = shown;
    }
    mState.Mode = shown;
    mState.World = world;
    mState.Rig = mRig;
    // §5.7: the vignette from the anchor's glides and the smooth turn's yaw rate, World only.
    SetVignette(world ? M::VignetteStrength(mAnchorSpeedMps, mYawRate) : 0.0);
    if (!world) {
        SetWallFade(0.0);
        if (mActorHeadPublished) PublishActorHead(false, {});
    }
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
    if (mCameraMode != XrCameraMode::Native) {
        uint64_t alwaysCandidates = 0, alwaysGranted = 0;
        if (mActors != nullptr) {
            alwaysCandidates = std::exchange(mActors->VrAlwaysCandidates, 0U);
            alwaysGranted = std::exchange(mActors->VrAlwaysGranted, 0U);
        }
        const double owned = mCamUpdates != 0U ? 100.0 * static_cast<double>(mCamOwned) / static_cast<double>(mCamUpdates) : 0.0;
        XrWorldLog("TRIAEVUM_XR_CAMWRITE camera=%s updates=%llu owned=%llu owned_pct=%.1f probes=%llu held=%llu "
                   "owner=%s eye=%.1f,%.1f,%.1f yaw=%.1f pitch=%.1f fov=%.0f fov_hooks=%llu fov_loaded=%.1f "
                   "head_failures=%llu seq_lag=%.2f seq_samples=%llu wall_fade_updates=%llu wall_fade=%.2f "
                   "always_candidates=%llu always_granted=%llu quakes=%llu turns=%llu probe_runs=%llu",
                   XrCameraModeName(mCameraMode), static_cast<unsigned long long>(mCamUpdates),
                   static_cast<unsigned long long>(mCamOwned), owned, static_cast<unsigned long long>(mCamProbes),
                   static_cast<unsigned long long>(mCamHeld), Owner(mDecision), mWrite.Eye.X, mWrite.Eye.Y,
                   mWrite.Eye.Z, Degrees(mWrite.Yaw), Degrees(mWrite.Pitch), mCullFov,
                   static_cast<unsigned long long>(mFovHooks), mFovLoaded,
                   static_cast<unsigned long long>(mHeadFailures),
                   mSeqSamples != 0U ? mSeqLagSum / static_cast<double>(mSeqSamples) : -1.0,
                   static_cast<unsigned long long>(mSeqSamples), static_cast<unsigned long long>(mWallFadeUpdates),
                   mWallFade, static_cast<unsigned long long>(alwaysCandidates),
                   static_cast<unsigned long long>(alwaysGranted), static_cast<unsigned long long>(mQuakes),
                   static_cast<unsigned long long>(mTurns), static_cast<unsigned long long>(mProbeRuns));
        if (mStickEnabled)
            XrWorldLog("TRIAEVUM_XR_STICK rotations=%llu bypassed=%llu theta_abs_max=%.1f mode=%s",
                       static_cast<unsigned long long>(mStickRotations), static_cast<unsigned long long>(mStickBypassed),
                       mThetaAbsMax, C::MoveReferenceName(mMoveRef));
        std::string held;
        for (size_t bit = 0; bit < mHeldEntries.size(); ++bit) {
            if (mHeldEntries[bit] == 0U) continue;
            held += (held.empty() ? "" : ",") + C::HeldNames(1U << bit) + ":" + std::to_string(mHeldEntries[bit]);
            mHeldEntries[bit] = 0U;
        }
        if (!held.empty()) XrWorldLog("TRIAEVUM_XR_HELD entries=%s", held.c_str());
        mCamUpdates = mCamOwned = mCamProbes = mCamHeld = mHeadFailures = mFovHooks = mWallFadeUpdates = 0;
        mStickRotations = mStickBypassed = mQuakes = mTurns = mSeqSamples = mProbeRuns = 0;
        mSeqLagSum = mThetaAbsMax = 0.0;
    }
    mOverruns = mBeats = mPresentations = mPlacements = mBlinks = mResyncs = mHeldFrames = 0;
}

}  // namespace Oot3dNativeGame
