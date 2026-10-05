#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "modlock/export.h"
#include "modlock/gameinterop/game_symbols.h"
#include "modlock/gameinterop/native_memory.h"

namespace modlock::gameinterop {

// Native modifier-event IDs decoded by the combat observer, as numbered in
// game build 6711. Game updates renumber EModifierEvent, so CombatEventsHook
// resolves the running game's ids by name (ModifierEventIndex) and never
// dispatches on these numbers; they remain for fixtures and existing callers.
// The broadcast ids are the same damage/heal facts delivered twice; only the
// modifier-path ids are decoded, so no event is counted twice.
inline constexpr uint32_t kCombatEventPreDamageTaken = 0x17;
inline constexpr uint32_t kCombatEventDamageTaken = 0x18;
inline constexpr uint32_t kCombatEventHealthTaken = 0x1a;
inline constexpr uint32_t kCombatEventAbilityExecuted = 0x25;
inline constexpr uint32_t kCombatBroadcastShieldAbsorbed = 0x0f;

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

// HealthTakenEvent carries one native heal. requested is the raw heal amount;
// applied is what the game clamped into current health, so requested > applied
// marks overheal. Regen coverage is not yet accepted; this event does not
// attribute a source.
struct HealthTakenEvent {
  uint32_t entity_handle = 0;
  float requested = 0.0f;
  int32_t applied = 0;
};

// AbilityExecutedEvent carries the raw packed handles from one native ability
// execution. The target may be invalid for an untargeted ability; consumers
// resolve ownership and classify the ability before applying their rules.
struct AbilityExecutedEvent {
  uint32_t caster_handle = 0;
  uint32_t ability_handle = 0;
  uint32_t target_handle = 0;
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

// CombatEvent is one decoded native event: the kind, the decoded values, and
// when decoding failed, the reason. A decode failure is a capability failure
// and must surface as kDecodeFailed, never as a zero-filled success.
struct CombatEvent {
  enum class Kind {
    kDamageTaken,
    kHealthTaken,
    kShieldDamage,
    kAbilityExecuted,
    kMovementExecuted,
    kDecodeFailed,
  };
  Kind kind = Kind::kDecodeFailed;
  // reason is set only for kDecodeFailed.
  std::string reason;
  // At most one of the payloads is meaningful, by kind.
  std::optional<DamageTakenEvent> damage;
  std::optional<HealthTakenEvent> health;
  std::optional<ShieldDamageEvent> shield;
  std::optional<AbilityExecutedEvent> ability;
  std::optional<MovementExecutedEvent> movement;
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

// DecodeHealthTaken reads one HealthTaken eventData payload (native layout:
// entity handle +0, requested float +4, applied int +8).
[[nodiscard]] MODLOCK_API std::expected<HealthTakenEvent, std::string> DecodeHealthTaken(
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
// healing and ability execution into immutable values. The broadcast hook
// observes shield absorption without duplicating modifier events. One hook may
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

 private:
  struct Impl;
  explicit CombatEventsHook(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace modlock::gameinterop
