#include "modlock/net/listen_boot.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string_view>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace modlock::net {

namespace {

// Dedicated-server flags allow local joins without VAC or a Steam lobby.
// Replay and TV capture paths are disabled so idle console activation cannot
// touch an unwritable replay sink.
// -playtest disables engine user-config reads and writes. The dedicated host
// shares the game installation with the player and must not overwrite it.
// -nodedicatedconsole drops the engine's text console, which polls console
// input and redraws a status line every frame; EngineLog carries the engine's
// messages to stdout instead. -novconsole keeps -insecure from opening the
// unauthenticated VConsole listener on every interface.
constexpr std::string_view kDedicatedFlags =
    "-dedicated -nodedicatedconsole -novconsole -dev -insecure"
    " -allow_no_lobby_connect -playtest"
    " +tv_citadel_auto_record 0 +spec_replay_enable 0 +tv_enable 0"
    " +citadel_upload_replay_enabled 0";

#if defined(_WIN32)
// EngineLog is a tier0 logging listener (tier0/logging.h ILoggingListener)
// that writes each engine message to stdout as it arrives. The slots after
// Log keep the interface's empty defaults.
class EngineLog {
 public:
  virtual void Log(const void* /*context*/, const char* message) {
    std::fwrite(message, 1, std::strlen(message), stdout);
    std::fflush(stdout);
  }
  virtual void OnFlush() {}
  virtual void OnChannelRegistered(int /*channel*/) {}
  virtual void OnChannelVerbosityChanged(int /*channel*/) {}
  virtual void OnChannelFlagsChanged(int /*channel*/) {}
};

// ListenEngineLog registers the process-lifetime EngineLog with tier0. Without
// tier0's export the engine runs with its messages unlogged.
void ListenEngineLog() {
  static EngineLog log;
  HMODULE tier0 = ::GetModuleHandleW(L"tier0.dll");
  if (tier0 == nullptr) return;
  const auto listen = reinterpret_cast<void (*)(EngineLog*)>(
      reinterpret_cast<void*>(::GetProcAddress(tier0, "LoggingSystem_RegisterLoggingListener")));
  if (listen != nullptr) listen(&log);
}
#endif

}  // namespace

std::string BuildDedicatedCommandLine(const LaunchConfig& config) {
  std::string line(kDedicatedFlags);
  line += " +hostport " + std::to_string(config.host_port);
  line += " +ip 0.0.0.0";
  line += " +map " + config.map;
  return line;
}

std::string BuildClientCommandLine(const LaunchConfig& config) {
  // The early-development notice is modal at startup and holds keyboard focus
  // until dismissed; dismissing it stores this same setting.
  return "-console -insecure -novid +deadlock_early_development_warning_disabled 1 +connect " +
         config.connect;
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
  std::string command_line =
      config.connect.empty() ? BuildDedicatedCommandLine(config) : BuildClientCommandLine(config);
  command_line += " -game \"" + (game_dir / "citadel").string() + "\"";
  if (!config.engine_arguments.empty()) command_line += " " + config.engine_arguments;
  // Stage 4: the game directory must exist; the engine resolves citadel
  // content relative to it.
  if (!std::filesystem::directory_entry(game_dir).exists()) {
    return std::unexpected("stage base-dir: directory does not exist: " + game_dir.string());
  }
  // Stage 5: handoff. Blocking by design: Source2Main runs the server or
  // client frame loop until shutdown. A client owns a visible game window; a
  // dedicated server has no console, so its messages reach stdout.
  const bool client = !config.connect.empty();
  if (!client) ListenEngineLog();
  const int code =
      source2_main(client ? ::GetModuleHandleW(nullptr) : nullptr, nullptr, command_line.c_str(),
                   client ? SW_SHOWDEFAULT : 0, game_dir.string().c_str(), "citadel");
  return code;
#else
  (void)config;
  (void)engine_bin_dir;
  return std::unexpected("the engine handoff requires the Windows host build");
#endif
}

}  // namespace modlock::net
