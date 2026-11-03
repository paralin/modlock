//go:build deps_only

#include "wasm/definition_changes.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <optional>
#include <string_view>

#include "modlock/gameinterop/ability_definitions.h"
#include "modlock/gameinterop/bot_difficulty_definitions.h"
#include "modlock/gameinterop/entity_abi.h"
#include "modlock/gameinterop/hero_definitions.h"
#include "modlock/gameinterop/keyvalues.h"

namespace modlock::wasm {
namespace {

using manifest::Definition;

// ForEachMapNode calls visit with the key and value of each used node of the
// CUtlMap at map, whose key and value together span element_size bytes. The
// node count is at +8 and the nodes at +16. A node is 16 bytes of links, then
// the key and value; a free node's left link names itself. It reports false
// when the map is invalid.
template <typename Visit>
bool ForEachMapNode(unsigned char* map, size_t element_size, Visit visit) {
  // Read the node array.
  int32_t count = 0;
  unsigned char* nodes = nullptr;
  std::memcpy(&count, map + 8, sizeof(count));
  std::memcpy(&nodes, map + 16, sizeof(nodes));
  if (!nodes || count <= 0 || count > 256) {
    return false;
  }

  // Visit each used node.
  for (int32_t i = 0; i < count; ++i) {
    auto* node = nodes + i * (16 + element_size);
    int32_t left = 0;
    std::memcpy(&left, node, sizeof(left));
    if (left != i) {
      visit(node + 16);
    }
  }
  return true;
}

// SetProperties replaces named values in an ability's parsed property map,
// the cache the game reads property values through. The schema supplies the
// property record's size, which follows each node's key pointer.
void SetProperties(unsigned char* definition, void* schema, const char* module,
                   std::map<std::string, float> values, DefinitionUndo& undo,
                   std::vector<std::string>& problems) {
  // An empty request touches nothing.
  if (values.empty()) {
    return;
  }

  // Find the property map and the property layout.
  auto properties =
      gameinterop::SchemaFieldOf(schema, module, "CitadelAbilityVData", "m_mapAbilityProperties");
  auto size = gameinterop::SchemaClassSizeOf(schema, module, "CitadelAbilityProperty_t");
  auto cache = gameinterop::SchemaFieldOf(schema, module, "CitadelAbilityProperty_t",
                                          "m_strStreetBrawlValue");
  if (!properties || !size || !cache) {
    problems.push_back("ability properties are unavailable");
    return;
  }

  // Set each requested property that the map holds. The parsed cache follows
  // the value string's pointer: the value and its street brawl variant.
  const bool valid = ForEachMapNode(
      definition + properties->offset, sizeof(char*) + *size, [&](unsigned char* element) {
        const char* key = nullptr;
        std::memcpy(&key, element, sizeof(key));
        auto found = key ? values.find(key) : values.end();
        if (found == values.end()) {
          return;
        }
        const std::array<float, 2> parsed{found->second, found->second};
        auto* storage = element + sizeof(key) + cache->offset + sizeof(void*);
        undo.Save(storage, sizeof(parsed), false);
        std::memcpy(storage, parsed.data(), sizeof(parsed));
        values.erase(found);
      });
  if (!valid) {
    problems.push_back("ability property map is invalid");
    return;
  }

  // Report the properties the map lacks.
  for (const auto& [name, value] : values) {
    problems.push_back("no ability property " + name);
  }
}

// Place is where a field path leads in an entry's data: a field, or the float
// value of one key in an enum-keyed map field.
struct Place {
  size_t offset;
  size_t size;
  std::optional<int32_t> key;
};

// ResolvePath sums the offsets along a path of Class.field steps separated by
// slashes, such as CitadelAbilityVData.m_projectileInfo/ProjectileInfo_t.m_flSpeed.
// A last step may end in [Enum.Value], naming one value of a map field.
std::expected<Place, std::string> ResolvePath(void* schema, const char* module,
                                              std::string_view path) {
  // Split off a map key.
  std::optional<int32_t> key;
  if (path.ends_with(']')) {
    const auto open = path.rfind('[');
    const auto inner = path.substr(open + 1, path.size() - open - 2);
    const auto dot = inner.find('.');
    if (open == std::string_view::npos || dot == std::string_view::npos) {
      return std::unexpected("a map key is Enum.Value");
    }
    const std::string type(inner.substr(0, dot));
    const std::string value(inner.substr(dot + 1));
    auto resolved = gameinterop::SchemaEnumValueOf(schema, module, type.c_str(), value.c_str());
    if (!resolved) {
      return std::unexpected(resolved.error());
    }
    key = static_cast<int32_t>(*resolved);
    path = path.substr(0, open);
  }

  // Add up each step's offset; the last step names the field.
  Place place{.offset = 0, .size = 0, .key = key};
  while (!path.empty()) {
    // Split off the next step.
    const auto slash = path.find('/');
    const auto step = path.substr(0, slash);
    path = slash == std::string_view::npos ? std::string_view{} : path.substr(slash + 1);

    // Each step is Class.field.
    const auto dot = step.find('.');
    if (dot == std::string_view::npos) {
      return std::unexpected("field path step lacks a class");
    }
    const std::string type(step.substr(0, dot));
    const std::string name(step.substr(dot + 1));

    // Add its offset.
    auto field = gameinterop::SchemaFieldOf(schema, module, type.c_str(), name.c_str());
    if (!field) {
      return std::unexpected(field.error());
    }
    place.offset += field->offset;
    place.size = field->size;
  }

  // A map value is one float.
  if (place.key) {
    place.size = sizeof(float);
  }
  return place;
}

// Locate returns the storage a place names in one entry's data, or null when
// its map holds no such key.
unsigned char* Locate(unsigned char* definition, const Place& place) {
  // A field lies at its offset.
  if (!place.key) {
    return definition + place.offset;
  }

  // A map value follows its key.
  unsigned char* found = nullptr;
  ForEachMapNode(definition + place.offset, sizeof(int32_t) + sizeof(float),
                 [&](unsigned char* element) {
                   int32_t key = 0;
                   std::memcpy(&key, element, sizeof(key));
                   if (key == *place.key) {
                     found = element + sizeof(key);
                   }
                 });
  return found;
}

// Change applies one entry's properties, fields and copies, keeping what
// they overwrite in undo, and adds its problems.
void Change(const Definition& change, unsigned char* definition, void* schema, const char* module,
            DefinitionUndo& undo, std::vector<std::string>& problems) {
  const bool world = change.file() == Definition::FILE_HEROES;
  SetProperties(definition, schema, module,
                {change.properties().begin(), change.properties().end()}, undo, problems);
  for (const auto& [path, value] : change.fields()) {
    auto place = ResolvePath(schema, module, path);
    auto* storage = place && place->size == sizeof(value) ? Locate(definition, *place) : nullptr;
    if (!storage) {
      problems.push_back(path + ": " +
                         (!place                         ? place.error()
                          : place->size != sizeof(value) ? "not a float"
                                                         : "no such map key"));
      continue;
    }
    undo.Save(storage, sizeof(value), world);
    std::memcpy(storage, &value, sizeof(value));
  }
  for (const auto& [target, source] : change.copy_fields()) {
    auto to = ResolvePath(schema, module, target);
    auto from = ResolvePath(schema, module, source);
    auto* to_storage = to ? Locate(definition, *to) : nullptr;
    auto* from_storage = from ? Locate(definition, *from) : nullptr;
    if (!to_storage || !from_storage || to->size != from->size || !to->size) {
      problems.push_back(target + ": cannot copy " + source);
      continue;
    }
    undo.Save(to_storage, to->size, world);
    std::memmove(to_storage, from_storage, to->size);
  }
}

// Missing is why an entry was not found: later while its data is still
// loading, or never when the module has no lookup for its file.
struct Missing {
  std::string error;
  bool later;
};

// Entries finds each file's entries in one module.
class Entries {
 public:
  Entries(const gameinterop::ModuleImage& module, void* schema, const char* module_name)
      : module_(module), schema_(schema), module_name_(module_name) {}

