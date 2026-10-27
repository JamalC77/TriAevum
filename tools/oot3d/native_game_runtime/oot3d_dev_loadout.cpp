#include "oot3d_dev_loadout.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <optional>
#include <span>
#include <string>

namespace Oot3dNativeGame {
namespace {

// ---- Where every offset comes from.
// SaveContext base: oot3d_dev_warp.cpp (kSaveContext), the TopScreen HUD and
// item code (oot3d_top_screen_mod_profile_hud.cpp, oot3d_top_screen_wide_items_runtime.cpp).
constexpr uint32_t kSave = 0x00587958U;
// s32 linkAge, 0 adult / 1 child: oot3d_dev_warp.cpp kSaveAge,
// guest_ui_layout.h save_context::kLinkAge.
constexpr uint32_t kAge = kSave + 0x04U;
// s16 healthCapacity / health in 1/16 hearts: guest_ui_layout.h
// save_context::kHealthCapacity (0x42) and kHealth (0x44; also read by the
// TopScreen HUD gate and oot3d_top_screen_gameplay_action_consumer.cpp);
// ui_hud_content.h kOot3dHealthUnitsPerHeart = 16.
constexpr uint32_t kHealthCapacity = kSave + 0x42U;
constexpr uint32_t kHealth = kSave + 0x44U;
constexpr int16_t kHealthPerHeart = 16;
// Current ItemEquips at +0x80 (guest_ui_layout.h save_context::kCurrentEquips
// and struct ItemEquips): button_items[5] B, Y, X, I, II (ui_semantics.h
// Oot3dButton) at +0x80..+0x84, button_slots[4] Y, X, I, II at +0x85..+0x88,
// equipped u16 at +0x8A. +0x80 is the B item the TopScreen equipment action
// writes; +0x83 / +0x84 are the TopScreen profile's ZR / ZL items
// (oot3d_top_screen_dpad_presentation.cpp ItemZr / ItemZl).
constexpr uint32_t kButtonItems = kSave + 0x80U;
constexpr uint32_t kButtonSlots = kSave + 0x85U;
constexpr uint32_t kEquipped = kSave + 0x8AU;
// Inventory at +0x8C (guest_ui_layout.h struct Inventory): items[26] +0x8C
// (oot3d_top_screen_gameplay_action_consumer.cpp kInventoryItems 0x005879E4),
// ammo[16] +0xA6 indexed by inventory slot (ui_hud_content.cpp
// ResolveButtonAmmo), owned equipment u16 +0xB6 (kOwnedEquipment 0x00587A0E),
// upgrades u32 +0xB8 (the TopScreen wallet read).
constexpr uint32_t kItems = kSave + 0x8CU;
constexpr uint32_t kAmmo = kSave + 0xA6U;
constexpr uint32_t kOwnedEquipment = kSave + 0xB6U;
constexpr uint32_t kUpgrades = kSave + 0xB8U;
// code.bin's item -> inventory slot table (gItemSlots), read by the TopScreen
// d-pad and item code at 0x0053CC44 + item. Its values for these items are
// pinned in ui_inventory_content.cpp kItemToInventorySlot; the loadout checks
// the guest table against them and leaves an item out on a mismatch.
constexpr uint32_t kItemSlotTable = 0x0053CC44U;

// Item ids: ui_value_domains.h ItemId.
constexpr uint8_t kItemStick = 0x00U, kItemNut = 0x01U, kItemBomb = 0x02U, kItemSlingshot = 0x06U,
                  kItemBoomerang = 0x0EU, kItemSwordKokiri = 0x3BU;
constexpr uint8_t kNone = 0xFFU;

// Equipment: ui_inventory_content.cpp kEquipmentShifts {0, 4, 8, 12} and
// masks; ui_pause_gear_semantics.cpp: Kokiri Sword is sword bit zero with
// equipped value one, Deku Shield is shield bit zero with equipped value one.
constexpr uint16_t kSwordMask = 0x000FU, kShieldMask = 0x00F0U;
constexpr uint16_t kOwnKokiriSword = 1U << 0U, kOwnDekuShield = 1U << 4U;
constexpr uint16_t kEquipKokiriSword = 1U << 0U, kEquipDekuShield = 1U << 4U;

// Upgrades: ui_inventory_content.cpp kUpgradeMasks / kUpgradeShifts /
// kUpgradeCapacities (UPG_BOMB_BAG 1, UPG_BULLET_BAG 5, UPG_DEKU_STICKS 6,
// UPG_DEKU_NUTS 7).
struct Upgrade {
  uint32_t Mask;
  uint32_t Shift;
  std::array<int, 4> Capacity;
};
constexpr Upgrade kBombBag{0x00000038U, 3U, {0, 20, 30, 40}};
constexpr Upgrade kBulletBag{0x0001C000U, 14U, {0, 30, 40, 50}};
constexpr Upgrade kStickUpgrade{0x000E0000U, 17U, {0, 10, 20, 30}};
constexpr Upgrade kNutUpgrade{0x00700000U, 20U, {0, 20, 30, 40}};

// Button indices (Oot3dButton): 0 B, 1 Y, 2 X, 3 I, 4 II.
constexpr int kButtonY = 1, kButtonX = 2, kButtonI = 3, kButtonII = 4;

// Player readiness for the refresh: the TopScreen equipment action's gate
// (oot3d_top_screen_gameplay_action_consumer.cpp ReadActionState): pause
// state 0x0050AF68 == 2, PlayState +0x100/+0x101 == 3/2, Player +0x1224
// (held actor) == 0, +0x12BC (cutscene action) == 0, +0x1A9 == +0x1AC (held
// vs. current item action). The player is PlayState +0x20AC (the same field
// as oot3d_dev_warp.cpp kPlayPlayerHead).
constexpr uint32_t kPauseState = 0x0050AF68U;
constexpr uint32_t kPlayStateType = 0x100U, kPlayStateSubtype = 0x101U, kPlayPlayer = 0x20ACU;
constexpr uint32_t kPlayerHeldActor = 0x1224U, kPlayerCutsceneAction = 0x12BCU, kPlayerHeldItemAction = 0x1A9U,
                   kPlayerItemAction = 0x1ACU;

constexpr uint32_t kSettleFrames = 15U;     // stable play before an application
constexpr uint64_t kRefreshWindow = 600U;   // guest frames a refresh may wait for a ready player

template <class T> bool ReadValue(DevWarpGuest &guest, uint32_t address, T &value) {
  std::array<uint8_t, sizeof(T)> bytes{};
  if (!guest.Read(address, bytes)) return false;
  std::memcpy(&value, bytes.data(), sizeof(T));
  return true;
}

uint32_t ReadSized(DevWarpGuest &guest, uint32_t address, uint32_t size, bool *ok) {
  uint32_t value = 0;
  bool read = false;
  if (size == 1U) {
    uint8_t v = 0;
    read = ReadValue(guest, address, v);
    value = v;
  } else if (size == 2U) {
    uint16_t v = 0;
    read = ReadValue(guest, address, v);
    value = v;
  } else {
    read = ReadValue(guest, address, value);
  }
  if (ok != nullptr) *ok = read;
  return value;
}

bool WriteSized(DevWarpGuest &guest, uint32_t address, uint32_t size, uint32_t value) {
  std::array<uint8_t, 4> bytes{};
  std::memcpy(bytes.data(), &value, 4);  // little-endian guest and host
  return guest.Write(address, std::span<const uint8_t>(bytes.data(), size));
}

std::string Lower(std::string_view text) {
  std::string out(text);
  for (char &c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

std::string_view Trim(std::string_view text) {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
  return text;
}

struct Planner {
  DevWarpGuest &Guest;
  std::vector<DevLoadoutField> Fields;

  uint32_t Read(uint32_t address, uint32_t size) { return ReadSized(Guest, address, size, nullptr); }
  void Set(std::string name, uint32_t address, uint32_t size, uint32_t value) {
    // A later write to the same field replaces the earlier one (one log line each).
    for (auto &field : Fields)
      if (field.Address == address && field.Size == size) {
        field.Value = value;
        return;
      }
    DevLoadoutField field;
    field.Name = std::move(name);
    field.Address = address;
    field.Size = size;
    field.Before = Read(address, size);
    field.Value = value;
    Fields.push_back(std::move(field));
  }
  // The value the plan leaves at `address` (planned or current).
  uint32_t Current(uint32_t address, uint32_t size) {
    for (const auto &field : Fields)
      if (field.Address == address && field.Size == size) return field.Value;
    return Read(address, size);
  }
};

// The guest's inventory slot for `item`, when it matches the pinned table.
std::optional<uint8_t> ItemSlot(DevWarpGuest &guest, uint8_t item, uint8_t expected) {
  uint8_t slot = kNone;
  if (!ReadValue(guest, kItemSlotTable + item, slot) || slot != expected) {
    std::fprintf(stderr,
                 "TRIAEVUM_DEV_LOADOUT skipped item=0x%02X reason=slot_table guest_slot=%u expected_slot=%u\n", item,
                 slot, expected);
    return std::nullopt;
  }
  return slot;
}

} // namespace

std::optional<DevLoadoutSpec> ParseDevLoadout(std::string_view text, std::string *error) {
  DevLoadoutSpec spec;
  const auto fail = [&](std::string message) -> std::optional<DevLoadoutSpec> {
    if (error != nullptr) *error = std::move(message);
    return std::nullopt;
  };
  while (!text.empty()) {
    const size_t comma = text.find(',');
    const std::string token = Lower(Trim(text.substr(0, comma)));
    text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
    if (token.empty()) continue;
    if (token == "sword") spec.Sword = true;
    else if (token == "shield") spec.Shield = true;
    else if (token == "slingshot") spec.Slingshot = true;
    else if (token == "nuts") spec.Nuts = true;
    else if (token == "sticks") spec.Sticks = true;
    else if (token == "boomerang") spec.Boomerang = true;
    else if (token == "bombs") spec.Bombs = true;
    else if (token == "full_child") {
      spec.Sword = spec.Shield = spec.Slingshot = spec.Nuts = spec.Sticks = spec.Boomerang = spec.Bombs = true;
      spec.Hearts = std::max(spec.Hearts, 10);
    } else if (token.rfind("hearts:", 0) == 0) {
      const std::string number = token.substr(7);
      int hearts = 0;
      if (number.empty() || number.size() > 2 ||
          !std::all_of(number.begin(), number.end(), [](char c) { return c >= '0' && c <= '9'; }) ||
          (hearts = std::stoi(number)) < 1 || hearts > 20)
        return fail("hearts takes 1..20 (\"hearts:6\"), not \"" + token + "\"");
      spec.Hearts = hearts;
    } else {
      return fail("unknown loadout item \"" + token +
                  "\" (sword, shield, slingshot, nuts, sticks, boomerang, bombs, hearts:N, full_child)");
    }
  }
  if (spec.Empty()) return fail("the loadout is empty");
  return spec;
}

std::string DescribeDevLoadout(const DevLoadoutSpec &spec) {
  std::string out;
  const auto add = [&](bool on, const char *name) {
    if (!on) return;
    if (!out.empty()) out += ',';
    out += name;
  };
  add(spec.Sword, "sword");
  add(spec.Shield, "shield");
  add(spec.Slingshot, "slingshot");
  add(spec.Nuts, "nuts");
  add(spec.Sticks, "sticks");
  add(spec.Boomerang, "boomerang");
  add(spec.Bombs, "bombs");
  if (spec.Hearts > 0) {
    if (!out.empty()) out += ',';
    out += "hearts:" + std::to_string(spec.Hearts);
  }
  return out;
}

std::vector<DevLoadoutField> PlanDevLoadout(DevWarpGuest &guest, const DevLoadoutSpec &spec, bool child) {
  Planner plan{guest, {}};
  if (child) {
    // Sword and shield: owned, equipped, the sword on B.
    if (spec.Sword || spec.Shield) {
      uint32_t owned = plan.Current(kOwnedEquipment, 2U);
      uint32_t equipped = plan.Current(kEquipped, 2U);
      if (spec.Sword) {
        owned |= kOwnKokiriSword;
        equipped = (equipped & ~uint32_t{kSwordMask}) | kEquipKokiriSword;
      }
      if (spec.Shield) {
        owned |= kOwnDekuShield;
        equipped = (equipped & ~uint32_t{kShieldMask}) | kEquipDekuShield;
      }
      plan.Set("owned_equipment", kOwnedEquipment, 2U, owned);
      plan.Set("equipped", kEquipped, 2U, equipped);
      if (spec.Sword) plan.Set("button_b", kButtonItems, 1U, kItemSwordKokiri);
    }

    struct Item {
      bool Wanted;
      const char *Name;
      uint8_t Id;
      uint8_t ExpectedSlot;  // ui_inventory_content.cpp kItemToInventorySlot
      const Upgrade *Bag;    // capacity upgrade, or null (no ammo)
      int Button;            // Oot3dButton index, or -1
    };
    const std::array<Item, 5> items{{
        {spec.Slingshot, "slingshot", kItemSlingshot, 0x06U, &kBulletBag, kButtonI},
        {spec.Nuts, "nuts", kItemNut, 0x01U, &kNutUpgrade, kButtonY},
        {spec.Sticks, "sticks", kItemStick, 0x00U, &kStickUpgrade, kButtonX},
        {spec.Boomerang, "boomerang", kItemBoomerang, 0x0CU, nullptr, kButtonII},
        {spec.Bombs, "bombs", kItemBomb, 0x02U, &kBombBag, -1},
    }};
    std::array<bool, 5> assigned{};
    std::array<uint8_t, 5> assignedItem{};
    for (const auto &item : items) {
      if (!item.Wanted) continue;
      const auto slot = ItemSlot(guest, item.Id, item.ExpectedSlot);
      if (!slot.has_value()) continue;
      const std::string name = item.Name;
      plan.Set("item_" + name, kItems + *slot, 1U, item.Id);
      if (item.Bag != nullptr) {
        uint32_t upgrades = plan.Current(kUpgrades, 4U);
        uint32_t level = (upgrades & item.Bag->Mask) >> item.Bag->Shift;
        if (level == 0U) {
          level = 1U;
          upgrades = (upgrades & ~item.Bag->Mask) | (level << item.Bag->Shift);
          plan.Set("upgrades", kUpgrades, 4U, upgrades);
        }
        const int capacity = item.Bag->Capacity[std::min<uint32_t>(level, 3U)];
        plan.Set("ammo_" + name, kAmmo + *slot, 1U, static_cast<uint8_t>(capacity));
      }
      if (item.Button > 0) {
        const auto button = static_cast<uint32_t>(item.Button);
        plan.Set("button_" + std::string(item.Button == kButtonY   ? "y"
                                          : item.Button == kButtonX ? "x"
                                          : item.Button == kButtonI ? "i"
                                                                    : "ii"),
                 kButtonItems + button, 1U, item.Id);
        plan.Set("button_slot_" + std::string(item.Button == kButtonY   ? "y"
                                               : item.Button == kButtonX ? "x"
                                               : item.Button == kButtonI ? "i"
                                                                         : "ii"),
                 kButtonSlots + button - 1U, 1U, *slot);
        assigned[button] = true;
        assignedItem[button] = item.Id;
      }
    }
    // The game never shows one item on two buttons: a button the loadout
    // leaves alone that still holds an item it placed elsewhere is cleared.
    static constexpr std::array<const char *, 5> kButtonNames{"b", "y", "x", "i", "ii"};
    for (uint32_t button = 1U; button < 5U; ++button) {
      if (assigned[button]) continue;
      const uint32_t held = plan.Current(kButtonItems + button, 1U);
      for (uint32_t other = 1U; other < 5U; ++other) {
        if (!assigned[other] || assignedItem[other] != held) continue;
        plan.Set(std::string("button_") + kButtonNames[button], kButtonItems + button, 1U, kNone);
        plan.Set(std::string("button_slot_") + kButtonNames[button], kButtonSlots + button - 1U, 1U, kNone);
        break;
      }
    }
  }
  if (spec.Hearts > 0) {
    const auto capacity = static_cast<int16_t>(spec.Hearts * kHealthPerHeart);
    plan.Set("health_capacity", kHealthCapacity, 2U, static_cast<uint16_t>(capacity));
    plan.Set("health", kHealth, 2U, static_cast<uint16_t>(capacity));
  }
  return std::move(plan.Fields);
}

bool DevLoadoutRuntime::PlayerRefreshEligible(DevWarpGuest &guest, uint32_t playState, uint32_t player) const {
  uint32_t pauseState = 0;
  uint8_t stateType = 0, stateSubtype = 0, cutsceneAction = 0, heldItemAction = 0, itemAction = 0;
  uint32_t heldActor = 0;
  return ReadValue(guest, kPauseState, pauseState) && pauseState == 2U &&
         ReadValue(guest, playState + kPlayStateType, stateType) && stateType == 3U &&
         ReadValue(guest, playState + kPlayStateSubtype, stateSubtype) && stateSubtype == 2U &&
         ReadValue(guest, player + kPlayerHeldActor, heldActor) && heldActor == 0U &&
         ReadValue(guest, player + kPlayerCutsceneAction, cutsceneAction) && cutsceneAction == 0U &&
         ReadValue(guest, player + kPlayerHeldItemAction, heldItemAction) &&
         ReadValue(guest, player + kPlayerItemAction, itemAction) && heldItemAction == itemAction;
}

bool DevLoadoutRuntime::Apply(DevWarpGuest &guest, bool child, const char *reason, uint64_t guestFrame) {
  auto fields = PlanDevLoadout(guest, mSpec, child);
  uint32_t mismatches = 0;
  bool playerVisible = false;
  for (auto &field : fields) {
    bool read = false;
    const bool wrote = WriteSized(guest, field.Address, field.Size, field.Value);
    field.ReadBack = ReadSized(guest, field.Address, field.Size, &read);
    field.Ok = wrote && read && field.ReadBack == field.Value;
    mismatches += field.Ok ? 0U : 1U;
    if (field.Before != field.Value &&
        (field.Address == kEquipped || field.Address == kOwnedEquipment ||
         (field.Address >= kButtonItems && field.Address < kButtonItems + 5U)))
      playerVisible = true;
    std::fprintf(stderr, "TRIAEVUM_DEV_LOADOUT field=%s addr=0x%08X before=0x%X wrote=0x%X read=0x%X ok=%d\n",
                 field.Name.c_str(), field.Address, field.Before, field.Value, field.ReadBack, field.Ok ? 1 : 0);
  }
  std::fprintf(stderr,
               "TRIAEVUM_DEV_LOADOUT applied=%s loadout=\"%s\" frame=%llu child=%d fields=%zu mismatches=%u%s\n",
               reason, DescribeDevLoadout(mSpec).c_str(), static_cast<unsigned long long>(guestFrame), child ? 1 : 0,
               fields.size(), mismatches, child ? "" : " child_gear=skipped_adult");
  mLastFields = std::move(fields);
  ++mApplications;
  return playerVisible;
}

void DevLoadoutRuntime::Advance(DevWarpGuest &guest, uint32_t playState, const DevWarpStatus &warp,
                                uint64_t guestFrame) {
  const auto &observation = warp.Observation;
  // A developer warp arrived (the boot warp, F1 or a lab warp).
  if (warp.Phase == DevWarpPhase::Arrived && mLastPhase != DevWarpPhase::Arrived) mDue = mStarted ? "warp" : "start";
  mLastPhase = warp.Phase;
  const bool warping = warp.Phase == DevWarpPhase::Waiting || warp.Phase == DevWarpPhase::Requested;

  int16_t health = 0;
  uint32_t player = 0;
  const bool healthRead = ReadValue(guest, kHealth, health);
  if (playState != 0U && !ReadValue(guest, playState + kPlayPlayer, player)) player = 0;
  const bool inPlay = healthRead && observation.PlayState && observation.Gameplay && observation.Player && player != 0U;

  // Game over and continue (or a fairy): Link back in play after zero health.
  if (inPlay && health <= 0) mDied = true;
  if (inPlay && health > 0 && mDied) {
    mDied = false;
    if (mStarted) mDue = "continue";
  }

  if (mRefreshDue) {
    if (player != 0U && player != mSettledPlayer) {
      // A new player (scene change) builds its equipment from the SaveContext itself.
      std::fprintf(stderr, "TRIAEVUM_DEV_LOADOUT refresh=not_needed reason=new_player frame=%llu\n",
                   static_cast<unsigned long long>(guestFrame));
      mRefreshDue = false;
    } else if (inPlay && health > 0 && PlayerRefreshEligible(guest, playState, player)) {
      std::string error;
      const bool ok = guest.RefreshPlayerEquipment(playState, player, &error);
      std::fprintf(stderr, "TRIAEVUM_DEV_LOADOUT refresh=%s frame=%llu%s%s\n", ok ? "ok" : "failed",
                   static_cast<unsigned long long>(guestFrame), ok ? "" : " error=", ok ? "" : error.c_str());
      mRefreshes += ok ? 1U : 0U;
      mRefreshDue = false;
    } else if (guestFrame > mRefreshDeadline) {
      std::fprintf(stderr,
                   "TRIAEVUM_DEV_LOADOUT refresh=gave_up frame=%llu (the player never became ready; the gear "
                   "shows after the next scene load)\n",
                   static_cast<unsigned long long>(guestFrame));
      mRefreshDue = false;
    }
  }

  if (mDue == nullptr || !inPlay || health <= 0 || warping) {
    mSettledFrames = 0;
    return;
  }
  if (player != mSettledPlayer) {
    mSettledPlayer = player;
    mSettledFrames = 0;
    mRefreshDue = false;
  }
  if (++mSettledFrames < kSettleFrames) return;
  int32_t age = 0;
  if (!ReadValue(guest, kAge, age)) return;
  const bool child = age != 0;
  const char *reason = mDue;
  mDue = nullptr;
  mStarted = true;
  if (Apply(guest, child, reason, guestFrame)) {
    mRefreshDue = true;
    mRefreshDeadline = guestFrame + kRefreshWindow;
  }
}

} // namespace Oot3dNativeGame
