#include "modlock/gameinterop/combat_events.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <utility>

#include "modlock/gameinterop/entity_abi.h"

#if defined(_WIN32)
#include <windows.h>

#include <safetyhook.hpp>
#endif

namespace modlock::gameinterop {
namespace {

// Native modifier-event payload offsets.
constexpr size_t kDamageVictimOffset = 0;
constexpr size_t kDamageAttackerOffset = 4;
constexpr size_t kDamageResultOffset = 16;
constexpr size_t kHealthEntityOffset = 0;
constexpr size_t kHealthRequestedOffset = 4;
constexpr size_t kHealthAppliedOffset = 8;
constexpr size_t kAbilityCasterOffset = 0;
constexpr size_t kAbilityHandleOffset = 4;
constexpr size_t kAbilityTargetOffset = 8;
constexpr size_t kBrokenPropOffset = 0;
constexpr size_t kBrokenInflictorOffset = 4;
constexpr size_t kBrokenBreakerOffset = 8;

std::string AddressName(const void* address) {
  char text[32] = {};
  std::snprintf(text, sizeof(text), "0x%llx",
                static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(address)));
  return text;
}

std::string Unreadable(const void* address, std::string_view what) {
  return "combat event: cannot read " + std::string(what) + " at " + AddressName(address);
}

// kMaxModifierEvent bounds a plausible EModifierEvent value.
constexpr int64_t kMaxModifierEvent = 1024;

// kMovementEventNames pairs each movement fact with its EModifierEvent name.
constexpr std::array<std::pair<std::string_view, MovementExecution>, 10> kMovementEventNames{{
    {"MODIFIER_EVENT_LANDED_ON_GROUND", MovementExecution::kLandedOnGround},
    {"MODIFIER_EVENT_ATTACHED_TO_ZIPLINE", MovementExecution::kAttachedToZipline},
    {"MODIFIER_EVENT_PLAYER_GROUND_DASH_STARTED", MovementExecution::kGroundDash},
    {"MODIFIER_EVENT_PLAYER_SLIDE_STARTED", MovementExecution::kSlide},
    {"MODIFIER_EVENT_BOUNCE_PAD_ACTIVATED", MovementExecution::kBouncePadActivated},
    {"MODIFIER_EVENT_DASH_JUMP_EXECUTED", MovementExecution::kDashJump},
    {"MODIFIER_EVENT_AIR_JUMP_EXECUTED", MovementExecution::kAirJump},
    {"MODIFIER_EVENT_WALL_JUMP_EXECUTED", MovementExecution::kWallJump},
    {"MODIFIER_EVENT_AIR_DASH_EXECUTED", MovementExecution::kAirDash},
    {"MODIFIER_EVENT_MELEE_ATTACK_STARTED", MovementExecution::kMeleeAttackStarted},
}};

#if defined(_WIN32)

// Thunk state: one hook may exist at a time. The handler and the schema
// offsets are set once at install; the decoded event is per call.
// EventIds are the running game's EModifierEvent values, resolved by name at
// install; the thunk dispatches on these, never on the framework's constants.
// An id the running game does not name stays kUnobservedEvent, which no
// EModifierEvent value reaches, so only that event goes unobserved.
constexpr uint32_t kUnobservedEvent = UINT32_MAX;

struct EventIds {
  uint32_t pre_damage = kUnobservedEvent;
  uint32_t damage = kUnobservedEvent;
  uint32_t health = kUnobservedEvent;
  uint32_t ability = kUnobservedEvent;
  uint32_t shield_broadcast = kUnobservedEvent;
  uint32_t prop_broken = kUnobservedEvent;
  std::array<std::pair<uint32_t, MovementExecution>, kMovementEventNames.size()> movement{};
};

EventIds g_ids;

// g_watched counts, for each EModifierEvent value, the consumers that asked to
// hear it whole. The engine thread changes a count and any thread that fires
// an event reads it.
std::array<std::atomic<uint32_t>, kMaxModifierEvent> g_watched{};

CombatEventsHook::Handler* g_handler = nullptr;
DamageResultOffsets g_damage_offsets;
DamageContactOffsets g_contact_offsets;
std::function<bool(const DamageContactEvent&)>* g_suppress_damage = nullptr;
AdjustDamage* g_adjust_damage = nullptr;
safetyhook::InlineHook* g_hook = nullptr;
safetyhook::InlineHook* g_broadcast_hook = nullptr;
safetyhook::InlineHook* g_pickup_hook = nullptr;
// g_interactions holds the object offsets; without them neither interaction
// kind is observed.
std::optional<InteractionOffsets> g_interactions;

// Describe copies an interaction's object while the native call owns it. A
// failed read still reports the interaction, without its place, so a
// completion is never lost for want of an origin.
void Describe(void* object, InteractionEvent& event) {
  event.object_handle = ReferenceHandleOf(object).value_or(kInvalidEHandleIndex);
  event.designer_name = std::string(DesignerName(object));
  if (auto described = DescribeInteractionObject(object, *g_interactions, ReadNative, event);
      !described) {
    std::fprintf(stderr, "[modlock] interaction: %s\n", described.error().c_str());
  }
}

void DispatchPropBroken(void* target, void* event_data) {
  CombatEvent decoded;
  if (auto value = DecodePropBroken(event_data, ReadNative)) {
    decoded.kind = CombatEvent::Kind::kInteraction;
    decoded.interaction = std::move(*value);
    Describe(target, *decoded.interaction);
  } else {
    decoded.reason = value.error();
  }
  (*g_handler)(decoded);
}

void __fastcall PickupThunk(void* pickup, void* collector) {
  // Copy the pickup and its collector before the completion applies and
  // deactivates it; the handles name what the native call was given.
  std::optional<CombatEvent> decoded;
  if (g_handler && pickup) {
    decoded.emplace();
    decoded->kind = CombatEvent::Kind::kInteraction;
    auto& event = decoded->interaction.emplace();
    event.kind = InteractionKind::kPickupCollected;
    event.actor_handle = ReferenceHandleOf(collector).value_or(kInvalidEHandleIndex);
    Describe(pickup, event);
  }

  // Complete it once, then report the completion.
  g_pickup_hook->call<void>(pickup, collector);
  if (decoded && g_handler) {
    (*g_handler)(*decoded);
  }
}

void DecodeAndDispatch(uint32_t event, void* event_data) {
  if (g_handler == nullptr) {
    return;
  }
  CombatEvent decoded;
  if (event == g_ids.damage) {
    if (auto value = DecodeDamageTaken(event_data, g_damage_offsets, ReadNative)) {
      decoded.kind = CombatEvent::Kind::kDamageTaken;
      decoded.damage = *value;
    } else {
      decoded.reason = value.error();
    }
  } else if (event == g_ids.health) {
    if (auto value = DecodeHealthTaken(event_data, ReadNative)) {
      decoded.kind = CombatEvent::Kind::kHealthTaken;
      decoded.health = *value;
    } else {
      decoded.reason = value.error();
    }
  } else if (event == g_ids.ability) {
    if (auto value = DecodeAbilityExecuted(event_data, ReadNative)) {
      decoded.kind = CombatEvent::Kind::kAbilityExecuted;
      decoded.ability = *value;
    } else {
      decoded.reason = value.error();
    }
  } else {
    return;
  }
  (*g_handler)(decoded);
}

// GuardedHandleOf reads an entity's handle through guarded loads. A watched
// event's arguments are not known to be entities for every event, so a null,
// foreign or leaving one reads kInvalidEHandleIndex instead of faulting.
uint32_t GuardedHandleOf(void* entity) {
  // Read the identity, which must point back at the entity.
  const uint8_t* identity = nullptr;
  const void* owner = nullptr;
  if (entity == nullptr ||
      !ReadNative(static_cast<const uint8_t*>(entity) + kIdentityOffset, &identity,
                  sizeof(identity)) ||
      identity == nullptr || !ReadNative(identity, &owner, sizeof(owner)) || owner != entity) {
    return kInvalidEHandleIndex;
  }

  // A deleted or deleting entity has no handle.
  uint32_t stored = 0;
  uint32_t flags = 0;
  if (!ReadNative(identity + kIdentityEHandleOffset, &stored, sizeof(stored)) ||
      !ReadNative(identity + kIdentityFlagsOffset, &flags, sizeof(flags))) {
    return kInvalidEHandleIndex;
  }
  return LiveHandleOf(stored, flags).value_or(kInvalidEHandleIndex);
}

__int64 __fastcall FireModifierEventThunk(uint32_t event, void* caster, void* target,
                                          void* cast_entity, void* event_data) {
  if (event == g_ids.pre_damage &&
      ((g_suppress_damage && *g_suppress_damage) || (g_adjust_damage && *g_adjust_damage))) {
    auto processed = ProcessDamageContact(
        event_data, g_contact_offsets, ReadNative,
        [](void* target, const void* source, size_t size) {
          SIZE_T written = 0;
          return WriteProcessMemory(GetCurrentProcess(), target, source, size, &written) &&
                 written == size;
        },
        *g_suppress_damage, *g_adjust_damage);
    if (!processed && g_handler) {
      CombatEvent failure;
      failure.reason = processed.error();
      (*g_handler)(failure);
    }
  }
  // Decode before the call: the payload may reference stack storage the
  // native dispatch is entitled to reuse once its listeners return. Damage,
  // healing and ability execution use modifier events; their broadcast
  // duplicates stay undecoded so nothing counts twice.
  if (event == g_ids.damage || event == g_ids.health || event == g_ids.ability) {
    DecodeAndDispatch(event, event_data);
  }
  if (event == g_ids.prop_broken && g_handler && g_interactions) {
    DispatchPropBroken(target, event_data);
  }
  for (const auto& [id, execution] : g_ids.movement) {
    if (event != id) {
      continue;
    }
    if (g_handler) {
      if (const auto handle = ReferenceHandleOf(caster)) {
        CombatEvent decoded;
        decoded.kind = CombatEvent::Kind::kMovementExecuted;
        decoded.movement = MovementExecutedEvent{execution, *handle};
        (*g_handler)(decoded);
      }
    }
    break;
  }
  // Report a watched event with the handles of its three entities.
  if (event < g_watched.size() && g_watched[event].load(std::memory_order_relaxed) != 0 &&
      g_handler) {
    CombatEvent decoded;
    decoded.kind = CombatEvent::Kind::kModifierFired;
    decoded.modifier = ModifierFiredEvent{event, GuardedHandleOf(caster), GuardedHandleOf(target),
                                          GuardedHandleOf(cast_entity)};
    (*g_handler)(decoded);
  }
  // The native returns the aggregated modifier response; every argument is
  // forwarded unconditionally so observation never suppresses native events.
  return g_hook->call<__int64>(event, caster, target, cast_entity, event_data);
}

__int64 __fastcall BroadcastThunk(uint32_t event, void* event_data) {
  // Damage/heal broadcasts duplicate modifier events and are not observed.
  if (event == g_ids.shield_broadcast && g_handler) {
    CombatEvent decoded;
    if (auto value = DecodeShieldDamage(event_data, ReadNative)) {
      decoded.kind = CombatEvent::Kind::kShieldDamage;
      decoded.shield = *value;
    } else {
      decoded.reason = value.error();
    }
    (*g_handler)(decoded);
  }
  return g_broadcast_hook->call<__int64>(event, event_data);
}

#endif

}  // namespace

