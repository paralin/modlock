#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "modlock/export.h"
#include "modlock/gameinterop/game_symbols.h"
#include "modlock/gameinterop/native_memory.h"

namespace modlock::gameinterop {

// DamageTakenEvent carries the immutable values one native damage application
// reported. health_lost is post-mitigation health damage; accounting clamps it
// to health_before for overkill. Shield absorption is reported separately.
// Handles are the game's packed entity handles as the event carried them.
struct DamageTakenEvent {
  uint32_t victim_handle = 0;
  uint32_t attacker_handle = 0;
  int32_t health_lost = 0;
  int32_t health_before = 0;
  int32_t damage_dealt = 0;
  uint32_t ability_handle = 0;
};

// DamageContactEvent identifies the native contact before damage is applied.
// Flags retain the engine's distinct light/heavy melee classification.
struct DamageContactEvent {
  uint32_t victim_handle = 0;
  uint32_t attacker_handle = 0;
  uint64_t flags = 0;
  uint32_t ability_handle = 0;
  // Inflictor is the native damage source, including a projectile on impact.
  uint32_t inflictor_handle = UINT32_MAX;
  // HitGroup is the engine trace classification; -1 means no hit group.
  int32_t hit_group = -1;
  // Amount is the native pre-mitigation damage.
  float amount = 0;
};

// AdjustDamage changes the native pre-mitigation amount in place. Subscribers
// run on the engine thread after suppression decisions and before native damage.
using AdjustDamage = std::function<void(const DamageContactEvent&, float&)>;

inline constexpr uint64_t kDamageLightMelee = 0x400000000;
inline constexpr uint64_t kDamageHeavyMelee = 0x200000000;

// DamageContactOffsets comes from CTakeDamageInfo's live schema.
struct DamageContactOffsets {
  size_t attacker = 0;
  size_t flags = 0;
  size_t amount = 0;
  size_t ability = 0;
  std::optional<size_t> inflictor;
  std::optional<size_t> hit_group;
};

// ShieldDamageEvent is the amount removed from a native shield by one hit.
// The broadcast carries the attacker, shield owner and shield caster handles;
// the caster is not necessarily the player who took the damage.
struct ShieldDamageEvent {
  uint32_t attacker_handle = 0;
  uint32_t victim_handle = 0;
  float absorbed = 0;
};

// HealedEvent carries one heal the game applied (native UNIT_HEALED layout:
// healer handle +0, target handle +4, ability handle +8, applied int +0xC,
// leftover uint +0x10). requested is applied plus the leftover the target's
// maximum health refused. Handles are kInvalidEHandleIndex when the game
// names none. Regeneration, heal types 1 and 3, fires no UNIT_HEALED.
struct HealedEvent {
  uint32_t healer_handle = 0;
  uint32_t target_handle = 0;
  uint32_t ability_handle = 0;
  int32_t applied = 0;
  int32_t requested = 0;
};

// LevelChangedEvent carries one hero reaching a new level (native layout:
// hero handle +0, level +4).
struct LevelChangedEvent {
  uint32_t hero_handle = 0;
  int32_t level = 0;
};

// MeleeEvent carries one melee swing as the game resolves it (native layout:
// attacker handle +0, heavy flag +5, and from +0x18 the count and storage of
// the hit entities' handles). A swing that hits nothing has no hits.
struct MeleeEvent {
  uint32_t attacker_handle = 0;
  bool heavy = false;
  std::vector<uint32_t> hit_handles;
};

// ParryEvent carries one parry that caught a melee swing (native layout:
// parrier handle +0, attacker handle +4).
struct ParryEvent {
  uint32_t parrier_handle = 0;
  uint32_t attacker_handle = 0;
};

// AbilityExecutedEvent carries the raw packed handles from one native ability
// execution. The target may be invalid for an untargeted ability; consumers
// resolve ownership and classify the ability before applying their rules.
struct AbilityExecutedEvent {
  uint32_t caster_handle = 0;
  uint32_t ability_handle = 0;
  uint32_t target_handle = 0;
};

// ModifierFiredEvent is one watched modifier event with the handles of the
// three entities the game fired it with. Which of them an event fills depends
// on the event; one the event leaves out, or that is not a live entity, reads
// kInvalidEHandleIndex.
struct ModifierFiredEvent {
  // id is the running game's EModifierEvent value.
  uint32_t id = 0;
  uint32_t caster_handle = 0;
  uint32_t target_handle = 0;
  // ability_handle is the entity the event names as its source, an ability
  // for the events an ability causes.
  uint32_t ability_handle = 0;
};

// Native movement executions from FireModifierEvent. The caster is copied
// while the engine owns its entity; consumers must match the full pawn handle.
// The enumerators name movement facts with their modifier event ids in game
// build 6711; the hook maps the running game's ids onto them by name, so they
// stay stable across game updates.
enum class MovementExecution : uint32_t {
  kLandedOnGround = 0x35,
  kAttachedToZipline = 0x36,
  kGroundDash = 0x3b,
  kSlide = 0x3f,
  kBouncePadActivated = 0x47,
  kDashJump = 0x49,
  kAirJump = 0x4a,
  kWallJump = 0x4b,
  kAirDash = 0x4c,
  kMeleeAttackStarted = 0x59,
};

struct MovementExecutedEvent {
  MovementExecution execution;
  uint32_t pawn_handle = 0;
};

// InteractionKind names a map object's completed interaction: a pickup the
// game's common completion handed to its collector, or a breakable prop the
// game's break callback announced through MODIFIER_EVENT_BREAKABLE_PROP_BROKEN.
// Starting a vacuum, an object's disappearance and the healing a pickup grants
// are not interactions; only completion is.
enum class InteractionKind : uint8_t {
  kPickupCollected,
  kPropBroken,
};

// InteractionEvent is one completed interaction, copied while the native call
// owned its entities. actor_handle is the collector or breaker the native call
// named, never an entity that merely benefited or stood near. origin is where
// the object stood before the interaction moved it: a vacuumed pickup's
// m_vVacuumStartPos, otherwise the object's absolute origin. vacuum_start is the
// game time the pickup began flying to this collector, absent when it did not.
struct InteractionEvent {
  InteractionKind kind = InteractionKind::kPickupCollected;
  uint32_t actor_handle = 0;
  uint32_t object_handle = 0;
  // inflictor_handle is the entity that broke a prop, such as a projectile;
  // zero for a pickup.
  uint32_t inflictor_handle = 0;
  std::string designer_name;
  uint32_t subclass_id = 0;
  std::array<float, 3> origin{};
  std::optional<float> vacuum_start;
};

// InteractionOffsets are the schema offsets the interaction decoder reads an
// object through. The vacuum fields are CCitadel_Pickup's and apply only to
// pickups.
struct InteractionOffsets {
  size_t subclass = 0;
  size_t body = 0;
  size_t scene_node = 0;
  size_t origin = 0;
  size_t vacuum_start_position = 0;
  size_t vacuum_start_time = 0;
  size_t vacuum_target = 0;
};

// DescribeInteractionObject reads one interaction's object through the
// bounded reader: its subclass and where it stood. A pickup whose vacuum
// target is the actor reports its vacuum start position and time; any other
// object reports its absolute origin. The designer name is the caller's,
// which DesignerName reads in place.
[[nodiscard]] MODLOCK_API std::expected<void, std::string> DescribeInteractionObject(
    const void* object, const InteractionOffsets& offsets, const BoundedReader& read,
    InteractionEvent& event);

// DecodePropBroken reads the MODIFIER_EVENT_BREAKABLE_PROP_BROKEN payload:
// three packed handles, the box +0, the inflictor +4 and the breaker +8.
[[nodiscard]] MODLOCK_API std::expected<InteractionEvent, std::string> DecodePropBroken(
    const void* event_data, const BoundedReader& read);

// CombatEvent is one decoded native event: the kind, the decoded values, and
// when decoding failed, the reason. A decode failure is a capability failure
// and must surface as kDecodeFailed, never as a zero-filled success.
struct CombatEvent {
  enum class Kind {
    kDamageTaken,
    kHealed,
    kLevelChanged,
    kMelee,
    kParry,
    kShieldDamage,
    kAbilityExecuted,
    kMovementExecuted,
    kInteraction,
    kModifierFired,
    kDecodeFailed,
  };
  Kind kind = Kind::kDecodeFailed;
  // reason is set only for kDecodeFailed.
  std::string reason;
  // At most one of the payloads is meaningful, by kind.
  std::optional<DamageTakenEvent> damage;
  std::optional<HealedEvent> heal;
  std::optional<LevelChangedEvent> level;
  std::optional<MeleeEvent> melee;
  std::optional<ParryEvent> parry;
  std::optional<ShieldDamageEvent> shield;
  std::optional<AbilityExecutedEvent> ability;
  std::optional<MovementExecutedEvent> movement;
  std::optional<InteractionEvent> interaction;
  std::optional<ModifierFiredEvent> modifier;
};

// ObservedInteractions reports which interactions the installed hook can
// deliver; a game build without a signature, enumerator or field leaves its
// kind unobserved, and consumers must treat that kind's absence as unknown.
struct ObservedInteractions {
  bool pickups = false;
  bool breaks = false;
};

// DamageResultOffsets holds the runtime-resolved CTakeDamageResult field
// offsets the damage decoder reads through. Every offset comes from
// SchemaFieldOf; a field with less than 4 bytes of storage is rejected at
// resolve time.
struct DamageResultOffsets {
  size_t health_lost = 0;
  size_t health_before = 0;
  size_t damage_dealt = 0;
  // ability reads CTakeDamageInfo, not CTakeDamageResult, when configured.
  std::optional<size_t> ability;
  // Game build 6711 replaced the int32 m_nDamageDealt with the float
  // m_flTotalledDamageDealt; the decoder rounds it into damage_dealt.
  bool damage_dealt_float = false;
};

// ProcessDamageContact decodes PreDamageTaken. Suppression blocks health,
// force, effects and combat procs; otherwise adjustment changes the native
// pre-mitigation amount. No pointer escapes either callback.
[[nodiscard]] MODLOCK_API std::expected<void, std::string> ProcessDamageContact(
    void* event_data, const DamageContactOffsets& offsets, const BoundedReader& read,
    const BoundedWriter& write, const std::function<bool(const DamageContactEvent&)>& suppress,
    const AdjustDamage& adjust = {});

// DecodeDamageTaken reads one DamageTaken eventData payload (native layout:
// victim handle +0, attacker handle +4, CTakeDamageInfo* +8,
// CTakeDamageResult* +16) through the bounded reader. Every
// pointer dereference goes through it; a bad address yields an error, never a
// crash and never a zero-filled success. Separately callable so byte fixtures
// exercise it without a hook.
[[nodiscard]] MODLOCK_API std::expected<DamageTakenEvent, std::string> DecodeDamageTaken(
    const void* event_data, const DamageResultOffsets& offsets, const BoundedReader& read);

// DecodeHealed reads one UNIT_HEALED payload.
[[nodiscard]] MODLOCK_API std::expected<HealedEvent, std::string> DecodeHealed(
    const void* event_data, const BoundedReader& read);

// DecodeLevelChanged reads one HERO_LEVEL_CHANGED payload.
[[nodiscard]] MODLOCK_API std::expected<LevelChangedEvent, std::string> DecodeLevelChanged(
    const void* event_data, const BoundedReader& read);

// DecodeMelee reads one MELEE_ATTACK payload, with at most kMaxMeleeHits hits.
inline constexpr int32_t kMaxMeleeHits = 64;
[[nodiscard]] MODLOCK_API std::expected<MeleeEvent, std::string> DecodeMelee(
    const void* event_data, const BoundedReader& read);

// DecodeParry reads one PARRY_SUCCESS payload.
[[nodiscard]] MODLOCK_API std::expected<ParryEvent, std::string> DecodeParry(
    const void* event_data, const BoundedReader& read);

[[nodiscard]] MODLOCK_API std::expected<ShieldDamageEvent, std::string> DecodeShieldDamage(
    const void* event_data, const BoundedReader& read);

// DecodeAbilityExecuted reads the native ability payload through the bounded
// reader: caster handle +0, ability handle +4, target handle +8. Handles are
// preserved without validation, including an invalid target handle.
[[nodiscard]] MODLOCK_API std::expected<AbilityExecutedEvent, std::string> DecodeAbilityExecuted(
    const void* event_data, const BoundedReader& read);

// ModifierEventIndex reads one EModifierEvent value by its schema name, such
// as "MODIFIER_EVENT_MELEE_ATTACK_STARTED", from the module's own enumerator
// records: the record that points at the name's string carries its value
// beside the pointer. A name that is absent or ambiguous is an error.
[[nodiscard]] MODLOCK_API std::expected<uint32_t, std::string> ModifierEventIndex(
    const ModuleImage& image, std::string_view name);

// CombatEventsHook interposes the native FireModifierEvent free function
// through a native detour. The thunk forwards every event
// with all five arguments and the original return value unchanged, so native
// modifier dispatch is never suppressed; before the call it decodes damage,
// healing, level changes, melee swings, parries, ability execution and prop
// breaks into immutable values. The
// broadcast hook observes shield absorption without duplicating modifier
// events, and the pickup hook observes each pickup's completion with its
// collector before forwarding it once. One hook may
// exist at a time; the handler runs on the native thread that fired the event
// and receives no entity pointers, only decoded values.
class MODLOCK_API CombatEventsHook {
 public:
  using Handler = std::function<void(const CombatEvent&)>;

