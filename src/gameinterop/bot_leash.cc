#include "modlock/gameinterop/bot_leash.h"

#include <cstddef>
#include <cstring>
#include <utility>

#include "modlock/gameinterop/entity_abi.h"

#if defined(_WIN32)
#include <safetyhook.hpp>
#endif

namespace modlock::gameinterop {
namespace {

#if defined(_WIN32)
// kBrainPawnOffset is the bot brain's hero pawn pointer. Build 6839: the
// schedule selector (server RVA 0x83cd30) calls the alive test through
// [brain+0x1868], and the retreat check passes it to its zipline route search.
constexpr size_t kBrainPawnOffset = 0x1868;

safetyhook::InlineHook* g_hook = nullptr;
BotLeash::Decide* g_decide = nullptr;
// g_walk is the SCHED_BOT_RETREAT_WALK symbol global; its value is read at
// each call, since the server registers it at static initialization.
const uint64_t* g_walk = nullptr;

// RetreatThunk stands in for the brain's retreat check, which writes the move
// goal and the schedule symbol and reports whether the bot retreats.
bool RetreatThunk(void* brain, float* goal, uint64_t* schedule) {
  void* pawn = nullptr;
  std::memcpy(&pawn, static_cast<std::byte*>(brain) + kBrainPawnOffset, sizeof(pawn));
  const auto handle = pawn ? ReferenceHandleOf(pawn) : std::nullopt;
  if (const auto walk = handle ? (*g_decide)(*handle) : std::nullopt) {
    std::memcpy(goal, walk->data(), sizeof(*walk));
    *schedule = *g_walk;
    return true;
  }
  return g_hook->call<bool>(brain, goal, schedule);
}
#endif

}  // namespace

struct BotLeash::Impl {
#if defined(_WIN32)
  safetyhook::InlineHook hook;
  Decide decide;

  ~Impl() {
    if (g_hook != &hook) return;
    hook.reset();
    g_hook = nullptr;
    g_decide = nullptr;
    g_walk = nullptr;
  }
#endif
};

BotLeash::BotLeash(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
BotLeash::BotLeash(BotLeash&&) noexcept = default;
BotLeash& BotLeash::operator=(BotLeash&&) noexcept = default;
BotLeash::~BotLeash() = default;

std::expected<BotLeash, std::string> BotLeash::Install(const ModuleImage& server,
                                                       Decide decide) {
#if defined(_WIN32)
  if (g_hook || !decide) return std::unexpected("bot leash is already installed or unbound");
  auto target = ResolveSignature(server, "bot.retreat-check");
  if (!target) return std::unexpected(target.error());
  auto walk = ResolveSignature(server, "bot.schedule-retreat-walk");
  if (!walk) return std::unexpected(walk.error());
  auto hook = safetyhook::create_inline(*target, reinterpret_cast<void*>(&RetreatThunk),
                                        safetyhook::InlineHook::StartDisabled);
  if (!hook) return std::unexpected("bot leash hook creation failed");
  auto impl = std::make_unique<Impl>();
  impl->hook = std::move(hook);
  impl->decide = std::move(decide);
  g_hook = &impl->hook;
  g_decide = &impl->decide;
  g_walk = static_cast<const uint64_t*>(*walk);
  if (auto enabled = impl->hook.enable(); !enabled)
    return std::unexpected("bot leash hook activation failed");
  return BotLeash(std::move(impl));
#else
  (void)server;
  (void)decide;
  return std::unexpected("native bot control requires the Windows host");
#endif
}

}  // namespace modlock::gameinterop
