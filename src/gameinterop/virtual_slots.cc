#include "modlock/gameinterop/virtual_slots.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <optional>

#include "modlock/gameinterop/mapped_module_image.h"
#include "modlock/gameinterop/signature.h"

namespace modlock::gameinterop {
namespace {

// kMaxEntries bounds a table walk; the largest game table holds a few hundred.
constexpr size_t kMaxEntries = 4096;

// kExecutable is IMAGE_SCN_MEM_EXECUTE in a PE section's characteristics.
constexpr uint32_t kExecutable = 0x20000000;

template <typename T>
std::optional<T> Read(std::span<const uint8_t> bytes, size_t at) {
  if (at > bytes.size() || bytes.size() - at < sizeof(T)) {
    return std::nullopt;
  }
  T value;
  std::memcpy(&value, bytes.data() + at, sizeof(T));
  return value;
}

// Find returns the offset of needle's only occurrence in bytes.
std::expected<size_t, std::string> Find(std::span<const uint8_t> bytes,
                                        std::span<const uint8_t> needle, std::string_view what) {
  // The needle must appear exactly once.
  const std::boyer_moore_horspool_searcher searcher(needle.begin(), needle.end());
  const auto first = std::search(bytes.begin(), bytes.end(), searcher);
  if (first == bytes.end()) {
    return std::unexpected(std::string(what) + " not found");
  }
  if (std::search(first + 1, bytes.end(), searcher) != bytes.end()) {
    return std::unexpected(std::string(what) + " found more than once");
  }
  return static_cast<size_t>(first - bytes.begin());
}

// Executable reports whether offset falls in a section the PE headers mark
// executable.
bool Executable(std::span<const uint8_t> bytes, size_t offset) {
  // Find the section table.
  const auto pe = Read<uint32_t>(bytes, 0x3C);
  if (!pe) {
    return false;
  }
  const auto count = Read<uint16_t>(bytes, *pe + 6);
  const auto optional_size = Read<uint16_t>(bytes, *pe + 20);
  if (!count || !optional_size) {
    return false;
  }
  const size_t sections = *pe + 24 + *optional_size;

  // The offset is executable when the section holding it is.
  for (size_t i = 0; i < *count; ++i) {
    const size_t header = sections + i * 40;
    const auto size = Read<uint32_t>(bytes, header + 8);
    const auto address = Read<uint32_t>(bytes, header + 12);
    const auto flags = Read<uint32_t>(bytes, header + 36);
    if (!size || !address || !flags) {
      return false;
    }
    if (offset >= *address && offset - *address < *size) {
      return (*flags & kExecutable) != 0;
    }
  }
  return false;
}

// Bytes views value's object representation.
template <typename T>
std::span<const uint8_t> Bytes(const T& value) {
  return {reinterpret_cast<const uint8_t*>(&value), sizeof(value)};
}

}  // namespace

std::expected<size_t, std::string> FindVtable(const ModuleImage& image,
                                              std::string_view class_name) {
  // Read the image and name the class for errors.
  const auto bytes = image.image_bytes();
  const std::string named = "class " + std::string(class_name);

  // The type descriptor holds a vtable pointer and a spare word before its
  // NUL-terminated decorated name.
  const std::string decorated = ".?AV" + std::string(class_name) + "@@";
  const auto name =
      Find(bytes, {reinterpret_cast<const uint8_t*>(decorated.c_str()), decorated.size() + 1},
           named + " type descriptor");
  if (!name) {
    return std::unexpected(name.error());
  }
  const auto descriptor = static_cast<uint32_t>(*name - 16);

  // The primary table's locator has signature 1, offset zero, no constructor
  // displacement, and the descriptor's image offset.
  const uint32_t locator_head[] = {1, 0, 0, descriptor};
  const auto locator = Find(bytes, Bytes(locator_head), named + " object locator");
  if (!locator) {
    return std::unexpected(locator.error());
  }

  // The table starts one pointer past the stored pointer to its locator.
  const uint64_t locator_pointer = image.pointer_base() + *locator;
  const auto table = Find(bytes, Bytes(locator_pointer), named + " dispatch table");
  if (!table) {
    return std::unexpected(table.error());
  }
  return *table + sizeof(uint64_t);
}

std::vector<size_t> VtableEntries(const ModuleImage& image, size_t table) {
  const auto bytes = image.image_bytes();
  std::vector<size_t> entries;
  while (entries.size() < kMaxEntries) {
    const auto pointer = Read<uint64_t>(bytes, table + entries.size() * sizeof(uint64_t));
    if (!pointer || *pointer < image.pointer_base()) {
      break;
    }
    const size_t function = *pointer - image.pointer_base();
    if (!Executable(bytes, function)) {
      break;
    }
    entries.push_back(function);
  }
  return entries;
}

std::expected<std::vector<size_t>, std::string> MatchingSlots(const ModuleImage& image,
                                                              size_t table,
                                                              const VirtualSlot& slot) {
  // Parse the slot's pattern.
  const auto pattern = ParseSignature(std::string(slot.id), slot.pattern);
  if (!pattern) {
    return std::unexpected(pattern.error());
  }

  // Match the pattern at each function's entry rather than scanning the image.
  const auto bytes = image.image_bytes();
  const auto entries = VtableEntries(image, table);
  std::vector<size_t> matches;
  for (size_t i = 0; i < entries.size(); ++i) {
    const auto entry = bytes.subspan(entries[i]);
    if (entry.size() < pattern->bytes.size()) {
      continue;
    }
    if (SignatureScan(entry.first(pattern->bytes.size()), *pattern).empty()) {
      continue;
    }
    matches.push_back(i);
  }
  return matches;
}

#if defined(_WIN32)
namespace {

// Resolver keeps each slot's resolved index for the life of the process. A
// module that is not mapped yet is not kept, so a later call can still
// resolve it.
//
// A hook installed through one slot rewrites that entry of the live table to
// point outside the module, which ends any later walk of the table there and
// hides the entries after it. So the first slot resolved in a class resolves
// every recorded slot of that class, before any of them can be hooked.
class Resolver {
 public:
  std::expected<size_t, std::string> Resolve(const VirtualSlot& slot) {
    // Answer a slot already resolved.
    const std::lock_guard lock(mutex_);
    if (const auto found = slots_.find(slot.id); found != slots_.end()) {
      return found->second;
    }

    // Map the slot's module.
    const std::wstring module(slot.module.begin(), slot.module.end());
    const auto image = MappedModuleImage::ForModule(module);
    if (!image) {
      return std::unexpected("slot '" + std::string(slot.id) + "': " + image.error());
    }

    // Locate every recorded slot of the class at once, sharing its table.
    const auto table = FindVtable(*image, slot.class_name);
    for (const auto& sibling : GameVirtualSlots()) {
      if (sibling.module != slot.module || sibling.class_name != slot.class_name) {
        continue;
      }
      slots_.emplace(std::string(sibling.id), Locate(*image, table, sibling));
    }
    return slots_.find(slot.id)->second;
  }

