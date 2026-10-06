#include "modlock/gameinterop/hero_initialization.h"

#include <cstdint>
#include <utility>

#include "modlock/gameinterop/entity_abi.h"

#if defined(_WIN32)
#include <windows.h>

#include <safetyhook.hpp>
#endif

namespace modlock::gameinterop {
namespace {

#if defined(_WIN32)
safetyhook::InlineHook* g_hook = nullptr;
HeroInitializationHook::Handler g_handler;

__int64 __fastcall InitializeHeroThunk(void* pawn, char wipe_items) {
  const __int64 result = g_hook->call<__int64>(pawn, wipe_items);
  if (g_handler && pawn) {
    g_handler(pawn, ConnectionTracker::StateForSlot(0));
  }
  return result;
}
#endif

}  // namespace

bool HeroReadiness::Publish(void* pawn, ConnectionTracker::SlotState slot_state) {
  if (!slot_state.occupied || slot_state.xuid == 0) {
    return false;
  }
  const auto handle = ReferenceHandleOf(pawn);
  if (!handle.has_value()) {
    return false;
  }
  std::lock_guard lock(mutex_);
  std::erase_if(pending_, [&](const HeroReadinessToken& token) {
    return token.steam_id == slot_state.xuid &&
           token.connection_generation != slot_state.generation;
  });
  pending_.push_back(HeroReadinessToken{.pawn_handle = *handle,
                                        .steam_id = slot_state.xuid,
                                        .connection_generation = slot_state.generation});
  constexpr size_t kMaxPendingSignals = 16;
  if (pending_.size() > kMaxPendingSignals) {
    pending_.pop_front();
  }
  return true;
}

bool HeroReadiness::Consume(uint32_t pawn_handle, uint64_t steam_id,
                            uint32_t connection_generation) {
  std::lock_guard lock(mutex_);
  for (auto it = pending_.begin(); it != pending_.end(); ++it) {
    if (it->pawn_handle == pawn_handle && it->steam_id == steam_id &&
        it->connection_generation == connection_generation) {
      pending_.erase(pending_.begin(), std::next(it));
      return true;
    }
  }
  return false;
}

struct HeroInitializationHook::Impl {
#if defined(_WIN32)
  safetyhook::InlineHook hook;

  ~Impl() {
    if (g_hook == &hook) {
      hook.reset();
      g_hook = nullptr;
      g_handler = {};
    }
  }
#endif
};

HeroInitializationHook::HeroInitializationHook(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
HeroInitializationHook::HeroInitializationHook(HeroInitializationHook&&) noexcept = default;
HeroInitializationHook& HeroInitializationHook::operator=(HeroInitializationHook&&) noexcept =
    default;
HeroInitializationHook::~HeroInitializationHook() = default;

std::expected<HeroInitializationHook, std::string> HeroInitializationHook::Install(
    const ModuleImage& server, Handler handler) {
#if defined(_WIN32)
  if (g_hook != nullptr) {
    return std::unexpected("hero initialization hook is already installed");
  }
  auto target = ResolveSignature(server, "pawn.initialize-hero");
  if (!target.has_value()) {
    return std::unexpected(target.error());
  }
  auto created = safetyhook::create_inline(*target, reinterpret_cast<void*>(&InitializeHeroThunk),
                                           safetyhook::InlineHook::StartDisabled);
  if (!created) {
    return std::unexpected("inline hook on InitializeHeroOnPawn failed");
  }
  auto impl = std::make_unique<Impl>();
  impl->hook = std::move(created);
  g_hook = &impl->hook;
  g_handler = std::move(handler);
  if (auto enabled = impl->hook.enable(); !enabled) {
    impl->hook.reset();
    g_hook = nullptr;
    g_handler = {};
    return std::unexpected("InitializeHeroOnPawn hook could not be enabled");
  }
  return HeroInitializationHook(std::move(impl));
#else
  (void)server;
  (void)handler;
  return std::unexpected("the hero initialization hook requires the Windows host build");
#endif
}

}  // namespace modlock::gameinterop
