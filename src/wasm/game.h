#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "modlock/engine_host.h"
#include "modlock/gameinterop/ability_input_hook.h"
#include "modlock/gameinterop/ability_modifier.h"
#include "modlock/gameinterop/ability_slots.h"
#include "modlock/gameinterop/bot_creation.h"
#include "modlock/gameinterop/bot_hero_guard.h"
#include "modlock/gameinterop/command_hook.h"
#include "modlock/gameinterop/engine_server.h"
#include "modlock/gameinterop/entity_abi.h"
#include "modlock/gameinterop/entity_dynamics.h"
#include "modlock/gameinterop/free_for_all_relations.h"
#include "modlock/gameinterop/full_updates.h"
#include "modlock/gameinterop/game_rules.h"
#include "modlock/gameinterop/hero_definitions.h"
#include "modlock/gameinterop/mapped_module_image.h"
#include "modlock/gameinterop/match_clock.h"
#include "modlock/gameinterop/movement_hook.h"
#include "modlock/gameinterop/native_damage.h"
#include "modlock/gameinterop/native_user_messages.h"
#include "modlock/gameinterop/pawn_observer.h"
#include "modlock/gameinterop/projectile_impact_hook.h"
#include "modlock/gameinterop/transmit_hook.h"
#include "modlock/gameinterop/voice_hook.h"
#include "modlock/gameinterop/world_entities.h"
#include "modlock/render/world_effect_game_factory.h"
#include "modlock/render/world_text_game_factory.h"
#include "src/wasm/wasm.pb.h"
#include "wasm/apart_players.h"
#include "wasm/camera.h"
#include "wasm/controller_table.h"
#include "wasm/effects.h"
#include "wasm/fields.h"
#include "wasm/hero_movement.h"
#include "wasm/hero_restore.h"
#include "wasm/heroes.h"
#include "wasm/hook_sources.h"
#include "wasm/host_service.gen.h"
#include "wasm/input.h"
#include "wasm/input_changes.h"
#include "wasm/match.h"
#include "wasm/modifier_holds.h"
#include "wasm/module.h"
#include "wasm/npc_restore.h"
#include "wasm/projectiles.h"
#include "wasm/restores.h"
#include "wasm/roster.h"
#include "wasm/seating.h"
#include "wasm/team_seating.h"
#include "wasm/voice.h"
#include "wasm/world.h"

namespace modlock::wasm {

class Game;

// GameServices holds the engine services a host's mods share. Each resolves on
// first use and stays for the host's lifetime; a failed resolution is retried
// on the next use, so a service that needs a loaded world becomes available
// once one loads. Everything runs on the engine thread.
class GameServices {
 public:
  // Server returns the engine server, for console commands and the clock.
  std::expected<const gameinterop::EngineServer*, std::string> Server();

  // ServerCommand runs one console command line. A line that sets a variable
  // may set a development-only or cheat-protected one. Until the engine starts
  // its first world the console is not ready, so commands wait and run in
  // order when WorldStarting reports it.
  std::expected<void, std::string> ServerCommand(std::string command);

  // WorldStarting runs the waiting console commands before a world's map
  // spawns.
  void WorldStarting();

  // Messages returns the user-message sender.
  std::expected<const gameinterop::NativeUserMessages*, std::string> Messages();

  // FullUpdates returns the full update sender for connected clients.
  std::expected<const gameinterop::FullUpdates*, std::string> FullUpdates();

  // WorldReady installs the input hook when a mod asked for input before the
  // first world loaded, and runs the waiting console commands when the mods
  // loaded after the world started.
  void WorldReady();

  // StepWith runs step for every hero's server movement that no Steer
  // replays, as a mod's own movement model does; an empty step stops it. One
  // mod's step may run at a time.
  std::expected<void, std::string> StepWith(gameinterop::MovementHook::Handler step);

  // Pause, Clock and Rift return the game's pause, match clock and rift
  // rules.
  std::expected<const gameinterop::GamePause*, std::string> Pause();
  std::expected<gameinterop::MatchClock*, std::string> Clock();
  std::expected<gameinterop::KothRules*, std::string> Rift();

