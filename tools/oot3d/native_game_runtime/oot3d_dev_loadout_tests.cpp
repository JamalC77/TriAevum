// TRIAEVUM_DEV_LOADOUT against a fake guest: the vocabulary, the SaveContext
// fields each item writes (and leaves alone), adult Link, the slot-table
// check, and when the runtime applies (gameplay start, warp arrival, continue)
// and refreshes the player.
#include "oot3d_dev_loadout.h"

#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
using namespace Oot3dNativeGame;

int gChecks = 0;
void Require(bool condition, const std::string &what) {
  ++gChecks;
  if (!condition) throw std::runtime_error(what);
}

constexpr uint32_t kSave = 0x00587958U;
constexpr uint32_t kPlay = 0x08100000U;
constexpr uint32_t kPlayer = 0x08200000U;
constexpr uint32_t kSlotTable = 0x0053CC44U;

class FakeGuest final : public DevWarpGuest {
 public:
  bool Read(uint32_t address, std::span<uint8_t> bytes) override {
    for (size_t index = 0; index < bytes.size(); ++index) {
      const auto found = Memory.find(address + static_cast<uint32_t>(index));
      bytes[index] = found == Memory.end() ? 0U : found->second;
    }
    return true;
  }
  bool Write(uint32_t address, std::span<const uint8_t> bytes) override {
    for (size_t index = 0; index < bytes.size(); ++index) Memory[address + static_cast<uint32_t>(index)] = bytes[index];
    return true;
  }
  bool RequestTransition(uint32_t, uint16_t, bool *accepted, std::string *) override {
    *accepted = true;
    return true;
  }
  bool RefreshPlayerEquipment(uint32_t playState, uint32_t player, std::string *) override {
    Refreshes.push_back({playState, player});
    return true;
  }
  template <class T> void Put(uint32_t address, T value) {
    std::array<uint8_t, sizeof(T)> bytes{};
    std::memcpy(bytes.data(), &value, sizeof(T));
    Write(address, bytes);
  }
  template <class T> T Get(uint32_t address) {
    std::array<uint8_t, sizeof(T)> bytes{};
    Read(address, bytes);
    T value{};
    std::memcpy(&value, bytes.data(), sizeof(T));
    return value;
  }
  std::unordered_map<uint32_t, uint8_t> Memory;
  std::vector<std::pair<uint32_t, uint32_t>> Refreshes;
};

// A fresh child file in play: 3 hearts, nothing owned but the Kokiri tunic
// and boots, empty buttons, code.bin's item -> slot table, a ready player.
FakeGuest ChildGame() {
  FakeGuest guest;
  guest.Put<int32_t>(kSave + 0x04U, 1);
  guest.Put<int16_t>(kSave + 0x42U, 0x30);
  guest.Put<int16_t>(kSave + 0x44U, 0x30);
  for (uint32_t button = 0; button < 5U; ++button) guest.Put<uint8_t>(kSave + 0x80U + button, 0xFF);
  for (uint32_t slot = 0; slot < 4U; ++slot) guest.Put<uint8_t>(kSave + 0x85U + slot, 0xFF);
  guest.Put<uint16_t>(kSave + 0x8AU, 0x1100);  // Kokiri tunic and boots equipped
  for (uint32_t slot = 0; slot < 26U; ++slot) guest.Put<uint8_t>(kSave + 0x8CU + slot, 0xFF);
  guest.Put<uint16_t>(kSave + 0xB6U, 0x1100);  // owned: Kokiri tunic and boots
  guest.Put<uint32_t>(kSave + 0xB8U, 0x00001000U);  // something else in the upgrades (wallet 1)
  const std::array<uint8_t, 16> slots{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                      0x07, 0x08, 0x09, 0x09, 0x0A, 0x0B, 0x0C, 0x0D};
  for (uint32_t item = 0; item < slots.size(); ++item) guest.Put<uint8_t>(kSlotTable + item, slots[item]);
  guest.Put<uint32_t>(kPlay + 0x20ACU, kPlayer);
  guest.Put<uint32_t>(0x0050AF68U, 2U);
  guest.Put<uint8_t>(kPlay + 0x100U, 3U);
  guest.Put<uint8_t>(kPlay + 0x101U, 2U);
  return guest;
}

