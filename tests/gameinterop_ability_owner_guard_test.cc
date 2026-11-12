// Contract tests for the ability owner guard: finding abilities and their
// owners in the entity list, and withholding an ability from a player whose
// update does not send its live owner.
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "modlock/gameinterop/ability_owner_guard.h"
#include "modlock/gameinterop/entity_abi.h"
#include "modlock/gameinterop/transmit_hook.h"

namespace {

using modlock::gameinterop::AbilityClasses;
using modlock::gameinterop::FindOwnedAbilities;
using modlock::gameinterop::kIdentitySize;
using modlock::gameinterop::kInvalidEHandleIndex;
using modlock::gameinterop::kMaxEntitiesInList;
using modlock::gameinterop::OwnedAbility;
using modlock::gameinterop::OwnerState;
using modlock::gameinterop::TransmitSet;
using modlock::gameinterop::WithholdOwnerless;

// kOwnerOffset stands in for CBaseEntity::m_hOwnerEntity's schema offset.
constexpr size_t kOwnerOffset = 0x300;

// SchemaClass is an independently encoded SchemaClassInfoData_t holding its
// self binding, name and first base, the parts the class check reads.
struct SchemaClass {
  std::array<unsigned char, 0x40> info{};
  std::array<void*, 2> bases{};

  SchemaClass(const char* name, SchemaClass* base) {
    void* self = info.data();
    std::memcpy(info.data(), &self, sizeof(self));
    std::memcpy(info.data() + 0x08, &name, sizeof(name));
    if (base == nullptr) {
      return;
    }
    info[0x29] = 1;
    bases[1] = base->info.data();
    void* table = bases.data();
    std::memcpy(info.data() + 0x38, &table, sizeof(table));
  }
};

// EntityClass is a CEntityClass reaching a schema class through
// CEntityClass::m_pClassInfo and CEntityClassInfo::m_pSchemaBinding.
struct EntityClass {
  std::array<void*, 12> entity_class{};
  std::array<void*, 6> class_info{};

  explicit EntityClass(SchemaClass& schema) {
    entity_class[11] = class_info.data();
    class_info[5] = schema.info.data();
  }
};

// FakeEntitySystem lays out CGameEntitySystem's chunk table, with the first
// two chunks allocated, and the instances placed in them.
class FakeEntitySystem {
 public:
  FakeEntitySystem() {
    for (auto& chunk : chunks_) {
      chunk = std::make_unique<std::array<uint64_t, kMaxEntitiesInList * kIdentitySize / 8>>();
      chunk->fill(0);
    }
    for (size_t i = 0; i < chunks_.size(); ++i) {
      void* entries = chunks_[i]->data();
      std::memcpy(system_.data() + 0x10 + i * sizeof(void*), &entries, sizeof(entries));
    }
  }

  const void* System() const { return system_.data(); }

  // Place puts an entity of a class at index with the serial and owner, and
  // returns its handle.
  uint32_t Place(uint32_t index, EntityClass& type, uint32_t serial, uint32_t owner,
                 uint32_t flags = 0) {
    auto& instance = instances_.emplace_back(std::make_unique<std::array<uint8_t, 0x400>>());
    instance->fill(0);
    uint8_t* identity = IdentityAt(index);
    void* entity = instance->data();
    void* entity_class = type.entity_class.data();
    const uint32_t handle = index | (serial << 15);
    std::memcpy(identity, &entity, sizeof(entity));
    std::memcpy(identity + 0x08, &entity_class, sizeof(entity_class));
    std::memcpy(identity + 0x10, &handle, sizeof(handle));
    std::memcpy(identity + 0x30, &flags, sizeof(flags));
    std::memcpy(instance->data() + 0x10, &identity, sizeof(identity));
    std::memcpy(instance->data() + kOwnerOffset, &owner, sizeof(owner));
    return handle;
  }

  // Detach makes the identity at index stop pointing at a live instance's
  // back-pointer, as a freed slot does.
  void Detach(uint32_t index) { std::memset(IdentityAt(index), 0, sizeof(void*)); }

 private:
  uint8_t* IdentityAt(uint32_t index) {
    return reinterpret_cast<uint8_t*>(chunks_[index / kMaxEntitiesInList]->data()) +
           (index % kMaxEntitiesInList) * kIdentitySize;
  }

