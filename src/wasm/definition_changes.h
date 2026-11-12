#pragma once

#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include "modlock/gameinterop/game_symbols.h"
#include "src/wasm/manifest.pb.h"

namespace modlock::wasm {

// DefinitionUndo keeps the game's own values that definition changes
// overwrote, so a mod's changes leave with the mod: ability, modifier, unit
// and bot difficulty definitions last for the process and would otherwise
// carry into the next mod the server runs. It is not thread safe; it is used
// on the thread that changes the definitions.
class DefinitionUndo {
 public:
  // Save keeps the size bytes at storage before a change writes them. Only
  // the first value kept for a place counts, so writing the changes again in
  // a later world keeps the game's own. world marks hero data, which belongs
  // to the current world.
  void Save(unsigned char* storage, size_t size, bool world);

  // Restore writes every kept value back and forgets them. The module that
  // holds them must still be loaded.
  void Restore();

  // ForgetWorld drops the kept hero values, whose storage ends with the world.
  void ForgetWorld();

 private:
  struct Saved {
    unsigned char* storage;
    std::vector<unsigned char> value;
    bool world;
  };

  std::vector<Saved> saved_;
};

// ChangeDefinition applies one definition's properties, fields, copies and
// text fields to definition, keeping each overwritten value in undo, and
// returns its problems. Each {addon} in a text field becomes addon when addon
// is not empty.
std::vector<std::string> ChangeDefinition(const wasm::Definition& change, unsigned char* definition,
                                          void* schema, const char* module_name,
                                          DefinitionUndo& undo, std::string_view addon = {});

// ChangeDefinitions writes a manifest's definition changes into one game
// module's parsed definitions, keeping each value it overwrites in undo.
// Ability, modifier, unit and bot difficulty definitions last for the process;
// hero definitions belong to the current world, so a caller applies the changes
// again in each world. Each {addon} in a text field becomes addon when addon
// is not empty. It fails without writing while an entry is not loaded yet, so
// a caller retries later. An entry, property or field it can never resolve is
// skipped and reported in the returned problems.
std::expected<std::vector<std::string>, std::string> ChangeDefinitions(
    const gameinterop::ModuleImage& module, void* schema, const char* module_name,
    const google::protobuf::RepeatedPtrField<wasm::Definition>& definitions, DefinitionUndo& undo,
    std::string_view addon = {});

}  // namespace modlock::wasm
