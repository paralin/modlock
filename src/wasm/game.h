#pragma once

#include <array>
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
#include "modlock/gameinterop/engine_server.h"
#include "modlock/gameinterop/entity_abi.h"
#include "modlock/gameinterop/game_rules.h"
#include "modlock/gameinterop/hero_definitions.h"
#include "modlock/gameinterop/mapped_module_image.h"
#include "modlock/gameinterop/match_clock.h"
#include "modlock/gameinterop/movement_hook.h"
#include "modlock/gameinterop/native_damage.h"
#include "modlock/gameinterop/native_sound.h"
#include "modlock/gameinterop/native_user_messages.h"
#include "modlock/gameinterop/pawn_observer.h"
#include "modlock/gameinterop/projectile_impact_hook.h"
#include "modlock/gameinterop/world_entities.h"
#include "modlock/render/world_effect_game_factory.h"
#include "modlock/render/world_text_game_factory.h"
#include "proto/modlock/wasm.pb.h"
#include "wasm/hero_restore.h"
#include "wasm/host_service.gen.h"
#include "wasm/npc_restore.h"

namespace modlock::wasm {

// kMaxPlayers bounds the player slots a Deadlock server holds.
inline constexpr int32_t kMaxPlayers = 64;

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

  // WorldReady installs the input hook when a mod asked for input before the
  // first world loaded, and runs the waiting console commands when the mods
  // loaded after the world started.
  void WorldReady();

  // StepWith runs step for every hero's server movement that no Steer
  // replays, as a mod's own movement model does; an empty step stops it. One
  // mod's step may run at a time.
  std::expected<void, std::string> StepWith(gameinterop::MovementHook::Handler step);

 private:
  friend class Game;

  // FieldKey names one schema field as class and field.
  using FieldKey = std::pair<std::string, std::string>;

  // ModifierLayout locates an entity's modifier state masks.
  struct ModifierLayout {
    size_t property = 0;
    size_t mask = 0;
    size_t predicted_mask = 0;
    size_t mask_size = 0;
  };

  std::expected<const gameinterop::ModuleImage*, std::string> Image();
  std::expected<void*, std::string> Schema();
  std::expected<const gameinterop::HeroDefinitions*, std::string> Heroes();
  std::expected<const gameinterop::PlayerSelectionCalls*, std::string> Selection();
  std::expected<const gameinterop::AbilityDefinitions*, std::string> Abilities();
  std::expected<const gameinterop::ItemFunctions*, std::string> Items();
  std::expected<const gameinterop::AbilitySlots*, std::string> Slots();
  std::expected<gameinterop::CreateAbility, std::string> CreateAbility();
  std::expected<gameinterop::RespawnPawn, std::string> Respawn();
  std::expected<gameinterop::TeleportClientCamera, std::string> Teleport();
  std::expected<const gameinterop::PawnMotion*, std::string> Motion();
  std::expected<const gameinterop::BotCreation*, std::string> Bots();
  std::expected<render::WorldTextGameFactory*, std::string> Text();
  std::expected<render::WorldEffectGameFactory*, std::string> Effects();
  std::expected<const gameinterop::NativeDamage*, std::string> Damage();
  std::expected<gameinterop::ModifyCurrency, std::string> Currency();
  std::expected<const gameinterop::NativeSound*, std::string> Sound();
  std::expected<const gameinterop::GamePause*, std::string> Pause();
  std::expected<gameinterop::MatchClock*, std::string> Clock();
  std::expected<gameinterop::KothRules*, std::string> Rift();
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

  // Filter applies every mod's presses, remaps and blocks to one player's input.
  uint64_t Filter(gameinterop::AbilityInputHook::Input& input);

  // Projectiles installs the shared projectile impact hook once a world has
  // loaded, or marks it wanted until then.
  void Projectiles();

