//go:build deps_only

#include "wasm/definition_changes.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

// Key is the key a path step selects in a map or script value list: an enum
// value, matched by its bytes at the key's storage size, or a symbol's text,
// matched against the characters a node's key points to.
struct Key {
  std::array<unsigned char, sizeof(int64_t)> bytes{};
  size_t size = 0;
  std::optional<std::string> text;

  // Matches reports whether the key stored at storage is this key.
  bool Matches(const unsigned char* storage) const {
    if (!text) {
      return std::memcmp(storage, bytes.data(), size) == 0;
    }
    const char* chars = nullptr;
    std::memcpy(&chars, storage, sizeof(chars));
    return chars != nullptr && *text == chars;
  }
};

// Layout is a schema type's storage size and alignment.
struct Layout {
  size_t size;
  size_t alignment;
};

// IsSymbol reports whether a type is a pointer to characters, the key type of
// a map keyed by name, such as m_mapWeaponInfos.
bool IsSymbol(std::string_view type) {
  return type == "CGlobalSymbol" || type == "CUtlSymbolLarge" || type == "CUtlString";
}

// LayoutOf returns the storage of a map's key or value type: a builtin
// number, a symbol, an enum or a declared class.
std::expected<Layout, std::string> LayoutOf(void* schema, const char* module,
                                            std::string_view type) {
  static constexpr std::array<std::pair<std::string_view, size_t>, 11> kBuiltins{{
      {"bool", 1},
      {"int8", 1},
      {"uint8", 1},
      {"int16", 2},
      {"uint16", 2},
      {"int32", 4},
      {"uint32", 4},
      {"float32", 4},
      {"int64", 8},
      {"uint64", 8},
      {"float64", 8},
  }};
  for (const auto& [name, size] : kBuiltins) {
    if (type == name) {
      return Layout{size, size};
    }
  }
  if (IsSymbol(type)) {
    return Layout{sizeof(char*), alignof(char*)};
  }
  const std::string name(type);
  if (auto size = gameinterop::SchemaEnumSizeOf(schema, module, name.c_str())) {
    return Layout{*size, *size};
  }
  auto size = gameinterop::SchemaClassSizeOf(schema, module, name.c_str());
  auto alignment = gameinterop::SchemaClassAlignmentOf(schema, module, name.c_str());
  if (!size || !alignment) {
    return std::unexpected("type " + name + " has no known layout");
  }
  return Layout{*size, *alignment};
}

// AlignUp rounds offset up to a power-of-two alignment.
size_t AlignUp(size_t offset, size_t alignment) {
  return (offset + alignment - 1) & ~(alignment - 1);
}

// KeyOf resolves the text between a step's brackets for a key of key_type: a
// symbol's text, or an enum value named Value or key_type.Value.
std::expected<Key, std::string> KeyOf(void* schema, const char* module, std::string_view key_type,
                                      std::string_view text) {
  // A symbol key is its text.
  if (IsSymbol(key_type)) {
    return Key{.size = sizeof(char*), .text = std::string(text)};
  }

  // An enum key is its value, stored in the enum's size.
  std::string_view value = text;
  if (const auto dot = text.find('.'); dot != std::string_view::npos) {
    if (text.substr(0, dot) != key_type) {
      return std::unexpected("the key is a " + std::string(key_type));
    }
    value = text.substr(dot + 1);
  }
  const std::string type(key_type);
  const std::string name(value);
  auto size = gameinterop::SchemaEnumSizeOf(schema, module, type.c_str());
  if (!size) {
    return std::unexpected("cannot key by " + type);
  }
  if (*size != 1 && *size != 2 && *size != 4 && *size != 8) {
    return std::unexpected(type + " has an invalid size");
  }
  auto resolved = gameinterop::SchemaEnumValueOf(schema, module, type.c_str(), name.c_str());
  if (!resolved) {
    return std::unexpected(resolved.error());
  }
  Key key{.size = *size};
  std::memcpy(key.bytes.data(), &*resolved, *size);
  return key;
}

// ScriptValues is the layout of ModifierScriptValue_t and the key a path
// selects in a modifier's script value list.
struct ScriptValues {
  size_t stride;
  size_t key_offset;
  size_t value_offset;
  Key key;
};

