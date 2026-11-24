//go:build deps_only

#include "wasm/modifier_masks.h"

#include <cstddef>

#include "modlock/gameinterop/native_memory.h"
#include "wasm/game.h"
#include "wasm/game_internal.h"

namespace modlock::wasm {

std::expected<std::optional<ModifierWord>, std::string> FindModifierWord(GameServices& services,
                                                                         void* entity,
                                                                         const std::string& state) {
  // Resolve the mask layout and the state's bit.
  auto layout = services.Modifiers();
  if (!layout) {
    return std::unexpected(layout.error());
  }
  auto value = services.ModifierState(state);
  if (!value) {
    return std::unexpected(value.error());
  }
  if (*value < 0 || static_cast<size_t>(*value) >= layout->mask_size * 8) {
    return std::unexpected(state + " is outside the modifier state masks");
  }

  // Read the entity's modifier property.
  unsigned char* property = nullptr;
  if (!gameinterop::ReadNative(static_cast<const unsigned char*>(entity) + layout->property,
                               &property, sizeof(property))) {
    return std::unexpected("cannot read the entity's modifiers");
  }
  if (property == nullptr) {
    return std::nullopt;
  }

  // Point at the word that holds the state in each mask.
  const auto word = static_cast<size_t>(*value / 32) * sizeof(uint32_t);
  return ModifierWord{
      .ordinary = property + layout->mask + word,
      .predicted = property + layout->predicted_mask + word,
      .bit = uint32_t{1} << (*value % 32),
  };
}

std::expected<bool, std::string> ReadModifierState(GameServices& services, uint32_t handle,
                                                   const std::string& state) {
  // Find the entity and the mask word that holds the state.
  auto entity = LiveEntity(handle);
  if (!entity) {
    return std::unexpected(entity.error());
  }
  auto word = FindModifierWord(services, *entity, state);
  if (!word) {
    return std::unexpected(word.error());
  }
  if (!*word) {
    return false;
  }

  // Either mask holding the bit makes the state active.
  uint32_t ordinary = 0;
  uint32_t predicted = 0;
  if (!gameinterop::ReadNative((*word)->ordinary, &ordinary, sizeof(ordinary)) ||
      !gameinterop::ReadNative((*word)->predicted, &predicted, sizeof(predicted))) {
    return std::unexpected("cannot read the entity's modifier states");
  }
  return ((ordinary | predicted) & (*word)->bit) != 0;
}

std::expected<void, std::string> SetModifierState(GameServices& services, uint32_t handle,
                                                  const std::string& state, bool active) {
  // Find the entity and the mask word that holds the state.
  auto entity = LiveEntity(handle);
  if (!entity) {
    return std::unexpected(entity.error());
  }
  auto word = FindModifierWord(services, *entity, state);
  if (!word) {
    return std::unexpected(word.error());
  }
  if (!*word) {
    return std::unexpected("the entity has no modifiers");
  }

  // Read the ordinary mask and set or clear the state's bit.
  uint32_t current = 0;
  if (!gameinterop::ReadNative((*word)->ordinary, &current, sizeof(current))) {
    return std::unexpected("cannot read the entity's modifier states");
  }
  const uint32_t desired = active ? current | (*word)->bit : current & ~(*word)->bit;
  if (desired == current) {
    return {};
  }

  // Write the mask back and send it.
  if (!gameinterop::WriteNative((*word)->ordinary, &desired, sizeof(desired))) {
    return std::unexpected("cannot write the entity's modifier states");
  }
  if (!gameinterop::NotifyEntityStateChanged(*entity)) {
    return std::unexpected("cannot replicate the entity's modifier states");
  }
  return {};
}

}  // namespace modlock::wasm
