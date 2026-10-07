#include "host_app/windows/import_patch.h"

#include <cstdint>
#include <cstring>

namespace modlock::host_app {
namespace {

// ImportSlot returns the import address table entry through which module
// calls dll's export name or ordinal, or nullptr when module does not import
// it.
void** ImportSlot(HMODULE module, std::string_view dll, std::string_view name, WORD ordinal) {
  const auto base = reinterpret_cast<uint8_t*>(module);
  const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (!directory.VirtualAddress) return nullptr;

  const std::string dll_name(dll);
  auto descriptor =
      reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
  for (; descriptor->Name; ++descriptor) {
    if (_stricmp(reinterpret_cast<const char*>(base + descriptor->Name), dll_name.c_str()) != 0) {
      continue;
    }
    auto names = reinterpret_cast<const IMAGE_THUNK_DATA*>(base + descriptor->OriginalFirstThunk);
    auto slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);
    for (; names->u1.AddressOfData; ++names, ++slots) {
      if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) {
        if (ordinal != 0 && IMAGE_ORDINAL(names->u1.Ordinal) == ordinal) {
          return reinterpret_cast<void**>(&slots->u1.Function);
        }
        continue;
      }
      auto import = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
      if (reinterpret_cast<const char*>(import->Name) == name) {
        return reinterpret_cast<void**>(&slots->u1.Function);
      }
    }
  }
  return nullptr;
}

}  // namespace

std::expected<void*, std::string> PatchImport(HMODULE module, std::string_view dll,
                                              std::string_view name, WORD ordinal,
                                              void* replacement) {
  void** slot = ImportSlot(module, dll, name, ordinal);
  if (!slot) {
    return std::unexpected("module does not import " + std::string(name) + " from " +
                           std::string(dll));
  }
  DWORD protection = 0;
  if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &protection)) {
    return std::unexpected("VirtualProtect failed with error code " +
                           std::to_string(GetLastError()));
  }
  void* original = *slot;
  *slot = replacement;
  VirtualProtect(slot, sizeof(*slot), protection, &protection);
  return original;
}

}  // namespace modlock::host_app