  // Controllers holds the controllers players' clients stream for the mods
  // that watch them.
  ControllerTable& Controllers() { return controllers_; }

  // The source sets the host-wide hooks read. A mod's game adds itself to
  // each while it runs, and removes itself before it goes.
  Sources<InputChanges>& InputSources() { return inputs_; }

  Sources<MoverSource>& MoverSources() { return movers_; }

  Sources<CameraSource>& CameraSources() { return cameras_; }

  Sources<TransmitSource>& TransmitSources() { return transmitters_; }

  Sources<ProjectileSource>& ProjectileSources() { return projectile_sources_; }

  Sources<VoiceSource>& VoiceSources() { return voices_; }

  // The services below resolve on first use, as the class comment describes;
  // each module asks for the ones its calls need.

  // ModifierLayout locates an entity's modifier list and state masks.
  struct ModifierLayout {
    size_t property = 0;
    size_t list = 0;
    size_t mask = 0;
    size_t predicted_mask = 0;
    size_t mask_size = 0;
  };

  std::expected<const gameinterop::ModuleImage*, std::string> Image();
  std::expected<void*, std::string> Schema();
  std::expected<const gameinterop::HeroDefinitions*, std::string> Heroes();
  std::expected<const gameinterop::WorldEntities::Calls*, std::string> EntityCalls();
  std::expected<const gameinterop::PlayerSelectionCalls*, std::string> Selection();
  std::expected<const gameinterop::AbilityDefinitions*, std::string> Abilities();
  std::expected<const gameinterop::AbilityModifier::Calls*, std::string> ModifierCalls();
  std::expected<const gameinterop::ItemFunctions*, std::string> Items();
  std::expected<const gameinterop::ImbueFunctions*, std::string> Imbues();
  std::expected<const gameinterop::TrainFunctions*, std::string> Training();
  std::expected<const gameinterop::BuyFunctions*, std::string> Shop();
  std::expected<const gameinterop::AbilitySlots*, std::string> Slots();
  std::expected<gameinterop::CreateAbility, std::string> CreateAbility();
  std::expected<gameinterop::RespawnPawn, std::string> Respawn();
  std::expected<gameinterop::ChangeTeamKeepingHero, std::string> ChangeTeam();
  std::expected<gameinterop::ResetHeroPawn, std::string> ResetHero();
  std::expected<gameinterop::TeleportClientCamera, std::string> Teleport();
  std::expected<gameinterop::WarpOntoZipline, std::string> Zipline();
  std::expected<const gameinterop::PawnMotion*, std::string> Motion();
  std::expected<const gameinterop::EntityDynamics*, std::string> Dynamics();
  std::expected<const gameinterop::BotCreation*, std::string> Bots();
  std::expected<gameinterop::FreeForAllRelations*, std::string> Relations();
  // DisableRelations restores the stock team relations, if Relations
  // installed them.
  void DisableRelations();
  std::expected<render::WorldTextGameFactory*, std::string> Text();
  std::expected<render::WorldEffectGameFactory*, std::string> Effects();

  // SweepEffects lets the effect factory release the effects that ended by
  // now, and InvalidateEffects forgets those the engine freed with the world.
  // Neither builds the factory.
  void SweepEffects(std::chrono::steady_clock::time_point now) {
    if (effects_) {
      effects_->Sweep(now);
    }
  }

  void InvalidateEffects() {
    if (effects_) {
      effects_->InvalidateAfterEngineReset();
    }
  }

  std::expected<const gameinterop::NativeDamage*, std::string> Damage();
  std::expected<gameinterop::ModifyCurrency, std::string> Currency();
  std::expected<gameinterop::RequiredSouls, std::string> RequiredSouls();
  // Rules returns the game rules object, which the rules proxy entity points
  // to.
  std::expected<void*, std::string> Rules();
  std::expected<gameinterop::SchemaField, std::string> Field(const std::string& class_name,
                                                             const std::string& field);
  // Derives reports whether the schema class class_info is class_name or
  // derives from it, caching each answer.
  bool Derives(const void* class_info, const std::string& class_name);
  std::expected<ModifierLayout, std::string> Modifiers();
  std::expected<int64_t, std::string> ModifierState(const std::string& name);

