#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "modlock/gameinterop/ability_definitions.h"
#include "modlock/gameinterop/engine_server.h"
#include "modlock/gameinterop/pawn_observer.h"

namespace modlock::wasm {

// RestoreTarget is the state a HeroRestore gives one hero.
struct RestoreTarget {
  // Timer sets one owned ability's or item's charges and cooldowns. Times are
  // seconds from the moment the restore writes them; a zero start and end
  // mean no timer.
  struct Timer {
    uint32_t subclass_id = 0;
    int32_t charges = 0;
    std::array<float, 2> cooldown{};
    std::array<float, 2> recharge{};
  };

  std::array<float, 3> position{};
  // angles is where the camera faces; a bot's hero faces it.
  std::array<float, 3> angles{};
  // fresh gives the hero full health and stamina and readies every ability
  // in place of health, max_health, upgrade_bonuses and timers. A bot's
  // ability upgrades may grow while the restore runs, since bots keep
  // spending ability points.
  bool fresh = false;
  // level is absent to keep the hero's level.
  std::optional<int32_t> level;
  int32_t health = 0;
  int32_t max_health = 0;
  std::optional<std::array<float, 3>> upgrade_bonuses;
  // abilities is empty to keep the hero's abilities.
  std::vector<gameinterop::AbilityUpgrade> abilities;
  // items is absent to keep the hero's items.
  std::optional<std::vector<gameinterop::ItemTarget>> items;
  std::vector<Timer> timers;
};

// HeroRestore gives one live hero a RestoreTarget over several frames and proves
// that it held. It writes the build, waits a second for derived stats and
// purchases to settle, writes health, stamina and pose, then charges and
// cooldowns rebased to the live clock, and succeeds once every field has
// matched for a second. It is fenced to the hero it started on: a death,
// respawn or reconnect ends it. It holds no engine pointer.
class HeroRestore {
 public:
  using Sample = gameinterop::PawnObserver::Sample;
  using Ability = gameinterop::PawnObserver::Ability;
  using Result = std::optional<std::expected<void, std::string>>;

  // Stage names one write the engine performs.
  enum class Stage {
    // kBuild sets the level, items, abilities and upgrade bonuses.
    kBuild,
    // kPawn sets health and stamina, then the pose.
    kPawn,
    // kPose sets the position and camera only.
    kPose,
  };

  // Engine is the game access a restore uses.
  struct Engine {
    std::function<bool(const Sample&, const RestoreTarget&, Stage)> apply;
    std::function<std::expected<std::vector<Ability>, std::string>(int32_t)> abilities;
    std::function<std::expected<gameinterop::AbilityDefinitions::Definition, std::string>(uint32_t)>
        definition;
    std::function<std::expected<gameinterop::EngineServer::SimulationClock, std::string>()> clock;
    std::function<std::expected<void, std::string>(int32_t, std::span<const Ability>)> set_timers;
    // ready_timers returns the owned abilities with full charges and no
    // cooldowns.
    std::function<std::expected<std::vector<Ability>, std::string>(int32_t)> ready_timers;
  };

  // Create checks target and fences the restore to the hero in pawn.
  static std::expected<HeroRestore, std::string> Create(const Sample& pawn, RestoreTarget target,
                                                        Engine engine);

  // Tick advances the restore by one frame, given this frame's sample of the
  // player's hero and the frame's tick. It returns nothing while the restore
  // runs and its result once it ends.
  Result Tick(const std::optional<Sample>& sample, uint64_t tick);

 private:
  enum class Phase { kStart, kSettle, kTimers, kVerify };

  HeroRestore(const Sample& pawn, RestoreTarget target, Engine engine);

  Result Start(const Sample& sample, uint64_t tick);
  Result Settle(const Sample& sample, uint64_t tick);
  Result WriteTimers(const Sample& sample, uint64_t tick);
  Result Verify(const Sample& sample, uint64_t tick);

  // RestoreTimers writes the timers and checks the immediate readback.
  std::expected<void, std::string> RestoreTimers(int32_t slot);

  // TimersMatch reports whether the written timers still hold on a later
  // frame, accepting a cooldown that has since run out.
  bool TimersMatch(int32_t slot);
  bool UpgradesMatch(int32_t slot) const;
  bool ItemsMatch(int32_t slot) const;

  // AdoptGrowth accepts upgrade tiers a fresh bot bought on top of the
  // target, never a removed tier or a changed ability.
  bool AdoptGrowth(const Sample& sample, uint64_t tick);

  Sample pawn_;
  RestoreTarget target_;
  Engine engine_;
  Phase phase_ = Phase::kStart;
  uint64_t started_ = 0;
  // since_ is the tick the current phase's last write happened.
  uint64_t since_ = 0;
  // health_ and max_health_ are the health the hero must show; a fresh
  // target learns them from the game's maximum once the build settles.
  int32_t health_ = 0;
  int32_t max_health_ = 0;
  std::vector<Ability> written_;
  std::optional<gameinterop::EngineServer::SimulationClock> written_clock_;
  std::string timer_error_;
  unsigned timer_retries_ = 0;
  uint32_t stable_frames_ = 0;
  // The pose and health each need one matching frame; the camera too, for an
  // exact target on a person's hero. An exact pose need not stay, since a
  // person may move once it has landed.
  bool placed_ = false;
  bool healthy_ = false;
  bool aimed_ = false;
};

}  // namespace modlock::wasm
