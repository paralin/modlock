#include <array>
#include <cmath>
#include <cstring>
#include <expected>
#include <string>

#include "modlock/gameinterop/entity_abi.h"
#include "modlock/gameinterop/mapped_module_image.h"
#include "modlock/gameinterop/pawn_observer.h"
#include "modlock/gameinterop/virtual_slots.h"
#include "pawn_observer_internal.h"
#include "utl_vector.h"

namespace modlock::gameinterop {

namespace {

// RestoreIntegerField writes a networked integer field of pawn and
// replicates it.
bool RestoreIntegerField(void* pawn, const char* class_name, const char* field_name,
                         int32_t value) {
  // Find the field.
  if (!pawn) {
    return false;
  }
  auto schema = ResolveSchemaSystem();
  if (!schema) {
    return false;
  }
  auto field = SchemaFieldOf(*schema, "server.dll", class_name, field_name);
  if (!field || field->size != sizeof(int32_t) || !field->networked) {
    return false;
  }

  // Write it and replicate it.
  std::memcpy(static_cast<unsigned char*>(pawn) + field->offset, &value, sizeof(value));
  return NotifyEntityStateChanged(pawn);
}

// kMaxHeroLevel bounds the levels RestorePawnLevel writes.
constexpr int32_t kMaxHeroLevel = 100;

// UpgradeBonusLayout locates CCitadel_Modifier_HeroUpgradeBonuses on the
// running build: its dispatch table, the schema offsets of its owner and its
// three bonuses, and the change notification its own deserializer calls.
struct UpgradeBonusLayout {
  size_t modifier_property;
  size_t modifier_rows;
  uintptr_t table;
  size_t owner;
  std::array<size_t, 3> values;
  void (*notify)(void* modifier);
};

#if defined(_WIN32)
std::expected<UpgradeBonusLayout, std::string> LocateUpgradeBonuses() {
  // Find the modifier's vtable and its state notifier.
  constexpr const char* kClass = "CCitadel_Modifier_HeroUpgradeBonuses";
  const auto server = MappedModuleImage::ForModule(L"server.dll");
  if (!server) {
    return std::unexpected(server.error());
  }
  const auto table = FindVtable(*server, kClass);
  if (!table) {
    return std::unexpected(table.error());
  }
  const auto notify = ResolveSignature(*server, "modifier.state-changed");
  if (!notify) {
    return std::unexpected(notify.error());
  }

  // Resolve the schema system.
  auto schema = ResolveSchemaSystem();
  if (!schema) {
    return std::unexpected(schema.error());
  }

  // Resolve each field, checking that its span covers the read.
  const auto field = [&](const char* class_name, const char* name,
                         size_t size) -> std::expected<size_t, std::string> {
    auto found = SchemaFieldOf(*schema, "server.dll", class_name, name);
    if (!found) {
      return std::unexpected(found.error());
    }
    // SchemaField reports the span to the next declared field, including
    // padding and undeclared members; it bounds a read rather than naming the
    // type size.
    if (found->size < size) {
      return std::unexpected(std::string(name) + " spans " + std::to_string(found->size) +
                             " bytes");
    }
    return found->offset;
  };

  // Resolve the modifier fields.
  const auto property = field("CBaseEntity", "m_pModifierProp", sizeof(void*));
  const auto rows = field("CModifierProperty", "m_vecModifiers", 24);
  const auto owner = field(kClass, "m_pOwningPlayer", sizeof(void*));
  const auto weapon = field(kClass, "m_flWeaponPower", sizeof(float));
  const auto armor = field(kClass, "m_flArmorPower", sizeof(float));
  const auto tech = field(kClass, "m_flTechPower", sizeof(float));
  for (const auto* offset : {&property, &rows, &owner, &weapon, &armor, &tech}) {
    if (!*offset) {
      return std::unexpected(offset->error());
    }
  }
  return UpgradeBonusLayout{
      .modifier_property = *property,
      .modifier_rows = *rows,
      .table = server->base() + *table,
      .owner = *owner,
      .values = {*weapon, *armor, *tech},
      .notify = reinterpret_cast<void (*)(void*)>(*notify),
  };
}

// UpgradeBonusLayoutOnce locates the layout once for the process: server.dll
// and its schema stay loaded across worlds. Locating it walks the module's
// type information, which costs a frame, so Prepare does it at map load.
const std::expected<UpgradeBonusLayout, std::string>& UpgradeBonusLayoutOnce() {
  static const auto layout = LocateUpgradeBonuses();
  return layout;
}
#endif

// UpgradeBonuses is the pawn's upgrade bonus modifier and the layout that
// reads it.
struct UpgradeBonuses {
  unsigned char* modifier;
  const UpgradeBonusLayout* layout;
};

// UpgradeBonusMiss is why a pawn's upgrade bonus modifier was not found. The
// observer reads the bonuses every frame and drops the reason, so finding
// the modifier allocates nothing; DescribeMiss spells it out for a restore.
enum class UpgradeBonusMiss {
  kNoPawn,
  kNoLayout,
  kNoProperty,
  kBadVector,
  kNotFound,
  kUnsupported,
};

std::expected<UpgradeBonuses, UpgradeBonusMiss> UpgradeBonusModifier(void* pawn) {
#if defined(_WIN32)
  // Find the pawn's modifier property.
  if (!pawn) {
    return std::unexpected(UpgradeBonusMiss::kNoPawn);
  }
  const auto& layout = UpgradeBonusLayoutOnce();
  if (!layout) {
    return std::unexpected(UpgradeBonusMiss::kNoLayout);
  }
  unsigned char* property = nullptr;
  std::memcpy(&property, static_cast<unsigned char*>(pawn) + layout->modifier_property,
              sizeof(property));
  if (!property) {
    return std::unexpected(UpgradeBonusMiss::kNoProperty);
  }

  // Read the modifier rows.
  const auto rows = ReadUtlVector<unsigned char*>(property + layout->modifier_rows, kMaxModifiers);
  if (!rows) {
    return std::unexpected(UpgradeBonusMiss::kBadVector);
  }

  // Find the bonus modifier the pawn owns.
  for (int32_t i = 0; i < rows->count; ++i) {
    // The pawn's own row with the bonus table is the one.
    auto* row = rows->data[i];
    if (!row) {
      continue;
    }
    uintptr_t table = 0;
    std::memcpy(&table, row, sizeof(table));
    if (table != layout->table) {
      continue;
    }
    void* owner = nullptr;
    std::memcpy(&owner, row + layout->owner, sizeof(owner));
    if (owner == pawn) {
      return UpgradeBonuses{row, &*layout};
    }
  }
  return std::unexpected(UpgradeBonusMiss::kNotFound);
#else
  (void)pawn;
  return std::unexpected(UpgradeBonusMiss::kUnsupported);
#endif
}

// DescribeMiss names why the modifier was not found.
std::string DescribeMiss(UpgradeBonusMiss miss) {
  switch (miss) {
    case UpgradeBonusMiss::kNoPawn:
      return "upgrade bonuses: pawn unavailable";
    case UpgradeBonusMiss::kNoLayout:
#if defined(_WIN32)
      return "upgrade bonuses: " + UpgradeBonusLayoutOnce().error();
#else
      return "upgrade bonuses: layout unavailable";
#endif
    case UpgradeBonusMiss::kNoProperty:
      return "upgrade bonuses: modifier property unavailable";
    case UpgradeBonusMiss::kBadVector:
      return "upgrade bonuses: invalid modifier vector";
    case UpgradeBonusMiss::kNotFound:
      return "upgrade bonuses: modifier not found";
    case UpgradeBonusMiss::kUnsupported:
      return "upgrade bonuses require the Windows host";
  }
  return "upgrade bonuses: unavailable";
}

}  // namespace

void PrepareUpgradeBonuses() {
#if defined(_WIN32)
  static_cast<void>(UpgradeBonusLayoutOnce());
#endif
}

bool RestorePawnHealth(void* pawn, int32_t health, int32_t network_max_health) {
  return health > 0 && network_max_health >= 0 &&
         RestoreIntegerField(pawn, "CBaseEntity", "m_iMaxHealth", network_max_health) &&
         RestoreIntegerField(pawn, "CBaseEntity", "m_iHealth", health);
}

bool RestorePawnLevel(void* pawn, int32_t level) {
  return level >= 0 && level <= kMaxHeroLevel &&
         RestoreIntegerField(pawn, "CCitadelPlayerPawn", "m_nLevel", level);
}

std::optional<std::array<float, 3>> ReadPawnUpgradeBonuses(void* pawn) {
  // Read the three finite bonus values.
  const auto bonuses = UpgradeBonusModifier(pawn);
  if (!bonuses) {
    return std::nullopt;
  }
  std::array<float, 3> values;
  for (size_t i = 0; i < values.size(); ++i) {
    std::memcpy(&values[i], bonuses->modifier + bonuses->layout->values[i], sizeof(float));
    if (!std::isfinite(values[i])) {
      return std::nullopt;
    }
  }
  return values;
}

std::expected<void, std::string> RestorePawnUpgradeBonuses(void* pawn,
                                                           const std::array<float, 3>& values) {
  // Check the values and find the bonus modifier.
  for (float value : values) {
    if (!std::isfinite(value)) {
      return std::unexpected("upgrade bonuses: nonfinite value");
    }
  }
  const auto bonuses = UpgradeBonusModifier(pawn);
  if (!bonuses) {
    return std::unexpected(DescribeMiss(bonuses.error()));
  }

  // The modifier's own deserializer notifies before each changed field; this
  // follows it instead of calling it with a fabricated engine message.
  for (size_t i = 0; i < values.size(); ++i) {
    // Write each changed value, notifying first.
    auto* field = bonuses->modifier + bonuses->layout->values[i];
    float current;
    std::memcpy(&current, field, sizeof(current));
    if (current == values[i]) {
      continue;
    }
    bonuses->layout->notify(bonuses->modifier);
    std::memcpy(field, &values[i], sizeof(float));
  }
  const auto actual = ReadPawnUpgradeBonuses(pawn);
  if (!actual || *actual != values) {
    return std::unexpected("upgrade bonuses: readback mismatch");
  }
  return {};
}

}  // namespace modlock::gameinterop