std::expected<void, std::string> ProcessDamageContact(
    void* event_data, const DamageContactOffsets& offsets, const BoundedReader& read,
    const BoundedWriter& write, const std::function<bool(const DamageContactEvent&)>& suppress,
    const AdjustDamage& adjust) {
  // Read the victim and the damage descriptor.
  if (!event_data || !read || !write || (!suppress && !adjust)) {
    return std::unexpected("combat contact: missing payload or handler");
  }
  auto* bytes = static_cast<unsigned char*>(event_data);
  void* info = nullptr;
  DamageContactEvent event;
  if (!read(bytes, &event.victim_handle, sizeof(event.victim_handle)) ||
      !read(bytes + 8, &info, sizeof(info)) || !info) {
    return std::unexpected("combat contact: unreadable native damage descriptor");
  }

  // Read the attacker, flags, hit group, amount, ability and inflictor.
  auto* damage = static_cast<unsigned char*>(info);
  if (!read(damage + offsets.attacker, &event.attacker_handle, sizeof(event.attacker_handle)) ||
      !read(damage + offsets.flags, &event.flags, sizeof(event.flags))) {
    return std::unexpected("combat contact: unreadable attacker or damage flags");
  }
  if (offsets.hit_group &&
      !read(damage + *offsets.hit_group, &event.hit_group, sizeof(event.hit_group))) {
    return std::unexpected("combat contact: unreadable damage hit group");
  }
  if (!read(damage + offsets.amount, &event.amount, sizeof(event.amount)) ||
      !read(damage + offsets.ability, &event.ability_handle, sizeof(event.ability_handle))) {
    return std::unexpected("combat contact: unreadable damage amount or ability");
  }
  if (offsets.inflictor &&
      !read(damage + *offsets.inflictor, &event.inflictor_handle, sizeof(event.inflictor_handle))) {
    return std::unexpected("combat contact: unreadable damage inflictor");
  }

  // Damage that is not suppressed may be adjusted in place.
  if (!suppress || !suppress(event)) {
    // Adjust the amount, writing back a valid change.
    if (!adjust) {
      return {};
    }
    float amount = event.amount;
    adjust(event, amount);
    if (amount == event.amount) {
      return {};
    }
    if (!std::isfinite(amount) || amount < 0) {
      return std::unexpected("combat contact: adjusted damage must be finite and nonnegative");
    }
    if (!write(damage + offsets.amount, &amount, sizeof(amount))) {
      return std::unexpected("combat contact: could not adjust native damage");
    }
    return {};
  }

  // Deadlock TakeDamageFlags: health, physics, effects, flinch, screenspace,
  // procs, damage flash and combat-state changes are suppressed together.
  constexpr uint64_t kSuppressed =
      0x1 | 0x2 | 0x4 | 0x1000 | 0x10000 | 0x80000000 | 0x20000000000ULL | 0x1000000000000000ULL;
  const uint64_t flags = event.flags | kSuppressed;
  if (!write(damage + offsets.flags, &flags, sizeof(flags))) {
    return std::unexpected("combat contact: could not suppress native damage");
  }
  return {};
}

