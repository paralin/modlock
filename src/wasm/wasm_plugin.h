#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "modlock/engine_host.h"
#include "modlock/gameinterop/movement_hook.h"
#include "modlock/plugin.h"
#include "modlock/wasm_host.h"
#include "modlock/wasm_settings.h"
#include "quake/player_movement.h"
#include "wasm/build.h"
#include "wasm/call_profile.h"
#include "wasm/files.h"
#include "wasm/game.h"
#include "wasm/instance.h"
#include "wasm/metrics.h"
#include "wasm/player_scope.h"
#include "wasm/runtime.h"
#include "wasm/settings.h"
#include "wasm/ui.h"

namespace modlock::wasm {

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
  // files holds a directory per mod for the files mods write, or is empty
  // when the host keeps none.
  std::filesystem::path files;
  // roles holds each mod's roles table by mod name, the empty name serving
  // every mod without its own, and roles_revision counts their changes.
  std::map<std::string, players::RolesTable, std::less<>> roles;
  uint64_t roles_revision = 0;

  // Notify calls report with each observer. An observer may stop observing
  // from its callback.
  template <typename Report>
  void Notify(const Report& report) {
    for (auto* observer : std::vector(observers)) {
      report(*observer);
    }
  }
};

// Tune writes manifest's definition changes into the loaded game module and
// returns whether that is done. A module whose definitions have not loaded
// yet is tried again on a later call.
bool Tune(std::wstring_view module, const char* module_name,
          const wasm::manifest::Manifest& manifest,
          const std::function<void(std::string_view)>& log);

// PluginGame is one mod's game with the calls that reach its plugin: its log,
// its interface and the services the host provides.
class PluginGame final : public wasm::Game {
 public:
  PluginGame(WasmPlugin& plugin, Mods& host, EngineHost* engine)
      : Game(host.game, engine), plugin_(plugin), host_(host) {}

  // AnswersWithoutGame reports whether the plugin answers method itself,
  // without the game: the calls this class overrides.
  static bool AnswersWithoutGame(std::string_view method);

 private:
  std::expected<void, std::string> Log(const wasm::LogRequest& request) override;
  std::expected<void, std::string> Ui(const wasm::ui::UiRequest& request) override;
  std::expected<wasm::players::PlayersResponse, std::string> Players() override;
  std::expected<wasm::ServiceReply, std::string> CallService(
      const wasm::ServiceCall& request) override;
  std::expected<wasm::settings::SettingResponse, std::string> PlayerSetting(
      const wasm::settings::PlayerSettingRequest& request) override;
  std::expected<void, std::string> SetPlayerSetting(
      const wasm::settings::SetPlayerSettingRequest& request) override;
  std::expected<void, std::string> AddMetric(
      const wasm::metrics::AddMetricRequest& request) override;
  std::expected<void, std::string> HoldReload(
      const wasm::abilities::HoldReloadRequest& request) override;
  std::expected<void, std::string> Handoff(const wasm::handoff::HandoffRequest& request) override;
  std::expected<wasm::handoff::HandedOffResponse, std::string> HandedOff() override;
  std::expected<void, std::string> PlayerHandoff(
      const wasm::handoff::HandoffPlayerRequest& request) override;
  std::expected<wasm::handoff::HandedOffResponse, std::string> PlayerHandedOff(
      const wasm::players::PlayerRequest& request) override;
  std::expected<wasm::files::ReadFileResponse, std::string> ReadFile(
      const wasm::files::ReadFileRequest& request) override;
  std::expected<void, std::string> WriteFile(const wasm::files::WriteFileRequest& request) override;
  void Impacted(const wasm::combat::ImpactEvent& event) override;

