#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "modlock/export.h"
#include "modlock/gameinterop/ability_definitions.h"
#include "modlock/gameinterop/connection_tracker.h"
#include "modlock/gameinterop/game_symbols.h"
#include "modlock/gameinterop/player_selection.h"
#include "proto/modlock/types.pb.h"

namespace modlock::gameinterop {

class NativeDamage;

// AbilityChargeSlots identifies the native charge queries used by HeroRefresh.
struct AbilityChargeSlots {
  size_t has_charges;
  size_t max_charges;
};

// ResolveAbilityChargeSlots decodes charge queries from the native refresh loop.
// The module must contain exactly one matching loop with aligned virtual calls.
[[nodiscard]] MODLOCK_API std::expected<AbilityChargeSlots, std::string> ResolveAbilityChargeSlots(
    const ModuleImage& server);

using SetMoveType = void (*)(void*, uint8_t, uint8_t);

// ResolvePreparationMovement verifies the native input and damage gates and
// resolves SetMoveType. The server module must outlive calls through the result.
[[nodiscard]] MODLOCK_API std::expected<SetMoveType, std::string> ResolvePreparationMovement(
    const ModuleImage& server);

// TeleportClientCamera uses the engine teleport helper that also sends
// SetClientCameraAngles to the owning client. Its velocity is zeroed.
using TeleportClientCamera = void (*)(void*, void*, const float*, const float*);
[[nodiscard]] MODLOCK_API std::expected<TeleportClientCamera, std::string>
ResolveTeleportClientCamera(const ModuleImage& server);

// PawnMotion holds the absolute-state setters the inner Teleport calls.
// Calling them directly moves an entity continuously: a Teleport every frame
// hides an animated hero model.
struct PawnMotion {
  void (*set_origin)(void*, const float*);
  void (*set_angles)(void*, const float*);
  void (*set_velocity)(void*, const float*);
};
[[nodiscard]] MODLOCK_API std::expected<PawnMotion, std::string> ResolvePawnMotion(
    const ModuleImage& server);

// RestorePawnHealth restores the network health fields and requests replication.
// The calculated maximum remains owned by engine progression and modifiers.
// A zero network maximum is valid for pawns with a calculated maximum.
[[nodiscard]] MODLOCK_API bool RestorePawnHealth(void* pawn, int32_t health,
                                                 int32_t network_max_health);
// RestorePawnLevel restores the schema-checked level and requests replication.
[[nodiscard]] MODLOCK_API bool RestorePawnLevel(void* pawn, int32_t level);
// Restores the existing hero-upgrade modifier's serialized float1..3 state.
// Refuses missing modifiers or an unsupported mapped implementation.
[[nodiscard]] MODLOCK_API std::expected<void, std::string> RestorePawnUpgradeBonuses(
    void* pawn, const std::array<float, 3>& values);
[[nodiscard]] MODLOCK_API std::optional<std::array<float, 3>> ReadPawnUpgradeBonuses(void* pawn);

// CombatLayout carries the schema-resolved offsets of the owning
// controller's inline PlayerDataGlobal_t lifetime counters: the struct's
// offset inside CCitadelPlayerController and each int32 counter inside it.
// Resolved as one capability; absent schema never disables the other fields.
struct CombatLayout {
  size_t player_data_global = 0;
  size_t hero_damage = 0;
  size_t hero_healing = 0;
  size_t self_healing = 0;
  size_t kills = 0;
  size_t deaths = 0;
  size_t assists = 0;
};

// CombatTotals preserves raw native counter values, without interpreting
// overkill, shields or effective healing.
struct CombatTotals {
  int32_t hero_damage = 0;
  int32_t hero_healing = 0;
  int32_t self_healing = 0;
  int32_t kills = 0;
  int32_t deaths = 0;
  int32_t assists = 0;
  friend bool operator==(const CombatTotals&, const CombatTotals&) = default;
};

// MovementLayout carries optional schema-resolved offsets for direct movement
// fields. Missing offsets remain absent instead of using guessed layout data.
struct MovementLayout {
  std::optional<size_t> abs_velocity;
  std::optional<size_t> ground_entity;
  std::optional<size_t> modifier_property;
  std::optional<size_t> modifier_state_mask;
};

// EntityLayout carries the schema-resolved field offsets the observer reads:
// CBasePlayerController::m_hPawn on the controller, then
// CBaseEntity::m_CBodyComponent, CBodyComponent::m_pSceneNode, and
// CGameSceneNode::m_vecAbsOrigin down the pawn's origin chain. Hero ID is
// inline in CCitadelHeroComponent::m_spawnedHero; team and health belong to
// CBaseEntity. These are observed values, not evidence of command dispatch.
// The stamina resource floats sit behind the pawn's inline ability component
// (AbilityResource_t); only the latch pair carries network metadata.
struct StaminaLayout {
  size_t current = 0;
  size_t max = 0;
  size_t latch_time = 0;
  size_t latch_value = 0;
};

struct EntityLayout {
  size_t pawn_handle = 0;
  // Hero selection must never pass the generic observer pawn to SelectHero.
  std::optional<size_t> hero_pawn_handle;
  size_t body_component = 0;
  size_t scene_node = 0;
  size_t abs_origin = 0;
  std::optional<MovementLayout> movement;
  size_t hero_id = 0;
  size_t team = 0;
  size_t health = 0;
  size_t max_health = 0;
  size_t level = 0;
  size_t eye_angles = 0;
  size_t camera_angles = 0;
  // Optional CNetworkViewOffsetVector field on CBaseModelEntity.
  std::optional<size_t> view_offset;
  // Optional inline EGold/EAbilityPoints/EAbilityUnlocks currency prefix.
  std::optional<size_t> currencies;
  // Optional controller-relative PlayerDataGlobal_t combat counters.
  std::optional<CombatLayout> combat;
  // Optional pawn-relative native stamina resource floats (AbilityResource_t
  // behind the inline ability component). Only the latch pair carries network
  // metadata; the plain floats are schema-resolved values the engine mutates
  // locally.
  std::optional<StaminaLayout> stamina;
};

struct AbilityLayout {
  size_t component = 0;
  size_t handles = 0;  // Inline pawn component plus m_vecAbilities.
  size_t owner = 0;
  size_t subclass = 0;
  size_t slot = 0;
  std::optional<size_t> consecutive_wall_jumps;
  std::optional<size_t> current_wall_normal;
  std::optional<size_t> wall_contact_position;
  std::optional<size_t> wall_jump_normal_used;
  std::optional<size_t> wall_jump_facing;
  std::optional<size_t> last_time_on_zipline;
  std::optional<size_t> channeling;
  std::optional<size_t> sliding;
  std::optional<size_t> mantle_start_time;
  size_t upgrade_info = 0;
  size_t charges = 0;
  size_t cooldown_end = 0;
  size_t cooldown_start = 0;
  size_t charge_recharge_start = 0;
  size_t charge_recharge_end = 0;
};

struct AbilityUpgrade {
  uint32_t subclass_id;
  uint16_t slot;
  uint32_t upgrade_info;
};

struct ItemTarget {
  uint32_t subclass_id;
  uint32_t upgrade_info;
  // slot is absent for older facts that did not observe placement.
  std::optional<uint16_t> slot;
};

using ModifyCurrency = void (*)(void*, uint32_t, int32_t, uint32_t, uint8_t, uint8_t, uint8_t,
                                void*, void*);
MODLOCK_API std::expected<ModifyCurrency, std::string> ResolveModifyCurrency(
    const ModuleImage& server);

using SetUpgradeBits = void (*)(void* ability, uint32_t bits);
MODLOCK_API std::expected<SetUpgradeBits, std::string> ResolveSetUpgradeBits(
    const ModuleImage& server);

// upgrade carries the initial upgrade bits in its low word; extra is an optional
// KeyValues3 node merged into the ability's spawn keyvalues.
using CreateAbility = void* (*)(void* component, void* definition, uint16_t slot, uint64_t upgrade,
                                bool flag, void* extra);
MODLOCK_API std::expected<CreateAbility, std::string> ResolveCreateAbility(
    const ModuleImage& server);

struct MODLOCK_API ItemFunctions {
  using Add = void* (*)(void* pawn, const char* name, uint64_t upgrade, void* extra);
  using Remove = void (*)(void* component, void* item, uint8_t flag);
  using SwapSlots = void (*)(void* component, uint16_t first, uint16_t second);
  Add add = nullptr;
  Remove remove = nullptr;
  SetUpgradeBits set_bits = nullptr;
  SwapSlots swap_slots = nullptr;
  static std::expected<ItemFunctions, std::string> Resolve(const ModuleImage& server);
};

// PawnObserver resolves the connected human player each frame through the
// addressed engine contracts: occupancy and xuid from ConnectionTracker's
// engine-reported ClientPutInServer/ClientDisconnect lifecycle (no unproven
// vtable call), the controller at entity index slot+1 through the
// CEntitySystem::GetEntityIdentity chunk table, and m_hPawn resolved through the
// GetRefEHandle serial proof.
//
// No entity scan, no signature scan; no pointer survives a frame except
// PawnForSlot's same-frame window.
//
// Session identity (slot, xuid) survives missed frames. Only a confirmed
// disconnect or a generation change clears it, and ConnectionTracker's engine
// generation is carried unchanged into every sample, which one-shot placement
// keys to.
class MODLOCK_API PawnObserver {
 public:
  // kSessionSlot is the default observed slot; a plugin may observe any claimed
  // client slot.
  static constexpr int32_t kSessionSlot = 0;

