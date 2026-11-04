#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "modlock/gameinterop/entity_abi.h"
#include "modlock/gameinterop/mapped_module_image.h"
#include "modlock/gameinterop/movement_hook.h"
#include "modlock/gameinterop/pawn_observer.h"
#include "pawn_observer_internal.h"

namespace modlock::gameinterop {

std::expected<ModifyCurrency, std::string> ResolveModifyCurrency(const ModuleImage& server) {
  auto address = ResolveSignature(server, "pawn.modify-currency");
  if (!address) {
    return std::unexpected(address.error());
  }
  return reinterpret_cast<ModifyCurrency>(*address);
}

std::expected<RequiredSouls, std::string> ResolveRequiredSouls(const ModuleImage& server) {
  auto address = ResolveSignature(server, "pawn.required-souls");
  if (!address) {
    return std::unexpected(address.error());
  }
  return reinterpret_cast<RequiredSouls>(*address);
}

std::expected<PawnObserver::Sample, std::string> PawnObserver::AdjustCurrency(
    int32_t slot, uint32_t currency, int32_t delta, bool silent, ModifyCurrency modify) {
  // Read the wallet and check that it covers a spend.
  auto before = Observe(slot);
  if (!modify || !before || !before->currencies || currency >= before->currencies->size()) {
    return std::unexpected("native wallet unavailable");
  }
  const auto balance = (*before->currencies)[currency];
  if (delta == 0) {
    return *before;
  }
  if (delta < 0 && balance < -delta) {
    return std::unexpected("the wallet holds less than the spend");
  }

  // ECheats=7. A grant forces gain; a spend only removes the currency.
  modify(PawnForSlot(slot), currency, delta, 7, silent ? 1 : 0, delta > 0 ? 1 : 0,
         delta < 0 ? 1 : 0, nullptr, nullptr);
  auto after = Observe(slot);
  if (!after || after->pawn_handle != before->pawn_handle || !after->currencies) {
    return std::unexpected("native wallet changed owner during adjustment");
  }

  // Native gain modifiers may scale a grant; a spend must remove exactly delta.
  const auto applied = (*after->currencies)[currency] - balance;
  if (delta < 0 ? applied != delta : applied <= 0) {
    // A spend that removed a different amount is rolled back to the balance.
    if (delta < 0 && applied != 0) {
      modify(PawnForSlot(slot), currency, -applied, 7, 1, applied < 0 ? 1 : 0, applied > 0 ? 1 : 0,
             nullptr, nullptr);
    }
    return std::unexpected("native wallet did not apply the adjustment");
  }
  return *after;
}

std::expected<PawnObserver::Sample, std::string> PawnObserver::HoldLevelSouls(
    int32_t slot, int32_t level, int32_t starting_souls, RequiredSouls required,
    ModifyCurrency modify) {
  // Read the wallet and the thresholds of the level and the next one.
  auto before = Observe(slot);
  if (!required || !modify || !before || !before->currencies || !before->spent_souls ||
      !before->pawn) {
    return std::unexpected("native level progression unavailable");
  }
  void* const pawn = before->pawn;
  const int32_t low = required(pawn, std::max(level, 1));
  const int32_t next = required(pawn, level + 1);
  if (level > 1 && low <= 0) {
    return std::unexpected("the hero's level thresholds are unavailable");
  }

  // The last level has no next threshold: the game clamps it to its own.
  const bool last_level = next <= low;
  const int64_t wallet = (*before->currencies)[0];
  const int64_t worth = wallet + *before->spent_souls - starting_souls;
  if (worth >= low && (last_level || worth < next)) {
    return *before;
  }

  // Move the souls to the level's own amount. A grant adds to the wallet; a
  // loss takes from the wallet first, as a hero loses souls on death, and
  // from the spent souls only for what the wallet cannot cover.
  const int64_t delta = low - worth;
  if (delta > 0) {
    modify(pawn, 0, static_cast<int32_t>(delta), 7, 1, 1, 0, nullptr, nullptr);
  } else {
    const int64_t from_wallet = std::min(std::max(wallet, int64_t{0}), -delta);
    if (from_wallet > 0) {
      modify(pawn, 0, -static_cast<int32_t>(from_wallet), 9, 1, 0, 0, nullptr, nullptr);
    }
    const int64_t from_spent = -delta - from_wallet;
    if (from_spent > 0 &&
        !RestorePawnSpentSouls(pawn, static_cast<int32_t>(*before->spent_souls - from_spent))) {
      return std::unexpected("the hero's spent souls could not be written");
    }
  }

  // Check that the pawn is the same and its souls now fit the level.
  auto after = Observe(slot);
  if (!after || after->pawn_handle != before->pawn_handle || !after->currencies ||
      !after->spent_souls) {
    return std::unexpected("native wallet changed owner during the level adjustment");
  }
  const int64_t held = (*after->currencies)[0] + *after->spent_souls - starting_souls;
  if (held < low || (!last_level && held >= next)) {
    return std::unexpected("native wallet did not reach the level's souls");
  }
  return *after;
}

std::expected<PawnObserver::Sample, std::string> PawnObserver::PrepareStartingSouls(
    int32_t slot, int32_t souls, ModifyCurrency modify) {
  // A wallet that already holds the starting souls is left alone.
  auto before = Observe(slot);
  if (souls < 0 || !modify || !before || !before->currencies) {
    return std::unexpected("native starting economy unavailable");
  }
  const auto balance = (*before->currencies)[0];
  if (balance >= souls) {
    return *before;
  }
  if (!CurrentAbilitiesForSlot(slot)) {
    return std::unexpected("starting pawn ownership changed");
  }

  // EGold=0, ECheats=7. Setup grants are silent, with forceGain=true,
  // spendOnly=false, no source ability/entity. Native net-worth progression runs.
  modify(PawnForSlot(slot), 0, souls - balance, 7, 1, 1, 0, nullptr, nullptr);
  auto after = Observe(slot);
  if (!after || after->pawn_handle != before->pawn_handle ||
      after->session_generation != before->session_generation ||
      after->steam_id != before->steam_id || !after->currencies ||
      (*after->currencies)[0] < souls) {
    return std::unexpected("native starting grant did not retain pawn and balance");
  }
  std::fprintf(stderr, "[modlock] starting souls slot=%d previous=%d granted=%d balance=%d\n", slot,
               balance, souls - balance, (*after->currencies)[0]);
  return *after;
}

std::expected<PawnObserver::Sample, std::string> PawnObserver::ResetStartingSouls(
    int32_t slot, int32_t souls, ModifyCurrency modify, ResetHeroPawn reset) {
  // Check the wallet, and match the player before and after each native call.
  auto before = Observe(slot);
  if (souls < 0 || !modify || !reset || !before || !before->currencies ||
      !CurrentAbilitiesForSlot(slot)) {
    return std::unexpected("native starting economy reset unavailable");
  }
  const auto same_player = [&](const std::optional<Sample>& sample) {
    return sample && sample->pawn_handle == before->pawn_handle &&
           sample->session_generation == before->session_generation &&
           sample->steam_id == before->steam_id && sample->hero_id == before->hero_id &&
           sample->team == before->team && sample->currencies;
  };

  // Reset the hero.
  reset(PawnForSlot(slot), true);
  auto sample = Observe(slot);
  if (!same_player(sample) || !CurrentAbilitiesForSlot(slot)) {
    return std::unexpected("native hero reset changed pawn ownership");
  }

  // Bring the balance to the starting souls.
  const int64_t delta = static_cast<int64_t>(souls) - (*sample->currencies)[0];
  if (delta < INT32_MIN || delta > INT32_MAX) {
    return std::unexpected("native starting currency adjustment exceeds integer range");
  }

  // Native hero reset and currency mutation own progression recalculation.
  // Dispatch even a zero adjustment so the native currency path still runs.
  modify(PawnForSlot(slot), 0, static_cast<int32_t>(delta), 7, 1, 1, 0, nullptr, nullptr);
  sample = Observe(slot);
  if (!same_player(sample) || !CurrentAbilitiesForSlot(slot) || (*sample->currencies)[0] != souls) {
    return std::unexpected("native starting reset did not retain pawn and balance");
  }
  return *sample;
}

std::expected<PawnObserver::Sample, std::string> PawnObserver::PrepareAbilityPoints(
    int32_t slot, ModifyCurrency modify) {
  auto sample = Observe(slot);
  if (!modify || !sample || !sample->currencies || !CurrentAbilitiesForSlot(slot)) {
    return std::unexpected("native ability currency unavailable");
  }
  // EAbilityPoints=1, EAbilityUnlocks=2. Each of four abilities costs 1+2+5
  // points to upgrade; native ModifyCurrency owns replication and progression.
  for (const auto [type, target] : {std::pair{1u, 32}, std::pair{2u, 4}}) {
    // Grant each currency short of its target and check that it held.
    const int32_t balance = (*sample->currencies)[type];
    if (balance >= target) {
      continue;
    }
    const auto before = *sample;
    modify(PawnForSlot(slot), type, target - balance, 7, 1, 1, 0, nullptr, nullptr);
    sample = Observe(slot);
    if (!sample || sample->pawn_handle != before.pawn_handle ||
        sample->session_generation != before.session_generation ||
        sample->steam_id != before.steam_id || sample->hero_id != before.hero_id ||
        !sample->currencies || (*sample->currencies)[type] < target) {
      return std::unexpected("native ability grant did not retain pawn and balance");
    }
  }
  return *sample;
}

std::expected<SetMoveType, std::string> ResolvePreparationMovement(const ModuleImage& server) {
  // Movement setup tests FL_FROZEN before clearing buttons and movement axes.
  const auto input = ResolveSignature(server, "preparation.frozen-input");
  if (!input) {
    return std::unexpected(input.error());
  }

  // TakeDamageOld skips damage when m_bTakesDamage is false. The following
  // instructions distinguish this entry gate from its later damage comparison.
  const auto damage = ResolveSignature(server, "preparation.damage-gate");
  if (!damage) {
    return std::unexpected(damage.error());
  }

  // SetMoveType owns replication and the physics-mode transition.
  const auto movement = ResolveSignature(server, "entity.set-move-type");
  if (!movement) {
    return std::unexpected(movement.error());
  }
  return reinterpret_cast<SetMoveType>(*movement);
}

std::expected<void, std::string> PawnObserver::SetPreparationFrozen(int32_t slot, bool frozen) {
#if defined(_WIN32)
  // Drop the freeze of a pawn this player no longer owns.
  const auto sample = Observe(slot);
  if (!sample || !CurrentAbilitiesForSlot(slot)) {
    return std::unexpected("preparation pawn ownership unavailable");
  }
  auto owned = frozen_pawns_.find(slot);
  if (owned != frozen_pawns_.end() &&
      (owned->second.handle != sample->pawn_handle || owned->second.steam_id != sample->steam_id)) {
    frozen_pawns_.erase(owned);
    owned = frozen_pawns_.end();
    // A replacement does not inherit this observer's freeze. Releasing the old
    // ownership is complete without touching flags belonging to the new pawn.
  }

  // Releasing a pawn this observer never froze changes nothing.
  if (!frozen && owned == frozen_pawns_.end()) {
    return {};
  }

  // Resolve the native move type setter once.
  if (!preparation_movement_) {
    const auto server = MappedModuleImage::ForModule(L"server.dll");
    if (!server) {
      return std::unexpected(server.error());
    }
    preparation_movement_ = ResolvePreparationMovement(*server);
  }
  if (!*preparation_movement_) {
    return std::unexpected(preparation_movement_->error());
  }
  const auto set_move_type = **preparation_movement_;

  // Resolve the schema and the damage field at its pinned offset.
  auto schema = ResolveSchemaSystem();
  if (!schema) {
    return std::unexpected(schema.error());
  }
  auto damage = SchemaFieldOf(*schema, "server.dll", "CBaseEntity", "m_bTakesDamage");
  if (!damage || damage->offset != 0x2e0 || damage->size < sizeof(uint8_t)) {
    return std::unexpected("native preparation damage field unavailable");
  }

  // Resolve the flags and movement fields at their pinned offsets.
  auto field = SchemaFieldOf(*schema, "server.dll", "CBaseEntity", "m_fFlags");
  if (!field || field->offset != 0x390 || field->size < sizeof(uint32_t) || !field->networked) {
    return std::unexpected("native preparation flags unavailable");
  }
  auto movement = SchemaFieldOf(*schema, "server.dll", "CBaseEntity", "m_MoveType");
  auto collision = SchemaFieldOf(*schema, "server.dll", "CBaseEntity", "m_MoveCollide");
  if (!movement || movement->offset != 0x2f3 || movement->size < sizeof(uint8_t) || !collision ||
      collision->offset != 0x2f2 || collision->size < sizeof(uint8_t)) {
    return std::unexpected("native preparation movement fields unavailable");
  }

  // Read the pawn's flags and damage state, refusing a freeze another action owns.
  auto* pawn = static_cast<unsigned char*>(PawnForSlot(slot));
  uint32_t flags = 0;
  std::memcpy(&flags, pawn + field->offset, sizeof(flags));
  constexpr uint32_t kFrozen = 1u << 5;
  if (frozen && (flags & kFrozen) && owned == frozen_pawns_.end()) {
    return std::unexpected("pawn already frozen by another engine action");
  }
  const uint8_t takes_damage = pawn[damage->offset];
  if (takes_damage > 1) {
    return std::unexpected("invalid native preparation damage Boolean");
  }

  // Remember the original state of a pawn this freeze takes. A pawn still
  // spawning holds a passing mode, MOVETYPE_NONE or MOVETYPE_PUSH, until the
  // engine gives it walking, so release restores only a player's own modes,
  // flying and noclip, and walks otherwise.
  if (frozen && owned == frozen_pawns_.end()) {
    // Record the flags, damage and movement it held.
    constexpr uint8_t kFlying = 3;
    constexpr uint8_t kNoclip = 7;
    const uint8_t current = pawn[movement->offset];
    const uint8_t move_type =
        current == kFlying || current == kNoclip ? current : MovementHook::kWalking;
    owned = frozen_pawns_
                .emplace(slot, FrozenPawn{sample->pawn_handle, sample->steam_id, takes_damage,
                                          move_type, pawn[collision->offset]})
                .first;
  }

  // Compute the desired flags, damage and movement.
  const uint32_t desired_flags = frozen ? flags | kFrozen : flags & ~kFrozen;
  const uint8_t desired_damage = frozen ? 0 : owned->second.takes_damage;

  // MOVETYPE_NONE prevents preparation from running native walking physics.
  // Keep the original mode across respawns and reconnects of this retained pawn.
  const uint8_t desired_movement = frozen ? 0 : owned->second.move_type;
  if (frozen && flags == desired_flags && takes_damage == desired_damage &&
      pawn[movement->offset] == desired_movement) {
    return {};
  }

  // Write the state, release the ownership on unfreeze, and replicate it.
  std::memcpy(pawn + field->offset, &desired_flags, sizeof(desired_flags));
  pawn[damage->offset] = desired_damage;
  set_move_type(pawn, desired_movement, owned->second.move_collide);
  SetEntityVelocity(pawn, {});
  if (!frozen) {
    frozen_pawns_.erase(owned);
  }
  if (!NotifyEntityStateChanged(pawn)) {
    return std::unexpected("preparation replication unavailable");
  }
  return {};
#else
  return std::unexpected("native preparation control requires Windows");
#endif
}

std::optional<std::array<float, 3>> PawnObserver::CurrentOriginForSlot(int32_t slot) const {
  // Read the located pawn's scene-node origin.
  const auto here = Locate(slot);
  if (!here) {
    return std::nullopt;
  }
  const float* origin = AbsOriginOf(layout_, here->pawn);
  if (origin == nullptr) {
    return std::nullopt;
  }
  return std::array<float, 3>{origin[0], origin[1], origin[2]};
}

std::expected<void, std::string> PawnObserver::SetEyeAngles(void* pawn,
                                                            const std::array<float, 3>& angles) {
  // Write finite angles to the pawn and replicate them.
  if (!pawn || !offsets_ok_) {
    return std::unexpected("pawn aim is unavailable in this frame");
  }
  if (!std::all_of(angles.begin(), angles.end(),
                   [](float angle) { return std::isfinite(angle); })) {
    return std::unexpected("pawn aim must be finite");
  }
  std::memcpy(static_cast<unsigned char*>(pawn) + layout_.eye_angles, angles.data(),
              sizeof(float) * angles.size());
  if (!NotifyEntityStateChanged(pawn)) {
    return std::unexpected("pawn aim replication unavailable");
  }
  return {};
}

}  // namespace modlock::gameinterop
