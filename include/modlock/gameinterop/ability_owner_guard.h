#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "modlock/export.h"
#include "modlock/gameinterop/entity_abi.h"
#include "modlock/gameinterop/native_memory.h"
#include "modlock/gameinterop/transmit_hook.h"

namespace modlock::gameinterop {

// OwnerState is what an ability's m_hOwnerEntity names in the server's entity
// list.
enum class OwnerState : uint8_t {
  // kLive is a live entity holding the handle.
  kLive,
  // kNone is the invalid handle: the ability has no owner.
  kNone,
  // kGone is a handle whose entity was removed, or whose index now holds
  // another entity.
  kGone,
  // kDeleting is an entity the game marked for deletion.
  kDeleting,
};

// OwnerStateName names a state for the log, such as "gone".
[[nodiscard]] MODLOCK_API std::string_view OwnerStateName(OwnerState state);

// OwnedAbility is one ability entity and the owner its m_hOwnerEntity names.
struct OwnedAbility {
  // entity is the ability's instance, borrowed for the pass.
  void* entity = nullptr;
  // index is the ability's entity index.
  uint32_t index = 0;
  // handle is the ability's own handle, index and serial, or the invalid
  // handle when its identity cannot prove one.
  uint32_t handle = kInvalidEHandleIndex;
  // owner is the handle m_hOwnerEntity holds.
  uint32_t owner = kInvalidEHandleIndex;
  // state is what the handle names.
  OwnerState state = OwnerState::kNone;
};

// AbilityClasses remembers which schema classes derive from
// CCitadelBaseAbility. A schema class is static data of server.dll, so an
// answer holds for the process.
using AbilityClasses = std::unordered_map<const void*, bool>;

// FindOwnedAbilities lists the abilities in entity_system's chunk list, items
// and hero abilities alike, with what each one's owner handle names. An entity
// the game is deleting is listed too, since it may still be sent. It reads
// every link through read and skips an entry it cannot read, so it lists
// fewer abilities rather than faulting. owner_offset is the offset of
// CBaseEntity::m_hOwnerEntity.
[[nodiscard]] MODLOCK_API std::vector<OwnedAbility> FindOwnedAbilities(const void* entity_system,
                                                                       size_t owner_offset,
                                                                       const BoundedReader& read,
                                                                       AbilityClasses& classes);

// HeldAbilities remembers, for each player slot, every ability the guard let
// through to that player's client, by index and serial. Such a client may
// hold the ability: the server counts an entity it sent as held, and clearing
// its bit later makes the client drop an entity the server still updates,
// which closes the client with "CopyExistingEntity: missing client entity".
// A record lasts until the ability leaves the world, another entity takes its
// index, or the slot's connection ends.
class MODLOCK_API HeldAbilities {
 public:
  // Track starts a pass for slot. A slot no connection occupies, or occupied
  // by a connection of another generation than the one recorded, holds
  // nothing.
  void Track(int32_t slot, bool occupied, uint32_t generation);

  // Holds reports whether the client in slot may hold ability. An ability
  // whose own handle is unknown is held when anything at its index is.
  [[nodiscard]] bool Holds(int32_t slot, const OwnedAbility& ability) const;

  // Hold records that the client in slot receives ability.
  void Hold(int32_t slot, const OwnedAbility& ability);

  // Prune forgets every record whose ability is not in abilities, the
  // abilities in the world now, with the same handle.
  void Prune(std::span<const OwnedAbility> abilities);

  // Count returns how many abilities the client in slot may hold.
  [[nodiscard]] size_t Count(int32_t slot) const;

 private:
  // Slot is one connection's records: the ability handle at each index.
  struct Slot {
    uint32_t generation = 0;
    std::unordered_map<uint32_t, uint32_t> handles;
  };

  std::unordered_map<int32_t, Slot> slots_;
};

// GuardedAbilities is what one pass of the guard did to one player's set.
struct GuardedAbilities {
  // withheld lists the ownerless abilities it kept from the client.
  std::vector<OwnedAbility> withheld;
  // kept lists the ownerless abilities it still sent, since the client may
  // hold them.
  std::vector<OwnedAbility> kept;
};

// WithholdOwnerless withholds from set each ability it sends whose owner is
// not a live entity it also sends, unless held says the client may already
// hold that ability. It records in held every ability the set still sends.
// held must be tracked for the set's slot first.
//
// The client resolves an ability's owner handle against the entities it
// holds, and its ability code reads through the result without a null check:
// an ability that arrives without its owner crashes the client. The crash
// dumps place the fault in the subclass change callback the client runs as it
// creates the ability. The client creates every entity of an update before it
// runs their post data update callbacks, so an owner sent in the same update
// is enough. An ability the client already holds is not created again, and
// clearing its bit would make the client drop an entity the server still
// counts as held, so it stays.
[[nodiscard]] MODLOCK_API GuardedAbilities
WithholdOwnerless(TransmitSet& set, std::span<const OwnedAbility> abilities, HeldAbilities& held);

// AbilityOwnerGuard keeps every ability from a player whose update does not
// also send its owner, whatever the game or a mod did to the sets, unless that
// player's client may already hold it. It runs as the transmit hook's final
// handler, after every mod's handler, and only clears bits. It logs each
// ability it starts withholding from a player, and each it keeps sending
// because the client may hold it.
// Install and destruction belong to the engine thread.
class MODLOCK_API AbilityOwnerGuard {
 public:
  // Install hooks the transmit check. Failure names the missing piece.
  static std::expected<AbilityOwnerGuard, std::string> Install();

  AbilityOwnerGuard(AbilityOwnerGuard&&) noexcept = default;
  AbilityOwnerGuard& operator=(AbilityOwnerGuard&&) noexcept = default;
  ~AbilityOwnerGuard();

 private:
  struct State;

  AbilityOwnerGuard(TransmitHook hook, std::shared_ptr<State> state);

  TransmitHook hook_;
  std::shared_ptr<State> state_;
};

}  // namespace modlock::gameinterop
