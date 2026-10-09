#include "modlock/gameinterop/match_clock.h"

#include <cmath>
#include <cstring>

#include "modlock/gameinterop/entity_abi.h"
#include "modlock/gameinterop/game_rules.h"

namespace modlock::gameinterop {

std::expected<MatchClock, std::string> MatchClock::Resolve(const ModuleImage& server,
                                                           void* schema_system) {
  auto current = ResolveCurrentGameRules(server);
  if (!current) return std::unexpected(current.error());
  MatchClock clock;
  clock.current_ = *current;
  clock.module_begin_ = server.base();
  clock.module_size_ = server.image_bytes().size();
  const char* names[] = {"m_flGameStartTime", "m_nTotalPausedTicks", "m_bGamePaused",
                         "m_nPauseStartTick"};
  for (size_t i = 0; i < clock.offsets_.size(); ++i) {
    auto field = SchemaFieldOf(schema_system, "server.dll", "CCitadelGameRules", names[i]);
    if (!field) return std::unexpected(field.error());
    if (field->size < (i == 2 ? sizeof(bool) : sizeof(uint32_t)))
      return std::unexpected(std::string("match clock storage too short: ") + names[i]);
    clock.offsets_[i] = field->offset;
  }
  return clock;
}

std::expected<void*, std::string> MatchClock::Current() const {
  if (!current_) return std::unexpected("match clock is unresolved");
  void* rules = nullptr;
  std::memcpy(&rules, current_, sizeof(rules));
  if (!rules) return std::unexpected("match clock game rules are not active");
  uintptr_t table = 0;
  std::memcpy(&table, rules, sizeof(table));
  if (table < module_begin_ || table - module_begin_ >= module_size_)
    return std::unexpected("match clock rules leave the server module");
  return rules;
}

std::expected<float, std::string> MatchClock::GameTime(float current_time, float interval) const {
  if (!std::isfinite(current_time) || !std::isfinite(interval) || interval <= 0)
    return std::unexpected("match clock requires a finite simulation clock");
  auto rules = Current();
  if (!rules) return std::unexpected(rules.error());
  const auto* bytes = static_cast<const unsigned char*>(*rules);
  int32_t paused_ticks = 0;
  int32_t pause_start = 0;
  bool paused = false;
  std::memcpy(&paused_ticks, bytes + offsets_[1], sizeof(paused_ticks));
  std::memcpy(&paused, bytes + offsets_[2], sizeof(paused));
  std::memcpy(&pause_start, bytes + offsets_[3], sizeof(pause_start));
  if (paused_ticks < 0 || pause_start < 0)
    return std::unexpected("native match clock fields are invalid");
  if (paused && current_time > pause_start * interval) current_time = pause_start * interval;
  return current_time - paused_ticks * interval;
}

std::expected<float, std::string> MatchClock::Read(float current_time, float interval) const {
  auto time = GameTime(current_time, interval);
  if (!time) return std::unexpected(time.error());
  float start = 0;
  std::memcpy(&start, static_cast<const unsigned char*>(*Current()) + offsets_[0], sizeof(start));
  if (!std::isfinite(start)) return std::unexpected("native match clock fields are invalid");
  return *time - start;
}

std::expected<void, std::string> MatchClock::Restore(float seconds, float current_time,
                                                     float interval) {
  if (!std::isfinite(seconds)) return std::unexpected("source match clock is nonfinite");
  auto before = Read(current_time, interval);
  if (!before) return std::unexpected(before.error());
  auto rules = Current();
  if (!rules) return std::unexpected(rules.error());
  auto* bytes = static_cast<unsigned char*>(*rules);
  float start = 0;
  std::memcpy(&start, bytes + offsets_[0], sizeof(start));
  start += *before - seconds;
  std::memcpy(bytes + offsets_[0], &start, sizeof(start));
  auto after = Read(current_time, interval);
  if (!after || std::abs(*after - seconds) > interval)
    return std::unexpected("native match clock readback differs from source");
  return {};
}

}  // namespace modlock::gameinterop
