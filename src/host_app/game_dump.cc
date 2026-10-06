#include "host_app/game_dump.h"

#include <google/protobuf/json/json.h>
#include <google/protobuf/struct.pb.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <span>
#include <string_view>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

#include "gameinterop/schema_layout.h"
#include "modlock/gameinterop/console_variables.h"
#include "modlock/gameinterop/entity_abi.h"
#include "modlock/gameinterop/mapped_module_image.h"
#include "proto/modlock/dump/dump.pb.h"

namespace modlock::host_app {
namespace {

namespace fs = std::filesystem;
namespace schema = gameinterop::schema;

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

// The data description flag of spawn key values (FTYPEDESC_KEY), and the
// field type of embedded data maps.
constexpr int32_t kKeyFlag = 1 << 2;
constexpr uint8_t kEmbeddedType = 10;

// tier0's KeyValues3 JSON writer: bool SaveKV3AsJSON(const KeyValues3*,
// CUtlString* error, CUtlString* json).
constexpr std::string_view kSaveKV3AsJSON =
    "?SaveKV3AsJSON@@YA_NPEBVKeyValues3@@PEAVCUtlString@@1@Z";

// The class flags a dump names, from SCHEMA_CF1_*. Other bits describe the
// schema system's own bookkeeping.
constexpr std::pair<uint32_t, std::string_view> kClassFlags[] = {
    {1u << 1, "abstract"},
    {1u << 2, "trivial_constructor"},
    {1u << 3, "trivial_destructor"},
    {1u << 9, "construct_disallowed"},
    {1u << 10, "MNetworkAssumeNotNetworkable"},
    {1u << 11, "MNetworkNoBase"},
    {1u << 12, "MIgnoreTypeScopeMetaChecks"},
    {1u << 13, "MDisableDataDescValidation"},
    {1u << 14, "MClassHasEntityLimitedDataDesc"},
    {1u << 15, "MClassHasCustomAlignedNewDelete"},
    {1u << 16, "MNonConstructibleClassBase"},
    {1u << 17, "MConstructibleClassBase"},
    {1u << 18, "MHasKV3TransferPolymorphicClassname"},
};

// The console flag names (FCVAR_*), indexed by bit.
constexpr std::string_view kConsoleFlags[] = {
    "linked_concommand",
    "developmentonly",
    "gamedll",
    "clientdll",
    "hidden",
    "protected",
    "sponly",
    "archive",
    "notify",
    "userinfo",
    "reference",
    "unlogged",
    "initial_setvalue",
    "replicated",
    "cheat",
    "per_user",
    "demo",
    "dontrecord",
    "performing_callbacks",
    "release",
    "menubar_item",
    "commandline_enforced",
    "notconnected",
    "vconsole_fuzzy_matching",
    "server_can_execute",
    "client_can_execute",
    "server_cannot_query",
    "vconsole_set_focus",
    "clientcmd_can_execute",
    "execute_per_tick",
    "snapshot_ignored",
    "",
    "defensive",
    "execute_immediately",
    "gameinfo_cannot_override",
    "",
    "",
    "enum_value",
};

// Load copies a T from bytes, which need not be aligned.
template <typename T>
T Load(const uint8_t* bytes) {
  T value;
  std::memcpy(&value, bytes, sizeof(value));
  return value;
}

// Image is one loaded game module.
struct Image {
  // file is the module's file name, such as server.dll.
  std::string file;
  std::span<const uint8_t> bytes;

  bool Contains(const void* pointer) const {
    const auto address = reinterpret_cast<uintptr_t>(pointer);
    const auto base = reinterpret_cast<uintptr_t>(bytes.data());
    return address >= base && address < base + bytes.size();
  }

  // Name returns the printable string at text when it lies in this image,
  // or nothing. Schema and entity records keep their names beside them.
  std::string_view Name(const char* text) const {
    if (!Contains(text)) return {};
    const auto* end = reinterpret_cast<const char*>(bytes.data() + bytes.size());
    for (const char* at = text; at < end && at - text < 512; ++at) {
      if (*at == '\0') return {text, size_t(at - text)};
      if (*at < 0x20 || *at > 0x7e) return {};
    }
    return {};
  }

