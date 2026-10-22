#include "modlock/gameinterop/server_addons.h"

#include <cstddef>
#include <utility>

#include "modlock/gameinterop/mapped_module_image.h"

#if defined(_WIN32)
#include <safetyhook.hpp>
#endif

namespace modlock::gameinterop {
namespace {
#if defined(_WIN32)
// kAddonsOffset is the server's addons CUtlString, whose only field is its
// character pointer. ReplyConnection copies it into the connection reply.
// Build 6723 moved it from 0x158: ReplyConnection (engine2 RVA 0xa0d20) reads
// [server+0x178] twice and substitutes "" when it is null.
constexpr size_t kAddonsOffset = 0x178;

safetyhook::InlineHook* g_detour = nullptr;
const char* g_addons = nullptr;

// ReplyThunk lends the advertised string for one reply. Swapping the pointer
// leaves the engine's allocation untouched and never frees across allocators.
void ReplyThunk(void* server, void* client) {
  // Serve this addon list for the reply, then restore the original.
  auto* addons = reinterpret_cast<const char**>(static_cast<std::byte*>(server) + kAddonsOffset);
  const char* original = *addons;
  *addons = g_addons;
  g_detour->call<void>(server, client);
  *addons = original;
}
#endif
}  // namespace

struct ServerAddonsHook::Impl {
  // addons is the advertised list; the thunk borrows its characters.
  std::string addons;
#if defined(_WIN32)
  safetyhook::InlineHook detour;
#endif
  ~Impl() {
#if defined(_WIN32)
    if (g_detour != &detour) {
      return;
    }
    detour.reset();
    g_detour = nullptr;
    g_addons = nullptr;
#endif
  }
};

ServerAddonsHook::ServerAddonsHook(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
ServerAddonsHook::ServerAddonsHook(ServerAddonsHook&&) noexcept = default;
ServerAddonsHook& ServerAddonsHook::operator=(ServerAddonsHook&&) noexcept = default;
ServerAddonsHook::~ServerAddonsHook() = default;

std::expected<ServerAddonsHook, std::string> ServerAddonsHook::Install(std::string addons) {
#if defined(_WIN32)
  // Install once.
  if (g_detour != nullptr) {
    return std::unexpected("server addons hook already installed");
  }

  // Resolve the reply function in the engine.
  const auto image = MappedModuleImage::ForModule(L"engine2.dll");
  if (!image) {
    return std::unexpected(image.error());
  }
  auto target = ResolveSignature(*image, "server.reply-connection");
  if (!target) {
    return std::unexpected(target.error());
  }

  // Create the hook disabled.
  auto impl = std::make_unique<Impl>();
  impl->addons = std::move(addons);
  impl->detour = safetyhook::create_inline(*target, reinterpret_cast<void*>(&ReplyThunk),
                                           safetyhook::InlineHook::StartDisabled);
  if (!impl->detour) {
    return std::unexpected("cannot create server addons hook");
  }

  // Publish the thunk state and enable.
  g_addons = impl->addons.c_str();
  g_detour = &impl->detour;
  if (!impl->detour.enable()) {
    return std::unexpected("cannot enable server addons hook");
  }
  return ServerAddonsHook(std::move(impl));

#else
  // Other platforms have no native server.
  (void)addons;
  return std::unexpected("server addons require the Windows host build");
#endif
}

}  // namespace modlock::gameinterop
