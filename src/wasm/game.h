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
#include "wasm/players.h"
#include "wasm/projectiles.h"
#include "wasm/team_seating.h"
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
  std::expected<const gameinterop::WorldEntities::Calls*, std::string> WorldCalls();
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
// The calls that reach beyond the game, Log, Ui and CallService, belong to a
// subclass.
class Game : public HostService, private VoiceSource {
 public:
  Game(GameServices& services, EngineHost* engine);
  ~Game() override;

  Game(const Game&) = delete;
  Game& operator=(const Game&) = delete;

  // Frame finishes placing the units the mod spawned last frame and advances
  // its hero restores.
  void Frame();

  using ButtonChange = InputChanges::ButtonChange;

  // TakeButtonChanges returns the watched button changes since the last call,
  // in the order players made them.
  std::vector<ButtonChange> TakeButtonChanges() { return input_.TakeButtonChanges(); }

  // Restored is the result of one player's hero restore; an empty error
  // means the target held.
  struct Restored {
    int32_t slot = 0;
    std::string error;
  };

  // TakeRestored returns the hero restores that ended since the last call.
  std::vector<Restored> TakeRestored();

  // TakeNpcsRestored returns the result of the unit restore that ended since
  // the last call: an empty string when the units hold their targets.
  std::optional<std::string> TakeNpcsRestored();

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
  std::vector<wasm::Players::Hero> Heroes() { return players_.Heroes(); }

  // AssignTeams applies host seats on join and assignment change. The table
  // survives Clear, so a build reload keeps confirmed seats and pending joins.
  std::expected<void, std::string> AssignTeams(const wasm::TeamAssignments& request);

  // StartFreeForAll sets whether free-for-all is on as a mod starts, from its
  // manifest, and turns team seating off. SetFreeForAll and SetTeamSeating
  // change them while the mod runs.
  void StartFreeForAll(bool enabled);

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
  bool FreeForAllCommand(std::string_view line) const;

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

  // Players lists the player slots with a seat, connected or away; a host may
  // add what it knows of each player.
  std::expected<players::PlayersResponse, std::string> Players() override {
    return players_.List();
  }

  // Impacted delivers a watched projectile's impact to the mod, inside the
  // game's impact.
  virtual void Impacted(const combat::ImpactEvent& event) = 0;

 private:
  // What the host-wide hooks read of this game, through the source
  // interfaces it registers with services_ (game_sources.cc).
  bool Silences(int32_t concept_value) const override;
  bool SilencesSpeaker(uint32_t speaker, int32_t concept_value) const override;

  // The calls, in the order the Host service declares them.
  std::expected<void, std::string> ServerCommand(const ServerCommandRequest& request) override;
  std::expected<void, std::string> Chat(const effects::ChatRequest& request) override;
  std::expected<void, std::string> CenterText(const effects::CenterTextRequest& request) override;
  std::expected<void, std::string> Announce(const effects::AnnounceRequest& request) override;

  std::expected<void, std::string> Precache(const PrecacheOptions& request) override {
    return world_.Precache(request);
  }

  std::expected<players::PawnResponse, std::string> Pawn(
      const players::PlayerRequest& request) override {
    return players_.Pawn(request);
  }

  std::expected<players::PawnsResponse, std::string> Pawns() override { return players_.Pawns(); }

  std::expected<players::HeroResponse, std::string> SelectHero(
      const players::SelectHeroRequest& request) override {
    return heroes_.SelectHero(request);
  }

  std::expected<void, std::string> Spectate(const players::PlayerRequest& request) override {
    return camera_.Spectate(request);
  }

  std::expected<void, std::string> Watch(const players::WatchRequest& request) override {
    return camera_.Watch(request);
  }

  std::expected<void, std::string> FreeCamera(const players::FreeCameraRequest& request) override {
    return camera_.FreeCamera(request);
  }

  std::expected<void, std::string> MoveCamera(const players::CameraOptions& request) override {
    return camera_.MoveCamera(request);
  }

  std::expected<void, std::string> ClearCamera(
      const players::ClearCameraRequest& request) override {
    return camera_.ClearCamera(request);
  }

  std::expected<void, std::string> Unwatch(const players::PlayerRequest& request) override {
    return camera_.Unwatch(request);
  }

  std::expected<void, std::string> Respawn(const players::PlayerRequest& request) override {
    return heroes_.Respawn(request);
  }

  std::expected<void, std::string> ChangeTeam(const players::ChangeTeamRequest& request) override {
    return heroes_.ChangeTeam(request);
  }

