#include "modlock/gameinterop/virtual_slots.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <unordered_map>

#include "modlock/gameinterop/mapped_module_image.h"
#include "modlock/gameinterop/native_memory.h"
#include "modlock/gameinterop/pe_layout.h"
#include "modlock/gameinterop/signature.h"

namespace modlock::gameinterop {
namespace {

// kMaxEntries bounds a table walk; the largest game table holds a few hundred.
constexpr size_t kMaxEntries = 4096;

// kMaxTypeName bounds a decorated type name; the longest in the game's
// modules, a nested template, runs to a few hundred bytes.
constexpr size_t kMaxTypeName = 4096;

template <typename T>
std::optional<T> Read(std::span<const uint8_t> bytes, size_t at) {
  if (at > bytes.size() || bytes.size() - at < sizeof(T)) {
    return std::nullopt;
  }
  T value;
  std::memcpy(&value, bytes.data() + at, sizeof(T));
  return value;
}

// Executable reports whether offset falls in a section the PE headers mark
// executable.
bool Executable(std::span<const uint8_t> bytes, size_t offset) {
  // Find the section table.
  const auto nt_headers = Read<uint32_t>(bytes, pe::kNtHeadersOffset);
  if (!nt_headers) {
    return false;
  }
  const size_t file_header = *nt_headers + pe::kSignatureSize;
  const auto count = Read<uint16_t>(bytes, file_header + pe::kSectionCountOffset);
  const auto optional_size = Read<uint16_t>(bytes, file_header + pe::kOptionalHeaderSizeOffset);
  if (!count || !optional_size) {
    return false;
  }
  const size_t sections = file_header + pe::kFileHeaderSize + *optional_size;

  // The offset is executable when the section holding it is.
  for (size_t i = 0; i < *count; ++i) {
    // Read the section header.
    const size_t header = sections + i * pe::kSectionHeaderSize;
    const auto size = Read<uint32_t>(bytes, header + pe::kSectionVirtualSizeOffset);
    const auto address = Read<uint32_t>(bytes, header + pe::kSectionVirtualAddressOffset);
    const auto flags = Read<uint32_t>(bytes, header + pe::kSectionCharacteristicsOffset);

    // An executable section holding the offset answers.
    if (!size || !address || !flags) {
      return false;
    }
    if (offset >= *address && offset - *address < *size) {
      return (*flags & pe::kSectionExecute) != 0;
    }
  }
  return false;
}

// LocatorClass returns the class named by the primary object locator at image
// offset at, or nothing when the bytes there are not one. The caller leaves
// room for the locator's first four words.
std::string_view LocatorClass(std::span<const uint8_t> bytes, size_t at) {
  // A primary table's locator opens with signature 1, offset zero and no
  // constructor displacement.
  static constexpr uint32_t kHead[] = {1, 0, 0};
  if (std::memcmp(bytes.data() + at, kHead, sizeof(kHead)) != 0) {
    return {};
  }

  // Its next word is the image offset of the type descriptor, which holds a
  // vtable pointer and a spare word before its NUL-terminated decorated name.
  uint32_t descriptor = 0;
  std::memcpy(&descriptor, bytes.data() + at + sizeof(kHead), sizeof(descriptor));
  const size_t name = size_t{descriptor} + 16;
  if (name >= bytes.size()) {
    return {};
  }
  const auto* text = reinterpret_cast<const char*>(bytes.data() + name);
  const size_t room = std::min(bytes.size() - name, kMaxTypeName);
  const auto* end = static_cast<const char*>(std::memchr(text, '\0', room));
  if (!end) {
    return {};
  }

  // A class's decorated name is .?AV<class>@@.
  const std::string_view decorated(text, end);
  if (decorated.size() <= 6 || !decorated.starts_with(".?AV") || !decorated.ends_with("@@")) {
    return {};
  }
  return decorated.substr(4, decorated.size() - 6);
}

// ClassTables is what an image's type information records for one class name.
struct ClassTables {
  // locators counts the primary object locators that name the class.
  size_t locators = 0;
  // tables counts the stored pointers to those locators.
  size_t tables = 0;
  // table is the image offset of the dispatch table that follows the last of
  // those pointers.
  size_t table = 0;
};

// TypeIndex maps each class in an image to its primary dispatch table. It
// reads the image's MSVC RTTI in two passes over its bytes, one for the
// object locators and one for the pointers to them, so a lookup after that
// costs no search of the image.
class TypeIndex {
 public:
  // The index views image's bytes, which must outlive it.
  explicit TypeIndex(const ModuleImage& image);

  // Table returns the image offset of class_name's primary dispatch table.
  // A class with no locator, or with more than one locator or table, is an
  // error naming the class.
  std::expected<size_t, std::string> Table(std::string_view class_name) const;