  // EntitySystemFn resolves the live CGameEntitySystem (production:
  // ResolveLiveEntitySystem).
  //
  // SlotStateFn reports the engine lifecycle state for one slot (production:
  // ConnectionTracker::StateForSlot).
  //
  // LayoutFn resolves EntityLayout through the game's schema system.
  using EntitySystemFn = std::function<std::expected<void*, std::string>()>;
  using SlotStateFn = std::function<ConnectionTracker::SlotState(int32_t)>;
  using LayoutFn = std::function<std::expected<EntityLayout, std::string>()>;

  struct Seams {
    EntitySystemFn entity_system;
    SlotStateFn slot_state;
    LayoutFn layout;
    std::function<std::expected<AbilityLayout, std::string>()> ability_layout;
    std::function<std::optional<int32_t>(void*)> effective_max_health;
    // Zero means the native ability has no charges; unavailable is an error.
    std::function<std::expected<int32_t, std::string>(void*)> max_charges;
  };

  // LiveSeams builds the production seams against the mapped game modules;
  // they report errors while those modules are absent.
  static Seams LiveSeams(bool movement_enabled = false);

  // Tests inject fakes over recorded memory fixtures; production uses the
  // default seams.
  PawnObserver();
  explicit PawnObserver(Seams seams, bool movement_enabled = false);

