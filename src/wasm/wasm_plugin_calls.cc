//go:build deps_only

#include <algorithm>
#include <array>
#include <format>
#include <string_view>
#include <utility>

#include "modlock/gameinterop/connection_tracker.h"
#include "wasm/wasm_plugin.h"

namespace modlock::wasm {

namespace {

// kPressCommand starts the console command a player's game sends when they
// press a mod's button: "modlockpress <mod> <node>". The engine names the
// player who sent it, so a press reaches the mod from any player's game.
constexpr std::string_view kPressCommand = "modlockpress ";

// kInputCommand starts the console command a player's game sends when they
// change an input in a mod's interface: "modlockinput <mod> <node> <value>",
// with the value as UiTrees::Input reads it.
constexpr std::string_view kInputCommand = "modlockinput ";

// kHandoffLimit is the most bytes of handoff state a mod holds for the game
// or for one player.
constexpr size_t kHandoffLimit = 64 * 1024;

}  // namespace

std::expected<wasm::Reply, std::string> WasmPlugin::Send(const wasm::Call& call) {
  // Every event after Start sees the current players, which the report
  // before it carries; a nested event is refused, so it reports nothing.
  if (!sending_ && call.method() != "Start" && call.method() != "PlayersChanged") {
    ReportPlayers();
  }
  if (!instance_) {
    return std::unexpected(name_ + " is not running");
  }

  // An event about a player the mod does not serve never reaches it.
  if (!scope_.Hears(call)) {
    return wasm::Reply();
  }
  const bool nested = std::exchange(sending_, true);
  auto reply = instance_->Deliver(call);
  sending_ = nested;
  if (!reply) {
    // A failed mod stops alone; a refused nested event leaves it running.
    if (instance_->Failure()) {
      Failed("stopped: " + reply.error());
      Halt();
      Stopped();
    }
    return reply;
  }

  // A handler's error goes to the service's caller, or else to the log. A
  // build from before PlayersChanged answers that it has no such method,
  // which it says at every report, so the report's error is not logged.
  if (!reply->error().empty() && call.method() != "Serve" && call.method() != "PlayersChanged") {
    Log(reply->error());
  }
  if (instance_->Slow()) {
    Slowed(instance_->Report());
  }
  return reply;
}

void WasmPlugin::ReportPlayers() {
  // Report only a changed set of players.
  if (!GameOpen() || !instance_) {
    return;
  }
  const auto revision = std::pair(gameinterop::ConnectionTracker::Revision(), host_.roles_revision);
  if (reported_ == revision) {
    return;
  }

  // Mark it heard first, so a build that refuses the report gets it once
  // per change rather than before every event.
  reported_ = revision;

  // The report lists the players as the mod's Players call does.
  auto players = static_cast<wasm::HostService&>(game_).Players();
  if (!players) {
    return;
  }
  scope_.Keep(*players->mutable_players());
  wasm::players::PlayersChangedEvent event;
  *event.mutable_players() = std::move(*players->mutable_players());
  static_cast<void>(mod_.PlayersChanged(event));
}

bool WasmPlugin::Command(int32_t slot, std::string_view line) {
  // A player the mod does not serve neither presses its buttons nor reaches
  // its commands.
  if (!scope_.Serves(slot)) {
    return false;
  }

  // A press names its mod; the other mods leave it alone and never see it.
  if (line.starts_with(kPressCommand)) {
    // A press names this mod, then its command.
    const auto pressed = line.substr(kPressCommand.size());
    const auto space = pressed.find(' ');
    if (space == std::string_view::npos || pressed.substr(0, space) != name_) {
      return false;
    }
    if (auto refused = Press(slot, pressed.substr(space + 1)); !refused) {
      Log(refused.error());
    }
    return true;
  }
  if (line.starts_with(kInputCommand)) {
    // A change names this mod, then its input and the value.
    const auto changed = line.substr(kInputCommand.size());
    const auto space = changed.find(' ');
    if (space == std::string_view::npos || changed.substr(0, space) != name_) {
      return false;
    }
    if (auto refused = Input(slot, changed.substr(space + 1)); !refused) {
      Log(refused.error());
    }
    return true;
  }

  // Then the free for all, then the mod, may claim the command.
  if (game_.FreeForAllCommand(line)) {
    return true;
  }
  wasm::CommandEvent event;
  event.set_player(slot);
  event.set_line(std::string(line));
  auto result = mod_.Command(event);
  return result && result->claimed();
}

void WasmPlugin::Chat(const gameinterop::NativeChatHook::Message& line) {
  wasm::ChatEvent event;
  event.set_player(line.slot);
  event.set_text(line.text);
  event.set_all(line.all);
  event.set_lane(line.lane);
  static_cast<void>(mod_.Chat(event));
}

void WasmPlugin::ShowUi(int32_t slot, const ui::Change& change) {
  host_.Notify([&](WasmHostObserver& observer) { observer.Ui(name_, slot, change); });
}

void WasmPlugin::Leave(int32_t slot) {
  // Hand on the player's metrics and drop their state.
  HandOn(slot);
  handoff_players_.erase(slot);
  game_.Leave(slot);
  if (!ui_.Drop(slot)) {
    return;
  }

  // Reset their interface.
  ui::Change reset;
  reset.set_reset(true);
  ShowUi(slot, reset);
}

void WasmPlugin::Scope(std::optional<std::set<int32_t>> players) {
  // Each occupied slot the old scope served and the new one does not leaves
  // the mod.
  PlayerScope next;
  next.Limit(std::move(players));
  for (int32_t slot = 0; slot < gameinterop::kMaxPlayerSlots; ++slot) {
    if (scope_.Serves(slot) && !next.Serves(slot) &&
        gameinterop::ConnectionTracker::StateForSlot(slot).occupied) {
      Leave(slot);
    }
  }

  // The mod hears its new players before its next event.
  scope_ = std::move(next);
  reported_.reset();
}

wasm::Reply WasmPlugin::Call(const wasm::Call& call) {
  // A mod that serves some players reaches only them. Without a game it
  // reaches only what the plugin answers itself.
  wasm::Reply reply;
  if (auto refused = scope_.Refuse(call); !refused.empty()) {
    reply.set_error(std::move(refused));
  } else if (GameOpen() || PluginGame::AnswersWithoutGame(call.method())) {
    reply = game_.Dispatch(call);
  } else {
    reply.set_error("no game is running");
  }

  // Its player list holds only its players.
  if (scope_.Limited() && call.method() == "Players" && reply.error().empty()) {
    players::PlayersResponse players;
    if (players.ParseFromString(reply.response())) {
      scope_.Keep(*players.mutable_players());
      reply.set_response(players.SerializeAsString());
    }
  }

  // Report a failed call in the server log too.
  if (!reply.error().empty()) {
    Log(reply.error());
  }
  return reply;
}

std::expected<void, std::string> WasmPlugin::ApplyUi(const wasm::ui::UiRequest& request) {
  if (auto applied = ui_.Apply(request.player(), request.change()); !applied) {
    return applied;
  }
  ShowUi(request.player(), request.change());
  return {};
}

std::expected<wasm::ServiceReply, std::string> WasmPlugin::CallService(
    const wasm::ServiceCall& call) {
  // Find the service, ask it, and wrap its answer.
  auto found = host_.extensions.find(call.service());
  if (found == host_.extensions.end()) {
    return std::unexpected("this host provides no service " + call.service());
  }
  auto answer = found->second(name_, call.method(), call.payload());
  if (!answer) {
    return std::unexpected(answer.error());
  }
  wasm::ServiceReply reply;
  reply.set_payload(std::move(*answer));
  return reply;
}

std::expected<std::string, std::string> WasmPlugin::PlayerSetting(int32_t slot,
                                                                  std::string_view key) {
  const auto* setting = wasm::FindSetting(manifest_, key);
  if (!setting) {
    return std::unexpected("mod.json declares no setting " + std::string(key));
  }
  if (const auto steam_id = SteamId(slot); steam_id != 0) {
    if (auto kept = host_.settings->Value(name_, steam_id, key)) {
      if (auto value = wasm::Canonical(*setting, *kept)) {
        return *value;
      }
    }
  }
  return wasm::DefaultValue(*setting);
}

std::expected<void, std::string> WasmPlugin::SetPlayerSetting(int32_t slot, std::string_view key,
                                                              std::string_view value) {
  // Keep only a value the manifest declares, for a player with an account.
  const auto* setting = wasm::FindSetting(manifest_, key);
  if (!setting) {
    return std::unexpected("mod.json declares no setting " + std::string(key));
  }
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
  if (!instance_ || !setting || steam_id == 0) {
    return;
  }
  const auto canonical = wasm::Canonical(*setting, value);
  if (!canonical) {
    return;
  }

  // Tell the mod for each slot the player holds.
  for (int32_t slot = 0; slot < gameinterop::kMaxPlayerSlots; ++slot) {
    // Send the change to this slot when the player holds it.
    if (SteamId(slot) != steam_id) {
      continue;
    }
    wasm::settings::SettingChangedEvent event;
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
  if (!metric) {
    return std::unexpected("mod.json declares no metric " + std::string(name));
  }
  if (!wasm::Labeled(*metric, label)) {
    return std::unexpected(std::string(label) + " is not a label of metric " + std::string(name));
  }
  const auto steam_id = SteamId(slot);
  if (steam_id == 0) {
    return {};
  }

  // A new player in the slot starts their own totals.
  if (auto found = tallies_.find(slot);
      found != tallies_.end() && found->second.first != steam_id) {
    HandOn(slot);
  }
  auto& [account, tally] = tallies_[slot];
  account = steam_id;
  tally.Add(*metric, label, value);
  return {};
}

std::expected<void, std::string> WasmPlugin::Handoff(std::optional<int32_t> slot,
                                                     std::string text) {
  // Refuse state past the limit, which the mod's memory limit does not cover.
  if (text.size() > kHandoffLimit) {
    return std::unexpected(std::format("handoff state of {} bytes passes the limit of {}",
                                       text.size(), kHandoffLimit));
  }

  // Hold the game's state, or the player's with their account.
  if (!slot) {
    handoff_ = std::move(text);
    return {};
  }
  const auto steam_id = SteamId(*slot);
  if (steam_id == 0 || text.empty()) {
    handoff_players_.erase(*slot);
    return {};
  }
  handoff_players_.insert_or_assign(*slot, std::pair{steam_id, std::move(text)});
  return {};
}

std::string WasmPlugin::HandedOff(std::optional<int32_t> slot) const {
  // A player reads only what was handed off for their own account in the slot.
  if (!slot) {
    return handoff_;
  }
  const auto found = handoff_players_.find(*slot);
  if (found == handoff_players_.end() || found->second.first != SteamId(*slot)) {
    return {};
  }
  return found->second.second;
}

void WasmPlugin::HandOn(int32_t slot) {
  // Take the player's tally.
  auto found = tallies_.find(slot);
  if (found == tallies_.end()) {
    return;
  }
  auto [steam_id, tally] = std::move(found->second);
  tallies_.erase(found);
  if (tally.Empty()) {
    return;
  }

  // Keep it, or log it without a metrics store.
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

bool PluginGame::AnswersWithoutGame(std::string_view method) {
  // Keep this list to the host calls this class overrides.
  static constexpr std::array<std::string_view, 13> kMethods = {
      "Log",        "Ui",      "CallService", "PlayerSetting", "SetPlayerSetting", "AddMetric",
      "HoldReload", "Handoff", "HandedOff",   "PlayerHandoff", "PlayerHandedOff",  "ReadFile",
      "WriteFile",
  };
  return std::ranges::find(kMethods, method) != kMethods.end();
}

std::expected<wasm::players::PlayersResponse, std::string> PluginGame::Players() {
  // Give each human the roles the table lists for its connection, or the
  // humans' roles when it lists none. A stale entry gives nothing.
  auto players = Game::Players();
  if (!players) {
    return players;
  }
  for (auto& connection : *players->mutable_players()) {
    if (connection.bot()) {
      continue;
    }
    const auto& listed = host_.roles.connections();
    const auto entry =
        std::ranges::find(listed, connection.steam_id(), &wasm::players::ConnectionRoles::steam_id);
    if (entry == listed.end()) {
      *connection.mutable_roles() = host_.roles.humans();
    } else if (entry->generation() == connection.generation()) {
      *connection.mutable_roles() = entry->roles();
    }
  }
  return players;
}

std::expected<void, std::string> PluginGame::Log(const wasm::LogRequest& request) {
  plugin_.Log(request.message());
  return {};
}

std::expected<void, std::string> PluginGame::Ui(const wasm::ui::UiRequest& request) {
  return plugin_.ApplyUi(request);
}

std::expected<wasm::ServiceReply, std::string> PluginGame::CallService(
    const wasm::ServiceCall& request) {
  return plugin_.CallService(request);
}

std::expected<wasm::settings::SettingResponse, std::string> PluginGame::PlayerSetting(
    const wasm::settings::PlayerSettingRequest& request) {
  // Read the setting and wrap it.
  auto value = plugin_.PlayerSetting(request.player(), request.key());
  if (!value) {
    return std::unexpected(value.error());
  }
  wasm::settings::SettingResponse response;
  response.set_value(std::move(*value));
  return response;
}

std::expected<void, std::string> PluginGame::SetPlayerSetting(
    const wasm::settings::SetPlayerSettingRequest& request) {
  return plugin_.SetPlayerSetting(request.player(), request.key(), request.value());
}

std::expected<void, std::string> PluginGame::AddMetric(
    const wasm::metrics::AddMetricRequest& request) {
  return plugin_.AddMetric(request.player(), request.name(), request.value(), request.label());
}

std::expected<void, std::string> PluginGame::HoldReload(
    const wasm::abilities::HoldReloadRequest& request) {
  plugin_.Hold(request.held());
  return {};
}

std::expected<void, std::string> PluginGame::Handoff(const wasm::handoff::HandoffRequest& request) {
  return plugin_.Handoff(std::nullopt, request.text());
}

std::expected<wasm::handoff::HandedOffResponse, std::string> PluginGame::HandedOff() {
  wasm::handoff::HandedOffResponse response;
  response.set_text(plugin_.HandedOff(std::nullopt));
  return response;
}

std::expected<void, std::string> PluginGame::PlayerHandoff(
    const wasm::handoff::HandoffPlayerRequest& request) {
  return plugin_.Handoff(request.player(), request.text());
}

std::expected<wasm::handoff::HandedOffResponse, std::string> PluginGame::PlayerHandedOff(
    const wasm::players::PlayerRequest& request) {
  wasm::handoff::HandedOffResponse response;
  response.set_text(plugin_.HandedOff(request.player()));
  return response;
}

std::expected<wasm::files::ReadFileResponse, std::string> PluginGame::ReadFile(
    const wasm::files::ReadFileRequest& request) {
  // Read the file and wrap it, leaving the text unset for a missing file.
  auto text = plugin_.Files().Read(request.name());
  if (!text) {
    return std::unexpected(text.error());
  }
  wasm::files::ReadFileResponse response;
  if (*text) {
    response.set_text(std::move(**text));
  }
  return response;
}

std::expected<void, std::string> PluginGame::WriteFile(
    const wasm::files::WriteFileRequest& request) {
  return plugin_.Files().Write(request.name(), request.text());
}

void PluginGame::Impacted(const wasm::combat::ImpactEvent& event) {
  plugin_.Impacted(event);
}

}  // namespace modlock::wasm
