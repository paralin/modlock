#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

#include "modlock/export.h"
#include "modlock/gameinterop/ability_definitions.h"
#include "modlock/gameinterop/entity_abi.h"
#include "modlock/gameinterop/game_symbols.h"
#include "modlock/gameinterop/pawn_observer.h"

namespace modlock::gameinterop {

// AbilityModifier attaches a modifier definition to a player's hero or a live
// entity for a fixed duration. The definition belongs to the loaded world and
// is shared with every other user of that modifier, so its duration applies to
// them too. Calls run on the engine thread; entity pointers are borrowed only
// for one call.
class MODLOCK_API AbilityModifier {
 public:
  using AddModifier = void* (*)(void*, void*, uint32_t, uint32_t, void*, void*, void*);
  // RemoveModifier marks the named modifier for native removal, with an
  // optional caster filter and the game's removal reason.
  using RemoveModifier = bool (*)(void* property, uint32_t subclass, void* caster, int32_t reason);

  // Calls holds the native calls and schema offsets every modifier shares.
  // They belong to the server module, not the loaded world, so a host resolves
  // them once and keeps them while server.dll stays loaded.
  struct Calls {
    AbilityDefinitions::Lookup lookup = nullptr;
    AddModifier add = nullptr;
    // remove is optional for one-shot attachments; holds require cleanup.
    RemoveModifier remove = nullptr;
    size_t duration = 0;
    size_t property = 0;
    size_t modifiers = 0;
    size_t predicted = 0;
    size_t life_state = 0;
  };

  // ResolveCalls scans server for the definition lookup and AddModifier, and
  // reads the modifier layout from the schema.
  static std::expected<Calls, std::string> ResolveCalls(const ModuleImage& server, void* schema);
  static std::expected<Calls, std::string> ResolveCalls(AbilityDefinitions::Lookup lookup,
                                                        AddModifier add, void* schema);

  // Resolve finds the "<ability>/<modifier>" or standalone modifier definition
  // and sets its duration; a negative duration never expires. The overload
  // that takes Calls does no signature scan or schema read.
  static std::expected<AbilityModifier, std::string> Resolve(const Calls& calls,
                                                             std::string_view key, float duration);
  static std::expected<AbilityModifier, std::string> Resolve(const ModuleImage& server,
                                                             void* schema, std::string_view key,
                                                             float duration);
  static std::expected<AbilityModifier, std::string> Resolve(AbilityDefinitions::Lookup lookup,
                                                             AddModifier add, void* schema,
                                                             std::string_view key, float duration);

  // Attach adds the modifier to a living player's pawn, sourced from the owned
  // ability with the given subclass identifier, through AttachEntity.
  std::expected<void, std::string> Attach(PawnObserver& observer, int32_t slot,
                                          uint32_t ability) const;

  // Attached reports whether a living player's pawn carries the modifier.
  std::expected<bool, std::string> Attached(PawnObserver& observer, int32_t slot) const;

  // AttachEntity adds the modifier to a live CBaseEntity, optionally sourced
  // from an ability entity handle.
  std::expected<void, std::string> AttachEntity(
      void* entity, uint32_t ability_handle = kInvalidEHandleIndex) const;

  // AttachedToEntity reports whether entity's modifier property carries the
  // modifier.
  std::expected<bool, std::string> AttachedToEntity(const void* entity) const;

  // RemoveFromEntity removes this definition through the game's modifier
  // property, including native callbacks and replication. Absence is success;
  // the engine finishes deleting the marked modifier on its next update.
  std::expected<void, std::string> RemoveFromEntity(void* entity) const;

  // EntityAlive reports whether entity's m_lifeState marks it alive.
  bool EntityAlive(const void* entity) const;

  // Restore gives the definition back the duration it had before Resolve.
  void Restore() const;

 private:
  // LivePawn returns slot's pawn while its hero is alive, or null.
  static void* LivePawn(PawnObserver& observer, int32_t slot);

  // EntityProperty returns the modifier property of entity.
  std::expected<unsigned char*, std::string> EntityProperty(const void* entity) const;

  // HasDefinition reports whether property's modifier vector holds definition_.
  std::expected<bool, std::string> HasDefinition(const unsigned char* property) const;

  AddModifier add_ = nullptr;
  RemoveModifier remove_ = nullptr;
  uint32_t subclass_ = 0;
  unsigned char* definition_ = nullptr;
  uint32_t ability_subclass_ = 0;
  size_t duration_ = 0;
  float previous_ = 0;
  size_t property_ = 0;
  size_t modifiers_ = 0;
  size_t predicted_ = 0;
  size_t life_state_ = 0;
};

}  // namespace modlock::gameinterop
