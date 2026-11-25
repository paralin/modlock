//go:build deps_only

#include "wasm/modifier_holds.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <optional>

#include "modlock/gameinterop/entity_abi.h"
#include "modlock/gameinterop/native_memory.h"
#include "modlock/gameinterop/virtual_slots.h"
#include "wasm/entity_fields.h"
#include "wasm/game.h"
#include "wasm/game_internal.h"
#include "wasm/modifier_masks.h"

namespace modlock::wasm {

namespace {

// ModifierName copies the name of the modifier at modifier: its definition,
// at +0x10 as in Deadworks CBaseModifier, keeps its interned name at +0x10, as
// every CEntitySubclassVDataBase does. An unreadable name is empty.
std::string ModifierName(const unsigned char* modifier) {
  // Follow the definition and its interned name only while both are readable.
  const unsigned char* definition = nullptr;
  const char* interned = nullptr;
  if (!gameinterop::ReadNative(modifier + 0x10, &definition, sizeof(definition)) || !definition ||
      !gameinterop::ReadNative(definition + 0x10, &interned, sizeof(interned)) || !interned) {
    return {};
  }
  std::array<char, 128> name{};
  if (!gameinterop::ReadNative(interned, name.data(), name.size() - 1)) {
    return {};
  }
  return name.data();
}

}  // namespace

void ModifierHolds::KeepState(uint32_t handle, const std::string& state) {
  states_.insert({handle, state});
}

void ModifierHolds::ForgetState(uint32_t handle, const std::string& state) {
  states_.erase({handle, state});
}

void ModifierHolds::ReleaseState(uint32_t handle, const std::string& state) {
  if (states_.erase({handle, state}) != 0) {
    static_cast<void>(SetModifierState(services_, handle, state, false));
  }
}

void ModifierHolds::WorldReady() {
  // The layout is kept once found; one that fails here is reported by the
  // call that needs it.
  static_cast<void>(services_.Modifiers());
}

void ModifierHolds::Frame() {
  Apply();
}

void ModifierHolds::WorldEnding() {
  // The world's entities go with it, so nothing needs releasing on them.
  states_.clear();
  ReleaseModifiers();
}

void ModifierHolds::Leave(int32_t slot) {
  // Release the modifiers the slot held.
  std::vector<std::string> keys;
  for (const auto& [held, key] : held_slots_) {
    if (held == slot) {
      keys.push_back(key);
    }
  }
  for (const auto& key : keys) {
    ReleaseModifier(slot, key);
  }
}

void ModifierHolds::Clear() {
  // Release held modifier states.
  for (const auto& [entity, state] : states_) {
    static_cast<void>(SetModifierState(services_, entity, state, false));
  }
  states_.clear();

  // Remove each held definition from its target. The native property
  // owns callbacks and deferred deletion; restore durations only afterwards.
  bool removal_logged = false;
  auto remove = [this, &removal_logged](void* entity, const std::string& key) {
    const auto modifier = modifiers_.find(key);
    if (!entity || modifier == modifiers_.end()) {
      return;
    }
    if (auto removed = modifier->second.RemoveFromEntity(entity); !removed && !removal_logged) {
      std::fprintf(stderr, "[modlock] held modifier cleanup: %s\n", removed.error().c_str());
      removal_logged = true;
    }
  };
  for (const auto& [slot, key] : held_slots_) {
    remove(players_.Observer().PawnForSlot(slot), key);
  }
  for (const auto& [entity, key] : held_entities_) {
    if (auto live = LiveEntity(entity)) {
      remove(*live, key);
    }
  }
  ReleaseModifiers();
}

void ModifierHolds::Apply() {
  // Set each held state again and drop the ones that no longer apply.
  std::erase_if(states_, [this](const auto& hold) {
    return !SetModifierState(services_, hold.first, hold.second, true);
  });
  for (const auto& [slot, key] : held_slots_) {
    static_cast<void>(RenewModifier(slot, key));
  }

  // Renew each entity's modifiers and release those of an entity that is gone.
  std::vector<std::pair<uint32_t, std::string>> gone;
  for (const auto& [entity, key] : held_entities_) {
    if (!LiveEntity(entity)) {
      gone.emplace_back(entity, key);
      continue;
    }
    static_cast<void>(RenewEntityModifier(entity, key, false));
  }
  for (const auto& [entity, key] : gone) {
    ReleaseEntityModifier(entity, key);
  }
}

std::expected<void, std::string> ModifierHolds::RenewModifier(int32_t slot,
                                                              const std::string& key) {
  // Find the modifier the slot holds.
  const auto modifier = modifiers_.find(key);
  if (modifier == modifiers_.end()) {
    return std::unexpected("the modifier is not held");
  }

  // A dead hero has no modifiers to renew.
  const auto sample = players_.Observer().Observe(slot);
  if (!sample || sample->health <= 0) {
    return {};
  }

  // Attach the modifier again if the game removed it.
  auto attached = modifier->second.Attached(players_.Observer(), slot);
  if (!attached) {
    return std::unexpected(attached.error());
  }
  if (*attached) {
    return {};
  }
  auto source = ModifierSource(slot, key);
  if (!source) {
    return std::unexpected(source.error());
  }
  return modifier->second.Attach(players_.Observer(), slot, *source);
}

std::expected<void, std::string> ModifierHolds::RenewEntityModifier(uint32_t handle,
                                                                    const std::string& key,
                                                                    bool initial) {
  // Find the held modifier and the live entity.
  const auto modifier = modifiers_.find(key);
  if (modifier == modifiers_.end()) {
    return std::unexpected("the modifier is not held");
  }
  auto entity = LiveEntity(handle);
  if (!entity) {
    return std::unexpected(entity.error());
  }
  if (!modifier->second.EntityAlive(*entity)) {
    if (initial) {
      return std::unexpected("the entity is not alive");
    }
    return {};
  }

  // Attach the modifier again if the game removed it.
  auto attached = modifier->second.AttachedToEntity(*entity);
  if (!attached) {
    return std::unexpected(attached.error());
  }
  if (*attached) {
    return {};
  }
  return modifier->second.AttachEntity(*entity, EntityAbilityHandle(handle, key));
}

void ModifierHolds::ReleaseModifier(int32_t slot, const std::string& key) {
  if (held_slots_.erase({slot, key}) != 0) {
    RestoreUnusedModifier(key);
  }
}

void ModifierHolds::ReleaseEntityModifier(uint32_t handle, const std::string& key) {
  if (held_entities_.erase({handle, key}) != 0) {
    RestoreUnusedModifier(key);
  }
}

void ModifierHolds::RestoreUnusedModifier(const std::string& key) {
  if (std::ranges::any_of(held_slots_, [&](const auto& hold) { return hold.second == key; }) ||
      std::ranges::any_of(held_entities_, [&](const auto& hold) { return hold.second == key; })) {
    return;
  }
  if (auto modifier = modifiers_.find(key); modifier != modifiers_.end()) {
    modifier->second.Restore();
    modifiers_.erase(modifier);
  }
}

void ModifierHolds::ReleaseModifiers() {
  for (const auto& [key, modifier] : modifiers_) {
    modifier.Restore();
  }
  modifiers_.clear();
  held_slots_.clear();
  held_entities_.clear();
}

std::expected<void, std::string> ModifierHolds::EnsureHeldModifier(const std::string& key) {
  // Share the definition duration across this mod's holds.
  if (modifiers_.contains(key)) {
    return {};
  }
  auto calls = services_.ModifierCalls();
  if (!calls) {
    return std::unexpected(calls.error());
  }
  if (!(*calls)->remove) {
    return std::unexpected("held modifiers require native modifier removal");
  }
  auto modifier = gameinterop::AbilityModifier::Resolve(**calls, key, -1);
  if (!modifier) {
    return std::unexpected(modifier.error());
  }
  modifiers_.emplace(key, *modifier);
  return {};
}

uint32_t ModifierHolds::EntityAbilityHandle(uint32_t handle, std::string_view key) {
  // Resolve a source only for an embedded modifier on a player's hero.
  const auto slash = key.find('/');
  const auto slot = players_.SlotOfPawn(handle);
  if (slash == std::string_view::npos || !slot) {
    return gameinterop::kInvalidEHandleIndex;
  }
  const auto owned = players_.Observer().CurrentAbilitiesForSlot(*slot);
  if (!owned) {
    return gameinterop::kInvalidEHandleIndex;
  }
  const auto named = gameinterop::MakeMemberName(key.substr(0, slash)).hash;
  const auto found =
      std::ranges::find(*owned, named, &gameinterop::PawnObserver::Ability::subclass_id);
  return found == owned->end() ? gameinterop::kInvalidEHandleIndex : found->handle;
}

std::expected<void, std::string> ModifierHolds::HoldModifier(
    const abilities::HoldModifierRequest& request) {
  // Releasing a hold drops the slot's claim on the modifier.
  const auto& key = request.modifier();
  if (request.has_active() && !request.active()) {
    ReleaseModifier(request.player(), key);
    return {};
  }

  // Resolve the modifier once for every slot or entity that holds it.
  if (auto resolved = EnsureHeldModifier(key); !resolved) {
    return resolved;
  }
  held_slots_.emplace(request.player(), key);

  // A hold that cannot start now ends, so the mod learns why.
  auto renewed = RenewModifier(request.player(), key);
  if (!renewed) {
    ReleaseModifier(request.player(), key);
  }
  return renewed;
}

std::expected<void, std::string> ModifierHolds::GiveModifier(
    const abilities::GiveModifierRequest& request) {
  // Resolve the modifier calls and the ability that applies it.
  auto calls = services_.ModifierCalls();
  if (!calls) {
    return std::unexpected(calls.error());
  }
  if (auto pawn = players_.LivePawn(request.player()); !pawn) {
    return std::unexpected(pawn.error());
  }
  auto source = ModifierSource(request.player(), request.modifier());
  if (!source) {
    return std::unexpected(source.error());
  }

  // The modifier takes its duration when attached, so the shared definition
  // gets its own duration back right away.
  auto modifier =
      gameinterop::AbilityModifier::Resolve(**calls, request.modifier(), request.seconds());
  if (!modifier) {
    return std::unexpected(modifier.error());
  }
  auto attached = modifier->Attach(players_.Observer(), request.player(), *source);
  modifier->Restore();
  return attached;
}

std::expected<void, std::string> ModifierHolds::HoldEntityModifier(
    const abilities::HoldEntityModifierRequest& request) {
  // Releasing a hold drops the entity's claim on the modifier.
  const auto& key = request.modifier();
  if (request.has_active() && !request.active()) {
    ReleaseEntityModifier(request.entity(), key);
    return {};
  }

  // Check the entity before resolving or mutating the shared definition.
  if (auto entity = LiveEntity(request.entity()); !entity) {
    return std::unexpected(entity.error());
  }
  if (auto resolved = EnsureHeldModifier(key); !resolved) {
    return resolved;
  }
  held_entities_.emplace(request.entity(), key);

  // A hold that cannot start now ends, so the mod learns why.
  auto renewed = RenewEntityModifier(request.entity(), key, true);
  if (!renewed) {
    ReleaseEntityModifier(request.entity(), key);
  }
  return renewed;
}

std::expected<void, std::string> ModifierHolds::GiveEntityModifier(
    const abilities::GiveEntityModifierRequest& request) {
  // Resolve the modifier calls and the target entity.
  auto calls = services_.ModifierCalls();
  if (!calls) {
    return std::unexpected(calls.error());
  }
  auto entity = LiveEntity(request.entity());
  if (!entity) {
    return std::unexpected(entity.error());
  }

  // The modifier takes its duration when attached, so the shared definition
  // gets its own duration back right away.
  auto modifier =
      gameinterop::AbilityModifier::Resolve(**calls, request.modifier(), request.seconds());
  if (!modifier) {
    return std::unexpected(modifier.error());
  }
  auto attached =
      modifier->AttachEntity(*entity, EntityAbilityHandle(request.entity(), request.modifier()));
  modifier->Restore();
  return attached;
}

std::expected<uint32_t, std::string> ModifierHolds::ModifierSource(int32_t slot,
                                                                   std::string_view key) {
  // Read the hero's owned abilities for the modifier's source.
  auto owned = players_.Observer().CurrentAbilitiesForSlot(slot);
  if (!owned) {
    return std::unexpected(owned.error());
  }

  // The modifier's own ability applies it when the hero owns that ability.
  if (const auto slash = key.find('/'); slash != std::string_view::npos) {
    const auto named = gameinterop::MakeMemberName(key.substr(0, slash)).hash;
    if (std::ranges::find(*owned, named, &gameinterop::PawnObserver::Ability::subclass_id) !=
        owned->end()) {
      return named;
    }
  }

  // Otherwise the hero's first ability does.
  const auto first = std::ranges::find(*owned, 0, &gameinterop::PawnObserver::Ability::slot);
  if (first == owned->end()) {
    return std::unexpected("the hero has no ability to apply the modifier");
  }
  return first->subclass_id;
}

std::expected<std::vector<unsigned char*>, std::string> ModifierHolds::EntityModifiers(
    void* entity) {
  // Find the entity's modifier property; an entity without one holds none.
  auto layout = services_.Modifiers();
  if (!layout) {
    return std::unexpected(layout.error());
  }
  const unsigned char* property = nullptr;
  if (!gameinterop::ReadNative(static_cast<unsigned char*>(entity) + layout->property, &property,
                               sizeof(property))) {
    return std::unexpected("cannot read the entity's modifiers");
  }
  if (!property) {
    return std::vector<unsigned char*>{};
  }

  // Copy the list's pointers and drop the empty slots.
  auto list = ReadList(property + layout->list);
  if (!list) {
    return std::unexpected(list.error());
  }
  std::vector<unsigned char*> modifiers(static_cast<size_t>(list->size));
  if (!modifiers.empty() && !gameinterop::ReadNative(list->elements, modifiers.data(),
                                                     modifiers.size() * sizeof(unsigned char*))) {
    return std::unexpected("cannot read the entity's modifiers");
  }
  std::erase(modifiers, nullptr);
  return modifiers;
}

std::expected<fields::FieldResponse, std::string> ModifierHolds::ReadModifierField(
    const fields::ReadModifierFieldRequest& request) {
  // Find the entity, the field and the entity's modifier list.
  auto entity = LiveEntity(request.entity());
  if (!entity) {
    return std::unexpected(entity.error());
  }
  if (Size(request.type()) == 0) {
    return std::unexpected("the field type is unknown");
  }
  auto found = services_.Field(request.class_name(), request.field());
  if (!found) {
    return std::unexpected(found.error());
  }
  if (auto holds = Holds(*found, request.type(), false, request.class_name(), request.field());
      !holds) {
    return std::unexpected(holds.error());
  }
  auto modifiers = EntityModifiers(*entity);
  if (!modifiers) {
    return std::unexpected(modifiers.error());
  }

  // Read the field of the first modifier with the name; without one, answer
  // no value.
  for (auto* modifier : *modifiers) {
    if (ModifierName(modifier) == request.modifier()) {
      return ReadValue(modifier + found->offset, request.type(),
                       request.modifier() + "." + request.field());
    }
  }
  return fields::FieldResponse{};
}

std::expected<abilities::ActiveResponse, std::string> ModifierHolds::ModifierState(
    const abilities::ModifierStateRequest& request) {
  // Read the native mask without creating a hold.
  auto active = ReadModifierState(services_, request.entity(), request.state());
  if (!active) {
    return std::unexpected(active.error());
  }
  abilities::ActiveResponse response;
  response.set_active(*active);
  return response;
}

std::expected<abilities::ModifiersResponse, std::string> ModifierHolds::Modifiers(
    const entities::EntityRequest& request) {
  // Find the entity, its modifiers and the clock their times count on.
  auto entity = LiveEntity(request.entity());
  if (!entity) {
    return std::unexpected(entity.error());
  }
  auto modifiers = EntityModifiers(*entity);
  if (!modifiers) {
    return std::unexpected(modifiers.error());
  }
  std::optional<float> now;
  if (auto server = services_.Server()) {
    if (auto clock = (*server)->ReadClock()) {
      now = clock->current_time;
    }
  }

  // Find each CBaseModifier field the answer reads once.
  struct Read {
    const char* field;
    size_t offset = 0;
    bool found = false;
  };

  // Request only the fields returned by the response.
  std::array<Read, 9> reads{{{"m_hCaster"},
                             {"m_hAbility"},
                             {"m_flCreationTime"},
                             {"m_flLastAppliedTime"},
                             {"m_flDuration"},
                             {"m_iStackCount"},
                             {"m_iMaxStackCount"},
                             {"m_iTeam"},
                             {"m_bDisabled"}}};
  for (auto& read : reads) {
    if (auto found = services_.Field("CBaseModifier", read.field)) {
      read.offset = found->offset;
      read.found = true;
    }
  }
  auto [caster, ability, created, applied, duration, stacks, max_stacks, team, disabled] = reads;

  // Read what each modifier keeps; a field the schema or memory denies stays
  // at its default, so one unreadable field still lists the modifier.
  abilities::ModifiersResponse response;
  for (auto* modifier : *modifiers) {
    // Read each optional schema field through a bounded native read.
    auto field = [&]<typename T>(const Read& read, T fallback) {
      T value = fallback;
      if (read.found && !gameinterop::ReadNative(modifier + read.offset, &value, sizeof(value))) {
        value = fallback;
      }
      return value;
    };

    // Identify this modifier and its source entities.
    auto* entry = response.add_modifiers();
    entry->set_name(ModifierName(modifier));
    entry->set_class_name(gameinterop::ObjectClassName(modifier));
    const auto caster_handle = field(caster, uint32_t{0xFFFFFFFFu});
    const auto ability_handle = field(ability, uint32_t{0xFFFFFFFFu});
    entry->set_caster(caster_handle == 0xFFFFFFFFu ? 0 : caster_handle);
    entry->set_ability(ability_handle == 0xFFFFFFFFu ? 0 : ability_handle);

    // Copy its state and timing into the response.
    entry->set_stacks(field(stacks, int16_t{0}));
    entry->set_max_stacks(field(max_stacks, int16_t{0}));
    entry->set_team(field(team, uint8_t{0}));
    entry->set_disabled(field(disabled, uint8_t{0}) != 0);

    // Times count on the server's simulation clock: a duration runs from the
    // last application, and a negative one lasts until removed.
    if (now) {
      entry->set_age(*now - field(created, *now));
      if (const auto seconds = field(duration, -1.0f); seconds >= 0) {
        entry->set_remaining(std::max(0.0f, field(applied, *now) + seconds - *now));
      }
    }
  }
  return response;
}

std::expected<void, std::string> ModifierHolds::HoldModifierState(
    const abilities::HoldModifierStateRequest& request) {
  // Releasing a hold clears the state the hold set.
  auto hold = std::pair(request.entity(), request.state());
  if (request.has_active() && !request.active()) {
    if (states_.erase(hold) == 0) {
      return {};
    }
    return SetModifierState(services_, request.entity(), request.state(), false);
  }

  // Holding sets the state now and every frame after.
  if (auto set = SetModifierState(services_, request.entity(), request.state(), true); !set) {
    return set;
  }
  states_.insert(std::move(hold));
  return {};
}

}  // namespace modlock::wasm
