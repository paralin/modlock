#include "modlock/gameinterop/projectile_impact_hook.h"

#include <cstdint>

#include "native_trace_layout.h"

#if defined(_WIN32)
#include <safetyhook.hpp>
#endif

namespace modlock::gameinterop {
namespace {

#if defined(_WIN32)
ProjectileImpactHook::Handler* g_handler = nullptr;
safetyhook::InlineHook* g_hook = nullptr;
safetyhook::InlineHook* g_contact_hook = nullptr;
struct Contact {
  void* projectile;
  const GameTrace* trace;
};
Contact g_contact{};

uintptr_t ContactThunk(void* projectile, void* entity, const GameTrace* contact,
                       const TraceVector* velocity, bool trigger) {
  // Hold this contact for the impact the original raises, then restore the outer one.
  const auto previous = g_contact;
  g_contact = {projectile, contact};
  const auto result =
      g_contact_hook->call<uintptr_t>(projectile, entity, contact, velocity, trigger);
  g_contact = previous;
  return result;
}

uintptr_t ImpactThunk(void* projectile, void* entity, void* owner, uintptr_t flags) {
  // The outer collision call owns the contact until this nested callback returns.
  // Impacts without a physical contact retain their native behavior.
  if (g_contact.projectile != projectile) {
    return g_hook->call<uintptr_t>(projectile, entity, owner, flags);
  }
  auto contact = ReadGameTrace(*g_contact.trace);
  contact.entity_handle = ReferenceHandleOf(entity);
  uintptr_t result = 0;
  (*g_handler)(projectile, contact,
               [&] { result = g_hook->call<uintptr_t>(projectile, entity, owner, flags); });
  return result;
}
#endif

}  // namespace

struct ProjectileImpactHook::Impl {
  Handler handler;
#if defined(_WIN32)
  safetyhook::InlineHook hook;
  safetyhook::InlineHook contact_hook;
  ~Impl() {
    // Only the installed hook clears the shared state.
    if (g_hook != &hook) {
      return;
    }
    contact_hook.reset();
    hook.reset();
    g_contact_hook = nullptr;
    g_contact = {};
    g_hook = nullptr;
    g_handler = nullptr;
  }
#endif
};

ProjectileImpactHook::ProjectileImpactHook(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
ProjectileImpactHook::ProjectileImpactHook(ProjectileImpactHook&&) noexcept = default;
ProjectileImpactHook& ProjectileImpactHook::operator=(ProjectileImpactHook&&) noexcept = default;
ProjectileImpactHook::~ProjectileImpactHook() = default;

std::expected<ProjectileImpactHook, std::string> ProjectileImpactHook::Install(
    const ModuleImage& server, Handler handler) {
#if defined(_WIN32)
  // Install once, with a handler.
  if (g_hook || !handler) {
    return std::unexpected("projectile impact hook already installed or empty");
  }

  // Resolve both targets.
  auto target = ResolveSignature(server, "projectile.impact");
  if (!target) {
    return std::unexpected(target.error());
  }
  auto contact = ResolveSignature(server, "projectile.contact");
  if (!contact) {
    return std::unexpected(contact.error());
  }

  // Create both hooks disabled.
  auto impl = std::make_unique<Impl>();
  impl->handler = std::move(handler);
  impl->hook = safetyhook::create_inline(*target, reinterpret_cast<void*>(&ImpactThunk),
                                         safetyhook::InlineHook::StartDisabled);
  if (!impl->hook) {
    return std::unexpected("cannot create native projectile impact hook");
  }
  impl->contact_hook = safetyhook::create_inline(*contact, reinterpret_cast<void*>(&ContactThunk),
                                                 safetyhook::InlineHook::StartDisabled);
  if (!impl->contact_hook) {
    return std::unexpected("cannot create native projectile contact hook");
  }

  // Publish the state the thunks read, then enable.
  g_handler = &impl->handler;
  g_hook = &impl->hook;
  g_contact_hook = &impl->contact_hook;
  if (!impl->hook.enable()) {
    return std::unexpected("cannot enable native projectile impact hook");
  }
  if (!impl->contact_hook.enable()) {
    return std::unexpected("cannot enable native projectile contact hook");
  }
  return ProjectileImpactHook(std::move(impl));

#else
  // Other platforms have no native server.
  (void)server;
  (void)handler;
  return std::unexpected("native projectile impact observation requires Windows");
#endif
}

}  // namespace modlock::gameinterop