  alignas(8) std::array<uint8_t, 0x10 + 64 * sizeof(void*)> system_{};
  std::array<std::unique_ptr<std::array<uint64_t, kMaxEntitiesInList * kIdentitySize / 8>>, 2>
      chunks_;
  std::vector<std::unique_ptr<std::array<uint8_t, 0x400>>> instances_;
};

// FakeInfo lays out the CCheckTransmitInfo fields the set reads, with every
// entity sent.
struct FakeInfo {
  explicit FakeInfo(int32_t slot) {
    bits.fill(~uint32_t{0});
    uint32_t* pointer = bits.data();
    std::memcpy(bytes.data(), &pointer, sizeof(pointer));
    std::memcpy(bytes.data() + TransmitSet::kSlotOffset, &slot, sizeof(slot));
  }

  bool Sends(uint32_t index) const { return (bits[index / 32] >> (index % 32)) & 1; }

  void Clear(uint32_t index) { bits[index / 32] &= ~(uint32_t{1} << (index % 32)); }

  std::array<uint32_t, TransmitSet::kEntityCount / 32> bits;
  alignas(void*) std::array<uint8_t, TransmitSet::kSlotOffset + 8> bytes{};
};

bool CopyMemory(const void* source, void* out, size_t size) {
  std::memcpy(out, source, size);
  return true;
}

// World holds the classes a Deadlock server binds: a pawn, a hero ability, an
// item and the slide, each with its first base chain.
struct World {
  SchemaClass base{"CBaseEntity", nullptr};
  SchemaClass model{"CBaseModelEntity", &base};
  SchemaClass pawn_schema{"CCitadelPlayerPawn", &model};
  SchemaClass ability_schema{"CCitadelBaseAbility", &base};
  SchemaClass item_schema{"CCitadel_Item", &ability_schema};
  SchemaClass slide_schema{"CCitadel_Ability_Slide", &ability_schema};
  EntityClass pawn{pawn_schema};
  EntityClass item{item_schema};
  EntityClass slide{slide_schema};
  FakeEntitySystem entities;
  AbilityClasses classes;