  struct Sample {
    // MovementState contains only available direct engine fields. An invalid
    // or unavailable field is absent without hiding independent exact fields.
    struct MovementState {
      std::optional<std::array<float, 3>> abs_velocity;
      std::optional<uint32_t> ground_entity_handle;
      std::optional<bool> grounded_by_handle;
      bool sliding = false;
      bool mantling = false;
      bool climbing = false;
      bool dashing = false;
      std::optional<uint32_t> mantle_ability_handle;
      std::optional<float> mantle_start_time;
      // The owned innate jump ability and its game-authored sequence counter.
      // Absence is unavailable evidence, never a zero count.
      std::optional<uint32_t> jump_ability_handle;
      std::optional<int8_t> consecutive_wall_jumps;
      // The Jump ability's wall state, as the game keeps it. The current
      // normal holds the last wall touched and is not cleared on release.
      // WallContactPosition is the pawn position when the ability last found
      // a wall it could jump off; it stops following the pawn once no wall
      // qualifies. WallJumpNormalUsed is the normal the last wall jump used,
      // and WallJumpFacing is EWallJumpFacing (0 when not on a wall).
      std::optional<std::array<float, 3>> current_wall_normal;
      std::optional<std::array<float, 3>> wall_contact_position;
      std::optional<std::array<float, 3>> wall_jump_normal_used;
      std::optional<uint16_t> wall_jump_facing;
      // LastTimeOnZipline is the simulation time the pawn last rode a zipline.
      std::optional<float> last_time_on_zipline;
    };

