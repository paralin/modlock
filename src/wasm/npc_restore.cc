#include "wasm/npc_restore.h"

#include <format>
#include <limits>
#include <map>
#include <utility>

namespace modlock::wasm {
namespace {

// kReach is how far, in units, a restored unit may stand from its target.
constexpr float kReach = 16;

float DistanceSquared(const std::array<float, 3>& a, const std::array<float, 3>& b) {
  float sum = 0;
  for (size_t axis = 0; axis < a.size(); ++axis) sum += (a[axis] - b[axis]) * (a[axis] - b[axis]);
  return sum;
}

std::string Describe(const NpcRestore::Target& unit) {
  return std::format("{} {} team {} health {}/{} lane {} at ({:.0f}, {:.0f}, {:.0f})",
                     unit.designer_name, unit.subclass_id, unit.team, unit.health, unit.max_health,
                     unit.lane.value_or(0), unit.position[0], unit.position[1], unit.position[2]);
}

}  // namespace

std::expected<NpcRestore, std::string> NpcRestore::Create(std::vector<Target> targets,
                                                          Engine engine) {
  for (const auto& target : targets) {
    if (target.health <= 0 || target.max_health <= 0) {
      return std::unexpected("each unit needs its health and maximum health: " +
                             target.designer_name);
    }
    if (target.designer_name == "npc_trooper" && !target.lane) {
      return std::unexpected("each trooper needs its lane");
    }
  }
  return NpcRestore(std::move(targets), std::move(engine));
}

NpcRestore::NpcRestore(std::vector<Target> targets, Engine engine)
    : targets_(std::move(targets)), engine_(std::move(engine)) {}

NpcRestore::Result NpcRestore::Tick() {
  switch (phase_) {
    case Phase::kRestore:
      if (auto done = engine_.restore(targets_); !done) return done;
      phase_ = Phase::kFinish;
      return std::nullopt;
    case Phase::kFinish:
      if (auto done = engine_.finish(); !done) return done;
      phase_ = Phase::kVerify;
      return std::nullopt;
    case Phase::kVerify:
      break;
  }
  auto samples = engine_.read();
  if (!samples) return std::unexpected(samples.error());
  if (auto mismatch = Match(targets_, *samples)) return std::unexpected(*mismatch);
  return std::expected<void, std::string>();
}

std::optional<std::string> NpcRestore::Match(std::span<const Target> targets,
                                             std::span<const Sample> samples) {
  if (samples.size() != targets.size()) {
    // Name each kind whose count differs.
    std::map<std::string, std::pair<size_t, size_t>> counts;
    for (const auto& target : targets) {
      ++counts[std::format("{} {}", target.designer_name, target.subclass_id)].first;
    }
    for (const auto& sample : samples) {
      ++counts[std::format("{} {}", sample.state.designer_name, sample.state.subclass_id)].second;
    }
    auto error =
        std::format("the map holds {} units for {} targets", samples.size(), targets.size());
    for (const auto& [kind, count] : counts) {
      if (count.first != count.second) {
        error += std::format("; {}: {} for {}", kind, count.second, count.first);
      }
    }
    return error;
  }

  std::vector<bool> matched(samples.size());
  for (const auto& target : targets) {
    const Sample* nearest = nullptr;
    float nearest_distance = std::numeric_limits<float>::max();
    bool found = false;
    for (size_t i = 0; i < samples.size() && !found; ++i) {
      const auto& unit = samples[i].state;
      if (matched[i] || unit.designer_name != target.designer_name ||
          unit.subclass_id != target.subclass_id || unit.team != target.team) {
        continue;
      }
      const float distance = DistanceSquared(unit.position, target.position);
      if (distance < nearest_distance) {
        nearest = &samples[i];
        nearest_distance = distance;
      }
      found = unit.health == target.health && unit.max_health == target.max_health &&
              unit.lane == target.lane && distance <= kReach * kReach;
      matched[i] = found;
    }
    if (!found) {
      auto error = "no unit matches " + Describe(target);
      if (nearest) error += "; the nearest is " + Describe(nearest->state);
      return error;
    }
  }
  return std::nullopt;
}

}  // namespace modlock::wasm