std::expected<DamageTakenEvent, std::string> DecodeDamageTaken(const void* event_data,
                                                               const DamageResultOffsets& offsets,
                                                               const BoundedReader& read) {
  // Refuse a missing payload or reader.
  if (event_data == nullptr) {
    return std::unexpected(std::string("combat event: damage payload pointer is null"));
  }
  if (!read) {
    return std::unexpected(std::string("combat event: no bounded reader is configured"));
  }

  // Read the victim, attacker and ability.
  DamageTakenEvent event{};
  const auto* bytes = static_cast<const unsigned char*>(event_data);
  if (!read(bytes + kDamageVictimOffset, &event.victim_handle, sizeof(event.victim_handle))) {
    return std::unexpected(Unreadable(event_data, "the damage victim handle"));
  }
  if (!read(bytes + kDamageAttackerOffset, &event.attacker_handle, sizeof(event.attacker_handle))) {
    return std::unexpected(Unreadable(bytes + kDamageAttackerOffset, "the attacker handle"));
  }
  if (offsets.ability) {
    const unsigned char* info = nullptr;
    if (!read(bytes + 8, &info, sizeof(info)) || !info ||
        !read(info + *offsets.ability, &event.ability_handle, sizeof(event.ability_handle))) {
      return std::unexpected("combat event: unreadable damage ability");
    }
  }

  // Read the damage result's fields, rounding a float damage.
  const void* result = nullptr;
  if (!read(bytes + kDamageResultOffset, &result, sizeof(result))) {
    return std::unexpected(Unreadable(bytes + kDamageResultOffset, "the damage result pointer"));
  }
  if (result == nullptr) {
    return std::unexpected("combat event: the damage result pointer is null");
  }
  if (!read(static_cast<const unsigned char*>(result) + offsets.health_lost, &event.health_lost,
            sizeof(event.health_lost)) ||
      !read(static_cast<const unsigned char*>(result) + offsets.health_before, &event.health_before,
            sizeof(event.health_before))) {
    return std::unexpected(Unreadable(result, "the damage result fields"));
  }
  const auto* dealt = static_cast<const unsigned char*>(result) + offsets.damage_dealt;
  if (offsets.damage_dealt_float) {
    float value = 0;
    if (!read(dealt, &value, sizeof(value)) || !std::isfinite(value)) {
      return std::unexpected(Unreadable(result, "the damage result fields"));
    }
    event.damage_dealt = static_cast<int32_t>(std::lround(value));
  } else if (!read(dealt, &event.damage_dealt, sizeof(event.damage_dealt))) {
    return std::unexpected(Unreadable(result, "the damage result fields"));
  }

  // A negative value is refused.
  if (event.health_lost < 0 || event.health_before < 0 || event.damage_dealt < 0) {
    return std::unexpected("combat event: the damage result carries a negative value");
  }
  return event;
}

