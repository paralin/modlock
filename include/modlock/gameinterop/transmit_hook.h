#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <span>
#include <string>

#include "modlock/export.h"
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

  // Sends reports whether this player's client receives the entity with the
  // packed handle in this update. A handle whose index lies past the set is
  // never sent.
  bool Sends(uint32_t handle) const;

  // Withhold keeps the entity with the packed handle from this player, so
  // their client drops it until an update sends it again. A handle whose
  // index lies past the set is ignored.
  void Withhold(uint32_t handle);

  // WithholdUnlessIn keeps the entity from this player unless the bit of
  // their slot is set in slots, so the entity reaches only those players.
  void WithholdUnlessIn(uint32_t handle, uint64_t slots);

 private:
  void* info_;
};

// TransmitHook lets the server withhold entities from single players. It
// patches ISource2GameEntities::CheckTransmit in server.dll to run the
// engine's own pass first, which fills every player's set, then hands each
// player's set to every installed handler on the same thread, oldest first.
// A final handler then sees every player's set at once, after every handler,
// so it can enforce a rule over the sets the others left.
// Several hooks may exist at once, such as a mod host's and a native
// plugin's: the first patches the slot and the last one destroyed restores
// it. Install, the handlers and destruction belong to the engine thread.
class MODLOCK_API TransmitHook {
 public:
  // Server.dll's ISource2GameEntities interface version string.
  static constexpr char kInterfaceVersion[] = "Source2GameEntities001";

  // Handler adjusts one player's set after the engine filled it. It must not
  // install or destroy a TransmitHook.
  using Handler = std::function<void(TransmitSet&)>;

  // FinalHandler adjusts the sets of one pass after every Handler ran on them,
  // once per pass. It must not install or destroy a TransmitHook.
  using FinalHandler = std::function<void(std::span<TransmitSet>)>;

  // Install resolves the mapped server.dll interface and hooks its
  // CheckTransmit slot. Failure names the missing piece.
  static std::expected<TransmitHook, std::string> Install(Handler handler);

  // InstallFinal hooks the slot as Install does, for a final handler. Final
  // handlers run oldest first.
  static std::expected<TransmitHook, std::string> InstallFinal(FinalHandler handler);

  TransmitHook(TransmitHook&& other) noexcept;
  TransmitHook& operator=(TransmitHook&& other) noexcept;
  ~TransmitHook();
  TransmitHook(const TransmitHook&) = delete;
  TransmitHook& operator=(const TransmitHook&) = delete;

 private:
  // Private by construction: only Install builds one, around a registered
  // handler.
  explicit TransmitHook(uint64_t id);

  // Reset removes the handler, and restores the slot after the last one.
  void Reset();

  // Hook patches the slot unless a handler already did.
  static std::expected<void, std::string> Hook();

  // id_ names the handler in the shared list; zero once moved from.
  uint64_t id_ = 0;
};

}  // namespace modlock::gameinterop
