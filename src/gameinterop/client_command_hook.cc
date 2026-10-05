#include "modlock/gameinterop/client_command_hook.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <optional>
#include <utility>

#include "modlock/gameinterop/connection_tracker.h"
#include "modlock/gameinterop/entity_abi.h"
#include "modlock/gameinterop/mapped_module_image.h"
#include "modlock/gameinterop/native_memory.h"
#include "modlock/gameinterop/thunk_owner.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace modlock::gameinterop {
namespace {
#if defined(_WIN32)
using CommandFn = void (*)(void*, int32_t, void*);
CommandFn g_original = nullptr;
ClientCommandHook::Handler* g_handler = nullptr;

// Preserve call-through for a dispatch already entering the old thunk.
void ClearThunkState() { g_handler = nullptr; }

// CCommand::m_ArgSBuffer is the first CUtlVectorFixedGrowable after an int.
// In the x64 SDK, its count is at +8 and allocator pointer at +16.
// Layout: SDK public/tier1/{convar,utlvector,utlvectormemory}.h.
// Copy through the OS so an incompatible command layout is forwarded untouched
// instead of dereferenced.
std::string ReadCommand(void* args) {
  if (args == nullptr) return {};
  int32_t count = 0;
  const char* data = nullptr;
  const auto* base = static_cast<const unsigned char*>(args);
  if (!ReadNative(base + 8, &count, sizeof(count)) || count <= 0 || count > 512 ||
      !ReadNative(base + 16, &data, sizeof(data)) || data == nullptr) {
    return {};
  }
  std::array<char, 512> text{};
  if (!ReadNative(data, text.data(), count)) return {};
  const auto* end = static_cast<const char*>(std::memchr(text.data(), '\0', count));
  if (end == nullptr) return {};
  return std::string(text.data(), static_cast<size_t>(end - text.data()));
}

// Source2GameClients001::ClientCommand is slot 17 in the pinned SDK.
// Intercept at the server ingress: the controller callback only sees commands
// the game recognizes, so new trial commands never reach that later callback.
void CommandThunk(void* self, int32_t slot, void* args) {
  const auto state = ConnectionTracker::StateForSlot(slot);
  if (g_handler != nullptr && state.occupied && !state.is_bot && state.xuid != 0) {
    auto command = ReadCommand(args);
    // Source console command names are case-insensitive; preserve argument case.
    const auto end = command.find_first_of(" \t");
    std::transform(command.begin(),
                   end == std::string::npos ? command.end() : command.begin() + end,
                   command.begin(),
                   [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; });
    if (InteropTraceEnabled() &&
        (command.starts_with("selecthero ") || command.starts_with("changeteam "))) {
      std::fprintf(stderr, "[modlock] client command slot %d: %s\n", slot, command.c_str());
      std::fflush(stderr);
    }
    if (!command.empty() && (*g_handler)(slot, command)) return;
  }
  if (g_original != nullptr) g_original(self, slot, args);
}
#endif
}  // namespace

struct ClientCommandHook::Impl {
  Handler handler;
  // Restore dispatch and release the borrowed handler before destroying it.
  std::optional<ThunkOwner> owner;
};

ClientCommandHook::ClientCommandHook(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
ClientCommandHook::ClientCommandHook(ClientCommandHook&&) noexcept = default;
ClientCommandHook& ClientCommandHook::operator=(ClientCommandHook&&) noexcept = default;
ClientCommandHook::~ClientCommandHook() = default;

std::expected<ClientCommandHook, std::string> ClientCommandHook::Install(Handler handler) {
#if defined(_WIN32)
  if (g_handler != nullptr || !handler)
    return std::unexpected("client command hook already installed or handler absent");
  const auto resolved = ResolveEngineInterface(L"server.dll", "Source2GameClients001");
  if (!resolved) return std::unexpected(resolved.error());
  void* clients = *resolved;
  auto impl = std::make_unique<Impl>();
  impl->handler = std::move(handler);
  auto hook = VtableSlotHook::Install(clients, 17, reinterpret_cast<void*>(&CommandThunk));
  if (!hook) return std::unexpected(hook.error());
  g_original = reinterpret_cast<CommandFn>(hook->Original());
  impl->owner.emplace(std::move(*hook), &ClearThunkState);
  g_handler = &impl->handler;
  return ClientCommandHook(std::move(impl));
#else
  (void)handler;
  return std::unexpected("client command hook requires the Windows host build");
#endif
}
}  // namespace modlock::gameinterop
