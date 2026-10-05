#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "host_app/control_link.h"
#include "modlock/build.h"
#include "modlock/engine_host.h"
#include "modlock/host.h"
#include "modlock/host_app/stdio_guard.h"
#include "modlock/plugin_library.h"
#include "modlock/wasm_host.h"

namespace {

void Help() {
  std::cout << "Modlock hosts Deadlock mods: sandboxed WebAssembly modules and native plugins.\n\n"
               "Usage: modlock-host --plugin PATH [options] [-- plugin arguments]\n\n"
               "  --plugin PATH       Load a built mod (a directory with mod.json or a .wasm\n"
               "                      file) or a plugin library; may be repeated\n"
               "  --game-dir PATH     Deadlock installation (or DEADLOCK_DIR)\n"
               "  --hostport PORT     Server UDP port (default 27067)\n"
               "  --map NAME          Startup map (default dl_midtown)\n"
               "  --connect ADDRESS   Run a game client that joins ADDRESS instead of a server\n"
               "  --engine-args ARGS  Append engine command-line arguments\n"
               "  --settings PATH     Keep players' mod settings in the JSON file PATH\n"
               "  --control ADDRESS   Report to and take reloads from the controller at\n"
               "                      ADDRESS, a host and port\n"
               "  --check-plugin      Check library compatibility and lifecycle without a game\n"
               "  --version           Print the framework source revision\n"
               "  --help              Show this help\n";
}

std::optional<uint16_t> Port(std::string_view value) {
  unsigned port = 0;
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), port);
  if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || port > 65535) {
    return std::nullopt;
  }
  return static_cast<uint16_t>(port);
}

// FailureRecorder keeps the first error of a mod that stopped, which fails
// a check: a mod stops itself without stopping the host.
class FailureRecorder final : public modlock::WasmHostObserver {
 public:
  std::string error;

  void Started(std::string_view, bool) override {}
  void Logged(std::string_view, std::string_view) override {}
  void Failed(std::string_view mod, std::string_view why) override {
    if (error.empty()) error = std::string(mod) + ": " + std::string(why);
  }
  void Ui(std::string_view, int32_t, const modlock::ui::Change&) override {}
};

}  // namespace

