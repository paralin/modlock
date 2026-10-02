#include "modlock/gameinterop/movement_hook.h"

#include <cstdio>
#include <cstring>
#include <optional>

#include "modlock/gameinterop/entity_abi.h"
#include "modlock/gameinterop/mapped_module_image.h"

#if defined(_WIN32)
#include <safetyhook.hpp>
#endif

namespace modlock::gameinterop {
namespace {

// CMoveData has no schema. These offsets were observed in both modules of the
// September 18 2026 build by recording scripted input, prediction replays and
// solver output against schema-resolved pawn state.
constexpr size_t kMoveAngles = 0x08;
constexpr size_t kMoveInput = 0x20;
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

// Layout holds the schema offsets of one module's movement services and pawn.
// The component's network chainer begins with its owning entity pointer.
struct Layout {
  size_t buttons = 0;
  size_t command = 0;
  size_t chain = 0;
  size_t services = 0;
  size_t ground = 0;
  size_t move_type = 0;
};

// Resolve reads the layout after the module has registered its schema. The
// client module prefixes its entity classes with C_.
std::expected<Layout, std::string> Resolve(const char* module, const char* entity,
                                           const char* pawn) {
  auto schema = ResolveSchemaSystem();
  if (!schema) return std::unexpected(schema.error());
  Layout layout;
  struct Field {
    const char* type;
    const char* name;
    size_t* offset;
  };
  for (const auto& field :
       {Field{"CPlayer_MovementServices", "m_nButtons", &layout.buttons},
        Field{"CPlayer_MovementServices", "m_nLastCommandNumberProcessed", &layout.command},
        Field{"CPlayerPawnComponent", "__m_pChainEntity", &layout.chain},
        Field{pawn, "m_pMovementServices", &layout.services},
        Field{entity, "m_hGroundEntity", &layout.ground},
        Field{entity, "m_MoveType", &layout.move_type}}) {
    auto resolved = SchemaFieldOf(*schema, module, field.type, field.name);
    if (!resolved)
      return std::unexpected(std::string(field.type) + "." + field.name + ": " + resolved.error());
    *field.offset = resolved->offset;
  }
  return layout;
}

// Hook is the process-wide state of one module's detour.
struct Hook {
  const char* module_name = nullptr;
  const char* entity_class = nullptr;
  const char* pawn_class = nullptr;
  MovementHook::Handler* handler = nullptr;
#if defined(_WIN32)
  safetyhook::InlineHook* detour = nullptr;
#endif
  std::optional<Layout> layout;
  bool layout_failed = false;
};

std::array<Hook, 2> g_hooks{Hook{.module_name = "server.dll",
                                 .entity_class = "CBaseEntity",
                                 .pawn_class = "CBasePlayerPawn"},
                            Hook{.module_name = "client.dll",
                                 .entity_class = "C_BaseEntity",
                                 .pawn_class = "C_BasePlayerPawn"}};

// PawnOf returns the component's owner only when that pawn points back to the
// same movement services, so a stale or foreign chain never receives writes.
void* PawnOf(const unsigned char* services, const Layout& layout) {
  void* pawn = nullptr;
  Read(services, layout.chain, pawn);
  if (!pawn) return nullptr;
  const void* owned = nullptr;
  Read(static_cast<const unsigned char*>(pawn), layout.services, owned);
  return owned == services ? pawn : nullptr;
}

void Dispatch(Hook& hook, void* services, void* data) {
#if defined(_WIN32)
  if (!hook.layout && !hook.layout_failed) {
    auto layout = Resolve(hook.module_name, hook.entity_class, hook.pawn_class);
    if (layout) hook.layout = *layout;
    hook.layout_failed = !layout;
    if (!layout)
      std::fprintf(stderr,
                   "[modlock] %s movement layout unavailable (%s); native movement retained\n",
                   hook.module_name, layout.error().c_str());
  }
  if (!hook.layout) return hook.detour->call<void>(services, data);

  const auto* owner = static_cast<const unsigned char*>(services);
  auto* move = static_cast<unsigned char*>(data);
  MovementCall call{.services = services};
  call.pawn = PawnOf(owner, *hook.layout);
  Read(owner, hook.layout->command, call.command);
  Read(owner, hook.layout->buttons + kButtonStates, call.buttons);
  const auto buttons = call.buttons;
  Read(move, kMoveAngles, call.angles);
  const auto angles = call.angles;
  std::array<float, 3> input;
  Read(move, kMoveInput, input);
  call.forward = input[0];
  call.left = input[1];
  call.up = input[2];
  Read(move, kMoveTicks, call.ticks);
  Read(move, kMoveFractions, call.fractions);
  Read(move, kMoveOrigin, call.origin);
  Read(move, kMoveVelocity, call.velocity);
  if (call.pawn) {
    Read(static_cast<const unsigned char*>(call.pawn), hook.layout->ground, call.ground_entity);
    Read(static_cast<const unsigned char*>(call.pawn), hook.layout->move_type, call.move_type);
  }
  // The handler may rewrite buttons even when it leaves movement native. The
  // native step always runs for its per-command bookkeeping, which client
  // prediction requires; a replacement result then overwrites its motion.
  const bool replaced = (*hook.handler)(call);
  if (call.buttons != buttons)
    std::memcpy(static_cast<unsigned char*>(services) + hook.layout->buttons + kButtonStates,
                call.buttons.data(), sizeof(call.buttons));
  // Rewritten angles and movement axes steer the native step itself, so its
  // movement state and animation follow the replaced command.
  if (call.angles != angles) std::memcpy(move + kMoveAngles, call.angles.data(), sizeof(call.angles));
  const std::array<float, 3> axes{call.forward, call.left, call.up};
  if (axes != input) std::memcpy(move + kMoveInput, axes.data(), sizeof(axes));
  hook.detour->call<void>(services, data);
  if (!replaced) return;
  std::memcpy(move + kMoveOrigin, call.origin.data(), sizeof(call.origin));
  std::memcpy(move + kMoveVelocity, call.velocity.data(), sizeof(call.velocity));
  if (call.pawn)
    std::memcpy(static_cast<unsigned char*>(call.pawn) + hook.layout->ground, &call.ground_entity,
                sizeof(call.ground_entity));
#else
  (void)hook;
  (void)services;
  (void)data;
#endif
}

void ServerThunk(void* services, void* data) { Dispatch(g_hooks[0], services, data); }
void ClientThunk(void* services, void* data) { Dispatch(g_hooks[1], services, data); }

}  // namespace

struct MovementHook::Impl {
  Hook* hook = nullptr;
  Handler handler;
#if defined(_WIN32)
  safetyhook::InlineHook detour;
#endif
  ~Impl() {
    if (!hook) return;
#if defined(_WIN32)
    detour.reset();
#endif
    *hook = Hook{.module_name = hook->module_name,
                 .entity_class = hook->entity_class,
                 .pawn_class = hook->pawn_class};
  }
};

MovementHook::MovementHook(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
MovementHook::MovementHook(MovementHook&&) noexcept = default;
MovementHook& MovementHook::operator=(MovementHook&&) noexcept = default;
MovementHook::~MovementHook() = default;

std::expected<MovementHook, std::string> MovementHook::Install(Module module, Handler handler) {
#if defined(_WIN32)
  auto& hook = g_hooks[module == Module::kServer ? 0 : 1];
  if (hook.handler || !handler) return std::unexpected("movement hook already installed or empty");
  const auto image =
      MappedModuleImage::ForModule(module == Module::kServer ? L"server.dll" : L"client.dll");
  if (!image) return std::unexpected(image.error());
  auto target = ResolveSignature(*image, "movement.process");
  if (!target) return std::unexpected(target.error());
  auto impl = std::make_unique<Impl>();
  impl->handler = std::move(handler);
  const auto thunk = module == Module::kServer ? &ServerThunk : &ClientThunk;
  impl->detour = safetyhook::create_inline(*target, reinterpret_cast<void*>(thunk),
                                           safetyhook::InlineHook::StartDisabled);
  if (!impl->detour) return std::unexpected("cannot create native movement hook");
  hook.handler = &impl->handler;
  hook.detour = &impl->detour;
  impl->hook = &hook;
  if (!impl->detour.enable()) return std::unexpected("cannot enable native movement hook");
  return MovementHook(std::move(impl));
#else
  (void)module;
  (void)handler;
  return std::unexpected("native movement replacement requires Windows");
#endif
}

}  // namespace modlock::gameinterop
