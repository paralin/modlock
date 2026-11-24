#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>

namespace modlock::wasm {

class GameServices;

// ModifierWord is the word of an entity's modifier masks that holds one state,
// and the state's bit in it.
struct ModifierWord {
  unsigned char* ordinary = nullptr;
  unsigned char* predicted = nullptr;
  uint32_t bit = 0;
};

// FindModifierWord locates state on entity, or returns nothing when the entity
// has no modifiers.
std::expected<std::optional<ModifierWord>, std::string> FindModifierWord(GameServices& services,
                                                                         void* entity,
                                                                         const std::string& state);

// ReadModifierState reports whether the entity with handle has state. The game
// predicts some states, such as parry, so a state is active when either mask
// holds it; an entity without modifiers has none.
std::expected<bool, std::string> ReadModifierState(GameServices& services, uint32_t handle,
                                                   const std::string& state);

// SetModifierState sets or clears state in the ordinary mask of the entity
// with handle and replicates the change.
std::expected<void, std::string> SetModifierState(GameServices& services, uint32_t handle,
                                                  const std::string& state, bool active);

}  // namespace modlock::wasm
