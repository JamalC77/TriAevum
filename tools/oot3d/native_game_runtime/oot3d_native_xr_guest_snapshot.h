// oot3d_native_xr_guest_snapshot.h: what the in-world mode reads from the guest once per completed
// visual frame (quest-research/inworld-spec.md §4.1), beside CollectNativeActorInteractions, so it
// belongs to the frame it is drawn with. Offsets are the research notes' (decomp numbering, OoT3D layout):
// play state +0x104 scene, +0x364 the main camera (status +0x188, setting +0x18A, yaw +0x17E/+0x184,
// water Y +0x15C), +0xA64 the active camera; the player actor +0x28 position, +0x60 velocity, +0x84
// floor height, +0xBE shape yaw, +0x1710/+0x1714 state flags; SaveContext (0x00587958) +0 entrance,
// +4 age, +8 cutscene index, +0x14E4 game mode.
#pragma once

#include <cstdint>

namespace oot3d::recomp::a32 {
class MemoryBus;
}  // namespace oot3d::recomp::a32

namespace Oot3dNativeGame {

struct XrGuestSnapshot {
    bool PlayerValid = false;  // the player actor (id 0, category PLAYER) at the probed address
    bool CameraValid = false;
    uint32_t Player = 0, PlayState = 0;
    int16_t Scene = -1;
    int32_t Entrance = -1;
    int32_t SaveGameMode = -1;  // 0 = normal gameplay
    int32_t CutsceneIndex = 0;
    int32_t LinkAge = 0;        // 0 adult, 1 child
    float Position[3] = {0.0F, 0.0F, 0.0F};
    float Velocity[3] = {0.0F, 0.0F, 0.0F};
    float FloorHeight = 0.0F;
    int16_t ShapeYaw = 0;
    uint32_t Flags1 = 0, Flags2 = 0;
    int16_t CameraStatus = 0, CameraSetting = 0, CameraYaw = 0, CameraInputYaw = 0;
    float WaterY = 0.0F;
    int16_t ActiveCamera = 0;
};

XrGuestSnapshot ReadXrGuestSnapshot(oot3d::recomp::a32::MemoryBus& memory, uint32_t player, uint32_t playState);

}  // namespace Oot3dNativeGame
