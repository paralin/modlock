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

// WithholdOwnerless withholds from set each ability it sends whose owner is
// not a live entity it also sends, and returns those abilities.
//
// The client resolves an ability's owner handle against the entities it
// holds, and its ability code reads through the result without a null check:
// an ability that arrives without its owner crashes the client in the
// ability's post data update. The client creates
// every entity of an update before it runs their post data update callbacks,
// so an owner sent in the same update is enough.
[[nodiscard]] MODLOCK_API std::vector<OwnedAbility> WithholdOwnerless(
    TransmitSet& set, std::span<const OwnedAbility> abilities);

// AbilityOwnerGuard keeps every ability from a player whose update does not
// also send its owner, whatever the game or a mod did to the sets. It runs as
// the transmit hook's final handler, after every mod's handler, and only
// clears bits. It logs each ability it starts withholding from a player.
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