  std::expected<void, std::string> ClearItems(const players::PlayerRequest& request) override {
    return heroes_.ClearItems(request);
  }

  std::expected<void, std::string> Freeze(const players::FreezeRequest& request) override {
    return heroes_.Freeze(request);
  }

  std::expected<void, std::string> RestoreStamina(const players::PlayerRequest& request) override {
    return heroes_.RestoreStamina(request);
  }

  std::expected<void, std::string> SetStamina(const players::SetStaminaRequest& request) override {
    return heroes_.SetStamina(request);
  }

  std::expected<void, std::string> RefreshAbility(
      const abilities::RefreshAbilityRequest& request) override {
    return heroes_.RefreshAbility(request);
  }

  std::expected<abilities::AbilitiesResponse, std::string> Abilities(
      const players::PlayerRequest& request) override {
    return heroes_.Abilities(request);
  }

  std::expected<void, std::string> SetAbility(const abilities::AbilityOptions& request) override {
    return heroes_.SetAbility(request);
  }

  std::expected<void, std::string> GiveItem(const abilities::GiveItemRequest& request) override {
    return heroes_.GiveItem(request);
  }

  std::expected<void, std::string> TrainAbility(
      const abilities::TrainAbilityRequest& request) override {
    return heroes_.TrainAbility(request);
  }

  std::expected<void, std::string> BuyItem(const abilities::BuyItemRequest& request) override {
    return heroes_.BuyItem(request);
  }

  std::expected<abilities::HeroBuildResponse, std::string> HeroBuild(
      const abilities::HeroBuildRequest& request) override {
    return heroes_.HeroBuild(request);
  }

  std::expected<void, std::string> ReplaceAbility(
      const abilities::ReplaceAbilityRequest& request) override {
    return heroes_.ReplaceAbility(request);
  }

  std::expected<void, std::string> HoldModifier(
      const abilities::HoldModifierRequest& request) override {
    return modifiers_.HoldModifier(request);
  }

  std::expected<void, std::string> GiveModifier(
      const abilities::GiveModifierRequest& request) override {
    return modifiers_.GiveModifier(request);
  }

  std::expected<void, std::string> HoldEntityModifier(
      const abilities::HoldEntityModifierRequest& request) override {
    return modifiers_.HoldEntityModifier(request);
  }

  std::expected<void, std::string> GiveEntityModifier(
      const abilities::GiveEntityModifierRequest& request) override {
    return modifiers_.GiveEntityModifier(request);
  }

  std::expected<void, std::string> Teleport(const movement::TeleportRequest& request) override {
    return movement_.Teleport(request);
  }

  std::expected<void, std::string> Zipline(const movement::ZiplineRequest& request) override {
    return movement_.Zipline(request);
  }

  std::expected<void, std::string> MovePlayer(const movement::MovePlayerRequest& request) override {
    return movement_.MovePlayer(request);
  }

  std::expected<void, std::string> Steer(const movement::SteerRequest& request) override {
    return movement_.Steer(request);
  }

  std::expected<void, std::string> Drive(const movement::DriveRequest& request) override {
    return movement_.Drive(request);
  }

  std::expected<void, std::string> AdjustSouls(
      const players::AdjustSoulsRequest& request) override {
    return heroes_.AdjustSouls(request);
  }

  std::expected<void, std::string> AdjustAbilityPoints(
      const players::AdjustAbilityPointsRequest& request) override {
    return heroes_.AdjustAbilityPoints(request);
  }

  std::expected<void, std::string> StartingSouls(
      const players::StartingSoulsRequest& request) override {
    return heroes_.StartingSouls(request);
  }

  std::expected<void, std::string> ResetHero(const players::ResetHeroRequest& request) override {
    return heroes_.ResetHero(request);
  }

  std::expected<void, std::string> Heal(const players::HealRequest& request) override;
  std::expected<effects::SoundResponse, std::string> Sound(
      const effects::SoundRequest& request) override;
  std::expected<void, std::string> RestoreHero(const players::RestoreHeroRequest& request) override;
  std::expected<void, std::string> ScreenEffect(
      const effects::ScreenEffectRequest& request) override;
  std::expected<void, std::string> ClearScreenEffect(
      const effects::ClearScreenEffectRequest& request) override;
  std::expected<void, std::string> SilenceVoice(
      const effects::SilenceVoiceRequest& request) override;
  std::expected<void, std::string> SilencePlayerVoice(
      const effects::SilencePlayerVoiceRequest& request) override;

  std::expected<fields::FieldResponse, std::string> ReadField(
      const fields::ReadFieldRequest& request) override {
    return fields_.ReadField(request);
  }

