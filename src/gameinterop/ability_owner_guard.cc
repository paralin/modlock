#include "modlock/gameinterop/ability_owner_guard.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>

#include "modlock/gameinterop/connection_tracker.h"
#include "modlock/gameinterop/entity_abi.h"

namespace modlock::gameinterop {
namespace {

// kIdentityClass is CEntityIdentity::m_pClass, the CEntityClass of the
// entity's designer name.
constexpr size_t kIdentityClass = 0x08;

// kAbilityClass is the schema class every hero ability and item derives from.
constexpr std::string_view kAbilityClass = "CCitadelBaseAbility";

// ReadValue copies a T from offset bytes into source through read, or returns
// no value when read refuses it.
template <typename T>
std::optional<T> ReadValue(const void* source, size_t offset, const BoundedReader& read) {
  T value{};
  if (source == nullptr ||
      !read(static_cast<const uint8_t*>(source) + offset, &value, sizeof(value))) {
    return std::nullopt;
  }
  return value;
}

// Identity is the start of one CEntityIdentity: its instance and its class.
struct Identity {
  void* instance;
  const void* entity_class;
};

// ReadIdentity copies the identity at index of a chunk and returns it when it
// belongs to an entity whose instance points back at it.
std::optional<Identity> ReadIdentity(const uint8_t* entries, uint32_t index,
                                     const BoundedReader& read) {
  const uint8_t* identity = entries + index * kIdentitySize;
  std::array<void*, 2> head{};
  if (!read(identity + kIdentityInstanceOffset, head.data(), sizeof(head)) || head[0] == nullptr) {
    return std::nullopt;
  }
  if (ReadValue<const void*>(head[0], kIdentityOffset, read) != identity) {
    return std::nullopt;
  }
  return Identity{head[0], head[1]};
}

// ChunkOf reads the address of the chunk holding entity index, or null.
const uint8_t* ChunkOf(const void* entity_system, uint32_t index, const BoundedReader& read) {
  const auto* chunks = static_cast<const uint8_t*>(entity_system) + kEntityListOffset;
  return static_cast<const uint8_t*>(
      ReadValue<const void*>(chunks, (index / kMaxEntitiesInList) * sizeof(void*), read)
          .value_or(nullptr));
}

// OwnerStateOf resolves an owner handle as the client would: the entity at its
// index must be live and hold the handle's serial.
OwnerState OwnerStateOf(const void* entity_system, uint32_t handle, const BoundedReader& read) {
  if (handle == kInvalidEHandleIndex) {
    return OwnerState::kNone;
  }

  // Find the identity at the handle's index.
  const uint32_t index = EntityIndexOf(handle);
  const uint8_t* entries = ChunkOf(entity_system, index, read);
  if (entries == nullptr) {
    return OwnerState::kGone;
  }
  const uint32_t slot = index % kMaxEntitiesInList;
  if (!ReadIdentity(entries, slot, read)) {
    return OwnerState::kGone;
  }

  // Its handle must match, serial and all.
  const uint8_t* identity = entries + slot * kIdentitySize;
  const auto stored = ReadValue<uint32_t>(identity, kIdentityEHandleOffset, read);
  const auto flags = ReadValue<uint32_t>(identity, kIdentityFlagsOffset, read);
  if (!stored || !flags) {
    return OwnerState::kGone;
  }
  if (LiveHandleOf(*stored, *flags & ~kEfDeleteFlags) != handle) {
    return OwnerState::kGone;
  }
  return (*flags & kEfDeleteFlags) != 0 ? OwnerState::kDeleting : OwnerState::kLive;
}

// HandleOf reads the handle of the entity at identity, index and serial, even
// while the game deletes it, or the invalid handle when it cannot be read.
uint32_t HandleOf(const uint8_t* identity, const BoundedReader& read) {
  const auto stored = ReadValue<uint32_t>(identity, kIdentityEHandleOffset, read);
  const auto flags = ReadValue<uint32_t>(identity, kIdentityFlagsOffset, read);
  if (!stored || !flags) {
    return kInvalidEHandleIndex;
  }
  return LiveHandleOf(*stored, *flags & ~kEfDeleteFlags).value_or(kInvalidEHandleIndex);
}

// IsAbilityClass reports whether entity_class binds a schema class deriving
// from CCitadelBaseAbility, remembering the answer for the schema class.
bool IsAbilityClass(const void* entity_class, const BoundedReader& read, AbilityClasses& classes) {
  const auto schema = SchemaClassOfEntityClass(entity_class, read);
  if (!schema) {
    return false;
  }
  auto [entry, added] = classes.try_emplace(*schema, false);
  if (added) {
    entry->second = SchemaClassDerivesFrom(*schema, kAbilityClass);
  }
  return entry->second;
}

}  // namespace

std::string_view OwnerStateName(OwnerState state) {
  switch (state) {
    case OwnerState::kLive:
      return "live";
    case OwnerState::kNone:
      return "none";
    case OwnerState::kGone:
      return "gone";
    case OwnerState::kDeleting:
      return "deleting";
  }
  return "unknown";
}

std::vector<OwnedAbility> FindOwnedAbilities(const void* entity_system, size_t owner_offset,
                                             const BoundedReader& read, AbilityClasses& classes) {
  std::vector<OwnedAbility> abilities;
  if (entity_system == nullptr) {
    return abilities;
  }
  for (uint32_t chunk = 0; chunk < kMaxEntityLists; ++chunk) {
    const uint8_t* entries = ChunkOf(entity_system, chunk * kMaxEntitiesInList, read);
    if (entries == nullptr) {
      continue;
    }
    for (uint32_t slot = 0; slot < kMaxEntitiesInList; ++slot) {
      // Keep each entity whose class is an ability's.
      const auto identity = ReadIdentity(entries, slot, read);
      if (!identity || !IsAbilityClass(identity->entity_class, read, classes)) {
        continue;
      }

      // Resolve what its owner handle names. An unreadable handle names no
      // owner.
      const uint32_t owner = ReadValue<uint32_t>(identity->instance, owner_offset, read)
                                 .value_or(kInvalidEHandleIndex);
      abilities.push_back(OwnedAbility{
          .entity = identity->instance,
          .index = chunk * kMaxEntitiesInList + slot,
          .handle = HandleOf(entries + slot * kIdentitySize, read),
          .owner = owner,
          .state = OwnerStateOf(entity_system, owner, read),
      });
    }
  }
  return abilities;
}

void HeldAbilities::Track(int32_t slot, bool occupied, uint32_t generation) {
  // A slot without its connection holds nothing.
  if (!occupied) {
    slots_.erase(slot);
    return;
  }

  // A new connection in the slot starts with nothing.
  auto [entry, added] = slots_.try_emplace(slot);
  if (!added && entry->second.generation != generation) {
    entry->second.handles.clear();
  }
  entry->second.generation = generation;
}

bool HeldAbilities::Holds(int32_t slot, const OwnedAbility& ability) const {
  const auto found = slots_.find(slot);
  if (found == slots_.end()) {
    return false;
  }
  const auto held = found->second.handles.find(ability.index);
  if (held == found->second.handles.end()) {
    return false;
  }
  return ability.handle == kInvalidEHandleIndex || held->second == ability.handle;
}

void HeldAbilities::Hold(int32_t slot, const OwnedAbility& ability) {
  // An untracked slot keeps no records.
  const auto found = slots_.find(slot);
  if (found == slots_.end()) {
    return;
  }

  // An unknown handle keeps a record of the ability before it.
  auto [held, added] = found->second.handles.try_emplace(ability.index, ability.handle);
  if (ability.handle != kInvalidEHandleIndex) {
    held->second = ability.handle;
  }
}

void HeldAbilities::Prune(std::span<const OwnedAbility> abilities) {
  // Index the abilities in the world by their entity index.
  std::unordered_map<uint32_t, uint32_t> present;
  present.reserve(abilities.size());
  for (const auto& ability : abilities) {
    present.insert_or_assign(ability.index, ability.handle);
  }

  // Forget each record whose ability left the world or whose index another
  // ability took. An unknown handle proves neither.
  for (auto& [slot, records] : slots_) {
    std::erase_if(records.handles, [&](const auto& record) {
      const auto found = present.find(record.first);
      if (found == present.end()) {
        return true;
      }
      return found->second != kInvalidEHandleIndex && record.second != kInvalidEHandleIndex &&
             found->second != record.second;
    });
  }
}

size_t HeldAbilities::Count(int32_t slot) const {
  const auto found = slots_.find(slot);
  return found == slots_.end() ? 0 : found->second.handles.size();
}

GuardedAbilities WithholdOwnerless(TransmitSet& set, std::span<const OwnedAbility> abilities,
                                   HeldAbilities& held) {
  GuardedAbilities guarded;
  const int32_t slot = set.Slot();
  for (const auto& ability : abilities) {
    if (!set.Sends(ability.index)) {
      continue;
    }

    // Withhold an ownerless ability the client cannot hold yet; one it may
    // hold stays, since clearing its bit drops it on the client only.
    const bool owned = ability.state == OwnerState::kLive && set.Sends(ability.owner);
    if (!owned && !held.Holds(slot, ability)) {
      set.Withhold(ability.index);
      guarded.withheld.push_back(ability);
      continue;
    }
    if (!owned) {
      guarded.kept.push_back(ability);
    }
    held.Hold(slot, ability);
  }
  return guarded;
}

// State is the guard's memory across passes, shared with its handler.
struct AbilityOwnerGuard::State {
  // kRelogPasses is how many passes a withhold must lapse before it is logged
  // again, about a second at the server's tick rate.
  static constexpr uint64_t kRelogPasses = 64;
  // kLinesPerMinute bounds the log a stuck or flapping withhold writes.
  static constexpr int kLinesPerMinute = 32;

