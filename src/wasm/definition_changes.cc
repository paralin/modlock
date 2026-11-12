//go:build deps_only

#include "wasm/definition_changes.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <string_view>
#include <utility>

#include "gameinterop/utl_vector.h"
#include "modlock/gameinterop/ability_definitions.h"
#include "modlock/gameinterop/bot_difficulty_definitions.h"
#include "modlock/gameinterop/entity_abi.h"
#include "modlock/gameinterop/hero_definitions.h"
#include "modlock/gameinterop/keyvalues.h"
#include "wasm/entity_rules.h"
#include "wasm/text_values.h"

namespace modlock::wasm {
namespace {

using wasm::Definition;

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

// ScriptValues is the layout of ModifierScriptValue_t when a path indexes a
// modifier's script value list by EModifierValue.
struct ScriptValues {
  size_t stride;
  size_t key_offset;
  size_t key_size;
  size_t value_offset;
};

// Place is where a field path leads in an entry's data: a field, or the value
// of one key in an enum-keyed map or modifier script value list.
struct Place {
  size_t offset;
  size_t size;
  size_t floats = 0;
  const char* type = nullptr;
  std::string leaf;
  std::optional<int32_t> key;
  std::optional<ScriptValues> script_values;
};

// FloatCount returns how many floats a field's scalar, range or level-float
// storage holds, or zero when a float change cannot write the field.
size_t FloatCount(const gameinterop::SchemaField& field) {
  if (field.type != nullptr) {
    const std::string_view type(field.type);
    if (type == "CModifierLevelFloat" || type == "CSkillFloat" || type == "float32[4]") {
      return field.size >= 4 * sizeof(float) ? 4 : 0;
    }
    if (type == "CRangeFloat" || type == "float32[2]") {
      return field.size >= 2 * sizeof(float) ? 2 : 0;
    }
    if (type == "float32") {
      return field.size >= sizeof(float) ? 1 : 0;
    }
  }
  return field.size == sizeof(float) ? 1 : 0;
}

// ResolveScriptValues resolves the layout of ModifierScriptValue_t for an
// indexed CUtlVector< ModifierScriptValue_t > field.
std::expected<ScriptValues, std::string> ResolveScriptValues(void* schema, const char* module) {
  auto stride = gameinterop::SchemaClassSizeOf(schema, module, "ModifierScriptValue_t");
  if (!stride) {
    return std::unexpected(stride.error());
  }
  auto key =
      gameinterop::SchemaFieldOf(schema, module, "ModifierScriptValue_t", "m_eModifierValue");
  if (!key) {
    return std::unexpected(key.error());
  }
  if (!key->type) {
    return std::unexpected("modifier script value key has no type");
  }
  auto key_size = gameinterop::SchemaEnumSizeOf(schema, module, key->type);
  if (!key_size) {
    return std::unexpected(key_size.error());
  }
  auto value = gameinterop::SchemaFieldOf(schema, module, "ModifierScriptValue_t", "m_value");
  if (!value) {
    return std::unexpected(value.error());
  }
  if ((*key_size != sizeof(uint8_t) && *key_size != sizeof(uint16_t) &&
       *key_size != sizeof(int32_t)) ||
      key->size < *key_size || value->size < 4 * sizeof(float) ||
      key->offset + *key_size > *stride || value->offset + 4 * sizeof(float) > *stride) {
    return std::unexpected("modifier script value layout is invalid");
  }
  return ScriptValues{.stride = *stride,
                      .key_offset = key->offset,
                      .key_size = *key_size,
                      .value_offset = value->offset};
}

// ResolvePath sums the offsets along a path of Class.field steps separated by
// slashes, such as CitadelAbilityVData.m_projectileInfo/ProjectileInfo_t.m_flSpeed.
// A last step may end in [Enum.Value], naming one value of a map field or a
// modifier's m_vecScriptValues list.
std::expected<Place, std::string> ResolvePath(void* schema, const char* module,
                                              std::string_view path) {
  // Split off an enum key.
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
  Place place{.offset = 0, .size = 0, .floats = 0, .key = key, .script_values = std::nullopt};
  std::optional<gameinterop::SchemaField> last;
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
    place.floats = FloatCount(*field);
    place.type = field->type;
    const auto last_dot = name.rfind('.');
    place.leaf = last_dot == std::string::npos ? name : name.substr(last_dot + 1);
    last = *field;
  }
  if (!last) {
    return std::unexpected("field path is empty");
  }