  // Input installs the shared input hook once a world has loaded, or marks
  // it wanted until then.
  void Input();

  // Silenced resolves the named voice concepts, caching each value. It
  // returns those it knows and, in error, the names it does not.
  std::pair<std::set<int32_t>, std::string> Silenced(
      const google::protobuf::RepeatedPtrField<std::string>& names);

  // Voice installs the shared voice hook on first use.
  std::expected<void, std::string> Voice();

  // Projectiles installs the shared projectile impact hook once a world has
  // loaded, or marks it wanted until then.
  void Projectiles();

  // Transmit installs the shared transmit hook on first use.
  std::expected<void, std::string> Transmit();

  // Commands installs the shared user command hook on first use.
  std::expected<void, std::string> Commands();

  // CameraPosition is the camera position of the player in slot, as the
  // command hook last read it; the first call installs the hook.
  std::optional<std::array<float, 3>> CameraPosition(int32_t slot);

  // Steps installs or removes the server movement hook to match what the
  // mods need of it.
  std::expected<void, std::string> Steps();

  // Command takes the commands pawn ran since the previous call, folded.
  std::optional<movement::MovementCommand> Command(const void* pawn);

 private:
  friend class Game;

  // FieldKey names one schema field as class and field.
  using FieldKey = std::pair<std::string, std::string>;

  // Cache fills cache from resolve the first time a caller asks and keeps it.
  // resolve takes the server image, and the schema system when it asks for
  // one. A failure leaves the cache empty, so the next caller retries.
  template <typename Value, typename Resolve>
  std::expected<void, std::string> Cache(Value& cache, Resolve resolve);

  // Filter applies every mod's presses, remaps and blocks to one player's input.
  uint64_t Filter(gameinterop::AbilityInputHook::Input& input);

  // Speaks reports whether no mod silences concept_value for speaker.
  bool Speaks(void* speaker, int32_t concept_value);

  // Impact runs the game's impact of a projectile and reports it to each mod
  // that watches it. When one keeps momentum, every live hero gets back the
  // motion it had before the game's explosion.
  void Impact(void* entity, const gameinterop::TraceResult& contact,
              const gameinterop::ProjectileImpactHook::NativeImpact& native);

  // Driven returns the command the first mod that drives slot gives it, and
  // spends that command's presses.
  std::optional<gameinterop::CommandHook::Command> Driven(int32_t slot);

  // Observed records a player's button changes from their commands, which
  // arrive whatever their hero does: dead, spectating, or parked while the
  // player watches through another camera. It returns the spectator buttons
  // the games block for the player, which the commands then withhold.
  uint64_t Observed(int32_t slot, uint64_t pressed, uint64_t released);

  // Record queues a player's button changes for every game, once: a change
  // the other path (the input hook or the commands) recorded in the last two
  // ticks is dropped, since both see the same press.
  void Record(int32_t slot, uint64_t pressed, uint64_t released);

  // Tick is the server's simulation tick, or nothing when it cannot be read.
  std::optional<int32_t> Tick();

  // Withhold keeps from one player every object a mod hid from them and
  // every entity a mod shows only to others.
  void Withhold(gameinterop::TransmitSet& set);

  // Step records the hero's command, replays a mod's steer over it, and runs
  // step_ otherwise; it reports whether it replaced the step's motion.
  bool Step(gameinterop::MovementCall& call);

  // Face points a bot's step and hero along angles in place of the view its
  // own command set: the step's view, the hero's aim, and the body's yaw.
  void Face(MoverSource& mover, gameinterop::MovementCall& call,
            const std::array<float, 3>& angles);

