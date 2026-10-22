#include "modlock/gameinterop/vtable_slot_hook.h"

#include <utility>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace modlock::gameinterop {
namespace {

// TableOf reads the dispatch table an object points at. The vptr is the first
// word of the object.
void** TableOf(void* instance) {
  return *static_cast<void***>(instance);
}

// WriteSlot stores one entry into a live dispatch table. A module's table sits
// in read-only image memory, so Windows grants write access for the store and
// puts the original protection back. Test fixtures own writable tables and
// need no grant.
bool WriteSlot(void** slot, void* value) {
#if defined(_WIN32)
  // Unprotect the slot and write it.
  DWORD previous = 0;
  if (::VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &previous) == 0) {
    return false;
  }
  *slot = value;

  // Restore the protection and flush the write.
  DWORD restored = 0;
  ::VirtualProtect(slot, sizeof(void*), previous, &restored);
  ::FlushInstructionCache(::GetCurrentProcess(), slot, sizeof(void*));
  return true;

#else
  // Other platforms write the slot directly.
  *slot = value;
  return true;
#endif
}

}  // namespace

VtableSlotHook::VtableSlotHook(VtableSlotHook&& other) noexcept {
  *this = std::move(other);
}

VtableSlotHook& VtableSlotHook::operator=(VtableSlotHook&& other) noexcept {
  // Self-assignment keeps the hook.
  if (this == &other) {
    return *this;
  }

  // Restore this hook, then take the other's slot.
  Restore();
  slot_ = other.slot_;
  original_ = other.original_;
  other.slot_ = nullptr;
  other.original_ = nullptr;
  return *this;
}

VtableSlotHook::~VtableSlotHook() {
  Restore();
}

std::expected<VtableSlotHook, std::string> VtableSlotHook::Install(void* instance, size_t slot,
                                                                   void* replacement) {
  // Read the table the object points at.
  if (instance == nullptr || replacement == nullptr) {
    return std::unexpected("vtable slot hook requires an object and a replacement target");
  }
  void** table = TableOf(instance);
  if (table == nullptr) {
    return std::unexpected("object carries a null vtable pointer");
  }

  // Swap the slot, keeping the original.
  VtableSlotHook hook;
  hook.original_ = table[slot];
  if (!WriteSlot(&table[slot], replacement)) {
    return std::unexpected("dispatch table slot " + std::to_string(slot) +
                           " could not be made writable");
  }
  hook.slot_ = &table[slot];
  return hook;
}

void VtableSlotHook::Restore() {
  if (slot_ != nullptr) {
    WriteSlot(slot_, original_);
    slot_ = nullptr;
    original_ = nullptr;
  }
}

}  // namespace modlock::gameinterop