DevWarpStatus InPlay(DevWarpPhase phase = DevWarpPhase::Idle) {
  DevWarpStatus status;
  status.Phase = phase;
  status.Observation.PlayState = true;
  status.Observation.Gameplay = true;
  status.Observation.Player = true;
  status.Observation.Scene = 0x63;
  return status;
}

void Run(DevLoadoutRuntime &runtime, FakeGuest &guest, const DevWarpStatus &status, uint64_t &frame, int frames) {
  for (int index = 0; index < frames; ++index) runtime.Advance(guest, kPlay, status, frame++);
}

void Vocabulary() {
  std::string error;
  auto spec = ParseDevLoadout("sword,shield,slingshot,nuts,sticks,hearts:6", &error);
  Require(spec.has_value() && spec->Sword && spec->Shield && spec->Slingshot && spec->Nuts && spec->Sticks &&
              !spec->Boomerang && !spec->Bombs && spec->Hearts == 6,
          "the example loadout parses");
  Require(DescribeDevLoadout(*spec) == "sword,shield,slingshot,nuts,sticks,hearts:6", "the canonical description");
  spec = ParseDevLoadout(" Full_Child ", &error);
  Require(spec.has_value() && spec->Sword && spec->Boomerang && spec->Bombs && spec->Hearts == 10, "full_child");
  spec = ParseDevLoadout("full_child,hearts:4", &error);
  Require(spec.has_value() && spec->Hearts == 4, "a later hearts token wins");
  Require(!ParseDevLoadout("sword,lightsaber", &error) && error.find("lightsaber") != std::string::npos,
          "an unknown token is refused by name");
  Require(!ParseDevLoadout("hearts:0", &error) && !ParseDevLoadout("hearts:21", &error) &&
              !ParseDevLoadout("hearts:x", &error) && !ParseDevLoadout("hearts:", &error),
          "hearts outside 1..20 are refused");
  Require(!ParseDevLoadout(" , ", &error), "an empty loadout is refused");
}

void ChildFields() {
  FakeGuest guest = ChildGame();
  DevLoadoutSpec spec;
  spec.Sword = spec.Shield = spec.Slingshot = spec.Nuts = spec.Sticks = true;
  spec.Hearts = 6;
  DevLoadoutRuntime runtime(spec);
  uint64_t frame = 100;
  Run(runtime, guest, InPlay(), frame, 20);
  Require(runtime.Applications() == 1U, "applied once gameplay settled");
  for (const auto &field : runtime.LastFields()) Require(field.Ok, "field " + field.Name + " reads back");
  Require(guest.Get<uint16_t>(kSave + 0xB6U) == 0x1111U, "owned: Kokiri Sword bit 0, Deku Shield bit 4, kept the rest");
  Require(guest.Get<uint16_t>(kSave + 0x8AU) == 0x1111U, "equipped: sword 1, shield 1, tunic/boots kept");
  Require(guest.Get<uint8_t>(kSave + 0x80U) == 0x3BU, "B: Kokiri Sword");
  Require(guest.Get<uint8_t>(kSave + 0x81U) == 0x01U && guest.Get<uint8_t>(kSave + 0x85U) == 0x01U, "Y: nuts, slot 1");
  Require(guest.Get<uint8_t>(kSave + 0x82U) == 0x00U && guest.Get<uint8_t>(kSave + 0x86U) == 0x00U, "X: sticks, slot 0");
  Require(guest.Get<uint8_t>(kSave + 0x83U) == 0x06U && guest.Get<uint8_t>(kSave + 0x87U) == 0x06U,
          "I: slingshot, slot 6");
  Require(guest.Get<uint8_t>(kSave + 0x84U) == 0xFFU && guest.Get<uint8_t>(kSave + 0x88U) == 0xFFU, "II untouched");
  Require(guest.Get<uint8_t>(kSave + 0x8CU + 0U) == 0x00U && guest.Get<uint8_t>(kSave + 0x8CU + 1U) == 0x01U &&
              guest.Get<uint8_t>(kSave + 0x8CU + 6U) == 0x06U && guest.Get<uint8_t>(kSave + 0x8CU + 2U) == 0xFFU,
          "items in slots 0, 1, 6 only");
  Require(guest.Get<int8_t>(kSave + 0xA6U + 0U) == 10 && guest.Get<int8_t>(kSave + 0xA6U + 1U) == 20 &&
              guest.Get<int8_t>(kSave + 0xA6U + 6U) == 30,
          "ammo full at the first capacities (sticks 10, nuts 20, seeds 30)");
  Require(guest.Get<uint32_t>(kSave + 0xB8U) == (0x00001000U | (1U << 14U) | (1U << 17U) | (1U << 20U)),
          "bullet bag, stick and nut upgrades at level 1; the wallet kept");
  Require(guest.Get<int16_t>(kSave + 0x42U) == 96 && guest.Get<int16_t>(kSave + 0x44U) == 96, "6 hearts, full");
  // The player re-reads its equipment once it is ready.
  Run(runtime, guest, InPlay(), frame, 2);
  Require(guest.Refreshes.size() == 1U && guest.Refreshes[0].first == kPlay && guest.Refreshes[0].second == kPlayer,
          "one player refresh with (play, player)");
  Run(runtime, guest, InPlay(), frame, 100);
  Require(runtime.Applications() == 1U && guest.Refreshes.size() == 1U, "nothing more while nothing happens");
}

