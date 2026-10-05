#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "proto/modlock/wasm.pb.h"

namespace modlock::wasm {

// FindSetting returns the setting named key that manifest declares, or null.
const Setting* FindSetting(const Manifest& manifest, std::string_view key);

// Canonical returns value as the setting holds it, or nothing when the
// setting does not accept it: a choice's value, "true" or "false", or a
// number from min to max on a step from min.
std::optional<std::string> Canonical(const Setting& setting, std::string_view value);

// DefaultValue returns the value a player starts with: the declared default,
// or the first choice, off or min when the setting declares none.
std::string DefaultValue(const Setting& setting);

}  // namespace modlock::wasm