// ResolveScriptValues resolves the layout of ModifierScriptValue_t and the
// EModifierValue key a path names in an indexed CUtlVector< ModifierScriptValue_t >.
std::expected<ScriptValues, std::string> ResolveScriptValues(void* schema, const char* module,
                                                             std::string_view text) {
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
  auto selected = KeyOf(schema, module, key->type, text);
  if (!selected) {
    return std::unexpected(selected.error());
  }
  auto value = gameinterop::SchemaFieldOf(schema, module, "ModifierScriptValue_t", "m_value");
  if (!value) {
    return std::unexpected(value.error());
  }
  if (key->size < selected->size || value->size < 4 * sizeof(float) ||
      key->offset + selected->size > *stride || value->offset + 4 * sizeof(float) > *stride) {
    return std::unexpected("modifier script value layout is invalid");
  }
  return ScriptValues{.stride = *stride,
                      .key_offset = key->offset,
                      .value_offset = value->offset,
                      .key = std::move(*selected)};
}

// MapLookup is one map a path passes through: the map's offset from the
// current base, the key it selects and its node layout. The selected value,
// value_type spelled as the schema spells it, is the base of the rest of the
// path.
struct MapLookup {
  size_t offset = 0;
  Key key;
  size_t value_offset;
  size_t element_size;
  std::string_view value_type;
  size_t value_size;
};

// ResolveMapLookup resolves the key text for a field of map_type, a
// CUtlOrderedMap< K, V > or CUtlMap< K, V >. A node's element is the key,
// then the value at its own alignment.
std::expected<MapLookup, std::string> ResolveMapLookup(void* schema, const char* module,
                                                       std::string_view map_type,
                                                       std::string_view text) {
  // Split the map's key and value types.
  std::string_view arguments;
  for (const std::string_view map : {"CUtlOrderedMap< ", "CUtlMap< "}) {
    if (map_type.starts_with(map) && map_type.ends_with(" >")) {
      arguments = map_type.substr(map.size(), map_type.size() - map.size() - 2);
    }
  }
  const auto comma = arguments.find(", ");
  if (comma == std::string_view::npos) {
    return std::unexpected("not a map");
  }
  const auto key_type = arguments.substr(0, comma);
  const auto value_type = arguments.substr(comma + 2);

  // Lay out the node's element.
  auto key = KeyOf(schema, module, key_type, text);
  if (!key) {
    return std::unexpected(key.error());
  }
  auto key_layout = LayoutOf(schema, module, key_type);
  auto value_layout = LayoutOf(schema, module, value_type);
  if (!key_layout || !value_layout) {
    return std::unexpected(!key_layout ? key_layout.error() : value_layout.error());
  }
  const size_t value_offset = AlignUp(key_layout->size, value_layout->alignment);
  return MapLookup{
      .key = std::move(*key),
      .value_offset = value_offset,
      .element_size = AlignUp(value_offset + value_layout->size,
                              std::max(key_layout->alignment, value_layout->alignment)),
      .value_type = value_type,
      .value_size = value_layout->size,
  };
}

// Place is where a field path leads in an entry's data: through each map in
// maps, then offset from the last map's value or the entry's start, or one
// entry of a modifier script value list there.
struct Place {
  std::vector<MapLookup> maps;
  size_t offset = 0;
  size_t size = 0;
  size_t floats = 0;
  std::string_view type;
  std::string leaf;
  std::optional<ScriptValues> script_values;
};

// FloatCount returns how many floats storage of a scalar, range or level-float
// type holds, or zero when a float change cannot write it.
size_t FloatCount(std::string_view type, size_t size) {
  if (type == "CModifierLevelFloat" || type == "CSkillFloat" || type == "float32[4]") {
    return size >= 4 * sizeof(float) ? 4 : 0;
  }
  if (type == "CRangeFloat" || type == "float32[2]") {
    return size >= 2 * sizeof(float) ? 2 : 0;
  }
  if (type == "float32") {
    return size >= sizeof(float) ? 1 : 0;
  }
  return type.empty() && size == sizeof(float) ? 1 : 0;
}