void UpgradesAndButtons() {
  FakeGuest guest = ChildGame();
  guest.Put<uint32_t>(kSave + 0xB8U, 2U << 14U);  // bullet bag 40 already
  guest.Put<uint8_t>(kSave + 0x84U, 0x01U);         // nuts already on II
  guest.Put<uint8_t>(kSave + 0x88U, 0x01U);
  DevLoadoutSpec spec;
  spec.Slingshot = spec.Nuts = true;
  const auto fields = PlanDevLoadout(guest, spec, true);
  bool clearedII = false;
  for (const auto &field : fields) {
    if (field.Address == kSave + 0x84U) clearedII = field.Value == 0xFFU;
    Require(field.Address != kSave + 0x80U, "no sword: B untouched");
    Require(field.Address != kSave + 0x42U && field.Address != kSave + 0x44U, "no hearts: health untouched");
  }
  Require(clearedII, "nuts move to Y: II no longer shows them too");
  DevLoadoutRuntime runtime(spec);
  uint64_t frame = 0;
  Run(runtime, guest, InPlay(), frame, 20);
  Require(guest.Get<int8_t>(kSave + 0xA6U + 6U) == 40, "an existing bigger bag keeps its level and fills to 40");
  Require(((guest.Get<uint32_t>(kSave + 0xB8U) >> 14U) & 7U) == 2U, "the bag level is not lowered");
}

void AdultAndSlotTable() {
  FakeGuest adult = ChildGame();
  adult.Put<int32_t>(kSave + 0x04U, 0);
  DevLoadoutSpec spec;
  spec.Sword = spec.Slingshot = true;
  spec.Hearts = 5;
  const auto fields = PlanDevLoadout(adult, spec, false);
  Require(fields.size() == 2U && fields[0].Address == kSave + 0x42U && fields[1].Address == kSave + 0x44U,
          "adult Link: hearts only");
  DevLoadoutRuntime runtime(spec);
  uint64_t frame = 0;
  Run(runtime, adult, InPlay(), frame, 40);
  Require(runtime.Applications() == 1U && adult.Refreshes.empty(), "no player refresh for hearts only");

  FakeGuest wrongTable = ChildGame();
  wrongTable.Put<uint8_t>(kSlotTable + 0x06U, 0x09U);  // not the table the slots were pinned from
  DevLoadoutSpec slingshot;
  slingshot.Slingshot = true;
  Require(PlanDevLoadout(wrongTable, slingshot, true).empty(), "a slot-table mismatch leaves the item out");
}