  std::vector<OwnedAbility> Find() {
    return FindOwnedAbilities(entities.System(), kOwnerOffset, CopyMemory, classes);
  }
};

TEST(FindOwnedAbilities, ListsAbilitiesWithTheirLiveOwner) {
  // A pawn owns a slide and an item; the pawn itself is not an ability.
  World world;
  const uint32_t pawn = world.entities.Place(3, world.pawn, 7, kInvalidEHandleIndex);
  world.entities.Place(600, world.slide, 2, pawn);
  world.entities.Place(601, world.item, 9, pawn);

  const auto abilities = world.Find();
  ASSERT_EQ(abilities.size(), 2u);
  EXPECT_EQ(abilities[0].index, 600u);
  EXPECT_EQ(abilities[1].index, 601u);
  for (const auto& ability : abilities) {
    EXPECT_EQ(ability.owner, pawn);
    EXPECT_EQ(ability.state, OwnerState::kLive);
  }
}

TEST(FindOwnedAbilities, NamesEachWayAnOwnerFailsToResolve) {
  World world;
  const uint32_t pawn = world.entities.Place(3, world.pawn, 7, kInvalidEHandleIndex);
  const uint32_t deleting = world.entities.Place(4, world.pawn, 1, kInvalidEHandleIndex, 0x200);
  const uint32_t detached = world.entities.Place(5, world.pawn, 1, kInvalidEHandleIndex);
  world.entities.Detach(5);

  // No owner, a stale serial, a deleting owner, a freed slot and an empty one.
  world.entities.Place(10, world.slide, 1, kInvalidEHandleIndex);
  world.entities.Place(11, world.slide, 1, (pawn & 0x7FFF) | (8u << 15));
  world.entities.Place(12, world.slide, 1, deleting);
  world.entities.Place(13, world.slide, 1, detached);
  world.entities.Place(14, world.slide, 1, 900);
  world.entities.Place(15, world.slide, 1, 20000);

  const auto abilities = world.Find();
  ASSERT_EQ(abilities.size(), 6u);
  EXPECT_EQ(abilities[0].state, OwnerState::kNone);
  EXPECT_EQ(abilities[1].state, OwnerState::kGone);
  EXPECT_EQ(abilities[2].state, OwnerState::kDeleting);
  EXPECT_EQ(abilities[3].state, OwnerState::kGone);
  EXPECT_EQ(abilities[4].state, OwnerState::kGone);
  EXPECT_EQ(abilities[5].state, OwnerState::kGone);
}

TEST(FindOwnedAbilities, ListsAnAbilityTheGameIsDeleting) {
  World world;
  const uint32_t pawn = world.entities.Place(3, world.pawn, 7, kInvalidEHandleIndex);
  world.entities.Place(600, world.slide, 2, pawn, 0x10);

  const auto abilities = world.Find();
  ASSERT_EQ(abilities.size(), 1u);
  EXPECT_EQ(abilities[0].index, 600u);
}

TEST(FindOwnedAbilities, ListsNothingFromUnreadableMemory) {
  World world;
  world.entities.Place(600, world.slide, 2, kInvalidEHandleIndex);
  const auto abilities = FindOwnedAbilities(
      world.entities.System(), kOwnerOffset, [](const void*, void*, size_t) { return false; },
      world.classes);
  EXPECT_TRUE(abilities.empty());
  EXPECT_TRUE(FindOwnedAbilities(nullptr, kOwnerOffset, CopyMemory, world.classes).empty());
}

TEST(FindOwnedAbilities, RemembersEachSchemaClass) {
  World world;
  world.entities.Place(3, world.pawn, 7, kInvalidEHandleIndex);
  world.entities.Place(600, world.slide, 2, kInvalidEHandleIndex);
  world.entities.Place(601, world.slide, 3, kInvalidEHandleIndex);
  EXPECT_EQ(world.Find().size(), 2u);
  EXPECT_EQ(world.classes.size(), 2u);
  EXPECT_EQ(world.Find().size(), 2u);
}

TEST(WithholdOwnerless, SendsAnAbilityWhoseOwnerIsSent) {
  World world;
  const uint32_t pawn = world.entities.Place(3, world.pawn, 7, kInvalidEHandleIndex);
  world.entities.Place(600, world.slide, 2, pawn);
  const auto abilities = world.Find();

  FakeInfo info(0);
  TransmitSet set(info.bytes.data());
  EXPECT_TRUE(WithholdOwnerless(set, abilities).empty());
  EXPECT_TRUE(info.Sends(600));
  EXPECT_TRUE(info.Sends(3));
}

TEST(WithholdOwnerless, WithholdsAnAbilityWhoseOwnerIsNotSent) {
  // A mod or the game keeps the pawn from this player.
  World world;
  const uint32_t pawn = world.entities.Place(3, world.pawn, 7, kInvalidEHandleIndex);
  world.entities.Place(600, world.slide, 2, pawn);
  world.entities.Place(601, world.item, 2, pawn);
  const auto abilities = world.Find();

  FakeInfo info(4);
  info.Clear(3);
  TransmitSet set(info.bytes.data());
  const auto withheld = WithholdOwnerless(set, abilities);
  ASSERT_EQ(withheld.size(), 2u);
  EXPECT_FALSE(info.Sends(600));
  EXPECT_FALSE(info.Sends(601));
  EXPECT_TRUE(info.Sends(599));
  EXPECT_TRUE(info.Sends(602));

  // Another player who receives the pawn keeps its abilities.
  FakeInfo other(5);
  TransmitSet other_set(other.bytes.data());
  EXPECT_TRUE(WithholdOwnerless(other_set, abilities).empty());
  EXPECT_TRUE(other.Sends(600));
}

TEST(WithholdOwnerless, WithholdsAnAbilityWhoseOwnerDoesNotResolve) {
  // Each owner the client cannot resolve, though the owner's index is sent.
  World world;
  const uint32_t pawn = world.entities.Place(3, world.pawn, 7, kInvalidEHandleIndex);
  const uint32_t deleting = world.entities.Place(4, world.pawn, 1, kInvalidEHandleIndex, 0x10);
  world.entities.Place(10, world.slide, 1, kInvalidEHandleIndex);
  world.entities.Place(11, world.slide, 1, (pawn & 0x7FFF) | (8u << 15));
  world.entities.Place(12, world.slide, 1, deleting);
  world.entities.Place(13, world.slide, 1, 900);
  const auto abilities = world.Find();

  FakeInfo info(0);
  TransmitSet set(info.bytes.data());
  const auto withheld = WithholdOwnerless(set, abilities);
  EXPECT_EQ(withheld.size(), 4u);
  for (uint32_t index : {10u, 11u, 12u, 13u}) {
    EXPECT_FALSE(info.Sends(index)) << index;
  }
  EXPECT_TRUE(info.Sends(3));
  EXPECT_TRUE(info.Sends(4));
}

TEST(WithholdOwnerless, LeavesAnythingElseAlone) {
  // A pawn without an owner is not an ability, and an ability already
  // withheld is not reported again.
  World world;
  world.entities.Place(3, world.pawn, 7, 900);
  world.entities.Place(600, world.slide, 2, kInvalidEHandleIndex);
  const auto abilities = world.Find();

  FakeInfo info(0);
  info.Clear(600);
  TransmitSet set(info.bytes.data());
  EXPECT_TRUE(WithholdOwnerless(set, abilities).empty());
  EXPECT_TRUE(info.Sends(3));
  for (uint32_t index = 0; index < TransmitSet::kEntityCount; ++index) {
    EXPECT_EQ(info.Sends(index), index != 600) << index;
  }
}

}  // namespace
