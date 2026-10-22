#include "modlock/gameinterop/pawn_observer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

#include "modlock/gameinterop/entity_abi.h"
#include "modlock/gameinterop/mapped_module_image.h"
#include "modlock/gameinterop/native_damage.h"
#include "modlock/gameinterop/virtual_slots.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace modlock::gameinterop {
namespace {

// Layout constants follow the Source SDK entity declarations:
// public/entity2/entityidentity.h (CEntityIdentity "Size: 0x70", field
// comments), public/entityhandle.h (m_Parts bitfields), public/eiface.h
// (ISource2GameClients), public/appframework/iappsystem.h (eleven base
// slots), and entity2/entitysystem.h plus entity2/concreteentitylist.h
// (CGameEntitySystem member order and the chunk table).
inline constexpr uint32_t kMaxEntitiesInList = 512;      // MAX_ENTITIES_IN_LIST
inline constexpr uint32_t kMaxEntityLists = 64;          // MAX_ENTITY_LISTS
inline constexpr size_t kIdentityInstanceOffset = 0x00;  // m_pInstance
inline constexpr size_t kIdentityEHandleOffset = 0x10;   // m_EHandle
inline constexpr size_t kIdentityFlagsOffset = 0x30;     // m_flags
inline constexpr uint32_t kEntityIndexBits = 0x7FFF;     // m_Parts.m_EntityIndex : 15
inline constexpr uint32_t kSerialShift = 15;             // m_Parts.m_Serial : 17
inline constexpr uint32_t kSerialBits = 0x1FFFF;
inline constexpr uint32_t kInvalidEHandleIndex = 0xFFFFFFFF;
inline constexpr uint32_t kEfInvalidEHandle = 0x1;  // EF_IS_INVALID_EHANDLE

// Offset of CConcreteEntityList m_EntityList inside CGameEntitySystem: the
// vptr and IEntityResourceManifest* m_pCurrentManifest precede it.
inline constexpr size_t kEntityListOffset = 0x10;

// ScoreCounter names one PlayerDataGlobal_t scoreboard counter, its offset in
// ScoreLayout and its value in ScoreTotals.
struct ScoreCounter {
  const char* field_name;
  size_t ScoreLayout::* offset;
  int32_t ScoreTotals::* total;
};

// kScoreCounters lists every scoreboard counter the observer reads.
constexpr ScoreCounter kScoreCounters[] = {
    {"m_iHeroDamage", &ScoreLayout::hero_damage, &ScoreTotals::hero_damage},
    {"m_iHeroHealing", &ScoreLayout::hero_healing, &ScoreTotals::hero_healing},
    {"m_iSelfHealing", &ScoreLayout::self_healing, &ScoreTotals::self_healing},
    {"m_iPlayerKills", &ScoreLayout::kills, &ScoreTotals::kills},
    {"m_iDeaths", &ScoreLayout::deaths, &ScoreTotals::deaths},
    {"m_iPlayerAssists", &ScoreLayout::assists, &ScoreTotals::assists},
    {"m_iLastHits", &ScoreLayout::last_hits, &ScoreTotals::last_hits},
    {"m_iDenies", &ScoreLayout::denies, &ScoreTotals::denies},
    {"m_iGoldNetWorth", &ScoreLayout::net_worth, &ScoreTotals::net_worth},
};

// EntityListOf reads the concrete entity list out of one live CGameEntitySystem.
unsigned char* EntityListOf(void* entity_system) {
  return static_cast<unsigned char*>(entity_system) + kEntityListOffset;
}

// IdentityOfIndex reproduces CEntitySystem::GetEntityIdentity(CEntityIndex)
// (sourcesdk entity2/entitysystem.cpp): chunk-table pick plus the identity's
// own index proof, so a stale identity slot never resolves.
unsigned char* IdentityOfIndex(unsigned char* entity_list, uint32_t index) {
  // Find the identity's slot in its chunk.
  if (index >= kMaxEntitiesInList * kMaxEntityLists) {
    return nullptr;
  }
  auto* chunks = reinterpret_cast<unsigned char**>(entity_list);
  unsigned char* chunk = chunks[index / kMaxEntitiesInList];
  if (chunk == nullptr) {
    return nullptr;
  }
  unsigned char* identity = chunk + (index % kMaxEntitiesInList) * 0x70;

  // The identity must hold this index and a live instance.
  uint32_t ehandle = 0;
  std::memcpy(&ehandle, identity + kIdentityEHandleOffset, sizeof(ehandle));
  if ((ehandle & kEntityIndexBits) != index) {
    return nullptr;
  }
  void* instance = nullptr;
  std::memcpy(&instance, identity + kIdentityInstanceOffset, sizeof(instance));
  if (instance == nullptr) {
    return nullptr;
  }

  // CEntityInstance::m_pEntity must point back at the identity that produced
  // the instance (sourcesdk public/entity2/entityinstance.h).
  void* back = nullptr;
  std::memcpy(&back, static_cast<unsigned char*>(instance) + kIdentityOffset, sizeof(back));
  if (back != identity) {
    return nullptr;
  }
  return identity;
}

// IdentityOfHandle reproduces CEntitySystem::GetEntityIdentity(CEntityHandle):
// same chunk-table pick, then GetRefEHandle proof where the stored serial is
// corrected by EF_IS_INVALID_EHANDLE before comparison.
unsigned char* IdentityOfHandle(unsigned char* entity_list, uint32_t handle) {
  // Find the identity at the handle's index.
  if (handle == kInvalidEHandleIndex) {
    return nullptr;
  }
  const uint32_t index = handle & kEntityIndexBits;
  auto* identity = IdentityOfIndex(entity_list, index);
  if (identity == nullptr) {
    return nullptr;
  }

  // Skip an identity marked for deletion.
  uint32_t ehandle = 0;
  std::memcpy(&ehandle, identity + kIdentityEHandleOffset, sizeof(ehandle));
  uint32_t flags = 0;
  std::memcpy(&flags, identity + kIdentityFlagsOffset, sizeof(flags));
  constexpr uint32_t kDeleteFlags = 0x10 | 0x200;
  if ((flags & kDeleteFlags) != 0) {
    return nullptr;
  }

  // The handle's serial must match the identity's.
  const uint32_t ref_serial =
      ((ehandle >> kSerialShift) - (flags & kEfInvalidEHandle)) & kSerialBits;
  const uint32_t ref_handle = (ehandle & kEntityIndexBits) | (ref_serial << kSerialShift);
  if (ref_handle != handle) {
    return nullptr;
  }
  return identity;
}

void* InstanceOf(unsigned char* identity) {
  void* instance = nullptr;
  std::memcpy(&instance, identity + kIdentityInstanceOffset, sizeof(instance));
  return instance;
}

const float* AbsOriginOf(const EntityLayout& layout, void* pawn) {
  // Follow the body component to its scene node's origin.
  void* body = nullptr;
  std::memcpy(&body, static_cast<const unsigned char*>(pawn) + layout.body_component, sizeof(body));
  if (body == nullptr) {
    return nullptr;
  }
  void* scene_node = nullptr;
  std::memcpy(&scene_node, static_cast<const unsigned char*>(body) + layout.scene_node,
              sizeof(scene_node));
  if (scene_node == nullptr) {
    return nullptr;
  }
  return reinterpret_cast<const float*>(static_cast<const unsigned char*>(scene_node) +
                                        layout.abs_origin);
}

std::expected<StaminaLayout, std::string> ResolveStaminaLayout();
void PrepareUpgradeBonuses();

uint32_t ReadUint32(const unsigned char* base, size_t offset) {
  uint32_t value = 0;
  std::memcpy(&value, base + offset, sizeof(value));
  return value;
}

std::optional<PawnObserver::Sample::MovementState> ReadMovementState(const EntityLayout& layout,
                                                                     void* pawn) {
  // Without movement fields there is no movement state.
  if (!layout.movement.has_value()) {
    return std::nullopt;
  }

  // Read velocity, ground and action states where their fields exist.
  const MovementLayout& movement = *layout.movement;
  const auto* pawn_bytes = static_cast<const unsigned char*>(pawn);
  PawnObserver::Sample::MovementState state;
  if (movement.abs_velocity.has_value()) {
    std::array<float, 3> velocity{};
    std::memcpy(velocity.data(), pawn_bytes + *movement.abs_velocity, sizeof(velocity));
    if (std::isfinite(velocity[0]) && std::isfinite(velocity[1]) && std::isfinite(velocity[2])) {
      state.abs_velocity = velocity;
    }
  }
  if (movement.ground_entity.has_value()) {
    state.ground_entity_handle = ReadUint32(pawn_bytes, *movement.ground_entity);
    state.grounded_by_handle = *state.ground_entity_handle != kInvalidEHandleIndex;
  }
  if (movement.modifier_property && movement.modifier_state_mask) {
    const unsigned char* property = nullptr;
    std::memcpy(&property, pawn_bytes + *movement.modifier_property, sizeof(property));
    if (property) {
      const auto enabled = [&](uint16_t flag) {
        const auto word = ReadUint32(property, *movement.modifier_state_mask + (flag / 32) * 4);
        return (word & (uint32_t{1} << (flag % 32))) != 0;
      };
      // EModifierState::GroundDashing and AirDashing are native action states.
      state.dashing = enabled(0x25) || enabled(0x29);
      state.climbing = enabled(0xbc);
    }
  }
  return state;
}

// ResolveEntityLayout reads core offsets through the game's own schema system.
// Each movement field is optional: a missing field or a storage span too small
// for its direct type omits only that field and leaves core observation usable.
std::expected<EntityLayout, std::string> ResolveEntityLayout(bool movement_enabled) {
  // Resolve schema fields, checking each one's storage size.
  auto schema_system = ResolveSchemaSystem();
  if (!schema_system) {
    return std::unexpected(schema_system.error());
  }
  auto resolve_schema_field = [&](const char* class_name, const char* field_name,
                                  size_t minimum_size) -> std::expected<SchemaField, std::string> {
    auto field = SchemaFieldOf(schema_system.value(), "server.dll", class_name, field_name);
    if (!field) {
      return std::unexpected(field.error());
    }
    if (field->size < minimum_size) {
      return std::unexpected(std::string("schema field '") + class_name + "." + field_name +
                             " storage span is too small");
    }
    return *field;
  };
  auto resolve_field = [&](const char* class_name, const char* field_name,
                           size_t minimum_size) -> std::expected<size_t, std::string> {
    auto field = resolve_schema_field(class_name, field_name, minimum_size);
    if (!field) {
      return std::unexpected(field.error());
    }
    return field->offset;
  };

  // Resolve the core fields that every observation needs.
  EntityLayout layout{};
  size_t hero_component = 0, spawned_hero = 0, hero_id = 0;
  struct CoreField {
    const char* class_name;
    const char* field_name;
    size_t minimum_size;
    size_t* out;
  };
  CoreField core[] = {
      {"CBasePlayerController", "m_hPawn", sizeof(uint32_t), &layout.pawn_handle},
      {"CBaseEntity", "m_CBodyComponent", sizeof(void*), &layout.body_component},
      {"CBodyComponent", "m_pSceneNode", sizeof(void*), &layout.scene_node},
      {"CGameSceneNode", "m_vecAbsOrigin", sizeof(float) * 3, &layout.abs_origin},
      {"CCitadelPlayerPawn", "m_CCitadelHeroComponent", 1, &hero_component},
      {"CCitadelHeroComponent", "m_spawnedHero", 1, &spawned_hero},
      {"CitadelHeroSpawnData_t", "m_nHeroID", sizeof(uint32_t), &hero_id},
      {"CBaseEntity", "m_iTeamNum", sizeof(uint8_t), &layout.team},
      {"CBaseEntity", "m_iHealth", sizeof(int32_t), &layout.health},
      {"CBaseEntity", "m_iMaxHealth", sizeof(int32_t), &layout.max_health},
      {"CCitadelPlayerPawn", "m_nLevel", sizeof(int32_t), &layout.level},
      {"CCitadelPlayerPawn", "m_angEyeAngles", sizeof(float) * 3, &layout.eye_angles},
      {"CCitadelPlayerPawn", "m_angClientCamera", sizeof(float) * 3, &layout.camera_angles},
  };
  for (const auto& wanted : core) {
    auto field = resolve_field(wanted.class_name, wanted.field_name, wanted.minimum_size);
    if (!field) {
      return std::unexpected(field.error());
    }
    *wanted.out = field.value();
  }

  // Resolve the optional controller and view fields.
  if (auto field = SchemaFieldOf(schema_system.value(), "server.dll", "CCitadelPlayerController",
                                 "m_hHeroPawn")) {
    layout.hero_pawn_handle = field->offset;
  }
  if (auto field =
          SchemaFieldOf(*schema_system, "server.dll", "CCitadelPlayerController", "m_ePlayState");
      field && field->size >= sizeof(int32_t)) {
    layout.play_state = field->offset;
  }
  if (auto field =
          SchemaFieldOf(*schema_system, "server.dll", "CBaseModelEntity", "m_vecViewOffset");
      field && field->size >= 0x24) {
    layout.view_offset = field->offset;
  }

  // Hero component and spawn data are inline schema structures.
  layout.hero_id = hero_component + spawned_hero + hero_id;

  // Native ECurrencyType places gold, ability points and unlocks first in
  // m_nCurrencies. Missing economy schema does not disable pose observation.
  auto currencies =
      SchemaFieldOf(*schema_system, "server.dll", "CCitadelPlayerPawn", "m_nCurrencies");
  if (currencies && currencies->size >= sizeof(int32_t) * 3) {
    layout.currencies = currencies->offset;
  }

  // Missing scoreboard schema leaves pose observation available. Each inline
  // counter must fit both its own field and the controller's enclosing storage.
  const auto data =
      SchemaFieldOf(*schema_system, "server.dll", "CCitadelPlayerController", "m_PlayerDataGlobal");
  ScoreLayout score{};
  bool score_available = data.has_value();
  for (const auto& counter : kScoreCounters) {
    if (!score_available) {
      break;
    }
    const auto field =
        SchemaFieldOf(*schema_system, "server.dll", "PlayerDataGlobal_t", counter.field_name);
    if (!field || field->size < sizeof(int32_t) || field->offset > data->size ||
        data->size - field->offset < sizeof(int32_t)) {
      score_available = false;
      break;
    }
    score.*counter.offset = field->offset;
  }
  if (score_available) {
    score.player_data_global = data->offset;
    layout.score = score;
  }

  // Missing stamina schema leaves pose observation available.
  if (auto stamina = ResolveStaminaLayout()) {
    layout.stamina = *stamina;
  }

  // Movement fields are optional and read only when enabled.
  MovementLayout movement{};
  if (!movement_enabled) {
    return layout;
  }
  struct OptionalField {
    const char* class_name;
    const char* field_name;
    size_t minimum_size;
    std::optional<size_t>* out;
  };
  OptionalField optional[] = {
      {"CBaseEntity", "m_vecAbsVelocity", sizeof(float) * 3, &movement.abs_velocity},
      {"CBaseEntity", "m_hGroundEntity", sizeof(uint32_t), &movement.ground_entity},
  };
  bool movement_available = false;
  for (const auto& wanted : optional) {
    auto field = resolve_field(wanted.class_name, wanted.field_name, wanted.minimum_size);
    if (field.has_value()) {
      *wanted.out = field.value();
      movement_available = true;
    }
  }

  // Modifier states mark dashing and climbing.
  const auto property = resolve_field("CBaseEntity", "m_pModifierProp", sizeof(void*));
  const auto mask = resolve_field("CModifierProperty", "m_bvEnabledStateMask", 24);
  if (property && mask) {
    movement.modifier_property = *property;
    movement.modifier_state_mask = *mask;
    movement_available = true;
  }
  if (movement_available) {
    layout.movement = movement;
  }
  return layout;
}

std::expected<AbilityLayout, std::string> ResolveAbilityLayout() {
  // Resolve the schema system.
  auto schema = ResolveSchemaSystem();
  if (!schema) {
    return std::unexpected(schema.error());
  }

  // Resolve the required ability fields.
  AbilityLayout layout;
  struct Wanted {
    const char* type;
    const char* name;
    size_t minimum;
    size_t* offset;
  };
  const Wanted fields[] = {
      {"CCitadelPlayerPawn", "m_CCitadelAbilityComponent", 24, &layout.component},
      {"CCitadelAbilityComponent", "m_vecAbilities", 24, &layout.handles},
      {"CBaseEntity", "m_hOwnerEntity", 4, &layout.owner},
      {"CBaseEntity", "m_nSubclassID", 4, &layout.subclass},
      {"CCitadelBaseAbility", "m_eAbilitySlot", 2, &layout.slot},
      {"CCitadelBaseAbility", "m_nUpgradeInfo", 4, &layout.upgrade_info},
      {"CCitadelBaseAbility", "m_iRemainingCharges", 4, &layout.charges},
      {"CCitadelBaseAbility", "m_flCooldownEnd", 4, &layout.cooldown_end},
      {"CCitadelBaseAbility", "m_flCooldownStart", 4, &layout.cooldown_start},
      {"CCitadelBaseAbility", "m_flChargeRechargeStart", 4, &layout.charge_recharge_start},
      {"CCitadelBaseAbility", "m_flChargeRechargeEnd", 4, &layout.charge_recharge_end},
  };
  for (const auto& wanted : fields) {
    auto field = SchemaFieldOf(*schema, "server.dll", wanted.type, wanted.name);
    if (!field) {
      return std::unexpected(field.error());
    }
    if (field->size < wanted.minimum) {
      return std::unexpected(std::string(wanted.name) + " storage is too small");
    }
    *wanted.offset = field->offset;
  }
  layout.handles += layout.component;

  // Resolve the optional wall jump fields.
  const auto wall_count =
      SchemaFieldOf(*schema, "server.dll", "CCitadel_Ability_Jump", "m_nConsecutiveWallJumps");
  if (wall_count && wall_count->size >= sizeof(int8_t)) {
    layout.consecutive_wall_jumps = wall_count->offset;
  }
  const auto wall_normal =
      SchemaFieldOf(*schema, "server.dll", "CCitadel_Ability_Jump", "m_vCurrentWallNormal");
  if (wall_normal && wall_normal->size >= 3 * sizeof(float)) {
    layout.current_wall_normal = wall_normal->offset;
  }
  const auto contact_position = SchemaFieldOf(*schema, "server.dll", "CCitadel_Ability_Jump",
                                              "m_vLastValidWallJumpNormal_PlayerPosition");
  if (contact_position && contact_position->size >= 3 * sizeof(float)) {
    layout.wall_contact_position = contact_position->offset;
  }
  const auto normal_used =
      SchemaFieldOf(*schema, "server.dll", "CCitadel_Ability_Jump", "m_vWallJumpNormalUsed");
  if (normal_used && normal_used->size >= 3 * sizeof(float)) {
    layout.wall_jump_normal_used = normal_used->offset;
  }

  // Resolve the optional jump facing and zipline fields.
  const auto facing =
      SchemaFieldOf(*schema, "server.dll", "CCitadel_Ability_Jump", "m_eWallJumpFacing");
  if (facing && facing->size >= sizeof(uint16_t)) {
    layout.wall_jump_facing = facing->offset;
  }
  const auto zipline_time =
      SchemaFieldOf(*schema, "server.dll", "CCitadel_Ability_Jump", "m_flLastTimeOnZipLine");
  if (zipline_time && zipline_time->size >= sizeof(float)) {
    layout.last_time_on_zipline = zipline_time->offset;
  }

  // Resolve the optional action fields: channeling, sliding and mantling.
  const auto channeling =
      SchemaFieldOf(*schema, "server.dll", "CCitadelBaseAbility", "m_bChanneling");
  if (channeling && channeling->size >= 1) {
    layout.channeling = channeling->offset;
  }
  const auto sliding =
      SchemaFieldOf(*schema, "server.dll", "CCitadel_Ability_Slide", "m_bIsSliding");
  if (sliding && sliding->size >= 1) {
    layout.sliding = sliding->offset;
  }
  const auto mantle_start =
      SchemaFieldOf(*schema, "server.dll", "CCitadel_Ability_Mantle", "m_flStartTime");
  if (mantle_start && mantle_start->size >= sizeof(float)) {
    layout.mantle_start_time = mantle_start->offset;
  }
  return layout;
}

// ResolveStaminaLayout resolves the pawn's inline stamina resource floats
// through the game's schema system. AbilityResource_t is the value type of
// CCitadelAbilityComponent::m_ResourceStamina; its offsets are relative to the
// resource struct, so they compose with the component and resource offsets the
// same way the hero-id chain does.
std::expected<StaminaLayout, std::string> ResolveStaminaLayout() {
  // Find the stamina resource inside the ability component.
  auto schema = ResolveSchemaSystem();
  if (!schema) {
    return std::unexpected(schema.error());
  }
  const auto component =
      SchemaFieldOf(*schema, "server.dll", "CCitadelPlayerPawn", "m_CCitadelAbilityComponent");
  const auto resource =
      SchemaFieldOf(*schema, "server.dll", "CCitadelAbilityComponent", "m_ResourceStamina");
  if (!component || !resource) {
    return std::unexpected("stamina resource component unavailable");
  }

  // Resolve the resource's float fields.
  struct Wanted {
    const char* name;
    size_t* offset;
  };
  StaminaLayout offsets;
  size_t current = 0, max = 0, latch_time = 0, latch_value = 0;
  const Wanted fields[] = {
      {"m_flCurrentValue", &current},
      {"m_flMaxValue", &max},
      {"m_flLatchTime", &latch_time},
      {"m_flLatchValue", &latch_value},
  };
  for (const auto& wanted : fields) {
    auto field = SchemaFieldOf(*schema, "server.dll", "AbilityResource_t", wanted.name);
    if (!field) {
      return std::unexpected(field.error());
    }
    if (field->size < sizeof(float)) {
      return std::unexpected(std::string(wanted.name) + " storage is too small for a float");
    }
    // Only the latch pair carries network metadata; current/max are plain
    // schema floats the engine mutates locally.
    const bool latch = wanted.offset == fields[2].offset || wanted.offset == fields[3].offset;
    if (latch && !field->networked) {
      return std::unexpected(std::string(wanted.name) + " lacks network metadata");
    }
    *wanted.offset = field->offset;
  }

  // Offset each field from the pawn.
  offsets.current = component->offset + resource->offset + current;
  offsets.max = component->offset + resource->offset + max;
  offsets.latch_time = component->offset + resource->offset + latch_time;
  offsets.latch_value = component->offset + resource->offset + latch_value;
  return offsets;
}

}  // namespace

