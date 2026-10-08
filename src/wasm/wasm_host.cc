#include "modlock/wasm_host.h"

#include <google/protobuf/json/json.h>

#include <algorithm>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "modlock/engine_host.h"
#include "modlock/gameinterop/connection_tracker.h"
#include "modlock/gameinterop/entity_abi.h"
#include "modlock/gameinterop/mapped_module_image.h"
#include "modlock/gameinterop/movement_hook.h"
#include "modlock/net/listen_boot.h"
#include "modlock/wasm_settings.h"
#include "quake/player_movement.h"
#include "wasm/ability_tuning.h"
#include "wasm/game.h"
#include "wasm/instance.h"
#include "wasm/metrics.h"
#include "wasm/runtime.h"
#include "wasm/settings.h"
#include "wasm/ui.h"

namespace modlock {
namespace {

class WasmPlugin;

// Mods is what a host's mods share: the runtime, the interpreter modules'
// directory, the game's services, the observers and the running mods by name.
struct Mods {
  wasm::Runtime runtime;
  std::filesystem::path interpreters;
  wasm::GameServices game;
  std::vector<WasmHostObserver*> observers;
  std::map<std::string, WasmPlugin*, std::less<>> plugins;
  // extensions holds the services the host provides, by name.
  std::map<std::string, WasmExtension, std::less<>> extensions;
  // settings keeps players' settings: kept unless the host was given a
  // keeper.
  FileSettings kept;
  WasmSettings* settings = &kept;
  // metrics receives players' metric totals; without one, each mod logs
  // them.
  WasmMetrics* metrics = nullptr;

  // Notify calls report with each observer. An observer may stop observing
  // from its callback.
  template <typename Report>
  void Notify(const Report& report) {
    for (auto* observer : std::vector(observers)) report(*observer);
  }
};

// Build is one built mod read from disk.
struct Build {
  // name identifies the mod among the host's plugins.
  std::string name;
  // module holds the WebAssembly bytes to run.
  std::vector<uint8_t> module;
  // source holds an interpreted mod's entry, which module evaluates at start.
  std::vector<uint8_t> source;
  // manifest is the mod's mod.json; a bare .wasm file has an empty one.
  wasm::Manifest manifest;
  // collision holds the map's Quake collision when the manifest's movement
  // model needs it.
  std::vector<uint8_t> collision;
};

// ReadFile returns the bytes of path.
std::expected<std::vector<uint8_t>, std::string> ReadFile(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return std::unexpected("cannot read " + path.string());
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(file),
                              std::istreambuf_iterator<char>());
}

// InterpreterFile names the interpreter module that runs an interpreted
// runtime, or is empty for a compiled one.
std::string_view InterpreterFile(wasm::Manifest::Runtime runtime) {
  switch (runtime) {
    case wasm::Manifest::RUNTIME_QUICKJS:
      return "quickjs.wasm";
    case wasm::Manifest::RUNTIME_LUAU:
      return "luau.wasm";
    case wasm::Manifest::RUNTIME_PYTHON:
      return "python.wasm";
    default:
      return {};
  }
}

// ReadBuild reads a built mod directory through its mod.json, or a bare
// .wasm file. An interpreted mod's module comes from interpreters.
std::expected<Build, std::string> ReadBuild(const std::filesystem::path& path,
                                            const std::filesystem::path& interpreters) {
  if (!std::filesystem::is_directory(path)) {
    auto module = ReadFile(path);
    if (!module) return std::unexpected(module.error());
    return Build{.name = path.stem().string(), .module = std::move(*module)};
  }

  // Parse the manifest, tolerating fields a newer command line writes.
  const auto manifest_path = path / "mod.json";
  auto text = ReadFile(manifest_path);
  if (!text) return std::unexpected(text.error());
  wasm::Manifest manifest;
  google::protobuf::json::ParseOptions options;
  options.ignore_unknown_fields = true;
  const auto parsed = google::protobuf::json::JsonStringToMessage(
      std::string_view(reinterpret_cast<const char*>(text->data()), text->size()), &manifest,
      options);
  if (!parsed.ok()) {
    return std::unexpected(manifest_path.string() + ": " + std::string(parsed.message()));
  }

  // Read the entry the manifest names, the interpreter that runs it and the
  // collision its movement model needs.
  if (manifest.slug().empty()) return std::unexpected(manifest_path.string() + ": no slug");
  auto entry = ReadFile(path / manifest.entry());
  if (!entry) return std::unexpected(entry.error());
  Build build{.name = manifest.slug()};
  if (manifest.movement().model() == wasm::Movement::MODEL_QUAKEWORLD) {
    auto collision = ReadFile(path / "maps" / (manifest.map() + ".bsp"));
    if (!collision) return std::unexpected(collision.error());
    build.collision = std::move(*collision);
  }
  if (manifest.runtime() == wasm::Manifest::RUNTIME_WASM) {
    build.module = std::move(*entry);
  } else {
    const auto interpreter = InterpreterFile(manifest.runtime());
    if (interpreter.empty()) {
      return std::unexpected(manifest_path.string() + ": this host does not know the runtime");
    }
    auto module = ReadFile(interpreters / interpreter);
    if (!module) return std::unexpected(module.error());
    build.module = std::move(*module);
    build.source = std::move(*entry);
  }
  build.manifest = std::move(manifest);
  return build;
}

// Tune writes manifest's ability tuning into the loaded game module and
// returns whether that is done. A module whose ability data has not loaded
// yet is tried again on a later call.
bool Tune(std::wstring_view module, const char* module_name, const wasm::Manifest& manifest,
          const std::function<void(std::string_view)>& log) {
  if (manifest.abilities().empty()) return true;
  auto image = gameinterop::MappedModuleImage::ForModule(module);
  auto schema = gameinterop::ResolveSchemaSystem();
  if (!image || !schema) return false;
  auto problems = wasm::TuneAbilities(*image, *schema, module_name, manifest.abilities());
  if (!problems) return false;
  for (const auto& problem : *problems) log("ability tuning: " + problem);
  return true;
}

// ClientMod runs the part of a mod that lives in a player's game: the
// manifest's ability tuning and its movement model's prediction. The server
// runs the mod itself, so the player's game runs no sandbox.
class ClientMod final : public Plugin {
 public:
  explicit ClientMod(Build build) : build_(std::move(build)) {}