  // Impact runs the game's impact of a projectile and reports it to each mod
  // that watches it. When one keeps momentum, every live hero gets back the
  // motion it had before the game's explosion.
  void Impact(void* entity, const gameinterop::TraceResult& contact,
              const gameinterop::ProjectileImpactHook::NativeImpact& native);

  // Steps installs or removes the server movement hook to match what the
  // mods need of it.
  std::expected<void, std::string> Steps();

  // Step records the hero's command, replays a mod's steer over it, and runs
  // step_ otherwise; it reports whether it replaced the step's motion.
  bool Step(gameinterop::MovementCall& call);

  // Command takes the commands pawn ran since the previous call, folded.
  std::optional<MovementCommand> Command(const void* pawn);

  std::optional<gameinterop::MappedModuleImage> image_;
  std::optional<gameinterop::EngineServer> server_;
  std::optional<gameinterop::NativeUserMessages> messages_;
  std::optional<gameinterop::HeroDefinitions> heroes_;
  std::optional<gameinterop::PlayerSelectionCalls> selection_;
  std::optional<gameinterop::AbilityDefinitions> abilities_;
  std::optional<gameinterop::ItemFunctions> items_;
  std::optional<gameinterop::AbilitySlots> slots_;
  gameinterop::CreateAbility create_ability_ = nullptr;
  gameinterop::RespawnPawn respawn_ = nullptr;
  gameinterop::TeleportClientCamera teleport_ = nullptr;
  std::optional<gameinterop::PawnMotion> motion_;
  std::optional<gameinterop::BotCreation> bots_;
  std::unique_ptr<render::WorldTextGameFactory> text_;
  std::unique_ptr<render::WorldEffectGameFactory> effects_;
  std::optional<gameinterop::NativeDamage> damage_;
  gameinterop::ModifyCurrency currency_ = nullptr;
  std::optional<gameinterop::NativeSound> sound_;
  std::optional<gameinterop::GamePause> pause_;
  std::optional<gameinterop::MatchClock> clock_;
  std::optional<gameinterop::KothRules> rift_;
  std::map<FieldKey, gameinterop::SchemaField> fields_;
  std::map<std::pair<const void*, std::string>, bool> derives_;
  std::optional<ModifierLayout> modifiers_;
  std::map<std::string, int64_t, std::less<>> modifier_states_;
  // games are the live mods' surfaces, which the input hook consults.
  std::set<Game*> games_;
  std::optional<gameinterop::AbilityInputHook> input_;
  bool input_wanted_ = false;
  std::optional<gameinterop::ProjectileImpactHook> impacts_;
  bool impacts_wanted_ = false;
  // steps_ is the server movement hook, held while step_ runs or a mod
  // watches or steers a hero. commands_ folds each pawn's commands since its
  // last sample, and ground_ is the last entity any hero stood on.
  std::optional<gameinterop::MovementHook> steps_;
  gameinterop::MovementHook::Handler step_;
  struct Folded {
    MovementCommand command;
    bool stepped = false;
  };
  std::map<const void*, Folded> commands_;
  uint32_t ground_ = gameinterop::MovementHook::kNoGround;
  bool world_ready_ = false;
  // pending_commands_ wait for the engine's first world; started_ marks it.
  std::vector<std::string> pending_commands_;
  bool started_ = false;
};

// Game answers one mod's calls into the game and owns what the mod placed in
// it: world objects, units, bots, frozen heroes and input changes. Clear and
// WorldEnding remove them, so a stopped or reloaded mod leaves nothing behind.
// The calls that reach beyond the game, Log, Ui and CallService, belong to a
// subclass.
class Game : public HostService {
 public:
  Game(GameServices& services, EngineHost* engine);
  ~Game() override;

  Game(const Game&) = delete;
  Game& operator=(const Game&) = delete;

  // Frame finishes placing the units the mod spawned last frame and advances
  // its hero restores.
  void Frame();

