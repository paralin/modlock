#include "modlock/gameinterop/modifier_states.h"

#include <cstring>
#include <optional>

#include "modlock/gameinterop/mapped_module_image.h"

namespace modlock::gameinterop {
namespace {

// kMaxModifierState bounds a plausible enumerator value; the native state
// mask holds a few hundred bits.
constexpr int64_t kMaxModifierState = 4096;

}  // namespace

std::expected<uint32_t, std::string> ModifierStateIndex(const ModuleImage& image,
                                                        std::string_view name) {
  const auto bytes = image.image_bytes();
  const std::string_view view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  // The name is a whole NUL-terminated string, not a prefix of a longer one.
  std::string needle;
  needle.reserve(name.size() + 2);
  needle.push_back('\0');
  needle.append(name);
  needle.push_back('\0');
  const auto at = view.find(needle);
  if (at == std::string_view::npos)
    return std::unexpected("modifier state " + std::string(name) + " is not in the game module");
  const uint64_t address = static_cast<uint64_t>(image.base()) + at + 1;
  char pointer[sizeof(address)];
  std::memcpy(pointer, &address, sizeof(address));
  const std::string_view pointer_view(pointer, sizeof(pointer));

  // Every enumerator record naming this string must agree on one value.
  std::optional<int64_t> value;
  for (auto hit = view.find(pointer_view); hit != std::string_view::npos;
       hit = view.find(pointer_view, hit + 1)) {
    if (hit % alignof(uint64_t) != 0 || hit + 2 * sizeof(uint64_t) > view.size()) continue;
    int64_t candidate = 0;
    std::memcpy(&candidate, view.data() + hit + sizeof(uint64_t), sizeof(candidate));
    if (candidate < 0 || candidate >= kMaxModifierState) continue;
    if (value && *value != candidate)
      return std::unexpected("modifier state " + std::string(name) + " has conflicting values");
    value = candidate;
  }
  if (!value)
    return std::unexpected("modifier state " + std::string(name) + " has no enumerator record");
  return static_cast<uint32_t>(*value);
}

std::expected<std::vector<uint32_t>, std::string> ModifierStateIndices(
    const ModuleImage& image, std::span<const std::string_view> names) {
  std::vector<uint32_t> out;
  out.reserve(names.size());
  for (const auto name : names) {
    auto index = ModifierStateIndex(image, name);
    if (!index) return std::unexpected(index.error());
    out.push_back(*index);
  }
  return out;
}

std::expected<std::vector<uint32_t>, std::string> ServerModifierStates(
    std::span<const std::string_view> names) {
  auto image = MappedModuleImage::ForModule(L"server.dll");
  if (!image) return std::unexpected(image.error());
  return ModifierStateIndices(*image, names);
}

}  // namespace modlock::gameinterop
