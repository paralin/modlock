//go:build deps_only

#include "wasm/game.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <ranges>
#include <utility>
#include <vector>

#include "modlock/gameinterop/connection_tracker.h"
#include "wasm/game_internal.h"
#include "wasm/modifier_masks.h"

namespace modlock::wasm {

Game::Game(GameServices& services, EngineHost* engine) : services_(services), engine_(engine) {
  // Register with the host-wide hooks; the destructor removes each before
  // the movement hook rechecks its movers.
  services_.InputSources().Add(&input_);
  services_.MoverSources().Add(this);
  services_.CameraSources().Add(this);
  services_.ProjectileSources().Add(this);
  services_.VoiceSources().Add(this);
}

Game::~Game() {
  services_.InputSources().Remove(&input_);
  services_.MoverSources().Remove(this);
  services_.CameraSources().Remove(this);
  services_.ProjectileSources().Remove(this);
  services_.VoiceSources().Remove(this);
  services_.Controllers().Release(this);
  if (auto steps = services_.Steps(); !steps) {
    std::cerr << "mod movement: " << steps.error() << '\n';
  }
}

void Game::Frame() {
  // Apply the per-frame state each feature keeps.
  SeatPlayers();
  ApplyFreeForAll();
  ApplyHolds();
  TickRestores();

  // Follow what moved or left since the last frame, then run the modules,
  // so the world finishes the units spawned this frame together.
  TrackProjectiles();
  for (auto* module : std::views::reverse(modules_)) {
    module->Frame();
  }
}

void Game::ApplyHolds() {
  // Set each held state again and drop the ones that no longer apply.
  std::erase_if(held_, [this](const auto& hold) {
    return !SetModifierState(services_, hold.first, hold.second, true);
  });
  for (const auto& [slot, key] : held_modifiers_) {
    static_cast<void>(RenewModifier(slot, key));
  }

  // Renew each entity's modifiers and release those of an entity that is gone.
  std::vector<std::pair<uint32_t, std::string>> gone;
  for (const auto& [entity, key] : held_entity_modifiers_) {
    if (!LiveEntity(entity)) {
      gone.emplace_back(entity, key);
      continue;
    }
    static_cast<void>(RenewEntityModifier(entity, key, false));
  }
  for (const auto& [entity, key] : gone) {
    ReleaseEntityModifier(entity, key);
  }
}

std::expected<void, std::string> Game::RenewModifier(int32_t slot, const std::string& key) {
  // Find the modifier the slot holds.
  const auto modifier = modifiers_.find(key);
  if (modifier == modifiers_.end()) {
    return std::unexpected("the modifier is not held");
  }

  // A dead hero has no modifiers to renew.
  const auto sample = players_.Observer().Observe(slot);
  if (!sample || sample->health <= 0) {
    return {};
  }

  // Attach the modifier again if the game removed it.
  auto attached = modifier->second.Attached(players_.Observer(), slot);
  if (!attached) {
    return std::unexpected(attached.error());
  }
  if (*attached) {
    return {};
  }
  auto source = ModifierSource(slot, key);
  if (!source) {
    return std::unexpected(source.error());
  }
  return modifier->second.Attach(players_.Observer(), slot, *source);
}

std::expected<void, std::string> Game::RenewEntityModifier(uint32_t handle, const std::string& key,
                                                           bool initial) {
  // Find the held modifier and the live entity.
  const auto modifier = modifiers_.find(key);
  if (modifier == modifiers_.end()) {
    return std::unexpected("the modifier is not held");
  }
  auto entity = LiveEntity(handle);
  if (!entity) {
    return std::unexpected(entity.error());
  }
  if (!modifier->second.EntityAlive(*entity)) {
    if (initial) {
      return std::unexpected("the entity is not alive");
    }
    return {};
  }

  // Attach the modifier again if the game removed it.
  auto attached = modifier->second.AttachedToEntity(*entity);
  if (!attached) {
    return std::unexpected(attached.error());
  }
  if (*attached) {
    return {};
  }
  return modifier->second.AttachEntity(*entity, EntityAbilityHandle(handle, key));
}

void Game::ReleaseModifier(int32_t slot, const std::string& key) {
  if (held_modifiers_.erase({slot, key}) != 0) {
    RestoreUnusedModifier(key);
  }
}

void Game::ReleaseEntityModifier(uint32_t handle, const std::string& key) {
  if (held_entity_modifiers_.erase({handle, key}) != 0) {
    RestoreUnusedModifier(key);
  }
}

void Game::RestoreUnusedModifier(const std::string& key) {
  if (std::ranges::any_of(held_modifiers_, [&](const auto& hold) { return hold.second == key; }) ||
      std::ranges::any_of(held_entity_modifiers_,
                          [&](const auto& hold) { return hold.second == key; })) {
    return;
  }
  if (auto modifier = modifiers_.find(key); modifier != modifiers_.end()) {
    modifier->second.Restore();
    modifiers_.erase(modifier);
  }
}

void Game::ReleaseModifiers() {
  for (const auto& [key, modifier] : modifiers_) {
    modifier.Restore();
  }
  modifiers_.clear();
  held_modifiers_.clear();
  held_entity_modifiers_.clear();
}

std::vector<Game::ButtonChange> Game::TakeButtonChanges() {
  return input_.TakeChanges();
}

std::vector<Game::Restored> Game::TakeRestored() {
  return std::exchange(restored_, {});
}

std::optional<std::string> Game::TakeNpcsRestored() {
  return std::exchange(npcs_restored_, {});
}

void Game::WorldReady() {
  // Each lookup is kept once found; one that fails here is reported by the
  // call that needs it.
  static_cast<void>(services_.Heroes());
  static_cast<void>(services_.WorldCalls());
  static_cast<void>(services_.Selection());
  static_cast<void>(services_.Modifiers());

  // Find every recorded server function now. Each is a scan of the whole
  // module, which would otherwise run inside the first call that needs it.
  if (const auto image = services_.Image()) {
    gameinterop::ResolveModuleSignatures(**image, gameinterop::GameModule::kServer);
  }
  for (auto* module : modules_) {
    module->WorldReady();
  }
}

void Game::WorldEnding() {
  // Drop the per-world player state and reapply host seats on the next map.
  assignments_.WorldEnding();
  frozen_.clear();
  steers_.clear();
  drives_.clear();
  held_.clear();
  ffa_held_.clear();

  // Release what the mod held in the world.
  ReleaseModifiers();
  input_.DropPresses();
  restores_.clear();
  npc_restore_.reset();
  for (auto* module : std::views::reverse(modules_)) {
    module->WorldEnding();
  }
}

void Game::Leave(int32_t slot) {
  // Forget the slot's movement, controllers and input.
  seat_failed_.erase(slot);
  frozen_.erase(slot);
  movers_.erase(slot);
  services_.Controllers().Watch(this, slot, false);
  steers_.erase(slot);
  drives_.erase(slot);

  // Forget its restores, blocks, presses, silences and what it was shown.
  restores_.erase(slot);
  picks_.Forget(slot);
  input_.Leave(slot);
  silenced_players_.erase(slot);

  // Release the modifiers the slot held.
  std::vector<std::string> keys;
  for (const auto& [held, key] : held_modifiers_) {
    if (held == slot) {
      keys.push_back(key);
    }
  }
  for (const auto& key : keys) {
    ReleaseModifier(slot, key);
  }
  for (auto* module : std::views::reverse(modules_)) {
    module->Leave(slot);
  }
}

void Game::Clear() {
  // Stop watching every player.
  for (const auto slot : players_.Observer().WatchingSlots()) {
    players::PlayerRequest request;
    request.set_player(slot);
    static_cast<void>(Unwatch(request));
  }
  watched_.clear();

  // Unfreeze the frozen heroes.
  for (const auto slot : frozen_) {
    if (players_.Observer().Observe(slot)) {
      static_cast<void>(players_.Observer().SetPreparationFrozen(slot, false));
    }
  }

  // Stop moving and steering heroes and watching controllers.
  frozen_.clear();
  movers_.clear();
  services_.Controllers().Release(this);
  steers_.clear();
  drives_.clear();

  // Releasing the movement hook cannot fail.
  static_cast<void>(services_.Steps());

  // Release held modifier states, the free-for-all and team seating.
  for (const auto& [entity, state] : held_) {
    static_cast<void>(SetModifierState(services_, entity, state, false));
  }
  held_.clear();
  free_for_all_ = false;
  team_seating_ = false;
  ReleaseFreeForAll();
  // Remove active held modifiers from live entities when the mod stops, so
  // infinite modifiers (such as camera-override modifiers) do not outlive it.
  if (auto* world = world_.Loaded()) {
    auto modifier_name = [](std::string_view key) -> std::string_view {
      const auto slash = key.find('/');
      return slash == std::string_view::npos ? key : key.substr(slash + 1);
    };
    for (const auto& [slot, key] : held_modifiers_) {
      if (const auto sample = players_.Observer().Observe(slot); sample && sample->pawn_handle) {
        static_cast<void>(world->FireInput(sample->pawn_handle, "RemoveModifier",
                                           gameinterop::EntityValue{modifier_name(key)},
                                           std::nullopt));
      }
    }
    for (const auto& [entity, key] : held_entity_modifiers_) {
      static_cast<void>(world->FireInput(
          entity, "RemoveModifier", gameinterop::EntityValue{modifier_name(key)}, std::nullopt));
    }
  }
  ReleaseModifiers();

  // Return every ability's cooldown lengths and most charges to the game's
  // own.
  cooldown_.reset();
  charge_limit_.reset();

  // Release the input the mod blocked, remapped or watched, and the voices
  // it silenced.
  input_.Clear();
  silenced_.clear();
  silenced_players_.clear();

  // Drop pending restores and their reports.
  restores_.clear();
  restored_.clear();
  npc_restore_.reset();
  npcs_restored_.reset();

  // Stop watching projectiles, then release what each module holds.
  projectiles_.Clear();
  for (auto* module : std::views::reverse(modules_)) {
    module->Clear();
  }
}

std::expected<void, std::string> Game::Pause(const wasm::PauseRequest& request) {
  return match_.Pause(request);
}

std::expected<wasm::MatchClockResponse, std::string> Game::MatchClock() {
  return match_.MatchClock();
}

std::expected<void, std::string> Game::HoldMatchClock(const wasm::HoldMatchClockRequest& request) {
  return match_.HoldMatchClock(request);
}

std::expected<wasm::RiftResponse, std::string> Game::Rift() {
  return match_.Rift();
}

std::expected<void, std::string> Game::StartRift(const wasm::StartRiftRequest& request) {
  return match_.StartRift(request);
}

std::expected<void, std::string> Game::ServerCommand(const ServerCommandRequest& request) {
  return services_.ServerCommand(request.command());
}

std::expected<void, std::string> Game::Chat(const effects::ChatRequest& request) {
  auto messages = services_.Messages();
  if (!messages) {
    return std::unexpected(messages.error());
  }
  return (*messages)->Chat(request.player(), request.text());
}

std::expected<void, std::string> Game::CenterText(const effects::CenterTextRequest& request) {
  auto messages = services_.Messages();
  if (!messages) {
    return std::unexpected(messages.error());
  }
  return (*messages)->CenterText(request.player(), request.text());
}

std::expected<void, std::string> Game::Announce(const effects::AnnounceRequest& request) {
  auto messages = services_.Messages();
  if (!messages) {
    return std::unexpected(messages.error());
  }
  return (*messages)->Announce(request.player(), request.title(), request.text());
}

}  // namespace modlock::wasm
