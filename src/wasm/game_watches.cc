//go:build deps_only

#include <algorithm>
#include <array>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

#include "wasm/entity_fields.h"
#include "wasm/game.h"
#include "wasm/game_internal.h"

namespace modlock::wasm {

namespace {

// Watches reports whether options name a projectile.
bool Watches(const combat::ProjectileOptions& options, std::string_view name) {
  return !name.empty() && std::ranges::find(options.names(), name) != options.names().end();
}

}  // namespace

std::expected<void, std::string> Game::WatchProjectiles(const combat::ProjectileOptions& request) {
  projectiles_ = request;
  if (!projectiles_.names().empty()) {
    services_.Projectiles();
  }
  return {};
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
    const auto name = gameinterop::DesignerName(entity);
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
    if (const auto origin = Origin(services_, entity)) {
      SetVector(launch.mutable_position(), *origin);
    }
    launches_.push_back(std::move(launch));
  }
  projectiles_seen_ = std::move(seen);
}

std::optional<combat::ImpactEvent> Game::Watching(void* entity,
                                                  const gameinterop::TraceResult& contact) {
  // Only a watched projectile reports an impact.
  const auto name = gameinterop::DesignerName(entity);
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
    const auto sample = players_.Observer().Observe(slot);
    if (sample && sample->pawn_handle == owner) {
      return {owner, slot};
    }
  }
  return {owner, -1};
}

}  // namespace modlock::wasm
