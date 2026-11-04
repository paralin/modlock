#include "modlock/gameinterop/movement_hook.h"

#include <cstdio>
#include <cstring>
#include <optional>

#include "modlock/gameinterop/connection_tracker.h"
#include "modlock/gameinterop/entity_abi.h"
#include "modlock/gameinterop/mapped_module_image.h"

#if defined(_WIN32)
#include <safetyhook.hpp>
#endif

namespace modlock::gameinterop {
namespace {

// CMoveData has no schema. These offsets were observed in server.dll of the
// September 18 2026 build by recording scripted input and solver output
// against schema-resolved pawn state. The step copies the command's axes to
// the last impulses at +0x20, but the walk reads the move axes at +0x2c (build
// 6819 CCitadelPlayer_MovementServices::ProcessMovement). Likewise the view
// sits at +0x08, but the walk and air steps turn the axes by the wish angles
// at +0xe8 (build 6819 FUN_180b14340).
constexpr size_t kMoveAngles = 0x08;
constexpr size_t kMoveWishAngles = 0xe8;
constexpr size_t kMoveImpulses = 0x20;
constexpr size_t kMoveAxes = 0x2c;
constexpr size_t kMoveVelocity = 0x38;
constexpr size_t kMoveOrigin = 0xc8;
constexpr size_t kMoveTicks = 0xd4;
constexpr size_t kMoveFractions = 0xdc;

// InButtonState begins with its vtable, followed by held, changed and scroll masks.
constexpr size_t kButtonStates = 0x08;

template <typename T>
void Read(const unsigned char* base, size_t offset, T& value) {
  std::memcpy(&value, base + offset, sizeof(value));
}

// Layout holds the schema offsets of the server's movement services and pawn.
// The component's network chainer begins with its owning entity pointer.
struct Layout {
  size_t buttons = 0;
  size_t command = 0;
  size_t chain = 0;
  size_t services = 0;
  size_t controller = 0;
  size_t ground = 0;
  size_t move_type = 0;
};

// Resolve reads the layout after server.dll has registered its schema.
std::expected<Layout, std::string> Resolve() {
  // Resolve each movement field's offset through the schema.
  auto schema = ResolveSchemaSystem();
  if (!schema) {
    return std::unexpected(schema.error());
  }
  Layout layout;

  // Fill every offset, or fail at the first missing one.
  const SchemaOffset fields[] = {
      {"CPlayer_MovementServices", "m_nButtons", 0, &layout.buttons},
      {"CPlayer_MovementServices", "m_nLastCommandNumberProcessed", 0, &layout.command},
      {"CPlayerPawnComponent", "__m_pChainEntity", 0, &layout.chain},
      {"CBasePlayerPawn", "m_pMovementServices", 0, &layout.services},
      {"CBasePlayerPawn", "m_hController", 0, &layout.controller},
      {"CBaseEntity", "m_hGroundEntity", 0, &layout.ground},
      {"CBaseEntity", "m_MoveType", 0, &layout.move_type},
  };
  if (auto resolved = ResolveSchemaOffsets(*schema, "server.dll", fields); !resolved) {
    return std::unexpected(resolved.error());
  }
  return layout;
}

// Hook is the process-wide state of the detour.
struct Hook {
  MovementHook::Handler* handler = nullptr;
#if defined(_WIN32)
  safetyhook::InlineHook* detour = nullptr;
#endif
  std::optional<Layout> layout;
  bool layout_failed = false;
};

Hook g_hook;

// PawnOf returns the component's owner only when that pawn points back to the
// same movement services, so a stale or foreign chain never receives writes.
void* PawnOf(const unsigned char* services, const Layout& layout) {
  // The pawn owns these services when its services point back to them.
  void* pawn = nullptr;
  Read(services, layout.chain, pawn);
  if (!pawn) {
    return nullptr;
  }
  const void* owned = nullptr;
  Read(static_cast<const unsigned char*>(pawn), layout.services, owned);
  return owned == services ? pawn : nullptr;
}

void Dispatch(Hook& hook, void* services, void* data) {
#if defined(_WIN32)
  // Resolve the layout once; without it movement stays native.
  if (!hook.layout && !hook.layout_failed) {
    auto layout = Resolve();
    if (layout) {
      hook.layout = *layout;
    }
    hook.layout_failed = !layout;
    if (!layout) {
      std::fprintf(stderr, "[modlock] movement layout unavailable (%s); native movement retained\n",
                   layout.error().c_str());
    }
  }
  if (!hook.layout) {
    return hook.detour->call<void>(services, data);
  }

  // Find the pawn and read the command's buttons.
  const auto* owner = static_cast<const unsigned char*>(services);
  auto* move = static_cast<unsigned char*>(data);
  MovementCall call{.services = services};
  call.pawn = PawnOf(owner, *hook.layout);
  Read(owner, hook.layout->command, call.command);
  Read(owner, hook.layout->buttons + kButtonStates, call.buttons);
  const auto buttons = call.buttons;

  // Read the angles and movement axes.
  Read(move, kMoveAngles, call.angles);
  const auto angles = call.angles;
  std::array<float, 3> input;
  Read(move, kMoveAxes, input);
  call.forward = input[0];
  call.left = input[1];
  call.up = input[2];

  // Read the move's timing, position and ground.
  Read(move, kMoveTicks, call.ticks);
  Read(move, kMoveFractions, call.fractions);
  Read(move, kMoveOrigin, call.origin);
  Read(move, kMoveVelocity, call.velocity);
  if (call.pawn) {
    Read(static_cast<const unsigned char*>(call.pawn), hook.layout->ground, call.ground_entity);
    Read(static_cast<const unsigned char*>(call.pawn), hook.layout->move_type, call.move_type);
    uint32_t controller = kInvalidEHandleIndex;
    Read(static_cast<const unsigned char*>(call.pawn), hook.layout->controller, controller);
    const auto slot = static_cast<int32_t>(EntityIndexOf(controller)) - 1;
    if (controller != kInvalidEHandleIndex && IsPlayerSlot(slot)) {
      call.slot = slot;
    }
  }

  // The handler may rewrite buttons even when it leaves movement native. The
  // native step always runs for its per-command bookkeeping; a replacement
  // result then overwrites its motion.
  const bool replaced = (*hook.handler)(call);
  if (call.buttons != buttons) {
    std::memcpy(static_cast<unsigned char*>(services) + hook.layout->buttons + kButtonStates,
                call.buttons.data(), sizeof(call.buttons));
  }

  // Rewritten angles and movement axes steer the native step itself, so its
  // movement state and animation follow the replaced command.
  if (call.angles != angles) {
    std::memcpy(move + kMoveAngles, call.angles.data(), sizeof(call.angles));
    std::memcpy(move + kMoveWishAngles, call.angles.data(), sizeof(call.angles));
  }
  const std::array<float, 3> axes{call.forward, call.left, call.up};
  if (axes != input) {
    std::memcpy(move + kMoveImpulses, axes.data(), sizeof(axes));
    std::memcpy(move + kMoveAxes, axes.data(), sizeof(axes));
  }

  // Run the native step, then write a replacement's motion over it.
  hook.detour->call<void>(services, data);
  if (!replaced) {
    return;
  }
  std::memcpy(move + kMoveOrigin, call.origin.data(), sizeof(call.origin));
  std::memcpy(move + kMoveVelocity, call.velocity.data(), sizeof(call.velocity));
  if (call.pawn) {
    std::memcpy(static_cast<unsigned char*>(call.pawn) + hook.layout->ground, &call.ground_entity,
                sizeof(call.ground_entity));
  }
#else
  (void)hook;
  (void)services;
  (void)data;
#endif
}

void Thunk(void* services, void* data) {
  Dispatch(g_hook, services, data);
}

}  // namespace

struct MovementHook::Impl {
  Hook* hook = nullptr;
  Handler handler;
#if defined(_WIN32)
  safetyhook::InlineHook detour;
#endif
  ~Impl() {
    if (!hook) {
      return;
    }
#if defined(_WIN32)
    detour.reset();
#endif
    *hook = Hook{};
  }
};

MovementHook::MovementHook(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

MovementHook::MovementHook(MovementHook&&) noexcept = default;
MovementHook& MovementHook::operator=(MovementHook&&) noexcept = default;
MovementHook::~MovementHook() = default;

std::expected<MovementHook, std::string> MovementHook::Install(Handler handler) {
#if defined(_WIN32)
  // Install once.
  auto& hook = g_hook;
  if (hook.handler || !handler) {
    return std::unexpected("movement hook already installed or empty");
  }

  // Resolve the native movement step.
  const auto image = MappedModuleImage::ForModule(L"server.dll");
  if (!image) {
    return std::unexpected(image.error());
  }
  auto target = ResolveSignature(*image, "movement.process");
  if (!target) {
    return std::unexpected(target.error());
  }

  // Detour it through the thunk, disabled until wired.
  auto impl = std::make_unique<Impl>();
  impl->handler = std::move(handler);
  impl->detour = safetyhook::create_inline(*target, reinterpret_cast<void*>(&Thunk),
                                           safetyhook::InlineHook::StartDisabled);
  if (!impl->detour) {
    return std::unexpected("cannot create native movement hook");
  }

  // Wire the hook, then enable it.
  hook.handler = &impl->handler;
  hook.detour = &impl->detour;
  impl->hook = &hook;
  if (!impl->detour.enable()) {
    return std::unexpected("cannot enable native movement hook");
  }
  return MovementHook(std::move(impl));
#else
  (void)handler;
  return std::unexpected("native movement replacement requires Windows");
#endif
}

}  // namespace modlock::gameinterop
