//go:build deps_only

#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <span>
#include <utility>
#include <vector>

#include "wasm/game.h"
#include "wasm/game_internal.h"

namespace modlock::wasm {

namespace {

// Watches reports whether options name a projectile.
bool Watches(const combat::ProjectileOptions& options, std::string_view name) {
  return !name.empty() && std::ranges::find(options.names(), name) != options.names().end();
}

}  // namespace

std::expected<void, std::string> Game::MoveEntity(const entities::MoveEntityRequest& request) {
  // Move the entity, keeping each part of its pose unless given.
  auto entity = Entity(request.entity());
  if (!entity) {
    return std::unexpected(entity.error());
  }
  const auto position = Floats(request.position());
  const auto facing = Floats(request.facing());
  const auto velocity = Floats(request.velocity());
  gameinterop::TeleportEntity(*entity, request.has_position() ? position.data() : nullptr,
                              request.has_facing() ? facing.data() : nullptr,
                              request.has_velocity() ? velocity.data() : nullptr);
  return {};
}

std::expected<void, std::string> Game::PushEntity(const entities::PushEntityRequest& request) {
  // Add the motion through the engine's impulse calls.
  auto dynamics = services_.Dynamics();
  if (!dynamics) {
    return std::unexpected(dynamics.error());
  }
  auto entity = Entity(request.entity());
  if (!entity) {
    return std::unexpected(entity.error());
  }
  const auto velocity = Floats(request.velocity());
  (*dynamics)->push(*entity, velocity.data());
  if (request.has_spin()) {
    const auto spin = Floats(request.spin());
    (*dynamics)->spin(*entity, spin.data());
  }
  return {};
}

std::expected<void, std::string> Game::EmitSound(const effects::EmitSoundRequest& request) {
  // Play the sound on the entity.
  auto sound = services_.Sound();
  if (!sound) {
    return std::unexpected(sound.error());
  }
  auto entity = Entity(request.entity());
  if (!entity) {
    return std::unexpected(entity.error());
  }
  if (!(*sound)->Emit(*entity, request.sound().c_str())) {
    return std::unexpected("the game did not play " + request.sound());
  }
  return {};
}

std::expected<void, std::string> Game::Kill(const players::PlayerRequest& request) {
  // Kill the live player's hero unless it is already dead.
  auto damage = services_.Damage();
  if (!damage) {
    return std::unexpected(damage.error());
  }
  auto sample = Live(request.player());
  if (!sample) {
    return std::unexpected(sample.error());
  }
  if (sample->health <= 0) {
    return {};
  }
  return (*damage)->Kill(sample->pawn);
}

std::expected<void, std::string> Game::SetVelocity(const movement::SetVelocityRequest& request) {
  auto pawn = LivePawn(request.player());
  if (!pawn) {
    return std::unexpected(pawn.error());
  }
  gameinterop::SetEntityVelocity(*pawn, Floats(request.velocity()));
  return {};
}

std::expected<input::ButtonsResponse, std::string> Game::Buttons(
    const players::PlayerRequest& request) {
  // Resolve the controller's pawn fields.
  auto pawn_field = services_.Field("CBasePlayerController", "m_hPawn");
  if (!pawn_field) {
    return std::unexpected(pawn_field.error());
  }
  auto hero_field = services_.Field("CCitadelPlayerController", "m_hHeroPawn");
  if (!hero_field) {
    return std::unexpected(hero_field.error());
  }

  // Resolve the button fields and the live entity system.
  auto movement = services_.Field("CBasePlayerPawn", "m_pMovementServices");
  if (!movement) {
    return std::unexpected(movement.error());
  }
  auto buttons = services_.Field("CPlayer_MovementServices", "m_nButtons");
  if (!buttons) {
    return std::unexpected(buttons.error());
  }
  auto entities = gameinterop::ResolveLiveEntitySystem();
  if (!entities) {
    return std::unexpected(entities.error());
  }

  // A player's controller is the entity at index slot + 1. A dead player's
  // input reaches the pawn that observes, so both pawns are read.
  const auto all = gameinterop::EntityInstances(*entities);
  const auto controller = std::ranges::find_if(all, [&](void* entity) {
    const auto handle = gameinterop::ReferenceHandleOf(entity);
    return handle &&
           static_cast<int32_t>(gameinterop::EntityIndexOf(*handle)) == request.player() + 1;
  });
  if (controller == all.end()) {
    return std::unexpected("the player is not connected");
  }
  input::ButtonsResponse response;
  for (const auto& field : {*pawn_field, *hero_field}) {
    // Read the pawn's movement services.
    uint32_t handle = 0;
    std::memcpy(&handle, static_cast<unsigned char*>(*controller) + field.offset, sizeof(handle));
    auto* pawn = static_cast<unsigned char*>(gameinterop::EntityInstance(*entities, handle));
    if (pawn == nullptr) {
      continue;
    }
    unsigned char* services = nullptr;
    std::memcpy(&services, pawn + movement->offset, sizeof(services));
    if (services == nullptr) {
      continue;
    }

    // InButtonState holds its vtable, then the held mask.
    uint64_t held = 0;
    std::memcpy(&held, services + buttons->offset + sizeof(void*), sizeof(held));
    response.set_buttons(response.buttons() | held);
  }
  return response;
}

std::expected<void, std::string> Game::WatchMovement(
    const movement::WatchMovementRequest& request) {
  if (request.player() < 0 || request.player() >= gameinterop::kMaxPlayerSlots) {
    return std::unexpected("no player has that slot");
  }
  if (request.watch()) {
    movers_.insert(request.player());
  } else {
    movers_.erase(request.player());
  }
  // A sample without its command still serves, so a missing hook is logged.
  if (auto steps = services_.Steps(); !steps) {
    std::cerr << "mod movement: " << steps.error() << '\n';
  }
  return {};
}

std::vector<movement::MovementSample> Game::Movement(std::span<const MovementFact> facts) {
  std::vector<movement::MovementSample> samples;
  for (const auto slot : movers_) {
    // Skip a dead or unmoving pawn.
    const auto sample = observer_.Observe(slot);
    if (!sample || sample->health <= 0 || !sample->movement) {
      continue;
    }
    const auto& movement = *sample->movement;
    if (!movement.abs_velocity || !movement.grounded_by_handle) {
      continue;
    }

    // Record its pose.
    auto& out = samples.emplace_back();
    out.set_player(slot);
    out.set_pawn(sample->pawn_handle);
    SetVector(out.mutable_position(), {static_cast<float>(sample->x), static_cast<float>(sample->y),
                                       static_cast<float>(sample->z)});
    SetVector(out.mutable_velocity(), *movement.abs_velocity);
    out.set_grounded(*movement.grounded_by_handle);

    // Its movement state.
    out.set_sliding(movement.sliding);
    out.set_mantling(movement.mantling);
    out.set_climbing(movement.climbing);
    out.set_dashing(movement.dashing);
    out.set_jump_ability(movement.jump_ability_handle.value_or(0));
    out.set_wall_jumps(movement.consecutive_wall_jumps.value_or(0));
    out.set_mantle_ability(movement.mantle_ability_handle.value_or(0));
    out.set_mantle_start(movement.mantle_start_time.value_or(0));

    // Its wall jump and zipline state, and its command.
    if (movement.wall_contact_position) {
      SetVector(out.mutable_wall_contact(), *movement.wall_contact_position);
    }
    if (movement.current_wall_normal) {
      SetVector(out.mutable_wall_normal(), *movement.current_wall_normal);
    }
    if (movement.wall_jump_normal_used) {
      SetVector(out.mutable_wall_jump_normal(), *movement.wall_jump_normal_used);
    }
    out.set_wall_jump_facing(movement.wall_jump_facing.value_or(0));
    if (auto command = services_.Command(sample->pawn)) {
      *out.mutable_command() = std::move(*command);
    }
    if (movement.last_time_on_zipline) {
      out.set_zipline_time(*movement.last_time_on_zipline);
    }

    // Its actions and casts this tick.
    for (const auto& fact : facts) {
      if (fact.pawn != sample->pawn_handle) {
        continue;
      }
      out.add_actions(fact.action);
      if (fact.action == movement::MOVEMENT_ACTION_ABILITY_EXECUTED) {
        out.add_casts(fact.ability);
      }
    }
  }
  return samples;
}

std::expected<void, std::string> Game::WatchProjectiles(const combat::ProjectileOptions& request) {
  projectiles_ = request;
  if (!projectiles_.names().empty()) {
    services_.Projectiles();
  }
  return {};
}

std::vector<Game::Hero> Game::Heroes() {
  std::vector<Hero> heroes;
  for (int32_t slot = 0; slot < gameinterop::kMaxPlayerSlots; ++slot) {
    // Collect each live hero.
    const auto sample = Live(slot);
    if (!sample || sample->health <= 0) {
      continue;
    }
    Hero hero{.slot = slot,
              .handle = sample->pawn_handle,
              .pawn = sample->pawn,
              .feet = {float(sample->x), float(sample->y), float(sample->z)}};
    if (sample->movement) {
      hero.velocity = sample->movement->abs_velocity;
    }
    heroes.push_back(hero);
  }
  return heroes;
}

void Game::TrackProjectiles() {
  // With no projectile watched, forget the ones seen.
  if (projectiles_.names().empty()) {
    projectiles_seen_.clear();
    return;
  }

  // Note each watched projectile and report the new ones as launches.
  auto entities = gameinterop::ResolveLiveEntitySystem();
  if (!entities) {
    return;
  }
  std::set<uint32_t> seen;
  for (void* entity : gameinterop::EntityInstances(*entities)) {
    // Track each watched projectile.
    const auto name = DesignerName(entity);
    if (!Watches(projectiles_, name)) {
      continue;
    }
    const auto handle = gameinterop::ReferenceHandleOf(entity);
    if (!handle) {
      continue;
    }
    seen.insert(*handle);

    // Skip one already seen.
    if (projectiles_seen_.contains(*handle) || launches_.size() >= kMaxChanges) {
      continue;
    }

    // Report the new one's launch with its owner and position.
    combat::LaunchEvent launch;
    launch.set_entity(*handle);
    launch.set_name(std::string(name));
    const auto [owner, player] = Owner(entity);
    launch.set_owner(owner);
    launch.set_player(player);
    if (const auto origin = Origin(entity)) {
      SetVector(launch.mutable_position(), *origin);
    }
    launches_.push_back(std::move(launch));
  }
  projectiles_seen_ = std::move(seen);
}

std::optional<combat::ImpactEvent> Game::Watching(void* entity,
                                                  const gameinterop::TraceResult& contact) {
  // Only a watched projectile reports an impact.
  const auto name = DesignerName(entity);
  if (!Watches(projectiles_, name)) {
    return std::nullopt;
  }

  // Name the projectile and its owner.
  combat::ImpactEvent event;
  event.set_entity(gameinterop::ReferenceHandleOf(entity).value_or(0));
  event.set_name(std::string(name));
  const auto [owner, player] = Owner(entity);
  event.set_owner(owner);
  event.set_player(player);

  // Add the contact's path and what it hit.
  SetVector(event.mutable_start(), contact.start);
  SetVector(event.mutable_end(), contact.end);
  SetVector(event.mutable_position(), contact.position);
  event.set_hit(HitEntity(contact.entity_handle));
  return event;
}

std::pair<uint32_t, int32_t> Game::Owner(void* entity) {
  // Read the entity's owner handle.
  auto field = services_.Field("CBaseEntity", "m_hOwnerEntity");
  if (!field) {
    return {0, -1};
  }
  uint32_t owner = 0;
  std::memcpy(&owner, static_cast<unsigned char*>(entity) + field->offset, sizeof(owner));
  if (owner == UINT32_MAX) {
    return {0, -1};
  }

  // Find the player whose pawn owns it.
  for (int32_t slot = 0; slot < gameinterop::kMaxPlayerSlots; ++slot) {
    const auto sample = observer_.Observe(slot);
    if (sample && sample->pawn_handle == owner) {
      return {owner, slot};
    }
  }
  return {owner, -1};
}

unsigned char* Game::SceneNode(void* entity) {
  // Resolve the body component's fields.
  auto body = services_.Field("CBaseEntity", "m_CBodyComponent");
  auto node = services_.Field("CBodyComponent", "m_pSceneNode");
  if (!body || !node) {
    return nullptr;
  }

  // Follow the body component to its scene node.
  unsigned char* component = nullptr;
  std::memcpy(&component, static_cast<unsigned char*>(entity) + body->offset, sizeof(component));
  if (component == nullptr) {
    return nullptr;
  }
  unsigned char* scene = nullptr;
  std::memcpy(&scene, component + node->offset, sizeof(scene));
  return scene;
}

std::optional<std::array<float, 3>> Game::Origin(void* entity) {
  return SceneVector(entity, "m_vecAbsOrigin");
}

std::optional<std::array<float, 3>> Game::SceneVector(void* entity, const std::string& field) {
  // Find the scene node and the field.
  auto* scene = SceneNode(entity);
  auto at = services_.Field("CGameSceneNode", field);
  if (scene == nullptr || !at) {
    return std::nullopt;
  }

  // Read the node's three floats.
  std::array<float, 3> value{};
  std::memcpy(value.data(), scene + at->offset, sizeof(value));
  return value;
}

}  // namespace modlock::wasm
