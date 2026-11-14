//go:build deps_only

#include <algorithm>
#include <iostream>
#include <memory>
#include <mutex>
#include <string_view>
#include <utility>

#include "modlock/gameinterop/connection_tracker.h"
#include "modlock/gameinterop/entity_abi.h"
#include "wasm/wasm_plugin.h"

namespace modlock::wasm {

namespace {

// kModifierEventPrefix starts every EModifierEvent name; a mod may leave it
// out.
constexpr std::string_view kModifierEventPrefix = "MODIFIER_EVENT_";

// kMaxDamaged bounds the applied hits, casts and the movement facts queued between
// frames.
constexpr size_t kMaxDamaged = 1024;

// MovementActionOf names a movement execution the game announced.
wasm::movement::MovementAction MovementActionOf(gameinterop::MovementExecution execution) {
  using enum gameinterop::MovementExecution;
  switch (execution) {
    case kLandedOnGround:
      return wasm::movement::MOVEMENT_ACTION_LANDED;
    case kAttachedToZipline:
      return wasm::movement::MOVEMENT_ACTION_ZIPLINE_ATTACHED;
    case kGroundDash:
      return wasm::movement::MOVEMENT_ACTION_GROUND_DASH;
    case kSlide:
      return wasm::movement::MOVEMENT_ACTION_SLIDE;
    case kBouncePadActivated:
      return wasm::movement::MOVEMENT_ACTION_BOUNCE_PAD;
    case kDashJump:
      return wasm::movement::MOVEMENT_ACTION_DASH_JUMP;
    case kAirJump:
      return wasm::movement::MOVEMENT_ACTION_AIR_JUMP;
    case kWallJump:
      return wasm::movement::MOVEMENT_ACTION_WALL_JUMP;
    case kAirDash:
      return wasm::movement::MOVEMENT_ACTION_AIR_DASH;
    case kMeleeAttackStarted:
      return wasm::movement::MOVEMENT_ACTION_MELEE_STARTED;
  }
  return wasm::movement::MOVEMENT_ACTION_UNKNOWN;
}

// InteractionOf describes a completed interaction to a mod.
wasm::movement::Interaction InteractionOf(const gameinterop::InteractionEvent& event) {
  wasm::movement::Interaction out;
  out.set_kind(event.kind == gameinterop::InteractionKind::kPropBroken
                   ? wasm::movement::INTERACTION_KIND_PROP_BROKEN
                   : wasm::movement::INTERACTION_KIND_PICKUP_COLLECTED);
  out.set_entity(event.object_handle);
  out.set_designer_name(event.designer_name);
  out.set_subclass_id(event.subclass_id);
  auto* origin = out.mutable_origin();
  origin->set_x(event.origin[0]);
  origin->set_y(event.origin[1]);
  origin->set_z(event.origin[2]);
  if (event.vacuum_start) {
    out.set_vacuum_start(*event.vacuum_start);
  }
  if (event.inflictor_handle != gameinterop::kInvalidEHandleIndex) {
    out.set_inflictor(event.inflictor_handle);
  }
  return out;
}

// Departures forwards player departures to a plugin, which owns the
// subscription that holds it.
class Departures final : public gameinterop::ConnectionEventSink {
 public:
  explicit Departures(std::function<void(int32_t, int32_t)> leave) : leave_(std::move(leave)) {}

  void OnConnected(int32_t, uint64_t, bool, const char*) override {}

  void OnDisconnecting(int32_t slot, uint64_t, int32_t reason) override { leave_(slot, reason); }