  std::expected<void, std::string> WriteField(const fields::WriteFieldRequest& request) override {
    return fields_.WriteField(request);
  }

  std::expected<fields::FieldListResponse, std::string> ReadFieldList(
      const fields::ReadFieldRequest& request) override {
    return fields_.ReadFieldList(request);
  }

  std::expected<fields::FieldResponse, std::string> ReadModifierField(
      const fields::ReadModifierFieldRequest& request) override {
    return modifiers_.ReadModifierField(request);
  }

  std::expected<abilities::ModifiersResponse, std::string> Modifiers(
      const entities::EntityRequest& request) override {
    return modifiers_.Modifiers(request);
  }

  std::expected<void, std::string> WriteFieldList(
      const fields::WriteFieldListRequest& request) override {
    return fields_.WriteFieldList(request);
  }

  std::expected<entities::EntityClassResponse, std::string> EntityClass(
      const entities::EntityRequest& request) override {
    return fields_.EntityClass(request);
  }

  std::expected<entities::EntityNameResponse, std::string> EntityName(
      const entities::EntityRequest& request) override {
    return fields_.EntityName(request);
  }

  std::expected<entities::EntityPositionResponse, std::string> EntityPosition(
      const entities::EntityRequest& request) override {
    return fields_.EntityPosition(request);
  }

  std::expected<abilities::ActiveResponse, std::string> ModifierState(
      const abilities::ModifierStateRequest& request) override {
    return modifiers_.ModifierState(request);
  }

  std::expected<void, std::string> HoldModifierState(
      const abilities::HoldModifierStateRequest& request) override {
    return modifiers_.HoldModifierState(request);
  }

  std::expected<void, std::string> SetFreeForAll(
      const wasm::SetFreeForAllRequest& request) override;
  std::expected<void, std::string> SetTeamSeating(
      const wasm::SetTeamSeatingRequest& request) override;

  std::expected<objects::ObjectResponse, std::string> CreateModel(
      const objects::ModelOptions& request) override {
    return world_.CreateModel(request);
  }

  std::expected<objects::ObjectResponse, std::string> CreateText(
      const objects::TextOptions& request) override {
    return world_.CreateText(request);
  }

  std::expected<objects::ObjectResponse, std::string> CreateParticle(
      const objects::ParticleOptions& request) override {
    return world_.CreateParticle(request);
  }

  std::expected<objects::ObjectResponse, std::string> CreateFog(
      const objects::FogOptions& request) override {
    return world_.CreateFog(request);
  }

  std::expected<void, std::string> MoveObject(const objects::MoveObjectRequest& request) override {
    return world_.MoveObject(request);
  }

  std::expected<void, std::string> SetText(const objects::SetTextRequest& request) override {
    return world_.SetText(request);
  }

  std::expected<void, std::string> SetObjectHidden(
      const objects::SetObjectHiddenRequest& request) override {
    return world_.SetObjectHidden(request);
  }

  std::expected<void, std::string> ShowObjectOnlyTo(
      const objects::ShowObjectOnlyToRequest& request) override {
    return world_.ShowObjectOnlyTo(request);
  }

  std::expected<void, std::string> ShowObjectToAll(const objects::ObjectRequest& request) override {
    return world_.ShowObjectToAll(request);
  }

  std::expected<void, std::string> RemoveObject(const objects::ObjectRequest& request) override {
    return world_.RemoveObject(request);
  }

  std::expected<entities::EntityResponse, std::string> ObjectEntity(
      const objects::ObjectRequest& request) override {
    return world_.ObjectEntity(request);
  }

  std::expected<players::BotResponse, std::string> AddBot(
      const players::BotOptions& request) override {
    return world_.AddBot(request);
  }

  std::expected<void, std::string> RemoveBot(const players::PlayerRequest& request) override {
    return world_.RemoveBot(request);
  }

  std::expected<void, std::string> BlockInput(const wasm::InputRequest& request) override {
    return input_.BlockInput(request);
  }

  std::expected<void, std::string> BlockPlayerInput(
      const wasm::PlayerInputRequest& request) override {
    return input_.BlockPlayerInput(request);
  }

  std::expected<void, std::string> Press(const wasm::PlayerInputRequest& request) override {
    return input_.Press(request);
  }

  std::expected<void, std::string> WatchInput(const wasm::InputRequest& request) override {
    return input_.WatchInput(request);
  }

  std::expected<void, std::string> RemapInput(const wasm::RemapInputRequest& request) override {
    return input_.RemapInput(request);
  }

