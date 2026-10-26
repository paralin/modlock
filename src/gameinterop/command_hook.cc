#include "modlock/gameinterop/command_hook.h"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <utility>

#include "modlock/gameinterop/connection_tracker.h"
#include "modlock/gameinterop/entity_abi.h"
#include "modlock/gameinterop/virtual_slots.h"

#if defined(_WIN32)
#include <safetyhook.hpp>
#endif

namespace modlock::gameinterop {
namespace {

#if defined(_WIN32)
// CUserCmd has no schema. Build 6795: CCitadelPlayerBot::PhysicsSimulate
// (server RVA 0x817cf0) and the test-convar writer at RVA 0x7e2700 build a
// bot's command with these, and the field getters at RVA 0x7e90e0 and
// 0x7e9100 read the axes back.
constexpr size_t kCommandStride = 0xa8;
// The CCitadelUserCmdPB begins at +0x10; its has-bits word, whose bit 0 marks
// the base message, is at +0x20, and its base message pointer at +0x48.
constexpr size_t kCommandHasBits = 0x20;
constexpr size_t kCommandBase = 0x48;
// Build 6819, through the CCitadelUserCmdPB merge: has-bit 2 marks
// vec_camera_position, a CMsgVector whose pointer is at +0x50 and whose x, y
// and z are floats from +0x18.
constexpr uint32_t kCameraBit = 0x2;
constexpr size_t kCommandCamera = 0x50;
constexpr size_t kVectorXyz = 0x18;
// CBaseUserCmdPB: has-bits, the view angles message, forwardmove (has-bit
// 0x40) and leftmove (has-bit 0x80).
constexpr size_t kBaseHasBits = 0x10;
constexpr size_t kBaseAngles = 0x40;
constexpr size_t kBaseForward = 0x58;
constexpr size_t kBaseLeft = 0x5c;
constexpr uint32_t kBaseAxesBits = 0x40 | 0x80;
// Has-bit 0x4 marks the view message. CMsgQAngle holds has-bits at +0x10 and
// x, y and z from +0x18, in 0x28 bytes.
constexpr uint32_t kBaseAnglesBit = 0x4;
constexpr size_t kAnglesHasBits = 0x10;
constexpr uint32_t kAnglesXyzBits = 0x7;
constexpr size_t kAnglesXyz = 0x18;
constexpr size_t kAnglesSize = 0x28;
// The command's InButtonState sits at +0x70, its held, changed and scroll
// masks after the vtable.
constexpr size_t kCommandButtons = 0x78;

safetyhook::InlineHook* g_hook = nullptr;
CommandHook::Decide* g_decide = nullptr;
// g_held is each slot's buttons in the last command driven, so a button that
// goes up or down reads as changed.
std::array<uint64_t, kMaxPlayerSlots> g_held{};
// g_cameras is each slot's camera position in its latest commands.
std::array<std::optional<std::array<float, 3>>, kMaxPlayerSlots> g_cameras{};
bool g_reported_bare = false;
// A bot's command carries no view message, so a driven bot's borrows g_lent,
// built on the message's vtable, for one call.
const void* g_angles_vtable = nullptr;
alignas(8) std::array<std::byte, kAnglesSize> g_lent{};

template <typename T>
T Load(const std::byte* base, size_t offset) {
  T value;
  std::memcpy(&value, base + offset, sizeof(value));
  return value;
}

template <typename T>
void Store(std::byte* base, size_t offset, const T& value) {
  std::memcpy(base + offset, &value, sizeof(value));
}

// Write puts the drive's controls in one command. A bot's brain builds its
// base message on its first tick; a command without one keeps its own axes and
// view this tick.
void Write(std::byte* command, const CommandHook::Command& drive, uint64_t& last_held,
           std::byte* lent) {
  // Write the held and pressed buttons.
  const uint64_t held = drive.held | drive.pressed;
  const std::array<uint64_t, 3> buttons{held, (held ^ last_held) | drive.pressed, 0};
  Store(command, kCommandButtons, buttons);
  last_held = held;

  // Write the axes and view into the base message.
  auto* base = Load<std::byte*>(command, kCommandBase);
  if (!base) {
    if (!std::exchange(g_reported_bare, true)) {
      std::fprintf(stderr, "[modlock] bot drive: a command arrived without movement fields\n");
    }
    return;
  }
  Store(command, kCommandHasBits, Load<uint32_t>(command, kCommandHasBits) | 1u);
  Store(base, kBaseHasBits, Load<uint32_t>(base, kBaseHasBits) | kBaseAxesBits);
  Store(base, kBaseForward, drive.forward);
  Store(base, kBaseLeft, drive.left);
  auto* angles = Load<std::byte*>(base, kBaseAngles);
  if (!angles && lent) {
    angles = lent;
    Store(base, kBaseAngles, angles);
    Store(base, kBaseHasBits, Load<uint32_t>(base, kBaseHasBits) | kBaseAnglesBit);
  }
  if (angles) {
    Store(angles, kAnglesHasBits, Load<uint32_t>(angles, kAnglesHasBits) | kAnglesXyzBits);
    Store(angles, kAnglesXyz, drive.angles);
  }
}

// Camera returns the camera position the batch's last command carries.
std::optional<std::array<float, 3>> Camera(const std::byte* commands, int count) {
  for (int i = count - 1; i >= 0; --i) {
    const auto* command = commands + static_cast<size_t>(i) * kCommandStride;
    if (!(Load<uint32_t>(command, kCommandHasBits) & kCameraBit)) {
      continue;
    }
    if (const auto* vector = Load<const std::byte*>(command, kCommandCamera)) {
      return Load<std::array<float, 3>>(vector, kVectorXyz);
    }
  }
  return std::nullopt;
}

int ProcessThunk(void* controller, std::byte* commands, int count, bool paused, float margin) {
  const auto handle = controller ? ReferenceHandleOf(controller) : std::nullopt;
  const int32_t slot = handle ? static_cast<int32_t>(EntityIndexOf(*handle)) - 1 : -1;
  std::byte* lent = nullptr;
  if (IsPlayerSlot(slot) && commands && count > 0) {
    g_cameras[slot] = Camera(commands, count);
    if (auto drive = (*g_decide)(slot)) {
      if (g_angles_vtable) {
        lent = g_lent.data();
        g_lent.fill(std::byte{});
        Store(lent, 0, g_angles_vtable);
      }
      for (int i = 0; i < count; ++i) {
        Write(commands + static_cast<size_t>(i) * kCommandStride, *drive, g_held[slot], lent);
        // A press counts once, in the batch's first command.
        drive->pressed = 0;
      }
    } else {
      g_held[slot] = 0;
    }
  }
  const int result = g_hook->call<int>(controller, commands, count, paused, margin);

  // Take the lent view message back out, so the game never frees it.
  for (int i = 0; lent && i < count; ++i) {
    const auto* command = commands + static_cast<size_t>(i) * kCommandStride;
    auto* base = Load<std::byte*>(command, kCommandBase);
    if (base && Load<std::byte*>(base, kBaseAngles) == lent) {
      Store(base, kBaseAngles, static_cast<std::byte*>(nullptr));
      Store(base, kBaseHasBits, Load<uint32_t>(base, kBaseHasBits) & ~kBaseAnglesBit);
    }
  }
  return result;
}
#endif

}  // namespace

struct CommandHook::Impl {
#if defined(_WIN32)
  safetyhook::InlineHook hook;
  Decide decide;

