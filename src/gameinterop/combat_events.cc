#include "modlock/gameinterop/combat_events.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <optional>
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

// OsBoundedReader is the production reader: it copies through the OS the same
// way client_command_hook.cc reads engine-owned memory, so a bad pointer
// returns false instead of faulting the process.
bool OsBoundedReader(const void* source, void* out, size_t size) {
#if defined(_WIN32)
  SIZE_T read = 0;
  return ReadProcessMemory(GetCurrentProcess(), source, out, size, &read) && read == size;
#else
  (void)source;
  (void)out;
  (void)size;
  return false;
#endif
}

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
struct EventIds {
  uint32_t pre_damage = 0;
  uint32_t damage = 0;
  uint32_t health = 0;
  uint32_t ability = 0;
  uint32_t shield_broadcast = 0;
  std::array<std::pair<uint32_t, MovementExecution>, kMovementEventNames.size()> movement{};
};
EventIds g_ids;

CombatEventsHook::Handler* g_handler = nullptr;
DamageResultOffsets g_damage_offsets;
DamageContactOffsets g_contact_offsets;
std::function<bool(const DamageContactEvent&)>* g_suppress_damage = nullptr;
AdjustDamage* g_adjust_damage = nullptr;
safetyhook::InlineHook* g_hook = nullptr;
safetyhook::InlineHook* g_broadcast_hook = nullptr;

