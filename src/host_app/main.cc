#include <charconv>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "host_app/control_link.h"
#include "host_app/game_dump.h"
#include "modlock/build.h"
#include "modlock/engine_host.h"
#include "modlock/gameinterop/engine_server.h"
#include "modlock/gameinterop/game_symbols.h"
#include "modlock/host.h"
#include "modlock/host_app/stdio_guard.h"
#include "modlock/plugin_library.h"
#include "modlock/wasm_host.h"

namespace {

void Help() {
  std::cout << "Modlock hosts Deadlock mods: sandboxed WebAssembly modules and native plugins.\n\n"
               "Usage: modlock-host --plugin PATH [options] [-- plugin arguments]\n"
               "       modlock-host --dump DIRECTORY [options]\n\n"
               "  --plugin PATH       Load a built mod (a directory with mod.json or a .wasm\n"
               "                      file) or a plugin library; may be repeated\n"
               "  --game-dir PATH     Deadlock installation (or DEADLOCK_DIR)\n"
               "  --hostport PORT     Server UDP port (default 27067)\n"
               "  --map NAME          Startup map (default dl_midtown)\n"
               "  --connect ADDRESS   Run the game's own client, unchanged, that joins ADDRESS\n"
               "                      instead of a server; takes no plugin, control or dump\n"
               "  --engine-args ARGS  Append engine command-line arguments\n"
               "  --settings PATH     Keep players' mod settings in the JSON file PATH\n"
               "  --files DIRECTORY   Keep the files mods write in DIRECTORY, one\n"
               "                      directory per mod\n"
               "  --game-data PATH    Read signatures and virtual slots from PATH instead\n"
               "                      of the game_signatures.txtpb beside the host\n"
               "  --control ADDRESS   Report to and take reloads from the controller at\n"
               "                      ADDRESS, a host and port\n"
               "  --dump DIRECTORY    Start the server, write its schemas, entity classes and\n"
               "                      console to DIRECTORY as JSON, and quit\n"
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

// LoadSignatures reads the first readable game data file of paths and logs how
// many entries it loaded and every entry it skips, so a broken entry disables
// only the features that resolve it. With none readable, the built-in game data
// stays.
void LoadSignatures(std::initializer_list<std::filesystem::path> paths) {
  // Load the first readable game data file and report what it skipped.
  std::expected<modlock::gameinterop::GameSignatureLoad, std::string> load =
      std::unexpected("no game data file");
  for (const auto& path : paths) {
    if (path.empty()) {
      continue;
    }
    load = modlock::gameinterop::LoadGameSignatures(path);
    if (load) {
      break;
    }
    std::cerr << "[modlock] " << load.error() << '\n';
  }
  if (!load) {
    std::cerr << "[modlock] using the built-in game data\n";
    return;
  }
  std::cerr << "[modlock] game data: " << load->loaded << " signatures and " << load->slots
            << " virtual slots loaded, " << load->skipped.size() << " skipped\n";
  for (const auto& skipped : load->skipped) {
    std::cerr << "[modlock] game data entry skipped: " << skipped << '\n';
  }
}

// FailureRecorder keeps the first error of a mod that stopped, which fails
// a check: a mod stops itself without stopping the host.
class FailureRecorder final : public modlock::WasmHostObserver {
 public:
  std::string error;

  void Started(std::string_view, bool) override {}

  void Logged(std::string_view, std::string_view) override {}

  void Failed(std::string_view mod, std::string_view why) override {
    if (error.empty()) {
      error = std::string(mod) + ": " + std::string(why);
    }
  }

  void Ui(std::string_view, int32_t, const modlock::wasm::ui::Change&) override {}
};

// Options holds what the environment and the command line select.
struct Options {
  std::vector<std::filesystem::path> libraries;
  std::filesystem::path game_dir;
  std::string_view control;
  std::filesystem::path settings_file;
  std::filesystem::path files_directory;
  std::filesystem::path dump_directory;
  std::filesystem::path game_data;
  modlock::net::LaunchConfig launch;
  bool check_only = false;
  int plugin_argc = 0;
  const char* const* plugin_argv = nullptr;
};

// The characters a connect address and a map name may hold.
constexpr std::string_view kAddressCharacters =
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.:-";
constexpr std::string_view kMapCharacters =
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_/-";

// ExitCode is the status main returns when parsing ends the program.
using ExitCode = int;

bool OnlyCharacters(std::string_view value, std::string_view allowed) {
  return !value.empty() && value.find_first_not_of(allowed) == std::string_view::npos;
}

// SetOption stores the value of one option that takes a value, or fails with
// the exit code after reporting why.
std::expected<void, ExitCode> SetOption(Options& options, std::string_view argument,
                                        std::string_view value) {
  // Store paths and strings as given.
  if (argument == "--plugin") {
    options.libraries.emplace_back(value);
  } else if (argument == "--engine-args") {
    options.launch.engine_arguments = value;
  } else if (argument == "--control") {
    options.control = value;
  } else if (argument == "--settings") {
    options.settings_file = value;
  } else if (argument == "--files") {
    options.files_directory = value;
  } else if (argument == "--dump") {
    options.dump_directory = value;
  } else if (argument == "--game-data") {
    options.game_data = value;
  } else if (argument == "--game-dir") {
    options.game_dir = value;
    return {};
  }

  // Check the address and map, and parse the port.
  if (argument == "--connect") {
    if (!OnlyCharacters(value, kAddressCharacters)) {
      std::cerr << "The connect address must be a host name or address with an optional port.\n";
      return std::unexpected(2);
    }
    options.launch.connect = value;
  } else if (argument == "--map") {
    if (!OnlyCharacters(value, kMapCharacters)) {
      std::cerr << "The map must be a Source 2 map name.\n";
      return std::unexpected(2);
    }
    options.launch.map = value;
  } else if (argument == "--hostport") {
    auto port = Port(value);
    if (!port) {
      std::cerr << "The host port must be an integer from 0 to 65535.\n";
      return std::unexpected(2);
    }
    options.launch.host_port = *port;
  }
  return {};
}

// ParseOptions reads the environment, then the command line. It fails with
// the exit code when an option is wrong or ends the program, as --help does.
std::expected<Options, ExitCode> ParseOptions(int argc, char** argv) {
  // Read the environment.
  Options options;
  if (const char* environment = std::getenv("DEADLOCK_DIR")) {
    options.game_dir = environment;
  }
  if (const char* environment = std::getenv("MODLOCK_HOST_PORT")) {
    auto port = Port(environment);
    if (!port) {
      std::cerr << "MODLOCK_HOST_PORT must be an integer from 0 to 65535.\n";
      return std::unexpected(2);
    }
    options.launch.host_port = *port;
  }

  // Then read the command line.
  for (int index = 1; index < argc; ++index) {
    // Read each option; flags take no value.
    const std::string_view argument(argv[index]);
    if (argument == "--help") {
      Help();
      return std::unexpected(0);
    }
    if (argument == "--version") {
      std::cout << "Modlock " << MODLOCK_SOURCE_REVISION << '\n';
      return std::unexpected(0);
    }
    if (argument == "--check-plugin") {
      options.check_only = true;
      continue;
    }
    if (argument == "--") {
      options.plugin_argc = argc - index;
      options.plugin_argv = argv + index;
      break;
    }

    // Every other option takes one.
    if (argument != "--plugin" && argument != "--game-dir" && argument != "--hostport" &&
        argument != "--map" && argument != "--connect" && argument != "--engine-args" &&
        argument != "--control" && argument != "--settings" && argument != "--files" &&
        argument != "--dump" && argument != "--game-data") {
      std::cerr << "Unknown option: " << argument << ". Use --help for usage.\n";
      return std::unexpected(2);
    }
    if (++index == argc) {
      std::cerr << argument << " needs a value.\n";
      return std::unexpected(2);
    }
    if (auto set = SetOption(options, argument, argv[index]); !set) {
      return std::unexpected(set.error());
    }
  }

  // A client launch runs the game's own client, which Modlock never changes,
  // so everything that runs inside the game belongs to a server launch.
  if (!options.launch.connect.empty() &&
      (!options.libraries.empty() || !options.control.empty() || !options.dump_directory.empty() ||
       options.check_only || options.plugin_argv)) {
    std::cerr << "--connect runs the game's own client unchanged; --plugin, --control, --dump "
                 "and --check-plugin need a server launch.\n";
    return std::unexpected(2);
  }
  return options;
}

// RunClient opens the game and runs its own client, which joins the server at
// the launch's connect address. The host loads the game's modules and hands
// the engine its command line; it installs no hook and loads no plugin.
int RunClient(const Options& options) {
  if (options.game_dir.empty()) {
    std::cerr << "Set DEADLOCK_DIR or pass --game-dir with your Deadlock installation.\n";
    return 2;
  }
  modlock::EngineHost engine;
  if (auto opened = engine.Open(std::filesystem::absolute(options.game_dir), options.launch);
      !opened) {
    std::cerr << opened.error() << '\n';
    return 1;
  }
  auto result = engine.Run(options.launch);
  if (!result) {
    std::cerr << result.error() << '\n';
    return 1;
  }
  return *result;
}

}  // namespace

int main(int argc, char** argv) {
  // Guard stdio and unbuffer the output.
  modlock::host_app::InstallStdioGuard();
  std::cout << std::unitbuf;

  // Read the options.
  auto parsed = ParseOptions(argc, argv);
  if (!parsed) {
    return parsed.error();
  }
  Options& options = *parsed;
  if (!options.launch.connect.empty()) {
    return RunClient(options);
  }

  // A server launch needs a plugin or a dump.
  if (options.libraries.empty() && options.dump_directory.empty()) {
    std::cerr << "Select a plugin with --plugin PATH. Use --help for usage.\n";
    return 2;
  }

  // The game data file and the interpreter modules ship beside the executable;
  // a newer game data file may come from the launcher.
  const auto directory = std::filesystem::absolute(argv[0]).parent_path();
  LoadSignatures({options.game_data, directory / "game_signatures.txtpb"});

  // Open the game unless only checking the plugin.
  modlock::EngineHost engine;
  if (!options.check_only) {
    if (options.game_dir.empty()) {
      std::cerr << "Set DEADLOCK_DIR or pass --game-dir with your Deadlock installation.\n";
      return 2;
    }
    if (auto opened = engine.Open(std::filesystem::absolute(options.game_dir), options.launch);
        !opened) {
      std::cerr << opened.error() << '\n';
      return 1;
    }
  }

  // The control link reports the mods' progress to the program that started
  // the host; a lost controller leaves the server running. A settings file
  // outlives the mods that keep players' settings in it.
  std::optional<modlock::FileSettings> settings;
  modlock::WasmHost mods(directory);
  if (!options.settings_file.empty()) {
    mods.KeepSettings(&settings.emplace(options.settings_file));
  }
  mods.KeepFiles(options.files_directory);
  FailureRecorder failures;
  mods.Observe(&failures);
  std::unique_ptr<modlock::host_app::ControlLink> link;
  if (!options.control.empty()) {
    // Dial the control link and watch the engine through it.
    auto dialed = modlock::host_app::ControlLink::Dial(options.control);
    if (!dialed) {
      std::cerr << dialed.error() << '\n';
      return 1;
    }
    link = std::move(*dialed);
    mods.Observe(link.get());
    if (auto watched = link->Watch(engine); !watched && !options.check_only) {
      std::cerr << "control: " << watched.error() << '\n';
    }
  }

  // The host destroys plugin instances and libraries before the mods, the
  // link and engine resources.
  modlock::host::PluginHost plugins;
  const modlock::PluginContext context{.engine = &engine,
                                       .argc = options.plugin_argc,
                                       .argv = options.plugin_argv,
                                       .check_only = options.check_only,
                                       .launch = &options.launch,
                                       .wasm = &mods};
  for (const auto& library : options.libraries) {
    // Load each plugin, built mod or native library.
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

  // Start the plugins; a check ticks and stops them once.
  std::string error;
  if (!plugins.StartAll(error)) {
    std::cerr << error << '\n';
    return 1;
  }
  if (options.check_only) {
    // A check runs one tick and stops.
    plugins.TickAll();
    plugins.StopAll();
    if (!failures.error.empty()) {
      std::cerr << failures.error << '\n';
      return 1;
    }
    std::cout << "Plugin compatibility and lifecycle check passed. No game was started.\n";
    return 0;
  }

  // A dump runs once the world is ready, when every module has registered its
  // schema, entity classes and console entries, then quits the server.
  std::optional<modlock::host_app::GameDumpCounts> dumped;
  std::string dump_error;
  modlock::Subscription dump_world;
  if (!options.dump_directory.empty()) {
    auto subscribed = engine.OnWorld({}, [&](std::string_view) {
      // Write the dump once, then quit the server.
      if (dumped || !dump_error.empty()) {
        return;
      }
      auto written = modlock::host_app::WriteGameDump(
          engine.Paths(), std::filesystem::absolute(options.dump_directory));
      if (written) {
        dumped = *written;
      } else {
        dump_error = written.error();
      }
      auto server = modlock::gameinterop::EngineServer::Resolve();
      if (server) {
        static_cast<void>(server->ServerCommand("quit\n"));
      }
    });
    if (!subscribed) {
      std::cerr << subscribed.error() << '\n';
      return 1;
    }
    dump_world = std::move(*subscribed);
  }

  // Tick the mods each frame.
  auto frames = engine.OnFrame([&plugins, &mods, &link] {
    if (link) {
      link->RunRequests(mods);
    }
    plugins.TickAll();
  });
  if (!frames) {
    std::cerr << frames.error() << '\n';
    return 1;
  }

  // Run the engine, then stop the plugins and report the result.
  auto result = engine.Run(options.launch);
  frames->Reset();
  dump_world.Reset();
  plugins.StopAll();
  if (!result) {
    std::cerr << result.error() << '\n';
    return 1;
  }
  if (!options.dump_directory.empty()) {
    if (!dumped) {
      std::cerr << "dump: " << (dump_error.empty() ? "the world never became ready" : dump_error)
                << '\n';
      return 1;
    }
    std::cout << "Dumped " << dumped->classes << " schema classes, " << dumped->enums << " enums, "
              << dumped->designer_names << " designer names, " << dumped->data_maps
              << " data maps, " << dumped->inputs << " inputs, " << dumped->outputs << " outputs, "
              << dumped->variables << " console variables and " << dumped->commands
              << " commands to " << options.dump_directory.string() << ".\n";
    return 0;
  }
  return plugins.ExitCode(*result);
}
