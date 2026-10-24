#include <cstdio>
#include <cstring>

#include "modlock/gameinterop/entity_abi.h"
#include "modlock/gameinterop/native_damage.h"
#include "modlock/gameinterop/pawn_observer.h"
#include "modlock/gameinterop/virtual_slots.h"
#include "pawn_observer_internal.h"

namespace modlock::gameinterop {

std::expected<std::pair<unsigned char*, unsigned char*>, std::string>
PawnObserver::ControllerOfSlot(int32_t slot) {
  // Check the slot and the layout.
  if (!IsPlayerSlot(slot) || !seams_.entity_system) {
    return std::unexpected("player controller unavailable");
  }
  if (auto resolved = ResolveLayout(); !resolved) {
    return std::unexpected(resolved.error());
  }
  if (!layout_.hero_pawn_handle) {
    return std::unexpected("native hero pawn handle unavailable");
  }

  // Find the controller at index slot + 1.
  auto system = seams_.entity_system();
  if (!system) {
    return std::unexpected(system.error());
  }
  auto* list = EntityListOf(*system);
  auto* identity = IdentityOfIndex(list, slot + 1);
  auto* controller = identity ? static_cast<unsigned char*>(InstanceOf(identity)) : nullptr;
  if (!controller) {
    return std::unexpected("player controller unavailable");
  }
  return std::pair{controller, list};
}

std::expected<void*, std::string> PawnObserver::CameraPawnForSlot(
    int32_t slot, const PlayerSelectionCalls& calls) {
  // Resolve the player's controller.
  auto current = ControllerOfSlot(slot);
  if (!current) {
    return std::unexpected(current.error());
  }

  // Read its active pawn and its hero.
  auto [controller, list] = *current;
  uint32_t handle = 0;
  uint32_t hero = 0;
  std::memcpy(&handle, controller + layout_.pawn_handle, sizeof(handle));
  std::memcpy(&hero, controller + *layout_.hero_pawn_handle, sizeof(hero));
  auto* identity = IdentityOfHandle(list, handle);
  void* pawn = identity ? InstanceOf(identity) : nullptr;

  // A spectator's pawn is its camera, and so is any pawn other than the hero:
  // an earlier watch's camera or the game's own camera after the hero died.
  if (controller[layout_.team] == 1 || (pawn && handle != hero)) {
    if (!pawn) {
      return std::unexpected("the player's camera is not ready");
    }
    return pawn;
  }

  // A player with a hero watches through a camera of its own while the hero
  // stays where it stands, as the game binds one when the hero dies. Record
  // whatever camera the spawn bound, even one the watch then refuses, so
  // ReturnToHero removes it.
  if (!calls.spawn_observer) {
    return std::unexpected("native camera unavailable");
  }
  void* created = calls.spawn_observer(controller);
  current = ControllerOfSlot(slot);
  if (!current) {
    return std::unexpected(current.error());
  }

  // Record the camera now bound, and accept it only if the spawn made it.
  std::memcpy(&handle, current->first + layout_.pawn_handle, sizeof(handle));
  identity = IdentityOfHandle(current->second, handle);
  pawn = identity ? InstanceOf(identity) : nullptr;
  if (pawn && handle != hero) {
    cameras_.insert_or_assign(slot, handle);
  }
  if (!created || pawn != created) {
    return std::unexpected("the player's camera is not ready");
  }
  return pawn;
}

std::expected<uint32_t, std::string> PawnObserver::ReturnToHero(int32_t slot,
                                                                const PlayerSelectionCalls& calls) {
  // The controller's binding decides, not the record: a camera bound by an
  // earlier connection in the slot, or one whose watch failed partway, still
  // returns to a living hero.
  const auto camera = cameras_.find(slot);
  const uint32_t owned = camera == cameras_.end() ? 0 : camera->second;
  auto current = ControllerOfSlot(slot);
  if (!current) {
    cameras_.erase(slot);
    return std::unexpected(current.error());
  }

  // Read the player's active pawn and hero.
  auto [controller, list] = *current;
  uint32_t handle = 0;
  uint32_t hero = 0;
  std::memcpy(&handle, controller + layout_.pawn_handle, sizeof(handle));
  std::memcpy(&hero, controller + *layout_.hero_pawn_handle, sizeof(hero));

  // Read the hero's health.
  auto* identity = IdentityOfHandle(list, hero);
  void* pawn = identity ? InstanceOf(identity) : nullptr;
  int32_t health = 0;
  if (pawn) {
    std::memcpy(&health, static_cast<const char*>(pawn) + layout_.health, sizeof(health));
  }

  // A spectator has no hero to return to, and a dead hero's respawn returns
  // the player once the game binds it; until then the camera stays bound.
  if (controller[layout_.team] == 1 || handle == hero || !pawn || health <= 0) {
    if (owned == handle && handle != hero) {
      return 0;
    }
    cameras_.erase(slot);
    return owned;
  }
  if (!calls.set_pawn) {
    return std::unexpected("the player's hero is not ready");
  }
  calls.set_pawn(controller, pawn, false, false, false, false);
  cameras_.erase(slot);
  return handle;
}

std::vector<int32_t> PawnObserver::WatchingSlots() const {
  std::vector<int32_t> slots;
  for (const auto& [slot, camera] : cameras_) {
    slots.push_back(slot);
  }
  return slots;
}

std::expected<void, std::string> PawnObserver::SelectPlayer(int32_t slot, uint64_t steam_id,
                                                            uint32_t generation, int32_t team,
                                                            void* definition,
                                                            const PlayerSelectionCalls& calls) {
  // Check the request and the layout.
  if (!IsPlayerSlot(slot) || !steam_id || team < 1 || team > 3 ||
      (team != 1 && (!definition || !calls.create_pawn || !calls.select_hero)) ||
      (team == 1 && (!calls.spawn_observer || !calls.set_pawn)) || !seams_.slot_state ||
      !seams_.entity_system) {
    return std::unexpected("player selection unavailable");
  }
  if (auto resolved = ResolveLayout(); !resolved) {
    return resolved;
  }
  if (!layout_.hero_pawn_handle) {
    return std::unexpected("native hero pawn handle unavailable");
  }

  // Borrow the controller again after each native call, and stop if the player changed.
  void* expected_controller = nullptr;
  auto borrow = [&]() -> std::expected<std::pair<void*, void*>, std::string> {
    // The connection must still be the same player's.
    const auto state = seams_.slot_state(slot);
    if (!state.occupied || state.is_bot || state.xuid != steam_id ||
        state.generation != generation) {
      return std::unexpected("player connection changed during selection");
    }

    // Find the controller, which must be the one seen before.
    auto system = seams_.entity_system();
    if (!system) {
      return std::unexpected(system.error());
    }
    auto* identity = IdentityOfIndex(EntityListOf(*system), slot + 1);
    if (!identity) {
      return std::unexpected("player controller unavailable");
    }
    void* controller = InstanceOf(identity);
    if (expected_controller && controller != expected_controller) {
      return std::unexpected("player controller changed during selection");
    }
    expected_controller = controller;
    return std::pair{controller, *system};
  };

  // Find the controller's team change.
  auto current = borrow();
  if (!current) {
    return std::unexpected(current.error());
  }
  const auto change_team = ResolveVirtualSlot("player-controller.change-team");
  if (!change_team) {
    return std::unexpected(change_team.error());
  }
  auto* table = *static_cast<void***>(current->first);
  if (!table || !table[*change_team]) {
    return std::unexpected("native team selection unavailable");
  }

  // Move to the spectators, or to the new team.
  uint8_t current_team = 0;
  std::memcpy(&current_team, static_cast<const char*>(current->first) + layout_.team, 1);
  if (current_team != team && team == 1) {
    // The game's own move to the spectators releases the hero before the team
    // changes: it clears the hero handle and unbinds the pawn. A client whose
    // hero stays bound reads its hero's data after the change and crashes.
    auto* controller = static_cast<unsigned char*>(current->first);
    uint32_t hero = 0;
    std::memcpy(&hero, controller + *layout_.hero_pawn_handle, sizeof(hero));
    if (IdentityOfHandle(EntityListOf(current->second), hero)) {
      hero = kInvalidEHandleIndex;
      std::memcpy(controller + *layout_.hero_pawn_handle, &hero, sizeof(hero));
      if (!NotifyEntityStateChanged(controller)) {
        return std::unexpected("player hero release unavailable");
      }
    }
    calls.set_pawn(controller, nullptr, true, false, false, false);
    current = borrow();
    if (!current) {
      return std::unexpected(current.error());
    }
  }
  if (current_team != team) {
    reinterpret_cast<void (*)(void*, int)>(table[*change_team])(current->first, team);
    current = borrow();
    if (!current) {
      return std::unexpected(current.error());
    }
  }

  // Find the pawn, creating it when the player has none.
  const size_t pawn_field = team == 1 ? layout_.pawn_handle : *layout_.hero_pawn_handle;
  uint32_t handle = 0;
  std::memcpy(&handle, static_cast<const char*>(current->first) + pawn_field, sizeof(handle));
  auto* identity = IdentityOfHandle(EntityListOf(current->second), handle);
  void* pawn = identity ? InstanceOf(identity) : nullptr;
  if (!pawn) {
    // Wait for a pawn this selection already asked for.
    const auto pending = pending_selections_.find(slot);
    if (pending != pending_selections_.end() && pending->second.steam_id == steam_id &&
        pending->second.generation == generation && pending->second.team == team) {
      return std::unexpected("native pawn creation awaiting controller publication");
    }

    // Create the pawn, or an observer for a spectator.
    void* created =
        team == 1 ? calls.spawn_observer(current->first) : calls.create_pawn(current->first, team);
    current = borrow();
    if (!current) {
      return std::unexpected(current.error());
    }
    if (!created) {
      return std::unexpected("native pawn creation failed");
    }
    pending_selections_.insert_or_assign(slot, PendingSelection{steam_id, generation, team});

    // Reborrow through the controller before writing to the created pawn. A
    // returned object alone does not prove that it is still this player's pawn.
    std::memcpy(&handle, static_cast<const char*>(current->first) + pawn_field, sizeof(handle));
    identity = IdentityOfHandle(EntityListOf(current->second), handle);
    pawn = identity ? InstanceOf(identity) : nullptr;
    if (!pawn) {
      return std::unexpected("native pawn creation awaiting controller publication");
    }
    if (pawn != created) {
      return std::unexpected("player pawn changed during creation");
    }
  }

  // Select the hero, then confirm the player is unchanged.
  pending_selections_.erase(slot);
  if (team != 1) {
    calls.select_hero(pawn, definition);
  }
  current = borrow();
  if (!current) {
    return std::unexpected(current.error());
  }
  return {};
}

std::optional<uint8_t> PawnObserver::ControllerTeam(int32_t slot) {
  const auto current = ControllerOfSlot(slot);
  if (!current) {
    return std::nullopt;
  }
  return current->first[layout_.team];
}

std::expected<bool, std::string> PawnObserver::SeatForChoice(int32_t slot, uint64_t steam_id,
                                                             uint32_t generation, int32_t team,
                                                             const PlayerSelectionCalls& calls) {
  // Check the request, the layout and the team change.
  if (!IsPlayerSlot(slot) || !steam_id || team < 2 || team > 3 || !calls.create_pawn ||
      !seams_.slot_state || !seams_.entity_system) {
    return std::unexpected("player seating unavailable");
  }
  if (auto resolved = ResolveLayout(); !resolved) {
    return std::unexpected(resolved.error());
  }
  if (!layout_.hero_pawn_handle) {
    return std::unexpected("native hero pawn handle unavailable");
  }
  if (!layout_.play_state) {
    return std::unexpected("native play state unavailable");
  }
  const auto change_team = ResolveVirtualSlot("player-controller.change-team");
  if (!change_team) {
    return std::unexpected(change_team.error());
  }

  // Borrow the controller again after each native call, and stop if the player changed.
  void* expected_controller = nullptr;
  auto borrow = [&]() -> std::expected<unsigned char*, std::string> {
    // The connection must still be the same player's.
    const auto state = seams_.slot_state(slot);
    if (!state.occupied || state.is_bot || state.xuid != steam_id ||
        state.generation != generation) {
      return std::unexpected("player connection changed during seating");
    }

    // Find the controller, which must be the one seen before.
    auto system = seams_.entity_system();
    if (!system) {
      return std::unexpected(system.error());
    }
    auto* identity = IdentityOfIndex(EntityListOf(*system), slot + 1);
    void* controller = identity ? InstanceOf(identity) : nullptr;
    if (!controller) {
      return std::unexpected("player controller unavailable");
    }
    if (expected_controller && controller != expected_controller) {
      return std::unexpected("player controller changed during seating");
    }
    expected_controller = controller;
    return static_cast<unsigned char*>(controller);
  };

  // Seat only a player who has not yet joined a team.
  auto controller = borrow();
  if (!controller) {
    return std::unexpected(controller.error());
  }
  if ((*controller)[layout_.team] != 0) {
    return false;
  }
  auto* table = *reinterpret_cast<void***>(*controller);
  if (!table || !table[*change_team]) {
    return std::unexpected("native team selection unavailable");
  }

  // The game's own join runs these three steps in order: ChangeTeam, the hero
  // pawn, then the play state. The state replicates to the client, which opens
  // the hero picker in place of the team screen.
  reinterpret_cast<void (*)(void*, int)>(table[*change_team])(*controller, team);
  controller = borrow();
  if (!controller) {
    return std::unexpected(controller.error());
  }

  // Create the hero pawn unless the controller holds one.
  uint32_t hero = 0;
  std::memcpy(&hero, *controller + *layout_.hero_pawn_handle, sizeof(hero));
  auto system = seams_.entity_system();
  if (!system) {
    return std::unexpected(system.error());
  }
  if (!IdentityOfHandle(EntityListOf(*system), hero)) {
    calls.create_pawn(*controller, team);
    controller = borrow();
    if (!controller) {
      return std::unexpected(controller.error());
    }
  }

  // Mark the player as choosing a hero.
  constexpr int32_t kChoosingHero = 2;  // EPlayerPlayState::EPlayerChoosingHero
  std::memcpy(*controller + *layout_.play_state, &kChoosingHero, sizeof(kChoosingHero));
  if (!NotifyEntityStateChanged(*controller)) {
    return std::unexpected("player play state did not replicate");
  }
  return true;
}

std::expected<void, std::string> PawnObserver::RespawnPlayer(int32_t slot, uint32_t generation,
                                                             RespawnPawn respawn) {
  // Check the request and the connection.
  if (!IsPlayerSlot(slot) || !respawn || !seams_.slot_state || !seams_.entity_system) {
    return std::unexpected("native player respawn unavailable");
  }
  const auto connection = seams_.slot_state(slot);
  if (!connection.occupied || connection.generation != generation) {
    return std::unexpected("player connection changed before respawn");
  }
  if (auto resolved = ResolveLayout(); !resolved) {
    return resolved;
  }
  if (!layout_.hero_pawn_handle) {
    return std::unexpected("native hero pawn handle unavailable");
  }

  // Find the player's controller.
  auto system = seams_.entity_system();
  if (!system) {
    return std::unexpected(system.error());
  }
  auto* controller_identity = IdentityOfIndex(EntityListOf(*system), slot + 1);
  auto* controller = controller_identity ? InstanceOf(controller_identity) : nullptr;
  if (!controller) {
    return std::unexpected("player controller unavailable");
  }

  // Find its hero pawn.
  uint32_t handle = 0;
  std::memcpy(&handle, static_cast<const char*>(controller) + *layout_.hero_pawn_handle,
              sizeof(handle));
  auto* identity = IdentityOfHandle(EntityListOf(*system), handle);
  auto* pawn = identity ? InstanceOf(identity) : nullptr;
  if (!pawn) {
    return std::unexpected("player hero pawn unavailable");
  }

  // Respawn a dead hero.
  int32_t health = 0;
  std::memcpy(&health, static_cast<const char*>(pawn) + layout_.health, sizeof(health));
  if (health <= 0) {
    std::fprintf(stderr,
                 "[modlock] player respawn requested: slot=%d generation=%u hero_handle=%u\n", slot,
                 generation, handle);
    respawn(pawn, true);
  }
  return {};
}

std::expected<void, std::string> PawnObserver::EliminatePlayer(int32_t slot, uint32_t generation,
                                                               const NativeDamage& damage) {
  // Kill the hero of this connection and confirm it died.
  const auto sample = Observe(slot);
  if (!sample || sample->session_generation != generation) {
    return std::unexpected("elimination player observation is absent or stale");
  }
  auto killed = damage.Kill(PawnForSlot(slot));
  if (!killed) {
    return killed;
  }
  const auto after = Observe(slot);
  if (!after || after->session_generation != generation || after->health > 0) {
    return std::unexpected("native elimination did not produce an observed dead pawn");
  }
  return {};
}

}  // namespace modlock::gameinterop