 private:
  static std::expected<size_t, std::string> Locate(const ModuleImage& image,
                                                   const std::expected<size_t, std::string>& table,
                                                   const VirtualSlot& slot) {
    // A slot needs its class's table.
    const std::string named = "slot '" + std::string(slot.id) + "'";
    if (!table) {
      return std::unexpected(named + ": " + table.error());
    }

    // The pattern must match exactly one function; a moved index is logged.
    const auto matches = MatchingSlots(image, *table, slot);
    if (!matches) {
      return std::unexpected(named + ": " + matches.error());
    }
    if (matches->size() != 1) {
      return std::unexpected(named + " matches " + std::to_string(matches->size()) +
                             " functions of " + std::string(slot.class_name));
    }
    if (matches->front() != slot.index) {
      std::fprintf(stderr, "[modlock] virtual slot %s moved from index %zu to %zu\n",
                   std::string(slot.id).c_str(), slot.index, matches->front());
    }
    return matches->front();
  }

  std::mutex mutex_;
  std::map<std::string, std::expected<size_t, std::string>, std::less<>> slots_;
};

}  // namespace
#endif

std::expected<size_t, std::string> ResolveVirtualSlot(std::string_view id) {
  // Look up the recorded slot, and on Windows resolve it in its module.
  const auto* slot = FindVirtualSlot(id);
  if (!slot) {
    return std::unexpected("slot '" + std::string(id) + "' is not recorded");
  }
#if defined(_WIN32)
  static Resolver resolver;
  return resolver.Resolve(*slot);
#else
  return slot->index;
#endif
}

}  // namespace modlock::gameinterop
