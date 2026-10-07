#include "modlock/gameinterop/transmit_hook.h"

#include <cstring>
#include <utility>

#include "modlock/gameinterop/mapped_module_image.h"
#include "modlock/gameinterop/thunk_owner.h"

namespace modlock::gameinterop {
namespace {

// The thunk state: one hook may exist at a time, so the replacement reads its
// original target and handler from these slots.
void* g_check_transmit_original = nullptr;
TransmitHook::Handler g_on_transmit;

// ClearThunkState drops the singleton thunk inputs after the slot is restored.
void ClearThunkState() {
  g_check_transmit_original = nullptr;
  g_on_transmit = nullptr;
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
  if (!g_on_transmit) return;
  for (int i = 0; i < info_count; ++i) {
    if (infos[i] == nullptr) continue;
    TransmitSet set(infos[i]);
    g_on_transmit(set);
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
  if (index >= kEntityCount) return;
  auto* bits = *static_cast<uint32_t**>(info_);
  if (bits == nullptr) return;
  bits[index / 32] &= ~(uint32_t{1} << (index % 32));
}

TransmitHook::TransmitHook(ThunkOwner owner) : owner_(std::move(owner)) {}

std::expected<TransmitHook, std::string> TransmitHook::Install(Handler handler) {
  const auto entities = ResolveEngineInterface(L"server.dll", kInterfaceVersion);
  if (!entities) return std::unexpected(entities.error());
  auto hook = VtableSlotHook::Install(*entities, kCheckTransmitSlot,
                                      reinterpret_cast<void*>(&CheckTransmitThunk));
  if (!hook) return std::unexpected(hook.error());
  g_check_transmit_original = hook->Original();
  g_on_transmit = std::move(handler);
  return TransmitHook(ThunkOwner(std::move(*hook), &ClearThunkState));
}

}  // namespace modlock::gameinterop