  // FileHeader returns the PE file header, which the PE32+ optional header
  // follows, or null when the image has none.
  const uint8_t* FileHeader() const {
    if (bytes.size() < 0x40) return nullptr;
    const auto offset = size_t(Load<uint32_t>(bytes.data() + 0x3c)) + 4;
    return offset + 20 + 240 <= bytes.size() ? bytes.data() + offset : nullptr;
  }

  // Export returns the address of the named export, or zero. The loader has
  // validated the export directory of a mapped image.
  uintptr_t Export(std::string_view name) const {
    // Find the export directory through the optional header's data directory.
    const auto* file_header = FileHeader();
    if (!file_header) return 0;
    const auto directory = Load<uint32_t>(file_header + 20 + 112);
    if (!directory || size_t(directory) + 40 > bytes.size()) return 0;

    // Match the name, then map its ordinal to the function's address.
    const auto* exports = bytes.data() + directory;
    const auto* functions = bytes.data() + Load<uint32_t>(exports + 28);
    const auto* names = bytes.data() + Load<uint32_t>(exports + 32);
    const auto* ordinals = bytes.data() + Load<uint32_t>(exports + 36);
    for (uint32_t i = 0, count = Load<uint32_t>(exports + 24); i < count; ++i) {
      const auto* text =
          reinterpret_cast<const char*>(bytes.data() + Load<uint32_t>(names + 4 * i));
      if (Name(text) != name) continue;
      const auto ordinal = Load<uint16_t>(ordinals + 2 * i);
      return reinterpret_cast<uintptr_t>(bytes.data() + Load<uint32_t>(functions + 4 * ordinal));
    }
    return 0;
  }

