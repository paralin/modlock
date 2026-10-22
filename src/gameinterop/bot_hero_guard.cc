#include "modlock/gameinterop/bot_hero_guard.h"

#include <utility>

#include "modlock/gameinterop/console_variables.h"

#if defined(_WIN32)
#include <safetyhook.hpp>
#endif

namespace modlock::gameinterop {
namespace {

#if defined(_WIN32)
safetyhook::MidHook* g_hook = nullptr;
ConsoleVariables* g_variables = nullptr;

// ClearSoloCheck runs at the TEST AL,AL that follows the brain's solo bot
// match check, before the human hero comparison. A false result there skips
// the comparison and leaves the bot's hero alone.
void ClearSoloCheck(safetyhook::Context& context) {
  const auto allowed = g_variables->ReadBool("citadel_allow_duplicate_heroes");
  if (allowed && *allowed) {
    context.rax &= ~uintptr_t{0xff};
  }
}
#endif

}  // namespace

struct BotHeroGuard::Impl {
#if defined(_WIN32)
  safetyhook::MidHook hook;
  ConsoleVariables variables;

  ~Impl() {
    if (g_hook != &hook) {
      return;
    }
    hook.reset();
    g_hook = nullptr;
    g_variables = nullptr;
  }
#endif
};

BotHeroGuard::BotHeroGuard(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
BotHeroGuard::BotHeroGuard(BotHeroGuard&&) noexcept = default;
BotHeroGuard& BotHeroGuard::operator=(BotHeroGuard&&) noexcept = default;
BotHeroGuard::~BotHeroGuard() = default;

std::expected<BotHeroGuard, std::string> BotHeroGuard::Install(const ModuleImage& server) {
#if defined(_WIN32)
  // Install once.
  if (g_hook) {
    return std::unexpected("bot hero guard is already installed");
  }

  // Resolve the variables and the check, and create the hook disabled.
  auto variables = ConsoleVariables::Resolve();
  if (!variables) {
    return std::unexpected(variables.error());
  }
  auto target = ResolveSignature(server, "bot.duplicate-hero-check");
  if (!target) {
    return std::unexpected(target.error());
  }
  auto hook = safetyhook::create_mid(*target, &ClearSoloCheck, safetyhook::MidHook::StartDisabled);
  if (!hook) {
    return std::unexpected("bot hero guard hook creation failed");
  }

  // Publish the thunk state and enable.
  auto impl = std::make_unique<Impl>();
  impl->hook = std::move(hook);
  impl->variables = *variables;
  g_hook = &impl->hook;
  g_variables = &impl->variables;
  if (auto enabled = impl->hook.enable(); !enabled) {
    return std::unexpected("bot hero guard hook activation failed");
  }
  return BotHeroGuard(std::move(impl));

#else
  // Other platforms have no native server.
  (void)server;
  return std::unexpected("the bot hero guard requires the Windows host");
#endif
}

}  // namespace modlock::gameinterop