  // plugin_ owns the game; host_ holds the players' roles.
  WasmPlugin& plugin_;
  Mods& host_;
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
  // name names this running instance and slug its build.
  WasmPlugin(Mods& host, std::string name, std::string slug, const PluginContext& context)
      : host_(host),
        name_(std::move(name)),
        slug_(std::move(slug)),
        engine_(context.engine),
        check_only_(context.check_only),
        mod_([this](const wasm::Call& call) { return Send(call); }),
        game_(*this, host, context.engine) {
    for (int i = 1; i < context.argc; ++i) {
      args_.emplace_back(context.argv[i]);
    }
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
    // Take the instance and the build it runs.
    instance_ = std::move(instance);
    module_ = std::move(build.module);
    source_ = std::move(build.source);
    manifest_ = std::move(build.manifest);
    collision_ = std::move(build.collision);
    data_ = std::move(build.data);
    tuned_ = false;
    Withdraw();
  }

  // Reload replaces the running instance with build and starts it once the
  // old instance's bots have left, or keeps build for the first frame after
  // a held instance releases its hold.
  std::expected<void, std::string> Reload(Build build);

  // Hold keeps the running instance through reloads while held is true.
  void Hold(bool held);

  // Held reports whether the running instance keeps itself through reloads.
  bool Held() const { return held_; }

  // Handoff replaces the handoff state for the game, or for the player in
  // slot when slot is set: what the running build hands the next one. It
  // outlives instances, so a reload reads it back, but lives only in memory:
  // it ends when the mod stops or, for a player's, when the player leaves. A
  // bot keeps none.
  std::expected<void, std::string> Handoff(std::optional<int32_t> slot, std::string text);

  // HandedOff returns the handoff state for the game, or for the player in
  // slot when slot is set, or empty.
  std::string HandedOff(std::optional<int32_t> slot) const;

  // Scope limits the mod to scope. Each player it stops serving leaves it as
  // a disconnected player does, and the mod hears its new player list before
  // its next event.
  void Scope(WasmScope scope);

  // Scoped is the part of the server the mod serves.
  const PlayerScope& Scoped() const { return scope_; }

  // AskRestart restarts the running build in a new instance on the next
  // frame, while the plugin offers that: after the instance stopped or ran a
  // slow event. It reports whether the restart was offered.
  bool AskRestart();

  // Press delivers a press on a button the mod shows the player in slot.
  std::expected<void, std::string> Press(int32_t slot, std::string_view node);

  // Input delivers a change to an input the mod shows the player in slot:
  // line is the input's id, a space, and the value as UiTrees::Input reads
  // it.
  std::expected<void, std::string> Input(int32_t slot, std::string_view line);

  // Serve delivers one call to a service the mod serves and returns its answer.
  std::expected<std::string, std::string> Serve(const wasm::ServiceCall& call);

  // Log writes one line to the server log under the mod's name.
  void Log(std::string_view text);

  // ApplyUi changes the interface the mod shows one player.
  std::expected<void, std::string> ApplyUi(const wasm::ui::UiRequest& request);

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

  // Files returns the mod's files: those it wrote over those the running
  // build ships.
  ModFiles Files() const {
    return {.written = host_.files.empty() ? host_.files : host_.files / slug_, .data = data_};
  }

  // Impacted delivers a watched projectile's impact to the mod.
  void Impacted(const wasm::combat::ImpactEvent& event) {
    if (instance_) {
      static_cast<void>(mod_.Impact(event));
    }
  }

  uint32_t InterfaceVersion() const override { return PluginInterfaceVersion; }

  const char* Name() const override { return name_.c_str(); }

  // Slug names the mod's build, which keys its files, roles and interface.
  const std::string& Slug() const { return slug_; }

  bool Start() override;
  void Tick() override;
  void Stop() override;

