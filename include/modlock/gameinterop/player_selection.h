#pragma once

#include "modlock/export.h"
#include "modlock/gameinterop/hero_definitions.h"

namespace modlock::gameinterop {

// These calls mirror NativeHero and NativeCallbacks' connected-player paths.
// Selection is separate from hero lookup and from client-less ghost creation.
struct MODLOCK_API PlayerSelectionCalls {
  void* (*create_pawn)(void* controller, int team) = nullptr;
  void (*select_hero)(void* pawn, void* definition) = nullptr;
  void* (*spawn_observer)(void* controller) = nullptr;
  void (*set_pawn)(void* controller, void* pawn, bool retain_old_pawn_team,
                   bool copy_movement_state, bool allow_team_mismatch,
                   bool preserve_movement_state) = nullptr;
  static std::expected<PlayerSelectionCalls, std::string> Resolve(const ModuleImage& server);
};

// ResetHeroPawn rebuilds the current hero with fresh native equipment and progression.
using ResetHeroPawn = int64_t (*)(void* pawn, bool reset_abilities);
MODLOCK_API std::expected<ResetHeroPawn, std::string> ResolveResetHeroPawn(
    const ModuleImage& server);

using RespawnPawn = void (*)(void* pawn, bool force);
MODLOCK_API std::expected<RespawnPawn, std::string> ResolveRespawnPawn(const ModuleImage& server);

}  // namespace modlock::gameinterop
