//go:build deps_only

#include "wasm/wasm_plugin.h"

#include <chrono>
#include <format>
#include <iostream>
#include <memory>
#include <string_view>
#include <utility>

#include "modlock/gameinterop/connection_tracker.h"
#include "modlock/gameinterop/entity_abi.h"
#include "modlock/gameinterop/mapped_module_image.h"
#include "wasm/definition_changes.h"

namespace modlock::wasm {

namespace {

// kFrameAllowance is the time a mod's frame may take before the mod skips
// frames to repay it: half of a 64-tick frame.
constexpr auto kFrameAllowance = std::chrono::milliseconds(8);

// kRestartCommand is the chat command that restarts a stopped or slow mod,
// alone for every such mod or followed by one mod's name. kWarnPeriod is how
// often the players hear of it, and kOfferWindow how long a slow event keeps
// the restart offered.
constexpr std::string_view kRestartCommand = "restartmod";
constexpr auto kWarnPeriod = std::chrono::minutes(1);
constexpr auto kOfferWindow = std::chrono::minutes(5);

// kDepartWait bounds how long a new instance waits for the bots its
// predecessor kicked to leave.
constexpr auto kDepartWait = std::chrono::seconds(5);

}  // namespace

bool Tune(std::wstring_view module, const char* module_name,
          const wasm::manifest::Manifest& manifest,
          const std::function<void(std::string_view)>& log) {
  // A manifest without definition changes needs no tuning.
  if (manifest.definitions().empty()) {
    return true;
  }

  // Wait for the module and the schema system to load.
  auto image = gameinterop::MappedModuleImage::ForModule(module);
  auto schema = gameinterop::ResolveSchemaSystem();
  if (!image || !schema) {
    return false;
  }

  // Change the definitions and log each problem.
  auto problems = wasm::ChangeDefinitions(*image, *schema, module_name, manifest.definitions());
  if (!problems) {
    return false;
  }
  for (const auto& problem : *problems) {
    log("definition change: " + problem);
  }
  return true;
}

std::expected<std::unique_ptr<wasm::Instance>, std::string> WasmPlugin::Instantiate(
    std::span<const uint8_t> module) {
  return wasm::Instance::Load(host_.runtime, module, wasm::Limits{},
                              [this](const wasm::Call& call) { return Call(call); });
}

bool WasmPlugin::Start() {
  // The mod starts before it hears of any world: a world that is already
  // ready reaches it while it subscribes. Send has logged a failed start.
  if (!Begin(false)) {
    return false;
  }
  if (check_only_) {
    return true;
  }

  // Player commands need the network system, which loads with the first world.
  auto world = engine_->OnWorld([this](std::string_view) { host_.game.WorldStarting(); },
                                [this](std::string_view map) {
                                  // Record the map and start everything that waits for a world.
                                  // Hero definitions belong to the world, so the definition
                                  // changes are written again.
                                  map_ = std::string(map);
                                  tuned_ = false;
                                  host_.game.WorldReady();
                                  game_.WorldReady();
                                  ListenForRestart();
                                  ListenToPlayers();
                                  ListenToCombat();
                                  world_due_ = true;
                                });
  if (world) {
    world_.Add(std::move(*world));
  } else {
    Log(std::string("player commands are unavailable: ") + world.error());
  }
  if (auto ending = engine_->OnWorldEnding([this] {
        world_due_ = false;
        game_.WorldEnding();
      })) {
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
  if (!instance) {
    return std::unexpected(instance.error());
  }

  return Succeed(std::move(*instance), std::move(build));
}

std::expected<void, std::string> WasmPlugin::Succeed(std::unique_ptr<wasm::Instance> instance,
                                                     std::optional<Build> build) {
  if (instance_) {
    Halt();
    instance_.reset();
  }
  successor_ = Successor{std::move(instance), std::move(build)};
  starting_until_ = std::chrono::steady_clock::now() + kDepartWait;
  return Arrive();
}

std::expected<void, std::string> WasmPlugin::Arrive() {
  // Wait for a removed bot to leave, unless the start is overdue.
  if (game_.Departing()) {
    if (std::chrono::steady_clock::now() < starting_until_) {
      return {};
    }
    Log("starts while a bot it removed has not left yet");
  }

  // Attach the successor and start it.
  auto successor = std::move(*successor_);
  successor_.reset();
  if (successor.build) {
    Attach(std::move(successor.instance), std::move(*successor.build));
  } else {
    instance_ = std::move(successor.instance);
  }
  return Begin(true);
}

std::expected<void, std::string> WasmPlugin::Press(int32_t slot, std::string_view node) {
  // Refuse a press the running mod does not show.
  if (!instance_) {
    return std::unexpected(name_ + " is not running");
  }
  if (!ui_.IsButton(slot, node)) {
    return std::unexpected(name_ + " shows no button " + std::string(node));
  }

  // Deliver the press to the mod.
  wasm::ui::UiPressEvent event;
  event.set_player(slot);
  event.set_node(std::string(node));
  static_cast<void>(mod_.UiPress(event));
  return {};
}

std::expected<std::string, std::string> WasmPlugin::Serve(const wasm::ServiceCall& call) {
  auto reply = mod_.Serve(call);
  if (!reply) {
    return std::unexpected(reply.error());
  }
  return std::move(*reply->mutable_payload());
}

std::expected<void, std::string> WasmPlugin::Begin(bool reloaded) {
  // Start the mod and learn which events it consumes.
  wasm::StartEvent event;
  for (const auto& arg : args_) {
    event.add_args(arg);
  }
  event.set_check_only(check_only_);
  event.set_source(source_.data(), source_.size());
  reported_.reset();
  game_.StartFreeForAll(manifest_.free_for_all());
  game_.StartContentAddon(manifest_.slug() + "-" + manifest_.version());

  // Start it and keep the events it wants.
  auto started = mod_.Start(event);
  if (!started) {
    return std::unexpected(started.error());
  }
  wants_ = std::move(*started);
  host_.Notify([&](WasmHostObserver& observer) { observer.Started(name_, reloaded); });
  if (map_) {
    ListenToPlayers();
    ListenToCombat();
    world_due_ = true;
  }
  return {};
}

void WasmPlugin::Hold(bool held) {
  if (held == held_) {
    return;
  }
  held_ = held;
  host_.Notify([&](WasmHostObserver& observer) { observer.Held(name_, held); });
}

void WasmPlugin::Halt() {
  // The hold ends with the instance that took it.
  Hold(false);
  if (movement_) {
    static_cast<void>(host_.game.StepWith(nullptr));
  }

  // Drop the movement, the subscriptions and the game state.
  movement_.reset();
  hero_slots_.clear();
  Unsubscribe();
  wants_.Clear();
  game_.Clear();

  // Reset every player's interface.
  ui::Change reset;
  reset.set_reset(true);
  for (const auto slot : ui_.Clear()) {
    ShowUi(slot, reset);
  }
}

void WasmPlugin::ListenForRestart() {
  // Hear "/restartmod" or "/restartmod <mod>", with the prefixes the mod's
  // own commands take.
  if (restart_command_.Active()) {
    return;
  }
  auto chats = engine_->OnChat([this](int32_t, std::string_view text) {
    // Accept only the restart command, for this mod or for all.
    if (text.empty() || !std::string_view("/.+-").contains(text.front())) {
      return;
    }
    text.remove_prefix(1);
    if (!text.starts_with(kRestartCommand)) {
      return;
    }
    text.remove_prefix(kRestartCommand.size());
    if (!text.empty() && text.front() != ' ') {
      return;
    }
    const auto named = text.find_first_not_of(' ');
    if (named == std::string_view::npos || text.substr(named) == name_) {
      AskRestart();
    }
  });
  if (!chats) {
    Log("the restart command is unavailable: " + chats.error());
    return;
  }
  restart_command_ = std::move(*chats);
}

void WasmPlugin::Slowed(std::string_view report) {
  // A slow event holds the server's tick, so the players hear of it at once
  // and may restart a mod that stays stuck.
  Log(std::string("slow: ") + std::string(report));
  host_.Notify([&](WasmHostObserver& observer) { observer.Slowed(name_, report); });
  const auto now = std::chrono::steady_clock::now();
  offer_until_ = now + kOfferWindow;
  if (now - warned_at_ >= kWarnPeriod) {
    Warn(std::format("is running slowly. If it seems stuck, type /{} to restart it.",
                     kRestartCommand));
  }
}

void WasmPlugin::Stopped() {
  // A restart runs the mod's start on the engine thread, which a mod that
  // keeps failing would hold, so the players choose when it happens.
  stopped_ = true;
  restart_ = false;
  Log(std::format("stays stopped until a player types /{}", kRestartCommand));
  Warn(std::format("stopped. Type /{} to start it again.", kRestartCommand));
}

void WasmPlugin::Warn(std::string_view what) {
  // Address each player, since the game's chat goes to one slot at a time.
  warned_at_ = std::chrono::steady_clock::now();
  if (check_only_) {
    return;
  }
  auto messages = host_.game.Messages();
  if (!messages) {
    return;
  }
  const auto& title = manifest_.name().empty() ? name_ : manifest_.name();
  const auto text = std::format("{} {}", title, what);
  for (int32_t slot = 0; slot < gameinterop::kMaxPlayerSlots; ++slot) {
    const auto player = gameinterop::ConnectionTracker::StateForSlot(slot);
    if (player.occupied && !player.is_bot) {
      static_cast<void>((*messages)->Chat(slot, text));
    }
  }
}

void WasmPlugin::Withdraw() {
  stopped_ = false;
  restart_ = false;
  offer_until_ = {};
}

void WasmPlugin::Restart() {
  // A successor already on its way is the restart.
  Withdraw();
  if (successor_) {
    return;
  }

  // A build that no longer instantiates stays stopped, still offered, and
  // clears what a running instance placed and shows, as a reload does.
  auto instance = Instantiate(module_);
  if (!instance) {
    if (instance_) {
      Halt();
      instance_.reset();
    }
    Failed("did not restart: " + instance.error());
    Stopped();
    return;
  }

  // Start it in the running world; Send stops it again if it fails.
  if (auto started = Succeed(std::move(*instance), std::nullopt); !started) {
    Log("did not restart: " + started.error());
  }
}

void WasmPlugin::World() {
  // Restart movement and tell the mod the world began.
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
  if (successor_) {
    if (auto started = Arrive(); !started) {
      Log("did not start: " + started.error());
    }
  }

  // Restart a stopped mod when asked, or remind players it stopped.
  if (restart_) {
    Restart();
  } else if (stopped_ && std::chrono::steady_clock::now() - warned_at_ >= kWarnPeriod) {
    Warn(std::format("stopped. Type /{} to start it again.", kRestartCommand));
  }

  // Tell the mod of its world on the world's first frame: the server starts
  // a world before it can take the objects a mod creates there, and a
  // particle created from the startup crashes the server.
  if (std::exchange(world_due_, false)) {
    World();
  }

  // Change the server's definitions once per world.
  if (!tuned_ && !check_only_) {
    tuned_ =
        Tune(L"server.dll", "server.dll", manifest_, [this](std::string_view text) { Log(text); });
  }

  // Run the game's frame work.
  game_.Frame();
  Steer();
  DeliverDamaged();
  DeliverGameEvents();
  if (!wants_.frames() || !instance_) {
    return;
  }

  // A mod that ran a slow frame skips frames until as much time passed, so
  // it keeps at most half of the engine thread and no player times out. A mod
  // reads the game clock, so a skipped frame delays its work without losing it.
  const auto began = std::chrono::steady_clock::now();
  if (began < frames_resume_) {
    ++skipped_;
    return;
  }
  wasm::FrameEvent frame;
  if (!check_only_) {
    if (auto server = host_.game.Server()) {
      // A frame without a clock reads tick zero, which stalls every timer a
      // mod keeps, so log each new reason the clock is unreadable.
      if (auto clock = (*server)->ReadClock()) {
        frame.set_tick(static_cast<uint64_t>(clock->tick));
        frame.set_time_seconds(clock->current_time);
        clock_error_.clear();
      } else if (clock.error() != clock_error_) {
        clock_error_ = clock.error();
        Log("the server clock is unreadable: " + clock_error_);
      }
    }
    if (map_ && game_.Moving()) {
      ListenToCombat();
    }
    Sample(frame);
  }
  static_cast<void>(mod_.Frame(frame));

  // Charge a slow frame to the frames after it. The slowest frame keeps its
  // processor time report, since time the engine thread spent waiting, as
  // on a page fault or a lock, is in the frame but not in its report.
  const auto ended = std::chrono::steady_clock::now();
  const auto spent = ended - began;
  if (spent > slowest_ && instance_) {
    slowest_ = spent;
    slowest_report_ = instance_->Report();
  }
  if (spent > kFrameAllowance) {
    frames_resume_ = ended + spent;
  }

  // Log the skipped frames at most once a minute.
  if (skipped_ > 0 && ended - shed_logged_ >= std::chrono::minutes(1)) {
    // Log the shed frames at most once a minute.
    const auto slowest = std::chrono::duration_cast<std::chrono::milliseconds>(slowest_);
    Log(std::format("skipped {} frames to repay slow ones; the slowest took {} ms: {}", skipped_,
                    slowest.count(), slowest_report_));
    skipped_ = 0;
    slowest_ = {};
    shed_logged_ = ended;
  }
}

void WasmPlugin::Stop() {
  // Drop the waiting builds and the restart command.
  waiting_.reset();
  successor_.reset();
  Withdraw();
  restart_command_.Reset();

  // Stop the mod and hand on every player's metrics.
  world_.Reset();
  Halt();
  instance_.reset();
  while (!tallies_.empty()) {
    HandOn(tallies_.begin()->first);
  }
}

void WasmPlugin::Log(std::string_view text) {
  std::cerr << '[' << name_ << "] " << text << '\n';
  host_.Notify([&](WasmHostObserver& observer) { observer.Logged(name_, text); });
}

void WasmPlugin::Failed(std::string_view error) {
  std::cerr << '[' << name_ << "] " << error << '\n';
  host_.Notify([&](WasmHostObserver& observer) { observer.Failed(name_, error); });
}

}  // namespace modlock::wasm