void WhenItApplies() {
  FakeGuest guest = ChildGame();
  DevLoadoutSpec spec;
  spec.Sword = true;
  spec.Hearts = 4;
  DevLoadoutRuntime runtime(spec);
  uint64_t frame = 0;
  // Not in play (title): nothing.
  DevWarpStatus title;
  Run(runtime, guest, title, frame, 50);
  Require(runtime.Applications() == 0U, "nothing before gameplay");
  // The boot warp is on its way: deferred until it arrives.
  Run(runtime, guest, InPlay(DevWarpPhase::Requested), frame, 50);
  Require(runtime.Applications() == 0U, "deferred while a developer warp is on its way");
  guest.Put<uint32_t>(kPlay + 0x20ACU, kPlayer + 0x1000U);  // the player of the new scene
  // Not ready (PlayState subtype != 2, as in a text box): the refresh waits.
  guest.Put<uint8_t>(kPlay + 0x101U, 1U);
  Run(runtime, guest, InPlay(DevWarpPhase::Arrived), frame, 20);
  Require(runtime.Applications() == 1U, "applied after the boot warp arrived");
  Require(guest.Get<uint8_t>(kSave + 0x80U) == 0x3BU, "the sword is on B");
  Run(runtime, guest, InPlay(DevWarpPhase::Arrived), frame, 30);
  Require(guest.Refreshes.empty(), "the refresh waits for a ready player");
  guest.Put<uint8_t>(kPlay + 0x101U, 2U);
  Run(runtime, guest, InPlay(DevWarpPhase::Arrived), frame, 2);
  Require(guest.Refreshes.size() == 1U && guest.Refreshes[0].second == kPlayer + 0x1000U, "then refreshes");

  // A second warp arrives: applied again.
  Run(runtime, guest, InPlay(DevWarpPhase::Requested), frame, 10);
  guest.Put<uint8_t>(kSave + 0x80U, 0xFFU);
  Run(runtime, guest, InPlay(DevWarpPhase::Arrived), frame, 20);
  Require(runtime.Applications() == 2U && guest.Get<uint8_t>(kSave + 0x80U) == 0x3BU, "applied after another warp");

  // Death: health 0, then game over and continue with 3 hearts.
  guest.Put<int16_t>(kSave + 0x44U, 0);
  Run(runtime, guest, InPlay(DevWarpPhase::Arrived), frame, 60);
  Require(runtime.Applications() == 2U, "nothing while Link is down");
  guest.Put<int16_t>(kSave + 0x44U, 0x30);
  guest.Put<uint32_t>(kPlay + 0x20ACU, kPlayer + 0x2000U);
  Run(runtime, guest, InPlay(DevWarpPhase::Arrived), frame, 20);
  Require(runtime.Applications() == 3U, "applied after continue");
  Require(guest.Get<int16_t>(kSave + 0x44U) == 64, "health full again (4 hearts)");
  // Ordinary damage does not re-apply.
  guest.Put<int16_t>(kSave + 0x44U, 40);
  Run(runtime, guest, InPlay(DevWarpPhase::Arrived), frame, 60);
  Require(runtime.Applications() == 3U && guest.Get<int16_t>(kSave + 0x44U) == 40, "damage alone changes nothing");
}

void StartWithoutWarp() {
  FakeGuest guest = ChildGame();
  DevLoadoutSpec spec;
  spec.Shield = true;
  DevLoadoutRuntime runtime(spec);
  uint64_t frame = 0;
  Run(runtime, guest, InPlay(), frame, 14);
  Require(runtime.Applications() == 0U, "waits 15 frames of stable play");
  Run(runtime, guest, InPlay(), frame, 1);
  Require(runtime.Applications() == 1U, "then applies at gameplay start");
  // A scene change before the refresh: the new player reads the SaveContext itself.
  guest.Put<uint8_t>(kPlay + 0x100U, 0U);
  Run(runtime, guest, InPlay(), frame, 1);
  guest.Put<uint32_t>(kPlay + 0x20ACU, kPlayer + 0x3000U);
  Run(runtime, guest, InPlay(), frame, 5);
  Require(guest.Refreshes.empty(), "no refresh of a player that was replaced");
}

} // namespace

int main() {
  try {
    Vocabulary();
    ChildFields();
    UpgradesAndButtons();
    AdultAndSlotTable();
    WhenItApplies();
    StartWithoutWarp();
  } catch (const std::exception &exception) {
    std::cerr << "oot3d_dev_loadout_tests FAILED after " << gChecks << " checks: " << exception.what() << '\n';
    return 1;
  }
  std::cout << "oot3d_dev_loadout_tests: " << gChecks << " checks passed\n";
  return 0;
}
