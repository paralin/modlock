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

// ResolveAbilityCooldownSlot decodes the index of the cooldown length query
// in the server's ability dispatch table from the native routine that starts
// a cooldown, which asks it each time. The module must contain exactly one
// such routine with an aligned virtual call.
[[nodiscard]] MODLOCK_API std::expected<size_t, std::string> ResolveAbilityCooldownSlot(
    const ModuleImage& server);

// AbilityCooldown replaces how long chosen abilities' cooldowns last. It
// hooks the native length query of an ability's dispatch table and answers
// the seconds given for the abilities that have them by entity handle; every
// other ability reaches the game's own answer. The game asks as it starts
// each cooldown and scales the answer by the hero's cooldown reduction, so a
// length holds for every later cast with no work between casts, and the game
// replicates the timers it computes from it as it does its own. The handle
// carries the entity's serial, so a length never reaches a different ability
// created at the same address.
// Calls, installation and destruction belong to the engine thread. Only one
// AbilityCooldown lives at a time. Destruction restores every entry.
class MODLOCK_API AbilityCooldown {
 public:
  // HandleOf reads an ability's entity handle, or nothing when it is leaving.
  using HandleOf = std::optional<uint32_t> (*)(void* ability);

  // Create hooks nothing yet. slot is the query's index from
  // ResolveAbilityCooldownSlot. handle_of reads an ability's handle; the
  // default is ReferenceHandleOf.
  static std::expected<std::unique_ptr<AbilityCooldown>, std::string> Create(
      size_t slot, HandleOf handle_of = nullptr);

  ~AbilityCooldown();
  AbilityCooldown(const AbilityCooldown&) = delete;
  AbilityCooldown& operator=(const AbilityCooldown&) = delete;

  // Set makes seconds the length of the ability's cooldowns, hooking the
  // ability's dispatch table the first time one of its class is given a
  // length. No value returns the ability to the game's own length. The
  // ability's handle must be the one handle_of reads from it, and seconds
  // finite and not negative.
  std::expected<void, std::string> Set(void* ability, uint32_t handle,
                                       std::optional<float> seconds);

  // Length reports the seconds Set gave the handle, or no value.
  [[nodiscard]] std::optional<float> Length(uint32_t handle) const;

  // Impl holds the hooks and lengths; the replacement entries read it.
  struct Impl;

 private:
  explicit AbilityCooldown(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace modlock::gameinterop
