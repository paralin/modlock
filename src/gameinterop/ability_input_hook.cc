#include "modlock/gameinterop/ability_input_hook.h"

#include "modlock/gameinterop/connection_tracker.h"
#include "modlock/gameinterop/entity_abi.h"

#if defined(_WIN32)
#include <windows.h>

#include <safetyhook.hpp>
#endif

namespace modlock::gameinterop {
namespace {

#if defined(_WIN32)
// The server executes AbilityThink on its engine thread. Hook storage outlives
// the installed detour, including every callback into its handler.
AbilityInputHook::Layout g_offsets;
AbilityInputHook::Handler* g_handler = nullptr;
std::function<void(std::string)>* g_failure = nullptr;
safetyhook::InlineHook* g_hook = nullptr;
bool g_failed = false;
bool g_include_bots = false;

void Filter(void* pawn) {
  if (g_failed) return;
  auto result = AbilityInputHook::Process(
      pawn, g_offsets, ReadNative, WriteNative,
      [](AbilityInputHook::Input& input) {
        const auto connection = ConnectionTracker::StateForSlot(input.slot);
        if (!connection.occupied || (connection.is_bot && !g_include_bots)) return uint64_t{0};
        input.steam_id = connection.xuid;
        input.session_generation = connection.generation;
        return (*g_handler)(input);
      });
  if (!result) {
    g_failed = true;
    (*g_failure)(result.error());
  }
}

void __fastcall AbilityThinkThunk(void* pawn) {
  Filter(pawn);
  g_hook->call<void>(pawn);
}
#endif

}  // namespace

std::expected<void, std::string> AbilityInputHook::Process(void* pawn, const Layout& layout,
                                                           const BoundedReader& read,
                                                           const BoundedWriter& write,
                                                           const Handler& handler) {
  if (!pawn || !read || !write || !handler)
    return std::unexpected("ability input: missing pawn or memory callback");
  auto* bytes = static_cast<unsigned char*>(pawn);
  Input input;
  if (!read(bytes + layout.controller, &input.controller_handle, sizeof(input.controller_handle)))
    return std::unexpected("ability input: cannot read pawn controller");
  if (input.controller_handle == UINT32_MAX || input.controller_handle == UINT32_MAX - 1) return {};
  input.slot = static_cast<int32_t>(input.controller_handle & 0x7fff) - 1;
  if (input.slot < 0 || input.slot >= 64) return {};

  unsigned char* movement = nullptr;
  if (!read(bytes + layout.movement, &movement, sizeof(movement)))
    return std::unexpected("ability input: cannot read movement services");
  if (!movement) return {};
  auto* target = movement + layout.buttons;
  if (!read(target, input.buttons.data(), sizeof(input.buttons)))
    return std::unexpected("ability input: cannot read button states");
  const auto original = input.buttons;
  const uint64_t blocked = handler(input);
  for (auto& state : input.buttons) state &= ~blocked;
  if (input.buttons == original) return {};
  if (!write(target, input.buttons.data(), sizeof(input.buttons)))
    return std::unexpected("ability input: cannot write filtered buttons");
  return {};
}

struct AbilityInputHook::Impl {
  Handler handler;
  std::function<void(std::string)> failure;
#if defined(_WIN32)
  safetyhook::InlineHook hook;
  ~Impl() {
    if (g_hook != &hook) return;
    hook.reset();
    g_hook = nullptr;
    g_handler = nullptr;
    g_failure = nullptr;
    g_failed = false;
    g_include_bots = false;
  }
#endif
};

AbilityInputHook::AbilityInputHook(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
AbilityInputHook::AbilityInputHook(AbilityInputHook&&) noexcept = default;
AbilityInputHook& AbilityInputHook::operator=(AbilityInputHook&&) noexcept = default;
AbilityInputHook::~AbilityInputHook() = default;

std::expected<AbilityInputHook, std::string> AbilityInputHook::Install(
    const ModuleImage& server, void* schema_system, Handler handler,
    std::function<void(std::string)> failure, bool include_bots) {
#if defined(_WIN32)
  if (g_hook || !handler || !failure)
    return std::unexpected("ability input: already installed or missing callbacks");
  // Resolve AbilityThink before binding its schema-derived input layout.
  auto target = ResolveSignature(server, "ability.think");
  if (!target) return std::unexpected(target.error());
  const auto controller =
      SchemaFieldOf(schema_system, "server.dll", "CBasePlayerPawn", "m_hController");
  const auto movement =
      SchemaFieldOf(schema_system, "server.dll", "CBasePlayerPawn", "m_pMovementServices");
  const auto buttons =
      SchemaFieldOf(schema_system, "server.dll", "CPlayer_MovementServices", "m_nButtons");
  const auto states =
      SchemaFieldOf(schema_system, "server.dll", "CInButtonState", "m_pButtonStates");
  if (!controller) return std::unexpected(controller.error());
  if (!movement) return std::unexpected(movement.error());
  if (!buttons) return std::unexpected(buttons.error());
  if (!states) return std::unexpected(states.error());
  if (controller->size < sizeof(uint32_t) || movement->size < sizeof(void*) ||
      states->size < sizeof(std::array<uint64_t, 3>))
    return std::unexpected("ability input: native field storage is too short");

  auto impl = std::make_unique<Impl>();
  impl->handler = std::move(handler);
  impl->failure = std::move(failure);
  impl->hook = safetyhook::create_inline(*target, reinterpret_cast<void*>(&AbilityThinkThunk),
                                         safetyhook::InlineHook::StartDisabled);
  if (!impl->hook) return std::unexpected("ability input: cannot create AbilityThink hook");
  g_offsets = {controller->offset, movement->offset, buttons->offset + states->offset};
  g_handler = &impl->handler;
  g_failure = &impl->failure;
  g_failed = false;
  g_include_bots = include_bots;
  g_hook = &impl->hook;
  if (!impl->hook.enable())
    return std::unexpected("ability input: cannot enable AbilityThink hook");
  return AbilityInputHook(std::move(impl));
#else
  (void)server;
  (void)schema_system;
  (void)handler;
  (void)failure;
  (void)include_bots;
  return std::unexpected("ability input requires the Windows host build");
#endif
}

}  // namespace modlock::gameinterop
