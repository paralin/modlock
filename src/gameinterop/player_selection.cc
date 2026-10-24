#include "modlock/gameinterop/player_selection.h"

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

}  // namespace modlock::gameinterop
