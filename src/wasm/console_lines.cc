//go:build deps_only

#include "wasm/console_lines.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace modlock::wasm {
namespace {

// Family names what a refused command controls, as the refusal reads.
enum class Family { kFiles, kLogging, kNetwork, kProcess, kDeferred };

// Rule refuses the commands and variables named pattern, or starting with it
// when prefix is set. Prefixes cover a family the game adds to between builds.
struct Rule {
  std::string_view pattern;
  bool prefix;
  Family family;
};

// kRules lists the server commands and variables of the game's console dump
// that act outside the game. Commands that set up the game's own recording,
// such as tv_enable and tv_delay, stay allowed; tv_record is checked by name.
constexpr auto kRules = std::to_array<Rule>({
    // Files: config execution, saved state, demos and paths the server writes.
    {"exec", true, Family::kFiles},
    {"host_writeconfig", false, Family::kFiles},
    {"host_readconfig", true, Family::kFiles},
    {"writeip", false, Family::kFiles},
    {"writeid", false, Family::kFiles},
    {"writekeybindings", false, Family::kFiles},
    {"condump", false, Family::kFiles},
    {"gameevents_dumptofile", false, Family::kFiles},
    {"mem_dump", false, Family::kFiles},
    {"v8_dump_heap_snapshot", false, Family::kFiles},
    {"v8_write_heap_stats", false, Family::kFiles},
    {"demo_writemetafile", false, Family::kFiles},
    {"save", false, Family::kFiles},
    {"save_", true, Family::kFiles},
    {"load", false, Family::kFiles},
    {"autosave", true, Family::kFiles},
    {"record", false, Family::kFiles},
    {"playdemo", false, Family::kFiles},
    {"timedemo", true, Family::kFiles},
    {"startdemos", false, Family::kFiles},
    {"filesystem_", true, Family::kFiles},
    {"fs_", true, Family::kFiles},
    {"servercfgfile", false, Family::kFiles},
    {"lservercfgfile", false, Family::kFiles},
    {"motdfile", false, Family::kFiles},
    {"hostfile", false, Family::kFiles},
    {"screenshot_subdir", false, Family::kFiles},
    {"sv_replaysdir", false, Family::kFiles},
    {"sv_crash_sentinel_filename", false, Family::kFiles},
    {"net_debug_to_file", false, Family::kFiles},
    {"net_filelogging", false, Family::kFiles},
    // Logging sinks and console capture.
    {"log", false, Family::kLogging},
    {"log_", true, Family::kLogging},
    {"con_log", true, Family::kLogging},
    {"sv_log", true, Family::kLogging},
    {"redirectstart", false, Family::kLogging},
    {"redirectend", false, Family::kLogging},
    // Network addresses, remote control, Steam identity and outside services.
    {"connect", true, Family::kNetwork},
    {"rcon", true, Family::kNetwork},
    {"sv_rcon", true, Family::kNetwork},
    {"vconsole_rcon", true, Family::kNetwork},
    {"net_public_adr", false, Family::kNetwork},
    {"hostip", false, Family::kNetwork},
    {"hostport", false, Family::kNetwork},
    {"ip", false, Family::kNetwork},
    {"clientport", false, Family::kNetwork},
    {"password", false, Family::kNetwork},
    {"sv_password", false, Family::kNetwork},
    {"addip", false, Family::kNetwork},
    {"banip", false, Family::kNetwork},
    {"removeip", false, Family::kNetwork},
    {"listip", false, Family::kNetwork},
    {"sv_setsteamaccount", false, Family::kNetwork},
    {"sv_steam", true, Family::kNetwork},
    {"sv_vac_webapi_auth_key", false, Family::kNetwork},
    {"sv_search_key", false, Family::kNetwork},
    {"sv_hosting_lobby", false, Family::kNetwork},
    {"sv_listen_directudp", false, Family::kNetwork},
    {"sv_matchperfstats_send_to_steam", false, Family::kNetwork},
    {"net_p2p_listen_dedicated", false, Family::kNetwork},
    {"steamlearn_", true, Family::kNetwork},
    {"asset_thumbnails_server_", true, Family::kNetwork},
    {"tv_broadcast", true, Family::kNetwork},
    {"tv_relay", true, Family::kNetwork},
    {"tv_port", true, Family::kNetwork},
    {"tv_password", true, Family::kNetwork},
    {"tv_secret_code", false, Family::kNetwork},
    {"tv_encryptdata", true, Family::kNetwork},
    {"tv_playcast", true, Family::kNetwork},
    {"tv_overridemaster", false, Family::kNetwork},
    {"tv_advertise_watchable", false, Family::kNetwork},
    {"tv_retry", false, Family::kNetwork},
    // The process: crashes, debuggers, tools and shutdown. quit and changelevel
    // end or replace the mod's own game and stay allowed.
    {"crash", true, Family::kProcess},
    {"server_test_crash", false, Family::kProcess},
    {"test_shipping_assert", false, Family::kProcess},
    {"test_exitprocess", false, Family::kProcess},
    {"test_startscript", false, Family::kProcess},
    {"sv_shutdown", false, Family::kProcess},
    {"sv_hoststate_quit_syscall", false, Family::kProcess},
    {"sv_dev_simulate_gcdown", false, Family::kProcess},
    {"generate_minidump", true, Family::kProcess},
    {"sys_minidump", true, Family::kProcess},
    {"script_attach_debugger", true, Family::kProcess},
    {"script_break_in_native_debugger", true, Family::kProcess},
    {"script_debug", false, Family::kProcess},
    {"unloadtool", false, Family::kProcess},
    {"open_current_map_", true, Family::kProcess},
    // Commands that store a line to run later, past this check.
    {"alias", false, Family::kDeferred},
    {"cmd", false, Family::kDeferred},
    {"execute_command_every_frame", false, Family::kDeferred},
    {"bind", true, Family::kDeferred},
    {"unbind", true, Family::kDeferred},
});

// What reads each family in a refusal.
std::string_view Describe(Family family) {
  switch (family) {
    case Family::kFiles:
      return "an engine file control";
    case Family::kLogging:
      return "an engine logging control";
    case Family::kNetwork:
      return "an engine network control";
    case Family::kProcess:
      return "an engine process control";
    case Family::kDeferred:
      return "a command that stores other commands";
  }
  return "an engine control";
}

// The tokenizer's break characters, each a token of its own.
constexpr std::string_view kBreaks = "{}()':";
constexpr std::string_view kSpace = " \t";

// Lower returns text in lower case; console names ignore case.
std::string Lower(std::string_view text) {
  std::string lower(text);
  std::ranges::transform(lower, lower.begin(),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return lower;
}

// Trim removes surrounding spaces and tabs.
std::string_view Trim(std::string_view text) {
  const auto first = text.find_first_not_of(kSpace);
  if (first == std::string_view::npos) {
    return {};
  }
  return text.substr(first, text.find_last_not_of(kSpace) - first + 1);
}

// Command splits one command's text into its name and arguments.
ConsoleCommand Command(std::string_view text) {
  if (text.front() == '"') {
    const auto close = text.find('"', 1);
    if (close == std::string_view::npos) {
      return {text.substr(1), {}};
    }
    return {text.substr(1, close - 1), Trim(text.substr(close + 1))};
  }
  auto end = kBreaks.contains(text.front()) ? 1 : text.find_first_of(" \t{}()':");
  end = std::min(end, text.size());
  return {text.substr(0, end), Trim(text.substr(end))};
}

// BareName reports whether a recording name stays in the recording directory.
bool BareName(std::string_view name) {
  if (name.ends_with(".dem")) {
    name.remove_suffix(4);
  }
  return !name.empty() && std::ranges::all_of(name, [](unsigned char c) {
    return std::isalnum(c) || c == '_' || c == '-';
  });
}

}  // namespace

std::vector<ConsoleCommand> SplitConsoleLine(std::string_view line) {
  std::vector<ConsoleCommand> commands;
  while (!line.empty()) {
    const auto end = std::min(line.find_first_of(";\r\n"), line.size());
    if (const auto text = Trim(line.substr(0, end)); !text.empty()) {
      commands.push_back(Command(text));
    }
    line.remove_prefix(std::min(end + 1, line.size()));
  }
  return commands;
}

std::string RefuseConsoleLine(std::string_view line) {
  for (const auto& command : SplitConsoleLine(line)) {
    const auto name = Lower(command.name);
    if (name == "tv_record" && !BareName(command.arguments)) {
      return "tv_record takes a bare file name of letters, digits, - and _";
    }
    for (const auto& rule : kRules) {
      if (rule.prefix ? name.starts_with(rule.pattern) : name == rule.pattern) {
        return name + " is " + std::string(Describe(rule.family)) + ", which a mod may not run";
      }
    }
  }
  return {};
}

}  // namespace modlock::wasm