std::expected<HealthTakenEvent, std::string> DecodeHealthTaken(const void* event_data,
                                                               const BoundedReader& read) {
  // Refuse a missing payload or reader.
  if (event_data == nullptr) {
    return std::unexpected(std::string("combat event: heal payload pointer is null"));
  }
  if (!read) {
    return std::unexpected(std::string("combat event: no bounded reader is configured"));
  }

  // Read the heal, refusing a nonfinite request or negative heal.
  HealthTakenEvent event{};
  const auto* bytes = static_cast<const unsigned char*>(event_data);
  if (!read(bytes + kHealthEntityOffset, &event.entity_handle, sizeof(event.entity_handle)) ||
      !read(bytes + kHealthRequestedOffset, &event.requested, sizeof(event.requested)) ||
      !read(bytes + kHealthAppliedOffset, &event.applied, sizeof(event.applied))) {
    return std::unexpected(Unreadable(event_data, "the heal payload"));
  }
  if (!std::isfinite(event.requested)) {
    return std::unexpected("combat event: the heal request is nonfinite");
  }
  if (event.applied < 0) {
    return std::unexpected("combat event: the applied heal is negative");
  }
  return event;
}

std::expected<ShieldDamageEvent, std::string> DecodeShieldDamage(const void* event_data,
                                                                 const BoundedReader& read) {
  // Refuse a missing payload or reader.
  if (!event_data || !read) {
    return std::unexpected("combat event: shield payload unavailable");
  }

  // Read the attacker, victim and absorbed amount.
  ShieldDamageEvent event;
  const auto* bytes = static_cast<const unsigned char*>(event_data);

  // The native emitter subtracts absorbed from the shield after broadcasting.
  // The third handle (+8) identifies the shield caster, not the damaged pawn.
  if (!read(bytes, &event.attacker_handle, sizeof(event.attacker_handle)) ||
      !read(bytes + 4, &event.victim_handle, sizeof(event.victim_handle)) ||
      !read(bytes + 12, &event.absorbed, sizeof(event.absorbed))) {
    return std::unexpected(Unreadable(event_data, "the shield payload"));
  }
  if (!std::isfinite(event.absorbed) || event.absorbed < 0) {
    return std::unexpected("combat event: invalid shield absorption");
  }
  return event;
}

