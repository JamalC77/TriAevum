// The in-world guest snapshot (see oot3d_native_xr_guest_snapshot.h).
#include "oot3d_native_xr_guest_snapshot.h"

#include "a32_runtime.h"

#include <bit>
#include <cmath>

namespace Oot3dNativeGame {

namespace {

constexpr uint32_t kSaveContext = 0x00587958U;
constexpr uint32_t kSaveEntrance = kSaveContext + 0x00U;
constexpr uint32_t kSaveAge = kSaveContext + 0x04U;
constexpr uint32_t kSaveCutscene = kSaveContext + 0x08U;
constexpr uint32_t kSaveGameMode = kSaveContext + 0x14E4U;
constexpr uint32_t kPlayScene = 0x104U;
constexpr uint32_t kPlayMainCamera = 0x364U;
constexpr uint32_t kPlayActiveCamera = 0xA64U;
constexpr uint32_t kCameraWaterY = 0x15CU;
constexpr uint32_t kCameraYaw = 0x17EU;
constexpr uint32_t kCameraInputYaw = 0x184U;
constexpr uint32_t kCameraStatus = 0x188U;
constexpr uint32_t kCameraSetting = 0x18AU;
constexpr uint32_t kActorId = 0x00U;
constexpr uint32_t kActorCategory = 0x02U;
constexpr uint8_t kActorCategoryPlayer = 2U;
constexpr uint32_t kActorPosition = 0x28U;
constexpr uint32_t kActorVelocity = 0x60U;
constexpr uint32_t kActorFloorHeight = 0x84U;
constexpr uint32_t kActorShapeYaw = 0xBEU;
constexpr uint32_t kPlayerFlags1 = 0x1710U;
constexpr uint32_t kPlayerFlags2 = 0x1714U;

bool ReadFloat(oot3d::recomp::a32::MemoryBus& memory, uint32_t address, float& value) {
    uint32_t bits = 0;
    if (!memory.Read32(address, &bits)) return false;
    value = std::bit_cast<float>(bits);
    return std::isfinite(value);
}

bool ReadS16(oot3d::recomp::a32::MemoryBus& memory, uint32_t address, int16_t& value) {
    uint16_t bits = 0;
    if (!memory.Read16(address, &bits)) return false;
    value = static_cast<int16_t>(bits);
    return true;
}

bool ReadS32(oot3d::recomp::a32::MemoryBus& memory, uint32_t address, int32_t& value) {
    uint32_t bits = 0;
    if (!memory.Read32(address, &bits)) return false;
    value = static_cast<int32_t>(bits);
    return true;
}

}  // namespace

XrGuestSnapshot ReadXrGuestSnapshot(oot3d::recomp::a32::MemoryBus& memory, uint32_t player, uint32_t playState) {
    XrGuestSnapshot s;
    s.Player = player;
    s.PlayState = playState;
    ReadS32(memory, kSaveEntrance, s.Entrance);
    ReadS32(memory, kSaveAge, s.LinkAge);
    ReadS32(memory, kSaveCutscene, s.CutsceneIndex);
    ReadS32(memory, kSaveGameMode, s.SaveGameMode);
    if (playState == 0U) return s;
    ReadS16(memory, playState + kPlayScene, s.Scene);
    ReadS16(memory, playState + kPlayActiveCamera, s.ActiveCamera);
    const uint32_t camera = playState + kPlayMainCamera;
    s.CameraValid = ReadS16(memory, camera + kCameraStatus, s.CameraStatus) &&
                    ReadS16(memory, camera + kCameraSetting, s.CameraSetting) &&
                    ReadS16(memory, camera + kCameraYaw, s.CameraYaw) &&
                    ReadS16(memory, camera + kCameraInputYaw, s.CameraInputYaw);
    if (!ReadFloat(memory, camera + kCameraWaterY, s.WaterY)) s.WaterY = std::nanf("");
    if (player == 0U) return s;
    // The probe's address is the last Player_Update argument: it is the player only while that actor
    // lives (id 0, category PLAYER); after a scene change it may point at freed memory.
    uint16_t id = 0xFFFFU;
    uint8_t category = 0xFFU;
    if (!memory.Read16(player + kActorId, &id) || !memory.Read8(player + kActorCategory, &category) || id != 0U ||
        category != kActorCategoryPlayer)
        return s;
    bool ok = true;
    for (uint32_t i = 0; i < 3; ++i) {
        ok = ok && ReadFloat(memory, player + kActorPosition + i * 4U, s.Position[i]);
        ok = ok && ReadFloat(memory, player + kActorVelocity + i * 4U, s.Velocity[i]);
    }
    if (!ReadFloat(memory, player + kActorFloorHeight, s.FloorHeight)) s.FloorHeight = std::nanf("");
    ok = ok && ReadS16(memory, player + kActorShapeYaw, s.ShapeYaw) &&
         memory.Read32(player + kPlayerFlags1, &s.Flags1) && memory.Read32(player + kPlayerFlags2, &s.Flags2);
    s.PlayerValid = ok;
    return s;
}

}  // namespace Oot3dNativeGame