  std::expected<entities::EntityResponse, std::string> CreateEntity(
      const entities::EntityOptions& request) override {
    return world_.CreateEntity(request);
  }

  std::expected<entities::AliveResponse, std::string> FireInput(
      const entities::FireInputRequest& request) override {
    return world_.FireInput(request);
  }

  std::expected<entities::AliveResponse, std::string> RemoveEntity(
      const entities::EntityRequest& request) override {
    return world_.RemoveEntity(request);
  }

  std::expected<void, std::string> ShowEntityOnlyTo(
      const entities::ShowEntityOnlyToRequest& request) override {
    return world_.ShowEntityOnlyTo(request);
  }

  std::expected<void, std::string> ShowEntityToAll(
      const entities::EntityRequest& request) override {
    return world_.ShowEntityToAll(request);
  }

  std::expected<npcs::NpcResponse, std::string> SpawnNpc(const npcs::NpcOptions& request) override {
    return world_.SpawnNpc(request);
  }

  std::expected<npcs::NpcStateResponse, std::string> ReadNpc(
      const npcs::NpcRequest& request) override {
    return world_.ReadNpc(request);
  }

  std::expected<entities::AliveResponse, std::string> MoveNpc(
      const npcs::MoveNpcRequest& request) override {
    return world_.MoveNpc(request);
  }

  std::expected<entities::AliveResponse, std::string> SetNpcHealth(
      const npcs::SetNpcHealthRequest& request) override {
    return world_.SetNpcHealth(request);
  }

  std::expected<entities::AliveResponse, std::string> RemoveNpc(
      const npcs::NpcRequest& request) override {
    return world_.RemoveNpc(request);
  }

  std::expected<entities::PickupResponse, std::string> CreatePickup(
      const entities::CreatePickupRequest& request) override {
    return world_.CreatePickup(request);
  }

  std::expected<entities::AliveResponse, std::string> PickupPresent(
      const entities::PickupRequest& request) override {
    return world_.PickupPresent(request);
  }

  std::expected<entities::AliveResponse, std::string> RemovePickup(
      const entities::PickupRequest& request) override {
    return world_.RemovePickup(request);
  }

  std::expected<entities::EntitiesResponse, std::string> FindEntities(
      const entities::FindEntitiesRequest& request) override {
    return world_.FindEntities(request);
  }

  std::expected<entities::EntitiesResponse, std::string> FindEntitiesByName(
      const entities::FindEntitiesByNameRequest& request) override {
    return world_.FindEntitiesByName(request);
  }

  std::expected<entities::CountResponse, std::string> RemoveEntities(
      const entities::RemoveEntitiesRequest& request) override {
    return world_.RemoveEntities(request);
  }

  std::expected<void, std::string> ClearMap() override { return world_.ClearMap(); }

  std::expected<void, std::string> Damage(const combat::HitOptions& request) override;
  std::expected<void, std::string> KillEntity(const combat::KillOptions& request) override;
  std::expected<combat::TraceResponse, std::string> Trace(
      const combat::TraceOptions& request) override;
  std::expected<void, std::string> RestoreNpcs(const npcs::RestoreNpcsRequest& request) override;
  std::expected<void, std::string> Pause(const wasm::PauseRequest& request) override;
  std::expected<wasm::MatchClockResponse, std::string> MatchClock() override;
  std::expected<void, std::string> HoldMatchClock(
      const wasm::HoldMatchClockRequest& request) override;
  std::expected<wasm::RiftResponse, std::string> Rift() override;
  std::expected<void, std::string> StartRift(const wasm::StartRiftRequest& request) override;

  std::expected<void, std::string> MoveEntity(const entities::MoveEntityRequest& request) override {
    return world_.MoveEntity(request);
  }

  std::expected<void, std::string> PushEntity(const entities::PushEntityRequest& request) override {
    return world_.PushEntity(request);
  }

  std::expected<effects::SoundResponse, std::string> EmitSound(
      const effects::EmitSoundRequest& request) override;
  std::expected<void, std::string> AdjustSound(const effects::AdjustSoundRequest& request) override;
  std::expected<void, std::string> StopSound(const effects::StopSoundRequest& request) override;

  std::expected<void, std::string> Kill(const players::PlayerRequest& request) override {
    return heroes_.Kill(request);
  }

  std::expected<void, std::string> SetVelocity(
      const movement::SetVelocityRequest& request) override {
    return movement_.SetVelocity(request);
  }

  std::expected<wasm::ButtonsResponse, std::string> Buttons(
      const players::PlayerRequest& request) override {
    return input_.Buttons(request);
  }

