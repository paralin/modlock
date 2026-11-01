#include <google/protobuf/json/json.h>
#include <google/protobuf/struct.pb.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string_view>
#include <tuple>
#include <unordered_set>
#include <vector>

#include "host_app/game_dump_internal.h"

namespace modlock::host_app {

namespace {

// The server's entity class records, after sourcesdk public/entity2 and
// public/datamap.h. Each designer name has one static EntityClassInfo in the
// module that links it; its data description lists the key values the class
// adds to its base. Inputs and outputs are not in data descriptions: the
// server declares them as Pulse bindings in its module metadata.

struct DataMap;

struct TypeDescription {
  // type is a one-byte fieldtype_t; the bytes after it are padding.
  uint8_t type;
  const char* name;
  int32_t offset;
  uint16_t count;
  int32_t flags;
  const char* external_name;
  const DataMap* embedded;
  int32_t size_in_bytes;
};

struct DataMap {
  const TypeDescription* fields;
  int32_t field_count;
  const char* name;
  const DataMap* base;
};

struct EntityClassInfo {
  const char* designer_name;
  const char* class_name;
  const char* description;
  const void* entity_class;
  const EntityClassInfo* base;
  const schema::ClassInfo* schema_binding;
  const DataMap* data_map;
  const DataMap* prediction_map;
};

static_assert(sizeof(TypeDescription) == 0x38);
static_assert(offsetof(TypeDescription, external_name) == 0x20);
static_assert(offsetof(DataMap, base) == 0x18);
static_assert(offsetof(EntityClassInfo, schema_binding) == 0x28);

// The data description flag of spawn key values (FTYPEDESC_KEY), the flag of
// a key the class parses itself, which has no field, such as the scene node's
// scale, and the field type of embedded data maps.
constexpr int32_t kKeyFlag = 1 << 2;
constexpr int32_t kParsedKeyFlag = 1 << 12;
constexpr uint8_t kEmbeddedType = 10;

// tier0's KeyValues3 JSON writer: bool SaveKV3AsJSON(const KeyValues3*,
// CUtlString* error, CUtlString* json).
constexpr std::string_view kSaveKV3AsJSON =
    "?SaveKV3AsJSON@@YA_NPEBVKeyValues3@@PEAVCUtlString@@1@Z";

// EntityClasses returns the server's entity class records, one per designer
// name: static records whose schema binding is a server class of the same
// name.
std::vector<const EntityClassInfo*> EntityClasses(
    const Image& server, const std::unordered_set<const void*>& server_classes) {
  // Keep each named entity class bound to a server schema class, once per designer name.
  std::vector<const EntityClassInfo*> classes;
  std::unordered_set<std::string_view> designer_names;
  for (const auto section : server.WritableSections()) {
    for (size_t offset = 0; offset + sizeof(EntityClassInfo) <= section.size();
         offset += kRecordStride) {
      // Keep one class for each designer name bound to a server class.
      const auto* info = reinterpret_cast<const EntityClassInfo*>(section.data() + offset);
      if (!server_classes.contains(info->schema_binding)) {
        continue;
      }
      const auto class_name = server.Name(info->class_name);
      const auto designer_name = server.Name(info->designer_name);
      if (designer_name.empty() || class_name != info->schema_binding->name) {
        continue;
      }
      if (designer_names.insert(designer_name).second) {
        classes.push_back(info);
      }
    }
  }

  // Order them by designer name.
  std::ranges::sort(classes, {}, [](const EntityClassInfo* info) {
    return std::string_view(info->designer_name);
  });
  return classes;
}

// CollectDataMaps adds map, its base chain and its embedded maps by name.
void CollectDataMaps(const DataMap* map, std::map<std::string_view, const DataMap*>& maps) {
  for (; map != nullptr && map->name != nullptr; map = map->base) {
    if (!maps.emplace(map->name, map).second) {
      return;
    }
    for (const auto& field : std::span(map->fields, size_t(std::max(map->field_count, 0)))) {
      if (field.type == kEmbeddedType) {
        CollectDataMaps(field.embedded, maps);
      }
    }
  }
}

// AddDataMap describes a data map's spawn key values and embedded maps.
void AddDataMap(const DataMap& map, dump::DataMap& out) {
  out.set_name(map.name);
  if (map.base && map.base->name) {
    out.set_base(map.base->name);
  }
  for (const auto& field : std::span(map.fields, size_t(std::max(map.field_count, 0)))) {
    if (field.flags & (kKeyFlag | kParsedKeyFlag)) {
      // A key field carries its external name; a key the class parses itself
      // has only the key's name.
      const bool stored = field.flags & kKeyFlag;
      auto& key = *out.add_key_values();
      key.set_name(Text(stored ? field.external_name : field.name));
      if (stored) {
        key.set_field(Text(field.name));
      }
      key.set_type(static_cast<dump::KeyType>(field.type + 1));
      key.set_count(field.count);
    }
    if (field.type == kEmbeddedType && field.embedded && field.embedded->name) {
      auto& embedded = *out.add_embedded();
      embedded.set_field(Text(field.name));
      embedded.set_data_map(field.embedded->name);
    }
  }
}

// UtlString is CUtlString: one pointer to text the game allocates.
struct UtlString {
  const char* text = nullptr;
};

// QuoteNonFinite quotes the bare nan and inf words SaveKV3AsJSON writes for
// non-finite floats, which JSON cannot spell. -nan precedes nan so its sign
// stays inside the quotes.
std::string QuoteNonFinite(std::string_view json) {
  // Copy the text, quoting nan and inf outside strings.
  constexpr std::string_view kWords[] = {"-nan", "nan", "-inf", "inf"};
  std::string out;
  out.reserve(json.size());
  bool in_string = false;
  for (size_t i = 0; i < json.size(); ++i) {
    // Copy strings as they are and quote bare words.
    const char c = json[i];
    if (in_string) {
      out += c;
      if (c == '\\' && i + 1 < json.size()) {
        out += json[++i];
      } else if (c == '"') {
        in_string = false;
      }
      continue;
    }
    const auto word = std::ranges::find_if(
        kWords, [rest = json.substr(i)](std::string_view word) { return rest.starts_with(word); });
    if (word != std::end(kWords)) {
      // Quote the word.
      out += '"';
      out += *word;
      out += '"';
      i += word->size() - 1;
      continue;
    }
    in_string = c == '"';
    out += c;
  }
  return out;
}

// ModuleMetadata returns the metadata a module's providers build, such as its
// Pulse bindings. The game allocates the text it returns, which the dump
// leaves to process exit.
std::expected<google::protobuf::Struct, std::string> ModuleMetadata(const Image& module,
                                                                    const Image& tier0) {
  // Resolve the module's metadata builder and tier0's JSON writer.
  using Extract = const void* (*)(UtlString&);
  using Save = bool (*)(const void*, UtlString*, UtlString*);
  const auto extract = reinterpret_cast<Extract>(module.Export("ExtractModuleMetadata"));
  const auto save = reinterpret_cast<Save>(tier0.Export(kSaveKV3AsJSON));
  if (!extract || !save) {
    return std::unexpected("module metadata exports are missing");
  }

  // Build the metadata and write it as JSON.
  UtlString info;
  const void* metadata = extract(info);
  if (!metadata) {
    return std::unexpected(module.file + " metadata failed to build: " + Text(info.text));
  }
  UtlString error;
  UtlString json;
  if (!save(metadata, &error, &json) || !json.text) {
    return std::unexpected("cannot write " + module.file + " metadata: " + Text(error.text));
  }

  // Parse the JSON once its non-finite numbers are quoted.
  google::protobuf::Struct parsed;
  if (auto status = google::protobuf::json::JsonStringToMessage(QuoteNonFinite(json.text), &parsed);
      !status.ok()) {
    return std::unexpected("cannot read " + module.file +
                           " metadata: " + std::string(status.message()));
  }
  return parsed;
}

// Member returns the named member of an object value, or null.
const google::protobuf::Value* Member(const google::protobuf::Value* value,
                                      const std::string& name) {
  if (!value || !value->has_struct_value()) {
    return nullptr;
  }
  const auto& fields = value->struct_value().fields();
  const auto found = fields.find(name);
  return found == fields.end() ? nullptr : &found->second;
}

// String returns a string value, or an empty string for any other value.
std::string String(const google::protobuf::Value* value) {
  return value && value->has_string_value() ? value->string_value() : "";
}

// Bool reports whether value is true.
bool Bool(const google::protobuf::Value* value) {
  return value && value->bool_value();
}

// AddParameters adds the members of a Pulse parameter list except the target
// entity, sorted by name since the parsed object keeps no order.
template <typename Repeated>
void AddParameters(const google::protobuf::Value* list, const std::string& target, Repeated* out) {
  if (!list || !list->has_struct_value()) {
    return;
  }
  for (const auto& [name, parameter] : list->struct_value().fields()) {
    if (name == target) {
      continue;
    }
    auto& added = *out->Add();
    added.set_name(name);
    added.set_type(String(Member(&parameter, "type")));
  }
  std::ranges::sort(*out, {}, &dump::Parameter::name);
}

// AddPulseBindings adds the entity inputs and outputs among the server's
// Pulse bindings. An entity binding targets an entity handle typed with the
// designer name of its class, or untyped for every entity.
void AddPulseBindings(const google::protobuf::Struct& metadata, dump::Entities& entities) {
  // Find the binding classes.
  google::protobuf::Value root;
  *root.mutable_struct_value() = metadata;
  const auto* bindings = Member(Member(Member(&root, "pulse_bindings"), "gamedata"), "m_Classes");
  if (!bindings || !bindings->has_struct_value()) {
    return;
  }

  // Add each binding that targets an entity as an input or an output.
  constexpr std::string_view kEntityHandle = "PVAL_EHANDLE";
  for (const auto& [key, binding] : bindings->struct_value().fields()) {
    // Keep a pulse method or output.
    const auto* meta = Member(&binding, "m_MetaData");
    const bool input = Bool(Member(meta, "is_pulse_target_method"));
    if (!input && !Bool(Member(meta, "is_pulse_target_output"))) {
      continue;
    }

    // Its target must be an entity handle, perhaps of one class.
    const auto target = String(Member(meta, "target_arg_name"));
    const auto* parameters = Member(meta, input ? "pulse_inparams" : "pulse_outparams");
    const auto target_type = String(Member(Member(parameters, target), "type"));
    std::string_view type = target_type;
    if (!type.starts_with(kEntityHandle)) {
      continue;
    }
    type.remove_prefix(kEntityHandle.size());
    if (!type.empty() && !type.starts_with(':')) {
      continue;
    }

    // Add it to the inputs or the outputs.
    const auto entity = std::string(type.empty() ? type : type.substr(1));
    const auto name = key.substr(key.rfind(':') + 1);
    const auto description = String(Member(&binding, "m_Description"));
    if (input) {
      // An input has parameters and returns.
      auto& added = *entities.add_inputs();
      added.set_entity(entity);
      added.set_name(name);
      added.set_description(description);
      AddParameters(parameters, target, added.mutable_parameters());
      AddParameters(Member(meta, "pulse_outparams"), target, added.mutable_returns());
    } else {
      // An output has parameters.
      auto& added = *entities.add_outputs();
      added.set_entity(entity);
      added.set_name(name);
      added.set_description(description);
      AddParameters(parameters, target, added.mutable_parameters());
    }
  }

  // Order both lists by entity, then name, since the parsed object keeps none.
  const auto by_entity = [](const auto& binding) {
    return std::tie(binding.entity(), binding.name());
  };
  std::ranges::sort(*entities.mutable_inputs(), {}, by_entity);
  std::ranges::sort(*entities.mutable_outputs(), {}, by_entity);
}

}  // namespace

std::expected<dump::Entities, std::string> DescribeEntities(
    const Image& server, const Image& tier0,
    const std::unordered_set<const void*>& server_classes) {
  // Describe the entity classes and collect the data maps they reach.
  dump::Entities entities;
  std::map<std::string_view, const DataMap*> data_maps;
  const auto entity_classes = EntityClasses(server, server_classes);
  const std::unordered_set<const EntityClassInfo*> listed(entity_classes.begin(),
                                                          entity_classes.end());
  for (const auto* info : entity_classes) {
    // Add the class's names, description and data map.
    auto& added = *entities.add_classes();
    added.set_designer_name(info->designer_name);
    added.set_class_name(info->class_name);
    if (server.Contains(info->description)) {
      added.set_description(info->description);
    }
    if (info->data_map && info->data_map->name) {
      added.set_data_map(info->data_map->name);
    }

    // Abstract records without a designer name sit between listed classes.
    const auto* base = info->base;
    while (base && !listed.contains(base)) {
      base = base->base;
    }
    if (base) {
      added.set_base(base->designer_name);
    }
    CollectDataMaps(info->data_map, data_maps);
  }

  // Entities also take the key values of their components, such as the scene
  // node's origin and scale behind the body component. No entity map reaches
  // a component's, so collect every server schema class's own map.
  for (const auto* binding : server_classes) {
    const auto* map = static_cast<const schema::ClassInfo*>(binding)->data_map;
    if (server.Contains(map)) {
      CollectDataMaps(static_cast<const DataMap*>(map), data_maps);
    }
  }
  for (const auto& [name, map] : data_maps) {
    AddDataMap(*map, *entities.add_data_maps());
  }

  // Add the inputs and outputs from the server's Pulse bindings.
  auto metadata = ModuleMetadata(server, tier0);
  if (!metadata) {
    return std::unexpected(metadata.error());
  }
  AddPulseBindings(*metadata, entities);
  return entities;
}

}  // namespace modlock::host_app
