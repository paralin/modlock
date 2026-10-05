#pragma once

#include <expected>
#include <string>
#include <vector>

#include "modlock/gameinterop/game_symbols.h"
#include "proto/modlock/wasm.pb.h"

namespace modlock::wasm {

// TuneAbilities writes a manifest's ability tuning into one game module's
// parsed ability data, which the module keeps for the process lifetime. It
// fails without writing while an ability's data is not loaded yet, so a caller
// retries later; a property or field it cannot resolve is skipped and reported
// in the returned problems.
std::expected<std::vector<std::string>, std::string> TuneAbilities(
    const gameinterop::ModuleImage& module, void* schema, const char* module_name,
    const google::protobuf::RepeatedPtrField<AbilityTuning>& abilities);

}  // namespace modlock::wasm