std::expected<AbilityExecutedEvent, std::string> DecodeAbilityExecuted(const void* event_data,
                                                                       const BoundedReader& read) {
  // Refuse a missing payload or reader.
  if (!event_data) {
    return std::unexpected("combat event: ability payload pointer is null");
  }
  if (!read) {
    return std::unexpected("combat event: no bounded reader is configured");
  }

  // Read the caster, ability and target.
  AbilityExecutedEvent event;
  const auto* bytes = static_cast<const unsigned char*>(event_data);
  if (!read(bytes + kAbilityCasterOffset, &event.caster_handle, sizeof(event.caster_handle)) ||
      !read(bytes + kAbilityHandleOffset, &event.ability_handle, sizeof(event.ability_handle)) ||
      !read(bytes + kAbilityTargetOffset, &event.target_handle, sizeof(event.target_handle))) {
    return std::unexpected(Unreadable(event_data, "the ability payload"));
  }
  return event;
}

std::expected<void, std::string> DescribeInteractionObject(const void* object,
                                                           const InteractionOffsets& offsets,
                                                           const BoundedReader& read,
                                                           InteractionEvent& event) {
  // Refuse a missing object or reader.
  if (!object || !read) {
    return std::unexpected("interaction: object or reader unavailable");
  }
  const auto* bytes = static_cast<const unsigned char*>(object);
  if (!read(bytes + offsets.subclass, &event.subclass_id, sizeof(event.subclass_id))) {
    return std::unexpected(Unreadable(object, "the object's subclass"));
  }

  // A pickup flying to this actor stood where its vacuum began.
  if (event.kind == InteractionKind::kPickupCollected) {
    uint32_t target = 0;
    float started = 0;
    if (!read(bytes + offsets.vacuum_target, &target, sizeof(target)) ||
        !read(bytes + offsets.vacuum_start_time, &started, sizeof(started))) {
      return std::unexpected(Unreadable(object, "the pickup's vacuum"));
    }
    if (target == event.actor_handle && started > 0 && std::isfinite(started)) {
      if (!read(bytes + offsets.vacuum_start_position, event.origin.data(), sizeof(event.origin))) {
        return std::unexpected(Unreadable(object, "the pickup's vacuum start"));
      }
      event.vacuum_start = started;
      return {};
    }
  }

  // Any other object stands at its absolute origin.
  const void* body = nullptr;
  const void* node = nullptr;
  if (!read(bytes + offsets.body, &body, sizeof(body)) || !body ||
      !read(static_cast<const unsigned char*>(body) + offsets.scene_node, &node, sizeof(node)) ||
      !node ||
      !read(static_cast<const unsigned char*>(node) + offsets.origin, event.origin.data(),
            sizeof(event.origin))) {
    return std::unexpected(Unreadable(object, "the object's origin"));
  }
  return {};
}

