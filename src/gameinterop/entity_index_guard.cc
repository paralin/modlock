#include "modlock/gameinterop/entity_index_guard.h"

#include <cstdint>
#include <utility>

#if defined(_WIN32)
#include <safetyhook.hpp>
#endif

namespace modlock::gameinterop {
namespace {

#if defined(_WIN32)
safetyhook::InlineHook* g_hook = nullptr;

// IndexThunk answers a null entity as the getter answers an entity without an
// identity, and leaves every other call to the native getter. The getter runs
// for most entity handle lookups, so the check stays a single branch.
int32_t* IndexThunk(void* entity, int32_t* index) {
  if (!entity) {
    *index = -1;
    return index;
  }
  return g_hook->call<int32_t*>(entity, index);
}
#endif

}  // namespace

struct EntityIndexGuard::Impl {
#if defined(_WIN32)
  safetyhook::InlineHook hook;

  ~Impl() {
    if (g_hook != &hook) {
      return;
    }
    hook.reset();
    g_hook = nullptr;
  }
#endif
};

EntityIndexGuard::EntityIndexGuard(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

EntityIndexGuard::EntityIndexGuard(EntityIndexGuard&&) noexcept = default;
EntityIndexGuard& EntityIndexGuard::operator=(EntityIndexGuard&&) noexcept = default;
EntityIndexGuard::~EntityIndexGuard() = default;

std::expected<EntityIndexGuard, std::string> EntityIndexGuard::Install(const ModuleImage& client) {
#if defined(_WIN32)
  // Install once.
  if (g_hook) {
    return std::unexpected("entity index guard is already installed");
  }

  // Resolve the getter and create the hook disabled.
  auto target = ResolveSignature(client, "entity.index-of");
  if (!target) {
    return std::unexpected(target.error());
  }
  auto hook = safetyhook::create_inline(*target, reinterpret_cast<void*>(&IndexThunk),
                                        safetyhook::InlineHook::StartDisabled);
  if (!hook) {
    return std::unexpected("entity index guard hook creation failed");
  }

  // Publish the thunk state and enable.
  auto impl = std::make_unique<Impl>();
  impl->hook = std::move(hook);
  g_hook = &impl->hook;
  if (auto enabled = impl->hook.enable(); !enabled) {
    return std::unexpected("entity index guard hook activation failed");
  }
  return EntityIndexGuard(std::move(impl));

#else
  // Other platforms have no native client.
  (void)client;
  return std::unexpected("the entity index guard requires the Windows host");
#endif
}

}  // namespace modlock::gameinterop
