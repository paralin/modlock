#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "src/wasm/combat/combat.pb.h"
#include "wasm/hook_sources.h"
#include "wasm/host_service.gen.h"
#include "wasm/module.h"
#include "wasm/roster.h"

namespace modlock::wasm {

class GameServices;

// Projectiles answers a mod's projectile watch: it reports each watched
// projectile's launch once, and each impact through the shared impact hook,
// which reads it as a projectile source while it lives.
class Projectiles final : public Module, private ProjectileSource, public ProjectilesCalls {
 public:
  // Deliver hands a watched impact to the mod, inside the game's impact.
  using Deliver = std::function<void(const combat::ImpactEvent&)>;

  // services and players outlive the projectiles, and deliver stays callable
  // while they live.
  Projectiles(GameServices& services, Roster& players, Deliver deliver);
  ~Projectiles() override;

  Projectiles(const Projectiles&) = delete;
  Projectiles& operator=(const Projectiles&) = delete;

  std::expected<void, std::string> WatchProjectiles(
      const combat::ProjectileOptions& request) override;

  // TakeLaunches returns the watched projectiles that appeared since the last
  // call.
  std::vector<combat::LaunchEvent> TakeLaunches() { return std::exchange(launches_, {}); }

  // Frame queues a launch for each watched projectile that appeared since the
  // last frame.
  void Frame() override;

  // Clear stops watching projectiles and drops the unread launches.
  void Clear() override;

 private:
  std::optional<combat::ImpactEvent> Watching(void* entity,
                                              const gameinterop::TraceResult& contact) override;
  bool KeepsMomentum() const override;
  std::vector<HeroMotion> Motions() override;

  void Impacted(const combat::ImpactEvent& event) override { deliver_(event); }

  // Owner returns a projectile's owning entity and the slot of the player
  // whose hero it is, or -1.
  std::pair<uint32_t, int32_t> Owner(void* entity);

  GameServices& services_;
  Roster& players_;
  Deliver deliver_;
  // watch_ is the mod's projectile watch. seen_ holds the watched projectiles
  // of the last frame, and launches_ queues the new ones until the mod takes
  // them.
  combat::ProjectileOptions watch_;
  std::set<uint32_t> seen_;
  std::vector<combat::LaunchEvent> launches_;
};

}  // namespace modlock::wasm