  std::mutex mutex;
  AbilityClasses classes;
  std::optional<size_t> owner_offset;
  bool reported_offset = false;
  bool reported_system = false;
  uint64_t pass = 0;
  // seen holds the pass in which each (kind, slot, ability index) report last
  // happened.
  std::unordered_map<uint64_t, uint64_t> seen;
  // held records the abilities each player's client may hold.
  HeldAbilities held;
  std::chrono::steady_clock::time_point minute{};
  int lines = 0;
  size_t unlogged = 0;

  // OwnerOffset resolves CBaseEntity::m_hOwnerEntity once.
  std::optional<size_t> OwnerOffset() {
    if (owner_offset) {
      return owner_offset;
    }
    auto field = [&]() -> std::expected<SchemaField, std::string> {
      auto schema = ResolveSchemaSystem();
      if (!schema) {
        return std::unexpected(schema.error());
      }
      return SchemaFieldOf(*schema, "server.dll", "CBaseEntity", "m_hOwnerEntity");
    }();
    if (field && field->size < sizeof(uint32_t)) {
      field = std::unexpected("CBaseEntity::m_hOwnerEntity spans less than a handle");
    }
    if (!field) {
      if (!reported_offset) {
        std::cerr << "[modlock] ability owner guard idle: " << field.error() << '\n';
        reported_offset = true;
      }
      return std::nullopt;
    }
    owner_offset = field->offset;
    return owner_offset;
  }

