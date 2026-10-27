// oot3d_native_vr_camera.h: mode R's camera source (quest-research/inworld-spec.md §4.2, W2 day 1). The
// TopScreen phased Camera_Update replacement calls it at every Camera_Update entry; it reads the guest
// state the decision needs and hands the update to its backend:
//
//  - the headset (XrWorldSession): the head predicted for the frame's target display time owns the main
//    camera unless a §4.3 row holds it native; one BgCheck segment per update tests the head inside
//    geometry or probes a placement; the FovScalar returns the cull FOV unclamped;
//  - the PC preview (TRIAEVUM_XR_PREVIEW_HEAD=circle:radius_m,period_s): a synthetic head walking a circle
//    around Link drives the game camera, so culling, billboards and LOD can be checked from every side
//    without a headset (§2.7, W2 day 5).
//
// Guest thread only. The camera writes are logged by the backend (TRIAEVUM_XR_CAMERA, _CAMWRITE).
#pragma once

#include "oot3d_top_screen_camera_guest.h"
#include "oot3d_xr_camera_logic.h"
#include "oot3d_xr_world_logic.h"

#include <cstdint>
#include <functional>
#include <memory>

namespace Oot3dNativeGame {

class XrWorldSession;
struct NativeActorVisibilityState;

class VrCameraSource final : public TopScreenVrCameraSource {
  public:
    // The headset backend; the session outlives the source (both belong to the window session).
    static std::shared_ptr<VrCameraSource> ForSession(XrWorldSession* session);
    // The PC preview backend; null unless `head` is the circle.
    static std::shared_ptr<VrCameraSource> ForPreview(const XrWorld::XrPreviewHead& head, double scale,
                                                      NativeActorVisibilityState* actors);
    // The visual-frame sequence being recorded (the stamp of the camera write, §4.5).
    void SetSequenceSource(std::function<uint64_t()> source) { mSequence = std::move(source); }

    bool Begin(NativeA32Memory& memory, const TopScreenVrCameraBegin& begin) override;
    TopScreenVrCameraSegment Segment() override { return mSegment; }
    TopScreenVrCameraWrite Commit(const TopScreenCameraVec3& collided, std::uint32_t bgId) override;
    void Quake(bool active) override;
    bool Fov(float loaded, float* degrees) override;
    float NativeLoadedFov(float loaded) override;

  private:
    VrCameraSource() = default;
    bool BeginPreview(NativeA32Memory& memory, const TopScreenVrCameraBegin& begin);

    XrWorldSession* mSession = nullptr;
    std::function<uint64_t()> mSequence;
    TopScreenVrCameraSegment mSegment;
    TopScreenVrCameraWrite mWrite;
    bool mOwned = false;
    bool mProbe = false;
    uint64_t mBgHits = 0;

    // PC preview.
    bool mPreview = false;
    XrCamera::CircleHeadConfig mCircle;
    double mScale = XrMath::kDefaultScale;
    NativeActorVisibilityState* mActors = nullptr;
    uint64_t mPreviewUpdates = 0;
    int mPreviewStep = -1;
    bool mPreviewOwned = false;
    XrCamera::HeadInGeometry mPreviewGeometry;
    XrMath::Vec3 mPreviewHead;
    double mPreviewFloorY = 0.0;
};

}  // namespace Oot3dNativeGame