 private:
  // Begin starts the mod and learns which events it consumes, reporting
  // whether it started.
  std::expected<void, std::string> Begin(bool reloaded);
  // Halt stops the running instance: it ends the instance's hold, movement,
  // subscriptions, game state and interfaces, then releases the instance.
  void Halt();
  // ListenForRestart hears the players' restart command for the plugin's
  // lifetime, since a stopped instance has no commands of its own.
  void ListenForRestart();
  // Slowed reports a slow event and offers the players a restart, in case
  // the mod is stuck.
  void Slowed(std::string_view report);
  // Stopped leaves a failed instance stopped and offers the players its
  // restart until one asks for it.
  void Stopped();
  // Warn tells every player in the game what happened to the mod, after its
  // name.
  void Warn(std::string_view what);
  // Withdraw ends the restart offer, and a request for it.
  void Withdraw();
  // Restart starts the running build again in a new instance, in place of a
  // stopped or a running one.
  void Restart();
  // Succeed halts the running instance and starts instance, with build when
  // a reload brought one, once the bots the halted instance kicked have left.
  std::expected<void, std::string> Succeed(std::unique_ptr<wasm::Instance> instance,
                                           std::optional<Build> build);
  // Arrive starts the instance that Succeed holds back, when its
  // predecessor's bots have left or kDepartWait passed.
  std::expected<void, std::string> Arrive();
  // World restarts movement and sends the mod its world event. Tick calls it
  // on the first frame after world_due_ is set.
  void World();
  // SteamId returns the account of the player in slot, or zero for a bot or
  // an empty slot.
  static uint64_t SteamId(int32_t slot);
  // Send is the transport of mod_: it delivers one event and stops a mod
  // whose instance failed.
  std::expected<wasm::Reply, std::string> Send(const wasm::Call& call);
  // ReportPlayers delivers PlayersChanged when the player slots changed
  // since the instance last heard of them.
  void ReportPlayers();
  void ListenToPlayers();
  void ListenToCombat();
  bool Hit(const gameinterop::DamageContactEvent& contact);
  // Contact returns what the mod does with a hit between the entities with
  // the handles attacker and victim.
  PlayerScope::Contact Contact(uint32_t attacker, uint32_t victim) const;
  // Fought queues an applied hit for a mod that wants it and a movement fact
  // for the movement samples.
  void Fought(const gameinterop::CombatEvent& event, bool damaged);
  void DeliverDamaged();
  // Sample fills a frame that starts a new tick with the watched heroes'
  // movement and the facts queued since the last one.
  void Sample(wasm::FrameEvent& frame);
  // ReportCalls sends the calls counted since the last report to the
  // observers, when there were any.
  void ReportCalls();
  // GameOpen reports whether the engine opened a game; a check opens none,
  // so its mod hears no game and reaches none.
  bool GameOpen() const;
  // Play runs a frame's game work: the definitions, the game's own frame,
  // the movement model and the queued deliveries.
  void Play();
  // Observe fills a frame with the game clock and the heroes' movement.
  void Observe(wasm::FrameEvent& frame);
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
  // Chat delivers one chat line to a mod that asked for chat.
  void Chat(const gameinterop::NativeChatHook::Message& line);
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
  // name_ names this running instance and slug_ its build; they differ when
  // one build runs as several instances.
  std::string name_;
  std::string slug_;
  // engine_ outlives the plugin; the host owns it.
  EngineHost* engine_;
  // check_only_ is true when the host only checks the mod, which the mod
  // hears at its start.
  bool check_only_;
  std::vector<std::string> args_;
  std::unique_ptr<wasm::Instance> instance_;
  // module_ is the running build's WebAssembly, which a restart instantiates
  // again; source_ is its interpreted entry, sent at each start.
  std::vector<uint8_t> module_;
  std::vector<uint8_t> source_;
  // manifest_ and collision_ are the running build's; tuned_ is true once the
  // manifest's definition changes are written in the current world.
  wasm::manifest::Manifest manifest_;
  std::vector<uint8_t> collision_;
  bool tuned_ = false;
  // data_ is the running build's data/ directory.
  std::filesystem::path data_;
  // clock_error_ is the last reason the server clock could not be read, so
  // each new reason is logged once.
  std::string clock_error_;
  // held_ is true while the running instance keeps itself through reloads,
  // and waiting_ holds the newest build a reload brought meanwhile.
  bool held_ = false;
  std::optional<Build> waiting_;

