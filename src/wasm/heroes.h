#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "modlock/gameinterop/ability_charge_limit.h"
#include "modlock/gameinterop/ability_cooldown.h"
#include "modlock/gameinterop/pawn_observer.h"
#include "src/wasm/abilities/abilities.pb.h"
#include "src/wasm/players/players.pb.h"
#include "wasm/hero_picks.h"
#include "wasm/host_service.gen.h"
#include "wasm/module.h"
#include "wasm/roster.h"

namespace modlock::wasm {

class GameServices;

// Heroes answers a mod's calls on its players' heroes: picking, respawning,
// moving between teams, freezing and killing a hero, its stamina, souls and
// ability points, and its abilities and items. It owns what the mod changed
// that outlives one call, the frozen heroes and the cooldown lengths and
// charge limits, and gives each back when the mod stops.
class Heroes final : public Module, public HeroesCalls {
 public:
  // services and players outlive the heroes.
  Heroes(GameServices& services, Roster& players) : services_(services), players_(players) {}

  Heroes(const Heroes&) = delete;
  Heroes& operator=(const Heroes&) = delete;

  std::expected<players::HeroResponse, std::string> SelectHero(
      const players::SelectHeroRequest& request) override;
  std::expected<void, std::string> Respawn(const players::PlayerRequest& request) override;
  std::expected<void, std::string> ChangeTeam(const players::ChangeTeamRequest& request) override;
  std::expected<void, std::string> ClearItems(const players::PlayerRequest& request) override;
  std::expected<void, std::string> Freeze(const players::FreezeRequest& request) override;
  std::expected<void, std::string> RestoreStamina(const players::PlayerRequest& request) override;
  std::expected<void, std::string> SetStamina(const players::SetStaminaRequest& request) override;
  std::expected<void, std::string> RefreshAbility(
      const abilities::RefreshAbilityRequest& request) override;
  std::expected<abilities::AbilitiesResponse, std::string> Abilities(
      const players::PlayerRequest& request) override;
  std::expected<void, std::string> SetAbility(const abilities::AbilityOptions& request) override;
  std::expected<void, std::string> GiveItem(const abilities::GiveItemRequest& request) override;
  std::expected<void, std::string> TrainAbility(
      const abilities::TrainAbilityRequest& request) override;
  std::expected<void, std::string> BuyItem(const abilities::BuyItemRequest& request) override;
  std::expected<abilities::HeroBuildResponse, std::string> HeroBuild(
      const abilities::HeroBuildRequest& request) override;
  std::expected<void, std::string> ReplaceAbility(
      const abilities::ReplaceAbilityRequest& request) override;
  std::expected<void, std::string> AdjustSouls(const players::AdjustSoulsRequest& request) override;
  std::expected<void, std::string> AdjustAbilityPoints(
      const players::AdjustAbilityPointsRequest& request) override;
  std::expected<void, std::string> StartingSouls(
      const players::StartingSoulsRequest& request) override;
  std::expected<void, std::string> ResetHero(const players::ResetHeroRequest& request) override;
  std::expected<void, std::string> Kill(const players::PlayerRequest& request) override;

  // ClientCommand notes a console command a player sent the game that the
  // host follows: a selecthero pick from the game's Change Hero, which
  // SelectHero leaves to land.
  void ClientCommand(int32_t slot, std::string_view line);

  // Cast describes one executed ability for the mod: the caster's player,
  // and the ability's name and slot when a player's hero cast it.
  abilities::AbilityCastEvent Cast(uint32_t caster, uint32_t ability, uint32_t target);

  // HoldLevelSouls gives the slot's hero the souls its restored level needs,
  // so the game's progression keeps the level.
  std::expected<void, std::string> HoldLevelSouls(int32_t slot, int32_t level);

  // WorldEnding forgets the frozen heroes, which go with the world.
  void WorldEnding() override;

  // Leave forgets the slot's freeze and its pending Change Hero pick.
  void Leave(int32_t slot) override;

  // Clear unfreezes the frozen heroes and gives every ability back the
  // game's own cooldown lengths and charge limits.
  void Clear() override;

 private:
  // LatchTime is the time a hero's stamina latch counts from: the simulation
  // time without the ticks the game spent paused.
  std::expected<float, std::string> LatchTime();

  // SetCooldownLength makes seconds the length of the ability's cooldowns,
  // or returns it to the game's own when seconds is negative.
  std::expected<void, std::string> SetCooldownLength(int32_t slot, uint32_t handle, float seconds);

  // LimitCharges makes maximum the most charges the player's ability holds.
  std::expected<void, std::string> LimitCharges(int32_t slot, uint32_t handle, int32_t maximum);

  // OwnedAbility returns the ability named name of slot's live hero.
  std::expected<gameinterop::PawnObserver::Ability, std::string> OwnedAbility(
      int32_t slot, std::string_view name);

  // ImbuedAbilities returns the definition ids of the abilities that the
  // ability with handle, owned by slot's hero, imbues; empty for most.
  std::vector<uint32_t> ImbuedAbilities(int32_t slot, uint32_t handle);

  GameServices& services_;
  Roster& players_;
  // picks_ holds each player's own Change Hero pick until it lands.
  HeroPicks picks_;
  // frozen_ is the slots whose heroes the mod froze.
  std::set<int32_t> frozen_;
  // cooldown_ holds the cooldown lengths the mod gave abilities, created the
  // first time one is given; it puts the game's own lengths back when the
  // mod stops.
  std::unique_ptr<gameinterop::AbilityCooldown> cooldown_;
  // charge_limit_ holds the most charges the mod gave abilities, created the
  // first time one is limited; it puts the game's own maximums back when the
  // mod stops.
  std::unique_ptr<gameinterop::AbilityChargeLimit> charge_limit_;
};

}  // namespace modlock::wasm