  // Log reports one withhold, or one ability kept because the client may
  // hold it, unless the same report for that player and ability was logged
  // within the last second or the minute's lines are spent.
  void Log(int32_t slot, const OwnedAbility& ability, bool kept) {
    const uint64_t key =
        (uint64_t{kept} << 63) | (uint64_t{static_cast<uint32_t>(slot)} << 32) | ability.index;
    auto [entry, added] = seen.try_emplace(key, pass);
    const bool lapsed = added || pass - entry->second > kRelogPasses;
    entry->second = pass;
    if (!lapsed) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - minute >= std::chrono::minutes(1)) {
      if (unlogged > 0) {
        std::cerr << "[modlock] ability owner guard: " << unlogged
                  << " more withholds were not logged\n";
      }
      minute = now;
      lines = 0;
      unlogged = 0;
    }
    if (lines >= kLinesPerMinute) {
      ++unlogged;
      return;
    }
    ++lines;
    char owner[16];
    std::snprintf(owner, sizeof(owner), "0x%08x", ability.owner);
    std::cerr << "[modlock] ability owner guard: " << (kept ? "kept " : "withheld ")
              << DesignerName(ability.entity) << " (entity " << ability.index << ") "
              << (kept ? "for slot " : "from slot ") << slot
              << (kept ? ", whose client may hold it" : "") << ": owner " << owner << ' '
              << OwnerStateName(ability.state)
              << (ability.state == OwnerState::kLive ? ", not sent" : "") << '\n';
  }

  // Pass withholds the ownerless abilities of one transmit pass.
  void Pass(std::span<TransmitSet> sets) {
    const std::lock_guard lock(mutex);
    ++pass;
    const auto offset = OwnerOffset();
    if (!offset) {
      return;
    }
    const auto system = ResolveLiveEntitySystem();
    if (!system || *system == nullptr) {
      if (!reported_system) {
        std::cerr << "[modlock] ability owner guard skipped a pass: "
                  << (system ? std::string("no entity system") : system.error()) << '\n';
        reported_system = true;
      }
      return;
    }
    const auto abilities = FindOwnedAbilities(*system, *offset, &ReadNative, classes);
    held.Prune(abilities);
    for (auto& set : sets) {
      // A slot's records belong to its current connection.
      const int32_t slot = set.Slot();
      const auto connection = ConnectionTracker::StateForSlot(slot);
      held.Track(slot, connection.occupied, connection.generation);

      // Withhold what the client cannot hold yet, and report both kinds.
      const auto guarded = WithholdOwnerless(set, abilities, held);
      for (const auto& ability : guarded.withheld) {
        Log(slot, ability, false);
      }
      for (const auto& ability : guarded.kept) {
        Log(slot, ability, true);
      }
    }

    // Forget withholds long past, so the memory stays bounded.
    if (seen.size() > 4096) {
      std::erase_if(seen, [this](const auto& entry) { return pass - entry.second > kRelogPasses; });
    }
  }
};

AbilityOwnerGuard::AbilityOwnerGuard(TransmitHook hook, std::shared_ptr<State> state)
    : hook_(std::move(hook)), state_(std::move(state)) {}

AbilityOwnerGuard::~AbilityOwnerGuard() = default;

std::expected<AbilityOwnerGuard, std::string> AbilityOwnerGuard::Install() {
  auto state = std::make_shared<State>();
  auto hook =
      TransmitHook::InstallFinal([state](std::span<TransmitSet> sets) { state->Pass(sets); });
  if (!hook) {
    return std::unexpected(hook.error());
  }
  return AbilityOwnerGuard(std::move(*hook), std::move(state));
}

}  // namespace modlock::gameinterop
