#include "modlock/gameinterop/thunk_owner.h"

#include <utility>

namespace modlock::gameinterop {

ThunkOwner::ThunkOwner(VtableSlotHook slot_hook, ClearThunk clear_thunk)
    : slot_hook_(std::move(slot_hook)), clear_thunk_(clear_thunk) {}

ThunkOwner::ThunkOwner(ThunkOwner&& other) noexcept
    : slot_hook_(std::move(other.slot_hook_)),
      clear_thunk_(std::exchange(other.clear_thunk_, nullptr)) {}

ThunkOwner& ThunkOwner::operator=(ThunkOwner&& other) noexcept {
  if (this != &other) {
    Reset();
    slot_hook_ = std::move(other.slot_hook_);
    clear_thunk_ = std::exchange(other.clear_thunk_, nullptr);
  }
  return *this;
}

ThunkOwner::~ThunkOwner() {
  Reset();
}

void ThunkOwner::Reset() {
  // Stop dispatch into the thunk before releasing the state it borrows.
  slot_hook_.Restore();
  if (const auto clear = std::exchange(clear_thunk_, nullptr)) {
    clear();
  }
}

}  // namespace modlock::gameinterop
