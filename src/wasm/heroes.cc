//go:build deps_only

#include "wasm/heroes.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "modlock/gameinterop/console_variables.h"
#include "modlock/gameinterop/entity_abi.h"
#include "modlock/gameinterop/keyvalues.h"
#include "modlock/gameinterop/native_memory.h"
#include "modlock/gameinterop/native_user_messages.h"
#include "wasm/game.h"
#include "wasm/game_internal.h"

namespace modlock::wasm {

namespace {

// SteadySeconds is the time on a clock that never pauses or steps, in seconds.
double SteadySeconds() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace

void Heroes::WorldEnding() {
  frozen_.clear();
}

void Heroes::Leave(int32_t slot) {
  frozen_.erase(slot);
  picks_.Forget(slot);
}

void Heroes::Clear() {
  // Unfreeze the frozen heroes.
  for (const auto slot : frozen_) {
    if (players_.Observer().Observe(slot)) {
      static_cast<void>(players_.Observer().SetPreparationFrozen(slot, false));
    }
  }
  frozen_.clear();

  // Return every ability's cooldown lengths and most charges to the game's
  // own.
  cooldown_.reset();
  charge_limit_.reset();
}

void Heroes::ClientCommand(int32_t slot, std::string_view line) {
  // Read the hero a selecthero pick names, as the client sends it.
  constexpr std::string_view kSelectHero = "selecthero ";
  if (!line.starts_with(kSelectHero)) {
    return;
  }
  auto name = line.substr(kSelectHero.size());
  while (!name.empty() && (name.front() == ' ' || name.front() == '"')) {
    name.remove_prefix(1);
  }
  while (!name.empty() && (name.back() == ' ' || name.back() == '"')) {
    name.remove_suffix(1);
  }

  // Note the pick for the player's connection.
  auto heroes = services_.Heroes();
  if (!heroes) {
    return;
  }
  const auto hero = (*heroes)->Find(name);
  const auto state = gameinterop::ConnectionTracker::StateForSlot(slot);
  if (!hero || hero->id <= 0 || !state.occupied || state.is_bot) {
    return;
  }
  picks_.Picked(slot, state.generation, static_cast<uint32_t>(hero->id), SteadySeconds());
}

std::expected<players::HeroResponse, std::string> Heroes::SelectHero(
    const players::SelectHeroRequest& request) {
  // Resolve the hero and the selection service.
  auto heroes = services_.Heroes();
  if (!heroes) {
    return std::unexpected(heroes.error());
  }
  auto hero = FindHero(**heroes, request);
  if (!hero) {
    return std::unexpected(hero.error());
  }
  auto selection = services_.Selection();
  if (!selection) {
    return std::unexpected(selection.error());
  }

  // Select the hero for the player in that slot.
  const auto state = gameinterop::ConnectionTracker::StateForSlot(request.player());
  if (!state.occupied) {
    return std::unexpected("no player is in that slot");
  }
  if (state.is_bot) {
    return std::unexpected("a bot keeps the hero AddBot gave it");
  }

  // The game's Change Hero removes the player's hero before it spawns the
  // one they picked. Their pick wins over a seat asked for in that gap.
  const auto live = players_.Live(request.player());
  const uint32_t current = live ? live->hero_id : 0;
  if (const auto arriving =
          picks_.Arriving(request.player(), state.generation, current, SteadySeconds())) {
    if (*arriving != static_cast<uint32_t>(hero->id)) {
      return std::unexpected("the player is changing to a hero they picked");
    }
    players::HeroResponse response;
    response.set_hero(*arriving);
    return response;
  }
  auto selected = players_.Observer().SelectPlayer(request.player(), state.xuid, state.generation,
                                                   request.team(), hero->native_definition_pointer,
                                                   **selection);
  if (!selected) {
    return std::unexpected(selected.error());
  }

  // Answer the hero selected.
  players::HeroResponse response;
  response.set_hero(static_cast<uint32_t>(hero->id));
  return response;
}

std::expected<void, std::string> Heroes::Respawn(const players::PlayerRequest& request) {
  // Resolve the respawn call and the player's live pawn.
  auto respawn = services_.Respawn();
  if (!respawn) {
    return std::unexpected(respawn.error());
  }
  auto sample = players_.Live(request.player());
  if (!sample) {
    return std::unexpected(sample.error());
  }
  return players_.Observer().RespawnPlayer(request.player(), sample->session_generation, *respawn);
}

std::expected<void, std::string> Heroes::ChangeTeam(const players::ChangeTeamRequest& request) {
  // Resolve the team change and the player's connection.
  auto change_team = services_.ChangeTeam();
  if (!change_team) {
    return std::unexpected(change_team.error());
  }
  const auto state = gameinterop::ConnectionTracker::StateForSlot(request.player());
  if (!state.occupied) {
    return std::unexpected("no player is in that slot");
  }
  return players_.Observer().MovePlayer(request.player(), state.generation, request.team(),
                                        *change_team);
}

std::expected<void, std::string> Heroes::ClearItems(const players::PlayerRequest& request) {
  // Resolve the item tables and the player's live pawn.
  auto abilities = services_.Abilities();
  if (!abilities) {
    return std::unexpected(abilities.error());
  }
  auto items = services_.Items();
  if (!items) {
    return std::unexpected(items.error());
  }
  if (auto pawn = players_.LivePawn(request.player()); !pawn) {
    return std::unexpected(pawn.error());
  }
  return players_.Observer().ReconcileItems(request.player(), {}, **abilities, **items);
}

std::expected<void, std::string> Heroes::Freeze(const players::FreezeRequest& request) {
  // Check the player's hero is live, then freeze or release it.
  const auto slot = request.player();
  const bool frozen = !request.has_frozen() || request.frozen();
  if (auto pawn = players_.LivePawn(slot); !pawn) {
    return std::unexpected(pawn.error());
  }
  if (auto done = players_.Observer().SetPreparationFrozen(slot, frozen); !done) {
    return done;
  }

  // Remember the frozen heroes, so Clear can release them.
  if (frozen) {
    frozen_.insert(slot);
  } else {
    frozen_.erase(slot);
  }
  return {};
}

std::expected<void, std::string> Heroes::RestoreStamina(const players::PlayerRequest& request) {
  auto now = LatchTime();
  if (!now) {
    return std::unexpected(now.error());
  }
  if (auto pawn = players_.LivePawn(request.player()); !pawn) {
    return std::unexpected(pawn.error());
  }
  return players_.Observer().SetPracticeStamina(request.player(), *now);
}

std::expected<void, std::string> Heroes::SetStamina(const players::SetStaminaRequest& request) {
  auto now = LatchTime();
  if (!now) {
    return std::unexpected(now.error());
  }
  if (auto pawn = players_.LivePawn(request.player()); !pawn) {
    return std::unexpected(pawn.error());
  }
  return players_.Observer().SetPracticeStamina(request.player(), *now, request.stamina());
}

std::expected<float, std::string> Heroes::LatchTime() {
  // The simulation time keeps running through a pause, so a latch written
  // with it after a pause sits in the future of a clock without the paused
  // ticks, and the hero shows empty stamina until that clock catches up. A
  // latch without the paused ticks fills the hero under either clock. Without
  // game rules no pause has happened, and the two clocks agree.
  auto server = services_.Server();
  if (!server) {
    return std::unexpected(server.error());
  }

  // Read the server clock.
  auto now = (*server)->ReadClock();
  if (!now) {
    return std::unexpected(now.error());
  }

  // Without a match clock, the server time is the game time.
  auto clock = services_.Clock();
  if (!clock) {
    return now->current_time;
  }
  return (*clock)->GameTime(now->current_time, now->interval).value_or(now->current_time);
}

std::expected<void, std::string> Heroes::RefreshAbility(
    const abilities::RefreshAbilityRequest& request) {
  // Find the player's ability and clear its cooldown.
  auto ability = OwnedAbility(request.player(), request.ability());
  if (!ability) {
    return std::unexpected(ability.error());
  }
  ability->cooldown_start = 0;
  ability->cooldown_end = 0;
  return players_.Observer().ApplyAbilityTimers(request.player(), std::span(&*ability, 1));
}

abilities::AbilityCastEvent Heroes::Cast(uint32_t caster, uint32_t ability, uint32_t target) {
  // Name the caster, its player and the target.
  abilities::AbilityCastEvent event;
  const auto slot = players_.Observer().SlotOfPawn(caster);
  event.set_caster(caster);
  event.set_player(slot.value_or(-1));
  event.set_ability(ability);
  event.set_slot(-1);
  event.set_target(target == gameinterop::kInvalidEHandleIndex ? 0 : target);

  // Name a hero's ability and its place on the hero.
  if (!slot) {
    return event;
  }
  const auto owned = players_.Observer().CurrentAbilitiesForSlot(*slot);
  if (!owned) {
    return event;
  }
  const auto definitions = services_.Abilities();
  for (const auto& candidate : *owned) {
    if (candidate.handle != ability) {
      continue;
    }
    event.set_slot(candidate.slot);
    if (definitions) {
      if (auto definition = (*definitions)->Find(candidate.subclass_id)) {
        event.set_name(definition->name);
      }
    }
    break;
  }
  return event;
}

std::expected<abilities::AbilitiesResponse, std::string> Heroes::Abilities(
    const players::PlayerRequest& request) {
  // Resolve the ability table and the player's live abilities.
  const auto slot = request.player();
  auto definitions = services_.Abilities();
  if (!definitions) {
    return std::unexpected(definitions.error());
  }
  if (auto pawn = players_.LivePawn(slot); !pawn) {
    return std::unexpected(pawn.error());
  }
  auto abilities = players_.Observer().CurrentAbilitiesForSlot(slot);
  if (!abilities) {
    return std::unexpected(abilities.error());
  }

  // Answer each ability's name, slot and timers.
  abilities::AbilitiesResponse response;
  for (const auto& ability : *abilities) {
    // Name the ability, its slot and its entity.
    auto* out = response.add_abilities();
    if (auto definition = (*definitions)->Find(ability.subclass_id)) {
      out->set_name(definition->name);
    }
    out->set_slot(ability.slot);
    out->set_entity(ability.handle);

    // Then its upgrades, charges and timers.
    out->set_upgrades(ability.upgrade_info >> 16);
    out->set_charges(ability.charges);
    out->set_cooldown_end(ability.cooldown_end);
    out->set_id(ability.subclass_id);
    out->set_state(ability.upgrade_info);
    out->set_cooldown_start(ability.cooldown_start);
    out->set_recharge_start(ability.charge_recharge_start);
    out->set_recharge_end(ability.charge_recharge_end);

    // An imbuing item names the abilities it strengthens.
    for (const auto token : ImbuedAbilities(slot, ability.handle)) {
      if (auto definition = (*definitions)->Find(token)) {
        out->add_imbues(definition->name);
      }
    }
  }
  return response;
}

std::vector<uint32_t> Heroes::ImbuedAbilities(int32_t slot, uint32_t handle) {
  // Find the ability's list of imbued abilities.
  auto* ability = static_cast<unsigned char*>(players_.Observer().AbilityInstance(slot, handle));
  auto field = services_.Field("CCitadelBaseAbility", "m_vecImbuedAbilities");
  if (ability == nullptr || !field) {
    return {};
  }
  auto list = ReadList(ability + field->offset);
  if (!list || list->size == 0) {
    return {};
  }

  // Copy their definition ids.
  std::vector<uint32_t> tokens(list->size);
  if (!gameinterop::ReadNative(list->elements, tokens.data(), tokens.size() * sizeof(uint32_t))) {
    return {};
  }
  return tokens;
}

std::expected<void, std::string> Heroes::SetAbility(const abilities::AbilityOptions& request) {
  // Find the player's ability.
  auto ability = OwnedAbility(request.player(), request.ability());
  if (!ability) {
    return std::unexpected(ability.error());
  }

  // Apply the requested upgrades.
  if (request.has_upgrades()) {
    auto items = services_.Items();
    if (!items) {
      return std::unexpected(items.error());
    }
    const gameinterop::AbilityUpgrade upgrade{
        .subclass_id = ability->subclass_id,
        .slot = ability->slot,
        .upgrade_info = (ability->upgrade_info & 0xffff) | (request.upgrades() << 16),
    };
    if (auto applied = players_.Observer().ApplyAbilityUpgrades(
            request.player(), std::span(&upgrade, 1), (*items)->set_bits);
        !applied) {
      return applied;
    }
  }

  // Limit the ability's charges.
  if (request.has_max_charges()) {
    if (auto limited = LimitCharges(request.player(), ability->handle, request.max_charges());
        !limited) {
      return limited;
    }
  }

  // Give its cooldowns the length asked for.
  if (request.has_cooldown()) {
    if (auto set = SetCooldownLength(request.player(), ability->handle, request.cooldown()); !set) {
      return set;
    }
  }

  // Stop when no timer was asked for.
  if (!request.has_charges() && !request.has_cooldown_end() && !request.has_recharge_end()) {
    return {};
  }

  // An upgrade can change the charges, so set them on the upgraded ability.
  ability = OwnedAbility(request.player(), request.ability());
  if (!ability) {
    return std::unexpected(ability.error());
  }
  if (request.has_charges()) {
    ability->charges = request.charges();
  }
  if (request.has_cooldown_end()) {
    ability->cooldown_end = request.cooldown_end();
  }
  if (request.has_recharge_end()) {
    ability->charge_recharge_end = request.recharge_end();
  }
  return players_.Observer().ApplyAbilityTimers(request.player(), std::span(&*ability, 1));
}

std::expected<void, std::string> Heroes::SetCooldownLength(int32_t slot, uint32_t handle,
                                                           float seconds) {
  // Create the cooldown lengths once from the server module.
  if (!cooldown_) {
    auto image = services_.Image();
    if (!image) {
      return std::unexpected(image.error());
    }
    auto query = gameinterop::ResolveAbilityCooldownSlot(**image);
    if (!query) {
      return std::unexpected(query.error());
    }
    auto created = gameinterop::AbilityCooldown::Create(*query);
    if (!created) {
      return std::unexpected(created.error());
    }
    cooldown_ = std::move(*created);
  }

  // Give the live ability the length, or the game's own for a negative one.
  auto* ability = players_.Observer().AbilityInstance(slot, handle);
  if (ability == nullptr) {
    return std::unexpected("the ability is not live");
  }
  return cooldown_->Set(ability, handle, seconds < 0 ? std::nullopt : std::optional(seconds));
}

std::expected<void, std::string> Heroes::LimitCharges(int32_t slot, uint32_t handle,
                                                      int32_t maximum) {
  // Create the charge limit once from the server module.
  if (!charge_limit_) {
    // Create the charge limit once.
    auto image = services_.Image();
    if (!image) {
      return std::unexpected(image.error());
    }
    auto slots = gameinterop::ResolveAbilityChargeSlots(**image);
    if (!slots) {
      return std::unexpected(slots.error());
    }
    auto created = gameinterop::AbilityChargeLimit::Create(slots->max_charges);
    if (!created) {
      return std::unexpected(created.error());
    }
    charge_limit_ = std::move(*created);
  }

  // Limit the live ability.
  auto* ability = players_.Observer().AbilityInstance(slot, handle);
  if (ability == nullptr) {
    return std::unexpected("the ability is not live");
  }
  return charge_limit_->Set(ability, handle, maximum);
}

std::expected<void, std::string> Heroes::GiveItem(const abilities::GiveItemRequest& request) {
  // Resolve the item tables and the player's live pawn.
  auto definitions = services_.Abilities();
  if (!definitions) {
    return std::unexpected(definitions.error());
  }
  auto items = services_.Items();
  if (!items) {
    return std::unexpected(items.error());
  }
  if (auto pawn = players_.LivePawn(request.player()); !pawn) {
    return std::unexpected(pawn.error());
  }

  // Grant the item to the player's hero, imbuing an ability when asked.
  const auto item_id = gameinterop::MakeMemberName(request.item()).hash;
  if (!request.has_imbue()) {
    auto item = players_.Observer().GrantItem(request.player(), item_id, **definitions, **items);
    if (!item) {
      return std::unexpected(item.error());
    }
    return {};
  }
  if (request.imbue() > gameinterop::kLastSignatureSlot) {
    return std::unexpected("imbue names an ability slot from 0 to 3");
  }
  auto imbues = services_.Imbues();
  if (!imbues) {
    return std::unexpected(imbues.error());
  }
  auto item = players_.Observer().GrantImbuingItem(request.player(), item_id,
                                                   static_cast<uint16_t>(request.imbue()),
                                                   **definitions, **items, **imbues);
  if (!item) {
    return std::unexpected(item.error());
  }
  return {};
}

std::expected<void, std::string> Heroes::BuyItem(const abilities::BuyItemRequest& request) {
  // Check the slot and the source, then resolve the shop and the player's pawn.
  if (request.has_imbue() && request.imbue() > gameinterop::kLastSignatureSlot) {
    return std::unexpected("imbue names an ability slot from 0 to 3");
  }
  if (request.source() >= gameinterop::kShopSources) {
    return std::unexpected("source is from 0 to 21");
  }
  auto definitions = services_.Abilities();
  if (!definitions) {
    return std::unexpected(definitions.error());
  }
  auto shop = services_.Shop();
  if (!shop) {
    return std::unexpected(shop.error());
  }
  const gameinterop::ImbueFunctions* imbue = nullptr;
  if (request.has_imbue()) {
    auto imbues = services_.Imbues();
    if (!imbues) {
      return std::unexpected(imbues.error());
    }
    imbue = *imbues;
  }
  if (auto pawn = players_.LivePawn(request.player()); !pawn) {
    return std::unexpected(pawn.error());
  }

  // The shop sells it, imbuing the chosen ability, or refuses.
  std::optional<uint16_t> target;
  if (request.has_imbue()) {
    target = static_cast<uint16_t>(request.imbue());
  }
  auto bought = players_.Observer().BuyItem(
      request.player(), gameinterop::MakeMemberName(request.item()).hash, target,
      static_cast<uint8_t>(request.source()), **definitions, **shop, imbue);
  if (!bought) {
    return std::unexpected(bought.error());
  }
  return {};
}

std::expected<void, std::string> Heroes::TrainAbility(
    const abilities::TrainAbilityRequest& request) {
  // Resolve the game's training and the player's live pawn.
  if (request.slot() > gameinterop::kLastSignatureSlot) {
    return std::unexpected("slot names an ability slot from 0 to 3");
  }
  auto training = services_.Training();
  if (!training) {
    return std::unexpected(training.error());
  }
  if (auto pawn = players_.LivePawn(request.player()); !pawn) {
    return std::unexpected(pawn.error());
  }

  // The game trains or upgrades the held ability, or refuses.
  auto trained = players_.Observer().TrainAbility(
      request.player(), static_cast<uint16_t>(request.slot()), **training);
  if (!trained) {
    return std::unexpected(trained.error());
  }
  return {};
}

std::expected<void, std::string> Heroes::ReplaceAbility(
    const abilities::ReplaceAbilityRequest& request) {
  // Check the slot and resolve the ability tables.
  if (request.index() > UINT16_MAX) {
    return std::unexpected("the ability slot is out of range");
  }
  auto definitions = services_.Abilities();
  if (!definitions) {
    return std::unexpected(definitions.error());
  }
  auto slots = services_.Slots();
  if (!slots) {
    return std::unexpected(slots.error());
  }
  auto create = services_.CreateAbility();
  if (!create) {
    return std::unexpected(create.error());
  }

  // Find the ability the slot holds now, whose progress keep carries.
  const auto player = request.player();
  const auto slot = static_cast<uint16_t>(request.index());
  if (auto pawn = players_.LivePawn(player); !pawn) {
    return std::unexpected(pawn.error());
  }
  std::optional<gameinterop::PawnObserver::Ability> old;
  if (request.keep()) {
    auto owned = players_.Observer().CurrentAbilitiesForSlot(player);
    if (!owned) {
      return std::unexpected(owned.error());
    }
    if (auto found = std::ranges::find(*owned, slot, &gameinterop::PawnObserver::Ability::slot);
        found != owned->end()) {
      old = *found;
    }
  }

  // Replace the ability, created with the old one's upgrade bits.
  const uint16_t upgrades = old ? static_cast<uint16_t>(old->upgrade_info >> 16) : 0;
  if (auto replaced = (*slots)->Replace(players_.Observer(), player, slot,
                                        gameinterop::MakeMemberName(request.ability()).hash,
                                        upgrades, **definitions, *create);
      !replaced || !old) {
    return replaced;
  }

  // Carry the cooldown, and the charges when both abilities hold them.
  auto owned = players_.Observer().CurrentAbilitiesForSlot(player);
  if (!owned) {
    return std::unexpected(owned.error());
  }
  auto fresh = std::ranges::find(*owned, slot, &gameinterop::PawnObserver::Ability::slot);
  if (fresh == owned->end() || fresh->handle == old->handle) {
    return {};
  }
  fresh->cooldown_start = old->cooldown_start;
  fresh->cooldown_end = old->cooldown_end;
  fresh->charge_recharge_start = old->charge_recharge_start;
  fresh->charge_recharge_end = old->charge_recharge_end;
  if (old->charges > 0 && fresh->charges > 0) {
    fresh->charges = std::min(old->charges, fresh->charges);
  }
  return players_.Observer().ApplyAbilityTimers(player, std::span(&*fresh, 1));
}

std::expected<gameinterop::PawnObserver::Ability, std::string> Heroes::OwnedAbility(
    int32_t slot, std::string_view name) {
  // Resolve the ability table and the player's live abilities.
  auto definitions = services_.Abilities();
  if (!definitions) {
    return std::unexpected(definitions.error());
  }
  if (auto pawn = players_.LivePawn(slot); !pawn) {
    return std::unexpected(pawn.error());
  }
  auto abilities = players_.Observer().CurrentAbilitiesForSlot(slot);
  if (!abilities) {
    return std::unexpected(abilities.error());
  }

  // Find the owned ability with that name.
  for (const auto& ability : *abilities) {
    const auto definition = (*definitions)->Find(ability.subclass_id);
    if (definition && definition->name == name) {
      return ability;
    }
  }
  return std::unexpected("the hero has no ability named " + std::string(name));
}

std::expected<void, std::string> Heroes::AdjustSouls(const players::AdjustSoulsRequest& request) {
  // Adjust the live player's souls.
  auto currency = services_.Currency();
  if (!currency) {
    return std::unexpected(currency.error());
  }
  if (auto pawn = players_.LivePawn(request.player()); !pawn) {
    return std::unexpected(pawn.error());
  }
  auto adjusted = players_.Observer().AdjustCurrency(request.player(), 0, request.delta(),
                                                     request.silent(), *currency);
  if (!adjusted) {
    return std::unexpected(adjusted.error());
  }
  return {};
}

std::expected<void, std::string> Heroes::AdjustAbilityPoints(
    const players::AdjustAbilityPointsRequest& request) {
  // Check that the live player holds every spend before changing either.
  auto currency = services_.Currency();
  if (!currency) {
    return std::unexpected(currency.error());
  }
  if (auto pawn = players_.LivePawn(request.player()); !pawn) {
    return std::unexpected(pawn.error());
  }
  const auto sample = players_.Observer().Observe(request.player());
  if (!sample || !sample->currencies) {
    return std::unexpected("native wallet unavailable");
  }
  // EAbilityPoints=1, EAbilityUnlocks=2.
  const std::array changes{std::pair{1u, request.points()}, std::pair{2u, request.unlocks()}};
  for (const auto [type, delta] : changes) {
    if (delta < 0 && (*sample->currencies)[type] < -delta) {
      return std::unexpected("the player holds fewer ability points or unlocks than the spend");
    }
  }

  // Apply each change silently.
  for (const auto [type, delta] : changes) {
    if (auto adjusted =
            players_.Observer().AdjustCurrency(request.player(), type, delta, true, *currency);
        !adjusted) {
      return std::unexpected(adjusted.error());
    }
  }
  return {};
}

std::expected<void, std::string> Heroes::StartingSouls(
    const players::StartingSoulsRequest& request) {
  // Set the live player's starting souls.
  auto currency = services_.Currency();
  if (!currency) {
    return std::unexpected(currency.error());
  }
  if (auto pawn = players_.LivePawn(request.player()); !pawn) {
    return std::unexpected(pawn.error());
  }
  auto prepared =
      players_.Observer().PrepareStartingSouls(request.player(), request.souls(), *currency);
  if (!prepared) {
    return std::unexpected(prepared.error());
  }
  return {};
}

std::expected<void, std::string> Heroes::ResetHero(const players::ResetHeroRequest& request) {
  // Reset the live player's hero to fresh starting souls.
  auto currency = services_.Currency();
  if (!currency) {
    return std::unexpected(currency.error());
  }
  auto reset = services_.ResetHero();
  if (!reset) {
    return std::unexpected(reset.error());
  }
  if (auto pawn = players_.LivePawn(request.player()); !pawn) {
    return std::unexpected(pawn.error());
  }
  auto fresh =
      players_.Observer().ResetStartingSouls(request.player(), request.souls(), *currency, *reset);
  if (!fresh) {
    return std::unexpected(fresh.error());
  }
  return {};
}

std::expected<void, std::string> Heroes::HoldLevelSouls(int32_t slot, int32_t level) {
  // Resolve the native calls and the souls progression leaves out of a level.
  auto required = services_.RequiredSouls();
  if (!required) {
    return std::unexpected(required.error());
  }
  auto currency = services_.Currency();
  if (!currency) {
    return std::unexpected(currency.error());
  }
  auto variables = gameinterop::ConsoleVariables::Resolve();
  if (!variables) {
    return std::unexpected(variables.error());
  }
  auto starting = variables->ReadInt32("citadel_player_starting_gold");
  if (!starting) {
    return std::unexpected(starting.error());
  }

  // Move the hero's souls into the level's band.
  auto held = players_.Observer().HoldLevelSouls(slot, level, *starting, *required, *currency);
  if (!held) {
    return std::unexpected(held.error());
  }
  return {};
}

std::expected<void, std::string> Heroes::Kill(const players::PlayerRequest& request) {
  // Kill the live player's hero unless it is already dead.
  auto damage = services_.Damage();
  if (!damage) {
    return std::unexpected(damage.error());
  }
  auto sample = players_.Live(request.player());
  if (!sample) {
    return std::unexpected(sample.error());
  }
  return (*damage)->Kill(sample->pawn);
}

}  // namespace modlock::wasm
