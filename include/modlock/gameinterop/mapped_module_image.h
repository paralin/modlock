#pragma once

#include <expected>
#include <string>
#include <string_view>

#include "modlock/export.h"
#include "modlock/gameinterop/game_symbols.h"

namespace modlock::gameinterop {

// MappedModuleImage borrows a loaded engine image. Its module must remain mapped
// while scanners or resolved engine functions use the returned view.
class MODLOCK_API MappedModuleImage final : public ModuleImage {
 public:
  // ForModule borrows an already-loaded image; it never loads or unloads a DLL.
  // Unsupported hosts return an error without accessing process memory.
  static std::expected<MappedModuleImage, std::string> ForModule(std::wstring_view name);

  // ForTesting borrows bytes standing in for a loaded image at base. Like a
  // loaded image, they must stay in place and unchanged until the process
  // ends, since ResolveSignature remembers what it found in them.
  static MappedModuleImage ForTesting(std::uintptr_t base, std::span<const uint8_t> bytes) {
    return MappedModuleImage(base, bytes);
  }

  std::uintptr_t base() const override { return base_; }

  std::span<const uint8_t> image_bytes() const override { return bytes_; }

 private:
  MappedModuleImage(std::uintptr_t base, std::span<const uint8_t> bytes)
      : base_(base), bytes_(bytes) {}

  std::uintptr_t base_;
  std::span<const uint8_t> bytes_;
};

// ResolveEngineInterface calls a loaded module's CreateInterface export.
// The returned singleton is borrowed: the module must remain loaded until all
// calls and hooks using it have ended. Failure identifies the requested
// interface and the missing module, export, or registration.
[[nodiscard]] MODLOCK_API std::expected<void*, std::string> ResolveEngineInterface(
    std::wstring_view module_name, std::string_view interface_name);

}  // namespace modlock::gameinterop
