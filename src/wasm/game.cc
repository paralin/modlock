//go:build deps_only

#include "wasm/game.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <utility>
#include <vector>

#include "modlock/gameinterop/connection_tracker.h"
#include "wasm/game_internal.h"

namespace modlock::wasm {

Game::Game(GameServices& services, EngineHost* engine)
    : services_(services),
      engine_(engine),
      observer_(gameinterop::PawnObserver::LiveSeams(true), true) {
  services_.games_.insert(this);
}

Game::~Game() {
  services_.games_.erase(this);
  if (auto steps = services_.Steps(); !steps) {
    std::cerr << "mod movement: " << steps.error() << '\n';
  }
}

void Game::Frame() {
  // Apply the per-frame state each feature keeps.
  ApplyFreeForAll();
  ApplyHolds();
  HoldClock();
  TickRestores();
  TrackProjectiles();
  PruneAudiences();
  if (services_.effects_) {
    services_.effects_->Sweep(std::chrono::steady_clock::now());
  }

  // Finish the entities spawned this frame together.
  if (!spawned_ || !world_) {
    return;
  }
  world_->FinishSpawns();
  spawned_ = false;
}

void Game::ApplyHolds() {
  std::erase_if(
      held_, [this](const auto& hold) { return !SetModifierState(hold.first, hold.second, true); });
  for (const auto& [slot, key] : held_modifiers_) {
    static_cast<void>(RenewModifier(slot, key));
  }
}

std::expected<void, std::string> Game::RenewModifier(int32_t slot, const std::string& key) {
  // Find the modifier the slot holds.
  const auto modifier = modifiers_.find(key);
  if (modifier == modifiers_.end()) {
    return std::unexpected("the modifier is not held");
  }

  // A dead hero has no modifiers to renew.
  const auto sample = observer_.Observe(slot);
  if (!sample || sample->health <= 0) {
    return {};
  }

  // Attach the modifier again if the game removed it.
  auto attached = modifier->second.Attached(observer_, slot);
  if (!attached) {
    return std::unexpected(attached.error());
  }
  if (*attached) {
    return {};
  }
  const auto ability = std::string_view(key).substr(0, key.find('/'));
  return modifier->second.Attach(observer_, slot, gameinterop::MakeMemberName(ability).hash);
}

void Game::ReleaseModifier(int32_t slot, const std::string& key) {
  // Drop the slot's hold.
  if (held_modifiers_.erase({slot, key}) == 0) {
    return;
  }

  // Restore the modifier once no slot holds it.
  if (std::ranges::any_of(held_modifiers_, [&](const auto& hold) { return hold.second == key; })) {
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
}

std::vector<Game::ButtonChange> Game::TakeButtonChanges() {
  return std::exchange(changes_, {});
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
  static_cast<void>(services_.Selection());
  static_cast<void>(services_.Modifiers());
  static_cast<void>(services_.ModifierState("MODIFIER_STATE_HOLDING_IDOL"));
  observer_.Prepare();
}

void Game::WorldEnding() {
  // Remove the world's objects and forget its entities.
  RemoveWorld();
  world_.reset();
  if (services_.effects_) {
    services_.effects_->InvalidateAfterEngineReset();
  }
  spawned_ = false;

  // Drop the per-world player state.
  frozen_.clear();
  steers_.clear();
  drives_.clear();
  held_.clear();
  ffa_held_.clear();

  // Release what the mod held in the world.
  ReleaseModifiers();
  presses_.clear();
  restores_.clear();
  npc_restore_.reset();
  held_clock_.reset();

  // Forget the paused clock and the observer's cached pawns.
  paused_ = false;
  observer_.Invalidate();
}

void Game::Leave(int32_t slot) {
  // Forget the slot's movement, input and visibility.
  ffa_failed_.erase(slot);
  frozen_.erase(slot);
  movers_.erase(slot);
  steers_.erase(slot);
  drives_.erase(slot);

  // Forget its restores, blocks, presses and hidden objects.
  restores_.erase(slot);
  slot_blocked_.erase(slot);
  presses_.erase(slot);
  hidden_.erase(slot);

  // Whoever takes the slot next is a different player, who was never listed.
  if (gameinterop::IsPlayerSlot(slot)) {
    for (auto& [entity, slots] : audiences_) {
      slots &= ~(uint64_t{1} << slot);
    }
  }

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
}

void Game::Clear() {
  // Stop watching every player.
  for (const auto slot : observer_.WatchingSlots()) {
    players::PlayerRequest request;
    request.set_player(slot);
    static_cast<void>(Unwatch(request));
  }

  // Remove the world's objects and unfreeze the frozen heroes.
  RemoveWorld();
  for (const auto slot : frozen_) {
    if (observer_.Observe(slot)) {
      static_cast<void>(observer_.SetPreparationFrozen(slot, false));
    }
  }

  // Stop moving and steering heroes.
  frozen_.clear();
  movers_.clear();
  steers_.clear();
  drives_.clear();

  // Releasing the movement hook cannot fail.
  static_cast<void>(services_.Steps());

  // Release held modifier states and the free-for-all.
  for (const auto& [entity, state] : held_) {
    static_cast<void>(SetModifierState(entity, state, false));
  }
  held_.clear();
  free_for_all_ = false;
  ffa_held_.clear();
  ffa_failed_.clear();
  ReleaseModifiers();

  // Release the input the mod blocked, remapped or watched.
  blocked_ = 0;
  slot_blocked_.clear();
  remap_.Clear();
  presses_.clear();
  watched_ = 0;
  changes_.clear();

  // Drop pending restores and their reports.
  restores_.clear();
  restored_.clear();
  npc_restore_.reset();
  npcs_restored_.reset();

  // Resume the clock and stop watching projectiles.
  held_clock_.reset();
  if (paused_) {
    static_cast<void>(SetPaused(false));
  }
  projectiles_.Clear();
}

void Game::RemoveWorld() {
  // Remove the text, effect and fog objects.
  for (auto& [id, object] : objects_) {
    std::visit([](auto& handle) { handle->Remove(); }, object);
  }
  objects_.clear();
  hidden_.clear();
  if (fog_) {
    fog_->Remove();
    fog_.reset();
  }

  // Remove the entities the mod created.
  if (world_) {
    for (const auto entity : created_) {
      static_cast<void>(world_->RemoveEntity(entity));
    }
  }
  created_.clear();
  audiences_.clear();

  // Only the engine's kick removes a bot's connection.
  if (auto server = services_.Server()) {
    for (const auto& [slot, name] : bots_) {
      if ((*server)->ServerCommand(("kick \"" + name + "\"\n").c_str())) {
        Depart(slot);
      }
    }
  }
  bots_.clear();
}

void Game::Depart(int32_t slot) {
  departing_[slot] = gameinterop::ConnectionTracker::StateForSlot(slot).generation;
}

bool Game::Departing() {
  std::erase_if(departing_, [](const auto& entry) {
    const auto state = gameinterop::ConnectionTracker::StateForSlot(entry.first);
    return !state.occupied || state.generation != entry.second;
  });
  return !departing_.empty();
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

std::expected<players::PlayersResponse, std::string> Game::Players() {
  players::PlayersResponse players;
  for (int32_t slot = 0; slot < gameinterop::kMaxPlayerSlots; ++slot) {
    // List each occupied slot.
    const auto state = gameinterop::ConnectionTracker::StateForSlot(slot);
    if (!state.occupied) {
      continue;
    }

    // With its identity and readiness.
    auto* player = players.add_players();
    player->set_player(slot);
    player->set_steam_id(state.is_bot ? 0 : state.xuid);
    player->set_name(state.name);
    player->set_bot(state.is_bot);
    player->set_ready(state.is_bot || state.fully_connected);
    player->set_generation(state.generation);
  }
  return players;
}

std::expected<players::PawnResponse, std::string> Game::Pawn(
    const players::PlayerRequest& request) {
  // Sample the player's pawn; a slot without one answers no pawn.
  players::PawnResponse response;
  const auto sample = observer_.Observe(request.player());
  if (!sample) {
    return response;
  }

  // Copy the pawn's identity and health.
  auto* pawn = response.mutable_pawn();
  pawn->set_entity(sample->pawn_handle);
  pawn->set_hero(sample->hero_id);
  pawn->set_team(sample->team);
  pawn->set_health(sample->health);
  pawn->set_max_health(sample->max_health);

  // Copy the pawn's position and view.
  pawn->mutable_position()->set_x(sample->x);
  pawn->mutable_position()->set_y(sample->y);
  pawn->mutable_position()->set_z(sample->z);
  SetAngles(pawn->mutable_eye_angles(), sample->eye_angles);
  SetAngles(pawn->mutable_camera_angles(), sample->camera_angles);

  // Copy the optional readings the sample carries.
  if (sample->stamina) {
    pawn->set_stamina(sample->stamina->current);
    pawn->set_max_stamina(sample->stamina->max);
  }
  pawn->set_generation(sample->session_generation);
  if (sample->currencies) {
    pawn->set_souls((*sample->currencies)[0]);
  }

  // Copy the pawn's eye and velocity when the sample has them.
  if (sample->eye_position) {
    SetVector(pawn->mutable_eye_position(), *sample->eye_position);
  }
  if (const auto camera = services_.CameraPosition(request.player())) {
    SetVector(pawn->mutable_camera_position(), *camera);
  }
  if (sample->movement && sample->movement->abs_velocity) {
    SetVector(pawn->mutable_velocity(), *sample->movement->abs_velocity);
  }

  // Copy the hero's scoreboard line.
  if (sample->score) {
    // Copy the scoreboard.
    auto* score = pawn->mutable_score();
    score->set_kills(sample->score->kills);
    score->set_deaths(sample->score->deaths);
    score->set_assists(sample->score->assists);
    score->set_last_hits(sample->score->last_hits);

    // Then denies, damage, healing and net worth.
    score->set_denies(sample->score->denies);
    score->set_hero_damage(sample->score->hero_damage);
    score->set_hero_healing(sample->score->hero_healing);
    score->set_net_worth(sample->score->net_worth);
  }

  // The game marks the hero carrying the urn with its holding-idol state.
  abilities::ModifierStateRequest holding;
  holding.set_entity(sample->pawn_handle);
  holding.set_state("MODIFIER_STATE_HOLDING_IDOL");
  if (const auto carrying = ModifierState(holding)) {
    pawn->set_carrying_urn(carrying->active());
  }
  return response;
}

std::expected<gameinterop::WorldEntities*, std::string> Game::World() {
  if (!world_) {
    // Resolve the world entities once.
    auto image = services_.Image();
    if (!image) {
      return std::unexpected(image.error());
    }
    auto schema = services_.Schema();
    if (!schema) {
      return std::unexpected(schema.error());
    }
    auto world = gameinterop::WorldEntities::Resolve(**image, *schema);
    if (!world) {
      return std::unexpected(world.error());
    }
    world_ = std::move(*world);
  }
  return &*world_;
}

std::expected<gameinterop::PawnObserver::Sample, std::string> Game::Live(int32_t slot) {
  auto sample = observer_.Observe(slot);
  if (!sample) {
    return std::unexpected("the player has no live hero");
  }
  return *sample;
}

std::expected<void*, std::string> Game::Entity(uint32_t handle) const {
  // Find the live entity with that handle.
  auto entities = gameinterop::ResolveLiveEntitySystem();
  if (!entities) {
    return std::unexpected(entities.error());
  }
  auto* entity = gameinterop::EntityInstance(*entities, handle);
  if (entity == nullptr) {
    return std::unexpected("no live entity has that handle");
  }
  return entity;
}

objects::ObjectResponse Game::Keep(Object object) {
  // Store the object under the next id and return the id.
  const auto id = next_object_++;
  objects_.emplace(id, std::move(object));
  objects::ObjectResponse response;
  response.set_object(id);
  return response;
}

}  // namespace modlock::wasm
