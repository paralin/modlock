#include "modlock/net/listen_boot.h"

#include <filesystem>
#include <string_view>

#if defined(_WIN32)
#include <windows.h>

#include <iostream>

#include "host_app/windows/engine_log.h"
#endif

namespace modlock::net {

namespace {

// Dedicated-server flags allow local joins without VAC or a Steam lobby.
// Replay and TV capture paths are disabled so idle console activation cannot
// touch an unwritable replay sink.
// -playtest disables engine user-config reads and writes. The dedicated host
// shares the game installation with the player and must not overwrite it.
// -nodedicatedconsole drops the engine's text console, which polls console
// input and redraws a status line every frame; host_app::ListenEngineLog
// carries the engine's messages to stdout instead. -novconsole keeps -insecure
// from opening the unauthenticated VConsole listener on every interface.
constexpr std::string_view kDedicatedFlags =
    "-dedicated -nodedicatedconsole -novconsole -dev -insecure"
    " -allow_no_lobby_connect -playtest"
    " +tv_citadel_auto_record 0 +spec_replay_enable 0 +tv_enable 0"
    " +citadel_upload_replay_enabled 0";

}  // namespace

std::string BuildDedicatedCommandLine(const LaunchConfig& config) {
  // Build the dedicated server's command line.
  std::string line(kDedicatedFlags);
  line += " +hostport " + std::to_string(config.host_port);
  line += " +ip 0.0.0.0";
  line += " +map " + config.map;
  return line;
}

std::expected<int, std::string> RunEngine(const LaunchConfig& config,
                                          const std::filesystem::path& engine_bin_dir) {
#if defined(_WIN32)
  // Stage 1: the engine module must already be mapped by the host app.
  HMODULE engine = ::GetModuleHandleW(L"engine2.dll");
  if (engine == nullptr) {
    return std::unexpected(
        "stage engine-module: engine2.dll is not mapped; the game modules "
        "must load before the engine handoff");
  }

  // Stage 2: resolve the exported entry point.
  const auto source2_main = reinterpret_cast<Source2MainFn>(
      reinterpret_cast<void*>(::GetProcAddress(engine, "Source2Main")));
  if (source2_main == nullptr) {
    return std::unexpected(
        "stage entry-point: Source2Main export not found in the mapped engine2.dll");
  }

  // Stage 3: render the staged command line. The game directory, the parent
  // of bin/win64, holds citadel and core; the -game argument names the mod
  // directory inside it, as stock dedicated servers do.
  const auto game_dir = engine_bin_dir.parent_path().parent_path();
  std::string command_line = BuildDedicatedCommandLine(config);
  command_line += " -game \"" + (game_dir / "citadel").string() + "\"";
  if (!config.engine_arguments.empty()) {
    command_line += " " + config.engine_arguments;
  }

  // Stage 4: the game directory must exist; the engine resolves citadel
  // content relative to it.
  if (!std::filesystem::directory_entry(game_dir).exists()) {
    return std::unexpected("stage base-dir: directory does not exist: " + game_dir.string());
  }

  // Stage 5: handoff. Blocking by design: Source2Main runs the server's
  // frame loop until shutdown. A dedicated server has no console, so its
  // messages reach stdout.
  if (auto listening = host_app::ListenEngineLog(engine); !listening) {
    std::cerr << "[modlock] " << listening.error() << '\n';
  }
  return source2_main(nullptr, nullptr, command_line.c_str(), 0, game_dir.string().c_str(),
                      "citadel");
#else
  (void)config;
  (void)engine_bin_dir;
  return std::unexpected("the engine handoff requires the Windows host build");
#endif
}

}  // namespace modlock::net