  // Install scans server.dll for the FireModifierEvent signature, resolves
  // CTakeDamageResult and requested CTakeDamageInfo fields through the live
  // schema system, and hooks the function. Failure names the rejected shape.
  static std::expected<CombatEventsHook, std::string> Install(
      const ModuleImage& server, void* schema_system, Handler handler,
      std::function<bool(const DamageContactEvent&)> suppress_damage = {},
      AdjustDamage adjust_damage = {});

  CombatEventsHook(CombatEventsHook&&) noexcept;
  CombatEventsHook& operator=(CombatEventsHook&&) noexcept;
  ~CombatEventsHook();

  CombatEventsHook(const CombatEventsHook&) = delete;
  CombatEventsHook& operator=(const CombatEventsHook&) = delete;

  // Watch reports every later modifier event with this EModifierEvent value
  // to the handler as kModifierFired, until a matching Unwatch or the hook's
  // destruction. Watches of one value count, so the event is reported while
  // any remains. The value comes from ModifierEventIndex.
  std::expected<void, std::string> Watch(uint32_t id);
  // Unwatch ends one Watch of the value.
  void Unwatch(uint32_t id);

  // Observed reports which interaction kinds this hook delivers.
  [[nodiscard]] ObservedInteractions Observed() const;

 private:
  struct Impl;
  explicit CombatEventsHook(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace modlock::gameinterop
