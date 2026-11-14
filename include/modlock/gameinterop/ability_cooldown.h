#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>

#include "modlock/export.h"
#include "modlock/gameinterop/game_symbols.h"

namespace modlock::gameinterop {

// AbilityCooldownSlots identifies the two length queries of the server's
// ability dispatch table. The game asks cooldown each time it starts an
// ability's cooldown or the return of one of its charges, and charge_delay
// each time an ability with charges spends one.
struct AbilityCooldownSlots {
  size_t cooldown;
  size_t charge_delay;
};

// ResolveAbilityCooldownSlots decodes both queries from the native routine
// that starts a cooldown. The module must contain exactly one such routine
// with aligned virtual calls.
[[nodiscard]] MODLOCK_API std::expected<AbilityCooldownSlots, std::string>
ResolveAbilityCooldownSlots(const ModuleImage& server);

// AbilityCooldown replaces how long chosen abilities' cooldowns last. It
// hooks the two native length queries of an ability's dispatch table and
// answers the seconds given for the abilities that have them by entity
// handle; every other ability, and every other query, reaches the game's own
// answer. The game asks as it starts each cooldown, so a length holds for
// every later cast with no work between casts, and the game replicates the
// timers it computes from it as it does its own. The handle carries the
// entity's serial, so a length never reaches a different ability created at
// the same address.
// Calls, installation and destruction belong to the engine thread. Only one
// AbilityCooldown lives at a time. Destruction restores every entry.
class MODLOCK_API AbilityCooldown {
 public:
  // HandleOf reads an ability's entity handle, or nothing when it is leaving.
  using HandleOf = std::optional<uint32_t> (*)(void* ability);

  // Timer names one of the two lengths.
  enum class Timer {
    // kCooldown is the ability's cooldown, and for an ability with charges
    // the time one charge takes to return.
    kCooldown,
    // kChargeDelay is the wait after spending a charge before the next.
    kChargeDelay,
  };

  // Create hooks nothing yet. slots are the query indexes from
  // ResolveAbilityCooldownSlots. handle_of reads an ability's handle; the
  // default is ReferenceHandleOf.
  static std::expected<std::unique_ptr<AbilityCooldown>, std::string> Create(
      AbilityCooldownSlots slots, HandleOf handle_of = nullptr);

  ~AbilityCooldown();
  AbilityCooldown(const AbilityCooldown&) = delete;
  AbilityCooldown& operator=(const AbilityCooldown&) = delete;

  // Set makes seconds the length of the ability's timer, hooking the
  // ability's dispatch table the first time one of its class is given a
  // length. Nothing returns the timer to the game's own. The ability's handle
  // must be the one handle_of reads from it, and seconds finite and not
  // negative.
  std::expected<void, std::string> Set(void* ability, uint32_t handle, Timer timer,
                                       std::optional<float> seconds);

  // Length reports the seconds Set gave the handle's timer, or nothing.
  [[nodiscard]] std::optional<float> Length(uint32_t handle, Timer timer) const;

  // Impl holds the hooks and lengths; the replacement entries read it.
  struct Impl;

 private:
  explicit AbilityCooldown(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace modlock::gameinterop