std::expected<InteractionEvent, std::string> DecodePropBroken(const void* event_data,
                                                              const BoundedReader& read) {
  // Refuse a missing payload or reader.
  if (!event_data || !read) {
    return std::unexpected("combat event: prop break payload unavailable");
  }

  // Read the prop, inflictor and breaker.
  InteractionEvent event;
  event.kind = InteractionKind::kPropBroken;
  const auto* bytes = static_cast<const unsigned char*>(event_data);
  if (!read(bytes + kBrokenPropOffset, &event.object_handle, sizeof(event.object_handle)) ||
      !read(bytes + kBrokenInflictorOffset, &event.inflictor_handle,
            sizeof(event.inflictor_handle)) ||
      !read(bytes + kBrokenBreakerOffset, &event.actor_handle, sizeof(event.actor_handle))) {
    return std::unexpected(Unreadable(event_data, "the prop break payload"));
  }
  return event;
}

struct CombatEventsHook::Impl {
  Handler handler;
  std::function<bool(const DamageContactEvent&)> suppress_damage;
  AdjustDamage adjust_damage;
#if defined(_WIN32)
  safetyhook::InlineHook hook;
  safetyhook::InlineHook broadcast;
  safetyhook::InlineHook pickup;
  ObservedInteractions observed;

  ~Impl() {
    if (g_hook == &hook) {
      // Remove the hooks.
      pickup.reset();
      broadcast.reset();
      hook.reset();

      // Clear the thunk state.
      g_pickup_hook = nullptr;
      g_interactions.reset();
      g_broadcast_hook = nullptr;
      g_hook = nullptr;
      g_handler = nullptr;
      g_damage_offsets = DamageResultOffsets{};
      g_contact_offsets = DamageContactOffsets{};
      g_suppress_damage = nullptr;
      g_adjust_damage = nullptr;
      for (auto& watched : g_watched) {
        watched.store(0, std::memory_order_relaxed);
      }
    }
  }
#endif
};

