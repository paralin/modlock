#include "modlock/gameinterop/player_selection.h"

namespace modlock::gameinterop {

std::expected<PlayerSelectionCalls, std::string> PlayerSelectionCalls::Resolve(
    const ModuleImage& server) {
  PlayerSelectionCalls calls;
  struct Entry {
    std::string_view id;
    void** slot;
  };
  const Entry entries[] = {
      {"controller.create-hero-pawn", reinterpret_cast<void**>(&calls.create_pawn)},
      {"pawn.select-hero-internal", reinterpret_cast<void**>(&calls.select_hero)},
      {"controller.spawn-observer", reinterpret_cast<void**>(&calls.spawn_observer)},
      {"controller.set-pawn", reinterpret_cast<void**>(&calls.set_pawn)},
  };
  for (const auto& entry : entries) {
    auto address = ResolveSignature(server, entry.id);
    if (!address) return std::unexpected(address.error());
    *entry.slot = *address;
  }
  return calls;
}

std::expected<ResetHeroPawn, std::string> ResolveResetHeroPawn(const ModuleImage& server) {
  auto target = ResolveSignature(server, "pawn.reset-hero");
  if (!target) return std::unexpected(target.error());
  return reinterpret_cast<ResetHeroPawn>(*target);
}

std::expected<RespawnPawn, std::string> ResolveRespawnPawn(const ModuleImage& server) {
  // CCitadelPlayerController's respawn command calls this on m_hHeroPawn with true.
  auto target = ResolveSignature(server, "pawn.respawn");
  if (!target) return std::unexpected(target.error());
  return reinterpret_cast<RespawnPawn>(*target);
}

}  // namespace modlock::gameinterop