std::expected<AbilityChargeSlots, std::string> ResolveAbilityChargeSlots(
    const ModuleImage& server) {
  // HeroRefresh calls HasCharges, GetMaxCharges and SetCharges in that order.
  // The loop tail distinguishes it from another refresh path with the same calls.
  const auto address = ResolveSignature(server, "ability.refresh-charges");
  if (!address) {
    return std::unexpected(address.error());
  }

  // Read the two virtual slot offsets from the call sites.
  const auto offset = reinterpret_cast<uintptr_t>(*address) - server.base();
  const auto* bytes = server.image_bytes().data() + offset;
  uint32_t has_charges = 0;
  uint32_t max_charges = 0;
  std::memcpy(&has_charges, bytes + 2, sizeof(has_charges));
  std::memcpy(&max_charges, bytes + 18, sizeof(max_charges));
  if (has_charges % sizeof(void*) || max_charges % sizeof(void*)) {
    return std::unexpected("ability charge queries have unaligned virtual slots");
  }
  return AbilityChargeSlots{has_charges / sizeof(void*), max_charges / sizeof(void*)};
}

PawnObserver::Seams PawnObserver::LiveSeams(bool movement_enabled) {
  // Read entities from the live entity system.
  Seams seams;
  seams.entity_system = [] {
    return ResolveLiveEntitySystem();
  };

  // Occupancy and xuid come from the engine-reported client lifecycle only.
  seams.slot_state = [](int32_t slot) {
    return ConnectionTracker::StateForSlot(slot);
  };

  // Resolve the layouts from the server's schema.
  seams.layout = [movement_enabled] {
    return ResolveEntityLayout(movement_enabled);
  };
  seams.ability_layout = [] {
    return ResolveAbilityLayout();
  };

  // Read the calculated maximum health through the pawn's override.
  seams.effective_max_health = [](void* pawn) -> std::optional<int32_t> {
    // Find the pawn's maximum health override.
    if (!pawn) {
      return std::nullopt;
    }
    const auto slot = ResolveVirtualSlot("entity.max-health");
    if (!slot) {
      return std::nullopt;
    }
    auto** table = *static_cast<void***>(pawn);
    if (!table || !table[*slot]) {
      return std::nullopt;
    }

    // The pawn override evaluates progression and modifiers; m_iMaxHealth
    // can remain zero even while this calculated maximum is positive.
    using GetMaxHealth = int32_t (*)(void*);
    const int32_t value = reinterpret_cast<GetMaxHealth>(table[*slot])(pawn);
    return value > 0 ? std::optional<int32_t>{value} : std::nullopt;
  };

  // Resolve once per observer; the loaded server module outlives its pawn seams.
  seams.max_charges = [slots = std::optional<std::expected<AbilityChargeSlots, std::string>>{}](
                          void* ability) mutable -> std::expected<int32_t, std::string> {
#if defined(_WIN32)
    // Resolve the charge query slots once.
    const auto server = reinterpret_cast<const unsigned char*>(GetModuleHandleW(L"server.dll"));
    if (!server || !ability) {
      return std::unexpected("native charge capability unavailable");
    }
    if (!slots) {
      const auto image = MappedModuleImage::ForModule(L"server.dll");
      if (!image) {
        return std::unexpected(image.error());
      }
      slots = ResolveAbilityChargeSlots(*image);
    }
    if (!*slots) {
      return std::unexpected(slots->error());
    }
    const auto queries = **slots;

    // Both queries must run mapped server code.
    auto** table = *static_cast<void***>(ability);
    MEMORY_BASIC_INFORMATION memory{};
    if (!table || !VirtualQuery(table, &memory, sizeof(memory)) ||
        memory.AllocationBase != server || memory.State != MEM_COMMIT ||
        reinterpret_cast<uintptr_t>(table) +
                (std::max(queries.has_charges, queries.max_charges) + 1) * sizeof(void*) >
            reinterpret_cast<uintptr_t>(memory.BaseAddress) + memory.RegionSize) {
      return std::unexpected("ability charge vtable is outside the mapped server");
    }
    for (const auto slot : {queries.has_charges, queries.max_charges}) {
      if (!VirtualQuery(table[slot], &memory, sizeof(memory)) || memory.AllocationBase != server ||
          memory.State != MEM_COMMIT ||
          !(memory.Protect &
            (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) {
        return std::unexpected("ability charge target is not mapped server code");
      }
    }

    // Read the maximum, or zero for an ability without charges.
    using HasCharges = bool (*)(void*);
    using MaxCharges = int32_t (*)(void*);
    if (!reinterpret_cast<HasCharges>(table[queries.has_charges])(ability)) {
      return 0;
    }
    const auto maximum = reinterpret_cast<MaxCharges>(table[queries.max_charges])(ability);
    if (maximum <= 0) {
      return std::unexpected("native ability charge maximum is invalid");
    }
    return maximum;
#else
    return std::unexpected("native ability charge capability requires Windows");
#endif
  };

  // Resolve the native lookups ahead of the first observation.
  seams.prepare = [] {
    static_cast<void>(ResolveVirtualSlot("entity.max-health"));
    PrepareUpgradeBonuses();
  };
  return seams;
}

PawnObserver::PawnObserver() : PawnObserver(LiveSeams(), false) {}

PawnObserver::PawnObserver(Seams seams, bool movement_enabled)
    : seams_(std::move(seams)), movement_enabled_(movement_enabled) {}

void PawnObserver::NoteDegradation(const char* reason) {
  if (!degradation_logged_) {
    degradation_logged_ = true;
    std::fprintf(stderr, "[modlock] pawn observation degraded: %s\n", reason);
    std::fflush(stderr);
  }
}

std::expected<void, std::string> PawnObserver::ResolveLayout() {
  // Resolve the layout once and keep it.
  if (offsets_ok_) {
    return {};
  }
  if (!seams_.layout) {
    return std::unexpected("player layout unavailable");
  }
  auto layout = seams_.layout();
  if (!layout) {
    return std::unexpected(layout.error());
  }
  layout_ = *layout;
  offsets_ok_ = true;
  return {};
}

void PawnObserver::Prepare() {
  static_cast<void>(ResolveLayout());
  if (seams_.prepare) {
    seams_.prepare();
  }
}

std::expected<std::pair<unsigned char*, unsigned char*>, std::string>
PawnObserver::ControllerOfSlot(int32_t slot) {
  // Check the slot and the layout.
  if (slot < 0 || slot >= 64 || !seams_.entity_system) {
    return std::unexpected("player controller unavailable");
  }
  if (auto resolved = ResolveLayout(); !resolved) {
    return std::unexpected(resolved.error());
  }
  if (!layout_.hero_pawn_handle) {
    return std::unexpected("native hero pawn handle unavailable");
  }

  // Find the controller at index slot + 1.
  auto system = seams_.entity_system();
  if (!system) {
    return std::unexpected(system.error());
  }
  auto* list = EntityListOf(*system);
  auto* identity = IdentityOfIndex(list, slot + 1);
  auto* controller = identity ? static_cast<unsigned char*>(InstanceOf(identity)) : nullptr;
  if (!controller) {
    return std::unexpected("player controller unavailable");
  }
  return std::pair{controller, list};
}

std::expected<void*, std::string> PawnObserver::CameraPawnForSlot(
    int32_t slot, const PlayerSelectionCalls& calls) {
  // Resolve the player's controller.
  auto current = ControllerOfSlot(slot);
  if (!current) {
    return std::unexpected(current.error());
  }

  // Read its active pawn and its hero.
  auto [controller, list] = *current;
  uint32_t handle = 0;
  uint32_t hero = 0;
  std::memcpy(&handle, controller + layout_.pawn_handle, sizeof(handle));
  std::memcpy(&hero, controller + *layout_.hero_pawn_handle, sizeof(hero));
  auto* identity = IdentityOfHandle(list, handle);
  void* pawn = identity ? InstanceOf(identity) : nullptr;

  // A spectator's pawn is its camera, and so is any pawn other than the hero:
  // an earlier watch's camera or the game's own camera after the hero died.
  if (controller[layout_.team] == 1 || (pawn && handle != hero)) {
    if (!pawn) {
      return std::unexpected("the player's camera is not ready");
    }
    return pawn;
  }

  // A player with a hero watches through a camera of its own while the hero
  // stays where it stands, as the game binds one when the hero dies. Record
  // whatever camera the spawn bound, even one the watch then refuses, so
  // ReturnToHero removes it.
  if (!calls.spawn_observer) {
    return std::unexpected("native camera unavailable");
  }
  void* created = calls.spawn_observer(controller);
  current = ControllerOfSlot(slot);
  if (!current) {
    return std::unexpected(current.error());
  }

  // Record the camera now bound, and accept it only if the spawn made it.
  std::memcpy(&handle, current->first + layout_.pawn_handle, sizeof(handle));
  identity = IdentityOfHandle(current->second, handle);
  pawn = identity ? InstanceOf(identity) : nullptr;
  if (pawn && handle != hero) {
    cameras_.insert_or_assign(slot, handle);
  }
  if (!created || pawn != created) {
    return std::unexpected("the player's camera is not ready");
  }
  return pawn;
}

std::expected<uint32_t, std::string> PawnObserver::ReturnToHero(int32_t slot,
                                                                const PlayerSelectionCalls& calls) {
  // The controller's binding decides, not the record: a camera bound by an
  // earlier connection in the slot, or one whose watch failed partway, still
  // returns to a living hero.
  const auto camera = cameras_.find(slot);
  const uint32_t owned = camera == cameras_.end() ? 0 : camera->second;
  auto current = ControllerOfSlot(slot);
  if (!current) {
    cameras_.erase(slot);
    return std::unexpected(current.error());
  }

  // Read the player's active pawn and hero.
  auto [controller, list] = *current;
  uint32_t handle = 0;
  uint32_t hero = 0;
  std::memcpy(&handle, controller + layout_.pawn_handle, sizeof(handle));
  std::memcpy(&hero, controller + *layout_.hero_pawn_handle, sizeof(hero));

  // Read the hero's health.
  auto* identity = IdentityOfHandle(list, hero);
  void* pawn = identity ? InstanceOf(identity) : nullptr;
  int32_t health = 0;
  if (pawn) {
    std::memcpy(&health, static_cast<const char*>(pawn) + layout_.health, sizeof(health));
  }

  // A spectator has no hero to return to, and a dead hero's respawn returns
  // the player once the game binds it; until then the camera stays bound.
  if (controller[layout_.team] == 1 || handle == hero || !pawn || health <= 0) {
    if (owned == handle && handle != hero) {
      return 0;
    }
    cameras_.erase(slot);
    return owned;
  }
  if (!calls.set_pawn) {
    return std::unexpected("the player's hero is not ready");
  }
  calls.set_pawn(controller, pawn, false, false, false, false);
  cameras_.erase(slot);
  return handle;
}

std::vector<int32_t> PawnObserver::WatchingSlots() const {
  std::vector<int32_t> slots;
  for (const auto& [slot, camera] : cameras_) {
    slots.push_back(slot);
  }
  return slots;
}

std::expected<void, std::string> PawnObserver::SelectPlayer(int32_t slot, uint64_t steam_id,
                                                            uint32_t generation, int32_t team,
                                                            void* definition,
                                                            const PlayerSelectionCalls& calls) {
  // Check the request and the layout.
  if (slot < 0 || slot >= 64 || !steam_id || team < 1 || team > 3 ||
      (team != 1 && (!definition || !calls.create_pawn || !calls.select_hero)) ||
      (team == 1 && (!calls.spawn_observer || !calls.set_pawn)) || !seams_.slot_state ||
      !seams_.entity_system) {
    return std::unexpected("player selection unavailable");
  }
  if (auto resolved = ResolveLayout(); !resolved) {
    return resolved;
  }
  if (!layout_.hero_pawn_handle) {
    return std::unexpected("native hero pawn handle unavailable");
  }

  // Borrow the controller again after each native call, and stop if the player changed.
  void* expected_controller = nullptr;
  auto borrow = [&]() -> std::expected<std::pair<void*, void*>, std::string> {
    // The connection must still be the same player's.
    const auto state = seams_.slot_state(slot);
    if (!state.occupied || state.is_bot || state.xuid != steam_id ||
        state.generation != generation) {
      return std::unexpected("player connection changed during selection");
    }

    // Find the controller, which must be the one seen before.
    auto system = seams_.entity_system();
    if (!system) {
      return std::unexpected(system.error());
    }
    auto* identity = IdentityOfIndex(EntityListOf(*system), slot + 1);
    if (!identity) {
      return std::unexpected("player controller unavailable");
    }
    void* controller = InstanceOf(identity);
    if (expected_controller && controller != expected_controller) {
      return std::unexpected("player controller changed during selection");
    }
    expected_controller = controller;
    return std::pair{controller, *system};
  };

  // Find the controller's team change.
  auto current = borrow();
  if (!current) {
    return std::unexpected(current.error());
  }
  const auto change_team = ResolveVirtualSlot("player-controller.change-team");
  if (!change_team) {
    return std::unexpected(change_team.error());
  }
  auto* table = *static_cast<void***>(current->first);
  if (!table || !table[*change_team]) {
    return std::unexpected("native team selection unavailable");
  }

  // Move to the spectators, or to the new team.
  uint8_t current_team = 0;
  std::memcpy(&current_team, static_cast<const char*>(current->first) + layout_.team, 1);
  if (current_team != team && team == 1) {
    // The game's own move to the spectators releases the hero before the team
    // changes: it clears the hero handle and unbinds the pawn. A client whose
    // hero stays bound reads its hero's data after the change and crashes.
    auto* controller = static_cast<unsigned char*>(current->first);
    uint32_t hero = 0;
    std::memcpy(&hero, controller + *layout_.hero_pawn_handle, sizeof(hero));
    if (IdentityOfHandle(EntityListOf(current->second), hero)) {
      hero = kInvalidEHandleIndex;
      std::memcpy(controller + *layout_.hero_pawn_handle, &hero, sizeof(hero));
      if (!NotifyEntityStateChanged(controller)) {
        return std::unexpected("player hero release unavailable");
      }
    }
    calls.set_pawn(controller, nullptr, true, false, false, false);
    current = borrow();
    if (!current) {
      return std::unexpected(current.error());
    }
  }
  if (current_team != team) {
    reinterpret_cast<void (*)(void*, int)>(table[*change_team])(current->first, team);
    current = borrow();
    if (!current) {
      return std::unexpected(current.error());
    }
  }

  // Find the pawn, creating it when the player has none.
  const size_t pawn_field = team == 1 ? layout_.pawn_handle : *layout_.hero_pawn_handle;
  uint32_t handle = 0;
  std::memcpy(&handle, static_cast<const char*>(current->first) + pawn_field, sizeof(handle));
  auto* identity = IdentityOfHandle(EntityListOf(current->second), handle);
  void* pawn = identity ? InstanceOf(identity) : nullptr;
  if (!pawn) {
    const auto pending = pending_selections_.find(slot);
    if (pending != pending_selections_.end() && pending->second.steam_id == steam_id &&
        pending->second.generation == generation && pending->second.team == team) {
      return std::unexpected("native pawn creation awaiting controller publication");
    }
    void* created =
        team == 1 ? calls.spawn_observer(current->first) : calls.create_pawn(current->first, team);
    current = borrow();
    if (!current) {
      return std::unexpected(current.error());
    }
    if (!created) {
      return std::unexpected("native pawn creation failed");
    }
    pending_selections_.insert_or_assign(slot, PendingSelection{steam_id, generation, team});
    // Reborrow through the controller before writing to the created pawn. A
    // returned object alone does not prove that it is still this player's pawn.
    std::memcpy(&handle, static_cast<const char*>(current->first) + pawn_field, sizeof(handle));
    identity = IdentityOfHandle(EntityListOf(current->second), handle);
    pawn = identity ? InstanceOf(identity) : nullptr;
    if (!pawn) {
      return std::unexpected("native pawn creation awaiting controller publication");
    }
    if (pawn != created) {
      return std::unexpected("player pawn changed during creation");
    }
  }

  // Select the hero, then confirm the player is unchanged.
  pending_selections_.erase(slot);
  if (team != 1) {
    calls.select_hero(pawn, definition);
  }
  current = borrow();
  if (!current) {
    return std::unexpected(current.error());
  }
  return {};
}

std::optional<uint8_t> PawnObserver::ControllerTeam(int32_t slot) {
  const auto current = ControllerOfSlot(slot);
  if (!current) {
    return std::nullopt;
  }
  return current->first[layout_.team];
}

std::expected<bool, std::string> PawnObserver::SeatForChoice(int32_t slot, uint64_t steam_id,
                                                             uint32_t generation, int32_t team,
                                                             const PlayerSelectionCalls& calls) {
  // Check the request, the layout and the team change.
  if (slot < 0 || slot >= 64 || !steam_id || team < 2 || team > 3 || !calls.create_pawn ||
      !seams_.slot_state || !seams_.entity_system) {
    return std::unexpected("player seating unavailable");
  }
  if (auto resolved = ResolveLayout(); !resolved) {
    return std::unexpected(resolved.error());
  }
  if (!layout_.hero_pawn_handle) {
    return std::unexpected("native hero pawn handle unavailable");
  }
  if (!layout_.play_state) {
    return std::unexpected("native play state unavailable");
  }
  const auto change_team = ResolveVirtualSlot("player-controller.change-team");
  if (!change_team) {
    return std::unexpected(change_team.error());
  }

  // Borrow the controller again after each native call, and stop if the player changed.
  void* expected_controller = nullptr;
  auto borrow = [&]() -> std::expected<unsigned char*, std::string> {
    // The connection must still be the same player's.
    const auto state = seams_.slot_state(slot);
    if (!state.occupied || state.is_bot || state.xuid != steam_id ||
        state.generation != generation) {
      return std::unexpected("player connection changed during seating");
    }

    // Find the controller, which must be the one seen before.
    auto system = seams_.entity_system();
    if (!system) {
      return std::unexpected(system.error());
    }
    auto* identity = IdentityOfIndex(EntityListOf(*system), slot + 1);
    void* controller = identity ? InstanceOf(identity) : nullptr;
    if (!controller) {
      return std::unexpected("player controller unavailable");
    }
    if (expected_controller && controller != expected_controller) {
      return std::unexpected("player controller changed during seating");
    }
    expected_controller = controller;
    return static_cast<unsigned char*>(controller);
  };

  // Seat only a player who has not yet joined a team.
  auto controller = borrow();
  if (!controller) {
    return std::unexpected(controller.error());
  }
  if ((*controller)[layout_.team] != 0) {
    return false;
  }
  auto* table = *reinterpret_cast<void***>(*controller);
  if (!table || !table[*change_team]) {
    return std::unexpected("native team selection unavailable");
  }

  // The game's own join runs these three steps in order: ChangeTeam, the hero
  // pawn, then the play state. The state replicates to the client, which opens
  // the hero picker in place of the team screen.
  reinterpret_cast<void (*)(void*, int)>(table[*change_team])(*controller, team);
  controller = borrow();
  if (!controller) {
    return std::unexpected(controller.error());
  }

  // Create the hero pawn unless the controller holds one.
  uint32_t hero = 0;
  std::memcpy(&hero, *controller + *layout_.hero_pawn_handle, sizeof(hero));
  auto system = seams_.entity_system();
  if (!system) {
    return std::unexpected(system.error());
  }
  if (!IdentityOfHandle(EntityListOf(*system), hero)) {
    calls.create_pawn(*controller, team);
    controller = borrow();
    if (!controller) {
      return std::unexpected(controller.error());
    }
  }

  // Mark the player as choosing a hero.
  constexpr int32_t kChoosingHero = 2;  // EPlayerPlayState::EPlayerChoosingHero
  std::memcpy(*controller + *layout_.play_state, &kChoosingHero, sizeof(kChoosingHero));
  if (!NotifyEntityStateChanged(*controller)) {
    return std::unexpected("player play state did not replicate");
  }
  return true;
}

std::expected<void, std::string> PawnObserver::RespawnPlayer(int32_t slot, uint32_t generation,
                                                             RespawnPawn respawn) {
  // Check the request and the connection.
  if (slot < 0 || slot >= 64 || !respawn || !seams_.slot_state || !seams_.entity_system) {
    return std::unexpected("native player respawn unavailable");
  }
  const auto connection = seams_.slot_state(slot);
  if (!connection.occupied || connection.generation != generation) {
    return std::unexpected("player connection changed before respawn");
  }
  if (auto resolved = ResolveLayout(); !resolved) {
    return resolved;
  }
  if (!layout_.hero_pawn_handle) {
    return std::unexpected("native hero pawn handle unavailable");
  }

  // Find the player's controller.
  auto system = seams_.entity_system();
  if (!system) {
    return std::unexpected(system.error());
  }
  auto* controller_identity = IdentityOfIndex(EntityListOf(*system), slot + 1);
  auto* controller = controller_identity ? InstanceOf(controller_identity) : nullptr;
  if (!controller) {
    return std::unexpected("player controller unavailable");
  }

  // Find its hero pawn.
  uint32_t handle = 0;
  std::memcpy(&handle, static_cast<const char*>(controller) + *layout_.hero_pawn_handle,
              sizeof(handle));
  auto* identity = IdentityOfHandle(EntityListOf(*system), handle);
  auto* pawn = identity ? InstanceOf(identity) : nullptr;
  if (!pawn) {
    return std::unexpected("player hero pawn unavailable");
  }

  // Respawn a dead hero.
  int32_t health = 0;
  std::memcpy(&health, static_cast<const char*>(pawn) + layout_.health, sizeof(health));
  if (health <= 0) {
    std::fprintf(stderr,
                 "[modlock] player respawn requested: slot=%d generation=%u hero_handle=%u\n", slot,
                 generation, handle);
    respawn(pawn, true);
  }
  return {};
}

std::expected<void, std::string> PawnObserver::EliminatePlayer(int32_t slot, uint32_t generation,
                                                               const NativeDamage& damage) {
  // Kill the hero of this connection and confirm it died.
  const auto sample = Observe(slot);
  if (!sample || sample->session_generation != generation) {
    return std::unexpected("elimination player observation is absent or stale");
  }
  auto killed = damage.Kill(PawnForSlot(slot));
  if (!killed) {
    return killed;
  }
  const auto after = Observe(slot);
  if (!after || after->session_generation != generation || after->health > 0) {
    return std::unexpected("native elimination did not produce an observed dead pawn");
  }
  return {};
}

std::optional<PawnObserver::Sample> PawnObserver::Observe(int32_t slot) {
  // Forget the previous frame's pawn.
  frame_pawn_ = nullptr;
  frame_entity_system_ = nullptr;
  frame_pawn_handle_ = 0;
  frame_slot_ = -1;

  // The stage trace covers the first ten occupied frames: frame one names the
  // walk stages, and the window distinguishes a pre-spawn zero origin from a
  // dead offset.
  static const bool stage_trace = InteropTraceEnabled();
  static int traced_frames = 0;
  const bool trace_now = stage_trace && traced_frames < 10;

  // Slot occupancy comes from the engine-reported client lifecycle, never
  // from entity bytes. An occupied-to-empty transition is the confirmed
  // disconnect that rearms placement; an empty slot while already empty
  // changes nothing.
  auto slot_state = seams_.slot_state ? seams_.slot_state(slot) : ConnectionTracker::SlotState{};
  if (!slot_state.occupied) {
    if (stage_trace && trace_slot_occupied_) {
      std::fprintf(stderr,
                   "[modlock] interop trace: pawn slot %d state occupied->empty generation %u\n",
                   slot, slot_state.generation);
      std::fflush(stderr);
      trace_slot_occupied_ = false;
    }
    return std::nullopt;
  }
  if (stage_trace && !trace_slot_occupied_) {
    std::fprintf(stderr,
                 "[modlock] interop trace: pawn slot %d state empty->occupied generation %u\n",
                 slot, slot_state.generation);
    std::fflush(stderr);
    trace_slot_occupied_ = true;
  }

  // Resolve the layout once.
  if (!offsets_ok_) {
    if (!seams_.layout) {
      NoteDegradation("no schema layout resolver");
      return std::nullopt;
    }
    auto layout = seams_.layout();
    if (!layout) {
      NoteDegradation(layout.error().c_str());
      return std::nullopt;
    }
    layout_ = layout.value();
    offsets_ok_ = true;
  }

  // Resolve the entity list.
  if (!seams_.entity_system) {
    NoteDegradation("no entity-system resolver");
    return std::nullopt;
  }
  auto entity_system = seams_.entity_system();
  if (!entity_system) {
    NoteDegradation(entity_system.error().c_str());
    return std::nullopt;
  }
  unsigned char* entity_list = EntityListOf(entity_system.value());

  // The controller for a Source player slot lives at entity index slot+1.
  auto* controller_identity = IdentityOfIndex(entity_list, slot + 1);
  if (controller_identity == nullptr) {
    if (trace_now) {
      ++traced_frames;
      std::fprintf(stderr,
                   "[modlock] observe trace: controller identity at index %u did not resolve\n",
                   slot + 1);
      std::fflush(stderr);
    }
    return std::nullopt;
  }
  auto* controller = InstanceOf(controller_identity);
  if (controller == nullptr) {
    return std::nullopt;
  }

  // Read the active pawn's handle.
  uint32_t pawn_handle = 0;

  // Death can switch the active pawn to a spectator while the hero remains
  // available for combat observation and round restoration.
  const auto controller_team = static_cast<const unsigned char*>(controller)[layout_.team];
  const size_t pawn_field = controller_team == 1
                                ? layout_.pawn_handle
                                : layout_.hero_pawn_handle.value_or(layout_.pawn_handle);
  std::memcpy(&pawn_handle, static_cast<const unsigned char*>(controller) + pawn_field,
              sizeof(pawn_handle));
  if (trace_now) {
    ++traced_frames;
    std::fprintf(
        stderr, "[modlock] observe trace: frame %d pawn handle = 0x%08x (index %u serial %u)\n",
        traced_frames, pawn_handle, pawn_handle & kEntityIndexBits, pawn_handle >> kSerialShift);
  }

  // Resolve the pawn and its origin, tracing each failure.
  auto* pawn_identity = IdentityOfHandle(entity_list, pawn_handle);
  if (pawn_identity == nullptr) {
    if (trace_now) {
      std::fprintf(stderr, "[modlock] observe trace: pawn identity failed the serial proof\n");
      std::fflush(stderr);
    }
    return std::nullopt;
  }
  void* pawn = InstanceOf(pawn_identity);
  if (pawn == nullptr) {
    if (trace_now) {
      std::fprintf(stderr, "[modlock] observe trace: pawn identity carries no instance\n");
      std::fflush(stderr);
    }
    return std::nullopt;
  }
  const float* origin = AbsOriginOf(layout_, pawn);
  if (origin == nullptr) {
    if (trace_now) {
      std::fprintf(stderr, "[modlock] observe trace: pawn origin chain is incomplete\n");
      std::fflush(stderr);
    }
    return std::nullopt;
  }
  if (trace_now) {
    std::fprintf(stderr, "[modlock] observe trace: full walk ok, origin %.1f %.1f %.1f xuid %llu\n",
                 origin[0], origin[1], origin[2], static_cast<unsigned long long>(slot_state.xuid));
    std::fflush(stderr);
  }
  if (!std::isfinite(origin[0]) || !std::isfinite(origin[1]) || !std::isfinite(origin[2])) {
    return std::nullopt;
  }

  // Movement is read directly from the pawn. In particular, velocity is not
  // reconstructed from origin deltas when this optional layout is absent.
  auto movement = movement_enabled_ ? ReadMovementState(layout_, pawn) : std::nullopt;

  // ConnectionTracker's generation changes for every accepted client,
  // including a same-XUID reconnect that occurs between observer frames.
  const uint64_t xuid = slot_state.xuid;
  uint32_t hero_id = 0;
  uint8_t team = 0;
  int32_t health = 0, max_health = 0, level = 0;

  // Read the hero, team, health and level.
  const auto* bytes = static_cast<const unsigned char*>(pawn);
  std::memcpy(&hero_id, bytes + layout_.hero_id, sizeof(hero_id));
  std::memcpy(&team, bytes + layout_.team, sizeof(team));
  std::memcpy(&health, bytes + layout_.health, sizeof(health));
  std::memcpy(&max_health, bytes + layout_.max_health, sizeof(max_health));
  std::memcpy(&level, bytes + layout_.level, sizeof(level));

  // Read the eye and camera angles, and the eye position when the view offset resolves.
  std::array<float, 3> eye_angles, camera_angles;
  std::memcpy(eye_angles.data(), bytes + layout_.eye_angles, sizeof(eye_angles));
  std::memcpy(camera_angles.data(), bytes + layout_.camera_angles, sizeof(camera_angles));
  std::optional<std::array<float, 3>> eye_position;
  if (layout_.view_offset) {
    std::array<float, 3> eye{};
    // CNetworkViewOffsetVector stores the three network floats eight bytes apart.
    for (size_t axis = 0; axis < eye.size(); ++axis) {
      std::memcpy(&eye[axis], bytes + *layout_.view_offset + 0x10 + axis * 8, sizeof(float));
      eye[axis] += origin[axis];
    }
    if (std::ranges::all_of(eye, [](float value) { return std::isfinite(value); })) {
      eye_position = eye;
    }
  }

  // Reject a pose whose angles are not finite.
  if (!std::all_of(camera_angles.begin(), camera_angles.end(),
                   [](float angle) { return std::isfinite(angle); }) ||
      !std::all_of(eye_angles.begin(), eye_angles.end(),
                   [](float angle) { return std::isfinite(angle); })) {
    return std::nullopt;
  }

  // Keep this frame's pawn for the calls that follow.
  frame_pawn_ = pawn;
  frame_entity_system_ = *entity_system;
  frame_pawn_handle_ = pawn_handle;
  frame_connection_ = slot_state;
  frame_slot_ = slot;

  // Read the currencies when their field resolves.
  std::optional<std::array<int32_t, 3>> currencies;
  if (layout_.currencies) {
    currencies.emplace();
    std::memcpy(currencies->data(), static_cast<const unsigned char*>(pawn) + *layout_.currencies,
                sizeof(*currencies));
  }

  // The totals borrow this frame's engine-owned controller instance; no
  // pointer survives Observe, and the read shares the pawn's lifetime.
  std::optional<ScoreTotals> score;
  if (layout_.score) {
    const ScoreLayout& offsets = *layout_.score;
    const auto* data = static_cast<const unsigned char*>(controller) + offsets.player_data_global;
    ScoreTotals& totals = score.emplace();
    for (const auto& counter : kScoreCounters) {
      std::memcpy(&(totals.*counter.total), data + offsets.*counter.offset, sizeof(int32_t));
    }
  }

  // Observed native stamina; absent when the schema capability did not
  // resolve or the values are not finite. Never a fabricated default.
  std::optional<Sample::Stamina> stamina;
  if (layout_.stamina) {
    const auto& st = *layout_.stamina;
    float st_cur = 0, st_max = 0;
    std::memcpy(&st_cur, bytes + st.current, sizeof(st_cur));
    std::memcpy(&st_max, bytes + st.max, sizeof(st_max));
    if (std::isfinite(st_cur) && std::isfinite(st_max) && st_max > 0) {
      stamina = Sample::Stamina{.current = st_cur, .max = st_max};
    }
  }
  if (movement) {
    const auto abilities = CurrentAbilitiesForSlot(slot);
    if (abilities) {
      const Ability* jump = nullptr;
      bool ambiguous = false;
      for (const auto& ability : *abilities) {
        if (ability.slot == 0xA) {
          movement->mantling = ability.channeling;
          movement->mantle_ability_handle = ability.handle;
          movement->mantle_start_time = ability.mantle_start_time;
        }
        if (ability.slot == 0xD) {
          movement->sliding = ability.sliding;
        }
        if (ability.slot != 0xC) {
          continue;
        }
        if (jump) {
          ambiguous = true;
        }
        jump = &ability;
      }
      if (jump && !ambiguous && jump->consecutive_wall_jumps) {
        movement->jump_ability_handle = jump->handle;
        movement->consecutive_wall_jumps = jump->consecutive_wall_jumps;
      }
      if (jump && !ambiguous) {
        movement->current_wall_normal = jump->current_wall_normal;
        movement->wall_contact_position = jump->wall_contact_position;
        movement->wall_jump_normal_used = jump->wall_jump_normal_used;
        movement->wall_jump_facing = jump->wall_jump_facing;
        movement->last_time_on_zipline = jump->last_time_on_zipline;
      }
    }
  }
  return Sample{.slot = slot,
                .x = origin[0],
                .y = origin[1],
                .z = origin[2],
                .steam_id = xuid,
                .session_generation = slot_state.generation,
                .pawn_handle = pawn_handle,
                .movement = std::move(movement),
                .hero_id = hero_id,
                .team = team,
                .health = health,
                .max_health = max_health,
                .level = level,
                .currencies = currencies,
                .eye_angles = eye_angles,
                .camera_angles = camera_angles,
                .eye_position = eye_position,
                .is_bot = slot_state.is_bot,
                .effective_max_health =
                    seams_.effective_max_health ? seams_.effective_max_health(pawn) : std::nullopt,
                .upgrade_bonuses = ReadPawnUpgradeBonuses(pawn),
                .score = score,
                .stamina = stamina};
}

void PawnObserver::Invalidate() {
  // Forget the frame's pawn and every resolved layout.
  frame_pawn_ = nullptr;
  frame_entity_system_ = nullptr;
  frame_pawn_handle_ = 0;
  frame_slot_ = -1;
  offsets_ok_ = false;
  layout_ = {};
  ability_layout_.reset();
  trace_slot_occupied_ = false;
}

void* PawnObserver::PawnForSlot(int32_t slot) const {
  if (frame_slot_ == slot) {
    return frame_pawn_;
  }
  return nullptr;
}

std::expected<ModifyCurrency, std::string> ResolveModifyCurrency(const ModuleImage& server) {
  auto address = ResolveSignature(server, "pawn.modify-currency");
  if (!address) {
    return std::unexpected(address.error());
  }
  return reinterpret_cast<ModifyCurrency>(*address);
}

std::expected<PawnObserver::Sample, std::string> PawnObserver::AdjustSouls(int32_t slot,
                                                                           int32_t delta,
                                                                           bool silent,
                                                                           ModifyCurrency modify) {
  // Read the wallet and check that it covers a spend.
  auto before = Observe(slot);
  if (!modify || !before || !before->currencies) {
    return std::unexpected("native wallet unavailable");
  }
  const auto balance = (*before->currencies)[0];
  if (delta == 0) {
    return *before;
  }
  if (delta < 0 && balance < -delta) {
    return std::unexpected("not enough souls");
  }

  // EGold=0, ECheats=7. A grant forces gain; a spend only removes souls.
  modify(PawnForSlot(slot), 0, delta, 7, silent ? 1 : 0, delta > 0 ? 1 : 0, delta < 0 ? 1 : 0,
         nullptr, nullptr);
  auto after = Observe(slot);
  if (!after || after->pawn_handle != before->pawn_handle || !after->currencies) {
    return std::unexpected("native wallet changed owner during adjustment");
  }

  // Native gain modifiers may scale a grant; a spend must remove exactly delta.
  const auto applied = (*after->currencies)[0] - balance;
  if (delta < 0 ? applied != delta : applied <= 0) {
    // A spend that removed a different amount is rolled back to the balance.
    if (delta < 0 && applied != 0) {
      modify(PawnForSlot(slot), 0, -applied, 7, 1, applied < 0 ? 1 : 0, applied > 0 ? 1 : 0,
             nullptr, nullptr);
    }
    return std::unexpected("native wallet did not apply the adjustment");
  }
  return *after;
}

std::expected<PawnObserver::Sample, std::string> PawnObserver::PrepareStartingSouls(
    int32_t slot, int32_t souls, ModifyCurrency modify) {
  // A wallet that already holds the starting souls is left alone.
  auto before = Observe(slot);
  if (souls < 0 || !modify || !before || !before->currencies) {
    return std::unexpected("native starting economy unavailable");
  }
  const auto balance = (*before->currencies)[0];
  if (balance >= souls) {
    return *before;
  }
  if (!CurrentAbilitiesForSlot(slot)) {
    return std::unexpected("starting pawn ownership changed");
  }

  // EGold=0, ECheats=7. Setup grants are silent, with forceGain=true,
  // spendOnly=false, no source ability/entity. Native net-worth progression runs.
  modify(PawnForSlot(slot), 0, souls - balance, 7, 1, 1, 0, nullptr, nullptr);
  auto after = Observe(slot);
  if (!after || after->pawn_handle != before->pawn_handle ||
      after->session_generation != before->session_generation ||
      after->steam_id != before->steam_id || !after->currencies ||
      (*after->currencies)[0] < souls) {
    return std::unexpected("native starting grant did not retain pawn and balance");
  }
  std::fprintf(stderr, "[modlock] starting souls slot=%d previous=%d granted=%d balance=%d\n", slot,
               balance, souls - balance, (*after->currencies)[0]);
  return *after;
}

std::expected<PawnObserver::Sample, std::string> PawnObserver::ResetStartingSouls(
    int32_t slot, int32_t souls, ModifyCurrency modify, ResetHeroPawn reset) {
  // Check the wallet, and match the player before and after each native call.
  auto before = Observe(slot);
  if (souls < 0 || !modify || !reset || !before || !before->currencies ||
      !CurrentAbilitiesForSlot(slot)) {
    return std::unexpected("native starting economy reset unavailable");
  }
  const auto same_player = [&](const std::optional<Sample>& sample) {
    return sample && sample->pawn_handle == before->pawn_handle &&
           sample->session_generation == before->session_generation &&
           sample->steam_id == before->steam_id && sample->hero_id == before->hero_id &&
           sample->team == before->team && sample->currencies;
  };

  // Reset the hero.
  reset(PawnForSlot(slot), true);
  auto sample = Observe(slot);
  if (!same_player(sample) || !CurrentAbilitiesForSlot(slot)) {
    return std::unexpected("native hero reset changed pawn ownership");
  }

  // Bring the balance to the starting souls.
  const int64_t delta = static_cast<int64_t>(souls) - (*sample->currencies)[0];
  if (delta < INT32_MIN || delta > INT32_MAX) {
    return std::unexpected("native starting currency adjustment exceeds integer range");
  }

  // Native hero reset and currency mutation own progression recalculation.
  // Dispatch even a zero adjustment so the native currency path still runs.
  modify(PawnForSlot(slot), 0, static_cast<int32_t>(delta), 7, 1, 1, 0, nullptr, nullptr);
  sample = Observe(slot);
  if (!same_player(sample) || !CurrentAbilitiesForSlot(slot) || (*sample->currencies)[0] != souls) {
    return std::unexpected("native starting reset did not retain pawn and balance");
  }
  return *sample;
}

std::expected<PawnObserver::Sample, std::string> PawnObserver::PrepareAbilityPoints(
    int32_t slot, ModifyCurrency modify) {
  auto sample = Observe(slot);
  if (!modify || !sample || !sample->currencies || !CurrentAbilitiesForSlot(slot)) {
    return std::unexpected("native ability currency unavailable");
  }
  // EAbilityPoints=1, EAbilityUnlocks=2. Each of four abilities costs 1+2+5
  // points to upgrade; native ModifyCurrency owns replication and progression.
  for (const auto [type, target] : {std::pair{1u, 32}, std::pair{2u, 4}}) {
    const int32_t balance = (*sample->currencies)[type];
    if (balance >= target) {
      continue;
    }
    const auto before = *sample;
    modify(PawnForSlot(slot), type, target - balance, 7, 1, 1, 0, nullptr, nullptr);
    sample = Observe(slot);
    if (!sample || sample->pawn_handle != before.pawn_handle ||
        sample->session_generation != before.session_generation ||
        sample->steam_id != before.steam_id || sample->hero_id != before.hero_id ||
        !sample->currencies || (*sample->currencies)[type] < target) {
      return std::unexpected("native ability grant did not retain pawn and balance");
    }
  }
  return *sample;
}

std::expected<SetMoveType, std::string> ResolvePreparationMovement(const ModuleImage& server) {
  // Movement setup tests FL_FROZEN before clearing buttons and movement axes.
  const auto input = ResolveSignature(server, "preparation.frozen-input");
  if (!input) {
    return std::unexpected(input.error());
  }

  // TakeDamageOld skips damage when m_bTakesDamage is false. The following
  // instructions distinguish this entry gate from its later damage comparison.
  const auto damage = ResolveSignature(server, "preparation.damage-gate");
  if (!damage) {
    return std::unexpected(damage.error());
  }

  // SetMoveType owns replication and the physics-mode transition.
  const auto movement = ResolveSignature(server, "entity.set-move-type");
  if (!movement) {
    return std::unexpected(movement.error());
  }
  return reinterpret_cast<SetMoveType>(*movement);
}

std::expected<void, std::string> PawnObserver::SetPreparationFrozen(int32_t slot, bool frozen) {
#if defined(_WIN32)
  // Drop the freeze of a pawn this player no longer owns.
  const auto sample = Observe(slot);
  if (!sample || !CurrentAbilitiesForSlot(slot)) {
    return std::unexpected("preparation pawn ownership unavailable");
  }
  auto owned = frozen_pawns_.find(slot);
  if (owned != frozen_pawns_.end() &&
      (owned->second.handle != sample->pawn_handle || owned->second.steam_id != sample->steam_id)) {
    frozen_pawns_.erase(owned);
    owned = frozen_pawns_.end();
    // A replacement does not inherit this observer's freeze. Releasing the old
    // ownership is complete without touching flags belonging to the new pawn.
  }

  // Releasing a pawn this observer never froze changes nothing.
  if (!frozen && owned == frozen_pawns_.end()) {
    return {};
  }

  // Resolve the native move type setter once.
  if (!preparation_movement_) {
    const auto server = MappedModuleImage::ForModule(L"server.dll");
    if (!server) {
      return std::unexpected(server.error());
    }
    preparation_movement_ = ResolvePreparationMovement(*server);
  }
  if (!*preparation_movement_) {
    return std::unexpected(preparation_movement_->error());
  }
  const auto set_move_type = **preparation_movement_;

  // Resolve the schema and the damage field at its pinned offset.
  auto schema = ResolveSchemaSystem();
  if (!schema) {
    return std::unexpected(schema.error());
  }
  auto damage = SchemaFieldOf(*schema, "server.dll", "CBaseEntity", "m_bTakesDamage");
  if (!damage || damage->offset != 0x2e0 || damage->size < sizeof(uint8_t)) {
    return std::unexpected("native preparation damage field unavailable");
  }

  // Resolve the flags and movement fields at their pinned offsets.
  auto field = SchemaFieldOf(*schema, "server.dll", "CBaseEntity", "m_fFlags");
  if (!field || field->offset != 0x390 || field->size < sizeof(uint32_t) || !field->networked) {
    return std::unexpected("native preparation flags unavailable");
  }
  auto movement = SchemaFieldOf(*schema, "server.dll", "CBaseEntity", "m_MoveType");
  auto collision = SchemaFieldOf(*schema, "server.dll", "CBaseEntity", "m_MoveCollide");
  if (!movement || movement->offset != 0x2f3 || movement->size < sizeof(uint8_t) || !collision ||
      collision->offset != 0x2f2 || collision->size < sizeof(uint8_t)) {
    return std::unexpected("native preparation movement fields unavailable");
  }

  // Read the pawn's flags and damage state, refusing a freeze another action owns.
  auto* pawn = static_cast<unsigned char*>(PawnForSlot(slot));
  uint32_t flags = 0;
  std::memcpy(&flags, pawn + field->offset, sizeof(flags));
  constexpr uint32_t kFrozen = 1u << 5;
  if (frozen && (flags & kFrozen) && owned == frozen_pawns_.end()) {
    return std::unexpected("pawn already frozen by another engine action");
  }
  const uint8_t takes_damage = pawn[damage->offset];
  if (takes_damage > 1) {
    return std::unexpected("invalid native preparation damage Boolean");
  }

  // Remember the original state of a pawn this freeze takes.
  if (frozen && owned == frozen_pawns_.end()) {
    owned = frozen_pawns_
                .emplace(slot, FrozenPawn{sample->pawn_handle, sample->steam_id, takes_damage,
                                          pawn[movement->offset], pawn[collision->offset]})
                .first;
  }

  // Compute the desired flags, damage and movement.
  const uint32_t desired_flags = frozen ? flags | kFrozen : flags & ~kFrozen;
  const uint8_t desired_damage = frozen ? 0 : owned->second.takes_damage;

  // MOVETYPE_NONE prevents preparation from running native walking physics.
  // Keep the original mode across respawns and reconnects of this retained pawn.
  const uint8_t desired_movement = frozen ? 0 : owned->second.move_type;
  if (frozen && flags == desired_flags && takes_damage == desired_damage &&
      pawn[movement->offset] == desired_movement) {
    return {};
  }

  // Write the state, release the ownership on unfreeze, and replicate it.
  std::memcpy(pawn + field->offset, &desired_flags, sizeof(desired_flags));
  pawn[damage->offset] = desired_damage;
  set_move_type(pawn, desired_movement, owned->second.move_collide);
  SetEntityVelocity(pawn, {});
  if (!frozen) {
    frozen_pawns_.erase(owned);
  }
  if (!NotifyEntityStateChanged(pawn)) {
    return std::unexpected("preparation replication unavailable");
  }
  return {};
#else
  return std::unexpected("native preparation control requires Windows");
#endif
}

std::optional<std::array<float, 3>> PawnObserver::CurrentOriginForSlot(int32_t slot) const {
  if (frame_slot_ != slot || frame_pawn_ == nullptr || !offsets_ok_) {
    return std::nullopt;
  }
  const float* origin = AbsOriginOf(layout_, frame_pawn_);
  if (origin == nullptr) {
    return std::nullopt;
  }
  return std::array<float, 3>{origin[0], origin[1], origin[2]};
}

std::expected<void, std::string> PawnObserver::SetEyeAngles(void* pawn,
                                                            const std::array<float, 3>& angles) {
  // Write finite angles to the pawn and replicate them.
  if (!pawn || !offsets_ok_) {
    return std::unexpected("pawn aim is unavailable in this frame");
  }
  if (!std::all_of(angles.begin(), angles.end(),
                   [](float angle) { return std::isfinite(angle); })) {
    return std::unexpected("pawn aim must be finite");
  }
  std::memcpy(static_cast<unsigned char*>(pawn) + layout_.eye_angles, angles.data(),
              sizeof(float) * angles.size());
  if (!NotifyEntityStateChanged(pawn)) {
    return std::unexpected("pawn aim replication unavailable");
  }
  return {};
}

std::expected<std::vector<PawnObserver::Ability>, std::string>
PawnObserver::CurrentAbilitiesForSlot(int32_t slot) {
  // The frame's connection must be unchanged.
  if (slot != frame_slot_ || !frame_pawn_ || !frame_entity_system_) {
    return std::unexpected("pawn is unavailable in this frame");
  }
  const auto connection =
      seams_.slot_state ? seams_.slot_state(slot) : ConnectionTracker::SlotState{};
  if (!connection.occupied || connection.xuid != frame_connection_.xuid ||
      connection.generation != frame_connection_.generation) {
    return std::unexpected("connection changed during this frame");
  }

  // The frame's pawn must still be the controller's.
  auto* entity_list = EntityListOf(frame_entity_system_);
  auto* pawn_identity = IdentityOfHandle(entity_list, frame_pawn_handle_);
  auto* controller_identity = IdentityOfIndex(entity_list, slot + 1);
  if (!pawn_identity || InstanceOf(pawn_identity) != frame_pawn_ || !controller_identity) {
    return std::unexpected("pawn identity changed during this frame");
  }
  uint32_t current_pawn_handle = 0;
  std::memcpy(
      &current_pawn_handle,
      static_cast<const unsigned char*>(InstanceOf(controller_identity)) + layout_.pawn_handle,
      sizeof(current_pawn_handle));
  if (current_pawn_handle != frame_pawn_handle_) {
    return std::unexpected("controller pawn changed during this frame");
  }

  // Resolve the ability layout once.
  if (!ability_layout_) {
    if (!seams_.ability_layout) {
      return std::unexpected("ability schema resolver is unavailable");
    }
    auto layout = seams_.ability_layout();
    if (!layout) {
      return std::unexpected(layout.error());
    }
    ability_layout_ = *layout;
  }
  const auto& layout = *ability_layout_;

  // Pinned x64 CUtlVectorBase: int count, alignment padding, then
  // CUtlVectorMemory's data pointer, allocation count and growth flags.
  struct HandleVector {
    int32_t count;
    uint32_t padding;
    const uint32_t* data;
    uint32_t capacity;
    uint32_t flags;
  } handles{};
  static_assert(sizeof(HandleVector) == 24);
  std::memcpy(&handles, static_cast<const unsigned char*>(frame_pawn_) + layout.handles,
              sizeof(handles));
  if (handles.count < 0 || handles.count > 256 ||
      static_cast<uint32_t>(handles.count) > handles.capacity || (handles.count && !handles.data)) {
    return std::unexpected("invalid ability handle vector");
  }

  // Read each owned ability.
  std::vector<Ability> abilities;
  abilities.reserve(handles.count);
  for (int32_t i = 0; i < handles.count; ++i) {
    const uint32_t handle = handles.data[i];
    if (handle == kInvalidEHandleIndex) {
      continue;
    }
    auto* identity = IdentityOfHandle(EntityListOf(frame_entity_system_), handle);
    if (!identity) {
      return std::unexpected("owned ability handle is unresolved");
    }
    const auto* entity = static_cast<const unsigned char*>(InstanceOf(identity));
    if (!entity) {
      return std::unexpected("owned ability has no live instance");
    }
    uint32_t owner = 0;
    Ability ability{.handle = handle};
    std::memcpy(&owner, entity + layout.owner, sizeof(owner));
    std::memcpy(&ability.subclass_id, entity + layout.subclass, sizeof(ability.subclass_id));
    std::memcpy(&ability.slot, entity + layout.slot, sizeof(ability.slot));
    std::memcpy(&ability.upgrade_info, entity + layout.upgrade_info, sizeof(ability.upgrade_info));
    std::memcpy(&ability.charges, entity + layout.charges, sizeof(ability.charges));
    std::memcpy(&ability.cooldown_end, entity + layout.cooldown_end, sizeof(ability.cooldown_end));

    std::memcpy(&ability.cooldown_start, entity + layout.cooldown_start,
                sizeof(ability.cooldown_start));
    std::memcpy(&ability.charge_recharge_start, entity + layout.charge_recharge_start,
                sizeof(ability.charge_recharge_start));
    std::memcpy(&ability.charge_recharge_end, entity + layout.charge_recharge_end,
                sizeof(ability.charge_recharge_end));
    if (!std::isfinite(ability.cooldown_end) || !std::isfinite(ability.cooldown_start) ||
        !std::isfinite(ability.charge_recharge_start) ||
        !std::isfinite(ability.charge_recharge_end)) {
      return std::unexpected("ability timer is not finite");
    }
    if (owner != frame_pawn_handle_) {
      return std::unexpected("ability owner does not match the pawn");
    }
    if (!ability.subclass_id) {
      return std::unexpected("owned ability has no subclass identity");
    }
    if (layout.channeling) {
      ability.channeling = entity[*layout.channeling] == 1;
    }
    if (ability.slot == 0xA && layout.mantle_start_time) {
      float start = 0;
      std::memcpy(&start, entity + *layout.mantle_start_time, sizeof(start));
      if (std::isfinite(start)) {
        ability.mantle_start_time = start;
      }
    }
    if (ability.slot == 0xD && layout.sliding) {
      ability.sliding = entity[*layout.sliding] == 1;
    }
    if (ability.slot == 0xC && layout.consecutive_wall_jumps) {
      int8_t count = 0;
      std::memcpy(&count, entity + *layout.consecutive_wall_jumps, sizeof(count));
      if (count >= 0) {
        ability.consecutive_wall_jumps = count;
      }
    }
    const auto vector_at =
        [entity](std::optional<size_t> offset) -> std::optional<std::array<float, 3>> {
      // Read a finite vector at an optional offset.
      if (!offset) {
        return std::nullopt;
      }
      std::array<float, 3> value{};
      std::memcpy(value.data(), entity + *offset, sizeof(value));
      if (!std::ranges::all_of(value, [](float v) { return std::isfinite(v); })) {
        return std::nullopt;
      }
      return value;
    };
    if (ability.slot == 0xC) {
      ability.current_wall_normal = vector_at(layout.current_wall_normal);
      ability.wall_contact_position = vector_at(layout.wall_contact_position);
      ability.wall_jump_normal_used = vector_at(layout.wall_jump_normal_used);
      if (layout.wall_jump_facing) {
        uint16_t facing = 0;
        std::memcpy(&facing, entity + *layout.wall_jump_facing, sizeof(facing));
        ability.wall_jump_facing = facing;
      }
    }
    if (ability.slot == 0xC && layout.last_time_on_zipline) {
      float time = 0;
      std::memcpy(&time, entity + *layout.last_time_on_zipline, sizeof(time));
      if (std::isfinite(time)) {
        ability.last_time_on_zipline = time;
      }
    }
    if (std::any_of(abilities.begin(), abilities.end(),
                    [handle](const auto& a) { return a.handle == handle; })) {
      return std::unexpected("duplicate owned ability handle");
    }
    abilities.push_back(ability);
  }
  return abilities;
}

std::expected<SetUpgradeBits, std::string> ResolveSetUpgradeBits(const ModuleImage& server) {
  auto address = ResolveSignature(server, "ability.set-upgrade-bits");
  if (!address) {
    return std::unexpected(address.error());
  }
  return reinterpret_cast<SetUpgradeBits>(*address);
}

std::expected<CreateAbility, std::string> ResolveCreateAbility(const ModuleImage& server) {
  auto address = ResolveSignature(server, "ability.create-and-register");
  if (!address) {
    return std::unexpected(address.error());
  }
  return reinterpret_cast<CreateAbility>(*address);
}

std::expected<void, std::string> PawnObserver::ReconcileAbilities(
    int32_t slot, std::span<const AbilityUpgrade> targets, const AbilityDefinitions& definitions,
    CreateAbility create, SetUpgradeBits set_bits) {
  // Check that every target's slot and definition is unique.
  std::vector<uint16_t> slots;
  std::vector<uint32_t> subclasses;
  for (const auto& target : targets) {
    // Slot 23 is the native unbound slot shared by triggered abilities.
    if (target.slot > 23 || (target.slot >= 4 && target.slot <= 7) ||
        (target.slot != 23 && std::ranges::find(slots, target.slot) != slots.end()) ||
        std::ranges::find(subclasses, target.subclass_id) != subclasses.end()) {
      return std::unexpected("invalid or duplicate replay ability slot");
    }
    slots.push_back(target.slot);
    subclasses.push_back(target.subclass_id);
  }

  // Create each target the pawn lacks, then apply the upgrades.
  for (const auto& target : targets) {
    auto owned = CurrentAbilitiesForSlot(slot);
    if (!owned) {
      return std::unexpected(owned.error());
    }
    const auto existing = std::ranges::find(*owned, target.subclass_id, &Ability::subclass_id);
    if (existing != owned->end()) {
      if (existing->slot != target.slot) {
        return std::unexpected("replay ability occupies another slot");
      }
      continue;
    }
    if (target.slot != 23 &&
        std::ranges::find(*owned, target.slot, &Ability::slot) != owned->end()) {
      return std::unexpected("replay ability slot contains another definition");
    }
    if (!create) {
      return std::unexpected("native ability creation unavailable");
    }
    const auto definition = definitions.Find(target.subclass_id);
    if (!definition) {
      return std::unexpected(definition.error());
    }
    if (definition->disabled || definition->name.starts_with("upgrade_")) {
      return std::unexpected("replay ability is not an enabled non-item definition");
    }
    create(static_cast<unsigned char*>(frame_pawn_) + ability_layout_->component,
           definition->native_definition_pointer, target.slot, 0, true, nullptr);
    owned = CurrentAbilitiesForSlot(slot);
    if (!owned) {
      return std::unexpected(owned.error());
    }
    if (std::ranges::count_if(*owned, [&](const Ability& ability) {
          return ability.slot == target.slot && ability.subclass_id == target.subclass_id;
        }) != 1) {
      return std::unexpected(
          "native ability creation did not publish the requested owner and slot");
    }
  }
  return ApplyAbilityUpgrades(slot, targets, set_bits);
}

namespace {
// UpgradeLowWordSetter resolves the engine setter for m_nUpgradeInfo's low
// word once; builds before game build 6711 have none.
using SetUpgradeLowWord = void (*)(void* ability, uint32_t value);
SetUpgradeLowWord UpgradeLowWordSetter() {
#if defined(_WIN32)
  static const SetUpgradeLowWord setter = []() -> SetUpgradeLowWord {
    const auto server = MappedModuleImage::ForModule(L"server.dll");
    if (!server) {
      return nullptr;
    }
    const auto address = ResolveSignature(*server, "ability.set-upgrade-low-word");
    return address ? reinterpret_cast<SetUpgradeLowWord>(*address) : nullptr;
  }();
  return setter;
#else
  return nullptr;
#endif
}
}  // namespace

std::expected<void, std::string> PawnObserver::ApplyAbilityUpgrades(
    int32_t slot, std::span<const AbilityUpgrade> upgrades, SetUpgradeBits set_bits) {
  // Each upgrade must match one owned ability, and a changed low word needs its setter.
  if (!set_bits) {
    return std::unexpected("engine upgrade setter is unavailable");
  }
  const auto set_low_word = UpgradeLowWordSetter();
  auto owned = CurrentAbilitiesForSlot(slot);
  if (!owned) {
    return std::unexpected(owned.error());
  }
  std::vector<uint32_t> handles;
  for (const auto& target : upgrades) {
    const auto matches = [&](const Ability& ability) {
      return ability.subclass_id == target.subclass_id && ability.slot == target.slot;
    };
    if (std::count_if(owned->begin(), owned->end(), matches) != 1) {
      return std::unexpected("upgrade target does not uniquely match an owned ability");
    }
    const auto& ability = *std::find_if(owned->begin(), owned->end(), matches);
    if ((ability.upgrade_info & 0xffff) != (target.upgrade_info & 0xffff) && !set_low_word) {
      return std::unexpected("upgrade packed low word differs; setter cannot restore it");
    }
    if (std::find(handles.begin(), handles.end(), ability.handle) != handles.end()) {
      return std::unexpected("duplicate ability upgrade target");
    }
    handles.push_back(ability.handle);
  }

  // Apply each upgrade.
  for (size_t i = 0; i < upgrades.size(); ++i) {
    // A setter can update other abilities. Revalidate ownership after every
    // engine call, without retaining any borrowed entity pointer across it.
    owned = CurrentAbilitiesForSlot(slot);
    if (!owned) {
      return std::unexpected(owned.error());
    }
    const auto& target = upgrades[i];
    const auto found = std::find_if(owned->begin(), owned->end(), [&](const Ability& ability) {
      return ability.handle == handles[i] && ability.subclass_id == target.subclass_id &&
             ability.slot == target.slot;
    });
    if (found == owned->end()) {
      return std::unexpected("upgrade target changed during apply");
    }
    if (found->upgrade_info == target.upgrade_info) {
      continue;
    }
    auto* identity = IdentityOfHandle(EntityListOf(frame_entity_system_), found->handle);
    if (!identity) {
      return std::unexpected("upgrade target expired during apply");
    }
    if ((found->upgrade_info & 0xffff) != (target.upgrade_info & 0xffff)) {
      if (!set_low_word) {
        return std::unexpected("upgrade target changed during apply");
      }
      set_low_word(InstanceOf(identity), target.upgrade_info & 0xffff);
    }
    set_bits(InstanceOf(identity), target.upgrade_info >> 16);
  }

  // Include the final setter in the ownership check before returning success.
  owned = CurrentAbilitiesForSlot(slot);
  if (!owned) {
    return std::unexpected(owned.error());
  }
  return {};
}

void* PawnObserver::AbilityInstance(int32_t slot, uint32_t handle) {
  const auto owned = CurrentAbilitiesForSlot(slot);
  if (!owned || std::none_of(owned->begin(), owned->end(),
                             [&](const auto& ability) { return ability.handle == handle; })) {
    return nullptr;
  }
  return InstanceOf(IdentityOfHandle(EntityListOf(frame_entity_system_), handle));
}

std::expected<void, std::string> PawnObserver::ApplyAbilityTimers(
    int32_t slot, std::span<const Ability> targets) {
  // Match targets by identity and by timers.
  auto owned = CurrentAbilitiesForSlot(slot);
  if (!owned) {
    return std::unexpected(owned.error());
  }
  const auto matches = [](const Ability& actual, const Ability& target) {
    return actual.handle == target.handle && actual.subclass_id == target.subclass_id &&
           actual.slot == target.slot;
  };
  const auto timers_match = [](const Ability& actual, const Ability& target) {
    return actual.charges == target.charges && actual.cooldown_start == target.cooldown_start &&
           actual.cooldown_end == target.cooldown_end &&
           actual.charge_recharge_start == target.charge_recharge_start &&
           actual.charge_recharge_end == target.charge_recharge_end;
  };

  // Check every target before writing any.
  std::vector<uint32_t> handles;
  for (const auto& target : targets) {
    if (target.charges < 0 || !std::isfinite(target.cooldown_start) ||
        !std::isfinite(target.cooldown_end) || !std::isfinite(target.charge_recharge_start) ||
        !std::isfinite(target.charge_recharge_end)) {
      return std::unexpected("invalid ability timer target");
    }
    if (std::find(handles.begin(), handles.end(), target.handle) != handles.end()) {
      return std::unexpected("duplicate ability timer target");
    }
    handles.push_back(target.handle);
    if (std::none_of(owned->begin(), owned->end(),
                     [&](const auto& a) { return matches(a, target); })) {
      return std::unexpected("timer target does not match an owned ability");
    }
    auto* identity = IdentityOfHandle(EntityListOf(frame_entity_system_), target.handle);
    if (!CanNotifyEntityStateChanged(InstanceOf(identity))) {
      return std::unexpected("ability replication notification is unavailable");
    }
  }

  // Write each target's timers and read them back.
  for (const auto& target : targets) {
    owned = CurrentAbilitiesForSlot(slot);
    if (!owned) {
      return std::unexpected(owned.error());
    }
    const auto found = std::find_if(owned->begin(), owned->end(),
                                    [&](const auto& a) { return matches(a, target); });
    if (found == owned->end()) {
      return std::unexpected("ability identity changed during timer restoration");
    }
    if (timers_match(*found, target)) {
      continue;
    }
    auto* identity = IdentityOfHandle(EntityListOf(frame_entity_system_), target.handle);
    auto* entity = static_cast<unsigned char*>(InstanceOf(identity));
    const auto& layout = *ability_layout_;
    std::memcpy(entity + layout.charges, &target.charges, sizeof(target.charges));
    std::memcpy(entity + layout.cooldown_start, &target.cooldown_start,
                sizeof(target.cooldown_start));
    std::memcpy(entity + layout.cooldown_end, &target.cooldown_end, sizeof(target.cooldown_end));
    std::memcpy(entity + layout.charge_recharge_start, &target.charge_recharge_start,
                sizeof(target.charge_recharge_start));
    std::memcpy(entity + layout.charge_recharge_end, &target.charge_recharge_end,
                sizeof(target.charge_recharge_end));
    if (!NotifyEntityStateChanged(entity)) {
      return std::unexpected("ability replication notification is unavailable");
    }
    owned = CurrentAbilitiesForSlot(slot);
    if (!owned) {
      return std::unexpected(owned.error());
    }
    const auto verified = std::find_if(owned->begin(), owned->end(),
                                       [&](const auto& a) { return matches(a, target); });
    if (verified == owned->end() || !timers_match(*verified, target)) {
      return std::unexpected("ability timer readback differs");
    }
  }

  // Confirm the batch held after the last write.
  for (const auto& target : targets) {
    if (std::none_of(owned->begin(), owned->end(), [&](const auto& actual) {
          return matches(actual, target) && timers_match(actual, target);
        })) {
      return std::unexpected("ability timers changed during batch restoration");
    }
  }
  return {};
}

std::expected<std::vector<PawnObserver::Ability>, std::string> PawnObserver::FreshAbilityTargets(
    int32_t slot) {
  // Read the owned abilities and the charge reader.
  auto targets = CurrentAbilitiesForSlot(slot);
  if (!targets) {
    return std::unexpected(targets.error());
  }
  if (!seams_.max_charges) {
    return std::unexpected("native ability charge reader unavailable");
  }

  // Each fresh ability has full charges and no cooldown.
  for (auto& target : *targets) {
    auto current = CurrentAbilitiesForSlot(slot);
    if (!current) {
      return std::unexpected(current.error());
    }
    const auto matches = [&](const Ability& value) {
      return value.handle == target.handle && value.subclass_id == target.subclass_id &&
             value.slot == target.slot;
    };
    if (std::none_of(current->begin(), current->end(), matches)) {
      return std::unexpected("ability changed before reading its charge maximum");
    }
    auto* identity = IdentityOfHandle(EntityListOf(frame_entity_system_), target.handle);
    auto maximum = seams_.max_charges(InstanceOf(identity));
    if (!maximum) {
      return std::unexpected(maximum.error());
    }
    current = CurrentAbilitiesForSlot(slot);
    if (!current || std::none_of(current->begin(), current->end(), matches)) {
      return std::unexpected("ability ownership changed while reading its charge maximum");
    }
    if (*maximum < 0) {
      return std::unexpected("native ability charge maximum is negative");
    }
    target.charges = *maximum;
    target.cooldown_start = target.cooldown_end = 0;
    target.charge_recharge_start = target.charge_recharge_end = 0;
  }
  return targets;
}

std::expected<void, std::string> PawnObserver::SetPracticeStamina(int32_t slot, float now,
                                                                  std::optional<float> stamina) {
  // The engine clock owns "now"; a non-finite time or value is a caller defect.
  if (!std::isfinite(now)) {
    return std::unexpected("stamina latch time is not finite");
  }
  if (stamina && !std::isfinite(*stamina)) {
    return std::unexpected("stamina is not finite");
  }

  // CurrentAbilitiesForSlot revalidates the frame-scoped pawn, connection and
  // controller identity before any write.
  auto owned = CurrentAbilitiesForSlot(slot);
  if (!owned) {
    return std::unexpected(owned.error());
  }
  if (!layout_.stamina) {
    return std::unexpected("stamina schema capability is unavailable");
  }

  // Read the pawn's stamina resource.
  const auto& st = *layout_.stamina;
  auto* pawn = static_cast<unsigned char*>(PawnForSlot(slot));
  if (!pawn) {
    return std::unexpected("pawn is unavailable in this frame");
  }
  float current = 0, max = 0;
  std::memcpy(&current, pawn + st.current, sizeof(current));
  std::memcpy(&max, pawn + st.max, sizeof(max));

  // The native maximum bounds the target; it must be a real capacity.
  if (!std::isfinite(current) || !std::isfinite(max) || max <= 0) {
    return std::unexpected("native stamina resource is not usable");
  }

  // Mirror the installed HeroRefresh field relationships: current = value,
  // latchValue = value, latchTime = the caller's simulation time. Native
  // m_flPrevRegenRate is retained untouched.
  const float value = std::clamp(stamina.value_or(max), 0.0f, max);
  std::memcpy(pawn + st.current, &value, sizeof(value));
  std::memcpy(pawn + st.latch_value, &value, sizeof(value));
  std::memcpy(pawn + st.latch_time, &now, sizeof(now));
  if (!NotifyEntityStateChanged(pawn)) {
    return std::unexpected("stamina replication notification is unavailable");
  }

  // The notification callback can mutate or destroy entities. Revalidate
  // ownership through the existing ability readback, then verify the write.
  owned = CurrentAbilitiesForSlot(slot);
  if (!owned) {
    return std::unexpected(owned.error());
  }
  float written_current = 0, written_latch_value = 0, written_latch_time = 0;
  std::memcpy(&written_current, pawn + st.current, sizeof(written_current));
  std::memcpy(&written_latch_value, pawn + st.latch_value, sizeof(written_latch_value));
  std::memcpy(&written_latch_time, pawn + st.latch_time, sizeof(written_latch_time));
  if (written_current != value || written_latch_value != value || written_latch_time != now) {
    return std::unexpected("stamina readback differs after replication notification");
  }
  return {};
}

std::expected<TeleportClientCamera, std::string> ResolveTeleportClientCamera(
    const ModuleImage& server) {
  // The first receiver is unused. The helper teleports its second argument,
  // then sends user message 321 with all three camera axes to that pawn's client.
  auto address = ResolveSignature(server, "pawn.teleport-client-camera");
  if (!address) {
    return std::unexpected(address.error());
  }
  return reinterpret_cast<TeleportClientCamera>(*address);
}

std::expected<PawnMotion, std::string> ResolvePawnMotion(const ModuleImage& server) {
  PawnMotion motion{};
  const std::pair<const char*, void (**)(void*, const float*)> setters[] = {
      {"entity.set-abs-origin", &motion.set_origin},
      {"entity.set-abs-angles", &motion.set_angles},
      {"entity.set-abs-velocity", &motion.set_velocity},
  };
  for (const auto& [id, setter] : setters) {
    auto address = ResolveSignature(server, id);
    if (!address) {
      return std::unexpected(address.error());
    }
    *setter = reinterpret_cast<void (*)(void*, const float*)>(*address);
  }
  return motion;
}

std::expected<ItemFunctions, std::string> ItemFunctions::Resolve(const ModuleImage& server) {
  // Resolve the item add and remove functions.
  auto add = ResolveSignature(server, "pawn.add-item");
  if (!add) {
    return std::unexpected(add.error());
  }
  auto remove = ResolveSignature(server, "ability.remove-item");
  if (!remove) {
    return std::unexpected(remove.error());
  }

  // Resolve the upgrade bits setter and the slot swap.
  auto bits = ResolveSetUpgradeBits(server);
  if (!bits) {
    return std::unexpected(bits.error());
  }
  auto swap = ResolveSignature(server, "ability.swap-item-slots");
  if (!swap) {
    return std::unexpected(swap.error());
  }
  return ItemFunctions{reinterpret_cast<Add>(*add), reinterpret_cast<Remove>(*remove), *bits,
                       reinterpret_cast<SwapSlots>(*swap)};
}

std::expected<PawnObserver::Ability, std::string> PawnObserver::GrantItem(
    int32_t slot, uint32_t subclass_id, const AbilityDefinitions& definitions,
    const ItemFunctions& functions) {
  // Grant only an enabled item the pawn does not own.
  const auto definition = definitions.Find(subclass_id);
  if (!definition) {
    return std::unexpected(definition.error());
  }
  if (definition->disabled || !definition->name.starts_with("upgrade_") || !functions.add) {
    return std::unexpected("enabled native item and grant function required");
  }
  const auto before = CurrentAbilitiesForSlot(slot);
  if (!before) {
    return std::unexpected(before.error());
  }
  const auto existing = std::ranges::find(*before, subclass_id, &Ability::subclass_id);
  if (existing != before->end()) {
    return *existing;
  }

  // The engine owns the returned pointer. Only the subsequent owned-ability
  // observation establishes the grant and its actual inventory slot.
  functions.add(frame_pawn_, definition->name.c_str(), 0, nullptr);
  const auto after = CurrentAbilitiesForSlot(slot);
  if (!after) {
    return std::unexpected(after.error());
  }
  for (const auto& previous : *before) {
    const auto retained = std::ranges::find(*after, previous.handle, &Ability::handle);
    if (retained == after->end() || retained->subclass_id != previous.subclass_id ||
        retained->slot != previous.slot) {
      return std::unexpected("native grant changed an existing ability or item slot");
    }
  }
  const auto granted = std::ranges::find(*after, subclass_id, &Ability::subclass_id);
  if (granted == after->end() || after->size() != before->size() + 1) {
    return std::unexpected("native grant did not produce one owned item");
  }
  return *granted;
}

std::expected<void, std::string> PawnObserver::ReconcileItems(int32_t slot,
                                                              std::span<const ItemTarget> targets,
                                                              const AbilityDefinitions& definitions,
                                                              const ItemFunctions& functions) {
  // Check the functions and every target's definition and slot.
  if (!functions.add || !functions.remove || !functions.set_bits) {
    return std::unexpected("engine item functions are unavailable");
  }
  if (targets.size() > 64) {
    return std::unexpected("too many replay items");
  }
  std::vector<std::string> names;
  std::vector<uint16_t> active_slots;
  for (const auto& target : targets) {
    auto definition = definitions.Find(target.subclass_id);
    if (!definition) {
      return std::unexpected(definition.error());
    }
    if (definition->disabled || !definition->name.starts_with("upgrade_")) {
      return std::unexpected("replay item is not an enabled upgrade definition");
    }
    if (std::find(names.begin(), names.end(), definition->name) != names.end()) {
      return std::unexpected("duplicate replay item definition");
    }
    if (target.slot && *target.slot != 23) {
      if (*target.slot < 4 || *target.slot > 7) {
        return std::unexpected("invalid replay item slot");
      }
      if (!functions.swap_slots) {
        return std::unexpected("engine item slot swap is unavailable");
      }
      if (std::find(active_slots.begin(), active_slots.end(), *target.slot) != active_slots.end()) {
        return std::unexpected("duplicate replay active item slot");
      }
      active_slots.push_back(*target.slot);
    }
    names.push_back(std::move(definition->name));
  }

  // Read the pawn's items.
  const auto read_items = [&]() -> std::expected<std::vector<Ability>, std::string> {
    // Keep the owned abilities that are items.
    auto owned = CurrentAbilitiesForSlot(slot);
    if (!owned) {
      return std::unexpected(owned.error());
    }
    std::vector<Ability> items;
    for (const auto& ability : *owned) {
      auto definition = definitions.Find(ability.subclass_id);
      if (!definition) {
        return std::unexpected(definition.error());
      }
      if (definition->name.starts_with("upgrade_")) {
        items.push_back(ability);
      }
    }
    return items;
  };
  auto items = read_items();
  if (!items) {
    return std::unexpected(items.error());
  }

  // Remove each item no target keeps, and each whose low word differs.
  std::vector<uint32_t> kept;
  std::vector<Ability> removals;
  for (const auto& item : *items) {
    const auto target = std::find_if(targets.begin(), targets.end(), [&](const auto& t) {
      return t.subclass_id == item.subclass_id;
    });
    if (target == targets.end() ||
        std::find(kept.begin(), kept.end(), item.subclass_id) != kept.end()) {
      removals.push_back(item);
    } else if ((item.upgrade_info & 0xffff) != (target->upgrade_info & 0xffff)) {
      // Only a fresh grant sets the packed low word; replace the item.
      removals.push_back(item);
    } else {
      kept.push_back(item.subclass_id);
    }
  }
  for (const auto& removal : removals) {
    items = read_items();
    if (!items) {
      return std::unexpected(items.error());
    }
    const auto found = std::find_if(items->begin(), items->end(), [&](const auto& item) {
      return item.handle == removal.handle && item.subclass_id == removal.subclass_id;
    });
    if (found == items->end()) {
      return std::unexpected("item changed before removal");
    }
    auto* identity = IdentityOfHandle(EntityListOf(frame_entity_system_), found->handle);
    if (!identity) {
      return std::unexpected("item expired before removal");
    }
    functions.remove(static_cast<unsigned char*>(frame_pawn_) + ability_layout_->component,
                     InstanceOf(identity), 0);
    items = read_items();
    if (!items) {
      return std::unexpected(items.error());
    }
    if (std::any_of(items->begin(), items->end(),
                    [&](const auto& item) { return item.handle == removal.handle; })) {
      return std::unexpected("engine item removal did not change the loadout");
    }
  }

  // Grant each missing target and apply its upgrades.
  for (size_t i = 0; i < targets.size(); ++i) {
    items = read_items();
    if (!items) {
      return std::unexpected(items.error());
    }
    const auto matches = [&](const auto& item) {
      return item.subclass_id == targets[i].subclass_id;
    };
    if (std::none_of(items->begin(), items->end(), matches)) {
      // The returned pointer is borrowed engine storage. Ownership readback,
      // rather than that pointer, determines whether the grant took effect.
      // The upgrade value's low word holds the initial upgrade bits and its
      // high dword the packed low word of m_nUpgradeInfo.
      const uint64_t upgrade =
          (uint64_t{targets[i].upgrade_info & 0xffffu} << 32) | (targets[i].upgrade_info >> 16);
      functions.add(frame_pawn_, names[i].c_str(), upgrade, nullptr);
      items = read_items();
      if (!items) {
        return std::unexpected(items.error());
      }
    }
    if (std::count_if(items->begin(), items->end(), matches) != 1) {
      return std::unexpected("engine item grant did not produce one owned item");
    }
    const auto& item = *std::find_if(items->begin(), items->end(), matches);
    const AbilityUpgrade upgrade{item.subclass_id, item.slot, targets[i].upgrade_info};
    auto applied = ApplyAbilityUpgrades(slot, std::span(&upgrade, 1), functions.set_bits);
    if (!applied) {
      return applied;
    }
  }

  // Move each item to its target slot.
  for (const auto& target : targets) {
    if (!target.slot) {
      continue;
    }
    items = read_items();
    if (!items) {
      return std::unexpected(items.error());
    }
    const auto found = std::find_if(items->begin(), items->end(), [&](const auto& item) {
      return item.subclass_id == target.subclass_id;
    });
    if (found == items->end()) {
      return std::unexpected("item disappeared before slot restoration");
    }
    if (found->slot == *target.slot) {
      continue;
    }
    if (found->slot < 4 || found->slot > 7 || *target.slot == 23) {
      return std::unexpected("item active/passive slot class differs from replay");
    }
    const auto handle = found->handle;
    functions.swap_slots(static_cast<unsigned char*>(frame_pawn_) + ability_layout_->component,
                         found->slot, *target.slot);
    items = read_items();
    if (!items) {
      return std::unexpected(items.error());
    }
    if (std::none_of(items->begin(), items->end(), [&](const auto& item) {
          return item.handle == handle && item.subclass_id == target.subclass_id &&
                 item.slot == *target.slot;
        })) {
      return std::unexpected("engine item slot swap did not persist");
    }
  }

  // Confirm the final loadout.
  items = read_items();
  if (!items) {
    return std::unexpected(items.error());
  }
  if (items->size() != targets.size()) {
    return std::unexpected("restored item count differs");
  }
  for (const auto& target : targets) {
    if (std::count_if(items->begin(), items->end(), [&](const auto& item) {
          return item.subclass_id == target.subclass_id &&
                 item.upgrade_info == target.upgrade_info &&
                 (!target.slot || item.slot == *target.slot);
        }) != 1) {
      return std::unexpected("restored item identity or upgrade readback differs");
    }
  }
  return {};
}

namespace {
bool RestoreIntegerField(void* pawn, const char* class_name, const char* field_name,
                         int32_t value) {
  // Write a networked integer field and replicate it.
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
  std::memcpy(static_cast<unsigned char*>(pawn) + field->offset, &value, sizeof(value));
  return NotifyEntityStateChanged(pawn);
}
}  // namespace

bool RestorePawnHealth(void* pawn, int32_t health, int32_t network_max_health) {
  return health > 0 && network_max_health >= 0 &&
         RestoreIntegerField(pawn, "CBaseEntity", "m_iMaxHealth", network_max_health) &&
         RestoreIntegerField(pawn, "CBaseEntity", "m_iHealth", health);
}

bool RestorePawnLevel(void* pawn, int32_t level) {
  return level >= 0 && level <= 100 &&
         RestoreIntegerField(pawn, "CCitadelPlayerPawn", "m_nLevel", level);
}

namespace {
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

void PrepareUpgradeBonuses() {
#if defined(_WIN32)
  static_cast<void>(UpgradeBonusLayoutOnce());
#endif
}

// UpgradeBonuses is the pawn's upgrade bonus modifier and the layout that
// reads it.
struct UpgradeBonuses {
  unsigned char* modifier;
  const UpgradeBonusLayout* layout;
};

std::expected<UpgradeBonuses, std::string> UpgradeBonusModifier(void* pawn) {
#if defined(_WIN32)
  // Find the pawn's modifier property.
  if (!pawn) {
    return std::unexpected("upgrade bonuses: pawn unavailable");
  }
  const auto& layout = UpgradeBonusLayoutOnce();
  if (!layout) {
    return std::unexpected("upgrade bonuses: " + layout.error());
  }
  unsigned char* property = nullptr;
  std::memcpy(&property, static_cast<unsigned char*>(pawn) + layout->modifier_property,
              sizeof(property));
  if (!property) {
    return std::unexpected("upgrade bonuses: modifier property unavailable");
  }

  // Read the modifier rows.
  struct PointerVector {
    int32_t count;
    uint32_t padding;
    unsigned char** data;
    uint32_t capacity;
    uint32_t flags;
  } rows{};
  static_assert(sizeof(rows) == 24);
  std::memcpy(&rows, property + layout->modifier_rows, sizeof(rows));
  if (rows.count < 0 || rows.count > 1024 || uint32_t(rows.count) > rows.capacity ||
      (rows.count && !rows.data)) {
    return std::unexpected("upgrade bonuses: invalid modifier vector");
  }

  // Find the bonus modifier the pawn owns.
  for (int32_t i = 0; i < rows.count; ++i) {
    auto* row = rows.data[i];
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
  return std::unexpected("upgrade bonuses: modifier not found");
#else
  (void)pawn;
  return std::unexpected("upgrade bonuses require the Windows host");
#endif
}
}  // namespace

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
    return std::unexpected(bonuses.error());
  }

  // The modifier's own deserializer notifies before each changed field; this
  // follows it instead of calling it with a fabricated engine message.
  for (size_t i = 0; i < values.size(); ++i) {
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