  // WritableSections returns the PE sections a module's static records live
  // in once their constructors have run.
  std::vector<std::span<const uint8_t>> WritableSections() const {
    std::vector<std::span<const uint8_t>> sections;
    const auto* file_header = FileHeader();
    if (!file_header) return sections;
    const auto count = Load<uint16_t>(file_header + 2);
    const auto* section = file_header + 20 + Load<uint16_t>(file_header + 16);
    for (uint16_t i = 0; i < count; ++i, section += 40) {
      if (section + 40 > bytes.data() + bytes.size()) break;
      const auto size = Load<uint32_t>(section + 8);
      const auto address = Load<uint32_t>(section + 12);
      const bool writable = Load<uint32_t>(section + 36) & 0x80000000u;
      if (writable && size_t(address) + size <= bytes.size())
        sections.push_back(bytes.subspan(address, size));
    }
    return sections;
  }
};

// LoadedModules returns the game modules the server has mapped.
std::vector<Image> LoadedModules(const GamePaths& paths) {
  std::vector<Image> images;
  for (const auto& directory : {paths.engine2_dll.parent_path(), paths.server_dll.parent_path()}) {
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(directory, error)) {
      if (entry.path().extension() != ".dll") continue;
      const auto file = entry.path().filename();
      auto image = gameinterop::MappedModuleImage::ForModule(file.wstring());
      if (!image) continue;
      if (std::ranges::any_of(images, [&](const Image& seen) { return seen.file == file; }))
        continue;
      images.push_back({file.string(), image->image_bytes()});
    }
  }
  return images;
}

// SchemaRecords returns the class and enum records a module declares. Both
// begin with a pointer to themselves, their name and their project; the
// module's type scope confirms each candidate by finding it by name.
std::pair<std::vector<const schema::ClassInfo*>, std::vector<const schema::EnumInfo*>>
SchemaRecords(void* schema_system, const Image& image) {
  std::pair<std::vector<const schema::ClassInfo*>, std::vector<const schema::EnumInfo*>> records;
  auto scope = gameinterop::FindTypeScopeOf(schema_system, image.file.c_str());
  if (!scope) return records;
  for (const auto section : image.WritableSections()) {
    for (size_t offset = 0; offset + sizeof(schema::EnumInfo) <= section.size(); offset += 8) {
      const auto* record = section.data() + offset;
      if (Load<const void*>(record) != record) continue;
      const auto* info = reinterpret_cast<const schema::ClassInfo*>(record);
      if (image.Name(info->name).empty() || image.Name(info->project).empty()) continue;
      if (auto found = gameinterop::FindDeclaredClassOf(*scope, info->name);
          found && *found == info)
        records.first.push_back(info);
      else if (auto found = gameinterop::FindDeclaredEnumOf(*scope, info->name);
               found && *found == record)
        records.second.push_back(reinterpret_cast<const schema::EnumInfo*>(record));
    }
  }
  return records;
}

// Text copies a game string that may be null.
std::string Text(const char* text) { return text ? text : ""; }

// AddMetadata adds the names of a schema record's metadata entries.
template <typename Repeated>
void AddMetadata(const schema::MetadataEntry* entries, int32_t count, Repeated* names) {
  for (int32_t i = 0; i < count; ++i) names->Add(Text(entries[i].name));
}

// ByModuleAndName orders schema records by declaring module, then name.
template <typename Record>
bool ByModuleAndName(const Record* a, const Record* b) {
  return std::tuple(std::string_view(a->project), std::string_view(a->name)) <
         std::tuple(std::string_view(b->project), std::string_view(b->name));
}

// AddClass describes a schema class: its layout, bases, fields and flags.
void AddClass(const schema::ClassInfo& info, dump::SchemaClass& out) {
  // Copy the class's identity and layout.
  out.set_name(info.name);
  out.set_module(info.project);
  out.set_size(static_cast<uint32_t>(info.size));
  out.set_alignment(info.alignment);

  // List its bases and fields in declaration order.
  for (const auto& base : std::span(info.bases, info.base_count)) {
    auto& added = *out.add_bases();
    added.set_name(Text(base.info->name));
    added.set_module(Text(base.info->project));
    added.set_offset(base.offset);
  }
  for (const auto& field : std::span(info.fields, info.field_count)) {
    auto& added = *out.add_fields();
    added.set_name(Text(field.name));
    added.set_type(field.type ? Text(field.type->name) : "");
    added.set_offset(static_cast<uint32_t>(field.offset));
    AddMetadata(field.metadata, field.metadata_count, added.mutable_metadata());
  }

  // Name its metadata and the flags a dump reports.
  AddMetadata(info.metadata, info.metadata_count, out.mutable_metadata());
  for (const auto& [bit, name] : kClassFlags) {
    if (info.flags & bit) out.add_flags(std::string(name));
  }
}

// AddEnum describes a schema enum and its enumerators.
void AddEnum(const schema::EnumInfo& info, dump::SchemaEnum& out) {
  out.set_name(info.name);
  out.set_module(info.project);
  out.set_size(info.size);
  for (const auto& enumerator : std::span(info.enumerators, info.enumerator_count)) {
    auto& added = *out.add_enumerators();
    added.set_name(Text(enumerator.name));
    added.set_value(enumerator.value);
  }
  AddMetadata(info.metadata, info.metadata_count, out.mutable_metadata());
}

// EntityClasses returns the server's entity class records, one per designer
// name: static records whose schema binding is a server class of the same
// name.
std::vector<const EntityClassInfo*> EntityClasses(
    const Image& server, const std::unordered_set<const void*>& server_classes) {
  std::vector<const EntityClassInfo*> classes;
  std::unordered_set<std::string_view> designer_names;
  for (const auto section : server.WritableSections()) {
    for (size_t offset = 0; offset + sizeof(EntityClassInfo) <= section.size(); offset += 8) {
      const auto* info = reinterpret_cast<const EntityClassInfo*>(section.data() + offset);
      if (!server_classes.contains(info->schema_binding)) continue;
      const auto class_name = server.Name(info->class_name);
      const auto designer_name = server.Name(info->designer_name);
      if (designer_name.empty() || class_name != info->schema_binding->name) continue;
      if (designer_names.insert(designer_name).second) classes.push_back(info);
    }
  }
  std::ranges::sort(classes, {}, [](const EntityClassInfo* info) {
    return std::string_view(info->designer_name);
  });
  return classes;
}

// CollectDataMaps adds map, its base chain and its embedded maps by name.
void CollectDataMaps(const DataMap* map, std::map<std::string_view, const DataMap*>& maps) {
  for (; map != nullptr && map->name != nullptr; map = map->base) {
    if (!maps.emplace(map->name, map).second) return;
    for (const auto& field : std::span(map->fields, size_t(std::max(map->field_count, 0)))) {
      if (field.type == kEmbeddedType) CollectDataMaps(field.embedded, maps);
    }
  }
}

// AddDataMap describes a data map's spawn key values and embedded maps.
void AddDataMap(const DataMap& map, dump::DataMap& out) {
  out.set_name(map.name);
  if (map.base && map.base->name) out.set_base(map.base->name);
  for (const auto& field : std::span(map.fields, size_t(std::max(map.field_count, 0)))) {
    if (field.flags & kKeyFlag) {
      auto& key = *out.add_key_values();
      key.set_name(Text(field.external_name));
      key.set_field(Text(field.name));
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
  constexpr std::string_view kWords[] = {"-nan", "nan", "-inf", "inf"};
  std::string out;
  out.reserve(json.size());
  bool in_string = false;
  for (size_t i = 0; i < json.size(); ++i) {
    const char c = json[i];
    if (in_string) {
      out += c;
      if (c == '\\' && i + 1 < json.size())
        out += json[++i];
      else if (c == '"')
        in_string = false;
      continue;
    }
    const auto word = std::ranges::find_if(
        kWords, [rest = json.substr(i)](std::string_view word) { return rest.starts_with(word); });
    if (word != std::end(kWords)) {
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
  if (!extract || !save) return std::unexpected("module metadata exports are missing");

  // Build the metadata and write it as JSON.
  UtlString info;
  const void* metadata = extract(info);
  if (!metadata)
    return std::unexpected(module.file + " metadata failed to build: " + Text(info.text));
  UtlString error;
  UtlString json;
  if (!save(metadata, &error, &json) || !json.text)
    return std::unexpected("cannot write " + module.file + " metadata: " + Text(error.text));

  // Parse the JSON once its non-finite numbers are quoted.
  google::protobuf::Struct parsed;
  if (auto status = google::protobuf::json::JsonStringToMessage(QuoteNonFinite(json.text), &parsed);
      !status.ok())
    return std::unexpected("cannot read " + module.file +
                           " metadata: " + std::string(status.message()));
  return parsed;
}

// Member returns the named member of an object value, or null.
const google::protobuf::Value* Member(const google::protobuf::Value* value,
                                      const std::string& name) {
  if (!value || !value->has_struct_value()) return nullptr;
  const auto& fields = value->struct_value().fields();
  const auto found = fields.find(name);
  return found == fields.end() ? nullptr : &found->second;
}

// String returns a string value, or an empty string for any other value.
std::string String(const google::protobuf::Value* value) {
  return value && value->has_string_value() ? value->string_value() : "";
}

// Bool reports whether value is true.
bool Bool(const google::protobuf::Value* value) { return value && value->bool_value(); }

// AddParameters adds the members of a Pulse parameter list except the target
// entity, sorted by name since the parsed object keeps no order.
template <typename Repeated>
void AddParameters(const google::protobuf::Value* list, const std::string& target, Repeated* out) {
  if (!list || !list->has_struct_value()) return;
  for (const auto& [name, parameter] : list->struct_value().fields()) {
    if (name == target) continue;
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
  if (!bindings || !bindings->has_struct_value()) return;

  // Add each binding that targets an entity as an input or an output.
  constexpr std::string_view kEntityHandle = "PVAL_EHANDLE";
  for (const auto& [key, binding] : bindings->struct_value().fields()) {
    const auto* meta = Member(&binding, "m_MetaData");
    const bool input = Bool(Member(meta, "is_pulse_target_method"));
    if (!input && !Bool(Member(meta, "is_pulse_target_output"))) continue;
    const auto target = String(Member(meta, "target_arg_name"));
    const auto* parameters = Member(meta, input ? "pulse_inparams" : "pulse_outparams");
    const auto target_type = String(Member(Member(parameters, target), "type"));
    std::string_view type = target_type;
    if (!type.starts_with(kEntityHandle)) continue;
    type.remove_prefix(kEntityHandle.size());
    if (!type.empty() && !type.starts_with(':')) continue;
    const auto entity = std::string(type.empty() ? type : type.substr(1));
    const auto name = key.substr(key.rfind(':') + 1);
    const auto description = String(Member(&binding, "m_Description"));
    if (input) {
      auto& added = *entities.add_inputs();
      added.set_entity(entity);
      added.set_name(name);
      added.set_description(description);
      AddParameters(parameters, target, added.mutable_parameters());
      AddParameters(Member(meta, "pulse_outparams"), target, added.mutable_returns());
    } else {
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

// AddConsoleFlags names each set console flag bit, as flag_N when the game
// gives it no name.
template <typename Repeated>
void AddConsoleFlags(uint64_t flags, Repeated* names) {
  for (size_t bit = 0; bit < 64; ++bit) {
    if (!(flags & (1ull << bit))) continue;
    if (bit < std::size(kConsoleFlags) && !kConsoleFlags[bit].empty())
      names->Add(std::string(kConsoleFlags[bit]));
    else
      names->Add("flag_" + std::to_string(bit));
  }
}

// Build reads the install's ClientVersion from steam.inf.
std::string Build(const GamePaths& paths) {
  std::ifstream in(paths.server_dll.parent_path().parent_path().parent_path() / "steam.inf");
  std::string line;
  while (std::getline(in, line)) {
    if (line.starts_with("ClientVersion=")) return line.substr(14);
  }
  return "unknown";
}

// WriteJson writes message to path as indented JSON with proto field names.
std::expected<void, std::string> WriteJson(const google::protobuf::Message& message,
                                           const fs::path& path) {
  std::string text;
  google::protobuf::json::PrintOptions options;
  options.add_whitespace = true;
  options.preserve_proto_field_names = true;
  if (auto printed = google::protobuf::json::MessageToJsonString(message, &text, options);
      !printed.ok())
    return std::unexpected(path.filename().string() + ": " + std::string(printed.message()));
  std::ofstream out(path, std::ios::binary);
  out << text;
  if (!out) return std::unexpected("cannot write " + path.string());
  return {};
}

}  // namespace

std::expected<GameDumpCounts, std::string> WriteGameDump(const GamePaths& paths,
                                                         const fs::path& directory) {
  // Resolve the schema system and the console, and make the directory.
  auto schema_system = gameinterop::ResolveSchemaSystem();
  if (!schema_system) return std::unexpected(schema_system.error());
  auto console = gameinterop::ConsoleVariables::Resolve();
  if (!console) return std::unexpected(console.error());
  std::error_code error;
  fs::create_directories(directory, error);
  if (error) return std::unexpected("cannot create " + directory.string() + ": " + error.message());

  const auto build = Build(paths);
  GameDumpCounts counts;

  // Gather the schemas of every loaded module that declares one.
  std::vector<const schema::ClassInfo*> classes;
  std::vector<const schema::EnumInfo*> enums;
  std::unordered_set<const void*> server_classes;
  const Image* server = nullptr;
  const Image* tier0 = nullptr;
  const auto images = LoadedModules(paths);
  for (const auto& image : images) {
    auto [module_classes, module_enums] = SchemaRecords(*schema_system, image);
    if (image.file == paths.server_dll.filename()) {
      server = &image;
      server_classes.insert(module_classes.begin(), module_classes.end());
    }
    if (image.file == "tier0.dll") tier0 = &image;
    classes.insert(classes.end(), module_classes.begin(), module_classes.end());
    enums.insert(enums.end(), module_enums.begin(), module_enums.end());
  }
  if (!server || !tier0) return std::unexpected("server.dll or tier0.dll is not loaded");

  // Write the schemas ordered by module and name.
  std::ranges::sort(classes, ByModuleAndName<schema::ClassInfo>);
  std::ranges::sort(enums, ByModuleAndName<schema::EnumInfo>);
  dump::Schemas schemas;
  schemas.set_build(build);
  for (const auto* info : classes) AddClass(*info, *schemas.add_classes());
  for (const auto* info : enums) AddEnum(*info, *schemas.add_enums());
  counts.classes = classes.size();
  counts.enums = enums.size();
  if (auto written = WriteJson(schemas, directory / "schemas.json"); !written)
    return std::unexpected(written.error());

  // Describe the entity classes and the data maps they reach.
  dump::Entities entities;
  entities.set_build(build);
  std::map<std::string_view, const DataMap*> data_maps;
  const auto entity_classes = EntityClasses(*server, server_classes);
  const std::unordered_set<const EntityClassInfo*> listed(entity_classes.begin(),
                                                          entity_classes.end());
  for (const auto* info : entity_classes) {
    auto& added = *entities.add_classes();
    added.set_designer_name(info->designer_name);
    added.set_class_name(info->class_name);
    if (server->Contains(info->description)) added.set_description(info->description);
    if (info->data_map && info->data_map->name) added.set_data_map(info->data_map->name);
    // Abstract records without a designer name sit between listed classes.
    const auto* base = info->base;
    while (base && !listed.contains(base)) base = base->base;
    if (base) added.set_base(base->designer_name);
    CollectDataMaps(info->data_map, data_maps);
  }
  for (const auto& [name, map] : data_maps) AddDataMap(*map, *entities.add_data_maps());

  // Add the inputs and outputs from the server's Pulse bindings and write the
  // entities.
  auto metadata = ModuleMetadata(*server, *tier0);
  if (!metadata) return std::unexpected(metadata.error());
  AddPulseBindings(*metadata, entities);
  counts.designer_names = size_t(entities.classes_size());
  counts.data_maps = data_maps.size();
  counts.inputs = size_t(entities.inputs_size());
  counts.outputs = size_t(entities.outputs_size());
  if (auto written = WriteJson(entities, directory / "entities.json"); !written)
    return std::unexpected(written.error());

  // Write the console variables and commands ordered by name.
  auto variables = console->Variables();
  if (!variables) return std::unexpected(variables.error());
  auto commands = console->Commands();
  if (!commands) return std::unexpected(commands.error());
  std::ranges::sort(*variables, {}, &gameinterop::ConsoleVariableInfo::name);
  std::ranges::sort(*commands, {}, &gameinterop::ConsoleCommandInfo::name);
  dump::Console listing;
  listing.set_build(build);
  for (const auto& variable : *variables) {
    auto& added = *listing.add_variables();
    added.set_name(variable.name);
    added.set_type(static_cast<dump::ConsoleVariableType>(variable.type + 1));
    added.set_default_value(variable.default_value);
    added.set_min(variable.min);
    added.set_max(variable.max);
    added.set_help(variable.help);
    AddConsoleFlags(variable.flags, added.mutable_flags());
  }
  for (const auto& command : *commands) {
    auto& added = *listing.add_commands();
    added.set_name(command.name);
    added.set_help(command.help);
    AddConsoleFlags(command.flags, added.mutable_flags());
  }
  counts.variables = variables->size();
  counts.commands = commands->size();
  if (auto written = WriteJson(listing, directory / "console.json"); !written)
    return std::unexpected(written.error());
  return counts;
}

}  // namespace modlock::host_app