  std::optional<gameinterop::MappedModuleImage> image_;
  std::optional<gameinterop::EngineServer> server_;
  std::optional<gameinterop::NativeUserMessages> messages_;
  std::optional<gameinterop::FullUpdates> full_updates_;
  std::optional<gameinterop::HeroDefinitions> heroes_;
  // world_calls_ are the entity calls every mod's world builds on; scanning
  // for them takes hundreds of milliseconds, so they resolve once per host.
  std::optional<gameinterop::WorldEntities::Calls> world_calls_;
  std::optional<gameinterop::PlayerSelectionCalls> selection_;
  std::optional<gameinterop::AbilityDefinitions> abilities_;
  // modifier_calls_ are the calls and layout every modifier shares; resolving
  // them scans server.dll, so they resolve once per host, not per modifier.
  std::optional<gameinterop::AbilityModifier::Calls> modifier_calls_;
  std::optional<gameinterop::ItemFunctions> items_;
  std::optional<gameinterop::ImbueFunctions> imbues_;
  std::optional<gameinterop::TrainFunctions> training_;
  std::optional<gameinterop::BuyFunctions> shop_;
  std::optional<gameinterop::AbilitySlots> slots_;
  gameinterop::CreateAbility create_ability_ = nullptr;
  gameinterop::RespawnPawn respawn_ = nullptr;
  gameinterop::ChangeTeamKeepingHero change_team_ = nullptr;
  gameinterop::ResetHeroPawn reset_hero_ = nullptr;
  gameinterop::TeleportClientCamera teleport_ = nullptr;
  gameinterop::WarpOntoZipline zipline_ = nullptr;
  std::optional<gameinterop::PawnMotion> motion_;
  std::optional<gameinterop::EntityDynamics> dynamics_;
  std::optional<gameinterop::BotCreation> bots_;
  // bot_heroes_ lets bots keep a human's hero once a mod adds one.
  std::optional<gameinterop::BotHeroGuard> bot_heroes_;
  // relations_ makes teammates enemies while a mod runs a free-for-all.
  std::optional<gameinterop::FreeForAllRelations> relations_;
  std::unique_ptr<render::WorldTextGameFactory> text_;
  std::unique_ptr<render::WorldEffectGameFactory> effects_;
  std::optional<gameinterop::NativeDamage> damage_;
  gameinterop::ModifyCurrency currency_ = nullptr;
  gameinterop::RequiredSouls required_souls_ = nullptr;
  std::optional<gameinterop::GamePause> pause_;
  std::optional<gameinterop::MatchClock> clock_;
  std::optional<gameinterop::KothRules> rift_;
  void* const* rules_ = nullptr;
  std::map<FieldKey, gameinterop::SchemaField> fields_;
  std::map<std::pair<const void*, std::string>, bool> derives_;
  std::optional<ModifierLayout> modifiers_;
  std::map<std::string, int64_t, std::less<>> modifier_states_;
  // The running mods' sources, one set per hook that reads them.
  Sources<InputChanges> inputs_;
  Sources<MoverSource> movers_;
  Sources<CameraSource> cameras_;
  Sources<TransmitSource> transmitters_;
  Sources<ProjectileSource> projectile_sources_;
  Sources<VoiceSource> voices_;
  std::optional<gameinterop::AbilityInputHook> input_;
  bool input_wanted_ = false;
  // held_ is each player's held buttons before blocking, for blocked edges.
  std::array<uint64_t, gameinterop::kMaxPlayerSlots> held_{};

  // recorded_ is each player's changes recorded since a tick, so the input
  // hook and the commands record a press once between them (Record).
  struct Recorded {
    int32_t tick = 0;
    uint64_t pressed = 0;
    uint64_t released = 0;
  };

  std::array<Recorded, gameinterop::kMaxPlayerSlots> recorded_{};
  std::optional<gameinterop::ProjectileImpactHook> impacts_;
  // voice_ drops the voice lines mods silence, installed the first time one
  // does; voice_concepts_ caches each concept name's value.
  std::optional<gameinterop::VoiceHook> voice_;
  std::map<std::string, int32_t, std::less<>> voice_concepts_;
  bool impacts_wanted_ = false;
  // transmit_ filters what each player's client receives once a mod hides
  // an object from someone.
  std::optional<gameinterop::TransmitHook> transmit_;
  // usercmds_ reads the players' cameras and rewrites driven bots' commands
  // once a mod reads a pawn or drives a bot.
  std::optional<gameinterop::CommandHook> usercmds_;
  std::optional<std::string> usercmds_error_;
  // steps_ is the server movement hook, held while step_ runs or a mod
  // watches or steers a hero. commands_ folds each pawn's commands since its
  // last sample, and ground_ is the last entity any hero stood on.
  std::optional<gameinterop::MovementHook> steps_;
  gameinterop::MovementHook::Handler step_;

