#pragma once

#include "oot3d_top_screen_camera.h"

#include <cstdint>
#include <string>

namespace Oot3dNativeGame {

class NativeA32Memory;

// In-world mode R (quest-research/inworld-spec.md §4.2): a host camera source that replaces the orbit with
// the player's head while it owns an update. The phases keep TopScreen's order (Water, Interface, BgCheck,
// Quake, CamData); the source picks the one BgCheck segment of the update and returns the camera to write.
// Installed by the native game runtime in the immersive world mode only.
struct TopScreenVrCameraBegin {
  std::uint32_t CameraAddress = 0U;
  std::uint32_t GlobalContextAddress = 0U;
  std::uint32_t PlayerAddress = 0U;
  TopScreenFreeCameraOwnershipInput Ownership;  // flags, setting, status, scene, cutscene, game mode
  TopScreenCameraVec3 At;   // the camera's current at/eye (+0x80/+0x8C)
  TopScreenCameraVec3 Eye;
};
struct TopScreenVrCameraSegment {
  TopScreenCameraVec3 From;  // BgCheckInfo's from (scratch at)
  TopScreenCameraVec3 To;    // ... and its CamColChk position (scratch eye)
};
struct TopScreenVrCameraWrite {
  TopScreenCameraVec3 Eye;
  TopScreenCameraVec3 At;
  std::int16_t Pitch = 0;
  std::int16_t Yaw = 0;
  float FovDegrees = 0.0F;   // play+0x198 and the FovScalar value, unclamped
};

class TopScreenVrCameraSource {
 public:
  virtual ~TopScreenVrCameraSource() = default;
  // Camera_Update entry of any camera (the guest thread). True: the host owns this update and the
  // phases below run; false: the compiled native Camera_Update runs unchanged.
  virtual bool Begin(NativeA32Memory &memory,
                     const TopScreenVrCameraBegin &begin) = 0;
  // Interface phase: the BgCheck segment of this update (head inside geometry or a placement probe).
  virtual TopScreenVrCameraSegment Segment() = 0;
  // Collision commit: BgCheckInfo's result (`collided` = the CamColChk position, `bgId` its return) and
  // the camera to write this update.
  virtual TopScreenVrCameraWrite Commit(const TopScreenCameraVec3 &collided,
                                        std::uint32_t bgId) = 0;
  // Quake phase: whether the native quake update reported a shake (its offsets are never applied).
  virtual void Quake(bool active) = 0;
  // FovScalar 0x00478EB8 of the main view: the unclamped FOV while the host owned the last main-camera
  // update; `loaded` is the view's FOV as the native code loads it (the readback of play+0x198).
  virtual bool Fov(float loaded, float *degrees) = 0;
};

enum class TopScreenCameraGuestPhase : std::uint8_t {
  Idle,
  Water,
  Interface,
  Collision,
  Quake,
  CameraData,
};

struct TopScreenFreeCameraGuestRuntime {
  TopScreenFreeCameraState Camera;
  TopScreenFreeCameraOwnershipInput LastOwnershipInput;
  std::uint32_t LastOwnershipBlockers = TopScreenFreeCameraBlockerNone;
  TopScreenCameraGuestPhase Phase = TopScreenCameraGuestPhase::Idle;
  std::uint16_t DistortionPhase = 0;
  std::uint32_t OutAddress = 0;
  std::uint32_t CameraAddress = 0;
  std::uint32_t GlobalContextAddress = 0;
  std::uint32_t PlayerAddress = 0;
  std::uint32_t OriginalStackAddress = 0;
  std::uint32_t ScratchAddress = 0;
  std::int16_t RightStickX = 0;
  std::int16_t RightStickY = 0;
  bool RelativeInput = false;
  // Mode R: the source (null outside the in-world mode) and whether it owns the running update.
  TopScreenVrCameraSource *Vr = nullptr;
  bool VrOwned = false;
  TopScreenVrCameraWrite VrWrite;
};

bool BeginTopScreenFreeCameraGuestUpdate(
    NativeA32Memory &memory, std::uint32_t outAddress,
    std::uint32_t cameraAddress, std::uint32_t stackAddress,
    std::int16_t rightStickX, std::int16_t rightStickY,
    bool relativeInput,
    TopScreenFreeCameraGuestRuntime *runtime, bool *active,
    std::string *error = nullptr);

bool ApplyTopScreenFreeCameraEnvironment(
    NativeA32Memory &memory, TopScreenFreeCameraGuestRuntime *runtime,
    std::string *error = nullptr);
bool PrepareTopScreenFreeCameraCollision(
    NativeA32Memory &memory, TopScreenFreeCameraGuestRuntime *runtime,
    std::string *error = nullptr);
// `bgCheckResult`: BgCheckInfo's return value (r0), handed to a mode-R source.
bool CommitTopScreenFreeCameraCollision(
    NativeA32Memory &memory, TopScreenFreeCameraGuestRuntime *runtime,
    std::string *error = nullptr, std::uint32_t bgCheckResult = 0U);
bool ApplyTopScreenFreeCameraQuake(
    NativeA32Memory &memory, TopScreenFreeCameraGuestRuntime *runtime,
    bool quakeActive, std::string *error = nullptr);
bool ApplyTopScreenFreeCameraData(
    NativeA32Memory &memory, TopScreenFreeCameraGuestRuntime *runtime,
    std::int16_t cameraDataIndex, std::string *error = nullptr);

void ResetTopScreenFreeCameraGuestUpdate(
    TopScreenFreeCameraGuestRuntime *runtime) noexcept;

inline constexpr std::uint32_t kTopScreenCameraCheckWater = 0x002D06A0U;
inline constexpr std::uint32_t kTopScreenCameraUpdateInterface = 0x00330D84U;
inline constexpr std::uint32_t kTopScreenCameraBgCheckInfo = 0x003553FCU;
inline constexpr std::uint32_t kTopScreenCameraQuakeUpdate = 0x004787E8U;
inline constexpr std::uint32_t kTopScreenCameraGetDataId = 0x0047BFF8U;

} // namespace Oot3dNativeGame