    int32_t slot = 0;
    double x = 0;
    double y = 0;
    double z = 0;
    uint64_t steam_id = 0;
    uint32_t session_generation = 0;
    uint32_t pawn_handle = 0;
    // Stamina is exact cumulative expenditure, not resource-bar depletion.
    std::optional<modlock::StaminaEvidence> stamina_evidence;
    // HeadshotCount is the number of distinct attributed head hits consumed
    // for this pawn during the current observed frame.
    uint32_t headshot_count = 0;
    std::optional<MovementState> movement;
    uint32_t hero_id = 0;
    uint8_t team = 0;
    int32_t health = 0;
    int32_t max_health = 0;
    int32_t level = 0;
    std::optional<std::array<int32_t, 3>> currencies;
    std::array<float, 3> eye_angles{};
    std::array<float, 3> camera_angles{};
    std::optional<std::array<float, 3>> eye_position;
    bool is_bot = false;
    // Native GetMaxHealth result; absent when unavailable, never the network base.
    std::optional<int32_t> effective_max_health;
    std::optional<std::array<float, 3>> upgrade_bonuses;
    // Raw native lifetime totals from the owning controller; absent when the
    // schema capability did not resolve.
    std::optional<CombatTotals> combat_totals;
    // Observed native stamina resource; absent when the schema capability did
    // not resolve or the values are not finite. Never a fabricated default.
    struct Stamina {
      float current;
      float max;
      friend bool operator==(const Stamina&, const Stamina&) = default;
    };
    std::optional<Stamina> stamina;
  };

  // Observe resolves this frame's sample for the requested client slot, or nullopt while
  // the slot is empty, the schema layout failed, or the pawn is not live
  // yet.
  std::optional<Sample> Observe(int32_t slot = kSessionSlot);

  // Invalidate drops the frame-scoped pawn and schema layout resolved for the
  // current world. The next observation resolves a fresh world layout.
  void Invalidate();

  // PawnForSlot returns the observed pawn pointer during the current frame
  // window, or nullptr after Observe starts the next frame.
  void* PawnForSlot(int32_t slot) const;

  // CurrentOriginForSlot rereads the observed pawn's scene-node origin during
  // the current frame window. It uses the same resolved layout and pawn as
  // Observe.
  //
  // Returns no value outside that window.
  std::optional<std::array<float, 3>> CurrentOriginForSlot(int32_t slot) const;

  // SetEyeAngles applies recorded aim to a hero pawn and requests native
  // replication. It uses the schema layout Observe resolved, so it works on any
  // live pawn of the world last observed, inside or outside the frame window.
  std::expected<void, std::string> SetEyeAngles(void* pawn, const std::array<float, 3>& angles);

