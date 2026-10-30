#include "modlock/gameinterop/full_updates.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "modlock/gameinterop/game_symbols.h"
#include "modlock/gameinterop/mapped_module_image.h"

namespace modlock::gameinterop {
namespace {

// The server.clients signature matches sv_fullupdate's callback, which reads
// these offsets as constants, so a game update that moves one fails the
// signature rather than this code. The server's client list is a CUtlVector of
// CServerSideClient pointers: its count at kClientCountOffset and its elements
// at kClientsOffset. kAwaitSlotOffset is CServerSideClient::Await, which waits
// for the client's snapshot job before its state may change.
constexpr size_t kClientCountOffset = 0x268;
constexpr size_t kClientsOffset = 0x270;
constexpr size_t kAwaitSlotOffset = 0x250;

// kDeltaTickOffset is the tick the client last acknowledged, which the next
// snapshot is a delta from. SendSnapshot (engine2 RVA 0xba3f0 in builds 6796
// and 6821) reads it and sends a full update, as "requested uncompressed
// update", when it is -1.
constexpr size_t kDeltaTickOffset = 0x15c;

template <typename Value>
Value Load(const std::byte* address) {
  Value value;
  std::memcpy(&value, address, sizeof(value));
  return value;
}

}  // namespace

std::expected<FullUpdates, std::string> FullUpdates::Resolve() {
  // Find the global the callback reads the server from.
  const auto image = MappedModuleImage::ForModule(L"engine2.dll");
  if (!image) {
    return std::unexpected(image.error());
  }
  auto global = ResolveSignature(*image, "server.clients");
  if (!global) {
    return std::unexpected(global.error());
  }
  FullUpdates updates;
  updates.server_ = static_cast<void* const*>(*global);
  return updates;
}

void FullUpdates::RequestAll() const {
  // No server runs between worlds.
  const auto* server = static_cast<const std::byte*>(*server_);
  if (!server) {
    return;
  }

  // Clear each client's delta tick once its snapshot job is done.
  const auto count = Load<int32_t>(server + kClientCountOffset);
  const auto* clients = Load<std::byte* const*>(server + kClientsOffset);
  for (int32_t index = 0; index < count; ++index) {
    auto* client = clients[index];
    if (!client) {
      continue;
    }
    const auto* table = Load<void* const*>(client);
    reinterpret_cast<void (*)(void*)>(table[kAwaitSlotOffset / sizeof(void*)])(client);
    const int32_t full = -1;
    std::memcpy(client + kDeltaTickOffset, &full, sizeof(full));
  }
}

}  // namespace modlock::gameinterop