void DecodeAndDispatch(uint32_t event, void* event_data) {
  if (g_handler == nullptr) return;
  CombatEvent decoded;
  if (event == g_ids.damage) {
    if (auto value = DecodeDamageTaken(event_data, g_damage_offsets, OsBoundedReader)) {
      decoded.kind = CombatEvent::Kind::kDamageTaken;
      decoded.damage = *value;
    } else {
      decoded.reason = value.error();
    }
  } else if (event == g_ids.health) {
    if (auto value = DecodeHealthTaken(event_data, OsBoundedReader)) {
      decoded.kind = CombatEvent::Kind::kHealthTaken;
      decoded.health = *value;
    } else {
      decoded.reason = value.error();
    }
  } else if (event == g_ids.ability) {
    if (auto value = DecodeAbilityExecuted(event_data, OsBoundedReader)) {
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

__int64 __fastcall FireModifierEventThunk(uint32_t event, void* caster, void* target,
                                          void* cast_entity, void* event_data) {
  if (event == g_ids.pre_damage &&
      ((g_suppress_damage && *g_suppress_damage) || (g_adjust_damage && *g_adjust_damage))) {
    auto processed = ProcessDamageContact(
        event_data, g_contact_offsets, OsBoundedReader,
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
  for (const auto& [id, execution] : g_ids.movement) {
    if (event != id) continue;
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
  // The native returns the aggregated modifier response; every argument is
  // forwarded unconditionally so observation never suppresses native events.
  return g_hook->call<__int64>(event, caster, target, cast_entity, event_data);
}

__int64 __fastcall BroadcastThunk(uint32_t event, void* event_data) {
  // Damage/heal broadcasts duplicate modifier events and are not observed.
  if (event == g_ids.shield_broadcast && g_handler) {
    CombatEvent decoded;
    if (auto value = DecodeShieldDamage(event_data, OsBoundedReader)) {
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
  if (!event_data || !read || !write || (!suppress && !adjust))
    return std::unexpected("combat contact: missing payload or handler");
  auto* bytes = static_cast<unsigned char*>(event_data);
  void* info = nullptr;
  DamageContactEvent event;
  if (!read(bytes, &event.victim_handle, sizeof(event.victim_handle)) ||
      !read(bytes + 8, &info, sizeof(info)) || !info)
    return std::unexpected("combat contact: unreadable native damage descriptor");
  auto* damage = static_cast<unsigned char*>(info);
  if (!read(damage + offsets.attacker, &event.attacker_handle, sizeof(event.attacker_handle)) ||
      !read(damage + offsets.flags, &event.flags, sizeof(event.flags)))
    return std::unexpected("combat contact: unreadable attacker or damage flags");
  if (offsets.hit_group &&
      !read(damage + *offsets.hit_group, &event.hit_group, sizeof(event.hit_group)))
    return std::unexpected("combat contact: unreadable damage hit group");
  if (!suppress || !suppress(event)) {
    if (!adjust) return {};
    float amount = 0;
    if (!read(damage + offsets.amount, &amount, sizeof(amount)) ||
        !read(damage + offsets.ability, &event.ability_handle, sizeof(event.ability_handle)))
      return std::unexpected("combat contact: unreadable damage amount or ability");
    if (offsets.inflictor &&
        !read(damage + *offsets.inflictor, &event.inflictor_handle, sizeof(event.inflictor_handle)))
      return std::unexpected("combat contact: unreadable damage inflictor");
    const float original = amount;
    adjust(event, amount);
    if (amount == original) return {};
    if (!std::isfinite(amount) || amount < 0)
      return std::unexpected("combat contact: adjusted damage must be finite and nonnegative");
    if (!write(damage + offsets.amount, &amount, sizeof(amount)))
      return std::unexpected("combat contact: could not adjust native damage");
    return {};
  }

  // Deadlock TakeDamageFlags: health, physics, effects, flinch, screenspace,
  // procs, damage flash and combat-state changes are suppressed together.
  constexpr uint64_t kSuppressed =
      0x1 | 0x2 | 0x4 | 0x1000 | 0x10000 | 0x80000000 | 0x20000000000ULL | 0x1000000000000000ULL;
  const uint64_t flags = event.flags | kSuppressed;
  if (!write(damage + offsets.flags, &flags, sizeof(flags)))
    return std::unexpected("combat contact: could not suppress native damage");
  return {};
}

std::expected<DamageTakenEvent, std::string> DecodeDamageTaken(const void* event_data,
                                                               const DamageResultOffsets& offsets,
                                                               const BoundedReader& read) {
  if (event_data == nullptr) {
    return std::unexpected(std::string("combat event: damage payload pointer is null"));
  }
  if (!read) {
    return std::unexpected(std::string("combat event: no bounded reader is configured"));
  }
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
        !read(info + *offsets.ability, &event.ability_handle, sizeof(event.ability_handle)))
      return std::unexpected("combat event: unreadable damage ability");
  }
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
    if (!read(dealt, &value, sizeof(value)) || !std::isfinite(value))
      return std::unexpected(Unreadable(result, "the damage result fields"));
    event.damage_dealt = static_cast<int32_t>(std::lround(value));
  } else if (!read(dealt, &event.damage_dealt, sizeof(event.damage_dealt))) {
    return std::unexpected(Unreadable(result, "the damage result fields"));
  }
  if (event.health_lost < 0 || event.health_before < 0 || event.damage_dealt < 0) {
    return std::unexpected("combat event: the damage result carries a negative value");
  }
  return event;
}

std::expected<HealthTakenEvent, std::string> DecodeHealthTaken(const void* event_data,
                                                               const BoundedReader& read) {
  if (event_data == nullptr) {
    return std::unexpected(std::string("combat event: heal payload pointer is null"));
  }
  if (!read) {
    return std::unexpected(std::string("combat event: no bounded reader is configured"));
  }
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
  if (!event_data || !read) return std::unexpected("combat event: shield payload unavailable");
  ShieldDamageEvent event;
  const auto* bytes = static_cast<const unsigned char*>(event_data);
  // The native emitter subtracts absorbed from the shield after broadcasting.
  // The third handle (+8) identifies the shield caster, not the damaged pawn.
  if (!read(bytes, &event.attacker_handle, sizeof(event.attacker_handle)) ||
      !read(bytes + 4, &event.victim_handle, sizeof(event.victim_handle)) ||
      !read(bytes + 12, &event.absorbed, sizeof(event.absorbed)))
    return std::unexpected(Unreadable(event_data, "the shield payload"));
  if (!std::isfinite(event.absorbed) || event.absorbed < 0)
    return std::unexpected("combat event: invalid shield absorption");
  return event;
}

std::expected<AbilityExecutedEvent, std::string> DecodeAbilityExecuted(const void* event_data,
                                                                       const BoundedReader& read) {
  if (!event_data) return std::unexpected("combat event: ability payload pointer is null");
  if (!read) return std::unexpected("combat event: no bounded reader is configured");

  AbilityExecutedEvent event;
  const auto* bytes = static_cast<const unsigned char*>(event_data);
  if (!read(bytes + kAbilityCasterOffset, &event.caster_handle, sizeof(event.caster_handle)) ||
      !read(bytes + kAbilityHandleOffset, &event.ability_handle, sizeof(event.ability_handle)) ||
      !read(bytes + kAbilityTargetOffset, &event.target_handle, sizeof(event.target_handle))) {
    return std::unexpected(Unreadable(event_data, "the ability payload"));
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

  ~Impl() {
    if (g_hook == &hook) {
      broadcast.reset();
      hook.reset();
      g_broadcast_hook = nullptr;
      g_hook = nullptr;
      g_handler = nullptr;
      g_damage_offsets = DamageResultOffsets{};
      g_contact_offsets = DamageContactOffsets{};
      g_suppress_damage = nullptr;
      g_adjust_damage = nullptr;
    }
  }
#endif
};

CombatEventsHook::CombatEventsHook(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
CombatEventsHook::CombatEventsHook(CombatEventsHook&&) noexcept = default;
CombatEventsHook& CombatEventsHook::operator=(CombatEventsHook&&) noexcept = default;
CombatEventsHook::~CombatEventsHook() = default;

std::expected<uint32_t, std::string> ModifierEventIndex(const ModuleImage& image,
                                                       std::string_view name) {
  const auto bytes = image.image_bytes();
  const std::string_view view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  // The name is a whole NUL-terminated string, not a prefix of a longer one.
  std::string needle;
  needle.reserve(name.size() + 2);
  needle.push_back('\0');
  needle.append(name);
  needle.push_back('\0');
  const auto at = view.find(needle);
  if (at == std::string_view::npos)
    return std::unexpected("modifier event " + std::string(name) + " is not in the game module");
  const uint64_t address = static_cast<uint64_t>(image.base()) + at + 1;
  char pointer[sizeof(address)];
  std::memcpy(pointer, &address, sizeof(address));
  const std::string_view pointer_view(pointer, sizeof(pointer));

  // Every enumerator record naming this string must agree on one value.
  std::optional<int64_t> value;
  for (auto hit = view.find(pointer_view); hit != std::string_view::npos;
       hit = view.find(pointer_view, hit + 1)) {
    if (hit % alignof(uint64_t) != 0 || hit + 2 * sizeof(uint64_t) > view.size()) continue;
    int64_t candidate = 0;
    std::memcpy(&candidate, view.data() + hit + sizeof(uint64_t), sizeof(candidate));
    if (candidate < 0 || candidate >= kMaxModifierEvent) continue;
    if (value && *value != candidate)
      return std::unexpected("modifier event " + std::string(name) + " has conflicting values");
    value = candidate;
  }
  if (!value)
    return std::unexpected("modifier event " + std::string(name) + " has no enumerator record");
  return static_cast<uint32_t>(*value);
}

std::expected<CombatEventsHook, std::string> CombatEventsHook::Install(
    const ModuleImage& server, void* schema_system, Handler handler,
    std::function<bool(const DamageContactEvent&)> suppress_damage, AdjustDamage adjust_damage) {
#if defined(_WIN32)
  if (g_hook != nullptr || !handler) {
    return std::unexpected("combat events hook is already installed or handler absent");
  }
  auto target = ResolveSignature(server, "combat.fire-modifier-event");
  if (!target) return std::unexpected(target.error());
  auto broadcast = ResolveSignature(server, "combat.broadcast");
  if (!broadcast) return std::unexpected(broadcast.error());

  // Game updates renumber EModifierEvent, so every id is read by name from
  // the running server module.
  EventIds ids;
  const std::pair<const char*, uint32_t*> named[] = {
      {"MODIFIER_EVENT_PRE_DAMAGE_TAKEN", &ids.pre_damage},
      {"MODIFIER_EVENT_DAMAGE_TAKEN", &ids.damage},
      {"MODIFIER_EVENT_HEALTH_TAKEN", &ids.health},
      {"MODIFIER_EVENT_ABILITY_EXECUTED", &ids.ability},
      {"MODIFIER_EVENT_UNIT_SHIELD_ABSORBED_DAMAGE_BROADCAST", &ids.shield_broadcast},
  };
  for (const auto& [name, slot] : named) {
    auto id = ModifierEventIndex(server, name);
    if (!id) return std::unexpected(id.error());
    *slot = *id;
  }
  for (size_t i = 0; i < kMovementEventNames.size(); ++i) {
    auto id = ModifierEventIndex(server, kMovementEventNames[i].first);
    if (!id) return std::unexpected(id.error());
    ids.movement[i] = {*id, kMovementEventNames[i].second};
  }

  // The result fields are schema-resolved at runtime; no offset is invented.
  DamageResultOffsets offsets{};
  const char* names[] = {"m_nHealthLost", "m_nHealthBefore", "m_nDamageDealt"};
  size_t* slots[] = {&offsets.health_lost, &offsets.health_before, &offsets.damage_dealt};
  for (size_t i = 0; i < std::size(names); ++i) {
    auto field = SchemaFieldOf(schema_system, "server.dll", "CTakeDamageResult", names[i]);
    if (!field && slots[i] == &offsets.damage_dealt) {
      field = SchemaFieldOf(schema_system, "server.dll", "CTakeDamageResult",
                            "m_flTotalledDamageDealt");
      offsets.damage_dealt_float = field.has_value();
    }
    if (!field) return std::unexpected(field.error());
    if (field->size < sizeof(int32_t)) {
      return std::unexpected(std::string("combat event: CTakeDamageResult field storage too "
                                         "short: ") +
                             names[i]);
    }
    *slots[i] = field->offset;
  }

  auto created =
      safetyhook::create_inline(*target, reinterpret_cast<void*>(&FireModifierEventThunk),
                                safetyhook::InlineHook::StartDisabled);
  if (!created) {
    return std::unexpected("inline hook on FireModifierEvent failed");
  }
  auto impl = std::make_unique<Impl>();
  DamageContactOffsets contact;
  if (suppress_damage || adjust_damage) {
    const auto attacker =
        SchemaFieldOf(schema_system, "server.dll", "CTakeDamageInfo", "m_hAttacker");
    const auto flags =
        SchemaFieldOf(schema_system, "server.dll", "CTakeDamageInfo", "m_nDamageFlags");
    if (!attacker || !flags || attacker->size < sizeof(uint32_t) || flags->size < sizeof(uint64_t))
      return std::unexpected("combat contact: native attacker or flags schema unavailable");
    contact = {attacker->offset, flags->offset};
  }
  if (adjust_damage) {
    const auto amount = SchemaFieldOf(schema_system, "server.dll", "CTakeDamageInfo", "m_flDamage");
    const auto ability =
        SchemaFieldOf(schema_system, "server.dll", "CTakeDamageInfo", "m_hAbility");
    const auto inflictor =
        SchemaFieldOf(schema_system, "server.dll", "CTakeDamageInfo", "m_hInflictor");
    if (!amount || !ability || !inflictor || amount->size < sizeof(float) ||
        ability->size < sizeof(uint32_t) || inflictor->size < sizeof(uint32_t))
      return std::unexpected("combat contact: native damage source schema unavailable");
    contact.amount = amount->offset;
    contact.ability = ability->offset;
    contact.inflictor = inflictor->offset;
    const auto hit_group =
        SchemaFieldOf(schema_system, "server.dll", "CTakeDamageInfo", "m_iHitGroupId");
    if (!hit_group || hit_group->size < sizeof(int32_t))
      return std::unexpected("combat contact: native hit group schema unavailable");
    contact.hit_group = hit_group->offset;
    offsets.ability = ability->offset;
  }
  impl->hook = std::move(created);
  impl->broadcast = safetyhook::create_inline(*broadcast, reinterpret_cast<void*>(&BroadcastThunk),
                                              safetyhook::InlineHook::StartDisabled);
  if (!impl->broadcast) return std::unexpected("combat broadcast hook could not be created");
  g_ids = ids;
  g_hook = &impl->hook;
  g_broadcast_hook = &impl->broadcast;
  g_damage_offsets = offsets;
  g_contact_offsets = contact;
  impl->suppress_damage = std::move(suppress_damage);
  g_suppress_damage = &impl->suppress_damage;
  impl->adjust_damage = std::move(adjust_damage);
  g_adjust_damage = &impl->adjust_damage;
  impl->handler = std::move(handler);
  g_handler = &impl->handler;
  if (auto enabled = impl->hook.enable(); !enabled) {
    return std::unexpected("FireModifierEvent hook could not be enabled");
  }
  if (auto enabled = impl->broadcast.enable(); !enabled)
    return std::unexpected("combat broadcast hook could not be enabled");
  return CombatEventsHook(std::move(impl));
#else
  (void)server;
  (void)schema_system;
  (void)handler;
  (void)suppress_damage;
  (void)adjust_damage;
  return std::unexpected("the combat events hook requires the Windows host build");
#endif
}

}  // namespace modlock::gameinterop
