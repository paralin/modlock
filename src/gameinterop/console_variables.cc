#include "modlock/gameinterop/console_variables.h"

#include <charconv>
#include <cstdint>
#include <cstring>

#include "modlock/gameinterop/mapped_module_image.h"

namespace modlock::gameinterop {
namespace {

// Registry describes one of CCvar's CUtlLinkedList registries. Its
// CUtlLeanVector storage holds a 15-bit capacity, the entry array and the head
// index; each entry links to its neighbors by 16-bit indices.
struct Registry {
  size_t capacity;
  size_t entries;
  size_t head;
  size_t stride;
  // links is the entry offset of the previous index; the next index follows.
  size_t links;
  // indirect entries hold a pointer to their data; others hold it inline.
  bool indirect;
};

// Since game build 6711 ICvar stores the split-screen slot count after its
// vtable. tier0.dll's variable walkers read a registry of ConVarData pointers;
// FindFirstConCommand and FindNextConCommand read a registry of inline
// ConCommandData. Public iteration and FindConVar filter DEVELOPMENTONLY
// entries, so discovery walks the registries itself.
constexpr Registry kVariables{0x4a, 0x50, 0x58, 16, 8, true};
constexpr Registry kCommands{0x102, 0x108, 0x110, 0x38, 0x30, false};
constexpr uint16_t kInvalidIndex = 0xffff;
constexpr uint64_t kExposureFlags = (1ull << 1) | (1ull << 4) | (1ull << 32);
constexpr uint64_t kCheatFlag = 1ull << 14;

// ConVarData x64 layout from sourcesdk public/tier1/convar.h. Every entry
// starts with its name.
constexpr size_t kDefaultOffset = 0x08;
constexpr size_t kMinOffset = 0x10;
constexpr size_t kMaxOffset = 0x18;
constexpr size_t kHelpOffset = 0x20;
constexpr size_t kTypeOffset = 0x28;
constexpr size_t kFlagsOffset = 0x30;
constexpr size_t kValueOffset = 0x58;

// ConCommandData x64 layout.
constexpr size_t kCommandHelpOffset = 0x08;
constexpr size_t kCommandFlagsOffset = 0x10;

// EConVarType values the typed accessors check.
constexpr int16_t kBoolType = 0;
constexpr int16_t kInt32Type = 3;
constexpr int16_t kFloat32Type = 7;

// Load copies the element at index of type T from bytes.
template <typename T>
T Load(const std::byte* bytes, size_t index = 0) {
  T value;
  std::memcpy(&value, bytes + index * sizeof(T), sizeof(T));
  return value;
}

// Text copies a game string that may be null.
std::string Text(const char* text) { return text ? text : ""; }

// Walk calls visit with each entry's data in registry order. It returns the
// first entry visit accepts, or null after the last entry.
template <typename Visit>
std::expected<std::byte*, std::string> Walk(const void* instance, const Registry& registry,
                                            Visit visit) {
  // Read the registry's capacity, entry array and head.
  if (!instance) return std::unexpected("console variables: interface is unavailable");
  const auto* base = static_cast<const std::byte*>(instance);
  const auto capacity = static_cast<uint16_t>(Load<uint16_t>(base + registry.capacity) & 0x7fff);
  const auto* entries = Load<const std::byte*>(base + registry.entries);
  auto index = Load<uint16_t>(base + registry.head);

  // Follow the links, checking each entry's bounds and back link.
  uint16_t previous = kInvalidIndex;
  for (size_t visited = 0; index != kInvalidIndex; ++visited) {
    if (!entries || index >= capacity || visited >= capacity)
      return std::unexpected("console variables: invalid registry bounds or cycle");
    const auto* entry = entries + size_t(index) * registry.stride;
    auto* data = registry.indirect ? Load<std::byte*>(entry) : const_cast<std::byte*>(entry);
    if (Load<uint16_t>(entry + registry.links) != previous || !data || !Load<const char*>(data))
      return std::unexpected("console variables: invalid registry link or data");
    if (visit(data)) return data;
    previous = index;
    index = Load<uint16_t>(entry + registry.links + 2);
  }
  return nullptr;
}

// Decimal writes a floating-point value in its shortest exact decimal form.
template <typename T>
std::string Decimal(T value) {
  char text[64];
  const auto written = std::to_chars(text, text + sizeof(text), value, std::chars_format::fixed);
  return std::string(text, written.ptr);
}

// Floats prints count floats as a bracketed list.
std::string Floats(const std::byte* value, size_t count) {
  std::string text = "[";
  for (size_t i = 0; i < count; ++i) {
    if (i) text += ", ";
    text += Decimal(Load<float>(value, i));
  }
  return text + "]";
}

// FormatValue prints a CVValue_t of the given EConVarType as the console
// does, or nothing when the value is absent.
std::string FormatValue(int16_t type, const std::byte* value) {
  if (!value) return {};
  switch (type) {
    case 0:
      return Load<uint8_t>(value) ? "true" : "false";
    case 1:
      return std::to_string(Load<int16_t>(value));
    case 2:
      return std::to_string(Load<uint16_t>(value));
    case 3:
      return std::to_string(Load<int32_t>(value));
    case 4:
      return std::to_string(Load<uint32_t>(value));
    case 5:
      return std::to_string(Load<int64_t>(value));
    case 6:
      return std::to_string(Load<uint64_t>(value));
    case 7:
      return Decimal(Load<float>(value));
    case 8:
      return Decimal(Load<double>(value));
    case 9:
      return Text(Load<const char*>(value));
    case 10: {
      std::string text = "[";
      for (size_t i = 0; i < 4; ++i) {
        if (i) text += ", ";
        text += std::to_string(Load<uint8_t>(value, i));
      }
      return text + "]";
    }
    case 11:
      return Floats(value, 2);
    case 12:
    case 14:
    case 15:
      return Floats(value, 3);
    case 13:
      return Floats(value, 4);
    default:
      return {};
  }
}

}  // namespace

std::expected<ConsoleVariables, std::string> ConsoleVariables::Resolve() {
  const auto instance = ResolveEngineInterface(L"tier0.dll", "VEngineCvar007");
  if (!instance) return std::unexpected(instance.error());
  return Bind(*instance);
}

std::expected<ConsoleVariables, std::string> ConsoleVariables::Bind(void* instance) {
  if (!instance) return std::unexpected("console variables: interface is null");
  ConsoleVariables variables;
  variables.instance_ = instance;
  return variables;
}

std::expected<std::byte*, std::string> ConsoleVariables::Find(const char* name) const {
  if (!name || !*name) return std::unexpected("console variables: empty name");
  auto data = Walk(instance_, kVariables, [name](const std::byte* data) {
    return std::strcmp(Load<const char*>(data), name) == 0;
  });
  if (data && !*data) return std::unexpected(std::string("console variable not found: ") + name);
  return data;
}

std::expected<std::byte*, std::string> ConsoleVariables::Value(const char* name, int16_t type,
                                                               const char* kind) const {
  auto data = Find(name);
  if (!data) return std::unexpected(data.error());
  if (Load<int16_t>(*data + kTypeOffset) != type)
    return std::unexpected(std::string("console variable is not ") + kind + ": " + name);
  return *data + kValueOffset;
}

std::expected<void, std::string> ConsoleVariables::ClearFlags(const char* name,
                                                              uint64_t flags) const {
  auto data = Find(name);
  if (!data) return std::unexpected(data.error());
  const auto kept = Load<uint64_t>(*data + kFlagsOffset) & ~flags;
  std::memcpy(*data + kFlagsOffset, &kept, sizeof(kept));
  return {};
}

std::expected<void, std::string> ConsoleVariables::Expose(const char* name) const {
  return ClearFlags(name, kExposureFlags);
}

std::expected<void, std::string> ConsoleVariables::AllowServerChanges(const char* name) const {
  return ClearFlags(name, kExposureFlags | kCheatFlag);
}

std::expected<bool, std::string> ConsoleVariables::ReadBool(const char* name) const {
  auto value = Value(name, kBoolType, "Boolean");
  if (!value) return std::unexpected(value.error());
  return Load<uint8_t>(*value) != 0;
}

std::expected<float, std::string> ConsoleVariables::ReadFloat(const char* name) const {
  auto value = Value(name, kFloat32Type, "a float");
  if (!value) return std::unexpected(value.error());
  return Load<float>(*value);
}

std::expected<void, std::string> ConsoleVariables::SetBool(const char* name, bool value) const {
  auto slot = Value(name, kBoolType, "Boolean");
  if (!slot) return std::unexpected(slot.error());
  **slot = std::byte{value};
  return {};
}

std::expected<void, std::string> ConsoleVariables::SetFloat(const char* name, float value) const {
  auto slot = Value(name, kFloat32Type, "a float");
  if (!slot) return std::unexpected(slot.error());
  std::memcpy(*slot, &value, sizeof(value));
  return {};
}

std::expected<void, std::string> ConsoleVariables::SetInt32(const char* name, int32_t value) const {
  auto slot = Value(name, kInt32Type, "a 32-bit integer");
  if (!slot) return std::unexpected(slot.error());
  std::memcpy(*slot, &value, sizeof(value));
  return {};
}

std::expected<std::vector<ConsoleVariableInfo>, std::string> ConsoleVariables::Variables() const {
  std::vector<ConsoleVariableInfo> variables;
  auto walked = Walk(instance_, kVariables, [&variables](const std::byte* data) {
    const auto type = Load<int16_t>(data + kTypeOffset);
    variables.push_back({
        .name = Load<const char*>(data),
        .help = Text(Load<const char*>(data + kHelpOffset)),
        .flags = Load<uint64_t>(data + kFlagsOffset),
        .type = type,
        .default_value = FormatValue(type, Load<const std::byte*>(data + kDefaultOffset)),
        .min = FormatValue(type, Load<const std::byte*>(data + kMinOffset)),
        .max = FormatValue(type, Load<const std::byte*>(data + kMaxOffset)),
    });
    return false;
  });
  if (!walked) return std::unexpected(walked.error());
  return variables;
}

std::expected<std::vector<ConsoleCommandInfo>, std::string> ConsoleVariables::Commands() const {
  std::vector<ConsoleCommandInfo> commands;
  auto walked = Walk(instance_, kCommands, [&commands](const std::byte* data) {
    commands.push_back({
        .name = Load<const char*>(data),
        .help = Text(Load<const char*>(data + kCommandHelpOffset)),
        .flags = Load<uint64_t>(data + kCommandFlagsOffset),
    });
    return false;
  });
  if (!walked) return std::unexpected(walked.error());
  return commands;
}

}  // namespace modlock::gameinterop
