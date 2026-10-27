#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace modlock::wasm {

// ConsoleCommand is one command of a console line: its name, without quotes,
// and the text after it.
struct ConsoleCommand {
  std::string_view name;
  std::string_view arguments;
};

// SplitConsoleLine splits line into the commands the engine's command buffer
// could run from it. It breaks at every newline and semicolon, even inside
// quotes, and ends a name at the tokenizer's break characters, so it may find
// more commands than the engine runs but never fewer. Empty commands are
// dropped.
std::vector<ConsoleCommand> SplitConsoleLine(std::string_view line);

// RefuseConsoleLine returns why a mod may not run line at the server console,
// or an empty string when it may. A mod changes the game: its rules, its map,
// its players and its recording. It may not reach the engine's controls for
// files, logging, network addresses, config execution or the process, nor
// define a command that runs later.
std::string RefuseConsoleLine(std::string_view line);

}  // namespace modlock::wasm