 private:
  // classes_ is keyed by class names that view the image's bytes.
  std::unordered_map<std::string_view, ClassTables> classes_;
};

TypeIndex::TypeIndex(const ModuleImage& image) {
  // Record each primary locator under its class, keyed by the pointer the
  // image stores to it. A locator is aligned as its 32-bit fields are.
  const auto bytes = image.image_bytes();
  std::unordered_map<uint64_t, ClassTables*> locators;
  uint64_t lowest = UINT64_MAX;
  uint64_t highest = 0;
  for (size_t at = 0; at + 4 * sizeof(uint32_t) <= bytes.size(); at += sizeof(uint32_t)) {
    // Most offsets hold no locator.
    const auto name = LocatorClass(bytes, at);
    if (name.empty()) {
      continue;
    }
    ClassTables& tables = classes_[name];
    ++tables.locators;

    // Remember the pointer and widen the range the pointers span.
    const uint64_t pointer = image.pointer_base() + at;
    locators.emplace(pointer, &tables);
    lowest = std::min(lowest, pointer);
    highest = std::max(highest, pointer);
  }

  // A table starts one pointer past the aligned pointer to its locator. The
  // range test skips the lookup for every pointer that cannot be one.
  for (size_t at = 0; at + sizeof(uint64_t) <= bytes.size(); at += sizeof(uint64_t)) {
    // Read the pointer and keep one that reaches a locator.
    uint64_t pointer = 0;
    std::memcpy(&pointer, bytes.data() + at, sizeof(pointer));
    if (pointer < lowest || pointer > highest) {
      continue;
    }
    const auto found = locators.find(pointer);
    if (found == locators.end()) {
      continue;
    }

    // Count the table under its class.
    ++found->second->tables;
    found->second->table = at + sizeof(uint64_t);
  }
}

std::expected<size_t, std::string> TypeIndex::Table(std::string_view class_name) const {
  // The class needs exactly one primary locator.
  const std::string named = "class " + std::string(class_name);
  const auto found = classes_.find(class_name);
  if (found == classes_.end()) {
    return std::unexpected(named + " object locator not found");
  }
  const ClassTables& tables = found->second;
  if (tables.locators > 1) {
    return std::unexpected(named + " object locator found more than once");
  }

  // Exactly one table points to it.
  if (tables.tables == 0) {
    return std::unexpected(named + " dispatch table not found");
  }
  if (tables.tables > 1) {
    return std::unexpected(named + " dispatch table found more than once");
  }
  return tables.table;
}

// LoadedTypes keeps the type index of each loaded module for the life of the
// process, built by the first lookup in that module.
class LoadedTypes {
 public:
  // Table returns the offset of class_name's dispatch table in the loaded
  // module image.
  std::expected<size_t, std::string> Table(const ModuleImage& image, std::string_view class_name) {
    const std::lock_guard lock(mu_);
    const auto index = indexes_.try_emplace(image.base(), image).first;
    return index->second.Table(class_name);
  }

 private:
  // mu_ guards indexes_, and is held while a module is indexed so a second
  // caller waits for that index instead of building its own.
  std::mutex mu_;
  // indexes_ holds each loaded module's index by its base.
  std::map<std::uintptr_t, TypeIndex> indexes_;
};

}  // namespace

std::expected<size_t, std::string> FindVtable(const ModuleImage& image,
                                              std::string_view class_name) {
  // A loaded module is indexed once; any other image, for this lookup.
  if (!image.loaded()) {
    return TypeIndex(image).Table(class_name);
  }
  static LoadedTypes loaded;
  return loaded.Table(image, class_name);
}

std::string ObjectClassName(const void* object) {
  // The table's slot before its first function points to the object locator.
  const unsigned char* table = nullptr;
  const unsigned char* locator = nullptr;
  if (!object || !ReadNative(object, &table, sizeof(table)) || !table ||
      !ReadNative(table - sizeof(void*), &locator, sizeof(locator)) || !locator) {
    return {};
  }

  // The locator holds image offsets: its type descriptor's at +0x0c and its
  // own at +0x14, which gives the image's base.
  uint32_t descriptor = 0;
  uint32_t self = 0;
  if (!ReadNative(locator + 0x0c, &descriptor, sizeof(descriptor)) ||
      !ReadNative(locator + 0x14, &self, sizeof(self))) {
    return {};
  }
  const unsigned char* name = locator - self + descriptor + 16;

  // Copy the decorated name, such as .?AVCBaseModifier@@, 16 bytes at a time,
  // so a short name reads little past its end.
  std::string decorated;
  for (std::array<char, 16> word{}; decorated.size() < 512; name += word.size()) {
    if (!ReadNative(name, word.data(), word.size())) {
      return {};
    }
    const auto* end = static_cast<const char*>(std::memchr(word.data(), '\0', word.size()));
    decorated.append(word.data(), end ? end - word.data() : word.size());
    if (end) {
      break;
    }
  }

  // Strip the class or struct prefix and the closing @@.
  if (decorated.size() < 6 || !(decorated.starts_with(".?AV") || decorated.starts_with(".?AU")) ||
      !decorated.ends_with("@@")) {
    return {};
  }
  return decorated.substr(4, decorated.size() - 6);
}

std::vector<size_t> VtableEntries(const ModuleImage& image, size_t table) {
  const auto bytes = image.image_bytes();
  std::vector<size_t> entries;
  while (entries.size() < kMaxEntries) {
    // Stop at the first entry that is not code in the image.
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