  // An indexed step selects one value in a modifier script value list or an
  // enum-keyed float map.
  if (place.key) {
    place.type = nullptr;
    if (last->list && last->type != nullptr &&
        std::string_view(last->type) == "CUtlVector< ModifierScriptValue_t >") {
      auto layout = ResolveScriptValues(schema, module);
      if (!layout) {
        return std::unexpected(layout.error());
      }
      place.script_values = *layout;
      place.floats = 4;
      place.size = 4 * sizeof(float);
    } else {
      place.floats = 1;
      place.size = sizeof(float);
    }
  }
  return place;
}

// Locate returns the storage a place names in one entry's data, or null when
// its map or list holds no such key.
unsigned char* Locate(unsigned char* definition, const Place& place) {
  // A field lies at its offset.
  if (!place.key) {
    return definition + place.offset;
  }

  // A modifier script value sits in a CUtlVector of ModifierScriptValue_t.
  if (place.script_values) {
    const auto list = gameinterop::ReadUtlVector<unsigned char>(definition + place.offset, 256);
    if (!list) {
      return nullptr;
    }
    for (int32_t i = 0; i < list->count; ++i) {
      auto* element = list->data + static_cast<size_t>(i) * place.script_values->stride;
      int32_t key = 0;
      if (place.script_values->key_size == sizeof(uint8_t)) {
        uint8_t raw = 0;
        std::memcpy(&raw, element + place.script_values->key_offset, sizeof(raw));
        key = raw;
      } else if (place.script_values->key_size == sizeof(uint16_t)) {
        uint16_t raw = 0;
        std::memcpy(&raw, element + place.script_values->key_offset, sizeof(raw));
        key = raw;
      } else {
        std::memcpy(&key, element + place.script_values->key_offset, sizeof(key));
      }
      if (key == *place.key) {
        return element + place.script_values->value_offset;
      }
    }
    return nullptr;
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

// kBufferStringInlineEmpty is the allocated word of an empty CBufferString.
constexpr uint32_t kBufferStringInlineEmpty = 0xc0000008u;

// kBufferStringAllowHeap marks a CBufferString whose borrowed characters the
// game may replace with a heap allocation without freeing the borrowed pointer.
constexpr uint32_t kBufferStringAllowHeap = 0x80000000u;

// kBufferStringMaxLength bounds the 30-bit capacity of a CBufferString.
constexpr size_t kBufferStringMaxLength = 0x3ffffffeu;

// BufferString is the 16-byte CBufferString header that CSoundEventName and
// CPanoramaImageName hold in parsed VData. Purge frees chars only when bit 31
// of length is set, so borrowed interned characters with bit 31 clear are
// never freed when a world unloads.
struct BufferString {
  uint32_t length;
  uint32_t allocated;
  const char* chars;
};

static_assert(sizeof(BufferString) == 16);

// WriteTextField writes value into the text, symbol, sound event or Panorama
// image field at place, keeping the overwritten bytes in undo, and returns any
// problem it hits.
std::string WriteTextField(unsigned char* definition, const Place& place, std::string_view value,
                           const DefinitionText& text, bool world, DefinitionUndo& undo) {
  // Refuse map keys, console/script fields, and non-text schema types.
  if (place.key || place.type == nullptr) {
    return "not a text field";
  }
  if (auto refused = RefuseField(place.leaf); !refused.empty()) {
    return refused;
  }
  auto* storage = Locate(definition, place);
  std::string expanded = WithAddon(std::string(value), text.addon);
  const std::string_view type(place.type);

  // Write an 8-byte symbol or CUtlString pointer. The game frees a CUtlString
  // and compares a CGlobalSymbol by pointer, so each takes the game's own
  // memory; an empty CUtlString is null.
  if (type == "CUtlSymbolLarge" || type == "CGlobalSymbol" || type == "CUtlString") {
    if (place.size < sizeof(const char*)) {
      return "not a text field";
    }
    const char* chars = nullptr;
    if (type == "CGlobalSymbol") {
      chars = text.intern_symbol(expanded.c_str());
      if (chars == nullptr) {
        return "the game's symbol table is unavailable";
      }
    } else if (type == "CUtlString") {
      if (!expanded.empty()) {
        auto* owned = static_cast<char*>(text.allocate(expanded.size() + 1));
        if (owned == nullptr) {
          return "the game's allocator is unavailable";
        }
        std::memcpy(owned, expanded.c_str(), expanded.size() + 1);
        chars = owned;
      }
    } else {
      chars = Interned(std::move(expanded));
    }
    undo.Save(storage, sizeof(chars), world);
    std::memcpy(storage, &chars, sizeof(chars));
    return {};
  }

  // Write a 16-byte CBufferString for sound events and Panorama images.
  if (type == "CSoundEventName" || type == "CPanoramaImageName") {
    if (place.size < sizeof(BufferString)) {
      return "not a text field";
    }
    if (expanded.size() > kBufferStringMaxLength) {
      return "text is too long";
    }
    const BufferString value =
        expanded.empty()
            ? BufferString{.length = 0, .allocated = kBufferStringInlineEmpty, .chars = nullptr}
            : BufferString{
                  .length = static_cast<uint32_t>(expanded.size()),
                  .allocated = kBufferStringAllowHeap | static_cast<uint32_t>(expanded.size() + 1),
                  .chars = Interned(std::move(expanded))};
    undo.Save(storage, sizeof(value), world);
    std::memcpy(storage, &value, sizeof(value));
    return {};
  }

  return "not a text field";
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
      case Definition::FILE_MODIFIERS:
        return FindSubclass(modifiers_, gameinterop::AbilityDefinitions::kModifiers, change);
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
  Subclasses modifiers_;
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

std::vector<std::string> ChangeDefinition(const Definition& change, unsigned char* definition,
                                          void* schema, const char* module_name,
                                          DefinitionUndo& undo, const DefinitionText& text) {
  // Apply properties, numeric fields, copied fields and text fields in turn.
  std::vector<std::string> problems;
  const bool world = change.file() == Definition::FILE_HEROES;
  SetProperties(definition, schema, module_name,
                {change.properties().begin(), change.properties().end()}, undo, problems);
  for (const auto& [path, value] : change.fields()) {
    auto place = ResolvePath(schema, module_name, path);
    auto* storage = place && place->floats ? Locate(definition, *place) : nullptr;
    if (!storage) {
      problems.push_back(path + ": " +
                         (!place           ? place.error()
                          : !place->floats ? "not a float"
                                           : "no such map key"));
      continue;
    }
    const std::array<float, 4> values{value, value, value, value};
    const size_t bytes = place->floats * sizeof(value);
    undo.Save(storage, bytes, world);
    std::memcpy(storage, values.data(), bytes);
  }
  for (const auto& [target, source] : change.copy_fields()) {
    auto to = ResolvePath(schema, module_name, target);
    auto from = ResolvePath(schema, module_name, source);
    auto* to_storage = to ? Locate(definition, *to) : nullptr;
    auto* from_storage = from ? Locate(definition, *from) : nullptr;
    if (!to_storage || !from_storage || to->size != from->size || !to->size) {
      problems.push_back(target + ": cannot copy " + source);
      continue;
    }
    undo.Save(to_storage, to->size, world);
    std::memmove(to_storage, from_storage, to->size);
  }
  for (const auto& [path, value] : change.text_fields()) {
    auto place = ResolvePath(schema, module_name, path);
    if (!place) {
      problems.push_back(path + ": " + place.error());
      continue;
    }
    if (auto problem = WriteTextField(definition, *place, value, text, world, undo);
        !problem.empty()) {
      problems.push_back(path + ": " + problem);
    }
  }
  return problems;
}

std::expected<std::vector<std::string>, std::string> ChangeDefinitions(
    const gameinterop::ModuleImage& module, void* schema, const char* module_name,
    const google::protobuf::RepeatedPtrField<Definition>& definitions, DefinitionUndo& undo,
    const DefinitionText& text) {
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
    for (auto& problem :
         ChangeDefinition(definitions[index], found[index], schema, module_name, undo, text)) {
      problems.push_back(definitions[index].entry() + ": " + problem);
    }
  }
  return problems;
}

}  // namespace modlock::wasm
