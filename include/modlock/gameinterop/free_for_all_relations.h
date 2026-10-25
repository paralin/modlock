#pragma once

#include <expected>
#include <memory>
#include <string>

#include "modlock/export.h"
#include "modlock/gameinterop/game_symbols.h"

namespace modlock::gameinterop {

// FreeForAllRelations makes heroes on the same team treat each other as
// enemies while enabled. Three server checks decide who is an enemy: the
// classifier that labels a target as a friendly or enemy hero, the filter that
// picks which team layers an ability's area query visits, and the collision
// setup that keeps an entity from interacting with its own team's layer.
// Enabled, a same-team hero classifies as both friendly and enemy, enemy-only
// queries visit the caster's own layer, and entities set up their collision
// with their own team too, so abilities, projectiles and target checks reach
// teammates. An entity whose collision was set before enabling keeps it until
// the game sets it again, as on respawn. Disabled, all three behave as stock.
// Install on the engine thread and retain until the engine stops; only one may
// be installed.
class MODLOCK_API FreeForAllRelations {
 public:
  static std::expected<FreeForAllRelations, std::string> Install(const ModuleImage& server);
  FreeForAllRelations(FreeForAllRelations&&) noexcept;
  FreeForAllRelations& operator=(FreeForAllRelations&&) noexcept;
  ~FreeForAllRelations();

  // SetEnabled turns the relations on or off for every check after it.
  void SetEnabled(bool enabled);

 private:
  struct Impl;
  explicit FreeForAllRelations(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace modlock::gameinterop
