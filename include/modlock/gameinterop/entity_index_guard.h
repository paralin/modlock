#pragma once

#include <expected>
#include <memory>
#include <string>

#include "modlock/export.h"
#include "modlock/gameinterop/game_symbols.h"

namespace modlock::gameinterop {

// EntityIndexGuard makes the client's entity index getter safe for a null
// entity. The getter writes an entity's index through its second argument and
// writes -1 when the entity has no identity or is not in the entity list, so
// every caller already handles -1; it only forgets to check the entity itself.
// Callers that resolve an entity handle pass the resolver's result straight
// through, and a handle that does not resolve on this client, such as an
// ability whose owner the client has not received, dereferences null and
// crashes the process. With the guard installed a null entity reports -1, the
// same answer as an entity with no identity. Install on the engine thread
// after the client module loads and retain until the engine stops; only one
// guard may be installed.
class MODLOCK_API EntityIndexGuard {
 public:
  static std::expected<EntityIndexGuard, std::string> Install(const ModuleImage& client);
  EntityIndexGuard(EntityIndexGuard&&) noexcept;
  EntityIndexGuard& operator=(EntityIndexGuard&&) noexcept;
  ~EntityIndexGuard();

 private:
  struct Impl;
  explicit EntityIndexGuard(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace modlock::gameinterop
