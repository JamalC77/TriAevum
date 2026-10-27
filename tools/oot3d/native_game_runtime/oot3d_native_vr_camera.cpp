// Mode R's camera source (see oot3d_native_vr_camera.h).
#include "oot3d_native_vr_camera.h"

#include "oot3d_native_a32_actor_visibility.h"
#include "oot3d_native_a32_memory.h"
#include "oot3d_native_xr_guest_snapshot.h"
#include "oot3d_native_xr_session.h"
#include "oot3d_native_xr_view.h"

#include <bit>
#include <cmath>

namespace Oot3dNativeGame {

namespace {

namespace M = XrMath;
namespace C = XrCamera;

constexpr uint32_t kCameraPlayState = 0xD4U;
constexpr uint32_t kPlayMainCamera = 0x364U;
constexpr uint32_t kPlayViewEye = 0x1B8U;
constexpr uint32_t kPlayViewAt = 0x1C4U;
constexpr uint32_t kPlayViewFov = 0x198U;

M::Vec3 ToVec(const TopScreenCameraVec3& v) { return {v.X, v.Y, v.Z}; }
TopScreenCameraVec3 FromVec(const M::Vec3& v) {
    return {static_cast<float>(v.X), static_cast<float>(v.Y), static_cast<float>(v.Z)};
}

bool ReadFloat(NativeA32Memory& memory, uint32_t address, float* value) {
    uint32_t bits = 0;
    if (!memory.Read32(address, &bits)) return false;
    *value = std::bit_cast<float>(bits);
    return std::isfinite(*value);
}

bool ReadVec(NativeA32Memory& memory, uint32_t address, M::Vec3* value) {
    float x = 0.0F, y = 0.0F, z = 0.0F;
    if (!ReadFloat(memory, address, &x) || !ReadFloat(memory, address + 4U, &y) ||
        !ReadFloat(memory, address + 8U, &z))
        return false;
    *value = {x, y, z};
    return true;
}

TopScreenVrCameraWrite ToWrite(const C::CameraWrite& write, float fov) {
    TopScreenVrCameraWrite out;
    out.Eye = FromVec(write.Eye);
    out.At = FromVec(write.At);
    out.Pitch = write.PitchBin;
    out.Yaw = write.YawBin;
    out.FovDegrees = fov;
    return out;
}

}  // namespace

std::shared_ptr<VrCameraSource> VrCameraSource::ForSession(XrWorldSession* session) {
    if (session == nullptr) return nullptr;
    auto source = std::shared_ptr<VrCameraSource>(new VrCameraSource());
    source->mSession = session;
    return source;
}

std::shared_ptr<VrCameraSource> VrCameraSource::ForPreview(const XrWorld::XrPreviewHead& head, double scale,
                                                           NativeActorVisibilityState* actors) {
    if (head.Kind != XrWorld::XrPreviewHeadKind::Circle) return nullptr;
    auto source = std::shared_ptr<VrCameraSource>(new VrCameraSource());
    source->mPreview = true;
    source->mCircle.RadiusM = head.RadiusM;
    source->mCircle.PeriodSeconds = head.PeriodSeconds;
    source->mScale = scale > 0.0 ? scale : M::kDefaultScale;
    source->mActors = actors;
    XrWorldLog("TRIAEVUM_XR_PREVIEW mode_r=circle radius_m=%.2f period_s=%.1f scale=%.1f cull_fov=%.0f",
               head.RadiusM, head.PeriodSeconds, source->mScale, C::kDefaultCullFovDegrees);
    return source;
}

bool VrCameraSource::Begin(NativeA32Memory& memory, const TopScreenVrCameraBegin& begin) {
    mOwned = false;
    mProbe = false;
    if (mPreview) return BeginPreview(memory, begin);
    if (mSession == nullptr) return false;
    XrCameraGuest camera;
    camera.MainCamera = begin.GlobalContextAddress != 0U &&
                        begin.CameraAddress == begin.GlobalContextAddress + kPlayMainCamera;
    if (!camera.MainCamera) return false;  // secondary cameras: the native update, unlogged
    camera.Camera = begin.CameraAddress;
    camera.PlayState = begin.GlobalContextAddress;
    camera.CameraEye = ToVec(begin.Eye);
    camera.CameraAt = ToVec(begin.At);
    camera.Status = begin.Ownership.CameraStatus;
    camera.Setting = begin.Ownership.CameraSetting;
    if (!ReadVec(memory, camera.PlayState + kPlayViewEye, &camera.ViewEye)) camera.ViewEye = camera.CameraEye;
    if (!ReadVec(memory, camera.PlayState + kPlayViewAt, &camera.ViewAt)) camera.ViewAt = camera.CameraAt;
    if (!ReadFloat(memory, camera.PlayState + kPlayViewFov, &camera.ViewFov)) camera.ViewFov = 60.0F;
    // The snapshot reader checks the player actor (id 0, category PLAYER) at the camera's player.
    const auto snapshot = ReadXrGuestSnapshot(memory, begin.PlayerAddress, begin.GlobalContextAddress);
    const auto update = mSession->BeginCameraUpdate(snapshot, camera, mSequence ? mSequence() : 0U);
    if (!update.Own) return false;
    mOwned = true;
    mProbe = update.Probe;
    mSegment = {FromVec(update.Segment.From), FromVec(update.Segment.To)};
    mWrite = ToWrite(update.Write, update.FovDegrees);
    return true;
}

TopScreenVrCameraWrite VrCameraSource::Commit(const TopScreenCameraVec3& collided, std::uint32_t bgId) {
    if (bgId != 0U) ++mBgHits;
    if (mPreview) {
        // The preview logs where the synthetic head would fade (no compositor on the PC).
        const auto step = mPreviewGeometry.Update(mPreviewHead, ToVec(collided), mPreviewFloorY, mScale);
        if (step.Fade > 0.0 && (mPreviewUpdates % 30U) == 0U)
            XrWorldLog("TRIAEVUM_XR_PREVIEW geometry_fade=%.2f p_u=%.1f", step.Fade, step.PenetrationU);
        return mWrite;
    }
    if (mSession != nullptr) mSession->CommitCameraUpdate(ToVec(collided));
    return mWrite;
}

void VrCameraSource::Quake(bool active) {
    if (mSession != nullptr && !mPreview) mSession->NoteCameraQuake(active);
}

bool VrCameraSource::Fov(float loaded, float* degrees) {
    if (mPreview) {
        if (!mPreviewOwned || degrees == nullptr) return false;
        *degrees = static_cast<float>(C::kDefaultCullFovDegrees);
        return true;
    }
    return mSession != nullptr && mSession->CameraFov(loaded, degrees);
}

float VrCameraSource::NativeLoadedFov(float loaded) {
    if (mPreview || mSession == nullptr) return loaded;
    return mSession->NativeLoadedFov(loaded);
}

bool VrCameraSource::BeginPreview(NativeA32Memory& memory, const TopScreenVrCameraBegin& begin) {
    const bool main = begin.GlobalContextAddress != 0U &&
                      begin.CameraAddress == begin.GlobalContextAddress + kPlayMainCamera;
    if (!main) return false;
    mPreviewOwned = false;
    const auto snapshot = ReadXrGuestSnapshot(memory, begin.PlayerAddress, begin.GlobalContextAddress);
    C::OwnerInput in;
    in.LayerWorld = true;
    in.RigValid = true;
    in.HeadValid = true;
    in.PlayerValid = snapshot.PlayerValid;
    in.Flags1 = snapshot.Flags1;
    in.Flags2 = snapshot.Flags2;
    in.Setting = begin.Ownership.CameraSetting;
    in.Status = begin.Ownership.CameraStatus;
    in.Scene = snapshot.Scene;
    in.ActiveCamera = snapshot.ActiveCamera;
    in.CutsceneIndex = snapshot.CutsceneIndex;
    in.GameMode = snapshot.SaveGameMode;
    const auto decision = C::DecideOwner(in);
    if (!decision.Vr) {
        if (mActors != nullptr) mActors->VrAlways = false;
        return false;
    }
    // Guest time: 30 updates per second, so the circle is the same on any host speed.
    const double seconds = static_cast<double>(mPreviewUpdates++) / 30.0;
    const M::Vec3 link{snapshot.Position[0], snapshot.Position[1], snapshot.Position[2]};
    const auto head = C::CirclePreviewHead(link, M::BinAngleToRadians(snapshot.ShapeYaw),
                                           C::LinkHeadHeightU(snapshot.LinkAge != 0), mCircle, seconds, mScale);
    const auto write = C::BuildCameraWrite(head);
    if (!write.Valid) return false;
    if (!mPreviewGeometry.Valid()) mPreviewGeometry.Reset(head.Position);
    mPreviewHead = head.Position;
    mPreviewFloorY = std::isfinite(snapshot.FloorHeight) ? snapshot.FloorHeight : link.Y;
    const auto segment = mPreviewGeometry.Segment(head.Position, link + M::Vec3{0.0, C::LinkHeadHeightU(snapshot.LinkAge != 0), 0.0});
    mSegment = {FromVec(segment.From), FromVec(segment.To)};
    mWrite = ToWrite(write, static_cast<float>(C::kDefaultCullFovDegrees));
    mOwned = true;
    mPreviewOwned = true;
    if (mActors != nullptr) {
        mActors->VrAlways = true;
        mActors->VrHead = {static_cast<float>(head.Position.X), static_cast<float>(head.Position.Y),
                           static_cast<float>(head.Position.Z)};
        mActors->VrAlwaysRadius = static_cast<float>(C::kAlwaysDrawRadiusM * mScale);
    }
    const int step = C::CircleStep(seconds, mCircle.PeriodSeconds);
    if (step != mPreviewStep) {
        // The W2 day-5 screenshot points: one line per 45 degrees around Link.
        mPreviewStep = step;
        XrWorldLog("TRIAEVUM_XR_PREVIEW circle deg=%d t=%.2f scene=0x%X link=%.1f,%.1f,%.1f eye=%.1f,%.1f,%.1f "
                   "yaw=%.1f pitch=%.1f",
                   step * 45, seconds, static_cast<unsigned>(static_cast<uint16_t>(snapshot.Scene)), link.X, link.Y,
                   link.Z, write.Eye.X, write.Eye.Y, write.Eye.Z, M::RadiansToDegrees(write.Yaw),
                   M::RadiansToDegrees(write.Pitch));
    }
    return true;
}

}  // namespace Oot3dNativeGame