  struct Ability {
    uint32_t handle;
    uint32_t subclass_id;
    uint16_t slot;
    uint32_t upgrade_info = 0;
    int32_t charges = 0;
    float cooldown_end = 0;
    // Timer endpoints are absolute simulation seconds, including inactive zero values.
    float cooldown_start = 0;
    float charge_recharge_start = 0;
    float charge_recharge_end = 0;
    std::optional<int8_t> consecutive_wall_jumps;
    std::optional<std::array<float, 3>> current_wall_normal;
    std::optional<std::array<float, 3>> wall_contact_position;
    std::optional<std::array<float, 3>> wall_jump_normal_used;
    std::optional<uint16_t> wall_jump_facing;
    std::optional<float> last_time_on_zipline;
    bool channeling = false;
    bool sliding = false;
    std::optional<float> mantle_start_time;
  };

  // SelectPlayer reborrows the connected controller after every native callback.
  std::expected<void, std::string> SelectPlayer(int32_t slot, uint64_t steam_id,
                                                uint32_t generation, int32_t team, void* definition,
                                                const PlayerSelectionCalls& calls);

  // RespawnPlayer requests native respawn only for this connection's dead hero pawn.
  // Subsequent observation establishes readiness; the native call may replace the pawn.
  std::expected<void, std::string> RespawnPlayer(int32_t slot, uint32_t generation,
                                                 RespawnPawn respawn);

  // EliminatePlayer requests native death on the currently observed connection.
  // The session retains round eligibility independently of native respawn state.
  std::expected<void, std::string> EliminatePlayer(int32_t slot, uint32_t generation,
                                                   const NativeDamage& damage);

  // SpectatorPawnForSlot returns the observer pawn of a player on the
  // spectator team, whose observer services aim the player's camera. It
  // fails while the player plays a hero or before the game publishes the pawn.
  std::expected<void*, std::string> SpectatorPawnForSlot(int32_t slot);

  // SetPreparationFrozen holds input, native movement, and damage on the current
  // pawn. Release restores the acquired pawn's movement mode and damage setting.
  std::expected<void, std::string> SetPreparationFrozen(int32_t slot, bool frozen);

  // AdjustSouls adds delta souls to the slot's wallet, or spends -delta. A
  // spend larger than the balance fails without changing it. A visible grant
  // plays the native soul pickup feedback; a silent one does not.
  std::expected<Sample, std::string> AdjustSouls(int32_t slot, int32_t delta, bool silent,
                                                 ModifyCurrency modify);
  // PrepareStartingSouls grants the shortfall to the selected starting wallet.
  // Existing funds and purchases survive; native progression owns earned levels.
  std::expected<Sample, std::string> PrepareStartingSouls(int32_t slot, int32_t souls,
                                                          ModifyCurrency modify);

  // ResetStartingSouls replaces the current build through native hero reset and
  // applies the exact starting wallet through native currency progression.
  std::expected<Sample, std::string> ResetStartingSouls(int32_t slot, int32_t souls,
                                                        ModifyCurrency modify, ResetHeroPawn reset);

  // PrepareAbilityPoints grants enough unspent currency to unlock and fully
  // upgrade all four abilities. The caller applies it once per fresh hero pawn.
  std::expected<Sample, std::string> PrepareAbilityPoints(int32_t slot, ModifyCurrency modify);

  // Reads the frame's owned ability handles, including items. Every nonempty
  // handle must resolve with the current serial and point back to this pawn.
  // Missing schema or incomplete ownership returns an error, never a partial
  // loadout. Empty handle slots are omitted; no engine pointer escapes.
  std::expected<std::vector<Ability>, std::string> CurrentAbilitiesForSlot(int32_t slot);

  // Validates every target against this frame's owned handles before applying.
  // The engine setter owns upgrade effects and replication. Its ABI writes
  // the packed high word only; a differing low word is refused, not overwritten.
  std::expected<void, std::string> ApplyAbilityUpgrades(int32_t slot,
                                                        std::span<const AbilityUpgrade> upgrades,
                                                        SetUpgradeBits set_bits);

  // ReconcileAbilities creates missing abilities through the native component
  // and applies their recorded upgrade masks. Existing slots must agree with
  // the requested definitions; ownership is reborrowed after each engine call.
  std::expected<void, std::string> ReconcileAbilities(int32_t slot,
                                                      std::span<const AbilityUpgrade> targets,
                                                      const AbilityDefinitions& definitions,
                                                      CreateAbility create,
                                                      SetUpgradeBits set_bits);