  uint32_t InterfaceVersion() const override { return PluginInterfaceVersion; }
  const char* Name() const override { return build_.name.c_str(); }

  bool Start() override {
    const auto& movement = build_.manifest.movement();
    if (movement.model() != wasm::Movement::MODEL_QUAKEWORLD) return true;
    auto created = quake::PlayerMovement::Create(quake::PlayerMovement::Side::kClient,
                                                 build_.collision, movement.scale());
    if (!created) {
      Log("movement prediction is unavailable: " + created.error());
      return true;
    }
    movement_ = std::move(*created);
    auto hook = gameinterop::MovementHook::Install(
        gameinterop::MovementHook::Module::kClient, [this](gameinterop::MovementCall& call) {
          // The client's ability data loads before its first predicted command.
          Tick();
          for (auto& state : call.buttons)
            state &= ~build_.manifest.movement().unpredicted_buttons();
          return movement_->Step(call);
        });
    if (!hook) {
      Log("movement prediction is unavailable: " + hook.error());
      movement_.reset();
      return true;
    }
    hook_ = std::move(*hook);
    return true;
  }

  void Tick() override {
    if (!tuned_) tuned_ = Tune(L"client.dll", "client.dll", build_.manifest, Log);
  }

  void Stop() override {
    hook_.reset();
    movement_.reset();
  }

 private:
  static void Log(std::string_view text) { std::cerr << "client mod: " << text << '\n'; }

  Build build_;
  bool tuned_ = false;
  // hook_ runs movement_ in the client's prediction; it goes first.
  std::optional<quake::PlayerMovement> movement_;
  std::optional<gameinterop::MovementHook> hook_;
};

// PluginGame is one mod's game with the calls that reach its plugin: its log,
// its interface and the services the host provides.
class PluginGame final : public wasm::Game {
 public:
  PluginGame(WasmPlugin& plugin, Mods& host, EngineHost* engine)
      : Game(host.game, engine), plugin_(plugin) {}

 private:
  std::expected<void, std::string> Log(const wasm::LogRequest& request) override;
  std::expected<void, std::string> Ui(const wasm::UiRequest& request) override;
  std::expected<wasm::ServiceReply, std::string> CallService(
      const wasm::ServiceCall& request) override;
  std::expected<wasm::SettingResponse, std::string> PlayerSetting(
      const wasm::PlayerSettingRequest& request) override;
  std::expected<void, std::string> SetPlayerSetting(
      const wasm::SetPlayerSettingRequest& request) override;
  std::expected<void, std::string> AddMetric(const wasm::AddMetricRequest& request) override;
  std::expected<void, std::string> HoldReload(const wasm::HoldReloadRequest& request) override;
  void Impacted(const wasm::ImpactEvent& event) override;

  // plugin_ owns the game.
  WasmPlugin& plugin_;
};

// WasmPlugin connects one sandboxed mod to the engine. Frames arrive through
// Tick. Player commands arrive as chat lines that start with a slash and as
// console commands the server receives; the game client rejects console
// commands it does not know, so chat is how players reach a mod. All of them
// run on the engine thread, as do the mod's host calls. A reload swaps the
// instance in place and starts it again; what the old instance placed in the
// game goes with it. While the running instance holds itself, a reload waits
// for its release.
class WasmPlugin final : public Plugin {
 public:
  WasmPlugin(Mods& host, std::string name, const PluginContext& context)
      : host_(host),
        name_(std::move(name)),
        engine_(context.engine),
        check_only_(context.check_only),
        mod_([this](const wasm::Call& call) { return Send(call); }),
        game_(*this, host, context.engine) {
    for (int i = 1; i < context.argc; ++i) args_.emplace_back(context.argv[i]);
  }

  ~WasmPlugin() override {
    if (auto found = host_.plugins.find(name_);
        found != host_.plugins.end() && found->second == this) {
      host_.plugins.erase(found);
    }
  }

  // Instantiate loads module, answering its host calls through this plugin.
  std::expected<std::unique_ptr<wasm::Instance>, std::string> Instantiate(
      std::span<const uint8_t> module);