  // ButtonChange is one player's watched buttons that went down or up in one
  // input.
  struct ButtonChange {
    int32_t slot = 0;
    uint64_t pressed = 0;
    uint64_t released = 0;
  };

  // TakeButtonChanges returns the watched button changes since the last call,
  // in the order players made them.
  std::vector<ButtonChange> TakeButtonChanges();

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

  // Leave forgets what the mod set for the player in slot, who left, so the
  // next player there starts clear.
  void Leave(int32_t slot);

  // Clear removes everything the mod placed, releases its frozen heroes,
  // held modifiers, input changes and match clock hold, resumes a game it
  // paused and stops watching projectiles.
  void Clear();

  // Hero is one live hero this frame. Velocity is absent when the game's
  // motion field cannot be read.
  struct Hero {
    int32_t slot = 0;
    uint32_t handle = 0;
    void* pawn = nullptr;
    std::array<float, 3> feet{};
    std::optional<std::array<float, 3>> velocity;
  };

  // Heroes returns every live hero this frame.
  std::vector<Hero> Heroes();

  // Moving reports whether the mod watches any player's movement.
  bool Moving() const { return !movers_.empty(); }

  // MovementFact is one movement fact the game announced for the hero with
  // the pawn handle; an executed ability carries the ability's handle.
  struct MovementFact {
    uint32_t pawn = 0;
    MovementAction action = MOVEMENT_ACTION_UNKNOWN;
    uint32_t ability = 0;
  };

  // Movement returns a sample of each watched, live hero, with the facts
  // that name its pawn.
  std::vector<MovementSample> Movement(std::span<const MovementFact> facts);

  // TakeLaunches returns the watched projectiles that appeared since the last
  // call.
  std::vector<LaunchEvent> TakeLaunches() { return std::exchange(launches_, {}); }

 protected:
  // Impacted delivers a watched projectile's impact to the mod, inside the
  // game's impact.
  virtual void Impacted(const ImpactEvent& event) = 0;

 private:
  friend class GameServices;

  // Object is one world object the mod created.
  using Object =
      std::variant<std::unique_ptr<render::WorldEffect>, std::unique_ptr<render::WorldTextEntity>>;