  // Find returns an entry's parsed data.
  std::expected<unsigned char*, Missing> Find(const Definition& change) {
    switch (change.file()) {
      case Definition::FILE_HEROES: {
        if (!heroes_) {
          heroes_ = gameinterop::HeroDefinitions::Resolve(module_);
        }
        if (!*heroes_) {
          return std::unexpected(Missing{.error = heroes_->error(), .later = false});
        }
        auto hero = (*heroes_)->Find(change.entry());
        if (!hero) {
          return std::unexpected(Missing{.error = hero.error(), .later = true});
        }
        return static_cast<unsigned char*>(hero->native_definition_pointer);
      }
      case Definition::FILE_BOT_DIFFICULTY: {
        if (!bots_) {
          bots_ = gameinterop::BotDifficultyDefinitions::Resolve(module_);
        }
        if (!*bots_) {
          return std::unexpected(Missing{.error = bots_->error(), .later = false});
        }
        auto difficulty = gameinterop::SchemaEnumValueOf(
            schema_, module_name_, "ECitadelBotDifficulty", change.entry().c_str());
        if (!difficulty) {
          return std::unexpected(Missing{.error = difficulty.error(), .later = false});
        }
        auto record = (*bots_)->Find(static_cast<int>(*difficulty));
        if (!record) {
          return std::unexpected(Missing{.error = record.error(), .later = true});
        }
        if (!*record) {
          return std::unexpected(Missing{.error = "the file has no such record", .later = false});
        }
        return static_cast<unsigned char*>(*record);
      }
      case Definition::FILE_NPC_UNITS:
        return FindSubclass(units_, gameinterop::AbilityDefinitions::kNpcUnits, change);
      default:
        return FindSubclass(abilities_, gameinterop::AbilityDefinitions::kAbilities, change);
    }
  }

