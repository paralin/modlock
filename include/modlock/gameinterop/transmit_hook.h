#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <string>

#include "modlock/export.h"
#include "modlock/gameinterop/thunk_owner.h"
#include "modlock/gameinterop/vtable_slot_hook.h"

namespace modlock::gameinterop {

// TransmitSet is the set of entities one player's client receives in one
// network update. It borrows the engine's CCheckTransmitInfo for the length of
// the update.
class MODLOCK_API TransmitSet {
 public:
  // kSlotOffset holds the info's int32 player slot: CheckTransmit passes it
  // to the slot-to-controller lookup.
  static constexpr size_t kSlotOffset = 0x240;
  // kEntityCount is the bit count of the CBitVec<16384> the info's first
  // field points to; a set bit sends that entity index.
  static constexpr uint32_t kEntityCount = 16384;

  // TransmitSet borrows info, a CCheckTransmitInfo.
  explicit TransmitSet(void* info) : info_(info) {}

  // Slot returns the receiving player's server slot.
  int32_t Slot() const;

  // Withhold keeps the entity with the packed handle from this player, so
  // their client drops it until an update sends it again. A handle whose
  // index lies past the set is ignored.
  void Withhold(uint32_t handle);

 private:
  void* info_;
};

// TransmitHook lets the server withhold entities from single players. It
// patches ISource2GameEntities::CheckTransmit in server.dll to run the
// engine's own pass first, which fills every player's set, then hands each
// player's set to the handler on the same thread. One hook may exist at a
// time; the move and destruction ordering is ThunkOwner's contract.
class MODLOCK_API TransmitHook {
 public:
  // ISource2GameEntities::CheckTransmit dispatch slot in server.dll, after
  // the eleven IAppSystem entries and ApplyGameSettings.
  static constexpr size_t kCheckTransmitSlot = 12;
  // Server.dll's ISource2GameEntities interface version string.
  static constexpr char kInterfaceVersion[] = "Source2GameEntities001";

  // Handler adjusts one player's set after the engine filled it.
  using Handler = std::function<void(TransmitSet&)>;

  // Install resolves the mapped server.dll interface and hooks its
  // CheckTransmit slot. Failure names the missing piece.
  static std::expected<TransmitHook, std::string> Install(Handler handler);

  TransmitHook(TransmitHook&& other) noexcept = default;
  TransmitHook& operator=(TransmitHook&& other) noexcept = default;
  ~TransmitHook() = default;

 private:
  // Private by construction: only Install builds one, around a live slot hook.
  explicit TransmitHook(ThunkOwner owner);

  ThunkOwner owner_;
};

}  // namespace modlock::gameinterop