  std::expected<void, std::string> WatchMovement(
      const movement::WatchMovementRequest& request) override {
    return movement_.WatchMovement(request);
  }

  std::expected<void, std::string> WatchControllers(
      const controllers::WatchControllersRequest& request) override {
    return input_.WatchControllers(request);
  }

  std::expected<void, std::string> WatchProjectiles(
      const combat::ProjectileOptions& request) override {
    return projectiles_.WatchProjectiles(request);
  }

  // ScreenOwner returns the entity index a screen effect for slot belongs to.
  std::expected<int32_t, std::string> ScreenOwner(int32_t slot);

  // HumanSlots returns the slot bits of the connected human players, whose
  // clients can hear a sound.
  static uint64_t HumanSlots();

  // PlaySound starts a sound on the source entity index, shaped and
  // addressed by options.
  std::expected<effects::SoundResponse, std::string> PlaySound(
      const std::string& sound, int32_t source, const effects::SoundOptions& options);

  // ApplyRestore performs one stage of a hero restore's writes.
  bool ApplyRestore(const HeroRestore::Sample& sample, const RestoreTarget& target,
                    HeroRestore::Stage stage);

  // TickRestores advances every hero restore and the unit restore by one
  // frame and queues the ones that ended.
  void TickRestores();

  // SeatAssignedPlayers retries pending host seats and records a seat only
  // after its native operation succeeds for the same connection generation.
  void SeatAssignedPlayers();
  // SeatAssignedPlayer applies one pending host seat without choosing a hero.
  std::expected<void, std::string> SeatAssignedPlayer(int32_t slot, uint32_t generation,
                                                      uint64_t steam_id, int32_t team);

  // SeatPlayers seats each player who has no team on the smaller side, with
  // no hero, while team seating or free-for-all is on. A host that cannot
  // seat a player leaves the game's own team screen to them.
  void SeatPlayers();
  // ApplyFreeForAll holds friendly fire on every living hero in free-for-all.
  void ApplyFreeForAll();
  // EnableRelations makes teammates enemies to abilities and projectiles,
  // installing the relations on first use.
  void EnableRelations();
  // ReleaseFreeForAll gives back the friendly fire holds free-for-all took
  // and restores the stock team relations.
  void ReleaseFreeForAll();

  // Silence installs the voice hook while the mod silences any concept, then
  // reports the concept names the game did not know, from unknown.
  std::expected<void, std::string> Silence(const std::string& unknown);

  // Combatant resolves a hero or unit that can safely receive damage events.
  std::expected<void*, std::string> Combatant(uint32_t handle);

  // services_ outlives the game.
  GameServices& services_;
  // engine_ outlives the game; the host owns it.
  EngineHost* engine_;
  // free_for_all_ is true while the host seats players and holds friendly fire,
  // and team_seating_ while it only seats them; ffa_held_ is the hero pawns it
  // holds friendly fire on, and seat_failed_ the players it could not seat, so
  // each failure is logged once.
  bool free_for_all_ = false;
  bool team_seating_ = false;
  // assignments_ survives Clear: the host owns it, independently of the mod.
  TeamSeating assignments_;

  std::set<uint32_t> ffa_held_;
  std::set<int32_t> seat_failed_;

  // silenced_ is the voice concepts the mod keeps every hero from speaking,
  // and silenced_players_ those it keeps from one player's hero, by slot.
  std::set<int32_t> silenced_;
  std::map<int32_t, std::set<int32_t>> silenced_players_;
  // restores_ are the running hero restores by player slot, ticked once per
  // frame; restored_ queues the ones that ended for the mod.
  std::map<int32_t, HeroRestore> restores_;
  uint64_t frames_ = 0;
  std::vector<Restored> restored_;
  // npc_restore_ is the running unit restore; npcs_restored_ is the result
  // of the last one that ended, until the mod receives it.
  std::optional<NpcRestore> npc_restore_;
  std::optional<std::string> npcs_restored_;

  // The modules own their domains' state. modules_ lists them in the order
  // they are built, each after the modules it reads. WorldReady runs them in
  // that order; Frame, WorldEnding, Leave and Clear run them in reverse, so a
  // module's frame work and release come before those of the modules it reads.
  wasm::Players players_{services_};
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
  std::array<Module*, 11> modules_{&players_,  &world_,     &match_,      &apart_,
                                   &fields_,   &modifiers_, &heroes_,     &camera_,
                                   &movement_, &input_,     &projectiles_};
};

}  // namespace modlock::wasm