  // Successor is an instance that waits until a deadline for the bots its
  // predecessor kicked to leave, since a bot it adds under a leaving bot's
  // name takes that bot's queued kick.
  struct Successor {
    std::unique_ptr<wasm::Instance> instance;
    std::optional<Build> build;
    std::chrono::steady_clock::time_point until;
  };

  std::optional<Successor> successor_;

  // SlowOffer is the restart a slow event offers until a deadline.
  struct SlowOffer {
    std::chrono::steady_clock::time_point until;
  };

  // StoppedOffer is the restart a failed instance offers until a player
  // asks for it.
  struct StoppedOffer {};

  // RestartAsked is a player's request, which the next frame carries out.
  struct RestartAsked {};

  // Offer is the restart the players may ask for: none, a slow event's, a
  // stopped instance's, or one already asked for.
  using Offer = std::variant<std::monostate, SlowOffer, StoppedOffer, RestartAsked>;

  // offer_ is the current restart offer, and warned_at_ the players' last
  // warning. restart_command_ hears the request once it is active.
  Offer offer_;
  std::chrono::steady_clock::time_point warned_at_;
  Subscription restart_command_;
  // movement_ steps every hero while a world is loaded. hero_slots_ maps
  // this frame's heroes to their slots, and floor_known_ is true once the
  // world entity under the heroes is.
  std::optional<quake::PlayerMovement> movement_;
  std::map<uint32_t, int32_t> hero_slots_;
  bool floor_known_ = false;
  // mod_ delivers events to the running instance. sending_ is true while
  // one is delivered, and reported_ is the connection and roles revisions
  // the running instance last heard, or nullopt before it heard them.
  wasm::ModClient mod_;
  bool sending_ = false;
  std::optional<std::pair<uint64_t, uint64_t>> reported_;
  // wants_ is the running instance's start answer: the events it consumes.
  wasm::StartResult wants_;
  // scope_ is the players the mod serves, across instances.
  PlayerScope scope_;
  // game_ answers the running instance's calls and holds what it placed in
  // the game.
  PluginGame game_;
  // world_ follows world loads and endings for the plugin's lifetime; map_
  // names the loaded world, so a reload starts in it at once, and world_due_
  // is true while the running instance waits for the next frame to hear of
  // it.
  Subscription world_;
  std::optional<std::string> map_;
  bool world_due_ = false;
  // players_ holds the instance's player callbacks.
  Subscription players_;
  // combat_ holds the instance's damage subscription, which starts with a
  // loaded world. adjusted_ carries the
  // amount the mod chose for the hit being decided, from suppression to
  // adjustment, which the engine runs back to back.
  Subscription combat_;
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
  // handoff_ is the game's handoff state, and handoff_players_ each player's
  // by slot, with the account it belongs to. Both outlive instances until
  // the mod stops; a player's ends when they leave.
  std::string handoff_;
  std::map<int32_t, std::pair<uint64_t, std::string>> handoff_players_;
  // frames_resume_ is when the mod's next frame is due after a slow one.
  // skipped_ counts the frames skipped since shed_logged_, the last log of
  // them, slowest_ is the slowest frame since then, and slowest_report_
  // describes where that frame's processor time went.
  std::chrono::steady_clock::time_point frames_resume_;
  std::chrono::steady_clock::time_point shed_logged_;
  int skipped_ = 0;
  std::chrono::steady_clock::duration slowest_{};
  std::string slowest_report_;

  // calls_ counts the mod's host calls since calls_reported_, the last time
  // they went to the observers.
  wasm::CallProfile calls_;
  std::chrono::steady_clock::time_point calls_reported_;
};

}  // namespace modlock::wasm
