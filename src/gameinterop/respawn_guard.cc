#include "modlock/gameinterop/respawn_guard.h"

#include "modlock/gameinterop/entity_abi.h"
#include "modlock/gameinterop/player_selection.h"

#if defined(_WIN32)
#include <safetyhook.hpp>
#endif

namespace modlock::gameinterop {
namespace {

#if defined(_WIN32)
safetyhook::InlineHook* g_hook = nullptr;
RespawnGuard::Blocked* g_blocked = nullptr;

void RespawnThunk(void* pawn, bool force) {
  if (const auto handle = ReferenceHandleOf(pawn); handle && (*g_blocked)(*handle)) {
    return;
  }
  g_hook->call<void>(pawn, force);
}
#endif

}  // namespace

struct RespawnGuard::Impl {
#if defined(_WIN32)
  safetyhook::InlineHook hook;
  Blocked blocked;

  ~Impl() {
    if (g_hook != &hook) {
      return;
    }
    hook.reset();
    g_hook = nullptr;
    g_blocked = nullptr;
  }
#endif
};

RespawnGuard::RespawnGuard(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
RespawnGuard::RespawnGuard(RespawnGuard&&) noexcept = default;
RespawnGuard& RespawnGuard::operator=(RespawnGuard&&) noexcept = default;
RespawnGuard::~RespawnGuard() = default;

std::expected<RespawnGuard, std::string> RespawnGuard::Install(const ModuleImage& server,
                                                               Blocked blocked) {
#if defined(_WIN32)
  // Install once.
  if (g_hook || !blocked) {
    return std::unexpected("respawn guard is already installed or unbound");
  }

  // Resolve the target and create the hook disabled.
  auto target = ResolveRespawnPawn(server);
  if (!target) {
    return std::unexpected(target.error());
  }
  auto hook = safetyhook::create_inline(reinterpret_cast<void*>(*target),
                                        reinterpret_cast<void*>(&RespawnThunk),
                                        safetyhook::InlineHook::StartDisabled);
  if (!hook) {
    return std::unexpected("respawn guard hook creation failed");
  }

  // Publish the thunk state and enable.
  auto impl = std::make_unique<Impl>();
  impl->hook = std::move(hook);
  impl->blocked = std::move(blocked);
  g_hook = &impl->hook;
  g_blocked = &impl->blocked;
  if (auto enabled = impl->hook.enable(); !enabled) {
    return std::unexpected("respawn guard hook activation failed");
  }
  return RespawnGuard(std::move(impl));

#else
  // Other platforms have no native server.
  (void)server;
  (void)blocked;
  return std::unexpected("native respawn control requires the Windows host");
#endif
}

}  // namespace modlock::gameinterop