// ResolvePath resolves a path of Class.field steps separated by slashes, such
// as CitadelAbilityVData.m_projectileInfo/ProjectileInfo_t.m_flSpeed. A step
// on a map may end in [key], an enum value such as [EStatsType.EMaxHealth] or
// the name of a symbol key such as [primary], and the next step reads the
// selected value. A last step on a modifier's m_vecScriptValues list may end
// in [EModifierValue.Value].
std::expected<Place, std::string> ResolvePath(void* schema, const char* module,
                                              std::string_view path) {
  if (path.empty()) {
    return std::unexpected("field path is empty");
  }
  Place place;
  while (!path.empty()) {
    // Split off the next step and its key.
    const auto slash = path.find('/');
    auto step = path.substr(0, slash);
    path = slash == std::string_view::npos ? std::string_view{} : path.substr(slash + 1);
    std::optional<std::string_view> key;
    if (step.ends_with(']')) {
      const auto open = step.rfind('[');
      if (open == std::string_view::npos) {
        return std::unexpected("a map key lacks its [");
      }
      key = step.substr(open + 1, step.size() - open - 2);
      step = step.substr(0, open);
    }

    // Each step is Class.field; add its offset.
    const auto dot = step.find('.');
    if (dot == std::string_view::npos) {
      return std::unexpected("field path step lacks a class");
    }
    const std::string type(step.substr(0, dot));
    const std::string name(step.substr(dot + 1));
    auto field = gameinterop::SchemaFieldOf(schema, module, type.c_str(), name.c_str());
    if (!field) {
      return std::unexpected(field.error());
    }
    place.offset += field->offset;
    place.size = field->size;
    place.type = field->type ? field->type : "";
    place.floats = FloatCount(place.type, place.size);
    const auto last_dot = name.rfind('.');
    place.leaf = last_dot == std::string::npos ? name : name.substr(last_dot + 1);
    if (!key) {
      continue;
    }

    // A key on a script value list selects one entry's level float, which
    // ends the path.
    if (field->list && place.type == "CUtlVector< ModifierScriptValue_t >") {
      if (!path.empty()) {
        return std::unexpected("a script value has no fields");
      }
      auto layout = ResolveScriptValues(schema, module, *key);
      if (!layout) {
        return std::unexpected(layout.error());
      }
      place.script_values = std::move(*layout);
      place.type = {};
      place.floats = 4;
      place.size = 4 * sizeof(float);
      break;
    }

    // A key on a map selects one value, the base of the rest of the path.
    auto lookup = ResolveMapLookup(schema, module, place.type, *key);
    if (!lookup) {
      return std::unexpected(name + "[" + std::string(*key) + "]: " + lookup.error());
    }
    lookup->offset = place.offset;
    place.offset = 0;
    place.size = lookup->value_size;
    place.type = lookup->value_type;
    place.floats = FloatCount(place.type, place.size);
    place.maps.push_back(std::move(*lookup));
  }
  return place;
}

// Locate returns the storage a place names in one entry's data, or null when
// a map or list on its path holds no such key.
unsigned char* Locate(unsigned char* definition, const Place& place) {
  // Enter each map's selected value.
  unsigned char* base = definition;
  for (const auto& map : place.maps) {
    unsigned char* found = nullptr;
    ForEachMapNode(base + map.offset, map.element_size, [&](unsigned char* element) {
      if (!found && map.key.Matches(element)) {
        found = element + map.value_offset;
      }
    });
    if (!found) {
      return nullptr;
    }
    base = found;
  }
  if (!place.script_values) {
    return base + place.offset;
  }

  // A modifier script value sits in a CUtlVector of ModifierScriptValue_t.
  const auto& values = *place.script_values;
  const auto list = gameinterop::ReadUtlVector<unsigned char>(base + place.offset, 256);
  if (!list) {
    return nullptr;
  }
  for (int32_t i = 0; i < list->count; ++i) {
    auto* element = list->data + static_cast<size_t>(i) * values.stride;
    if (values.key.Matches(element + values.key_offset)) {
      return element + values.value_offset;
    }
  }
  return nullptr;
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
  // Refuse console/script fields and non-text schema types.
  if (place.type.empty()) {
    return "not a text field";
  }
  if (auto refused = RefuseField(place.leaf); !refused.empty()) {
    return refused;
  }
  auto* storage = Locate(definition, place);
  if (storage == nullptr) {
    return "no such map key";
  }
  std::string expanded = WithAddon(std::string(value), text.addon);
  const std::string_view type = place.type;

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
