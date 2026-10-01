#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "modlock/export.h"
#include "modlock/gameinterop/game_symbols.h"

namespace modlock::gameinterop {

// ModifierStateIndex reads one EModifierState value by its schema name, such as
// "MODIFIER_STATE_DO_NOT_DRAW_MODEL", from the mapped module's own schema enum
// table: the enumerator record that points at the name's string carries its
// value beside the pointer. Game updates renumber these states, so callers
// never hard-code them. A name that is absent or ambiguous is an error.
[[nodiscard]] MODLOCK_API std::expected<uint32_t, std::string> ModifierStateIndex(
    const ModuleImage& image, std::string_view name);

// ModifierStateIndices resolves several names in order; the first failure is
// returned.
[[nodiscard]] MODLOCK_API std::expected<std::vector<uint32_t>, std::string> ModifierStateIndices(
    const ModuleImage& image, std::span<const std::string_view> names);

// ServerModifierStates resolves names against the loaded server.dll.
[[nodiscard]] MODLOCK_API std::expected<std::vector<uint32_t>, std::string> ServerModifierStates(
    std::span<const std::string_view> names);

}  // namespace modlock::gameinterop
