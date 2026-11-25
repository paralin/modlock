#include "modlock/gameinterop/player_selection.h"

#include <cstdint>
#include <cstring>

namespace modlock::gameinterop {

std::expected<PlayerSelectionCalls, std::string> PlayerSelectionCalls::Resolve(
    const ModuleImage& server) {
  // Resolve each call by signature into its slot.
  PlayerSelectionCalls calls;

  // Fill every call, or fail at the first missing one.
  const SignatureCall signatures[] = {
      {"controller.create-hero-pawn", &calls.create_pawn},
      {"pawn.select-hero-internal", &calls.select_hero},
      {"controller.spawn-observer", &calls.spawn_observer},
      {"controller.set-pawn", &calls.set_pawn},
  };
  auto resolved = ResolveSignatures(server, signatures);
  if (!resolved) {
    return std::unexpected(resolved.error());
  }

  // Removal is optional: without it a stripped hero stays in the world.
  if (auto remove = ResolveSignature(server, "entity.remove")) {
    calls.remove = reinterpret_cast<void (*)(void*)>(*remove);
  }
  return calls;
}

std::expected<ResetHeroPawn, std::string> ResolveResetHeroPawn(const ModuleImage& server) {
  auto target = ResolveSignature(server, "pawn.reset-hero");
  if (!target) {
    return std::unexpected(target.error());
  }
  return reinterpret_cast<ResetHeroPawn>(*target);
}

std::expected<RespawnPawn, std::string> ResolveRespawnPawn(const ModuleImage& server) {
  // CCitadelPlayerController's respawn command calls this on m_hHeroPawn with true.
  auto target = ResolveSignature(server, "pawn.respawn");
  if (!target) {
    return std::unexpected(target.error());
  }
  return reinterpret_cast<RespawnPawn>(*target);
}

std::expected<ChangeTeamKeepingHero, std::string> ResolveChangeTeamKeepingHero(
    const ModuleImage& server) {
  // Resolve the two native transitions before exposing team changes.
  auto target = ResolveSignature(server, "controller.change-team-keeping-hero");
  if (!target) {
    return std::unexpected(target.error());
  }
  auto pawn = ResolveSignature(server, "pawn.change-team");
  if (!pawn) {
    return std::unexpected(pawn.error());
  }

  // The matched wrapper clears and restores one controller flag. Both stores
  // must name the same byte before the pawn transition can share its guard.
  const auto* code = static_cast<const unsigned char*>(*target);
  uint32_t clear = 0;
  uint32_t restore = 0;
  std::memcpy(&clear, code + 14, sizeof(clear));
  std::memcpy(&restore, code + 27, sizeof(restore));
  if (clear != restore) {
    return std::unexpected("keep-hero wrapper uses different controller flags");
  }
  return ChangeTeamKeepingHero{
      .controller = reinterpret_cast<void (*)(void*, int)>(*target),
      .pawn = reinterpret_cast<void (*)(void*, int)>(*pawn),
      .keep_hero = clear,
  };
}

void ChangeTeamKeepingHero::Move(void* controller_entity, void* pawn_entity, int team) const {
  // The controller path updates its team lists and ordinarily moves its pawn.
  controller(controller_entity, team);

  // A controller already on this team skips the pawn. Run its own native team
  // transition under the same teardown flag as the game's keep-hero wrapper.
  auto* flag = static_cast<unsigned char*>(controller_entity) + keep_hero;
  const unsigned char previous = *flag;
  *flag = 0;
  pawn(pawn_entity, team);
  *flag = previous;
}

}  // namespace modlock::gameinterop