  ~Impl() {
    // Only the installed hook clears the shared state.
    if (g_hook != &hook) {
      return;
    }
    hook.reset();
    g_hook = nullptr;
    g_decide = nullptr;
    g_held = {};
    g_cameras = {};
    g_angles_vtable = nullptr;
  }
#endif
};

CommandHook::CommandHook(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

CommandHook::CommandHook(CommandHook&&) noexcept = default;
CommandHook& CommandHook::operator=(CommandHook&&) noexcept = default;
CommandHook::~CommandHook() = default;

std::expected<CommandHook, std::string> CommandHook::Install(const ModuleImage& server,
                                                             Decide decide) {
#if defined(_WIN32)
  // Install once.
  if (g_hook || !decide) {
    return std::unexpected("command hook is already installed or unbound");
  }

  // Resolve the target and create the hook disabled.
  auto target = ResolveSignature(server, "controller.process-usercmds");
  if (!target) {
    return std::unexpected(target.error());
  }
  auto hook = safetyhook::create_inline(*target, reinterpret_cast<void*>(&ProcessThunk),
                                        safetyhook::InlineHook::StartDisabled);
  if (!hook) {
    return std::unexpected("command hook creation failed");
  }

  // Publish the thunk state and enable.
  auto impl = std::make_unique<Impl>();
  impl->hook = std::move(hook);
  impl->decide = std::move(decide);
  g_hook = &impl->hook;
  g_decide = &impl->decide;
  const auto table = FindVtable(server, "CMsgQAngle");
  g_angles_vtable = table ? reinterpret_cast<const void*>(server.base() + *table) : nullptr;
  if (auto enabled = impl->hook.enable(); !enabled) {
    return std::unexpected("command hook activation failed");
  }
  return CommandHook(std::move(impl));

#else
  // Other platforms have no native server.
  (void)server;
  (void)decide;
  return std::unexpected("native bot control requires the Windows host");
#endif
}

std::optional<std::array<float, 3>> CommandHook::CameraPosition(int32_t slot) const {
#if defined(_WIN32)
  if (IsPlayerSlot(slot)) {
    return g_cameras[slot];
  }
#else
  (void)slot;
#endif
  return std::nullopt;
}

}  // namespace modlock::gameinterop
