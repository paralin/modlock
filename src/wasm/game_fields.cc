//go:build deps_only

#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "modlock/gameinterop/native_memory.h"
#include "modlock/gameinterop/virtual_slots.h"
#include "wasm/entity_fields.h"
#include "wasm/game.h"
#include "wasm/game_internal.h"
#include "wasm/modifier_masks.h"
#include "wasm/text_values.h"

namespace modlock::wasm {

namespace {

// ModifierName copies the name of the modifier at modifier: its definition,
// at +0x10 as in Deadworks CBaseModifier, keeps its interned name at +0x10, as
// every CEntitySubclassVDataBase does. An unreadable name is empty.
std::string ModifierName(const unsigned char* modifier) {
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

std::expected<std::vector<unsigned char*>, std::string> Game::EntityModifiers(void* entity) {
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

std::expected<fields::FieldResponse, std::string> Game::ReadModifierField(
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

std::expected<abilities::ActiveResponse, std::string> Game::ModifierState(
    const abilities::ModifierStateRequest& request) {
  auto active = ReadModifierState(services_, request.entity(), request.state());
  if (!active) {
    return std::unexpected(active.error());
  }
  abilities::ActiveResponse response;
  response.set_active(*active);
  return response;
}

std::expected<abilities::ModifiersResponse, std::string> Game::Modifiers(
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
    auto field = [&]<typename T>(const Read& read, T fallback) {
      T value = fallback;
      if (read.found && !gameinterop::ReadNative(modifier + read.offset, &value, sizeof(value))) {
        value = fallback;
      }
      return value;
    };
    auto* entry = response.add_modifiers();
    entry->set_name(ModifierName(modifier));
    entry->set_class_name(gameinterop::ObjectClassName(modifier));
    const auto caster_handle = field(caster, uint32_t{0xFFFFFFFFu});
    const auto ability_handle = field(ability, uint32_t{0xFFFFFFFFu});
    entry->set_caster(caster_handle == 0xFFFFFFFFu ? 0 : caster_handle);
    entry->set_ability(ability_handle == 0xFFFFFFFFu ? 0 : ability_handle);
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

std::expected<void, std::string> Game::HoldModifierState(
    const abilities::HoldModifierStateRequest& request) {
  // Releasing a hold clears the state the hold set.
  auto hold = std::pair(request.entity(), request.state());
  if (request.has_active() && !request.active()) {
    if (held_.erase(hold) == 0) {
      return {};
    }
    return SetModifierState(services_, request.entity(), request.state(), false);
  }

  // Holding sets the state now and every frame after.
  if (auto set = SetModifierState(services_, request.entity(), request.state(), true); !set) {
    return set;
  }
  held_.insert(std::move(hold));
  return {};
}

void Game::StartFreeForAll(bool enabled) {
  free_for_all_ = enabled;
  team_seating_ = false;
  if (free_for_all_) {
    EnableRelations();
  }
}

void Game::EnableRelations() {
  // Without the relations only bullets reach teammates; the match still runs.
  if (auto relations = services_.Relations()) {
    (*relations)->SetEnabled(true);
  } else {
    std::cerr << "free-for-all relations: " << relations.error() << '\n';
  }
}

std::expected<void, std::string> Game::SetFreeForAll(const wasm::SetFreeForAllRequest& request) {
  if (free_for_all_ == request.enabled()) {
    return {};
  }
  free_for_all_ = request.enabled();
  if (free_for_all_) {
    EnableRelations();
  } else {
    ReleaseFreeForAll();
  }
  return {};
}

std::expected<void, std::string> Game::SetTeamSeating(const wasm::SetTeamSeatingRequest& request) {
  team_seating_ = request.enabled();
  return {};
}

bool Game::FreeForAllCommand(std::string_view line) const {
  // Only changeteam is blocked, and only in free-for-all.
  if (!free_for_all_) {
    return false;
  }
  const auto name = line.substr(0, line.find_first_of(" \t"));
  return name == "changeteam";
}

void Game::SeatPlayers() {
  SeatAssignedPlayers();

  // Seat players only while seating or free-for-all is on.
  if (!free_for_all_ && !team_seating_) {
    return;
  }
  auto selection = services_.Selection();
  if (selection) {
    // Seat each player with no team on the side with fewer players.
    std::array<int, 4> sides{};
    std::vector<int32_t> unseated;
    for (int32_t slot = 0; slot < gameinterop::kMaxPlayerSlots; ++slot) {
      // Count each side and collect the humans without a team.
      const auto state = gameinterop::ConnectionTracker::StateForSlot(slot);
      if (!state.occupied || !Serves(slot)) {
        continue;
      }
      const auto team = players_.Observer().ControllerTeam(slot);
      if (!team) {
        continue;
      }
      if (*team == 0 && !state.is_bot && state.xuid != 0 && !assignments_.Contains(state.xuid)) {
        unseated.push_back(slot);
      } else if (*team < sides.size()) {
        ++sides[*team];
      }
    }
    for (const auto slot : unseated) {
      const auto state = gameinterop::ConnectionTracker::StateForSlot(slot);
      const int team = sides[3] < sides[2] ? 3 : 2;
      auto seated =
          players_.Observer().SeatForChoice(slot, state.xuid, state.generation, team, **selection);
      if (seated && *seated) {
        ++sides[team];
        seat_failed_.erase(slot);
      } else if (!seated && seat_failed_.insert(slot).second) {
        std::cerr << "team seating: " << seated.error() << '\n';
      }
    }
  } else if (seat_failed_.insert(-1).second) {
    std::cerr << "team seating: " << selection.error() << '\n';
  }
}

void Game::ApplyFreeForAll() {
  // Hold friendly fire only in free-for-all.
  if (!free_for_all_) {
    return;
  }
  static constexpr const char* kFriendlyFire = "MODIFIER_STATE_FRIENDLY_FIRE_ENABLED";

  // Hold friendly fire on each living hero, and let a gone hero's hold go.
  std::set<uint32_t> living;
  for (const auto& hero : players_.Heroes()) {
    living.insert(hero.handle);
    if (ffa_held_.insert(hero.handle).second) {
      held_.insert({hero.handle, kFriendlyFire});
    }
  }
  std::erase_if(ffa_held_, [&](uint32_t handle) {
    if (living.contains(handle)) {
      return false;
    }
    held_.erase({handle, kFriendlyFire});
    return true;
  });
}

void Game::ReleaseFreeForAll() {
  if (services_.relations_) {
    services_.relations_->SetEnabled(false);
  }
  static constexpr const char* kFriendlyFire = "MODIFIER_STATE_FRIENDLY_FIRE_ENABLED";
  for (const auto handle : ffa_held_) {
    if (held_.erase({handle, kFriendlyFire}) != 0) {
      static_cast<void>(SetModifierState(services_, handle, kFriendlyFire, false));
    }
  }
  ffa_held_.clear();
  seat_failed_.clear();
}

}  // namespace modlock::wasm