  struct Folded {
    movement::MovementCommand command;
    bool stepped = false;
  };

  std::map<const void*, Folded> commands_;
  uint32_t ground_ = gameinterop::MovementHook::kNoGround;
  bool world_ready_ = false;
  // pending_commands_ wait for the engine's first world; started_ marks it.
  std::vector<std::string> pending_commands_;
  bool started_ = false;
  ControllerTable controllers_;
};

// Game answers one mod's calls into the game and owns what the mod placed in
// it: world objects, units, bots, frozen heroes and input changes. Clear and
// WorldEnding remove them, so a stopped or reloaded mod leaves nothing behind.
// Its modules answer the mod's Host calls through Calls; the calls that reach
// beyond the game, such as Log, Ui and CallService, belong to a subclass.
class Game {
 public:
  Game(GameServices& services, EngineHost* engine);
  virtual ~Game();

  Game(const Game&) = delete;
  Game& operator=(const Game&) = delete;

  // Calls returns the modules that answer each part of the mod's Host calls.
  // The plugin part, the calls that reach beyond the game, is left to a
  // subclass.
  HostCalls Calls();

  // Frame finishes placing the units the mod spawned last frame and advances
  // its hero restores.
  void Frame();

  using ButtonChange = InputChanges::ButtonChange;

  // TakeButtonChanges returns the watched button changes since the last call,
  // in the order players made them.
  std::vector<ButtonChange> TakeButtonChanges() { return input_.TakeButtonChanges(); }

  // TakeRestored returns the hero restores that ended since the last call.
  std::vector<Restores::Restored> TakeRestored() { return restores_.TakeRestored(); }

  // TakeNpcsRestored returns the result of the unit restore that ended since
  // the last call: an empty string when the units hold their targets.
  std::optional<std::string> TakeNpcsRestored() { return restores_.TakeNpcsRestored(); }

  // WorldEnding removes the mod's objects, units and bots and ends its
  // restores and match clock hold before the world goes. The mod's input
  // blocks stay for the next world.
  void WorldEnding();

  // WorldReady resolves at map load what a mod's calls would otherwise look
  // up in the server module on first use, every recorded signature included,
  // so no first call stalls a frame with players in it.
  void WorldReady();

  // Leave forgets what the mod set for the player in slot, who left, so the
  // next player there starts clear.
  void Leave(int32_t slot);

  // Clear removes everything the mod placed, ends its seating, releases its
  // frozen heroes, held modifiers, input changes and match clock hold, resumes a game it
  // paused and stops watching projectiles.
  void Clear();

  // Cast describes one executed ability for the mod: the caster's player,
  // and the ability's name and slot when a player's hero cast it.
  abilities::AbilityCastEvent Cast(uint32_t caster, uint32_t ability, uint32_t target) {
    return heroes_.Cast(caster, ability, target);
  }

  // Apart hides the heroes of the players in the slot bits from every other
  // player, and every other player's hero from them, until the next call; zero
  // shows every hero to everyone again. It survives Clear and world changes,
  // since it follows the mod's scope rather than what the mod placed.
  std::expected<void, std::string> Apart(uint64_t slots) { return apart_.Set(slots); }

  // Departing reports whether a bot the mod removed still holds its slot.
  // The engine finishes the disconnect of a client it is processing after
  // the call returns, so the slot can stay held for a moment.
  bool Departing() { return world_.Departing(); }

  // SlotOfPawn returns the slot of the player whose hero the entity handle
  // names, or no value for any other entity.
  std::optional<int32_t> SlotOfPawn(uint32_t handle) const { return players_.SlotOfPawn(handle); }

  // Heroes returns every live hero this frame.
  std::vector<wasm::Roster::Hero> Heroes() { return players_.Heroes(); }

  // AssignTeams applies host seats on join and assignment change. The table
  // survives Clear, so a build reload keeps confirmed seats and pending joins.
  std::expected<void, std::string> AssignTeams(const wasm::TeamAssignments& request) {
    return seating_.AssignTeams(request);
  }