 private:
  using Subclasses = std::optional<std::expected<gameinterop::AbilityDefinitions, std::string>>;

  // FindSubclass returns a subclass entry of one scope, resolving the scope's
  // lookup on first use.
  std::expected<unsigned char*, Missing> FindSubclass(Subclasses& subclasses,
                                                      gameinterop::AbilityDefinitions::Scope scope,
                                                      const Definition& change) {
    if (!subclasses) {
      subclasses = gameinterop::AbilityDefinitions::Resolve(module_, scope);
    }
    if (!*subclasses) {
      return std::unexpected(Missing{.error = subclasses->error(), .later = false});
    }
    auto entry = (*subclasses)->Find(gameinterop::MakeMemberName(change.entry()).hash);
    if (!entry) {
      return std::unexpected(Missing{.error = entry.error(), .later = true});
    }
    return static_cast<unsigned char*>(entry->native_definition_pointer);
  }

  const gameinterop::ModuleImage& module_;
  void* schema_;
  const char* module_name_;
  Subclasses abilities_;
  Subclasses units_;
  std::optional<std::expected<gameinterop::HeroDefinitions, std::string>> heroes_;
  std::optional<std::expected<gameinterop::BotDifficultyDefinitions, std::string>> bots_;
};

}  // namespace

void DefinitionUndo::Save(unsigned char* storage, size_t size, bool world) {
  if (std::ranges::any_of(saved_, [&](const Saved& saved) { return saved.storage == storage; })) {
    return;
  }
  saved_.push_back(Saved{.storage = storage, .value = {storage, storage + size}, .world = world});
}

void DefinitionUndo::Restore() {
  // Write back newest first, so a place saved twice through overlapping
  // changes ends at its oldest value.
  for (auto saved = saved_.rbegin(); saved != saved_.rend(); ++saved) {
    std::memcpy(saved->storage, saved->value.data(), saved->value.size());
  }
  saved_.clear();
}

void DefinitionUndo::ForgetWorld() {
  std::erase_if(saved_, [](const Saved& saved) { return saved.world; });
}

std::expected<std::vector<std::string>, std::string> ChangeDefinitions(
    const gameinterop::ModuleImage& module, void* schema, const char* module_name,
    const google::protobuf::RepeatedPtrField<Definition>& definitions, DefinitionUndo& undo) {
  // Find every entry first, so a module still loading its data takes no
  // partial change. An entry the module can never find is reported and
  // skipped.
  Entries entries(module, schema, module_name);
  std::vector<unsigned char*> found;
  std::vector<std::string> problems;
  for (const auto& change : definitions) {
    auto definition = entries.Find(change);
    if (!definition && definition.error().later) {
      return std::unexpected(change.entry() + ": " + definition.error().error);
    }
    if (!definition) {
      problems.push_back(change.entry() + ": " + definition.error().error);
    }
    found.push_back(definition.value_or(nullptr));
  }

  // Change each entry, naming it in its problems.
  for (int index = 0; index < definitions.size(); ++index) {
    if (!found[index]) {
      continue;
    }
    std::vector<std::string> own;
    Change(definitions[index], found[index], schema, module_name, undo, own);
    for (auto& problem : own) {
      problems.push_back(definitions[index].entry() + ": " + problem);
    }
  }
  return problems;
}

}  // namespace modlock::wasm