CombatEventsHook::CombatEventsHook(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

CombatEventsHook::CombatEventsHook(CombatEventsHook&&) noexcept = default;
CombatEventsHook& CombatEventsHook::operator=(CombatEventsHook&&) noexcept = default;
CombatEventsHook::~CombatEventsHook() = default;

std::expected<void, std::string> CombatEventsHook::Watch(uint32_t id) {
#if defined(_WIN32)
  if (id >= g_watched.size()) {
    return std::unexpected("modifier event " + std::to_string(id) + " is out of range");
  }
  g_watched[id].fetch_add(1, std::memory_order_relaxed);
  return {};
#else
  static_cast<void>(id);
  return std::unexpected("modifier events require a Windows game server");
#endif
}

void CombatEventsHook::Unwatch(uint32_t id) {
#if defined(_WIN32)
  if (id < g_watched.size() && g_watched[id].load(std::memory_order_relaxed) != 0) {
    g_watched[id].fetch_sub(1, std::memory_order_relaxed);
  }
#else
  static_cast<void>(id);
#endif
}

ObservedInteractions CombatEventsHook::Observed() const {
#if defined(_WIN32)
  return impl_ ? impl_->observed : ObservedInteractions{};
#else
  return {};
#endif
}

std::expected<uint32_t, std::string> ModifierEventIndex(const ModuleImage& image,
                                                        std::string_view name) {
  auto value = EnumeratorValue(image, name, kMaxModifierEvent);
  if (!value) {
    return std::unexpected("modifier event " + value.error());
  }
  return *value;
}

std::expected<CombatEventsHook, std::string> CombatEventsHook::Install(
    const ModuleImage& server, void* schema_system, Handler handler,
    std::function<bool(const DamageContactEvent&)> suppress_damage, AdjustDamage adjust_damage) {
#if defined(_WIN32)
  // Install once, and find both hook targets.
  if (g_hook != nullptr || !handler) {
    return std::unexpected("combat events hook is already installed or handler absent");
  }
  auto target = ResolveSignature(server, "combat.fire-modifier-event");
  if (!target) {
    return std::unexpected(target.error());
  }
  auto broadcast = ResolveSignature(server, "combat.broadcast");
  if (!broadcast) {
    return std::unexpected(broadcast.error());
  }

  // Game updates renumber EModifierEvent, so every id is read by name from
  // the running server module. A name the game no longer has disables only
  // that event.
  const auto resolve = [&server](std::string_view name) {
    auto id = ModifierEventIndex(server, name);
    if (id) {
      return *id;
    }
    std::fprintf(stderr, "[modlock] combat events: %s; it is not observed\n", id.error().c_str());
    return kUnobservedEvent;
  };
  EventIds ids;
  ids.pre_damage = resolve("MODIFIER_EVENT_PRE_DAMAGE_TAKEN");
  ids.damage = resolve("MODIFIER_EVENT_DAMAGE_TAKEN");
  ids.health = resolve("MODIFIER_EVENT_HEALTH_TAKEN");
  ids.ability = resolve("MODIFIER_EVENT_ABILITY_EXECUTED");
  ids.shield_broadcast = resolve("MODIFIER_EVENT_UNIT_SHIELD_ABSORBED_DAMAGE_BROADCAST");
  ids.prop_broken = resolve("MODIFIER_EVENT_BREAKABLE_PROP_BROKEN");
  for (size_t i = 0; i < kMovementEventNames.size(); ++i) {
    ids.movement[i] = {resolve(kMovementEventNames[i].first), kMovementEventNames[i].second};
  }

  // The result fields are schema-resolved at runtime; no offset is invented.
  DamageResultOffsets offsets{};
  const char* names[] = {"m_nHealthLost", "m_nHealthBefore", "m_nDamageDealt"};
  size_t* slots[] = {&offsets.health_lost, &offsets.health_before, &offsets.damage_dealt};
  for (size_t i = 0; i < std::size(names); ++i) {
    // Each field must hold an int; dealt damage may instead be a float.
    auto field = SchemaFieldOf(schema_system, "server.dll", "CTakeDamageResult", names[i]);
    if (!field && slots[i] == &offsets.damage_dealt) {
      field = SchemaFieldOf(schema_system, "server.dll", "CTakeDamageResult",
                            "m_flTotalledDamageDealt");
      offsets.damage_dealt_float = field.has_value();
    }
    if (!field) {
      return std::unexpected(field.error());
    }
    if (field->size < sizeof(int32_t)) {
      return std::unexpected(std::string("combat event: CTakeDamageResult field storage too "
                                         "short: ") +
                             names[i]);
    }
    *slots[i] = field->offset;
  }

  // Create the hook, disabled.
  auto created =
      safetyhook::create_inline(*target, reinterpret_cast<void*>(&FireModifierEventThunk),
                                safetyhook::InlineHook::StartDisabled);
  if (!created) {
    return std::unexpected("inline hook on FireModifierEvent failed");
  }

  // Find the damage info fields the contact handlers need.
  auto impl = std::make_unique<Impl>();
  DamageContactOffsets contact;
  if (suppress_damage || adjust_damage) {
    const auto attacker =
        SchemaFieldOf(schema_system, "server.dll", "CTakeDamageInfo", "m_hAttacker");
    const auto flags =
        SchemaFieldOf(schema_system, "server.dll", "CTakeDamageInfo", "m_nDamageFlags");
    if (!attacker || !flags || attacker->size < sizeof(uint32_t) ||
        flags->size < sizeof(uint64_t)) {
      return std::unexpected("combat contact: native attacker or flags schema unavailable");
    }
    contact = {attacker->offset, flags->offset};
  }
  if (adjust_damage) {
    // Find the damage info's amount, ability and inflictor.
    const auto amount = SchemaFieldOf(schema_system, "server.dll", "CTakeDamageInfo", "m_flDamage");
    const auto ability =
        SchemaFieldOf(schema_system, "server.dll", "CTakeDamageInfo", "m_hAbility");
    const auto inflictor =
        SchemaFieldOf(schema_system, "server.dll", "CTakeDamageInfo", "m_hInflictor");
    if (!amount || !ability || !inflictor || amount->size < sizeof(float) ||
        ability->size < sizeof(uint32_t) || inflictor->size < sizeof(uint32_t)) {
      return std::unexpected("combat contact: native damage source schema unavailable");
    }
    contact.amount = amount->offset;
    contact.ability = ability->offset;
    contact.inflictor = inflictor->offset;

    // Then its hit group.
    const auto hit_group =
        SchemaFieldOf(schema_system, "server.dll", "CTakeDamageInfo", "m_iHitGroupId");
    if (!hit_group || hit_group->size < sizeof(int32_t)) {
      return std::unexpected("combat contact: native hit group schema unavailable");
    }
    contact.hit_group = hit_group->offset;
    offsets.ability = ability->offset;
  }

  // Create the broadcast hook, disabled.
  impl->hook = std::move(created);
  impl->broadcast = safetyhook::create_inline(*broadcast, reinterpret_cast<void*>(&BroadcastThunk),
                                              safetyhook::InlineHook::StartDisabled);
  if (!impl->broadcast) {
    return std::unexpected("combat broadcast hook could not be created");
  }

  // Interactions need the objects' fields and, for pickups, the completion;
  // either missing disables only that feature.
  InteractionOffsets interaction;
  const SchemaOffset interaction_fields[] = {
      {"CBaseEntity", "m_nSubclassID", sizeof(uint32_t), &interaction.subclass},
      {"CBaseEntity", "m_CBodyComponent", sizeof(void*), &interaction.body},
      {"CBodyComponent", "m_pSceneNode", sizeof(void*), &interaction.scene_node},
      {"CGameSceneNode", "m_vecAbsOrigin", sizeof(float) * 3, &interaction.origin},
      {"CCitadel_Pickup", "m_vVacuumStartPos", sizeof(float) * 3,
       &interaction.vacuum_start_position},
      {"CCitadel_Pickup", "m_flVacuumStartTime", sizeof(float), &interaction.vacuum_start_time},
      {"CCitadel_Pickup", "m_hVacuumTarget", sizeof(uint32_t), &interaction.vacuum_target},
  };
  if (auto fields = ResolveSchemaOffsets(schema_system, "server.dll", interaction_fields);
      !fields) {
    std::fprintf(stderr, "[modlock] combat events: %s; interactions are not observed\n",
                 fields.error().c_str());
  } else {
    g_interactions = interaction;
    impl->observed.breaks = ids.prop_broken != kUnobservedEvent;
    if (auto complete = ResolveSignature(server, "pickup.complete"); !complete) {
      std::fprintf(stderr, "[modlock] combat events: %s; pickups are not observed\n",
                   complete.error().c_str());
    } else {
      impl->pickup = safetyhook::create_inline(*complete, reinterpret_cast<void*>(&PickupThunk),
                                               safetyhook::InlineHook::StartDisabled);
      impl->observed.pickups = static_cast<bool>(impl->pickup);
    }
  }

  // Publish the ids, hooks and offsets to the thunks.
  g_ids = ids;
  g_pickup_hook = impl->pickup ? &impl->pickup : nullptr;
  g_hook = &impl->hook;
  g_broadcast_hook = &impl->broadcast;
  g_damage_offsets = offsets;
  g_contact_offsets = contact;

  // Keep the handlers and publish them.
  impl->suppress_damage = std::move(suppress_damage);
  g_suppress_damage = &impl->suppress_damage;
  impl->adjust_damage = std::move(adjust_damage);
  g_adjust_damage = &impl->adjust_damage;
  impl->handler = std::move(handler);
  g_handler = &impl->handler;

  // Enable both hooks.
  if (auto enabled = impl->hook.enable(); !enabled) {
    return std::unexpected("FireModifierEvent hook could not be enabled");
  }
  if (auto enabled = impl->broadcast.enable(); !enabled) {
    return std::unexpected("combat broadcast hook could not be enabled");
  }
  if (impl->pickup && !impl->pickup.enable()) {
    std::fprintf(stderr,
                 "[modlock] combat events: pickup completion hook could not be "
                 "enabled; pickups are not observed\n");
    impl->observed.pickups = false;
  }
  return CombatEventsHook(std::move(impl));

#else
  // Other platforms have no hook.
  (void)server;
  (void)schema_system;
  (void)handler;
  (void)suppress_damage;
  (void)adjust_damage;
  return std::unexpected("the combat events hook requires the Windows host build");
#endif
}

}  // namespace modlock::gameinterop
