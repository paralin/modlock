//go:build deps_only

#include "wasm/entity_rules.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace modlock::wasm {
namespace {

// kClasses are the designer names of the game's entity dump that run console
// lines or load what no check sees: the command points run their Command
// input's line, a commentary node runs its pre and post commands, a script
// runs a script file, and a prefab spawns a map's entities.
constexpr auto kClasses = std::to_array<std::string_view>({
    "point_servercommand",
    "point_clientcommand",
    "point_broadcastclientcommand",
    "point_commentary_node",
    "logic_script",
    "point_prefab",
});

// kKeys are the key values that name a console line or a script; keys ending
// in kScriptSuffix name a script function, such as an item's
// spawn_script_func.
constexpr auto kKeys = std::to_array<std::string_view>({
    "vscripts",
    "precommands",
    "postcommands",
});
constexpr std::string_view kScriptSuffix = "_script_func";

// kFields are the schema fields those key values fill, which a mod could
// otherwise write as text before the entity spawns.
constexpr auto kFields = std::to_array<std::string_view>({
    "m_iszprivatevscripts",
    "m_iszprecommands",
    "m_iszpostcommands",
});

// kInputs are the inputs refused on every entity.
constexpr auto kInputs = std::to_array<std::string_view>({
    "addoutput",
    "runscriptcode",
    "runscriptfile",
    "callscriptfunction",
    "callprivatescriptfunction",
    "callglobalscriptfunction",
});

// Lower returns text in lower case; Source matches these names in any case.
std::string Lower(std::string_view text) {
  std::string lower(text);
  std::ranges::transform(lower, lower.begin(),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return lower;
}

}  // namespace

std::string RefuseEntityClass(std::string_view designer_name) {
  const auto name = Lower(designer_name);
  if (std::ranges::contains(kClasses, name)) {
    return name +
           " runs console lines or loads entities past a mod's checks, which a mod may not use";
  }
  return {};
}

std::string RefuseKeyValue(std::string_view key) {
  const auto name = Lower(key);
  if (std::ranges::contains(kKeys, name) || name.ends_with(kScriptSuffix)) {
    return "key value " + name + " names a console line or a script, which a mod may not set";
  }
  return {};
}

std::string RefuseField(std::string_view field) {
  const auto name = Lower(field);
  if (std::ranges::contains(kFields, name)) {
    return "field " + std::string(field) +
           " names a console line or a script, which a mod may not write";
  }
  return {};
}

std::string RefuseInput(std::string_view input) {
  const auto name = Lower(input);
  if (std::ranges::contains(kInputs, name)) {
    return "input " + name + " rewires an entity or runs a script, which a mod may not send";
  }
  return {};
}

}  // namespace modlock::wasm