 private:
  std::function<void(int32_t, int32_t)> leave_;
};

}  // namespace

void WasmPlugin::Move() {
  // Only a running QuakeWorld mod moves players, and only once.
  if (movement_ || !instance_ || manifest_.movement().model() != wasm::Movement::MODEL_QUAKEWORLD) {
    return;
  }

  // Build the server's movement model.
  auto movement = quake::PlayerMovement::Create(collision_, manifest_.movement().scale());
  if (!movement) {
    Log("movement is unavailable: " + movement.error());
    return;
  }
  movement_ = std::move(*movement);

  // Step every hero's commands through it.
  auto stepping = host_.game.StepWith([this](gameinterop::MovementCall& call) {
    const auto ground = call.ground_entity;
    if (!movement_->Step(call)) {
      return false;
    }
    // The hook does not replicate the step, so a changed ground goes out
    // here.
    if (call.ground_entity != ground) {
      static_cast<void>(gameinterop::NotifyEntityStateChanged(call.pawn));
    }
    return true;
  });
  if (!stepping) {
    Log("movement is unavailable: " + stepping.error());
    movement_.reset();
  }
}

void WasmPlugin::Steer() {
  // Tell the mod about each landing.
  if (!movement_) {
    return;
  }
  for (const auto& landing : movement_->TakeLandings()) {
    // Find the player who landed and what they landed on.
    const auto player = hero_slots_.find(landing.player);
    if (player == hero_slots_.end()) {
      continue;
    }
    const auto on = landing.on == 0 ? hero_slots_.end() : hero_slots_.find(landing.on);

    // Tell the mod, which may stop movement.
    wasm::movement::LandedEvent event;
    event.set_player(player->second);
    event.set_on(on == hero_slots_.end() ? -1 : on->second);
    event.set_speed(landing.speed);
    static_cast<void>(mod_.Landed(event));
    if (!movement_) {
      return;
    }
  }

  // Hand the model the current heroes.
  std::vector<quake::PlayerMovement::Player> players;
  hero_slots_.clear();
  for (const auto& hero : game_.Heroes()) {
    players.push_back({.pawn = hero.pawn, .handle = hero.handle, .feet = hero.feet});
    hero_slots_.emplace(hero.handle, hero.slot);
  }
  movement_->SetPlayers(std::move(players));
  if (floor_known_) {
    return;
  }

  // Heroes spawn in the air, so a trace down names the world entity they land
  // on before any has.
  auto trace = engine_->Trace();
  if (!trace || !(*trace)->IsReady()) {
    return;
  }
  gameinterop::TraceOptions options;
  options.interacts_with = gameinterop::TraceMask(gameinterop::TraceLayer::kSolid);
  auto floor = (*trace)->Query({0, 0, 100}, {0, 0, -100}, gameinterop::TraceLine{}, options);
  if (floor && floor->entity_handle) {
    movement_->SetWorld(*floor->entity_handle);
  }
  floor_known_ = true;
}

void WasmPlugin::ListenToPlayers() {
  // Listen once, while a mod runs.
  if (players_.Active() || !instance_) {
    return;
  }

  // Offer the mod every chat line it asked for, then every player command:
  // "/hello there" in chat arrives as "hello there", as does the console
  // command of the same line. Chat also accepts ".", "+" or "-" in place of
  // the slash, for players whose chat keeps a slash line to itself.
  // Each subscription stands alone: a hook that cannot install disables its
  // feature and leaves the others running.
  if (auto chats = engine_->OnChat([this](const gameinterop::NativeChatHook::Message& line) {
        if (wants_.chat()) {
          Chat(line);
        }
        const std::string_view text = line.text;
        if (!text.empty() && std::string_view("/.+-").contains(text.front())) {
          Command(line.slot, text.substr(1));
        }
      })) {
    players_.Add(std::move(*chats));
  } else {
    Log(std::string("chat commands are unavailable: ") + chats.error());
  }
  if (auto commands = engine_->OnCommand([this](int32_t slot, std::string_view line) {
        game_.ClientCommand(slot, line);
        return Command(slot, line);
      })) {
    players_.Add(std::move(*commands));
  } else {
    Log(std::string("console commands are unavailable: ") + commands.error());
  }
  if (auto departures = engine_->OnConnection(std::make_shared<Departures>(
          [this](int32_t slot, int32_t reason) { Leave(slot, reason); }))) {
    players_.Add(std::move(*departures));
  } else {
    Log(std::string("interfaces may outlive their players: ") + departures.error());
  }
  if (players_.Active()) {
    std::cerr << name_ << ": player commands ready\n";
  }
}

void WasmPlugin::ListenToCombat() {
  // Listen once, while a mod wants damage, casts, heals, levels, melee,
  // parries or modifier events, moves players or serves some.
  if (combat_.Active() || !instance_ || combat_unavailable_ ||
      (!wants_.damage() && !wants_.damaged() && !wants_.kills() && !wants_.casts() &&
       !wants_.heals() && !wants_.levels() && !wants_.melees() && !wants_.parries() &&
       wants_.modifier_events().empty() && !game_.Moving() && !scope_.Limited())) {
    return;
  }

  // A mod that wants damage can block or change each hit, and a limited one
  // blocks each hit between its players and others.
  std::function<bool(const gameinterop::DamageContactEvent&)> suppress;
  gameinterop::AdjustDamage adjust;
  if (wants_.damage() || scope_.Limited()) {
    suppress = [this](const gameinterop::DamageContactEvent& contact) {
      return Hit(contact);
    };
  }
  if (wants_.damage()) {
    adjust = [this](const gameinterop::DamageContactEvent&, float& amount) {
      if (adjusted_) {
        amount = *std::exchange(adjusted_, std::nullopt);
      }
    };
  }

  // Subscribe to combat.
  auto combat = engine_->OnCombat(
      [this, wants = wants_](const gameinterop::CombatEvent& event) { Fought(event, wants); },
      std::move(suppress), std::move(adjust));
  if (!combat) {
    // The hook installs from the game's code, so it fails the same way until
    // the next world; trying again on every frame would only repeat the log.
    combat_unavailable_ = true;
    Log(std::string("damage events are unavailable: ") + combat.error());
    return;
  }
  combat_ = std::move(*combat);

  // Watch the modifier events the mod named. A name the game lacks disables
  // only that event.
  for (const auto& name : wants_.modifier_events()) {
    const auto full =
        name.starts_with(kModifierEventPrefix) ? name : std::string(kModifierEventPrefix) + name;
    auto id = engine_->WatchModifierEvent(full);
    if (!id) {
      Log("modifier event " + name + " is unavailable: " + id.error());
      continue;
    }

    // An event named twice, with and without the prefix, keeps one watch and
    // answers to both names.
    std::lock_guard lock(damaged_lock_);
    auto& names = watched_[*id];
    if (!names.empty()) {
      engine_->UnwatchModifierEvent(*id);
    }
    names.push_back(name);
  }
}

bool WasmPlugin::Hit(const gameinterop::DamageContactEvent& contact) {
  // Forget the last adjustment.
  adjusted_.reset();

  // The mod rules only hits it hears, and blocks those that cross its scope.
  switch (Contact(contact.attacker_handle, contact.victim_handle)) {
    case PlayerScope::Contact::kBlock:
      return true;
    case PlayerScope::Contact::kPass:
      return false;
    case PlayerScope::Contact::kHear:
      break;
  }
  if (!wants_.damage()) {
    return false;
  }

  // Describe the hit to the mod.
  wasm::combat::DamageEvent event;
  event.set_victim(contact.victim_handle);
  event.set_attacker(contact.attacker_handle);
  event.set_inflictor(contact.inflictor_handle == UINT32_MAX ? 0 : contact.inflictor_handle);
  event.set_ability(contact.ability_handle);
  event.set_flags(contact.flags);
  event.set_hit_group(contact.hit_group);
  event.set_amount(contact.amount);

  // A hit the mod's own damage request causes arrives while the mod runs, and
  // the instance refuses it; the hit goes through unchanged.
  auto result = mod_.Damage(event);
  if (!result) {
    return false;
  }
  if (result->block()) {
    return true;
  }
  if (result->has_amount()) {
    adjusted_ = result->amount();
  }
  return false;
}

PlayerScope::Contact WasmPlugin::Contact(uint32_t attacker, uint32_t victim) const {
  // Only a limited mod pays for the players' slots.
  if (!scope_.Limited()) {
    return PlayerScope::Contact::kHear;
  }
  return scope_.Hit(game_.SlotOfPawn(attacker), game_.SlotOfPawn(victim));
}

void WasmPlugin::Fought(const gameinterop::CombatEvent& event, const wasm::StartResult& wants) {
  // Queue damage, casts, heals, levels, melee, parries, watched events and
  // movement facts under the lock, up to a bound.
  std::lock_guard lock(damaged_lock_);
  if (event.damage && (wants.damaged() || wants.kills()) && damaged_.size() < kMaxDamaged) {
    damaged_.push_back(*event.damage);
  }
  if (event.ability && wants.casts() && casts_.size() < kMaxDamaged) {
    casts_.push_back(*event.ability);
  }
  const bool typed = (event.heal && wants.heals()) || (event.level && wants.levels()) ||
                     (event.melee && wants.melees()) || (event.parry && wants.parries());
  if (typed && typed_.size() < kMaxDamaged) {
    typed_.push_back(event);
  }
  if (event.modifier && fired_.size() < kMaxDamaged && watched_.contains(event.modifier->id)) {
    fired_.push_back(*event.modifier);
  }
  if (facts_.size() >= kMaxDamaged) {
    return;
  }
  if (event.movement) {
    facts_.push_back({event.movement->pawn_handle, MovementActionOf(event.movement->execution)});
  }
  if (event.ability) {
    facts_.push_back({event.ability->caster_handle,
                      wasm::movement::MOVEMENT_ACTION_ABILITY_EXECUTED,
                      event.ability->ability_handle});
  }
  if (event.interaction) {
    facts_.push_back({.pawn = event.interaction->actor_handle,
                      .interaction = InteractionOf(*event.interaction)});
  }
}

void WasmPlugin::Sample(wasm::FrameEvent& frame) {
  // Sample once per tick.
  if (frame.tick() == sampled_) {
    return;
  }
  sampled_ = frame.tick();

  // Take the queued facts.
  std::vector<wasm::Game::MovementFact> facts;
  {
    std::lock_guard lock(damaged_lock_);
    facts.swap(facts_);
  }

  // Sample movement while the mod moves players.
  if (!game_.Moving()) {
    return;
  }
  for (auto& sample : game_.Movement(facts)) {
    *frame.add_movement() = std::move(sample);
  }
  const auto observed = engine_->ObservedInteractions();
  if (observed.pickups) {
    frame.add_interactions(wasm::movement::INTERACTION_KIND_PICKUP_COLLECTED);
  }
  if (observed.breaks) {
    frame.add_interactions(wasm::movement::INTERACTION_KIND_PROP_BROKEN);
  }
}

void WasmPlugin::DeliverDamaged() {
  std::vector<gameinterop::DamageTakenEvent> hits;
  {
    std::lock_guard lock(damaged_lock_);
    hits.swap(damaged_);
  }
  for (const auto& hit : hits) {
    // Stop once the mod is gone.
    if (!instance_) {
      return;
    }

    // Tell the mod about each hit it hears.
    if (Contact(hit.attacker_handle, hit.victim_handle) != PlayerScope::Contact::kHear) {
      continue;
    }
    if (wants_.damaged()) {
      wasm::combat::DamagedEvent event;
      event.set_victim(hit.victim_handle);
      event.set_attacker(hit.attacker_handle);
      event.set_ability(hit.ability_handle);
      event.set_health_lost(hit.health_lost);
      event.set_health_before(hit.health_before);
      event.set_dealt(hit.damage_dealt);
      static_cast<void>(mod_.Damaged(event));
    }

    // A hit that removed all the health its victim had left killed it.
    if (!instance_ || !wants_.kills() || hit.health_before <= 0 ||
        hit.health_lost < hit.health_before) {
      continue;
    }
    wasm::combat::KillEvent kill;
    kill.set_victim(hit.victim_handle);
    kill.set_victim_player(SlotOf(hit.victim_handle));
    kill.set_attacker(EntityOf(hit.attacker_handle));
    kill.set_attacker_player(SlotOf(hit.attacker_handle));
    kill.set_ability(EntityOf(hit.ability_handle));
    static_cast<void>(mod_.Killed(kill));
  }
}

bool WasmPlugin::Serves(uint32_t entity) const {
  if (!scope_.Limited()) {
    return true;
  }
  const auto slot = game_.SlotOfPawn(entity);
  return slot && scope_.Serves(*slot);
}

void WasmPlugin::DeliverCombat() {
  // Take what the game queued, and the names of the watched events.
  std::vector<gameinterop::AbilityExecutedEvent> casts;
  std::vector<gameinterop::CombatEvent> typed;
  std::vector<gameinterop::ModifierFiredEvent> fired;
  std::unordered_map<uint32_t, std::vector<std::string>> watched;
  {
    std::lock_guard lock(damaged_lock_);
    casts.swap(casts_);
    typed.swap(typed_);
    fired.swap(fired_);
    if (!fired.empty()) {
      watched = watched_;
    }
  }

  // Tell the mod about each cast it hears.
  for (const auto& cast : casts) {
    if (!instance_) {
      return;
    }
    if (Serves(cast.caster_handle)) {
      static_cast<void>(mod_.AbilityCast(
          game_.Cast(cast.caster_handle, cast.ability_handle, cast.target_handle)));
    }
  }

  // Then about each heal, level, swing and parry.
  for (const auto& event : typed) {
    if (!instance_) {
      return;
    }
    DeliverTyped(event);
  }

  // Then about each watched event, naming it as the mod did. A limited mod
  // hears the events of its own players' heroes.
  for (const auto& fact : fired) {
    if (!instance_) {
      return;
    }
    const auto names = watched.find(fact.id);
    if (names == watched.end() || (!Serves(fact.caster_handle) && !Serves(fact.target_handle))) {
      continue;
    }
    wasm::combat::ModifierFiredEvent event;
    event.set_caster(EntityOf(fact.caster_handle));
    event.set_player(SlotOf(fact.caster_handle));
    event.set_target(EntityOf(fact.target_handle));
    event.set_ability(EntityOf(fact.ability_handle));

    // Report it under each name the mod gave it.
    for (const auto& name : names->second) {
      if (!instance_) {
        return;
      }
      event.set_name(name);
      static_cast<void>(mod_.ModifierFired(event));
    }
  }
}

void WasmPlugin::DeliverTyped(const gameinterop::CombatEvent& fact) {
  // A heal goes to a mod that serves its target or healer.
  if (fact.heal) {
    const auto& heal = *fact.heal;
    if (!Serves(heal.target_handle) && !Serves(heal.healer_handle)) {
      return;
    }
    wasm::combat::HealEvent event;
    event.set_target(EntityOf(heal.target_handle));
    event.set_target_player(SlotOf(heal.target_handle));
    event.set_healer(EntityOf(heal.healer_handle));
    event.set_healer_player(SlotOf(heal.healer_handle));
    event.set_ability(EntityOf(heal.ability_handle));
    event.set_applied(heal.applied);
    event.set_requested(heal.requested);
    static_cast<void>(mod_.Healed(event));
    return;
  }

  // A level change goes to a mod that serves the hero.
  if (fact.level) {
    const auto& level = *fact.level;
    if (!Serves(level.hero_handle)) {
      return;
    }
    wasm::combat::LevelEvent event;
    event.set_hero(EntityOf(level.hero_handle));
    event.set_player(SlotOf(level.hero_handle));
    event.set_level(level.level);
    static_cast<void>(mod_.LevelChanged(event));
    return;
  }

  // A swing goes to a mod that serves its attacker or any entity it hit.
  if (fact.melee) {
    const auto& melee = *fact.melee;
    if (!Serves(melee.attacker_handle) &&
        std::ranges::none_of(melee.hit_handles, [this](uint32_t hit) { return Serves(hit); })) {
      return;
    }
    wasm::combat::MeleeEvent event;
    event.set_attacker(EntityOf(melee.attacker_handle));
    event.set_attacker_player(SlotOf(melee.attacker_handle));
    event.set_heavy(melee.heavy);
    for (const auto hit : melee.hit_handles) {
      event.add_hits(EntityOf(hit));
    }
    static_cast<void>(mod_.Melee(event));
    return;
  }

  // A parry goes to a mod that serves the parrier or the attacker.
  if (fact.parry) {
    const auto& parry = *fact.parry;
    if (!Serves(parry.parrier_handle) && !Serves(parry.attacker_handle)) {
      return;
    }
    wasm::combat::ParryEvent event;
    event.set_parrier(EntityOf(parry.parrier_handle));
    event.set_player(SlotOf(parry.parrier_handle));
    event.set_attacker(EntityOf(parry.attacker_handle));
    event.set_attacker_player(SlotOf(parry.attacker_handle));
    static_cast<void>(mod_.Parried(event));
  }
}

uint32_t WasmPlugin::EntityOf(uint32_t handle) {
  return handle == gameinterop::kInvalidEHandleIndex ? 0 : handle;
}

int32_t WasmPlugin::SlotOf(uint32_t handle) const {
  return game_.SlotOfPawn(handle).value_or(-1);
}

void WasmPlugin::DeliverGameEvents() {
  for (const auto& change : game_.TakeButtonChanges()) {
    // Tell the mod about each change while it runs.
    if (!instance_) {
      return;
    }
    wasm::InputEvent event;
    event.set_player(change.slot);
    event.set_pressed(change.pressed);
    event.set_released(change.released);
    static_cast<void>(mod_.Input(event));
  }
  for (const auto& launch : game_.TakeLaunches()) {
    if (!instance_) {
      return;
    }
    static_cast<void>(mod_.Launch(launch));
  }
  for (const auto& restored : game_.TakeRestored()) {
    // Tell the mod about each restore while it runs.
    if (!instance_) {
      return;
    }
    wasm::players::RestoredEvent event;
    event.set_player(restored.slot);
    event.set_error(restored.error);
    static_cast<void>(mod_.Restored(event));
  }
  if (auto restored = game_.TakeNpcsRestored(); restored && instance_) {
    wasm::npcs::NpcsRestoredEvent event;
    event.set_error(*restored);
    static_cast<void>(mod_.NpcsRestored(event));
  }
}

void WasmPlugin::Unsubscribe() {
  // Drop the player and combat subscriptions.
  players_.Reset();
  combat_.Reset();
  combat_unavailable_ = false;
  adjusted_.reset();
  sampled_.reset();

  // Drop the queued damage and facts, and stop the game reporting the
  // modifier events the mod watched.
  std::lock_guard lock(damaged_lock_);
  damaged_.clear();
  facts_.clear();
  casts_.clear();
  typed_.clear();
  fired_.clear();
  for (const auto& [id, names] : watched_) {
    engine_->UnwatchModifierEvent(id);
  }
  watched_.clear();
}

}  // namespace modlock::wasm