  // The calls, in the order the Host service declares them.
  std::expected<void, std::string> ServerCommand(const ServerCommandRequest& request) override;
  std::expected<void, std::string> Chat(const ChatRequest& request) override;
  std::expected<void, std::string> CenterText(const CenterTextRequest& request) override;
  std::expected<void, std::string> Announce(const AnnounceRequest& request) override;
  std::expected<void, std::string> Precache(const PrecacheOptions& request) override;
  std::expected<PlayersResponse, std::string> Players() override;
  std::expected<PawnResponse, std::string> Pawn(const PlayerRequest& request) override;
  std::expected<HeroResponse, std::string> SelectHero(const SelectHeroRequest& request) override;
  std::expected<void, std::string> Spectate(const PlayerRequest& request) override;
  std::expected<void, std::string> Respawn(const PlayerRequest& request) override;
  std::expected<void, std::string> ClearItems(const PlayerRequest& request) override;
  std::expected<void, std::string> Freeze(const FreezeRequest& request) override;
  std::expected<void, std::string> RestoreStamina(const PlayerRequest& request) override;
  std::expected<void, std::string> RefreshAbility(const RefreshAbilityRequest& request) override;
  std::expected<AbilitiesResponse, std::string> Abilities(const PlayerRequest& request) override;
  std::expected<void, std::string> SetAbility(const AbilityOptions& request) override;
  std::expected<void, std::string> GiveItem(const GiveItemRequest& request) override;
  std::expected<void, std::string> ReplaceAbility(const ReplaceAbilityRequest& request) override;
  std::expected<void, std::string> HoldModifier(const HoldModifierRequest& request) override;
  std::expected<void, std::string> GiveModifier(const GiveModifierRequest& request) override;
  std::expected<void, std::string> Teleport(const TeleportRequest& request) override;
  std::expected<void, std::string> MovePlayer(const MovePlayerRequest& request) override;
  std::expected<void, std::string> Steer(const SteerRequest& request) override;
  std::expected<void, std::string> AdjustSouls(const AdjustSoulsRequest& request) override;
  std::expected<void, std::string> StartingSouls(const StartingSoulsRequest& request) override;
  std::expected<void, std::string> Heal(const HealRequest& request) override;
  std::expected<void, std::string> Sound(const SoundRequest& request) override;
  std::expected<void, std::string> RestoreHero(const RestoreHeroRequest& request) override;
  std::expected<void, std::string> ScreenEffect(const ScreenEffectRequest& request) override;
  std::expected<void, std::string> ClearScreenEffect(
      const ClearScreenEffectRequest& request) override;
  std::expected<FieldResponse, std::string> ReadField(const ReadFieldRequest& request) override;
  std::expected<void, std::string> WriteField(const WriteFieldRequest& request) override;
  std::expected<EntityClassResponse, std::string> EntityClass(const EntityRequest& request) override;
  std::expected<ActiveResponse, std::string> ModifierState(
      const ModifierStateRequest& request) override;
  std::expected<void, std::string> HoldModifierState(
      const HoldModifierStateRequest& request) override;
  std::expected<ObjectResponse, std::string> CreateModel(const ModelOptions& request) override;
  std::expected<ObjectResponse, std::string> CreateText(const TextOptions& request) override;
  std::expected<ObjectResponse, std::string> CreateParticle(
      const ParticleOptions& request) override;
  std::expected<ObjectResponse, std::string> CreateFog(const FogOptions& request) override;
  std::expected<void, std::string> MoveObject(const MoveObjectRequest& request) override;
  std::expected<void, std::string> SetText(const SetTextRequest& request) override;
  std::expected<void, std::string> RemoveObject(const ObjectRequest& request) override;
  std::expected<EntityResponse, std::string> ObjectEntity(const ObjectRequest& request) override;
  std::expected<BotResponse, std::string> AddBot(const BotOptions& request) override;
  std::expected<void, std::string> RemoveBot(const PlayerRequest& request) override;
  std::expected<void, std::string> BlockInput(const InputRequest& request) override;
  std::expected<void, std::string> BlockPlayerInput(const PlayerInputRequest& request) override;
  std::expected<void, std::string> Press(const PlayerInputRequest& request) override;
  std::expected<void, std::string> WatchInput(const InputRequest& request) override;
  std::expected<void, std::string> RemapInput(const RemapInputRequest& request) override;
  std::expected<NpcResponse, std::string> SpawnNpc(const NpcOptions& request) override;
  std::expected<NpcStateResponse, std::string> ReadNpc(const NpcRequest& request) override;
  std::expected<AliveResponse, std::string> MoveNpc(const MoveNpcRequest& request) override;
  std::expected<AliveResponse, std::string> SetNpcHealth(
      const SetNpcHealthRequest& request) override;
  std::expected<AliveResponse, std::string> RemoveNpc(const NpcRequest& request) override;
  std::expected<PickupResponse, std::string> CreatePickup(
      const CreatePickupRequest& request) override;
  std::expected<AliveResponse, std::string> PickupPresent(const PickupRequest& request) override;
  std::expected<AliveResponse, std::string> RemovePickup(const PickupRequest& request) override;
  std::expected<CountResponse, std::string> RemoveEntities(
      const RemoveEntitiesRequest& request) override;
  std::expected<void, std::string> ClearMap() override;
  std::expected<void, std::string> Damage(const HitOptions& request) override;
  std::expected<TraceResponse, std::string> Trace(const TraceOptions& request) override;
  std::expected<void, std::string> RestoreNpcs(const RestoreNpcsRequest& request) override;
  std::expected<void, std::string> Pause(const PauseRequest& request) override;
  std::expected<MatchClockResponse, std::string> MatchClock() override;
  std::expected<void, std::string> HoldMatchClock(const HoldMatchClockRequest& request) override;
  std::expected<RiftResponse, std::string> Rift() override;
  std::expected<void, std::string> StartRift(const StartRiftRequest& request) override;
  std::expected<void, std::string> MoveEntity(const MoveEntityRequest& request) override;
  std::expected<void, std::string> EmitSound(const EmitSoundRequest& request) override;
  std::expected<void, std::string> Kill(const PlayerRequest& request) override;
  std::expected<void, std::string> SetVelocity(const SetVelocityRequest& request) override;
  std::expected<ButtonsResponse, std::string> Buttons(const PlayerRequest& request) override;
  std::expected<void, std::string> WatchMovement(const WatchMovementRequest& request) override;
  std::expected<void, std::string> WatchProjectiles(const ProjectileOptions& request) override;

