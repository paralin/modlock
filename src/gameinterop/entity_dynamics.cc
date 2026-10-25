#include "modlock/gameinterop/entity_dynamics.h"

#include "modlock/gameinterop/game_symbols.h"

namespace modlock::gameinterop {

std::expected<EntityDynamics, std::string> EntityDynamics::Resolve(const ModuleImage& server) {
  EntityDynamics dynamics{};
  const SignatureCall signatures[] = {
      {"entity.abs-velocity", &dynamics.velocity},
      {"entity.apply-abs-velocity-impulse", &dynamics.push},
      {"entity.apply-local-angular-velocity-impulse", &dynamics.spin},
  };
  if (auto resolved = ResolveSignatures(server, signatures); !resolved) {
    return std::unexpected(resolved.error());
  }
  return dynamics;
}

}  // namespace modlock::gameinterop
