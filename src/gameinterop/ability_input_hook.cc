#include "modlock/gameinterop/ability_input_hook.h"

#include <vector>

#include "modlock/gameinterop/connection_tracker.h"
#include "modlock/gameinterop/entity_abi.h"

#if defined(_WIN32)
#include <windows.h>

#include <safetyhook.hpp>
#endif

namespace modlock::gameinterop {
namespace {

// Owner is one installation's handler and failure report.
struct Owner {
  AbilityInputHook::Handler handler;
  std::function<void(std::string)> failure;
  bool include_bots = false;
};

#if defined(_WIN32)
// The server executes AbilityThink on its engine thread, which also installs
// and destroys owners, so the owner list needs no lock. One detour serves every
// owner and lives while any owner does.
AbilityInputHook::Layout g_offsets;
std::vector<Owner*> g_owners;
safetyhook::InlineHook g_hook;
bool g_failed = false;

// Consume passes the input through each owner in installation order. Each
// owner sees the buttons earlier owners remapped; the consumed bits combine.
uint64_t Consume(AbilityInputHook::Input& input) {
  // Name the slot's player and let each owner block buttons.
  const auto connection = ConnectionTracker::StateForSlot(input.slot);
  if (!connection.occupied) {
    return 0;
  }
  input.steam_id = connection.xuid;
  input.session_generation = connection.generation;
  uint64_t blocked = 0;
  for (size_t i = 0; i < g_owners.size(); ++i) {
    if (connection.is_bot && !g_owners[i]->include_bots) {
      continue;
    }
    blocked |= g_owners[i]->handler(input);
  }
  return blocked;
}

void Filter(void* pawn) {
  // Filter until a read fails.
  if (g_failed) {
    return;
  }
  auto result = AbilityInputHook::Process(pawn, g_offsets, ReadNative, WriteNative, Consume);
  if (result) {
    return;
  }

  // A layout that cannot be read stays unreadable, so the detour stops filtering.
  g_failed = true;
  for (size_t i = 0; i < g_owners.size(); ++i) {
    g_owners[i]->failure(result.error());
  }
}

void __fastcall AbilityThinkThunk(void* pawn) {
  Filter(pawn);
  g_hook.call<void>(pawn);
}
#endif

}  // namespace

std::expected<void, std::string> AbilityInputHook::Process(void* pawn, const Layout& layout,
                                                           const BoundedReader& read,
                                                           const BoundedWriter& write,
                                                           const Handler& handler) {
  // Find the player's slot from the pawn's controller.
  if (!pawn || !read || !write || !handler) {
    return std::unexpected("ability input: missing pawn or memory callback");
  }
  auto* bytes = static_cast<unsigned char*>(pawn);
  Input input;
  if (!read(bytes + layout.controller, &input.controller_handle, sizeof(input.controller_handle))) {
    return std::unexpected("ability input: cannot read pawn controller");
  }
  if (input.controller_handle == UINT32_MAX || input.controller_handle == UINT32_MAX - 1) {
    return {};
  }
  input.slot = static_cast<int32_t>(input.controller_handle & 0x7fff) - 1;
  if (input.slot < 0 || input.slot >= 64) {
    return {};
  }

  // Read the movement services' button states.
  unsigned char* movement = nullptr;
  if (!read(bytes + layout.movement, &movement, sizeof(movement))) {
    return std::unexpected("ability input: cannot read movement services");
  }
  if (!movement) {
    return {};
  }
  auto* target = movement + layout.buttons;
  if (!read(target, input.buttons.data(), sizeof(input.buttons))) {
    return std::unexpected("ability input: cannot read button states");
  }

  // Clear the blocked buttons and write back any change.
  const auto original = input.buttons;
  const uint64_t blocked = handler(input);
  for (auto& state : input.buttons) {
    state &= ~blocked;
  }
  if (input.buttons == original) {
    return {};
  }
  if (!write(target, input.buttons.data(), sizeof(input.buttons))) {
    return std::unexpected("ability input: cannot write filtered buttons");
  }
  return {};
}

// Impl leaves the owner list on destruction; the last owner removes the detour.
struct AbilityInputHook::Impl {
  Owner owner;
#if defined(_WIN32)
  ~Impl() {
    std::erase(g_owners, &owner);
    if (!g_owners.empty()) {
      return;
    }
    g_hook.reset();
    g_failed = false;
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
  // Both callbacks are required.
  if (!handler || !failure) {
    return std::unexpected("ability input: missing callbacks");
  }
  auto impl = std::make_unique<Impl>();
  impl->owner = {std::move(handler), std::move(failure), include_bots};

  // A later owner joins the detour the first owner created.
  if (!g_owners.empty()) {
    g_owners.push_back(&impl->owner);
    return AbilityInputHook(std::move(impl));
  }

  // Resolve AbilityThink before binding its schema-derived input layout.
  auto target = ResolveSignature(server, "ability.think");
  if (!target) {
    return std::unexpected(target.error());
  }
  const auto controller =
      SchemaFieldOf(schema_system, "server.dll", "CBasePlayerPawn", "m_hController");
  const auto movement =
      SchemaFieldOf(schema_system, "server.dll", "CBasePlayerPawn", "m_pMovementServices");
  const auto buttons =
      SchemaFieldOf(schema_system, "server.dll", "CPlayer_MovementServices", "m_nButtons");
  const auto states =
      SchemaFieldOf(schema_system, "server.dll", "CInButtonState", "m_pButtonStates");

  // Every field must resolve, with room for its value.
  if (!controller) {
    return std::unexpected(controller.error());
  }
  if (!movement) {
    return std::unexpected(movement.error());
  }
  if (!buttons) {
    return std::unexpected(buttons.error());
  }
  if (!states) {
    return std::unexpected(states.error());
  }
  if (controller->size < sizeof(uint32_t) || movement->size < sizeof(void*) ||
      states->size < sizeof(std::array<uint64_t, 3>)) {
    return std::unexpected("ability input: native field storage is too short");
  }

  // The first owner creates the detour; a failed enable removes it again.
  g_offsets = {controller->offset, movement->offset, buttons->offset + states->offset};
  g_hook = safetyhook::create_inline(*target, reinterpret_cast<void*>(&AbilityThinkThunk),
                                     safetyhook::InlineHook::StartDisabled);
  if (!g_hook) {
    return std::unexpected("ability input: cannot create AbilityThink hook");
  }

  // Join the owner and enable the detour.
  g_failed = false;
  g_owners.push_back(&impl->owner);
  if (!g_hook.enable()) {
    return std::unexpected("ability input: cannot enable AbilityThink hook");
  }
  return AbilityInputHook(std::move(impl));

#else
  // Other platforms have no ability input.
  (void)server;
  (void)schema_system;
  (void)handler;
  (void)failure;
  (void)include_bots;
  return std::unexpected("ability input requires the Windows host build");
#endif
}

}  // namespace modlock::gameinterop
