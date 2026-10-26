#pragma once

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>

#include "modlock/export.h"
#include "modlock/gameinterop/game_symbols.h"
#include "modlock/gameinterop/keyvalues.h"
#include "modlock/render/world_text_seam.h"

namespace modlock::render {

// WorldTextGameCalls is the resolved raw-ABI surface of point_worldtext
// creation. Shared CEntityKeyValues resolution refuses absent or ambiguous
// native functions.
struct MODLOCK_API WorldTextGameCalls {
  // CEntitySystem::CreateEntityByName. The implementation reads the global
  // entity system internally and ignores the this argument; callers pass
  // nullptr and the name rides the second register
  // (EntitySystemHelper::CreateEntityByName).
  void* (*create_entity_by_name)(void* ignored_this, const char* class_name,
                                 int force_edict_index) = nullptr;
  // Live CGameEntitySystem for the queued-creation pair; wired through
  // SetEntitySystem before creating any text.
  void* entity_system = nullptr;
  // CEntitySystem::QueueSpawnEntity(entity_system, identity, key_values).
  void (*queue_spawn_entity)(void* entity_system, void* identity, void* key_values) = nullptr;
  // CEntitySystem::ExecuteQueuedCreation(entity_system).
  void (*execute_queued_creation)(void* entity_system) = nullptr;
  // CEntityInstance::AcceptInput enables point_worldtext after queued creation
  // and changes its text with the SetMessage input.
  bool (*accept_input)(void* entity, const char* input_name, void* activator, void* caller,
                       void* value, int output_id, void* unknown) = nullptr;
  // Writes CPointWorldText::m_messageText before the new entity's first
  // network snapshot. The live implementation resolves the network string
  // through the engine schema instead of guessing a second input ABI.
  bool (*write_message)(void* entity, const char* message) = nullptr;
  // Shared source-matched CEntityKeyValues construction boundary.
  modlock::gameinterop::KeyValuesCalls key_values;
  // UTIL_Remove; tolerates nullptr per its own prologue.
  void (*util_remove)(void* entity) = nullptr;
};

// WorldTextGameFactory is the live WorldTextEntityFactory over scanned
// server.dll symbols. Creation requires:
// reorient_mode 1, roll-90 plus manual yaw carried by the caller's angles,
// keyvalues applied before spawn, and a
// Teleport (vtable slot 163) to origin ahead of queueing. Every failure path
// returns an error naming the failed game step; nothing here throws or crashes
// the host on its own.
class MODLOCK_API WorldTextGameFactory final : public WorldTextEntityFactory {
 public:
  // TryCreate resolves every recorded world-text signature against
  // server and returns the factory, or an error naming the first unresolved
  // world-text or keyvalues symbol.
  static std::expected<std::unique_ptr<WorldTextGameFactory>, std::string> TryCreate(
      const modlock::gameinterop::ModuleImage& server);

  // ForCalls builds the factory over caller-supplied raw calls; TryCreate is
  // the production path, tests bind recording fakes here.
  WorldTextGameFactory(WorldTextGameCalls calls);

  ~WorldTextGameFactory() override;

  // SetEntitySystem wires the live CGameEntitySystem pointer the
  // queued-creation pair consumes.
  void SetEntitySystem(void* entity_system) { calls_.entity_system = entity_system; }

  std::expected<std::unique_ptr<WorldTextEntity>, std::string> Create(
      std::string_view message, const modlock::geometry::Vec3& origin,
      const modlock::geometry::EulerAngles& angles, const WorldTextStyle& style) override;

 private:
  friend class GameWorldTextEntity;

  void TraceFailure(std::string_view message);
  void TraceSuccess();

  WorldTextGameCalls calls_;
  std::set<std::string> trace_failures_;
  bool trace_success_logged_ = false;
};

}  // namespace modlock::render