  // FieldAddress resolves a schema field of a live entity that holds a value
  // of type, returning the entity and the field's address in it.
  std::expected<std::pair<void*, unsigned char*>, std::string> FieldAddress(
      uint32_t handle, const std::string& class_name, const std::string& field, FieldType type);

  // ScreenOwner returns the entity index a screen effect for slot belongs to.
  std::expected<int32_t, std::string> ScreenOwner(int32_t slot);

  // SetPaused pauses or resumes the game.
  std::expected<void, std::string> SetPaused(bool paused);

  // Keep stores a created object and returns its response.
  ObjectResponse Keep(Object object);

  // ApplyRestore performs one stage of a hero restore's writes.
  bool ApplyRestore(const HeroRestore::Sample& sample, const RestoreTarget& target,
                    HeroRestore::Stage stage);

  // TickRestores advances every hero restore and the unit restore by one
  // frame and queues the ones that ended.
  void TickRestores();

  // HoldClock sets the held match clock again, since the game advances it
  // each frame.
  void HoldClock();

  // OwnedAbility returns the ability named name of slot's live hero.
  std::expected<gameinterop::PawnObserver::Ability, std::string> OwnedAbility(
      int32_t slot, std::string_view name);

  // ModifierWord is the word of an entity's modifier masks that holds one
  // state, and the state's bit in it.
  struct ModifierWord {
    unsigned char* ordinary = nullptr;
    unsigned char* predicted = nullptr;
    uint32_t bit = 0;
  };

  // FindModifierWord locates state on entity, or returns nothing when the
  // entity has no modifiers.
  std::expected<std::optional<ModifierWord>, std::string> FindModifierWord(
      void* entity, const std::string& state);

  // ApplyHolds sets every held modifier state again, since the game
  // recomputes states each frame and on teleport, and forgets a hold whose
  // entity is gone. It gives held modifiers back to living heroes that lost
  // them.
  void ApplyHolds();

  // RenewModifier gives slot's living hero the held modifier when it lacks
  // it. A dead or absent hero waits for its return.
  std::expected<void, std::string> RenewModifier(int32_t slot, const std::string& key);

  // ReleaseModifier ends slot's hold on key and restores the modifier's
  // duration once no player holds it.
  void ReleaseModifier(int32_t slot, const std::string& key);

  // ReleaseModifiers ends every modifier hold.
  void ReleaseModifiers();

  // SetModifierState sets or clears state in entity's ordinary mask and
  // replicates the change.
  std::expected<void, std::string> SetModifierState(uint32_t entity, const std::string& state,
                                                    bool active);

  // World returns this world's unit operations.
  std::expected<gameinterop::WorldEntities*, std::string> World();

  // Live returns the observed pawn of slot for this frame.
  std::expected<gameinterop::PawnObserver::Sample, std::string> Live(int32_t slot);

  // Entity resolves a handle to the live entity it names.
  std::expected<void*, std::string> Entity(uint32_t handle) const;

