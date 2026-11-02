#include <cstring>

#include "modlock/gameinterop/entity_abi.h"
#include "modlock/gameinterop/pawn_observer.h"
#include "pawn_observer_internal.h"

namespace modlock::gameinterop {

namespace {

// m_bvEnabledStateMask is a CBitVec of the native modifier states.
constexpr size_t kModifierStateMaskSize = 24;

// ResolveScoreLayout resolves the scoreboard counters inside the controller's
// PlayerDataGlobal_t. Each inline counter must fit both its own field and the
// controller's enclosing storage.
std::optional<ScoreLayout> ResolveScoreLayout(void* schema_system) {
  // Find the enclosing player data.
  const auto data =
      SchemaFieldOf(schema_system, "server.dll", "CCitadelPlayerController", "m_PlayerDataGlobal");
  if (!data) {
    return std::nullopt;
  }

  // Resolve every counter inside it.
  ScoreLayout score{.player_data_global = data->offset};
  for (const auto& counter : kScoreCounters) {
    const auto field =
        SchemaFieldOf(schema_system, "server.dll", "PlayerDataGlobal_t", counter.field_name);
    if (!field || field->size < sizeof(int32_t) || field->offset > data->size ||
        data->size - field->offset < sizeof(int32_t)) {
      return std::nullopt;
    }
    score.*counter.offset = field->offset;
  }
  return score;
}

// ResolveMovementLayout resolves the movement fields the schema has, or is
// empty when it has none of them.
std::optional<MovementLayout> ResolveMovementLayout(void* schema_system) {
  // Resolve velocity and ground.
  MovementLayout movement{};
  const OptionalSchemaOffset optional[] = {
      {"CBaseEntity", "m_vecAbsVelocity", sizeof(float) * 3, &movement.abs_velocity},
      {"CBaseEntity", "m_hGroundEntity", sizeof(uint32_t), &movement.ground_entity},
  };
  bool available = ResolveOptionalSchemaOffsets(schema_system, "server.dll", optional) > 0;

  // Modifier states mark dashing and climbing; they need both fields.
  size_t property = 0;
  size_t mask = 0;
  const SchemaOffset modifier[] = {
      {"CBaseEntity", "m_pModifierProp", sizeof(void*), &property},
      {"CModifierProperty", "m_bvEnabledStateMask", kModifierStateMaskSize, &mask},
  };
  if (ResolveSchemaOffsets(schema_system, "server.dll", modifier)) {
    movement.modifier_property = property;
    movement.modifier_state_mask = mask;
    available = true;
  }
  if (!available) {
    return std::nullopt;
  }
  return movement;
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

  // Name the four floats.
  StaminaLayout offsets;
  size_t current = 0, max = 0, latch_time = 0, latch_value = 0;
  const Wanted fields[] = {
      {"m_flCurrentValue", &current},
      {"m_flMaxValue", &max},
      {"m_flLatchTime", &latch_time},
      {"m_flLatchValue", &latch_value},
  };
  for (const auto& wanted : fields) {
    // Each must hold a float.
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

std::expected<EntityLayout, std::string> ResolveEntityLayout(bool movement_enabled) {
  // Resolve the schema system.
  auto schema_system = ResolveSchemaSystem();
  if (!schema_system) {
    return std::unexpected(schema_system.error());
  }

  // Resolve the core fields that every observation needs.
  EntityLayout layout{};
  size_t hero_component = 0, spawned_hero = 0, hero_id = 0;

  // Fill every one, or fail at the first missing or short one.
  const SchemaOffset core[] = {
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
  if (auto resolved = ResolveSchemaOffsets(*schema_system, "server.dll", core); !resolved) {
    return std::unexpected(resolved.error());
  }

  // Hero component and spawn data are inline schema structures.
  layout.hero_id = hero_component + spawned_hero + hero_id;

  // Resolve the optional controller, view and economy fields. Native
  // ECurrencyType places gold, ability points and unlocks first in
  // m_nCurrencies.
  const OptionalSchemaOffset optional[] = {
      {"CCitadelPlayerController", "m_hHeroPawn", 0, &layout.hero_pawn_handle},
      {"CCitadelPlayerController", "m_ePlayState", sizeof(int32_t), &layout.play_state},
      {"CCitadelPlayerController", "m_PlayerDataGlobal.m_nHeroID", sizeof(uint32_t),
       &layout.player_hero_id},
      {"CCitadelPlayerController", "m_PlayerDataGlobal.m_nPreGameHeroID", sizeof(uint32_t),
       &layout.pregame_hero_id},
      {"CBaseModelEntity", "m_vecViewOffset", kViewOffsetSize, &layout.view_offset},
      {"CCitadelPlayerPawn", "m_nCurrencies", sizeof(int32_t) * 3, &layout.currencies},
      {"CCitadelPlayerPawn", "m_nSpentCurrencies", sizeof(int32_t), &layout.spent_souls},
  };
  ResolveOptionalSchemaOffsets(*schema_system, "server.dll", optional);

  // Missing scoreboard, stamina or movement schema leaves pose observation
  // available.
  layout.score = ResolveScoreLayout(*schema_system);
  if (auto stamina = ResolveStaminaLayout()) {
    layout.stamina = *stamina;
  }
  if (movement_enabled) {
    layout.movement = ResolveMovementLayout(*schema_system);
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

  // Fill every one, or fail at the first missing or short one.
  const SchemaOffset fields[] = {
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
  if (auto resolved = ResolveSchemaOffsets(*schema, "server.dll", fields); !resolved) {
    return std::unexpected(resolved.error());
  }
  layout.handles += layout.component;

  // Resolve the optional jump, slide and mantle fields.
  constexpr size_t kVector = 3 * sizeof(float);
  const OptionalSchemaOffset optional[] = {
      {"CCitadel_Ability_Jump", "m_nConsecutiveWallJumps", sizeof(int8_t),
       &layout.consecutive_wall_jumps},
      {"CCitadel_Ability_Jump", "m_vCurrentWallNormal", kVector, &layout.current_wall_normal},
      {"CCitadel_Ability_Jump", "m_vLastValidWallJumpNormal_PlayerPosition", kVector,
       &layout.wall_contact_position},
      {"CCitadel_Ability_Jump", "m_vWallJumpNormalUsed", kVector, &layout.wall_jump_normal_used},
      {"CCitadel_Ability_Jump", "m_eWallJumpFacing", sizeof(uint16_t), &layout.wall_jump_facing},
      {"CCitadel_Ability_Jump", "m_flLastTimeOnZipLine", sizeof(float),
       &layout.last_time_on_zipline},
      {"CCitadelBaseAbility", "m_bChanneling", sizeof(bool), &layout.channeling},
      {"CCitadel_Ability_Slide", "m_bIsSliding", sizeof(bool), &layout.sliding},
      {"CCitadel_Ability_Mantle", "m_flStartTime", sizeof(float), &layout.mantle_start_time},
  };
  ResolveOptionalSchemaOffsets(*schema, "server.dll", optional);
  return layout;
}

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

}  // namespace modlock::gameinterop
