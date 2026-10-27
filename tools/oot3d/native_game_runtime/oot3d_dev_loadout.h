#pragma once

// Developer starting loadout (developer builds only, TRIAEVUM_DEVELOPER_TOOLS):
// TRIAEVUM_DEV_LOADOUT gives Link gear so a developer run (a boot warp into a
// field) does not start empty-handed. It writes OOT3D's SaveContext in guest
// memory, then has the player re-read its equipment with the same native call
// the TopScreen equipment action uses.
//
// Vocabulary (comma separated, case-insensitive):
//   sword      Kokiri Sword: owned, equipped, on B
//   shield     Deku Shield: owned, equipped
//   slingshot  Fairy Slingshot + Bullet Bag (30), full seeds, on I (ZR in the TopScreen UI)
//   nuts       Deku Nuts (20), on Y
//   sticks     Deku Sticks (10), on X
//   boomerang  Boomerang, on II (ZL in the TopScreen UI)
//   bombs      Bomb Bag (20), full bombs (inventory only, no button)
//   hearts:N   heart containers N (1..20), health full
//   full_child sword,shield,slingshot,nuts,sticks,boomerang,bombs,hearts:10
// The gear is child gear: on an adult Link only hearts are applied.
//
// When: once gameplay starts (deferred while a developer warp is on its way),
// again when a developer warp arrives (TRIAEVUM_DEV_WARP's boot warp, F1 or lab
// warps), and again when Link is back in play after his health reached zero
// (game over and continue, or a fairy).

#include "oot3d_dev_warp.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Oot3dNativeGame {

struct DevLoadoutSpec {
  bool Sword = false;
  bool Shield = false;
  bool Slingshot = false;
  bool Nuts = false;
  bool Sticks = false;
  bool Boomerang = false;
  bool Bombs = false;
  int Hearts = 0;  // 0: leave the hearts alone
  [[nodiscard]] bool Empty() const noexcept {
    return !Sword && !Shield && !Slingshot && !Nuts && !Sticks && !Boomerang && !Bombs && Hearts == 0;
  }
};

// nullopt and `error` for an unknown or malformed token.
std::optional<DevLoadoutSpec> ParseDevLoadout(std::string_view text, std::string *error);
// The canonical token list ("sword,shield,hearts:6").
std::string DescribeDevLoadout(const DevLoadoutSpec &spec);

// One SaveContext field the loadout writes, with what it read back.
struct DevLoadoutField {
  std::string Name;
  uint32_t Address = 0;
  uint32_t Size = 0;  // bytes: 1, 2 or 4
  uint32_t Before = 0;
  uint32_t Value = 0;
  uint32_t ReadBack = 0;
  bool Ok = false;  // written and read back equal
};

// The field writes a spec makes on the given guest (read-modify-write for the
// packed equipment and upgrade words). Pure: reads only; Apply writes.
std::vector<DevLoadoutField> PlanDevLoadout(DevWarpGuest &guest, const DevLoadoutSpec &spec, bool child);

class DevLoadoutRuntime {
 public:
  explicit DevLoadoutRuntime(DevLoadoutSpec spec) : mSpec(spec) {}

  // Frame side: after DevWarpRuntime::Advance, with its status.
  void Advance(DevWarpGuest &guest, uint32_t playState, const DevWarpStatus &warp, uint64_t guestFrame);

  [[nodiscard]] const DevLoadoutSpec &Spec() const noexcept { return mSpec; }
  [[nodiscard]] uint32_t Applications() const noexcept { return mApplications; }
  [[nodiscard]] uint32_t Refreshes() const noexcept { return mRefreshes; }
  [[nodiscard]] const std::vector<DevLoadoutField> &LastFields() const noexcept { return mLastFields; }

 private:
  bool Apply(DevWarpGuest &guest, bool child, const char *reason, uint64_t guestFrame);
  static bool PlayReady(DevWarpGuest &guest, uint32_t playState);
  bool PlayerRefreshEligible(DevWarpGuest &guest, uint32_t playState, uint32_t player) const;

  DevLoadoutSpec mSpec;
  // Why the next application is due ("start", "warp", "continue"), or null.
  const char *mDue = "start";
  bool mStarted = false;
  bool mDied = false;
  DevWarpPhase mLastPhase = DevWarpPhase::Idle;
  uint32_t mSettledFrames = 0;
  uint32_t mSettledPlayer = 0;
  // A player refresh still owed after an application, and its deadline.
  bool mRefreshDue = false;
  uint64_t mRefreshDeadline = 0;
  uint32_t mApplications = 0;
  uint32_t mRefreshes = 0;
  std::vector<DevLoadoutField> mLastFields;
};

} // namespace Oot3dNativeGame