  // Attach takes the first loaded instance and the build it runs.
  void Attach(std::unique_ptr<wasm::Instance> instance, Build build) {
    instance_ = std::move(instance);
    source_ = std::move(build.source);
    manifest_ = std::move(build.manifest);
    collision_ = std::move(build.collision);
    tuned_ = false;
  }

  // Reload replaces the running instance with build and starts it, or keeps
  // build for the first frame after a held instance releases its hold.
  std::expected<void, std::string> Reload(Build build);

  // Hold keeps the running instance through reloads while held is true.
  void Hold(bool held);

  // Held reports whether the running instance keeps itself through reloads.
  bool Held() const { return held_; }

  // Press delivers a press on a button the mod shows the player in slot.
  std::expected<void, std::string> Press(int32_t slot, std::string_view node);

  // Serve delivers one call to a service the mod serves and returns its answer.
  std::expected<std::string, std::string> Serve(const wasm::ServiceCall& call);

  // Log writes one line to the server log under the mod's name.
  void Log(std::string_view text);

  // ApplyUi changes the interface the mod shows one player.
  std::expected<void, std::string> ApplyUi(const wasm::UiRequest& request);

  // CallService answers the mod's call to a service the host provides.
  std::expected<wasm::ServiceReply, std::string> CallService(const wasm::ServiceCall& call);

  // PlayerSetting returns the value of the setting named key for the player
  // in slot: the kept value while the manifest still accepts it, and the
  // default otherwise.
  std::expected<std::string, std::string> PlayerSetting(int32_t slot, std::string_view key);

  // SetPlayerSetting keeps the player's new value of the setting named key.
  std::expected<void, std::string> SetPlayerSetting(int32_t slot, std::string_view key,
                                                    std::string_view value);

  // SettingChanged delivers a setting a player changed outside the mod, when
  // the player is in the game and the manifest accepts the value.
  void SettingChanged(uint64_t steam_id, std::string_view key, std::string_view value);

  // AddMetric adds to the total of the metric named name for the player in
  // slot.
  std::expected<void, std::string> AddMetric(int32_t slot, std::string_view name, double value,
                                             std::string_view label);

  // Impacted delivers a watched projectile's impact to the mod.
  void Impacted(const wasm::ImpactEvent& event) {
    if (instance_) static_cast<void>(mod_.Impact(event));
  }

  uint32_t InterfaceVersion() const override { return PluginInterfaceVersion; }
  const char* Name() const override { return name_.c_str(); }
  bool Start() override;
  void Tick() override;
  void Stop() override;

 private:
  // Begin starts the mod and learns which events it consumes, reporting
  // whether it started.
  std::expected<void, std::string> Begin(bool reloaded);
  void Halt();
  void World();
  // SteamId returns the account of the player in slot, or zero for a bot or
  // an empty slot.
  static uint64_t SteamId(int32_t slot);
  // Send is the transport of mod_: it delivers one event and stops a mod
  // whose instance failed.
  std::expected<wasm::Reply, std::string> Send(const wasm::Call& call);
  void ListenToPlayers();
  void ListenToCombat();
  bool Hit(const gameinterop::DamageContactEvent& contact);
  // Fought queues an applied hit for a mod that wants it and a movement fact
  // for the movement samples.
  void Fought(const gameinterop::CombatEvent& event, bool damaged);
  void DeliverDamaged();
  // Sample fills a frame that starts a new tick with the watched heroes'
  // movement and the facts queued since the last one.
  void Sample(wasm::FrameEvent& frame);
  // DeliverGameEvents passes on the button changes and ended restores the
  // game queued.
  void DeliverGameEvents();
  void Unsubscribe();
  // Move installs the manifest's movement model on the server.
  void Move();
  // Steer delivers last frame's landings and gives the movement model this
  // frame's heroes.
  void Steer();
  bool Command(int32_t slot, std::string_view line);
  void ShowUi(int32_t slot, const ui::Change& change);
  void Leave(int32_t slot);
  // HandOn hands the player's metric totals to the host's metrics, or logs
  // them, and forgets them.
  void HandOn(int32_t slot);
  // Call answers one call from the mod and logs a failed one.
  wasm::Reply Call(const wasm::Call& call);
  void Failed(std::string_view error);

