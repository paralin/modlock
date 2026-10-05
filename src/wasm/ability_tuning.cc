#include "wasm/ability_tuning.h"

#include <array>
#include <cstring>
#include <map>
#include <string_view>

#include "modlock/gameinterop/ability_definitions.h"
#include "modlock/gameinterop/entity_abi.h"
#include "modlock/gameinterop/keyvalues.h"

namespace modlock::wasm {
namespace {

using gameinterop::SchemaField;

// SetProperties replaces named values in an ability's parsed property map,
// the cache the game reads property values through. The schema supplies the
// property record's size, which sets the map's node stride.
void SetProperties(unsigned char* definition, void* schema, const char* module,
                   std::map<std::string, float> values, std::vector<std::string>& problems) {
  if (values.empty()) return;
  auto properties =
      gameinterop::SchemaFieldOf(schema, module, "CitadelAbilityVData", "m_mapAbilityProperties");
  auto size = gameinterop::SchemaClassSizeOf(schema, module, "CitadelAbilityProperty_t");
  auto cache = gameinterop::SchemaFieldOf(schema, module, "CitadelAbilityProperty_t",
                                          "m_strStreetBrawlValue");
  if (!properties || !size || !cache) {
    problems.push_back("ability properties are unavailable");
    return;
  }

  // The map is a CUtlMap: its node count at +8 and its nodes at +16. A node
  // is 24 bytes of links and key, then the property. A free node links to
  // itself.
  auto* map = definition + properties->offset;
  int32_t count = 0;
  unsigned char* nodes = nullptr;
  std::memcpy(&count, map + 8, sizeof(count));
  std::memcpy(&nodes, map + 16, sizeof(nodes));
  if (!nodes || count <= 0 || count > 256) {
    problems.push_back("ability property map is invalid");
    return;
  }
  for (int32_t i = 0; i < count; ++i) {
    auto* node = nodes + i * (24 + *size);
    int32_t left = 0;
    std::memcpy(&left, node, sizeof(left));
    if (left == i) continue;
    const char* key = nullptr;
    std::memcpy(&key, node + 16, sizeof(key));
    if (!key) continue;
    auto found = values.find(key);
    if (found == values.end()) continue;

    // The parsed cache follows the value string's pointer: the value and its
    // street brawl variant.
    const std::array<float, 2> parsed{found->second, found->second};
    std::memcpy(node + 24 + cache->offset + sizeof(void*), parsed.data(), sizeof(parsed));
    values.erase(found);
  }
  for (const auto& [name, value] : values) problems.push_back("no ability property " + name);
}

// ResolvePath sums the offsets along a path of class.field steps separated by
// slashes, such as CitadelAbilityVData.m_projectileInfo/ProjectileInfo_t.m_flSpeed,
// and returns the last step's field.
std::expected<SchemaField, std::string> ResolvePath(void* schema, const char* module,
                                                    std::string_view path) {
  SchemaField result{};
  size_t offset = 0;
  while (!path.empty()) {
    const auto slash = path.find('/');
    const auto step = path.substr(0, slash);
    path = slash == std::string_view::npos ? std::string_view{} : path.substr(slash + 1);
    const auto dot = step.find('.');
    if (dot == std::string_view::npos) return std::unexpected("field path step lacks a class");
    const std::string type(step.substr(0, dot));
    const std::string name(step.substr(dot + 1));
    auto field = gameinterop::SchemaFieldOf(schema, module, type.c_str(), name.c_str());
    if (!field) return std::unexpected(field.error());
    offset += field->offset;
    result = *field;
  }
  result.offset = offset;
  return result;
}

}  // namespace

std::expected<std::vector<std::string>, std::string> TuneAbilities(
    const gameinterop::ModuleImage& module, void* schema, const char* module_name,
    const google::protobuf::RepeatedPtrField<AbilityTuning>& abilities) {
  auto definitions = gameinterop::AbilityDefinitions::Resolve(module);
  if (!definitions) return std::unexpected(definitions.error());

  // Find every ability first, so a module still loading its data takes no
  // partial tuning.
  std::vector<unsigned char*> found;
  for (const auto& ability : abilities) {
    auto definition =
        definitions->Find(gameinterop::MakeMemberName(ability.ability()).hash);
    if (!definition) return std::unexpected(ability.ability() + ": " + definition.error());
    found.push_back(static_cast<unsigned char*>(definition->native_definition_pointer));
  }

  std::vector<std::string> problems;
  for (int index = 0; index < abilities.size(); ++index) {
    const auto& ability = abilities[index];
    auto* definition = found[index];
    std::vector<std::string> own;
    SetProperties(definition, schema, module_name,
                  {ability.properties().begin(), ability.properties().end()}, own);
    for (const auto& [path, value] : ability.fields()) {
      auto field = ResolvePath(schema, module_name, path);
      if (!field || field->size != sizeof(value)) {
        own.push_back(path + ": " + (field ? "not a float" : field.error()));
        continue;
      }
      std::memcpy(definition + field->offset, &value, sizeof(value));
    }
    for (const auto& [target, source] : ability.copy_fields()) {
      auto to = ResolvePath(schema, module_name, target);
      auto from = ResolvePath(schema, module_name, source);
      if (!to || !from || to->size != from->size || !to->size) {
        own.push_back(target + ": cannot copy " + source);
        continue;
      }
      std::memmove(definition + to->offset, definition + from->offset, to->size);
    }
    for (auto& problem : own) problems.push_back(ability.ability() + ": " + problem);
  }
  return problems;
}

}  // namespace modlock::wasm
