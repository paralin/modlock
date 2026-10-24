//go:build deps_only

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

// kMaxDamaged bounds the applied hits and the movement facts queued between
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

// Departures forwards player departures to a plugin, which owns the
// subscription that holds it.
class Departures final : public gameinterop::ConnectionEventSink {
 public:
  explicit Departures(std::function<void(int32_t)> leave) : leave_(std::move(leave)) {}

  void OnConnected(int32_t, uint64_t, bool, const char*) override {}

  void OnDisconnecting(int32_t slot, uint64_t) override { leave_(slot); }

 private:
  std::function<void(int32_t)> leave_;
};

}  // namespace

void WasmPlugin::Move() {
  // Only a running QuakeWorld mod moves players, and only once.
  if (movement_ || !instance_ || manifest_.movement().model() != wasm::manifest::Movement::MODEL_QUAKEWORLD) {
    return;
  }

  // Build the server's movement model.
  auto movement = quake::PlayerMovement::Create(quake::PlayerMovement::Side::kServer, collision_,
                                                manifest_.movement().scale());
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
  if (listening_ || !instance_) {
    return;
  }

  // Offer the mod every player command: "/hello there" in chat arrives as
  // "hello there", as does the console command of the same line. Chat also
  // accepts ".", "+" or "-" in place of the slash, for players whose chat
  // keeps a slash line to itself.
  auto chats = engine_->OnChat([this](int32_t slot, std::string_view text) {
    if (!text.empty() && std::string_view("/.+-").contains(text.front())) {
      Command(slot, text.substr(1));
    }
  });
  if (!chats) {
    Log(std::string("player commands are unavailable: ") + chats.error());
    return;
  }
  players_.Add(std::move(*chats));
  listening_ = true;
  if (auto commands = engine_->OnCommand(
          [this](int32_t slot, std::string_view line) { return Command(slot, line); })) {
    players_.Add(std::move(*commands));
  } else {
    Log(std::string("console commands are unavailable: ") + commands.error());
  }
  if (auto departures = engine_->OnConnection(
          std::make_shared<Departures>([this](int32_t slot) { Leave(slot); }))) {
    players_.Add(std::move(*departures));
  } else {
    Log(std::string("interfaces may outlive their players: ") + departures.error());
  }
  std::cerr << name_ << ": player commands ready\n";
}

void WasmPlugin::ListenToCombat() {
  // Listen once, while a mod wants damage or moves players.
  if (fighting_ || !instance_ || (!wants_.damage() && !wants_.damaged() && !game_.Moving())) {
    return;
  }

  // A mod that wants damage can block or change each hit.
  std::function<bool(const gameinterop::DamageContactEvent&)> suppress;
  gameinterop::AdjustDamage adjust;
  if (wants_.damage()) {
    suppress = [this](const gameinterop::DamageContactEvent& contact) {
      return Hit(contact);
    };
    adjust = [this](const gameinterop::DamageContactEvent&, float& amount) {
      if (adjusted_) {
        amount = *std::exchange(adjusted_, std::nullopt);
      }
    };
  }

  // Subscribe to combat.
  auto combat =
      engine_->OnCombat([this, damaged = wants_.damaged()](
                            const gameinterop::CombatEvent& event) { Fought(event, damaged); },
                        std::move(suppress), std::move(adjust));
  if (!combat) {
    Log(std::string("damage events are unavailable: ") + combat.error());
    return;
  }
  combat_ = std::move(*combat);
  fighting_ = true;
}

bool WasmPlugin::Hit(const gameinterop::DamageContactEvent& contact) {
  // Forget the last adjustment.
  adjusted_.reset();

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

void WasmPlugin::Fought(const gameinterop::CombatEvent& event, bool damaged) {
  // Queue damage and movement facts under the lock, up to a bound.
  std::lock_guard lock(damaged_lock_);
  if (event.damage && damaged && damaged_.size() < kMaxDamaged) {
    damaged_.push_back(*event.damage);
  }
  if (facts_.size() >= kMaxDamaged) {
    return;
  }
  if (event.movement) {
    facts_.push_back({event.movement->pawn_handle, MovementActionOf(event.movement->execution)});
  }
  if (event.ability) {
    facts_.push_back({event.ability->caster_handle, wasm::movement::MOVEMENT_ACTION_ABILITY_EXECUTED,
                      event.ability->ability_handle});
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

    // Tell the mod about each hit.
    wasm::combat::DamagedEvent event;
    event.set_victim(hit.victim_handle);
    event.set_attacker(hit.attacker_handle);
    event.set_ability(hit.ability_handle);
    event.set_health_lost(hit.health_lost);
    event.set_health_before(hit.health_before);
    event.set_dealt(hit.damage_dealt);
    static_cast<void>(mod_.Damaged(event));
  }
}

void WasmPlugin::DeliverGameEvents() {
  for (const auto& change : game_.TakeButtonChanges()) {
    // Tell the mod about each change while it runs.
    if (!instance_) {
      return;
    }
    wasm::input::InputEvent event;
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
  listening_ = false;
  combat_.Reset();
  fighting_ = false;
  adjusted_.reset();
  sampled_.reset();

  // Drop the queued damage and facts.
  std::lock_guard lock(damaged_lock_);
  damaged_.clear();
  facts_.clear();
}

}  // namespace modlock::wasm
