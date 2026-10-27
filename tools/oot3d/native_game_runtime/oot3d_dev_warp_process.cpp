#include "oot3d_dev_warp.h"

#include "oot3d_native_a32_process.h"

#include <array>

namespace Oot3dNativeGame {

namespace {

class DevWarpProcessGuest final : public DevWarpGuest {
 public:
  explicit DevWarpProcessGuest(NativeA32Process &process) : mProcess(process) {}

  bool Read(uint32_t address, std::span<uint8_t> bytes) override {
    return mProcess.Memory().ReadBytes(address, bytes);
  }

  bool Write(uint32_t address, std::span<const uint8_t> bytes) override {
    return mProcess.Memory().WriteBytes(address, bytes);
  }

  bool RequestTransition(uint32_t playState, uint16_t entrance, bool *accepted, std::string *error) override {
    constexpr uint32_t kRequestTransition = 0x003348E8U;
    constexpr uint32_t kPrepareTransitionEffect = 0x0035DA3CU;
    constexpr uint32_t kDirectCallReturn = 0x60000000U;
    constexpr uint32_t kTransitionTriggerStart = 20U;
    const std::array<uint32_t, 3> arguments{playState, entrance, kTransitionTriggerStart};
    uint32_t result = 0U;
    if (!mProcess.InvokeFunctionWithResult(kRequestTransition, arguments, kDirectCallReturn, &result, error))
      return false;
    *accepted = result != 0U;
    if (!*accepted) return true;
    const std::array<uint32_t, 1> prepare{playState};
    return mProcess.InvokeFunction(kPrepareTransitionEffect, prepare, kDirectCallReturn, error);
  }

  bool RefreshPlayerEquipment(uint32_t playState, uint32_t player, std::string *error) override {
    // The same sequence as ConsumeTopScreenGameplayActions'
    // equipment refresh (oot3d_top_screen_gameplay_action_consumer.cpp),
    // without its sound: clear Player+0x172C, call Player_SetEquipmentData
    // (0x0034913C) with (play, player), then set Player+0x1714 bit 3.
    constexpr uint32_t kPlayerSetEquipmentData = 0x0034913CU;
    constexpr uint32_t kDirectCallReturn = 0x60000000U;
    auto &memory = mProcess.Memory();
    uint32_t stateFlags2 = 0U;
    if (!memory.IsWritable(player + 0x172CU, 4U) || !memory.IsWritable(player + 0x1714U, 4U) ||
        !memory.Read32(player + 0x1714U, &stateFlags2)) {
      if (error != nullptr) *error = "the player's equipment state is not writable";
      return false;
    }
    if (!memory.Write32(player + 0x172CU, 0U)) {
      if (error != nullptr) *error = "cannot reset the player's equipment state";
      return false;
    }
    const std::array<uint32_t, 2> arguments{playState, player};
    if (!mProcess.InvokeFunction(kPlayerSetEquipmentData, arguments, kDirectCallReturn, error)) return false;
    if (!memory.Write32(player + 0x1714U, stateFlags2 | 8U)) {
      if (error != nullptr) *error = "cannot complete the player's equipment refresh";
      return false;
    }
    return true;
  }

 private:
  NativeA32Process &mProcess;
};

} // namespace

std::unique_ptr<DevWarpGuest> MakeDevWarpProcessGuest(NativeA32Process &process) {
  return std::make_unique<DevWarpProcessGuest>(process);
}

} // namespace Oot3dNativeGame
