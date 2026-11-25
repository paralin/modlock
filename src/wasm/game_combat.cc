//go:build deps_only

#include <utility>
#include <vector>

#include "modlock/gameinterop/native_memory.h"
#include "wasm/game.h"
#include "wasm/game_internal.h"

namespace modlock::wasm {

std::expected<void, std::string> Game::Damage(const combat::HitOptions& request) {
  // Resolve the damage service and the victim.
  auto damage = services_.Damage();
  if (!damage) {
    return std::unexpected(damage.error());
  }
  auto victim = Combatant(request.victim());
  if (!victim) {
    return std::unexpected(victim.error());
  }

  // A hit nobody dealt is a hazard: it may kill and credits no one.
  if (request.attacker() == 0) {
    return (*damage)->Hazard(*victim, request.amount());
  }

  // Resolve who dealt the hit and with what.
  auto attacker = LiveEntity(request.attacker());
  if (!attacker) {
    return std::unexpected(attacker.error());
  }
  auto inflictor = request.inflictor() == 0 ? attacker : LiveEntity(request.inflictor());
  if (!inflictor) {
    return std::unexpected(inflictor.error());
  }
  const int32_t hit_group = request.has_hit_group() ? request.hit_group() : -1;
  void* ability = nullptr;
  if (request.has_ability()) {
    auto found = LiveEntity(request.ability());
    if (!found) {
      return std::unexpected(found.error());
    }
    ability = *found;
  }
  return (*damage)->Hit(*victim, *inflictor, *attacker, ability, request.amount(), hit_group);
}

std::expected<void*, std::string> Game::Combatant(uint32_t handle) {
  // Damage events for the world, props or buildings crash clients.
  auto victim = LiveEntity(handle);
  if (!victim) {
    return std::unexpected(victim.error());
  }
  auto type = gameinterop::SchemaClassOfEntity(*victim, gameinterop::ReadNative);
  if (!type) {
    return std::unexpected(type.error());
  }
  if (!services_.Derives(*type, "CBaseCombatCharacter")) {
    return std::unexpected("the victim is a " + std::string(gameinterop::SchemaClassNameOf(*type)) +
                           ", not a hero or unit");
  }
  return *victim;
}

std::expected<void, std::string> Game::KillEntity(const combat::KillOptions& request) {
  // Resolve a live combat entity before invoking the game's death path.
  auto damage = services_.Damage();
  if (!damage) {
    return std::unexpected(damage.error());
  }
  auto victim = Combatant(request.victim());
  if (!victim) {
    return std::unexpected(victim.error());
  }
  if (request.attacker() == 0) {
    return (*damage)->Kill(*victim);
  }

  // Preserve generation-checked source handles; stale sources cannot get credit.
  auto attacker = LiveEntity(request.attacker());
  if (!attacker) {
    return std::unexpected(attacker.error());
  }
  auto inflictor = request.inflictor() == 0 ? attacker : LiveEntity(request.inflictor());
  if (!inflictor) {
    return std::unexpected(inflictor.error());
  }
  void* ability = nullptr;
  if (request.ability() != 0) {
    auto found = LiveEntity(request.ability());
    if (!found) {
      return std::unexpected(found.error());
    }
    ability = *found;
  }
  return (*damage)->Kill(*victim, *attacker, *inflictor, ability);
}

}  // namespace modlock::wasm