  // host_ outlives the plugin.
  Mods& host_;
  std::string name_;
  // engine_ outlives the plugin; the host owns it.
  EngineHost* engine_;
  bool check_only_;
  std::vector<std::string> args_;
  std::unique_ptr<wasm::Instance> instance_;
  // source_ is the running build's interpreted entry, sent at each start.
  std::vector<uint8_t> source_;
  // manifest_ and collision_ are the running build's; tuned_ is true once the
  // manifest's ability tuning is written.
  wasm::Manifest manifest_;
  std::vector<uint8_t> collision_;
  bool tuned_ = false;
  // held_ is true while the running instance keeps itself through reloads,
  // and waiting_ holds the newest build a reload brought meanwhile.
  bool held_ = false;
  std::optional<Build> waiting_;
  // movement_ steps every hero while a world is loaded. hero_slots_ maps
  // this frame's heroes to their slots, and floor_known_ is true once the
  // world entity under the heroes is.
  std::optional<quake::PlayerMovement> movement_;
  std::map<uint32_t, int32_t> hero_slots_;
  bool floor_known_ = false;
  // mod_ delivers events to the running instance.
  wasm::ModClient mod_;
  // wants_ is the running instance's start answer: the events it consumes.
  wasm::StartResult wants_;
  // game_ answers the running instance's calls and holds what it placed in
  // the game.
  PluginGame game_;
  // world_ follows world loads and endings for the plugin's lifetime; map_
  // names the loaded world, so a reload starts in it at once.
  Subscription world_;
  std::optional<std::string> map_;
  // players_ holds the instance's player callbacks; listening_ is true while
  // it does.
  Subscription players_;
  bool listening_ = false;
  // combat_ holds the instance's damage subscription while fighting_ is
  // true; it starts with a loaded world. adjusted_ carries the
  // amount the mod chose for the hit being decided, from suppression to
  // adjustment, which the engine runs back to back.
  Subscription combat_;
  bool fighting_ = false;
  std::optional<float> adjusted_;
  // damaged_ queues applied hits and facts_ movement facts, which the engine
  // may report off the engine thread, until a frame delivers them.
  std::mutex damaged_lock_;
  std::vector<gameinterop::DamageTakenEvent> damaged_;
  std::vector<wasm::Game::MovementFact> facts_;
  // sampled_ is the last tick the movement samples went out on.
  std::optional<uint64_t> sampled_;
  // ui_ follows the interface the running instance shows each player.
  wasm::UiTrees ui_;
  // tallies_ holds each player's metric totals by slot, with the account
  // they belong to, until the player leaves or the mod stops.
  std::map<int32_t, std::pair<uint64_t, wasm::Tally>> tallies_;
};

// kMaxDamaged bounds the applied hits and the movement facts queued between
// frames.
constexpr size_t kMaxDamaged = 1024;

// MovementActionOf names a movement execution the game announced.
wasm::MovementAction MovementActionOf(gameinterop::MovementExecution execution) {
  using enum gameinterop::MovementExecution;
  switch (execution) {
    case kLandedOnGround:
      return wasm::MOVEMENT_ACTION_LANDED;
    case kAttachedToZipline:
      return wasm::MOVEMENT_ACTION_ZIPLINE_ATTACHED;
    case kGroundDash:
      return wasm::MOVEMENT_ACTION_GROUND_DASH;
    case kSlide:
      return wasm::MOVEMENT_ACTION_SLIDE;
    case kBouncePadActivated:
      return wasm::MOVEMENT_ACTION_BOUNCE_PAD;
    case kDashJump:
      return wasm::MOVEMENT_ACTION_DASH_JUMP;
    case kAirJump:
      return wasm::MOVEMENT_ACTION_AIR_JUMP;
    case kWallJump:
      return wasm::MOVEMENT_ACTION_WALL_JUMP;
    case kAirDash:
      return wasm::MOVEMENT_ACTION_AIR_DASH;
    case kMeleeAttackStarted:
      return wasm::MOVEMENT_ACTION_MELEE_STARTED;
  }
  return wasm::MOVEMENT_ACTION_UNKNOWN;
}

// kPressCommand starts the console command a player's game sends when they
// press a mod's button: "modlockpress <mod> <node>". The engine names the
// player who sent it, so a press reaches the mod from any player's game.
constexpr std::string_view kPressCommand = "modlockpress ";

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

std::expected<std::unique_ptr<wasm::Instance>, std::string> WasmPlugin::Instantiate(
    std::span<const uint8_t> module) {
  return wasm::Instance::Load(host_.runtime, module, wasm::Limits{},
                              [this](const wasm::Call& call) { return Call(call); });
}

bool WasmPlugin::Start() {
  // The mod starts before it hears of any world: a world that is already
  // ready reaches it while it subscribes. Send has logged a failed start.
  if (!Begin(false)) return false;
  if (check_only_) return true;

  // Player commands need the network system, which loads with the first world.
  auto world = engine_->OnWorld([this](std::string_view) { host_.game.WorldStarting(); },
                                [this](std::string_view map) {
                                  map_ = std::string(map);
                                  host_.game.WorldReady();
                                  ListenToPlayers();
                                  ListenToCombat();
                                  World();
                                });
  if (world) {
    world_.Add(std::move(*world));
  } else {
    Log(std::string("player commands are unavailable: ") + world.error());
  }
  if (auto ending = engine_->OnWorldEnding([this] { game_.WorldEnding(); })) {
    world_.Add(std::move(*ending));
  } else {
    Log(std::string("world objects may outlive their world: ") + ending.error());
  }
  return true;
}

std::expected<void, std::string> WasmPlugin::Reload(Build build) {
  if (held_) {
    waiting_ = std::move(build);
    Log("the new build waits until the running one releases its hold");
    return {};
  }

  // Load the new build first, so a broken one leaves the old one running.
  auto instance = Instantiate(build.module);
  if (!instance) return std::unexpected(instance.error());

  // Stop the old build and start the new one in its place.
  Halt();
  Attach(std::move(*instance), std::move(build));
  return Begin(true);
}

std::expected<void, std::string> WasmPlugin::Press(int32_t slot, std::string_view node) {
  if (!instance_) return std::unexpected(name_ + " is not running");
  if (!ui_.IsButton(slot, node)) {
    return std::unexpected(name_ + " shows no button " + std::string(node));
  }
  wasm::UiPressEvent event;
  event.set_player(slot);
  event.set_node(std::string(node));
  static_cast<void>(mod_.UiPress(event));
  return {};
}

std::expected<std::string, std::string> WasmPlugin::Serve(const wasm::ServiceCall& call) {
  auto reply = mod_.Serve(call);
  if (!reply) return std::unexpected(reply.error());
  return std::move(*reply->mutable_payload());
}

std::expected<void, std::string> WasmPlugin::Begin(bool reloaded) {
  // Start the mod and learn which events it consumes.
  wasm::StartEvent event;
  for (const auto& arg : args_) event.add_args(arg);
  event.set_check_only(check_only_);
  event.set_source(source_.data(), source_.size());
  auto started = mod_.Start(event);
  if (!started) return std::unexpected(started.error());
  wants_ = std::move(*started);
  host_.Notify([&](WasmHostObserver& observer) { observer.Started(name_, reloaded); });
  if (map_) {
    ListenToPlayers();
    ListenToCombat();
    World();
  }
  return {};
}

void WasmPlugin::Hold(bool held) {
  if (held == held_) return;
  held_ = held;
  host_.Notify([&](WasmHostObserver& observer) { observer.Held(name_, held); });
}

void WasmPlugin::Halt() {
  // The hold ends with the instance that took it.
  Hold(false);
  if (movement_) static_cast<void>(host_.game.StepWith(nullptr));
  movement_.reset();
  hero_slots_.clear();
  Unsubscribe();
  wants_.Clear();
  game_.Clear();
  ui::Change reset;
  reset.set_reset(true);
  for (const auto slot : ui_.Clear()) ShowUi(slot, reset);
}

void WasmPlugin::World() {
  Move();
  floor_known_ = false;
  wasm::WorldEvent event;
  event.set_map(*map_);
  static_cast<void>(mod_.World(event));
}

void WasmPlugin::Tick() {
  // A build that waited for the hold's release replaces the running one
  // outside the mod's own calls.
  if (waiting_ && !held_) {
    auto build = std::move(*waiting_);
    waiting_.reset();
    if (auto reloaded = Reload(std::move(build)); !reloaded) {
      Log("the waiting build did not load: " + reloaded.error());
    }
  }
  if (!tuned_ && !check_only_) {
    tuned_ =
        Tune(L"server.dll", "server.dll", manifest_, [this](std::string_view text) { Log(text); });
  }
  game_.Frame();
  Steer();
  DeliverDamaged();
  DeliverGameEvents();
  if (!wants_.frames() || !instance_) return;
  wasm::FrameEvent frame;
  if (!check_only_) {
    if (auto server = host_.game.Server()) {
      if (auto clock = (*server)->ReadClock()) {
        frame.set_tick(static_cast<uint64_t>(clock->tick));
        frame.set_time_seconds(clock->current_time);
      }
    }
    if (map_ && game_.Moving()) ListenToCombat();
    Sample(frame);
  }
  static_cast<void>(mod_.Frame(frame));
}

void WasmPlugin::Stop() {
  waiting_.reset();
  world_.Reset();
  Halt();
  instance_.reset();
  while (!tallies_.empty()) HandOn(tallies_.begin()->first);
}

void WasmPlugin::Move() {
  if (movement_ || !instance_ || manifest_.movement().model() != wasm::Movement::MODEL_QUAKEWORLD) {
    return;
  }
  auto movement = quake::PlayerMovement::Create(quake::PlayerMovement::Side::kServer, collision_,
                                                manifest_.movement().scale());
  if (!movement) {
    Log("movement is unavailable: " + movement.error());
    return;
  }
  movement_ = std::move(*movement);
  auto stepping = host_.game.StepWith([this](gameinterop::MovementCall& call) {
    const auto ground = call.ground_entity;
    if (!movement_->Step(call)) return false;
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
  if (!movement_) return;
  for (const auto& landing : movement_->TakeLandings()) {
    const auto player = hero_slots_.find(landing.player);
    if (player == hero_slots_.end()) continue;
    const auto on = landing.on == 0 ? hero_slots_.end() : hero_slots_.find(landing.on);
    wasm::LandedEvent event;
    event.set_player(player->second);
    event.set_on(on == hero_slots_.end() ? -1 : on->second);
    event.set_speed(landing.speed);
    static_cast<void>(mod_.Landed(event));
    if (!movement_) return;
  }

  std::vector<quake::PlayerMovement::Player> players;
  hero_slots_.clear();
  for (const auto& hero : game_.Heroes()) {
    players.push_back({.pawn = hero.pawn, .handle = hero.handle, .feet = hero.feet});
    hero_slots_.emplace(hero.handle, hero.slot);
  }
  movement_->SetPlayers(std::move(players));
  if (floor_known_) return;

  // Heroes spawn in the air, so a trace down names the world entity they land
  // on before any has.
  auto trace = engine_->Trace();
  if (!trace || !(*trace)->IsReady()) return;
  gameinterop::TraceOptions options;
  options.interacts_with = gameinterop::TraceMask(gameinterop::TraceLayer::kSolid);
  auto floor = (*trace)->Query({0, 0, 100}, {0, 0, -100}, gameinterop::TraceLine{}, options);
  if (floor && floor->entity_handle) movement_->SetWorld(*floor->entity_handle);
  floor_known_ = true;
}

void WasmPlugin::ListenToPlayers() {
  if (listening_ || !instance_) return;

  // Offer the mod every player command: "/hello there" in chat arrives as
  // "hello there", as does the console command of the same line. Chat also
  // accepts ".", "+" or "-" in place of the slash, for players whose chat
  // keeps a slash line to itself.
  auto chats = engine_->OnChat([this](int32_t slot, std::string_view text) {
    if (!text.empty() && std::string_view("/.+-").contains(text.front()))
      Command(slot, text.substr(1));
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
  if (fighting_ || !instance_ || (!wants_.damage() && !wants_.damaged() && !game_.Moving())) {
    return;
  }
  std::function<bool(const gameinterop::DamageContactEvent&)> suppress;
  gameinterop::AdjustDamage adjust;
  if (wants_.damage()) {
    suppress = [this](const gameinterop::DamageContactEvent& contact) { return Hit(contact); };
    adjust = [this](const gameinterop::DamageContactEvent&, float& amount) {
      if (adjusted_) amount = *std::exchange(adjusted_, std::nullopt);
    };
  }
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
  adjusted_.reset();
  wasm::DamageEvent event;
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
  if (!result) return false;
  if (result->block()) return true;
  if (result->has_amount()) adjusted_ = result->amount();
  return false;
}

void WasmPlugin::Fought(const gameinterop::CombatEvent& event, bool damaged) {
  std::lock_guard lock(damaged_lock_);
  if (event.damage && damaged && damaged_.size() < kMaxDamaged) {
    damaged_.push_back(*event.damage);
  }
  if (facts_.size() >= kMaxDamaged) return;
  if (event.movement) {
    facts_.push_back({event.movement->pawn_handle, MovementActionOf(event.movement->execution)});
  }
  if (event.ability) {
    facts_.push_back({event.ability->caster_handle, wasm::MOVEMENT_ACTION_ABILITY_EXECUTED,
                      event.ability->ability_handle});
  }
}

void WasmPlugin::Sample(wasm::FrameEvent& frame) {
  if (frame.tick() == sampled_) return;
  sampled_ = frame.tick();
  std::vector<wasm::Game::MovementFact> facts;
  {
    std::lock_guard lock(damaged_lock_);
    facts.swap(facts_);
  }
  if (!game_.Moving()) return;
  for (auto& sample : game_.Movement(facts)) *frame.add_movement() = std::move(sample);
}

void WasmPlugin::DeliverDamaged() {
  std::vector<gameinterop::DamageTakenEvent> hits;
  {
    std::lock_guard lock(damaged_lock_);
    hits.swap(damaged_);
  }
  for (const auto& hit : hits) {
    if (!instance_) return;
    wasm::DamagedEvent event;
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
    if (!instance_) return;
    wasm::InputEvent event;
    event.set_player(change.slot);
    event.set_pressed(change.pressed);
    event.set_released(change.released);
    static_cast<void>(mod_.Input(event));
  }
  for (const auto& launch : game_.TakeLaunches()) {
    if (!instance_) return;
    static_cast<void>(mod_.Launch(launch));
  }
  for (const auto& restored : game_.TakeRestored()) {
    if (!instance_) return;
    wasm::RestoredEvent event;
    event.set_player(restored.slot);
    event.set_error(restored.error);
    static_cast<void>(mod_.Restored(event));
  }
  if (auto restored = game_.TakeNpcsRestored(); restored && instance_) {
    wasm::NpcsRestoredEvent event;
    event.set_error(*restored);
    static_cast<void>(mod_.NpcsRestored(event));
  }
}

void WasmPlugin::Unsubscribe() {
  players_.Reset();
  listening_ = false;
  combat_.Reset();
  fighting_ = false;
  adjusted_.reset();
  sampled_.reset();
  std::lock_guard lock(damaged_lock_);
  damaged_.clear();
  facts_.clear();
}

std::expected<wasm::Reply, std::string> WasmPlugin::Send(const wasm::Call& call) {
  if (!instance_) return std::unexpected(name_ + " is not running");
  auto reply = instance_->Deliver(call);
  if (!reply) {
    // A failed mod stops alone; a refused nested event leaves it running.
    if (instance_->Failure()) {
      Failed("stopped: " + reply.error());
      Halt();
      instance_.reset();
    }
    return reply;
  }

  // A handler's error goes to the service's caller, or else to the log.
  if (!reply->error().empty() && call.method() != "Serve") Log(reply->error());
  return reply;
}

bool WasmPlugin::Command(int32_t slot, std::string_view line) {
  // A press names its mod; the other mods leave it alone and never see it.
  if (line.starts_with(kPressCommand)) {
    const auto pressed = line.substr(kPressCommand.size());
    const auto space = pressed.find(' ');
    if (space == std::string_view::npos || pressed.substr(0, space) != name_) return false;
    if (auto refused = Press(slot, pressed.substr(space + 1)); !refused) Log(refused.error());
    return true;
  }

  wasm::CommandEvent event;
  event.set_player(slot);
  event.set_line(std::string(line));
  auto result = mod_.Command(event);
  return result && result->claimed();
}

void WasmPlugin::ShowUi(int32_t slot, const ui::Change& change) {
  host_.Notify([&](WasmHostObserver& observer) { observer.Ui(name_, slot, change); });
}

void WasmPlugin::Leave(int32_t slot) {
  HandOn(slot);
  game_.Leave(slot);
  if (!ui_.Drop(slot)) return;
  ui::Change reset;
  reset.set_reset(true);
  ShowUi(slot, reset);
}

wasm::Reply WasmPlugin::Call(const wasm::Call& call) {
  // A check runs no game; the mod may still log, reach the host's services
  // and hold its build.
  wasm::Reply reply;
  if (!check_only_ || call.method() == "Log" || call.method() == "CallService" ||
      call.method() == "HoldReload") {
    reply = game_.Dispatch(call);
  } else {
    reply.set_error("no game is running");
  }

  // Report a failed call in the server log too.
  if (!reply.error().empty()) Log(reply.error());
  return reply;
}

std::expected<void, std::string> WasmPlugin::ApplyUi(const wasm::UiRequest& request) {
  if (auto applied = ui_.Apply(request.player(), request.change()); !applied) return applied;
  ShowUi(request.player(), request.change());
  return {};
}

std::expected<wasm::ServiceReply, std::string> WasmPlugin::CallService(
    const wasm::ServiceCall& call) {
  auto found = host_.extensions.find(call.service());
  if (found == host_.extensions.end()) {
    return std::unexpected("this host provides no service " + call.service());
  }
  auto answer = found->second(name_, call.method(), call.payload());
  if (!answer) return std::unexpected(answer.error());
  wasm::ServiceReply reply;
  reply.set_payload(std::move(*answer));
  return reply;
}

std::expected<std::string, std::string> WasmPlugin::PlayerSetting(int32_t slot,
                                                                  std::string_view key) {
  const auto* setting = wasm::FindSetting(manifest_, key);
  if (!setting) return std::unexpected("mod.json declares no setting " + std::string(key));
  if (const auto steam_id = SteamId(slot); steam_id != 0) {
    if (auto kept = host_.settings->Value(name_, steam_id, key)) {
      if (auto value = wasm::Canonical(*setting, *kept)) return *value;
    }
  }
  return wasm::DefaultValue(*setting);
}

std::expected<void, std::string> WasmPlugin::SetPlayerSetting(int32_t slot, std::string_view key,
                                                              std::string_view value) {
  // Keep only a value the manifest declares, for a player with an account.
  const auto* setting = wasm::FindSetting(manifest_, key);
  if (!setting) return std::unexpected("mod.json declares no setting " + std::string(key));
  const auto canonical = wasm::Canonical(*setting, value);
  if (!canonical) {
    return std::unexpected(std::string(value) + " is not a value of setting " + std::string(key));
  }
  if (const auto steam_id = SteamId(slot); steam_id != 0) {
    host_.settings->Store(name_, steam_id, key, *canonical);
  }
  return {};
}

void WasmPlugin::SettingChanged(uint64_t steam_id, std::string_view key, std::string_view value) {
  // Deliver only a declared value to a running mod.
  const auto* setting = wasm::FindSetting(manifest_, key);
  if (!instance_ || !setting || steam_id == 0) return;
  const auto canonical = wasm::Canonical(*setting, value);
  if (!canonical) return;

  // Tell the mod for each slot the player holds.
  for (int32_t slot = 0; slot < wasm::kMaxPlayers; ++slot) {
    if (SteamId(slot) != steam_id) continue;
    wasm::SettingChangedEvent event;
    event.set_player(slot);
    event.set_key(std::string(key));
    event.set_value(*canonical);
    static_cast<void>(mod_.SettingChanged(event));
  }
}

std::expected<void, std::string> WasmPlugin::AddMetric(int32_t slot, std::string_view name,
                                                       double value, std::string_view label) {
  // Count only a declared metric and label, for a player with an account.
  const auto* metric = wasm::FindMetric(manifest_, name);
  if (!metric) return std::unexpected("mod.json declares no metric " + std::string(name));
  if (!wasm::Labeled(*metric, label)) {
    return std::unexpected(std::string(label) + " is not a label of metric " + std::string(name));
  }
  const auto steam_id = SteamId(slot);
  if (steam_id == 0) return {};

  // A new player in the slot starts their own totals.
  if (auto found = tallies_.find(slot); found != tallies_.end() && found->second.first != steam_id)
    HandOn(slot);
  auto& [account, tally] = tallies_[slot];
  account = steam_id;
  tally.Add(*metric, label, value);
  return {};
}

void WasmPlugin::HandOn(int32_t slot) {
  auto found = tallies_.find(slot);
  if (found == tallies_.end()) return;
  auto [steam_id, tally] = std::move(found->second);
  tallies_.erase(found);
  if (tally.Empty()) return;
  if (host_.metrics) {
    host_.metrics->Keep(name_, steam_id, tally.Totals());
  } else {
    Log("metrics for " + std::to_string(steam_id) + ": " + tally.Text());
  }
}

uint64_t WasmPlugin::SteamId(int32_t slot) {
  const auto state = gameinterop::ConnectionTracker::StateForSlot(slot);
  return state.occupied && !state.is_bot ? state.xuid : 0;
}

void WasmPlugin::Log(std::string_view text) {
  std::cerr << '[' << name_ << "] " << text << '\n';
  host_.Notify([&](WasmHostObserver& observer) { observer.Logged(name_, text); });
}

void WasmPlugin::Failed(std::string_view error) {
  std::cerr << '[' << name_ << "] " << error << '\n';
  host_.Notify([&](WasmHostObserver& observer) { observer.Failed(name_, error); });
}

std::expected<void, std::string> PluginGame::Log(const wasm::LogRequest& request) {
  plugin_.Log(request.message());
  return {};
}

std::expected<void, std::string> PluginGame::Ui(const wasm::UiRequest& request) {
  return plugin_.ApplyUi(request);
}

std::expected<wasm::ServiceReply, std::string> PluginGame::CallService(
    const wasm::ServiceCall& request) {
  return plugin_.CallService(request);
}

std::expected<wasm::SettingResponse, std::string> PluginGame::PlayerSetting(
    const wasm::PlayerSettingRequest& request) {
  auto value = plugin_.PlayerSetting(request.player(), request.key());
  if (!value) return std::unexpected(value.error());
  wasm::SettingResponse response;
  response.set_value(std::move(*value));
  return response;
}

std::expected<void, std::string> PluginGame::SetPlayerSetting(
    const wasm::SetPlayerSettingRequest& request) {
  return plugin_.SetPlayerSetting(request.player(), request.key(), request.value());
}

std::expected<void, std::string> PluginGame::AddMetric(const wasm::AddMetricRequest& request) {
  return plugin_.AddMetric(request.player(), request.name(), request.value(), request.label());
}

std::expected<void, std::string> PluginGame::HoldReload(const wasm::HoldReloadRequest& request) {
  plugin_.Hold(request.held());
  return {};
}

void PluginGame::Impacted(const wasm::ImpactEvent& event) { plugin_.Impacted(event); }

}  // namespace

// Impl holds the host's mods.
struct WasmHost::Impl : Mods {};

WasmHost::WasmHost(std::filesystem::path interpreters) : impl_(std::make_unique<Impl>()) {
  impl_->interpreters = std::move(interpreters);
}

WasmHost::~WasmHost() = default;

void WasmHost::Observe(WasmHostObserver* observer) {
  if (std::ranges::find(impl_->observers, observer) == impl_->observers.end()) {
    impl_->observers.push_back(observer);
  }
}

void WasmHost::Unobserve(WasmHostObserver* observer) { std::erase(impl_->observers, observer); }

std::expected<std::unique_ptr<Plugin>, std::string> WasmHost::Load(
    const std::filesystem::path& path, const PluginContext& context) {
  auto build = ReadBuild(path, impl_->interpreters);
  if (!build) return std::unexpected(build.error());
  if (context.launch != nullptr && !context.launch->connect.empty()) {
    return std::make_unique<ClientMod>(std::move(*build));
  }
  if (impl_->plugins.contains(build->name)) {
    return std::unexpected(path.string() + ": a mod named " + build->name + " is already loaded");
  }

  // Load the mod into a sandbox that answers through the plugin.
  auto plugin = std::make_unique<WasmPlugin>(*impl_, build->name, context);
  auto instance = plugin->Instantiate(build->module);
  if (!instance) return std::unexpected(build->name + ": " + instance.error());
  impl_->plugins.emplace(build->name, plugin.get());
  plugin->Attach(std::move(*instance), std::move(*build));
  return plugin;
}

std::expected<void, std::string> WasmHost::Reload(const std::filesystem::path& path) {
  auto build = ReadBuild(path, impl_->interpreters);
  if (!build) return std::unexpected(build.error());
  auto found = impl_->plugins.find(build->name);
  if (found == impl_->plugins.end()) {
    return std::unexpected("no running mod is named " + build->name);
  }
  return found->second->Reload(std::move(*build));
}

bool WasmHost::Held(std::string_view mod) const {
  const auto found = impl_->plugins.find(mod);
  return found != impl_->plugins.end() && found->second->Held();
}

void WasmHost::Provide(std::string service, WasmExtension extension) {
  if (extension) {
    impl_->extensions.insert_or_assign(std::move(service), std::move(extension));
  } else {
    impl_->extensions.erase(service);
  }
}

void WasmHost::KeepSettings(WasmSettings* settings) {
  impl_->settings = settings ? settings : &impl_->kept;
}

void WasmHost::KeepMetrics(WasmMetrics* metrics) { impl_->metrics = metrics; }

void WasmHost::SettingChanged(std::string_view mod, uint64_t steam_id, std::string_view key,
                              std::string_view value) {
  if (auto found = impl_->plugins.find(mod); found != impl_->plugins.end()) {
    found->second->SettingChanged(steam_id, key, value);
  }
}

std::expected<std::string, std::string> WasmHost::Call(std::string_view mod,
                                                       std::string_view service,
                                                       std::string_view method,
                                                       std::string_view payload) {
  auto found = impl_->plugins.find(mod);
  if (found == impl_->plugins.end())
    return std::unexpected("no running mod is named " + std::string(mod));
  wasm::ServiceCall call;
  call.set_service(std::string(service));
  call.set_method(std::string(method));
  call.set_payload(std::string(payload));
  return found->second->Serve(call);
}

}  // namespace modlock
