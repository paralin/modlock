#include "wasm/hero_restore.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <utility>

namespace modlock::wasm {
namespace {

// kSecond is one second of server frames.
constexpr uint64_t kSecond = 64;

// kDeadline bounds a restore. A build that never settles or a capability that
// never resolves fails here instead of holding the player.
constexpr uint64_t kDeadline = 30 * kSecond;

bool SamePawn(const HeroRestore::Sample& a, const HeroRestore::Sample& b) {
  return a.slot == b.slot && a.steam_id == b.steam_id && a.is_bot == b.is_bot &&
         a.session_generation == b.session_generation && a.pawn_handle == b.pawn_handle;
}

// Beside reports whether the hero stands within 64 units of position across
// the ground, at any height: a placed hero may fall or be pushed out of
// geometry, while a respawn moves it across the map.
bool Beside(const HeroRestore::Sample& sample, const std::array<float, 3>& position) {
  return std::hypot(sample.x - position[0], sample.y - position[1]) <= 64;
}

// Aimed reports whether the camera faces angles, allowing for the degree
// quantization of the client's echo.
bool Aimed(const HeroRestore::Sample& sample, const std::array<float, 3>& angles) {
  for (size_t axis = 0; axis < angles.size(); ++axis) {
    if (std::abs(std::remainder(sample.camera_angles[axis] - angles[axis], 360.0f)) > 0.36f) {
      return false;
    }
  }
  return true;
}

bool Finite(const std::array<float, 3>& values) {
  return std::ranges::all_of(values, [](float value) { return std::isfinite(value); });
}

}  // namespace

std::expected<HeroRestore, std::string> HeroRestore::Create(const Sample& pawn,
                                                            RestoreTarget target, Engine engine) {
  if (pawn.health <= 0) return std::unexpected("the player has no live hero");
  if (!Finite(target.position) || !Finite(target.angles)) {
    return std::unexpected("the target's position and angles must be finite");
  }
  if (!target.fresh && (target.health <= 0 || target.max_health <= 0)) {
    return std::unexpected("an exact target needs its health and maximum health");
  }
  if (target.level && (*target.level < 0 || *target.level > 100)) {
    return std::unexpected("the target's level is out of range");
  }
  for (size_t i = 0; i < target.abilities.size(); ++i) {
    for (size_t j = 0; j < i; ++j) {
      if (target.abilities[i].slot == target.abilities[j].slot) {
        return std::unexpected("the target repeats an ability slot");
      }
    }
  }
  if (target.items) {
    for (const auto& item : *target.items) {
      if (std::ranges::count(*target.items, item.subclass_id,
                             &gameinterop::ItemTarget::subclass_id) > 1) {
        return std::unexpected("the target repeats an item");
      }
      if (item.slot && *item.slot != 23 && (*item.slot < 4 || *item.slot > 7)) {
        return std::unexpected("an item slot must be 4 to 7 or 23");
      }
    }
  }
  return HeroRestore(pawn, std::move(target), std::move(engine));
}

HeroRestore::HeroRestore(const Sample& pawn, RestoreTarget target, Engine engine)
    : pawn_(pawn),
      target_(std::move(target)),
      engine_(std::move(engine)),
      health_(target_.health),
      max_health_(target_.max_health) {}

HeroRestore::Result HeroRestore::Tick(const std::optional<Sample>& sample, uint64_t tick) {
  if (!sample || !SamePawn(*sample, pawn_) || sample->health <= 0) {
    return std::unexpected("the hero died or changed during the restore");
  }
  if (phase_ == Phase::kStart) started_ = tick;
  if (tick - started_ > kDeadline) {
    return std::unexpected(timer_error_.empty()
                               ? "the hero did not settle in 30 seconds"
                               : "the timers could not be written: " + timer_error_);
  }
  switch (phase_) {
    case Phase::kStart:
      return Start(*sample, tick);
    case Phase::kSettle:
      return Settle(*sample, tick);
    case Phase::kTimers:
      return WriteTimers(*sample, tick);
    case Phase::kVerify:
      return Verify(*sample, tick);
  }
  return std::nullopt;
}

HeroRestore::Result HeroRestore::Start(const Sample& sample, uint64_t tick) {
  if (!engine_.apply(sample, target_, Stage::kBuild)) {
    return std::unexpected("the build could not be written");
  }
  phase_ = Phase::kSettle;
  since_ = tick;
  return std::nullopt;
}

HeroRestore::Result HeroRestore::Settle(const Sample& sample, uint64_t tick) {
  // Leave a second for derived stats, purchases and the spawn area's healing
  // to settle before health is written; the later readback is the proof.
  if (tick - since_ < kSecond) return std::nullopt;
  if (target_.fresh) {
    if (!sample.effective_max_health || *sample.effective_max_health <= 0 ||
        sample.max_health < 0) {
      return std::nullopt;
    }
    health_ = *sample.effective_max_health;
    max_health_ = sample.max_health;
  }
  const bool upgrades = UpgradesMatch(sample.slot);
  if (!upgrades && AdoptGrowth(sample, tick)) return std::nullopt;
  const bool items = ItemsMatch(sample.slot);
  if ((target_.level && sample.level != *target_.level) || !upgrades || !items) {
    return std::unexpected(
        std::format("the build did not take: level {} of {}, abilities {}, items {}", sample.level,
                    target_.level.value_or(sample.level), upgrades ? "match" : "differ",
                    items ? "match" : "differ"));
  }
  if (!engine_.apply(sample, target_, Stage::kPawn)) {
    return std::unexpected("health and pose could not be written");
  }

  // Timers follow in the same frame as health and pose.
  phase_ = Phase::kTimers;
  return WriteTimers(sample, tick);
}

HeroRestore::Result HeroRestore::WriteTimers(const Sample& sample, uint64_t tick) {
  // A refused write retries each second until the deadline.
  if (!timer_error_.empty() && tick - since_ < kSecond) return std::nullopt;
  since_ = tick;
  if (auto written = RestoreTimers(sample.slot); !written) {
    timer_error_ = written.error();
    return std::nullopt;
  }
  timer_error_.clear();
  phase_ = Phase::kVerify;
  return std::nullopt;
}

HeroRestore::Result HeroRestore::Verify(const Sample& sample, uint64_t tick) {
  const bool held = Beside(sample, target_.position);
  healthy_ = healthy_ || std::abs(sample.health - health_) <= 1 || (target_.fresh && held);
  aimed_ = aimed_ || sample.is_bot || Aimed(sample, target_.angles);

  // A respawn or a client command in flight can overwrite a fresh pose once.
  if (target_.fresh && !held && tick - since_ == kSecond &&
      !engine_.apply(sample, target_, Stage::kPose)) {
    return std::unexpected("the pose could not be written again");
  }

  const bool upgrades = UpgradesMatch(sample.slot);
  if (!upgrades && AdoptGrowth(sample, tick)) return std::nullopt;
  const bool items = ItemsMatch(sample.slot);
  const bool stamina =
      !target_.fresh ||
      (sample.stamina && std::isfinite(sample.stamina->current) && sample.stamina->max > 0 &&
       std::abs(sample.stamina->current - sample.stamina->max) <= 0.01f);
  const bool health = healthy_ && sample.max_health == max_health_ &&
                      (target_.fresh ? sample.effective_max_health == health_ &&
                                           std::abs(sample.health - health_) <= 1
                                     : sample.health >= health_ - 1);
  const bool bonuses = target_.fresh || !target_.upgrade_bonuses ||
                       sample.upgrade_bonuses == target_.upgrade_bonuses;
  const bool level = !target_.level || sample.level == *target_.level;
  const bool pawn = (held || (!target_.fresh && placed_)) && health && stamina &&
                    (target_.fresh || aimed_) && level && bonuses && upgrades && items;
  const bool timers = TimersMatch(sample.slot);
  if (pawn && timers) {
    placed_ = true;
    if (++stable_frames_ < kSecond) return std::nullopt;
    return std::expected<void, std::string>();
  }
  stable_frames_ = 0;
  if (tick - since_ <= 2 * kSecond) return std::nullopt;

  // An ability that ends after the write can start a new cooldown. A fresh
  // target clears the timers twice more, each needing its own stable second.
  if (pawn && target_.fresh && timer_retries_ < 2) {
    ++timer_retries_;
    if (RestoreTimers(sample.slot)) {
      since_ = tick;
      return std::nullopt;
    }
  }
  return std::unexpected(std::format(
      "the hero did not hold the target: position ({:.1f}, {:.1f}, {:.1f}) for ({:.1f}, {:.1f}, "
      "{:.1f}), health {} of {} (maximum {} of {}), level {}, stamina {}, camera {}, bonuses {}, "
      "abilities {}, items {}, timers {}",
      sample.x, sample.y, sample.z, target_.position[0], target_.position[1], target_.position[2],
      sample.health, health_, sample.max_health, max_health_, level ? "match" : "differs",
      stamina ? "full" : "short", aimed_ ? "aimed" : "off", bonuses ? "match" : "differ",
      upgrades ? "match" : "differ", items ? "match" : "differ", timers ? "match" : "differ"));
}

std::expected<void, std::string> HeroRestore::RestoreTimers(int32_t slot) {
  written_.clear();
  written_clock_.reset();
  if (!target_.fresh && target_.timers.empty()) return {};
  auto clock = engine_.clock();
  if (!clock) return std::unexpected(clock.error());
  if (target_.fresh) {
    auto ready = engine_.ready_timers(slot);
    if (!ready) return std::unexpected(ready.error());
    written_ = std::move(*ready);
  } else {
    auto owned = engine_.abilities(slot);
    if (!owned) return std::unexpected(owned.error());
    for (const auto& timer : target_.timers) {
      const auto matches = [&](const Ability& a) { return a.subclass_id == timer.subclass_id; };
      if (std::ranges::count_if(*owned, matches) != 1) {
        return std::unexpected(
            std::format("the hero does not own exactly one of {}", timer.subclass_id));
      }
      auto ability = *std::ranges::find_if(*owned, matches);
      const auto rebase = [&](const std::array<float, 2>& interval) {
        if (interval[0] == 0 && interval[1] == 0) return interval;
        return std::array{clock->current_time + interval[0], clock->current_time + interval[1]};
      };
      const auto cooldown = rebase(timer.cooldown);
      const auto recharge = rebase(timer.recharge);
      ability.charges = timer.charges;
      ability.cooldown_start = cooldown[0];
      ability.cooldown_end = cooldown[1];
      ability.charge_recharge_start = recharge[0];
      ability.charge_recharge_end = recharge[1];
      written_.push_back(ability);
    }
  }
  if (auto set = engine_.set_timers(slot, written_); !set) return set;

  // Check the write took before trusting later frames to keep it.
  auto owned = engine_.abilities(slot);
  if (!owned) return std::unexpected(owned.error());
  for (const auto& target : written_) {
    if (std::ranges::none_of(*owned, [&](const Ability& a) {
          return a.handle == target.handle && a.subclass_id == target.subclass_id &&
                 a.slot == target.slot && a.charges == target.charges &&
                 a.cooldown_start == target.cooldown_start &&
                 a.cooldown_end == target.cooldown_end &&
                 a.charge_recharge_start == target.charge_recharge_start &&
                 a.charge_recharge_end == target.charge_recharge_end;
        })) {
      return std::unexpected("the timers read back differently");
    }
  }
  written_clock_ = *clock;
  return {};
}

bool HeroRestore::TimersMatch(int32_t slot) {
  if (written_.empty()) return true;
  auto clock = engine_.clock();
  if (!clock || !written_clock_ || clock->tick <= written_clock_->tick ||
      clock->current_time <= written_clock_->current_time) {
    return false;
  }
  auto owned = engine_.abilities(slot);
  if (!owned) return false;
  const auto holds = [&](float start, float end, float actual_start, float actual_end) {
    return (start == actual_start && end == actual_end) ||
           (end <= clock->current_time && actual_start == 0 && actual_end == 0);
  };
  for (const auto& target : written_) {
    const auto found = std::ranges::find_if(*owned, [&](const Ability& a) {
      return a.handle == target.handle && a.subclass_id == target.subclass_id &&
             a.slot == target.slot;
    });
    // Clearing a recharge lets the game replenish charges. A fresh target
    // allows that gain but never a loss; an exact one keeps the count.
    if (found == owned->end() ||
        (target_.fresh ? found->charges < target.charges : found->charges != target.charges) ||
        !holds(target.cooldown_start, target.cooldown_end, found->cooldown_start,
               found->cooldown_end) ||
        !holds(target.charge_recharge_start, target.charge_recharge_end,
               found->charge_recharge_start, found->charge_recharge_end)) {
      return false;
    }
  }
  written_clock_ = *clock;
  return true;
}

bool HeroRestore::UpgradesMatch(int32_t slot) const {
  if (target_.abilities.empty()) return true;
  auto owned = engine_.abilities(slot);
  if (!owned) return false;
  return std::ranges::all_of(target_.abilities, [&](const gameinterop::AbilityUpgrade& target) {
    const auto matches = [&](const Ability& a) {
      return a.subclass_id == target.subclass_id && a.slot == target.slot;
    };
    return std::ranges::count_if(*owned, matches) == 1 &&
           std::ranges::find_if(*owned, matches)->upgrade_info == target.upgrade_info;
  });
}

bool HeroRestore::ItemsMatch(int32_t slot) const {
  if (!target_.items) return true;
  auto owned = engine_.abilities(slot);
  if (!owned) return false;
  const auto& items = *target_.items;
  const auto same = [](const gameinterop::ItemTarget& target, const Ability& a) {
    return target.subclass_id == a.subclass_id && target.upgrade_info == a.upgrade_info &&
           (!target.slot || a.slot == *target.slot);
  };

  // Every owned item is one of the targets, and every target is owned.
  size_t count = 0;
  for (const auto& ability : *owned) {
    auto definition = engine_.definition(ability.subclass_id);
    if (!definition) return false;
    if (!definition->name.starts_with("upgrade_")) continue;
    if (definition->disabled) return false;
    ++count;
    if (std::ranges::count_if(items, [&](const auto& target) { return same(target, ability); }) !=
        1) {
      return false;
    }
  }
  return count == items.size() && std::ranges::all_of(items, [&](const auto& target) {
           return std::ranges::count_if(*owned, [&](const auto& a) { return same(target, a); }) ==
                  1;
         });
}

bool HeroRestore::AdoptGrowth(const Sample& sample, uint64_t tick) {
  if (!sample.is_bot || !target_.fresh || !ItemsMatch(sample.slot)) return false;
  auto owned = engine_.abilities(sample.slot);
  if (!owned) return false;
  auto abilities = target_.abilities;
  for (auto& target : abilities) {
    const auto matches = [&](const Ability& a) {
      return a.subclass_id == target.subclass_id && a.slot == target.slot;
    };
    if (std::ranges::count_if(*owned, matches) != 1) return false;
    const auto& ability = *std::ranges::find_if(*owned, matches);
    if ((ability.upgrade_info & 0xffffu) != (target.upgrade_info & 0xffffu) ||
        (ability.upgrade_info & target.upgrade_info) != target.upgrade_info) {
      return false;
    }
    target.upgrade_info = ability.upgrade_info;
  }
  target_.abilities = std::move(abilities);

  // Settle again so health follows the grown build.
  phase_ = Phase::kSettle;
  since_ = tick;
  stable_frames_ = 0;
  return true;
}

}  // namespace modlock::wasm
