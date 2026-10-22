#include "modlock/gameinterop/transmit_hook.h"

#include <cstring>
#include <optional>
#include <utility>
#include <vector>

#include "modlock/gameinterop/mapped_module_image.h"
#include "modlock/gameinterop/thunk_owner.h"
#include "modlock/gameinterop/virtual_slots.h"

namespace modlock::gameinterop {
namespace {

// The thunk state: the replacement reads the original target and the
// handlers from these slots. The engine thread owns them.
void* g_check_transmit_original = nullptr;
std::vector<std::pair<uint64_t, TransmitHook::Handler>> g_handlers;
uint64_t g_next_id = 1;
// g_owner holds the patched slot while any handler is installed.
std::optional<ThunkOwner> g_owner;

// ClearThunkState drops the original target after the slot is restored.
void ClearThunkState() {
  g_check_transmit_original = nullptr;
}

using CheckTransmitFn = void (*)(void* self, void** infos, int info_count, void* union_bits,
                                 void* always_bits, const void** networkables,
                                 const uint16_t* indices, int index_count);

// CheckTransmitThunk runs the engine's pass, then the handler for each
// player's filled set.
void CheckTransmitThunk(void* self, void** infos, int info_count, void* union_bits,
                        void* always_bits, const void** networkables, const uint16_t* indices,
                        int index_count) {
  reinterpret_cast<CheckTransmitFn>(g_check_transmit_original)(
      self, infos, info_count, union_bits, always_bits, networkables, indices, index_count);
  if (g_handlers.empty()) {
    return;
  }
  for (int i = 0; i < info_count; ++i) {
    if (infos[i] == nullptr) {
      continue;
    }
    TransmitSet set(infos[i]);
    for (const auto& [id, handler] : g_handlers) {
      handler(set);
    }
  }
}

}  // namespace

int32_t TransmitSet::Slot() const {
  int32_t slot = 0;
  std::memcpy(&slot, static_cast<const uint8_t*>(info_) + kSlotOffset, sizeof(slot));
  return slot;
}

void TransmitSet::Withhold(uint32_t handle) {
  // The engine clears its own withheld entities' bits the same way.
  const uint32_t index = handle & 0x7FFF;
  if (index >= kEntityCount) {
    return;
  }
  auto* bits = *static_cast<uint32_t**>(info_);
  if (bits == nullptr) {
    return;
  }
  bits[index / 32] &= ~(uint32_t{1} << (index % 32));
}

void TransmitSet::WithholdUnlessIn(uint32_t handle, uint64_t slots) {
  // A slot past the mask's 64 bits is never listed.
  const int32_t slot = Slot();
  if (slot >= 0 && slot < 64 && (slots >> slot) & 1) {
    return;
  }
  Withhold(handle);
}

TransmitHook::TransmitHook(uint64_t id) : id_(id) {}

TransmitHook::TransmitHook(TransmitHook&& other) noexcept : id_(std::exchange(other.id_, 0)) {}

TransmitHook& TransmitHook::operator=(TransmitHook&& other) noexcept {
  if (this != &other) {
    Reset();
    id_ = std::exchange(other.id_, 0);
  }
  return *this;
}

TransmitHook::~TransmitHook() {
  Reset();
}

void TransmitHook::Reset() {
  if (id_ == 0) {
    return;
  }
  std::erase_if(g_handlers, [this](const auto& entry) { return entry.first == id_; });
  id_ = 0;
  if (g_handlers.empty()) {
    g_owner.reset();
  }
}

std::expected<TransmitHook, std::string> TransmitHook::Install(Handler handler) {
  if (!g_owner) {
    const auto entities = ResolveEngineInterface(L"server.dll", kInterfaceVersion);
    if (!entities) {
      return std::unexpected(entities.error());
    }
    const auto slot = ResolveVirtualSlot("game-entities.check-transmit");
    if (!slot) {
      return std::unexpected(slot.error());
    }
    auto hook =
        VtableSlotHook::Install(*entities, *slot, reinterpret_cast<void*>(&CheckTransmitThunk));
    if (!hook) {
      return std::unexpected(hook.error());
    }
    g_check_transmit_original = hook->Original();
    g_owner.emplace(std::move(*hook), &ClearThunkState);
  }
  const uint64_t id = g_next_id++;
  g_handlers.emplace_back(id, std::move(handler));
  return TransmitHook(id);
}

}  // namespace modlock::gameinterop
