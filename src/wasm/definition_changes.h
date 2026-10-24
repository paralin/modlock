#pragma once

#include <expected>
#include <string>
#include <vector>

#include "modlock/gameinterop/game_symbols.h"
#include "src/wasm/manifest/manifest.pb.h"

namespace modlock::wasm {

// ChangeDefinitions writes a manifest's definition changes into one game
// module's parsed definitions. Ability definitions last for the process;
// hero definitions belong to the current world, so a caller applies the
// changes again in each world. It fails without writing while an entry is not
// loaded yet, so a caller retries later. An entry, property or field it can
// never resolve is skipped and reported in the returned problems.
std::expected<std::vector<std::string>, std::string> ChangeDefinitions(
    const gameinterop::ModuleImage& module, void* schema, const char* module_name,
    const google::protobuf::RepeatedPtrField<manifest::Definition>& definitions);

}  // namespace modlock::wasm
