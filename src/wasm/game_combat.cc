//go:build deps_only

#include <iostream>
#include <span>
#include <utility>
#include <vector>

#include "modlock/gameinterop/native_memory.h"
#include "wasm/game.h"
#include "wasm/game_internal.h"

namespace modlock::wasm {

std::expected<void, std::string> Game::Damage(const combat::HitOptions& request) {
  // Resolve the damage service and the victim.
  auto damage = services_.Damage();
  if (!damage) {
    return std::unexpected(damage.error());
  }
  auto victim = Entity(request.victim());
  if (!victim) {
    return std::unexpected(victim.error());
  }

  // Only heroes and units take hits: the client crashes on a damage event for
  // the world, a prop or a building.
  auto victim_class = gameinterop::SchemaClassOfEntity(*victim, gameinterop::ReadNative);
  if (!victim_class) {
    return std::unexpected(victim_class.error());
  }
  if (!services_.Derives(*victim_class, "CBaseCombatCharacter")) {
    return std::unexpected("the victim is a " +
                           std::string(gameinterop::SchemaClassNameOf(*victim_class)) +
                           ", not a hero or unit");
  }

  // A hit nobody dealt is a hazard: it may kill and credits no one.
  if (request.attacker() == 0) {
    return (*damage)->Hazard(*victim, request.amount());
  }

  // Resolve who dealt the hit and with what.
  auto attacker = Entity(request.attacker());
  if (!attacker) {
    return std::unexpected(attacker.error());
  }
  auto inflictor = request.inflictor() == 0 ? attacker : Entity(request.inflictor());
  if (!inflictor) {
    return std::unexpected(inflictor.error());
  }
  const int32_t hit_group = request.has_hit_group() ? request.hit_group() : -1;
  void* ability = nullptr;
  if (request.has_ability()) {
    auto found = Entity(request.ability());
    if (!found) {
      return std::unexpected(found.error());
    }
    ability = *found;
  }
  return (*damage)->Hit(*victim, *inflictor, *attacker, ability, request.amount(), hit_group);
}

std::expected<void, std::string> Game::AdjustSouls(const players::AdjustSoulsRequest& request) {
  // Adjust the live player's souls.
  auto currency = services_.Currency();
  if (!currency) {
    return std::unexpected(currency.error());
  }
  if (auto pawn = LivePawn(request.player()); !pawn) {
    return std::unexpected(pawn.error());
  }
  auto adjusted =
      observer_.AdjustSouls(request.player(), request.delta(), request.silent(), *currency);
  if (!adjusted) {
    return std::unexpected(adjusted.error());
  }
  return {};
}

std::expected<void, std::string> Game::StartingSouls(const players::StartingSoulsRequest& request) {
  // Set the live player's starting souls.
  auto currency = services_.Currency();
  if (!currency) {
    return std::unexpected(currency.error());
  }
  if (auto pawn = LivePawn(request.player()); !pawn) {
    return std::unexpected(pawn.error());
  }
  auto prepared = observer_.PrepareStartingSouls(request.player(), request.souls(), *currency);
  if (!prepared) {
    return std::unexpected(prepared.error());
  }
  return {};
}

std::expected<void, std::string> Game::ResetHero(const players::ResetHeroRequest& request) {
  // Reset the live player's hero to fresh starting souls.
  auto currency = services_.Currency();
  if (!currency) {
    return std::unexpected(currency.error());
  }
  auto reset = services_.ResetHero();
  if (!reset) {
    return std::unexpected(reset.error());
  }
  if (auto pawn = LivePawn(request.player()); !pawn) {
    return std::unexpected(pawn.error());
  }
  auto fresh = observer_.ResetStartingSouls(request.player(), request.souls(), *currency, *reset);
  if (!fresh) {
    return std::unexpected(fresh.error());
  }
  return {};
}

std::expected<void, std::string> Game::RestoreHero(const players::RestoreHeroRequest& request) {
  // Build the restore target from the request.
  const auto& source = request.target();
  RestoreTarget target{
      .position = Floats(source.position()),
      .angles = Floats(source.facing()),
      .fresh = source.fresh(),
      .health = source.health(),
      .max_health = source.max_health(),
  };
  if (source.has_level()) {
    target.level = source.level();
  }
  if (!source.upgrade_bonuses().empty()) {
    if (source.upgrade_bonuses_size() != 3) {
      return std::unexpected("upgrade bonuses hold exactly three values");
    }
    target.upgrade_bonuses = {source.upgrade_bonuses(0), source.upgrade_bonuses(1),
                              source.upgrade_bonuses(2)};
  }
  for (const auto& ability : source.abilities()) {
    if (!ability.has_slot() || ability.slot() < 0 || ability.slot() > UINT16_MAX) {
      return std::unexpected("each ability needs its slot");
    }
    target.abilities.push_back({.subclass_id = ability.id(),
                                .slot = static_cast<uint16_t>(ability.slot()),
                                .upgrade_info = ability.state()});
  }
  if (source.replace_items()) {
    auto& items = target.items.emplace();
    for (const auto& item : source.items()) {
      gameinterop::ItemTarget out{.subclass_id = item.id(), .upgrade_info = item.state()};
      if (item.has_slot()) {
        if (item.slot() < 0 || item.slot() > UINT16_MAX) {
          return std::unexpected("an item slot is out of range");
        }
        out.slot = static_cast<uint16_t>(item.slot());
      }
      items.push_back(out);
    }
  }
  for (const auto& timer : source.timers()) {
    target.timers.push_back({.subclass_id = timer.id(),
                             .charges = timer.charges(),
                             .cooldown = {timer.cooldown_start(), timer.cooldown_end()},
                             .recharge = {timer.recharge_start(), timer.recharge_end()}});
  }

  // Resolve the live pawn and the engine services.
  auto sample = Live(request.player());
  if (!sample) {
    return std::unexpected(sample.error());
  }
  auto definitions = services_.Abilities();
  if (!definitions) {
    return std::unexpected(definitions.error());
  }
  auto server = services_.Server();
  if (!server) {
    return std::unexpected(server.error());
  }

  // Bind the restore to the engine and start it.
  const auto* abilities = *definitions;
  const auto* engine = *server;
  HeroRestore::Engine access{
      .apply = [this](const HeroRestore::Sample& pawn, const RestoreTarget& target,
                      HeroRestore::Stage stage) { return ApplyRestore(pawn, target, stage); },
      .abilities = [this](int32_t slot) { return observer_.CurrentAbilitiesForSlot(slot); },
      .definition = [abilities](uint32_t id) { return abilities->Find(id); },
      .clock = [engine] { return engine->ReadClock(); },
      .set_timers =
          [this](int32_t slot, std::span<const HeroRestore::Ability> timers) {
            return observer_.ApplyAbilityTimers(slot, timers);
          },
      .ready_timers = [this](int32_t slot) { return observer_.FreshAbilityTargets(slot); },
  };
  auto restore = HeroRestore::Create(*sample, std::move(target), std::move(access));
  if (!restore) {
    return std::unexpected(restore.error());
  }
  restores_.insert_or_assign(request.player(), std::move(*restore));
  return {};
}

bool Game::ApplyRestore(const HeroRestore::Sample& sample, const RestoreTarget& target,
                        HeroRestore::Stage stage) {
  // Log a step that fails, and borrow the player's pawn.
  const auto log = [](std::string_view what, const std::string& error) {
    std::cerr << "[modlock] hero restore: " << what << ": " << error << "\n";
    return false;
  };
  const auto slot = sample.slot;
  void* pawn = observer_.PawnForSlot(slot);

  // The build stage restores the level, items, abilities and upgrade bonuses.
  if (stage == HeroRestore::Stage::kBuild) {
    // Restore the level, items and abilities.
    if (target.level && !gameinterop::RestorePawnLevel(pawn, *target.level)) {
      return false;
    }
    auto definitions = services_.Abilities();
    if (!definitions) {
      return log("abilities", definitions.error());
    }
    auto items = services_.Items();
    if (!items) {
      return log("items", items.error());
    }
    if (target.items) {
      if (auto done = observer_.ReconcileItems(slot, *target.items, **definitions, **items);
          !done) {
        return log("items", done.error());
      }
    }
    if (!target.abilities.empty()) {
      auto create = services_.CreateAbility();
      if (!create) {
        return log("abilities", create.error());
      }
      if (auto done = observer_.ReconcileAbilities(slot, target.abilities, **definitions, *create,
                                                   (*items)->set_bits);
          !done) {
        return log("abilities", done.error());
      }
    }

    // Item and ability calls can replace the pawn, so borrow it again.
    if (!observer_.CurrentAbilitiesForSlot(slot)) {
      return false;
    }
    pawn = observer_.PawnForSlot(slot);
    if (!target.fresh && target.upgrade_bonuses) {
      if (auto done = gameinterop::RestorePawnUpgradeBonuses(pawn, *target.upgrade_bonuses);
          !done) {
        return log("upgrade bonuses", done.error());
      }
    }
    return true;
  }

  // The pawn stage restores health, and full stamina for a fresh hero.
  if (stage == HeroRestore::Stage::kPawn) {
    if (target.fresh) {
      // A fresh hero starts with full stamina once its health is live.
      if (!sample.effective_max_health || *sample.effective_max_health <= 0) {
        return false;
      }
      auto server = services_.Server();
      if (!server) {
        return log("stamina", server.error());
      }
      auto clock = (*server)->ReadClock();
      if (!clock) {
        return log("stamina", clock.error());
      }
      if (auto done = observer_.SetPracticeStamina(slot, clock->current_time); !done) {
        return log("stamina", done.error());
      }
      pawn = observer_.PawnForSlot(slot);
    }
    const auto health = target.fresh ? *sample.effective_max_health : target.health;
    const auto max_health = target.fresh ? sample.max_health : target.max_health;
    if (!gameinterop::RestorePawnHealth(pawn, health, max_health)) {
      return false;
    }
  }

  // A bot has no camera to turn; its hero faces the angles and its AI aims.
  auto teleport = services_.Teleport();
  if (!teleport) {
    return log("pose", teleport.error());
  }
  pawn = observer_.PawnForSlot(slot);
  if (pawn == nullptr) {
    return false;
  }
  if (sample.is_bot) {
    gameinterop::TeleportEntity(pawn, target.position, target.angles, {});
  } else {
    (*teleport)(nullptr, pawn, target.position.data(), target.angles.data());
  }

  // A teleport recomputes the hero's modifier states, dropping held ones.
  ApplyHolds();
  return true;
}

void Game::TickRestores() {
  if (npc_restore_) {
    if (auto result = npc_restore_->Tick()) {
      npcs_restored_ = *result ? std::string() : result->error();
      npc_restore_.reset();
    }
  }
  if (restores_.empty()) {
    return;
  }
  ++frames_;
  for (auto it = restores_.begin(); it != restores_.end();) {
    auto result = it->second.Tick(observer_.Observe(it->first), frames_);
    if (!result) {
      ++it;
      continue;
    }
    if (restored_.size() < kMaxChanges) {
      restored_.push_back({it->first, *result ? std::string() : result->error()});
    }
    it = restores_.erase(it);
  }
}

std::expected<void, std::string> Game::RestoreNpcs(const npcs::RestoreNpcsRequest& request) {
  // Describe each NPC to restore.
  std::vector<NpcRestore::Target> targets;
  for (const auto& npc : request.npcs()) {
    targets.push_back({
        .designer_name = npc.class_name(),
        .subclass_id = npc.id(),
        .team = npc.team(),
        .position = Floats(npc.position()),
        .facing = Floats(npc.facing()),
        .velocity = Floats(npc.velocity()),
        .health = npc.health(),
        .max_health = npc.max_health(),
        .lane = npc.has_lane() ? std::optional(npc.lane()) : std::nullopt,
    });
  }

  // Restore the NPCs into the world over the coming frames.
  auto world = World();
  if (!world) {
    return std::unexpected(world.error());
  }
  auto* entities = *world;
  auto restore = NpcRestore::Create(
      std::move(targets), {
                              .restore =
                                  [entities](std::span<const NpcRestore::Target> targets) {
                                    return entities->Restore(targets);
                                  },
                              .finish = [entities] { return entities->FinishRestore(); },
                              .read = [entities] { return entities->Read(); },
                          });
  if (!restore) {
    return std::unexpected(restore.error());
  }
  npc_restore_ = std::move(*restore);
  return {};
}

}  // namespace modlock::wasm
