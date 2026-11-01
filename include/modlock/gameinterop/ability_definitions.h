#pragma once

#include <cstdint>
#include <expected>
#include <string>

#include "modlock/export.h"
#include "modlock/gameinterop/game_symbols.h"

namespace modlock::gameinterop {

// Resolves subclass IDs through the engine's current subclass registry, in
// one scope: abilities and items by default, or NPC units such as the bot
// brain. The mapped server module owns the function and returned definition
// storage; callers discard borrowed definitions on world shutdown.
class MODLOCK_API AbilityDefinitions {
 public:
  using Lookup = void* (*)(int32_t scope, uint32_t subclass_id);

  // Scope is EntitySubclassScope_t: the vdata file a subclass comes from.
  enum Scope : int32_t {
    kNpcUnits = 3,   // scripts/npc_units.vdata
    kAbilities = 4,  // scripts/abilities.vdata
  };

  explicit AbilityDefinitions(Lookup lookup, Scope scope = kAbilities)
      : lookup_(lookup), scope_(scope) {}

  static std::expected<AbilityDefinitions, std::string> Resolve(const ModuleImage& server,
                                                                Scope scope = kAbilities);

  struct Definition {
    std::string name;
    bool disabled;
    // native_definition_pointer is borrowed from the mapped engine registry.
    void* native_definition_pointer;
  };

  // Missing, disabled and enabled definitions remain distinct. This lookup
  // grants neither ownership nor permission to claim an item was restored.
  std::expected<Definition, std::string> Find(uint32_t subclass_id) const;

 private:
  Lookup lookup_;
  Scope scope_;
};

}  // namespace modlock::gameinterop