  // StartFreeForAll sets whether free-for-all is on as a mod starts, from its
  // manifest, and turns team seating off. SetFreeForAll and SetTeamSeating
  // change them while the mod runs.
  void StartFreeForAll(bool enabled) { seating_.StartFreeForAll(enabled); }

  // StartContentAddon names the content addon of the mod that starts, which
  // CreateEntity writes in place of {addon} in a text key value, and
  // CreateModel and CreateParticle in a resource path, so a world panel names
  // its layout as file://{resources}/layout/{addon}/panel.xml and a model its
  // resource as models/{addon}/pawn.vmdl.
  void StartContentAddon(std::string addon) { world_.StartContentAddon(std::move(addon)); }

  // WithAddon returns text with the mod's content addon in place of each
  // {addon}.
  std::string WithAddon(std::string text) const { return world_.WithAddon(std::move(text)); }

  // FreeForAllCommand reports whether free-for-all consumes a player's console
  // line. Seating is the host's, so it drops the team commands.
  bool FreeForAllCommand(std::string_view line) const { return seating_.FreeForAllCommand(line); }

  // ClientCommand notes a console command a player sent the game that the
  // host follows: a selecthero pick from the game's Change Hero, which
  // SelectHero leaves to land.
  void ClientCommand(int32_t slot, std::string_view line) { heroes_.ClientCommand(slot, line); }

  // Moving reports whether the mod watches any player's movement.
  bool Moving() const { return movement_.Moving(); }

  // Movement returns a sample of each watched, live hero, with the facts
  // that name its pawn.
  std::vector<movement::MovementSample> Movement(
      std::span<const wasm::HeroMovement::MovementFact> facts) {
    return movement_.Samples(facts);
  }

  // Controllers appends each player whose controllers the mod watches, with
  // the presses and releases since its last call.
  void Controllers(google::protobuf::RepeatedPtrField<controllers::PlayerControllers>* out) {
    input_.Controllers(out);
  }

  // TakeLaunches returns the watched projectiles that appeared since the last
  // call.
  std::vector<combat::LaunchEvent> TakeLaunches() { return projectiles_.TakeLaunches(); }

 protected:
  // Serves reports whether this game currently controls the player's slot.
  virtual bool Serves(int32_t) const { return true; }

  // Describe adds what a host knows of each player to the list the Players
  // call returns.
  virtual void Describe(players::PlayersResponse&) {}

  // Impacted delivers a watched projectile's impact to the mod, inside the
  // game's impact.
  virtual void Impacted(const combat::ImpactEvent& event) = 0;

 private:
  // services_ outlives the game.
  GameServices& services_;
  // engine_ outlives the game; the host owns it.
  EngineHost* engine_;

  // The modules own their domains' state. modules_ lists them in the order
  // they are built, each after the modules it reads. WorldReady runs them in
  // that order; Frame, WorldEnding, Leave and Clear run them in reverse, so a
  // module's frame work and release come before those of the modules it reads.
  Roster players_{services_, [this](players::PlayersResponse& players) { Describe(players); }};
  wasm::World world_{services_, engine_};
  Match match_{services_, world_};
  ApartPlayers apart_{services_, players_};
  Fields fields_{services_, world_};
  ModifierHolds modifiers_{services_, players_, world_};
  wasm::Heroes heroes_{services_, players_};
  Camera camera_{services_, players_, world_};
  wasm::HeroMovement movement_{services_, players_, world_, modifiers_};
  wasm::Input input_{services_};
  Projectiles projectiles_{services_, players_,
                           [this](const combat::ImpactEvent& event) { Impacted(event); }};
  Restores restores_{services_, players_, world_, modifiers_, heroes_};
  Seating seating_{services_, players_, modifiers_, [this](int32_t slot) { return Serves(slot); }};
  wasm::Voice voice_{services_, players_};
  // effects_ keeps no state, so it is not one of the modules.
  Effects effects_{services_, engine_, players_};
  std::array<Module*, 14> modules_{&players_,     &world_,    &match_,   &apart_,    &fields_,
                                   &modifiers_,   &heroes_,   &camera_,  &movement_, &input_,
                                   &projectiles_, &restores_, &seating_, &voice_};
};

}  // namespace modlock::wasm