int main(int argc, char** argv) {
  modlock::host_app::InstallStdioGuard();
  std::cout << std::unitbuf;
  std::vector<std::filesystem::path> libraries;
  std::filesystem::path game_dir;
  std::string_view control;
  std::filesystem::path settings_file;
  modlock::net::LaunchConfig launch;
  bool check_only = false;
  int plugin_argc = 0;
  const char* const* plugin_argv = nullptr;
  if (const char* environment = std::getenv("DEADLOCK_DIR")) game_dir = environment;
  if (const char* environment = std::getenv("MODLOCK_HOST_PORT")) {
    auto port = Port(environment);
    if (!port) {
      std::cerr << "MODLOCK_HOST_PORT must be an integer from 0 to 65535.\n";
      return 2;
    }
    launch.host_port = *port;
  }
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--help") {
      Help();
      return 0;
    }
    if (argument == "--version") {
      std::cout << "Modlock " << MODLOCK_SOURCE_REVISION << '\n';
      return 0;
    }
    if (argument == "--check-plugin") {
      check_only = true;
      continue;
    }
    if (argument == "--") {
      plugin_argc = argc - index;
      plugin_argv = argv + index;
      break;
    }
    if (argument != "--plugin" && argument != "--game-dir" && argument != "--hostport" &&
        argument != "--map" && argument != "--connect" && argument != "--engine-args" &&
        argument != "--control" && argument != "--settings") {
      std::cerr << "Unknown option: " << argument << ". Use --help for usage.\n";
      return 2;
    }
    if (++index == argc) {
      std::cerr << argument << " needs a value.\n";
      return 2;
    }
    const std::string_view value(argv[index]);
    if (argument == "--plugin") libraries.emplace_back(value);
    if (argument == "--engine-args") launch.engine_arguments = value;
    if (argument == "--control") control = value;
    if (argument == "--settings") settings_file = value;
    if (argument == "--connect") {
      if (value.empty() ||
          value.find_first_not_of(
              "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.:-") !=
              std::string_view::npos) {
        std::cerr << "The connect address must be a host name or address with an optional port.\n";
        return 2;
      }
      launch.connect = value;
    }
    if (argument == "--game-dir") game_dir = value;
    if (argument == "--map") {
      if (value.empty() ||
          value.find_first_not_of(
              "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_/-") !=
              std::string_view::npos) {
        std::cerr << "The map must be a Source 2 map name.\n";
        return 2;
      }
      launch.map = value;
    }
    if (argument == "--hostport") {
      auto port = Port(value);
      if (!port) {
        std::cerr << "The host port must be an integer from 0 to 65535.\n";
        return 2;
      }
      launch.host_port = *port;
    }
  }
  if (libraries.empty()) {
    std::cerr << "Select a plugin with --plugin PATH. Use --help for usage.\n";
    return 2;
  }
  modlock::EngineHost engine;
  if (!check_only) {
    if (game_dir.empty()) {
      std::cerr << "Set DEADLOCK_DIR or pass --game-dir with your Deadlock installation.\n";
      return 2;
    }
    if (auto opened = engine.Open(std::filesystem::absolute(game_dir), launch); !opened) {
      std::cerr << opened.error() << '\n';
      return 1;
    }
  }

  // The interpreter modules ship beside the executable. The
  // control link reports the mods' progress to the program that started the
  // host; a lost controller leaves the server running.
  // A settings file outlives the mods that keep players' settings in it.
  std::optional<modlock::FileSettings> settings;
  modlock::WasmHost mods(std::filesystem::absolute(argv[0]).parent_path());
  if (!settings_file.empty()) mods.KeepSettings(&settings.emplace(settings_file));
  FailureRecorder failures;
  mods.Observe(&failures);
  std::unique_ptr<modlock::host_app::ControlLink> link;
  if (!control.empty()) {
    auto dialed = modlock::host_app::ControlLink::Dial(control);
    if (!dialed) {
      std::cerr << dialed.error() << '\n';
      return 1;
    }
    link = std::move(*dialed);
    mods.Observe(link.get());
    if (auto watched = link->Watch(engine); !watched && !check_only) {
      std::cerr << "control: " << watched.error() << '\n';
    }
  }

  // The host destroys plugin instances and libraries before the mods, the
  // link and engine resources.
  modlock::host::PluginHost plugins;
  const modlock::PluginContext context{.engine = &engine,
                                       .argc = plugin_argc,
                                       .argv = plugin_argv,
                                       .check_only = check_only,
                                       .launch = &launch,
                                       .wasm = &mods};
  for (const auto& library : libraries) {
    const auto path = std::filesystem::absolute(library);
    const bool built = path.extension() == ".wasm" || std::filesystem::is_directory(path);
    auto plugin = built ? mods.Load(path, context) : modlock::LoadPluginLibrary(path, context);
    if (!plugin) {
      std::cerr << plugin.error() << '\n';
      return 1;
    }
    if (auto error = plugins.Register(std::move(*plugin))) {
      std::cerr << *error << '\n';
      return 1;
    }
  }
  std::string error;
  if (!plugins.StartAll(error)) {
    std::cerr << error << '\n';
    return 1;
  }
  if (check_only) {
    plugins.TickAll();
    plugins.StopAll();
    if (!failures.error.empty()) {
      std::cerr << failures.error << '\n';
      return 1;
    }
    std::cout << "Plugin compatibility and lifecycle check passed. No game was started.\n";
    return 0;
  }
  auto frames = engine.OnFrame([&plugins, &mods, &link] {
    if (link) link->RunRequests(mods);
    plugins.TickAll();
  });
  if (!frames) {
    std::cerr << frames.error() << '\n';
    return 1;
  }
  auto result = engine.Run(launch);
  frames->Reset();
  plugins.StopAll();
  if (!result) {
    std::cerr << result.error() << '\n';
    return 1;
  }
  return plugins.ExitCode(*result);
}