  // TrackProjectiles queues a Launch for each watched projectile that
  // appeared since the last frame.
  void TrackProjectiles();

  // Watching returns the Impact event for a projectile the mod watches, or
  // nothing.
  std::optional<ImpactEvent> Watching(void* entity, const gameinterop::TraceResult& contact);

  // Owner returns a projectile's owning entity and the slot of the player
  // whose hero it is, or -1.
  std::pair<uint32_t, int32_t> Owner(void* entity);

  // Origin returns where an entity is.
  std::optional<std::array<float, 3>> Origin(void* entity);

  // RemoveWorld removes the objects and units and kicks the bots.
  void RemoveWorld();

  // services_ outlives the game.
  GameServices& services_;
  // engine_ outlives the game; the host owns it.
  EngineHost* engine_;
  gameinterop::PawnObserver observer_;
  std::map<uint32_t, Object> objects_;
  // fog_ is the world's fog controller, which the fog objects need; it is
  // created with the first and removed after the objects.
  std::unique_ptr<render::WorldEffect> fog_;
  uint32_t next_object_ = 1;
  // world_ spawns and steers units in the loaded world; npcs_ are the units
  // and pickups the mod created, and spawned_ is true until the units'
  // placement finishes.
  std::optional<gameinterop::WorldEntities> world_;
  std::set<uint32_t> npcs_;
  bool spawned_ = false;
  // bots_ maps each bot's slot to its name, which the console kicks by.
  std::map<int32_t, std::string> bots_;
  std::set<int32_t> frozen_;
  // movers_ is the players whose movement the mod watches.
  std::set<int32_t> movers_;
  // Steered is one hero's replayed command, with its pawn.
  struct Steered {
    void* pawn = nullptr;
    Steering steering;
  };
  // steers_ maps each steered player to the command its hero replays.
  std::map<int32_t, Steered> steers_;
  // held_ is the modifier states the mod holds, by entity handle and name;
  // ApplyHolds sets them again.
  std::set<std::pair<uint32_t, std::string>> held_;
  // held_modifiers_ is the modifiers the mod holds, by player slot and
  // "<ability>/<modifier>" key; modifiers_ resolves each held key.
  std::set<std::pair<int32_t, std::string>> held_modifiers_;
  std::map<std::string, gameinterop::AbilityModifier, std::less<>> modifiers_;
  // blocked_ withholds buttons from every player and slot_blocked_ from one
  // player; presses_ adds buttons to one player's next input.
  uint64_t blocked_ = 0;
  std::map<int32_t, uint64_t> slot_blocked_;
  // remap_ makes its from buttons act as its to buttons for every player.
  RemapInputRequest remap_;
  std::map<int32_t, uint64_t> presses_;
  // watched_ selects the buttons whose changes changes_ queues for the mod.
  uint64_t watched_ = 0;
  std::vector<ButtonChange> changes_;
  // projectiles_ is the mod's projectile watch. projectiles_seen_ holds the
  // watched projectiles of the last frame, and launches_ queues the new ones
  // for the mod.
  ProjectileOptions projectiles_;
  std::set<uint32_t> projectiles_seen_;
  std::vector<LaunchEvent> launches_;
  // restores_ are the running hero restores by player slot, ticked once per
  // frame; restored_ queues the ones that ended for the mod.
  std::map<int32_t, HeroRestore> restores_;
  uint64_t frames_ = 0;
  std::vector<Restored> restored_;
  // npc_restore_ is the running unit restore; npcs_restored_ is the result
  // of the last one that ended, until the mod receives it.
  std::optional<NpcRestore> npc_restore_;
  std::optional<std::string> npcs_restored_;
  // held_clock_ is the match time the mod holds; paused_ is true while the
  // mod has the game paused.
  std::optional<float> held_clock_;
  bool paused_ = false;
};

}  // namespace modlock::wasm
