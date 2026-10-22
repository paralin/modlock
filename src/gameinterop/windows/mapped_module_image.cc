#include "modlock/gameinterop/mapped_module_image.h"

#include <windows.h>

// Windows declarations must precede psapi.h.
#include <psapi.h>

namespace modlock::gameinterop {

std::expected<MappedModuleImage, std::string> MappedModuleImage::ForModule(std::wstring_view name) {
  // Read the loaded module's image extent.
  const std::wstring terminated(name);
  auto module = GetModuleHandleW(terminated.c_str());
  MODULEINFO info{};
  if (!module || !GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info))) {
    return std::unexpected("loaded engine module information unavailable: " +
                           std::to_string(GetLastError()));
  }
  if (!info.lpBaseOfDll || !info.SizeOfImage) {
    return std::unexpected("loaded engine module has an empty image");
  }
  return MappedModuleImage(reinterpret_cast<std::uintptr_t>(info.lpBaseOfDll),
                           {static_cast<const uint8_t*>(info.lpBaseOfDll), info.SizeOfImage});
}

std::expected<void*, std::string> ResolveEngineInterface(std::wstring_view module_name,
                                                         std::string_view interface_name) {
  // Map the module's image.
  const std::string name(interface_name);
  const auto image = MappedModuleImage::ForModule(module_name);
  if (!image) {
    return std::unexpected("interface '" + name + "': " + image.error());
  }

  // The factory and its result belong to the module, not to this borrowed view.
  using CreateInterface = void* (*)(const char*, int*);
  const auto create = reinterpret_cast<CreateInterface>(reinterpret_cast<void*>(
      GetProcAddress(reinterpret_cast<HMODULE>(image->base()), "CreateInterface")));
  if (!create) {
    return std::unexpected("interface '" + name + "': CreateInterface export missing");
  }
  void* instance = create(name.c_str(), nullptr);
  if (!instance) {
    return std::unexpected("interface '" + name + "': module refused registration");
  }
  return instance;
}

}  // namespace modlock::gameinterop