  // ApplyAbilityTimers writes charge counts and absolute destination-clock intervals.
  // Every target must retain this frame's owned handle, subclass and slot. The entire
  // request is validated before mutation; replication and immediate readback follow
  // each changed entity. Clock rebasing and sustained verification belong to the caller.
  std::expected<void, std::string> ApplyAbilityTimers(int32_t slot,
                                                      std::span<const Ability> targets);

  // SetPracticeStamina sets the pawn's native stamina resource to stamina,
  // clamped to the current native maximum, or to that maximum when stamina is
  // absent, and latches the value at the caller's simulation time. It mirrors
  // the installed HeroRefresh modifier's field relationships (current = value,
  // latchValue = value, latchTime = now) without repeating the modifier's
  // incremental +1 refill. Native max and regen rate are retained. The
  // explicit practice caller owns the policy; no exact-replay path calls this.
  // Reborrows the pawn through this frame's observation before writing and
  // after the replication notification.
  std::expected<void, std::string> SetPracticeStamina(int32_t slot, float now,
                                                      std::optional<float> stamina = {});

  // FreshAbilityTargets preserves the owned build, clears cooldown intervals,
  // and obtains full charges from each native ability before any timer write.
  std::expected<std::vector<Ability>, std::string> FreshAbilityTargets(int32_t slot);

  // GrantItem adds one enabled item through native inventory and verifies ownership.
  // An already owned definition is returned without granting another copy. Readback
  // must retain every previous ability and active slot; this never rebuilds a loadout.
  // Callers select component-free items: native upgrade merging is not reversible here.
  std::expected<Ability, std::string> GrantItem(int32_t slot, uint32_t subclass_id,
                                                const AbilityDefinitions& definitions,
                                                const ItemFunctions& functions);

  // Converges owned upgrade definitions and masks through engine callbacks.
  // Restores observed active-item slots through the engine slot-table owner.
  // Imbuements, charges and cooldowns have separate restoration operations.
  // Immediate readback is required; sustained restore verification is separate.
  std::expected<void, std::string> ReconcileItems(int32_t slot, std::span<const ItemTarget> targets,
                                                  const AbilityDefinitions& definitions,
                                                  const ItemFunctions& functions);

 private:
  // A successful creation may precede controller-handle publication. Never
  // create another actor while that connection is awaiting its first pawn.
  struct PendingSelection {
    uint64_t steam_id;
    uint32_t generation;
    int32_t team;
  };
  std::map<int32_t, PendingSelection> pending_selections_;
  // Freeze ownership follows the retained pawn, including across a reconnect.
  struct FrozenPawn {
    uint32_t handle;
    uint64_t steam_id;
    uint8_t takes_damage;
    uint8_t move_type;
    uint8_t move_collide;
  };
  std::map<int32_t, FrozenPawn> frozen_pawns_;
  // The loaded server outlives this observer; cache failed bindings as well.
  std::optional<std::expected<SetMoveType, std::string>> preparation_movement_;
  void NoteDegradation(const char* reason);
  // ResolveLayout resolves the schema layout once per world.
  std::expected<void, std::string> ResolveLayout();

  Seams seams_;
  bool movement_enabled_ = false;
  bool offsets_ok_ = false;
  bool degradation_logged_ = false;
  EntityLayout layout_{};
  bool trace_slot_occupied_ = false;
  void* frame_pawn_ = nullptr;
  void* frame_entity_system_ = nullptr;
  uint32_t frame_pawn_handle_ = 0;
  ConnectionTracker::SlotState frame_connection_;
  std::optional<AbilityLayout> ability_layout_;
  int32_t frame_slot_ = -1;
};

}  // namespace modlock::gameinterop
